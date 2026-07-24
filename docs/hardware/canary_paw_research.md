# Canary Paw — "dog at the door" witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Outdoor,
year-round, all-weather. Marketing claims are flagged; the honest limits (especially dog-vs-person on
radar, and snow burial) are stated plainly.

**The one-sentence version:** a cheap, weatherproof sensor that knows when your dog is **at the door
waiting to come in** — not merely passing by — and tells you, without a camera, in sun, rain, cold,
and snow.

---

## 1 · Why there's no single cheap sensor — the two hard problems

The research is decisive: **no one cheap sensor solves this.** The physics splits in two, and the
sensors that win one lose the other:

1. **Detecting a dog at all, in weather.** Snow cover, ice glaze, blowing snow, wind-driven rain, and
   a sun-warmed door slab each defeat a *different* sensor. Optical/IR fails on sun and lens
   contamination; **PIR** fails when a cold-coated dog matches a snow background *and* false-fires on
   sun-warmed surfaces (and it can't see through glass at all); contact sensors (mat, capacitive) fail
   under snow/ice and water bridging. **RF (radar, BLE tag) is the only weather-immune family** — but
   it struggles to say *what* it saw.
2. **"Waiting" vs. "passing."** This is a *zone + dwell + identity* problem, not raw detection. A
   motion blip is noise; *"a dog-sized target, in the 0.5 m zone at the door, near-zero velocity, for
   ≥ N seconds"* is the useful event. Only sensors that give **position + persistence** (FMCW radar
   with zones) or **identity + proximity** (a collar tag) can express it. Doppler motion, PIR, and
   break-beams fundamentally cannot.

So Canary Paw ships as **two honest tiers**, and lets the install pick.

---

## 2 · Tier 1 (cheapest, most reliable): the trained paw-button

A **trained, IP65 waterproof paw/nose button** the dog presses — the commercial "smart dog doorbell"
pattern (Mighty Paw Smart Bell ~$30; generic IP65 units $13–18).

- **Weatherproof by design:** the outdoor *activator* is a **self-powered RF fob** — no exposed
  electronics to corrode. "Water-resistant/IP65" is real for the button.
- **Near-zero false alarms:** it only fires on a deliberate press — immune to wind, sun, passing
  people, and the wrong animal. This is its superpower.
- **It's literally a `ContactStateChange` claim** — a switch closes. The cleanest possible mapping
  onto the fleet's existing vocabulary; the door-side receiver is a XIAO that turns the RF press into
  a signed claim.

**Costs, stated honestly:** (1) **you must train the dog** — most learn in days-to-weeks, some never
generalize "in" vs. "out"; (2) the button can **ice over or get snow-buried**, and its coin/A23 cell
sags in deep cold — mitigate with a **sheltered, angled mount in the door's lee**, and (optionally) a
Tier-2 radar backstop for buried-button days.

---

## 3 · Tier 2 (no training): FMCW radar zone + dwell, behind a radome

A **24 GHz FMCW presence radar** (HLK-LD2410 ~$4–9, or **HLK-LD2450** ~$10–15 which outputs X/Y +
speed for up to 3 targets) sealed inside an **IP67 enclosure that doubles as a low-loss radome** —
RF passes through plastic and even iced-over covers, so **this is the only approach that keeps working
buried behind snow/ice.**

**It must be FMCW, not Doppler — and that's the same lesson from the radar work.** A *waiting* dog is
nearly still; cheap Doppler parts (RCWL-0516, HB100 — the [Canary Ranger](./canary_ranger_research.md)
tier) detect *motion only* and go blind exactly when the dog stops. FMCW senses a stationary target
via micro-motion and gives **distance/position** — which is what "waiting" requires.

**"Waiting, not passing" — the core logic, cheap and robust:** require
**(target in a ~0.5 m threshold zone) AND (velocity ≈ 0) AND (dwell ≥ N seconds)**. A dog trotting
past crosses in <1 s → rejected; a dog parked at the door persists → alert. On an LD2450 (X/Y + speed)
this is a few lines of ESPHome-style logic.

