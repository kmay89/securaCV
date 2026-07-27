# Waveshare ESP32-C6-LCD-1.69 ("Nightstand Line candidate — battery glance node")

A 1.69" 240×280 ST7789V2 IPS glance node on the same single-core ESP32-C6
budget as the [1.47 nightstand](../waveshare-esp32c6-lcd147/README.md), but a
different personality: **no ambient WS2812, no TF slot** — instead an onboard
**QMI8658 6-axis IMU**, a **PCF85063 RTC**, a **3.7 V battery interface with
onboard charging**, and (per Waveshare's "AI speech" marketing) an **ES8311
audio codec + microphone** path. That makes it the *portable/pocket* shape of
the glance family — and, if the mic is confirmed, a **mic-bearing privacy
surface** that must ship mic-off by default, exactly like the
[4.3C dash](../waveshare-esp32s3-lcd43c/README.md).

## Hardware Specifications

- **MCU**: ESP32-C6 (single-core RISC-V, 160 MHz)
- **Flash**: 4 MB · **PSRAM**: none — the ~134 KB 240×280 framebuffer fits
  internal SRAM single-buffered only (same lean rendering budget as the 1.47)
- **Display**: 1.69" 240×280 ST7789V2 IPS, 4-wire SPI — **20-px row offset**
  at rotation 0 (240×320 controller RAM; the 1.47 has the same disease as a
  34-px *column* offset)
- **IMU**: QMI8658 (3-axis accel + 3-axis gyro) on I2C
- **RTC**: PCF85063 on the same I2C bus
- **Audio**: "AI speech" path (ES8311 codec + mic + speaker) per vendor
  marketing — **verify on schematic**; treat as mic-bearing until proven not
- **Power**: USB-C **or 3.7 V MX1.25 lithium battery** with onboard
  charge/discharge management — the only battery board in the C6 glance family
- **Radios**: Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4 (unused in v0.1)
- **Buttons**: BOOT (GPIO9) + PWR
- **Touch**: none on this SKU (the `ESP32-C6-Touch-LCD-1.69` sibling adds a
  CST816T on I2C)

## Pin map status — PARTIAL, do not flash blind

The vendor wiki ([docs.waveshare.com/ESP32-C6-LCD-1.69](https://docs.waveshare.com/ESP32-C6-LCD-1.69))
was **unreachable from the CI sandbox** (network policy), so
[`pins/pins.h`](pins/pins.h) commits only what is verifiable:

- **chip facts**: BOOT strap GPIO9, USB-Serial/JTAG GPIO12/13, UART0 GPIO16/17;
- **the ST7789 SPI block** as the C6-LCD **family-shared map**
  (SCK7 / MOSI6 / CS14 / DC15 / RST21 / BL22 — identical on the 1.47), each
  line still marked `VERIFY`.

Everything else (I2C bus for IMU+RTC, IMU/RTC interrupts, the I2S/codec pins,
battery ADC, PWR key) is **`-1` with a `VERIFY` note** — fill them from the
wiki pin-allocation table / schematic PDF and bench-check before first flash.
The [pin budget](../PIN_BUDGET.md) intentionally reflects only the verified
copper.

## Thermals

Battery charging is the extra heat source this board adds over the 1.47: the
charge path warms the PCB right next to the RTC crystal (drift) and the IMU
(bias). Prefer charging while the backlight is dimmed, and don't calibrate
the IMU during charge. Runtime gauge: the die-temp watchdog
(`FEATURE_DIAGNOSTICS`) that every build ships.

## Support Status

**Registered, pin map partial** — not yet in any build env (`used_by` empty).
Like the 1.47 C6, a future env rides the core-3.x display base
(`docs/hardware/display_nightstand_line.md` §7). Bring-up order: confirm the
pin map from the vendor schematic → add a `canary-display` portrait env
(the 240×280 portrait layout is close kin to the 172×320 one) → bench.

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative for what is
verified; `-1` means unverified, not absent).
