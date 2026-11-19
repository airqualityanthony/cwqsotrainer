/* CW QSO Trainer - practise one side of a CW QSO.
 *
 * Licensed under the Apache License, Version 2.0.
 * https://www.apache.org/licenses/LICENSE-2.0
 * Local sidetone method and USB-C paddle port handling adapted from
 * CW Trainer / CW USB / CW Keyer (Armel F4HWN, Johan Denoyer F4WAT,
 * Sebastien F4LPF and contributors), Apache 2.0.
 *
 * The other station's lines are played as local sidetone and shown; your
 * lines are shown, you key them, the app decodes and scores them (%), then
 * plays the next line. A summary closes the QSO.
 *
 * RX ONLY: no transmitter call is made. The tone is the BK4829 local alarm
 * tone (REG_71), the same method as the CW Trainer app.
 *
 * Key inputs (read directly: Brass switches its keyer and port off in apps):
 *   tip  (straight key / dit): PTT line PB10 - the PTT button, or the key
 *        port tip; with INPUT = USB-C also PA12 (USB D+).
 *   ring (dah):  SIDE1; INPUT = PORT also PA13, INPUT = USB-C also PA11.
 *   INPUT = PORT grounds the key port's common on PA10, as Brass does.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../app_api.h"
#include "cwqsotrainer_assets.h"

#ifndef HOST_TEST  /* host tests supply their own registers */
#define REG(a)      ((volatile uint32_t *)(a))
#define GPIOA_IDR   (*(volatile const uint32_t *)0x50000010u)
#define GPIOB_IDR   (*(volatile const uint32_t *)0x50000410u)
#endif
/* Input-mode register patches (USB-C, key port) are in the assets: A_PATCH. */

#define CYC_MS      48000u         /* cycles_48mhz() per millisecond */
#define KEY_SIDE1   18u
#define CALL_MAX    10u
#define BUF         48u
#define AUTO_CHECK_MS 3000u
#define CFG_MAGIC   0xC7u

enum { ST_HOME, ST_CALL, ST_PLAY, ST_KEY, ST_RESULT, ST_SUMMARY };
enum { STRAIGHT, PADDLE };
enum { IN_RADIO, IN_USB, IN_PORT };

typedef struct {
    uint8_t magic, wpm, tone50, mode, in, rev;   /* in: IN_RADIO, IN_USB, IN_PORT */
    char    call[CALL_MAX];          /* NUL padded, not NUL terminated */
} cfg_t;
_Static_assert(sizeof(cfg_t) == 16u, "config is 16 bytes");

/* Bytes first: Thumb-1 byte loads reach offsets 0..31. */
static struct {
    uint8_t state, step, dx, key, raw, stable, element, nEl, code, nGot;
    uint8_t pIdx, pCode, portSaved, fArm;
    bool    mark, pending, ready, redraw, playing, checked;
    uint8_t score[2];
    cfg_t   cfg;
    uint32_t edgeAt, deadline, candAt, ditEst, dahEst, pAt, svcAt;
    uint32_t saved[6];               /* register bits the input mode changed */
    const app_api_t *api;
    char    *line;                   /* current script line, expanded (on app_main's stack) */
    char    *got;                    /* what the user keyed           (on app_main's stack) */
} g;
#define A (g.api)

/* ---- freestanding helpers -------------------------------------------- */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
uint64_t __aeabi_idivmod(int32_t n, int32_t d)    { return g.api->idivmod(n, d); }
uint64_t __aeabi_idiv(int32_t n, int32_t d)       __attribute__((alias("__aeabi_idivmod")));
uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d) { return g.api->uidivmod(n, d); }
uint64_t __aeabi_uidiv(uint32_t n, uint32_t d)    __attribute__((alias("__aeabi_uidivmod")));

static uint8_t slen(const char *s) { uint8_t n = 0; while (s[n]) n++; return n; }
static char *cat(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }

static char tbuf[TEXT_SIZE];
static const char *text(uint8_t id)
{
    A->asset_read((uint16_t)(A_TEXT + id * TEXT_SIZE), tbuf, TEXT_SIZE);
    return tbuf;
}
static char *put(char *o, uint8_t id) { return cat(o, text(id)); }

static uint8_t asset8(uint16_t off) { uint8_t v = 0; A->asset_read(off, &v, 1); return v; }

