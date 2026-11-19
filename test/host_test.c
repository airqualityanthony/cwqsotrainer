/* Host simulation of CW QSO Trainer: runs the real app code against a mock
 * API with a simulated clock, key contacts and tone output.
 *   cc -DHOST_TEST -O1 -I.. -I. test/host_test.c -o /tmp/qt && /tmp/qt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static uint32_t host_gpioa(void);
static uint32_t host_gpiob(void);
/* fake peripheral registers, by address */
static uint32_t regAddr[16], regVal[16]; static int nReg;
static volatile uint32_t *host_reg(uint32_t a) {
    for (int i = 0; i < nReg; i++) if (regAddr[i] == a) return &regVal[i];
    printf("  FAIL: app touched unexpected register 0x%08X\n", a); exit(3);
}
static uint32_t rv(uint32_t a) { return *host_reg(a); }
#define REG(a)      host_reg(a)
#define GPIOA_IDR   host_gpioa()
#define GPIOB_IDR   host_gpiob()
static uint32_t irqSave(void) { return 0; }
static void irqRestore(uint32_t p) { (void)p; }
#define app_main qt_main
#include "../cwqsotrainer_app.c"
#undef app_main

/* ---------- simulated world ---------- */
static uint64_t T;                 /* cycles */
#define MS(x) ((uint64_t)(x) * 48000u)
static uint8_t assets[4096]; static int assetLen;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } else { printf("  ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* contacts: list of intervals [a,b) for tip (bit0) and ring (bit1) */
typedef struct { uint64_t a, b; uint8_t bits; } iv_t;
static iv_t ivs[4000]; static int nIv;
static uint8_t contactBits(void) { uint8_t r = 0; for (int i = 0; i < nIv; i++) if (T >= ivs[i].a && T < ivs[i].b) r |= ivs[i].bits; return r; }
/* where the key is plugged in: 0 = PTT/SIDE1 buttons, 1 = USB-C, 2 = rework key port */
static int wired;
#define FIELD(r, pin) ((rv(r) >> (2 * (pin))) & 3u)
static bool pa10Ground(void) { return FIELD(0x50000000u, 10) == 1u && !(rv(0x50000004u) & (1u << 10)) && !(rv(0x50000014u) & (1u << 10)); }
static bool pulledInput(int pin) { return FIELD(0x50000000u, pin) == 0u && FIELD(0x5000000Cu, pin) == 1u; }
static uint32_t host_gpiob(void) {          /* PB10 = PTT line = button, or port tip via PA10 */
    uint8_t c = contactBits();
    bool low = (wired == 0 && (c & 1)) || (wired == 2 && (c & 1) && pa10Ground());
    return low ? 0 : (1u << 10);
}
static uint32_t host_gpioa(void) {
    uint32_t v = 0xFFFFFFFFu; uint8_t c = contactBits();
    if (wired == 1) {                       /* reads only as pulled-up inputs */
        if (!pulledInput(12) || (c & 1)) v &= ~(1u << 12);
        if (!pulledInput(11) || (c & 2)) v &= ~(1u << 11);
    }
    if (wired == 2 && (c & 2) && pa10Ground() && pulledInput(13)) v &= ~(1u << 13);
    return v;
}

/* scripted UI keys */
static int pendKey = -1; static uint64_t keyFrom, keyTo;
static void press(uint8_t k) { pendKey = k; keyFrom = T + MS(5); keyTo = T + MS(400); }

/* tone log */
typedef struct { uint64_t t; bool on; } ev_t;
static ev_t evs[20000]; static int nEv;

/* screen capture */
static char screen[16][64]; static int nScr;
static void scr(const char *tag, const char *s) { if (nScr < 16) snprintf(screen[nScr++], 64, "%s%s", tag, s); }

/* ---------- mock API ---------- */
static uint8_t fb[7][128], statusLine[128];
static uint8_t cfgStore[16]; static bool cfgValid;
static void m_display_clear(void) { nScr = 0; }
static void m_status_clear(void) {}
static void m_draw_line(app_fb_t f, int16_t a, int16_t b, int16_t c, int16_t d, bool k) { (void)f;(void)a;(void)b;(void)c;(void)d;(void)k; }
static void m_print_bold(const char *s, uint8_t a, uint8_t b, uint8_t l) { (void)a;(void)b; char t[8]; snprintf(t,8,"B%u| ",l); scr(t, s); }
static void m_print_normal(const char *s, uint8_t a, uint8_t b, uint8_t l) {
    (void)a; char t[8]; snprintf(t,8,"N%u| ",l); scr(t, s);
    if (strlen(s) * 7 > 128 || (b == 0 && strlen(s) > 18)) { fails++; printf("  FAIL: text too wide '%s'\n", s); }
}
static void m_print_tiny(const char *s, uint8_t x, uint8_t y, bool st, bool f) {
    (void)f; char t[12]; snprintf(t,12,"%s%u| ", st?"S":"t", y); scr(t, s);
    if (x + strlen(s) * 4 > 128) { fails++; printf("  FAIL: tiny too wide '%s'\n", s); }
}
static void m_print_inverse(const char *s, uint8_t x, uint8_t l, bool st, bool f, uint8_t e) { (void)x;(void)l;(void)st;(void)f;(void)e; scr("S*| ", s); }
static void m_blit(void) {} static void m_blit_line(unsigned l) { (void)l; }
static int dumpNext;
static void m_blit_full(void) { if (dumpNext) { dumpNext = 0; printf("    +--------- screen ---------\n"); for (int i = 0; i < nScr; i++) printf("    | %s\n", screen[i]); printf("    +--------------------------\n"); } }
static uint8_t m_get_key(void) {
    T += MS(1) / 5;               /* keypad scan ~0.2 ms */
    if (wired == 0 && (contactBits() & 2)) return KEY_SIDE1;
    if (pendKey >= 0 && T >= keyFrom && T < keyTo) return (uint8_t)pendKey;
    if (pendKey >= 0 && T >= keyTo) pendKey = -1;
    return APP_KEY_INVALID;
}
static void director(void);
static void m_delay_ms(uint32_t ms) { T += MS(ms); director(); }
static void m_play_tone(uint16_t a, uint16_t b) { (void)a;(void)b; }
static void m_led(bool o) { (void)o; }
static uint16_t m_bk_read(uint8_t r) { (void)r; return 0; }
static void m_bk_write(uint8_t r, uint16_t v) {
    if (r != 0x71) return;
    bool on = v != 0;
    if (nEv && evs[nEv-1].on == on) return;
    evs[nEv].t = T; evs[nEv].on = on; nEv++;
}
static void m_v(void) {} static void m_vb(bool b) { (void)b; } static void m_af(uint8_t m) { (void)m; }
static void m_cfg_load(uint8_t *b, uint8_t n) { if (cfgValid) memcpy(b, cfgStore, n); else memset(b, 0xFF, n); }
static void m_cfg_save(const uint8_t *b, uint8_t n) { memcpy(cfgStore, b, n); cfgValid = true; }
static int8_t m_nav(uint8_t k) { return k == APP_KEY_UP ? 1 : k == APP_KEY_DOWN ? -1 : 0; }
static uint32_t m_cycles(void) { return (uint32_t)T; }
static uint32_t m_ticks(void) { return (uint32_t)(T / 48000u); }
static uint32_t rs = 12345; static uint32_t m_rand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint16_t m_asset(uint16_t off, void *b, uint16_t n) { if (off >= assetLen) return 0; if (off + n > assetLen) n = assetLen - off; memcpy(b, assets + off, n); return n; }
static uint64_t m_idiv(int32_t n, int32_t d) { int32_t q = d ? n / d : 0, r = d ? n % d : n; return (uint32_t)q | ((uint64_t)(uint32_t)r << 32); }
static uint64_t m_udiv(uint32_t n, uint32_t d) { uint32_t q = d ? n / d : 0, r = d ? n % d : n; return q | ((uint64_t)r << 32); }
static char *m_app_u32(char *o, uint32_t u) { char t[12]; int n = sprintf(t, "%u", u); memcpy(o, t, n); return o + n; }
static char *m_app_i32(char *o, int32_t u) { char t[12]; int n = sprintf(t, "%d", u); memcpy(o, t, n); return o + n; }

static app_api_t api;

/* ---------- Morse helpers for the simulated operator ---------- */
static const char *morse(char c) {
    static const char *t[128]; static int init;
    if (!init) { init = 1;
        const char *m[][2] = {{"A",".-"},{"B","-..."},{"C","-.-."},{"D","-.."},{"E","."},{"F","..-."},{"G","--."},{"H","...."},{"I",".."},{"J",".---"},{"K","-.-"},{"L",".-.."},{"M","--"},{"N","-."},{"O","---"},{"P",".--."},{"Q","--.-"},{"R",".-."},{"S","..."},{"T","-"},{"U","..-"},{"V","...-"},{"W",".--"},{"X","-..-"},{"Y","-.--"},{"Z","--.."},{"0","-----"},{"1",".----"},{"2","..---"},{"3","...--"},{"4","....-"},{"5","....."},{"6","-...."},{"7","--..."},{"8","---.."},{"9","----."},{"/","-..-."}};
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) t[(int)m[i][0][0]] = m[i][1]; }
    return t[(int)c];
}
static double jit(double x, double j) { return x * (1.0 + j * ((rand() % 2001) / 1000.0 - 1.0)); }

/* straight key: schedule marks on tip */
static uint64_t keyStraight(const char *s, int wpm, double j, uint64_t t) {
    double u = 1200.0 / wpm;
    for (; *s; s++) {
        if (*s == ' ') { t += MS(jit(4 * u, j)); continue; }
        for (const char *e = morse(*s); *e; e++) {
            uint64_t len = MS(jit(*e == '-' ? 3 * u : u, j));
            ivs[nIv++] = (iv_t){ t, t + len, 1 }; t += len;
            t += e[1] ? MS(jit(u, j)) : MS(jit(3 * u, j));
        }
    }
    return t;
}
/* paddle: press the dit (bit0) or dah (bit1) contact for part of each element */
static uint64_t keyPaddle(const char *s, int wpm, uint64_t t) {
    double u = 1200.0 / wpm;
    for (; *s; s++) {
        if (*s == ' ') { t += MS(5 * u); continue; }
        for (const char *e = morse(*s); *e; e++) {
            double m = *e == '-' ? 3 * u : u;
            ivs[nIv++] = (iv_t){ t, t + MS(m * 0.6), (uint8_t)(*e == '-' ? 2 : 1) };
            t += MS(m + u + 5);
        }
        t += MS(2.5 * u);
    }
    return t;
}

/* decode the tone log between two times at the app's speed */
static void decodeTones(int from, int wpm, char *out) {
    double u = 1200.0 / wpm; char sym[16]; int ns = 0; *out = 0;
    for (int i = from; i + 1 < nEv; i++) {
        if (evs[i].on) {
            double d = (evs[i+1].t - evs[i].t) / 48000.0;
            sym[ns++] = d > 2 * u ? '-' : '.';
            sym[ns] = 0;
            double gap = (i + 2 < nEv) ? (evs[i+2].t - evs[i+1].t) / 48000.0 : 1e9;
            if (gap > 2 * u) {
                char c = '?';
                for (int k = 32; k < 128; k++) if (morse(k) && !strcmp(morse(k), sym)) c = (char)k;
                size_t L = strlen(out); out[L] = c; out[L+1] = 0; ns = 0;
                if (gap > 5 * u && gap < 1e8) strcat(out, " ");
            }
        }
    }
    size_t L = strlen(out); while (L && out[L-1] == ' ') out[--L] = 0;
}

/* ---------- the test director ---------- */
static int phase; static uint64_t waitUntil; static int playFrom; static uint8_t lastState = 0xEE;
static bool paddle; static char sentWith[64];
static const char *call = "M7ABC";

static void director(void) {
    if (T > MS(400000)) { printf("  WATCHDOG: state %u phase %d step %u ready %d nGot %u got '%s' playing %d nEv %d\n", g.state, phase, g.step, g.ready, g.nGot, g.got ? g.got : "", g.playing, nEv); exit(2); }
    if (g.state != lastState) {
        lastState = g.state;
        if (g.state == ST_PLAY) playFrom = nEv;
        if (g.state == ST_KEY) {
            char heard[96]; decodeTones(playFrom, g.cfg.wpm, heard);
            /* the line just played is g.step-1 */
            char want[64]; uint8_t st = g.step; expand(st - 1u); strcpy(want, g.line); expand(st);
            CHECK(!strcmp(heard, want), "played line %u sounds like '%s' (expected '%s')", st - 1u, heard, want);
        }
        if (g.state == ST_SUMMARY) {
            char heard[96]; decodeTones(playFrom, g.cfg.wpm, heard);
            CHECK(!strcmp(heard, g.line), "closing line sounds like '%s'", heard);
        }
        dumpNext = 1;
    }
    if (T < waitUntil) return;
    switch (phase) {
    case 0: /* home: set call via the editor: 6x delete then key it in */
        if (g.state == ST_HOME) { press(APP_KEY_1); phase = 1; waitUntil = T + MS(300); }
        break;
    case 1: case 2: case 3: case 4: case 5: case 6:
        press(APP_KEY_STAR); phase++; waitUntil = T + MS(250); break;
    case 7: {
        uint64_t end = keyStraight(call, g.cfg.wpm, 0.10, T + MS(200));
        waitUntil = end + MS(600); phase = 8; break; }
    case 8:
        CHECK(!strcmp(g.got, call), "callsign editor decoded '%s'", g.got);
        press(APP_KEY_MENU); phase = 9; waitUntil = T + MS(300); break;
    case 9:
        CHECK(!memcmp(g.cfg.call, call, strlen(call)) && !g.cfg.call[strlen(call)], "callsign saved in config");
        if (paddle) { press(APP_KEY_4); phase = 10; waitUntil = T + MS(300); } else phase = 11;
        break;
    case 10: CHECK(g.cfg.mode == PADDLE, "paddle mode selected"); phase = 11; break;
    case 11: press(APP_KEY_MENU); phase = 12; break;     /* start QSO */
    case 12: /* wait for a keying turn */
        if (g.state == ST_KEY && g.ready) {
            const char *s = g.line;
            char bad[64]; strcpy(bad, s);
            if (g.step == 3) { char *p = strstr(bad, "GL"); memmove(p, p + 1, strlen(p)); } /* drop the G */
            strcpy(sentWith, g.step == 3 ? bad : s);
            uint64_t end = paddle ? keyPaddle(sentWith, g.cfg.wpm, T + MS(300))
                                  : keyStraight(sentWith, g.cfg.wpm, 0.15, T + MS(300));
            phase = 13; waitUntil = end + MS(g.step == 3 ? 800 : 10);
        }
        if (g.state == ST_SUMMARY) phase = 20;
        break;
    case 13: /* step 1: rely on the 3 s auto-check; step 3: press MENU */
        if (g.step == 3 && g.state == ST_KEY) { press(APP_KEY_MENU); }
        phase = 14; break;
    case 14:
        if (g.state == ST_RESULT) {
            uint8_t sc = g.score[g.step >> 1];
            printf("    keyed '%s'\n    decoded '%s'  -> %u%%\n", sentWith, g.got, sc);
            if (g.step == 1) CHECK(sc == 100, "answer line scored 100%%");
            else CHECK(sc == 94, "report with one dropped letter scored %u%% (17 of 18 letters = 94)", sc);
            phase = 15; waitUntil = T + MS(700);
        }
        break;
    case 15: press(APP_KEY_MENU); phase = 12; waitUntil = T + MS(300); break;
    case 20:
        CHECK(g.score[0] == 100 && g.score[1] == 94, "summary scores %u%% / %u%%", g.score[0], g.score[1]);
        press(APP_KEY_EXIT); phase = 21; waitUntil = T + MS(300); break;
    case 21:
        CHECK(g.state == ST_HOME, "EXIT from summary returns home");
        press(APP_KEY_EXIT); phase = 22; break;
    }
}

static void run(bool pad, int in) {
    memset(&g, 0, sizeof g); nIv = nEv = 0; phase = 0; waitUntil = 0; lastState = 0xEE;
    paddle = pad; wired = in; cfgValid = false; T = MS(1000);
    if (in) { cfgValid = true; cfg_t c = { CFG_MAGIC, in == 1 ? 22 : 16, 14, 0, (uint8_t)in, 0, "N0CALL" }; memcpy(cfgStore, &c, 16); }
    /* registers as Brass leaves them after CW_OverlayEnter: UART on PA9/PA10 (AF,
     * pull-up), USB on PA11/PA12 (AF), PA13 SWDIO (AF) without pull, USART1 + DMA on */
    uint32_t a[] = { 0x50000000u, 0x50000004u, 0x5000000Cu, 0x50000014u, 0x4002103Cu, 0x4001380Cu, 0x4002001Cu };
    uint32_t v[] = { 0x2AA80000u | 0x3u, 0x00000002u, 0x24140000u, 0x00000600u, 0x00800001u, 0x0000200Cu, 0x000000A1u };
    nReg = 7; memcpy(regAddr, a, sizeof a); memcpy(regVal, v, sizeof v);
    qt_main(&api);
    CHECK(g.state == 0xFF && T < MS(400000), "app exited cleanly after %.1f s", T / 48000000.0);
    CHECK(!memcmp(regVal, v, sizeof v), "GPIOA, USB clock, USART1 and DMA restored exactly on exit");
    CHECK(nEv == 0 || !evs[nEv-1].on, "tone off at exit");
}

int main(void) {
    FILE *f = fopen("cwqsotrainer_assets.bin", "rb"); assetLen = fread(assets, 1, sizeof assets, f); fclose(f);
    api = (app_api_t){ .abi_major = 1, .api_level = 3, .api_size = sizeof(app_api_t), .fb = fb,
        .display_clear = m_display_clear, .status_clear = m_status_clear, .draw_line = m_draw_line, .draw_rect = m_draw_line,
        .print_bold = m_print_bold, .print_tiny = m_print_tiny, .blit_full = m_blit_full, .blit_line = m_blit_line, .blit_status = m_blit,
        .get_key = m_get_key, .delay_ms = m_delay_ms, .play_tone = m_play_tone, .led = m_led,
        .print_normal = m_print_normal, .print_inverse = m_print_inverse,
        .bk_read = m_bk_read, .bk_write = m_bk_write, .set_af = m_af, .audio_path = m_vb, .prepare_tone = m_v, .tones_off_rx = m_v,
        .cfg_load = m_cfg_load, .cfg_save = m_cfg_save, .draw_battery = m_v, .battery_sample = m_v, .backlight_on = m_v, .backlight_update = m_v,
        .status_line = statusLine, .nav_dir = m_nav, .ticks_ms = m_ticks, .rand32 = m_rand, .asset_read = m_asset,
        .idivmod = m_idiv, .uidivmod = m_udiv, .append_i32 = m_app_i32, .cycles_48mhz = m_cycles, .append_u32 = m_app_u32 };
    srand(7);
    printf("== STRAIGHT KEY on the PTT button, 18 WPM, 15%% timing jitter ==\n"); run(false, 0);
    printf("\n== PADDLE (iambic) on USB-C, 22 WPM ==\n"); run(true, 1);
    printf("\n== STRAIGHT KEY in the reworked key port, 16 WPM ==\n"); run(false, 2);
    printf("\n== PADDLE in the reworked key port (tip PTT line, ring PA13), 16 WPM ==\n"); run(true, 2);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
