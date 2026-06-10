# Home Assistant + Frigate + MQTT + SecuraCV (Local Compose)

This folder provides a local Docker Compose setup for running **Home Assistant**, **Frigate**, **Mosquitto**, and **SecuraCV** together with MQTT event flow:

```
Frigate → MQTT (frigate/events) → frigate_bridge → SecuraCV log → event_mqtt_bridge → MQTT → Home Assistant
```

The SecuraCV container is the [sidecar image](../../docker/sidecar/)
(`docker/sidecar/Dockerfile`), which supervises three binaries from this
repo via its entrypoint:

- `witness_api` (Event API only, no RTSP ingestion)
- `frigate_bridge` (Frigate MQTT → privacy-preserving events)
- `event_mqtt_bridge` (SecuraCV events → MQTT discovery/state)

> Just want SecuraCV next to an **existing** Frigate? Skip this full stack
> and use `docker/sidecar/quickstart.compose.yml` instead — one service,
> one env var.

## Quickstart

1) **Set up MQTT credentials** (anonymous access is disabled by default):

```bash
# Create the password file BEFORE starting the broker (mosquitto exits if the
# configured password_file is missing, so it can't be created via `exec` on a
# running broker). You'll be prompted for the password.
docker compose run --rm --no-deps --entrypoint sh mosquitto -c \
  'mosquitto_passwd -c /mosquitto/config/passwd securacv &&
   chown mosquitto:mosquitto /mosquitto/config/passwd &&
   chmod 600 /mosquitto/config/passwd'
docker compose up -d mosquitto
# Tell the frigate and securacv services the password you chose:
echo 'SECURACV_MQTT_PASSWORD=<the password>' >> .env
```

The `.env` value is injected into the SecuraCV bridges
(`MQTT_USERNAME`/`MQTT_PASSWORD`) and into Frigate
(`FRIGATE_MQTT_USER`/`FRIGATE_MQTT_PASSWORD` placeholders in `frigate.yml`).
When you configure Home Assistant's MQTT integration, give it the same
credentials.

A device key seed is generated automatically on first start (persisted in
the `securacv_data` volume — back it up). To pin one instead, see the
commented `secrets` block in `docker-compose.yml`.

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

Diagnose the pipeline at any time (broker reachability, auth, live Frigate
traffic, sealed-log verification):

```bash
docker compose run --rm securacv doctor
```

## Security Notes

- **MQTT authentication** is required. Anonymous access is disabled in `mosquitto.conf`.
- **Ports are bound to localhost** (`127.0.0.1`) by default to prevent network exposure.
- **DEVICE_KEY_SEED** is auto-generated into the data volume (0600), or supplied via a Docker secret (`/run/secrets/`) — never required in the environment.
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

## Verifying the pipeline

- **Automated (no Docker needed):** the SecuraCV-owned path is gated in CI.
  - `cargo test --test frigate_mqtt_e2e` — a `frigate/events` payload → sealed log →
    real `log_verify` against the SQLCipher-encrypted DB.
  - `./ci_smoke.sh` — the real `frigate_bridge` binary ingesting a `frigate/events`
    message from a live mosquitto broker (run `BRIDGE_BIN=path/to/frigate_bridge ./ci_smoke.sh`).
- **Manual operator smoke check:** `./verify_pipeline.sh` against the live 4-container
  stack above. It confirms Frigate is publishing and `frigate_bridge` is ingesting; it
  does not read the encrypted `witness.db` directly, expect vault envelopes, or build a
  break-glass export bundle (none of which this bridge produces). Set `MQTT_USER`/`MQTT_PASS`
  if your broker requires auth.

## Notes

- This setup keeps MQTT as the shared event bus across Frigate, SecuraCV, and Home Assistant.
- The Frigate config disables recordings and snapshots to avoid storing raw media.
