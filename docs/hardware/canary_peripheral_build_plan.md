# Canary Peripheral Build Plan & Bill of Materials

> **Document:** SCV-HW-BOM-0001 · **Revision:** B (2026-05-28) · **Status:** Released
> **Scope:** Audible chirp (buzzer), status LED, and operator input/tamper
> peripherals — plus battery and enclosure — for the SecuraCV **Canary WAP**
> (XIAO ESP32-S3 Sense) and **Canary Vision** (ESP32-C3 + Grove Vision AI V2)
> witness devices.
> **Audience:** Hardware builders, integrators, and procurement.
>
> **Disclaimer:** This document is informational only and provided **"as is"
> with no warranty of any kind**. Verify every part against current manufacturer
> datasheets and comply with all applicable safety codes and regulations. The
> **lithium-battery guidance is safety-critical — read §6.5 and the §9.2 safety
> notice in full.** Any design that powers, charges, or encloses a battery
> should be reviewed and validated by a qualified engineer and certified where
> required before deployment. SecuraCV and the authors accept no liability for
> any damage, injury, or loss arising from use of this information; **you assume
> all risk.**

A SecuraCV **Canary** is a small witness device that watches the *shape* of an
environment — RF presence, motion, optionally person detection — without ever
learning who you are. This document is the hardware counterpart to the firmware
that already drives these peripherals: it tells you **what to buy, how it wires
to the MCU, and which parts are required versus optional**.

Machine-readable bills of materials accompany this document:

| File | Variant |
|------|---------|
| [`bom_canary_wap.csv`](./bom_canary_wap.csv) | Canary WAP — XIAO ESP32-S3 Sense |
| [`bom_canary_vision.csv`](./bom_canary_vision.csv) | Canary Vision — ESP32-C3 + Grove Vision AI V2 |

---

## 1 · Overview & scope

### 1.1 Two meanings of "chirp"

This is a frequent source of confusion, so it is stated up front:

| Term | What it is | In this BOM? |
|------|-----------|--------------|
| **Audible chirp** | A literal sound from a **PWM passive buzzer** (or LED-blink fallback), driven by the ESP32 LEDC peripheral. Firmware: [`audible_chirp.h`](../../firmware/projects/canary-wap/arduino/canary_wap/audible_chirp.h). | ✅ Yes — the buzzer (`BZ1`). |
| **Chirp Channel** | A privacy-first **RF community-witness mesh protocol** (radio, not sound) carried over the onboard WiFi/BLE radios. Spec: [`chirp_channel_v0.md`](../../spec/chirp_channel_v0.md). | ❌ No extra parts — uses the MCU's built-in radio. |

When this document says "chirp," it means **the buzzer**.

### 1.2 Variants covered

| Variant | MCU board | Primary sense | Audible chirp | Doc/BOM |
|---------|-----------|---------------|:-------------:|---------|
| **Canary WAP** | Seeed XIAO ESP32-S3 Sense | RF presence, GPS, SD, camera peek | ✅ | `bom_canary_wap.csv` |
| **Canary Vision** | ESP32-C3 DevKitM-1 + Grove Vision AI V2 | On-sensor person detection | ⚠️ not in firmware build | `bom_canary_vision.csv` |

> **Note on Vision + buzzer:** `FEATURE_AUDIBLE_CHIRP` is **not** compiled into
> the Canary Vision firmware (it is an event-publish device). The C3 board
> *does* define `BUZZER_PIN_DEFAULT = 2`, so a buzzer is listed in the Vision
> BOM as **Optional / unpopulated** for builders who fork the firmware.

---

## 2 · Build tiers (firmware ↔ populated parts)

The firmware selects a build profile in
[`build_config.h`](../../firmware/projects/canary-wap/arduino/canary_wap/build_config.h).
Populate parts to match your target profile — there is no reason to solder a
buzzer onto a `MINIMAL` build that never calls it.

| Peripheral | Gating flag | MINIMAL | DEV | FULL |
|------------|-------------|:-------:|:---:|:----:|
| Power/battery monitor | `FEATURE_POWER_MONITOR` | ✅ | ✅ | ✅ |
| Audible chirp (buzzer) | `FEATURE_AUDIBLE_CHIRP` | — | ✅ | ✅ |
| Tamper input | `FEATURE_TAMPER_GPIO` | — | optional | optional |
| External RGB status LED | (replaces `LED_BUILTIN` fallback) | optional | optional | optional |
| microSD storage | `FEATURE_SD_STORAGE` | — | ✅ | ✅ |
| GNSS (L76K) | `FEATURE_GNSS` | ✅ | ✅ | ✅ |

