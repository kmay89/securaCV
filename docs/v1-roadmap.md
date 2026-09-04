# SecuraCV v1 Roadmap

## The Two Pillars

1. **Privacy-preserving detection** — events without surveillance
2. **Tamper-evident artifacts** — cryptographic proof of integrity

Both required for v1. CV flexibility without real crypto is just a detection pipeline. Crypto without CV flexibility is academically interesting but not useful.

---

## Current State

> _Refreshed 2026-05-31 against the source tree. The earlier table predated the Stream A/B/C
> work below and was stale (it still said detection was "Only StubDetector", the vault was "not
> wired", and RTSP was "synthetic frames only" — all since superseded). Statuses below cite the
> code that backs them._

| Component | Status | Reality |
|-----------|--------|---------|
| Frame isolation | ✅ Works | Type-level enforcement is real |
| Hash-chained log | ✅ Works | `log_verify` proves integrity |
| Event contract | ✅ Works | Allowlist enforced |
| Break-glass structure | ✅ Works | Policy storage + approval flow |
| Detection | ✅ Works (stub is the default) | `DetectorBackend` trait + `BackendRegistry`; `StubBackend`/`CpuBackend`/`TractBackend` (ONNX via `tract`). Default build registers the motion stub; `TractBackend` is feature-gated (`backend-tract`) and needs a `--model` (tests: `tests/tract_backend.rs`). |
| Signatures | ✅ Works | Ed25519 signatures on log + `log_verify` checks |
| Vault sealing | ✅ Wired (opt-in) | `witnessd` seals buffered frames via `seal_latest_frame()` → `Vault::seal_frame()` with real `VaultCryptoMode::{Classical,Pq,Hybrid}`; gated on a valid `BREAK_GLASS_SEAL_TOKEN`. Remaining work is key management + setup UX, not the encryption. |
| RTSP ingestion | ✅ Works (ffmpeg path, CI-verified) | `RtspSource` does real decode via GStreamer/FFmpeg (`src/ingest/rtsp.rs`, `rtsp_ffmpeg.rs`), synthetic only for explicit `stub://` URLs. Feature-gated (`rtsp-ffmpeg`/`rtsp-gstreamer`). The **ffmpeg** path now has an end-to-end CI roundtrip: the `ingest-rtsp` job serves the committed mp4 fixture over RTSP (MediaMTX + ffmpeg publisher) and `tests/rtsp_e2e.rs` drives the real `RtspSource` through decode → detection → signed events → verify. The GStreamer backend shares the same `RtspSource` interface but is not separately exercised in CI. |
| Sandboxing | ⚠️ Partial (by design) | Optional seccomp sandbox: adapters opt in via `with_sandbox()` (frigate/mqtt/webhook/ble-presence); parsing then runs inside `parse_in_sandbox()` (`src/adapter/sandbox.rs`). Detection backends remain a trusted/*audited* boundary by design (see `AGENTS.md`). |

---

## v1 Definition

v1 means **everything documented works end-to-end** — not a reduced "minimally credible" subset.
This matches `CHANGELOG.md` [1.0.0] and is the single canonical definition for the project.
Concretely, to tag v1:

- Every feature described in the README/docs runs end-to-end on a first-try install.
- `cargo test` passes cleanly.
- At least one real CV backend, one real crypto/tamper path, and one real video source — **and any
  other capability the docs claim** — actually works end-to-end, not merely compiles.
- Documentation matches the code: honest about what is *enforced* vs *auditable*, with no claim
  that outruns the implementation.

This is a **higher** bar than the earlier "minimally credible" framing, so adopting it makes the
remaining gaps explicit rather than waiving them. **Open blockers to this bar (must close before
tagging v1):**

- ~~The Frigate → Home Assistant MQTT pipeline passes the release gate end-to-end —
  `integrations/ha_frigate_mqtt/verify_pipeline.sh` exits `0` against a live stack.~~ **Automated in
  CI:** `cargo test --test frigate_mqtt_e2e` drives a `frigate/events` payload → sealed log → real
  `log_verify` (encrypted DB), and the `frigate-mqtt-e2e` job runs the real `frigate_bridge` binary
  ingesting from a live mosquitto broker (`ci_smoke.sh`). The full 4-container operator stack with
  real Frigate ML detection (`verify_pipeline.sh`, now corrected) stays a manual smoke check, since
  ML detection on a fixture isn't deterministic.
- ~~The "audit boundary vs security boundary" documentation item (Acceptance, below) is still open.~~
  **Closed:** stated authoritatively in [`docs/security/THREAT_MODEL.md`](security/THREAT_MODEL.md)
  under *Trust Boundaries → Audit Boundary vs Security Boundary*.
- ~~RTSP ingestion is documented, so under this definition it is **in scope** and must work
  (moved out of "nice to have").~~ **Closed:** the ffmpeg RTSP path now has an end-to-end CI
  roundtrip (`ingest-rtsp` job + `tests/rtsp_e2e.rs`).
- ~~Firmware must not surface raw MAC addresses or precise GPS over its APIs — the docs and
  `spec/invariants.md` forbid it, so documented behavior must match actual.~~ **Closed:** every
  firmware tree now routes operator-facing identity through the shared salted `device_pseudonym`
  (`firmware/common/identity/`) and GPS through `gps_coarsen_deg` (`firmware/common/gnss/`);
  `regression_check.sh` hard-fails on raw MAC or un-coarsened lat/lon in *any* tree
  (see `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`).
- **Still open:** all of the above gates are green **in CI**, but the v1 tag also waits on
  **on-device hardware validation** (the kernel/bridge pipeline and firmware exercised on real
  ESP32 devices, not just CI). The driver sheet that sequences the whole gate — tracks,
  sign-off matrix, tag step — is
  [`docs/V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md); the detailed per-track
  procedures, pass/fail criteria, and required artifacts are in
  [`docs/hardware/v1_bench_validation_runbook.md`](hardware/v1_bench_validation_runbook.md)
  (Track A: single canary → HA verified-✓; Track B: kernel pipeline smoke; Track C: 2–3 board
  mesh/chirp fleet). Until that's done the README status stays `v1-rc`, the
  `CHANGELOG.md` `[1.0.0]` entry stays `Unreleased`, and no tag is cut.

---

## Work Streams (Parallel)

### Stream A: CV Backend Abstraction

**Goal:** Decouple detection from `StubDetector`

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| A1 | **Done:** `DetectorBackend` trait + `BackendRegistry` | ✅ |
| A2 | **Done:** Port `StubBackend` (+ bonus `CpuBackend`) | ✅ |
| A3 | **Done:** `TractBackend` loads model + runs forward pass | ✅ |
| A4 | **Done:** `TractBackend` produces correct boxes for test model | ✅ |
| A5 | **Done:** Integration with `InferenceView` + registry routing | ✅ |

> **Default-build caveat:** A1–A5 mean the `TractBackend` (ONNX) *exists and works when
> compiled*. The default build does **not** enable it — `detect.backend=auto` resolves to a
> frame-difference **motion** backend (`cpu`/`stub`), so a stock `witnessd` reports motion
> presence, not classified objects. Real object detection requires a `--features backend-tract`
> build **and** setting both `detect.backend=tract` and `detect.tract_model` (each alone is
> insufficient). `witnessd` now logs a `WARN` at startup whenever a
> motion-only backend is active so this is never silent. (In the primary Frigate-bridge
> deployment, object detection is Frigate's job; this caveat is about the direct-ingest path.)

**Total:** ~2-3 weeks

### Stream B: Crypto Hardening

**Goal:** One end-to-end tamper-evident path

**v1 choice: Ed25519 signed log** (not encrypted vault)

Rationale: Signed log is easier to demonstrate end-to-end without designing envelope formats and media storage semantics. The vault seal path is wired into `witnessd` (opt-in via `BREAK_GLASS_SEAL_TOKEN`); v1 leads with the signed log for the headline end-to-end demo, with vault sealing available as the optional sealed-evidence path.

**Remaining crypto gaps to close:**
- Device signing key is still seed-derived from config, not hardware-backed. (The DB key is
  no longer coupled to it — see the B2 note below.)
- Vault sealing is wired but opt-in/UX-gated; the remaining gap is key management (see the
  device-key item above) and a trustee/seal setup UI — not the encryption itself.

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| B1 | **Done:** Ed25519 log signing in kernel + CLI paths | ✅ |
| B2 | Device key generation + secure storage (replace seed-derived key) | 3-5 days |
| B3 | **Done:** `log_verify` validates Ed25519 signatures | ✅ |
| B4 | **Done:** Tampering demo (`tamper_demo` binary: modify log, verify fails) | ✅ |

**B2 note:** The **storage-layer prerequisite** — decoupling the SQLCipher DB key from the device
signing key — is now **done**. Set `SECURACV_DB_KEY_SEED` to an independent secret and the kernel
derives the DB key from it (`resolve_db_encryption_key`) instead of the Ed25519 signing key, so the
DB key no longer pins the identity; `rekey_database_file()` rotates the DB key itself in place. See
[`docs/db_key_rotation.md`](db_key_rotation.md). **Still open** before signing-key rotation
works end-to-end: `Kernel::open` pins the device public key in `device_metadata` and rejects a
mismatching identity, so rotation also needs identity-rotation support (record the new key, keep the
log verifiable across the boundary). Beyond that, the higher bar is **hardware-backed keys**
(TPM/Secure Element), which needs hardware to validate.

**Total:** ~2-3 weeks

### Stream C: Real Video Ingestion

**Goal:** Process actual video, not synthetic frames

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| C1 | **Done:** File reader (mp4→frames) via ffmpeg `FileSource` | ✅ |
| C2 | **Done:** Pixel format handling (NV12→RGB; ffmpeg swscale to RGB24) | ✅ |
| C3 | **Done:** Timestamp coarsening at capture (`TimeBucket::now_10min`) | ✅ |
| C4 | GStreamer/FFmpeg RTSP source | ✅ (ffmpeg path CI-verified end-to-end; gstreamer shares the interface) |

**Roundtrip proven:** the `ingest_run` binary processes a real mp4 end to
end (file → frames → RGB → detection → signed events → verify), exercised
in CI against a committed mp4 fixture. Run it with
`cargo run --features ingest-file-ffmpeg --bin ingest_run -- --video clip.mp4`.

**RTSP roundtrip proven:** `tests/rtsp_e2e.rs` (the `ingest-rtsp` CI job)
serves the same fixture over RTSP via MediaMTX + an ffmpeg publisher and drives
the real `RtspSource` (ffmpeg backend) through the identical
decode → detection → signed events → verify path. Run it locally with
`MEDIAMTX_BIN=/path/to/mediamtx SECURACV_RTSP_E2E=1 cargo test --features rtsp-ffmpeg --test rtsp_e2e`.

**Total:** ~2-3 weeks

---

## Recommended Order

```

Week 1-2:  A1, A2 (backend trait + stub)
B1 (Ed25519 signing)

Week 3-4:  A3 (tract loads model)
B2, B3 (device key + verify)

Week 5-6:  A4 (tract produces correct output)
C1, C2 (file ingestion)

Week 7-8:  A5 (InferenceView integration)
C3 (timestamp coarsening)
B4 (tampering demo)
Integration testing

```

---

## What v1 Does NOT Include

- **WASM sandboxing** — backends are trusted, must be audited
- **GPU acceleration** — `ort` backend is v1.1
- **Firmware OTA updates** — *since closed*: the engine was promoted to
  [`firmware/common/ota/`](../firmware/common/ota/) and now ships signed
  pull-updates (HTTPS manifest + Ed25519 release signature + SHA-256 + A/B
  rollback) across the active firmware trees — see
  [`docs/firmware_ota.md`](firmware_ota.md) and the
  [Feature-Parity Dashboard](../firmware/FEATURES.md), which still marks
  canary-sense partial pending ESP32-C6 bench validation. That bench pass
  rides with the hardware-validation blocker above.
- **Encrypted-vault UX / key management** — sealing is wired (opt-in); the trustee/seal setup UI
  and hardware-backed keys are v1.1
- **Real-time performance guarantees** — benchmark, don't promise
- **Remote attestation** — future

(RTSP was previously listed here as a stretch goal. Under the canonical "everything documented
works end-to-end" definition it is a documented feature, so it is now **in scope** for v1 — see
the v1 Definition and Acceptance Criteria.)

---

## Acceptance Criteria

### Must have:
- [x] `cargo test` passes
- [x] `StubBackend` works for motion detection
- [x] `TractBackend` loads a model and runs forward pass
- [x] `TractBackend` produces correct bounding boxes for a known test model
- [x] Log entries are Ed25519 signed
- [x] `log_verify` validates signatures and catches tampering
- [x] Can process video from file
- [x] Documentation states audit boundary vs security boundary (`docs/security/THREAT_MODEL.md` → *Trust Boundaries*)
- [x] RTSP ingestion works end-to-end (ffmpeg path; `ingest-rtsp` CI job + `tests/rtsp_e2e.rs`)
- [x] Frigate → HA MQTT pipeline gated in CI — `frigate_mqtt_e2e` (event → sealed log → `log_verify`)
  + `frigate-mqtt-e2e` job (real bridge ingesting from a live mosquitto broker). The 4-container
  operator stack (`verify_pipeline.sh`) remains a manual smoke check.
- [x] Firmware APIs expose no raw MAC / precise GPS (every tree routes identity through the salted
  `device_pseudonym` and GPS through `gps_coarsen_deg`; `regression_check.sh` hard-fails on either)

### Nice to have:
- [ ] Performance benchmarks

### Explicitly out of scope:
- [ ] GPU acceleration
- [ ] WASM sandboxing
- [ ] Face/plate detection (forbidden by design)
- [ ] Encrypted-vault setup UX + hardware-backed keys (v1.1) — sealing itself is already wired

---

## Success Metrics

v1 is successful if:

1. `witnessd` runs with file input and tract detection
2. Events are logged with Ed25519 signatures
3. `log_verify --tamper-test` proves tampering is detectable
4. External auditor can verify privacy claims by reading code
5. Documentation doesn't overclaim
