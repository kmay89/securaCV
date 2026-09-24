# Operator runbook — Home Assistant + Frigate + MQTT + SecuraCV (local Compose)

> **What this file is.** A tick-as-you-go runbook for ONE bring-up of the
> stack in this directory. Copy it (or tick a printed copy) per run; never
> commit the ticks. It is not a plan of unfinished work — every step below,
> in the order written, is runnable today against `docker-compose.yml` as
> committed.
>
> **Where the expected outputs come from.** They are *derived* from
> [`docker-compose.yml`](docker-compose.yml), [`README.md`](README.md) and
> [`verify_pipeline.sh`](verify_pipeline.sh) as of 2026-09-22 — not from a
> recorded live run. Container names, service names, the three
> verification steps and their `✅` / `All verification steps passed.` lines
> are what those files produce. Docker Compose's own progress output is
> shown as abbreviated samples ("lines like"), because its headers, counts
> and ordering vary by Compose version and by what already exists;
> third-party log lines (Mosquitto, Frigate, Home Assistant) are described,
> not quoted, because nothing here pins them.
> **If a step's real output differs from what is written, the runbook is
> wrong: fix it here in the same change that fixes the stack.** When someone
> records a full live run, replace this note with the date of that run.
>
> Quickstart and security notes: [`README.md`](README.md). Longer
> walkthrough with the Home Assistant screens:
> [`docs/integrations/home-assistant-frigate-mqtt.md`](../../docs/integrations/home-assistant-frigate-mqtt.md).

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
  - [ ] Ensure ports 1883 (MQTT), 5000 (Frigate UI), and 8123 (Home Assistant)
        are free on `127.0.0.1` — `docker-compose.yml` binds all three to
        localhost only.
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
    - Expected output includes (the order depends on your locale):
      ```
      docker-compose.yml
      frigate.yml
      mosquitto.conf
      verify_pipeline.sh
      ```
  - [ ] Give Frigate a real RTSP source. `frigate.yml` ships with one
        placeholder camera (`demo`, `rtsp://127.0.0.1:8554/demo`, which
        nothing serves); Frigate starts it, logs ffmpeg errors for it and
        never publishes a detection until you replace the path, so
        verification step 1 below cannot pass without a live camera. Edit
        the `cameras:` block per the README's step 2.