/* ---- port and tone ----------------------------------------------------- */
#ifndef HOST_TEST
static uint32_t irqSave(void) { uint32_t p; __asm volatile("mrs %0, primask\n cpsid i" : "=r"(p) :: "memory"); return p; }
static void irqRestore(uint32_t p) { __asm volatile("msr primask, %0" :: "r"(p) : "memory"); }
#endif

/* Input modes as register patches, applied in order and undone in reverse.
 * Only the masked bits are touched; the old bits are saved and put back.
 *
 * USB-C: PA11/PA12 pulled-up inputs, USB clock stopped (as CW USB / CW Trainer).
 * PORT:  the Brass key-port rework, as its CW_ConfigurePortGround/Ring do -
 *        USART1 and its RX DMA stopped, PA10 (K-plug UART RX) driven low as
 *        the key's ground, PA13 a pulled-up input for the ring. Brass undoes
 *        this itself when an app opens, so the app must redo it. */
typedef struct { uint32_t addr, mask, set; } patch_t;   /* table in the assets */

static void port(uint8_t in)
{
    patch_t P[9];
    A->asset_read(A_PATCH, P, sizeof P);
    uint32_t irq = irqSave();
    for (uint8_t pass = 0; pass < 2; pass++) {
        uint8_t m = pass ? in : g.portSaved, n = m == IN_PORT ? 6u : m == IN_USB ? 3u : 0u;
        const patch_t *p = P + (m == IN_PORT ? 3u : 0u);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t k = pass ? i : (uint8_t)(n - 1u - i);         /* undo in reverse */
            volatile uint32_t *r = REG(p[k].addr);
            uint32_t v = *r & ~p[k].mask;
            if (pass) { g.saved[k] = *r & p[k].mask; v |= p[k].set; }
            else v |= g.saved[k];
            *r = v;
        }
    }
    g.portSaved = in;
    irqRestore(irq);
}

static void tone(bool on, uint16_t hz)
{
    A->bk_write(0x71u, on ? (uint16_t)((hz * 1353245u + 65536u) >> 17) : 0u);
    g.mark = on;
}
#define SIDETONE ((uint16_t)(g.cfg.tone50 * 50u))

static void beep(uint16_t hz, uint16_t ms) { tone(true, hz); A->delay_ms(ms); tone(false, 0); A->delay_ms(40); }

static uint32_t unit(void) { return 1200u * CYC_MS / g.cfg.wpm; }

/* tip = bit 0, ring = bit 1, before the dit/dah swap */
static uint8_t contacts(uint8_t key)
{
    uint32_t a = GPIOA_IDR;
    uint8_t tip = !(GPIOB_IDR & (1u << 10));                 /* PTT / port tip */
    uint8_t ring = key == KEY_SIDE1;
    if (g.cfg.in == IN_PORT) ring |= !(a & (1u << 13));      /* port ring */
    if (g.cfg.in == IN_USB) {
        tip |= !(a & (1u << 12));
        ring |= !(a & (1u << 11));
    }
    if (g.cfg.mode == STRAIGHT) return tip;                   /* mono plug: ring ignored */
    return g.cfg.rev ? (uint8_t)((tip << 1) | ring) : (uint8_t)(tip | (ring << 1));
}

/* ---- decoder ----------------------------------------------------------- */
static void append(char c)
{
    if (g.state == ST_CALL) {                                 /* callsign editor */
        if (c == ' ' || c == '*' || c == '|' || g.nGot >= CALL_MAX) return;
    } else if (g.nGot >= BUF - 2u) return;
    if (c == '|') { g.got[g.nGot++] = 'B'; c = 'K'; }
    g.got[g.nGot++] = c;
    g.got[g.nGot] = 0;
    g.redraw = true;
}

static void finishLetter(void)
{
    if (!g.nEl) return;
    append(g.nEl > 7u ? '*' : (char)asset8((uint16_t)(A_DEC + (g.code | (1u << g.nEl)))));
    g.nEl = g.code = 0;
}

/* A key-down or key-up edge. el: 0 = straight key (classify by time),
 * 1 / 2 = dit / dah generated by the paddle keyer. */
