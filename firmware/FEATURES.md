# SecuraCV Canary Firmware — Feature Audit Matrix

**Last updated:** 2026-07-11 (`_securacv._tcp` mDNS fleet adverts with the canonical TXT schema + HA MQTT Identify buttons on canary-vision and canary-sense — compile/CI-verified, hardware bench validation pending. 2026-07-02: canary-sense witness signing — Ed25519 events + NVS hash chain + wap-schema chain/health trust surface + task watchdog; earlier same day: Phase 2 network stack + canary-vision robustness parity)
**Original audit:** 2026-02-20
**Companion docs:** [VARIANT_POLICY.md](VARIANT_POLICY.md) (lifecycle labels), [FIRMWARE_VARIANT_AUDIT.md](FIRMWARE_VARIANT_AUDIT.md) (risk analysis), [PARITY_PLAN.md](PARITY_PLAN.md) (ACTIVE ⇄ canary-wap parity closure program)

**Variant identifiers used below:**

| Identifier | Path | Lifecycle |
|---|---|---|
| **canary (PIO)** | `firmware/canary/` | ACTIVE |
| **canary-wap (Arduino)** | `firmware/projects/canary-wap/arduino/canary_wap/` | COMPATIBILITY |
| **canary-wap (PIO)** | `firmware/projects/canary-wap/` | COMPATIBILITY |
| **canary-vision** | `firmware/projects/canary-vision/` | SPECIALIZED |
| **canary-sense** | `firmware/projects/canary-sense/` | SPECIALIZED |
| **canary-ota** | `firmware/projects/canary-ota/` | SPECIALIZED |
| **snapshot (removed)** | _(deleted 2026-05-29; history in git)_ | REMOVED (was ARCHIVED 2026-02-20) |

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Feature present and functional |
| ❌ | Feature missing entirely |
| ⚠️ | Feature partially implemented or stubbed |
| ➖ | Not applicable to this variant's scope |

---

## Feature-Parity Dashboard

Single-row-per-capability summary across every non-archived variant. This is the **authoritative at-a-glance view**; the deeper per-subsystem tables below remain as detail.

> **CI contract (enforced):** a PR that regresses a ✅ → ⚠️/❌ cell in this dashboard must include an issue reference in the PR body (e.g. `Regresses FEATURES.md: <cell> (#1234)`). The `.github/workflows/features-dashboard-guard.yml` workflow parses this table (via `firmware/scripts/features_dashboard_guard.py`) and fails if a cell downgrades without an accompanying `#<number>` reference in the PR description.

