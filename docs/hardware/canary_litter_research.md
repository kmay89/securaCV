# Canary Litter — cat litter-box health witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Indoor (no
weatherproofing). Everything below is framed as **non-diagnostic wellbeing signals** — the honest
product claim is *"we surface changes worth asking your vet about,"* never *"we detect kidney
disease."* That's also how the one peer-reviewed player in this space frames it.

**The one-sentence version:** a weight platform under a standard litter box that quietly learns each
cat's body weight and bathroom pattern, and — as opt-in wellbeing signals, never a camera — flags the
changes vets treat as early warnings, including the one that's a genuine emergency.

> Not to be confused with [`litterbox_witness_demo.md`](../litterbox_witness_demo.md), the repo's
> smallest end-to-end *teaching demo* ("starring a cat"). This is a real health-monitoring **product
> concept** — same fleet, different thing.

---

## 1 · Why this is credible, not cute — the veterinary case

This is a real, vet-validated category. Four measurable quantities carry almost all the signal, and
**body weight is the single strongest, easiest-to-measure one** — vets treat unexplained weight loss
(~5%) as an early red flag that precedes visible illness. The metric→condition map:

| What you measure | Points at |
|---|---|
| **Body-weight trend ↓** | **CKD, diabetes, hyperthyroidism** — non-specific but highest-value; "losing weight" is honest and useful on its own |
| **↑ visit frequency + ↑ urine deposit volume** | CKD, diabetes (polyuria — dilute urine, more/bigger clumps) |
| **↑↑ frequency + straining + ~zero output** | **Urinary blockage — a life-threatening emergency, mostly male cats (24–48 h)** |
| **↓ frequency / ↓ output** | constipation, dehydration, reduced intake |

The evidence is real: Purina's **Petivity** litter-box data trained a **peer-reviewed model that flags
CKD at ~90% F1** (MDPI *Animals* 2026, Quimby et al., Ohio State), with a companion JFMS study on
altered defecation frequency in CKD cats. The blockage signature — *many short straining visits with
no weight deposited* — is detectable from frequency + deposit-weight alone, and it's the one alert
that justifies an urgent "call your vet today," especially for male cats.

The honest limit built into the design: the sensor **cannot separate** hyperthyroid vs. diabetic vs.
CKD weight loss — it flags *"losing weight,"* which is exactly the right, non-diagnostic thing to say.

---

## 2 · The sensor stack

### Weight via load cells + HX711 — the core

A **weight platform under any standard box** (the Petivity pattern — cheap, retrofit, no camera).
Load cell(s) → an **HX711** 24-bit ADC → a XIAO ESP32. From the continuous weight stream you derive:

- **Cat body weight** = the plateau while the cat stands in, minus the tared box+litter baseline.
- **Per-visit deposit weight** = stable baseline *after* minus *before*.
- **Visit detection** = the step up (entry) / step down (exit).

**Resolution reality — size the load cells to the load, don't reflexively go big.** The HX711 is
24-bit but real noise-free resolution is ~14–17 bits, ≈ ±0.1% of full scale. With a **4×50 kg array
(200 kg FS)** that's ±200 g raw — *too coarse* for a 30–100 g urine clump and marginal for a 4 kg
cat's trend. So:

- **Small/covered box → a single 5–10 kg bar load cell** (~±5–10 g) is plenty for one cat's weight.
- Use the 4×50 kg array only for large/heavy boxes, and lean on **heavy time-averaging over the
  multi-second plateau** — the body-weight *trend* only needs ~±20–50 g over days, which averaging
  gets even off a coarse array. Treat **deposit weight as coarse buckets** (small/medium/large), not
  grams.

**Drift, not resolution, is the real enemy.** Load cells drift with temperature and creep. Mitigations
that must be in the design: **continuous auto-tare** (re-baseline whenever the signal is flat and no
visit is active — this also absorbs litter scatter, scooping, refills, and evaporation), a warm-up
before trusting calibration, rigid flat mounting, and a big-refill re-baseline event so a litter top-up
isn't logged as a giant "deposit."

### Entry/exit gate — cheap, and it earns its place

Weight alone detects whole-cat visits well, but a **single VL53L1X ToF** (~$3–5) across the entrance
gives crisp entry/exit edges (clean *duration*), handles the **paws-on-rim half-step-in** ambiguity,
and can even hint at posture. Recommended as a secondary gate; a $1 PIR is the budget fallback.

### Multi-cat — weight first, tag as the certainty upgrade

