# ESP32-C3 Super Mini

The most-owned sub-$3 board in the hobby — a white-label ESP32-C3
postage-stamp board sold under dozens of storefront names, and the budget
default in 2025 ESPHome/Home Assistant guides. This entry is nearly free
reach: it runs the same `canary-vision` app as the generic C3 DevKitM-1
entry with the same Grove-cable wiring, differing only in onboard
peripherals. See
[docs/board_market_research.md](../../../docs/board_market_research.md)
(recommendation 2).

## Hardware Specifications

- **MCU**: ESP32-C3 (RISC-V single-core, 160 MHz)
- **Flash**: 4 MB
- **SRAM**: 400 KB
- **WiFi**: 2.4 GHz 802.11 b/g/n
- **Bluetooth**: BLE 5.0
- **USB**: Native USB-C (CDC/JTAG) — the in-browser flasher works directly

## Supported Configurations

| Config ID | Description |
|-----------|-------------|
| `canary-vision/default` | Vision AI presence detection (Grove Vision AI V2 over a Grove cable) |

Wiring is identical to the generic C3 DevKit entry: Grove white wire →
GPIO4 (SDA), Grove yellow wire → GPIO5 (SCL), 3V3 and GND.

## Constraints & gotchas

- **Only GPIO0-10 and GPIO20/21 are broken out** — a strict subset of the
  DevKitM-1. This map stays inside that subset.
- **GPIO8 is both the onboard LED and a boot strap.** The LED is a plain
  active-low LED here, where the DevKitM-1 has a WS2812 on the same pin —
  the one behavioral difference between the two ports, and why this board
  gets its own OTA product id rather than reusing the DevKit image.
- GPIO9 is the boot button and a strap; GPIO2 is a strap. Avoid external
  pulls on all three.
- Many clones ship with marginal PCB antennas — if Wi-Fi is flaky at
  range, it's the antenna, not the port.

## Support Status

**Tier: compile-tested** — CI-built on every PR, not yet validated on
hardware by a maintainer. Pin map traces the common Super Mini pinout
diagrams and the ESP32-C3 datasheet; clone quality varies. To promote to
`community`: bench a real board through the Vision getting-started guide
and file a Hardware Test Report issue. See
[../../PORTING.md](../../PORTING.md) step 7.

## References

- [ESP32-C3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
- [Vision getting-started guide](../../../docs/hardware/canary_vision_getting_started.md)