| Capability | canary (PIO) | canary-wap (Arduino) | canary-wap (PIO) | canary-vision | canary-sense | canary-ota | snapshot (archived) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Ed25519 signing of witness records | ✅ | ✅ | ⚠️ | ✅ | ✅ | ➖ | ✅ |
| SHA-256 hash-chain continuity (domain-separated, NVS-persisted) | ✅ | ✅ | ⚠️ | ✅ | ✅ | ➖ | ✅ |
| GPS (NMEA parse + fix FSM + motion hysteresis) | ✅ | ✅ | ⚠️ | ❌ | ➖ | ➖ | ✅ |
| SD storage (append-only, `/WITNESS` `/HEALTH` `/CHAIN` `/EXPORT`) | ✅ | ✅ | ⚠️ | ❌ | ➖ | ➖ | ✅ |
| SD graceful shutdown flush | ✅ | ✅ | ⚠️ | ➖ | ➖ | ➖ | ⚠️ |
| SD status counters (`witness_count` / `health_count` / `unacked_count`) | ✅ | ✅ | ⚠️ | ➖ | ➖ | ➖ | ⚠️ |
| WiFi AP (SecuraCV-XXXX SSID, device-unique password) | ✅ | ✅ | ⚠️ | ❌ | ❌ | ➖ | ✅ |
| WiFi STA (home network dual-mode) | ✅ | ✅ | ❌ | ✅ | ✅ | ➖ | ✅ |
| Web UI (embedded PROGMEM dashboard) | ⚠️ | ✅ | ⚠️ | ❌ | ❌ | ➖ | ✅ |
| Camera peek (MJPEG stream, no frame storage) | ⚠️ | ✅ | ⚠️ | ➖ | ➖ | ➖ | ✅ |
| Mesh network (Opera / ESP-NOW) | ⚠️ | ✅ | ⚠️ | ❌ | ❌ | ➖ | ✅ |
| Mesh RSSI from ESP-NOW radio | ⚠️ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| BLE discovery (Opera/Chirp/Nearby) | ❌ | ✅ | ❌ | ❌ | ❌ | ➖ | ✅ |
| BLE Scout (paired-beacon room attribution, hashed MAC, Kalman RSSI) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| RF presence detection | ❌ | ✅ | ⚠️ | ❌ | ❌ | ➖ | ✅ |
| WiFi CSI sensing (motion / breathing / micro-activity) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| CSI module pipeline + privacy chokepoint + 10-min bundler (v1: presence, breathing, ribbon, daily summary, anomaly) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| CSI active probe (50 Hz ESP-NOW unicast, deterministic frame rate) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Multi-link fusion (2-link confirmation gate, motion direction, breathing median) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Multipath shimmer filter (RSSI swing >8 dB without Doppler → reject) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| CSI watchdog (5 s silence → rx toggle; 3× escalation → WiFi restart) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Coordinated channel-hop (Hub proposes, peers follow, 50% util × 60 s trigger) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Hub failover election (lowest fingerprint wins, deterministic, no voting) | ⚠️ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Empty-room auto-calibration (10-min baseline, NVS-persisted) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| mmWave radar presence (MR60 frame parser + stall-safe FSM, 0/1/2+ bucket, range band) | ➖ | ➖ | ➖ | ➖ | ✅ | ➖ | ➖ |
| Radar vitals wellbeing channel (P1-gated breathing/heart lock, single-target suppression) | ➖ | ➖ | ➖ | ➖ | ⚠️ | ➖ | ➖ |
| Ambient light (BH1750) tamper-corroboration channel | ➖ | ➖ | ➖ | ➖ | ⚠️ | ➖ | ➖ |
| Acoustic alarm-cadence detection (T3 smoke / T4 CO) | ✅ | ❌ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Capacitive touch (silent panic / enclosure tamper) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Native deep-sleep HAL (esp_sleep_* abstraction; ULP-RISC-V capable) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| IR appliance activity (RMT NEC/RC5/Sony, salted-hash buckets) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Internal temp drift tamper (ESP32-S3 die sensor) | ✅ | ❌ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Sensing events signed into Ed25519 witness chain (T3/T4/panic/tamper/temp) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Home Assistant MQTT auto-discovery for sensing entities (11 entities) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Sensing dashboard panel (gauges + acoustic + touch + IR + temp + power) | ✅ | ❌ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Battery power monitor (ADC + software inference, SoC, charge state) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Power policy engine (6-mode battery-driven feature gating) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| First-time setup wizard (captive portal, device naming, NVS flag) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Heap monitoring + automatic feature degradation (3-level with hysteresis) | ✅ | ✅ | ❌ | ✅ | ✅ | ➖ | ❌ |
| SD card health tracking (write/error counters, space warnings) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| SD endurance wear estimate (NVS lifetime counters, TBW wear %, replace-recommended latch on MQTT health) | ✅ | ❌ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Boot self-test suite (10 subsystem probes, 0-100% health score) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| BLE GATT status service (battery + health + chain over BLE) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| WiFi power save (modem sleep on battery, TX power control) | ✅ | ❌ | ❌ | ⚠️ | ⚠️ | ➖ | ❌ |
| WiFi auto-reconnect with exponential backoff | ✅ | ❌ | ❌ | ✅ | ✅ | ➖ | ❌ |
| SD log rotation (witness 500, health 200, auto-rotate at 85% SD) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Chain backup/restore (NVS ↔ SD, HMAC-SHA256 integrity) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Chain integrity verification (Ed25519 sig + hash continuity walk) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ❌ |
| Witness record export to /EXPORT/ | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Battery health history (NVS-persisted charge cycles, voltage extremes) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ❌ |
| Chirp channel (broadcast beacon) | ⚠️ | ✅ | ⚠️ | ❌ | ❌ | ➖ | ✅ |
| MQTT publish + HA Discovery | ✅ | ✅ | ❌ | ✅ | ✅ | ➖ | ❌ |
| mDNS fleet advert (`_securacv._tcp`, canonical TXT schema incl. `dt`/`role` + broker gossip) | ⚠️ | ✅ | ✅ | ✅ | ✅ | ➖ | ❌ |
| Remote identify blink (HA `Identify` button on MQTT variants; HTTP `/api/identify` on WAP) | ❌ | ✅ | ✅ | ✅ | ✅ | ➖ | ❌ |
| OTA A/B with rollback safety | ✅ | ✅ | ✅ | ✅ | ⚠️ | ✅ | ❌ |
| Signed pull-OTA (HTTPS manifest + Ed25519 release signature) | ✅ | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| HA `update` entity (MQTT discovery, Install button + auto-update switch) | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |
| Local/LAN update server option (air-gapped hosting) | ✅ | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| API authentication (bearer token + HKDF derivation) | ✅ | ✅ | ❌ | ❌ | ➖ | ➖ | ✅ |
| Rate limiting on HTTP API | ✅ | ✅ | ❌ | ➖ | ➖ | ➖ | ✅ |
| TLS (HTTPS self-signed) | ❌ | ❌ | ❌ | ❌ | ❌ | ➖ | ✅ |
| Watchdog timer | ✅ | ✅ | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| Provisioning gate (BOOT button) | ✅ | ✅ | ❌ | ❌ | ❌ | ➖ | ✅ |
| Release-build fail-closed guards¹ | ✅ | ✅ | ⚠️ | ✅ | ❌ | ✅ | ✅ (archive-only) |

