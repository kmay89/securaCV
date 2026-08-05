# Waveshare ESP32-C3-LCD-1.47 ("Canary Nightlight")

Pocket 1.47" clock/lamp/companion node for the Canary Display **Nightstand
Line**: an ESP32-C3 (single-core RISC-V, Wi-Fi 4 + BLE 5) driving a 1.47"
portrait ST7789T IPS panel over 4-wire SPI. This is the board inside the
**C3 pocket display case** (`docs/hardware/enclosure/canary_c3_lcd147.scad`)
and the home of the **nightlight** flavor — the glass itself is the lamp;
there is no WS2812 on this board.

## Hardware Specifications

- **MCU**: ESP32-C3FH4 (single-core RISC-V, 160 MHz)
- **Flash**: 4 MB (in-package)
- **PSRAM**: none — the ~113 KB framebuffer fits internal SRAM, but there is
  no headroom to double-buffer; render lean (single buffer, dirty-region
  LVGL), exactly like the C6 nightstand
- **Display**: 1.47" portrait ST7789T IPS, 4-wire SPI (RGB565). The vendor
  demo and factory firmware drive it as **180x320 with a 30-px column
  offset** (marketing says 172x320 — see the geometry gotcha below)
- **Backlight**: PWM **via the EXIO expander's I2C register** — there is no
  LEDC backlight pin (gotcha below)
- **IMU**: QMI8658 6-axis on the shared I2C bus, INT on GPIO2 (unused in
  v0.1; tap-to-react is the scoped follow-up)
- **Touch**: none — attention is the BOOT/user button
- **microSD**: TF slot on the shared SPI bus, CS via EXIO (unused in v0.1)
- **Radios**: Wi-Fi 4 (2.4 GHz), BLE 5
- **USB**: USB-Serial/JTAG (no native USB-OTG on the C3)
- **Power**: USB-C (VBUS→VSYS diode-OR + MP1605 buck; no battery charger IC)

## The EXIO gotcha — CS, RST and the backlight are not GPIOs

Unlike its C6/S3 siblings, this board hangs LCD CS, LCD RST, SD CS **and
the backlight PWM** off Waveshare's I2C IO expander (addr `0x24`, shared
I2C bus on SCL=GPIO3 / SDA=GPIO4 with the QMI8658). Brightness is a
one-byte register write, 0–255, into the expander's PWM output —
`hal/display_1in47.cpp` takes this path when the pin map defines
`TFT_USE_EXIO`. Consequence: the fine 13-bit night-floor profile the LEDC
boards enjoy compresses to 256 steps here; the nightlight flavor caps duty
at 50% anyway (heat, closed PETG case), so the loss is theoretical.

## Display gotchas — geometry and MADCTL

- The vendor demo (github.com/waveshareteam/ESP32-C3-LCD-1.47) and the
  factory firmware drive the glass as **180x320, column offset 30**
  (30+180+30 = the ST7789's 240-wide RAM). Waveshare's marketing page says
  172x320. We follow the shipping code; if a board revision proves to be
  the 172 glass, the symptom is a 4-px junk band on each long edge and the
  fix is two lines in `pins/pins.h` (172 / offset 34).
- The vendor init ends with **MADCTL 0x48 (MX | BGR) + INVON** — mirrored-X
  scan and BGR order, which the stock ST7789 rotation-0 init does not set.
  `TFT_PANEL_VENDOR_INIT` makes the HAL replay the vendor table. A swapped
  panel shows blue where it should show yellow — bench-verify colors on
  first boot.

## Sibling SKUs

The **ESP32-C6-LCD-1.47** ([`waveshare-esp32c6-lcd147`](../waveshare-esp32c6-lcd147/README.md))
and **ESP32-S3-LCD-1.47** ([`waveshare-esp32s3-lcd147`](../waveshare-esp32s3-lcd147/README.md))
share the 1.47" portrait format but differ on every axis that matters here:
both drive CS/RST/BL from real GPIOs (no EXIO), both carry a WS2812, and
both are 172x320/offset-34. Do not share a pin map.

Unlike the C6 (which needs arduino-esp32 3.x), the **C3 builds on the
core-2.x display base** — the same espressif32@6.9.0 / GFX 1.4.9 / LVGL 8.4
stack as the S3 envs — so it slots straight into the CI build matrix.

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)): pin map compiled
from Waveshare's engineering-sample demo repo and the schematic PDF it
ships (both agree), NOT yet validated on bench hardware. Verify the panel
geometry, MADCTL/color order and EXIO behavior against your board revision.

## Used By

The `canary-display` **nightlight** flavor (env `canary-display-nightlight-c3`,
OTA product `securacv-canary-display-nightlight-c3`): a 7-segment bedside
clock with a canary companion and a hard 50%-duty lamp. See
[`firmware/configs/canary-display/nightlight/`](../../configs/canary-display/nightlight/)
and [`docs/hardware/display_nightstand_line.md`](../../../docs/hardware/display_nightstand_line.md).

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative).