`FEATURE_TAMPER_GPIO` defaults to `0` in
[`configs/canary-wap/default/config.h`](../../firmware/configs/canary-wap/default/config.h);
enable it when a tamper switch is fitted.

---

## 3 · System block diagram (Canary WAP)

```
                       ┌──────────────────────────────┐
        USB-C  ───────►│  XIAO ESP32-S3 Sense          │
        5V/data        │  (ESP32-S3, 8MB flash,         │
                       │   8MB PSRAM, OV2640 cam,        │
   3.7V LiPo ─JST-PH──►│   PDM mic)                      │
   (+ 2×100k divider   │                                 │
    → VBAT GPIO1/A0)   │  GPIO2  (D1) ──► BZ1  Passive buzzer ──┐
                       │  GPIO3  (D2) ──► D(LED) WS2812 RGB     │
                       │  GPIO4  (D3) ◄── SW2  Reed/Hall tamper │
                       │  GPIO5  (D4) ◄~~ TP1  Cap-touch pad    │
                       │  GPIO0 (BOOT)◄── SW1  Multifn button   │
                       │  GPIO43/44(D6/7)─ L76K GNSS (UART)     │
                       │  GPIO7/8/9 (D8-10)─ microSD (SPI)      │
                       └──────────────────────────────┘        │
                                                                ▼
                                              (enclosure: light-pipe + vent)
```

`~~` = capacitive coupling (no galvanic contact). All assignments above are the
firmware defaults — see §5.

---

## 4 · Bill of Materials (summary)

Full sourcing detail (MPN, distributor SKUs, unit & extended cost, lifecycle,
RoHS) lives in the CSVs. This table is the human-readable summary.

### 4.1 Canary WAP

| # | RefDes | Qty | Req? | Part | Notes |
|---|--------|-----|------|------|-------|
| 1 | U1 | 1 | **Required** | Seeed XIAO ESP32-S3 Sense | MPN **102010469**; −40…+65 °C; camera + mic onboard |
| 2 | — | 1 | **Required** | USB-C cable, **data-capable** | Not charge-only |
| 3 | SD1 | 1 | **Required** | microSD, **high-endurance** 32 GB+, FAT32 | 24/7 write-rated (SanDisk High/MAX Endurance) — not a consumer card |
| 4 | BZ1 | 1 | Optional | Passive magnetic/piezo buzzer | `CHIRP_GPIO`=2; 1–3 kHz patterns |
| 5 | Q1 | 1 | Optional | N-ch MOSFET buzzer driver (2N7002) | Only for magnetic buzzers >10 mA |
| 6 | R1 | 1 | Optional | 100 Ω series (buzzer) | Inrush limit |
| 7 | DLED1 | 1 | Optional | WS2812B addressable RGB LED | Status colors; `EXT_LED`=GPIO3 |
| 8 | R2 | 1 | Optional | 330 Ω WS2812 data series | Ringing/ESD |
| 9 | C1 | 1 | Optional | 0.1 µF X7R decoupling (WS2812) | Local bypass |
| 10 | SW2 | 1 | Optional | Reed switch **or** Hall sensor | Tamper; `TAMPER_PIN`=4, INPUT_PULLUP |
| 11 | MAG1 | 1 | Optional | Magnet for reed/Hall | Pairs with SW2 |
| 12 | SW1 | 1 | Optional | 6 mm tactile button (indoor) | Multifn; reuse onboard BOOT; sealed IP67 alt for outdoor |
| 13 | TP1 | 1 | Optional | Cap-touch electrode (Cu pad/foil) | Native touch on GPIO5; **senses through sealed wall** (best outdoor input) |
| 14 | R6 | 1 | Optional | 1 kΩ touch series (ESD) | Recommended for touch |
| 15 | BT1 | 1 | Optional | Battery — **chemistry per climate** (see §9) | LiPo indoor; LiFePO4 / Li-SOCl2 outdoor |
| 16 | R4,R5 | 2 | Optional | 100 kΩ 1% (VBAT divider) | Enables battery sense |
| 17 | M1 | 1 | Optional | L76K GNSS module | UART 9600, D6/D7; −40…+85 °C |
| 18 | ENC1 | 1 | Optional | **Polycarbonate** enclosure, IP66/NEMA 4X, UV-stab (Hammond 1554/1555) | −40…+110 °C; clear-lid option = camera window |
| 18a | VENT1 | 1 | Optional | GORE adhesive acoustic/protective vent | Buzzer sound out + pressure equalization, keeps IP rating |
| 19 | LP1 | 1 | Optional | Light pipe (gasketed) | LED to enclosure face |
| 20 | ANT1 | 1 | Optional | External 2.4 GHz u.FL antenna | If onboard insufficient; IP bulkhead for outdoor |

