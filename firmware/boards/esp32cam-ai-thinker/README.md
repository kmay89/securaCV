# AI-Thinker ESP32-CAM

The most widely owned camera dev board in existence (~$6, sold by the pallet
for a decade) — the board a newcomer most likely already has in a drawer when
they discover SecuraCV. Classic dual-core ESP32 with an OV2640, 4 MB flash
and 4 MB quad PSRAM. See
[docs/board_market_research.md](../../../docs/board_market_research.md) for
why this port exists.

## Hardware Specifications

- **MCU**: ESP32 (classic dual-core Xtensa LX6, 240 MHz)
- **Flash**: 4 MB (fits `partitions_ota.csv` — 1.9 MB A/B slots — exactly)
- **PSRAM**: 4 MB quad (chip-select on GPIO16 — reserved)
- **Camera**: OV2640 (1600x1200), the `CAMERA_MODEL_AI_THINKER` pin map
- **SD**: microSD slot, used in SPI mode by the canary storage driver
- **WiFi**: 2.4 GHz 802.11 b/g/n — **Wi-Fi CSI supported**
- **Bluetooth**: Classic + BLE 4.2
- **USB**: None — flashing needs an external 3.3 V UART adapter (or the
  ESP32-CAM-MB shield) with GPIO0 low at reset

## What the port keeps, and what it can't

The two sensing pillars that justify the port survive intact: **camera PEEK**
and **Wi-Fi CSI** presence/breathing. What the hardware cannot carry:

| Feature | Status | Why |
|---|---|---|
| Camera PEEK | ✅ | OV2640 + 4 MB PSRAM |
| Wi-Fi CSI sensing | ✅ | Classic ESP32 has native CSI |
| SD witness storage | ✅ | Onboard slot in SPI mode |
| Signed pull-OTA (A/B) | ✅ | 4 MB flash fits the OTA table exactly |
| Acoustic events (mic) | ❌ | No onboard mic, and no free GPIO to add one |
| GNSS | ❌ | No free pins with camera + SD in use |
| Touch / IR / tamper GPIO | ❌ | No free pins; touch lib is S2/S3-only |
| Die-temp tamper | ❌ | Classic ESP32 has no supported tsens driver path |
| BLE / mesh (`full` level) | ❌ | `full` needs an 8 MB flash layout |

The build environment (`firmware/canary/platformio.ini` → `[env:esp32cam]`)
encodes exactly this list, and the OTA product id
(`securacv-canary-esp32cam`) is distinct so this board can never install an
S3 image or vice versa.

## Flashing (the honest version)

There is no USB port. You need:

1. A 3.3 V USB-UART adapter (or the ESP32-CAM-MB backplane): adapter TX →
   GPIO3 (U0RXD), adapter RX → GPIO1 (U0TXD), GND → GND, 5V → 5V.
2. GPIO0 → GND while resetting to enter the bootloader; disconnect and
   reset again to run.

The in-browser flasher cannot drive this dance automatically on boards
without native USB — use the desktop Flasher app or `esptool.py` until the
UART story is wired into the web flasher (tracked in
[docs/strategy/30-supplier-breadth-and-customer-hardware-roadmap.md](../../../docs/strategy/30-supplier-breadth-and-customer-hardware-roadmap.md),
Phase 0).

## Constraints & gotchas

- **GPIO0 doubles as camera XCLK** — the flashing strap and the camera
  clock share a pin. That's normal for this board; the camera driver owns
  it after boot.
- **GPIO4 is both the flash LED and SD DAT1.** In SPI mode DAT1 is unused,
  so the LED is drivable — but it is blindingly bright and drains the
  camera's thermal budget; the port leaves it off.
- **GPIO16 is the PSRAM chip-select.** Never route anything to it.
- **Brown-outs are this board's signature failure.** The camera + Wi-Fi
  burst load needs a solid 5 V / 500 mA supply; most "random reboots"
  reported against ESP32-CAM are thin USB cables.
- Some clone modules ship without PSRAM or with a different camera
  (OV3660/OV5640 kits exist). The firmware auto-detects the sensor PID;
  a missing-PSRAM clone will fail camera init at higher resolutions.

## Support Status

**Tier: compile-tested** — this port is CI-built on every PR but has not
been validated on hardware by a maintainer. Pin assignments trace the
AI-Thinker schematic / `CAMERA_MODEL_AI_THINKER` reference map rather than
a bench session. To promote it to `community`: run a bench bring-up on real
hardware (boot log, camera PEEK, CSI event, SD write, OTA A/B swap) and
file a Hardware Test Report issue; a maintainer flips the tier with your
report as `tier_evidence`. See
[../../PORTING.md](../../PORTING.md) step 7.

## References

- [AI-Thinker ESP32-CAM schematic](https://docs.ai-thinker.com/en/esp32-cam)
- [Espressif esp32-camera pin maps](https://github.com/espressif/esp32-camera)
- [ESP32 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf)
