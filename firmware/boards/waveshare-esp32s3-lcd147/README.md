# Waveshare ESP32-S3-LCD-1.47 ("Canary Nightstand", USB-A stick)

USB-A plug-in ambient node for the Canary Display **Nightstand Line**: an
ESP32-S3 (dual-core LX7, Wi-Fi + BLE 5) driving the **same** 1.47" 172x320
ST7789 IPS panel as the C6 sibling over 4-wire SPI, plus one onboard WS2812
addressable RGB LED — the **primary ambient state channel**. With 8 MB PSRAM
and two cores this board can double-buffer and run the full breath/flourish
set; same look as the C6, a richer budget.

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz)
- **Flash**: 16 MB
- **PSRAM**: 8 MB quad — room to double-buffer and animate
- **Display**: 1.47" 172x320 ST7789 IPS, 4-wire SPI (RGB565)
- **Ambient LED**: 1x WS2812-class addressable RGB (GPIO38) — drive via
  **RMT**
- **Backlight**: LEDC PWM on the BL pin — night dimming works
- **Touch**: none — attention is the RGB LED + the BOOT/user button
- **microSD**: onboard slot (separate SPI; unused in v0.1)
- **USB**: USB-A plug wired to the S3's **native USB** (D-=GPIO19,
  D+=GPIO20); default flashing is over USB-Serial/JTAG — mind "USB CDC On
  Boot"
- **Power**: USB-A / pin-header (no battery)

## Panel Quirks

- **34-px column offset**: the 172-wide glass is a window into the ST7789's
  240-wide controller RAM, so every draw needs a 34-px column offset at
  rotation 0 (`TFT_COL_OFFSET`), migrating on rotation.
- **Community fixes** (TFT_eSPI board defs): this board wants the **HSPI**
  port, **BGR** color order, and **active-HIGH** backlight; init at ~8 MHz
  then run fast.

## Sibling SKU

The **ESP32-C6-LCD-1.47** ([`waveshare-esp32c6-lcd147`](../waveshare-esp32c6-lcd147/README.md))
carries the same ST7789 172x320 panel but **every display pin differs**, has
no PSRAM, and is single-core (lean single-buffer rendering). There is also a
non-stick "-LCD-1.47B" variant with a different form factor and map — verify
which one you have.

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)) — actually
**Phase 0**: the pin map is vendor-sourced (Waveshare wiki +
TFT_eSPI/espp/CircuitPython board defs) but has **not** been bench-validated;
the ST7789 HAL, nightstand flavor, emulator, and registry wiring are the next
slice.

## Used By

| Flavor | Env | Notes |
|--------|-----|-------|
| `canary-display` | `canary-display-nightstand-s3` | Nightstand portrait face + WS2812 beacon |

Firmware: `display_1in47.cpp` (HAL), `portrait_ui.cpp` (face), `ambient_led.cpp`
(beacon), `care/hallway.cpp` + `common/color/plumage.cpp` (Hallway mode and the
lamp's light-language). Design:
[`docs/hardware/display_nightstand_line.md`](../../../docs/hardware/display_nightstand_line.md)
(§5e for Hallway mode).

## Enclosure

[`canary_s3_lcd147.scad`](../../../docs/hardware/enclosure/canary_s3_lcd147.scad) —
the hallway stick case: screwless snap fit, thumb-release back for the microSD, a
window that turns the WS2812 into a wall wash, and black PETG with the house mark
inlaid in yellow. **Do not use the C6 case** ([`canary_c6_display.scad`](../../../docs/hardware/enclosure/canary_c6_display.scad));
that board has a USB-C port on its short edge where this one has a USB-A male plug,
which changes the entire plug end.

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
