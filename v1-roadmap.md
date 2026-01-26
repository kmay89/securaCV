# SecuraCV v1 Roadmap

## The Two Pillars

1. **Privacy-preserving detection** — events without surveillance
2. **Tamper-evident artifacts** — cryptographic proof of integrity

Both required for v1. CV flexibility without real crypto is just a detection pipeline. Crypto without CV flexibility is academically interesting but not useful.

---

## Current State

| Component | Status | Reality |
|-----------|--------|---------|
| Frame isolation | ✅ Works | Type-level enforcement is real |
| Hash-chained log | ✅ Works | `log_verify` proves integrity |
| Event contract | ✅ Works | Allowlist enforced |
| Break-glass structure | ✅ Works | Policy storage + approval flow |
| Detection | ⚠️ Hardcoded | Only `StubDetector`, no model flexibility |
| Signatures | ⚠️ Placeholder | HMAC, not Ed25519 |
| Vault encryption | ⚠️ Placeholder | Structure exists, not wired |
| RTSP ingestion | ⚠️ Stub | Synthetic frames only |
| Backend sandboxing | ❌ Missing | Backends run with full privileges |

---

## v1 Definition

v1 is "minimally credible," not feature complete:

- At least one real CV backend that can detect objects
- At least one real crypto path that proves tamper detection
- At least one real video source (RTSP or file)
- Documentation that's honest about what's enforced vs auditable

---

## Work Streams (Parallel)

### Stream A: CV Backend Abstraction

**Goal:** Decouple detection from `StubDetector`

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| A1 | `DetectorBackend` trait + `BackendRegistry` | 3-5 days |
| A2 | Port `StubBackend` | 1-2 days |
| A3 | `TractBackend` loads model + runs forward pass | 3-5 days |
| A4 | `TractBackend` produces correct boxes for test model | 3-5 days |
| A5 | Integration with `InferenceView` | 2-3 days |

**Total:** ~2-3 weeks

### Stream B: Crypto Hardening

**Goal:** One end-to-end tamper-proof path

**v1 choice: Ed25519 signed log** (not encrypted vault)

Rationale: Signed log is easier to demonstrate end-to-end without designing envelope formats and media storage semantics. Vault structure remains present but inactive for v1.

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| B1 | Replace HMAC with Ed25519 for log signing | 3-5 days |
| B2 | Device key generation + secure storage | 3-5 days |
| B3 | `log_verify` validates Ed25519 signatures | 2-3 days |
| B4 | Tampering demo (modify log, verify fails) | 1-2 days |

**Total:** ~2-3 weeks

### Stream C: Real Video Ingestion

**Goal:** Process actual video, not synthetic frames

| Step | Deliverable | Est. Effort |
|------|-------------|-------------|
| C1 | File reader (mp4→frames) as test harness | 3-5 days |
| C2 | Pixel format handling (NV12→RGB) | 2-3 days |
| C3 | Timestamp coarsening at capture | 1-2 days |
| C4 | GStreamer RTSP source (optional, after file works) | 5-7 days |

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
- **Encrypted vault** — structure present, wiring is v1.1
- **Real-time performance guarantees** — benchmark, don't promise
- **RTSP** — file reader first, RTSP is stretch goal
- **Remote attestation** — future

---

## Acceptance Criteria

### Must have:
- [ ] `cargo test` passes
- [ ] `StubBackend` works for motion detection
- [ ] `TractBackend` loads a model and runs forward pass
- [ ] `TractBackend` produces correct bounding boxes for a known test model
- [ ] Log entries are Ed25519 signed
- [ ] `log_verify` validates signatures and catches tampering
- [ ] Can process video from file
- [ ] Documentation states audit boundary vs security boundary

### Nice to have:
- [ ] RTSP ingestion
- [ ] Performance benchmarks

### Explicitly out of scope:
- [ ] GPU acceleration
- [ ] WASM sandboxing
- [ ] Face/plate detection (forbidden by design)
- [ ] Encrypted vault (v1.1)

---

## Success Metrics

v1 is successful if:

1. `witnessd` runs with file input and tract detection
2. Events are logged with Ed25519 signatures
3. `log_verify --tamper-test` proves tampering is detectable
4. External auditor can verify privacy claims by reading code
5. Documentation doesn't overclaim
