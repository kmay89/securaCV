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

1) **Create a device key seed** (stored as a Docker secret, never in environment):

```bash
openssl rand -hex 32 > .device_key_seed
chmod 600 .device_key_seed
```

2) **Set up MQTT credentials** (anonymous access is disabled by default):

```bash
# Start mosquitto first to create the password file
docker compose up -d mosquitto
docker compose exec mosquitto mosquitto_passwd -c /mosquitto/config/passwd securacv
docker compose restart mosquitto
```

3) **Review the demo camera** in `frigate.yml` and replace the RTSP URL:

```yaml
cameras:
  demo:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/demo
```

Use a real RTSP source (camera, go2rtc, etc.) so Frigate can emit detections.

4) **Start the stack**:

```bash
docker compose up -d --build
```

5) **Open the UIs**:

- Home Assistant: http://localhost:8123
- Frigate: http://localhost:5000

## Security Notes

- **MQTT authentication** is required. Anonymous access is disabled in `mosquitto.conf`.
- **Ports are bound to localhost** (`127.0.0.1`) by default to prevent network exposure.
- **DEVICE_KEY_SEED** is loaded via Docker secrets (`/run/secrets/`), not environment variables.
- For production, enable TLS on the MQTT broker (see comments in `mosquitto.conf`).

## What to expect

- Frigate publishes detections to `frigate/events` on Mosquitto.
- `frigate_bridge` consumes those events, strips identifiers, and writes to the SecuraCV log.
- `event_mqtt_bridge` publishes Home Assistant discovery + state topics under:
  - `homeassistant/...` (discovery)
  - `witness/...` (state)

## Optional MQTT checks

From another terminal (requires `mosquitto_sub` installed locally):

```bash
mosquitto_sub -h localhost -u securacv -P <password> -t 'frigate/events' -v
mosquitto_sub -h localhost -u securacv -P <password> -t 'homeassistant/#' -v
mosquitto_sub -h localhost -u securacv -P <password> -t 'witness/#' -v
```

## Notes

- This setup keeps MQTT as the shared event bus across Frigate, SecuraCV, and Home Assistant.
- The Frigate config disables recordings and snapshots to avoid storing raw media.
