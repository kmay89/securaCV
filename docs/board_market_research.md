# Board ownership research — where the installed base actually is

**Date:** 2026-07-25
**Question:** By units owned (and by value/fit), which boards should SecuraCV
build firmware for first?
**Companion docs:** [firmware/HARDWARE.md](../firmware/HARDWARE.md) (support
tiers & scope), [firmware/boards/boards.json](../firmware/boards/boards.json)
(current registry), [firmware/PORTING.md](../firmware/PORTING.md).

---

## TL;DR

The numbers say we are already on the right horse. The ESP32 family's
installed base (~2.5 **billion** chips) is roughly 30× the entire Raspberry
Pi board line (~75M) and two orders of magnitude beyond official Arduino
(~10M Unos), and it is the only family in the list that natively covers all
three of our witness pillars: camera, Wi-Fi CSI, and cheap radios. The one
big gap in our tree is the **classic ESP32 (WROOM-32 / ESP32-CAM)** — the
single most-owned camera-capable dev board in the world — and that should be
the next port. Raspberry Pi and Arduino AVR are better served as *hosts* and
*non-goals* respectively, not firmware targets.

---

## Installed base, by the numbers

| Platform | Cumulative units | What the number means | Camera? | Wi-Fi (CSI)? | Fit for witness role |
|---|---|---|---|---|---|
| **ESP32 family (all variants)** | **~2.5B chips/modules** (early 2026; passed 1B in 2023) | Chips shipped by Espressif across ESP32/S2/S3/C3/C6/H2 | ✅ (S3, S2, classic) | ✅ native CSI | **Ideal — already our platform** |
| **Raspberry Pi (SBCs)** | ~75M boards (FY2025 results; 61M in early 2024) | Linux single-board computers, all models | ✅ (CSI-2 cam, USB) | ❌ (no ESP-style CSI; Linux Wi-Fi) | Host/server lane, not MCU firmware |
| **Raspberry Pi Pico / RP2040 / RP2350** | ~10M+ (≈4M Picos by mid-2024; 5.7M RP chips in 2024 alone, 8.4M semis in FY2025) | MCU chips + Pico boards | ❌ | ❌ (Pico W: CYW43, no CSI) | Weak — no camera, no CSI, wireless only on W variants |
| **Arduino (official)** | ~10M Unos lifetime; ~10M+ active users across the ecosystem | Classic AVR Uno has no radio; ecosystem is mostly clones | ❌ | ❌ (Uno R4 WiFi's radio *is* an ESP32-S3) | Non-goal as hardware; we already use the Arduino *core* as our framework |
| **STM32 / nRF52** | Billions of MCUs, but overwhelmingly soldered into products, not owned as dev boards | Pro install base, not hobbyist-owned boards | ❌ | ❌ (no Wi-Fi on-die) | Poor — no Wi-Fi, no camera pipeline, no reach advantage |

Three observations that matter more than the raw totals:

1. **The ESP32 number is the only one that compounds for us.** Every pillar
   of the Canary — OV2640/OV3660 camera capture, Wi-Fi CSI presence sensing,
   ESP-NOW-style fleet mesh, $5-per-node cost — is an Espressif-only
   combination. A Pico or STM32 port wouldn't just be work; it would be a
   *feature-stripped* build (no PEEK, no CSI, no mesh) chasing a smaller
   installed base.
2. **Raspberry Pi's 75M units are a different lane, and we already occupy
   it.** Pi owners run Linux — they are served by `canary-vision` (Node
   device API + SPA), the Home Assistant integration, and the Docker images,
   all of which run fine on a Pi today. The right "Pi support" is documenting
   that host story, not porting MCU firmware to it.
3. **Community sentiment tracks our current registry almost exactly.** 2025
   ESPHome/Home Assistant guides call the **ESP32-S3 the gold standard**,
   the **ESP32-C3 (Super Mini) the most-owned budget board**, and the
   **ESP32-C6 the best all-rounder** (Wi-Fi 6 + Thread/Zigbee/Matter). Those
   are precisely the three MCUs in `firmware/boards/` today.

---

## The gap: classic ESP32 (ESP32-CAM / WROOM-32 DevKit)

Our registry covers S3, C3, and C6 — but not the original dual-core ESP32,
which is the *largest single slice* of that 2.5B installed base. In
particular:

- The **AI-Thinker ESP32-CAM** (~$6, OV2640, 4MB flash + 4MB PSRAM) is the
  most widely owned camera dev board in existence — it's the default
  "cheap camera" answer in every maker guide, sold by the pallet for a
  decade. It is exactly the hardware a newcomer already has in a drawer when
  they discover SecuraCV.
- The **ESP32-WROOM-32 DevKit** is the same story without the camera — the
  single most common "an ESP32" that people own.
- Classic ESP32 supports **Wi-Fi CSI** and the camera driver, so a port
  keeps PEEK + CSI + mic — a real Canary, not a stripped one. The 4MB flash
  parts fit our `partitions_ota.csv` (1.9MB A/B) dev/release levels; only
  the `full` level (8MB, BLE+mesh) would be out of reach, which our
  level system already expresses.
- Cost per verified reach is unbeatable: one ~$6 board unlocks the biggest
  owned base of camera hardware on Earth, entering at **compile-tested**
  tier per [HARDWARE.md](../firmware/HARDWARE.md) with zero hardware
  obligation on maintainers.

Known friction to document in the port: no native USB (UART flashing via
IO0 strap on ESP32-CAM), fewer free GPIOs, and the camera + PSRAM pin
overlap — all well-trodden territory.

---

## Recommendation (priority order)

1. **ESP32-S3 (XIAO Sense + Waveshare LCD boards) — keep as flagship.**
   Verified tier today; community consensus "gold standard"; nothing to
   change.
2. **ESP32-C3 — keep, and consider adding the C3 Super Mini pin map.** The
   most-owned sub-$3 board in the hobby; we already build `xiao-esp32c3`,
   so a Super Mini entry is nearly free reach.
3. **ESP32-C6 — keep.** Smaller owned base today but the strategic radio
   set (Thread/Zigbee/Matter, Wi-Fi 6); already in tree via XIAO + Waveshare.
4. **NEW: classic ESP32 port — `esp32cam-ai-thinker` and/or
   `esp32-wroom-devkit`.** Largest owned base we don't serve; full witness
   feature set minus the 8MB `full` level; enters compile-tested per the
   existing tier rules. This is the highest-value next port.
5. **Raspberry Pi (SBC): support as a host, not a firmware target.**
   Document the "run canary-vision / Home Assistant / viewer on a Pi" story;
   75M owners reachable with docs, not ports.
6. **Non-goals for now: RP2040/RP2350, Arduino AVR/R4, STM32, nRF52.** Each
   fails at least two of camera/CSI/Wi-Fi, so a port buys reach only for a
   feature-stripped build — poor value per the
   [HARDWARE.md](../firmware/HARDWARE.md) scope rule ("a board belongs in
   this tree when it serves a witness role").

## Sources

- [Espressif: over 1 billion IoT chips shipped](https://www.espressif.com/en/news/1_Billion_Chip_Sales) and [company milestones](https://www.espressif.com/en/company/about-us/milestones) (~2.5B cumulative by early 2026 per [Elektor's CES 2026 coverage](https://www.elektormagazine.com/news/new-espressif-microcontrollers-ces-2026))
- [Raspberry Pi Holdings FY2025 results — 75M+ units lifetime, 7.6M boards + 8.4M semiconductors in FY25](https://investors.raspberrypi.com/reports/11/document); [Tom's Hardware: 61M units at 12 years](https://www.tomshardware.com/raspberry-pi/raspberry-pi-celebrates-12-years-as-sales-break-61-million-units)
- [heise: 5.7M RP2040/RP2350 chips sold in 2024](https://www.heise.de/en/news/Raspi-manufacturer-discloses-sales-figures-and-costs-10339630.html); [Raspberry Pi: ~4M Picos by Pico 2 launch](https://www.raspberrypi.com/news/raspberry-pi-pico-2-our-new-5-microcontroller-board-on-sale-now/)
- [Control Design: 10M Arduino Unos sold](https://www.controldesign.com/management/financials/news/11291667/10m-arduino-uno-boards-sold-worldwide); [eeNews: Arduino 2025 open-source report](https://www.eenewseurope.com/en/arduino-open-source-report-2025-ecosystem-growth/)
- Community consensus on S3/C3/C6: [DroneBot Workshop ESP32 selection guide 2026](https://dronebotworkshop.com/esp32-2026/), [Soldered: best ESP32 boards for IoT](https://soldered.com/blogs/learn/best-esp32-boards-for-iot), [Electromaker: top dev boards 2025](https://www.electromaker.io/blog/article/the-best-development-boards-for-every-project)
