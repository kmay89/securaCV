# Waveshare ESP32-C6-LCD-1.47 ("Canary Nightstand")

Pin-header 1.47" glance/ambient node for the Canary Display **Nightstand
Line**: an ESP32-C6 (single-core RISC-V, Wi-Fi 6 + BLE 5 + 802.15.4)
driving a 1.47" 172x320 ST7789 IPS panel over 4-wire SPI, plus one onboard
WS2812 addressable RGB LED that is the **primary ambient state channel** —
the point of color you read across a dark room from bed.

## Hardware Specifications

- **MCU**: ESP32-C6 (single-core RISC-V, 160 MHz)
- **Flash**: 4 MB
- **PSRAM**: none — the ~110 KB framebuffer fits internal SRAM, but there is
  no headroom to double-buffer; the C6 renders lean (single buffer,
  dirty-region LVGL)
- **Display**: 1.47" 172x320 ST7789 IPS, 4-wire SPI (RGB565)
- **Ambient LED**: 1x WS2812-class addressable RGB (GPIO8) — drive via the
  **RMT** peripheral, never bit-banged (a bit-banged strobe glitches colors
  under Wi-Fi load on this single-core part)
- **Backlight**: LEDC PWM on the BL pin — night dimming works
- **Touch**: none — attention is the RGB LED + the BOOT/user button
- **microSD**: TF slot on the shared SPI bus (unused in v0.1)
- **Radios**: Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4 (Thread/Zigbee radio unused
  in v0.1)
- **USB**: USB-Serial/JTAG (no native USB-OTG on the C6)
- **Power**: USB-C / pin-header (no battery)

## Display Gotcha — the 34-px column offset

The 172-wide glass is a window into the ST7789's 240-wide controller RAM, so
every draw needs a **34-px column offset** at rotation 0 (`TFT_COL_OFFSET`).
The offset migrates to the opposite edge when rotated — recompute per
rotation in the HAL.

## Sibling SKU

The **ESP32-S3-LCD-1.47** ([`waveshare-esp32s3-lcd147`](../waveshare-esp32s3-lcd147/README.md))
carries the **same ST7789 172x320 panel** but **every display pin differs**,
and it has 8 MB PSRAM + two cores (so it can double-buffer and animate
richly). Same look, two budgets — do not share a pin map.

## Support Status

**Compile-tested** in name (see [`boards.json`](../boards.json)), but the C6
build is **toolchain-blocked** today: the ESP32-C6 needs arduino-esp32 3.x
(the pioarduino fork, as `canary-sense` pins), while `canary-display`'s
graphics stack is pinned to `GFX Library for Arduino@1.4.9` — the last
**core-2.x**-compatible release. So there is **no C6 env in the CI build
matrix yet**; a core-3.x display base (GFX@^1.5.0 + an LVGL/NimBLE 3.x audit)
is the gating slice. Verify the backlight/RGB-LED lines and the ST7789 offset
against your board revision before relying on them.

## Used By

The `canary-display` **nightstand** firmware (`display_1in47.cpp` HAL,
`portrait_ui.cpp`, `ambient_led.cpp`) is already C6-ready — single internal
buffer (no PSRAM), RMT-driven LED. It runs today on the S3 sibling
([`waveshare-esp32s3-lcd147`](../waveshare-esp32s3-lcd147/README.md), env
`canary-display-nightstand-s3`); `used_by` stays empty here until the
core-3.x display base lands. See
[`docs/hardware/display_nightstand_line.md`](../../../docs/hardware/display_nightstand_line.md) §7.

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
