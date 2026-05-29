# Changelog

## [1.0.0] - Unreleased

### What v1.0 means

This is the first minimally credible release: every documented feature works
end-to-end, the install path succeeds on the first try, and the test suite
passes cleanly. It is **not** feature-complete — see "Explicitly deferred"
below.

### What's included

- **Privacy Witness Kernel** (Rust): hash-chained, Ed25519-signed append-only
  event log with break-glass N-of-M quorum access, vault sealing, event
  contract enforcement, module sandboxing (seccomp on Linux).
- **9 CLI binaries**: witnessd, log_verify, break_glass, export_events,
  export_verify, frigate_bridge, event_mqtt_bridge, witness_api,
  grove_vision2_ingest.
- **Sensor Adapter framework** (`src/adapter/`): an open, vendor-neutral interface that
  generalizes the `frigate_bridge` pattern so any source (acoustic/impulse, PIR/contact,
  presence, generic MQTT/webhook sensors, Frigate) can feed coarse, privacy-preserving claims
  into the same `append_event_checked` choke point — broad integration with no vendor lock-in and
  no new privilege. Includes the `adapter_host` binary (config-driven, one daemon, many adapters),
  Frigate + generic MQTT reference adapters, and the normative
  `spec/sensor_adapter_contract_v0.md` / `spec/witness_mesh_os_v0.md`. Expanded the event
  vocabulary with `acoustic_impulse_in_zone`, `presence_in_restricted_zone`,
  `vehicle_presence_after_hours`, `contact_state_change`, and `object_removed_from_zone`.
  - **Webhook ingress adapter** (`adapter-webhook`): a std-only HTTP `POST` listener so any
    device/script can register a sensor with a single `curl` — no MQTT broker required.
  - **Optional seccomp sandboxing** (`adapter-sandbox`, `with_sandbox(true)`): adapters can parse
    untrusted payloads inside the kernel's forked seccomp sandbox, upgrading the adapter audit
    boundary toward a security boundary for the parse step.
  - **Home Assistant surfacing**: the new claim types render in the "Last Event" sensor with
    friendly labels and per-type icons (`EVENT_TYPE_METADATA` in `const.py`).
  - **Webhook authentication + rate limiting + worker pool**: the webhook ingress (the one
    untrusted, network-facing surface) supports constant-time `Authorization: Bearer` or
    HMAC-SHA256 body-signature auth, per-path token-bucket rate limiting (`429`), and a bounded
    connection worker pool (`503` when saturated) that ends the unbounded per-connection thread
    spawn.
  - **BLE presence adapter** (`adapter-ble-presence`): turns ESPresense-style room-presence MQTT
    feeds into coarse presence claims, deliberately discarding device identity.
  - **Adapter observability**: per-adapter counters (polls/emitted/sealed/filtered/rejected +
    last-seal time) on the host, a periodic stats log, and an optional read-only `/stats` +
    `/healthz` HTTP endpoint (`stats_addr`) — operational counts only, never event content.
  - **Webhook TLS** (`adapter-webhook-tls`): optional rustls TLS on the webhook listener
    (`tls_cert`/`tls_key`), so bearer tokens aren't sent in clear on non-loopback deployments.
  - **HMAC replay protection**: opt-in `X-Timestamp` + `X-Nonce` bound into the signature
    (`hmac_replay_window_secs`), rejecting replayed or stale signed requests.
  - **Home Assistant native adapter-stats sensor**: configuring an "Adapter Host stats URL" adds a
    diagnostic sensor (per-adapter counters as attributes) via a dedicated coordinator — no
    hand-written YAML needed.
  - **Parser fuzz sweep** (`tests/adapter_parser_fuzz.rs`): seeded, panic-free robustness tests
    over the untrusted webhook/mqtt/BLE/Frigate parsers.
  - **Webhook mutual TLS**: optional client-certificate auth (`tls_client_ca`) — machine-to-machine
    sensors authenticate by certificate, with no shared secret on the wire.
  - **Prometheus metrics**: the stats endpoint serves `/metrics` (text exposition format) alongside
    JSON `/` and `/healthz`, for Grafana/Alertmanager scraping.
  - **SIGHUP config hot-reload**: `adapter_host` reloads `min_confidence` and each adapter's
    routes/rooms/filters live on SIGHUP, without restarting listeners or dropping connections;
    topology changes still require a restart.
- **Home Assistant integration** (HACS): 3 setup modes (MQTT / Kernel HTTP /
  both), MQTT auto-discovery, device PKI trust management (TOFU + manual pin +
  rotation), 5 sensor types, 11 binary sensor types (tamper + transport),
  Ed25519 signature verification, diagnostics.
- **Home Assistant add-on**: first-run setup wizard with preflight checks,
  camera TCP test, Frigate config generation, post-setup health verification,
  two operating modes (Frigate integration, standalone RTSP).
- **Install script**: single `curl | bash` command installs Mosquitto, Frigate,
  integration, add-on, generates device key, deploys automations + dashboard.
- **Firmware** (ESP32): canary-vision (ESP32-C3 + Grove Vision AI V2),
  canary-wap (XIAO ESP32-S3 Sense) — BLE discovery, Chirp community alerts,
  Beacon harm-reduction broadcast, Opera mesh networking, OTA updates.
- **Detection backends**: stub (testing), CPU (background subtraction),
  Tract ONNX (local inference).
