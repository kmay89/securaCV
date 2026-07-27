# Canary Ranger — low-power Doppler approach witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Same honesty
tier as Fence Guard, Guardian, and Vehicle Guard: grounded in real parts and real published radar
work, nothing built or measured by us yet. Marketing claims are flagged as such; peer-reviewed and
datasheet facts are cited.

**The one-sentence version:** a cheap, camera-free radar node that watches an open approach — a
driveway, a gate, a yard, a trail — sips microwatts so it runs for months on a battery or forever on
a little solar, and reports coarse **"something's moving, this way, roughly this big"** claims to your
mesh. It's the motion counterpart to [Fence Guard](./canary_fence_guard_research.md)'s vibration on
the fence line, and the low-power sibling of [Canary Sense](./mr60bha2_radar_notes.md)'s rich indoor
radar.

---

## 1 · What we learned, and the line we're drawing

This concept comes directly out of studying **Samraksh's "BumbleBee" radar** — a 5.8 GHz pulsed-Doppler
"mote" (~40 mW, ~$50, 1–10 m, 60° cone) that reads motion, radial velocity, **approaching vs. receding**
direction, and — with a little on-mote micro-Doppler ML — coarse activity classes (walk / run / crawl)
and human-vs-vehicle separation ([MDPI *Sensors* 2012, 12(2):1336](https://www.mdpi.com/1424-8220/12/2/1336),
peer-reviewed; [IEEE GRSL 2015 micro-Doppler activity paper](https://ieeexplore.ieee.org/document/7172472/)).

**Their grants split exactly along the line we care about.** The military money funded *tracking and
identifying* — unattended-ground-sensor "surveillance and tracking of vehicles and dismounts," a DARPA
radar SoC, target classification. The **civilian** money is the interesting part for us: the NSF
**"Virtual Fences for Sustainable Protection"** award — a battery/solar radar mesh for anti-poaching and
anti-deforestation, with on-node discrimination and multi-hop relay to a ranger base station
([NSF award 1648337](https://www.nsf.gov/awardsearch/showAward?AWD_ID=1648337)) — plus NSF work on
building **occupancy + energy/HVAC** with eldercare health monitoring.

That civilian "virtual fence" is almost a description of our own roadmap — except we build it for a
homeowner's driveway, not a border, and we refuse the part that made the defense version a surveillance
tool. **Ranger replicates the civilian capability and hard-stops the military one:** it will say *"a
large mover is approaching zone X"* (a physical predicate, camera-free, coarse) and will never say
*who*, never reconstruct a path, never keep a per-target ID. See §4.

---

## 2 · Where it fits: the fleet's radar becomes a spectrum

We already ship the *rich* end (60 GHz FMCW). Ranger adds the *cheap, always-on, outdoor* end. Between
Ranger, Sense, and Fence Guard, the fleet covers the approach, the room, and the fence line without any
of them pretending to be the others:

| | **Canary Ranger** (this concept) | **Canary Sense** (shipped) | **Fence Guard** (concept) |
|---|---|---|---|
| Sensor | low-power Doppler radar (10–24 GHz) | 60 GHz FMCW (ADT6101P) | fence vibration + mesh |
| Best at | an **open approach**, outdoors, on battery/solar | a **room**, indoors, on power | a **fence line**, past power |
| Reads | motion, speed, **direction**, coarse size | presence (even still), occupancy 0/1/2+, **breathing/heart**, direction | climb / cut / rattle |
| Power | **µW–mW** (pulse-duty) — months on a battery | ~1–2 W-class — mains/large battery | µW sleep, solar |
| Range | 2–16 m (part-dependent) | ~4–6 m, 80° sector | contact |
| Transport | Meshtastic LoRa (same kit as Fence Guard) | WiFi/MQTT indoors | Meshtastic LoRa |

**The insight from the research:** a 60 GHz FMCW radar is a *better* sensor than a 5.8 GHz Doppler mote
in almost every way — except **power**, and that one exception is decisive for a node that must live on a
fencepost for a season. BumbleBee wins nothing indoors; it wins the driveway. So Ranger isn't a downgrade
of Sense — it's the tier Sense's power budget can't reach.

---

## 3 · What Ranger does

- **Motion + radial speed.** The core Doppler signal: something is moving, and how fast (radially).
- **Direction — approaching vs. receding.** The capability that makes it more than a PIR: it can tell a
  car *arriving* from one *leaving*, a person walking *up* the path from one walking *away*. (This needs
  an **I/Q / quadrature** Doppler part — see §5; a single-channel module gives speed but not sign.)
- **Coarse size/type class.** From the Doppler signature — torso return, micro-Doppler bandwidth of limbs,
  cadence — a *coarse* bucket: **large mover** (person/vehicle) vs **small mover** (animal), and with more
  processing, human-vs-vehicle. This maps onto claim kinds the fleet already has
  (`boundary_crossing_object_large` / `_small`, the vehicle kinds). It is a **class, never an identity**
  (§4).
- **Low false-alarm vs. PIR.** Doppler ignores the thermal ghosts (sun on pavement, an HVAC plume) that
  plague PIR, and it works through light foliage, rain, and darkness — the reason BumbleBee was pitched as
  a PIR replacement, and the reason it survives outdoors where a camera would be blinded or creepy.
- **Coarse activity hint (dashboard-only, not sealed).** Walk/run/crawl-style micro-Doppler classification
  is real and published — but we keep it as a *local, unsealed hint* (like Sense's existing "Active" state),
  not a signed claim, because activity inference sits close to the behavioral line we don't cross (§4).

What it deliberately **can't** do — and shouldn't pretend to: still-person presence (Doppler is blind when
the target stops — that's Sense's job), vitals, people-counting, or range profiles. It's a motion witness,
not an occupancy sensor.

---

## 4 · The privacy line — the whole point of doing this "not for military"

BumbleBee's defense grants funded exactly the capabilities our invariants forbid. Ranger takes the sensing
and refuses the surveillance:

**Does (all coarse, physical predicates):**
- "A **mover** is present / approaching / receding in zone X."
- "The mover is **large** vs **small**" (person/vehicle vs animal) — a size/type class.

**Refuses (the military/surveillance half):**
- **No tracking / trajectory reconstruction** — no "it went from the gate to the door to the window."
  (`canary_sense_mr60bha2_design.md` §2.2 already forbids this for the 60 GHz radar; Ranger inherits it.)
- **No per-target IDs**, ever — two movers are "two movers," not "target A again."
- **No gait or identity inference.** Micro-Doppler *can* fingerprint a person's walk — that's a biometric,
  and it's [absent from the codebase by design](../strategy/14-pose-estimation-v2-ai.md) ("not disabled —
  missing"). Ranger classifies *what kind of thing is moving*, never *which individual*.

**The one-sentence line:** *a coarse class ("a large mover, approaching") is a physical fact about your
property; a track, an ID, or a gait signature is surveillance of a person.* Ranger stays entirely on the
first side — which is precisely what turns a defense sensor into a homeowner's witness.

---

## 5 · Hardware — the Doppler part, honestly

The radar front-end is a cheap, well-trodden commodity. The honest catch is **direction needs I/Q**:

| Part | Freq | Direction? | Notes | Rough $ |
|---|---|---|---|---|
| **RCWL-0516** | ~3.2 GHz | ❌ | ultra-cheap microwave "switch," crude, no speed — a bare motion trip only | ~$1 |
| **HB100** | 10.525 GHz | ❌ (single-ch) | classic PIR-replacement CW Doppler; **~2 mA at 5% pulse duty**, 30–40 mA CW; 2–16 m; analog IF → amp → MCU ADC/FFT ([datasheet/pattern lab](https://antennatestlab.com/antenna-examples/radar-antenna-pattern-rcwl-0516-hb100-cdm324)) | ~$5 |
| **CDM324** | 24.125 GHz | ❌ (single-ch) | smaller than HB100; speed-proportional IF; people ~25 ft, cars ~100 ft; clone of InnoSenT IPM-165 | ~$6 |
| **InnoSenT IPM-165 / HB100-IQ** | 24 / 10.5 GHz | ✅ (two IF outputs) | the **quadrature** variant — I and Q channels give the sign of the Doppler shift, i.e. **approaching vs. receding**. This is the real BumbleBee analog. | ~$15–30 |
| **Seeed MR24HPC1** | 24 GHz **FMCW** | ✅ | not Doppler — a "human static presence Lite" module that reports motion, direction, distance *and* still-presence ([Seeed wiki](https://wiki.seeedstudio.com/Radar_MR24HPC1/)). A cheaper, lower-res cousin of Sense — an alternative "richer tier" if still-presence matters at the perimeter. | ~$10–20 |

**Design lead:** an **I/Q Doppler part** (IPM-165-class) for the true low-power approach witness with
direction, with **MR24HPC1 (24 GHz FMCW)** offered as the "I also want still-presence" upgrade path. The
cheap single-channel parts (HB100/CDM324) are the fallback for a *motion-only* build where direction isn't
needed.

- **Host + radio:** the same **XIAO ESP32-S3 + Wio-SX1262 Meshtastic kit** Fence Guard specs — the raw IF
  gets amplified into the MCU's ADC, a small on-device FFT extracts speed/direction/coarse-class, and the
  result leaves as a signed claim over LoRa. (BumbleBee did its displacement detection on a tiny on-board
  FPGA; an ESP32-S3 with the ADC + a windowed FFT is comfortably enough for coarse Doppler features.)
- **Power:** the whole reason for this tier. A pulsed/duty-cycled Doppler front-end (HB100 at ~2 mA/5%
  duty; IPM-class similar) plus the ESP32 asleep between looks lands in the **µW–mW average** band —
  months on a LiPo, indefinite on a small solar panel with the same charge-controller guidance Fence Guard
  already worked out. Continuous-wave operation (30–40 mA) is the thing to *avoid* on battery.
- **Enclosure & siting:** a weatherproof radome (radar sees through plastic, not metal), aimed *along* the
  approach so approach/recede maps to the path; shares Fence Guard's solar/antenna-standoff guidance rather
  than a second dossier.

---

## 6 · Applying it to what we already have (Canary Sense)

The micro-Doppler learnings aren't only a new device — they're a **software upgrade the 60 GHz radar we
already ship can grow into**, with no new hardware:

- Sense already computes **approaching/receding** (the four direction arrows) and a Doppler motion
  spectrum. The BumbleBee papers' recipe — spectrogram features → a light SVM/decision-tree — is exactly
  what would turn that into a **coarse size/type claim** (large vs small mover) on the indoor radar.
- That would ride entirely on existing claim kinds (`boundary_crossing_object_large/_small`) and the
  existing privacy contract (§2.2) — a classifier that outputs a *coarse bucket*, never a track or an ID.
- **Caveat that keeps us honest:** this stays behind the same line — *class, not identity; hint, not
  gait.* The moment a "classifier" starts telling people apart, it's the thing we said we don't build.

So the answer to "can we do what they do and more, with what we have?" is: **on the indoor radar, largely
yes, in software**; the genuinely *new* build is the **low-power outdoor tier** (Ranger) that our FMCW
part's power budget can't reach.

This "with what we have" upgrade is now scoped in its own design doc:
[**Coarse mover-class from the Sense radar**](../canary_sense_coarse_class_design.md) — features, the
tiny classifier, the contract-respecting claim mapping (an attribute on the existing presence claim, no
dictionary drift), the honest accuracy limits, and a phased plan.

---

## 7 · Claim vocabulary

Ranger reuses existing kinds and needs at most one small addition:

| Signal | Proposed claim kind | New? | Notes |
|---|---|---|---|
| Large mover in a zone | reuse **`boundary_crossing_object_large`** | no | person/vehicle-scale return |
| Small mover in a zone | reuse **`boundary_crossing_object_small`** | no | animal-scale return |
| Vehicle approaching | reuse the vehicle kinds / `vehicle_arrival_departure` | no | when the class is vehicle-shaped |
| **Direction** (approaching vs receding) | a claim **attribute**, or a proposed **`approach_detected`** kind | tbd | the one open dictionary call — whether direction rides as metadata on the above, or earns its own kind so "someone's coming up the drive" reads as itself |
| Walk/run/crawl activity | *(local dashboard hint, not sealed)* | no | kept unsealed on purpose (§4) |

**Decision to lock at firmware time:** how direction is represented. No new kind is strictly required to
prototype the coarse large/small-mover claims.

---

## 8 · Never let it rot

- **Commodity front-end, standard math.** Doppler FFT on an ADC stream is decades-stable; no proprietary,
  per-vehicle, or vendor-locked anything.
- **Shared kit & mesh with Fence Guard** — same XIAO + Wio-SX1262, same solar/antenna guidance, same
  [`meshtastic_integration.md`](../meshtastic_integration.md) transport. One radio doc, not three.
- **One privacy contract.** Ranger inherits `canary_sense_mr60bha2_design.md` §2.2 wholesale — the line is
  defined once and applies to every radar in the fleet.
- **Honest tiering.** `concept` until a node is built and lived-with outdoors through weather; the activity
  hint never graduates to a sealed claim; classification never graduates to identity.

---

## 9 · Open items

- **No bench unit.** Every power/range figure is a datasheet or someone else's measurement (cited), not
  ours on this build. The real work is the **coarse-class false-alarm floor** outdoors (wind-blown branch
  vs. deer vs. person vs. car) — the classic Doppler tuning problem, on a real approach through real
  weather.
- **Direction needs an I/Q part** (§5) — a single-channel HB100/CDM324 build is motion-only; budget the
  IPM-165-class part if approach/recede matters.
- **`approach_detected` vs. direction-as-attribute** is an open dictionary decision (§7).
- **The Sense classification upgrade** (§6) is a separate, hardware-free workstream — worth its own scoping
  if we want the indoor win first.
- Micro-Doppler ML on the ESP32-S3 (features + a small SVM/tree) is a real firmware effort, not a config
  change; the coarse large/small-mover split is the tractable Phase 0.

---

*Sources: Samraksh BumbleBee — [MDPI *Sensors* 2012](https://www.mdpi.com/1424-8220/12/2/1336) (peer-reviewed
specs), [IEEE GRSL 2015 micro-Doppler activity](https://ieeexplore.ieee.org/document/7172472/), and the
civilian [NSF "Virtual Fences" award 1648337](https://www.nsf.gov/awardsearch/showAward?AWD_ID=1648337).
Doppler parts: [HB100/CDM324/RCWL-0516 pattern comparison](https://antennatestlab.com/antenna-examples/radar-antenna-pattern-rcwl-0516-hb100-cdm324),
[Seeed MR24HPC1 24 GHz FMCW](https://wiki.seeedstudio.com/Radar_MR24HPC1/). Shared kit + privacy contract:
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md),
[`mr60bha2_radar_notes.md`](./mr60bha2_radar_notes.md),
[`canary_sense_mr60bha2_design.md`](../canary_sense_mr60bha2_design.md) §2.2.*
