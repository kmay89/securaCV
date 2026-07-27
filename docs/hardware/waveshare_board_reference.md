# Waveshare 4.3″ dash boards — our reference, kept fresh

The Canary Dash is built on Waveshare's **ESP32-S3-Touch-LCD-4.3** family, and
we lean on it hard — three variants across the display line. This page is our
**first-party reference** for that hardware: the pin maps, the onboard silicon,
the interfaces, and how *our* firmware uses each. It exists so the facts we
depend on are ours to read, at a glance, without chasing a browser tab — and so
they can't quietly go stale under us.

> **Waveshare owns the hardware and its documentation. Their wiki is
> canonical.** What lives here is a *machine-readable snapshot of the facts*
> (GPIO numbers, chip part numbers, interface pinouts, parameters) transcribed
> from those pages, plus our own notes on how we drive them. We don't mirror
> Waveshare's prose or figures — where you want the vendor's own words,
> diagrams, and schematics, follow the canonical links below.

## Where the exact data lives

| Thing | Where |
|---|---|
| **Exhaustive, machine-readable pin maps + specs** (all 3 boards) | [`canary-local/devices/board_facts.json`](../../canary-local/devices/board_facts.json) |
| **The refresher** that pulls it from the vendor and self-heals | [`canary-local/tools/gen_board_facts.py`](../../canary-local/tools/gen_board_facts.py) |
| **The freshness loop** (weekly + on-demand, PR on any change) | [`.github/workflows/board-facts-freshness.yml`](../../.github/workflows/board-facts-freshness.yml) |
| **Our firmware pin maps** (authoritative for what we build) | [`firmware/boards/waveshare-esp32s3-lcd43*/pins/pins.h`](../../firmware/boards/) |
| **Drift-lock** proving our `pins.h` matches the vendor snapshot | [`canary-local/tests/board_facts.test.mjs`](../../canary-local/tests/board_facts.test.mjs) |

The snapshot carries a `verified_utc` date per board — when these facts were
last recorded (it moves only when a fact actually changes, so a stable board's
date reads "unchanged since"). The freshness workflow's own run history (the
Actions tab) is where you confirm the loop is alive.

## The three boards

All three share the same ESP32-S3-WROOM-1-N16R8 (16 MB flash / 8 MB PSRAM), the
same 4.3″ 800×480 RGB IPS panel, the same GT911 5-point touch on I²C
(GPIO8/GPIO9), and the same CH422G I/O expander that switches board functions
over its EXIO bits. They diverge in what they add around that core.

### ESP32-S3-Touch-LCD-4.3 — the plain dash
Canonical: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3> · our map:
[`waveshare-esp32s3-lcd43/pins/pins.h`](../../firmware/boards/waveshare-esp32s3-lcd43/pins/pins.h)

The baseline. Adds an RS485 transceiver (SP3485) and a CAN transceiver
(TJA1051) brought out to screw terminals, a TF slot, and a CS8501 single-cell
Li-ion charge/boost path (PH2.0 header, PWR/CHG/DONE LEDs). USB and CAN are
**muxed** through the expander — `EXIO5` low = USB, high = CAN — so only one is
live at a time. RS485 rides GPIO16/GPIO15.

### ESP32-S3-Touch-LCD-4.3B — the industrial dash (our Canary Dash host)
Canonical: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B> · our map:
[`waveshare-esp32s3-lcd43b/pins/pins.h`](../../firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h)

The BOX/industrial build. Trades the muxing for **dedicated** CAN (GPIO15/16)
alongside native USB, moves RS485 to GPIO43/44 (the same wires as the CH343
USB-UART console — mutually exclusive in use), adds **isolated digital I/O**
(DI0/DI1 in, DO0/DO1 out) on a screw-terminal strip, and carries a **PCF85063
RTC** on the shared I²C bus. This is the board most of the display firmware
targets.

### ESP32-S3-Touch-LCD-4.3C — the "AI voice" dash (the mic variant)
Canonical: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3C> · our map:
[`waveshare-esp32s3-lcd43c/pins/pins.h`](../../firmware/boards/waveshare-esp32s3-lcd43c/pins/pins.h)
· contract: [`display_mic_variant.md`](./display_mic_variant.md)

The one display in the family that physically carries **microphones**: a
dual-MIC array behind an **ES7210** ADC, an **ES8311** codec alongside for the
speaker path, both on the shared I²C bus. Its I²S lines are vendor-documented
(MCLK GPIO6, SCLK GPIO44, LRCK GPIO16, mic-data-in GPIO43 — the ES7210 ADC's
serial-data-out — speaker-data-out GPIO15; the power-amp enable is CH422G
`EXIO4`). It also brings the PCF85063
RTC, a backlight-PWM line via the expander (`EXIO_PWM`), and isolated I/O
(`EXIO0/5/6/7`). Because it hears, it's a **distinct privacy surface** — see the
[listening contract](./display_mic_variant.md).

### The CH422G "switched" bits (shared)
The expander is how the board flips functions that aren't wired to a bare GPIO —
the "switched GPIO" worth knowing when a peripheral won't wake:

| EXIO | Function |
|---|---|
| `EXIO1` | Touch reset (`TP_RST`) |
| `EXIO2` | Backlight enable (`DISP`) |
| `EXIO4` | TF-card chip-select (`SD_CS`); on the 4.3C also the audio PA enable (`PA_CTRL`) |
| `EXIO5` | USB/CAN select on the 4.3 (`USB_SEL`/`CAN_SEL`); a DI on the 4.3C |
| `EXIO0/5/6/7` | Isolated `DI0/DI1/DO0/DO1` (4.3C) |
| `EXIO_PWM` | Backlight PWM (`BL_PWM`, 4.3C) |

The exhaustive per-board expander and pin tables are in `board_facts.json`;
this is the orientation, not the whole map.

## How "kept fresh" works

Facts age on their own — a vendor silently corrects a pin, adds a note, moves a
line — so this data shares the anti-rot design the Home-Assistant hub page uses:

1. `gen_board_facts.py --refresh-upstream` fetches each board's vendor page and
   re-parses the pin maps, onboard list, and parameters into the snapshot.
2. It's **self-healing**: a board's facts move forward only on a clean
   fetch+parse. A dead feed, a 403, or a reshaped page keeps the last good
   snapshot verbatim (→ "no diff, no PR") rather than writing something broken.
   `verified_utc` advances only on a real read.
3. The [freshness workflow](../../.github/workflows/board-facts-freshness.yml)
   runs it weekly (and on demand) and opens a PR when any fact changed, so a
   correction upstream becomes a reviewable diff here — and the drift-lock test
   re-checks that our `pins.h` still agrees with the refreshed facts before it
   can merge.

So the data stays ours, stays exact, and tells on itself if the loop ever
stops. When a fact genuinely changes upstream, the PR is where we decide whether
our firmware pin map should follow.

## Mechanical (BOX variants)

The cased 4.3B/4.3C ("BOX") outline is 116.30 × 79.00 × 18.00 mm, with two M4
mounting holes on 75.00 × 39.00 mm centres and the screw-terminal strip on a
3.50 mm pitch. Side ports (top to bottom): TF card, USB-C, BOOT, RESET. Finer
geometry — bezel radius, exact lip profile — isn't published as dimensions;
where a build needs it, measure the unit or pull the vendor's mechanical
drawing/STEP from the wiki. The published figures live in `board_facts.json`
under each board's `parameters`.
