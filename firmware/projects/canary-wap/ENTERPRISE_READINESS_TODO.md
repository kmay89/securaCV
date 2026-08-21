# Canary WAP Enterprise-Readiness TODO

This checklist is based on a repository audit focused on:

- ESP32 Canary WAP firmware quality and onboarding UX
- Arduino-first build path, then PlatformIO parity
- Witness-kernel backbone readiness
- Home Assistant + Frigate + MQTT integration readiness

Reference baseline inventory: [`firmware/FIRMWARE_VARIANT_AUDIT.md`](../../FIRMWARE_VARIANT_AUDIT.md) (canonical-path and rot-risk analysis).

## 0) Current Snapshot (from this audit)

- [x] Regression guard script runs and passes with warnings (`firmware/scripts/regression_check.sh`).
- [ ] PlatformIO CLI available in CI/dev shell (`pio` missing in this environment).
- [ ] Arduino CLI available in CI/dev shell (`arduino-cli` missing in this environment).
- [ ] Witness-kernel full tests passing in this environment (`cargo test` currently blocked by missing `libseccomp`).
- [ ] HA/Frigate/MQTT pipeline verification runnable in this environment (`docker compose` missing).

---

## 1) Critical security/privacy fixes before enterprise rollout

- [x] **Eliminate fallback AP password from production paths**
  - [x] Replace the static legacy fallback password with a mandatory device-unique generated credential.
  - [x] Remove the fallback define entirely (`AP_PASSWORD_DEFAULT` deleted from `canary_config.h`;
        `ScvNetworkManager::begin()` / `network_init()` now require an explicit password), and
        upgrade `regression_check.sh`'s probe for the old literal from warn to **fail**.
  - [x] Update docs to remove any default-password onboarding language.

- [x] **Stop exposing raw MAC addresses in WAP APIs/logs** *(canary-wap arduino; F-03)*
  - [x] Raw efuse/`WiFi.macAddress()` MAC replaced with a salted pseudonym (`device_pseudonym` —
        SHA-256 of a per-device NVS salt + MAC) in the diagnostics JSON/serial, device-info and
        provisioning-receipt APIs, the provisioning + boot serial banners, and the web UI
        (`Hardware ID`). Pure derivation host-tested (`tests_host/test_device_pseudonym.cpp`).
  - [x] `regression_check.sh` now **fails** if the device efuse MAC is formatted as a raw MAC string
        or if `WiFi.macAddress()` feeds a payload/log in the audited project.
  - [x] _Follow-up closed:_ all firmware trees now adopt the shared, salted, MAC-free
        `device_pseudonym` helper (`firmware/common/identity/device_pseudonym.h`).
        `canary-vision/src` uses it for the boot-banner "Hardware ID" and the MQTT client-ID
        suffix (the efuse MAC no longer reaches the broker); `canary-wap/src` no longer reads the
        MAC. `regression_check.sh` now **hard-fails** on raw MAC in *any* tree (not just the
        audited arduino project). Host-tested via `tests_host/test_device_pseudonym_common.cpp`.

- [x] **Coarsen operator-visible GPS precision** *(canary-wap arduino; F-03)*
  - [x] All operator-facing lat/lon (CBOR telemetry, `/gps` JSON, status/record serial logs) routed
        through `gps_coarsen_deg()` (3 dp ≈ 110 m); high-precision (`%.7f`) coordinate formats removed.
  - [x] `regression_check.sh` now **fails** on un-coarsened lat/lon emission and on high-precision
        (≥4 dp) coordinate format strings in the audited project.
  - [x] _Follow-up closed:_ `firmware/canary/src` now routes all operator-facing lat/lon (CBOR
        telemetry, status JSON, GPS/health serial logs) through the shared `gps_coarsen_deg()`
        (`firmware/common/gnss/gps_privacy.h`, 3 dp ≈ 110 m). `regression_check.sh` now hard-fails
        on un-coarsened lat/lon in *any* tree. Host-tested via `tests_host/test_gps_coarsen.cpp`.

- [x] **Constrain outbound behavior to explicit opt-in**
  - [x] AP-only default mode kept: the MQTT bridge's NVS `enabled` flag
        defaults to `false` (`csi_mqtt::config_load`) — a fresh device
        publishes nothing.
  - [x] Explicit user action required: STA credentials and the broker
        config each arrive only via authenticated POSTs from the owner.
  - [x] Outbound state highly visible: the headline dashboard's Today
        sheet carries a sharing pill — "Sharing: off — nothing leaves
        unless you turn it on" vs "Sharing: on — connected to <host>"
        (warm-tinted whenever a sharing path is enabled), backed by
        `/api/mqtt/config`'s `enabled`/`connected`/`host` fields.

---

## 2) Onboarding UX (router-like, nontechnical friendly)

