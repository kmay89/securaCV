# XIAO ESP32-S3 Sense Board

Seeed Studio XIAO ESP32S3 Sense with camera and microphone module.

## Hardware Specifications

- **MCU**: ESP32-S3R8 (Xtensa LX7 dual-core, 240 MHz)
- **Flash**: 8MB QSPI
- **PSRAM**: 8MB Octal PSRAM
- **WiFi**: 2.4 GHz 802.11 b/g/n — external u.FL antenna (included) required for usable range
- **Bluetooth**: BLE 5.0
- **Camera**: OV3660 (3MP, 2048x1536) on current kits; earlier units shipped the
  now-discontinued OV2640 (2MP, 1600x1200); OV5640 supported as a drop-in.
  Firmware auto-detects the sensor PID at init.
- **Microphone**: PDM digital mic (MSM261D3526H1CPM per the module datasheet /
  community teardowns; the Seeed wiki lists it only as "digital microphone")
- **SD Card**: microSD slot (SPI mode), up to 32GB, FAT32
- **USB**: Type-C (native USB, GPIO19/20)
- **Battery charging**: 50mA fast / 3.8mA trickle (the 100mA figure applies to
  the XIAO ESP32-S3 *Plus*, not this board)

## Power (official Seeed figures, 3.8V supply)

| State | Bare XIAO ESP32-S3 | With Sense expansion |
|---|---|---|
| Deep sleep | 14 µA | **3 mA** |
| Light sleep | 2 mA | 5 mA |
| Modem sleep | 27 mA | 44 mA |
| WiFi active | 100 mA | 110 mA |
| BLE active | 85 mA | 102 mA |

The Sense expansion board raises deep-sleep draw ~200x (3 mA vs 14 µA) —
decisive for any battery duty-cycle plan. USB-CDC serial is unavailable
during deep sleep.

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-wap/default` | Wireless Access Point witness device |
| `canary-wap/mobile` | Mobile/portable witness device |

## Pin Groups

### GPIO Expansion
- D0-D10 available on expansion connector
- A0-A5 analog inputs (12-bit ADC); D8-D10 double as A8-A10.
  GPIO41/42 (PDM mic) do **not** support ADC despite silk labeling.

### Camera (OV2640/OV3660, kit-dependent)
- Parallel DVP interface on dedicated GPIOs (10-18, 38, 47, 48)
- No GPIO reconfiguration needed

### SD Card (SPI)
- CS = GPIO21; shares the GPIO7/8/9 SPI bus with header pins D8-D10
  (any external SPI peripheral on the header contends with the SD card)
- Note: Seeed's Getting Started Sense pin map erroneously lists SD CS as
  GPIO3; the filesystem wiki page and all official examples use GPIO21

### GNSS Module
- Optional L76K GPS module via UART (D6/D7 = GPIO43/44, 9600 baud default)

## Constraints

- **GPIO21 is triple-booked**: user LED (active-LOW), SD chip-select, and
  Arduino `LED_BUILTIN`. While the SD card is mounted, GPIO21 belongs to
  the SD driver — LED writes assert/deassert the card's CS and can corrupt
  in-flight transactions. Use `EXT_LED_PIN_DEFAULT` for status when the SD
  card is in use.
- Camera frames are DMA'd from the parallel DVP interface into PSRAM —
  PSRAM must be enabled for the camera to work. (The camera does **not**
  share the flash/PSRAM QSPI bus; that bus is GPIO26-37, internal.)
- GPIO26-37 are bonded to flash (26-32) and octal PSRAM (33-37) — never
  route peripherals to them.
- GPIO0 is the BOOT button strapping pin.
- USB CDC active during development - disable for production

## References

- [Product Page](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
- [Wiki: Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Wiki: Camera Usage](https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/)
- [Wiki: SD / Filesystem](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
- [Wiki: PDM Microphone](https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/)
- [Wiki: Power Consumption](https://wiki.seeedstudio.com/XIAO_ESP32S3_Consumption/)
