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
| RTSP ingestion | ⚠️ Implemented but unverified | `RtspSource` does real decode via GStreamer/FFmpeg (`src/ingest/rtsp.rs`, `rtsp_ffmpeg.rs`), synthetic only for explicit `stub://` URLs. Feature-gated (`rtsp-ffmpeg`/`rtsp-gstreamer`); **not** exercised in CI or by a test — only *file* ingestion (`ingest-file-ffmpeg`/`ingest_run`) has the CI roundtrip. |
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

- The Frigate → Home Assistant MQTT pipeline passes the release gate end-to-end —
  `integrations/ha_frigate_mqtt/verify_pipeline.sh` exits `0` against a live stack (README release gate).
- The "audit boundary vs security boundary" documentation item (Acceptance, below) is still open.
- RTSP ingestion is documented, so under this definition it is **in scope** and must work
  (moved out of "nice to have").
- Firmware must not surface raw MAC addresses or precise GPS over its APIs — the docs and
  `spec/invariants.md` forbid it, so documented behavior must match actual
  (see `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`).

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

**Goal:** One end-to-end tamper-proof path

**v1 choice: Ed25519 signed log** (not encrypted vault)

Rationale: Signed log is easier to demonstrate end-to-end without designing envelope formats and media storage semantics. The vault seal path is wired into `witnessd` (opt-in via `BREAK_GLASS_SEAL_TOKEN`); v1 leads with the signed log for the headline end-to-end demo, with vault sealing available as the optional sealed-evidence path.

**Remaining crypto gaps to close:**
- Device key handling is still seed-derived from config, not hardware-backed or rotated.
- Vault sealing is wired but opt-in/UX-gated; the remaining gap is key management (see the
  device-key item above) and a trustee/seal setup UI — not the encryption itself.

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| B1 | **Done:** Ed25519 log signing in kernel + CLI paths | ✅ |
| B2 | Device key generation + secure storage (replace seed-derived key) | 3-5 days |
| B3 | **Done:** `log_verify` validates Ed25519 signatures | ✅ |
| B4 | **Done:** Tampering demo (`tamper_demo` binary: modify log, verify fails) | ✅ |

**B2 note:** Deferred. Software-only "encrypted at rest" key storage adds
little on an unattended device that must auto-decrypt at boot — the real
bar (hardware-backed keys via TPM/Secure Element) needs hardware to
validate. The prerequisite architectural fix is decoupling the SQLCipher
DB key from the device identity key (`derive_db_encryption_key` currently
derives the DB key from the signing key), which is what would unblock safe
key rotation.

**Total:** ~2-3 weeks

### Stream C: Real Video Ingestion

**Goal:** Process actual video, not synthetic frames

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| C1 | **Done:** File reader (mp4→frames) via ffmpeg `FileSource` | ✅ |
| C2 | **Done:** Pixel format handling (NV12→RGB; ffmpeg swscale to RGB24) | ✅ |
| C3 | **Done:** Timestamp coarsening at capture (`TimeBucket::now_10min`) | ✅ |
| C4 | GStreamer/FFmpeg RTSP source | ✅ (implemented, feature-gated) |

**Roundtrip proven:** the `ingest_run` binary processes a real mp4 end to
end (file → frames → RGB → detection → signed events → verify), exercised
in CI against a committed mp4 fixture. Run it with
`cargo run --features ingest-file-ffmpeg --bin ingest_run -- --video clip.mp4`.

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
- [ ] Documentation states audit boundary vs security boundary
- [ ] RTSP ingestion works end-to-end (documented feature → required under the v1 definition)
- [ ] Frigate → HA MQTT release gate passes (`integrations/ha_frigate_mqtt/verify_pipeline.sh` == 0)
- [ ] Firmware APIs expose no raw MAC / precise GPS (documented invariants hold on-device)

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
