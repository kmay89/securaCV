# Canary Feeder — solar bird-feeder witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Same honesty
tier as the rest of the concept fleet. Marketing numbers are flagged as such; the honest ceiling is
stated plainly.

**The one-sentence version:** a solar-powered camera on your feeder that wakes when a bird actually
lands, decides *on-device* "a bird visited" (and a coarse kind — songbird / dove / corvid / not-a-bird
squirrel), seals that as a small signed claim, keeps a snapshot locally for your delight, and never
ships raw video anywhere.

It's a **Vision-family** device: it rides the same on-device-inference-then-coarse-claim pattern as
[Canary Vision](./canary_vision_getting_started.md), the same solar/cold-weather homework as
[Fence Guard](./canary_fence_guard_research.md), and the same "a small animal crossed" claim the
fleet already speaks.

---

## 1 · The honest problem shape

A feeder is an **easy detection geometry** — the bird lands at a fixed perch 5–30 cm from the lens —
but a **hard power geometry**: an inference-capable camera at 45°N in December. **Almost all the real
risk is in the power stack and the trigger, not the vision.** So this dossier spends its ink there.

---

## 2 · The trigger — why the obvious choices are wrong

The naïve pick is a PIR motion sensor. It's the wrong tool at a feeder, for three concrete reasons:

- **A small bird is a marginal thermal target.** A chickadee is tiny and feather-insulated; to catch
  it you must crank PIR sensitivity — which is exactly what makes it false-fire on sun-warmed
  surfaces, a wind-swung feeder, and moving foliage.
- **PIR can't look through the camera's window.** PIR senses 8–13 µm long-wave IR and needs an
  IR-transmissive **HDPE Fresnel lens**; ordinary glass/acrylic *blocks* LWIR. So a PIR needs its
  **own second window** in the housing — another sealing and fogging point.
- **Cheap Doppler radar (RCWL-0516 / HB100) is worse here**, not better: its 5–8 m range and
  "any motion" behavior wake the node for the whole yard, and metal near the module (a metal feeder)
  false-triggers it and cuts range. Radar is the *approach-over-open-ground* tool
  ([Canary Ranger](./canary_ranger_research.md)) — not a 20 cm perch.

**The right trigger exploits the fixed landing point: a sensor on the perch itself.** A
**perch break-beam or a load cell (HX711)** — the bird's weight/beam-break *is* the wake — draws
~0 mA until it fires, is utterly immune to sun/snow/wind/cold, and produces almost no false wakes.
The camera then wakes and *confirms* with a tiny FOMO model. (A load-cell perch is already a proven
DIY pattern — the ShillehTek ESP32-CAM feeder uses an HX711 both to weigh seed and to detect a
landing.)

**Recommended trigger architecture:** perch break-beam / load-cell wake → camera wakes → on-device
"bird / not-bird" confirm. If a touch-free feeder is required, a **tightly-aimed PIR with its own
HDPE window** is the fallback, still camera-confirmed so false wakes never cost a sealed claim.

**Why trigger *selectivity* beats *sensitivity*:** every non-bird wake (sun, wind, a squirrel) costs
a full capture-plus-inference cycle. At a solar node, a twitchy trigger is a *power-survival* bug, not
just an accuracy one.

---

## 3 · On-device inference — and the honest ceiling

Be clear about a marketing point: **the famous commercial feeders (Bird Buddy et al.) do species ID
in the cloud**, after uploading video. "10,000+ species" is a server capability, not an edge one —
and it's incompatible with our "coarse claims, no raw video off-device" rule.

What genuinely runs on the edge:

