# Canary Curbwatch — "child near the street" awareness aid (research & design dossier)

> **⚠️ Read this first. This is an *awareness aid*, not a barrier.**
> **"This is an awareness aid, not a barrier: it can alert a nearby adult when it *thinks* a child may
> be crossing toward the street, but it will sometimes miss and sometimes false‑alarm, it cannot
> physically stop anyone, and it must never replace fences, gates, and active supervision — it only
> adds a chance of earlier warning."** Verbatim, everywhere. Never "keeps your child safe,"
> "child‑proof," or "prevents your child from reaching the street." A false sense of security is
> itself a hazard.

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Solar, outdoor,
year‑round.

**The one‑sentence version:** a solar sensor at the yard/street boundary that watches a *virtual plane*
at the curb and warns a nearby adult when a small child appears to be **crossing toward the road** —
one extra set of eyes, never a fence.

---

## 1 · The highway‑wildlife analog (the right model — and its hard lesson)

Your instinct is exact: this is the consumer version of **roadside Animal Detection Systems (ADS/
RADS)** — a sensor watching a linear boundary zone that triggers a warning. FHWA/WTI evaluated nine.
They split into **break‑the‑beam** (IR/laser/microwave line — fires on *anything* crossing) and
**area‑cover** (radar / passive‑IR watching a volume, can filter by motion). When an animal is
detected they light a flashing warning sign upstream; ~80% of drivers slowed, with real crash
reductions **when detection was reliable**.

**The uncomfortable lesson that must set our honesty bar:** even mature, DOT‑funded systems live or die
on false alarms —
- Field deployments ran **>90% false positives**, which **desensitizes** the very people meant to
  react (the "cry wolf" effect that erodes the whole system).
- Operators found it **"impossible to eliminate false triggers while still guaranteeing large animals
  are detected"** — the sensitivity trade‑off is *fundamental*, not an engineering gap.
- Weather is the recurring failure: one microwave system hit 97% detection in good conditions but
  **~40% downtime in a snowstorm**.

The hard part was never "sense motion at a boundary." It's **classifying the right target and rejecting
everything else, in weather, cheaply, for years** — and the professionals only partly solved it.

---

## 2 · The core primitive: a directional boundary‑plane crossing

Not "motion" — a **virtual plane at the curb/inner property edge, with detection gated on crossing
*toward the road*** (the security‑industry line‑crossing/tripwire analytic, with direction). Sensor
options, ranked:

- **Camera + line‑crossing AI — best (and only) real classifier.** Draw the plane in the image, detect
  a person, bias to child‑size, fire only on the toward‑street direction. Line‑crossing is far less
  false‑alarm‑prone than raw motion (needs a *tracked, classified* object crossing a *specific line* in
  a *specific direction*). **Tier: a starlight/low‑lux camera ([Vision Pro](./canary_vision_pro_recamera.md)
  class), not Grove Vision AI V2** — Grove's tiny sensor/short range/weak low light suit a narrow gate,
  not a wide yard at dusk.
  - **The catch that dominates everything:** **children are the pedestrian class detectors miss most**
    — ~20%+ higher miss rate than adults across datasets/detectors, because they're small (tiny
    bounding boxes) and move erratically, and training data is adult‑biased. The target we care most
    about is the one vision is *worst* at.
- **FMCW radar — best weather immunity + direction/dwell; weak classifier.** 24/60 GHz gives range,
  radial velocity (toward/away), and dwell, immune to fog/rain/snow/dark/blowing‑leaves. But **single‑
  radar child‑vs‑adult‑vs‑pet‑vs‑car classification is a research problem, not a guarantee** — radar
  knows *something crossed toward the street*, not that it was a toddler. (Same still‑target/ classifier
  limits as [Ranger](./canary_ranger_research.md).)
- **Thermal — night adjunct only** (a warm body crossing a line in the dark; poor class/range, fails on
  hot days when ground ≈ body temp).
- **Break‑beam / lidar tripwire — precise line, zero discrimination** (fires on the dog, the ball, the
  leaf); a geometry‑precise *wake* trigger at best.

**The honest technical answer is fusion (what the highway systems trend toward):** a low‑power **radar
runs always‑on** as the weather‑immune *directional‑crossing + dwell* trigger; a toward‑street event
**wakes the starlight camera** to *confirm child‑class* and reject cars/adults/(mostly) pets. Radar
can't classify a child; vision can't survive the weather or the solar power budget alone — and vision
is *worst* at the child class, so it must be the confirmation layer, not the trigger.

---

## 3 · Meaning "child heading toward the street," not "someone passing by"

No single cheap feature captures intent — **AND four weak signals into one strong meaning**:

1. **Directional crossing of the plane, toward the road** (radar radial velocity / camera track
   direction) — the single most valuable filter; removes the biggest confound (walking *along* the
   sidewalk, playing safely *in* the yard).
2. **Zone geometry** — place the plane at the inner boundary and shape the zone to geometrically
   exclude sidewalk‑parallel and in‑yard motion.
