# SecuraCV Canary Display — Arduino IDE Build

The Arduino-IDE-buildable **parity** version of the Canary Display firmware —
the same code as the PlatformIO tree in `firmware/projects/canary-display/`,
packaged flat for the Arduino toolchain. One sketch, two flavors:

| Flavor | Board | Panel |
|---|---|---|
| **watch** | Seeed XIAO ESP32-S3 + Round Display for XIAO | GC9A01 1.28" 240×240, CST816S touch |
| **dash**  | ESP32-S3-DevKitC-1 profile + Waveshare 4.3/4.3B | 800×480 RGB, GT911 touch, CH422G expander |
| **playground** | Waveshare ESP32-S3-Touch-LCD-4.3B (vendor board, core 3.x) | dash hardware + isolated-IO bench mode — [dev playground doc](../../../../../docs/hardware/dev_playground_43b.md) |

> ⚠️ **DEV STATUS (v0.1):** compile/CI-verified; not yet validated on bench
> hardware. See the [bench bring-up runbook](../../../../../docs/hardware/display_bench_bringup.md).

## This sketch is generated — don't hand-edit it

The canonical source is the **PlatformIO tree** (`../../src/` + `../../include/`).
The flat `.ino`, `.cpp` and `.h` here are produced from it by `../../setup.sh`,
which flattens the namespaced `#include "canary/…"` paths and resolves the one
name collision the flat layout creates (both the composition header
`canary/config.h` and the flavor `configs/<flavor>/config.h` want to be
`config.h` — the flavor one is staged as `flavor_config.h`). A CI guard
(`check_display_arduino_sync.sh`) fails if the committed sketch drifts from the
canonical tree. **Fix bugs in `../../src`, then regenerate** (below).

## Two toolchains, one important difference

