# Feature gaps: one consolidated status list (2026-09)

**What this is.** A single page listing the open gaps found in a documentation
honesty pass on 2026-09-29, each with the kind of person or work that closes it.
It does not replace the ledgers it draws from; it says what is open *now*, checked
against the tree, and points back to the ledger that owns the detail. Where a
ledger and the tree disagree, the tree wins and the disagreement is a gap of its
own (section 7).

**Owner types.**

| Owner | Meaning |
|---|---|
| **code** | An engineer (human or AI) can finish it from a checkout; CI is the proof. |
| **bench** | Needs real hardware on a desk, a runbook and a captured artifact. Code may exist; the claim is not earned until the bench passes. |
| **human decision** | A maintainer call, or a human act (account, key ceremony, money, legal, paperwork) before code is written or a state can change. |
| **crypto review** | A maintainer's cryptographic review of a protocol or key-handling change. Implemented and host-tested does not mean reviewed. |

**How each row was checked.** The last column says `re-read` when this pass
opened the named file, workflow or manifest and the claim held, and `ledger`
when the row is carried from the named source and was not re-checked here (the
sweep's own rule: re-read the source before building on a line). Nothing below
was added from memory. `re-read` is a manual check, not a signature check: this document never uses "verified" in the AGENTS.md sense (an Ed25519 signature against a pinned key).

**Where the detail lives.** [`repo_todo_sweep_2026-09.md`](repo_todo_sweep_2026-09.md)
(IDs like F30, A18, W15), [`../../firmware/FEATURES.md`](../../firmware/FEATURES.md),
[`../../firmware/PARITY_PLAN.md`](../../firmware/PARITY_PLAN.md),
[`../../firmware/ESP32S3_OPTIMIZATION_ROADMAP.md`](../../firmware/ESP32S3_OPTIMIZATION_ROADMAP.md),
[`../../spec/canary_mesh_network_v0.md`](../../spec/canary_mesh_network_v0.md),
[`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md),
[`../NEXT_STEPS_2026-07.md`](../NEXT_STEPS_2026-07.md),
[`../../desktop-lab/MOBILE.md`](../../desktop-lab/MOBILE.md), and the website's
store, compare, download and ecosystem pages.

---

## 1. Default-build truth (the four claims this pass corrected)

The copy now states what a plain `cargo build` does. The rows below are what stays
open in *code* after the copy was made honest. The Status grid on the landing page
and in `docs/witness-kernel.html` was already correct: it is derived by
`tools/gen_kernel_status.py` and carried to the website's `kernel-status.json`, and
it reads RTSP as in progress and WASM sandboxing as planned. What was loose was the
prose around it.

| ID | Gap | Owner | Evidence | Checked |
|---|---|---|---|---|
| D1 | **WASM module sandboxing does not exist.** No wasm runtime (`wasmtime`, `wasmer`, `wasmi`) is a dependency. Modules run in a forked, seccomp-restricted child process per frame (`execute_sandboxed`), Linux only. That is process isolation, and the copy now says so. A real WASM sandbox is a large build plus a dependency that must pass the FR-14 gate. | code (large) + human decision on whether to build it at all (`docs/v1-roadmap.md` lists it as out of scope for v1) | `Cargo.toml` (no wasm crate); `tools/gen_kernel_status.py` `st_wasm`; `src/module_runtime/sandbox.rs`; `src/bin/witnessd.rs:470` | re-read |
| D2 | **RTSP decoders are not in the default build, and neither is any other live frame source for `witnessd`.** `Cargo.toml` has no `default` feature set; `rtsp-ffmpeg` and `rtsp-gstreamer` are opt-in, and so are `ingest-v4l2`, `ingest-esp32` and `ingest-file-ffmpeg`. The event-side binaries (`frigate_bridge` over MQTT, `grove_vision2_ingest` over serial) have no `required-features` and do build by default; they ingest events, not video. A plain build bails on a real `rtsp://` URL ("RTSP requires the rtsp-gstreamer or rtsp-ffmpeg feature"); only `stub://` gives synthetic frames. The Docker image and the Home Assistant app build with `rtsp-gstreamer`. Decision needed: turn one on by default (which pulls system libraries into every build) or keep documenting the flags. | human decision, then code | `Cargo.toml` `[features]`; `src/ingest/rtsp.rs:90-137`; `Dockerfile:31`; `privacy_witness_kernel/Dockerfile:83` | re-read |
| D3 | **The GStreamer RTSP path is compile-gated in CI only; only ffmpeg has an end-to-end roundtrip.** | code (add a GStreamer e2e job) | `.github/workflows/rust.yml` (`ingest-gstreamer` compile gate; `rtsp_e2e` runs `--features rtsp-ffmpeg`) | re-read |
| D4 | **`detect.backend = "auto"` is frame-difference motion, not object detection.** `auto`, `stub` and `cpu` all resolve to the motion backends; `auto` never selects Tract. Real object classes need `--features backend-tract`, a model fetched by `scripts/fetch_detection_model.sh`, and `detect.backend = "tract"`. `witnessd` already logs this warning at startup; the README, config example and inference doc now say it too. Open: the accelerator backend is declared but unavailable. | code (accelerator) / bench (accuracy on real footage, no numbers may be published without a benchmark, per AGENTS.md rule 4) | `src/bin/witnessd.rs:294-345`; `src/frame.rs:281`; `docs/inference_backends.md` | re-read |
| D5 | **Hybrid PQ TLS needs `--features pqc-tls`.** Off by default; without the feature `MQTT_TLS_BACKEND=hybrid_pq` exits with an error. It applies to the MQTT clients (`frigate_bridge`, `event_mqtt_bridge`, `alert_relay`; `busybar_surface` always uses the classic default), not to the event API. CI runs clippy with `pqc-tls` but the test step runs only `pqc-signatures,pqc-vault`, so the hybrid handshake has no automated test and has never been run against a PQ-capable broker. The docs name `X25519Kyber768Draft00`; which group the pinned `rustls 0.23.45` / `aws-lc-rs 1.18.1` pair actually offers has not been confirmed. | crypto review (group name and fallback behavior) + bench (PQ broker interop) + code (a test) | `src/transport/tls.rs:61-70,222-290`; `.github/workflows/rust.yml:95-97`; `Cargo.lock` | re-read |
| D6 | **`ONVIF` was named in copy but no ONVIF code exists.** Copy now says "any camera that serves an RTSP stream" (README, FAQ, website; `docs/tvos/README.md` and the `docs/README.md` index line fixed in the follow-up to this doc). Open only if ONVIF discovery or control is wanted. | human decision | `grep -ri onvif src/` returns nothing | re-read |

---

## 2. Firmware

The ledger for defects is the roadmap's section 6; the ledger for parity is the
FEATURES.md dashboard. The sweep (`repo_todo_sweep_2026-09.md`) tracks most of these
by ID; the wave that closed F1-F5 and F33 left the rows below open.

| ID | Gap | Owner | Source | Checked |
|---|---|---|---|---|
| FW1 | **A/B rollback in the shipping builds is unconfirmed, not known-inert.** The revert net in `securacv_ota.cpp` is behind `CONFIG_APP_ROLLBACK_ENABLE` / `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Our own `sdkconfig.*` sets them only under `firmware/projects/canary-ota/`, but an Arduino build does not read those files: the pinned core ships a prebuilt sdkconfig, and #1746 reports that arduino-esp32 2.0.17's sets both options for esp32, esp32s3 and esp32c3 (core 3.3.8 checked against lib-builder master only). #1746 turns the silent `#if` into an `#error` so a core without the option fails the build, and wires the crash-loop decision layer (`common/health/boot_policy.h`, which had no boot-path consumer) into the PlatformIO `canary`. Still needs a deliberate bad-image flash (bench Track E) to close. | code (#1746, open) + bench | `NEXT_STEPS_2026-07.md` P0; #1746 | re-read (config and consumer search); the core's prebuilt config is #1746's finding, not re-read here |
| FW2 | **Roadmap 6: CSI stops under modem sleep; the probe is not wired into the canary PIO build.** (The airtime gate half landed as F4.) | code + bench | roadmap row 6 | ledger |
| FW3 | **Toolchain unification on Arduino-ESP32 core 3.x / IDF 5.x** (roadmap 9). The display's C6 env is on a core-3.x base (`canary_display_c6_core3`); the rest are not. Blocks WPA3-SAE SoftAP on the rest, the `i2s_pdm` / `rmt_rx` migrations (19), esp-dsp (20), and enabling `esp_pm` (12). | human decision (platform pin) + code | roadmap 9, 12, 19, 20; `FEATURES.md` | ledger |
| FW4 | **`esp_pm` is never enabled.** `set_cpu_freq()` takes the `#else` `setCpuFrequencyMhz()` branch unless `CONFIG_PM_ENABLE` is set, so the "power modes" run locked-frequency with no light sleep. | code + bench (idle mA) | roadmap 12 and section 1.4; `securacv_power_policy.cpp:157-181` | re-read (code path); the mA figures are unmeasured |
| FW5 | **Roadmap rows 10, 11, 14-16, 23-30 remain open**: dual-core task model, one 8 MB partition table plus `witness_log`, off-loop SD writes, PHY pinning, fast reconnect, camera SCCB standby and OV5640 tuning, graceful sleep teardown, ULP watcher, I2C RTC/IMU, co-signing implementation, ESP-NOW rate/LR/FTM, dead-code cleanup. Row 13 (SD SPI 4 to 20 MHz) has already landed and is stale (section 7). | code, most also bench | roadmap section 6 | ledger |
| FW6 | **Hardware key protection (roadmap 18 c and d).** A DS/HMAC-bound key (the DS route is RSA-only) and an eFuse/RTC rollback anchor. Every image reports `key_at_rest = plaintext-nvs`. | human decision + code + bench | sweep F38, F5 | ledger |
| FW7 | **Canary NVS write honesty.** canary-wap NVS helpers report success whatever the write did (F59), the session-balance check is textual (F60), 64 direct puts in the canary tree are unaudited (F61), and the canary identity-key store ignores `putBytes` (F58, halt versus loud ephemeral identity is a maintainer choice). | code; F58 human decision | sweep F58-F61 | ledger |
| FW8 | **No TLS on `canary.local` in release builds.** Dev/full CI-compiled, release pending the size-guard delta; canary-wap is a runtime opt-in. Neither has run on hardware (runbook Track D). Also open: MQTT broker TLS has had no bench pass against a TLS broker, and the browser flasher's "Broker encryption" select is host-tested only. | bench + human decision (release slot budget) | `FEATURES.md` TLS rows; sweep F15; `download.html` | re-read (FEATURES rows, download page wording) |
| FW9 | **canary-wap does not refuse a foreign `Host`** on its receipt route or bearer-gated API; needs a decision on the captive-portal exemption first. | human decision, then code | sweep F57 | ledger |
| FW10 | **Sensing events are not appended to the canary-wap signed chain** (deliberate: a detection is not the same claim as a seal). The dashboard also shows it absent for the vision, sense and display columns. | code (if wanted) | `FEATURES.md` | re-read |
| FW11 | **canary-sentinel is Phase 0 / 1a**, compile-gated and unreleased. | code + bench | sweep F22 | ledger |
| FW12 | **The 23-item hardware verification checklist has never been run**, 12 board pin maps say "NOT yet validated on bench", seven capabilities are "Built, bench-gated", and the enclosure caliper and print passes (C6, C7) are pending. This is the single largest unlock. | bench | sweep U1, C6, C7; `docs/audit/hardware_verification_checklist.md` | ledger |
| FW13 | **canary-wap enterprise readiness**: the two unmet acceptance criteria (unbox in under 10 minutes; Docker + Frigate end-to-end on a real Docker host), plus the core-pin box. `integrations/ha_frigate_mqtt/up.sh` is written and shellcheck-clean but has never had a real run. | bench + human decision | `ENTERPRISE_READINESS_TODO.md` section 6 | re-read (up.sh and checklist wording) |

**Parity plan.** `PARITY_PLAN.md`'s eight Group-A rows (Web UI, camera peek, mesh,
mesh RSSI, BLE discovery, RF presence, hub failover, chirp) all still read partial or
missing for the canary PIO column in `FEATURES.md`, and all but the web UI are bench
or multi-board-bench gated: code + bench.

---

## 3. Beacon, Opera mesh and mesh crypto

| ID | Gap | Owner | Source | Checked |
|---|---|---|---|---|
| M1 | **Beacon pairing flow (F30).** Nothing can add a member to a beacon set: `PAIR_OFFER` exchange, six-digit confirmation, and the three-gateway cap are unbuilt, so the encrypted two-device ALERT/CANCEL path is unreachable. | code + human decision | sweep F30 | ledger |
| M2 | **The Beacon runtime loop is not wired (F31).** `beacon_api::register_routes` is registered, but the canary-wap sketch never calls `beacon_channel::init()` or `update()`. A COSIGN_REQ frame is 310 B against a 250 B receive limit. Two policy calls ride along (CANCEL versus the 5/24 h bucket; a cosigner that never saw the ALERT). | code + human decision | sweep F31 | re-read (no `init`/`update` call sites in `canary_wap.ino` or `mesh_network.cpp`) |
| M3 | **CAP-gateway attestation milestone (F32)** is deferred behind a trust-root decision, a separately named build and per-deployment legal review. | human decision | sweep F32; `spec/beacon_cap_gateway_v0.md` | ledger |
| M4 | **Opera mesh v0.3/v0.4 crypto has not been reviewed.** Ephemeral-X25519 pairing, `REKEY_*` secret rotation on remove, concurrent-removal convergence and the revocation deny-list are implemented and host-tested in both trees, and v0.4 (#1748) put both trees on one outer frame; the spec repeats "maintainer crypto review and the U1 Track C2/C3 bench passes pending" and says none of it has crossed a radio. v0.4's open review questions (wire break with no negotiation, signing domain string still `v0`, unhandled-but-valid types spending the replay counter) are listed in #1748. | crypto review + bench | `spec/canary_mesh_network_v0.md` header, sections 4.5, 5.3, 5.6, changelog; #1748 | re-read |
| M5 | **The spec contradicts itself on required message types.** Conformance (section 13) requires `POWER_ALERT` and `OFFLINE_IMMINENT`; section 12 says they are not implemented. Either implement or move them out of the REQUIRED list. Also not implemented: alert relay (section 6.1 step 3); alerts are per-boot and not persisted. | human decision, then code | `spec/canary_mesh_network_v0.md:917` (section 12), `:1095` (section 13) | re-read |
| M6 | **The two trees do not interoperate on alerts, on payload only.** Since v0.4 (#1748) both trees send one outer frame and one type byte for `TAMPER_ALERT` (18); the payloads still differ (a 6-byte template-only record versus a 54-byte struct with free text), and the WAP's `broadcast_tamper_alert` has no caller. | code | spec sections 4.5, 8.3 | re-read (spec text) |
| M7 | **canary-wap's mesh crypto still runs X25519 over long-term Ed25519 keys** (the bug class fixed for the PIO tree's pairing) and HKDFs the pairing key where the spec uses it raw, so the two trees cannot pair with each other even though v0.4 (#1748) gave them the same pairing frame numbers (8..12) and outer frame. Concurrent removals cannot converge without a wire change. Crypto review and a derivation decision come first. Small mesh leftovers (joiner `CodeReadyCallback` never fires, no radio-MAC learning, residual removal splits at 5% loss) are F49. | crypto review + human decision, then code | sweep F48, F49; #1748 | ledger; numbering re-read |
| M8 | **The one event-id space is not bench-tested.** Every committed csi_event, bundled or not, now takes its id at commit from one allocator starting at 0xC0000000, and canary-wap's backfill watermark persists (F46, F47). Both are host-tested only, and the commit lock between the loop task and the NimBLE host task cannot be exercised on a host; the F46 rows of `hardware_verification_checklist.md` are open. | bench | sweep F46, F47 | re-read |
| M9 | **C6 mesh/chirp gaps** (G6/G7) are deferred to a later phase, and `SensorDisagreement` / `FirmwareIntegrity` failure types are withheld until a mechanism exists. | code | `NEXT_STEPS_2026-07.md` P3 | ledger |

---

## 4. Kernel, custody and disclosure (`spec/quorum_unseal_v2.md`)

The spec is marked spec-only, with four implemented exceptions (quorum-gated policy
mutation, WYSIWYS approval, break-glass human-context fields, `court export` for
event-export bundles, plus per-device anchoring primitives). Everything below is
design, per its own sequencing section.

| ID | Gap | Owner | Checked |
|---|---|---|---|
| Q1 | **Ceremony mode in code (section 3.4)**: the runbook docs exist, the mode does not. | code | re-read (spec section 6 step 1) |
| Q2 | **Merkle tree, checkpoints, fleet witnessing, tlog-policy and `log_review`** (section 6 step 3); anchoring upgrades are only in part. `court export` does not cover vault unseal outputs or OTS anchoring. | code | re-read |
| Q3 | **Token delay/veto (3.5) and trustee-credential hardening (3.7).** | code + crypto review | re-read |
| Q4 | **VSS-Shamir threshold wrap with resharing (section 2)**: "the one large build". Until it exists the vault key is not split. | code + crypto review (mandatory) | re-read |
| Q5 | **C2PA witness-manifest profile freeze and enterprise projections** (FHIR AuditEvent, CAWG, conformance tracking). The `c2pa-export` feature exists and is off by default. | code + human decision | re-read (spec sections 5, 6) |
| Q6 | **Post-quantum**: `pqc-signatures`, `pqc-vault` and `pqc-tls` are all off by default and marked experimental; no PQ key rotation or migration tooling. | crypto review + code | re-read (`docs/pqc_mode.md`, `docs/feature-flags.md`) |

---

## 5. Apple, Lab and desktop

| ID | Gap | Owner | Source | Checked |
|---|---|---|---|---|
| A1 | **The Tauri iOS/iPadOS Lab shell was never built.** Scaffolded recipe only; the dispatch-only CI workflow was retired 2026-09-08 without a build, and the native `ios/` app is what ships. USB flashing, mDNS/BLE discovery and notifications are not available in the unbuilt Tauri shell (the native `ios/` app already has Bonjour discovery, CoreBluetooth and notifications: `Transport/Discovery.swift`, `Transport/BLEConsole.swift`, `Alerts/AlertCenter.swift`). Reviving it needs an Apple Developer account. | human decision (revive or retire) | `desktop-lab/MOBILE.md` | re-read (`desktop-mobile-release.yml` is absent; `desktop-lab-check.yml`, `ios-release.yml` present) |
| A2 | **Apple signing and store presence** are gated on a paid Apple Developer account and secrets; signed workflows exit green without them. CloudKit household/away path, macOS notarization, iPhone on-device snapshot pass follow. | human decision + bench | sweep U3, A8, A13, A15 | ledger |
| A3 | **Release signing key ceremony.** The premise in the sweep's U2 ("pinned key is the all-zero placeholder") is stale: `firmware/common/ota/src/ota_release_key.h` and `canary-local/devices/flash.json` carry a non-zero key (key id `532429078dc47c04`). What the tree cannot show is whether the matching private key is set as the `OTA_SIGNING_KEY_PEM` secret and a signed release has been cut; `docs/RELEASE_BUTTONS.md` still describes the ceremony as pending. A human should confirm and update whichever doc is wrong. | human decision | sweep U2; `RELEASE_BUTTONS.md` | re-read (key in tree); secret state not checkable here |
| A4 | **The Wall cannot walk an add-on or root-image install** (no viewer-token minting path), and the kernel's auth lockout also closes `/api/fleet` and `/health` (A18, A19). Lab mDNS + BLE discovery is HTTP-poll only (A5). Windows raw-disk write is unproven (A9). | code + human decision | sweep A5, A9, A18, A19 | ledger |
| A5 | **The fleet contract has an aggregator on the hub** (`src/fleet_peers.rs`) but `NEXT_STEPS` P1 predates it; the display's portrait `buildDisplaySheet()` widths are still hardcoded. | code | `NEXT_STEPS_2026-07.md` sections 2 and 4 | ledger |

---

## 6. Home Assistant, website and store

The store rows (W1, W2, W5) are launch gates. The store is in preview on purpose and sells nothing yet; they are listed so the gates are visible, not because the preview is a defect.

| ID | Gap | Owner | Source | Checked |
|---|---|---|---|---|
| H1 | **HA entity ids are documented but never set.** Neither the kernel bridge nor canary-wap sends `default_entity_id`, so a new install names the entities differently from `docs/homeassistant_setup.md`, the dashboard YAML and the HomeKit recipe. | code (both publishers) | sweep HA16 | re-read (no `default_entity_id` in `event_mqtt_bridge.rs` or `csi_mqtt.cpp`) |
| H2 | **canary-wap signed publishes likely read as a key mismatch in HA**: firmware writes the fingerprint in capitals (`hex_to_str` uses `0123456789ABCDEF`), HA pins lowercase and compares exactly (`signature.py:216`). Not observed on a bench. | code (HA side, case-insensitive compare) + bench | sweep HA18 | re-read (both code sites) |
| H3 | **canary-sense and canary-sentinel show no full public key out of band**, so a user can check but not set a manual pin. | code + human decision (portal is shared) | sweep HA17 | ledger |
| H4 | **A working example camera, a live Frigate bring-up recording, and the HA kernel device's double "SecuraCV" prefix** (HA8, HA13, HA12). The HACS mirror sits behind until `MIRROR_PAT` is set (U6); the brands submission is unsubmitted (U7). | bench + human decision | sweep HA8, HA12, HA13, U6, U7 | ledger |
| W1 | **The store sells nothing yet, by design.** `store.json` is `mode: "preview"`, the checkout reads "coming soon", international shipping is "planned": that is the intended status, not a defect, and nothing in this pass touched pricing or legal wording. What stands between preview and a launch is human work: the store carries no radio SKU (`compliance.part15b_sdoc: false`; kits and bundles are the roadmap, printed parts first), FCC testing, the responsible-party facts, and Stripe/PirateShip setup. | human decision (launch gates) | sweep U4, U5, W1 | re-read (`store.json`, `store.html`) |
| W2 | **Watch Station and Canary Dash are waitlist-only**; the compare page prices the kits as "planned" ($79, $109). Canary Vision Pro is called a concept. | human decision | `compare.html`, `store.html` | re-read |
| W3 | **Placeholder supplier quotes** (5 rows marked `"status": "placeholder"` in `suppliers.json`), and the Tizen package still carries `REPLACEME` app ids. | human decision | sweep W6, W10 | re-read |
| W4 | **The CANARY trademark exposure** is unresolved (clearance opinion, reposition, or rename); the audit says not to scale store sales until it is settled. Homepage editorial reduction (W14) and the remaining claims-audit rows needing human facts (W16: mailing address, role email, policy date) are open. | human decision | sweep W14-W16 | ledger |
| W5 | **Ecosystem promises that are not yet built**: the donation fund's fiscal host is "not yet chosen"; OSHWA certification is "pending". | human decision | `ecosystem.html` | re-read |
| W6 | **The Lab app's native USB flashing is in the source, not in a published Lab release** (`download.html`); the Flasher does it today. | code + human decision (release) | `download.html` | re-read |
| W7 | **Render quality**: GPU cinematic mode and the micro-wear map amplitude need a GPU session; Blender hero bake has one hero. | code (GPU session) | sweep W5, W13 | ledger |
| W8 | **The advisory Claude review workflow is inert** until `ANTHROPIC_API_KEY` is set in both repos, so a green "Canary Reviewer" check means nothing was reviewed. | human decision | sweep W11 | ledger |

---

## 7. Where the docs disagree with the tree (fix the doc, not the code)

| ID | Disagreement | Fix | Checked |
|---|---|---|---|
| R1 | `ESP32S3_OPTIMIZATION_ROADMAP.md` row 13 lists "SD SPI 4 to 20 MHz" as open with a stale anchor. In the PlatformIO tree `canary_config.h:480` already defines `SD_SPI_FAST` as 20 MHz (unbenched, per its own comment) while the roadmap's section 3.5 still says 4 MHz; canary-wap's `sd_storage.h:45` really is still 4 MHz. | Mark 13 landed for the PIO tree, keep the bench caveat, scope the WAP separately. | re-read |
| R2 | `PARITY_PLAN.md` section 1 Group B and the section 4 scorecard still show OTA A/B rollback, acoustic T3/T4 and WiFi auto-reconnect as missing in the Arduino tree, while the `FEATURES.md` dashboard (which the plan names as its source of truth) shows them present for canary-wap. | Refresh the scorecard from the dashboard. | re-read |
| R3 | `FEATURES.md` shows "OTA A/B with rollback safety" as present for the shipping canary and canary-wap columns. FW1 could not confirm that from the tree alone; #1746's reading of the core's prebuilt sdkconfig says the row is right, and its `#error` makes CI prove it. Bench Track E decides. | Bench (Track E), then correct whichever is wrong. | re-read (config search); dashboard cell read |
| R4 | The sweep's U2 says the release key is the all-zero placeholder; the tree carries a non-zero key (A3). | Re-word U2 after a human confirms the secret state. | re-read |
| R5 | `integrations/ha_frigate_mqtt/up.sh:21` cited `ENTERPRISE_READINESS_TODO.md` with no path. The file exists only at `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` (section 6, "Make integration runnable in one command", line 242), so the bare name did not resolve from the repo root. **Fixed in this pass**: the comment now carries the full path. | done | re-read |
| R6 | `docs/v1-roadmap.md` and `AGENTS.md` say WASM sandboxing is out of scope or a future option; the roadmap's Sandboxing and RTSP rows now say what the default build does. | done | re-read |

---

## 8. What this pass changed

Copy only; no behavior, pricing or legal wording. In this repo: `README.md`,
`config.example.toml`, `docs/FAQ.md`, `docs/inference_backends.md`,
`docs/pqc_mode.md`, `docs/integrations/home-assistant-frigate-mqtt.md`,
`docs/v1-roadmap.md`, the `up.sh` comment, this file and its entry on the docs map.
In the website repo: `index.html` (a note under the Status grid), `how-it-works.html`
(motion versus object events) and `apple-tv.html` (ONVIF wording). No generated file
was hand-edited: `tools/gen_kernel_status.py` was re-run and produced no change, and
the website's generated outputs are unchanged.