**The honest limit — dog vs. person:** single-radar **human-vs-dog is the one classification that
fails** (~90% only with *four* radars in the literature). The practical, imperfect fixes: **mount low
and aim the sensed slab knee-height/≤0.5 m tall** so a standing adult's torso falls outside it while a
dog's body sits inside; and/or add the **Tier-1-style collar tag** for identity. Storm clutter also
drops radar detection 10–30% and can false-trigger — suppressed (not eliminated) by CFAR + trajectory
continuity + the dwell timer (weather clutter doesn't persist in one small zone the way a body does).

Maps to **`PresenceInRestrictedZone`** at `zone:door` with a dwell attribute — existing vocabulary.

---

## 4 · The best hybrids

- **Cheapest robust:** trained paw-button (Tier 1, zero-false-alarm intent) **+** an LD2410 radar
  backstop for snow-buried-button days. ~$25–35 on a XIAO.
- **Best no-training:** LD2450 radar zone/dwell (position) **+** a **BLE collar tag** (identity) —
  radar says "dog-sized target dwelling at the threshold," the tag confirms "it's *our* dog, within
  1 m," the dwell timer confirms "waiting, not passing." Strongest false-alarm rejection: weather
  clutter has no tag, people/cats have no tag, a passing dog has no dwell. ~$30–40.

The collar tag is an **owned tag** (like the fleet's other BLE-tag ideas) — not scanning strangers.
Its exposed risk is the tag itself: IP67-potted, on a CR2032 that lasts months but sags in deep cold.

---

## 5 · What loses, and why (so we don't get talked into them)

| Approach | Why it fails outdoors year-round |
|---|---|
| **PIR** | cold-coated dog vs. snow background = no contrast; sun-warmed surfaces false-fire; **can't see through glass**; latches on motion, can't hold "still dog waiting" |
| **Pressure/weight mat** | the snow/ice killer — buried under snow the weight spreads and never trips; ice bridging destroys sealing; snow load false-triggers |
| **Capacitive touch plate** | water *is* what capacitance senses — rain/wet-snow/ice bridge the plate → constant false triggers |
| **Break-beam / cheap ToF** | a beam-break is a *crossing* ("passing"), the opposite of "waiting"; snow/ice on the window blocks it; VL53L1X is blinded by direct sun |
| **Doppler radar (RCWL/HB100)** | motion-only — blind to a *waiting* (still) dog; no range → can't express a zone |

---

## 6 · Claim mapping & privacy

- **Tier 1 (button) → `ContactStateChange`**; **Tier 2 (radar dwell) → `PresenceInRestrictedZone`**.
  Both already exist — **no new claim vocabulary.**
- **No camera** — this is deliberately a camera-free door sensor (privacy + weather both favor it).
- **The collar tag is owned** — identity by a tag you put on your own dog, never by fingerprinting
  anything. Coarse claim out ("dog at door, waiting"), no tracking of where the dog goes.

---

## 7 · Never let it rot

- **Reuses fleet radar know-how** — the FMCW-not-Doppler still-target lesson and the radome/enclosure
  thinking come straight from [Ranger](./canary_ranger_research.md) and
  [Sense](./mr60bha2_radar_notes.md); the outdoor cold-battery rule from
  [Fence Guard](./canary_fence_guard_research.md).
- **Two tiers, one contract** — button and radar both emit existing coarse claims through the existing
  path; nothing bespoke in the kernel.
- **Honest tiering** — `concept` until built and wintered; the dog-vs-person limit and snow-burial
  risk are documented, not glossed.

---

## 8 · Open items

- **No bench unit.** Radar zone/dwell thresholds (dog dwell vs. a passing dog vs. wind clutter) and
  the knee-height geometry are unmeasured — the real tuning work, on a real door with a real dog.
- **Dog-vs-person on a single radar is imperfect** (~90% needs 4 radars) — the design leans on zone
  geometry + optional collar tag rather than pretending radar classifies reliably alone.
- **Snow burial / icing** remains the residual failure for the button (and even a thick wet-snow slab
  degrades the radome) — the honest reason the two tiers back each other up.
- **Cold-killed batteries** in the button (A23) and collar tag (CR2032) — spec low-temp cells, expect
  shorter winter life.
- **"In vs. out"** (dog wants *out* vs. wants *in*) isn't sensed by either tier directly — a second
  interior trigger, or training two buttons, is a future refinement, not built.

---

*Sources: buttons — Mighty Paw Smart Bell, Chewy reviews, generic IP65 doorbells, independent
mydoglikes review. Radar — HLK-LD2410/LD2450 (DroneBotWorkshop, ESPHome ld2450), DFRobot C4001, TI
radome guide, mmWave weather-reliability (Linpowave), human/animal radar classification (IEEE, MDPI).
Losers — PIR outdoor limits (SafeHome, PIRHOME), VL53L1X-in-sun (ST community), capacitive rain
failure (TI FDC1004), pressure mat (Ideal Security). BLE/UWB tags — BlueIOT, Pozyx, Tractive
cold-battery. Full URLs in the concept card's `sources` block; shared radar/cold facts in
[`canary_ranger_research.md`](./canary_ranger_research.md) and
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md).*
