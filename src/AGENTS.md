# src/ — the brief for the kernel

Loads when you touch a file here; the root `AGENTS.md` still applies. The
normative text is `spec/invariants.md`; this is what it means in this code.

## The invariants, in code

| Invariant | What the code keeps |
|---|---|
| I. No raw export | `RawFrame.data` is private: no getter, no `Clone`, no `AsRef<[u8]>`. The only way out is `export_for_vault()` behind a `BreakGlassToken` (n-of-m trustees). |
| II. No identity substrate | `ObjectClass` is `Person`, `Vehicle`, `Animal`, `Package`, `Unknown` — `#[non_exhaustive]`, never `Face` or `LicensePlate`. No field on `DetectionResult` that could hold an embedding, a plate string, a re-ID vector or a demographic estimate. |
| III. Metadata minimization | Timestamps coarsen to 10-minute buckets; zone ids are local; correlation tokens are single-use. |
| IV. Local ownership | Logs stay local. No remote indexing, no telemetry. |
| V. Break-glass by quorum | Raw media needs N-of-M trustee approval (`src/break_glass/`). |
| VI. No retroactive expansion | `ReprocessGuard` checks the ruleset hash; a new ruleset cannot reprocess old data. |
| VII. Non-queryable | No bulk search, no identity selectors, in any API. |

## Detection backends

`DetectorBackend` is an **audit boundary** backed by process isolation, not a
proof. In `witnessd`, `detect()` runs in a forked, seccomp-restricted child
(`module_runtime::execute_sandboxed`, `src/module_runtime/sandbox.rs`) that
denies the filesystem, the network, `execve`, `ptrace`, `process_vm_*`,
signals, SysV IPC and process creation, runs under a CPU limit and a reply
deadline, and whose only output — the candidate list — is rebuilt by the
parent from what it already knows (`admit_candidates`: declared zone ids,
allowed types, the frame's bucket, a parent-minted token). The child is a
fork of a parent that holds the signing key; a module declares its zone ids
(`Module::declared_zone_ids`) or emits nothing. The sandbox is a denylist and
assumes an uncompromised host (`docs/root_paradox.md`): write "must be
audited", never "cryptographically enforces".

- Capabilities in v1: `Motion`, `ObjectDetection`, `Classification`. No
  segmentation, pose, depth or optical flow.
- Forbidden, ever: face recognition or embedding, license-plate OCR, person
  re-identification, demographic estimation, audio transcription.
- Feature-gate every backend that is not pure Rust and dependency-minimal:
  `StubBackend` is ungated, `TractBackend` sits behind `backend-tract`, and a
  backend with native dependencies gets a feature of its own.
- Before merging a backend: `detect()` keeps no pixels past the call, does no
  network, disk or telemetry, computes nothing identity-linked, and its
  dependencies were read for the same. `detect` takes `&mut self` (internal
  buffers), not `&self`.
- Cross-frame state crosses the sandbox only through `export_state` /
  `import_state`.

## Dependencies

Always fine: `anyhow`, `thiserror`, `sha2`, `ed25519-dalek`, `serde`,
`serde_json`, `rusqlite`. Feature-gated and audited: `tract-onnx`, `ort`,
`gstreamer`. Never: anything that phones home, carries telemetry, or exposes a
face-recognition API. `deny.toml` and `scripts/lint_feature_flags.sh` keep
the feature list honest.

## Style and checks

- `anyhow::Result` for errors; `#[non_exhaustive]` on public enums;
  `#[derive(Debug, Clone, PartialEq)]` on data types; a doc comment on every
  public item.
- `cargo test`, `cargo clippy` (no warnings), `cargo doc` (no warnings),
  `cargo test --no-default-features`. Integration tests are `tests/*.rs`; the
  fuzz targets are `fuzz/`.
- Binaries are the `[[bin]]` entries in `Cargo.toml` (`src/bin/`): `witnessd`
  is the daemon, `log_verify` checks a log. `src/lib.rs` (326 KB) and
  `src/api/mod.rs` (154 KB) are read by range, never whole.

## Where things are

`src/frame.rs` — `RawFrame`, `InferenceView`, `FrameBuffer`. `src/detect/` —
`backend.rs` (the trait), `result.rs`, `registry.rs`, `backends/`.
`src/break_glass/` — quorum and the vault path. `src/module_runtime/` — the
sandbox. `spec/event_contract.md`, `spec/threat_model.md` — the contracts.
