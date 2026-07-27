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

**`DetectorBackend` is an audit boundary backed by a process-isolation sandbox.**

The trait is an API contract that still must be manually audited for what the kernel can't
enforce in-process:
- Cloning/caching pixel data internally
- Computing identity-linked data

But in the production pipeline `detect()` runs inside a forked, seccomp-restricted child process
(`module_runtime::execute_sandboxed`, used by `witnessd`; see `src/module_runtime/sandbox.rs`).
The child denies the filesystem, network, `execve`, `ptrace`, and `process_vm_*` syscalls, so a
malicious/buggy backend **cannot**:
- Make network calls
- Write to disk
- Shell out (`execve`) or read the parent's key material (`process_vm_readv` / `ptrace`)

Cross-frame state (e.g. motion's previous-frame hash) survives the boundary via
`export_state`/`import_state`. The sandbox is a syscall **denylist** (defense in depth), not an
airtight allowlist, and — like every kernel guarantee — assumes an uncompromised host
(`docs/root_paradox.md`). WASM sandboxing remains a future hardening option for an allowlist model.

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

## Release & Packaging (all app targets)

Before touching **any** app build/release workflow — the Flasher, the Lab,
or the **iPhone / iPad / tvOS / Mac** targets — read
[`.github/RELEASE_LESSONS.md`](.github/RELEASE_LESSONS.md). It's the canonical,
growable home for build/release lessons (each a real failure paid for once),
with principles that hold on every platform: dereference symlinks when copying
a payload into a bundle (`cp -RL`), pin-or-log every upstream ref, prove the
bundle with the build-only/`dry_run` path before publishing, and verify
bundled resources exist in the copy step. When you fix a release/packaging
bug, **append a dated entry there** (symptom → cause → fix → applies-to) and
generalize it to the other targets — don't fix only the one that broke.

To *ship* something, or to work out why something didn't ship, start from
[`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md) — every button, when to
press it, when not to, and the three failures that have cost real time (no OTA
signing key; a flasher pinned to a release nobody cut; an app version that was
already published, where "publishing" silently overwrites instead of releasing).
The default is **Actions → "Update everything (only what needs it)"**, whose
decision engine is unit-tested in `.github/scripts/test_release_plan.py` and
driven by the catalog in `.github/release-targets.yml` — **add a new target
there, not in workflow YAML.**

Two things that are easy to half-fix, so check both sides:
- **Two flashers, two frontends.** `canary-local/assets/` (browser) and
  `desktop/src/` (desktop app) share no UI code — a user-facing diagnostic
  belongs in both.
- **Three version files per app.** `tauri.conf.json` (names the release tag),
  `package.json`, and `Cargo.toml` (what the app reports to the user).
  `desktop/scripts/check_app_versions.py` holds them together.

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

---

## Beacon Channel Invariants (v0.2 — must be enforced)

The Beacon channel (`spec/beacon_channel_v0.md`) is the project's
harm-reduction broadcast layer. It carries life-safety advisories with
smoke-detector-grade reliability requirements. Any agent modifying Beacon
firmware (`firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`)
or the Chirp v0.2 channel (`chirp_channel.{h,cpp}`) MUST observe:

1. **No automatic origination.** Beacons are originated only on explicit
   user action. Sensors (CSI, motion, audio) may prompt a human; sensors
   must not originate.
2. **Two-pubkey rule.** Every Beacon ALERT/UPDATE/CANCEL frame requires two
   distinct Ed25519 signatures from two distinct device pubkeys, both in
   the local beacon set with `trust_level != REVOKED`. Do not introduce a
   "trusted device skips co-sign" shortcut.

   **Solo-degraded exception (spec §6.2):** the single-device origination
   path is permitted ONLY when the physical BOOT button on the device is
   held at the moment of origination (real-time GPIO check), the frame
   carries `BCN_FLAG_SOLO_ORIGIN` in its header, and `certainty` is
   `Observed`. Receivers MUST verify all three invariants before
   accepting; the spec promises this property so the UI can downweight
   solo frames a notch. Do NOT add a software-only path that bypasses
   the BOOT GPIO check.
3. **No red, no reserved emergency-broadcast tones, no reserved phrasing.**
   See `spec/beacon_cap_gateway_v0.md` §4. The
   `scripts/lint_no_impersonation.sh` CI lint enforces this; do not bypass.
4. **No PII on the wire.** No descriptions of people, no license plates, no
   GPS, no MAC addresses, no `opera_id`, no household identifiers.
   Templates only.
5. **`scope = BCN_SCOPE_PRIVATE` always.** Never `Public`, never
   `Restricted`. Lint-enforced.
6. **No restoration of `TPL_AUTH_FEDERAL_PRESENCE`** or any
   "authority/government/federal presence" template in Chirp or Beacon.
7. **No persistence of Chirp session keys.** Session keys are ephemeral by
   design — that's the privacy firewall between Chirp and Opera.
8. **NVS writes of `opera_secret` and Beacon `beacon_set` require flash
   encryption.** Both code paths refuse on FE-off devices. Do not weaken.
9. **The Beacon audit log is append-only and chain-hashed.** Do not add a
   "rotate" or "delete" operation; export-only.
10. **Drills (`msgType = Exercise`) are wire-format-distinct from real
    alerts.** Do not merge their counters or rate-limit buckets.
11. **Tamper-flagged neighbors auto-revoke (v0.5).** When a paired
    Beacon-set member sends an Opera tamper alert (`MSG_TAMPER_ALERT`),
    `beacon_channel::on_peer_tampered(device_pubkey)` automatically
    transitions that entry to `trust_level = REVOKED`. Do not introduce
    a manual-only path or add a "trust them anyway, fail loud later"
    shortcut. Recovery is operational: physically inspect the device,
    then re-pair through the standard flow.
12. **Audible self-test is NFPA-72 §14 cadence (v0.5).** The
    `PATTERN_SELFTEST_OK` chirp plays monthly during waking hours
    (06:00–22:00 local, wall clock synced) only. Do not increase the
    cadence below monthly and do not lift the night-mode suppression —
    a 3 AM beep is a worse failure mode than a missing chirp.

Modifying any of the above requires updating
`docs/audit/mesh_and_chirp_audit_v1.md` and `docs/security/THREAT_MODEL.md` in the same
PR.
