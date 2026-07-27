# Waveshare ESP32-S3-Touch-LCD-1.69 ("Canary Nightstand Touch")

The **touch member of the nightstand family**: a 1.69" 240×280 ST7789V2 IPS
panel with a **CST816T capacitive touch** layer on an ESP32-S3R8 (16 MB flash,
8 MB **octal** PSRAM — the roomiest budget in the family), plus a **QMI8658
6-axis IMU**, a **PCF85063 RTC**, and a **3.7 V MX1.25 battery interface with
onboard charging**. Where the 1.47" boards speak through a WS2812 and the BOOT
button, this one has no LED and doesn't need the button: **the glass itself is
both the ambient surface and the input** — the standardized tap / long-press
ladder (wake, peek, acknowledge, the first-boot commissioning wizard) works by
finger, exactly as on the watch and dash.

## Hardware Specifications

- **MCU**: ESP32-S3R8, dual-core LX7 @ 240 MHz
- **Flash**: 16 MB · **PSRAM**: 8 MB octal (`qio_opi`; **GPIO33–37 reserved**)
- **Display**: 1.69" 240×280 ST7789V2 IPS, 4-wire SPI — **20-px row offset**
  at rotation 0 (240×320 controller RAM; the 1.47" siblings have the same
  disease as a 34-px *column* offset)
- **Touch**: CST816T capacitive (I2C 0x15), INT/RST wired
- **IMU**: QMI8658 (3-axis accel + 3-axis gyro) on the shared I2C bus —
  declared (`HAS_IMU 1`), driver is room-to-grow (raise/tilt wake later)
- **RTC**: PCF85063 on the same bus (same 0x51 family the dash RTC layer probes)
- **Power**: USB-C **or 3.7 V MX1.25 lithium battery**, onboard charging, PWR
  button
- **Radios**: Wi-Fi (2.4 GHz), BLE 5
- **No** camera, microphone, TF slot, or addressable LED

## Firmware — the `touch169` flavor (full nightstand parity + touch)

Runs the nightstand app (`CD_FLAVOR_NIGHTSTAND`: `display_1in47.cpp` ST7789
HAL + `portrait_ui.cpp` portrait face) via env **`canary-display-touch169`**
(OTA product `securacv-canary-display-touch169`), config
[`configs/canary-display/touch169`](../../configs/canary-display/touch169/config.h).
Full feature parity with the S3 nightstand — MQTT fleet witness column,
on-device Ed25519 chain verify, the **standardized pre-WiFi commissioning
wizard** (per-session AP password, join-QR, scan portal), honest night
blackout, gentle wake, signed pull-OTA — **plus `FEATURE_TOUCH 1`**: the HAL
compiles the CST816 path and `main.cpp`'s existing touch ladder just works.
The portrait face reads its geometry from the pin header (`TFT_WIDTH` ×
`TFT_HEIGHT`), so the 240×280 glass lays out proportionally to the 172×320
original. No WS2812: `FEATURE_AMBIENT_LED 0`, the wash + backlight ladder is
the beacon.

## Pin map status — vendor-demo map, VERIFY before first flash

The vendor wiki was unreachable from the CI sandbox, so
[`pins/pins.h`](pins/pins.h) carries the widely-mirrored demo-code map
(LCD SCK6/MOSI7/CS5/DC4/RST8/BL15; touch SDA11/SCL10/INT14/RST13) with every
line `VERIFY`-tagged, and the low-confidence power lines (`PWR_KEY_PIN`) as
`-1`. Check against the
[schematic PDF](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69/ESP32-S3-Touch-LCD-1.69-Sch.pdf)
on first bring-up. The [pin budget](../PIN_BUDGET.md) counts only what is
declared.

## Thermals

Two heat sources stack on this small PCB: **battery charging** and the S3 +
octal-PSRAM render load. Charging warms the board next to the PCF85063
crystal (clock drift) and the QMI8658 (bias) — charge with the backlight
dimmed and don't calibrate the IMU mid-charge. The touch layer sits on the
glass: CST816-family controllers drift when hot, so keep it out of direct
sun. Runtime gauge: the die-temp watchdog (`FEATURE_DIAGNOSTICS`-class
passive watchdog) plus the night backlight floor, which is also the thermal
relief valve.

## Support Status

**Compile-tested** (CI builds `canary-display-touch169`); bench validation
pending — day-one bring-up follows the nightstand Track N runbook
(`docs/hardware/display_bench_bringup.md`), with the pin-map VERIFY pass
first. Battery gauge (BAT_ADC calibration), the QMI8658 driver, and PWR-key
handling are staged follow-ups.

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative for what is
declared; `-1` means unverified, not absent).