### 4.2 Canary Vision

| # | RefDes | Qty | Req? | Part | Notes |
|---|--------|-----|------|------|-------|
| 1 | U1 | 1 | **Required** | ESP32-C3 DevKitM-1 (Espressif) | RISC-V, 4 MB flash |
| 2 | U2 | 1 | **Required** | Seeed Grove Vision AI V2 | MPN 101021040; I2C on GPIO4/5 |
| 3 | — | 1 | **Required** | Grove 4-pin cable | Usually ships with U2 |
| 4 | — | 1 | **Required** | USB-C cable, data-capable | Power + flash |
| 5 | DLED1 | 1 | Optional | WS2812B RGB LED | `EXT_LED`=GPIO3 (onboard `LED_BUILTIN`=GPIO8) |
| 6 | SW1 | 1 | Optional | 6 mm tactile button | Or reuse onboard BOOT (GPIO9) |
| 7 | BZ1 | 1 | Optional | Passive buzzer (**unpopulated**) | `BUZZER_PIN_DEFAULT`=2; needs firmware fork |
| 8 | BT1 | 1 | Optional | Battery + external charger/DC-DC | C3 DevKit has **no charger** — see §6.5 & §9 |
| 9 | ENC1 | 1 | Optional | **Polycarbonate** enclosure w/ **clear lid** | Hammond 1554/1555 + clear lid = camera window; UV-stab IP66 |

---

## 5 · Pin allocation map

All values below are taken **verbatim** from the board pin headers and the
chirp driver — they are the firmware defaults, so a board wired this way needs
no code changes.

### 5.1 Canary WAP — XIAO ESP32-S3 Sense
Source: [`pins.h`](../../firmware/boards/xiao-esp32s3-sense/pins/pins.h),
[`audible_chirp.h`](../../firmware/projects/canary-wap/arduino/canary_wap/audible_chirp.h)

| Peripheral | RefDes | XIAO pin | GPIO | Direction / mode | Firmware symbol |
|------------|--------|----------|------|------------------|-----------------|
| Passive buzzer | BZ1 | D1 | 2 | LEDC PWM out | `CHIRP_GPIO = 2` |
| External RGB LED | DLED1 | D2 | 3 | Digital out (active HIGH) | `EXT_LED_PIN_DEFAULT = 3` |
| Tamper switch | SW2 | D3 | 4 | Input, `INPUT_PULLUP`, active LOW | `TAMPER_PIN_DEFAULT = 4` |
| Cap-touch pad | TP1 | D4 | 5 | Touch (touch-capable GPIO) | (touch peripheral) |
| Multifunction button | SW1 | BOOT | 0 | Input, active LOW | `BOOT_BUTTON_PIN = 0` |
| Battery sense | — | D0 / A0 | 1 | ADC1_CH0, 2:1 divider | `VBAT_PIN = 1`, `VBAT_DIVIDER_RATIO = 2.0` |
| Onboard status LED | — | — | 21 | Digital (fallback) | `LED_BUILTIN = 21` |

**Reserved — do not repurpose:** D6/D7 (GPIO43/44) = L76K GNSS UART · D8–D10
(GPIO7/8/9) = microSD SPI · GPIO21 = SD CS / onboard LED · camera (GPIO10–18,
38–48) and PDM mic (GPIO41/42) are hardwired · GPIO26–33 = flash/PSRAM bus.

