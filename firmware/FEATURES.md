# SecuraCV Canary Firmware — Feature Audit Matrix

**Audit Date:** 2026-02-20
**WAP Snapshot Version:** 2.1.0-wap (canonical reference)
**Arduino IDE Version:** 2.1.0-arduino (simplified)
**PlatformIO Canary Version:** modular library build

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Feature present and functional |
| ❌ | Feature missing entirely |
| ⚠️ | Feature partially implemented or stubbed |

---

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
| mDNS (canary.local) | ✅ | ❌ | ✅ | ❌ |
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
| GPS fix state tracking | ✅ (full FSM) | ❌ | ✅ gnss_parser_t | ⚠️ |
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
| API authentication (bearer token) | ✅ api_auth.h | ❌ | ❌ | ❌ |
| Provisioning gate (BOOT button) | ✅ | ❌ | ❌ | ❌ |
| HKDF API token derivation | ✅ | ❌ | ❌ | ❌ |
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

The **WAP Snapshot** is the canonical reference with ~45+ features. The other build targets have significant gaps:

- **Arduino IDE build**: ~15% feature parity (basic WiFi AP + crypto init, no hash chain, no camera, no GPS, no health logs, no chain export, minimal web UI)
- **PlatformIO canary/**: ~75% feature parity (MQTT publishing, HA Discovery, rate limiting, factory reset, device-unique AP password, WiFi STA, export — still missing camera streaming, GPS motion FSM, full web UI tabs, API auth)
- **PlatformIO canary-wap/**: ~40% feature parity (uses common headers, good config system, but implementations are skeleton/stub)

### Priority Actions

1. **Arduino IDE build**: Complete rewrite needed — replace stub with full WAP-equivalent implementation
2. **PlatformIO canary/**: Fill in missing API endpoints, camera streaming, web UI tabs
3. **PlatformIO canary-wap/**: Implement common module bodies that currently exist only as headers
4. **Fleet management**: Create scalable multi-device dashboard
