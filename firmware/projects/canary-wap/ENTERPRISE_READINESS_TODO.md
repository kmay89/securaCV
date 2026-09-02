# Canary WAP Enterprise-Readiness TODO

This checklist is based on a repository audit focused on:

- ESP32 Canary WAP firmware quality and onboarding UX
- Arduino-first build path, then PlatformIO parity
- Witness-kernel backbone readiness
- Home Assistant + Frigate + MQTT integration readiness

Reference baseline inventory: [`firmware/FIRMWARE_VARIANT_AUDIT.md`](../../FIRMWARE_VARIANT_AUDIT.md) (canonical-path and rot-risk analysis).

## 0) Where the toolchain checks actually run

The first version of this list recorded which tools were missing from the
audit *environment*. Those boxes could never be checked from inside the
repo, so they were replaced with the CI jobs that run each tool on every PR
(status is the job's, not a shell's):

- [x] Regression guard — `firmware/scripts/regression_check.sh` in `.github/workflows/firmware.yml`.
- [x] PlatformIO CLI — `firmware.yml` and `firmware-release.yml` install it and build every env in `flavors.json`.
- [x] Arduino CLI — the `setup-arduino-esp32` composite action, used by `firmware.yml` (compile gate on `arduino/canary_wap/canary_wap.ino`) and `firmware-release.yml`.
- [x] Witness-kernel full tests — `.github/workflows/rust.yml` installs `libseccomp` and runs `cargo test`; `fuzz.yml` and `detect-eval.yml` do the same for their targets.
- [x] HA/Frigate/MQTT pipeline — `docker-sidecar.yml` builds and smoke-tests the sidecar image; the end-to-end `verify_pipeline.sh` acceptance run is still manual (see §6).

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

- [x] **Codify Arduino build matrix in CI** — `firmware.yml` compiles
  `arduino/canary_wap/canary_wap.ino` through the `setup-arduino-esp32`
  composite action on every PR; `firmware-release.yml` publishes from the same
  matrix. Profiles are the `CANARY_PROFILE_*` defines the sketch reads.

- [ ] **Stabilize Arduino dependency pinning**
  - Lock tested ESP32 core version and library versions in docs + CI scripts.
  - Add explicit compatibility notes for NimBLE/ArduinoJson/Crypto versions.

- [ ] **Split oversized `web_ui.h` payload**
  - Break monolithic PROGMEM UI into chunked assets (html/css/js) to reduce compile/link risk.
  - Add memory-size budget checks during build.

- [x] **Add static analysis pass that actually runs in CI** — `cppcheck` runs
  in `firmware.yml` and `csi_module_disable_matrix.yml` and fails the job on
  its findings.

---

## 4) PlatformIO parity and promotion path

- [ ] **Match Arduino and PlatformIO feature behavior**
  - Validate feature-flag parity (WiFi AP/STA, MQTT, camera peek, SD, BLE, mesh).
  - Build a parity checklist and run through each release candidate.

- [ ] **PlatformIO environment hardening**
  - Keep `dev`, `release`, `dev_ha`, `release_ha`, `minimal` envs green.
  - Add size, RAM, and boot-time budgets per environment.

- [x] **Release artifacts** — `firmware-release.yml` emits binaries, checksums
  and the Ed25519-signed manifest (refusing to publish without the key).
- [ ] **SBOM** — `.github/workflows/sbom.yml` generates the Rust and Node
  inventories; the firmware SBOM is still a hand-written component list that
  no longer matches the build files it cites (`docs/IMPROVEMENT_ROADMAP.md`
  item 35).

---

## 5) Witness-kernel backbone readiness

- [x] **Fix host dependency gap for CI/test runners** — `rust.yml`, `fuzz.yml`
  and `detect-eval.yml` install `libseccomp-dev` before building; the
  `Dockerfile` at the repo root is the containerized target.

- [ ] **Required kernel quality gates in CI** (`rust.yml`)
  - [x] `cargo test`
  - [x] `cargo clippy --all-targets -- -D warnings`
  - [ ] `RUSTDOCFLAGS="-D warnings" cargo doc --no-deps`
  - [x] `--no-default-features` build (note: `Cargo.toml` defines no `default` feature, so this is the same build as the default one)
  - [x] `--features backend-tract` build

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
- [x] Arduino build path compiles in CI for documented profiles.
- [x] PlatformIO release environments compile (smoke tests on hardware are still manual — `docs/V1_BENCH_TEST_RUNBOOK.md`).
- [x] Witness-kernel tests/clippy pass in CI (`rust.yml`); the doc gate is tracked above.
- [ ] HA+Frigate+MQTT verification script passes end-to-end.
- [ ] Security/privacy regression checks return zero critical warnings for release builds.