> **¹ Release-guard macro names differ per tree:** the canary (PIO) tree
> gates on `SECURACV_BUILD_RELEASE` (defined in `canary/platformio.ini`),
> while the canary-wap tree gates on the transposed `SECURACV_RELEASE_BUILD`
> (`build_config.h`). Each tree is internally consistent, but a guard copied
> verbatim between trees compiles as an always-false `#if defined(...)` and
> silently drops the protection — check the spelling when porting guards.

> **2026-07-11 rows:** the mDNS fleet advert and identify rows reflect code
> that is compile/CI-verified but not yet bench-verified on hardware for
> canary-vision and canary-sense. The canary (PIO) tree advertises
> `_securacv._tcp` with `device_id`/`fw`/`model` TXT keys but not yet the
> full canonical schema (`name`/`host`/`dt`/`role`/`broker`) — hence ⚠️.
> The canary-wap (PIO) cells are ✅ because that env builds the same
> `arduino/canary_wap/` sources (`src_dir` points at the sketch).

---


## Detailed Per-Subsystem Tables

> **Note:** The tables below originate from the 2026-02-20 audit and use **WAP Snapshot** as the original canonical reference column. That tree was archived on 2026-02-20 and **removed from the repository on 2026-05-29** (its history remains in git); the dashboard above reflects the current state. The **WAP Snapshot** columns are retained below purely as a historical parity record; when adding new capability rows, mirror them into the dashboard first.

## Core Cryptographic Witness Chain

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Ed25519 key generation (hardware RNG) | ✅ | ⚠️ Basic init only | ✅ securacv_crypto lib | ⚠️ Via common headers |
| Ed25519 signing of every witness record | ✅ | ❌ No signing | ✅ | ⚠️ Skeleton |
| SHA256 hash chain (domain-separated) | ✅ | ❌ | ✅ | ⚠️ Via common witness_chain.h |
| CBOR-encoded payloads | ✅ | ❌ | ✅ CborWriter class | ⚠️ Via common encoding/cbor.h |
| Self-verification (boot + periodic) | ✅ | ❌ | ✅ hal_crypto_self_test | ⚠️ |
| Sequence number persistence (NVS) | ✅ | ⚠️ Basic only | ✅ | ⚠️ |
| Boot attestation record | ✅ | ❌ | ✅ witness_chain_create_boot_attestation | ⚠️ |
| Tamper-evident append-only logging | ✅ | ❌ | ✅ | ⚠️ |
| Previous hash persistence (chain continuity) | ✅ | ❌ | ✅ | ⚠️ |

