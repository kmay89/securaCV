# Pose estimation for v2 — adopt the model, refuse the skeleton

**Status:** research + design proposal (spec-first; no code in this doc)
**Owner:** firmware maintainers + spec maintainers
**Trigger:** study of the Hackster project
[*"Live Pose→3D: XIAO AI Cam / Grove Vision AI + Processing IDE"*](https://www.hackster.io/tvjk/live-pose-3d-xiao-ai-cam-grove-vision-ai-processing-ide-df4b44)
(tvjk / Vijay Kumar) — "can we use this for our own v2 AI?"
**Companions:**
[`docs/strategy/10-grove-vision-ai-v2-program.md`](10-grove-vision-ai-v2-program.md) (this is Phase 3's worked example) ·
[`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md) (claim-vocabulary discipline) ·
[`spec/invariants.md`](../../spec/invariants.md) (Invariant I & II are the whole story here) ·
[`firmware/projects/canary-vision/`](../../firmware/projects/canary-vision/) (where the model would run)

---

## 1. Decision (the one-liner)

**Adopt the pose *model*; refuse the pose *stream*.**

Run YOLOv8-Pose on the Grove Vision AI V2's Himax NPU — the exact module and
stack `canary-vision` already drives — and use the richer perception to derive
*coarse physical claims* ("a person went horizontal and stayed down"). Do **not**
do what the Hackster project does: stream a live 17-point skeleton off the device.
A full skeleton is approximately-reversible biometric data; under
[Invariant I](../../spec/invariants.md) it must be treated as **raw media** and
must never leave the sensor. The skeleton stays on the device and dies in RAM;
only the coarsened, signed claim crosses the wire.

This is not a new program. It is the **worked example** that
[doc 10, Phase 3](10-grove-vision-ai-v2-program.md#phase-3--model-lifecycle--multi-signal-vocabulary)
already reserved a slot for ("at least one non-person model shipped end-to-end
with spec, firmware constants, HA entities, and bench numbers").

---

## 2. What the Hackster project actually is

A single-wire, zero-cloud edge-AI demo. Reconstructed pipeline (the article renders
via Hackster's JS shell and 403s to automated fetches; the following is assembled
from the project's own summary, Seeed's Grove Vision AI V2 / SenseCraft docs, and
the SSCMA output format we already consume):

```
  XIAO AI Cam  ─┐
  (OR Grove     │  YOLOv8-Pose runs ON the Himax HX6538 NPU
   Vision AI V2)│  → 17 COCO keypoints per detected person
                │
   [USB serial, one wire] ──► PC running Processing IDE 4.4.7
                                • parses keypoints per frame
                                • renders a 3D "mirror-mode" stickman
                                • stickman scales with distance (bbox size)
                                • SPACE toggles a cinematic orbit camera
   (optional) ──► MAX7219 8×8 LED matrix shows a low-res pose glyph
```

| Element | Detail |
|---|---|
| Sensor / NPU | XIAO AI Cam **or** Grove Vision AI V2 — Himax WiseEye2 **HX6538** (Ethos-U55 + Cortex-M55), on-module inference |
| Model | YOLOv8-Pose, flashed once via **SenseCraft** web flasher over the module's USB port |
| Output | **17 COCO keypoints** per person (nose, eyes, ears, shoulders, elbows, wrists, hips, knees, ankles), each `[x, y, score]` |
| Transport | Raw keypoints over **USB serial** to a PC (Processing 4.4.7 is pinned because it bundles the serial library) |
| Renderer | Processing sketch → 3D stickman, mirror mode, distance scaling, SPACE cinematic cam |
| Extras | Optional MAX7219 8×8 matrix; no WiFi, no cloud, one wire |

**Why it matters to us:** strip the visualization and what remains is *"YOLOv8-Pose
running on the same HX6538 module our `canary-vision` firmware already talks to over
I2C."* We are one model-swap in SenseCraft away from having keypoints available. The
project is, for our purposes, a proof that stock pose models load and run on our
hardware today.

---

## 3. Why this is a natural fit — we already own the whole stack

`canary-vision` is not adjacent to this project; it *is* this hardware, minus the
skeleton stream:

| Layer | What `canary-vision` does today | Source |
|---|---|---|
| Module | Grove Vision AI V2 (HX6538), inference on-module | [`README`](../../firmware/projects/canary-vision/README.md) |
| Library | `Seeed_Arduino_SSCMA` — `AI.invoke()` → `AI.boxes()` | [`vision_mgr.cpp`](../../firmware/projects/canary-vision/src/vision/vision_mgr.cpp) |
| Model | **Person detection** (bounding boxes only) | SenseCraft, loaded once |
| Host | XIAO ESP32-C3 / S3 reads results over **I2C** — no pixels cross | [doc 10 §1](10-grove-vision-ai-v2-program.md) |
| Coarsen | bbox center → **3×3 voxel** grid | `bbox_to_voxel()` |
| Decide | presence / dwell / interaction FSM | [`presence_fsm.cpp`](../../firmware/projects/canary-vision/src/state/presence_fsm.cpp) |
| Emit | Ed25519-signed, hash-chained claim → MQTT + HA Discovery | `witness.cpp` |
| Kernel | `grove_vision2_ingest` accepts **only** `event_type/time_bucket/zone_id/confidence`, rejects extras as conformance alarms | [`grove_vision2_ingest.rs`](../../src/bin/grove_vision2_ingest.rs) |

Pose changes exactly one thing: the model emits **keypoints in addition to boxes**.
Everything downstream of "derive a coarse claim" is already built, signed, and
CI-guarded. The SSCMA library already surfaces keypoints for pose models — today's
firmware simply never calls for them (it only reads `AI.boxes()`).

---

## 4. The fault line — a skeleton is raw media, not an event

This is the crux, and it is where our design must diverge hard from the Hackster
project.

[Invariant I](../../spec/invariants.md) (No Raw Export) says, verbatim:

> Derived artifacts that are reversible or approximately reversible to raw media
> (e.g., feature maps, embeddings, **high-resolution masks**) MUST be treated as raw
> media for the purposes of this invariant.

A live 17-point skeleton is squarely inside that definition:

- **It is approximately reversible.** Joint trajectories reconstruct gait, posture,
  and body proportions — enough to re-identify individuals and to replay behavior.
  That is precisely the "reconstruction of identity, appearance, or continuous
  movement" Invariant I exists to prevent.
- **It is a biometric identity substrate.** Skeletal proportions and gait are
  recognized biometrics — a direct hit on [Invariant II](../../spec/invariants.md)
  (No Identity Substrate). The head keypoints (nose/eyes/ears, indices 0–4) are the
  most identity-bearing of the seventeen.
- **The Hackster design's *entire point* is to export it** frame-by-frame over
  serial so a PC can render "you." For a demo that is delightful. For a witness it
  is the exact anti-pattern the kernel forbids.

So the pose model sits on the wrong side of two invariants **if you keep the
keypoints**. The design work is making sure we never do.

| | Hackster "Live Pose→3D" | SecuraCV v2 pose |
|---|---|---|
| Model location | On-module (HX6538) | On-module (HX6538) — same |
| Keypoints leave the device? | **Yes** — streamed every frame | **No** — die in RAM after derivation |
| What crosses the wire | 17× `[x,y,score]` per frame | one coarse physical claim, signed |
| Consumer | Processing 3D renderer (a person) | witness chain → MQTT → HA (an event log) |
| Goal | *render* a body | *notice* a physical state, then forget the body |

---

## 5. How we'd actually use it — "model yes, stream no"

The pose model buys us **physical states a bounding box cannot see**. A box knows
*where* a person is; a skeleton knows *what posture they are in*. Turned into coarse,
hedged, strictly-physical claims (the discipline of
[`canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md), Invariant E:
"Physics, Not Politics"), that unlocks genuinely new witness value:

| Candidate claim | Physical basis (from keypoints, on-device only) | Why it's worth witnessing |
|---|---|---|
| `pose_horizontal_sustained` | Shoulder–hip axis near-horizontal for > N s while present | Fall / collapse / person down — the highest-value safety signal a camera can offer without watching |
| `pose_prone` (ordinal) | Body horizontal + low in frame | Distinguishes "lying down" from "standing" without identifying who |
| `pose_upright` | Shoulder–hip axis vertical | The negative/idle case; anchors the FSM |
| `pose_reach_across_boundary` | Wrist keypoint crosses a configured zone edge while torso stays outside | "Something reached over the fence" — a boundary event a bbox smears |
| `pose_hands_raised` | Both wrists above both shoulders, sustained | **Duress-gesture candidate** — pairs with the existing beacon duress model (see `spec/beacon_channel_v0.md`); advisory only |

Rules that keep every one of these inside the invariants:

1. **Keypoints are raw media in RAM.** They are read from the module, consumed by a
   pure derivation function, and **zeroized** — never logged, never in an MQTT
   payload, never in a snapshot. This is exactly the pre-roll-buffer discipline
   Invariant I §3.1 already mandates for frames.
2. **Emit predicates, not coordinates.** The claim carries a handful of booleans /
   ordinals + the existing 3×3 voxel for location — never a joint angle, never a
   pixel coordinate, never a height. This is the pose analog of `bbox_to_voxel()`:
   a "high-resolution mask" coarsened to a coarse state.
3. **Physics, not intent.** `pose_horizontal_sustained`, never `person_collapsed_needs_help`;
   `pose_hands_raised`, never `robbery_in_progress`. Meaning is the human's to assign
   (Invariant D, Human-in-the-Loop).
4. **Vocabulary is spec-gated.** Per Invariant VI (no silent vocabulary growth), each
   new claim above lands in `spec/canary_free_signals_v0.md` (and the kernel's
   allowed `event_type` set) in a spec PR **before** any firmware emits it.
5. **No new export fields.** `grove_vision2_ingest` still accepts only
   `event_type/time_bucket/zone_id/confidence`. Pose claims ride the existing
   contract — if a pose claim can't be expressed in those four fields, it's too rich
   to emit.

### 5.1 Where the derivation runs — the honest trade

There is a real weakening to be explicit about. Today the privacy boundary is
*physical*: the HX6538 emits boxes and the ESP32 never sees pixels. With stock pose
models, **keypoints cross the I2C bus into host RAM** before we coarsen them — so the
boundary moves from "the module's silicon" to "host firmware we control." Two options:

- **Option A — host-side derivation (recommended for v2.0).** Stock SSCMA YOLOv8-Pose
  on the module; host reads keypoints, derives the claim, zeroizes. Fast, reuses the
  existing library and flash flow. Keypoints briefly transit I2C and live in RAM —
  compliant **iff** treated as raw media (rules 1–2 above), which is the same
  guarantee the camera-bearing XIAO-S3-Sense canary already relies on for frames.
- **Option B — on-module derivation (v2.x hardening).** A custom SenseCraft model /
  post-processing head that outputs only the claim (e.g. an upright-vs-horizontal
  classifier), so **only the predicate crosses I2C**. Restores the "boundary by
  construction" strength of doc 10 §1, at the cost of a training/validation loop. Do
  this *after* a claim proves its worth on the bench.

Recommendation: ship Option A first behind the raw-media rules; graft Option B onto
any claim that earns a permanent slot.

---

## 6. Concrete integration points

Small, localized, and mostly additive to files that already exist:

| File | Change |
|---|---|
| `firmware/.../vision/vision_mgr.cpp` | After `AI.invoke(...)`, also read pose keypoints (SSCMA points API) into a **stack-local** buffer; pass to a new deriver; do not store. Any USB-serial echo of those keypoints for the bench viewer is wrapped in `#if POSE_BENCH_STREAM` (see §7) and is absent from shipped builds |
| `firmware/.../include/canary/types.h` | Add a `PoseClaim` struct of **predicates only** (e.g. `bool horizontal; bool hands_raised; uint8_t orientation_ord;`) — no coordinates. Give it a **default `inactive` state and a `valid()` predicate, mirroring the existing `Voxel::Invalid()` / `valid()` pattern** in this same header, so the person-detection model (which produces no keypoints) leaves `PoseClaim` inactive when it fills `VisionSample` — no code path is ever forced to fabricate a pose, and consumers gate on `claim.valid()` exactly as they already do for `Voxel`. Extend `VisionSample` with a default-inactive `PoseClaim` |
| `firmware/.../state/pose_deriver.*` (new) | Pure function: keypoints → `PoseClaim`. The **only** place keypoints exist; must zeroize its input before return. Unit-testable host-side (`firmware/tests_host`) |
| `firmware/.../state/presence_fsm.cpp` | Consume `PoseClaim` to gate new events (`pose_horizontal_sustained` needs a dwell-style timer, mirroring `dwell_start_ms`) — reuse the existing latch/timeout machinery |
| `firmware/.../detect_config.*` | NVS-backed thresholds (horizontal-angle tolerance, sustain-ms, hands-raised-ms) as HA `number` entities — same pattern as the person-detect knobs shipped in #788 |
| `spec/canary_free_signals_v0.md` | **First PR.** Add the pose claim vocabulary + "keypoints are raw media" conformance clause |
| `src/bin/grove_vision2_ingest.rs` | Add the new `EventType`s to `allowed_event_types`; **no schema change** — pose claims fit the existing 4-field contract |
| `custom_components/securacv` + `homeassistant/lovelace/` | Surface the new claims as binary sensors / device-triggers; extend the vision dashboard (fall alert card). Advisory automations only |

Note the shape: **one new pure function (`pose_deriver`) and one spec PR** are the
load-bearing additions. Everything else is wiring into machinery that already signs,
chains, coarsens, and surfaces claims.

---

## 7. The Processing viewer — keep it, but cage it

The 3D-stickman renderer is genuinely useful — *as a bench tool*, never as a product
data path. We already have a precedent for exactly this move: doc 10 §5 keeps
SenseCraft "for exactly one job" (loading models over USB) while rejecting its cloud
telemetry path. Apply the same discipline to Processing — and make it a **compile-time
guarantee, not a documentation promise.**

Any code path that echoes keypoints over USB serial for the viewer is gated behind a
single build flag, **`POSE_BENCH_STREAM`**, with **explicit per-build values** (never
one ambiguous default):

| Build env | `POSE_BENCH_STREAM` | Keypoint serial stream |
|---|---|---|
| every shipped env — `canary-vision-xiao-c3`, `canary-vision-xiao-s3`, `canary-vision-default` | **`0`** | `#if`-compiled **out** — the streaming code is not in the binary |
| a dedicated, never-released bench env (e.g. `canary-vision-bench`) | **`1`** | present, USB-only, feeds the Processing harness |

The flag **defaults to `0`**, every production env pins `0` explicitly, and only the
bench env sets `1` — and that env is **excluded from `firmware-release.yml`** so it
can never ship. Because the stream is `#if POSE_BENCH_STREAM` *compiled out* of shipped
builds, there is no runtime toggle and no accidental-enable path: the boundary is
enforced by the preprocessor, not by config. The feature-flag hygiene lint
(`scripts/lint_feature_flags.sh`) is the natural gate to assert `POSE_BENCH_STREAM`
stays default-off, matching doc 10's "no new compile-time features enabled by default."

- **Allowed:** with `POSE_BENCH_STREAM=1` on the bench env only — an **offline,
  human-attended, USB-only** calibration/demo harness. Aim the sensor, watch the live
  skeleton on a laptop, tune the horizontal-angle and sustain thresholds by eye,
  confirm a fake fall trips `pose_horizontal_sustained`. Analogous to
  `docs/hardware/bench_bringup.md` bring-up rituals.
- **Forbidden:** shipping any env with `POSE_BENCH_STREAM=1`, or emitting the skeleton
  to MQTT, the SPA, HA, the witness chain, or the network under *any* flag value. The
  viewer talks to a dev's USB port on a bench, full stop. It is a debug scope, not a
  sensor output.

If we adopt the Processing sketch, it lives under `tools/` or `firmware/examples/`
(built only by the bench env) with a README that says, in the repo's own voice: *this
renders raw biometric data and therefore must never touch a witness path.*

---

## 8. Fit with the existing roadmap

This slots directly into [doc 10 Phase 3](10-grove-vision-ai-v2-program.md#phase-3--model-lifecycle--multi-signal-vocabulary),
which already calls for:

> New claim types from alternate models, kept within
> `spec/canary_free_signals_v0.md` discipline: e.g. gesture-as-duress signal … Each
> new claim needs a spec PR first (Invariant VI: no silent vocabulary growth).

and:

> **Exit:** at least one non-person model shipped end-to-end with spec, firmware
> constants, HA entities, and bench numbers.

Pose is the cleanest candidate to *be* that exit criterion: same board, same
library, same flash flow, and a safety signal (fall/collapse) with obvious value to
the exact users the product exists for (doc 08 personas — the at-risk / evidence
user). No new hardware, no new BOM line, no new vendor dependency.

---

## 9. Phased plan (spec-first, per Invariant VI)

| Phase | Work | Exit |
|---|---|---|
| **P0 — Spec** | Add pose claim vocabulary + "keypoints = raw media" clause to `canary_free_signals_v0.md`; add `EventType`s to the kernel's allow-list | Spec PR merged; kernel accepts (but nothing emits) the new claims |
| **P1 — Deriver** | `pose_deriver` pure function + host unit tests (synthetic keypoint fixtures for horizontal / upright / hands-raised); zeroization test | `firmware/tests_host` green; deriver never retains input |
| **P2 — Bench** | Option A firmware behind a build flag; Processing bench harness (§7); tune thresholds; measure invoke latency / FPS / power for pose vs person-detect | `docs/hardware/bench_bringup.md`-style numbers; one claim (recommend `pose_horizontal_sustained`) trips reliably |
| **P3 — Ship one claim** | Wire the one proven claim into the FSM, HA entities, dashboard fall-alert card, log-verify parity | A vision canary emits a signed fall-like claim; verifiable end-to-end; doc 10 Phase 3 exit met |
| **P4 — Harden (opt.)** | Option B on-module derivation for the shipped claim; retire host-side keypoint transit for it | Only the predicate crosses I2C for that claim |

Ship **one** claim well before adding more. Each additional claim repeats P0→P3.

---

## 10. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **Keypoints leak** into a log/MQTT/snapshot | `pose_deriver` is the sole keypoint site + zeroizes; add a conformance test asserting no coordinate-shaped field appears in any emitted payload (extend the `grove_vision2_ingest` reject-extras test) |
| **Scope creep into surveillance** (gait ID, "who fell", re-identification) | Invariant II + Free Signals §5 hard-stops; claims are physical predicates only; head keypoints dropped first |
| **Pose model is slower** than person-detect on HX6538 | Measure in P2; pose is heavier, expect lower FPS — fine for sustained-state claims (fall needs seconds, not milliseconds) |
| **False falls** (sitting on floor, yoga, bending) | Sustain timer + angle tolerance are NVS-tunable; ship as *advisory* (Invariant D), never an auto-escalation |
| **Class/keypoint-index drift** across SSCMA model versions | Same failure mode doc 10 §6 already tracks for `PERSON_TARGET`; pin the pose model, document indices, runtime-tunable |
| **"Duress gesture" over-promises** | Keep it a *candidate* signal fused with the beacon duress path (`spec/beacon_channel_v0.md`), never a standalone accusation |

---

## 11. Open questions to settle on the bench

1. Pose FPS and invoke latency on HX6538 vs. the current person-detect model — does a
   sustained-state claim tolerate the frame rate? (Almost certainly yes.)
2. Does the stock SSCMA YOLOv8-Pose model expose per-keypoint scores we can gate on
   (drop low-confidence joints before deriving)?
3. Minimum keypoint subset for a robust `horizontal` predicate — can we ignore head
   points entirely and use only shoulders+hips (the least identifying joints)?
4. Power draw delta (pose vs. detect) for battery/solar canary variants.
5. Is Option B (on-module classifier) trainable in SenseCraft to a quality where the
   predicate is reliable without host-side keypoints?

---

## 12. Recommendation

**Yes — adopt it, on our terms.** The Hackster project is a free proof that our exact
module runs YOLOv8-Pose today. Take the model and the on-device inference; leave the
skeleton stream on the workbench. Ship **one** high-value physical claim
(`pose_horizontal_sustained` — fall/collapse) end-to-end, spec-first, as the concrete
fulfillment of doc 10's Phase 3 exit criterion. The restraint — running a model
capable of drawing your body in 3D, and choosing to emit only *"someone went down and
stayed down, roughly there, roughly then"* — is not a limitation we tolerate. It **is
the product**.

---

## 13. Sources

- [Hackster — *Live Pose→3D: XIAO AI Cam / Grove Vision AI + Processing IDE*](https://www.hackster.io/tvjk/live-pose-3d-xiao-ai-cam-grove-vision-ai-processing-ide-df4b44) (tvjk / Vijay Kumar)
- [Hackster — *Computer Vision at the Edge with Grove Vision AI Module V2*](https://www.hackster.io/mjrobot/computer-vision-at-the-edge-with-grove-vision-ai-module-v2-0003c7) (SSCMA output format reference)
- [Seeed — XIAO Vision AI Camera (Grove Vision AI V2 + XIAO + OV5647)](https://www.seeedstudio.com/XIAO-Vision-AI-Camera-p-6450.html)
- Internal: [`docs/strategy/10-grove-vision-ai-v2-program.md`](10-grove-vision-ai-v2-program.md), [`spec/invariants.md`](../../spec/invariants.md), [`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md), [`spec/event_contract.md`](../../spec/event_contract.md), [`firmware/projects/canary-vision/`](../../firmware/projects/canary-vision/), [`src/bin/grove_vision2_ingest.rs`](../../src/bin/grove_vision2_ingest.rs)
</content>
</invoke>
