# Board Definitions

Pin maps and board-specific wiring live here. The machine-readable source of
truth is [`boards.json`](boards.json) — the board-level counterpart of
[`../flavors.json`](../flavors.json). CI
([`../scripts/check_board_registry.py`](../scripts/check_board_registry.py))
keeps this README, the registry, the board directories, and the build
environments consistent, so the table below cannot silently rot.

## Supported Boards

| Board ID | MCU | Tier | Used by | Description |
|----------|-----|------|---------|-------------|
| `xiao-esp32s3-sense` | ESP32-S3 | verified | canary, canary-wap | Seeed XIAO with camera, mic, SD slot — the primary witness board |
| `xiao-esp32s3` | ESP32-S3 | verified | canary-vision | Seeed XIAO (plain, non-Sense) — Vision AI host (the "Grove Vision AI V2 Kit" pairing) |
| `xiao-esp32c3` | ESP32-C3 | verified | canary-vision | Seeed XIAO C3 — Vision AI host (stacks on the module's XIAO socket) |
| `esp32-c3` | ESP32-C3 | compile-tested | canary-vision | Generic C3 dev board for Vision AI over a Grove cable |
| `xiao-esp32c6-mr60` | ESP32-C6 | compile-tested | canary-sense | Seeed MR60BHA2 60 GHz mmWave kit (radar witness host) |
| `xiao-esp32s3-round` | ESP32-S3 | compile-tested | canary-display | XIAO + Round Display — "Canary Watch" glance puck |
| `waveshare-esp32s3-lcd43` | ESP32-S3 | compile-tested | canary-display | Waveshare 4.3" touch panel — "Canary Dash" |

**Tiers** (defined in [`../HARDWARE.md`](../HARDWARE.md)):
**verified** = CI-built *and* validated on real hardware by a maintainer;
**community** = CI-built, validated on real hardware by a community member
(linked test report); **compile-tested** = builds in CI, not yet validated
on hardware. Every tier above compile-tested requires `tier_evidence` in
`boards.json` — a repo doc or issue proving the hardware test.

## Directory Structure

Each board directory follows this pattern:

```
boards/
  boards.json           # Machine-readable board registry (single source of truth)
  <board-id>/
    README.md           # Board metadata, constraints, support status
    pins/
      pins.h            # Main pin definitions (data only — CI-enforced)
      camera.h          # Camera pin config (if applicable)
      ...               # Other peripheral-specific pin files
    variants/           # Optional board revisions
```

## Adding a New Board

See [`../PORTING.md`](../PORTING.md) for the full bring-up guide and
submission checklist. Short version:

1. Create `boards/<board-id>/` with `README.md` and `pins/pins.h`
2. Register the board in [`boards.json`](boards.json) (tier: `compile-tested`)
3. Add a build environment in `envs/platformio/` and, if it's a new flavor,
   an entry in `../flavors.json`
4. Update the table in this README
5. Run `python3 firmware/scripts/check_board_registry.py` — CI runs it on
   every PR

## Pin Definition Rules

- `pins.h` must ONLY contain preprocessor directives (`#define`, `#pragma`,
  `#if...`) — no logic, functions, or variables. **CI enforces this.**
- Use `-1` for pins that are not connected
- `BOARD_ID` must match the directory name exactly (CI-enforced)
- Include capability flags for conditional compilation
- Document any pin conflicts or boot strapping requirements

## Board Capability Flags

Baseline flags — every board must take a position on all of these, even if
the answer is 0 (an undefined flag is an accidental 0; CI enforces
presence):

```cpp
#define HAS_CAMERA          0/1
#define HAS_MICROPHONE      0/1
#define HAS_SD_CARD         0/1
#define HAS_PSRAM           0/1
#define HAS_USB_CDC         0/1
#define HAS_WIFI            0/1
#define HAS_BLE             0/1
```

Additional flags as the hardware warrants (`HAS_GNSS_UART`,
`HAS_TAMPER_INPUT`, `HAS_VISION_AI`, `HAS_DISPLAY`, `HAS_TOUCH`, `HAS_RTC`,
`HAS_BATTERY`, `HAS_BACKLIGHT_PWM`, `HAS_MMWAVE_RADAR`, ...). Feature code
gates on these via
[`../common/core/feature_sanity.h`](../common/core/feature_sanity.h) — a
config that enables a feature the board can't carry fails the build with an
actionable message instead of failing at runtime.
