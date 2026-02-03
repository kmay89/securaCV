# XIAO ESP32-S3 Sense Board

Seeed Studio XIAO ESP32S3 Sense with camera and microphone module.

## Hardware Specifications

- **MCU**: ESP32-S3 (Xtensa LX7 dual-core, 240 MHz)
- **Flash**: 8MB QSPI
- **PSRAM**: 8MB Octal PSRAM
- **WiFi**: 2.4 GHz 802.11 b/g/n
- **Bluetooth**: BLE 5.0
- **Camera**: OV2640 (2MP, 1600x1200)
- **Microphone**: MSM261D3526H1CPM (PDM)
- **SD Card**: microSD slot (SPI mode)
- **USB**: Type-C (native USB)

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-wap/default` | Wireless Access Point witness device |
| `canary-wap/mobile` | Mobile/portable witness device |

## Pin Groups

### GPIO Expansion
- D0-D10 available on expansion connector
- A0-A5 analog inputs (12-bit ADC)

### Camera (OV2640)
- Directly connected to ESP32-S3 camera interface
- No GPIO reconfiguration needed

### SD Card (SPI)
- Uses dedicated SPI bus
- CS, SCK, MISO, MOSI pins defined in pins/

### GNSS Module
- Optional L76K GPS module via UART
- TX/RX pins configurable

## Constraints

- Camera and PSRAM share QSPI bus - camera uses DMA
- SD card SPI must not conflict with camera data lines
- USB CDC active during development - disable for production

## References

- [Product Page](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
- [Wiki](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
