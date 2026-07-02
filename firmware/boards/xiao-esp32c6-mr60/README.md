# XIAO ESP32-C6 + MR60BHA2 mmWave Kit Board

Host board for the **canary-sense** radar witness: a Seeed Studio MR60BHA2
60GHz mmWave kit whose host MCU is a XIAO ESP32-C6. The radar module is treated
as a fixed black-box claim source; all SecuraCV firmware runs on the C6 host.

## Hardware Specifications

- **MCU**: ESP32-C6 (RISC-V single-core, 160 MHz)
- **Flash**: 4 MB
- **WiFi**: WiFi 6 (2.4 GHz)
- **Bluetooth**: BLE 5
- **802.15.4**: Thread / Zigbee capable (unused by canary-sense)
- **USB**: Native USB Serial/JTAG (CDC console)
- **Radar**: MR60BHA2, 57–64 GHz FMCW (ADT6101P DSP, 2T2R). Presence 1.5–6 m;
  breathing/heart rate ≤1.5 m. Pre-digested scalars only over UART.
- **Ambient light**: BH1750 (I2C)
- **RGB LED**: WS2812 (single pixel)

## Pin Groups

### Radar UART (MR60BHA2)
- Host TX `GPIO16` → radar RX, Host RX `GPIO17` ← radar TX
- 115200 8N1, UART1 (UART0 stays on the USB-CDC console)

### I2C (BH1750 lux)
- SDA `GPIO22`, SCL `GPIO23`
- Address `0x23` (ADDR low; `0x5C` when high)

### Onboard peripherals
- WS2812 RGB LED on `GPIO1`
- BOOT button on `GPIO9` (active LOW) — see assumption below

## Constraints / Assumptions

- **GPIO24–GPIO30** are in-package SPI flash — never use.
- **BOOT button = GPIO9** is an *assumption* (the XIAO/ESP32-C download-mode
  strapping convention). Confirm on bench hardware during the Phase 0 spike;
  only `pins.h` needs to change if it differs.
- **ESP32-C6 toolchain risk**: the C6 needs arduino-esp32 3.x, which the
  official PlatformIO `espressif32` platform lags. The build envs pin the
  community `pioarduino` fork (see `firmware/envs/platformio/canary-sense.ini`).

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-sense/default`   | Presence/count/lux only (vitals compiled out) |
| `canary-sense/wellbeing` | Adds P1-gated breathing/heart-rate vitals |

## References

- Seeed wiki: `getting_started_with_mr60bha2_mmwave_kit`
- Seeed `xiao-esphome-projects` reference YAML (UART 115200 GPIO16/17, I2C
  GPIO22/23, WS2812 GPIO1)
- `Seeed-Studio/Seeed_Arduino_mmWave` (reference frame protocol)
- `docs/canary_sense_mr60bha2_design.md` (design + phased roadmap)
