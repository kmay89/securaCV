# Waveshare ESP32-S3-Touch-LCD-4.3B ("Canary Dash B" / Dev Playground host)

The industrial-IO sibling of the [4.3 dash board](../waveshare-esp32s3-lcd43/):
same ESP32-S3 + 4.3" 800x480 IPS panel + GT911 touch + CH422G expander,
plus a field-wiring terminal block — isolated digital IO, RS485, CAN, an
I2C sensor header, and a 6–36 V supply. Because **no raw ESP32 GPIO is
broken out**, every external wire lands on an isolated, buffered, or bused
interface — which is what makes this SKU the host for the
**dev playground mode**
([`docs/hardware/dev_playground_43b.md`](../../../docs/hardware/dev_playground_43b.md)).

Vendor docs: [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B) ·
[docs platform](https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B) ·
[product page](https://www.waveshare.com/esp32-s3-touch-lcd-4.3b.htm)

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz)
- **Flash**: 16 MB · **PSRAM**: 8 MB octal (hosts the 800x480 framebuffer)
- **Display**: 4.3" 800x480 IPS, RGB565 parallel (DE mode)
- **Touch**: GT911 5-point capacitive (I2C)
- **IO expander**: CH422G (I2C) — touch-reset, backlight-enable (on/off
  only, no PWM), LCD-reset, SD_CS, **and** the isolated DI/DO channels
- **Isolated inputs**: DI0/DI1 + DI COM — 5–36 V, dry or wet contact,
  NPN/PNP; optocoupled, referenced to DI COM (never board GND)
- **Isolated outputs**: DO0/DO1 — optocoupled open-drain, up to 450 mA
  sink per channel (external supply)
- **RS485**: A/B terminals, auto direction — **shares GPIO43/44 with the
  CH343 USB-UART console** (logging and RS485 are mutually exclusive)
- **CAN**: H/L terminals, dedicated transceiver on GPIO15/16 (no USB mux —
  unlike the plain 4.3); jumper-selectable 120R terminator
- **I2C header**: VOUT/GND/SDA/SCL — the same GPIO8/9 bus as touch and
  the expander; VOUT is 5 V/3.3 V by onboard resistor option (default 5 V)
- **microSD**: TF slot (unused in v0.1)
- **USB**: Type-C native CDC (GPIO19/20) + Type-C CH343 UART
- **Power**: 6–36 V DC terminal (board silk `6~36V`; product page says
  7–36 V — stay ≥7 V to be safe) or USB-C. No battery.

## Terminal block (top to bottom, per the rear silk)

| Group | Pin | Reaches the S3 via |
|-------|-----|--------------------|
| Isolated I/O | DI1 | CH422G EXIO5 (read) |
| Isolated I/O | DI0 | CH422G EXIO0 (read) |
| Isolated I/O | GND | isolated-side ground |
| Isolated I/O | DI COM | input common (5–36 V side) |
| Isolated I/O | DO1 | CH422G OD1 (open-drain) |
| Isolated I/O | DO0 | CH422G OD0 (open-drain) |
| RS485 | A / B | GPIO44 (TX) / GPIO43 (RX) — shared with UART console |
| CAN | H / L | GPIO15 (TX) / GPIO16 (RX), TWAI |
| I2C | SCL / SDA | GPIO9 / GPIO8 (shared with GT911 + CH422G) |
| I2C | GND / VOUT | VOUT = 5 V or 3.3 V (resistor option, default 5 V) |
| Power | GND / VIN | 6–36 V DC in |

## I2C sensor header — reserved addresses

The CH422G is *command-addressed*: each function is a fixed I2C address,
so these are burned on the shared bus and **no external device may use
them**:

| Address | Owner | Trap |
|---------|-------|------|
| `0x23` | CH422G WR_OC (DO latch) | **BH1750 default addr** — strap BH1750 ADDR high (0x5C) or prefer VEML7700 |
| `0x24` | CH422G WR_SET (mode) | |
| `0x26` | CH422G RD_IO (DI read) | |
| `0x38` | CH422G WR_IO (EXIO latch) | **AHT20/AHT21 default** — unusable on this bus |
| `0x5D` / `0x14` | GT911 touch | |

Known-good playground sensors: VEML7700 (0x10), VL53L0X/L1X (0x29),
MPR121 (0x5A–0x5B), CAP1188 (0x28–0x2D), BME280 (0x76/0x77), SHT4x
(0x44), TCA9548A mux (0x70) as the escape hatch.

## Support Status

**Compile-tested** (see [`boards.json`](../boards.json)): the pin map is
compile-verified against Waveshare's wiki/demo sources but has **not**
been bench-validated. Items flagged VERIFY in
[`pins/pins.h`](pins/pins.h): RGB timings, CH422G bit map + DI-read mode
blip (backlight flicker check), DO latch polarity, RS485 TX/RX
orientation, SD SPI pins, VOUT default.
[`docs/hardware/display_bench_bringup.md`](../../../docs/hardware/display_bench_bringup.md)
**Track D** plus the playground smoke tests
([`docs/hardware/dev_playground_43b.md`](../../../docs/hardware/dev_playground_43b.md))
retire this status.

## Used By

| Flavor | Env | Notes |
|--------|-----|-------|
| `canary-display` | `canary-display-playground` | Dash flavor + `FEATURE_PLAYGROUND=1` — guided peripheral bench mode |
| `canary-display` | (dash hardware alternative) | The dash env pin map (`waveshare-esp32s3-lcd43`) covers this SKU's panel; use this board dir when the terminal block matters |

Pin definitions: [`pins/pins.h`](pins/pins.h) (authoritative). The live
used-vs-open budget for every pin lives in the playground doc's
[pin tracker table](../../../docs/hardware/dev_playground_43b.md#pin-tracker).