3. **Small‑size / low‑height bias** — bias toward small targets, reject adult‑height and vehicle‑size.
   **A bias, never a suppression gate** — because failing to alert on a real child is catastrophic, and
   a crouching toddler and a low dog look alike to both radar and a small camera (you may deliberately
   *keep* pet triggers rather than risk suppressing a child).
4. **Dwell / trajectory persistence** — a coherent approaching track, not a single blip; kills leaves,
   rain streaks, insects, flag/shadow motion.

**The unavoidable tension** (the DOT operators' exact wall): **you cannot simultaneously maximize child
sensitivity and minimize false alarms.** A life‑safety alert must bias to sensitivity, which *guarantees*
nuisance alerts (the dog, the neighbor's kid, the delivery driver, a gust of leaves). A tuning that
produces "zero false alarms" is one that will one day stay silent for a real child. We say that plainly.

---

## 4 · Solar / outdoor power (year‑round) — the shared rule

Same cold‑battery homework as the outdoor fleet: **no lithium chemistry charges below 0 °C** (plating =
permanent loss + hazard), so **LiFePO4 + a low‑temp‑charge‑cutoff BMS**, sized for **winter worst‑case**
(short days, low sun, snow on panel) with multi‑day autonomy; the charge controller's PV input must
cover cold‑raised Voc. **Duty‑cycle is another argument for the radar‑wakes‑camera fusion:** keep the
low‑power radar awake continuously, power the hungry starlight camera + inference only on a boundary
event, so the winter budget stays feasible. Cited from
[Fence Guard](./canary_fence_guard_research.md)/[Ranger](./canary_ranger_research.md), not re‑derived.

---

## 5 · What exists (and the gap)

Driveway/perimeter alarms (**Dakota Alert**, **Guardline**) reliably tell you *something* entered a zone
— but **don't classify** child vs. adult vs. dog vs. car (PIR fires on any warm mass; notorious for
animal/weather nuisance). Child geofence trackers (**AngelSense**, **Jiobit**) warn before a child
reaches danger but are **worn on the child**, not on the boundary (GPS drift, must‑be‑worn, cellular
latency). School‑zone/pedestrian and work‑zone intrusion systems are the professional pedestrian
analog. **Reality check: nothing on the market reliably does "solar, outdoor, classifies a small child
crossing a boundary toward a street, year‑round, low false alarm."** The pieces exist; the guarantee
does not — so we ship a bias‑to‑sensitivity *awareness aid*, honestly labeled, not a guarantee.

---

## 6 · Claim mapping, privacy & alerts

- **A toward‑street boundary crossing → `SmallObjectBoundaryCrossing`** (direction + child‑size as
  attributes), or `PresenceInRestrictedZone` at `zone:curb`. **No new claim vocabulary.**
- **On‑device inference, coarse claim out — never raw video**; the time‑critical warning rides the
  [alert relay](../design/alert_relay.md) (a nearby‑adult push; latency stated honestly).
- Privacy: pointed at your own boundary; inference in, coarse claim out, no frames in the log; radar
  adds no imagery.

---

## 7 · Never let it rot & open items

- **Reuses Vision Pro (starlight) + Ranger radar (fusion trigger) + the alert relay + the solar/cold
  rule + existing boundary‑crossing vocabulary** — nothing bespoke.
- **The three honest risks, documented, not hidden:** (1) **child under‑detection** — the target we
  care most about is the one detectors miss most; a silent miss is the catastrophic failure; (2) the
  **>90% false‑positive desensitization trap** — bias for sensitivity → frequent nuisance alerts →
  users disable it → silent for the real event; (3) **weather/power downtime** (snow/ice/fog + winter
  solar starvation) can blank it exactly in bad conditions; plus (4) the **latency chain** — the device
  never intervenes, it only warns.
- **No bench unit** — the plane geometry, directional/dwell thresholds, and the child‑size bias need
  real‑yard tuning + golden vectors, with sensitivity deliberately favored over quiet.
- **The positioning line is part of the product** — enforced in copy; never a "keeps your child safe"
  claim.

---

*Sources: FHWA/WTI (Huijser et al.) roadside animal‑detection test‑bed comparison; FL panther US‑41
RADS assessment (>90% false positives, desensitization); VDOT/VTTI buried‑cable warning system; child
pedestrian‑detection miss‑rate studies (arXiv 1906.10490; MDPI "Age Should Not Matter"); 60 GHz FMCW
presence/child‑in‑cabin + micro‑Doppler human/vehicle classification; Battery University / RELiON cold
lithium‑charging limits; Dakota Alert / Guardline / AngelSense / Jiobit as adjacent products. Full URLs
in the concept card's `sources` block; shared parts/rules from
[`canary_vision_pro_recamera.md`](./canary_vision_pro_recamera.md),
[`canary_ranger_research.md`](./canary_ranger_research.md),
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md), and
[`../design/alert_relay.md`](../design/alert_relay.md).*
