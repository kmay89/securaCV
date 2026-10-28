# Canary Vision - Default Configuration

Standard configuration for the Vision AI presence detection device.

This directory's `config.h` holds what something reads from it: the
`FEATURE_*` flags, the watchdog timeout, and a copy of the device type the
fleet figures read. Every other compiled setting it used to repeat, the
detection defaults below among them, is set in the project's own
`include/canary/config.h` (topic patterns in `include/canary/topics.h`),
under `firmware/projects/canary-vision/`.

## Target Hardware

- **Board**: ESP32-C3 DevKitM-1
- **Sensor**: Grove Vision AI V2 (Seeed)

## Features Enabled

| Feature | Status | Description |
|---------|--------|-------------|
| Vision AI | ✅ | Person detection with SSCMA |
| WiFi STA | ✅ | Connect to home network |
| MQTT | ✅ | Publish events to broker |
| HA Discovery | ✅ | Home Assistant auto-discovery |
| Watchdog | ✅ | Hardware watchdog timer |

## MQTT Topics

| Topic | Retained | Description |
|-------|----------|-------------|
| `securacv/{id}/events` | No | Event notifications |
| `securacv/{id}/state` | Yes | State snapshot |
| `securacv/{id}/status` | Yes | Online/offline |

## Home Assistant Entities

Created automatically via MQTT Discovery:

- `binary_sensor.canary_vision_001_presence`
- `binary_sensor.canary_vision_001_dwelling`
- `sensor.canary_vision_001_confidence`
- `sensor.canary_vision_001_voxel`
- `sensor.canary_vision_001_last_event`
- `sensor.canary_vision_001_uptime`

## Detection Settings

| Setting | Value | Description |
|---------|-------|-------------|
| Score Minimum | 70% | Detection confidence threshold |
| Lost Timeout | 1.5s | Time before presence clears |
| Dwell Start | 10s | Time to confirm dwelling |
| Voxel Grid | 3x3 | Spatial resolution |

These values live in the project's own `include/canary/config.h`
(`../../../projects/canary-vision/include/canary/config.h`; the topic
patterns in `include/canary/topics.h`), which owns every setting the
firmware reads. The score, lost timeout and dwell start are first-boot
values: the live ones are kept in NVS and tuned from Home Assistant. The
voxel grid is compiled in. `config.h` here owns only the `FEATURE_*` flags,
the watchdog timeout and one seed (sweep F182): `CONFIG_DEVICE_TYPE`, a copy
of the project's device type that `canary-local/tools/figures/gen_figures.mjs`
reads from this file, which `scripts/tests/test_vision_flavor_config_readers.py`
holds equal to its owner.

## Usage

1. Copy `secrets/secrets.example.h` to `secrets/secrets.h`
2. Configure WiFi and MQTT credentials
3. Build and flash:

```bash
cd firmware/projects/canary-vision
pio run -t upload
```
