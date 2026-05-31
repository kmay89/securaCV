# SecuraCV — Flag Report (Inconsistencies, Unfinished & Poorly-Implemented Work)

> Companion to [`00-requirements-spec.md`](00-requirements-spec.md) and
> [`02-roadmap.md`](02-roadmap.md). Findings carry file evidence and a severity. The guiding
> instruction was **"do not assume anything is complete or finished"** — so this errs toward
> flagging. Severity: **Blocker** (undermines a core claim / ships a privacy or integrity gap) ·
> **Major** (materially misleads a re-implementer or user) · **Minor** (rot/cleanup) ·
> **Doc-debt** (documentation vs code drift).

## Summary table

| ID | Severity | Area | One-line |
|----|----------|------|----------|
| F-01 | Blocker | Detection | Default detection is a frame-hash **stub**; real CV is feature-gated & off by default, while roadmap marks the stream "Done". |
| F-02 | Blocker | Versioning | "v1" is defined **three incompatible ways** across CHANGELOG / v1-roadmap / README badge. |
| F-03 | Blocker | Firmware privacy | `ENTERPRISE_READINESS_TODO` admits raw **MAC exposure** and uncoarsened **GPS** in WAP APIs — contradicts Invariants II & III. |
| F-04 | Major | Crypto | Device key is **seed-derived from config**; DB key **coupled** to signing key → rotation blocked (acknowledged, still open). |
| F-05 | Minor | Vault | *Corrected after code review:* vault sealing **is wired** into `witnessd` (real crypto modes); the real gap is that it's **opt-in / UX-gated**, not absent. |
| F-06 | Major | Firmware flash | **Divergent partition tables**; canary-wap Arduino pins **no** scheme; one secure table assumes **4 MB** flash on an 8 MB board. |
| F-07 | Major | Transports | `TRANSPORT_LORA` / `TRANSPORT_AUDIO` (+`audio_anomaly` tamper) are declared but **unimplemented**. |
| F-08 | Major | Mesh | ESP-NOW WiFi-AP bridge & BLE fallback marked **"❌ not implemented"** in the mesh evaluation. |
| F-09 | Minor | Repo hygiene | Root **`spec.md` is mis-titled** — it's the `zone_crossing` module spec, not a system spec. |
| F-10 | Minor | CLI count | CHANGELOG says **"9 CLI binaries"**; tree has **14**. |
| F-11 | Minor | Ingest dup | **Two RTSP** implementations (`rtsp.rs` vs `rtsp_ffmpeg.rs`) risk divergence. |
| F-12 | Doc-debt | README | README has live **TODOs** (missing screenshot) and ships "core works end-to-end" beside an unshipped v1. |
| F-13 | Doc-debt | Roadmap | `v1-roadmap.md` overclaims completion (✅) on items contradicted by code (F-01) and omits shipped work (PQC, adapters). |
| F-14 | Minor | Spec maturity | Many normative specs are `_v0` / "Draft v0.1" but referenced as if stable contracts. |
| F-15 | Minor | Build env | Tests need `libseccomp`; firmware needs `pio`/`arduino-cli` — none present in the audit env, so "passes cleanly" is **unverified here**. |

---

## Blockers

### F-01 — Default detection is a stub; "real CV" is off by default
**Evidence.** `src/bin/witnessd.rs:133-141`, `demo.rs:192-202`, `ingest_run.rs:129-134` all
register `StubBackend` + `CpuBackend` and default to them. `StubBackend`
(`src/detect/backends/stub.rs`) declares motion purely by SHA-256 frame-hash inequality and
returns a **fixed** `confidence 0.85`, `SizeClass::Large`. The real ONNX path
(`TractBackend`, `src/detect/backends/tract.rs`) is **feature-gated** (`backend-tract`) and only
registered when the feature is compiled **and** a `--model` path is given; its
`confidence_threshold` is **hardcoded 0.5** (`tract.rs:49`).
**Why it's a blocker.** `v1-roadmap.md` Stream A marks A1–A5 all ✅ ("backend produces correct
boxes", "integration with InferenceView + registry routing"), and `docs/strategy/06` calls the
0.5 threshold the only gap. A reader concludes object detection is the default behavior. In a
default build it is **frame-differencing motion only**. **Fix:** make the default build's
detection behavior explicit in README/roadmap; ship a bundled model + enable `backend-tract` by
default for the witnessd path, or clearly label the stub as the default.

