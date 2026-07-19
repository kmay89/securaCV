# Canary Fence Guard — hardware research dossier

Staged platform research for the **Canary Fence Guard** concept
(`firmware/projects/canary-fence-guard/` — coming-soon teaser on the
Canary House and concept card in canary.local). This document is the
single home for the numbers: the firmware stub and the registry card
deliberately point here instead of repeating them.

**Subject:** Seeed Studio **XIAO ESP32S3 & Wio-SX1262 Kit for
Meshtastic & LoRa** — the ESP32 member of Seeed's Meshtastic kit family,
same XIAO line as every other Canary host.

*Research date: 2026-07-19. Method note: seeedstudio.com,
wiki.seeedstudio.com and meshtastic.org blocked direct page fetches from
the research environment, so wiki content was pulled from their
canonical open-source repositories (Seeed-Studio/wiki-documents and
meshtastic/meshtastic on GitHub — the exact markdown that renders those
sites) plus search excerpts of the product pages. Items verified from
only one origin are marked **single-source**.*

## 1. Product identity, SKUs, price, variants

| Item | Value |
|---|---|
| Product name | XIAO ESP32S3 & Wio-SX1262 Kit for Meshtastic & LoRa |
| Seeed SKU (bare kit) | **102010611** (Seeed wiki frontmatter; Botland retail listing corroborates) |
| Product page | seeedstudio.com `Wio-SX1262-with-XIAO-ESP32S3-p-5982.html` |
| Price (bare kit) | **US $9.90** (Seeed store per search results; Seeed launch blog "only $9.9", Oct 2024; ~£9.60 UK per Concretedog review) |
| Case variant | Kit with 3D case — SKU **113110064**. USD price unverified (gap); Antratek EU lists €11.90 ex-VAT — **single-source** |
| Sibling variant | **XIAO nRF52840 & Wio-SX1262 Kit** — same concept, nRF52840 MCU, through-hole module, **pin-incompatible** with the ESP32S3 version |
| Standalone module | Wio-SX1262 Wireless Module (p-5981), SKUs 114993390 / 113991436 (IPEX / no-IPEX) |
| Kit contents | XIAO ESP32S3 ×1, Wio-SX1262 extension ×1, WiFi antenna ×1, LoRa antenna ×1 — Seeed warns the FPC LoRa antenna is "only for testing" |
| Case-kit contents | Assembled kit in case + 2 dBi SMA antenna + USB-C cable |
| Firmware as shipped | Bare kit: Meshtastic pre-flashed (units after Oct 24, 2024); case-kit page mentions single-channel LoRaWAN hub firmware — check on arrival |

## 2. MCU side: XIAO ESP32S3

- **Chip:** ESP32-S3R8, Xtensa LX7 dual-core @ up to 240 MHz
- **Memory:** 8 MB on-chip PSRAM + 8 MB flash
- **Wireless:** 2.4 GHz WiFi 802.11 b/g/n; BLE 5.0 + Bluetooth Mesh
- **USB:** USB-C, native USB (Meshtastic variant VID 0x2886 / PID 0x0059)
- **Battery charging:** onboard Li-ion management, 5 V USB-C or 4.2 V BAT pads. Charge current: kit wiki says **100 mA**; the XIAO board spec table says **50 mA fast / 3.8 mA trickle** — **conflicting vendor figures; plan around "≈50–100 mA, i.e. slow"**
- **Sleep/active (Seeed-published, single-source):** deep sleep **14 µA** (XIAO alone); light sleep 2 mA; modem sleep 27 mA; WiFi active ~100 mA; BLE active ~85 mA
- **GPIO:** 11× GPIO (PWM), 9× ADC, UART, I²C, I²S, SPI; user + charge LEDs, reset/boot buttons; U.FL WiFi antenna; back-side B2B connector with extra GPIOs
- **Dimensions:** 21 × 17.8 mm

## 3. Radio side: Wio-SX1262

