# Porting SecuraCV Firmware to a New Board

This is the full bring-up guide for adding hardware support — the expansion
of the four-line recipe in [README.md](README.md). It's written so that a
community member with a board on their desk can go from zero to a CI-built,
registered port in one PR, without touching core code.

**Read first:** [ARCHITECTURE.md](ARCHITECTURE.md) (the layering contract —
what may import what) and [HARDWARE.md](HARDWARE.md) (support tiers and what
your port's "supported" badge will mean).

**The golden rule** (learned from Marlin and Klipper, the two largest
hardware-fleet firmware projects): a new board is a **data contribution**.
Pin maps and configs are data; behavior lives in `common/` behind capability
flags. If your port needs an `#ifdef MY_BOARD` inside shared code, stop —
that's a missing capability abstraction, and the PR review will send it
back. Open an issue and we'll design the `HAS_*` flag / HAL seam together.

---

## Before you start

1. **Check the registry.** Your board may already be covered by an existing
   entry or a sibling SKU note ([`boards/boards.json`](boards/boards.json),
   board READMEs).
2. **Decide which flavor drives it.** A board exists to serve a project
   flavor (`canary`, `canary-vision`, `canary-sense`, `canary-display`, …).
   If none fits, you're proposing a new flavor — bigger conversation, open
   a Hardware Support Request issue first.
3. **Have the vendor documentation open.** Every pin assignment must cite a
   source (schematic, wiki, demo code). Pin maps copied from forum posts
   get `VERIFY` notes.

## Step 1 — Board directory

```
firmware/boards/<board-id>/
  README.md          # metadata, constraints, gotchas, support status
  pins/
    pins.h           # ALL pin definitions + capability flags (data only)
```

- `<board-id>`: lowercase, dash-separated, specific enough to be
  unambiguous (`xiao-esp32c6-mr60`, not `esp32c6`).
- `pins.h` **must contain only preprocessor directives** — `#define`,
  `#pragma`, `#if…`. No functions, variables, or logic. CI enforces this
  (`scripts/check_board_registry.py`). Peripheral-specific helper headers
  (like a camera init struct) may live beside it as separate files.
- Required identity defines (CI checks `BOARD_ID` matches the directory):

  ```cpp
  #define BOARD_NAME    "Human-readable name"
  #define BOARD_ID      "<board-id>"
  #define BOARD_VENDOR  "Vendor"
  #define BOARD_MCU     "ESP32-S3"
  #define BOARD_VARIANT "<variant>"
  ```

- Required baseline capability flags — take a position on every one, even
  when the answer is 0 (an undefined flag is an accidental 0):

  ```cpp
  #define HAS_CAMERA      0/1
  #define HAS_MICROPHONE  0/1
  #define HAS_SD_CARD     0/1
  #define HAS_PSRAM       0/1
  #define HAS_USB_CDC     0/1
  #define HAS_WIFI        0/1
  #define HAS_BLE         0/1
  ```

  Add hardware-specific flags as needed (`HAS_DISPLAY`, `HAS_MMWAVE_RADAR`,
  …) — grep existing pin maps before inventing a new name.
- Use `-1` for not-connected pins. Document boot-strapping pins and bus
  conflicts inline.
- Mark every assignment you have **not** confirmed on real hardware with a
  `VERIFY` note. These are honest port currency: they flip to plain
  comments only when a bench test passes (see
  [docs/hardware/display_bench_bringup.md](../docs/hardware/display_bench_bringup.md)
  for what that looks like).
- README.md: specs, constraints, sibling SKUs, and a **Support Status**
  section stating the tier and what retires it. If the physical build has a
  privacy implication (onboard mic on a "quiet by construction" role, say),
  it MUST be documented here — witness devices make promises the hardware
  has to keep.

## Step 2 — Configuration (only if behavior differs)

If the existing flavor config works, skip this step. Otherwise add
`configs/<app-id>/<config-id>/` with `config.h` (+ README) per
[configs/README.md](configs/README.md). Feature flags only — no pins, no
code. Never put board workarounds in a config; that's a capability flag.

## Step 3 — Build environment

Add an `[env:<app-id>-<descriptor>]` section in
`envs/platformio/<app-id>.ini`, binding exactly one board + one config +
one entry point:

```ini
[env:canary-vision-myboard]
extends = common_esp32s3          ; pick the matching MCU base section
board = <pio-board-name>
build_flags =
    ${common_esp32s3.build_flags}
    -I${PROJECT_DIR}/../../boards/<board-id>/pins
    -I${PROJECT_DIR}/../../configs/<app-id>/<config-id>
    -I${PROJECT_DIR}/../../common
```

Copy the nearest existing env section and adjust — the inis carry
hard-earned comments (partition tables, LDF quirks, platform pins); read
them.

The project's `main.cpp` includes
[`core/feature_sanity.h`](common/core/feature_sanity.h) after `pins.h` and
the config — your board's capability flags are cross-checked against the
config's feature flags at compile time. If your build dies with a
`feature_sanity` `#error`, the message names the exact flag to fix. That's
the system working; don't delete the include.

## Step 4 — Register everything

1. **`boards/boards.json`** — add your board: id, name, vendor, mcu,
   flash/psram, pio_board, `"tier": "compile-tested"`, `"tier_evidence":
   ""`, used_by flavors, notes. New ports always enter as
   `compile-tested` — hardware validation is a separate, later claim (see
   [HARDWARE.md](HARDWARE.md)).
2. **`flavors.json`** — if you added a new env, list it in the flavor's
   `build_envs` (and add a `size_guards` entry for each env that OTA-updates,
   with that env's own slot budget — an image bigger than its OTA slot can
   never be installed over the air; see [PARTITIONS.md](PARTITIONS.md)).
3. **`boards/README.md`** — add a row to the Supported Boards table.

## Step 5 — Verify locally

```bash
python3 firmware/scripts/check_board_registry.py     # registry contract
bash firmware/scripts/test_feature_sanity.sh          # sanity-header suite
cd firmware/projects/<project> && pio run -e <your-env>   # it compiles
```

CI runs all of this on your PR — every registered board builds on every PR,
permanently. That's the deal: registration buys your board rot-protection;
in exchange the port must keep building.

## Step 6 — The PR

One PR, containing only the port (board dir + config + env + manifests +
README rows). Don't mix in unrelated fixes. The reviewer will apply the
checklist at the bottom of [HARDWARE.md](HARDWARE.md).

State in the PR body:
- What hardware you tested on, if any (revision matters — vendors respin
  boards silently).
- Which pin assignments are vendor-documented vs. inferred (`VERIFY`
  notes should match this).
- Serial log of a successful boot if you have the hardware.

## Step 7 — After merge: earning a tier

Your board enters as **compile-tested**. To promote it to **community**:
run the relevant bring-up runbook on real hardware and file a **Hardware
Test Report** issue with the results (template guides you through it). A
maintainer flips the tier with your report as `tier_evidence`. Your name
stays on the evidence — you validated that hardware for everyone after you.

---

## Porting to a genuinely new MCU family

Everything above assumes an ESP32-family part (the `common/` tree leans on
ESP-IDF/Arduino-ESP32). A port beyond that (RP2040, STM32…) means a HAL
conversation first — open a Hardware Support Request issue before writing
code. Klipper's `Code_Overview.md` porting recipe is the model we'd follow:
get serial + timers + GPIO up behind the existing HAL seams in
`common/hal/`, then peripherals.
