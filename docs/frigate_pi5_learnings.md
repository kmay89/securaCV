# Architectural Learnings from Frigate — a fast Pi 5 + ESP32-S3 daylight pipeline

Status: Informational
Intended Status: Reference (Non-Normative)
Last Updated: 2026-07-30

## Purpose

We already ship a Frigate *integration* ([`frigate_integration.md`](frigate_integration.md)):
Frigate detects, `frigate_bridge` strips the detection down to a coarse claim, the
kernel seals it. That document is about **consuming** Frigate.

This one is about the opposite direction: **what Frigate's pipeline knows that ours
doesn't**, and how to apply it to a first-party SecuraCV pipeline running on a
Raspberry Pi 5 hub fed by ESP32-S3 cameras — the daylight use cases (movement in a
zone, a package on the porch, the cat using the litter box, the dog at the back door)
where an ESP32-class sensor is genuinely sufficient and night IR is explicitly out of
scope.

**We are not vendoring Frigate code.** Frigate is MIT-licensed (`Copyright (c) 2026
Frigate, Inc.`), so reuse would be legally clean, but the shapes worth taking are
*algorithmic* — a gating strategy, a region-sizing heuristic, a stationary-object
rule — and our runtime is Rust with a sealed-log contract Frigate has no notion of.
This is knowledge transfer. Everything below was read from Frigate at commit
`f1cc0e4` (0.18 beta).

This document is **informational** and does not override normative specifications.
For binding constraints see [`spec/invariants.md`](../spec/invariants.md),
[`spec/event_contract.md`](../spec/event_contract.md), and
[`kernel/architecture.md`](../kernel/architecture.md).

---

## 1. The one sentence that matters

> Frigate does not run object detection on frames. It runs object detection on
> **motion regions**, at **5 fps**, on a **~100-pixel-tall** grayscale reduction of
> the frame — and it stops looking at things that have stopped moving.

Everything else in this document is a consequence of that sentence. Our current
kernel pipeline does the opposite of all four clauses, which is why a Pi 5 port that
copies our present design would be slow, and why one that copies Frigate's could be
absurdly fast for the daylight cases.

---

## 2. What Frigate actually does, stage by stage

Frigate's own [`video_pipeline.md`](https://docs.frigate.video/frigate/video_pipeline)
names five stages; the interesting machinery is in stages 3–5.

### 2.1 Motion detection is a cheap, self-calibrating background model

`frigate/motion/improved_motion.py` — the whole detector is ~200 lines of OpenCV and
runs on a downscale so aggressive it surprises people. Config defaults
(`frigate/config/camera/motion.py`):

| Knob | Default | What it does |
|---|---|---|
| `frame_height` | **100** | Motion runs on a 100px-tall grayscale image, aspect preserved |
| `threshold` | 30 | Per-pixel delta vs. the background model to count as changed |
| `contour_area` | 10 | Contours smaller than this (in the 100px frame!) are discarded |
| `frame_alpha` | 0.01 | `accumulateWeighted` rate — how fast the background absorbs change |
| `lightning_threshold` | 0.8 | If >80% of the frame changed, treat as a lighting event, recalibrate |
| `improve_contrast` | true | Rolling 50-frame percentile stretch before differencing |

The sequence is: resize → percentile-based contrast normalization → mask → gaussian
blur → `absdiff` against a running-average background → threshold → dilate → contours
→ keep contours above `contour_area` → scale boxes back up to full frame coordinates.

Four details are the actual engineering, and each one is a bug we would otherwise
ship:

1. **Contrast normalization uses a rolling average of the 4th/96th percentiles over
   the last 50 frames**, not the current frame's min/max. A single frame's min/max
   makes the normalization itself a source of false motion when a bright object
   enters. Keeping a history makes the stretch stable.
2. **Motion is folded into the background only after it persists for 10 frames**
   (`motion_frame_count >= 10`). A thing that moves and stops becomes background;
   a thing that flickers does not corrupt the model.
