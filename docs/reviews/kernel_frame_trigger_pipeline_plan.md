# Kernel Frame + Event Trigger Pipeline Review & Plan

## Objective
Verify whether camera frames are reaching the witness-kernel processing path during event trigger flow, identify existing trigger mechanisms, and define a pragmatic implementation plan for next iterations.

## Current Pipeline (as implemented)

1. `witnessd` configures an ingest source and connects it (`IngestSource::new`, `source.connect`).
2. Main loop reads frames from ingest via `source.next_frame()`.
3. Frames are pushed into `FrameBuffer` (`frame_buffer.push(frame)`).
4. Latest frame is converted to `InferenceView` (`frame_ref.inference_view()`).
5. Module runtime calls `execute_sandboxed(... module.process ...)`.
6. `ZoneCrossingModule` runs detector backend through `InferenceView::run_detector`.
7. Module emits `CandidateEvent` if motion/object result passes threshold.
8. Kernel validates and writes event via `append_event_checked`.

This confirms a complete frame-to-event path exists in-process today.

## Event Triggering Methods Present Today

### 1) In-kernel detector-triggered events (primary)
- Trigger source: detector result from backend (`stub`, `cpu`, optional `tract`).
- Event production: `ZoneCrossingModule::process` emits `BoundaryCrossingObjectSmall/Large` candidates.
- Commit path: `Kernel::append_event_checked` contract enforcement and sealed log append.

### 2) Transport bridge-triggered events
- `frigate_bridge` maps external detection labels to `EventType` and appends candidate events to kernel.
- This is an integration-side trigger path (event ingress), not raw frame ingress.

### 3) API/bridge ecosystem around events
- Event API server and MQTT bridge publish/consume event artifacts, but they are downstream of trigger generation.

## What "camera frames sent properly" means here
For this codebase, correct behavior means:
- Ingest source produces `RawFrame` continuously.
- Frame buffer receives frames (bounded ring, TTL/capacity behavior).
- At least one backend is registered and default-selected.
- Module runtime returns candidates when detector indicates motion/object.
- Kernel accepts candidates and appends events (non-rejected).

## Gaps / Risks Identified

1. **No explicit per-frame tracing counters at each hop**
   - Health logs report source frame counts, but there is no end-to-end pipeline telemetry tuple like:
     `captured -> buffered -> inferred -> candidates -> appended -> rejected`.

2. **Trigger semantics are backend-dependent**
   - CPU backend currently treats any frame hash delta as motion. This is deterministic but can be noisy.

3. **Bridges can inject events without local frame coupling**
   - Frigate bridge event insertion is intentional, but operators should distinguish local-frame-derived events from externally asserted events.

## Recommended Structure (best next design)

### A. Add a minimal `PipelineCounters` struct in `witnessd`
Track monotonic counters:
- `frames_captured`
- `frames_buffered`
- `inference_attempts`
- `inference_errors`
- `candidate_events`
- `events_appended`
- `events_rejected`

Emit every 5s with ingest health log for easy operational verification.

### B. Add trigger provenance to event metadata (non-identity)
Attach a lightweight source marker in module payload or kernel metadata:
- `trigger_source = "local_detector" | "bridge_frigate" | "manual"`

This preserves invariant compliance while improving auditability.

### C. Define an acceptance test matrix for frame-to-event flow
1. File source + stub backend -> deterministic event generation.
2. File source + cpu backend -> motion delta event generation.
3. RTSP source smoke test -> non-zero captured frames and non-zero inference attempts.
4. Frigate bridge -> events appended with `trigger_source=bridge_frigate`.

### D. Keep privacy invariant boundaries explicit
- Continue passing only `InferenceView` to modules.
- Keep backend audit-boundary docs front-and-center in runtime and backend modules.

## Immediate Execution Plan

1. Implement `PipelineCounters` + periodic structured log line in `witnessd`.
2. Add provenance metadata for appended events where trigger path is known.
3. Add/extend integration tests validating the counter progression and trigger source tagging.
4. Add operator runbook snippet (`docs/manual_test_plan_mqtt.md`) to verify frame-trigger-path in deployment.

## Success Criteria
- Operator can prove, from logs alone, that frames are entering inference and resulting in appended events.
- Event records can be filtered by trigger provenance without exposing raw media.
- No privacy invariants are weakened (no raw export, no identity substrate).
