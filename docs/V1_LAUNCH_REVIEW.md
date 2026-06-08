# SecuraCV — v1 Launch Master Review & Plan

> **Audience:** founders, investors, and technical reviewers.
> **Date:** 2026-06-07 · **Reviewed tree:** `main` @ `b9ce99a` + branch `claude/securacv-v1-launch-review`.
> **Method:** read against the source tree, CI run history, and the live issue/PR backlog —
> not against the marketing copy. Every status below cites the code or check that backs it.
>
> **One-sentence verdict:** the software is *substantially v1-complete and CI-green*; the only
> thing standing between SecuraCV and a tagged, advertisable v1 is **on-device hardware
> validation** plus a small number of well-scoped decisions. Nothing headline is vaporware.

---

## 0. The thesis we are shipping

Every other camera asks *"who was that?"* SecuraCV answers a better question: **"Is everything
okay — and can I prove it?"** Two architectural commitments make that sentence true, and both
are real in the code today:

1. **Privacy-preserving perception** — the kernel converts camera/sensor detections into coarse
   *semantic events* and never persists raw frames, precise timestamps, faces, plates, or
   identity. These are not policy promises; they are enforced in the **type system** (you
   cannot compile code that serializes a `RawFrame` or escapes an `InferenceView` — see the
   five `tests/compile_fail/*` cases) and at the single `Kernel::append_event_checked` choke
   point.
2. **Tamper-evident proof** — every event is appended to a hash-chained, Ed25519-signed,
   SQLCipher-encrypted log. Alter one bit and `log_verify` fails. This is demonstrable end to
   end today (`tamper_demo`, `frigate_mqtt_e2e`).

That combination — *authenticatable perception for humans, not surveillance for enterprises* —
is the moat. It is architecture, not a feature, and a competitor whose business model is your
footage cannot copy it.

---

## 1. Where we are: the honest state of every subsystem

Legend: ✅ real & CI-verified · 🟡 real but opt-in / gated / unproven-on-hardware · 🔴 stubbed or deferred.

### 1.1 Privacy Witness Kernel (Rust) — the heart

| Capability | State | Evidence |
|---|---|---|
| Hash-chained, Ed25519-signed append-only log | ✅ | `src/log`, `frigate_mqtt_e2e`, `tamper_demo` |
| SQLCipher-encrypted storage; DB key decoupled from signing key | ✅ | `resolve_db_encryption_key`, `SECURACV_DB_KEY_SEED`, `docs/db_key_rotation.md` |
| Type-level privacy enforcement (no raw-frame escape) | ✅ | `tests/compile_fail/` (5 cases) |
| Event-contract allowlist enforced at one choke point | ✅ | `append_event_checked`, `tests/adapter_cannot_bypass_enforcer.rs` |
| Break-glass N-of-M quorum + signed receipts | ✅ | `src/break_glass`, `export_events_authorized.rs` |
| Vault frame sealing (ChaCha20-Poly1305 / ML-KEM hybrid) | 🟡 | Real & tested, but **opt-in** behind a valid `BREAK_GLASS_SEAL_TOKEN`; `witnessd` logs ENABLED/DISABLED at startup (F-05) |
| Device signing key | 🟡 | Seed-derived from config/file (0600), persistent. **Not hardware-backed** (TPM/SE = v1.1, F-04); full identity-rotation still pins `device_metadata` |
| Post-quantum signatures & vault (ML-DSA / ML-KEM) | 🟡 | Compiles + tested in CI (`pqc-signatures,pqc-vault`); no on-hardware cycle |

Default `cargo check --workspace --all-targets` is **clean** (verified this review). Crate
version is `0.5.0` — the version bump to `1.0.0` is part of the tag step (§4).

### 1.2 Detection & ingestion

| Capability | State | Evidence |
|---|---|---|
| Detector backend abstraction (`DetectorBackend` + registry) | ✅ | `src/detect` |
| Motion backend (default) | ✅ | default build; startup `WARN` makes "motion, not objects" explicit |
| ONNX object detection (Tract, tiny-YOLOv2) | 🟡 | Real & tested (`tract_backend.rs`, `detect_eval`), but **feature-gated** (`backend-tract`) and needs a model file — not in the stock build |
| File ingest (mp4 → frames → signed events) | ✅ | `ingest-ffmpeg` CI job |
| RTSP ingest (ffmpeg), full decode→detect→sign→verify | ✅ | `ingest-rtsp` CI job (MediaMTX + `tests/rtsp_e2e.rs`) |
| RTSP (GStreamer) / V4L2 / ESP32-HTTP sources | 🟡 | Compile-gated in CI; share the `RtspSource`/source interface; not separately exercised end-to-end |