> **Pin budget:** with buzzer (2), RGB LED (3), tamper (4) and touch (5)
> populated, **all four optional inputs/outputs coexist** without touching any
> reserved pin. The multifunction button reuses BOOT (GPIO0), so it costs no
> expansion pin. Battery sense shares D0/A0 (GPIO1) — fine, since it is analog
> and read-only.

### 5.2 Canary Vision — ESP32-C3
Source: [`firmware/boards/esp32-c3/pins/pins.h`](../../firmware/boards/esp32-c3/pins/pins.h)

| Peripheral | RefDes | GPIO | Notes |
|------------|--------|------|-------|
| Grove Vision AI V2 (I2C) | U2 | SDA 4 / SCL 5 | `I2C_PIN_SDA/SCL`; Grove white/yellow |
| External RGB LED | DLED1 | 3 | `EXT_LED_PIN_DEFAULT = 3` |
| Onboard LED | — | 8 | `LED_BUILTIN = 8` (WS2812 or std) — strapping pin |
| Multifunction button | SW1 | 9 | `BOOT_BUTTON_PIN = 9` — strapping pin |
| Buzzer (unpopulated) | BZ1 | 2 | `BUZZER_PIN_DEFAULT = 2` — strapping pin |

**Reserved — do not use:** GPIO11 = flash VDD (`PIN_RESERVED_FLASH_VDD`) ·
GPIO12–17 may conflict with flash · GPIO2/8/9 are **strapping** pins (avoid
strong pull at boot) · ADC2/GPIO5 is shared with WiFi.

---

## 6 · Wiring & assembly

### 6.1 Audible chirp (BZ1)
- The driver runs the ESP32 **LEDC** peripheral: `ledcAttach(gpio, 2000, 8)`
  — 2 kHz base timer, 8-bit resolution, ~50 % duty. It expects a **passive**
  transducer (it generates the tone), **not** a self-driving active buzzer.
- **Piezo passive** transducers draw only a few mA and may be driven **directly**
  from GPIO2 through `R1` (100 Ω). **Magnetic** passive buzzers can exceed the
  GPIO drive limit — drive them through `Q1` (2N7002 N-MOSFET, gate to GPIO2,
  buzzer between V+ and drain) with a flyback diode if inductive.
- Acoustic output must align with an **enclosure vent** (§6.6) or it will be
  muffled below useful SPL.

### 6.2 RGB status LED (DLED1)
- WS2812B `DIN` ← GPIO3 through `R2` (330 Ω). Place `C1` (0.1 µF) across the
  pixel's V+/GND right at the package.
- The chip is 5 V-native but operates on the XIAO's 3.3 V logic in practice; for
  long runs or marginal levels, add a level shifter (not required for a single
  on-board pixel). Active-HIGH per `EXT_LED_ACTIVE = HIGH`.

### 6.3 Tamper switch (SW2)
- Reed/Hall between GPIO4 and GND. Firmware sets `INPUT_PULLUP`, active LOW
  (`TAMPER_ACTIVE = LOW`) — so a **normally-closed** sensor held shut by `MAG1`
  reads "closed/secure," and opening the enclosure (magnet leaves) trips it.
- The internal pull-up is sufficient; external `R3` (10 kΩ) only if you want a
  stiffer pull or external filtering. Enable `FEATURE_TAMPER_GPIO = 1`.

### 6.4 Multifunction button (SW1) & cap-touch (TP1)
- **Button:** the firmware already gates physical-presence actions on the BOOT
  button (`boot_button_held()`, hold ≥ `CONFIG_BOOT_BUTTON_HOLD_MS = 2000` ms).
  Fit `SW1` in parallel with BOOT to GND for a panel-mountable button, or simply
  use the board's BOOT button — no extra wiring.
- **Touch:** route a small copper pad/foil `TP1` to GPIO5 through `R6` (1 kΩ,
  ESD) and keep a ground guard ring away from it. Sealable behind the enclosure
  wall for a no-moving-parts, weather-resistant control.

### 6.5 Battery (BT1) & monitoring

