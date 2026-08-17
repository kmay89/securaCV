# Board Definitions

Pin maps and board-specific wiring live here. The machine-readable source of
truth is [`boards.json`](boards.json) — the board-level counterpart of
[`../flavors.json`](../flavors.json). CI
([`../scripts/check_board_registry.py`](../scripts/check_board_registry.py))
keeps this README, the registry, the board directories, and the build
environments consistent, so the table below cannot silently rot.

How full is each board — pins committed vs free, peripheral demand, thermal
notes? See the generated gauge: [`PIN_BUDGET.md`](PIN_BUDGET.md)
(regenerate with `python3 firmware/scripts/pin_budget.py --write`; CI fails
if it drifts from the pin maps).

## Supported Boards

| Board ID | MCU | Tier | Used by | Description |
|----------|-----|------|---------|-------------|
| `xiao-esp32s3-sense` | ESP32-S3 | verified | canary, canary-wap | Seeed XIAO with camera, mic, SD slot — the primary witness board |
| `xiao-esp32s3` | ESP32-S3 | verified | canary-vision | Seeed XIAO (plain, non-Sense) — Vision AI host (the "Grove Vision AI V2 Kit" pairing) |
| `xiao-esp32c3` | ESP32-C3 | verified | canary-vision | Seeed XIAO C3 — Vision AI host (stacks on the module's XIAO socket) |
| `esp32-c3` | ESP32-C3 | compile-tested | canary-vision | Generic C3 dev board for Vision AI over a Grove cable |
| `esp32c3-super-mini` | ESP32-C3 | compile-tested | canary-vision | The most-owned sub-$3 board (white-label) — Vision AI host over a Grove cable, same wiring as the generic DevKit entry |
| `esp32cam-ai-thinker` | ESP32 | compile-tested | canary | AI-Thinker ESP32-CAM — the most-owned camera board in existence; camera PEEK + Wi-Fi CSI + SD, UART-only flashing (env `esp32cam`) |
| `esp32-wroom-devkit` | ESP32 | compile-tested | canary | Generic WROOM-32 DevKit family (DevKitC/DOIT/NodeMCU-32S/clones) — Wi-Fi CSI presence witness, no camera/mic/SD (env `esp32-wroom`) |
| `freenove-esp32s3-cam` | ESP32-S3 | compile-tested | canary | Freenove FNK0085, the default Amazon S3 camera kit — S3-EYE camera map, SD slot 1-bit-SDMMC-only so SD stays off (env `freenove-s3`) |
| `xiao-esp32c6-mr60` | ESP32-C6 | compile-tested | canary-sense | Seeed MR60BHA2 60 GHz mmWave kit (radar witness host) |
| `xiao-esp32s3-round` | ESP32-S3 | compile-tested | canary-display | XIAO + Round Display — "Canary Watch" glance puck |
| `waveshare-esp32s3-lcd43` | ESP32-S3 | compile-tested | canary-display | Waveshare 4.3" touch panel — "Canary Dash" |
| `waveshare-esp32s3-lcd43b` | ESP32-S3 | compile-tested | canary-display | Waveshare 4.3B (isolated DI/DO, RS485, CAN, I2C header) — "Canary Dash B" / dev playground host |
| `waveshare-esp32s3-lcd43c` | ESP32-S3 | compile-tested | canary-display | Waveshare 4.3C ("AI voice") — the MIC-BEARING dash: a distinct privacy surface, mic off by default (docs/hardware/display_mic_variant.md) |
| `xiao-esp32c6-sentinel` | ESP32-C6 | compile-tested | — (Phase 0) | Canary Sentinel Standard/Heavy head — MR60BHA2 radar + PIR + lux + WiFi/BLE; fusion core host-tested, on-device build/bench pending |
| `xiao-esp32c3-sentinel-lite` | ESP32-C3 | compile-tested | — (Phase 0) | Canary Sentinel Lite — PIR + lux + WiFi/BLE, no radar (honest tier limit); fusion core host-tested, on-device build/bench pending |
| `waveshare-esp32c3-lcd147` | ESP32-C3 | compile-tested | canary-display | Nightstand Line pocket node ("Canary Nightlight") — ST7789T portrait, vendor-demo geometry 180x320/offset-30, CS/RST/SD-CS/**backlight** all behind the I2C EXIO expander (0x24), QMI8658 IMU, NO WS2812 (the glass is the lamp); `nightlight` flavor on the core-2.x base (env `canary-display-nightlight-c3`), 50%-duty backlight cap in the HAL, bench pending |
| `waveshare-esp32c6-lcd147` | ESP32-C6 | compile-tested | canary-display | Nightstand Line 1.47" — ST7789 172x320 SPI + 1x WS2812 ambient LED, single-core C6, no PSRAM; `nightstand` flavor on the core-3.x base (env `canary-display-nightstand-c6`), bench pending (docs/hardware/display_nightstand_line.md) |
| `waveshare-esp32c6-lcd169` | ESP32-C6 | compile-tested | — | Nightstand Line candidate 1.69" (battery/pocket) — ST7789V2 240x280 (20-px row offset), QMI8658 IMU + PCF85063 RTC, battery charging, possible mic ("AI speech" — treat as mic-bearing); PARTIAL pin map, vendor wiki unreachable from CI sandbox, unverified pins are -1 VERIFY |
| `waveshare-esp32s3-lcd147` | ESP32-S3 | compile-tested | canary-display | Nightstand Line 1.47" (USB-A stick) — same ST7789 172x320 panel, all pins differ, 8 MB PSRAM (can animate); `nightstand` flavor (env `canary-display-nightstand-s3`), bench pending |
| `waveshare-esp32s3-touch-lcd169` | ESP32-S3 | compile-tested | canary-display | Nightstand Line TOUCH member 1.69" (battery) — ST7789V2 240x280 (20-px row offset) + CST816T touch, QMI8658 IMU + PCF85063 RTC, battery charging, S3R8 16MB/8MB octal; `touch169` flavor (env `canary-display-touch169`), pin map VERIFY-tagged (vendor demo layout), bench pending |
| `waveshare-esp32s3-lcd7` | ESP32-S3 | compile-tested | canary-display | Nightstand Line 7" big glass — 800x480 RGB + GT911 5-point touch + CH422G; electrically the 4.3" Dash at 7", reuses the dash HAL (env `canary-display-dash7`), bench pending |
| `waveshare-esp32s3-amoled206` | ESP32-S3 | compile-tested | — (Phase 0) | **The Tin Can** wrist board — 2.06" 410x502 AMOLED (CO5300 QSPI, brightness is a panel command), FT3168 touch (NOT the CST9220 the store copy claims), QMI8658 + PCF85063 + AXP2101 + Li-po, 32MB/8MB octal. **No vibration motor** — a DRV2605L + LRA on the I²C port is required hardware. Mics present and deliberately unused. Pin map from the vendor tree, bench pending |
| `waveshare-esp32s3-amoled241` | ESP32-S3 | compile-tested | canary-display | **The flagship glance glass** (V2, metal case) — 2.41" 450x600 AMOLED (RM690B0 QSPI, 16-px column window, brightness is a panel command), FT6336 touch with INT behind the TCA9554 (poll), QMI8658 + PCF85063, dedicated SDMMC microSD, ETA6098 charger (no I2C PMU) + power latch on GPIO16 + readable PWR button on GPIO15, 16MB/8MB octal; `amoled241` flavor (env `canary-display-amoled241`). Pin map from the bench-tested CircuitPython board def + the V2 case label, bench pending |

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