3. **A `calibrating` state.** On startup, after a mask change, or after a whole-frame
   change, the detector marks itself calibrating, raises the background learning rate
   to 0.2, and — critically — *the consumer ignores its boxes while calibrating*
   (`frigate/video/detect.py:362`). It exits calibration when motion is <5% of the
   frame with ≤4 contours.
4. **Lighting-storm rejection.** >80% of the frame changing is a cloud, an IR cut
   filter, or an automatic light — not an intruder. It forces recalibration rather
   than emitting one giant box.

### 2.2 Detection runs on regions, and regions are square and clustered

`frigate/video/detect.py:305-424`. Per frame, the candidate regions are:

- one region per cluster of currently-tracked object boxes, plus
- one region per cluster of motion boxes **that aren't already inside a tracked
  object's region** (`inside_any`), plus
- on the first frame only, 8 "startup scan" regions from the history grid.

Clustering (`get_cluster_candidates` / `get_cluster_boundary` in
`frigate/util/object.py`) merges nearby boxes only when the merged square region
wouldn't make either box smaller than 5% of the region's area. That rule is the
guard against the failure that kills naive implementations: merging a near box and a
far box into one giant region, then downscaling that region to the model input until
the far object is four pixels tall.

Each region is then cropped from the YUV frame and resized to the model's input
(`create_tensor_input`), so **inference cost is per-region, not per-frame, and every
inference sees the object at a useful scale.** A 1920×1080 frame with one person in
it costs *one* 320×320 inference, and that person occupies a large fraction of it.

### 2.3 The region grid: the camera learns its own perspective

`get_camera_regions_grid` (`frigate/util/object.py:37`) divides the frame into an 8×8
grid and, from the historical timeline of confirmed objects, records the mean and
standard deviation of region size for objects whose centroid fell in each cell. It
rebuilds nightly at 02:00.

When a motion cluster produces a region, `get_region_from_grid` looks up the cell for
the region's centroid: if the computed region is smaller than one standard deviation
below the historical mean for that cell, it is **grown to the historical mean**. In
practice this means a scrap of motion at the top of the driveway gets a region sized
like a person-at-the-top-of-the-driveway, instead of a 32-pixel region containing a
person's hat.

This is self-tuning perspective compensation with zero configuration and no
calibration ritual. It is the single cleverest thing in the codebase and it is
directly portable — it needs only a history of (centroid cell → region size).

### 2.4 Stationary objects: the CPU saver that also makes package detection work

This is the mechanism that matters most for the use cases in the request.

`frigate/track/stationary_classifier.py` + `detect.py:316-347` + config defaults at
`frigate/config/config.py:842-858` (for `detect.fps: 5`: `min_initialized` 2,
`max_disappeared` 25 frames ≈ 5 s, `stationary.threshold` and `.interval` both 50
frames ≈ 10 s):

- An object whose box hasn't moved for `stationary.threshold` frames is marked
  stationary. **Stationary objects are excluded from region generation and their
  previous detection is simply re-emitted** — zero inference cost per frame.
- Every `stationary.interval` frames, the stationary set is cleared for one frame so
  everything is re-verified.
- A stationary object is also re-verified early if a **motion box intersects it**.
- Per-label thresholds (`stationary_classifier.py:45-64`) encode the domain
  knowledge: `package`, `waste_bin`, and `bbq_grill` are *expected* to be stationary
  (`known_active_iou: 0.0` — never flip to active on IoU alone); `car`, `bicycle`,
  `truck` are dynamic-but-can-park; `license_plate` is never stationary.
- The `StationaryMotionClassifier` is the anti-jitter fallback: it keeps an anchor
  96×96 Y-plane crop of the object's *historical median box*, and each check computes
  normalized cross-correlation (`cv2.matchTemplate`, TM_CCOEFF_NORMED) plus
  `cv2.phaseCorrelate` sub-pixel shift against that anchor. NCC ≥ 0.90 and shift
  < 0.02 → definitely still stationary. It requires 2 consecutive "changed" frames,
  or cumulative drift over 5 frames, before flipping to active.