static void edge(bool on, uint32_t now, uint8_t el)
{
    uint32_t dur = now - g.edgeAt;
    if (!on) {
        bool dah = el ? el == 2u : dur >= (g.ditEst + g.dahEst) >> 1;
        if (!el) {                       /* straight key: follow the operator's fist */
            if (dur * 3u < g.ditEst) goto out;                /* glitch */
            uint32_t *e = dah ? &g.dahEst : &g.ditEst, u = unit();
            *e = (3u * *e + dur) >> 2;
            if (g.ditEst < u / 2u) g.ditEst = u / 2u;
            if (g.ditEst > 2u * u) g.ditEst = 2u * u;
            if (g.dahEst < 2u * g.ditEst) g.dahEst = 2u * g.ditEst;
            if (g.dahEst > 5u * g.ditEst) g.dahEst = 5u * g.ditEst;
        }
        if (g.nEl < 8u) { g.code |= (uint8_t)(dah << g.nEl); g.nEl++; }
    }
out:
    tone(on, SIDETONE);
    g.edgeAt = now;
    g.checked = false;
    A->backlight_on();
}

static void startElement(uint8_t el, uint32_t now)
{
    g.element = el;
    g.pending = false;
    edge(true, now, 0);
    g.deadline = now + unit() * (el == 2u ? 3u : 1u);
}

/* Debounce (3 ms), then straight key or iambic paddle with memory. */
static void keyer(uint32_t now, uint8_t p)
{
    if (p != g.raw) { g.raw = p; g.candAt = now; }
    if (now - g.candAt >= 3u * CYC_MS) g.stable = p;
    if (!g.ready) {                                           /* wait for a clean release */
        if (g.stable) g.candAt = now;
        else if (now - g.candAt >= 20u * CYC_MS) { g.ready = true; g.edgeAt = now; }
        return;
    }
    if (g.cfg.mode == STRAIGHT) {
        if (g.stable != g.mark) edge(g.stable, now, 0);
        return;
    }
    if (g.element) {
        if (g.stable & (g.element ^ 3u)) g.pending = true;   /* opposite paddle: alternate */
        if (!app_cycles_reached(now, g.deadline)) return;
        if (g.mark) { edge(false, now, g.element); g.deadline = now + unit(); return; }
        uint8_t last = g.element, next = g.pending ? (uint8_t)(last ^ 3u) : g.stable;
        g.element = 0;
        if (next) startElement(next == 3u ? (uint8_t)(last ^ 3u) : next, now);
    } else if (g.stable) {
        startElement(g.stable == 3u ? 1u : g.stable, now);
    }
}

static void resetKeyer(void)
{
    tone(false, 0);
    g.element = g.raw = g.stable = g.nEl = g.code = 0;
    g.pending = g.ready = false;
    g.ditEst = unit();
    g.dahEst = 3u * g.ditEst;
}

/* ---- script ------------------------------------------------------------ */
static void expand(uint8_t step)
{
    char t[32], *o = g.line;
    A->asset_read((uint16_t)(A_SCRIPT + step * 32u), t, 32);
    for (char *s = t; *s; s++) {
        if (*s == '@') {
            for (uint8_t i = 0; i < CALL_MAX && g.cfg.call[i]; i++) *o++ = g.cfg.call[i];
        } else if (*s == '#') {
            char dx[9] = { 0 };
            A->asset_read((uint16_t)(A_DX + g.dx * 8u), dx, 8);
            o = cat(o, dx);
        } else *o++ = *s;
    }
    *o = 0;
}

static void clearGot(void) { g.nGot = 0; g.got[0] = 0; g.checked = false; }

static void enter(uint8_t state)
{
    g.state = state;
    resetKeyer();
    g.playing = false;
    if (state == ST_PLAY || state == ST_KEY || state == ST_RESULT) expand(g.step);
    if (state == ST_PLAY) { g.playing = true; g.pIdx = g.pCode = 0; g.pAt = A->cycles_48mhz() + 300u * CYC_MS; }
    if (state == ST_PLAY || state == ST_KEY) clearGot();
    g.redraw = true;
}

/* Non-blocking playback of g.line at the set speed. */
static void play(uint32_t now)
{
    if (!app_cycles_reached(now, g.pAt)) return;
    uint32_t u = unit();
    if (g.mark) {
        tone(false, 0);
        g.pAt = now + (g.pCode <= 1u ? 3u : 1u) * u;          /* letter or element gap */
        if (g.pCode <= 1u) g.pCode = 0;
        return;
    }
    if (!g.pCode) {
        char c = g.line[g.pIdx];
        if (!c) {                                             /* line finished */
            g.playing = false;
            if (g.step >= SCRIPT_LINES - 1u) enter(ST_SUMMARY);
            else { g.step++; enter(ST_KEY); }
            return;
        }
        g.pIdx++;
        if (c == ' ') { g.pAt = now + 4u * u; return; }      /* 3 + 4 = 7 unit word gap */
        g.pCode = (c >= 32 && c < 96) ? asset8((uint16_t)(A_ENC + c - 32)) : 0;
        if (g.pCode < 2u) { g.pCode = 0; return; }
    }
    tone(true, SIDETONE);
    g.pAt = now + ((g.pCode & 1u) ? 3u : 1u) * u;
    g.pCode >>= 1;
}