- **Frame sources**: RTSP (GStreamer/FFmpeg), V4L2, ESP32 HTTP, local files.
- **Automations**: daily digest, pattern-break alerts, integrity failure alerts.
- **CI**: Rust tests + clippy, firmware builds, HACS/hassfest validation,
  SBOM generation, secrets scanning, CodeQL analysis, release workflow.

### Explicitly deferred (not in v1.0)

- Multi-camera standalone mode (currently single-camera only in standalone;
  Frigate mode supports multiple cameras via Frigate's own config)
- LoRa transport
- SCQCS audio transport
- CAP gateway interop (specification exists, implementation deferred)
- GPU-accelerated detection
- Tract detection confidence threshold override (hardcoded at 0.5)
- Pre-built Docker images on ghcr.io / Docker Hub
- Custom Lovelace card (uses standard HA entities)

### Known limitations

- v1 e2e pipeline verification (`verify_pipeline.sh`) requires a live
  docker-compose stack with Frigate + Mosquitto — not automated in CI.
- The HA add-on builds from source inside the container, which is slow on
  first install (~5-10 min on Pi 4). Pre-built images are planned for v1.1.
- Standalone RTSP mode processes one camera at a time. For multi-camera,
  use Frigate mode.

## [2.1.0] - 2026-05-27

### Added — Production Feature Plan (Phases 0-6) for ESP32-S3 Canary firmware

Seven-phase plan completing the firmware's production-readiness across both
PlatformIO (canary/) and Arduino WAP (canary-wap/) builds with full parity.

**New PlatformIO libraries:**

- **securacv_power** — Battery ADC (2:1 voltage divider on GPIO 1), 16-point
  LiPo discharge curve for SoC, software inference fallback, charge state
  machine with hysteresis, graceful brownout shutdown, battery health history
  persisted to NVS (charge cycles, voltage extremes, brownout count).
- **securacv_power_policy** — 6-mode runtime state machine (PLUGGED_IN,
  BATTERY_NORMAL, BATTERY_SAVER, LOW_POWER, SHUTDOWN, USB_ONLY). Per-mode
  CPU frequency scaling, WiFi power save, record interval tuning, progressive
  feature gating. Deep sleep cycling in emergency mode.
- **securacv_setup** — First-boot captive portal with DNS hijack, device
  naming, 15-minute timeout. NVS flag persists setup completion.
  - **Stays-connected onboarding**: OS connectivity probes are answered
    per-platform so the phone never flags the AP "no internet" and
    disconnects mid-setup — Apple gets the instruction page (Captive Network
    Assistant sheet), Android gets `204 No Content`, Windows gets the exact
    NCSI bodies. The captive DNS redirector runs for the whole life of the
    always-on AP (not just first boot), so rejoining the management AP after
    provisioning works too. The redirector answers only `A` queries and
    returns NODATA for `AAAA`/`HTTPS`, so `canary.local` resolves promptly on
    Android Chrome; `192.168.4.1` is the always-works fallback. Pure DNS
    builder (`captive_dns.h`) is host-unit-tested in CI.
- **securacv_diagnostics** — Heap monitoring (free/min/largest block/PSRAM/
  stack HWM/fragmentation), 3-level automatic feature degradation with 5KB
  hysteresis, SD health tracking (atomic write/error counters, space warnings),
  10-test boot self-test suite (NVS, heap, PSRAM, crypto, SD, WiFi, temp,
  uptime, watchdog, chain).
- **securacv_ble_status** — NimBLE GATT server with standard Battery Service
  (0x180F) and custom SecuraCV service exposing device name, firmware version,
  chain sequence, health score, degradation level, uptime, SD usage over BLE.
- **securacv_data_mgmt** — SD log rotation (witness 500, health 200, auto at
  85% SD usage), chain backup/restore with HMAC-SHA256 integrity (keyed by
  device private key), chain integrity verification (Ed25519 + hash continuity,
  capped at 100 records with watchdog yield), witness record export to /EXPORT/.

**New WAP header-only ports (full parity):**
power_monitor.h, power_policy.h, ble_status_api.h, data_mgmt_api.h, plus
sys_monitor.h enhanced with heap degradation levels and SD health tracking.

**New REST API endpoints (both builds):**
- `GET /api/diagnostics` — full diagnostic snapshot as JSON
- `GET /api/selftest` — re-run self-test suite on demand
- `GET /api/battery/history` — NVS-persisted battery health stats

**New serial commands:** `b` (battery), `p` (power policy), `d` (diagnostics),
`r` (data management).

**New feature flags:** FEATURE_POWER_MONITOR, FEATURE_POWER_POLICY,
FEATURE_SETUP_WIZARD, FEATURE_DIAGNOSTICS, FEATURE_BLE_STATUS,
FEATURE_DATA_MGMT.

### Security hardening

- Chain backup uses HMAC-SHA256 (was CRC-32) — prevents SD-level forgery
- Chain verification capped at 100 records with delay(1) yield — prevents
  watchdog timeout on large directories
- BLE GATT characteristics are read-only (no write/auth bypass possible)
- New REST endpoints (/api/diagnostics, /api/battery/history) auth-gated with
  rate limiting. Note: WAP's /api/selftest is intentionally unauthenticated
  (reachable on the captive-portal AP during setup, by design)
- Power policy rejects manual override to LOW_POWER/SHUTDOWN (anti-blinding)

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