# Seeed XIAO ESP32-S3 + Round Display for XIAO ("Canary Watch")

A Seeed Studio XIAO ESP32-S3 seated in the Round Display for XIAO — a 39 mm
disc carrying the glance-station glass for the `canary-display` **watch**
flavor. Zero wiring: the XIAO seats into the disc's back socket, like the
Sense/Vision stacks.

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz) on the XIAO ESP32-S3
- **Flash**: 8 MB
- **PSRAM**: 8 MB
- **Display**: GC9A01 1.28" 240x240 round TFT (SPI @ 40 MHz, PWM-dimmable backlight)
- **Touch**: CST816S capacitive, single-point + gestures (I2C, INT on D7)
- **RTC**: PCF8563 on the display board (I2C; unused in v0.1 — NTP is the time source)
- **microSD**: slot on the display board, shared SPI (unused in v0.1)
- **Battery**: JST-1.25 LiPo connector + charger on the display board
- **USB**: Type-C (native USB CDC) on the XIAO

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)): the pin map is
compile-verified and mirrors Seeed's published wiki, but has **not** been
bench-validated. Seeed has revised this board before — verify the
backlight/touch-INT lines against your display revision.
[`docs/hardware/display_bench_bringup.md`](../../../docs/hardware/display_bench_bringup.md)
**Track W** is the runbook that retires this status.

## Constraints & Gotchas

- The GC9A01 reset line is not broken out (tied to board reset) — drivers
  must be configured with "no reset pin". Same for the CST816S touch reset.
- The SPI bus is shared between TFT and microSD; the GC9A01 is write-only
  (MISO belongs to the SD card).
- `BUZZER_PIN` (D0/GPIO1) is a provision for an optional passive piezo —
  unpopulated on stock builds; `FEATURE_CHIME` stays 0 until fitted.
- BOOT button (GPIO0, active LOW) is reserved for a future factory-reset
  long-press — no gesture is wired to it yet. To re-run WiFi setup today,
  use the on-glass path: settings › reset › wifi › forget.

## Used By

| Flavor | Env | Notes |
|--------|-----|-------|
| `canary-display` | `canary-display-watch` | Watch-puck fleet status display |

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