> ⚠️ **Battery safety — read before sourcing or wiring any cell.**
> Lithium cells (LiPo, Li-ion, LiFePO4, Li-SOCl2) can **vent, catch fire, or
> rupture** if over-charged, over-discharged, short-circuited, charged outside
> their rated temperature window, physically damaged, or paired with the wrong
> charger. Treat the guidance below as a **starting point only** — it does **not**
> replace the cell and charger manufacturers' datasheets, safety data sheets, or
> instructions, nor applicable standards (**IEC 62133, UL 2054/1642, UN 38.3**
> for transport, plus local fire/electrical codes).
> - Use **only certified cells/packs with integral protection (PCM/BMS)** and a
>   charger matched to the **exact** chemistry and voltage. Never use bare,
>   uncertified, salvaged, or damaged cells.
> - Charge **temperature-gated** (battery thermistor / BMS), within the
>   manufacturer's window, and **never unattended outside spec.**
> - Have any battery power/charge/enclosure design **reviewed by a qualified
>   engineer** and certified where required. SecuraCV provides no warranty and
>   accepts no liability — **you assume all risk** (see document disclaimer).

- XIAO ESP32-S3 has an **onboard charger sized for Li-ion/LiPo (4.2 V)** on the
  BAT+/BAT− pads; connect a 3.7 V LiPo via JST-PH 2.0. The C3 DevKitM-1 has
  **no charger** — add an external charge/boost module if battery-powering Vision.
- **Chemistry is climate-dependent — see §9.** The onboard charger is LiPo/Li-ion
  only; **it will not correctly charge LiFePO4** (3.6 V) — outdoor/wide-temp
  builds need an external charge controller (or a Li-SOCl2 primary cell with a
  buck and no charging at all).
- Battery sense: `VBAT → R4(100 kΩ) → GPIO1(A0) → R5(100 kΩ) → GND`
  (`VBAT_DIVIDER_RATIO = 2.0`). The firmware auto-detects whether the divider is
  present and falls back to software estimation if not.
- Observe LiPo safety: use cells with integral protection (PCM), respect JST
  polarity, never charge unattended outside spec, and **never charge a LiPo below
  0 °C** (lithium plating) — many bare charge ICs do not enforce this, so add a
  thermistor cutoff for anywhere it can freeze.

### 6.6 Enclosure (ENC1)
- **Indoor / bench:** a general-purpose ABS box (e.g., Hammond 1551) is fine.
  **Outdoor / unconditioned:** use a **UV-stabilized polycarbonate** enclosure
  rated **IP66/67, NEMA 4X** (Hammond **1554/1555** — −40…+110 °C, IK08, UL
  listed). The 1551 ABS box originally listed is **indoor-only** and is *not*
  the outdoor part.
- Provide: a **light-pipe** (`LP1`, gasketed) from DLED1 to the face; for the
  buzzer, a **GORE adhesive acoustic vent** (`VENT1`) instead of a bare hole —
  it passes sound and equalizes pressure while keeping IP67/68; a **camera/sensor
  window** (the **clear-lid** 1554/1555 option is ideal — keep it untinted for
  the camera); and a sealed USB-C access port or pigtail gland.
- Mount the tamper magnet on the lid so opening the enclosure separates `MAG1`
  from `SW2`. A 3D-printable enclosure (STL) is a planned follow-up and is out
  of scope for this revision.

---

## 7 · Audible chirp pattern reference

Exact note tables from
[`audible_chirp.h`](../../firmware/projects/canary-wap/arduino/canary_wap/audible_chirp.h)
(`{frequency Hz, duration ms}`, `0 Hz` = silence). Useful for buzzer
SPL/frequency-response selection — pick a transducer whose resonant peak sits in
the **1–3 kHz** band.

| Pattern | Meaning | Notes |
|---------|---------|-------|
| `CONFIRM` | "I'm here" (setup) | 2000·100, –50, 2000·100 |
| `ALERT` | Attention needed | 1000·150, 1500·150, 2000·200 (rising) |
| `TAMPER` | Tamper detected | 3000·80 ×5 (rapid) |
| `SUCCESS` | Operation OK | 523·100, 659·100, 784·150 (C5-E5-G5) |
| `ERROR` | Operation failed | 400·200, 300·300 (descending) |
| `BEACON` | Beacon-channel alarm | 1200·150, 1700·150, 2200·200 |
| `SELFTEST_OK` | Monthly self-test | 1500·80 (one quiet tone) |

---

## 8 · Power budget (estimate)

