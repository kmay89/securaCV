# Canary WAP Enterprise-Readiness TODO

This checklist is based on a repository audit focused on:

- ESP32 Canary WAP firmware quality and onboarding UX
- Arduino-first build path, then PlatformIO parity
- Witness-kernel backbone readiness
- Home Assistant + Frigate + MQTT integration readiness

Reference baseline inventory: [`firmware/FIRMWARE_VARIANT_AUDIT.md`](../../FIRMWARE_VARIANT_AUDIT.md) (canonical-path and rot-risk analysis).

> **Status reconciliation (2026-09-23).** Every box that was still open was
> re-checked against the tree. A box is ticked only when the claim is proven
> by a host test, a lint, or a CI compile, and the evidence is named under
> it; stale boxes whose work landed elsewhere are ticked with that pointer.
> Three boxes can only be closed by a person with hardware or Docker and are
> labeled **U1 / human** so they stop reading like code debt. What is left
> open is either its own change (named) or waiting on a maintainer decision
> (named).

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

- [x] **Simple status language**
  - [x] Plain-language status text + “Advanced details” expanders: the
        headline dashboard (`csi_dashboard_html.h`) already keeps the live
        numbers behind collapsed `<details>` ("How is it sensing?", "Details"),
        and every user-facing string lives in its `COPY` bank, gated by
        `firmware/scripts/microcopy_lint.sh` (banned jargon, tooltip coverage,
        reading grade).
  - [x] “Good / Needs attention / Action required” strip (2026-09): the
        verdict is decided by `arduino/canary_wap/status_tier_logic.h`
        (worst-first, one reason code per tier, a missing card is not a fault;
        host-tested in `tests_host/test_status_tier_logic.cpp`), served as
        `status_tier` / `status_reason` on `GET /api/status`, and rendered
        under the topbar from `COPY.tier` (`web_assets_gz.h` regenerated).
        Compile proof: the `firmware.yml` Arduino CLI build; not yet seen on a
        device. The three labels are the wording this checklist asked for —
        changing them is a maintainer's call and a `COPY` edit only.

- [ ] **Recovery-safe flows**
  - [ ] Guided factory-reset confirmation UX (with explicit data-loss warning).
        **Open — its own change, blocked on a maintainer decision:** today the
        WAP has no factory-reset route at all (BOOT held ≥ 3 s is the only
        path). Whether `POST /api/factory-reset` may exist is the decision;
        the recommendation is yes, but only behind the Bearer credential AND
        a BOOT-tap `provisioning_gate_take()` (a remote credential alone must
        never be able to wipe a witness), with a two-step confirmation in the
        companion PWA, `total_handlers` raised, and both route audits kept
        green.
  - [x] Credential reset path without the serial monitor: one BOOT tap
        releases the provisioning receipt (`GET /api/provisioning-receipt`,
        one tap = one fetch), which re-reveals the API token and the AP
        password; the wizard's "Save your recovery kit" block walks it.

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

- [x] **Stabilize Arduino dependency pinning**
  - [x] Tested core + libraries locked: `arduino/canary_wap/sketch.yaml` pins
        `esp32:esp32 (3.3.8)` and lists ArduinoJson / Crypto / NimBLE-Arduino;
        the firmware SBOM (`scripts/gen_firmware_sbom.py --check --validate`,
        `lint.yml`) asserts the pins against the workflows' rows.
  - [x] Compatibility notes: `README.md` "Install Libraries" names
        ArduinoJson 7.x, Crypto, and NimBLE-Arduino **2.3.8 or later** (1.x
        compiles with Bluetooth silently disabled).
  - Residual, on purpose: the WAP's Arduino-CLI CI rows build on the weekly
    "latest" core while the sketch pins 3.3.8 —
    [`firmware/PLATFORMS.md`](../../PLATFORMS.md) records it as a release
    decision for the maintainer, not a lint's call.

- [x] **Split oversized `web_ui.h` payload** — superseded by the gzip
  shipping model.
  - [x] The raw PROGMEM literals (`web_ui.h`, `csi_dashboard_html.h`,
        `companion_pwa.h`) are compiled OUT; the device serves the generated,
        byte-gated `web_assets_gz.h` (`gen_web_assets_gz.py --check` in
        `firmware.yml`; `regression_check.sh` reports "shipped gzip").
  - [x] Size budgets: `firmware/flavors.json` `size_guards` (the 0x330000 OTA
        slot) and the Arduino CLI job's "Check binary size against the OTA
        slot" step in `firmware.yml`.

- [x] **Add static analysis pass that actually runs in CI** — `cppcheck` runs
  in `firmware.yml` and `csi_module_disable_matrix.yml` and fails the job on
  its findings.

---

## 4) PlatformIO parity and promotion path

- [x] **Match Arduino and PlatformIO feature behavior**
  - [x] Feature-flag parity is structural now: `platformio.ini` sets
        `src_dir = arduino/canary_wap`, so `pio run` and `arduino-cli compile`
        build the same files (`firmware/FEATURES.md` 2026-09-05 note — the two
        canary-wap columns collapsed into one).
  - [x] Parity checklist: the `FEATURES.md` dashboard plus
        [`firmware/PARITY_PLAN.md`](../../PARITY_PLAN.md) §4. The live parity
        debt is `canary (PIO)` ↔ canary-wap (backlog F20), not Arduino ↔
        PlatformIO.

- [ ] **PlatformIO environment hardening**
  - [x] Envs green: every `firmware/flavors.json` `build_envs` entry is built
        by `firmware.yml` on every PR.
  - [x] Size and RAM budgets: `flavors.json` `size_guards` (per-slot) and
        `.github/workflows/ram_audit.yml`.
  - [ ] Boot-time budget per environment — **U1 / human**: needs a board on
        the bench and a stopwatch-grade serial capture; no CI runner can
        measure it.

