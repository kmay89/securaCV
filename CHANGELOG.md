# Changelog

## [0.5.0] - 2026-05-12

### Added — harm-reduction broadcast layer (Beacon channel) + audit artifacts

- **Beacon channel** (`spec/beacon_channel_v0.md`,
  `firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`):
  Smoke-detector-grade neighborhood harm-reduction broadcast layer with
  two-pubkey cryptographic co-signing on every origination, NFPA-72-style
  supervised health state (`Normal / Trouble / Alarm / Supervisory`),
  CAP-aligned wire fields, narrow life-safety-only template set (~13
  templates), daily Ed25519-signed self-test heartbeat with 36 h Trouble
  threshold, append-only chain-hashed audit log as a ring buffer
  NVS-persisted under the flash-encryption gate. Off by default
  (`FEATURE_BEACON_CHANNEL=0`).
- **Beacon REST API** (`beacon_api.h`): Bearer-token-gated surface —
  `/api/beacon`, `/set`, `/pair/*`, `/revoke`, `/originate`, `/cosign`,
  `/cancel`, `/active`, `/audit` (paginated), `/selftest`.
- **COSIGN_REQ/RESP encryption**: X25519 ECDH between paired device
  pubkeys, HKDF-SHA256 domain-separated key derivation
  (`securacv:beacon:cosign:v0`), ChaCha20-Poly1305 AEAD. X25519 keypair
  NVS-persisted across reboots.
- **Distinct Beacon airtime telemetry** in `airtime_governor`.
- **HA MQTT discovery** for both Chirp + Beacon NFPA states.
- **Audible `PATTERN_BEACON`** (1200/1700/2200 Hz ≤600 ms sequential),
  deliberately distinct from any reserved emergency-broadcast tone.
- **CAP gateway interop spec** (`spec/beacon_cap_gateway_v0.md`) —
  specification only; implementation deferred.
- `docs/audit/mesh_and_chirp_audit_v1.md` — full audit of Opera mesh +
  Chirp channel with per-finding traceability.
- `docs/audit/v0.3_closeout.md` — closure summary mapping each finding
  to source location, test, and PR.
- `docs/audit/hardware_verification_checklist.md` — outstanding
  hardware-bound verification recipes for the QA team.
- `docs/research/harm_reduction_prior_art.md` — CAP, IPAWS/WEA/EAS,
  NFPA 72, MUTCD DMS, Hawaii 2018 false-alert, harm-reduction movement,
  Meshtastic / GoTenna prior art.
- Host tests: `test_chirp_protocol_invariants.cpp`,
  `test_chirp_security.cpp`, `test_beacon_origination.cpp`,
  `test_mesh_opera_security.cpp` — per-finding regression coverage.
- CI lints: `scripts/lint_no_impersonation.sh` (reserved phrases /
  tones / colors) and `scripts/lint_cap_mapping.sh` (CAP template
  coverage).

### Changed — Chirp v0.2 hardening (closes audit C1–C17)

- End-to-end Ed25519 signature verification on every received witness /
  ACK / suppress-vote.
- `confirm_count` no longer carried on the wire; receivers track
  confirmations locally as a set of unique confirmer `session_pubkey`s.
- Relayers re-sign with their own session key; original signer's
  pubkey + signature preserved in `signed_origin` envelope so
  downstream receivers verify end-to-end.
- Signed `CHIRP_MSG_SUPPRESS_VOTE` wired end-to-end.
- Priority storage for received chirps — EMERGENCY survives a flood.
- 4 KB / 4-hash Bloom-filter nonce dedup with periodic reset.
- Wall-clock-anchored timestamps; origination refused when SNTP
  unsynced; conservative night-mode when unsynced.
- 5-emoji session display (~1 M distinct).
- `chirp_api.h` REST endpoints Bearer-gated via the standard
  template-trampoline pattern; wired into `canary_wap.ino`.
- Presence requirement also gates ACK origination.
- Per-`session_pubkey` rate limit on incoming witnesses.
- `TPL_AUTH_FEDERAL_PRESENCE` removed; 0x04 slot reserved.
- `PROTOCOL_VERSION` bumped from 0 to 1.

### Changed — Opera mesh v0.2 hardening (closes audit O1–O3)

- Message freshness anchored on per-peer monotonic counter; wall-clock
  TTL retired.
- `opera_secret` NVS persistence requires flash encryption enabled.
- `remove_peer()` now executes a full transactional rekey:
  generate new `opera_secret`, encrypt under each surviving member's
  session key, wait for ACKs, commit on all-ACK or 60 s timeout.

### Security