### 1.3 Sensor-adapter framework (the integration story, no vendor lock-in)

✅ Vendor-neutral `SensorAdapter` contract feeding the same privacy choke point: Frigate, generic
MQTT, webhook (std-only HTTP, bearer/HMAC auth, replay protection, TLS + mTLS), BLE-presence.
Plus the `adapter_host` daemon (config-driven, SIGHUP hot-reload), per-adapter observability
(`/stats`, `/healthz`, `/metrics` Prometheus), optional seccomp sandboxing of untrusted parsers,
and a seeded parser fuzz sweep (`tests/adapter_parser_fuzz.rs`). This is genuinely strong and
under-advertised.

### 1.4 Home Assistant integration

✅ HACS-valid (`hacs.json`, HA ≥ 2024.4.1). Three setup modes (MQTT / kernel-HTTP / both),
MQTT auto-discovery, per-device PKI trust (TOFU + manual pin + rotation), Ed25519 signature
verification on every payload, 5 sensor + 11 binary-sensor types (tamper + transport), adapter-
stats diagnostic sensor, and a bundled **verified-✓ timeline Lovelace card** with an honest
badge taxonomy (the "Signature verified" ✓ appears *only* when the signature actually verified).
The one structural gap: there is **no full HA instance in CI** (you cannot boot HA in a unit
runner), so HA-side coverage is unit-level plus the manual `verify_pipeline.sh` smoke check.

### 1.5 Frigate → MQTT → kernel → HA pipeline (the flagship operator path)

✅ Gated in CI two ways: `tests/frigate_mqtt_e2e.rs` (a `frigate/events` payload → privacy-strip →
sealed encrypted log → real `log_verify`) and the `frigate-mqtt-e2e` job running the real
`frigate_bridge` binary against a **live mosquitto broker** (`ci_smoke.sh`). The 4-container
operator stack (`verify_pipeline.sh`) is an honest **manual** smoke check (ML detection on a
fixture isn't deterministic, so it can't be a deterministic gate).

### 1.6 canary-vision device API + SPA (the device's HTTP surface & companion UI)

✅ Production-quality reference server (Node, zero framework) + vanilla-JS SPA. 16 routes + an
SSE timeline, witness chain (hash + Ed25519), **coarsen-or-fail** evidence envelope sharing the
*same* verifier module as the offline tool, signed firmware update with anti-downgrade, and a
defense-in-depth middleware stack (mandatory token auth w/ constant-time compare + server-side
session TTL, DNS-rebinding host validation, restrictive CORS that **denies peer origins to block
lateral movement**, Chrome PNA preflight, per-IP rate limit + auth-failure lockout, strict CSP,
camera-peek locked immutable). **25/25 tests pass** (`npm test`). Honest caveat: a few device-side
actions are simulated in the Node reference (reboot, thumbnails, the update-*check*), and the
`/peers` endpoint is **passive discovery only** — see §2.

### 1.7 Firmware (ESP32 Canary family)

The firmware is broad and mature in places — but it is also where the **real launch risk** lives,
for three reasons developed in §2 and §3.

✅ Strong & host-tested: on-device Ed25519 witness chain (domain-separated, NVS-persisted
sequence), GPS coarsening (`gps_coarsen_deg`, ~110 m) and MAC-free salted `device_pseudonym`
shared across *all* trees, captive-portal onboarding, per-device AP password, mDNS unique
hostnames + multi-canary naming/Identify, bearer-token API + rate limit, CSI sensing pipeline,
and a 19-check `regression_check.sh` that hard-fails on raw MAC / un-coarsened GPS / hardcoded
secrets. Dual-build CI (PlatformIO + Arduino) is green.