| Module | Silicon | Honest on-device capability | ~$ |
|---|---|---|---|
| ESP32-CAM (AI-Thinker) | ESP32 + OV2640 | frame-diff + a tiny classifier — realistically **"bird vs. not"** | ~$7 |
| **XIAO ESP32-S3 Sense** | ESP32-S3 + PSRAM + OV2640 | **Edge Impulse FOMO** — bird-vs-not **+ a few coarse buckets** | ~$14 |
| **Grove Vision AI V2** | Himax WiseEye2 + Ethos-U55 NPU | real object detection, dependable multi-class buckets, ~0.35 W | ~$49 |
| Seeed reCamera | SG2002 RISC-V | full YOLO — but Linux-class idle power, not a sleep/wake µW node | ~$35–55 |

**Honest ceiling:** on a ~$15 ESP32-S3 node, **"a bird is present" is reliable**, and **coarse
buckets** (small songbird / dove-sized / corvid / squirrel-not-bird) are achievable *if you trim the
dataset to locally-relevant species* — which real projects do deliberately. **Fine 10k-species ID is
not an honest edge claim.** So the sealed claim is *"a bird visited (coarse bucket)"* — and that's
both the achievable thing and the privacy-right thing.

**Recommended:** XIAO ESP32-S3 Sense for a self-contained node; step up to Grove Vision AI V2's NPU
when you want dependable buckets at a fixed, low energy-per-inference.

---

## 4 · Power — the part that actually decides if it lives through winter

**Demand** is modest and *sleep-dominated*: deep sleep with the camera rail truly off is ~1–5 mA;
a wake+capture+inference burst is ~160–300 mA for 1–3 s. A busy feeder lands around **~0.5–1 Wh/day**,
most of it sleep — which is why killing *false* wakes (§2) matters more than shaving inference time.

**Supply at 45°N is the real problem.** December peak-sun-hours are ~⅓ of June's, and a week of
overcast drops you far below even that. So:

- **Oversize the panel: 5–10 W (STC).** The classic DIY failure is a 1–2 W panel that's fine in July
  and starves the node in December. (Every panel wattage is an STC number, *not* December-at-45°N
  reality.)
