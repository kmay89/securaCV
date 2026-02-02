# SecuraCV Firmware

This directory contains firmware projects for SecuraCV hardware nodes (ESP32, etc.). Each firmware project is designed to publish **privacy-preserving semantic telemetry** (events + state), not raw video.

## Architecture (Required)

Before adding or restructuring firmware work, read the canonical architecture guide:

- `firmware/ARCHITECTURE.md`

## Projects

### Canary Vision (ESP32-C3 + Grove Vision AI V2)

Path: `firmware/projects/canary-vision/`

Purpose:
- Publishes semantic events like `presence_started`, `dwell_started`, `interaction_likely`
- Publishes retained state snapshots for Home Assistant
- Publishes **MQTT Discovery** config so Home Assistant auto-registers entities

Key topics (by default):
- `securacv/<device_id>/events`  (non-retained JSON)
- `securacv/<device_id>/state`   (retained JSON snapshot)
- `securacv/<device_id>/status`  (retained JSON status: online/offline)

Home Assistant discovery topics:
- `homeassistant/binary_sensor/<device_id>/presence/config`
- `homeassistant/binary_sensor/<device_id>/dwelling/config`
- `homeassistant/sensor/<device_id>/confidence/config`
- `homeassistant/sensor/<device_id>/voxel/config`
- `homeassistant/sensor/<device_id>/last_event/config`
- `homeassistant/sensor/<device_id>/uptime/config`

## Build & Flash (PlatformIO)

Prereqs:
- PlatformIO installed (VS Code extension or CLI)
- A configured `secrets/secrets.h` file (never committed)

### Using PlatformIO CLI

From the repo root:

```bash
cd firmware/projects/canary-vision
pio run
pio run -t upload
pio device monitor -b 115200

Secrets
Firmware projects must keep secrets local.

########Do not commit:

WiFi SSID/password
MQTT credentials
API tokens / private keys

This repo’s .gitignore and the firmware project’s .gitignore are configured to prevent secret commits by default.
Notes
If Home Assistant does not auto-discover entities, verify:
MQTT integration is configured with discovery enabled
broker allows retained messages
device publishes discovery topics under the homeassistant/ prefix
