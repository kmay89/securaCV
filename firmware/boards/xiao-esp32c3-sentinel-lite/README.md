# Board: xiao-esp32c3-sentinel-lite (Sentinel Lite)

Seeed **XIAO ESP32-C3** host with a **PIR** motion input and a **BH1750** lux
sensor on I²C — the whole sensor set for Canary Sentinel's Lite tier. There is
**no radar and no CSI pipeline** on Lite: detection is PIR + WiFi-RF + BLE +
ambient light, with WiFi/BLE on the onboard radio. This is the low-cost tier and
its limits are stated honestly (a slow, device-free, still intruder can evade
it — see the design doc threat table).

Pin numbers live in [`pins/pins.h`](pins/pins.h) and nowhere else.

| Function | Pin | Notes |
|---|---|---|
| I²C SDA/SCL (BH1750) | 6 / 7 | XIAO C3 D4/D5, addr 0x23 |
| PIR motion in | 3 | `[BENCH]` XIAO C3 D1, active-HIGH, INPUT |
| User LED | 10 | onboard, active-LOW |

**Constraints.** GPIO11–17 are in-package SPI flash — never use. The PIR pin is
a bench assumption (§ project README checklist). ESP32-C3 builds on the standard
espressif32 2.0.x core (no pioarduino pin needed).
