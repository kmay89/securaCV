# Canary Gatekeeper — solar gate‑open witness (research & design dossier)

**Status:** concept — a design, **no firmware, no bench unit**. The simplest concept in the fleet: it's
mostly an *assembly of bricks we already have* (Fence Guard's sensor/mesh/solar research, the
`ContactStateChange` claim, the alert relay, the absence‑inference idiom).

**The one‑sentence version:** a coin‑sized, ultra‑low‑power node you **stick on a gate** that tells you
when it **opens, closes, or gets left open** — and, if you want, when someone's **rattling or climbing
it** — running for years on a tiny cell or forever on a scrap of solar.

---

## 1 · The sensor — two honest options, one recommended

A gate opening is a **state change**, and there are two clean ways to sense it. Both are trivially
low‑power because the MCU **deep‑sleeps and wakes on a hardware interrupt** — it does nothing until the
gate actually moves.

**Recommended (matches "just from movement, mounted on it"): an accelerometer — LIS3DH.** A single
stick‑on unit on the gate leaf, the *same part* Fence Guard specs. It reads the gate's **resting tilt**
(open vs. closed as an angle) and its **motion** (the swing), on a **wake‑on‑motion interrupt** so it
sleeps at ~single‑µA between events. Three wins over a contact switch:
- **One self‑contained unit, no alignment** — nothing to line up across the gate gap; just adhesive/screw
  it to the leaf.
- **Open‑angle, not just open/closed** — "ajar 15°" vs. "flung wide" is knowable from tilt.
- **Security bonus for free** — a **rattle/climb/shove** is a distinct vibration signature (the exact
  Fence Guard trick), so the same node can flag tampering, not just opening.

**Simplest/cheapest alternative: a reed (magnetic) switch.** A magnet on the gate + a sealed reed on the
post. Truly **~0 µA quiescent** (it draws nothing until the contact changes), unambiguous binary
open/closed, ~$1, and weatherproof reed sensors are a commodity. The trade‑offs: **two aligned parts**,
the gap must stay within the reed's range, and it can't tell open‑angle or sense a climb. A **Hall‑effect**
sensor is the solid‑state version of the same idea.

Either way this maps to the fleet's existing **`ContactStateChange`** claim — **no new vocabulary.**

---

## 2 · Power — "super low power," and the honest cell‑vs‑solar call

At a handful of events per day with the MCU asleep between them, this is one of the lowest‑duty nodes
imaginable. Two honest ways to power it:

- **A primary (non‑rechargeable) lithium cell — often the *better* choice here.** A CR2032, or an
  LiSOCl₂ cell for the cold, can run a wake‑on‑interrupt gate sensor for **years**, and it **sidesteps the
  cold‑battery problem entirely**: since it's never charged, the "no lithium charges below 0 °C" rule
  (which bites every solar/rechargeable outdoor node) simply doesn't apply. For a gate that lives in the
  cold, a multi‑year primary cell can beat solar + a rechargeable that refuses to charge on a freezing
  morning.
- **Solar + a small rechargeable — the "never touch it" option.** A scrap of PV + a small LiFePO4 cell
  makes it maintenance‑free, but then the shared outdoor rule applies: **LiFePO4 + a low‑temp‑charge‑cutoff
  BMS, sized for winter autonomy** (cited from [Fence Guard](./canary_fence_guard_research.md)/
  [Ranger](./canary_ranger_research.md), not re‑derived).

**The honest recommendation:** for most gates, a **multi‑year primary cell is simpler and dodges the cold
issue**; reach for **solar** only if you truly never want to swap a cell (a remote, hard‑to‑reach gate).
Either is "super low power" — the design choice is *maintenance model*, not feasibility.

For the full reasoning behind both calls, see the two shared references this device leans on:
the [**cold‑weather envelope**](./cold_weather_envelope.md) (how cold each part runs; why a primary cell
sidesteps the 0 °C charge wall) and the [**solar & battery sizing guide**](./solar_power_sizing.md)
(the energy‑balance method and the "right‑size, not biggest" optimization — a µA gate is its clearest
"solar is the wrong answer" example).

---

## 3 · The events it reports

- **Opened / closed** → `ContactStateChange` (the core).
- **Left open too long** → the **absence‑inference** idiom again: no "closed" within N minutes of an
  "open" → a "gate still open" nag (the same pattern as Car Mode/Guardian/Chore).
- **Rattled / climbed / shoved** (accelerometer build only) → a distinct vibration signature, a
  tamper/intrusion signal — reuse `TamperDetected` or the Fence Guard fence‑event pattern.

All processed **on‑device** to a coarse state; only the state/alert leaves.

---

## 4 · Transport & alerts

A gate is often at the **property edge, past the WiFi** — so the transport mirrors Fence Guard:

- **LoRa/Meshtastic** (the XIAO + Wio‑SX1262 kit) for a far gate, hopping to a home node.
- **WiFi or BLE** if the gate is close to the house / a Canary.
- Either way, the coarse event rides the [alert relay](../design/alert_relay.md) to your phone
  ("Back gate opened," "Gate left open 10 min").

---

## 5 · Claim mapping, privacy & BOM

- **`ContactStateChange`** (open/closed) + optional `TamperDetected` (climb/rattle) — **no new claim
  vocabulary.** Privacy: it senses a *gate's* position/motion, nothing about people; no camera, no PII.

| Part | Choice | ~$ |
|---|---|---|
| MCU | XIAO ESP32‑C3/C6 (BLE/WiFi; + Wio‑SX1262 for a far gate) | $5–14 |
| Sensor | LIS3DH accelerometer (recommended) **or** a sealed reed switch (+ magnet) | $1–2 |
| Power | a multi‑year primary cell (CR2032 / LiSOCl₂) **or** small solar + LiFePO4 + low‑temp‑cutoff BMS | $2–12 |
| Mount | adhesive/screw to the gate leaf (accelerometer) or leaf+post (reed) | $1 |
| **Total** | | **~$10–25** |

---

## 6 · Never let it rot & open items

- **Almost pure reuse** — Fence Guard's LIS3DH + Meshtastic + solar/cold research, `ContactStateChange`,
  the alert relay, and the absence‑inference idiom. Nothing bespoke in the kernel; no new vocabulary.
- **No bench unit.** The accelerometer **open‑angle threshold + debounce** (a gust vs. a real open, a
  bounce on latch) need a real gate to tune — plus a brief **auto‑learn of the closed resting angle** on
  install, since every gate hangs a little differently. A pure `classify_gate(accel_window)` with golden
  vectors, mirroring the fleet's other classifiers.
- **The cell‑vs‑solar call** (§2) is a real, honest maintenance‑model choice, not a foregone "add solar."
- **Reed vs. accelerometer** is an install‑preference fork (cheapest binary vs. single stick‑on with
  angle + climb) — the doc keeps both rather than pretending one is universally right.
- Adjacent to [Fence Guard](./canary_fence_guard_research.md) (a *fence‑line vibration* witness) — the
  gate is the *hinge point* version; noted so the two don't blur.

---

*Sources: the LIS3DH pin/interrupt/power + Meshtastic + solar/cold‑battery research in
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md); the `ContactStateChange` claim kind
(the fleet's door/gate‑contact vocabulary); the absence‑inference idiom (Car Mode/Guardian/Chore); the
remote‑alert transport in [`../design/alert_relay.md`](../design/alert_relay.md). Reed/Hall gate‑contact
and multi‑year primary‑cell (CR2032/LiSOCl₂) low‑duty sensing are long‑established commodity patterns
(Aqara/Xiaomi door sensors, Dragino/YoLink LoRaWAN door/gate sensors).*
