# Seeed Studio XIAO ESP32-C3 Board

Thumb-sized ESP32-C3 board. Primary host for the Grove Vision AI V2 in the
`canary-vision` flavor `canary-vision-xiao-c3` — it plugs straight into the
module's XIAO socket (no wiring) or connects over a Grove cable.

## Hardware Specifications

- **MCU**: ESP32-C3 (RISC-V single-core, 160 MHz)
- **Flash**: 4MB
- **SRAM**: 400KB
- **WiFi**: 2.4 GHz 802.11 b/g/n — **external u.FL antenna required** (included with the board)
- **Bluetooth**: BLE 5.0
- **USB**: Native USB (CDC/JTAG), USB-C connector
- **Battery**: BAT pads on the underside (no fuel gauge; charge LED only)

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-vision/default` | Vision AI presence detection (`canary-vision-xiao-c3` env) |

## Connecting the Grove Vision AI V2

Two supported hookups (see `docs/hardware/grove_vision_ai_v2_guide.md` for
photos and the dual-USB-C explanation):

1. **Stacked (recommended)** — solder headers and seat the XIAO in the
   module's XIAO socket, both USB-C ports facing the same direction.
   This routes I2C (D4/D5 → GPIO6/7) and UART (D6/D7 → GPIO21/20).
2. **Grove cable** — module's Grove connector to D4/D5. I2C only
   (SDA=GPIO6 white, SCL=GPIO7 yellow, VCC red, GND black).

The firmware talks to the module over I2C at address `0x62` either way.

## Pin Groups

### XIAO header (11 pins)
- D0–D3: GPIO2–5 (also A0–A3)
- D4/D5: GPIO6/7 (I2C SDA/SCL)
- D6/D7: GPIO21/20 (UART TX/RX)
- D8–D10: GPIO8/9/10 (SPI SCK/MISO/MOSI)

### UART
- Console: native USB Serial/JTAG (no GPIO cost)
- Header UART on D6/D7 for external sensors / Vision AI UART mode (921600 baud)

## Constraints

- GPIO2, GPIO8, GPIO9 are strapping pins — keep them in the documented boot
  state during reset (GPIO9 doubles as the BOOT button).
- GPIO18/19 are the USB data lines — do not repurpose.
- ADC2 (A3/GPIO5) cannot be used while WiFi is active.
- No user LED — status indication needs an external LED (D1 by default).
- WiFi range is poor-to-nonexistent without the u.FL antenna attached.

## References

- [Seeed wiki — XIAO ESP32C3 Getting Started](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/)
- [Grove Vision AI V2 device guide](../../../docs/hardware/grove_vision_ai_v2_guide.md)
