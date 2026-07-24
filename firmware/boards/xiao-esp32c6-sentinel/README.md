# Board: xiao-esp32c6-sentinel (Sentinel Standard)

Seeed **XIAO ESP32-C6** host carrying the **MR60BHA2** 60GHz radar (over UART),
a **PIR** motion input, and a **BH1750** lux sensor on I²C — the sensor head for
Canary Sentinel's Standard tier. WiFi-RF/CSI and BLE ride the onboard radio.

This is the canary-sense MR60BHA2 wiring plus a PIR pin, so the radar bring-up
is shared with `boards/xiao-esp32c6-mr60`. Pin numbers live in
[`pins/pins.h`](pins/pins.h) and nowhere else.

| Function | Pin | Notes |
|---|---|---|
| Radar UART TX/RX | 16 / 17 | UART1; UART0 stays on USB-CDC console |
| I²C SDA/SCL (BH1750) | 22 / 23 | addr 0x23 |
| PIR motion in | 2 | `[BENCH]` active-HIGH, INPUT |
| WS2812 status LED | 1 | single pixel |

**Constraints.** GPIO24–30 are in-package SPI flash — never use. The PIR pin is
a bench assumption (§ project README checklist); if it moves, it is a one-line
change here. ESP32-C6 needs the pioarduino core-3.x platform (same pin as
canary-sense) — see `envs/platformio/canary-sentinel.ini`.
