# Canary Clime — environmental & power wellbeing witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Indoor (with an
outdoor‑air variant). Environmental readings are framed as **non‑diagnostic wellbeing signals** (like
[Canary Sense](../canary_sense_mr60bha2_design.md)'s vitals); the useful *away* moments are **coarse
threshold alerts** carried by the [alert relay](../design/alert_relay.md).

**The one‑sentence version:** a small node that watches a room's **temperature, humidity, and air
quality** — and whether it still has **power** — and pokes your phone when the room leaves your pet's
safe band, the air turns smoky or stuffy, the HVAC seems to have failed, or the power goes out.

This one concept folds three asks — *pet room‑temp / thermostat monitoring*, *air quality*, and
*power‑outage alerting* — into a single environmental node, because they share sensors, thresholds,
and (above all) the same remote‑alert transport.

---

## 1 · Temperature & humidity — the easy half, done honestly

- **Sensor:** **Sensirion SHT4x** (accurate T/RH, I²C, ~$3–8) or **Bosch BME280** (adds pressure).
  Cheap, accurate, low‑power. Mount away from self‑heating parts.
- **Species/zone safe bands, not one threshold.** A reptile vivarium, a bird room, a dog's crate, and
  a garage each have different safe ranges; Clime holds a **configurable safe band per zone/animal**
  and alerts on exit. (Cold‑blooded pets in a failed vivarium heater are a *fast* emergency — worth a
  tighter band and a louder alert.)
- **The clever part is detecting HVAC *failure*, not just reading a number.** A fixed threshold is
  slow and dumb. Watch **rate‑of‑change**: temperature **rising when the AC should be cooling**, or
  **falling in winter** past a slope, means the furnace/AC (or the power) is down — often minutes
  before a fixed threshold trips. Pair with the mains‑power signal (§3) to distinguish "HVAC broke"
  from "power's out."

---

## 2 · Air quality — integrate first, own it later

Air quality is the same environmental node's job. The research verdict is clear:

- **Integrate [AirGradient](https://github.com/airgradienthq/arduino) now (zero new firmware).** It's
  the only option genuinely open in *both* hardware and firmware, ESP32‑C3‑based, and it exposes a
  documented **local HTTP API (`GET /measures/current`) + native MQTT** — point its `mqttBrokerUrl` at
  our broker, or poll it, and the adapter maps its JSON to wellbeing signals + threshold alerts. No
  cloud, no fork.
- **Build our own node later (the never‑rot v2):** XIAO ESP32 + **Sensirion SEN5x** (PM1/2.5/10 +
  VOC/NOx index + T/RH in one) **+ SCD41** (true NDIR CO₂), signing claims on‑device. (Skip forking
  AirGradient's firmware — its CC BY‑SA copyleft plus inheriting their board is the worst of both.)

**The honest thresholds** (as coarse alerts, debounced):

- **PM2.5 → US EPA AQI 101** (24‑h PM2.5 > 35.4 µg/m³) is the first "Unhealthy for Sensitive Groups"
  alert; escalate at AQI 151. **Wildfire smoke is the killer use case** — a **rate‑of‑change** spike
  (PM2.5 climbing sharply over minutes) fires *before* the 24‑h average catches up, and pets at floor
  level are more exposed.
- **CO₂ is a ventilation proxy, not toxicity.** Frame honestly: the famous **"1000 ppm = unhealthy"
  is a retired ASHRAE myth** (it was a body‑odor comfort proxy). Practical: **sustained > ~1000 ppm =
  "ventilate," > ~1400 ppm = "poorly ventilated."** Never "danger."

**Two hard rules the design must encode (both are common, dangerous marketing lies):**

1. **"eCO₂" is NOT CO₂.** SGP41/BME680 output a VOC‑derived *guess* labeled eCO₂ — right only for
   human‑occupancy, wrong for cooking/cleaning/off‑gassing. If we want CO₂, we use a **true NDIR**
   sensor (SCD4x / SenseAir S8). Never present eCO₂ as CO₂.
2. **CO ≠ CO₂.** Carbon monoxide is an acute, odorless *killer* (furnaces, generators) needing a
   **dedicated electrochemical sensor and a hard life‑safety alarm** — logically separate from
   everything above. Clime does **not** replace a UL‑listed CO alarm, and must say so.

---

## 3 · Power — the outage witness, and why it's the same design problem

Losing power is the alert whose *transport must survive losing power* — so it's designed hand‑in‑hand
with the [alert relay](../design/alert_relay.md) §5, not bolted on:

- **Sense mains presence** (an opto/mains‑present input, or simply "the mains‑powered node is up").
- **Detect the outage by ABSENCE, not a heroic last gasp.** The mains node heartbeats; a **battery/
  solar or mesh peer** (or the hub on a UPS) infers the outage from the *silence* — the same
  presence‑via‑active‑signal/absence‑via‑timeout idiom used for Car Mode, Guardian, and the meshtastic
  adapter. A supercap "I'm losing power" last chirp is a **best‑effort bonus**, never trusted (the
  Meshtastic OFF‑transition is unreliable, and a supercap last‑gasp is timing‑critical).
- **Egress over the mesh:** because WiFi/internet die too, the poke leaves over **LoRa/Meshtastic to
  an independently‑powered gateway** that runs the relay. This is *the* reason a climate node for pets
  wants a mesh path, not just WiFi.
- **The payoff for pets:** power out → HVAC dead → the room starts drifting (§1's rate‑of‑change) →
  you get told while you can still act, even though the house is dark and offline.

---

## 4 · Claim mapping & privacy

- **Continuous readings → wellbeing signals** (T/RH, PM2.5, CO₂, VOC index): non‑diagnostic, **P1
  opt‑in, HA‑only, never sealed‑logged** — exactly Sense's vitals discipline. The descriptor allowlist
  makes it structurally impossible for an air number to seal a witness event.
- **Threshold crossings → coarse alerts** via the relay (room out of safe band, AQI 101, smoke spike,
  CO₂ high, mains lost). These primarily *notify*; a coarse sealed event is optional.
- **Mains‑lost / tamper‑adjacent events** can reuse **`TamperDetected`** (a corroborating environmental
  signal) where an owner wants it in the witness log.
- **No camera, no PII, no new claim vocabulary** — reuses the wellbeing channel + `TamperDetected` +
  the relay.

---

## 5 · Cheapest reliable BOM

| Part | Choice | ~$ |
|---|---|---|
| MCU | XIAO ESP32‑S3/C6 (WiFi/BLE; add Wio‑SX1262 for the mesh outage path) | $5–14 |
| Temp/RH | Sensirion SHT4x (or BME280 for pressure) | $3–8 |
| Air quality (own node) | Sensirion SEN5x (PM+VOC+T/RH) + SCD41 (NDIR CO₂) | $65–90 |
| Air quality (integrate) | an off‑the‑shelf **AirGradient ONE** via its local API/MQTT | (its price) |
| CO (life‑safety, separate) | dedicated electrochemical CO sensor **or** keep the owner's UL CO alarm | — |
| Mains sense | opto/mains‑present input + a small supercap/cell for the last‑gasp bonus | $2–5 |

---

## 6 · Never let it rot

- **One relay, many signals** — every Clime alert flows through the [alert relay](../design/alert_relay.md);
  adding a channel is a hub config line.
- **Integrate‑then‑own** for air quality — ship value on AirGradient's open local API now, migrate to
  a sign‑on‑device Sensirion node later without changing the wellbeing/alert model.
- **Shared cold‑battery rule** for any solar/outdoor‑air or mesh‑gateway variant: *no lithium charges
  below 0 °C; size for autonomy* — cited from [Fence Guard](./canary_fence_guard_research.md)/
  [Ranger](./canary_ranger_research.md), not re‑derived.
- **Honest tiering** — `concept` until built; wellbeing signals ship labeled non‑diagnostic; the
  eCO₂/CO honesty rules are load‑bearing, not footnotes.

---

## 7 · Open items

- **No bench unit.** Safe‑band defaults per species, the HVAC‑failure rate‑of‑change thresholds, and
  the smoke‑spike slope all need real‑room tuning + golden vectors.
- **AirGradient integration vs. own node** is a real v1/v2 sequencing decision (§2) — v1 integrates,
  v2 signs on‑device.
- **The mesh‑gateway outage path** (§3) is shared, unbuilt work with the alert relay and the Fence
  Guard/Ranger mesh.
- **CO is deliberately out of the coarse‑wellbeing scope** — if we ever add it, it's a dedicated
  life‑safety alarm with its own certification conversation, never a soft signal.
- The **"away" policy** (when a threshold escalates to a remote poke vs. a quiet log entry) is the
  relay's open item, shared here.

---

*Sources: AirGradient open HW/FW + local API/MQTT; Sensirion SEN5x / SCD4x / SHT4x datasheets; EPA
2024 AQI PM2.5 breakpoints; ASHRAE indoor‑CO₂ position doc (the retired 1000 ppm myth); WHO 2021 PM2.5
targets; the eCO₂‑vs‑NDIR and CO‑vs‑CO₂ distinctions. Full URLs live with the research that produced
this doc; the remote‑alert + outage transport is [`../design/alert_relay.md`](../design/alert_relay.md);
the wellbeing‑channel discipline is [`../canary_sense_mr60bha2_design.md`](../canary_sense_mr60bha2_design.md) §2.2.*
