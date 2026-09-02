# SecuraCV documentation — the map

Every doc in this repo, organized by what you're trying to do. This index is
**enforced in CI** (`scripts/lint_docs_index.py`): a doc that isn't reachable
from this page fails the build, so the map can't rot and no file can go
straggler. Prefer clicking around? The same getting-started paths run
interactively — with one-tap-copy commands, a progress bar, and your OS
picked once — in **[the Lab's Get Started guide](https://kmay89.github.io/securaCV/canary-local/start.html)**. The Lab also has a generated **[complete site map](https://kmay89.github.io/securaCV/canary-local/site-map.html)** that folds in every bench, depth page, redirect, iPad runbook, evidence viewer, and standalone HTML doc from the same manifest.

## Look something up

Three pages exist so you never have to infer an answer from context — useful to
a newcomer, and to an AI assistant answering someone's question about the
project:

- [**The glossary**](GLOSSARY.md) — every proper noun defined once: SecuraCV vs
  the Canary vs the witness kernel, the device line, Opera/Chirp/Beacon,
  break-glass and quorum, the seven invariants, and the words we deliberately
  don't use.
- [**The FAQ**](FAQ.md) — the questions people actually ask ("does it do face
  recognition?", "how do I get footage after a break-in?", "is this
  production-ready?"), answered honestly with pointers.
- [**AGENTS.md**](../AGENTS.md) — the brief every AI coding assistant works
  from. Each tool's own entrypoint file (`CLAUDE.md`, `GEMINI.md`, `QWEN.md`,
  Copilot, Cursor, Cline, Windsurf) is generated from it, so they can't drift.

## Steer the project (no code required)

- [**How an idea becomes a shipped feature**](IDEAS.md) — the suggestion
  pipeline end to end: say it in one sentence, it becomes a public issue, people
  back it with a 👍, and the label lane tracks it to a release. Includes the
  honest case for **why a privacy project runs its suggestion box on GitHub**,
  the maintainer triage loop, and the one string (`idea`) that joins all four
  moving parts together.

## Start here

Pick the row that sounds like you (same four paths as the interactive guide):

| You | Your path |
|---|---|
| **"I run Home Assistant."** | [Home Assistant setup](homeassistant_setup.md) → [Frigate integration](frigate_integration.md) → [the timeline card](lovelace_timeline.md) |
| **"I have a spare Raspberry Pi."** | [**The full stack, end to end**](full_stack_setup.md) — the golden path: flash the hub, boot it, add cameras — or click through it on [the Hub bench](https://kmay89.github.io/securaCV/canary-local/homeassistant.html) first |
| **"I want the whole thing — hub, cameras, devices."** | [**The full stack, end to end**](full_stack_setup.md) — one page, in the right order, with the gotchas |
| **"I run Docker / a homelab."** | [Frigate integration](frigate_integration.md) (compose quickstart) → [container guide](container.md) → [operator guide](operator_guide.md) |
| **"I like building little devices."** | [Getting started with Canaries](getting_started_canary.md) → [hardware guides](hardware/README.md) → [the Lab](https://kmay89.github.io/securaCV/canary-local/) |
| **"I just want to understand it."** | [Why witnessing matters](why_witnessing_matters.md) → [why it's secure](why_secure.md) → [the whitepaper](securaCV_whitepaper.md) |

<details>
<summary><strong>Try it without hardware</strong> — demos that run on your desk or in your browser</summary>

- [The demo](demo.md) — `cargo run --bin demo`, then break the log and watch verification fail
- [The litterbox witness](litterbox_witness_demo.md) — the smallest end-to-end build, starring a cat
- [First Light](first_light_demo.md) — a Canary Vision + Canary Nightlight, no WiFi, no hub: wave at the camera and the glass answers with the trigger timing on it
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

- [**Reaching your fleet from away**](away_access.md) — read this *before* you
  follow anyone's advice about remote access. The popular answer (forward port
  8123, put a certificate on it) publishes your hub's login form to the
  internet; the correct answer is free, takes ten minutes, and opens no port
  at all. One blessed path, the anti-patterns named out loud, an honest
  accounting of what a subscription would actually buy you — and
  `tools/away_access_check.py`, which asks your own router what it's
  forwarding so the answer is a verdict instead of a hope.

<details>
<summary><strong>Home Assistant</strong> — the best-supported path</summary>

- [**The full stack, end to end**](full_stack_setup.md) — the golden path across every piece: flash the hub → first boot → broker + securaCV → camera detection (Pi, Coral, or a Jetson) → Canaries, with what "working" looks like at each step
- [Home Assistant setup](homeassistant_setup.md) — flashing, first boot, add-ons, entity catalog
- [Frigate integration](frigate_integration.md) — compose quickstart, doctor check, API tokens
- [The Verified Timeline card](lovelace_timeline.md) — ✓-badged events on your dashboard
- [Alert & digest blueprints](blueprints/canary_sense_wellbeing.md) — plus the YAML: [alerts](blueprints/securacv_alerts.yaml), [daily digest](blueprints/securacv_daily_digest.yaml), [after-hours presence](blueprints/canary_sense_after_hours_presence.yaml), [lights-out tamper](blueprints/canary_sense_lights_out_tamper.yaml), [welfare check](blueprints/canary_sense_welfare_check.yaml), [Busy Bar desk light](blueprints/securacv_busybar_alert.yaml)
- [Busy Bar alerts, worked recipe](integrations/busy-bar.md) — the busy.app desk light as an alert beacon, both lanes: the HA blueprint (hold-until-clear) and the alert relay's LAN-only sink (auto-expiring)
- [Example automations](homeassistant_automations.yaml) — copy-paste YAML
- [HA + Frigate over MQTT, worked example](integrations/home-assistant-frigate-mqtt.md)
- [Apple Home quickstart](integrations/apple-home-quickstart.md) — nothing to a sensor in the Home app, both lanes, with the troubleshooting table
- [MR60BHA2 radar via ESPHome](integrations/mr60bha2_esphome.md)
- [**Talking to your fleet — local voice on the hub**](voice_control.md) — "is the fleet OK?" answered out loud with every stage (wake word, Whisper speech-to-text, the SecuraCV intents, Piper) on your own Pi: the add-on recipe, the copyable [sentences file](voice_sentences_en.yaml), the read-only-by-construction intent list, and the honest wake-word trade. Design and contract: [Whisper local voice](research/whisper_local_voice.md)
- [Apple Home via the HA HomeKit Bridge, worked recipe](integrations/apple-home-homekit-bridge.md) — phase A0 of the [Apple Home RFC](design/apple_home_integration.md): motion/smoke/occupancy into the Home app with zero new code, remote notifications via the household's own Apple TV/HomePod hub, the three redundant push lanes (Apple Home / HA companion / AwayPush), and the honest power-outage story (UPS today, outage-as-evidence on restore, alert-relay mesh designed)
- Onboarding flows: [multiple Canaries](onboarding_multiple_canaries.md) · [the unified wizard](onboarding_unified_wizard.md) · [workflow evaluation](onboarding_workflow_evaluation.md)
- [One onboarding, every board (design)](design/onboarding_shared_module.md) — inventory + phased plan for extracting the WAP's network stack (captive DNS/probes, `canary.local`, the join wizard) into `firmware/common/network/`: today three divergent portals exist and Sense/Vision ship with none
- [The one-flash Pi hub (design)](design/raspberry_pi_hub_flashing.md) — RFC: type Wi-Fi, write the card, boot a self-healing Home Assistant hub — built on HAOS so it never rots
- [Seeding the hub's Wi-Fi without mounting the card (design)](design/hub_wifi_seed_injection.md) — DESIGNED, NOT BUILT: why the flasher re-mounts a freshly written card to drop in one NetworkManager keyfile, why macOS regularly refuses (stranding a headless Pi at `wlan0: No address` with Home Assistant stuck on its landing page), and the fix — inject the file into the FAT32 boot partition in memory before the write, exactly as the ESP32 path already patches NVS into a firmware image and never fails for this reason
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
- [Which button do I press?](RELEASE_BUTTONS.md) — the operator's index: every release button, when to press it, when not to, and the three failures that cost us time (no signing key, a dark flasher, an app version that already shipped)
- [Release process & channels](RELEASE_PROCESS.md) — tags → releases; how the dev channel stays invisible to release devices
- [Apple signing, every target](APPLE_SIGNING.md) — which certificate signs which app and which secret carries it: the split is not Mac-vs-phone but *how the user gets the app* (website download needs Developer ID + notarization, an App Store needs Apple Distribution), why adding an app usually needs no new signing setup, and a symptom→cause table for the four ways this broke — including the `.p12` that `openssl` reads happily and macOS rejects as "wrong password?"
- [USB evidence drive (design)](design/usb_evidence_drive.md) — the Canary as a read-only USB drive + drop-file signed updates
- [USB onboarding (design)](design/usb_onboard.md) — "plug me in": consented HID help-launch, read-only drive, guided recovery/unsealing
- [Serial test console (design)](design/test_console.md) — run tests over serial safely: read-only `t`, tiered demo/mutate gating, the BLE bring-up ladder
- [Themed serial console (design)](design/serial_console_theming.md) — the `l` identity banner: key fingerprint as drunken-bishop randomart, capability-probed, ASCII-safe by default
- [Self-* roadmap (design)](design/self_star_roadmap.md) — "plug it in and it proves itself": what shipped (self-manifest `j`, randomart handshake, self-repair) + coming-soon TODOs (fleet map, boot safe-mode / A/B rollback)
- [Power-event resilience + outage log (design)](design/power_events.md) — harden against brownouts, flickers, and outages, and keep a correctly-named log of when the power went out: a pure, host-tested boot-lineage classifier (cold boot / clean reboot / brownout / restored outage) with an honest lower-bound duration + a durable ring, shared across firmwares
- [Browser flasher](browser_flasher.md) — flash a blank board from Chrome over USB, no toolchain
- [Setup profiles, the secret drawer & the fleet book](flasher_profiles_fleet_book.md) — type your home's setup once (passwords in the OS keychain, with the consent copy naming it), the flash-time-minted device API token both flashers seed, and the Fleet tab's book: every board you flashed, found live over mDNS, with one-click device-verified OTA
- [Unflashed board intake](unflashed_board_intake.md) — a board bought unflashed arrives running somebody else's firmware: why no app can shield the moment you plug it in, the BOOT-held cold start that actually does (mask ROM, not us), the read-only customs check before writing (security eFuses, flash-size honesty, what it shipped with), the erase that is no longer optional on first contact — and the parts nothing reaches
- [Fleet figures — one picture of every thing, everywhere (design)](design/FLEET_FIGURES.md) — **built**: every physical thing in the fleet gets one isometric figure, drawn from one fixed camera under one light rig, and every surface draws the same one — the glass, the wrist, the phone, the web, the emulator. Dimensions are read live off the committed STLs (a drift guard stops the generator when the picture and the print part company), the massing is a function of the CAD rather than a copy of it, and the four-rung confidence ladder (shipping / confirmed / prototype / idea) is **derived from evidence on disk** with that evidence recorded beside every verdict — so an idea renders as a dashed ghost and can never be mistaken for something you can buy or print
- [The help ecosystem layout (design)](design/help_ecosystem_layout.md) — one map, every surface: the Lab → Test bench → Factory story, the self-linking/never-rots rules the website now enforces in CI, and the write-once-wrap-thin platform strategy (web/PWA/Tauri, with the native iOS companion as the one named exception)
- [The automated Help Desk (design)](design/automated_help_desk.md) — no humans in the loop, no dead ends: Phase 1 (the website's symptom-first `/help` front door over a sourced catalog) is shipped; this doc is the cross-repo architecture — routing may be fuzzy but answers must be pinned, the device answers for itself pull-based only, diagnosis before dispatch — plus the Phase 2 firmware asks (WebSerial verdict reads, the on-request help QR), the build-order warning for anything touching canary-display, and the list of things we will not build (hosted-LLM chat, support telemetry, autonomy over money or safety)
- [The Lab & Flasher experience (design)](design/flasher_experience.md) — the spine for the magical bring-up-and-tend arc: come-to-life receipt, two-port Vision flow, and the native-app fleet-view over the network (always know every Canary is up to date + healthy) — what to reuse, the browser-vs-native platform reality, and the security model
- [The iPhone companion app (design)](design/iphone_companion_app.md) — RFC for the living-with-it native iOS surface (a witness console, not a camera app): a thin renderer over self-describing device contracts so it rarely needs an App Store update, two-tier alerts (local MQTT/SSE/BLE + an opt-in metadata-only wake relay), and own-nothing identity (no account locally, CloudKit private DB, Sign in with Apple / Hide My Email, Secure-Enclave key custody)
- [iCloud as the backend we don't have (design)](design/cloudkit_backend.md) — the canonical account of why a local-first project has a cloud container: the two record types that exist (a content-free wake, the fleet list) and the list of what will never join them, the CloudKit Console tile-by-tile verdict (schema — yes; push metrics — read-only; Private Access Tokens — nothing to attach them to), the development-auto-creates/production-refuses trap that kills a feature silently and the two gates that now catch it, the three residuals argued instead of waved away (event-correlated timing, why the wake class must travel in the clear, and the plaintext fleet fields that should be encrypted before there is data to migrate), and the plain list of how the user wins — no account, no subscription, nothing of theirs to lose if we vanish
- [**The five moments — what the voice is actually for (design)**](design/voice_moments.md) — the scope document, whose most important content is the list of things we will not build. Grounded in what voice research actually finds (usage collapses to a handful of commands; poor discoverability is the leading cause of abandonment; speech is serial and cannot be re-read), it names the five moments where a screen is the wrong answer — the 2 a.m. question above all — and the design laws that follow: a night register, no lists read aloud, teach on every failure, one question in and one answer out
- [**Watches — attention that expires on purpose (design)**](design/watches.md) — the fix for why temporary monitoring never gets built: every alerting tool assumes attention is permanent, so a two-week concern (a cat recovering from surgery, soil moisture in August) costs a setup you must remember to dismantle. A **watch** is bounded attention that ends by itself, learns what normal looks like instead of asking for a threshold nobody knows, and is created in one sentence. Five concerns, three sensitivity words, no numbers — plus why voice may start one but never end one
- [The alert relay (design)](design/alert_relay.md) — RFC: remote "pokes" without a cloud — a metadata-only, pluggable (Apprise-shaped) alert-sink on the hub, ntfy as the flagship free/self-hostable default, GitHub-issue/Trello as an optional durable "away timeline," and the outage-resilient mesh path (WiFi/internet die in the very outage you're reporting, so absence-inference + a powered mesh gateway carry the poke)
- [Apple Watch app & the notification experience (design)](design/apple_watch_and_notifications.md) — scoping RFC: the quiet-by-default three-tier notification doctrine (Digest/Important/Critical, ack travels, escalate by silence), the Watch app in three phases (mirrored notifications with wrist-ack → Smart Stack widget + Live Activity → three-screen glance app), "live truth, not live video" analytics from the fleet time-machine, and the everything-works-without-Apple path (installable PWA + SSE + Web Push/VAPID on Chrome/Firefox/Safari)
- [Alert freshness & event history (design)](design/alerts_event_history.md) — the lifecycle that keeps the Alerts surface alive for years: open → resolved ("still happening" and "over" never look the same), seen/unseen driving the app badge, 30-day aging that never silently deletes what the user hasn't seen, relaunch continuity (an ongoing alarm is not re-news), day-sectioned history — plus the 2026 industry survey (Ring/Nest/Arlo/UniFi/SimpliSafe/PagerDuty/Tesla) it distills, and the scoped roadmap (snooze durations, reconciliation, heartbeat ingestion, escalation, local actioned-rate self-tuning)
- [Apple Home & the fleet (design)](design/apple_home_integration.md) — RFC, research complete (post-WWDC-2026 landscape pinned with sources): each Canary becomes an Apple Home *sensor* accessory — motion/occupancy/contact/tamper/liveness as present-tense booleans, by default no more than a dumb PIR would tell it — so the house answers the witness (lights before eyes, tamper goes house-wide) with automations that run on Apple's hub, app closed; one dictionary-governed projection table with three bridge sites (HA HomeKit Bridge today, a `witnessd` HAP lane, device-native HomeSpan, Matter later), the argued-once HKSV verdict with revisit triggers (the events — absolutely; the video — never, Invariant I), and the works-without-Apple degradation table
- [The LAN baby monitor (deferred)](design/lan_baby_monitor.md) — research complete, deliberately out of scope for now: all-night raw A/V streaming is exactly what Invariant I forbids, no device has a speaker for talkback, and the honest slice (radar "no camera to point" nursery care, cry-cadence coarse events) already lives in the wellbeing line — with the revisit triggers recorded
- [The Tin Can — a kids' wrist Canary (design)](design/canary_tincan_kids_watch.md) — RFC for the Waveshare AMOLED 2.06 watch board: two kids **tie a string** (parent-witnessed, LAN-only pairing) and knock/tug/stamp/doodle at each other with **no voice, no text, no location, no cloud** — the refusal list is the design; plus the parent's one privileged message (the **Ring**: "come inside") with honest delivery states, the tin-can research the vocabulary comes from, the missing haptic motor the board doesn't have, and the CPSIA/ASTM/battery-containment gate before anyone writes "kids" on a store listing
- [SecuraCV on Apple TV — the Witness Wall (design)](tvos/README.md) — the big-screen witness surface for homes **and venues** (bars, restaurants, shops): the *verified record* on the shared screen, not a wall of feeds; a tamper-evident, dispute-proof timeline over the cameras you already own (RTSP/ONVIF/Frigate); and a never-rot autopipeline ([the self-heal / self-publish / self-update mapping](tvos/AUTOPIPELINE.md))
- [Secure provisioning](secure_provisioning.md) — how a device earns its keys
- [The Canary Companion — a night-side clock and a pocket bird (design)](design/canary_companion.md) — **Phase 0 cores landed, host-tested in CI**: two products on the same Waveshare AMOLED 2.06 wrist board. **The Night Watch** inverts the display's night default because AMOLED makes black genuinely free — with the rule that outranks the owner's preference (*silence is never rendered as safety*: trouble, or a clock unsure of the time, breaks blackout every time), a light→haptic→sound gentle wake, and a wake-on-raise biased hard against the 3 a.m. roll-over. **The Pocket Canary** is a virtual pet built from the 1996 Tamagotchi research and against it — the care loop and the earned rarity kept, the death, the guilt, the hourly leash, the bell, the variable reward, the streak and the overfeeding trap each refused and each pinned by a test named after the mistake; plus the two-channel split (real fleet health owns the bird's posture, the child's care owns its growth) that lets a toy bird and a diagnostic bird share a codebase without either one lying
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
- [Court export](court_export.md) — the FRE 902(13)/(14) disclosure kit: evidence + digests + custody record + anchor tokens + certification drafts, verifiable with `sha256sum` and `openssl` alone
- [Sealed snapshot vault](sealed_snapshot_vault.md) · [scheduled exports](scheduled_exports.md)
- [C2PA Content Credentials for exports (design)](design/c2pa_export.md) — implemented behind `c2pa-export`: `export_events --c2pa` signs an industry-standard sidecar manifest any Content Credentials tool can verify — keys derived from the device seed, reproducible device-local CA, fully offline, chain stays the root of trust
- [The Witness Reading Room (scope)](design/witness_log_viewer.md) — scoping RFC for the log viewer + verifier: one app that verifies everything (chain, receipts, C2PA) *and* shows the record — timeline, disclosure audit, chain health; offline single-file first, live `witness_api` mode second, native full-fat verification third
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
- [WiFi CSI sensing quickstart](csi_quickstart.md) · [CSI modules](csi_modules.md) · [CSI developer API](csi_developer_api.md) · [Wi-Fi sensing research: the open-source landscape and what we took from it](csi_wifi_sensing_research.md)
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
- Learnings from elsewhere: [Marlin & Klipper](marlin_klipper_learnings.md) · [OpenIPC](openipc_architecture_learnings.md) · [Frigate → a fast Pi 5 + ESP32-S3 daylight pipeline](frigate_pi5_learnings.md)

</details>

<details>
<summary><strong>The engineering record</strong> — audits, reviews, research, and roadmaps</summary>

- **v1:** [roadmap](v1-roadmap.md) · [launch review](V1_LAUNCH_REVIEW.md) · [bench-test runbook](V1_BENCH_TEST_RUNBOOK.md)
- **Next steps:** [what the last ~100 PRs built, and what they left open (2026-07)](NEXT_STEPS_2026-07.md) — a July-2026 snapshot of the open edges after `#1080 → #1226`, checked against the tree at the time: the inert A/B rollback net, the Nightstand Line's tail, the missing fleet aggregator, and a three-wave sequence. Items closed since are marked *since closed* inline
- **Audits:** [firmware full audit 2026-07](audit/esp32s3_firmware_full_audit_2026-07.md) · [hardware verification checklist](audit/hardware_verification_checklist.md) · [mesh & chirp audit](audit/mesh_and_chirp_audit_v1.md) · [v0.3 closeout](audit/v0.3_closeout.md) · [WAP multi-device UX audit](audit/wap_multi_device_ux_audit.md) · [UX/UI audit 2026-06](ux_ui_audit_2026-06.md) · [legal, claims & risk audit 2026-07](legal-audit-2026-07.md)
- **Code reviews:** [Docker container](reviews/2026-06-10_docker_container_review.md) · [full repo](reviews/2026-06-10_full_repo_code_review.md) · [HA setup audit](reviews/2026-07-11_home_assistant_setup_audit.md) · [flasher hatching](reviews/2026-07-22_flasher_hatching_review.md) · [Arduino demo commands](reviews/arduino_demo_review_commands.md) · [Arduino demo tasks](reviews/arduino_demo_review_tasks.md) · [frame-trigger pipeline plan](reviews/kernel_frame_trigger_pipeline_plan.md)
- **Requirements review series:** [review/README](review/README.md)
- **Research:** [bitchat protocol](research/bitchat_protocol_review.md) · [**the competitor app landscape (Ring/Wyze/Eufy/Reolink, 2026-08)**](research/competitor_app_landscape.md) — what the four consumer security apps actually do, what they paywall, where they broke trust, the feature matrix against our surfaces, and the ranked match-and-exceed roadmap built only from capabilities the platform already has · [**the enterprise surveillance landscape — and the flip (2026-08)**](research/enterprise_surveillance_landscape.md) — the institutional companion: the ALPR dragnet (Flock Safety/FlockOS), the retail loss-prevention stack, and Palantir-class fusion, their dated liability record, the dimension-by-dimension structural table against our invariants, the honest-limits list (what a venue *cannot* do with us, and why that refusal is the product), and twelve ranked flip moves marked buildable-now / positioning / needs-design · [display market](research/display_market_research.md) · [harm-reduction prior art](research/harm_reduction_prior_art.md) · [pool water monitor](research/pool_water_monitor.md) · [Whisper local voice](research/whisper_local_voice.md)
- **Strategy series:** [strategy/README](strategy/README.md) — the numbered docs plus the whitepaper
- **Ecosystem & branding:** [brand & positioning](BRAND.md) — the canonical "the Canary" positioning, the one-line promise, and who we learn from · [trademark & branding policy](../TRADEMARK.md) · [trademark grants registry](trademark-grants.md) — the free "Works with SecuraCV" badge, the "… for SecuraCV" naming rule, and the public record of primary-mark grants (strategy doc 19 §5)
- **Legal:** [legal posture — copyright, licensing & the rules](LEGAL.md) — the canonical copyright line, Apache-2.0 rationale, inbound=outbound + DCO, the trademark moat strategy, claims-discipline rules, and the counsel punch list · [encryption & export note](ENCRYPTION.md)
- **Marketing:** [launch posts](marketing/launch_posts.md)

</details>

---

**Housekeeping rules** (kept by CI):

1. Every new doc gets a home on this map in the same PR that adds it —
   `scripts/lint_docs_index.py` fails otherwise.
2. A new docs directory gets its own `README.md`, linked from here; the
   directory then indexes itself.
3. Links here are relative, so the map works on GitHub, in editors, and offline.
