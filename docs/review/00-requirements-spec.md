# SecuraCV — Rebuild-From-Scratch Requirements Specification

> **Purpose.** This document specifies SecuraCV precisely enough that a competent team could
> rebuild the entire feature set from scratch with no injected errors. It is an *audited* spec:
> every requirement carries a **Status** tag derived from reading the current code, not from the
> project's own marketing or roadmap. Where the implementation is partial, this says so, so that
> a re-implementer does not bake a half-finished behavior in as if it were complete.
>
> Companion documents: [`01-flag-report.md`](01-flag-report.md) (defects & inconsistencies) and
> [`02-roadmap.md`](02-roadmap.md) (independent roadmap + hardware plan). This review was produced
> by reading the tree at branch `claude/securacv-review-roadmap-lPAU2`; it deliberately treats the
> existing `v1-roadmap.md` and `docs/strategy/` as claims to verify.

## Status legend

| Tag | Meaning |
|-----|---------|
| **Implemented** | Code path exists and is exercised by a test or a runnable bin. |
| **Partial** | Real code exists but a documented part is missing/inactive. |
| **Stub** | Placeholder/heuristic standing in for the real thing. |
| **Spec-only** | Defined in `spec/`/`docs/` with no (or only scaffold) code. |
| **Config** | A build/deploy knob, not runtime logic. |

Requirement IDs are stable (`REQ-<AREA>-NNN`) and referenced from the flag report and roadmap.

---

## L0 — System requirements

### L0.1 Product definition
- **REQ-SYS-001 (Implemented):** SecuraCV is a *privacy-preserving computer-vision witness
  system*. It records **that** something happened (coarse, non-identifying semantic events) and
  proves the record is untampered, without building a searchable surveillance archive of **who**.
- **REQ-SYS-002 (Implemented):** The system is **local-first and subscription-free**. All
  authoritative logs and evidence are locally owned (Invariant IV); no component may require cloud
  custody for correctness or verification.
- **REQ-SYS-003 (Implemented):** Two pillars, both required: (a) privacy-preserving detection
  (events without surveillance) and (b) tamper-evident artifacts (cryptographic proof of
  integrity). Source: `v1-roadmap.md`, enforced by `spec/invariants.md`.

### L0.2 The seven invariants (the spine — bind every component)
Source of truth: `spec/invariants.md`. A re-implementation that violates any invariant is
**non-conforming**. These MUST be enforced in code and fail closed, not configured.

- **REQ-INV-I — No raw export by design.** Raw media may be processed in-memory only and MUST be
  discarded immediately unless explicitly sealed; no API may stream/mirror/replay raw media.
  Reversible derivatives (feature maps, embeddings, hi-res masks) are treated as raw media.
  Pre-event buffering is a bounded, in-memory ring buffer, zeroized on eviction, drained only
  under a break-glass token.
- **REQ-INV-II — No identity substrate.** No plates, facial embeddings, biometrics, or stable
  cross-device/cross-time identifiers; no identity-selector query interface. Only short-lived,
  non-invertible, device+bucket-scoped correlation artifacts permitted.
- **REQ-INV-III — Structural metadata minimization.** No precise external timestamps, no absolute
  location in records, no stable device IDs in shared telemetry; SHOULD batch/delay exports to
  reduce event-correlated network signals absent explicit cover-traffic config.
- **REQ-INV-IV — Local ownership & custody.** No centralized custody required; third parties
  cannot query/index logs remotely; every export yields a tamper-evident receipt tied to a
  specific disclosure act.
- **REQ-INV-V — Break-glass by quorum.** No single actor can access sealed evidence; N-of-M
  trustee authorization; each break-glass event logged immutably and externally verifiable; vault
  confidentiality relies on distinct device-local/quorum-derived key material (identifiers are
  never key material).
- **REQ-INV-VI — No retroactive capability expansion.** Events are permanently bound to the
  ruleset active at creation; upgrades may add modules but MUST NOT reinterpret/reprocess sealed
  logs; any such attempt MUST fail with an auditable error. `ruleset_hash` is an identifier, not a
  secret.
- **REQ-INV-VII — Non-queryability by design.** No retrospective identity search, no bulk
  historical pattern mining; inspection is sequential and context-bound only.

