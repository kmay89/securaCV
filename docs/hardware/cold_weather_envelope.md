# Cold‑weather operating envelope for outdoor Canaries (reference)

**Status:** reference / design note — the canonical "how cold can it run?" answer for every outdoor
Canary (Fence Guard, Ranger, Feeder, Clime, Curbwatch, Gatekeeper, a vehicle node). Written once here
so the device dossiers cite it instead of re‑deriving it. Figures are datasheet/vendor numbers, cited;
where a source blocks automated fetch it's flagged.

**The one‑paragraph answer.** For a *rechargeable* (solar) node the limit is almost always the **0 °C
charge floor** — lithium physically cannot be charged below freezing without permanent damage — not
discharge and not the silicon. For a *primary‑cell* node the battery drops out of the discussion
(LiSOCl₂ runs to **−55 °C**) and the real floor becomes the **−40 °C‑rated silicon and sensors**.
**Camera nodes are the exception:** the visible image sensor (usable only ≳ 0 °C) and winter solar
starvation pull their floor up to roughly **−10 to −20 °C**, warmer than everything else.

---

## 1 · The battery is usually the limit — chemistry by chemistry

| Chemistry | Discharge floor | Charge floor | The honest note |
|---|---|---|---|
| **Li‑ion / LiPo** (rechargeable) | ~**−20 °C** (loses ~25–40% below −10 °C) | **0 °C — hard** | below 0 °C, charging plates metallic lithium on the anode: **irreversible** capacity loss + short/thermal‑runaway risk. A *single* sub‑freezing charge does lasting damage, and it "looks" normal while it happens. |
| **LiFePO4** (rechargeable) | ~**−20 °C** | **0 °C — just as strict** | its win is *not* colder charging — it's graceful cold discharge, safety, and cycle life. This is why solar builds standardize on it. |
| **LiSOCl₂ primary** (Tadiran/Saft, non‑rechargeable) | **−55 °C** (−80 °C special) | n/a — never charged | the cold champion, ~40‑yr shelf life, <1%/yr self‑discharge. **Catch:** high impedance → only ~µA–low‑mA continuous, can't source a LoRa TX pulse alone → needs a buffer (§3). |
| **Alkaline** | ~**−18 °C** (~50% capacity, R doubles) | n/a | dies in cold, fails high‑drain first, can leak. Not a serious cold contender. |
| **Supercap / HLC** | ~**−40 °C** | — | a *pulse buffer*, not a store. **ESR rises ~2× by −20 °C, up to ~9× at −40 °C** — size on the *cold* ESR, not the 25 °C number. |

**The asymmetry that decides everything for solar nodes:** discharge to −20 °C is fine, but **charging
stops dead at 0 °C.** In a sustained freeze a solar node **coasts on stored charge only** and drains —
no top‑up happens while frozen. So in cold climates you either **size the battery for the whole cold
spell with zero recharge**, accept winter downtime, or **use a primary cell** and sidestep the wall
entirely (the [Gatekeeper](./canary_gatekeeper_research.md) finding: for a µA‑class node a multi‑year
LiSOCl₂ cell often beats solar).

**The self‑heating‑pack workaround** (a BMS heater film warms cells above 0 °C before charging, kicking
in ~< 5 °C) is real but costs a **parasitic energy tax** — it burns scarce winter solar to warm cells —
so on a solar‑starved node it's often net‑negative; better for nodes with a real power source.

---

## 2 · What the silicon and sensors can take

**Rated to −40 °C (the whole low‑power sensing stack):** ESP32‑S3/C6 & nRF52840 MCUs, Semtech SX1262
LoRa, **LIS3DH** accelerometer, **SHT4x/BME280** temp‑humidity, **MLX90640** thermal array, and a **reed
switch** (essentially temperature‑independent, tested well below −40 °C). One caveat: some ESP32‑S3R8
PSRAM variants are only −40 to +65 °C.

**NOT −40 °C — the weak links:**
- **Visible camera modules (OV2640/OV5647):** silicon spec ~−20/−30 °C, but **stable image quality only
  ≳ 0 °C.** This is *the* reason camera nodes can't go as cold. (A thermal array is the exception at
  −40 °C.)
- **Displays:** standard **e‑ink only to 0 °C** (refresh slows/ghosts near freezing; wide‑temp EPD
  reaches −15/−25 °C as a separate part); **LCDs** smear then blank in cold. Design local UI to tolerate
  "no display below spec."

*Marketing‑vs‑spec flag:* a camera module's quoted "operating temperature" is the *silicon* number
(−20/−30 °C); **usable image** is 0–50 °C. Don't confuse them.

---

## 3 · The LoRa‑pulse‑on‑a‑cold‑cell problem (and the fix)

