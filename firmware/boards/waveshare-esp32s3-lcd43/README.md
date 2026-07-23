# Waveshare ESP32-S3-Touch-LCD-4.3 ("Canary Dash")

Integrated wall/desk dashboard board for the `canary-display` **dash**
flavor: an ESP32-S3 driving a 4.3" 800x480 IPS panel over the S3's parallel
RGB565 LCD peripheral.

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz)
- **Flash**: 16 MB
- **PSRAM**: 8 MB octal — hosts the 800x480 framebuffer
- **Display**: 4.3" 800x480 IPS, RGB565 parallel (DE mode)
- **Touch**: GT911 5-point capacitive (I2C)
- **IO expander**: CH422G (I2C) — owns touch-reset and backlight-enable
  (backlight is on/off only, no PWM dimming)
- **microSD**: TF slot (unused in v0.1)
- **CAN/RS485**: terminal block (unused)
- **USB**: Type-C (native USB CDC)
- **Power**: mains/USB-C (no battery)

## Sibling SKUs

The pin map is expected to cover (bench-verify per SKU):

- **ESP32-S3-Touch-LCD-4.3B** — same panel/pins, but the terminal-block
  IO differs enough that it has its own board dir now:
  [`waveshare-esp32s3-lcd43b`](../waveshare-esp32s3-lcd43b/) (isolated
  DI/DO on the CH422G, dedicated CAN on 15/16, RS485 on the console
  UART, no USB_SEL bit). Use that map — and the dev playground mode —
  when the terminals matter; this one still covers the B's panel.
- **ESP32-S3-Touch-LCD-4.3C** ("AI voice") — same RGB control/data pins and
  I2C, but an ST7701 panel controller (verify timings; its LCD_RST rides an
  extra CH422G bit) and an ES8311/ES7210 audio stack this firmware never
  initializes. ⚠️ The C variant carries a **dual-MIC array** — physically
  present even though untouched — so the "no microphone by construction"
  posture does **not** hold on that SKU. Prefer the plain 4.3/4.3B for the
  canonical Canary Dash. The C now has its **own first-class board map and
  mic contract** as a distinct privacy surface —
  [`waveshare-esp32s3-lcd43c`](../waveshare-esp32s3-lcd43c/README.md) +
  [`display_mic_variant.md`](../../../docs/hardware/display_mic_variant.md).

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)): the pin map is
compile-verified against Waveshare's wiki/demo sources but has **not** been
bench-validated. Waveshare does not publish a full mechanical/electrical
drawing — verify RGB timings and expander bits against your board revision.
[`docs/hardware/display_bench_bringup.md`](../../../docs/hardware/display_bench_bringup.md)
**Track D** is the runbook that retires this status.

## Used By

| Flavor | Env | Notes |
|--------|-----|-------|
| `canary-display` | `canary-display-dash` | 4.3" dash fleet status display |

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