- [x] **Release artifacts** — `firmware-release.yml` emits binaries, checksums
  and the Ed25519-signed manifest (refusing to publish without the key).
- [x] **SBOM** — `.github/workflows/sbom.yml` generates the Rust and Node
  inventories; the firmware SBOM is now generated and committed
  (`scripts/gen_firmware_sbom.py`, checked with `--check --validate` in
  `lint.yml`; [`sbom/README.md`](../../../sbom/README.md)) — the hand-written
  list this box described is gone (`docs/IMPROVEMENT_ROADMAP.md` item 35).

---

## 5) Witness-kernel backbone readiness

- [x] **Fix host dependency gap for CI/test runners** — `rust.yml`, `fuzz.yml`
  and `detect-eval.yml` install `libseccomp-dev` before building; the
  `Dockerfile` at the repo root is the containerized target.

- [x] **Required kernel quality gates in CI** (`rust.yml`)
  - [x] `cargo test`
  - [x] `cargo clippy --all-targets -- -D warnings`
  - [x] `RUSTDOCFLAGS="-D warnings" cargo doc --no-deps --document-private-items` (`rust.yml` build job, after clippy)
  - [x] `--no-default-features` build (note: `Cargo.toml` defines no `default` feature, so this is the same build as the default one)
  - [x] `--features backend-tract` build

- [ ] **Backend audit trail** — **open, its own change** (lives in the
  kernel, not this firmware): a `docs/security/backend_audit.md` row per
  detector backend (the default pure-Rust path; `backend-tract` behind its
  cargo feature) and a `cargo test` asserting every non-pure-Rust backend is
  feature-gated.
  - Document audit results for each enabled detector backend.
  - Ensure all non-pure-Rust backends remain feature-gated.

---

## 6) Home Assistant + Frigate + MQTT backbone

- [ ] **Make integration runnable in one command**
  - [x] Single script: `integrations/ha_frigate_mqtt/up.sh` (2026-09) checks
        the broker password file and `.env`, builds and starts the stack,
        waits for 1883 / 5000 / 8123, then runs `verify_pipeline.sh`.
        Shellcheck-clean (`docker-sidecar.yml` lint job).
  - [x] `verify_pipeline.sh` stays the final acceptance gate (`up.sh` ends in it).
  - [ ] First real run on a Docker host — **U1 / human** (no runner here
        starts Frigate + Home Assistant).

- [ ] **Device-to-broker contract tests**
  - [x] Frigate event topic compatibility: `cargo test --test frigate_mqtt_e2e`
        (`tests/frigate_mqtt_e2e.rs`) and `ci_smoke.sh` against a live broker
        (`rust.yml`).
  - [x] Payload schemas on the Home Assistant side:
        `custom_components/securacv/tests/test_mqtt_payload_hardening.py`
        (18 cases, stubbed HA).
  - [ ] **Open, its own change:** a firmware-side fixture — the WAP
        `csi_mqtt.cpp` discovery / retained payload templates extracted into a
        pytest fixture checked against the HA parsers; and QoS written down as
        the delivery bound (QoS 0 everywhere today). Reconnect behavior is
        bench (U1).

- [ ] **Operational failover behavior**
  - [ ] Confirm local witness recording continues when the broker is down —
        bench (**U1**): a live broker-down run.
  - [x] Buffering must not block chain generation: the chain never calls into
        MQTT; the bounded offline queue (`firmware/common/mqtt/mqtt_offline_queue.h`)
        drops the oldest record instead of waiting, and
        `firmware/tests_host/test_mqtt_offline_queue.cpp` now pins it — 10 000
        pushes with nothing draining are all accepted, the queue stays at its
        bound, every overflow is counted, the newest survive in order. The WAP
        additionally backfills from the SD event log once the broker returns
        (`csi_mqtt.cpp`).

- [ ] **User-friendly setup profile presets** — **open, its own change,
  blocked on a maintainer decision:** what "Frigate bridge mode" means on the
  device (device-side vs hub-side) must be defined before the three presets
  (each a `/api/mqtt/config` body with plain-language tradeoff copy in the
  companion PWA close-out, microcopy lint + gzip regen) can be written.
  - “Local-only (default)”
  - “Home Assistant + MQTT”
  - “Frigate bridge mode”
  - Each preset should describe privacy/network tradeoffs in plain language.

---

## 7) Suggested acceptance criteria (Definition of Done)

- [ ] Nontechnical user can unbox device and complete setup in <10 minutes with only phone browser. — **U1 / human** (a timed usability run with hardware and a phone).
- [x] Arduino build path compiles in CI for documented profiles.
- [x] PlatformIO release environments compile (smoke tests on hardware are still manual — `docs/V1_BENCH_TEST_RUNBOOK.md`).
- [x] Witness-kernel tests/clippy pass in CI (`rust.yml`); the doc gate is tracked above.
- [ ] HA+Frigate+MQTT verification script passes end-to-end. — **U1 / human** (needs Docker + Frigate: `integrations/ha_frigate_mqtt/up.sh`). The automated halves already run in CI: `cargo test --test frigate_mqtt_e2e` and `ci_smoke.sh` (`rust.yml`).
- [x] Security/privacy regression checks return zero critical warnings for release builds. — `firmware/scripts/regression_check.sh --strict` counts every Security/Privacy warning as a failure; its four false-positive greps (camera PWDN, outbound, raw MAC, GPS format) now match real call shapes, the documented plaintext listeners and the display line's disclosed outbound paths sit in reviewed allowlists that fail when stale, and the strict run passes today. Wired into `firmware-release.yml` (a release refuses on it); PR CI keeps the advisory mode.