## NVS (Non-Volatile Storage)

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| NvsManager singleton | ✅ (nvs_store.h) | ❌ Direct Preferences | ✅ securacv_crypto lib | ⚠️ Via common |
| Device key pair stored in `securacv` ns | ✅ | ✅ | ✅ | ✅ |
| Boot count persistence | ✅ | ✅ | ✅ | ✅ |
| Chain sequence persistence | ✅ | ⚠️ Basic | ✅ | ⚠️ |
| Previous hash persistence | ✅ | ❌ | ✅ | ⚠️ |
| Factory reset via BOOT button | ✅ (short/long press) | ❌ | ✅ (5s hold) | ❌ |
| API token NVS storage | ✅ | ❌ | ❌ | ❌ |
| TLS cert NVS storage | ✅ | ❌ | ❌ | ❌ |
| WiFi credentials NVS storage | ✅ | ❌ | ❌ | ❌ |

## SD Card Storage

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Append-only file storage | ✅ (sd_storage.h) | ⚠️ Mount only | ✅ securacv_storage lib | ⚠️ Via common |
| Directory structure (/WITNESS, /HEALTH, /CHAIN, /EXPORT) | ✅ | ❌ | ✅ | ⚠️ |
| SPI pin config (CS=21, SCK=7, MISO=8, MOSI=9) | ✅ | ✅ | ✅ | ✅ |
| Graceful degradation if absent | ✅ | ✅ | ✅ | ✅ |
| Hot-plug/unplug detection | ✅ | ❌ | ❌ | ❌ |
| Safe mount with timeout | ✅ | ❌ | ❌ | ❌ |

## WiFi Access Point

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| AP mode with dynamic SSID | ✅ SecuraCV-<MAC> | ✅ | ✅ | ✅ |
| Device-unique AP password | ✅ (derived from fingerprint) | ❌ Static default | ✅ (derived from pubkey fp) | ❌ Static default |
| Max client limit (1) | ✅ Hardened | ❌ (4 clients) | ✅ (1 client) | ❌ (configurable) |
| mDNS (canary.local + _securacv._tcp, device_id in TXT record) | ✅ | ✅ (2026-07: unique `canary-<name>.local` + catch-all + canonical `dt`/`role` TXT schema) | ✅ | ✅ (same sources as Arduino IDE via `src_dir`) |
| Rate limiting on API | ✅ | ❌ | ✅ (120 req/min) | ❌ |
| TLS (HTTPS) support | ✅ Self-signed cert | ❌ | ❌ | ❌ |
| WiFi STA (home network connect) | ✅ | ❌ | ✅ (AP+STA dual mode) | ❌ |
| Captive portal redirect | ✅ | ❌ | ❌ | ❌ |

## REST API Endpoints

