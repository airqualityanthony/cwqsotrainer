# CW QSO Trainer 0.2

Practise your side of a CW QSO on the UV-K5 V3 / UV-K1, for the Brass CW
firmware (briand) and other F4HWN-based firmware with overlay ABI 1, API 3+.

The other station's lines are **played as sidetone and shown**. Your lines are
**shown, you key them**, the app decodes them and gives a **% score**, then
the QSO continues. A summary closes it.

| # | Who | Line (`@` = your call, `#` = their call) |
|---|---|---|
| 1 | Them (played) | `CQ CQ POTA DE # K` |
| 2 | **You (scored)** | `@ @ K` |
| 3 | Them (played) | `@ DE # TU UR RST 599 BK` |
| 4 | **You (scored)** | `RR TU UR RST 599 GL 73 EE` |
| 5 | Them (played) | `RR TU 73 EE` |

The other station's callsign is picked at random each QSO from K1FW, W1AW,
N7XR, K4SWL, AA1BB, VE3KZ, W6OUT, KD9XYZ. 

**Receive only.** Nothing is transmitted: the tone is the radio's local alarm
tone, as in the CW Trainer app. Brass's own CW keyer is suspended while an app
runs, so this app reads the key itself.

## Key inputs

Press **6** on the home screen to choose where your key is plugged in:

| `INPUT` | Tip: straight key / dit | Ring: dah |
|---|---|---|
| `PTT/SIDE1` (default) | PTT button | SIDE1 |
| `USB-C + PTT` | USB-C tip (D+), or PTT | USB-C ring (D-) |
| `KEY PORT` | Reworked key port tip (PTT line), or PTT | Reworked key port ring (PA13) |

- **KEY PORT** is for the Brass paddle rework. When an app opens, Brass
  switches its keyer off and releases the port. That un-grounds PA10, gives it
  back to the serial port, and drops the PA13 pull-up, so a key in the port
  reads nothing. In KEY PORT mode the app does the same setup as Brass's
  `CW_ConfigurePortGround` / `CW_ConfigurePortRing`: it stops USART1 and its
  RX DMA, drives PA10 low as the key's ground, and makes PA13 a pulled-up
  input. On exit, or when you change input, it puts back exactly the bits it
  changed. While it's on, the K-plug serial port (programming cable) doesn't
  work, just as when Brass is keying from the port.
- **USB-C** turns the USB data lines into key inputs and restores them on
  exit. While it's on, UV Studio's connection and Live Viewer won't work, and
  a USB cable to a PC reads as a closed key. Use a key/paddle adapter.
- `STRAIGHT KEY` uses the tip only, so a mono plug works. `PADDLE` is an
  iambic keyer (squeeze alternates dit/dah), and `REV` swaps dit and dah.

## Controls

**Home screen**

| Key | Action |
|---|---|
| MENU | Start a QSO |
| 1 | Set your callsign |
| 2 / UP / DOWN | Speed, 5–40 WPM (playback and decoding) |
| 3 | Tone, 300–1000 Hz |
| 4 | Straight key / paddle |
| 5 | Reverse paddle |
| 6 | Input: PTT/SIDE1 → USB-C → KEY PORT |
| F then 2 or 3 | Lower speed or tone |
| EXIT | Quit (settings are saved) |

**Set callsign:** key it in Morse, or press 0–9 for digits. UP/DOWN turns the
last character into a letter and steps through A–Z. `*` deletes, MENU saves,
EXIT cancels. Up to 10 characters, `/` allowed.

**During the QSO**

| Screen | MENU | * | 1 | EXIT |
|---|---|---|---|---|
| They're sending | | | Replay | Home |
| Your turn | Check now | Clear and restart | Replay their line | Home |
| Score | Next line | Try again | Replay their line | Home |
| Summary | New QSO | | | Home |

Your line is checked automatically **3 seconds after you stop keying**, or
when you press MENU.

## Scoring

The score is the share of letters you got right, in order. Spaces are ignored,
so your word spacing doesn't count. Each missing, extra or wrong letter costs
about one letter's worth. For example, `RR TU UR RST 599 L 73 EE` (G missed)
scores 17/18 = 94%.

The straight-key decoder starts at the set speed and adapts to your timing by
tracking your average dit and dah, so set WPM near your sending speed. Send
`BK` as B K or run together; both count.

## Build

Docker, from the repo root: `./compile-app.sh cwqsotrainer` gives `build/Apps/CWQSOTrainer.app`

Native (arm-none-eabi-gcc): `cd App/apps/cwqsotrainer && ./build.sh`

Host simulation (Linux, gcc with 32-bit support):
`gcc -m32 -DHOST_TEST -O1 -w -fno-builtin -I. test/host_test.c -o /tmp/qt && /tmp/qt`
(run from this folder after `./build.sh`, which generates the assets).

Size: 4060 of 4096 bytes of overlay and 1440 of 3840 bytes of assets. Lengthen
script lines or add DX calls in `gen_assets.py` freely, since they live in the
assets. Code changes have only about 36 bytes to spare.

Edit `SCRIPT` and `DX` at the top of
`gen_assets.py` to change the QSO or the stations, then rebuild.