🟡 / 🔴 Risk areas: device-to-device **mesh ships disabled by default and is hardware-unproven**
(§2); firmware **HTTPS/TLS is implemented in `canary-wap` but cert-gated and variant-dependent**
(the `canary-wap` Arduino tree starts an `httpd_ssl_start` server on port 443 with an HTTP→443
redirect when `SECURACV_HAS_HTTPS_SERVER` and a provisioned cert are present — it is *not* absent;
the gap is that it depends on cert provisioning and the ESP-IDF HTTPS config, and other variants
fall back to HTTP); and there is **variant fragmentation** in which capabilities are wired/enabled
per tree (§3).

---

## 2. The headline goal — "multiple canaries talk to one another without Home Assistant"

This is the most important thing to get *honestly* right, because it is both a marquee claim and
the area where the code and the aspiration are furthest apart. There are **two** distinct
mechanisms in the repo, and only one is the real answer:

**A. Firmware ESP-NOW "Opera" mesh + Chirp/Beacon channels — the real device-to-device fabric.**
This is genuine, substantial code: peer discovery via ESP-NOW beacons, visual 6-digit pairing,
per-peer ChaCha20-Poly1305, witness-record gossip, deterministic hub-failover election, 3-hop
relay with a 2-witness confirmation gate, and community-governed Chirp alerts. It is fully
implemented and **host-tested** in the `canary-wap` Arduino tree (~2,000-line `mesh_network.cpp`,
~1,500-line `chirp_channel.cpp`, plus security tests). **But:**

- It **ships disabled in production builds.** `firmware/canary/platformio.ini` sets
  `FEATURE_MESH_NETWORK=0` in `dev`/`release`/`standalone` and `=1` only in `[env:full]`.
- It has **never been validated on hardware.** The v0.2 security hardening (counter freshness
  O1, FE-gated provisioning O2, transactional rekey O3) and cross-reboot replay defense have no
  on-device repro artifacts (the `docs/audit/repro/*` dirs are empty). This is tracked as the
  open **issue #610** and is explicitly hardware-blocked.
- The **ACTIVE** modular tree (`firmware/canary/`) **does** wire the mesh path:
  `firmware/canary/src/main.cpp` includes `mesh_transport.h`/`mesh_session.h` under
  `FEATURE_MESH_NETWORK`, calls `mesh_transport::init/start` + `mesh_session::init/start` in
  `setup()`, and drives them from `loop()` (the paired-callback fires from `mesh_session::process()`).
  `[env:full]` additionally sets `FEATURE_RF_PRESENCE=1` and `FEATURE_CHIRP=1`. So the gap is
  **default enablement + on-hardware proof + chirp/RF parity across envs**, *not* absent wiring —
  the earlier "header stubs" framing was wrong (corrected per PR review).

**B. canary-vision `/api/v1/peers` — passive discovery only, by design.** It lists peers and
offers one-tap pairing in the SPA; it does **not** gossip, sync, vote, or replicate. This is the
correct security posture for the HTTP surface, but it is **not** "canaries talking to each
other." Don't let the two get conflated in marketing.