### L0.3 Data flow (canonical)
- **REQ-SYS-010 (Implemented):**
  `Camera (RTSP/V4L2/ESP32/Frigate-MQTT) → witnessd (in-memory frame → detect → contract
  enforcement) → hash-chained, Ed25519-signed, SQLCipher log (+ sealed vault for sensitive
  payloads) → Home Assistant (sensors, verification, digest, alerts) → phone (HA Companion push)`.
  Every **host-side** event producer (camera detector, sensor adapters, Frigate/MQTT bridges)
  MUST funnel through the **single choke point** `Kernel::append_event_checked`
  (`src/lib.rs:1202`; called from `adapter/host.rs`, `bin/{witnessd,frigate_bridge,
  grove_vision2_ingest,ingest_run}.rs`); contract enforcement there is authoritative and cannot be
  bypassed by any host producer. **Firmware is a separate trust domain:** canary-wap/canary-vision
  are C/C++ and do **not** call this Rust function — they produce and **independently sign** their
  own witness records and achieve contract *parity* by construction (same coarse vocabulary, no
  identity/MAC, Ed25519), per REQ-FW-020 and `spec/event_contract.md` §11. A rebuild MUST NOT
  assume the Rust enforcer covers firmware-originated events.

### L0.4 Repository topology (rebuild map)
| Path | Component | Language/runtime | Entry |
|------|-----------|------------------|-------|
| `src/` | Privacy Witness Kernel (log/crypto/vault/ingest/detect/api/break-glass/adapter) | Rust 2021 | `src/lib.rs` + 15 bins in `src/bin/` |
| `custom_components/securacv/` | Home Assistant integration | Python 3 | `__init__.py` |
| `privacy_witness_kernel/` | HA add-on wrapping `witnessd` + setup wizard | Docker/shell/Python | `run.sh`, `serve_wizard.py` |
| `canary-vision/` | Device-hosted HTTP API + vanilla-JS SPA (timeline UI) | Node/Express + JS | `device-api/server.js`, `spa/` |
| `viewer/` | Standalone offline evidence viewer + JS verifier | HTML/JS | `evidence_viewer.html`, `verify_core.js` |
| `firmware/` | ESP32 "Canary" firmware + shared `common/` | C/C++ (PlatformIO/Arduino) | `firmware/projects/*` |
| `spec/` | Normative specs (invariants, event contract, channels, break-glass) | Markdown | `spec/invariants.md` first |
| `kernel/`, `docs/` | Architecture + setup/philosophy docs | Markdown | — |
| `integrations/ha_frigate_mqtt/` | Worked Frigate+HA MQTT example + verify script | YAML/shell | `verify_pipeline.sh` |
| `tests/` | Rust integration tests | Rust | `cargo test` |

### L0.5 Threat-model scope (must be reproduced honestly)
- **REQ-SYS-020 (Implemented/Doc):** The kernel is tamper-**evident** within a trusted host
  boundary; a host root/admin operating outside the kernel is **out of scope** (`docs/root_paradox.md`).
  Detection backends are an **audited** boundary, not a sandbox, *except* where the optional
  seccomp sandbox is enabled (REQ-KRNL-061). A rebuild MUST document this audit-vs-security
  boundary and MUST NOT overclaim it (this is an open v1 acceptance item — see flag report).

---

## L1 — Component requirements

### L1.A Privacy Witness Kernel (`src/`)

#### A.1 Append-only sealed log
- **REQ-KRNL-001 (Implemented):** Events are appended to a hash-chained, append-only log. Each
  entry hashes its predecessor (`hash_entry`); altering any entry breaks the chain.
- **REQ-KRNL-002 (Implemented):** Every entry is **Ed25519-signed**; the device signing key is
  required at runtime and MUST reject the MVP placeholder seed (`src/lib.rs:815, 2016`;
  test `device_key_seed_rejects_mvp_placeholder` at `src/lib.rs:2948`).
- **REQ-KRNL-003 (Implemented):** Persistence is **SQLCipher** (encrypted SQLite). The DB
  encryption key is currently derived from the device signing key via `derive_db_encryption_key`
  (see flag report — coupling blocks key rotation; REQ-KRNL-072).
- **REQ-KRNL-004 (Implemented):** `log_verify` (bin) validates the hash chain **and** Ed25519
  signatures and detects tampering; `tamper_demo` (bin) demonstrates a modified log failing
  verification. Acceptance: `cargo run --bin demo` then
  `cargo run --bin log_verify -- --db demo_witness.db`.

