# Canary Hearth — stove/cooking‑awareness witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. **Alert‑only,
camera‑free.** Read §5 (positioning) first: this is an *awareness aid*, **not** a fire detector, not a
substitute for UL‑listed smoke/CO alarms, and not an auto‑shutoff device. Overselling a fire sensor is
how someone gets hurt, so the honesty here is load‑bearing.

**The one‑sentence version:** a low‑resolution thermal sensor under the range hood that notices when a
**burner's been left hot and unattended** — the leading cause of home fires — and a microphone that
**relays your existing smoke/CO alarm** to your phone while you're out, with no camera and no video.

---

## 1 · Why this is worth doing (and what it actually is)

Cooking is the **#1 cause of home structure fires**, and **unattended cooking is by far the leading
ignition factor** — roughly a third of cooking fires, ~3× the next factor (NFPA: ~172,900 home cooking
fires/yr, ~550 deaths, ~4,820 injuries). So the value is **not** "detect flames" — it's *"notice a hot
cooktop left unattended,"* which is a **thermal + presence** problem, not flame spectroscopy.

Your instinct ("a flame sensor for flames at least") is reasonable but the honest answer is richer — a
flame sensor is a weak *primary* (see §3).

---

## 2 · The hero sensor: a low‑res thermal IR array — MLX90640, not AMG8833

A thermal array under the hood looking **down** at the cooktop measures the quantity that *is* the
hazard — surface/pan temperature and how it evolves. The part choice is decisive:

- **MLX90640 (32×24, −40 to +300 °C, ~$40–60) is the correct hero.** A pan runs 150–250 °C, a dry/
  scorching pan 300 °C+ — well within range. It maps 4 burners to distinct pixel clusters at hood
  height (~55–75 cm), and detects: **per‑burner hot/on**, **pot boiled dry / scorching** (a hot region
  with low thermal mass runs away past 200–300 °C while a full pot plateaus near 100 °C), **sudden
  flare** (rapid hot‑area growth), and **boil‑over** (a hot region spreading *beyond* the pot footprint
  + a burner temperature drop as liquid quenches it). Run it at ~2–4 Hz.
- **Do NOT use the AMG8833 (8×8) for the cooktop — its 0–80 °C ceiling saturates on any real cooking**
  and can't tell a simmer from a scorching dry pan, which is the whole game. (It's fine only as a coarse
  room‑warmth hint.)

**Privacy — confirmed benign.** 32×24 = 768 pixels; you cannot resolve a face, read text, or identify a
person from a thermal frame that coarse. It sees warm blobs. That makes it a **witness‑grade sensor, not
a camera** — it fits our "coarse claim, no raw video" model cleanly (process on‑device, emit the derived
claim, never the frame). This is a genuine differentiator vs. camera‑based "smart stove" systems.

**The prior art validates the approach:** Innohome Stove Guard keys on **max temperature + rate‑of‑
change**; Inirv React explicitly implements **unattended = hazard + no‑motion‑for‑15‑min**. We copy the
*sensing logic*, not their auto‑shutoff (we have no actuator — §5).

---

## 3 · The highest‑value detection: unattended cooking (thermal + presence)

The differentiated, defensible core feature, straight off the NFPA #1 statistic:

> **hot cooktop region (MLX90640) persisting past a threshold, with no human motion in the room
> (presence sensor) for N minutes → an escalating alert.**

This reuses the fleet's existing **presence sensing** (a PIR, or a [Canary Sense](../canary_sense_mr60bha2_design.md)
radar in the kitchen). Absolute‑temp + rate‑of‑rise + hot‑area growth handle the acute events (dry pan,
flare, boil‑over); presence fusion handles the *leading* cause.

---

## 4 · The clever cheap add: relay your existing smoke/CO alarm

A microphone that recognizes the **standardized alarm cadence** turns any UL alarm you already own into
a remote notification — without us pretending to be a certified alarm:

- The patterns are machine‑detectable standards: **T3 (fire) = three ~0.5 s pulses + pause**, **T4 (CO)
  = four pulses + pause** (ISO 8201 / ANSI‑ASA S3.41 / NFPA 72), ~3–4 kHz. **Match the temporal cadence,
  not loudness**, to reject kitchen clatter — the approach real projects (Hackaday, ESPHome listeners)
  and the shipping FireAvert product use.