- Non-impersonation contract CI-enforced. No reserved
  emergency-broadcast phrases, no reserved-tone audio pair, no pure
  red as a primary alert color in any alert/chirp/beacon firmware or
  UI source.
- Beacon `scope = Private` always.
- No PII on the Beacon wire — templates only.
- 10 hardwired Beacon-channel invariants added to `AGENTS.md`.

## [0.4.0] - 2026-02-18

### Added
- **BLE Discovery subsystem** for Canary firmware (Opera/Chirp/Nearby):
  - **Opera**: BLE server advertising with SecuraCV custom GATT service — privacy-safe device
    identifier derived from Ed25519 pubkey hash, read-only status characteristics, writable
    command characteristic
  - **Chirp**: Connectionless BLE broadcast alerts between Canary devices — manufacturer-specific
    advertising data with truncated chain hash, coarsened timestamps, rate-limited (10s minimum)
  - **Nearby**: BLE scanner running on dedicated FreeRTOS task — discovers other Canaries via
    service UUID, tracks RSSI for proximity, thread-safe with mutex-protected shared state
  - New firmware files: `ble_config.h`, `ble_opera.h`, `ble_chirp.h`, `ble_nearby.h`, `ble_manager.h`
  - `FEATURE_BLE` compile flag in `build_config.h` (disabled in MINIMAL/DEV, enabled in FULL)
  - HTTP API endpoints: `GET /api/ble/status`, `GET /api/nearby`, `POST /api/chirp/send`
  - Web UI: BLE Discovery tab in Community panel with signal strength bars, nearby Canary list,
    chirp send buttons
  - NimBLE-Arduino library dependency (lighter than bluedroid, ~60% less RAM)
  - BLE protocol specification: `docs/ble_protocol.md`
  - BLE semantic events added to `spec/event_contract.md`

### Security
- BLE uses NimBLE only (no Bluetooth Classic — smaller binary blob surface)
- Device identity from Ed25519 pubkey hash, not hardware MAC address
- Non-Canary BLE devices counted only, never individually logged (privacy by default)
- All BLE code gated behind feature flag — compiles out completely when disabled
- Graceful degradation: firmware continues if BLE hardware unavailable

## [0.3.1] - 2026-01-21
### Fixed
- `log_verify` now verifies break-glass receipt chain (called from `main`)
- Receipt verification uses `[u8; 32]` device key signature input and supports `--verbose`


All notable changes to the Privacy Witness Kernel will be documented in this file.

## [0.2.0] - 2026-01-21

### Added
- **Frame isolation layer** (`src/frame.rs`):
  - `RawFrame`: Opaque container with private bytes (no Clone, no AsRef<[u8]>)
  - `InferenceView`: Restricted interface for modules (cannot export bytes)
  - `FrameBuffer`: Bounded ring buffer with build-time caps (30s, 300 frames)
  - `Detector` trait: Modules run inference without capturing pixel data
  - `StubDetector`: MVP motion detection via pixel hash comparison
  - `BreakGlassToken`: Placeholder for quorum-gated vault access

- **Ingestion layer** (`src/ingest/`):
  - `RtspSource`: Stub RTSP source with synthetic frames
  - `RtspConfig`: Configuration for RTSP streams
  - Timestamp coarsening at capture time
  - Non-invertible feature hash computation at capture time

- **Runtime improvements**:
  - `env_logger` for structured logging
  - Frame buffer stats logging
  - Verbose mode for `log_verify`
  - Conformance alarm checking in `log_verify`

### Changed
- `Module` trait now receives `InferenceView` instead of `Frame`
- `ZoneCrossingModule` uses `StubDetector` for motion detection
- `witnessd` uses `RtspSource` and `FrameBuffer` for frame handling

### Security
- Raw bytes are now physically inaccessible to modules (type-level enforcement)
- Frame buffer auto-zeroizes on drop and eviction
- Only path to raw bytes is `RawFrame::export_for_vault()` requiring `BreakGlassToken`

## [0.1.2] - 2026-01-21

### Fixed
- `validate_zone_id()` regex now compiled once via OnceLock
- Added negative test for module event-type allowlist rejection

## [0.1.1] - 2026-01-21

### Added
- `ReprocessGuard` wired into `read_events_ruleset_bound()`
- `conformance_alarms` actively written on contract/module violations
- `RawMediaBoundary` choke point scaffold
- Runtime module event-type authorization via `ModuleDescriptor`

### Changed
- Zone ID validation: blocklist → strict allowlist regex

## [0.1.0] - 2026-01-20

### Added
- Initial kernel: sealed log, contract enforcer, bucket key manager
- `witnessd` daemon and `log_verify` tool
- Spec documents: invariants, event contract, threat model, architecture