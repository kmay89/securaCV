# ESP32-C3 DevKitM-1 Board

Generic ESP32-C3 development board for vision sensor applications.

## Hardware Specifications

- **MCU**: ESP32-C3 (RISC-V single-core, 160 MHz)
- **Flash**: 4MB
- **SRAM**: 400KB
- **WiFi**: 2.4 GHz 802.11 b/g/n
- **Bluetooth**: BLE 5.0
- **USB**: Native USB (CDC/JTAG)

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-vision/default` | Vision AI presence detection |
| `canary-vision/lab` | Lab/development configuration |

## Pin Groups

### GPIO
- GPIO0-GPIO21 available (with some restrictions)
- ADC1: GPIO0-GPIO4
- ADC2: GPIO5 (shared with WiFi)

### I2C (Vision AI Module)
- Default SDA/SCL for Grove Vision AI V2 connection
- 100kHz/400kHz supported

### UART
- UART0: USB CDC (default console)
- UART1: Available for external sensors

## Constraints

- GPIO11 is flash VDD - do not use
- GPIO12-17 may be used by flash (check board variant)
- ADC2 cannot be used when WiFi is active

## References

- [ESP32-C3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
- [DevKitM-1 User Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/hw-reference/esp32c3/user-guide-devkitm-1.html)