- **Transceiver:** Semtech SX1262
- **Frequency:** kit pages **862–930 MHz** — one hardware covers EU868 and US915, region set in firmware. No 433 MHz version of this kit
- **TX power:** up to **+22 dBm**
- **Sensitivity:** −136.73 dBm best-case per module datasheet — **single-source** (don't substitute Semtech's chip-level −148 dBm, which applies at minimum bandwidth)
- **Modulation:** LoRa + (G)FSK; LoRa BW 7.8–500 kHz; DC-DC supply; TCXO (DIO3-fed 1.8 V); DIO2 drives the RF switch; 50 Ω port
- **Antenna:** U.FL/IPEX (case kit adapts to SMA)
- **Attachment:** B2B connector to the XIAO (the nRF52840-kit module is through-hole — different pins, not interchangeable). SPI pin map from Meshtastic `variants/esp32s3/seeed_xiao_s3/variant.h`: SCK=GPIO7, MISO=GPIO8, MOSI=GPIO9, CS=41, RESET=42, BUSY=40, DIO1=39, DIO2=38 — GPIO38–42 ride the B2B connector, not the castellated pads
- **Extras:** user button (press = screen, double = broadcast, triple = GPS mode); STEP + schematic published
- **Range claims:** 2–5 km typical; Seeed measured >2.5 km on the 2 dBi IPEX antenna; "20 km+" open-terrain is marketing
- **Module dimensions:** not stated in reachable sources — **gap**

## 4. Power