/* Similarity in %: longest common subsequence of the two texts, spaces
 * ignored, over the longer length. One missed or extra letter costs one. */
static uint8_t similarity(const char *want, const char *have)
{
    uint8_t row[BUF + 1], m = 0, n = 0;
    char b[BUF];
    for (const char *s = have; *s; s++) if (*s != ' ') b[m++] = *s;
    memset(row, 0, sizeof row);
    for (const char *s = want; *s; s++) {
        if (*s == ' ') continue;
        n++;
        uint8_t diag = 0;
        for (uint8_t j = 1; j <= m; j++) {
            uint8_t up = row[j];
            row[j] = *s == b[j - 1u] ? (uint8_t)(diag + 1u) : (row[j - 1u] > up ? row[j - 1u] : up);
            diag = up;
        }
    }
    uint8_t big = n > m ? n : m;
    return big ? (uint8_t)(row[m] * 100u / big) : 0u;
}

static void draw(void);
static void check(void)
{
    finishLetter();
    while (g.nGot && g.got[g.nGot - 1u] == ' ') g.got[--g.nGot] = 0;
    uint8_t s = similarity(g.line, g.got);
    g.score[g.step >> 1] = s;
    enter(ST_RESULT);
    draw();
    beep(s == 100u ? 1175u : 330u, 150);
}

/* ---- drawing ----------------------------------------------------------- */
/* Word-wrapped small font, 18 characters per row, from page `page`. */
static void rows(const char *s, uint8_t page, uint8_t max)
{
    char buf[19];
    uint8_t n = slen(s);
    if (n > max * 18u) s += n - max * 18u;                    /* too long: show the end */
    while (*s && max--) {
        n = slen(s);
        if (n > 18u) { n = 18u; while (n && s[n] != ' ') n--; if (!n) n = 18u; }
        memcpy(buf, s, n); buf[n] = 0;
        A->print_normal(buf, 0, 0, page++);
        s += n;
        while (*s == ' ') s++;
    }
}

static void tiny(uint8_t id, uint8_t y) { A->print_tiny(text(id), 2, y, false, true); }
static void centre(const char *s, uint8_t page) { A->print_normal(s, 0, 127, page); }

static char *pct(char *o, uint8_t v) { o = A->append_u32(o, v); *o++ = '%'; *o = 0; return o; }

