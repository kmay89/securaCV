# SecuraCV Firmware

This directory contains firmware projects for SecuraCV hardware nodes (ESP32, etc.). Each firmware project publishes **privacy-preserving semantic telemetry** (events + state), not raw video.

## Architecture (Required, Normative)

Before adding or restructuring firmware work, read the canonical architecture guide:

- `firmware/ARCHITECTURE.md`

This guide is **normative** for the `firmware/` subtree. If another document
conflicts, the architecture guide takes precedence.

### Required Layout

```
firmware/
├── ARCHITECTURE.md     # Normative architecture document
├── README.md           # This file
├── boards/             # Board definitions and pin maps
│   ├── xiao-esp32s3-sense/  # XIAO ESP32-S3 Sense
│   └── esp32-c3/            # ESP32-C3 DevKit
├── common/             # Board-agnostic core logic
│   ├── core/           # Types, logging, utilities
│   ├── hal/            # Hardware abstraction layer
│   ├── witness/        # Witness chain management
│   ├── gnss/           # GPS/GNSS parsing
│   ├── storage/        # NVS and SD storage
│   ├── network/        # Mesh networking
│   ├── bluetooth/      # BLE management
│   ├── rf_presence/    # Privacy-preserving RF detection
│   └── web/            # HTTP server and Web UI
├── configs/            # App/product configurations
│   ├── canary-wap/     # WAP device configs
│   └── canary-vision/  # Vision device configs
├── envs/               # Build environments
│   └── platformio/     # PlatformIO configurations
└── projects/           # Product wrappers
    ├── canary-wap/     # WAP witness device
    ├── canary-vision/  # Vision AI presence detector
    └── canary-wap-snapshot/  # Legacy reference
```

Key rule: **composition happens only in `envs/` and `projects/`**. Core logic
never imports boards or configs.

## Projects

### Canary WAP (ESP32-S3 + GPS + SD + Mesh)

Path: `firmware/projects/canary-wap/`

Full-featured Wireless Access Point witness device with:
- Ed25519 signed, hash-chained witness records
- GPS/GNSS location tracking with time coarsening
- SD card append-only storage
- WiFi AP for local monitoring
- HTTP REST API and Web UI
- Opera protocol mesh networking
- Bluetooth pairing and configuration
- Privacy-preserving RF presence detection

Build configurations:
- `canary-wap-default` - Full-featured
- `canary-wap-mobile` - Power-optimized for battery
- `canary-wap-debug` - Verbose logging

### Canary Vision (ESP32-C3 + Grove Vision AI V2)

Path: `firmware/projects/canary-vision/`

Vision AI presence detection with:
- Person detection using SSCMA
- Presence/dwelling state machine
- MQTT publishing to Home Assistant
- Auto-discovery for HA integration

Key MQTT topics:
- `securacv/<device_id>/events`  (non-retained JSON)
- `securacv/<device_id>/state`   (retained JSON snapshot)
- `securacv/<device_id>/status`  (retained JSON status)

Home Assistant discovery topics:
- `homeassistant/binary_sensor/<device_id>/presence/config`
- `homeassistant/binary_sensor/<device_id>/dwelling/config`
- `homeassistant/sensor/<device_id>/confidence/config`
- `homeassistant/sensor/<device_id>/voxel/config`
- `homeassistant/sensor/<device_id>/last_event/config`
- `homeassistant/sensor/<device_id>/uptime/config`

### Canary WAP Snapshot (Legacy Reference)

Path: `firmware/projects/canary-wap-snapshot/`

Purpose:
- Preserves a working Arduino sketch as a frozen snapshot
- Reference implementation for WAP features
- Historical baseline (do not modify)

## Quick Start

### Option 1: Arduino IDE (Easiest)

Generate an Arduino-compatible sketch and open in Arduino IDE:

```bash
cd firmware

# Build for Canary WAP (full-featured)
make arduino APP=canary-wap CONFIG=default

# Build for Canary WAP (power-optimized)
make arduino APP=canary-wap CONFIG=mobile

# Build for Canary Vision
make arduino APP=canary-vision CONFIG=default
```

This creates a ready-to-use sketch in `build/arduino/<name>/`. Open the `.ino`
file in Arduino IDE, configure your board, and upload.

**Arduino IDE Setup:**

1. Install ESP32 board support:
   - Preferences > Additional Board URLs:
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   - Board Manager > Install "esp32 by Espressif Systems"

2. Select board:
   - Canary WAP: `XIAO_ESP32S3`
   - Canary Vision: `ESP32C3 Dev Module`

3. Install libraries (Library Manager):
   - ArduinoJson
   - NimBLE-Arduino
   - TinyGPSPlus

### Option 2: PlatformIO (Recommended for Development)

```bash
cd firmware

# Build
make pio-build APP=canary-wap CONFIG=default

# Build and upload
make pio-upload APP=canary-wap CONFIG=default

# Serial monitor
make pio-monitor APP=canary-wap
```

Or use PlatformIO directly:

```bash
cd firmware/projects/canary-wap
pio run -e canary-wap-default
pio run -e canary-wap-default -t upload
pio device monitor -b 115200
```

### List Available Configurations

```bash
make list-configs
```

## Secrets (Never Commit)

Keep secrets local only:
- WiFi SSID/password
- MQTT credentials
- API tokens / private keys

This repo’s `.gitignore` and the firmware project’s `.gitignore` are configured
to prevent secret commits by default.

## Troubleshooting

If Home Assistant does not auto-discover entities, verify:
- MQTT integration is configured with discovery enabled
- Broker allows retained messages
- Device publishes discovery topics under the `homeassistant/` prefix