#### A.2 Event model & contract enforcement
- **REQ-KRNL-010 (Implemented):** An Event MUST contain `event_type` (constrained vocabulary),
  `time_bucket` (coarse), `zone_id` (local), `confidence`, `kernel_version`, `ruleset_id`; MAY
  contain ephemeral `correlation_token` and `replication_status`; MUST NOT contain raw media,
  precise timestamps, absolute coordinates, stable IDs, or free-form text
  (`spec/event_contract.md` §2).
- **REQ-KRNL-011 (Implemented):** Time is expressed as **buckets** (min 5 min, default 10 min via
  `TimeBucket::now_10min`); bucket size and export jitter are **conformance-critical** and MUST NOT
  be narrowed without a ruleset change (`spec/event_contract.md` §3).
- **REQ-KRNL-012 (Implemented):** The permitted `event_type` allowlist is enforced; forbidden
  claims (`license_plate_detected`, `person_identified`, `same_vehicle_as_yesterday`, `face_match_score`,
  …) are rejected regardless of source. Tests: `tests/kernel_hardening.rs`,
  `tests/adapter_cannot_bypass_enforcer.rs`, `tests/compile_fail/`.
- **REQ-KRNL-013 (Implemented):** Correlation tokens MUST be non-invertible, device-scoped, expire
  ≤15 min, rotate per time-bucket with a key destroyed at bucket expiry, and be non-comparable
  across devices/buckets (`spec/event_contract.md` §6).
- **REQ-KRNL-014 (Implemented):** Canonical event vocabulary (current): `boundary_crossing_object_large/small`,
  `acoustic_impulse_in_zone`, `presence_in_restricted_zone`, `vehicle_presence_after_hours`,
  `contact_state_change`, `object_removed_from_zone`, `tamper_detected`, `vehicle_arrival_departure`, plus BLE-discovery
  semantic events (`spec/event_contract.md` §10) and tamper-alert records. New types may be added
  **only** via a ruleset change and only if coarse/non-identifying.

#### A.3 Detection backends
- **REQ-KRNL-020 (Implemented):** A `DetectorBackend` trait (`src/detect/backend.rs`) with a
  thread-safe `BackendRegistry` (`src/detect/registry.rs`); first-registered backend is default;
  capability-based selection (`backend_for_capability`).
- **REQ-KRNL-021 (Stub):** `StubBackend` (`src/detect/backends/stub.rs`) detects "motion" by
  SHA-256 frame-hash inequality, returning a fixed `confidence 0.85`, `SizeClass::Large`. This is
  the **default** path in `witnessd`/`demo`/`ingest_run` unless overridden. As of #660 `witnessd`
  emits a startup **WARN** that this motion-only path reports presence, not classified objects, with
  the flags to enable real detection (`src/bin/witnessd.rs:153-162`) — so the default is no longer
  silent.