**Why this is the package answer.** A package is a thing that arrives and then does
nothing for eight hours. Without stationary handling, either you run inference on it
forever (cost) or your tracker loses it and re-announces it every few minutes
(noise). With it, "package present" is a stable, nearly free state, and — this is the
part our contract already anticipates — **its disappearance is the event**. We have
`ClaimKind::ObjectRemovedFromZone` in `src/adapter/contract.rs:44` with nothing
capable of producing it.

### 2.5 Zones and severity: the false-positive killers that cost nothing

- **Zone inertia** (default 3): the object's box must be inside the zone for N
  consecutive frames before it counts as inside. Kills the single-frame bounding-box
  wobble that puts a car "in" the porch zone.
- **`loitering_time`**: seconds inside the zone before zone membership is granted.
- **`required_zones`** on both alerts and object filters.
- **Two-tier severity** (`frigate/review/types.py`): every review segment is either
  an `alert` or a `detection`. Alerts require a configured label *and* configured
  zones; everything else is a detection. A segment can be promoted from detection to
  alert mid-life but never demoted. The "is this worth waking a human" decision is a
  separate, cheap, declarative layer on top of detection — not a confidence threshold.

### 2.6 Classification is a second, much cheaper model — in two flavors

Frigate 0.16+ ships custom MobileNetV2 classification
(`frigate/data_processing/real_time/custom_classification.py`) in two shapes:

- **Object classification** — runs on the crop of a *tracked object*. Refines a
  detection ("this `dog` is Buddy", "this `car` is a mail truck"). Those two examples
  are Frigate's, and they are **descriptions, not recommendations**: recognizing
  *which* dog is an identity inference we do not build (§5.2, §6). The mechanism is
  still useful to us for coarse non-identifying attributes; the recognizer framing is
  not.
- **State classification** — runs on a **fixed crop of the frame**, on a schedule
  and/or when motion overlaps that crop. Frigate's own documented examples are garage
  door open/closed, gate open/closed, bins at curb, pool cover on/off.

Both use a two-step assignment rule worth stealing wholesale: a prediction is only
accepted when it clears a confidence `threshold` (default 0.8) **and**, after at
least 3 attempts, 60% of attempts agree on the same class. Documented as running
"very fast on CPU".

**State classification is the litter box.** "Cat in the box / not in the box" is a
fixed crop and two classes. It needs no object detector, no tracker, and no
perspective handling. Same for "package on the mat / mat empty" if the porch geometry
is fixed.

### 2.7 Process topology

One detector process owns the accelerator and serves all cameras
(`DetectorRunner`, `frigate/object_detection/base.py:110`); each camera has its own
tracker process. Frames move through **shared memory** — the queue carries a frame
*name*, and the detector writes results into a fixed `(20, 6)` float32 output SHM
segment. Nothing large is ever serialized between processes.

---

## 3. Where SecuraCV stands today (read from the tree, not the roadmap)

Honest gap analysis of the current Rust pipeline. Each of these is a real line of
code, not a hypothetical.