static void draw(void)
{
    char s[34], *o;
    A->display_clear();
    A->status_clear();

    /* status bar: title capsule, speed, battery */
    if (g.state >= ST_PLAY) { o = put(s, T_QSO); o = A->append_u32(o, g.step + 1u); *o++ = '/'; o = A->append_u32(o, SCRIPT_LINES); *o = 0; }
    else put(s, g.state == ST_CALL ? T_SETCALL : T_TITLE);
    A->print_inverse(s, 2, 0, true, true, (uint8_t)(2u + slen(s) * 4u));
    o = A->append_u32(s, g.cfg.wpm); *put(o, T_WPM) = 0;
    A->print_tiny(s, 70, 1, true, true);
    A->draw_battery();

    switch (g.state) {
    case ST_HOME:
        o = put(s, T_CALL); for (uint8_t i = 0; i < CALL_MAX && g.cfg.call[i]; i++) *o++ = g.cfg.call[i]; *o = 0;
        A->print_bold(s, 0, 127, 0);
        o = A->append_u32(s, g.cfg.wpm); o = put(o, T_WPM); o = A->append_u32(o, SIDETONE); put(o, T_HZ);
        centre(s, 2);
        o = put(s, g.cfg.mode ? T_PADDLE : T_STRAIGHT); if (g.cfg.rev && g.cfg.mode) put(o, T_REV);
        centre(s, 3);
        centre(text((uint8_t)(T_IN_RADIO + g.cfg.in)), 4);
        tiny(T_HELP_H1, 42); tiny(T_HELP_H2, 50);
        if (g.fArm) A->print_inverse("F", 120, 0, true, true, 124);
        break;
    case ST_CALL:
        o = cat(s, g.got); *o++ = '_'; *o = 0;
        A->print_bold(s, 0, 127, 1);
        tiny(T_HELP_C1, 34); tiny(T_HELP_C2, 44);
        break;
    case ST_PLAY: {
        char dx[9] = { 0 };
        A->asset_read((uint16_t)(A_DX + g.dx * 8u), dx, 8);
        put(cat(s, dx), T_SENDS);
        A->print_tiny(s, 2, 1, false, true);
        rows(g.line, 1, 3);
        tiny(T_HELP_P, 50);
        break;
    }
    case ST_KEY:
    case ST_RESULT:
        if (g.state == ST_KEY) tiny(T_YOURTURN, 1);
        else { pct(put(s, T_SCORE), g.score[g.step >> 1]); A->print_bold(s, 0, 127, 0); }
        rows(g.line, 1, 2);
        A->draw_line(A->fb, 0, 25, 127, 25, true);
        tiny(T_YOUSENT, 27);
        o = cat(s, g.got); if (g.state == ST_KEY) *o++ = '_'; *o = 0;
        rows(s, 4, 2);
        tiny(g.state == ST_KEY ? T_HELP_K : T_HELP_R, 50);
        break;
    case ST_SUMMARY:
        A->print_bold(text(T_DONE), 0, 127, 0);
        pct(put(s, T_ANSWER), g.score[0]);  centre(s, 2);
        pct(put(s, T_REPORT), g.score[1]);  centre(s, 3);
        pct(put(s, T_OVERALL), (uint8_t)((g.score[0] + g.score[1]) / 2u)); A->print_bold(s, 0, 127, 4);
        tiny(T_HELP_S, 50);
        break;
    }
    A->blit_status();
    A->blit_full();                                            /* also feeds the Live Viewer */
    g.redraw = false;
}

/* ---- keys -------------------------------------------------------------- */
static void bump8(uint8_t *v, int8_t d, uint8_t lo, uint8_t hi)
{
    int16_t n = (int16_t)(*v + d);
    if (n >= lo && n <= hi) *v = (uint8_t)n;
}

static void ui(uint8_t key)
{
    if (key == g.key) return;                                  /* act on a new press only */
    g.key = key;
    if (key == APP_KEY_INVALID || key == APP_KEY_PTT || key == KEY_SIDE1 || key >= APP_KEY_SAVER) return;
    A->backlight_on();
    int8_t dir = A->nav_dir(key), down = g.fArm ? -1 : 1;
    g.redraw = true;

    switch (g.state) {
    case ST_HOME:
        if (key == APP_KEY_F) { g.fArm ^= 1u; return; }
        if (dir) bump8(&g.cfg.wpm, dir, 5, 40);
        else if (key == APP_KEY_MENU) { g.dx = (uint8_t)(A->rand32() % DX_COUNT); g.step = 0; enter(ST_PLAY); }
        else if (key == APP_KEY_1) {
            clearGot();
            for (uint8_t i = 0; i < CALL_MAX && g.cfg.call[i]; i++) g.got[g.nGot++] = g.cfg.call[i];
            g.got[g.nGot] = 0;
            enter(ST_CALL);
        }
        else if (key == APP_KEY_2) bump8(&g.cfg.wpm, down, 5, 40);
        else if (key == APP_KEY_3) bump8(&g.cfg.tone50, down, 6, 20);
        else if (key == APP_KEY_4) g.cfg.mode ^= 1u;
        else if (key == APP_KEY_5) g.cfg.rev ^= 1u;
        else if (key == APP_KEY_6) { g.cfg.in = g.cfg.in >= IN_PORT ? IN_RADIO : (uint8_t)(g.cfg.in + 1u); port(g.cfg.in); }
        else if (key == APP_KEY_EXIT) g.state = 0xFF;          /* quit */
        g.fArm = 0;
        resetKeyer();
        return;
    case ST_CALL:
        if (key <= APP_KEY_9) { finishLetter(); append((char)('0' + key)); }
        else if (dir && g.nGot) {                              /* cycle the last character */
            char *c = &g.got[g.nGot - 1u];
            if (*c < 'A' || *c > 'Z') *c = 'A';
            else *c = (char)(*c + dir < 'A' ? 'Z' : *c + dir > 'Z' ? 'A' : *c + dir);
        }
        else if (dir) append('A');
        else if (key == APP_KEY_STAR) { if (g.nGot) g.got[--g.nGot] = 0; }
        else if (key == APP_KEY_MENU) {
            finishLetter();
            if (g.nGot) { memset(g.cfg.call, 0, CALL_MAX); memcpy(g.cfg.call, g.got, g.nGot); }
            enter(ST_HOME);
        }
        else if (key == APP_KEY_EXIT) enter(ST_HOME);
        return;
    case ST_PLAY:
        if (key == APP_KEY_1) enter(ST_PLAY);
        else if (key == APP_KEY_EXIT) enter(ST_HOME);
        return;
    case ST_KEY:
        if (key == APP_KEY_MENU) check();
        else if (key == APP_KEY_STAR) { clearGot(); resetKeyer(); }
        else if (key == APP_KEY_1) { g.step--; enter(ST_PLAY); }
        else if (key == APP_KEY_EXIT) enter(ST_HOME);
        return;
    case ST_RESULT:
        if (key == APP_KEY_MENU) { g.step++; enter(ST_PLAY); }
        else if (key == APP_KEY_STAR) enter(ST_KEY);
        else if (key == APP_KEY_1) { g.step--; enter(ST_PLAY); }
        else if (key == APP_KEY_EXIT) enter(ST_HOME);
        return;
    default: /* ST_SUMMARY */
        if (key == APP_KEY_MENU) { g.dx = (uint8_t)(A->rand32() % DX_COUNT); g.step = 0; enter(ST_PLAY); }
        else if (key == APP_KEY_EXIT) enter(ST_HOME);
        return;
    }
}

