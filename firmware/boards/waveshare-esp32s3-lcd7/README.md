# Waveshare ESP32-S3-Touch-LCD-7 ("Canary Dash 7" — the big glass)

The 7" desk dashboard of the Canary Display **Nightstand Line**: an
ESP32-S3R8 (16 MB flash / 8 MB octal PSRAM) driving a 7" 800x480 IPS panel
over the S3's parallel RGB565 LCD peripheral, a GT911 5-point capacitive
touch controller, and a CH422G I2C IO expander that owns the touch-reset and
backlight-enable lines. This is the **same electrical architecture as the
4.3" Canary Dash** ([`waveshare-esp32s3-lcd43`](../waveshare-esp32s3-lcd43/README.md))
at the **same 800x480** — only the physical glass is larger. So the Dash RGB
HAL, GT911 driver, and CH422G handling all carry over; the layout is "same
canvas, roomier glass + bigger touch targets."

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz)
- **Flash**: 16 MB
- **PSRAM**: 8 MB octal — **REQUIRED**; the 800x480x2 = ~768 KB framebuffer
  cannot live in internal SRAM. Enable the S3 "bounce buffer", run PSRAM at
  80 MHz, and keep PCLK modest (16-21 MHz) — the 7" is bandwidth-bound at
  high PCLK; verify tearing.
- **Display**: 7" 800x480 IPS, RGB565 parallel (DE mode)
- **Touch**: GT911 5-point capacitive (I2C) — the Nightstand Line wires the
  **full 5-point report** for real multitouch gestures, not just point 0
- **IO expander**: CH422G (I2C, 0x24/0x38) — owns touch-reset (EXIO1) and
  backlight-enable (EXIO2). The GT911 will **not** enumerate until it is
  reset through the expander first; the backlight is on/off only (no PWM), so
  night dimming is dark rendering + scheduled backlight-off, same as the 4.3"
  Dash.
- **microSD**: TF slot (unused in v0.1)
- **CAN/RS485**: broken out (unused)
- **USB**: native USB CDC
- **Power**: mains/USB-C (no battery)

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)) — actually
**Phase 0**: the pin map is compiled from vendor sources but has **not** been
bench-validated. Waveshare revised the 7" RGB porch timings and the touch-INT
GPIO across V1.x revisions — verify against your board's demo
`esp_lcd_rgb_timing_t` and the CH422G bit map before relying on them. The
`dash7` flavor (roomier Dash layout + full 5-point touch gestures), emulator,
and registry wiring are the next slice.

## Used By

| Flavor | Env | Notes |
|--------|-----|-------|
| `canary-display` | `canary-display-dash7` | Dash layout on 7" glass (reuses `dash_ui`) |

Same firmware as the 4.3" Dash — only board pins + OTA product differ. Full
5-point touch gestures are the follow-up (the pins already carry
`TOUCH_MAX_POINTS 5`). Design:
[`docs/hardware/display_nightstand_line.md`](../../../docs/hardware/display_nightstand_line.md) §6.

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
