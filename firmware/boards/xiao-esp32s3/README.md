# Seeed Studio XIAO ESP32-S3 Board (plain, non-Sense)

Thumb-sized ESP32-S3 board without the Sense expansion (no camera, mic, or
SD slot). Host for the Grove Vision AI V2 in the `canary-vision` flavor
`canary-vision-xiao-s3` — this is the pairing Seeed sells as the
**Grove Vision AI V2 Kit**. For the Sense variant (camera/mic/SD) used by
`canary` and `canary-wap`, see [`xiao-esp32s3-sense`](../xiao-esp32s3-sense/).

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa dual-core, 240 MHz)
- **Flash**: 8MB
- **PSRAM**: 8MB
- **WiFi**: 2.4 GHz 802.11 b/g/n — **external u.FL antenna required** (included with the board)
- **Bluetooth**: BLE 5.0
- **USB**: Native USB OTG (CDC), USB-C connector
- **Battery**: BAT pads on the underside

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-vision/default` | Vision AI presence detection (`canary-vision-xiao-s3` env) |

## Connecting the Grove Vision AI V2

Two supported hookups (see `docs/hardware/grove_vision_ai_v2_guide.md` for
photos and the dual-USB-C explanation):

1. **Stacked (recommended)** — solder headers and seat the XIAO in the
   module's XIAO socket, both USB-C ports facing the same direction.
   This routes I2C (D4/D5 → GPIO5/6) and UART (D6/D7 → GPIO43/44).
2. **Grove cable** — module's Grove connector to D4/D5. I2C only
   (SDA=GPIO5 white, SCL=GPIO6 yellow, VCC red, GND black).

The firmware talks to the module over I2C at address `0x62` either way.

## Pin Groups

### XIAO header (11 pins)
- D0–D3: GPIO1–4 (also A0–A3)
- D4/D5: GPIO5/6 (I2C SDA/SCL)
- D6/D7: GPIO43/44 (UART TX/RX)
- D8–D10: GPIO7/8/9 (SPI SCK/MISO/MOSI)

## Constraints

- GPIO0 is the BOOT button strapping pin.
- GPIO26–37 are bonded to flash/PSRAM (26–32 quad flash bus; 33–37 octal-PSRAM SPIIO4–7 + SPIDQS on the S3R8) — never use.
- GPIO21 is both the user LED (active-low) and, with the Sense expansion board, the SD chip-select.
- GPIO19/20 are the USB data lines — do not repurpose.
- WiFi range is poor-to-nonexistent without the u.FL antenna attached.

## References

- [Seeed wiki — XIAO ESP32S3 Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Grove Vision AI V2 device guide](../../../docs/hardware/grove_vision_ai_v2_guide.md)
