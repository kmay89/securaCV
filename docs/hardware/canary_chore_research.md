# Canary Chore — appliance‑cycle witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Indoor. A
convenience/utility witness, not a safety device.

**The one‑sentence version:** a coin‑sized node you **stick on the washer or dryer** (or any machine
that shakes while it works) that knows — purely from **vibration** — when the cycle is *running* and,
more usefully, when it's **done**, and pokes your phone so the laundry doesn't sit and sour.

---

## 1 · Why vibration, and why it's clever

A washer/dryer's defining tell is that it **vibrates while running and goes still when done**. That
makes the cheapest, most universal sensor an **accelerometer stuck to the machine's shell** — no
electrical work, no CT clamp, no smart plug, no per‑appliance wiring. It works on any brand, gas or
electric, and it's the exact vibration‑sensing the fleet already researched for
[Fence Guard](./canary_fence_guard_research.md).

- **Sensor:** an **LIS3DH** (or MPU‑class) accelerometer on the I²C bus — the *same part* Fence Guard
  specs for fence vibration — with a **hardware motion/vibration interrupt** so the MCU **deep‑sleeps**
  and wakes only when the machine starts shaking. Cheap (~$1–2), low‑power, adhesive‑mount.

---

## 2 · The detection logic — running, and "done" by absence

- **Running** = a sustained vibration signature above a learned floor (spin/tumble has a characteristic
  band; a passing truck or a door slam is a transient that a short debounce rejects).
- **Done** = **the vibration ceasing for N minutes** — the same **presence‑via‑active‑signal /
  absence‑via‑timeout** idiom this fleet keeps reusing (Car Mode departure, Guardian check‑in, the
  meshtastic adapter, the power‑outage witness). "Done" is *inferred from stillness*, which is robust
  and needs no coupling to the machine's electronics.
- **Optional anomaly:** an **unbalanced‑load** thud (a large, off‑rhythm vibration spike) is a distinct
  signature worth a "your washer is walking" nudge — a cheap bonus, not the core.

The whole thing is **on‑device**: the accelerometer stream is processed to a coarse state, and only the
state/alert leaves — never a raw motion trace.

---

## 3 · Why not the alternatives

| Approach | Why vibration wins |
|---|---|
| **Smart plug / CT‑clamp power sensing** | works, but needs electrical access (plug type, 240 V dryer circuits, panel clamp) — vibration is a stick‑on with none of that |
| **Door/contact sensor** | tells you the door state, not whether the *cycle* finished |
| **Listening for the end‑of‑cycle beep** | brand‑specific tones, easily missed under ambient noise; vibration is universal |
| **Current/vibration combined** | overkill for a convenience alert; vibration alone is honest and sufficient |

---

## 4 · Claim mapping, alert & BOM

- **This is a utility notification, not a witness/security event.** "Cycle done" primarily rides the
  [alert relay](../design/alert_relay.md) as a coarse poke ("Dryer finished"). If an owner wants the
  machine's on/off in the timeline, it maps cleanly to **`ContactStateChange`** (running↔idle) — **no
  new claim vocabulary.**
- **Privacy:** it senses a *machine's* vibration, nothing about people; no camera, no audio, no PII.

| Part | Choice | ~$ |
|---|---|---|
| MCU | XIAO ESP32‑C3/C6 (WiFi/BLE; low‑power) | $5–8 |
| Sensor | LIS3DH accelerometer (motion‑wake interrupt) | $1–2 |
| Power | small LiPo + USB‑C, or USB from a nearby outlet | $5–10 |
| Mount | adhesive pad / magnet to the machine shell | $1 |
| **Total** | | **~$12–20** |

Deep‑sleep‑until‑vibration means a small battery lasts a long time; if mains is handy, USB is simplest.

---

## 5 · Never let it rot & open items

- **Reuses Fence Guard's LIS3DH + the absence‑inference idiom + the alert relay** — no new sensor family,
  no new claim kind, no bespoke kernel path.
- **No bench unit:** the running/done vibration thresholds and the debounce window need real washer/dryer
  traces to tune + seed golden vectors (a pure `classify_cycle(vibration_window)` function) — the one
  real prerequisite. Different machines vibrate differently, so a brief **auto‑learn** of the idle floor
  and the running band on first use is the right call.
- **"Done" latency** = the absence timeout (a couple of minutes) — deliberate, and honest: it reports
  *shortly after* the machine stops, not the instant, because instant‑stop is exactly the unreliable
  signal we avoid elsewhere.
- **Not a safety device** — it won't catch a leak or an overheat; it's a "your laundry's done / your
  washer's walking" convenience witness.

---

*Sources: the vibration‑sensing kit + LIS3DH pin/interrupt research in
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md); the absence‑inference idiom used
across Car Mode, Guardian, and the meshtastic adapter; the shared remote‑alert transport in
[`../design/alert_relay.md`](../design/alert_relay.md). "Washer/dryer‑done via a stuck‑on accelerometer +
inactivity timeout" is a well‑trodden DIY pattern (SmartThings/ESPHome vibration‑sensor laundry
notifiers).*