- Maps to the existing **`AcousticImpulseInZone`** claim — **no new vocabulary** — and the honest
  framing is *"we heard your smoke/CO alarm and notified you."* Caveats stated: fan/music can mask it,
  distance/walls attenuate, it detects the *alarm sounding*, not the hazard.

**What we deliberately skip:** the **NIR flame module** (sun/incandescent false‑fire; a gas flame is
present the *whole* time the stove is on, so "flame" ≠ hazard; blind on electric/induction) and the
**UVtron** (same flame≠hazard mismatch, blind on electric/induction). A flame sensor detects *fire*, not
*cooking risk* — the thermal array already sees a flare better. **Gas leak (unlit gas)** is a *separate*
hazard needing a proper catalytic/NDIR methane sensor (MQ‑series are too unreliable for a safety claim) —
explicitly out of scope; Hearth does not replace a gas detector.

---

## 5 · The honest safety‑positioning line (non‑negotiable)

We have **no actuator** — we're alert‑only — so we're in the "awareness/notification" lane (like
Wallflower), **not** the EN 50615 stove‑guard *prevention* lane (those all cut power). The positioning
must reflect that, verbatim‑ish:

> *Canary Hearth is a privacy‑first cooking‑**awareness** aid. Using a low‑resolution thermal sensor
> (no camera, no video), it lets you know when a burner has been left hot and unattended, and it relays
> your existing smoke or CO alarm to your phone. It is **not** a fire detector or a substitute for
> UL‑listed smoke and carbon‑monoxide alarms or an automatic stove shut‑off — please keep those in place.
> It gives you awareness; it cannot prevent, suppress, or shut off a fire.*

Rules: never use "detect/prevent fire," "fire alarm/detector," or "safety device" in the primary claim;
always state it does not replace certified alarms; state the failure modes (may miss, may false‑alarm,
depends on power/WiFi/mounting).

---

## 6 · Claim mapping, alerts & BOM

- **Smoke/CO alarm relay → `AcousticImpulseInZone`** (existing). **Unattended‑hot‑cooktop** is primarily
  a **relay alert** ([alert relay](../design/alert_relay.md)) + optionally a coarse sealed event; whether
  the thermal hazard earns its own claim kind is the one open dictionary question (flagged, not moved).
- Thermal frames are processed **on‑device**; only the derived coarse claim/alert leaves — never the
  768‑pixel frame.

| Part | Choice | ~$ |
|---|---|---|
| MCU | XIAO ESP32‑S3 | $14 |
| Thermal hero | **MLX90640** 32×24 (−40..+300 °C) | $40–60 |
| Presence (fusion) | a PIR, or reuse a kitchen Canary Sense radar | $1–20 |
| Mic (alarm relay) | any I²S/analog mic | $2–5 |
| (optional) hood RH | SHT4x — *weak* boil‑over corroborator only | $3 |

---

## 7 · Never let it rot & open items

- **Reuses presence + `AcousticImpulseInZone` + the alert relay** — nothing bespoke in the kernel.
- **No bench unit:** the unattended thresholds, per‑burner clustering at real hood heights, dry‑pan
  rate‑of‑rise, and the T3/T4 cadence matcher all need real‑kitchen tuning + golden vectors (a pure
  `classify_thermal_window()` and a `match_alarm_cadence()` with labeled captures).
- **The positioning line is part of the product**, enforced in copy, not an afterthought.
- **AMG8833 is explicitly the wrong part** (§2) — documented so no one "value‑engineers" it back in.
- **Gas leak and CO are out of scope** as certified functions — Hearth relays a CO alarm's *sound*, it
  is not a CO detector.

---

*Sources: NFPA *Home Cooking Fires* + US Fire Administration (unattended = leading ignition factor);
Melexis MLX90640 and Panasonic AMG8833 Grid‑EYE datasheets (the 80 °C ceiling); ISO 8201 / ANSI‑ASA
S3.41 / NFPA 72 T3/T4 patterns + ESPHome/Hackaday/FireAvert alarm‑listener prior art; Innohome Stove
Guard, Inirv React, CookStop, FireAvert, Wallflower (sensing + liability posture); EN 50615 (the
prevention standard we deliberately don't claim). Full URLs live with the research that produced this
doc; the alert transport is [`../design/alert_relay.md`](../design/alert_relay.md).*
