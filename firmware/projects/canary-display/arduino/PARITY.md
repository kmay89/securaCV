# Canary Display — PlatformIO ⇄ Arduino parity

This project ships **two build packagings of one firmware**, mirroring the
repo's dual-tree policy (`firmware/PARITY_PLAN.md`): the modular **PlatformIO**
tree (`src/` + `include/`, the canonical source of truth) and this flat
**Arduino IDE** sketch (`arduino/canary_display/`). They are the *same code* —
the Arduino sketch is mechanically generated from the PlatformIO tree, not a
hand-maintained fork, so the two cannot silently diverge.

## Why generated, not hand-copied

The display firmware is modular and namespaced (`#include "canary/fleet/…"`,
`#include "canary/net/…"`) with a composition header whose whole trick is an
angle-bracket `#include <config.h>` that resolves to the *flavor* config via
`-I` paths. Arduino's flat sketch model has none of that — one folder, quote
includes, sketch-root search. Hand-porting would mean maintaining ~45 files in
two include dialects and re-doing it on every change. Instead, `setup.sh`
transforms the canonical tree deterministically:

1. `src/main.cpp` → `canary_display.ino` (it already *is* `setup()/loop()`).
2. `src/**/*.cpp` + `include/canary/**/*.h` → flat sketch-root files.
3. `#include "canary/…/x.h"` → `#include "x.h"` (basename).
4. **Collision fix:** both `include/canary/config.h` (composition) and
   `configs/<flavor>/config.h` (flavor) flatten to `config.h`. The flavor file
   is staged as **`flavor_config.h`** and `#include <config.h>` is rewritten to
   it. The composition header keeps the name `config.h`.
5. Per-flavor board `pins.h` + `flavor_config.h` + `secrets.h` are staged.
6. The shared `firmware/common` code the display consumes (boot banner, the
   MAC-free device pseudonym, the signed pull-OTA engine + release key) is
   staged flat too, with its `boot/` / `identity/` include prefixes stripped —
   so the sketch is **self-contained** and needs no `--libraries` path (the
   same committed-copy approach the canary-wap sketch uses).

## What is committed vs staged

- **Committed** (flavor-agnostic, sync-guarded): `canary_display.ino`, the
  shared `*.cpp`/`*.h`, `config.h`, `lv_conf.h`, `secrets.ci.h`, plus the hand-
  written `sketch.yaml` / `README.md` / `.gitignore`.
- **Staged, git-ignored** (per-flavor / secret): `pins.h`, `flavor_config.h`,
  `secrets.h` — produced by `./setup.sh arduino <watch|dash>`.

So a fresh checkout has the whole sketch except the three flavor/secret files,
which one `setup.sh` invocation stages — the same "run setup before first
build" step the canary-wap Arduino tree uses.

## Regeneration workflow (making a change)

1. Edit the **canonical** source under `src/` or `include/`.
2. Regenerate the committed sketch: from `firmware/projects/canary-display/`,
   run `./setup.sh regen`.
3. Commit the canonical change **and** the regenerated sketch together.

## The sync guard

`firmware/scripts/check_display_arduino_sync.sh` runs `./setup.sh regen` in a
clean tree and fails if the committed sketch differs — i.e. if someone edited
`src/` without regenerating, or hand-edited the sketch. It is wired into CI
alongside the OTA/CSI sync guards.

## The compile gate

`.github/workflows/firmware.yml` builds **both flavors × both core lines**
(2×2 matrix) with `arduino-cli` — the same job class as the canary-wap Arduino
build. This is what proves the generated sketch actually compiles; until a
flavor is green here, treat its parity as *claimed, not proven* (repo
convention: land code CI-green, flip the dashboard on proof).

## Core-version matrix (load-bearing)

The Arduino packaging builds on **both** arduino-esp32 major lines; the source
carries the differences (`include/canary/hal/core_compat.h` shims LEDC + task-
watchdog, `src/net/chirp_scan.cpp` gates the NimBLE scan-callback API on
`ESP_ARDUINO_VERSION_MAJOR`). What is NOT interchangeable is the library set —
GFX and NimBLE split their majors along the core boundary:

| arduino-esp32 core | GFX Library for Arduino | NimBLE-Arduino | Profile suffix |
|---|---|---|---|
| **2.0.17** (matches PlatformIO `espressif32 @ 6.9.0` — the bench-validated release path) | 1.4.9 (1.5+ needs core 3) | 1.4.3 (2.x needs core 3) | `watch` / `dash` |
| **3.x** (Boards Manager default) | 1.6.0 (won't build on core 2) | 2.5.0 (won't build on core 2) | `watch-core3` / `dash-core3` |

LVGL 8.4.0, PubSubClient, Crypto, and ArduinoJson are core-agnostic and shared.
Always move a whole row at once — mixing lines fails the build. The canonical
PlatformIO tree stays on the 2.0.17 line (that is what ships); the core-3 row
exists so a stock Boards Manager install builds without a downgrade.
