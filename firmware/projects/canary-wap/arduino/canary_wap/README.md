# SecuraCV Canary WAP — Arduino IDE Build

Full-featured Privacy Witness Device firmware for the XIAO ESP32-S3 Sense.

This is the **complete** Arduino IDE version of the Canary WAP firmware — the full WAP feature set (witness chain, SD storage, web dashboard, mesh, BLE, camera peek) in a single sketch tree. All features are controlled via compile-time `#define` flags in `build_config.h`. The same sources are built by PlatformIO (`platformio.ini` points `src_dir` here), so both toolchains produce the same firmware.

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

### 0. Stage environment-specific sources (required before first compile)

The CSI library files (`csi_*.{h,cpp}`, `core_*.{h,cpp}`,
`anomaly_baseline.*`, `meta_daily_summary.*`) and the header-only
`device_pseudonym.h` are committed alongside the sketch, so a fresh GitHub zip
download compiles without any staging step for the library itself. Their
canonical copies live at `firmware/common/csi/src/` and
`firmware/common/identity/`; a CI guard (`firmware/scripts/check_csi_sync.sh`)
keeps the committed copies in sync with the canonical sources.

You still need to stage the environment-specific files (board pins, default
config, secrets) once before the first build:

```bash
cd firmware/projects/canary-wap
./setup.sh arduino
```

If you skip this, the build fails with errors about `pins.h` / `config.h`.
Re-run the script any time the shared board/config sources change. (The
script also creates a `secrets.h`, but nothing reads it — canary-wap takes no
compile-time credentials; Wi-Fi is provisioned over the captive portal and
the AP password and API token are derived per device. See
`secrets/secrets.example.h`.)

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
| Partition Scheme | 8M with spiffs (3MB APP/1.5MB SPIFFS) — this menu entry is the core's `default_8MB.csv` table |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |

> **⚠️ PSRAM defaults to "Disabled"** in the XIAO_ESP32S3 board definition. If
> you leave it, the camera boots but silently falls back to QVGA-in-DRAM (you'll
> see `[CAMERA] PSRAM: not found` in the serial log). You **must** set
> **PSRAM → OPI PSRAM** for full-resolution streaming.
>
> **Skip the manual settings:** Arduino IDE 2.3+ reads the committed
> [`sketch.yaml`](sketch.yaml) — open the sketch and select the **`xiao_sense`**
> profile from the toolbar dropdown to apply the board + PSRAM options
> automatically. On the command line: `arduino-cli compile --profile xiao_sense`
> (or `make arduino-build` from `firmware/projects/canary-wap`, which uses the
> same PSRAM-enabled FQBN).

> **Partition/filesystem parity with PlatformIO:** both toolchains use the
> same **`default_8MB.csv`** partition table (the Arduino IDE menu labels it
> "8M with spiffs (3MB APP/1.5MB SPIFFS)"; `sketch.yaml` and
> `envs/platformio/canary-wap.ini` both pin it), so the app/OTA slot layout is
> identical. They differ in one deliberate way: the PlatformIO env sets
> `board_build.filesystem = littlefs`, so a data-partition image built by
> PlatformIO is LittleFS, while the Arduino IDE's default for that partition
> is SPIFFS. The firmware stores witness data on the SD card, not the internal
> data partition, so this difference does not affect runtime behavior — but
> the images are not bit-identical.

### 3. Library Dependencies

Install via **Tools > Manage Libraries**:

| Library | Author | Version |
|---------|--------|---------|
| Crypto | Rhys Weatherley | ^0.4.0 |
| ArduinoJson | Benoit Blanchon | ^7.0.0 |
| NimBLE-Arduino | h2zero | ^2.3.8 |

**Note:** `esp_camera` is built into the ESP32 Arduino Core, but **`NimBLE-Arduino` (2.x) must be installed separately**. The default (FULL) profile enables Bluetooth, and `ble_config.h` now *errors* at compile time if `<NimBLEDevice.h>` is missing rather than silently shipping a build with BLE disabled — so install NimBLE-Arduino (by h2zero) before your first build. CI installs the same library (`arduino-cli lib install "NimBLE-Arduino"`).

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

### 7. Troubleshooting

**`Multiple libraries were found for "SD.h"`**

This is a *warning*, not an error — the build still succeeds. The compiler
correctly picks the ESP32 core's bundled `SD` library:

```
Multiple libraries were found for "SD.h"
  Used:     .../packages/esp32/hardware/esp32/<ver>/libraries/SD   <- correct
  Not used: .../Arduino15/libraries/SD
  Not used: .../Documents/Arduino/libraries/SD
```

It appears because one or more standalone `SD` libraries are also installed
in a user libraries folder. The exact location depends on your IDE version
and platform — on macOS the candidates are:

- `~/Documents/Arduino/libraries/SD` — IDE 1.x sketchbook
- `~/Arduino/libraries/SD` — IDE 2.x sketchbook
- `~/Library/Arduino15/libraries/SD` — Library-Manager / data dir

Check the `Not used:` lines in your own build output for the real paths — they
tell you exactly which copies the compiler skipped. The ESP32 build needs the
*core-bundled* `SD`, so those standalone copies are redundant. To silence the
warning, delete the ones the build reported as unused — for example on macOS:

```sh
rm -rf ~/Documents/Arduino/libraries/SD
rm -rf ~/Arduino/libraries/SD
rm -rf ~/Library/Arduino15/libraries/SD
```

Leave the `.../packages/esp32/.../libraries/SD` copy in place — that's the one
the build uses. (`arduino-cli` reports the same warning and resolves it the
same way; no sketch change is needed.)

## Connecting to Your Canary

1. Upload the sketch to your XIAO ESP32-S3
2. Open Serial Monitor at **115200 baud**
3. Note the WiFi SSID and password from the boot banner
4. Connect your phone/laptop to the `SecuraCV-XXXX` WiFi network
5. Open `https://192.168.4.1` (or the device's unique mDNS URL, e.g. `http://canary-s3-ab7k.local` — shown in the boot banner)
6. On first boot, save the **Provisioning Receipt** from Serial output

## Security Properties

- Unique device identity from hardware TRNG
- Every witness record signed with Ed25519
- Tamper-evident hash chain with domain separation
- Time coarsened to 5-second buckets (privacy)
- No frame storage — camera peek is positioning only
- API token derived via HKDF (never stored in plaintext)
- Device-unique AP password derived from the device private key (HMAC, domain-separated)
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
