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
| 11 | Provisioning gate (BOOT button) | ✅ (2026-09, option D — maintainer to confirm: BOOT tap → one consumer, one `GET /api/provisioning-receipt` or one home-LAN page load, whichever asks first; the dashboard's bearer token is injected only for first-boot setup, a bearer-authenticated request, a SoftAP-subnet peer or a spent tap, never for a bare home-LAN load; CI-compiled on #1704, no bench pass; the WAP's session-cookie model is the Phase 6 follow-up) | `canary_wap/*` gate logic → `firmware/common/network/provisioning_gate.h` | Low | High — corrected: the canary never had an unauthenticated receipt route (it had none at all); the real exposure was `GET /` and `GET /setup` putting the bearer token in the HTML for any home-LAN caller |
| 12 | Audible chirp / buzzer alerts | ❌ | `canary_wap/audible_chirp.h` | Low | Low |
| 13 | WiFi presence detection | ❌ | `canary_wap/wifi_presence.h` | Medium | Medium (MAC hygiene) |
| 14 | System monitor (temp / heap / PSRAM) | ❌ | `canary_wap/sys_monitor.h` | Low | Low |
| 15 | Log acknowledgment system + categories | ⚠️ partial | `canary_wap/health_log.h` + `log_level.h` | Medium | Low |
| 16 | Committed CSI events over MQTT (`events` topic, signed, + `tamper` bridge), SD event log + reconnect backfill | ⚠️ live publish + Ed25519 signing (`src/csi_event_egress.cpp`, the shared `common/csi/src/csi_event_wire.h` body), with the key on MQTT health as `public_key` for Home Assistant's first-sight pin; SD event log + reconnect backfill (F37): `/EVENTS/today.ndjson` in the canary-wap's line format (`common/csi/src/csi_event_log_line.h`, now shared by both trees), written by a loop-task adapter under this tree's single-writer SD rule (`src/csi_event_log.cpp`, owner-bound to the witness key), replayed after the MQTT offline queue in id order, never below the delivered watermark HA's replay gate keys on (`common/csi/src/csi_event_backfill.h`, host-tested against a model of that gate); CI compile only, bench pending (an outage longer than the 12-slot queue, and a reboot inside one) | `canary_wap/csi_event_log.{h,cpp}` — ported | Medium | Low |

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