How the field does it: Petivity and Litter-Robot's SmartScale assign visits to per-cat profiles by
**body weight + behavioral pattern, no tag** (works well when cats differ by ≳0.5 kg; blurs for
same-weight cats — the known failure). Litter-Robot 5 Pro adds a camera (off the table for us). The
clean fix for same-weight cats is an **optional opt-in RFID/BLE collar tag + an entrance reader**,
which stamps each visit with an identity and collapses the ambiguity — matching the privacy-first,
you-own-the-tag posture we use elsewhere.

---

## 3 · Claim mapping & privacy

- **Health metrics ride the wellbeing channel — never the sealed log.** Body weight, visit frequency,
  duration, and coarse deposit bucket are **wellbeing signals** on the health/status topic, exactly
  like [Canary Sense](../canary_sense_mr60bha2_design.md)'s breathing/heart-rate: non-diagnostic,
  **P1 opt-in**, HA-only, never sealed-logged, never in an evidence bundle. The contract enforcer's
  descriptor allowlist makes it structurally impossible for a health number to seal an event.
- **The one thing that may seal is a coarse *visit* event** (`PresenceInRestrictedZone` at
  `zone:litter`) if the owner wants litter visits on the timeline — coarse presence, no metrics. Even
  that is optional; the device is fundamentally a *wellbeing* monitor, not a witness of intrusions.
- **No camera, ever.** Multi-cat is by weight (coarse) or an owned tag — never visual ID.
- **No new claim vocabulary** — reuses the wellbeing channel and `PresenceInRestrictedZone`.

---

## 4 · Cheapest reliable BOM

| Part | Choice | ~$ |
|---|---|---|
| MCU | XIAO ESP32-C3/S3 (WiFi/BLE) | $5–8 |
| Load-cell ADC | HX711 (separate VCC/VDD) | $1–3 |
| Load cell(s) | 1× 5–10 kg bar (small box) *or* 4×50 kg (large) | $3–10 |
| Entry gate (optional) | VL53L1X ToF | $3–5 |
| Multi-cat (optional) | RFID reader + collar tags | $8–15 |
| Base plate + feet | even load transfer, flat mount | $5 |
| **Core (weight-only)** | | **~$15–25** |
| **Loaded (+ ToF + RFID)** | | **~$30–45** |

**Calibration/drift recipe:** two-point calibration at install (empty, then a known mass) stored in
NVS → continuous auto-tare → visit segmentation with a min-weight + min-duration gate to reject
paws-on-rim → **trimmed-median over the plateau** (a moving cat is the dominant noise source, worse
than ADC resolution) → report body weight as a **multi-day rolling median** (where the clinical value
lives).

---

## 5 · Never let it rot

- **Wellbeing-channel discipline reused wholesale** from Canary Sense — non-diagnostic, P1-gated,
  HA-only; the health data never touches the sealed chain, by construction.
- **Software absorbs the hardware's sins** — auto-tare and plateau-median filtering are the load-bearing
  logic; they're testable pure functions, not field-tuned magic (see open items).
- **Honest tiering** — `concept` until it's lived with real cats; every health signal ships labeled
  non-diagnostic with the vet-consult framing.

---

## 6 · Open items

- **No bench unit, no cats.** The calibration/auto-tare/segmentation logic needs real litter-box
  weight traces to tune and to seed golden test vectors (a pure `segment_visit(weight_trace)` function
  with labeled captures, mirroring the fleet's other pure-function-plus-golden-vectors patterns).
- **The honest reliability risks that remain:** (1) **weight-only multi-cat ID fails for similar-weight
  cats** — the RFID tag is the only real fix; (2) **drift + tare disruption** from scatter/scooping/
  refills/evaporation — managed by auto-tare, never eliminated; (3) **motion & half-in visits** are the
  dominant error source — plateau-median is mandatory; (4) **deposit weight is inherently coarse** —
  bucket it; (5) two cats in overlapping visits break the step model — detect and discard rather than
  mis-attribute; (6) litter dust + urine humidity on the HX711/connectors — conformal coat and seal.
- **The blockage alert is the one urgent path** — its "many visits, ~0 deposit" logic deserves its own
  careful threshold + a clear, non-alarmist "this can be an emergency for male cats, see a vet today"
  message. Getting that copy right is part of the design, not an afterthought.

---

*Sources: veterinary — MDPI *Animals* smart-litter-box CKD model (~90% F1, Quimby et al.); JFMS
defecation-frequency-in-CKD study; Scientific American on the Petivity dataset; Univ. of Illinois
"Blocked Cats" (male emergency); Cornell feline diabetes; VCA hyperthyroidism. Sensors — SparkFun
HX711 resolution guide; Home Assistant HX711 drift thread; Random Nerd / ESPHome litterbox DIY builds.
Products — Purina Petivity, Whisker Litter-Robot SmartScale, PetSafe ScoopFree, Footloose, SureFeed
RFID. Full URLs in the concept card's `sources` block.*
