# ESP32-WROOM-32 DevKit (generic)

The single most common "an ESP32" that people own: Espressif's DevKitC, the
DOIT DevKit V1, NodeMCU-32S, and the ocean of white-label 30/38-pin clones
built on the ESP32-WROOM-32 module. No camera, no mic, no SD — what this
port delivers is a **Wi-Fi CSI presence witness** (motion / breathing /
channel-activity sensing) with the full signed witness chain, on hardware a
huge share of newcomers already have. See
[docs/board_market_research.md](../../../docs/board_market_research.md).

## Hardware Specifications

- **MCU**: ESP32 (classic dual-core Xtensa LX6, 240 MHz)
- **Flash**: 4 MB (fits `partitions_ota.csv` — 1.9 MB A/B slots — exactly)
- **PSRAM**: None (the WROVER module variant has it; that's a different board)
- **WiFi**: 2.4 GHz 802.11 b/g/n — **Wi-Fi CSI supported**
- **Bluetooth**: Classic + BLE 4.2
- **USB**: Micro-USB/USB-C through an onboard USB-UART bridge
  (CP2102/CH340) — flashing "just works" via auto-reset, but there is no
  native USB CDC

## What the port keeps, and what it can't

| Feature | Status | Why |
|---|---|---|
| Wi-Fi CSI sensing | ✅ | Classic ESP32 has native CSI; this is the board's witness role |
| Signed witness chain + HTTP/AP onboarding | ✅ | Core firmware, hardware-independent |
| Signed pull-OTA (A/B) | ✅ | 4 MB flash fits the OTA table exactly |
| GNSS (external module) | ✅ opt-in | Free UART2 on GPIO16/17 |
| Tamper GPIO (external switch) | ✅ opt-in | Plenty of free pins |
| Camera PEEK | ❌ | No camera connector |
| Acoustic events | ❌ | No mic (external I2S is future opt-in work) |
| SD witness storage | ❌ | No slot; witness log lives in flash |
| Touch / die-temp tamper | ❌ | Touch lib is S2/S3-only; no supported classic-ESP32 tsens path |
| BLE / mesh (`full` level) | ❌ | `full` needs an 8 MB flash layout |

The build environment (`firmware/canary/platformio.ini` →
`[env:esp32-wroom]`) encodes exactly this list, and the OTA product id
(`securacv-canary-wroom`) is distinct so this board can never install a
camera-board image or vice versa.

## Constraints & gotchas

- **Board revisions differ in which pins reach the headers** (30-pin vs
  38-pin), not in what the pins do. The map here uses only pins present on
  every family member, except the documented flash pins on 38-pin boards.
- **GPIO12 must not be pulled high at reset** (flash-voltage strap) — mind
  it when wiring external sensors.
- GPIO34/35/36/39 are input-only, with no internal pull-ups.
- Clone boards vary in USB-UART bridge chip (CP2102 vs CH340) — driver
  availability differs per OS, flashing behavior is otherwise identical.

## Support Status

**Tier: compile-tested** — CI-built on every PR, not yet validated on
hardware by a maintainer. Pin assignments trace the Espressif DevKitC
reference documentation. To promote to `community`: bench a real board
(boot log, AP onboarding, CSI event, OTA A/B swap) and file a Hardware
Test Report issue. See [../../PORTING.md](../../PORTING.md) step 7.

## References

- [ESP32-DevKitC user guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/hw-reference/esp32/get-started-devkitc.html)
- [ESP32 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf)
