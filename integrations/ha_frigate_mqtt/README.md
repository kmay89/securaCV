# Home Assistant + Frigate + MQTT + SecuraCV (Local Compose)

This folder provides a local Docker Compose setup for running **Home Assistant**, **Frigate**, **Mosquitto**, and **SecuraCV** together with MQTT event flow:

```
Frigate → MQTT (frigate/events) → frigate_bridge → SecuraCV log → event_mqtt_bridge → MQTT → Home Assistant
```

The SecuraCV container runs three binaries from this repo:

- `witness_api` (Event API only, no RTSP ingestion)
- `frigate_bridge` (Frigate MQTT → privacy-preserving events)
- `event_mqtt_bridge` (SecuraCV events → MQTT discovery/state)

## Quickstart

1) **Set a device key seed** (required by the kernel):

```bash
export DEVICE_KEY_SEED=$(openssl rand -hex 32)
```

2) **Review the demo camera** in `frigate.yml` and replace the RTSP URL:

```yaml
cameras:
  demo:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/demo
```

Use a real RTSP source (camera, go2rtc, etc.) so Frigate can emit detections.

3) **Start the stack**:

```bash
docker compose up -d --build
```

4) **Open the UIs**:

- Home Assistant: http://localhost:8123
- Frigate: http://localhost:5000

## What to expect

- Frigate publishes detections to `frigate/events` on Mosquitto.
- `frigate_bridge` consumes those events, strips identifiers, and writes to the SecuraCV log.
- `event_mqtt_bridge` publishes Home Assistant discovery + state topics under:
  - `homeassistant/...` (discovery)
  - `witness/...` (state)

## Optional MQTT checks

From another terminal (requires `mosquitto_sub` installed locally):

```bash
mosquitto_sub -h localhost -t 'frigate/events' -v
mosquitto_sub -h localhost -t 'homeassistant/#' -v
mosquitto_sub -h localhost -t 'witness/#' -v
```

## Notes

- This setup keeps MQTT as the shared event bus across Frigate, SecuraCV, and Home Assistant.
- The Frigate config disables recordings and snapshots to avoid storing raw media.
- If you need MQTT authentication, edit `mosquitto.conf` and add credentials to the SecuraCV and Home Assistant MQTT configs.
