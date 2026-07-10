# SecuraCV Canary Display — Arduino IDE Build

The Arduino-IDE-buildable **parity** version of the Canary Display firmware —
the same code as the PlatformIO tree in `firmware/projects/canary-display/`,
packaged flat for the Arduino toolchain. One sketch, two flavors:

| Flavor | Board | Panel |
|---|---|---|
| **watch** | Seeed XIAO ESP32-S3 + Round Display for XIAO | GC9A01 1.28" 240×240, CST816S touch |
| **dash**  | ESP32-S3-DevKitC-1 profile + Waveshare 4.3B | 800×480 RGB, GT911 touch, CH422G expander |

> ⚠️ **DEV STATUS (v0.1):** compile/CI-verified; not yet validated on bench
> hardware. See the [bench bring-up runbook](../../../../docs/hardware/display_bench_bringup.md).

## This sketch is generated — don't hand-edit it

The canonical source is the **PlatformIO tree** (`../../src/` + `../../include/`).
The flat `.ino`, `.cpp` and `.h` here are produced from it by `../../setup.sh`,
which flattens the namespaced `#include "canary/…"` paths and resolves the one
name collision the flat layout creates (both the composition header
`canary/config.h` and the flavor `configs/<flavor>/config.h` want to be
`config.h` — the flavor one is staged as `flavor_config.h`). A CI guard
(`check_display_arduino_sync.sh`) fails if the committed sketch drifts from the
canonical tree. **Fix bugs in `../../src`, then regenerate** (below).

## Pick the profile that matches your installed core

The sketch builds on **both** arduino-esp32 major lines, but GFX and NimBLE
split their library majors along the core boundary — so each core line has its
own profiles carrying the right pins:

| Your `esp32` platform | Profiles | GFX | NimBLE-Arduino |
|---|---|---|---|
| **3.x** (what Boards Manager installs by default) | `watch-core3` / `dash-core3` | 1.6.0 | 2.5.0 |
| **2.0.17** (matches the PlatformIO release path) | `watch` / `dash` | 1.4.9 | 1.4.3 |

Just installed the esp32 platform and got the latest? Use the `-core3`
profiles — no downgrade needed. Mixing rows (core 3 + GFX 1.4.9, or core 2 +
NimBLE 2.x) fails the build; switch profiles instead of editing a single pin.

## Build

### 1. Stage the flat sketch for your flavor (required, first time + after edits)

```bash
cd firmware/projects/canary-display
./setup.sh arduino watch     # or: ./setup.sh arduino dash
```

This generates the flat sources and stages the flavor's `pins.h`,
`flavor_config.h`, and a `secrets.h` (edit it with your WiFi/broker; it is
git-ignored). Set your timezone for quiet hours with e.g.
`#define CD_TZ "EST5EDT,M3.2.0,M11.1.0"` in `secrets.h`.

### 2. Toolchain

- **Arduino IDE 2.3+**: add the ESP32 board URL
  `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
  install **esp32 by Espressif Systems** (3.x default is fine — see the profile
  table above), install the libraries below, open `canary_display.ino`, and
  pick the profile matching your flavor + core from the toolbar dropdown
  (board + PSRAM + flash travel with the profile).
- **arduino-cli**:
  ```bash
  arduino-cli compile --profile watch-core3    # or dash-core3 / watch / dash
  arduino-cli upload  --profile watch-core3 -p /dev/ttyACM0
  ```

Libraries (also declared per-profile in `sketch.yaml`): `lvgl @ 8.4.0`,
`PubSubClient`, `Crypto`, `ArduinoJson`, plus the core-matched pair from the
table above — `GFX Library for Arduino` @ **1.6.0** (core 3) or **1.4.9**
(core 2, EXACT), and `NimBLE-Arduino` @ **2.5.0** (core 3) or **1.4.3**
(core 2).

### 3. LVGL config

`lv_conf.h` is generated into the sketch root; the profiles compile LVGL with
`LV_CONF_INCLUDE_SIMPLE` so it is picked up from there. If the IDE can't find
it, ensure the sketch folder is on the include path (it is by default for the
sketch's own sources).

## What lives where

| In the sketch | Source of truth |
|---|---|
| `canary_display.ino` | `../../src/main.cpp` |
| shared `*.cpp` / `*.h` | `../../src/**` + `../../include/canary/**` (flattened) |
| `config.h` | `../../include/canary/config.h` (composition header) |
| `flavor_config.h` | `../../../../configs/canary-display/<flavor>/config.h` |
| `pins.h` | `../../../../boards/<board>/pins/pins.h` |
| `lv_conf.h` | `../../include/lv_conf.h` |
| `secrets.h` | your credentials (git-ignored) |

Full parity model + regeneration workflow: [`../PARITY.md`](../PARITY.md).