- **REQ-KRNL-022 (Partial/Implemented):** `TractBackend` (`src/detect/backends/tract.rs`) loads an
  ONNX model via `tract-onnx` and runs a forward pass. The confidence threshold is **configurable**
  via the `detect.confidence` config key (env `WITNESS_DETECT_CONFIDENCE`), surfaced as
  `DetectSettings::confidence_threshold` and applied to `TractBackend` at `src/bin/witnessd.rs:552`
  (#665), defaulting to 0.5 when unset — the earlier hardcoded 0.5 is gone. It is **feature-gated** behind
  `backend-tract` and is **not** registered unless the feature is built AND a model is supplied; a
  one-command verified fetch + default model path (`scripts/fetch_detection_model.sh`, #667) removes
  the manual ONNX hand-download. A rebuild MUST NOT present real CV detection as the default
  behavior.
- **REQ-KRNL-023 (Implemented):** A `CpuBackend` is also registered by default.

#### A.4 Video / sensor ingestion
- **REQ-KRNL-030 (Implemented, feature-gated):** Ingest sources behind Cargo features:
  `ingest-file-ffmpeg` (mp4→frames, NV12→RGB24 via swscale), `rtsp-ffmpeg` / `rtsp-gstreamer`
  (two implementations — `src/ingest/rtsp.rs` and `src/ingest/rtsp_ffmpeg.rs`; see flag report
  REQ-KRNL-073), `ingest-v4l2`, `ingest-esp32`, `stub-frame-source`.
- **REQ-KRNL-031 (Implemented):** Timestamps are coarsened **at capture** (`TimeBucket::now_10min`).
- **REQ-KRNL-032 (Implemented):** End-to-end file roundtrip is exercised in CI against a committed
  mp4 fixture: `cargo run --features ingest-file-ffmpeg --bin ingest_run -- --video clip.mp4`.

#### A.5 Vault & break-glass
- **REQ-KRNL-040 (Implemented):** Break-glass requires **N-of-M quorum** trustee approval;
  policy storage + approval flow + immutable, externally-verifiable receipts
  (`src/break_glass/`, `BreakGlassReceipt`, `approvals_commitment`).
- **REQ-KRNL-041 (Implemented, opt-in):** The vault **is wired** into the live `witnessd` path
  (corrected after code review — see flag report F-05). `witnessd` constructs `Vault::new` with a
  `VaultCryptoMode` (`{Classical,Pq,Hybrid}`, `src/vault/crypto.rs`), buffers pre-roll frames in a
  bounded ring, and on boundary events seals the latest buffered frame via `seal_latest_frame()` →
  `vault.seal_frame()` (`src/bin/witnessd.rs:87-98,111,180,237-255,471-490`). Sealing is **opt-in
  and gated**: it runs only when `BREAK_GLASS_SEAL_TOKEN` supplies a valid token JSON and a frame
  is buffered (Invariant I/V: drain requires a break-glass token). A rebuild MUST implement the
  seal path; the remaining work is UX/config (no setup UI; crypto-mode default + key handling tie
  into REQ-KRNL-072), not the encryption itself.

#### A.6 Evidence envelope & canonical JSON (interchange format — reproduce exactly)
- **REQ-KRNL-050 (Implemented):** A single versioned, self-verifying **evidence envelope**
  (`src/envelope.rs`): `envelope_format = "securacv-evidence-envelope"`, `envelope_version = 1`,
  carrying manifest (permitted/forbidden fields, time granularity `min 300s`/`default 600s`,
  hash rule, canonicalization id, signature domains/schemes), provenance, four hash-chained
  ledgers verbatim, the coarse `ExportArtifact`, explicit gaps, disclosure manifest, export
  receipt, and a `whole_envelope_digest` (SHA-256 over the canonical digest input).
  Normative spec: `spec/evidence_envelope.md`.
- **REQ-KRNL-051 (Implemented):** **Two verifiers MUST agree byte-for-byte** — the Rust
  `verify_envelope` and the offline JS `viewer/verify_core.js`
  (test: `viewer/verify_core.test.js`). Canonicalization is centralized in
  `src/canonical_json.rs` (`CANONICALIZATION_ID`). This dual-verifier equivalence is a hard
  acceptance requirement.
- **REQ-KRNL-052 (Implemented):** Signature domains are separated (`DOMAIN_SEALED_LOG_ENTRY`,
  `DOMAIN_CHECKPOINT`, `DOMAIN_EXPORT_RECEIPT`, `DOMAIN_BREAK_GLASS_RECEIPT`) so a signature for
  one purpose cannot be replayed for another.

#### A.7 Sensor Adapter framework
- **REQ-KRNL-060 (Implemented):** A vendor-neutral adapter interface (`src/adapter/`) generalizing
  the Frigate bridge so any source (Frigate, generic MQTT, webhook, BLE-presence/ESPresense) emits
  a narrow pre-sanitized `Claim`; the trusted `adapter_host` stamps the coarse bucket, sanitizes
  the zone, maps to an allowlisted event type, and submits via `append_event_checked`. One daemon,
  many adapters, config-driven (`adapter_host` bin).
- **REQ-KRNL-061 (Implemented, opt-in):** Optional **seccomp** sandbox (`adapter-sandbox`,
  `with_sandbox(true)`) parses untrusted payloads inside a forked sandbox (Linux), upgrading the
  parse step from audit to security boundary.
- **REQ-KRNL-062 (Implemented):** Webhook ingress (`adapter-webhook`) is the one untrusted,
  network-facing surface: constant-time Bearer / HMAC-SHA256 body-signature auth, optional
  `X-Timestamp`+`X-Nonce` replay protection, per-path token-bucket rate limiting (429), bounded
  worker pool (503 on saturation), optional rustls TLS + mutual-TLS. Observability: per-adapter
  counters via `/stats`, `/healthz`, `/metrics` (Prometheus). SIGHUP hot-reload of confidence/route
  config. Parser fuzz sweep: `tests/adapter_parser_fuzz.rs`.

#### A.8 Crypto suite & post-quantum
- **REQ-KRNL-070 (Implemented):** Ed25519 signatures (`src/crypto/signatures.rs`), SHA-256 hashing.
- **REQ-KRNL-071 (Implemented, feature-gated):** Optional **post-quantum** modes exist as Cargo
  features: `pqc-signatures` (ML-DSA-44), `pqc-vault` (ML-KEM), `pqc-tls`. A `SignatureSet`/
  `SignatureMode` abstraction carries scheme IDs (`ED25519_SCHEME_ID`, `PQ_SCHEME_MLDSA44`). Doc:
  `docs/pqc_mode.md`. (Not surfaced in `v1-roadmap.md` — see flag report.)
- **REQ-KRNL-072 (Partial — known gap):** Device key is **seed-derived from config**, not
  hardware-backed or rotated; `derive_db_encryption_key` couples the DB key to the signing key.
  Rotation is blocked until these are decoupled.

#### A.9 CLI binaries (must all exist and run)
- **REQ-KRNL-080 (Implemented):** Binaries in `src/bin/`: `witnessd` (daemon), `log_verify`,
  `break_glass`, `export_events`, `export_verify`, `envelope_verify`, `frigate_bridge`,
  `event_mqtt_bridge`, `witness_api`, `adapter_host`, `grove_vision2_ingest`, `ingest_run`,
  `tamper_demo`, `demo`, `detect_eval` (15 total; `adapter_host`, `ingest_run`, `detect_eval` are
  feature-gated). CHANGELOG count reconciled to 15 (F-10 resolved).

### L1.B Home Assistant integration (`custom_components/securacv/`)
- **REQ-HA-001 (Implemented):** HACS-installable Python integration (HA 2024.4.1+). Connects via
  **HTTP API (required)** and optional **MQTT** (`SETUP_MODE_KERNEL|MQTT|BOTH`, `const.py`).
- **REQ-HA-002 (Implemented):** MQTT topic tree under `prefix/{device_id}/`: `status`, `events`,
  `health`, `chain`, `counts`, `command`, `tamper`, `mesh`, `chirp`, `transport`, `presence`.
- **REQ-HA-003 (Implemented):** **Multi-transport resilience** — a Canary uses ANY available
  transport to get witness data out before being silenced. `ALL_TRANSPORTS` =
  wifi_ap, wifi_sta, mqtt, ble, mesh, chirp. **lora** and **audio/SCQCS** are declared constants
  with **no transport implementation**, held in a separate `FUTURE_TRANSPORTS` list (deliberately
  **out of** `ALL_TRANSPORTS`) so the integration never advertises a transport no device can report
  on (F-07).
- **REQ-HA-004 (Implemented):** **Tamper/survivability event types** (`ALL_TAMPER_TYPES`):
  power_loss, battery_remove, sd_remove, sd_error, gps_jamming, gps_spoof, motion, enclosure,
  capacitive, gpio, watchdog, unexpected_reboot, memory_critical. **`audio_anomaly`** is declared but
  unimplemented, held in `FUTURE_TAMPER_TYPES` (out of `ALL_TAMPER_TYPES`) for the same reason (F-07).
- **REQ-HA-005 (Implemented):** Per-event-type friendly labels + icons via `EVENT_TYPE_METADATA`;
  optional adapter-stats diagnostic sensor (`CONF_ADAPTER_STATS_URL`) via a dedicated coordinator.
- **REQ-HA-006 (Spec/Example):** Example automations + Lovelace dashboard in `homeassistant/`;
  Frigate+HA MQTT worked example + `integrations/ha_frigate_mqtt/verify_pipeline.sh` (the v1
  release gate — must exit 0 against a live stack).

### L1.C Installer & add-on (`privacy_witness_kernel/`, `scripts/`)
- **REQ-INST-001 (Implemented):** `curl -fsSL …/scripts/install.sh | bash` one-liner; HA add-on
  ("Privacy Witness Kernel") wraps `witnessd` with a setup wizard (`serve_wizard.py`, `run.sh`,
  `Dockerfile`). Onboarding is currently developer-grade (see roadmap P2).

### L1.D Device SPA & offline viewer (`canary-vision/`, `viewer/`)
- **REQ-UI-001 (Implemented):** Node/Express device API (`canary-vision/device-api/server.js`) +
  vanilla-JS SPA (`canary-vision/spa/`) presenting an **in-app verified evidence timeline**
  (Phase 4), with an evidence-envelope bridge and shared verifier reuse.
- **REQ-UI-002 (Implemented):** A **standalone, offline** evidence viewer (`viewer/evidence_viewer.html`
  built from `viewer/template.html` via `viewer/build.mjs`) that verifies an exported envelope
  client-side using `viewer/verify_core.js` — the same logic the Rust kernel uses (REQ-KRNL-051).

### L1.E Firmware — Canary devices (`firmware/`)

#### E.1 Board & build matrix
- **REQ-FW-001 (Config):** Two hardware targets and three build profiles selected in
  `firmware/projects/canary-wap/arduino/canary_wap/build_config.h`, settable via Arduino IDE
  defines or PlatformIO `build_flags`:
  - Targets: `HARDWARE_XIAO_ESP32S3` (dual-core LX7, camera, 8 MB PSRAM OPI, BLE 5.0, no Classic,
    GPIO 0–48) and `HARDWARE_XIAO_ESP32C3` (single-core RISC-V, **no camera, no PSRAM**, BLE 5.0,
    GPIO 0–21).
  - Profiles: `MINIMAL` (crypto+GPS, no radios), `DEV` (+WiFi/HTTP/SD/BLE pairing), `FULL`
    (+Mesh+BLE discovery+scout+camera). `FULL` is treated as **release-grade**
    (`SECURACV_RELEASE_BUILD 1`: no debug credential fallbacks; device-unique AP password).
  - Exactly one target and one profile MUST be selected (`#error` guards enforce this).
- **REQ-FW-002 (Config):** Feature flags derive from target×profile (`FEATURE_SD_STORAGE`,
  `FEATURE_WIFI_AP`, `FEATURE_HTTP_SERVER`, `FEATURE_CAMERA_PEEK`, `FEATURE_MESH_NETWORK`,
  `FEATURE_BLUETOOTH`/`FEATURE_BLE`/`FEATURE_BLE_SCAN`/`FEATURE_BLE_STATUS`, `FEATURE_WIFI_PRESENCE`,
  `FEATURE_AUDIBLE_CHIRP`, `FEATURE_DATA_MGMT`, `FEATURE_POWER_MONITOR/POLICY`, `FEATURE_QR_PROVISION`).
  Camera/PSRAM-dependent features auto-disable on C3.

#### E.2 Canary projects
- **REQ-FW-010 (Implemented):** **canary-vision** (`firmware/projects/canary-vision/`, ESP32-C3 +
  Grove Vision AI V2): runs on-sensor inference on the Grove module and publishes
  privacy-preserving semantic events + HA MQTT discovery. The S3 does **not** run CV itself.
- **REQ-FW-011 (Implemented):** **canary-wap** (`firmware/projects/canary-wap/`, XIAO ESP32-S3
  Sense): WiFi-CSI presence sensing, BLE-Scout paired-beacon room attribution, **Opera** BLE mesh,
  beacon/chirp/bluetooth channels, captive-portal provisioning (`SecuraCV-XXXX` AP, last 4 MAC
  hex), web UI + companion PWA, airtime governor, anomaly baseline, power policy, QR provisioning,
  Ed25519-signed witness records. The Arduino sketch (`canary_wap.ino`, ~7.8K lines) is the single
  source of truth; the former `src/` HAL scaffold is archived (did not link).
- **REQ-FW-012 (Implemented):** **canary-ota** (`firmware/projects/canary-ota/`): A/B OTA update
  path with factory recovery slot.

#### E.3 Firmware crypto/contract parity
- **REQ-FW-020 (Implemented):** Firmware emits the **same** coarse, non-identifying event/tamper
  vocabulary and Ed25519-signed records as the kernel; BLE events use a truncated Ed25519 pubkey
  hash, never MAC/stable IDs; RSSI may give proximity context but MUST NOT be stored at
  tracking precision (`spec/event_contract.md` §10).

#### E.4 Flash / memory budget (S3) — must be honored on rebuild
- **REQ-FW-030 (Config — constraint):** On XIAO ESP32-S3 the binding constraint is **flash, not
  RAM** (PSRAM 8 MB OPI is ample). The Arduino FULL build runs near the ~3.0–3.3 MB app-slot
  ceiling. Web assets are shipped **pre-gzipped** (`web_assets_gz.h`, `CANARY_WEB_ASSETS_GZIPPED 1`):
  raw 457,779 B → gz 113,931 B (saves ~336 KB / ~10–11 pts of the app partition). The raw PROGMEM
  literals are compiled out. Re-run `gen_web_assets_gz.py` after editing any HTML.
  (`firmware/projects/canary-wap/FLASH_MEMORY_ANALYSIS.md`.)
- **REQ-FW-031 (Documented):** The canary-wap Arduino build does **not pin a PartitionScheme**
  (board default ~3 MB app — correct, because the FULL binary needs the full single app slot and
  does not fit a sub-2 MB OTA slot). The previously **divergent** partition tables (flag report
  F-06) are now reconciled by a canonical reference, [`firmware/PARTITIONS.md`](../../firmware/PARTITIONS.md),
  which maps one deliberate scheme per deployment (flash size × OTA × profile) and states the
  FULL-needs-no-OTA-on-8 MB / FULL+OTA-needs-16 MB rule. Each CSV cross-links it.
- **REQ-FW-032 (Implemented):** Large CSI/scratch buffers MUST live in **PSRAM**, not DRAM (recent
  fixes moved the 90 KB CSI event ring and `/api/events/today` scratch buffer to PSRAM to clear
  DRAM overflow). A rebuild MUST keep big buffers in PSRAM.

### L1.F Specs & channels (`spec/`) — normative, reproduce as contracts
- **REQ-SPEC-001 (Spec):** Normative documents to honor verbatim: `invariants.md`,
  `event_contract.md`, `threat_model.md`, `evidence_envelope.md`, `break_glass.md`, `co_signing.md`,
  `sensor_adapter_contract_v0.md`, `witness_mesh_os_v0.md`, and the v0 channel specs
  (`beacon_channel_v0.md`, `chirp_channel_v0.md`, `gossip_replication_v0.md`,
  `canary_mesh_network_v0.md`, `beacon_cap_gateway_v0.md`, `canary_free_signals_v0.md`). Note the
  `_v0` suffix marks these as early/experimental — a rebuild should treat them as design intent,
  not stable contracts, and check each against code.

---

## L2 — Cross-cutting contracts & acceptance

- **REQ-ACC-001:** `cargo test` passes (unit + integration: `kernel_hardening`, `frigate_integration`,
  `mqtt_e2e`, `tract_backend`, `adapter_*`, `time_bucket`, `compile_fail/`).
- **REQ-ACC-002:** Witness happy-path reproducible: `demo` → `log_verify` shows verified chain +
  signatures; `tamper_demo` shows verification failing on mutation.
- **REQ-ACC-003:** File-ingest roundtrip reproducible (REQ-KRNL-032).
- **REQ-ACC-004:** Rust ↔ JS envelope verifiers agree byte-for-byte (REQ-KRNL-051).
- **REQ-ACC-005:** v1 release gate: `integrations/ha_frigate_mqtt/verify_pipeline.sh` exits 0
  against a live HA+Frigate stack (per README release gate).
- **REQ-ACC-006:** Invariant enforcement is in **code, fails closed**, and is covered by tests that
  prove forbidden fields/event-types/cross-bucket tokens cannot be emitted by any producer
  (camera, adapter, firmware).

## How to reproduce the build (toolchains)
- Kernel: `cargo build`/`cargo test` (Ubuntu deps: `build-essential libseccomp-dev pkg-config`).
  Feature flags select ingest/detect/adapter/pqc capabilities (see `Cargo.toml [features]`).
- Firmware: PlatformIO (`pio run -e canary-wap-default`) **or** `arduino-cli compile` with FQBN
  `esp32:esp32:XIAO_ESP32S3:PSRAM=opi,FlashSize=8M,...` (see `firmware/.../Makefile`). Both compile
  the same sketch.
- HA: copy/HACS `custom_components/securacv/`; add-on from `privacy_witness_kernel/`.