- [ ] **Bring-up steps (Docker Compose)**
  - [ ] Choose the broker password, write it to `.env`, then create the
        broker's password file with the same password. The order matters:
        Compose interpolates EVERY service when it loads
        `docker-compose.yml`, even for a one-off `run` of `mosquitto`, and
        the `securacv` and `frigate` services require
        `SECURACV_MQTT_PASSWORD` (`${SECURACV_MQTT_PASSWORD:?...}`), so any
        `docker compose run` or `up` before `.env` holds it stops with
        `required variable SECURACV_MQTT_PASSWORD is missing a value`.
        The password file comes next, BEFORE the first `up`:
        `mosquitto.conf` disables anonymous access, and Mosquitto exits if
        its configured `password_file` is missing.
    - Command (or `cp .env.example .env` and set the same variable there;
      `mosquitto_passwd` prompts twice — type the password you wrote to
      `.env`):
      ```bash
      cd integrations/ha_frigate_mqtt
      echo 'SECURACV_MQTT_PASSWORD=<the password>' >> .env
      docker compose run --rm --no-deps --entrypoint sh mosquitto -c \
        'mosquitto_passwd -c /mosquitto/config/passwd securacv &&
         chown mosquitto:mosquitto /mosquitto/config/passwd &&
         chmod 600 /mosquitto/config/passwd'
      ```
    - Expected output: on a first run, Compose's own progress lines come
      first (pulling `eclipse-mosquitto:2` if it is not local yet, and
      creating the project network and volumes), then the
      `mosquitto_passwd` prompts (`Password:` / `Reenter password:`) and
      no error; `.env` now holds the `SECURACV_MQTT_PASSWORD=` line.
  - [ ] Start the stack. `--build` because the `securacv` service is built
        from this repo (`docker/sidecar/Dockerfile`), not pulled.
    - Command:
      ```bash
      docker compose up -d --build
      ```
    - Expected output, abbreviated: one `Started` line per container,
      named by `container_name` in `docker-compose.yml`, in an order Compose
      chooses. Around them Compose prints a progress header, pull lines for
      images not yet local, `Created` lines for any volume that does not
      exist yet and, on the first run, the sidecar image's build log
      first. Lines like:
      ```
      ✔ Container ha-mosquitto  Started
      ✔ Container ha-core       Started
      ✔ Container ha-frigate    Started
      ✔ Container ha-securacv   Started
      ```
  - [ ] Verify all four services are running.
    - Command:
      ```bash
      docker compose ps
      ```
    - Expected output: one row per service with a STATUS of `Up <how
      long>` (an image that declares a health check adds a suffix such as
      `(health: starting)` or `(healthy)`); the IMAGE, COMMAND, CREATED and
      PORTS columns are left out here. Lines like:
      ```
      NAME           SERVICE         STATUS
      ha-core        homeassistant   Up 20 seconds
      ha-frigate     frigate         Up 20 seconds
      ha-mosquitto   mosquitto       Up 21 seconds
      ha-securacv    securacv        Up 19 seconds
      ```
      A row reading `Restarting` or `Exited`, or a service missing from
      the list, means that container is not up; start with its logs below.
  - [ ] Tail the logs to confirm startup completion.
    - Command:
      ```bash
      docker compose logs -f --tail=50
      ```
    - What to look for (wording is each project's own and not pinned here):
      Mosquitto reports its version and an open listener on 1883; Frigate
      reports it connected to MQTT; Home Assistant reports it finished
      starting; `ha-securacv` shows its entrypoint starting `witness_api`,
      `frigate_bridge` and `event_mqtt_bridge`. An auth error from
      `ha-securacv` or `ha-frigate` at this point means `.env` and the
      password file disagree — redo the previous two steps.

- [ ] **Home Assistant GUI steps**
  - [ ] Open Home Assistant in the browser.
    - URL: `http://localhost:8123`
    - You should see the Home Assistant onboarding screen (first run) or
      the dashboard.
  - [ ] Add the MQTT integration, with the broker credentials from the
        bring-up step (anonymous connections are refused).
    - Steps:
      1. Settings → Devices & Services → Add Integration.
      2. Search for **MQTT** and select it.
      3. Broker: `mosquitto` (Home Assistant runs inside the Compose
         network, so the service name resolves). Port: `1883`.
      4. Username `securacv`, password: the one you chose above.
    - You should see the MQTT integration added with a green success
      message.
  - [ ] Confirm events reach Home Assistant. This is the ONE check that
        `verify_pipeline.sh` does not make — it never talks to Home
        Assistant — so it is done here, by eye.
    - Steps (the MQTT listen tool lives on the MQTT integration's own
      settings page, not in Developer Tools):
      1. Settings → Devices & services → MQTT → **Configure** →
         *Listen to a topic*: `frigate/events`, then Start Listening;
         trigger a detection on the camera.
      2. Repeat for `homeassistant/#` (retained discovery payloads from
         `event_mqtt_bridge`) and `witness/#` (its state topics).
    - You should see JSON payloads arrive on all three within a few
      seconds of a detection.
  - [ ] Add the Frigate integration (optional; it is a HACS custom
        integration, so only if this environment has HACS).
    - Steps:
      1. Settings → Devices & Services → Add Integration.
      2. Search for **Frigate** and select it.
      3. Frigate URL: `http://frigate:5000` (Compose network) or
         `http://localhost:5000` (host).
    - You should see Frigate devices/entities added.

- [ ] **Verification steps (run `verify_pipeline.sh`)**
  - [ ] Run the script from this directory, with the broker credentials
        (it uses `mosquitto_sub` / `mosquitto_pub` inside the broker
        container, which are refused without them).
    - Command:
      ```bash
      MQTT_USER=securacv MQTT_PASS=<the password> ./verify_pipeline.sh
      ```
    - Expected output: the script's three steps, each announced with
      `==>` and confirmed with `✅`, then its closing line and exit code 0.
      Step 1 waits up to 15 s for a LIVE `frigate/events` publish (retained
      messages are excluded), so a detection has to happen while it waits;
      step 2 publishes one nonce-tagged synthetic event and prints the
      `Event logged` line `frigate_bridge` writes for it; step 3 checks that
      `witness.db` (or its WAL) was written during this run.
      ```
      ==> Confirm MQTT publishes Frigate events (live, retained excluded)
      {"before": ..., "after": ..., "type": "..."}
      ✅ Confirm MQTT publishes Frigate events (live, retained excluded)

      ==> Confirm frigate_bridge ingests a nonce-tagged event right now
      ... Event logged ... verifysmoke<timestamp><pid> ...
      ✅ Confirm frigate_bridge ingests a nonce-tagged event right now

      ==> Confirm the sealed-log database was written during this run
      ✅ Confirm the sealed-log database was written during this run

      All verification steps passed.
      ```
    - If any step prints `❌` the script ends with
      `Verification failed: N step(s) did not pass.` and exit code 1, and
      points at the deterministic CI gate (`cargo test --test
      frigate_mqtt_e2e` and `ci_smoke.sh`) that needs no live stack.
    - What the script deliberately does NOT do (see its header comment):
      read `witness.db` directly (it is SQLCipher-encrypted), expect vault
      envelopes, build an export bundle, or check Home Assistant — the GUI
      step above covers the last one.

- [ ] **Troubleshooting**
  - [ ] One command first: the sidecar's built-in doctor checks broker
        reachability, auth, live Frigate traffic and sealed-log
        verification.
    - Command:
      ```bash
      docker compose run --rm securacv doctor
      ```
  - [ ] MQTT broker not reachable.
    - Command:
      ```bash
      docker compose logs --tail=200 mosquitto
      ```
    - Look for: the version line and an open listener on port 1883. A
      complaint about a missing password file means the bring-up's first
      step was skipped; a line saying a client was not authorized means
      that client sent the wrong credentials.
  - [ ] Frigate not publishing events.
    - Command:
      ```bash
      docker compose logs --tail=200 frigate
      ```
    - Look for: an MQTT connected line, and no camera/ffmpeg errors for the
      RTSP source. A connection refused or auth failure here means
      `FRIGATE_MQTT_PASSWORD` (from `.env`) does not match the password
      file.
  - [ ] SecuraCV sidecar not ingesting (verification step 2 or 3 fails).
    - Command:
      ```bash
      docker compose logs --tail=200 securacv
      ```
    - Look for: `Event logged` lines as detections arrive (the line
      `verify_pipeline.sh` greps for); an MQTT auth error means
      `MQTT_PASSWORD` (from `.env`) does not match the password file.
  - [ ] Home Assistant not receiving MQTT events.
    - Command:
      ```bash
      docker compose logs --tail=200 homeassistant
      ```
    - Look for: the MQTT integration reporting a connection to
      `mosquitto`; an auth failure means the credentials typed in the
      integration dialog differ from the password file.
  - [ ] Inspect topics manually from the host (credentials required).
    - Command:
      ```bash
      docker compose exec mosquitto mosquitto_sub -u securacv -P <the password> -t 'frigate/#' -v
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
    - Expected output, abbreviated (after a progress header, in an order
      Compose chooses). Lines like:
      ```
      ✔ Container ha-securacv            Removed
      ✔ Container ha-frigate             Removed
      ✔ Container ha-core                Removed
      ✔ Container ha-mosquitto           Removed
      ✔ Network ha_frigate_mqtt_default  Removed
      ```
  - [ ] Remove the volumes for a clean reset. This deletes the broker
        password file (`mosquitto_config`), Home Assistant's config, and the
        sidecar's data volume — including the auto-generated device key
        seed and the sealed log. Back up `securacv_data` first if you want
        to keep the chain.
    - Command:
      ```bash
      docker compose down -v
      ```
    - Expected output, abbreviated: the container and network lines of
      the previous step if anything was still up, and one `Removed` line
      for each of the five named volumes from `docker-compose.yml`. Lines
      like:
      ```
      ✔ Volume ha_frigate_mqtt_mosquitto_data        Removed
      ✔ Volume ha_frigate_mqtt_mosquitto_config      Removed
      ✔ Volume ha_frigate_mqtt_frigate_media         Removed
      ✔ Volume ha_frigate_mqtt_homeassistant_config  Removed
      ✔ Volume ha_frigate_mqtt_securacv_data         Removed
      ```