- **Tilt ~60° (latitude + 15°)** — biases toward weak winter sun *and* sheds snow within hours of
  sunlight (a tilted panel's annual snow loss is only ~2–5%). Cold actually *helps* the panel.
- **Size the battery for 5–10 days of autonomy** (~6–10 Wh usable), because of the next point.

**The cold-battery truth, stated precisely — because it's the most misunderstood point:**
**no common lithium chemistry can safely *charge* below 0 °C** (charging plates lithium, permanent
capacity loss). This is true for Li-ion/LiPo **and** LiFePO4 — LiFePO4 is actually *stricter* about
it. So **LiFePO4 does not "solve" cold charging.** What it *does* buy: much longer cycle life, better
safety, and a wide *discharge* range (only ~10–15% capacity lost at 0 °C). The winning recipe:

- **LiFePO4 (~6–10 Wh) with a low-temp-charge-cutoff BMS** (mandatory — it refuses charge below 0 °C
  so the pack isn't damaged).
- **Autonomy sizing, not chemistry, carries you through a freeze:** during a hard cold snap the pack
  *discharges only* and lives on stored energy until the sun both returns and warms it.
- Optional **supercap front-buffer** for the 250 mA wake pulses — supercaps charge/discharge fine
  below 0 °C, so they absorb the spikes when the cold battery is current-limited.

This "no lithium charges below freezing; size for autonomy" rule is shared with every outdoor node
(Fence Guard, Ranger) — cited once, not re-derived per device.

---

## 5 · Weatherproofing — and why a *sealed* box is the wrong instinct

- **Enclosure:** IP66/IP67 baseline.
- **Condensation is the #1 optics failure, and full sealing makes it worse** — a sealed box traps the
  humid air you assembled it with and fogs it onto the coldest surface (your lens) at dawn. The fix
  is a **breathable Gore-type vent + a small rechargeable desiccant pack**, assembled dry.
- **Lens:** an integrated **hydrophobic nano-coating** sheds rain/melt and delays fog far better than
  spray; add a **short hood** to keep driven snow and low winter sun off the glass; aim the camera
  slightly **downward** at the perch so water runs off, not in.

---

## 6 · Claim mapping & privacy

- **A bird visit → `SmallObjectBoundaryCrossing`** — the exact claim the fleet already uses for a
  small animal/package (Vision, Frigate). The coarse bucket rides as a confidence-weighted
  *attribute*, like target-count on presence. **No new claim vocabulary.**
- **The snapshot is a local, opt-in delight — never sealed, never uploaded.** A bird photo is the joy
  of a feeder cam, so we allow the owner to view a local snapshot (same posture as Vision's local
  preview), but the *sealed log* only ever carries the coarse claim. Raw video never crosses the
  boundary — the webhook payload grammar has no image field by construction.
- **This is a camera pointed at wildlife, not people** — but the rule is the same as every Canary
  camera: on-device inference in, coarse claim out, no raw frames in the log.

---

## 7 · Cheapest reliable BOM

| Part | Choice | ~$ |
|---|---|---|
| Compute + camera | XIAO ESP32-S3 Sense | $14 |
| Inference step-up (optional) | Grove Vision AI V2 (NPU) | $49 |
| Trigger | perch break-beam or load cell + HX711 | $2–6 |
| Battery | LiFePO4 ~6–10 Wh + low-temp-cutoff BMS | $10–18 |
| Pulse buffer (optional) | supercap | $3 |
| Solar | 5–10 W panel + MPPT/solar-charge IC | $12–20 |
| Enclosure | IP66 box + Gore vent + desiccant | $8–12 |
| Optics | hydrophobic cover glass + printed hood | $3–5 |
| **Self-contained total** | | **~$50–70** |
| **With Grove NPU** | | **~$100–120** |

---

## 8 · Never let it rot

- **Vision pattern, not new firmware philosophy** — on-device inference → coarse claim → existing
  webhook/claim path. No bespoke ingestion.
- **Shared winter-power + solar rule** cited from Fence Guard/Ranger, not re-derived.
- **Honest tiering** — `concept` until a node is built and lives outdoors through a winter; species
  ID stays "coarse bucket," never "10k species."

---

## 9 · Open items

- **No bench unit, no winter deployment.** Every power/solar number is an STC or third-party figure
  (cited), not ours through a real December.
- **The three honest reliability risks that remain:** (1) **winter energy starvation** — short,
  snow-interrupted days plus no-charge cold spells; the design lives or dies on panel oversizing +
  autonomy; (2) **lens fogging** at dawn until the vent/desiccant equilibrates; (3) **false-wake power
  drain** — the reason the perch trigger (selectivity) beats PIR (sensitivity).
- **Coarse species buckets need a locally-trimmed dataset** — a small labeled capture set per region,
  not a universal model.
- Enclosure/perch-trigger mechanical integration is a real design task (a printed feeder body with an
  integrated break-beam/load-cell perch), not yet drawn.
- Distinct from [`litterbox_witness_demo.md`](../litterbox_witness_demo.md) and the real
  [Canary Litter](./canary_litter_research.md) health monitor — different animal, different sensing;
  noted so the two don't blur.

---

*Sources: perch/trigger & PIR limits (ShillehTek ESP32-CAM+HX711 feeder; SafeHome/shallowsky PIR
wildlife notes; Apollo/fresnelfactory on HDPE-vs-glass LWIR); RCWL/HB100 range & metal false-trigger
notes; on-device inference (Seeed XIAO TinyML wildlife, Edge Impulse Grove Vision AI V2, reCamera
specs; Bird Buddy cloud-ID as the marketing contrast); winter solar & LiFePO4 cold-charge physics
(RELiON, greenenergycalc, NREL snow-loss, diysolarforum); condensation/vent (securitytoday, ipvm,
reolink). Full URLs are in the concept card's `sources` block; shared kit/power facts in
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md).*
