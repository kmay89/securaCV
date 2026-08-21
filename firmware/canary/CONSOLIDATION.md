# Canary Consolidation Plan

**Status:** Phase 4 complete (Phases 1 ✅ #305, 2 ✅ #306, 4 ✅ mesh sensing v1)
**Created:** 2026-04-17
**Owner:** firmware maintainers
**Companion docs:** [VARIANT_POLICY.md](../VARIANT_POLICY.md), [FEATURES.md](../FEATURES.md), [FIRMWARE_VARIANT_AUDIT.md](../FIRMWARE_VARIANT_AUDIT.md)

This document records the decision to **consolidate the Arduino-IDE monolithic `canary-wap` sketch into the modular `firmware/canary/` PlatformIO build** (Option A from the repository audit plan) and tracks the phased work required to get there.

---

## Decision: Option A — Port into Modular Canary

Two options were considered in the audit plan:

- **Option A:** Port Arduino WAP features into `canary/lib/*` components; retire the Arduino-IDE path once parity is reached.
- **Option B:** Keep both lanes in parallel indefinitely, documented as separate tracks.

**Decision: Option A.**

### Rationale

1. **Maintainability.** The Arduino sketch is ~22 KLOC in a single monolith with large PROGMEM web UI. Every change recompiles the world and risks regressions across unrelated subsystems.
2. **Testability.** Modular canary libs can be unit-tested individually; the monolithic sketch cannot.
3. **Security hygiene.** CI regression checks (`firmware/scripts/regression_check.sh`) treat the modular tree as primary. Duplicated logic is the #1 drift vector we've seen (witnessed by the snapshot archival).
4. **Single source of truth.** [VARIANT_POLICY.md](../VARIANT_POLICY.md) designates `firmware/canary/` as the ACTIVE lane; Option B is inconsistent with that policy.
5. **User impact is limited.** Arduino IDE users continue to build from `firmware/projects/canary-wap/arduino/canary_wap/` during the transition; nothing they use today breaks until the final retirement step.

### What "consolidated" means

- `firmware/canary/` (ACTIVE) reaches ≥ 95% feature parity with `canary-wap/arduino/canary_wap/` on the [FEATURES.md](../FEATURES.md) dashboard (every ✅/⚠️ cell in the Arduino column is also ✅ in the canary PIO column, excluding items intentionally scoped out).
- Arduino IDE path is re-labeled **SUNSET** in `VARIANT_POLICY.md` for one release, then moved to `firmware/projects/_archive/` with the same archive-guard protections as the snapshot.
- `firmware/FEATURES.md`, `firmware/README.md`, and `firmware/ARCHITECTURE.md` are updated to reflect the single-lane world.

---

## Feature Gap Inventory

Derived from the post-archive Feature-Parity Dashboard in [FEATURES.md](../FEATURES.md). "Gap" means the Arduino column is ✅ and the canary (PIO) column is ⚠️ or ❌.

| # | Gap | Current canary (PIO) state | Source to port from | Port complexity | Security impact |
|---|---|---|---|---|---|
| 1 | API authentication (bearer token + HKDF derivation + exponential backoff + constant-time compare) | ✅ (Phase 2.5: SPA injects bearer; all read + mutating endpoints gated) | `canary_wap/api_auth.h` (~275 LOC) | **Low** (self-contained, ESP-IDF native) | **High** (every REST endpoint is currently unauthenticated) |
| 2 | Rate limiting on HTTP API | ✅ (already present) | — | N/A | — |
| 3 | Camera peek MJPEG streaming | ❌ | `canary_wap/wap_server.cpp` peek handlers | High (streaming loop + resolution control) | Medium (no frame storage, still subject to DoS) |
| 4 | GPS motion FSM (EMA + hysteresis + debounce) | ⚠️ NMEA parse only | `canary_wap/*` motion detection | Medium | Low |
| 5 | Full web UI tabs (Peek/Logs filtering/Chain/Export) | ⚠️ partial | `canary_wap/web_ui.h` (large PROGMEM) | High (need to split/componentize, not copy verbatim — existing is oversize) | Low |
| 6 | Mesh network (Opera / ESP-NOW) body | ⚠️ header only in canary lib | `canary_wap/mesh_network.{h,cpp}` (+ now-fixed RSSI wiring) | Medium | Medium (MAC hygiene, peer pairing state) |
| 7 | BLE discovery (Opera / Chirp / Nearby) | ❌ | `canary_wap/ble_*` headers | Medium (Bluetooth compile is trimmed in current canary sdkconfig — must be feature-flagged per CVE-2025-27840) | High (CVE-2025-27840) |
| 8 | RF presence detection | ❌ | `canary_wap/rf_presence.{h,cpp}` | Medium | Low |
| 9 | Chirp channel body | ⚠️ header only | `canary_wap/chirp_channel.cpp` (+ now-fixed real RSSI) | Medium | Low |
| 10 | Hardware state & safe mode | ❌ | `canary_wap/hardware_state.h` (+ now-fixed SD flush) | Low | Medium (boot safety) |
| 11 | Provisioning gate (BOOT button) | ❌ | `canary_wap/*` gate logic | Low | High (unauthenticated `/api/provisioning-receipt`) |
| 12 | Audible chirp / buzzer alerts | ❌ | `canary_wap/audible_chirp.h` | Low | Low |
| 13 | WiFi presence detection | ❌ | `canary_wap/wifi_presence.h` | Medium | Medium (MAC hygiene) |
| 14 | System monitor (temp / heap / PSRAM) | ❌ | `canary_wap/sys_monitor.h` | Low | Low |
| 15 | Log acknowledgment system + categories | ⚠️ partial | `canary_wap/health_log.h` + `log_level.h` | Medium | Low |

---

## Phased Roadmap

Phases are ordered by **security impact first**, then **blast radius**, then **reviewability**. Each phase is a separate PR so reviewers can approve one piece at a time.

### Phase 1 — API auth primitives (this PR)

- Create `firmware/canary/lib/securacv_auth/` with the auth primitives ported from `canary_wap/api_auth.h`:
  - Constant-time compare
  - Exponential-backoff lockout state machine
  - Token redaction helper
  - `api_auth_check` / `api_auth_check_optional` for `esp_http_server`
- Library is **not yet wired** into `securacv_network` — primitives first, integration review second.
- `library.json` ships independently so existing PlatformIO builds ignore it until opted in.
- Update [FEATURES.md](../FEATURES.md) dashboard to mark gap #1 as ⚠️ (library present, handlers not yet wired).

### Phase 2 — Wire API auth into sensitive endpoints (this PR)

- ✅ Added `hmac_sha256()` + `derive_api_token()` to `securacv_crypto` using two-step HKDF (token-key-derive then transport-secret w/ STA MAC), with base62 rejection sampling — byte-for-byte parity with `canary_wap/canary_wap.ino:816-908`.
- ✅ Bearer credential lives entirely in `securacv_auth` (`auth_load_or_derive` + `auth_get_token`); the witness module only triggers derivation. Regression-check `Token isolation` rule enforces this boundary so the credential cannot enter chain payloads.
- ✅ `auth_gate()` helper in `securacv_network.cpp` wraps `auth_check()` and fails closed if the bearer is unprovisioned (returns 503 `not_provisioned`).
- ✅ Wired `auth_gate` into the 8 mutating handlers:
  - `POST /api/export`, `POST /api/reboot`
  - `POST /api/logs/*/ack`, `POST /api/logs/ack-all`
  - `POST /api/wifi/connect`, `POST /api/wifi/disconnect`
  - `POST /api/mqtt/config` (`FEATURE_HA_MQTT`)
  - `POST /api/ota` (`FEATURE_OTA_UPDATE`)
- Left unauthenticated for now: `GET /api/status`, `GET /api/chain`, `GET /api/logs`, `GET /api/wifi/status`, `GET /api/mqtt/status`, `GET /api/peek/*`, `GET /` — these still feed the embedded SPA which doesn't yet hold the bearer token.
- Gap #1 flipped to ⚠️ (mutating endpoints gated). Final ✅ flips when SPA token wiring lands.

### Phase 2.5 — SPA token wiring (this PR)

- ✅ `handle_ui` performs a one-shot byte-swap of `__CV_TOKEN__` in the HTML template with `auth_get_token()` before sending the SPA, so the rendered page carries the per-device bearer credential.
- ✅ SPA `api()` helper threads `Authorization: Bearer cv_…` into every `fetch()` call. Defensive guard skips the header if the placeholder survived (e.g. dev preview), so the server's fail-closed 503 surfaces cleanly.
- ✅ `auth_gate` wired into the 9 remaining SPA-driven read endpoints:
  - `GET /api/status`, `GET /api/chain`, `GET /api/logs`
  - `GET /api/wifi/status`, `GET /api/mqtt/status` (`FEATURE_HA_MQTT`)
  - `POST /api/peek/start`, `GET /api/peek/stream`, `POST /api/peek/stop`, `GET /api/peek/status` (`FEATURE_CAMERA_PEEK`)
- Carve-out still unauthenticated: `GET /` (the SPA shell itself — must be reachable to receive the token). `GET /api/wifi/scan` was later brought under `auth_gate` like its sibling WiFi endpoints — the setup wizard and SPA both send the bearer token, so the carve-out was never needed.
- Bearer credential remains confined to `securacv_auth`; `regression_check.sh` "Token isolation" rule still greps clean.
- Gap #1 flipped to ✅.

### Phase 3 — Provisioning gate + hardware state

- Port `hardware_state.h` safe-mode / shutdown FSM into a new `securacv_runtime` component.
- Port the BOOT-button provisioning gate so `/api/provisioning-receipt` is gated by a physical button press.
- Flip gaps #10 and #11.

### Phase 4 — Mesh + CSI mesh sensing v1 ✅

Delivered across PRs #465–#494 (ESP32 Mesh Sensing Design plan):

**securacv_mesh library** (`firmware/canary/lib/securacv_mesh/`):
- ✅ Ported from `canary-wap/mesh_network.cpp` (1540 LOC monolith → modular PIO lib)
- ✅ Ed25519 device auth, ChaCha20-Poly1305 AEAD, 6-char pairing code
- ✅ ESP-IDF 4.x / 5.x compat via `ESP_IDF_VERSION_MAJOR` guards
- ✅ Opera-authenticated envelope: HEARTBEAT, CSI_FEATURES, TAMPER_ALERT, POWER_ALERT, OFFLINE_IMMINENT, WITNESS_RECORD, BEACON_EVENT, CHANNEL_LOCK, HUB_ELECTION
- ✅ NVS persistence for opera_secret + trusted-peer pubkeys (flash-encryption gated)
- ✅ 9 host test suites in CI (crypto, envelope, pairing, session, transport, beacon, state, channel_hop, hub_election)

**BLE Scout** (`firmware/canary/lib/securacv_ble_scan/`):
- ✅ Passive NimBLE scan, hashed MAC (HMAC-SHA256 per-device key), Kalman RSSI
- ✅ Paired-beacon registry with printable-ASCII label sanitization
- ✅ Broadcast hook → mesh via MPSC FreeRTOS queue (atomic pointer + CAS creation)
- ✅ 3 host test suites in CI (ble_scan, ble_scout_state, ble_scout_broadcast)

**CSI active probe** (`firmware/common/csi/src/csi_probe.{h,cpp}`):
- ✅ 50 Hz unicast ESP-NOW frames for deterministic CSI collection

**Multi-link fusion** (`firmware/common/csi/src/core_multilink_fusion.{h,cpp}`):
- ✅ 2-link confirmation gate, motion direction, breathing median

**Channel-hop coordination** (`mesh_channel_hop.{h,cpp}`):
- ✅ HopTracker: airtime >50% for 60s → next_channel (1→6→11→1)
- ✅ CHANNEL_LOCK signed broadcast, peers apply csi_hal::set_channel_lock()
- ✅ Coordinator election gates channel-hop to lowest-fingerprint live node

**Hub failover election** (`mesh_hub_election.{h,cpp}`):
- ✅ HubMonitor: Hub heartbeat absent 60s → deterministic election (lowest fingerprint wins)
- ✅ HUB_ELECTION signed broadcast, no voting protocol needed
- ✅ Coordinator role evaluates every 5s on peer state changes

**CSI watchdog + WiFi recovery**:
- ✅ 5s silence → CSI rx toggle (gentle); 3× consecutive → csi_hal::stop/start (escalation)
- ✅ PIO csi_hal shim in securacv_csi for full watchdog + channel-lock parity

**Empty-room auto-calibration** (`meta_empty_room_baseline.{h,cpp}`):
- ✅ 10-min baseline, quiet-hours triggered, NVS-persisted

**Multipath shimmer filter** (in `core_presence.cpp`):
- ✅ RSSI swing >8 dB without Doppler → reject as non-human

**Privacy conformance** (15 assertions in `csi_event_invariants_test.cpp`):
- ✅ No peer MAC in feature payloads, beacon MAC hashed, RSSI bucketed int8

**Canary-wap parity**: all wire formats, dispatch, and integration glue byte-synced.

Gap #6 flipped to ⚠️ (mesh body present, chirp + RF presence still header-only).
Gaps #8, #9 deferred to Phase 4b (chirp + RF presence bodies).

### Phase 5 — BLE discovery (feature-flagged)

- Port BLE Opera/Chirp/Nearby behind `FEATURE_BLE`, defaulting **off** per CVE-2025-27840.
- Add regression_check.sh assertion that `FEATURE_BLE` stays off in production profiles unless explicitly overridden.
- Flip gap #7.

### Phase 6 — Camera streaming + Web UI tabs

- Port camera peek streaming into `securacv_camera` with the same no-frame-storage guarantees.
- Split and port the web UI tabs into `securacv_webui` without re-introducing the oversize `web_ui.h` flagged by regression_check.sh.
- Flip gaps #3 and #5.

### Phase 7 — Remaining features

- GPS motion FSM (#4), audible chirp (#12), WiFi presence (#13), system monitor (#14), log acknowledgments (#15).

### Phase 8 — Sunset the Arduino IDE path

- Re-label `firmware/projects/canary-wap/` as **SUNSET** in `VARIANT_POLICY.md` for one release cycle.
- Publish migration guide in `firmware/README.md` pointing Arduino-IDE users at PlatformIO or Arduino-in-PIO.
- After one release cycle with SUNSET: move to `firmware/projects/_archive/canary-wap/` and apply the same `SECURACV_ALLOW_ARCHIVED_BUILD` gate + archive-guard CI check.
- Update the Feature-Parity Dashboard to collapse the Arduino and canary (PIO) columns.

---

## Cross-Cutting Rules

1. **No verbatim copy.** Each port re-implements with the target lib conventions — namespaces, class-based state, `canary_config.h` feature flags, `Serial.printf` → project logger.
2. **Keep `regression_check.sh` green.** Every port must pass the existing gates (hardcoded passwords, oversize web UI, mbedTLS `_ret`, PWDN=-1, SD SPI pins, BLE guarded).
3. **Feature flags default off during port-in.** New feature flags land `#define FEATURE_X 0` on first PR and are flipped on per-phase once wired.
4. **Tests land with the port.** Minimum: one compile-time smoke + one runtime sanity test per library.
5. **No un-archiving the snapshot.** All source references resolve to `firmware/projects/canary-wap/arduino/canary_wap/`.
6. **Dashboard is authoritative.** Each phase PR updates [FEATURES.md](../FEATURES.md) in the same commit that flips the state, so `main` is always consistent.

---

## Exit Criteria

Consolidation is **complete** when:

- All 15 gaps in the inventory are ✅ in the canary (PIO) column (or explicitly scoped out with a note).
- `firmware/projects/canary-wap/` is labeled ARCHIVED in [VARIANT_POLICY.md](../VARIANT_POLICY.md).
- `firmware/README.md` documents a single primary build path (`firmware/canary/` PlatformIO).
- `firmware/FEATURES.md` dashboard collapses to one column for WAP.
- CI `archive-guard` protects the newly-archived Arduino tree.

---

## See Also

- [../VARIANT_POLICY.md](../VARIANT_POLICY.md) — variant lifecycle labels and transitions
- [../FEATURES.md](../FEATURES.md) — feature-parity dashboard (authoritative progress tracker)
- [../FIRMWARE_VARIANT_AUDIT.md](../FIRMWARE_VARIANT_AUDIT.md) — per-variant risk analysis
- [../scripts/regression_check.sh](../scripts/regression_check.sh) — CI regression gate
