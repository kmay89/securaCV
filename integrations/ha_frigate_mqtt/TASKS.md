# HA Frigate MQTT Integration Tasks

- [ ] **Prerequisites**
  - [ ] Install Docker Engine and Docker Compose v2.
    - Command:
      ```bash
      docker --version
      docker compose version
      ```
    - Expected output (examples):
      ```
      Docker version 24.x.x, build ...
      Docker Compose version v2.x.x
      ```
  - [ ] Ensure ports 1883 (MQTT), 5000 (Frigate UI), and 8123 (Home Assistant) are available.
    - Command:
      ```bash
      lsof -iTCP:1883 -sTCP:LISTEN || true
      lsof -iTCP:5000 -sTCP:LISTEN || true
      lsof -iTCP:8123 -sTCP:LISTEN || true
      ```
    - Expected output:
      ```
      # no output means the port is free
      ```
  - [ ] Confirm the integration directory exists.
    - Command:
      ```bash
      ls -1 integrations/ha_frigate_mqtt
      ```
    - Expected output includes:
      ```
      docker-compose.yml
      frigate.yml
      mosquitto.conf
      verify_pipeline.sh
      ```

- [ ] **Bring-up steps (Docker Compose)**
  - [ ] Start the stack from the integration directory.
    - Command:
      ```bash
      cd integrations/ha_frigate_mqtt
      docker compose up -d
      ```
    - Expected output:
      ```
      [+] Running 3/3
      ✔ Container ha_frigate_mqtt-mosquitto-1  Started
      ✔ Container ha_frigate_mqtt-frigate-1    Started
      ✔ Container ha_frigate_mqtt-homeassistant-1 Started
      ```
  - [ ] Verify containers are healthy/running.
    - Command:
      ```bash
      docker compose ps
      ```
    - Expected output contains running services:
      ```
      mosquitto
      frigate
      homeassistant
      ```
  - [ ] Tail the logs to confirm startup completion.
    - Command:
      ```bash
      docker compose logs -f --tail=50
      ```
    - Expected output checkpoints:
      ```
      mosquitto | mosquitto version ... running
      frigate   | Starting Frigate...
      homeassistant | Home Assistant initialized
      ```

- [ ] **Home Assistant GUI steps**
  - [ ] Open Home Assistant in the browser.
    - URL: `http://localhost:8123`
    - You should see the Home Assistant onboarding screen or dashboard.
  - [ ] Add the MQTT integration.
    - Steps:
      1. Settings → Devices & Services → Add Integration.
      2. Search for **MQTT** and select it.
      3. Set broker to `mosquitto` (if inside the Docker network) or `localhost` (if accessing from host).
      4. Port: `1883`.
    - You should see the MQTT integration added with a green success message.
  - [ ] Add the Frigate integration (if available in this environment).
    - Steps:
      1. Settings → Devices & Services → Add Integration.
      2. Search for **Frigate** and select it.
      3. Set Frigate URL to `http://frigate:5000` (Docker network) or `http://localhost:5000` (host).
    - You should see Frigate devices/entities added.

- [ ] **Verification steps (run `verify_pipeline.sh`)**
  - [ ] Run the verification script.
    - Command:
      ```bash
      ./verify_pipeline.sh
      ```
    - Expected output includes topic checks and success:
      ```
      OK: MQTT broker reachable
      OK: Frigate publishing to topic frigate/events
      OK: Home Assistant received events
      ```
    - Topic names to confirm during verification:
      - `frigate/events`
      - `frigate/<camera_name>/events`
      - `frigate/<camera_name>/snapshot`

- [ ] **Troubleshooting**
  - [ ] MQTT broker not reachable.
    - Command:
      ```bash
      docker compose logs --tail=200 mosquitto
      ```
    - You should see:
      ```
      mosquitto version ... running
      Opening ipv4 listen socket on port 1883
      ```
  - [ ] Frigate not publishing events.
    - Command:
      ```bash
      docker compose logs --tail=200 frigate
      ```
    - You should see:
      ```
      MQTT connected
      Publishing frigate/events
      ```
  - [ ] Home Assistant not receiving MQTT events.
    - Command:
      ```bash
      docker compose logs --tail=200 homeassistant
      ```
    - You should see:
      ```
      MQTT connection established
      ```
  - [ ] Inspect topics manually from the host.
    - Command:
      ```bash
      docker compose exec mosquitto mosquitto_sub -t 'frigate/#' -v
      ```
    - You should see messages like:
      ```
      frigate/events {"type":"new", ...}
      ```

- [ ] **Rollback/cleanup steps**
  - [ ] Stop and remove containers.
    - Command:
      ```bash
      docker compose down
      ```
    - Expected output:
      ```
      ✔ Network ha_frigate_mqtt_default  Removed
      ✔ Container ha_frigate_mqtt-...    Removed
      ```
  - [ ] Remove any local volumes if you want a clean reset.
    - Command:
      ```bash
      docker compose down -v
      ```
    - Expected output:
      ```
      ✔ Volume ha_frigate_mqtt_...  Removed
      ```