- 5 V USB-C or single-cell Li-ion on **BAT solder pads** — no JST, battery must be soldered, none included
- **No stock fuel gauge:** the Meshtastic variant sets `BATTERY_PIN -1`; community adds a divider on D0/GPIO1 or an INA219/226 on I²C
- **No solar input and no official Seeed solar guidance.** Community practice (LoRaMeshDevices' solar build + 25 h test on this exact kit): external solar charge controller (CN3065/BQ24074-class, up to ~1 A) charging the cell directly, bypassing the slow onboard charger
- **Measured Meshtastic draw (community):** Tutoduino, **~130 mA average** (BLE on, screenless client) → ~19 h on 2,500 mAh — **single-source** measurement but consistent with Seeed's active figures. TX bursts ~100+ mA extra, <1 s. The **nRF52840 sibling: ~11.7 mA idle / ~107.6 mA TX** (same tester) — **~11× idle difference**
- Meshtastic power saving (default in ROUTER role on ESP32) enables light sleep — community reports "a dozen hours to over a hundred" — at the cost of BLE availability

## 5. Meshtastic support

- **Mainline-supported**, flashable from the official Web Flasher ("Seeed XIAO S3")
- **Firmware target:** PlatformIO env `seeed-xiao-s3`, HW_MODEL 81, `actively_supported = true` — listed in the **Community Supported** docs section (the nRF52840 kit sits in the fully-supported section)
- **Roles:** CLIENT, CLIENT_MUTE/HIDDEN/BASE, TRACKER, SENSOR, TAK, ROUTER, ROUTER_LATE (REPEATER deprecated as of fw 2.7.11); ROUTER on ESP32 auto-enables power saving (BLE off)
- **GNSS:** the L76K module stacks on the same footprint, but its RST shares the Wio-SX1262 RST line — the pin must be cut to run both
- **S3 vs nRF52 (community consensus):** ESP32-S3 buys WiFi (MQTT gateway, web client) and price; **nRF52840 wins on power by an order of magnitude and is the standard pick for solar/battery-first nodes**

## 6. Expansion — what a fence sensor can use

- **Free with the radio attached:** the entire I²C bus (D4/GPIO5 SDA, D5/GPIO6 SCL), one UART (D6/D7), and D0–D3 (GPIO/ADC, D3 touch-capable). The radio consumes only the B2B GPIOs (38–42) plus the shared SPI
- **Accelerometer:** **LIS3DH is natively supported by Meshtastic** (`src/motion/LIS3DHSensor.cpp`, auto-detected on I²C 0x18/0x19) — wake-on-motion/tap works stock
- **Telemetry module:** auto-detects supported I²C sensors at boot (BME280/68x, SHT3x/4x, INA219/226/260/3221, …) and broadcasts on interval; SENSOR role prioritizes those packets
- **Detection Sensor module** (fw ≥2.2.2): monitors any free GPIO high/low with optional pull-up and sends mesh alerts — intended for exactly this class of vibration/reed/motion switch. **A vibration switch on D1 + LIS3DH on I²C is a zero-custom-code Phase 0**
- No Grove socket on the kit; Seeed's path is the XIAO Expansion Board / Grove Base for XIAO

## 7. Physical

- Bare kit: XIAO footprint 21 × 17.8 mm; stacked height unpublished — **gap**
- Case kit: 22 × 23 × 57 mm ABS (case rated −40…+100 °C), 37.1 g, external SMA 2 dBi antenna (13 × 195 mm)
- **Electronics temp range:** kit wiki −40…+65 °C vs XIAO spec −20…+65 °C — conflicting; **use −20…+65 °C as the conservative bound**
- Mounting: no ears/flanges on the official case; community STLs add wall/pole/18650 mounts; Seeed's case STL is free (Thingiverse 6888371)
- **IP rating: none.** Not sealed, not gasketed — the weatherproof clamp enclosure is ours to design (R7)

## 8. Outdoor solar fence-node notes

- Precedent: LoRaMeshDevices solar build/25 h test on this exact kit; Kaspars Dambis, Dan Pupius, Muldrf, Hackaday document the general pattern; Atlavox S4 ships 2×18650 (6,000 mAh)
- **Sizing reality:** at ~130 mA untuned average, 2,500 mAh ≈ 19 h. An always-on S3 node realistically needs light sleep enabled + 1–2× 18650 (~5,000+ mAh) + a 5–6 W panel; nRF52 builds get away with 3 W and one cell. The community is blunt: for set-and-forget solar repeaters, the nRF52840 kit (or RAK4631) is the better base
- **Chemistry/heat (drives R6 "shade preferred"):** Li-ion loses 25–40 % capacity below −10 °C and must not be **charged** below freezing; LiPo held at full charge in heat can lose 20–30 % capacity in a summer. For outdoor extremes the community recommends **LiFePO₄ 18650s** — but LiFePO₄'s 3.65 V profile is *not* supported by the XIAO's 4.2 V onboard charger, so it needs its own external controller either way. Mount the enclosure shaded (north side / under the fence cap), panel in sun, cell out of direct sun

## What this decides for the concept

1. **Phase 0 needs zero custom firmware:** stock Meshtastic + Detection
   Sensor module (vibration switch on D1) + LIS3DH on I²C is a
   field-mountable mule today. That reframes the stub's open question 1.
2. **The S3-vs-nRF52 question (stub open question 3) is real and
   quantified:** ~11× idle draw difference. The mule's winter power data
   decides it; the nRF52840 sibling kit stays on the table.
3. **Solar (R5/R6):** external charge controller mandatory; panel 5–6 W
   for S3, cell 5,000+ mAh, LiFePO₄ preferred for temperature but needs
   its own charging path.
4. **Enclosure (R7):** nothing shipped is weather-rated; antenna must
   stand off the fence's metal plane; IPEX→SMA adaption is proven by the
   case kit.

## Key caveats

1. Seeed's own docs conflict on charge current (100 vs 50 mA) and temp
   range (−40 vs −20 °C) — both flagged above.
2. The ESP32-S3 and nRF52840 versions of the Wio-SX1262 module are
   pin-incompatible despite identical names.
3. Unverified / gaps: case-kit USD price, module sensitivity
   (datasheet-only), module dimensions, stacked kit height.

## Sources

- Seeed wiki (via github.com/Seeed-Studio/wiki-documents, the markdown source of wiki.seeedstudio.com):
  `wio_sx1262_with_xiao_esp32s3_kit`, `wio_sx1262_xiao_esp32s3_for_meshtastic`,
  `wio_sx1262`, `wio_sx1262_and_xiao_esp32s3_kit_with_3dprinted_enclosure…`,
  `xiao_esp32s3_getting_started`
- Meshtastic docs (via github.com/meshtastic/meshtastic):
  `wio-sx1262.mdx` + device index, `device.mdx` (roles),
  `detection-sensor.mdx`, `telemetry.mdx`
- github.com/meshtastic/firmware:
  `variants/esp32s3/seeed_xiao_s3/{variant.h,platformio.ini,pins_arduino.h}`,
  `src/motion/LIS3DHSensor.cpp`, `src/motion/AccelerometerThread.h`
- seeedstudio.com product pages p-5982, p-6314, p-5981 (search excerpts);
  Seeed launch blog 2024-10-22 ($9.9)
- files.seeedstudio.com `Wio-SX1262_Module_Datasheet.pdf` (search excerpt; PDF unreachable)
- tutoduino.fr `power-consumption-meshtastic` (130 mA / 11.7 mA measurements)
- lorameshdevices.com "Exploring the ESP32-S3 Solar Node: a 25-hour battery test"
- Price/SKU corroboration: adrelien.com, concretedog.blogspot.com, botland.store (102010611), antratek.com
- ESP32-vs-nRF52 tradeoffs: meshunderground.com, filipnet.de, smartnmagic.com, keepteen.com (20-device test)
- Solar/battery guidance: adkmesh.com, hamradiotherapy.com, hackaday.com 2025-09-17, kaspars.net, pupius.com, atlavox.com (S4)
- Cases/mounts: thingiverse.com thing:6888371, printables.com model 1246072
- amazon.com B0GY4QC6GN (case-kit contents)