| Area | What we have now | Consequence on a Pi 5 |
|---|---|---|
| **Motion** | `src/detect/backends/motion.rs` — `FrameHashMotion` declares motion when `SHA-256(frame) != SHA-256(prev_frame)`. | Sensor noise changes the hash every frame. Motion is **always true**, so there is effectively no gate: the detector runs on 100% of frames. It also cannot produce boxes, so there is nothing to build regions from. |
| **Object detection** | `src/detect/backends/tract.rs` — tiny-YOLOv2, fixed 416×416, `resize_to_input` bilinear-resizes the **whole frame** every time (`tract.rs:118-134`). | A person at the end of a driveway in a 1600×1200 frame is a handful of pixels after the resize. Also: tiny-YOLOv2/VOC-20 has no `package` class, so package detection is not merely untuned, it is unrepresentable. |
| **Sandbox** | `witnessd` calls `execute_sandboxed` per frame (`src/bin/witnessd.rs:444`), which `fork()`s a seccomp child, pipes the result back, and shuttles detector state across the boundary via `export_state`/`import_state` (`src/detect/backend.rs`). | A per-frame `fork` + `pipe` + `waitpid` is a fixed cost paid on *every* frame, which is comfortably absorbed at the current ~100 ms cadence. **Hypothesis, not a measurement:** that this fixed cost becomes significant relative to JPEG decode and inference as the cadence rises, and therefore that motion gating is worth *more* to us than to Frigate. Nothing in this repo profiles it on a Pi 5. Per AGENTS.md non-negotiable #4 ("no performance claim without a benchmark") this must be benched before any design leans on it — see the phase-1 bench harness in §7, which should report fork/pipe/waitpid, decode, and inference as separate line items. |
| **Tracking** | None. | No object identity across frames ⇒ no stationary logic, no zone inertia, no loitering, no `ObjectRemovedFromZone`, no dedup. Every frame is an independent world. |
| **Regions** | None. Whole-frame inference only. | See above. |
| **Perspective** | None. | Uniform sensitivity across the frame; far objects are unreachable. |
| **Claim vocabulary** | `ClaimKind` (`src/adapter/contract.rs:30`) already has `SmallObjectBoundaryCrossing`, `ObjectRemovedFromZone`, `PresenceInRestrictedZone`. `ObjectClass::Package` exists in `src/detect/result.rs`. | The **contract is ahead of the pipeline**. Most of what follows produces claims we can already seal — which keeps Invariant VI (no retroactive reinterpretation) out of the way for the first phases. |

And one genuinely good thing we already have that Frigate doesn't:

**The ESP32-S3 camera firmware already contains a real motion detector.**
`firmware/canary/lib/securacv_vision/src/securacv_vision.cpp` decodes to 160×120
grayscale and maintains a **10×8 block grid** with an EMA baseline per block, a
per-block variance estimate, a decayed per-block intensity, a global-illumination
rejection rule (>80% of blocks changed ⇒ lighting, not motion — the same insight as
Frigate's `lightning_threshold`), and an object-removal rule (multiple blocks
reverting to baseline simultaneously). This is a coarse `ImprovedMotionDetector`
already running at the edge, and it already publishes the block-intensity grid (the
"voxel heat grid" the Vision dashboard renders).

That changes the shape of the best Pi 5 design considerably.

---

## 4. Proposed architecture: gate at the edge, region at the hub

The Pi 5's constraint is not FLOPS. Four Cortex-A76 at 2.4 GHz is a lot of CPU. The
constraints are (a) **WiFi airtime** once several ESP32-S3 cameras stream
concurrently, (b) **JPEG decode**, which is pure CPU on every frame, and (c) our own
**per-frame fork**. Frigate's answer to (b)/(c) is motion gating. Ours can be
better, because *our cameras can gate before the frame is ever transmitted.*