static void loadConfig(void)
{
    A->cfg_load((uint8_t *)&g.cfg, sizeof g.cfg);
    cfg_t *c = &g.cfg;
    if (c->magic != CFG_MAGIC || c->wpm < 5u || c->wpm > 40u || c->tone50 < 6u || c->tone50 > 20u ||
        c->mode > 1u || c->in > IN_PORT || c->rev > 1u) {
        memset(c, 0, sizeof *c);
        c->magic = CFG_MAGIC; c->wpm = 18; c->tone50 = 14;   /* 18 WPM, 700 Hz */
        memcpy(c->call, "N0CALL", 6);
    }
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    if (!api || api->abi_major != 1u || api->api_level < 3u) return;
    char line[BUF], got[BUF];
    g.api = api;
    g.line = line; g.got = got;
    uint32_t magic = 0;
    A->asset_read(0, &magic, 4);
    if (magic != 0x31545143u) return;                          /* "CQT1": assets missing */
    loadConfig();
    port(g.cfg.in);

    /* local sidetone only: alarm tone route, TX modulation never enabled */
    A->audio_path(false);
    A->prepare_tone();
    A->bk_write(0x70u, 0x9C00u);
    tone(false, 0);
    A->set_af(2u);
    A->delay_ms(20);
    A->audio_path(true);

    g.key = A->get_key();                                      /* ignore the launch key */
    A->backlight_on();
    enter(ST_HOME);

    while (g.state != 0xFF) {
        uint8_t key = A->get_key();
        uint32_t now = A->cycles_48mhz();

        if (g.playing) play(now);
        else if (g.state == ST_KEY || g.state == ST_CALL) {
            keyer(now, contacts(key));
            if (!g.mark && g.ready) {                          /* gaps end letters and words */
                uint32_t gap = now - g.edgeAt;
                if (g.nEl && gap >= (g.ditEst + g.dahEst) >> 1) finishLetter();
                if (g.state == ST_KEY && g.nGot && g.got[g.nGot - 1u] != ' ' && gap >= 2u * g.dahEst) append(' ');
                if (g.state == ST_KEY && g.nGot && !g.checked && !g.element && gap >= AUTO_CHECK_MS * CYC_MS) {
                    g.checked = true;
                    check();
                }
            }
        }
        ui(key);

        A->cfg_save((const uint8_t *)&g.cfg, sizeof g.cfg);   /* a change only, survives power-off */
        if (g.redraw && !g.mark && !g.element) draw();
        if (now - g.svcAt >= 100u * CYC_MS) {
            g.svcAt = now;
            A->battery_sample();
            A->backlight_update();
        }
        A->delay_ms(1);
    }

    tone(false, 0);
    A->audio_path(false);
    A->bk_write(0x70u, 0);
    A->set_af(APP_AF_MUTE);
    A->tones_off_rx();
    port(IN_RADIO);
    A->display_clear();
}