The `sketch.yaml` **build profiles** (core + pinned libraries per flavor)
are an **arduino-cli feature — the Arduino IDE does not read them** ([the
IDE feature request is still open](https://github.com/arduino/arduino-ide/issues/2573)).
So:

- **arduino-cli**: `--profile watch-core3` auto-installs the right core and
  every pinned library in an isolated build. Nothing to manage.
- **Arduino IDE**: you install the core + libraries yourself via Boards /
  Library Manager (exact list below) — the IDE builds against your global
  installs, same as our CI compile gate does.

The core matters because GFX, NimBLE, and LVGL split their library majors
along the arduino-esp32 core boundary:

| Your `esp32` platform | GFX Library for Arduino | NimBLE-Arduino | LVGL | CLI profiles |
|---|---|---|---|---|
| **3.x** (Boards Manager default) | 1.6.6 | 2.5.0 | 9.5.0 | `watch-core3` / `dash-core3` |
| **2.0.17** (PlatformIO release path) | 1.4.9 (EXACT) | 1.4.3 | 8.4.0 | `watch` / `dash` |

Mixing GFX/NimBLE rows (core 3 + GFX 1.4.9, or core 2 + NimBLE 2.x) fails
the build. LVGL is the friendly exception: the source is dual-major, so
either 8.4 or 9.x builds on either core — the rows above are just what the
profiles pin (8.4 = PlatformIO/bench parity, 9.5 = stock Library Manager
install).

## Build — Arduino IDE (step by step)

1. **Board core**: Boards Manager → install **esp32 by Espressif Systems**
   (the 3.x default is fine). If it's missing from the list, add the URL
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   under Settings → Additional boards manager URLs.
2. **Libraries**: Library Manager → install, matching your core line from
   the table above:
   - `lvgl` — the Library Manager latest (9.x) works as-is; **8.4.0** is
     the bench-parity pairing for core 2 (the source builds on both majors)
   - `GFX Library for Arduino` **1.6.6** (core 3) / **1.4.9** (core 2)
   - `NimBLE-Arduino` **2.5.0** (core 3) / **1.4.3** (core 2)
   - `PubSubClient`, `ArduinoJson`, and `Crypto` (by Rhys Weatherley) — latest
3. **LVGL config** (one file copy): LVGL looks for `lv_conf.h` one level
   above its own library folder. Copy the sketch's copy there:
   ```bash
   # macOS/Windows sketchbook default:
   cp <sketch dir>/lv_conf.h ~/Documents/Arduino/libraries/lv_conf.h
   # Linux: ~/Arduino/libraries/lv_conf.h
   ```
   (`./setup.sh arduino <flavor>` does this copy for you when it can find
   your sketchbook.)
4. **Tools menu** — and this picks your flavor too (the firmware follows
   the board, so a board/firmware mismatch can't happen by accident):
   - Watch: Board → `XIAO_ESP32S3`, PSRAM → `OPI PSRAM`
   - Dash: Board → `ESP32S3 Dev Module`, PSRAM → `OPI PSRAM`, Flash Size →
     `16MB`, Partition Scheme → `Huge APP (3MB No OTA/1MB SPIFFS)`
5. **Build.** With no `secrets.h` compiled in, the display boots into its
   on-glass onboarding wizard — WiFi setup happens on the device: scan the
   QR (or join the `SecuraCV-XXXX` network it shows) from your phone, and
   the setup page opens by itself via the captive-WiFi sheet. If no page
   appears within a few seconds, open **Safari** (or any browser) and go to
   `http://192.168.4.1` — the glass shows this hint too.

**Waveshare's own board entries are recognized** (core 3.x Boards
Manager): picking `Waveshare ESP32-S3-Touch-LCD-4.3` or `…-4.3B` builds
the **dash** flavor, and the 4.3B entry additionally selects the 4.3B pin
map (terminal-block isolated IO — required by the dev playground). The
vendor variant bakes in the right flash/PSRAM, so no Tools tweaks needed.
Any *other* vendor board still stops with a clear error; force a flavor
with `#define CD_BUILD_DASH 0|1` in a `flavor_local.h` next to the sketch
if you know what you're doing — an explicit choice beats inference.

**Dev playground** (guided peripheral bench on the 4.3B —
[doc](../../../../../docs/hardware/dev_playground_43b.md)): run
`../../setup.sh arduino playground`, then build with the IDE (board
`Waveshare ESP32-S3-Touch-LCD-4.3B`) or `arduino-cli compile --profile
playground`. Core 3.x only — the vendor board entry doesn't exist on
2.0.x.

## Build — arduino-cli (zero manual installs)

```bash
cd firmware/projects/canary-display/arduino/canary_display
arduino-cli compile --profile watch-core3    # or dash-core3 / watch / dash
arduino-cli upload  --profile watch-core3 -p /dev/ttyACM0
```

Profiles auto-download the pinned core + libraries into an isolated build,
and the **flavor follows the profile's board** (`dash-core3` really builds
dash firmware — the dispatchers infer it from the board define).

Working from a **git checkout**? `../../setup.sh arduino <watch|dash>` also
works — it writes the git-ignored `flavor_local.h` override (explicit beats
inference), stages a `secrets.h` template you can pre-fill (timezone for
quiet hours lives there, e.g. `#define CD_TZ "EST5EDT,M3.2.0,M11.1.0"`), and
copies `lv_conf.h` into your sketchbook libraries dir for IDE builds.

## What lives where

| In the sketch | Source of truth |
|---|---|
| `canary_display.ino` | `../../src/main.cpp` |
| shared `*.cpp` / `*.h` | `../../src/**` + `../../include/canary/**` (flattened) |
| `config.h` | `../../include/canary/config.h` (composition header) |
| `flavor_config.h` / `pins.h` | generated dispatchers keyed on `flavor_select.h` |
| `flavor_watch.h` / `flavor_dash.h` | `../../../../configs/canary-display/<flavor>/config.h` |
| `pins_watch.h` / `pins_dash.h` / `pins_dash43b.h` | `../../../../boards/<board>/pins/pins.h` |
| `flavor_local.h` | your flavor override, written by `setup.sh` (git-ignored) |
| `lv_conf.h` | `../../include/lv_conf.h` |
| `secrets.h` | your credentials (git-ignored; optional — the wizard covers it) |

Full parity model + regeneration workflow: [`../PARITY.md`](../PARITY.md).
