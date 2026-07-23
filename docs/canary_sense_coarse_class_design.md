# Canary Sense — coarse mover-class from the radar we already ship (design/spec)

**Status:** proposed capability — a **software-only** upgrade to the shipped
[Canary Sense](./canary_sense_mr60bha2_design.md) 60 GHz radar. **No new hardware.** Not built;
this is the scoping doc. It is the "do it with what we have" half of
[Canary Ranger](./hardware/canary_ranger_research.md) §6 — the same micro-Doppler idea distilled
from Samraksh's BumbleBee work, applied to the FMCW radar already on the bench instead of a new
Doppler node.

**The one-sentence version:** teach the Sense radar to attach a *coarse size/type class* — **"large
mover"** (person/vehicle-scale) vs **"small mover"** (animal-scale) — to the presence it already
reports, from Doppler features it already receives, without tracking, identifying, or storing a path
for anyone.

---

## 1 · Why this is nearly free

Sense already computes almost everything the classifier needs and throws most of it away:

- It already reports **presence**, **occupancy 0/1/2+**, **approaching/receding direction**, a
  **motion/Doppler spectrum** (the 8-band activity display), and range bands.
- The BumbleBee papers' recipe for "walk / run / crawl / human-vs-vehicle" is just **spectrogram
  features → a light SVM/decision-tree** ([MDPI *Sensors* 2012](https://www.mdpi.com/1424-8220/12/2/1336),
  [IEEE GRSL 2015](https://ieeexplore.ieee.org/document/7172472/)). We already have the spectrogram;
  we're adding the small classifier on top.
- The output maps onto vocabulary and a pipeline the fleet **already has** (see §5) — so the
  drift-gated dictionary doesn't move for the recommended Phase 0.

The genuinely new *device* (Ranger) exists because our FMCW radar's ~1–2 W budget can't live on a
fencepost. But indoors, on mains power, **the classification itself is a firmware feature, not a new
part.**

---

## 2 · Signal source — use what's decoded, not the unverified frame

**Primary features come from the aggregate Doppler/motion data the firmware already decodes** — the
per-frame velocity/Doppler spectrum, motion magnitude, and range band. This keeps the feature path
on ground we already stand on.

**Deliberately NOT depended on: the raw 3D point cloud (`0x0A08`).** The radar notes flag that frame
as *partnership-gated and unverified* ([`mr60bha2_radar_notes.md`](./hardware/mr60bha2_radar_notes.md)
§1–2). A classifier built on an unverified frame would rot the first time the vendor changes it. If
the point cloud is ever verified, it becomes an *optional enhancement* (better multi-target
separation), never the dependency.

---

## 3 · Features — the micro-Doppler recipe, coarse and range-honest

A short window (~1–2 s of frames), then **discard the window** — no persistent per-target history
(§4). From that window, ~6 cheap features:

| Feature | What it separates | Note |
|---|---|---|
| **Range-normalized RCS / amplitude** | big vs small return | *must* normalize by range band — a small close mover and a large far one look alike otherwise (§7 honesty) |
| **Torso Doppler centroid** | bulk radial speed | a walking person's torso vs a fast small animal vs a rolling vehicle |
| **Micro-Doppler bandwidth / spectral spread** | limbed vs rigid | swinging limbs (human/animal) widen the spectrum; a vehicle is spectrally narrow |
| **Cadence / periodicity** | gait *tempo*, not gait *identity* | stride/limb periodicity distinguishes a walker from a vehicle — a **rate**, never a signature (§4) |
| **Spectral entropy** | complex vs simple motion | corroborates limbed-vs-rigid |
| **Velocity magnitude** | crawl/walk/run band | feeds the *unsealed* activity hint only |

All six are single-window scalars. None require identifying, following, or storing anything about a
specific mover.

---

## 4 · The classifier — and the line it must not cross

A **tiny decision-tree / SVM** (a handful of nodes) over those features, running on the Sense host
MCU, emitting a **coarse bucket + confidence**:

- `large_mover` (person / vehicle scale) · `small_mover` (animal scale) · `unsure`

**The privacy contract is inherited wholesale from
[`canary_sense_mr60bha2_design.md`](./canary_sense_mr60bha2_design.md) §2.2, and this feature adds
three hard rules of its own:**

1. **Single-window, then forget.** The classifier reads one short window and discards it. **No
   cross-frame association, no per-target IDs, no trajectory** — exactly §2.2, applied to the
   classifier's own state. Two movers are "two movers," never "mover A again."
2. **Class, never identity.** Cadence is used as a *tempo* feature to tell limbed motion from a
   vehicle — it is **never** retained or matched as a gait signature. Gait ID is
   [absent from the codebase by design](./strategy/14-pose-estimation-v2-ai.md) and stays that way.
   The moment a "classifier" starts telling *individuals* apart, it's the thing we don't build.
3. **Activity (walk/run/crawl) is an unsealed hint only.** Like Sense's existing "Active" state, the
   velocity-band activity guess is a *local/dashboard* signal — never a sealed claim, because
   behavior inference sits closer to the line than size does.

**Fail safe.** Radar size-class is genuinely coarse (§7). On low confidence the output is `unsure`,
and the emitted claim degrades gracefully to plain presence — *never* a false-confident "small
animal" that makes someone ignore a real intruder, and never a false "person" that cries wolf.

---

## 5 · Claim mapping — two options, contract-respecting

The recommended path moves the frozen contract **not at all**:

**Option A (recommended, Phase 0): size-class as an *attribute* of the existing
`PresenceInRestrictedZone` claim.** This is exactly how the design doc already handles **target
count** — "count as a confidence-weighted aggregate attribute … never a track log"
([design §2](./canary_sense_mr60bha2_design.md)). A `mover_class ∈ {large, small, unsure}` attribute
rides the same presence claim. **Zero dictionary change, zero descriptor-allowlist change**, and it
inherits the presence claim's existing signing/coarsening.

**Option B (later, if it should read as its own timeline event): emit
`boundary_crossing_object_large` / `boundary_crossing_object_small`.** Both kinds **already exist** in
the dictionary (used by the camera/Frigate path), so no new vocabulary — but it *does* require
**allowlisting them on the `mr60bha2` adapter descriptor** (which today permits only
`PresenceInRestrictedZone` + `TamperDetected`, design §4). That's a real, reviewable contract-surface
change, and it changes the semantics from "a large mover is present" to "a large object crossed" —
promote to it only if the product wants the distinct event, not by default.

**Privacy class:** P1 opt-in for the size attribute, at least initially — it's more inferential than
bare presence, so it should be a deliberate enable, not on-by-default, until the false-alarm floor is
measured (§7).

---

## 6 · Pipeline integration

- **Where it runs:** on the Sense host, at the existing **privacy chokepoint** that already reads and
  drops phase/point-cloud data. The classifier consumes the window, emits `{presence, mover_class,
  confidence}`, and drops the raw features — same shape as today, one field richer.
- **Track A (native `canary-sense` firmware):** the coarse class rides the existing signed MQTT event
  path; Option A needs no schema change.
- **Track B (ESPHome + `mqtt_sensor` adapter):** extend the existing `mr60bha2` route profile
  (design §4.2) to carry the attribute. For Option B, the adapter descriptor's allowlist is where the
  new claim kinds would be permitted — the contract enforcer guarantees a buggy payload still can't
  seal anything outside the allowlist.
- **Sealed vs. wellbeing:** the coarse class is a **physical predicate → it seals** (unlike vitals,
  which bypass the log). That's correct: "a large mover was present in the workshop at 02:10 bucket"
  is exactly the kind of coarse, signed fact the witness log is for.

---

## 7 · Honesty — what radar size-class is and isn't

This section is load-bearing; overselling it would be the failure mode.

- **RCS is confounded.** Return strength depends on range, aspect angle, and material as much as
  size — a person side-on, a person crawling, a large dog, and a mover behind partial cover can all
  break a naïve size threshold. Range normalization (§3) helps but doesn't erase it.
- **Animal-vs-person is the hard case.** Large-vs-small is tractable; "dog vs. crawling child" is
  not, reliably, from coarse Doppler alone. The doc must never imply otherwise, and the claim must
  carry confidence so downstream automations can weight it honestly.
- **It is a *hint that corroborates*, not an oracle.** Its best use is the design doc's existing
  **two-physics corroboration** (§2.1): radar "large mover" + camera "person" in the same zone bucket
  is strong; radar alone is a weighted hint. Contradictions surface as anomalies, as today.
- **Non-diagnostic tier**, same discipline as the vitals: documented accuracy bounds, fail-safe to
  `unsure`/presence, opt-in.

---

## 8 · Test & drift discipline

The classifier logic must be a **pure, testable function with golden vectors**, mirroring
`scripts/dbc_signal_resolve.py`'s `selftest` and the dictionary-sync linter:

- **`classify_window(features) -> (class, confidence)`** is pure — no I/O, no time, no state. Feed it
  captured/synthetic feature vectors with known labels; assert the bucket and a confidence band.
- **Golden vectors** from a handful of labeled captures (person walk/still, small animal, vehicle,
  empty-with-fan) become the CI selftest — a regression in the feature math or tree fails loudly
  before it reaches a claim.
- **No dictionary drift for Option A** (attribute only). Option B would add
  `boundary_crossing_object_large/_small` to the `mr60bha2` descriptor and must pass the existing
  `lint_dictionary_sync.py` unchanged (the kinds already exist; only the allowlist grows).

---

## 9 · Phased plan

- **Phase 0 (tractable, no contract change):** range-normalized amplitude + velocity-spread →
  `{large, small, unsure}` as a **confidence-weighted attribute on `PresenceInRestrictedZone`**,
  P1 opt-in, fail-safe. Golden-vector selftest. This is the shippable "with what we have" slice.
- **Phase 1:** add the micro-Doppler spectral features (§3) + a small **trained** decision-tree from
  labeled captures; improve the large/small margin and the vehicle case. Still an attribute.
- **Phase 2 (optional, product call):** promote to distinct
  `boundary_crossing_object_large/_small` emissions (Option B) if the timeline should show them as
  their own events — a reviewable descriptor-allowlist change, nothing more.
- **Never:** gait/identity, cross-frame tracking, per-target IDs, sealed activity claims.

---

## 10 · Open items

- **No captured feature data yet.** The classifier needs a small labeled corpus (person/animal/
  vehicle/empty) from a real MR60BHA2 to fit and to seed golden vectors — the one real prerequisite.
- **Feature availability audit:** confirm which of §3's features are cleanly derivable from the
  *already-decoded* aggregate stream vs. would need the unverified point cloud (§2) — this bounds
  what Phase 0 can actually use.
- **Attribute vs. distinct-kind** (§5) is the one product/contract decision; Phase 0 assumes the
  attribute.
- **Range normalization constants** are device/mount-specific — they belong in the same drift-gated
  `SIM:` table discipline the radar notes already use, not hand-tuned in code.

---

*Provenance: the micro-Doppler classification approach is distilled from Samraksh BumbleBee work —
[MDPI *Sensors* 2012](https://www.mdpi.com/1424-8220/12/2/1336) (peer-reviewed) and
[IEEE GRSL 2015 micro-Doppler activity](https://ieeexplore.ieee.org/document/7172472/) — via
[`hardware/canary_ranger_research.md`](./hardware/canary_ranger_research.md) §6. It inherits the
privacy contract of [`canary_sense_mr60bha2_design.md`](./canary_sense_mr60bha2_design.md) §2.2 and
the signal reality of [`hardware/mr60bha2_radar_notes.md`](./hardware/mr60bha2_radar_notes.md).*