**Bottom line:** the capability *exists as real, wired code* in both the ACTIVE and Arduino trees
and is the right design — but to advertise "multiple canaries talk to one another without HA"
truthfully at v1, we must (a) confirm chirp/RF parity in the chosen env, (b) turn mesh on in a
shippable build that meets its flash-encryption precondition (it's `=0` everywhere except
`[env:full]`), and (c) prove O1/O2/O3 + replay on two physical devices (#610). Until then the
honest phrasing is "mesh-ready (beta), validated on hardware in v1.0."

---

## 3. Decisions only the founder can make (these gate the plan)

1. **~~Which firmware tree is the v1 shipping image?~~ DECIDED: bring both to full parity.** Both
   the `canary-wap` **Arduino** tree (broadest single sketch; already runs HTTPS:443) and the
   ACTIVE modular `firmware/canary/` tree (mesh wired under `FEATURE_MESH_NETWORK`; RF/chirp in
   `[env:full]`) ship as supported v1 images, brought to **bidirectional feature parity**. The
   divergences and the sequenced closure program are tracked in
   [`firmware/PARITY_PLAN.md`](../firmware/PARITY_PLAN.md). **This shapes Phase 2.**
2. **Hardware-backed keys: implement or defer?** v1 ships seed-derived device keys (persistent,
   0600). The threat model's institutional/coercion classes are better served by a Secure
   Element. Recommended: **defer to v1.1, state it plainly** (it already is in the roadmap) —
   don't let perfect block the tag.
3. **Firmware HTTPS/TLS: confirm it's on in the v1 image.** `canary-wap` *already* serves HTTPS on
   port 443 (with an HTTP→443 redirect) when `SECURACV_HAS_HTTPS_SERVER` and a provisioned cert are
   present — so the work is not "build TLS" but **ensure the shipping build enables the ESP-IDF
   HTTPS config and provisions/generates a cert as part of setup**, and verify the handshake
   on-device (a bench-test item). Where a variant can't provide a cert it falls back to HTTP; scope
   the threat-model TLS claim to the variants/configs that actually run 443.
4. **Object-detection default.** Stock `witnessd` is motion-only. For the direct-ingest demo to
   "wow," bundle a small ONNX model + flip the build so first-run shows *classified* events.
   (In the primary Frigate deployment this is moot — detection is Frigate's job.)

---

## 4. The launch plan (phased, exhaustive)

### Phase 0 — Make CI fully green *(hours; mostly done)*
- [x] **Fix `Detect Eval` CI.** It has been red on *every* commit since it was added — a YAML
      startup failure (the unquoted `cargo test --lib eval::` makes the file unparseable, so 0
      jobs run). Fixed on this branch by quoting the scalar; `python -c "yaml.safe_load(...)"`
      now parses. (Open PR #734 fixes the same line — close it as superseded or merge it.)
- [ ] Confirm the full check set is green on the merge commit: Rust, HACS, CodeQL, SBOM, Secret
      Scanning, Repo Lints, Evidence Viewer, Add-on image, firmware, **Detect Eval**.

### Phase 1 — The on-device hardware gauntlet *(the true v1 gate)*
This is the **only** remaining hard blocker in the canonical v1 definition. Everything below
needs real boards; none of it can run in the cloud. **Driver sheet:**
[`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md) sequences every step, command, expected
result, and artifact, with a sign-off matrix.
- [ ] Flash the chosen firmware image on ≥ 2 XIAO ESP32-S3 boards; run capture → on-device
      witness chain → MQTT → kernel/bridge → HA end-to-end; capture serial + chain-verify logs.
- [ ] Run the live 4-container operator stack and `integrations/ha_frigate_mqtt/verify_pipeline.sh`
      against real Frigate ML detection; archive the run.
- [ ] Execute the mesh hardware-verification checklist (**issue #610**: O1 counter freshness, O2
      FE-gated provisioning, O3 transactional rekey, replay-after-reboot) on two
      flash-encryption-enabled devices; fill `docs/audit/repro/*`; tick `docs/audit/v0.3_closeout.md`.

### Phase 2 — Deliver the "canaries talk to each other" claim *(after #610 passes)*
- [ ] Resolve Decision #1 (which tree); ensure mesh + chirp are wired into the shipping image.
- [ ] Flip `FEATURE_MESH_NETWORK=1` in the production env that meets the FE precondition; confirm
      the FE gate fails closed on non-FE boards.
- [ ] Add a `regression_check.sh` assertion that **mesh only enables alongside flash encryption**.
- [ ] Advance the `firmware/FEATURES.md` mesh row to ✅ and record the hardware evidence.

### Phase 3 — Close stated-vs-actual security gaps
- [ ] Resolve Decision #3: confirm the v1 image enables the ESP-IDF HTTPS config + cert
      provisioning so `canary-wap`'s port-443 server actually runs (bench-verify the handshake);
      scope the TLS claim to the configs that serve 443.
- [ ] Resolve Decision #2: confirm hardware-backed keys are documented as v1.1 (not silently
      implied) everywhere identity is discussed.
- [ ] Re-run the firmware `regression_check.sh` and the security-review skill over the diff.

### Phase 4 — Ship & advertise
- [ ] Single source of truth for status: bump crate `0.5.0 → 1.0.0`, flip `CHANGELOG.md`
      `[1.0.0] Unreleased → dated`, update the README badge from `v1-rc` to `v1.0`.
- [ ] **Show, don't tell:** add a real screenshot of the verified-✓ timeline + morning digest to
      the top of the README (today there's only the logo GIF). This is the single most
      persuasive missing asset.
- [ ] Publish the capability one-pager (§5) and the honest-caveats box (keep the audit-boundary
      caveat visible — it builds trust with exactly the technical buyer who adopts first).
- [ ] (Go-to-market, parallel track) Pre-flashed Canary kit so the at-risk user and the
      mainstream homeowner can *buy*, not build — the biggest market-expansion lever, and out of
      scope for the *tag* itself.

---

## 5. What it can do — the capability sheet (for advertising)

**The product, in one line:** *peace of mind you can take to court — a private camera with a
24-hour memory and an unforgeable witness log.*

- **Private by architecture, not by policy.** No faces, no plates, no precise timestamps, no raw
  frames. The seven invariants are enforced in code and the Rust type system — an auditor can
  *read* the guarantee, not just trust it.
- **Tamper-evident by cryptography.** Hash-chained, Ed25519-signed, encrypted-at-rest event log.
  Alter the record and verification fails — *even if the operator does it.* Demonstrable in 30
  seconds (`demo` → `tamper_demo` → `log_verify`).
- **No subscription, ever.** Runs on your own Pi/PC. Nothing phones home.
- **Plays well with what you already run.** Wraps Frigate; surfaces a verified-✓ timeline,
  tamper sensors, and a morning "all-clear" digest natively in Home Assistant via HACS.
- **Open integration, no lock-in.** A vendor-neutral sensor-adapter framework (Frigate, MQTT,
  webhook, BLE) feeds the same privacy choke point — acoustic, contact, presence, and vehicle
  events all become coarse, signed claims.
- **Evidence you can hand to a lawyer.** Break-glass N-of-M trustee quorum, signed export
  bundles, and a standalone offline verifier.
- **A sensor mesh, not just a camera.** ESP32 "Canary" devices add CSI presence/breathing,
  tamper detection, and (v1.0, on validated hardware) a device-to-device ESP-NOW mesh + community
  Chirp alerts that work with no hub and no cloud.
- **Future-proofed.** Optional post-quantum signatures and hybrid vault sealing (ML-DSA / ML-KEM)
  are already in-tree and tested.

**Honest caveats we keep visible (this is a feature, not a confession):**
detection backends are an *audited* trust boundary, not a sandbox; the stock direct-ingest build
reports motion unless an ONNX model is supplied; vault sealing and hardware-backed keys are opt-in
/ v1.1; and the device-to-device mesh is mesh-ready, enabled on flash-encryption hardware and
validated before we advertise it. A host's root operator runs outside the kernel's control — see
`docs/root_paradox.md`. **We never overclaim; the honest brand *is* the moat.**

---

## 6. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| On-device validation surfaces real-world bugs not seen in CI | High | Phase 1 is the gate; do it before any tag or press |
| Mesh advertised before hardware proof (#610) | High | Phase 2 gated on #610; ship "mesh-ready (beta)" phrasing until then |
| Firmware variant fragmentation causes "which build?" confusion | Medium | Decision #1 up front; document the one v1 image |
| TLS depends on per-variant cert provisioning / IDF config (HTTP fallback) | Medium | Phase 3 confirms 443 runs in the v1 image; scope the claim to those configs |
| `v1-rc`/`Unreleased`/`0.5.0` inconsistency reads as "unshipped" | Low | Phase 4 single-source-of-truth bump |
| No HA-in-CI means an HA regression ships silently | Low | Keep `verify_pipeline.sh` in the release checklist |

---

## 7. The bottom line for investors & reviewers

SecuraCV is **not** a slide-deck product. The privacy guarantees are enforced in a type system;
the tamper-evidence is demonstrable end to end; the Frigate→HA pipeline, RTSP/file ingest, the
adapter framework, the HA integration, and the device API + SPA are all real and CI-gated, with
25/25 device-API tests and a clean default Rust build. The remaining work to a defensible,
advertisable v1.0 is **disciplined validation, not invention**: run the on-device gauntlet, turn
on and prove the mesh, close two small stated-vs-actual gaps, and bump the version. The roadmap
is honest about what's deferred, which is exactly the posture that earns the trust of the
privacy-conscious and at-risk users who are the brand. **Finish the gauntlet and it ships.**