> ⚠️ **Estimate only.** The firmware sources reviewed for this BOM contain **no
> measured current figures**. The values below are typical-datasheet
> assumptions for the named parts and ESP32-S3 radio states; **measure on real
> hardware before committing to a battery size.**

| Subsystem | Typical | Peak | Assumption |
|-----------|--------:|-----:|------------|
| ESP32-S3 (WiFi AP active) | ~110 mA | ~350 mA (TX) | Espressif typical |
| ESP32-S3 (modem-sleep idle) | ~25 mA | — | Between records |
| OV2640 camera (active) | ~60 mA | ~120 mA | Only during peek/QR |
| Buzzer BZ1 (piezo) | <5 mA | ~10 mA | While chirping (<1 s) |
| WS2812 RGB | ~1 mA | ~60 mA | Per-pixel, full white |
| L76K GNSS (acquiring) | ~25 mA | ~40 mA | Cold fix |

**Worked example (assumptions):** average draw ~90 mA on a 1500 mAh LiPo at
~80 % usable → ≈ **13 h** continuous WAP operation; substantially longer with
duty-cycled records / modem-sleep. **Validate empirically.**

---

## 9 · Climate & environmental durability

A Canary may live anywhere from a hallway to an exposed wall. **The MCU is the
easy part** (−40…+65 °C for the XIAO ESP32-S3 Sense; −40…+85 °C for the C3) —
the limiting components are the **battery, enclosure, microSD, and any exposed
mechanical input**. Pick a deployment tier and source accordingly.

### 9.1 Per-part environmental rating

| Part | Temp range | Outdoor verdict | Action for harsh climate |
|------|-----------|-----------------|--------------------------|
| XIAO ESP32-S3 Sense (U1) | −40…+65 °C | OK | Watch self-heating in a sealed box; the camera runs warm |
| ESP32-C3 DevKitM-1 (U1, Vision) | −40…+85 °C | OK | — |
| **LiPo battery (default BT1)** | charge **0…45 °C**, discharge −20…60 °C | ❌ **not for sub-freezing/hot** | Switch chemistry (§9.2) |
| LiFePO4 (BT1-ALT1) | charge 0…55 °C, discharge −20…60 °C | ⚠️ better in heat | Add low-temp charge cutoff; needs external charger |
| Li-SOCl2 primary (BT1-ALT2) | −40…+85 °C | ✅ best wide-temp | Non-rechargeable; buck regulator |
| microSD, high-endurance (SD1) | −25…+85 °C (industrial −40) | ✅ | Use High/MAX Endurance; avoid consumer cards |
| Reed switch (SW2) | −40…+125 °C, hermetic | ✅ excellent | Preferred tamper sensor outdoors |
| Neodymium magnet (MAG1) | demag > ~80 °C; corrodes bare | ⚠️ | Ni/epoxy coating; SmCo if hot |
| Tactile button B3F (SW1) | not sealed | ❌ outdoors | Sealed IP67 button, or use cap-touch (TP1) |
| **Cap-touch pad (TP1)** | n/a | ✅ **best outdoor input** | Sense through a sealed wall — no opening at all |
| Piezo buzzer (BZ1) | sealed variants to IP67 | ✅ | Sealed piezo or mount behind GORE vent |
| ABS enclosure (Hammond 1551) | indoor, UL94-HB | ❌ outdoors | Use polycarbonate 1554/1555 |
| **Polycarbonate enclosure (1554/1555)** | −40…+110 °C, IP66/67/68, UV-stab | ✅ | The outdoor enclosure |
| GORE acoustic vent (VENT1) | IP67/68 ePTFE | ✅ | Keeps sealing while buzzer/pressure breathe |

### 9.2 Battery decision (the #1 climate trap)

> A LiPo is the convenient default because the XIAO charges it directly — but it
> is the **wrong part for anywhere that freezes or bakes.** Charging a LiPo below
> 0 °C plates lithium and permanently damages it; sustained heat swells it.

| Deployment | Recommended chemistry | Why |
|------------|----------------------|-----|
| Indoor / climate-controlled | **LiPo** (default BT1) | Charges off the onboard XIAO charger; simplest |
| Outdoor, occasionally freezing, mains/solar topped | **LiFePO4** + low-temp-cutoff charger | Safer in heat, long cycle life; **external charger required** |
| Remote / set-and-forget / extreme temp | **Li-SOCl2 primary** (e.g., Tadiran) | −40…+85 °C, ~years of shelf life, ultra-low self-discharge; not rechargeable |