### F-02 — "v1" is defined three incompatible ways
**Evidence.**
- `CHANGELOG.md` `[1.0.0] - Unreleased`: "every documented feature works end-to-end, the install
  path succeeds on the first try, and the test suite passes cleanly" — with a large feature list.
- `v1-roadmap.md`: v1 is "minimally credible," and still has **unchecked** acceptance boxes
  (e.g. `[ ] Documentation states audit boundary vs security boundary`, `[ ] RTSP ingestion`).
- `README.md:5` badge: `status-pre--v1 (core works end-to-end)`.
**Why it's a blocker.** Tagging v1 against any one of these contradicts the others. **Fix:** pick a
single v1 definition (recommended: the roadmap's minimal one), make CHANGELOG match what's
actually verified, and gate the tag on `verify_pipeline.sh` (README release gate).

### F-03 — Firmware leaks identity/precise-location (contradicts Invariants II & III)
**Evidence.** `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` §1 (unchecked):
"Stop exposing raw MAC addresses in WAP APIs/logs … fail if `WiFi.macAddress()` appears in API
payloads/logging" and "Coarsen operator-visible GPS precision … enforce policy-consistent
precision in serial logs and HTTP responses." `spec/event_contract.md` §10 forbids MACs; Invariant
III forbids precise location.
**Why it's a blocker.** A device that surfaces raw MAC / fine GPS over its HTTP API violates the
core promise *on the at-risk persona's device*. **Fix:** implement the salted/rotating
pseudonymous presence token and GPS coarsening; add the grep guardrails the TODO itself proposes.
(Note: AP-password hardening and the device-unique credential in the same file **are** done — credit
where due — but these two remain open.)

---

## Major

### F-04 — Device key is config-seed-derived; DB key coupled to signing key
**Evidence.** `v1-roadmap.md` B2 note + `derive_db_encryption_key` (couples SQLCipher key to signing
key). Keys are not hardware-backed or rotated. **Impact:** no safe key rotation; "tamper-proof"
rests on a config-stored seed. **Fix (architectural prerequisite):** decouple DB key from device
identity key, then add hardware-backed keys (Secure Element/eFuse) — see roadmap P3.

### F-05 — Vault sealing is opt-in / UX-gated (NOT "unwired") — *corrected after code review*
**Correction.** An earlier draft of this finding (and the requirements spec) called the vault
"structure only, not wired," trusting `v1-roadmap.md`. **That was wrong — and is itself a lesson
in not trusting the roadmap over the code.** The vault *is* wired into the live `witnessd` path:
`witnessd` constructs `Vault::new(VaultConfig{ crypto_mode, .. })`, reads `BREAK_GLASS_SEAL_TOKEN`,
buffers pre-roll frames, and on boundary events calls `seal_latest_frame()` → `vault.seal_frame()`
which drains a buffered frame and seals it (`src/bin/witnessd.rs:87-98, 111, 180, 237-255, 454-490`).
Real encryption modes exist: `VaultCryptoMode::{Classical,Pq,Hybrid}` (`src/vault/crypto.rs`).
**Actual remaining gap (narrowed).** Sealing is **opt-in and UX-gated**, not absent: it only runs
when `BREAK_GLASS_SEAL_TOKEN` points at a valid token JSON and a frame is buffered; there is no
setup UI, and the crypto-mode default/key handling still ties into the device-key story (F-04).
**Severity downgraded Major → Minor/Doc-debt.** **Fix:** describe the vault as *wired but opt-in*,
document the token/crypto-mode config path, and build the trustee/seal setup UX (roadmap P2).

### F-06 — Divergent firmware partition tables; risky flash assumptions
**Evidence.** Three different layouts for the "same" device family:
- `firmware/canary/partitions_ota.csv`: app0/app1 = `0x1E0000` (1.96 MB) each + 192 KB spiffs.
- `firmware/projects/canary-ota/partitions.csv`: factory + ota_0 + ota_1 = `0x180000` (1.5 MB)
  each + a `witness_log` data partition — "For XIAO ESP32-S3 with 8MB Flash".
- `firmware/provisioning/partitions_secure.csv`: header says **"4MB flash total"**, app slots
  1.9 MB — but the S3 Sense ships 8 MB; this table wastes half the flash if used as-is.
- `firmware/projects/canary-wap/` (the *real* FULL build): **no PartitionScheme pinned**, board
  default ~3 MB (`FLASH_MEMORY_ANALYSIS.md`).
**Impact:** A re-implementer can't tell which is canonical; the FULL sketch is near its ceiling
*because* it uses the unpinned default, while a 1.96 MB-app OTA table would not fit the FULL
binary at all. **Fix:** one documented scheme per (flash size × OTA-or-not) deployment; reconcile
the 4 MB secure table to 8/16 MB (see requirement REQ-FW-031).

### F-07 — Declared-but-unimplemented transports & tamper types
**Evidence.** `custom_components/securacv/const.py:46-47` `TRANSPORT_LORA = "lora" # Future`,
`TRANSPORT_AUDIO = "audio" # Future: SCQCS`; both included in `ALL_TRANSPORTS`. `TAMPER_AUDIO =
"audio_anomaly" # Future`. No firmware grep hits for `lora`/`SCQCS`. **Impact:** the HA entity
surface advertises transports that can never report. **Fix:** drop from `ALL_*` until implemented,
or gate behind a capability flag. (`docs/strategy/06` already lists these as "keep deferred" —
align the code.)

### F-08 — Mesh fallbacks not implemented
**Evidence.** `docs/mesh_esp_now_evaluation.md:99-100`: "§2.2 WiFi-AP bridge (secondary) ❌ not
implemented (G3)", "§2.2 BLE fallback (tertiary) ❌ not implemented (G4)". **Impact:** the
multi-path mesh resilience story has unbuilt legs. **Fix:** scope mesh claims to what exists
(Opera BLE primary), or build the fallbacks.

---

## Minor / Doc-debt

- **F-09** Root `spec.md` is titled **"Module Spec: zone_crossing"** — a module template living at
  repo root, easily mistaken for the system spec. Move to `spec/modules/` and add a real root
  pointer. (`spec.md:1`.)
- **F-10** CHANGELOG: "**9 CLI binaries**" but `src/bin/` has **14** (`adapter_host, break_glass,
  demo, envelope_verify, event_mqtt_bridge, export_events, export_verify, frigate_bridge,
  grove_vision2_ingest, ingest_run, log_verify, tamper_demo, witness_api, witnessd`). Reconcile.
- **F-11** Two RTSP ingest implementations (`src/ingest/rtsp.rs`, `src/ingest/rtsp_ffmpeg.rs`)
  plus separate file/file_ffmpeg paths — confirm one is canonical and the other isn't bit-rotting.
- **F-12** `README.md:26` ships a literal `<!-- TODO: add a screenshot … -->`; the "verified ✓
  timeline" payoff has **no screenshot** even though `docs/strategy/05` flags this as a top adoption
  blocker. Badge "core works end-to-end" sits beside an unshipped v1.
- **F-13** `v1-roadmap.md` is stale in both directions: it marks contradicted items ✅ (F-01) **and**
  omits shipped capabilities (post-quantum `pqc-*` features, the whole Sensor Adapter framework,
  webhook TLS/mTLS/Prometheus). Treat it as a planning artifact to be rewritten, not a status board.
- **F-14** Spec maturity: `spec/*_v0.md` and "Draft v0.1" headers are referenced from README/specs
  as normative. Add a maturity column so re-implementers know which contracts are stable.
- **F-15** Build verifiability: `ENTERPRISE_READINESS_TODO` §0 records that in the audit
  environment `cargo test` is blocked by missing `libseccomp`, and `pio`/`arduino-cli`/`docker
  compose` are absent. So CHANGELOG's "test suite passes cleanly" and the firmware flash figures
  are **not independently verified in this review** — they should be re-run in a provisioned CI
  before any v1 tag. (CI workflows exist: `.github/workflows/firmware*.yml`, CodeQL.)

## What is genuinely solid (counter-balance — not everything is unfinished)
- Hash-chain + Ed25519 signing + `log_verify`/`tamper_demo` are real and testable (REQ-KRNL-001/004).
- The **dual Rust↔JS envelope verifier** with centralized canonical JSON is a strong, well-tested
  design (`src/envelope.rs`, `viewer/verify_core.js`, `viewer/verify_core.test.js`).
- The Sensor Adapter framework with a single `append_event_checked` choke point + seccomp option +
  fuzz tests is a clean, defensible integration boundary.
- Invariant enforcement has real teeth: `tests/adapter_cannot_bypass_enforcer.rs`,
  `tests/kernel_hardening.rs`, `tests/compile_fail/`.
- Firmware AP-password hardening (device-unique, no static fallback in release) is done.