Note also that the Pi 5 **dropped the Pi 4's hardware H.264 decoder** (it kept HEVC;
H.264 is software-only now — see
[Raspberry Pi forums](https://forums.raspberrypi.com/viewtopic.php?t=391283) and
[Jeff Geerling's writeup](https://www.jeffgeerling.com/blog/2024/can-raspberry-pi-5-handle-4k/)).
For an RTSP-IP-camera hub that's a real regression. For an **MJPEG ESP32-S3 fleet it
is irrelevant** — we never touch H.264. Our chosen sensor sidesteps the Pi 5's one
notable video weakness, which is a point in favor of this whole direction.

### Hop 0 — the camera decides whether to speak (ESP32-S3)

The Canary already computes the 10×8 changed-block grid. Publish, per frame interval,
a **10-byte bitmask** of changed blocks (plus the existing coarse intensity grid),
and stream JPEG **only when the mask is non-empty and the illumination filter did not
fire**.

- Idle porch at night-time-quiet or midday-still: ~10 bytes/frame instead of ~40 KB.
- The hub's JPEG decoder, motion detector, and fork all run **zero times**.
- This is Frigate's motion gate moved one hop upstream, across the link that is
  actually scarce. Frigate cannot do this; its cameras are dumb RTSP sources.

The privacy story improves too: a camera that transmits nothing when nothing is
happening is a stronger statement than one that transmits and is ignored.

**Two firmware preconditions, both real work — this is not a free reuse of the
existing grid.**

*Precondition A — the mask is not currently computed every frame.* `vision_process()`
(`securacv_vision.cpp`) returns early, **before** `decode_and_downsample` and
`layer2_check` ever run, in several cases: the duty-cycle rest window, the
`process_interval_ms` rate limit (extended to `sustained_backoff_ms` during sustained
activity), peek-active, non-normal thermal state, and — most importantly — when the
**Layer 1 JPEG-size-delta gate** fails with no tamper counter pending
(`securacv_vision.cpp:591-604`). So the block grid today reflects only the frames
Layer 1 already let through. Publishing that mask as the *sole* transmit gate would
silently inherit Layer 1's blind spots: slow or small motion that barely moves the
compressed frame size. Phase 1 must therefore either compute the mask on every
processed frame (bypassing Layer 1 for mask generation and keeping Layer 1 only as a
cheap *pre*-filter for the expensive decode) or transmit conservatively whenever
Layer 1 is uncertain. Decide this deliberately — the duty cycle and rate limit are
battery features and should stay, but they must then be understood as part of the
gate's latency budget, not bypassed by accident.

*Precondition B — an empty mask must not mean silence forever.* The block baseline is
an EMA (`BLOCK_EMA_ALPHA`), so a stationary object is absorbed into the baseline and
its blocks stop reporting as changed. Under a strictly "transmit only when the mask is
non-empty" rule the hub would then receive **no image at all** for the stationary
re-verification promised in §2.4 — and a "package present" state could persist
indefinitely if the package's removal is occluded or the firmware's own object-removal
heuristic misses it. The link protocol therefore needs a **periodic keyframe** (a
frame sent on a slow timer regardless of the mask) and ideally a **hub-requested
frame**, so the Pi can drive `stationary.interval` re-checks on its own schedule
rather than hoping the camera volunteers. Size the keyframe interval against
`stationary.interval`, not against the frame rate.

### Hop 1 — the hub converts the block mask into motion boxes (Pi 5)

The changed-block mask **is** the motion box set, at grid resolution. Union adjacent
changed blocks into rectangles, scale to frame coordinates, and feed them straight
into region clustering. Frigate spends its motion budget rediscovering what our
camera already knows.

**Scale each axis independently — the grid is not a uniform fraction of the frame.**
`VISION_GRID_COLS` is 10 and `VISION_GRID_ROWS` is 8 over a 160×120 decode buffer, so
a block is 16×15 px *in that intermediate buffer* — neither square nor a uniform 1/16
of anything. Map straight from grid cell to full-frame coordinates per axis:

```
x0 = col       * frame_width  / 10        x1 = (col + 1) * frame_width  / 10
y0 = row       * frame_height /  8        y1 = (row + 1) * frame_height /  8
```

Treating the mask as a single 1/16-scale image produces vertically shifted or clipped
regions, which then get handed to region clustering as if they were real motion
extents. Anything that consumes the mask should take the two grid dimensions as
inputs rather than assuming a scalar scale factor.

Keep a hub-side `ImprovedMotionDetector` port as the **second opinion** for two
cases: cameras that can't gate (third-party RTSP via the existing `RtspSource`), and
verification that the edge gate isn't dropping real events. Port it faithfully —
rolling-percentile contrast, 10-frame persistence before background absorption,
calibrating state, lightning threshold. Skipping any of the four re-earns the bug.

### Hop 2 — regions, not frames (Pi 5)

Port `get_cluster_boundary` / `get_cluster_candidates` / `get_cluster_region` and the
5%-of-region-area anti-merge rule. Crop and resize **regions**, never the frame.
Replace tiny-YOLOv2 with a model that has the classes the use cases need — a COCO-80
YOLO at 320×320 covers `person`, `cat`, `dog`, `car`, `bicycle`, `backpack`,
`suitcase`. (COCO has no `package`; see §5.1.)

Add the 8×8 region grid from §2.3 once there is event history to build it from. It is
a pure win with no config surface, which fits our onboarding philosophy exactly.

### Hop 3 — a tracker (Pi 5)

This is the largest genuinely new component and everything downstream needs it. It
does not need to be Norfair-grade: a centroid/IoU tracker with `min_initialized`,
`max_disappeared`, and stationary state covers all four use cases. Frigate keeps
`centroid_tracker.py` around for exactly this reason. Then layer:

- stationary threshold + interval + re-verify-on-motion-intersection,
- per-label stationary policy (`package`/`waste_bin` never flip on IoU alone),
- the NCC + phase-correlation anchor check — ~200 lines and the difference between a
  stable "package present" and an alert storm,
- zone inertia and `loitering_time`.

### Hop 4 — state classifiers for the fixed-geometry questions

A small MobileNetV2-class TFLite/ONNX state classifier on a fixed crop, triggered by
motion overlapping that crop, with Frigate's threshold + 3-attempt/60%-consensus
rule. This is the cheap path to litter box and to porch-specific package presence,
and it composes with `ContactStateChange` in the existing claim vocabulary.

### The per-frame budget this buys

Illustrative, for one 800×600 ESP32-S3 camera at 5 fps on a Pi 5. **These are design
targets to bench against, not measurements** — nothing here has been run yet:

| Scene | Today (whole-frame, hash motion) | Proposed |
|---|---|---|
| Nothing happening | JPEG decode + SHA-256 + fork + 416×416 YOLO, every frame | Nothing. No bytes on the wire, no decode, no fork. |
| One person walking up the path | same as above | 1 decode, 1 fork, 1× 320×320 inference on one region |
| Package sitting on the mat for 8 h | full inference, every frame, forever | 1 inference per `stationary.interval` (~10 s), plus re-verify when motion touches it |
| Cloud shadow crossing the yard | full inference, every frame | 0 — the illumination filter fires at the camera |

The headline is the first row, and it is the row that describes ~99% of a front
door's day.

---

## 5. The four use cases, concretely

### 5.1 Package at the front door

**Detection.** COCO has no `package` class. Three honest options, in order of
increasing effort:

1. **Object-removal-first framing.** Don't detect the package; detect the *delivery*.
   A `person` enters the porch zone and leaves, and the porch's stationary set gained
   an unclassified stationary blob → `SmallObjectBoundaryCrossing` in the porch zone.
   When the blob's blocks revert to baseline → `ObjectRemovedFromZone`. Both claims
   already exist in the contract. This needs no new model at all.
2. **State classification on the mat crop** (§2.6): `mat_empty` / `mat_occupied`.
   Fixed geometry, two classes, CPU-cheap, trainable from the user's own porch.
3. **A `package` class in a custom-trained detector.** Highest fidelity, highest
   effort, and the only one that generalizes across porch layouts.

Recommendation: ship (1) as the first-party behavior, offer (2) as the per-home
upgrade. Note that Frigate's own `package` support leans on their `+` model, not a
stock COCO one — this is genuinely hard, and we should say so in user-facing copy
rather than promise a package detector we haven't trained.

**Stationary policy is non-negotiable here** — see §2.4.

### 5.2 Pet arrived at the back door

COCO `cat` and `dog` are solid daylight classes. The pipeline is: zone at the back
door + inertia 3 + `SmallObjectBoundaryCrossing`. The interesting failure is a pet
that lies down in the zone and becomes stationary, then "reappears" when it shifts —
which the stationary anchor check (§2.4) is precisely designed to suppress.

**Distinguishing *our* pet from a neighbor's is out of scope — permanently, not
pending a spec change.** Frigate does this with object classification (§2.6), and it
would be easy to bolt on once a tracker exists, which is exactly why it needs saying
explicitly here. AGENTS.md non-negotiable #1 forbids adding an identity-inferring
capability and fixes the output vocabulary at `Person | Vehicle | Animal | Package`;
it is "a rejected PR, not a config flag." A per-animal recognizer is an
identity-inferring capability and a `pet-identity` sub-label is a vocabulary widening
past `Animal` — so this is not a candidate for the spec-first path in §7, it is a
thing we don't build. The guarantee is `can't`, not `won't`: the recognizer isn't
written, so there's no setting to disable.

The honest capability is coarse and zone-shaped: *an animal arrived at the back door*.
If a user needs "my dog specifically," that is a collar tag on a different modality
(BLE presence — `src/adapter/ble_presence.rs`), not a camera learning to tell animals
apart.

### 5.3 Litter box

**State classification, not object detection.** Fixed camera, fixed crop, two or
three classes (`empty` / `cat_present`, optionally `cat_departed_recently`). Runs on
CPU in milliseconds, no tracker, no regions, no perspective grid.

Note the existing [`litterbox_witness_demo.md`](litterbox_witness_demo.md) — the
demo framing already exists; this gives it a real detector. The health-signal framing
(visit frequency as a coarse trend, never a per-visit log) fits the 10-minute time
buckets we already coarsen to, and the daily-digest sensor we already publish.

### 5.4 Baseline daylight movement sensing

This is the one that is *already nearly free* and that we currently do badly. The
ESP32 block grid plus the hub's motion detector, with zone inertia and a
`loitering_time`, gives `PresenceInRestrictedZone` with no object detector in the
loop at all. Ship this first: it is the shortest path from "hash-diff noise" to
"trustworthy motion", and it validates the edge-gating link before any model work.

---

## 6. What we must NOT borrow

Frigate is an NVR. Most of its surface area is exactly what our invariants exist to
forbid, and the pipeline learnings must not smuggle any of it in:

| Frigate feature | Why it stays out |
|---|---|
| Recordings, snapshots, thumbnails, Birdseye, restream | Invariant I (No Raw Export). Our region crops must live and die inside the detect call — the `DetectorBackend` audit boundary in `src/detect/backend.rs` already says so. |
| Face recognition, license plate recognition | Invariant II (No Identity). Not a tuning decision; these capabilities must not exist. |
| Object classification used as a *recognizer* — per-animal pet identity, "known vs. stranger" person sub-labels, delivery-carrier identification | Same rule. AGENTS.md non-negotiable #1 forbids identity-inferring capabilities outright and fixes the vocabulary at `Person \| Vehicle \| Animal \| Package`. Object classification is only admissible for coarse, non-identifying attributes; the moment it distinguishes *which* individual, it is out. See §5.2. |
| Semantic search / embeddings, GenAI descriptions | Invariant VII (Non-Queryable). An embedding index over past events is a bulk-search substrate by construction. |
| Full event DB with boxes, trajectories, precise timestamps | Invariant III (Metadata Minimization). We coarsen to 10-minute buckets and drop coordinates at the trust boundary — see the field table in [`frigate_integration.md`](frigate_integration.md). |
| Speed estimation | Tempting and cheap once you have a tracker. It is also a trajectory, and it is the feature most likely to attract exactly the customer we don't want. Leave it out. |

The **region grid** deserves a specific note: it is derived from a history of object
positions, which is a form of retained metadata. It must be stored as *aggregate
statistics per grid cell only* (mean and standard deviation of region size, as
Frigate does — never the underlying box list), and it must live outside the sealed
log as detector tuning state, not as events.

---

## 7. Suggested sequence

Each phase is independently shippable and independently useful.

1. **Edge gating + honest motion.** ESP32 publishes the changed-block bitmask; hub
   stops streaming idle cameras; replace `FrameHashMotion` with a real background
   model for non-gating sources. Deliverable: `PresenceInRestrictedZone` that a user
   trusts. Extends the `PipelineCounters` idea already sketched in
   [`reviews/kernel_frame_trigger_pipeline_plan.md`](reviews/kernel_frame_trigger_pipeline_plan.md)
   with a `frames_gated` counter — the number that proves the gate is working.
2. **Regions + a COCO model.** Cluster boxes into square regions; crop-and-resize per
   region; retire whole-frame resize. Deliverable: `person`/`cat`/`dog` at real
   range.
3. **Tracker + stationary + zone inertia.** Deliverable: `SmallObjectBoundaryCrossing`
   without duplicates, and the first working `ObjectRemovedFromZone`.
4. **Region grid.** Deliverable: far-field detection, no new config.
5. **State classifiers.** Deliverable: litter box, porch-specific package presence.

**Spec-first checkpoints.** Phases 1–3 produce claims that already exist in
`ClaimKind`, so they are implementation work. Anything that introduces a *new* claim
(a distinct "package delivered" versus a generic small-object crossing, a litter-box
visit) is an Invariant VI change and needs
[`spec/event_contract.md`](../spec/event_contract.md) and
[`spec/witness_dictionary.json`](../spec/witness_dictionary.json) updated **before**
the code, per the same rule that governs promoting the Vision coarse signals.

Note the difference between that path and §6: spec-first is for claims that are
*coarse but new*. A capability that infers identity is not on the spec-first path at
all — per AGENTS.md non-negotiable #1 it is simply not built, and no dictionary entry
makes it acceptable.

**Bench before believing.** Every number in §4 is a target, and the sandbox-cost
claim in §3 is an explicit hypothesis. The first deliverable of
phase 1 should be a Pi 5 bench harness reporting frames gated, frames decoded,
inference count, and end-to-end latency per camera — otherwise "jaw-droppingly fast"
is a claim rather than a measurement, and this project does not ship claims it
cannot verify.

---

## 8. Source index

Everything cited above, for the next person who reads this with the Frigate tree
open (commit `f1cc0e4`, 0.18 beta):

| Topic | Frigate file |
|---|---|
| Motion detector | `frigate/motion/improved_motion.py` |
| Motion defaults | `frigate/config/camera/motion.py` |
| Per-camera loop, region assembly, stationary gating | `frigate/video/detect.py` |
| Region clustering, region grid, NMS consolidation | `frigate/util/object.py` |
| Stationary policy + NCC/phase-correlation classifier | `frigate/track/stationary_classifier.py` |
| Detect defaults (`fps: 5`, `min_initialized`, `max_disappeared`) | `frigate/config/camera/detect.py`, `frigate/config/config.py:842-858` |
| Detector process + shared-memory topology | `frigate/object_detection/base.py` |
| Alert vs. detection severity | `frigate/review/maintainer.py`, `frigate/review/types.py` |
| Custom classification (object + state) | `frigate/data_processing/real_time/custom_classification.py`, `docs/docs/configuration/custom_classification/` |
| Zones: inertia, loitering, required zones | `docs/docs/configuration/zones.md` |
| Hailo-8L on Pi 5 timings | `docs/docs/frigate/hardware.md` |

And on our side: `src/detect/backends/motion.rs`, `src/detect/backends/tract.rs`,
`src/detect/result.rs`, `src/adapter/contract.rs`, `src/ingest/esp32.rs`,
`src/bin/witnessd.rs`, `firmware/canary/lib/securacv_vision/src/securacv_vision.cpp`.

---

## 9. Accelerators — the "do we even need one" question

Frigate documents Hailo-8L on the Pi 5 AI Kit at **~11 ms for YOLOv6n** and ~10 ms
for SSD MobileNet v1 (`docs/docs/frigate/hardware.md`). That is the ceiling if we ever
want many cameras at high frame rates.

But the honest read of §4 is that **for the four daylight use cases, a Pi 5 with no
accelerator is likely sufficient**, because the proposed design reduces inference to
"a few 320×320 crops per second, only when something actually moved." Buying a Hailo
to run YOLO on every frame of an empty porch is solving the wrong problem — and the
Hailo also adds a proprietary runtime to the trust boundary, which for us is a cost
Frigate doesn't pay.

Recommendation: build phases 1–3 accelerator-free, bench it, and let the measurement
decide. If the answer is "we need one", the `BackendSelection::Accelerator` path in
[`inference_backends.md`](inference_backends.md) is already specified and already
fails closed — the slot exists.