> ⚠️ **Safety notice.** The chemistries above differ in their hazards as well as
> their temperature range; selecting one does **not** make a design safe.
> Re-read the **battery safety box in §6.5** and follow the cell/charger
> manufacturer datasheets and the standards listed there. Use certified,
> protection-equipped cells/packs and a chemistry-matched, temperature-gated
> charger; have the design reviewed by a qualified engineer. This table is
> guidance, **not** a safety certification, and carries no warranty.

In all rechargeable cases, charging must be **temperature-gated** (battery
thermistor / BMS) to the manufacturer's safe window — bare charge ICs typically
are not, so do not rely on them alone.

### 9.3 Sealing & condensation
- Target **IP66/IP67** for outdoor; pair every penetration (LED, buzzer, button,
  antenna, USB-C) with a gasket, gland, or GORE vent — a single bare hole voids
  the rating.
- Add the **GORE vent** even on otherwise "sealed" boxes: it equalizes pressure
  across day/night thermal cycling so the gaskets don't get sucked in and so
  condensation can escape. Consider a small desiccant pack inside.
- Conformal-coat the PCB and the WS2812 for high-humidity / coastal sites.

## 10 · Compliance & quality notes

- **RoHS / REACH:** prefer RoHS-compliant, REACH-SVHC-free parts; the
  `RoHS` column in the CSVs flags each line. Mark NRND/EOL parts in `Lifecycle`.
- **Audible signalling:** `PATTERN_SELFTEST_OK` is designed as a quiet monthly
  self-test consistent with **NFPA 72 §14** cadence; `PATTERN_BEACON` uses a
  deliberately distinct 1200/1700/2200 Hz sequence chosen to avoid impersonating
  reserved alert tones (**47 CFR §10 / §11**). A firmware build check fails if a
  reserved frequency pair is introduced — do not retune the buzzer patterns
  without re-reading `audible_chirp.h`.
- **Lithium battery (safety-critical):** use only certified cells/packs with
  integral protection (PCM/BMS) meeting **IEC 62133 / UL 2054 / UL 1642**; charge
  only with a chemistry-matched, temperature-gated charger; transport per
  **UN 38.3** and local regulations; recycle per local e-waste rules. See the
  full battery safety notice in **§6.5** and the document disclaimer. This
  document is informational and confers no warranty or safety certification.

---

## 11 · Sourcing & revision

- Distributor SKUs in the CSVs (Mouser / DigiKey / LCSC) are **indicative** and
  were not live-verified at authoring time — confirm availability, MOQ, and
  current pricing before ordering, and update the `Lifecycle` column for any
  NRND/EOL parts.
- The XIAO ESP32-S3 Sense, L76K, and Grove Vision AI V2 are most reliably
  sourced from **Seeed Studio** and its authorized resellers.

| Rev | Date | Author | Change |
|-----|------|--------|--------|
| A | 2026-05-28 | SecuraCV | Initial release: WAP + Vision peripheral BOM. |
| B | 2026-05-28 | SecuraCV | Sourcing review: corrected MCU SKU (102010469), high-endurance microSD, polycarbonate IP66 enclosure (1554/1555), climate-tiered battery guidance, GORE vent; added §9 environmental durability. |

### References
- Buzzer driver: [`audible_chirp.h`](../../firmware/projects/canary-wap/arduino/canary_wap/audible_chirp.h)
- WAP pins: [`pins.h`](../../firmware/boards/xiao-esp32s3-sense/pins/pins.h) · Vision pins: [`esp32-c3/pins/pins.h`](../../firmware/boards/esp32-c3/pins/pins.h)
- Build profiles: [`build_config.h`](../../firmware/projects/canary-wap/arduino/canary_wap/build_config.h) · Feature flags: [`config.h`](../../firmware/configs/canary-wap/default/config.h)
- RF "Chirp Channel" (not this doc): [`spec/chirp_channel_v0.md`](../../spec/chirp_channel_v0.md)
- Board overview: [`firmware/boards/README.md`](../../firmware/boards/README.md) · Setup: [`getting_started_canary.md`](../getting_started_canary.md)
