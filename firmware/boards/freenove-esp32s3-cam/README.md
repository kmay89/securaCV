# Freenove ESP32-S3-WROOM CAM (FNK0085)

The default Amazon "ESP32-S3 camera kit" a US newcomer actually receives —
consistently the top search result and the board most 2025 ESP32-S3 camera
tutorials assume. ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM)
with an OV2640 on Espressif's ESP32-S3-EYE reference camera wiring. See
[docs/board_market_research.md](../../../docs/board_market_research.md) and
[docs/strategy/30-supplier-breadth-and-customer-hardware-roadmap.md](../../../docs/strategy/30-supplier-breadth-and-customer-hardware-roadmap.md)
(Phase 0) for why this entry exists.

## Hardware Specifications

- **MCU**: ESP32-S3 (dual-core LX7, 240 MHz)
- **Flash**: 16 MB
- **PSRAM**: 8 MB octal
- **Camera**: OV2640 (1600x1200), `CAMERA_MODEL_ESP32S3_EYE` pin map
- **SD**: microSD slot — **1-bit SDMMC wiring only** (see below)
- **WiFi**: 2.4 GHz 802.11 b/g/n — Wi-Fi CSI supported
- **Bluetooth**: BLE 5.0
- **USB**: Two USB-C ports — one native USB (CDC/JTAG/flashing), one
  UART-bridge; the in-browser flasher works on the native port

## What the port keeps, and what it can't

| Feature | Status | Why |
|---|---|---|
| Camera PEEK | ✅ | OV2640 + 8 MB PSRAM |
| Wi-Fi CSI sensing | ✅ | S3, same as the flagship |
| Signed pull-OTA (A/B) | ✅ | 16 MB flash, OTA table fits with room to spare |
| GNSS / tamper (external) | ✅ opt-in | Free header pins |
| SD witness storage | ❌ for now | Slot is 1-bit SDMMC; the card's DAT3/CS
  line is not routed, so the SPI-mode storage driver is electrically
  impossible on it. Needs an SDMMC storage path — witness log lives in
  flash until then |
| Acoustic events | ❌ | No onboard mic |
| Touch | ❌ | S3 supports it, but no touch pad is wired on this board |
| BLE / mesh (`full` level) | ❌ for now | `full` is tuned for the flagship; not offered for this board yet |

The build environment (`firmware/canary/platformio.ini` →
`[env:freenove-s3]`) encodes exactly this list, and the OTA product id
(`securacv-canary-freenove-s3`) is distinct so this board and the XIAO
flagship can never install each other's images (different camera pins).

## Constraints & gotchas

- **This image fits only a 16 MB chip.** Flashed onto an 8 MB S3 (a XIAO
  ESP32-S3 / Sense — same chip family, so the chip guard alone won't stop
  you), the write succeeds and the board then boot-loops before printing a
  single firmware line: an `esp_core_dump_flash: Core dump flash config is
  corrupted!` error ~300 ms in, then `RTC_SW_CPU_RST` and around again.
  That log names everything except the actual cause. Both flashers now
  refuse the combination up front (the size gate — see
  `docs/browser_flasher.md`); the fix is simply to flash the image built
  for the board in hand.

- **The camera map is S3-EYE, not XIAO.** Same sensor, entirely different
  pins — which is exactly why the OTA product id is distinct.
- **SD requires SDMMC.** If you need removable witness storage today, the
  XIAO ESP32-S3 Sense or the ESP32-CAM port carry it; this board's slot
  waits on an SDMMC driver path.
- The onboard LED is a WS2812 (addressable) on GPIO48, not a plain LED —
  simple on/off writes won't light it.
- GPIO4/5 belong to the camera SCCB bus; the port's external I2C default
  is GPIO1/2.

## Support Status

**Tier: compile-tested** — CI-built on every PR, not yet validated on
hardware by a maintainer. Camera pins trace the Espressif S3-EYE reference
map (which Freenove's own demo code uses); the SD-slot limitation traces
Freenove's schematic. To promote to `community`: bench a real board (boot
log, camera PEEK, CSI event, OTA A/B swap) and file a Hardware Test Report
issue. See [../../PORTING.md](../../PORTING.md) step 7.

## References

- [Freenove FNK0085 repository](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board)
- [Espressif esp32-camera pin maps](https://github.com/espressif/esp32-camera)
- [ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