- ✅ `handle_ui` performs a one-shot byte-swap of `__CV_TOKEN__` in the HTML template with `auth_get_token()` before sending the SPA, so the rendered page carries the per-device bearer credential. *(Since gap #11, 2026-09: only when `provisioning_gate.h`'s `page_token_policy` grants it — see Phase 3 below; a home-LAN load with no grant gets an empty token.)*
- ✅ SPA `api()` helper threads `Authorization: Bearer cv_…` into every `fetch()` call. Defensive guard skips the header if the placeholder survived (e.g. dev preview), so the server's fail-closed 503 surfaces cleanly.
- ✅ `auth_gate` wired into the 9 remaining SPA-driven read endpoints:
  - `GET /api/status`, `GET /api/chain`, `GET /api/logs`
  - `GET /api/wifi/status`, `GET /api/mqtt/status` (`FEATURE_HA_MQTT`)
  - `POST /api/peek/start`, `GET /api/peek/stream`, `POST /api/peek/stop`, `GET /api/peek/status` (`FEATURE_CAMERA_PEEK`)
- Carve-out still unauthenticated: `GET /` (the SPA shell itself — must be reachable to receive the token; since the 2026-09 security sweep the token is injected only for a `Host` that names this device — see "Later additions" below). `GET /api/wifi/scan` was later brought under `auth_gate` like its sibling WiFi endpoints — the setup wizard and SPA both send the bearer token, so the carve-out was never needed.
- Bearer credential remains confined to `securacv_auth`; `regression_check.sh` "Token isolation" rule still greps clean.
- Gap #1 flipped to ✅.

### Later additions to the gated set

- 2026-09 (broker TLS, roadmap row 12): `POST /api/mqtt/ca` and
  `DELETE /api/mqtt/ca` (`FEATURE_HA_MQTT`) joined the mutating handlers
  behind the same `auth_gate()` + `rate_limit_check(req, true)` pair, and
  `POST /api/mqtt/config` gained optional `tls` / `fp` fields. The CA route
  reads a raw PEM body into one static buffer bounded to the shared
  `kCaPemMax` and never echoes it; `GET /api/mqtt/status` reports the
  transport decision and presence flags only. What may be written is judged
  by `lib/securacv_mqtt/src/mqtt_tls_fields.h` (host-tested in
  `firmware/tests_host/test_mqtt_tls_fields.cpp`) with the shared decision
  header, so the API refuses exactly what the connect would. One request's
  writes land in one NVS session (`mqtt_save_config`: the pin, the mode
  byte, then the credentials — `mqtt_tls_fields::write_order`) with one
  main-loop reload after it, never a reload between two writes. Every
  route registration now goes through `register_route()`, which names a
  registration the handler table dropped on the serial log, and the
  `max_uri_handlers` budget counts the dev-only `POST /api/ota`.
- 2026-09 (security sweep, roadmap row 12 follow-up): three hardenings of
  the same API, each decided in a pure header and host-tested, with the
  HTTP glue compile-tested by CI's `release_ha` leg only (no bench pass).
  - **A stored broker password never follows the link to a new endpoint.**
    `POST /api/mqtt/config` always rewrote host / port / enabled but kept a
    stored username / password the body omitted, so `{"host":"<elsewhere>"}`
    from any bearer holder repointed the link AND sent the household's
    broker password to the new host in the next CONNECT packet. The rule is
    `mqtt_tls_fields::credential_carry` (host-tested): an endpoint is host +
    port (host trimmed and case-insensitive); the **same** endpoint keeps
    whatever the body omitted (a `/setup` re-run that changes only the
    password, or only the username, works as before); a **new** host or
    port carries nothing — the stored username and password are removed
    unless the body supplies them again (`write_credentials` removes with
    `nvs.isKey` → `nvs.remove`, checked like every other write, inside the
    one write session, still pin → mode → credentials) — and is **refused
    before any write** with `400 {ok:false, error:"password_required_for_new_host",
    reason:…}` when a password is stored and the body gives none. A fresh
    unit, or a row with no stored password, moves freely. A port-only change
    (the wizard's *use 8883* button on a unit that already holds a password)
    therefore asks for the password again — deliberate; the reason sentence
    names the password box. The setup page does **not** re-implement the
    rule: it renders the API's `reason` (pinned by the page test), and
    because it posts the CA **before** the config, a refused config can
    leave a freshly uploaded CA stored — harmless (the credential row is
    untouched; the retry re-sends the CA).
  - **The `Host` a request targeted must name this device.** No canary route
    checked it, so a DNS-rebinding page (a public domain re-pointed at the
    Canary's LAN address) loaded `GET /` same-origin, read `__CV_TOKEN__`
    out of the HTML and could drive every gated route, the two broker
    writers included. The display's `host_guard.h` now lives at
    `firmware/common/network/host_guard.h` (one header, one host test in
    `firmware/tests_host`), and `securacv_network` asks it first on every
    path that can hand out the token or spend the BOOT tap: the page-token
    decision serves the page with an **empty** token for a foreign Host
    before any grant is read or the tap is taken (the page's fetch helper
    then sends no `Authorization`, so the failure shows on the dashboard
    rather than as a blank 403), `auth_gate` answers `403 {"error":"host"}`
    before the token compare, and `GET /api/provisioning-receipt`, whose
    gate is a bearer or the tap rather than `auth_gate`, answers the same
    `403 {"error":"host"}` before either is consulted (2026-09 follow-up:
    the receipt route refuses a foreign Host like every other token-bearing
    route, and a page load under a foreign Host no longer spends the BOOT
    tap; `provisioning_gate.h`'s `page_token_decide` and `receipt_decide`
    take the Host verdict first, host-tested; the receipt handler sends the
    receipt only on an explicit `SERVE_BEARER` / `SERVE_TAP` verdict and
    answers every other one with the Host refusal, so it fails closed; and
    `scripts/check_route_security.py` holds every token path to that order
    and the receipt handler to that shape).
    A missing or oversize Host is foreign. **One exemption, by interface, never
    by name:** a request that arrived over the Canary's own softAP (local
    address = the AP address and the peer in the AP subnet). The captive DNS
    redirector runs for the AP's lifetime and answers every non-`.local` name
    with the AP address, so the phone's captive sheet loads the wizard under
    its OS's probe name (`captive.apple.com`) and calls the API under it —
    the hub step included, after `setup_mark_complete()` has fired. Over the
    AP that Host is this device by construction, and a rebinding page has
    nowhere to load from. **The trade, the same one the display took
    (`glass_web.cpp`, note 2b):** a household that reaches the Canary by a
    public split-horizon name (`canary.example.com` → a LAN address) gets the
    tokenless dashboard and 403 on the API, and must use the IP, the
    `.local` name, a single label or a private-suffix alias (`.lan`,
    `.internal`, `.home.arpa`) instead. Chrome's Local Network Access
    prompt narrows the attack on its own; Firefox and Safari do not.
  - **An honest CA verdict.** `mqtt_tls_read_current` reported `ca_set` from
    `isKey` alone, while the transport's `load()` reads a stored CA longer
    than its buffer back as empty — so the API called a CA-verified mode Ok
    for a unit whose connect would refuse `CaMissing`. The reader now sets
    `ca_set` only when the stored CA fits the same 3072-byte buffer and
    `ca_unreadable` for a key it cannot read back; `plan()` refuses a
    CA-verified mode over an unreadable CA with `Verdict::CaUnreadable` →
    `409 {ok:false, error:"ca_unreadable", reason:"… DELETE /api/mqtt/ca
    and upload it again"}`, and `GET /api/mqtt/status` reports
    `ca_unreadable:true` (absent otherwise). Reachable only through a
    third-party NVS writer — both flashers and the API cap at 3071 bytes.

### Phase 3 — Provisioning gate + hardware state

- Port `hardware_state.h` safe-mode / shutdown FSM into a new `securacv_runtime` component. **Open** (gap #10).
- ✅ 2026-09 (gap #11, option D — maintainer to confirm): the BOOT-button
  provisioning gate. The gate itself is the pure header
  `firmware/common/network/provisioning_gate.h` (host-tested in
  `firmware/tests_host/test_provisioning_gate.cpp`: one tap admits exactly one
  consumer through an atomic exchange, a 30 s TTL, the `millis()==0` sentinel,
  uint32 wraparound). `main.cpp` opens it on a short BOOT tap and hands
  take/peek hooks to the network lib. `GET /api/provisioning-receipt` answers
  a valid bearer, or consumes one tap; otherwise 403
  `physical_confirmation_required` with the TTL (the WAP's receipt shape, the
  one the iOS app parses). The same header's `page_token_policy` decides per
  request whether `/` and `/setup` carry the bearer token: first-boot setup,
  bearer-authenticated, a peer inside the live SoftAP subnet (IPv4, or the
  IPv4-mapped `::ffff:a.b.c.d` the dual-stack httpd socket reports;
  conservative), or an unspent BOOT tap, which the page load then takes —
  one tap is one consumer across both paths, a page load or a receipt
  fetch, whichever asks first (`page_token_decide`). A home-LAN load with
  none of those gets the page with an empty token, `X-CV-Token: withheld`,
  and a banner naming the three unlocks. `firmware/canary/scripts/check_route_security.py`
  (in `firmware.yml`) now fails any route that reaches no credential gate and
  is not on its documented public allowlist.
- **Follow-up (Phase 6, with the web UI port):** the WAP's one-shot pair token
  + 24 h HttpOnly `cv_session` cookie (option A), so a home-LAN reload stops
  needing a fresh BOOT tap each time.
- Flip gap #10 when the runtime component lands; gap #11 is flipped.

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
- ✅ PIO build compiles the canonical `csi_hal.cpp` directly (roadmap 22); `lib/securacv_csi` is a thin `csi::` adapter. The watchdog now actually runs on the PIO build — the former shim only checked it inside a `csi_hal::process()` nobody called

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