| Endpoint | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|----------|:---:|:---:|:---:|:---:|
| `GET /` (web dashboard) | ✅ Full PROGMEM UI | ✅ Minimal inline | ✅ web_ui_register_routes | ⚠️ |
| `GET /api/status` | ✅ Full JSON | ✅ Basic JSON | ✅ http_register_standard_api | ⚠️ |
| `GET /api/device-info` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/provisioning-receipt` | ✅ (BOOT button gated) | ❌ | ❌ | ❌ |
| `GET /api/system` (sys metrics) | ✅ | ❌ | ❌ | ❌ |
| `GET /api/chain` | ✅ | ❌ | ✅ | ❌ |
| `GET /api/logs` | ✅ | ❌ | ✅ | ❌ |
| `POST /api/logs/*/ack` | ✅ | ❌ | ✅ | ❌ |
| `POST /api/logs/ack-all` | ✅ | ❌ | ✅ | ❌ |
| `GET /api/witness` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/config` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/export` | ✅ PWK bundle | ❌ | ✅ | ❌ |
| `POST /api/reboot` | ✅ | ❌ | ✅ | ❌ |
| `GET /api/wifi/status` | ✅ | ❌ | ✅ | ❌ |
| `GET /api/wifi/scan` | ✅ | ❌ | ✅ | ❌ |
| `POST /api/wifi/connect` | ✅ | ❌ | ✅ | ❌ |
| `POST /api/wifi/disconnect` | ✅ | ❌ | ✅ | ❌ |
| `POST /api/wifi/forget` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/wifi/reconnect` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/peek/start` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/peek/stream` (MJPEG) | ✅ | ❌ | ❌ | ❌ |
| `GET /api/peek/snapshot` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/peek/stop` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/peek/status` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/peek/resolution` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/mesh` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/mesh/peers` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/mesh/alerts` | ✅ | ❌ | ❌ | ❌ |
| Mesh pair/leave/remove endpoints | ✅ (6 endpoints) | ❌ | ❌ | ❌ |
| `GET /api/ble/status` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/nearby` | ✅ | ❌ | ❌ | ❌ |
| `POST /api/chirp/send` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/download` | ✅ | ❌ | ❌ | ❌ |
| `GET /api/diagnostics` (heap, SD health, degradation, selftest) | ✅ | ✅ | ✅ | ❌ |
| `GET /api/selftest` (re-run self-test suite on demand) | ✅ | ✅ | ✅ | ❌ |
| `GET /api/battery/history` (NVS-persisted battery health stats) | ✅ | ✅ | ✅ | ❌ |
| `GET /api/sensing` (per-source sensor telemetry) | ✅ | ❌ | ❌ | ❌ |

## Camera Peek Feature

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Feature flag `FEATURE_CAMERA_PEEK` | ✅ | ❌ | ✅ | ⚠️ |
| XIAO ESP32S3 Sense camera pins | ✅ | ❌ | ✅ securacv_camera lib | ⚠️ |
| VGA resolution MJPEG stream | ✅ | ❌ | ❌ | ❌ |
| Resolution controls | ✅ | ❌ | ❌ | ❌ |
| No frame storage (privacy) | ✅ | N/A | ✅ | ✅ |
| Graceful degradation | ✅ | N/A | ✅ | ⚠️ |
| Camera status in web UI | ✅ | ❌ | ❌ | ❌ |

## GPS Telemetry

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| GPS fix state tracking | ✅ (full FSM) | ❌ | ✅ `securacv_gps` (own NMEA parser) | ⚠️ |
| Lat/Lon in witness records | ✅ | ❌ | ✅ | ⚠️ |
| Speed, altitude, heading | ✅ | ❌ | ✅ | ⚠️ |
| NMEA parsing (GGA, RMC, GSA, VTG) | ✅ | ❌ | ✅ | ⚠️ |
| GPS probe with timeout | ✅ | ❌ | ❌ | ❌ |
| Motion detection with hysteresis | ✅ (EMA + debounce) | ❌ | ❌ | ❌ |

## Health Monitoring

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Log severity levels (DEBUG..TAMPER) | ✅ log_level.h | ❌ | ⚠️ | ⚠️ |
| Log categories (BOOT..SYSTEM) | ✅ | ❌ | ❌ | ❌ |
| Log acknowledgment system | ✅ (append-only ack) | ❌ | ❌ | ❌ |
| Circular log ring buffer | ✅ (100 entries) | ❌ | ❌ | ❌ |
| SD health log persistence | ✅ | ❌ | ⚠️ | ❌ |
| System monitor (temp, heap, PSRAM) | ✅ sys_monitor.h | ❌ | ❌ | ❌ |

## Web Dashboard (embedded in PROGMEM)

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Professional tabbed dashboard | ✅ web_ui.h | ❌ Basic card | ⚠️ securacv_webui lib | ⚠️ |
| Status tab | ✅ | ⚠️ Basic | ⚠️ | ⚠️ |
| Peek (camera) tab | ✅ | ❌ | ❌ | ❌ |
| Logs tab with filtering | ✅ | ❌ | ❌ | ❌ |
| Chain tab | ✅ | ❌ | ❌ | ❌ |
| Export tab | ✅ | ❌ | ❌ | ❌ |
| Dark professional theme | ✅ | ⚠️ Basic dark | ⚠️ | ⚠️ |
| Mobile responsive | ✅ | ⚠️ | ⚠️ | ⚠️ |
| Real-time polling | ✅ | ✅ 5s poll | ⚠️ | ⚠️ |

## Additional WAP Features

| Feature | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|---------|:---:|:---:|:---:|:---:|
| Opera mesh network | ✅ mesh_network.h/.cpp | ❌ | ⚠️ mesh_network.h header | ⚠️ |
| Bluetooth channel | ✅ bluetooth_channel.h/.cpp | ❌ | ⚠️ bluetooth_mgr.h header | ⚠️ |
| BLE Discovery (Opera/Chirp/Nearby) | ✅ ble_manager.h | ❌ | ❌ | ❌ |
| WiFi presence detection | ✅ wifi_presence.h | ❌ | ❌ | ❌ |
| Audible chirp (buzzer/LED alerts) | ✅ audible_chirp.h | ❌ | ❌ | ❌ |
| RF presence detection | ✅ rf_presence.h/.cpp | ❌ | ⚠️ rf_presence.h header | ⚠️ |
| Chirp channel | ✅ chirp_channel.cpp | ❌ | ⚠️ chirp_channel.h header | ⚠️ |
| Hardware state & safe mode | ✅ hardware_state.h | ❌ | ❌ | ❌ |
| API authentication (bearer token) | ✅ api_auth.h | ❌ | ✅ securacv_auth lib | ❌ |
| Provisioning gate (BOOT button) | ✅ | ❌ | ❌ | ❌ |
| HKDF API token derivation | ✅ | ❌ | ✅ securacv_crypto::derive_api_token | ❌ |
| Serial command handler | ✅ (h/i/s/t/g/c/m) | ❌ | ✅ (h/i/s/g/m/r) | ❌ |
| Watchdog timer | ✅ | ❌ | ✅ | ⚠️ |
| OTA update support | ❌ | ❌ | ✅ FEATURE_OTA_UPDATE | ❌ |

## Feature Flags Comparison

| Flag | WAP Snapshot | Arduino IDE | PlatformIO (canary/) | PlatformIO (canary-wap/) |
|------|:---:|:---:|:---:|:---:|
| `FEATURE_CAMERA_PEEK` | ✅ | ❌ | ✅ | ⚠️ |
| `FEATURE_GPS` / `FEATURE_GNSS` | ✅ (implicit) | ❌ | ❌ | ✅ FEATURE_GNSS |
| `FEATURE_SD_STORAGE` / `FEATURE_SD_CARD` | ✅ | ❌ | ✅ | ✅ |
| `FEATURE_WIFI_AP` / `FEATURE_WAP` | ✅ | ❌ (always on) | ✅ | ✅ |
| `FEATURE_HTTP_SERVER` | ✅ | ❌ (always on) | ✅ | ✅ |
| `FEATURE_MESH_NETWORK` | ✅ | ❌ | ✅ | ✅ |
| `FEATURE_BLUETOOTH` | ✅ | ❌ | ✅ | ✅ |
| `FEATURE_BLE` | ✅ | ❌ | ❌ | ❌ |
| `FEATURE_WIFI_PRESENCE` | ✅ | ❌ | ❌ | ❌ |
| `FEATURE_AUDIBLE_CHIRP` | ✅ | ❌ | ❌ | ❌ |
| `FEATURE_SYS_MONITOR` | ✅ | ❌ | ❌ | ❌ |
| `FEATURE_WATCHDOG` | ✅ | ❌ | ✅ | ✅ |
| `FEATURE_STATE_LOG` | ✅ | ❌ | ✅ | ✅ |
| `FEATURE_TAMPER_GPIO` | ✅ | ❌ | ✅ | ⚠️ |
| `FEATURE_RF_PRESENCE` | ✅ | ❌ | ❌ | ✅ |
| `FEATURE_CHIRP` | ✅ | ❌ | ❌ | ✅ |
| `FEATURE_VAULT_SNAPSHOT` | ✅ FULL/S3 only (camera + mic; all triggers off by default) | ❌ | ❌ | ❌ |
| `FEATURE_OTA_UPDATE` | ❌ | ❌ | ✅ | ❌ |
| `FEATURE_HA_MQTT` | ❌ | ❌ | ✅ | ❌ |
| `DEBUG_VERBOSE` / `DEBUG_*` | ✅ (5 flags) | ❌ | ✅ (5 flags) | ⚠️ |

## Provisioning System

| Component | Status | Notes |
|-----------|--------|-------|
| `generate_keys.sh` | ✅ | RSA-3072 + XTS-AES key generation, entropy checks |
| `verify_device.py` | ✅ | eFuse verification (pre/post provisioning) |
| `provision_canary.sh` | ✅ | Full workflow with --dry-run |
| `create_manifest.py` | ✅ | Fleet device manifest management |
| `platformio_secure.ini` | ✅ | Secure Boot v2 + Flash Encryption env |
| `partitions_secure.csv` | ✅ | OTA A/B + encrypted NVS |
| BT disabled at compile time | ✅ | CVE-2025-27840 mitigation |

## Home Assistant Integration

| Feature | Status | Notes |
|---------|--------|-------|
| Custom component (`custom_components/securacv/`) | ✅ | |
| Hub integration type | Needs verification | |
| MQTT auto-discovery | ✅ | Per-device sensors |
| Per-device sensors | ✅ | witness count, chain length, health |
| Binary sensors | ✅ | online, tamper_detected, chain_valid |
| Services | ✅ | export_chain, verify_chain |
| No camera entities | ✅ | Privacy by design |
| Multi-device support | ✅ | MQTT topic: `securacv/{DEVICE_ID}/...` |
| MQTT publishing from firmware | ✅ | securacv_mqtt lib with HA Discovery |
| MQTT-only config flow | ✅ | No kernel required for Canary-only setups |
| HA Blueprint for alerts | ✅ | One-click notification setup |
| Notification automations | ✅ | Tamper, chain fail, offline, GPS loss |

## Fleet Management UI

| Feature | Status | Notes |
|---------|--------|-------|
| canary-vision SPA | ✅ | Standalone web app for device management |
| Multi-device discovery | ⚠️ | Needs verification of scalability |
| Pagination for 50+ devices | ❌ | Not implemented |
| Fleet health aggregation | ❌ | Not implemented |
| Individual device drill-down | ⚠️ | Basic |
| Mobile responsive | ⚠️ | Basic |

---

## Summary

Post-archive (2026-04), the ACTIVE canonical tree is `firmware/canary/` (PlatformIO). The WAP Snapshot tree — archived 2026-02-20 — was removed from the repository on 2026-05-29; its history remains in git and the WAP UX it captured lives on in the COMPATIBILITY tree (`firmware/projects/canary-wap/`).

- **canary-wap Arduino (COMPATIBILITY)**: ~100% WAP parity; recently hardened (real ESP-NOW RSSI 2026-04, SD flush-on-unmount 2026-04). MQTT publish + HA Discovery is now present here too (`csi_mqtt.cpp`, compiled in the Arduino CLI build; HA side validated by `hassfest`) — see the 2026-06-09 reconciliation note in [PARITY_PLAN.md](PARITY_PLAN.md).
- **canary (PIO, ACTIVE)**: ~88% feature parity — modular libs, MQTT + HA Discovery, WiFi STA, export, storage status counters (2026-04), HKDF-derived bearer token gating every SPA-driven endpoint (2026-04). Gaps: camera streaming, full GPS motion FSM, some web UI tabs.
- **canary-vision (SPECIALIZED)**: 2026-07 robustness parity with the S3 tree — supervised WiFi STA (exponential-backoff reconnect + outage reboot), WiFi power-save policy (⚠️ pending bench), heap monitor with 3-level degradation that stretches the inference cadence under pressure, and RSSI/heap HA diagnostic entities. 2026-07-11 adds the `_securacv._tcp` mDNS fleet advert (canonical TXT schema; `DEVICE_TYPE` is now the canonical hyphenated `canary-vision`) and the HA Identify button (bench validation pending).
- **canary-sense (SPECIALIZED)**: Phase 2 landed 2026-07 — the MR60 radar witness now publishes: supervised WiFi STA, MQTT with LWT + HA discovery (presence / occupants / range band / radar-link health / illuminance; wellbeing builds add the P0 breathing lock and P1-gated BPM entities), NVS-backed runtime config, heap diagnostics, and the shared signed pull-OTA engine with HA update entity. OTA cells sit at ⚠️ until the engine is bench-proven on the ESP32-C6 (new MCU for the A/B flow). Ed25519 witness-chain signing landed 2026-07-02 (events signed over the v1 `sense` canonical + NVS hash chain + wap-schema chain/health trust surface) — ✅ in the dashboard. 2026-07-11 adds the `_securacv._tcp` mDNS fleet advert and the HA Identify button (bench validation pending).
- **canary-wap (PIO, COMPATIBILITY)**: ~40% parity — uses common headers, many implementations still skeleton.

### Priority Actions

1. **canary (PIO)**: Close the remaining UX gaps (camera streaming, full dashboard tabs) so it can fully replace the Arduino compatibility lane.
2. **canary-wap (PIO)**: Implement common module bodies that currently exist only as headers; decide whether to retire this lane in favor of the Arduino compatibility tree + canary.
3. **Fleet management**: Create scalable multi-device dashboard (canary-vision SPA follow-up).
4. **Dashboard integrity**: Wire CI to fail on `✅ → ⚠️/❌` regressions in the Feature-Parity Dashboard unless the PR cites an issue.
