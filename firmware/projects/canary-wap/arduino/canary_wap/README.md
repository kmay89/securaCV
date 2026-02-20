# SecuraCV Canary WAP — Arduino IDE Build

Full-featured Privacy Witness Device firmware for the XIAO ESP32-S3 Sense.

This is the **complete** Arduino IDE version of the Canary WAP firmware, with full feature parity with the canonical WAP snapshot. All features are controlled via compile-time `#define` flags in `build_config.h`.

## Features

- **Cryptographic Witness Chain**: Ed25519 signing, SHA256 hash chain with domain separation, CBOR payloads
- **SD Card Storage**: Append-only witness records, health logs, chain state, export bundles
- **WiFi Access Point**: Device-unique SSID/password, TLS support, API authentication
- **REST API**: 40+ endpoints for status, chain, logs, camera, mesh, BLE, WiFi management
- **Web Dashboard**: Professional tabbed UI (Status, Peek, Logs, Chain, Export)
- **Camera Peek**: MJPEG streaming for positioning only (no frame storage — privacy by design)
- **GPS/GNSS**: Full telemetry with motion detection and hysteresis
- **Opera Mesh Network**: Peer-to-peer witness network with signed alerts
- **Bluetooth Low Energy**: Device discovery, chirp alerts, nearby Canary scanning
- **WiFi Presence Detection**: Privacy-preserving probe request counting
- **Health Monitoring**: Structured logging with severity levels, categories, and acknowledgment
- **Hardware Resilience**: Safe mode, watchdog, graceful degradation

## Hardware Requirements

- **Board**: Seeed XIAO ESP32-S3 Sense (primary) or XIAO ESP32-C3
- **GPS Module**: L76K GNSS (connected via UART)
- **SD Card**: microSD card (FAT32, any size)
- **Optional**: Buzzer/LED for audible chirp alerts

## Arduino IDE Setup

### 1. Board Installation

1. Open Arduino IDE
2. Go to **File > Preferences**
3. Add ESP32 board URL:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Go to **Tools > Board > Boards Manager**
5. Search and install **"esp32 by Espressif Systems"** (version 3.x)

### 2. Board Settings (Tools menu)

| Setting | Value |
|---------|-------|
| Board | XIAO_ESP32S3 (or ESP32S3 Dev Module) |
| USB CDC On Boot | **Enabled** |
| Flash Size | **8MB (64Mb)** |
| Partition Scheme | 8M with spiffs (3MB APP/1.5MB SPIFFS) |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |

### 3. Library Dependencies

Install via **Tools > Manage Libraries**:

| Library | Author | Version |
|---------|--------|---------|
| Crypto | Rhys Weatherley | ^0.4.0 |
| ArduinoJson | Benoit Blanchon | ^7.0.0 |

**Note:** `esp_camera` and `NimBLE` are built into the ESP32 Arduino Core — no separate install needed.

### 4. Hardware Target Selection

Edit `build_config.h` to select your hardware target:

```cpp
// Uncomment exactly ONE:
#define HARDWARE_XIAO_ESP32S3   // XIAO ESP32-S3 Sense (camera + PSRAM)
// #define HARDWARE_XIAO_ESP32C3   // XIAO ESP32-C3 (BLE only, no camera)
```

### 5. Build Profile Selection

Edit `build_config.h` to select your build profile:

```cpp
// Uncomment exactly ONE:
// #define BUILD_PROFILE_MINIMAL   // Crypto + GPS only (~45s build)
// #define BUILD_PROFILE_DEV       // + WiFi + HTTP + SD (~90s build)
#define BUILD_PROFILE_FULL         // All features (~150s build)
```

### 6. Build Tips

- Enable **"Aggressively cache compiled core"** in File > Preferences
- Use MINIMAL profile during development iteration
- Don't close Arduino IDE between builds (keeps cache warm)

## Connecting to Your Canary

1. Upload the sketch to your XIAO ESP32-S3
2. Open Serial Monitor at **115200 baud**
3. Note the WiFi SSID and password from the boot banner
4. Connect your phone/laptop to the `SecuraCV-XXXX` WiFi network
5. Open `https://192.168.4.1` (or `http://canary.local`)
6. On first boot, save the **Provisioning Receipt** from Serial output

## Security Properties

- Unique device identity from hardware TRNG
- Every witness record signed with Ed25519
- Tamper-evident hash chain with domain separation
- Time coarsened to 5-second buckets (privacy)
- No frame storage — camera peek is positioning only
- API token derived via HKDF (never stored in plaintext)
- Device-unique AP password derived from public key fingerprint
- Optional TLS with self-signed certificate

## File Structure

```
canary_wap/
├── canary_wap.ino          # Main sketch (must match folder name)
├── build_config.h          # Hardware target & build profile selection
├── log_level.h             # Log severity levels and categories
├── health_log.h            # Health logging declarations
├── nvs_store.h             # NVS manager singleton
├── sd_storage.h/.cpp       # SD card append-only storage
├── wap_server.h/.cpp       # WiFi AP + HTTP server stubs
├── web_ui.h                # Embedded web dashboard (PROGMEM)
├── api_auth.h              # API authentication (bearer token)
├── hardware_state.h        # Safe mode & hardware resilience
├── sys_monitor.h           # System metrics (temp, heap, PSRAM)
├── mesh_network.h/.cpp     # Opera mesh network
├── bluetooth_channel.h/.cpp # Bluetooth classic channel
├── ble_config.h            # BLE configuration
├── ble_manager.h           # BLE discovery subsystem
├── ble_opera.h             # Opera BLE advertising
├── ble_chirp.h             # Chirp BLE protocol
├── ble_nearby.h            # Nearby Canary scanning
├── bluetooth_api.h         # Bluetooth REST API handlers
├── rf_presence.h/.cpp      # RF presence detection
├── rf_presence_api.h       # RF presence REST API
├── chirp_channel.cpp       # Chirp community witness network
├── chirp_api.h             # Chirp REST API handlers
├── wifi_presence.h         # WiFi probe request monitoring
├── wifi_presence_api.h     # WiFi presence REST API
├── audible_chirp.h         # Buzzer/LED alert system
├── audible_chirp_api.h     # Audible chirp REST API
└── README.md               # This file
```
