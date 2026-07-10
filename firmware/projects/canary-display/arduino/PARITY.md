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

`.github/workflows/firmware.yml` builds **both** flavors with `arduino-cli`
(esp32 core 2.0.17, GFX 1.4.9, LVGL v8) — the same job class as the canary-wap
Arduino build. This is what proves the generated sketch actually compiles;
until a flavor is green here, treat its parity as *claimed, not proven*
(repo convention: land code CI-green, flip the dashboard on proof).

## Core-version constraint (load-bearing)

The Arduino build **must** use arduino-esp32 **2.0.x**. GFX 1.4.9 (pinned for
the panels) does not build on core 3.x, and 1.5+ needs 3.x — the same reason
the PlatformIO env pins `espressif32 @ 6.9.0`. The `sketch.yaml` profiles and
the CI job both pin 2.0.17; do not bump without also moving GFX/LVGL and
re-validating both packagings.
