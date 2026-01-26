# AGENTS.md — Witness-Kernel Development Guide

## Project Identity

SecuraCV witness-kernel: Privacy-preserving video event system. Outputs semantic events ("motion in zone A") without exposing raw video or identity data.

**Core value proposition:** Tamper-evident, privacy-preserving witness artifacts.

---

## Architecture Overview

```

┌─────────────────────────────────────────────────────────────┐
│                     WITNESS-KERNEL                          │
├─────────────────────────────────────────────────────────────┤
│  Video Source ──► RawFrame ──► InferenceView ──► Backend   │
│      (private data)    (restricted view)    (audit boundary)│
│                                                  │          │
│                                       DetectionResult       │
│                                       (no identity data)    │
│                                                  │          │
│                                       CandidateEvent        │
│                                                  │          │
│                                       Sealed Log            │
│                                       (Ed25519 signed)      │
└─────────────────────────────────────────────────────────────┘

````

---

## Privacy Invariants (Non-Negotiable)

### Invariant I: No Raw Export
`RawFrame.data` is private. No public getter, no `Clone`, no `AsRef<[u8]>`.
Only path to raw bytes: `export_for_vault()` with `BreakGlassToken`.

### Invariant II: No Identity Substrate  
No face embeddings, plate strings, person re-ID vectors, demographic estimates.
`ObjectClass` enum: `Person`, `Vehicle`, `Animal`, `Package` — NOT `Face`, `LicensePlate`.

### Invariant III: Metadata Minimization
Timestamps coarsened to 10-minute buckets. Zone IDs are local only. Correlation tokens are single-use.

### Invariant IV: Local Ownership
All logs stored locally. No remote indexing. No telemetry.

### Invariant V: Break-Glass by Quorum
Raw media access requires N-of-M trustee approval.

### Invariant VI: No Retroactive Expansion
`ReprocessGuard` checks ruleset hash. New rulesets cannot reprocess old data.

### Invariant VII: Non-Queryable
No bulk search. No identity selectors.

---

## Detection Backend Guidelines

### ⚠️ CRITICAL: Security Model

**`DetectorBackend` is an AUDIT BOUNDARY, not a security boundary.**

The trait defines an API contract. It does NOT prevent backends from:
- Cloning/caching pixel data internally
- Making network calls
- Writing to disk
- Computing identity-linked data

**Each backend must be manually audited.** True isolation requires WASM sandboxing or process isolation (not yet implemented).

### Backend Feature-Gate Rule

**Backends must be feature-gated unless they are pure-Rust and dependency-minimal.**

- `StubBackend` — no feature gate (pure Rust, no deps beyond sha2)
- `TractBackend` — `#[cfg(feature = "backend-tract")]`
- `OnnxBackend` — `#[cfg(feature = "backend-onnx")]`
- Any backend with native deps — must be feature-gated

### Allowed Capabilities (v1)

```rust
pub enum DetectionCapability {
    Motion,           // Frame differencing
    ObjectDetection,  // Bounding boxes
    Classification,   // Person/vehicle/animal
}
````

That's it for v1. No segmentation, pose, depth, optical flow.

### Forbidden Capabilities (NEVER implement)

* `FaceRecognition` / `FaceEmbedding`
* `LicensePlateOCR`
* `PersonReidentification`
* `DemographicEstimation` (age, gender, race)
* `AudioTranscription`

### Backend Audit Checklist

Before merging ANY new backend:

* [ ] `detect()` does not store pixels beyond the call
* [ ] No network operations (no HTTP, no sockets, no DNS)
* [ ] No disk writes (no file I/O, no temp files)
* [ ] No identity-linked computations
* [ ] No telemetry or analytics
* [ ] Dependencies audited for the above
* [ ] Feature-gated if has native dependencies

---

## Code Style

```rust
// Use anyhow for errors
use anyhow::{anyhow, Result};

// Non-exhaustive enums
#[non_exhaustive]
pub enum ObjectClass { ... }

// Derive standard traits
#[derive(Debug, Clone, PartialEq)]
pub struct Detection { ... }

// Document public APIs
/// Runs detection on a frame.
pub fn detect(&mut self, pixels: &[u8]) -> Result<DetectionResult>
```

---

## Testing Requirements

### Every PR must pass:

* `cargo test`
* `cargo clippy` (no warnings)
* `cargo doc` (no warnings)

### Feature flag testing:

* `cargo test --no-default-features`
* `cargo test --features backend-tract`

---

## Common Pitfalls

### DON'T: Add `Clone` to `RawFrame`

```rust
// BAD
#[derive(Clone)]
pub struct RawFrame { ... }
```

### DON'T: Add identity fields to `DetectionResult`

```rust
// BAD
pub struct DetectionResult {
    pub face_embedding: Vec<f32>,  // FORBIDDEN
}
```

### DON'T: Oversell security properties

```rust
// BAD
/// Cryptographically enforces privacy at the type level

// GOOD
/// Defines an audit boundary. Implementations must be manually audited.
```

### DON'T: Promise performance without benchmarks

```rust
// BAD
/// Runs YOLOv8 at 30fps

// GOOD
/// Performance varies by hardware. Benchmark before deployment.
```

### DON'T: Use `&self` when backend needs mutable state

```rust
// BAD - forces Mutex everywhere
fn detect(&self, ...) -> Result<DetectionResult>;

// GOOD - allows internal buffers
fn detect(&mut self, ...) -> Result<DetectionResult>;
```

---

## File Locations

### Core kernel

* `src/lib.rs` — main kernel, types, event handling
* `src/frame.rs` — `RawFrame`, `InferenceView`, `FrameBuffer`

### Detection

* `src/detect/mod.rs` — module exports
* `src/detect/backend.rs` — `DetectorBackend` trait
* `src/detect/result.rs` — `DetectionResult`, `Detection`
* `src/detect/registry.rs` — `BackendRegistry`
* `src/detect/backends/` — backend implementations

### Binaries

* `src/bin/witnessd.rs` — main daemon
* `src/bin/log_verify.rs` — log integrity verification

### Specs

* `spec/invariants.md` — canonical invariant definitions
* `spec/event_contract.md` — event type contracts
* `spec/threat_model.md` — threat model

---

## Dependencies Policy

### Always allowed

* `anyhow`, `thiserror` — error handling
* `sha2`, `ed25519-dalek` — cryptography
* `serde`, `serde_json` — serialization
* `rusqlite` — local storage

### Feature-gated (audit required)

* `tract-onnx` — ONNX inference
* `ort` — ONNX Runtime (native)
* `gstreamer` — video ingestion

### Forbidden

* Anything that phones home
* Anything with face recognition APIs
* Anything with telemetry

---

## Commit Message Format

```
<type>(<scope>): <description>
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `chore`

Examples:

```
feat(detect): add BackendRegistry for runtime backend selection
fix(frame): prevent InferenceView from leaking raw bytes
docs(agents): clarify audit boundary vs security boundary
```

````