A LoRa TX burst is ~100–130 mA at the SX1262 (up to ~1–3 A at pack level on high‑PA designs) for tens–
hundreds of ms. A bobbin LiSOCl₂ cell's **high internal impedance — worse cold, worse after
passivation** — droops under that pulse and can brown out the MCU. **Fix: a parallel HLC (Tadiran
PulsesPlus) or supercap buffer** — the cell trickle‑charges it during the long idle, the cap delivers
the pulse (and it cures passivation voltage‑delay too). Rough sizing: a **~1 F‑class HLC** covers ~2 A
pulses comfortably; **size using the cold ESR** (several× the 25 °C value), and match the buffer voltage
to the cell.

---

## 4 · The honest floor, per device class

| Class | Power stack | Realistic cold floor | Set by |
|---|---|---|---|
| **Contact / vibration / radar** (Gatekeeper, Fence Guard, Ranger, Chore) | LiSOCl₂ + HLC buffer | **≈ −40 °C** | the −40 °C‑rated MCU/radio/sensors — *not* the battery (good to −55 °C in reserve) |
| **Camera / higher‑power** (Feeder, Poolwatch, Curbwatch, Vision) | solar + LiFePO4 | **≈ −10 to −20 °C** | the visible image sensor (usable ≳ 0 °C) **and** winter solar starvation + the 0 °C charge floor |

> **Contact/vibration/radar node:** *"On a LiSOCl₂‑plus‑HLC power stack, reliable operation to about
> −40 °C — limited by the −40 °C‑rated MCU, LoRa radio, and sensors, not the battery, which itself holds
> to −55 °C."*
>
> **Camera / higher‑power node:** *"Usable operation to roughly −10 to −20 °C — limited first by image
> quality (visible sensors are stable only to ~0 °C) and then by the winter energy budget: a rechargeable
> pack can't recharge below 0 °C, so in a sustained freeze it coasts until it drains. A thermal‑array
> variant tolerates −40 °C hardware but stays energy‑limited."*

---

## 5 · The failure modes that aren't the battery or the chip

- **Condensation / frost / ice — the dominant real‑world killer.** Diurnal cycling pumps moist air in
  and out of the enclosure → internal condensation → frost on PCB, connectors, lens, and the *solar
  cell* (ice on the panel blocks charging entirely, compounding the 0 °C floor). Mitigate with conformal
  coating, a Gore‑type breather vent + desiccant, potting, and a sealed IP67 enclosure — the same
  vent‑not‑sealed lesson as [Canary Feeder](./canary_feeder_research.md) §5.
- **Ice on the moving part — a *sensing* failure with a healthy sensor.** Ice can widen a reed‑magnet
  gap (missed closures), freeze a gate so the contact never actuates, or change a fence node's vibration
  signature. The electronics are fine; the world froze.
- **Real deployment lessons (worth copying):** a fleet of **Meshtastic winter solar nodes in the
  Canadian Rockies ran ~2 years with zero failures** using **LiFePO4** + two tricks — **charge only
  during the warmest part of the day** (discharge overnight in the cold), and let the **sun‑warmed
  plastic enclosure act as a heat trap** raising internal temp above ambient — plus very low charge
  rates (<0.05C). Arctic/industrial sensors standardize on **LiSOCl₂ + HLC** precisely to sidestep
  recharge and run to −55 °C.

---

## 6 · Design rules that fall out of this

1. **Cold climate + µA‑class node → prefer a multi‑year primary cell (LiSOCl₂ + HLC)** over solar; it
   dodges the 0 °C charge wall and the whole solar‑starvation problem (Gatekeeper).
2. **Cold climate + solar node → LiFePO4 + a low‑temp‑charge‑cutoff BMS**, sized for winter autonomy
   *with zero recharge during the freeze*; charge warm‑of‑day; use the enclosure as a heat trap; expect
   winter downtime rather than defying the 0 °C rule.
3. **Buffer every LoRa TX with an HLC/supercap** on any primary‑cell node, sized on cold ESR.
4. **Cameras are a warmer‑climate / warmer‑season proposition** (≈ −10/−20 °C floor); if you truly need
   sub‑freezing "vision," use a **thermal array** (−40 °C) and solve the energy separately.
5. **Seal against condensation with a vent, not a sealed box** (a sealed box fogs; §5).

Pair this with the [solar & battery sizing guide](./solar_power_sizing.md) — cold decides the *chemistry
and the recharge reality*; sizing decides *how much panel and battery* for your worst month.

---

*Sources: Battery University BU‑410 (cold charging / lithium plating) & BU‑502 (cold discharge);
Tadiran low‑temperature LiSOCl₂ + PulsesPlus HLC; LiFePO4 vendor temp ranges (LiTime, Ace); component
datasheets (Espressif ESP32‑S3, Nordic nRF52840, Semtech SX1261/2, ST LIS3DH, Sensirion SHT4x, Bosch
BME280, Melexis MLX90640, OmniVision OV2640/OV5647, Littelfuse reed app‑note, Pervasive/Good Display
wide‑temp EPD); YYCMesh cold‑weather‑charging field report; ScienceDirect Li/SOCl₂+capacitor. A few
(Battery University, Tadiran, YYCMesh) return 403 to automated fetch — figures here came via search
extracts corroborated across vendor pages; open those in a browser for verbatim datasheet quotes.*
