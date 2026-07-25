# SecuraCV documentation — the map

Every doc in this repo, organized by what you're trying to do. This index is
**enforced in CI** (`scripts/lint_docs_index.py`): a doc that isn't reachable
from this page fails the build, so the map can't rot and no file can go
straggler. Prefer clicking around? The same getting-started paths run
interactively — with one-tap-copy commands, a progress bar, and your OS
picked once — in **[the Lab's Get Started guide](https://kmay89.github.io/securaCV/canary-local/start.html)**. The Lab also has a generated **[complete site map](https://kmay89.github.io/securaCV/canary-local/site-map.html)** that folds in every bench, depth page, redirect, iPad runbook, evidence viewer, and standalone HTML doc from the same manifest.

## Start here

Pick the row that sounds like you (same four paths as the interactive guide):

| You | Your path |
|---|---|
| **"I run Home Assistant."** | [Home Assistant setup](homeassistant_setup.md) → [Frigate integration](frigate_integration.md) → [the timeline card](lovelace_timeline.md) |
| **"I have a spare Raspberry Pi."** | [Home Assistant setup](homeassistant_setup.md) (flashing + first boot are its first sections) — or click through it on [the Hub bench](https://kmay89.github.io/securaCV/canary-local/homeassistant.html) first |
| **"I run Docker / a homelab."** | [Frigate integration](frigate_integration.md) (compose quickstart) → [container guide](container.md) → [operator guide](operator_guide.md) |
| **"I like building little devices."** | [Getting started with Canaries](getting_started_canary.md) → [hardware guides](hardware/README.md) → [the Lab](https://kmay89.github.io/securaCV/canary-local/) |
| **"I just want to understand it."** | [Why witnessing matters](why_witnessing_matters.md) → [why it's secure](why_secure.md) → [the whitepaper](securaCV_whitepaper.md) |

<details>
<summary><strong>Try it without hardware</strong> — demos that run on your desk or in your browser</summary>

- [The demo](demo.md) — `cargo run --bin demo`, then break the log and watch verification fail
- [The litterbox witness](litterbox_witness_demo.md) — the smallest end-to-end build, starring a cat
- [The Lab](https://kmay89.github.io/securaCV/canary-local/) — real firmware compiled to WebAssembly, in your browser
- [The Lab site map](https://kmay89.github.io/securaCV/canary-local/site-map.html) — every Lab bench plus standalone HTML documentation, generated from the build-line manifest

</details>

## The idea (read these to get the soul of the project)

- [Why witnessing matters](why_witnessing_matters.md) — the case for tamper-evident perception
- [Why this matters](why_this_matters.md) — the plain-language version
- [Why exports work this way](why_secure.md) — the security rationale behind the UX
- [The root paradox](root_paradox.md) — what host compromise does and doesn't break
- [The whitepaper](securaCV_whitepaper.md) — the full system, formally
- [Governance & invariants](governance_and_invariants.md) — how the privacy rules are kept
- [Evidence lifecycle](evidence_lifecycle.md) — from photon to court-ready export

## Set up

<details>
<summary><strong>Home Assistant</strong> — the best-supported path</summary>

- [Home Assistant setup](homeassistant_setup.md) — flashing, first boot, add-ons, entity catalog
- [Frigate integration](frigate_integration.md) — compose quickstart, doctor check, API tokens
- [The Verified Timeline card](lovelace_timeline.md) — ✓-badged events on your dashboard
- [Alert & digest blueprints](blueprints/canary_sense_wellbeing.md) — plus the YAML: [alerts](blueprints/securacv_alerts.yaml), [daily digest](blueprints/securacv_daily_digest.yaml), [after-hours presence](blueprints/canary_sense_after_hours_presence.yaml), [lights-out tamper](blueprints/canary_sense_lights_out_tamper.yaml), [welfare check](blueprints/canary_sense_welfare_check.yaml)
- [Example automations](homeassistant_automations.yaml) — copy-paste YAML
- [HA + Frigate over MQTT, worked example](integrations/home-assistant-frigate-mqtt.md)
- [MR60BHA2 radar via ESPHome](integrations/mr60bha2_esphome.md)
- Onboarding flows: [multiple Canaries](onboarding_multiple_canaries.md) · [the unified wizard](onboarding_unified_wizard.md) · [workflow evaluation](onboarding_workflow_evaluation.md)
- [The one-flash Pi hub (design)](design/raspberry_pi_hub_flashing.md) — RFC: type Wi-Fi, write the card, boot a self-healing Home Assistant hub — built on HAOS so it never rots
- [Hub hardware-validation runbook](hub_validation_runbook.md) — the one-Pi-5 session that gates a tagged flasher release: write/read-back, Wi-Fi seed, USB-C gadget, account-restore mechanism
- [The Hub as network witness (concept)](design/hub_network_witness.md) — CONCEPT, no code: a Hub-hosted DNS witness/shield — see which devices phone home, mute them per-device, verify the fleet's self-manifests over LAN; counters-not-logs by invariant, engine research (Pi-hole vs blocky) recorded

</details>

<details>
<summary><strong>Cameras & ingest</strong> — pointing real optics at the kernel</summary>

- [RTSP cameras](rtsp_setup.md) — Hikvision, Dahua, Reolink, Amcrest, Ubiquiti…
- [USB / V4L2 cameras](v4l2_setup.md)
- [ESP32-S3 camera boards](esp32_s3_setup.md) · [Seeed XIAO Vision AI](seeed_xiao_vision_ai_setup.md)
- [Detection backends](inference_backends.md) — ONNX/Tract and friends
- [Container deployment](container.md) — images, volumes, hardening
- [The operator guide](operator_guide.md) — break-glass, exports, day-2 operations

</details>

<details>
<summary><strong>Canary devices</strong> — build, flash, and live with the hardware</summary>

- [Getting started with Canaries](getting_started_canary.md) — first device, end to end
- [Hardware guides & BOMs](hardware/README.md) — build plans, bring-up benches, enclosures, per-device guides
- [Board ownership research](board_market_research.md) — installed-base numbers (ESP32 vs Pi vs Arduino vs the rest) and which boards deserve firmware first; names the classic ESP32 (ESP32-CAM / WROOM-32) as the highest-value next port
- [Firmware OTA](firmware_ota.md) — signed pull-updates with rollback
- [Parity by architecture](FLEET_PARITY.md) — how a fleet-wide capability (like the `/api/fleet` self-report) lives in one host-tested `common/` core so one edit reaches every board, never a per-board copy-paste
- [Release process & channels](RELEASE_PROCESS.md) — tags → releases; how the dev channel stays invisible to release devices
- [USB evidence drive (design)](design/usb_evidence_drive.md) — the Canary as a read-only USB drive + drop-file signed updates
- [USB onboarding (design)](design/usb_onboard.md) — "plug me in": consented HID help-launch, read-only drive, guided recovery/unsealing
- [Serial test console (design)](design/test_console.md) — run tests over serial safely: read-only `t`, tiered demo/mutate gating, the BLE bring-up ladder
- [Themed serial console (design)](design/serial_console_theming.md) — the `l` identity banner: key fingerprint as drunken-bishop randomart, capability-probed, ASCII-safe by default
- [Self-* roadmap (design)](design/self_star_roadmap.md) — "plug it in and it proves itself": what shipped (self-manifest `j`, randomart handshake, self-repair) + coming-soon TODOs (fleet map, boot safe-mode / A/B rollback)
- [Power-event resilience + outage log (design)](design/power_events.md) — harden against brownouts, flickers, and outages, and keep a correctly-named log of when the power went out: a pure, host-tested boot-lineage classifier (cold boot / clean reboot / brownout / restored outage) with an honest lower-bound duration + a durable ring, shared across firmwares
- [Browser flasher](browser_flasher.md) — flash a blank board from Chrome over USB, no toolchain
- [The help ecosystem layout (design)](design/help_ecosystem_layout.md) — one map, every surface: the Lab → Playground → Factory story, the self-linking/never-rots rules the website now enforces in CI, and the write-once-wrap-thin platform strategy (web/PWA/Tauri)
- [The Lab & Flasher experience (design)](design/flasher_experience.md) — the spine for the magical bring-up-and-tend arc: come-to-life receipt, two-port Vision flow, and the native-app fleet-view over the network (always know every Canary is up to date + healthy) — what to reuse, the browser-vs-native platform reality, and the security model
- [The iPhone companion app (design)](design/iphone_companion_app.md) — RFC for the living-with-it native iOS surface (a witness console, not a camera app): a thin renderer over self-describing device contracts so it rarely needs an App Store update, two-tier alerts (local MQTT/SSE/BLE + an opt-in metadata-only wake relay), and own-nothing identity (no account locally, CloudKit private DB, Sign in with Apple / Hide My Email, Secure-Enclave key custody)
- [The alert relay (design)](design/alert_relay.md) — RFC: remote "pokes" without a cloud — a metadata-only, pluggable (Apprise-shaped) alert-sink on the hub, ntfy as the flagship free/self-hostable default, GitHub-issue/Trello as an optional durable "away timeline," and the outage-resilient mesh path (WiFi/internet die in the very outage you're reporting, so absence-inference + a powered mesh gateway carry the poke)
- [SecuraCV on Apple TV — the Witness Wall (design)](tvos/README.md) — the big-screen witness surface for homes **and venues** (bars, restaurants, shops): the *verified record* on the shared screen, not a wall of feeds; a tamper-evident, dispute-proof timeline over the cameras you already own (RTSP/ONVIF/Frigate); and a never-rot autopipeline ([the self-heal / self-publish / self-update mapping](tvos/AUTOPIPELINE.md))
- [Secure provisioning](secure_provisioning.md) — how a device earns its keys
- [Hardware root of trust (design)](design/hardware_root_of_trust.md) — RFC: opt-in Secure Boot / flash encryption / attestation, tiered so the default Canary stays un-brickable
- [Device settings access](canary_settings_access_validation.md) — who may change what, verified
- [SD-card health](sd_card_health.md) · [thermal guide](thermal_guide.md)
- [Flipper Zero as a debug probe](flipper_zero_debug_guide.md)

</details>

## Go deeper

<details>
<summary><strong>Evidence & trust</strong> — the cryptographic spine</summary>

- [Security docs](security/README.md) — security model, threat model, audit
- [Supply-chain transparency](supply_chain_transparency.md) — signed build provenance + a public transparency log; verify a download was built from the open source
- [Device trust & PKI](device_trust.md) — pinned keys, stricter than TOFU
- [Log verification](log_verify.md) — proving the chain, offline
- [Timestamping](timestamping.md) — coarse time as a feature
- [Sealed snapshot vault](sealed_snapshot_vault.md) · [scheduled exports](scheduled_exports.md)
- [C2PA Content Credentials for exports (design)](design/c2pa_export.md) — implemented behind `c2pa-export`: `export_events --c2pa` signs an industry-standard sidecar manifest any Content Credentials tool can verify — keys derived from the device seed, reproducible device-local CA, fully offline, chain stays the root of trust
- [Vault operator UX & hardware-backed keys (v1.1 design)](design/vault_operator_ux_v1_1.md) — RFC: guided setup/enrollment, a `KeyStore` seam (file default; TPM/PKCS#11/FIDO2), and the request→approve→unseal flow — the crypto's already wired, this scopes the operator experience around it
- [Database key rotation](db_key_rotation.md) · [post-quantum mode](pqc_mode.md)
- [Identity & transport](identity_transport.md) — who speaks, on what wire
- [Failure semantics](failure_semantics.md) — what breaks loudly, and why
- [Evidence lifecycle](evidence_lifecycle.md) — the full custody story

</details>

<details>
<summary><strong>Radio, mesh & sensing internals</strong> — how Canaries feel and talk</summary>

- [BLE protocol](ble_protocol.md) · [BLE mesh + chirp tandem](BLE_MESH_OPERA_TANDEM.md)
- [ESP-NOW mesh evaluation](mesh_esp_now_evaluation.md) · [ESP32 mesh sensing design](esp32_mesh_sensing_design.md)
- [Meshtastic integration](meshtastic_integration.md) — witnesses on the property line
- [Network coexistence](network_coexistence.md) — being a good neighbor on 2.4 GHz
- [WiFi CSI sensing quickstart](csi_quickstart.md) · [CSI modules](csi_modules.md) · [CSI developer API](csi_developer_api.md)
- [mmWave radar design (MR60BHA2)](canary_sense_mr60bha2_design.md)
- [Coarse mover-class from the Sense radar (design)](canary_sense_coarse_class_design.md) — the software-only "do it with what we have" upgrade: coarse large-vs-small mover class on the shipped 60 GHz radar, distilled from the BumbleBee micro-Doppler work, class-not-identity
- [Canary Sentinel — multi-sensor fusion guardian (design + spec)](canary_sentinel_fusion_design.md) — the "near impossible to evade" doorway/window guardian: fuse physically independent channels (PIR + radar + WiFi CSI + WiFi/BLE + light + contact/tamper/vision), score corroboration across independent modalities, treat a blinded channel as suspicion (the ATM/fraud-detection posture); Lite/Standard/Heavy tiers, five presets, host-tested fusion core
- [RF sensing HTTP routes](rf_sensing_phase12_http_routes.md)
- [ESP32-S3 power resilience](esp32s3_power_resilience.md) · [thermal review](esp32s3_thermal_review.md) · [wireless review](seeed_xiao_esp32s3_wireless_review.md) · [BLE/WAP audit](esp32s3_ble_wap_audit.md)

</details>

<details>
<summary><strong>Developing on it</strong> — contributing code and keeping CI green</summary>

- [CI](ci.md) — what runs, what gates
- [Feature flags](feature-flags.md) · [logging](logging.md) · [CLI UI conventions](cli_ui.md)
- [Flight rules](FLIGHT_RULES.md) — the engineering constitution
- [Tree map & consolidation](CONSOLIDATION.md) — what each similarly-named tree actually is (`src/` vs `kernel/` vs `privacy_witness_kernel/`, the desktop apps, the firmware lanes), plus open cleanup decisions
- [Manual test plans](manual_test_plan_captive_portal.md) — captive portal · [MQTT](manual_test_plan_mqtt.md)
- [The ambient display standard](standard/AMBIENT_DISPLAY_STANDARD.md) · [Canary Cards — the widget-card schema](standard/CANARY_CARDS.md)
- Learnings from elsewhere: [Marlin & Klipper](marlin_klipper_learnings.md) · [OpenIPC](openipc_architecture_learnings.md)

</details>

<details>
<summary><strong>The engineering record</strong> — audits, reviews, research, and roadmaps</summary>

- **v1:** [roadmap](v1-roadmap.md) · [launch review](V1_LAUNCH_REVIEW.md) · [bench-test runbook](V1_BENCH_TEST_RUNBOOK.md)
- **Next steps:** [what the last ~100 PRs built, and what they left open (2026-07)](NEXT_STEPS_2026-07.md) — the open edges after `#1080 → #1226`, checked against the tree rather than the roadmaps: the inert A/B rollback net, the Nightstand Line's tail, the missing fleet aggregator, and a three-wave sequence
- **Audits:** [firmware full audit 2026-07](audit/esp32s3_firmware_full_audit_2026-07.md) · [hardware verification checklist](audit/hardware_verification_checklist.md) · [mesh & chirp audit](audit/mesh_and_chirp_audit_v1.md) · [v0.3 closeout](audit/v0.3_closeout.md) · [WAP multi-device UX audit](audit/wap_multi_device_ux_audit.md) · [UX/UI audit 2026-06](ux_ui_audit_2026-06.md) · [legal, claims & risk audit 2026-07](legal-audit-2026-07.md)
- **Code reviews:** [Docker container](reviews/2026-06-10_docker_container_review.md) · [full repo](reviews/2026-06-10_full_repo_code_review.md) · [HA setup audit](reviews/2026-07-11_home_assistant_setup_audit.md) · [flasher hatching](reviews/2026-07-22_flasher_hatching_review.md) · [Arduino demo commands](reviews/arduino_demo_review_commands.md) · [Arduino demo tasks](reviews/arduino_demo_review_tasks.md) · [frame-trigger pipeline plan](reviews/kernel_frame_trigger_pipeline_plan.md)
- **Requirements review series:** [review/README](review/README.md)
- **Research:** [bitchat protocol](research/bitchat_protocol_review.md) · [display market](research/display_market_research.md) · [harm-reduction prior art](research/harm_reduction_prior_art.md)
- **Strategy series (01–17):** [strategy/README](strategy/README.md)
- **Ecosystem & branding:** [brand & positioning](BRAND.md) — the canonical "the Canary" positioning, the one-line promise, and who we learn from · [trademark & branding policy](../TRADEMARK.md) · [trademark grants registry](trademark-grants.md) — the free "Works with SecuraCV" badge, the "… for SecuraCV" naming rule, and the public record of primary-mark grants (strategy doc 19 §5)
- **Marketing:** [launch posts](marketing/launch_posts.md)

</details>

---

**Housekeeping rules** (kept by CI):

1. Every new doc gets a home on this map in the same PR that adds it —
   `scripts/lint_docs_index.py` fails otherwise.
2. A new docs directory gets its own `README.md`, linked from here; the
   directory then indexes itself.
3. Links here are relative, so the map works on GitHub, in editors, and offline.