- [x] **First-run onboarding wizard** on device captive portal
  > **Status reconciliation (2026-06-10, second pass):** the structured flow now
  > lives in the `/companion` PWA wizard (`companion_pwa.h`), which the captive
  > portal hands every first-run user to.
  - [x] Step 1: Device identity + trust explanation — collapsed "What your Canary
        does" primer on the wizard welcome step (three plain-language promises).
  - [x] Step 2: ~~Set owner password / admin passphrase~~ **Decided against a
        separate owner password.** The device already has three layers that fill
        this role: the AP password (unique per device, in the recovery kit), the
        per-device Bearer API token, and the physical BOOT-tap gate for the
        receipt. A fourth credential would add a storage/verification/reset
        surface to a security product without a designed reset path — if this
        is revisited, it needs its own security review first.
  - [x] Step 3: Connect to home WiFi — the wizard's original steps 2–4 (scan →
        password → connect), plus QR-code capture fallback.
  - [x] Step 4: Configure MQTT broker — optional "Use Home Assistant?" block in
        the wizard close-out chain (`/api/mqtt/config` + `/api/mqtt/test`,
        unlocked by the recovery-kit session cookie).
  - [x] Step 5: Verify health and save recovery kit — pre-flight checks
        (`/api/selftest`) plus the "Save your recovery kit" block, which walks
        the BOOT-tap gate and downloads the provisioning receipt JSON.

- [ ] **Simple status language**
  - Replace technical-only labels with plain-language status text + “Advanced details” expanders.
  - Add “Good / Needs attention / Action required” health summary strip.

- [ ] **Recovery-safe flows**
  - Guided factory-reset confirmation UX (with explicit data-loss warning).
  - Credential reset path without requiring serial monitor for normal users.

- [x] **Provisioning confirmation artifacts**
  - [x] Downloadable setup receipt — `/api/provisioning-receipt` (BOOT-tap gated),
        downloaded as `canary-recovery-kit.json` from the wizard close-out.
  - [x] QR code — `/api/pairing-qr` (device id, base URL, token; PR #768) shown
        via Settings › Device "Show QR".

---

## 3) Arduino-first compile and quality gate

- [ ] **Codify Arduino build matrix in CI**
  - Board targets: XIAO ESP32-S3 Sense + fallback ESP32S3 Dev Module profile.
  - Profiles: minimal/dev/full.
  - Include compile-only gate for `arduino/canary_wap/canary_wap.ino`.

- [ ] **Stabilize Arduino dependency pinning**
  - Lock tested ESP32 core version and library versions in docs + CI scripts.
  - Add explicit compatibility notes for NimBLE/ArduinoJson/Crypto versions.

- [ ] **Split oversized `web_ui.h` payload**
  - Break monolithic PROGMEM UI into chunked assets (html/css/js) to reduce compile/link risk.
  - Add memory-size budget checks during build.

- [ ] **Add static analysis pass that actually runs in CI**
  - Install `cppcheck` and run nontrivial ruleset for firmware directories.
  - Fail build on high-confidence defects.

---

## 4) PlatformIO parity and promotion path

- [ ] **Match Arduino and PlatformIO feature behavior**
  - Validate feature-flag parity (WiFi AP/STA, MQTT, camera peek, SD, BLE, mesh).
  - Build a parity checklist and run through each release candidate.

- [ ] **PlatformIO environment hardening**
  - Keep `dev`, `release`, `dev_ha`, `release_ha`, `minimal` envs green.
  - Add size, RAM, and boot-time budgets per environment.

- [ ] **Release artifacts and SBOM**
  - Emit firmware binaries + checksums + signed manifest.
  - Add dependency inventory for enterprise procurement/security review.

---

## 5) Witness-kernel backbone readiness

- [ ] **Fix host dependency gap for CI/test runners**
  - Provide reproducible environment with `libseccomp` for full test/link success.
  - Add containerized test target so contributors get consistent outcomes.

- [ ] **Required kernel quality gates in CI**
  - `cargo test`
  - `cargo clippy --all-targets --all-features -- -D warnings`
  - `RUSTDOCFLAGS="-D warnings" cargo doc --no-deps`
  - Feature gate checks from AGENTS policy (`--no-default-features`, `--features backend-tract`).

- [ ] **Backend audit trail**
  - Document audit results for each enabled detector backend.
  - Ensure all non-pure-Rust backends remain feature-gated.

---

## 6) Home Assistant + Frigate + MQTT backbone

- [ ] **Make integration runnable in one command**
  - Provide a single script to spin up broker + Frigate + HA and perform health checks.
  - Keep `integrations/ha_frigate_mqtt/verify_pipeline.sh` as final acceptance gate.

- [ ] **Device-to-broker contract tests**
  - Validate retained topics, QoS expectations, payload schemas, and reconnect behavior.
  - Verify HA entity discovery and Frigate event topic compatibility.

- [ ] **Operational failover behavior**
  - Confirm local witness recording continues when broker is down.
  - Buffer/retry strategy must not block witness chain generation.

- [ ] **User-friendly setup profile presets**
  - “Local-only (default)”
  - “Home Assistant + MQTT”
  - “Frigate bridge mode”
  - Each preset should describe privacy/network tradeoffs in plain language.

---

## 7) Suggested acceptance criteria (Definition of Done)

- [ ] Nontechnical user can unbox device and complete setup in <10 minutes with only phone browser.
- [ ] Arduino build path compiles in CI for documented profiles.
- [ ] PlatformIO release environments compile and pass smoke tests.
- [ ] Witness-kernel tests/clippy/doc pass in reproducible CI image.
- [ ] HA+Frigate+MQTT verification script passes end-to-end.
- [ ] Security/privacy regression checks return zero critical warnings for release builds.

