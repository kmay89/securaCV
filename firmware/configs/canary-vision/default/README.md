# Canary Vision - Default Configuration

Standard configuration for the Vision AI presence detection device.

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
firmware reads; `config.h` here owns only the `FEATURE_*` flags and the
watchdog timeout (sweep F182).
Its five `CONFIG_*` lines are seeds — copies of the project's constants that
two tools read from this file: `CONFIG_DEVICE_TYPE` for
`canary-local/tools/figures/gen_figures.mjs` and the four detection defaults
for `canary-local/tools/gen_flash.py` — and
`scripts/tests/test_vision_flavor_config_readers.py` holds each equal to its
owner.

## Usage

1. Copy `secrets/secrets.example.h` to `secrets/secrets.h`
2. Configure WiFi and MQTT credentials
3. Build and flash:

```bash
cd firmware/projects/canary-vision
pio run -t upload
```
