# Home Assistant Integration Guide

Install the SecuraCV Home Assistant integration via HACS, then connect it to your SecuraCV Canary devices via MQTT or the Privacy Witness Kernel via HTTP API.

> **Prefer the guided version?** [`canary-local/homeassistant.html`](../canary-local/homeassistant.html)
> ("The Hub") walks this same setup interactively — a 3D Raspberry Pi build you
> can scrub apart, a bench terminal that replays every command below, and a
> working sketch of the dashboard you end up with. Its entity names, topics,
> and versions are generated from this doc and drift-checked in CI, so the two
> can't disagree.

## What you need

| Item | Notes |
|------|-------|
| Raspberry Pi 4 (4 GB+) or x86 PC | Pi 5 works great; ~3 cameras at 10 fps |
| Home Assistant OS | Installed from the official image |
| IP camera(s) with RTSP | Hikvision, Dahua, Reolink, Amcrest, Ubiquiti, etc. |
| HA Companion App (optional) | For push notifications to your phone |

## Two witness logs, two trust roots

> The kernel's sealed log covers the Frigate
> pipeline. Canary devices are independent witnesses: each keeps its **own**
> Ed25519-signed hash chain on-device and publishes signed events over MQTT, which the
> Home Assistant integration verifies against that device's pinned key. Canary events are
> *not* re-sealed into the kernel's log — a "fleet" today is N independently-signed
> canaries converging in your dashboard, each verifiable on its own.

## Quick Start: Canary via MQTT (Recommended)

Most users should use this path. Canary devices auto-discover in Home Assistant via MQTT Discovery.

### Prerequisites

1. **MQTT broker** (Mosquitto recommended) running and configured in HA
2. **SecuraCV Canary** device powered on with firmware v2.1.0+
3. **Home WiFi** credentials entered into the Canary via its web dashboard

### Step 1: Install the Integration

SecuraCV is distributed as a HACS **custom repository** (it is not in the
default HACS store yet):

1. Open HACS in Home Assistant
2. Click **⋮ → Custom repositories**
3. Add `https://github.com/kmay89/securacv-homeassistant` (the integration's
   distribution repository) with type **Integration**
4. Search HACS for "SecuraCV" and install it
5. Restart Home Assistant

### Step 2: Configure the Integration

1. Go to **Settings > Devices & Services > Add Integration**
2. Search for "SecuraCV"
3. Select **"Canary devices via MQTT (Recommended)"**
4. Set the MQTT topic prefix (default: `securacv`) — this must match your Canary firmware config
5. Click Submit

### Step 3: Configure the Canary Device

Connect to your Canary's WiFi AP (SSID shown on device, password is device-unique):

1. On first boot, joining the AP from a phone pops the Canary's **setup
   wizard** on its own (the captive-portal sheet) — pick your home WiFi
   there and enter its password. The wizard's finish screen also offers an
   optional **"point it at your hub"** step, prefilled with the values that
   are right for a SecuraCV hub (`homeassistant.local`, port `1883`) — fill
   in username/password only if your broker requires them, save, and let it
   restart.
2. If the wizard doesn't appear (or the Canary was set up before), open
   `http://canary-<id>.local` (the device's unique hostname, shown in the
   boot banner — e.g. `http://canary-s3-ab7k.local`) or `http://192.168.4.1`
   in your browser. If you only have one Canary, plain `http://canary.local`
   may also resolve, but each Canary always advertises its unique hostname
   so multiple devices on the same network don't collide. `/setup` on any of
   those addresses reopens the wizard.
3. In the dashboard, go to the **Network** tab and enter your home WiFi
   credentials (the Canary needs WiFi to reach the MQTT broker)
4. MQTT broker details (if you skipped the wizard's hub step):
   - **Host**: Your Mosquitto broker IP (e.g., `192.168.1.10` or `homeassistant.local`)
   - **Port**: `1883` (default)
   - **Username/Password**: If your broker requires authentication
5. Save and reboot the Canary

### Step 4: Verify Discovery

Within 30 seconds of the Canary connecting to MQTT:

1. Go to **Settings > Devices & Services > SecuraCV**
2. You should see your Canary device listed with sensors:
   - **Witness Count** — Total witness records created
   - **Chain Sequence** — Current chain sequence number
   - **Uptime** — Device uptime
   - **Free Heap** — Available memory
   - **GPS Satellites** — Satellite count
   - **Online** — Device connectivity (via MQTT LWT)
   - **Chain Valid** — Witness chain integrity
   - **Tamper Detected** — Tamper event status
   - **GPS Fix** — GPS fix status
   - **SD Card Healthy** — Storage health
   - **Die Temperature** — Chip die temperature, whole degrees (silicon, not room)
   - **Thermal Performance** — `normal` / `throttled` / `paused` (the adaptive-performance ladder)
   - **Thermal Advisory** — ON when any thermal advisory is active (too hot, saturation, sensor fault, cold)

On canary-wap builds with the microphone enabled (XIAO ESP32-S3 Sense), the
device also announces acoustic entities via MQTT discovery:

   - **Smoke Alarm Heard** / **CO Alarm Heard** — binary sensors that latch ON
     when the mic matches an NFPA 72 T3 smoke or UL 2034 CO cadence, and clear
     about 30 seconds after the alarm stops. State rides the retained
     `securacv/<device_id>/sensing` topic (`acoustic_event` field plus
     detection counters and `mic_muted`).
   - **Knock / Doorbell / Glass Break Detected** — additional binary sensors on
     FULL builds with the transient detectors compiled in.
   - **Microphone Mute** — a switch entity for the hard mute. State is retained
     on `securacv/<device_id>/mic/state` (`muted`/`live`); commands go to
     `securacv/<device_id>/mic/cmd` (`mute`/`unmute`, or HA's default `ON` =
     muted / `OFF` = live). Every toggle — including ones from HA — is signed
     into the device's witness chain with its source, so an investigator can
     later verify when the mic was off and who turned it off.

### Step 4b: Add the verified-✓ timeline card

For the "single pane of glass" view, add the bundled Lovelace card: edit a
dashboard → **Add Card** → search **"SecuraCV Verified Timeline"**. It renders a
verified-✓ event timeline with a hash-chain status header from the entities
above, and auto-discovers them. Full options and a no-custom-card YAML fallback:
[`docs/lovelace_timeline.md`](lovelace_timeline.md).

### Step 5: Set Up Notifications

Import the SecuraCV Alert Blueprint for one-click notification setup:

1. Go to **Settings > Automations > Blueprints > Import Blueprint**
2. Enter URL: `https://github.com/kmay89/securaCV/blob/main/docs/blueprints/securacv_alerts.yaml`
3. Create an automation from the blueprint
4. Pick the sensors you want monitored (each alert type is optional — leave
   an input empty to skip it): smoke/CO alarm heard (critical pushes that
   bypass silent mode), tamper, chain failure, offline, GPS loss
5. Enter your notification service (e.g. `notify.mobile_app_your_phone`)

Or copy automations from `docs/homeassistant_automations.yaml` for manual setup.

**Have a Philips Hue (or any color) bulb?** Import the Alert Light blueprint
the same way — URL:
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/securacv_hue_alert_light.yaml`
— pick your Canary's sensors and a bulb, and the light becomes a silent
beacon: it pulses and holds red on tamper / smoke or CO heard / chain
failure, blinks green and restores itself when the sensors clear, and gives
three gentle amber pulses if the device goes offline. Pair the bulb first
via HA's built-in Hue integration (**Settings > Devices & Services > Add
Integration > Philips Hue**, press the bridge button).

**Radar (canary-sense / MR60BHA2) witnesses** get three extra blueprints —
after-hours presence (with optional two-physics corroboration),
lights-out-with-presence tamper, and a non-diagnostic welfare check — plus a
stock-card **wellbeing tile**. See
[`docs/blueprints/canary_sense_wellbeing.md`](blueprints/canary_sense_wellbeing.md).

### Step 6: Verify per-device PKI (optional but recommended)

Each Canary signs its `chain`, `events`, and `counts` MQTT publishes
with an on-device Ed25519 key. HA auto-pins the public key the first
time a device's health publish appears (TOFU), then verifies every
subsequent publish.

To check that verification is live: open the chain-length sensor's
attributes — you should see `verified: true`, `trust_reason: ok`, and
matching `pinned_fingerprint` / `received_fingerprint` values. If your
threat model needs stricter trust than TOFU, pin the device's pubkey
manually from **Settings → Devices & services → SecuraCV → Configure →
Pin a device pubkey** (the fingerprint + pubkey hex are on each
device's `/enroll` page, e.g. `http://canary-<fp>.local/enroll`).

Full background, threat model, and rotation procedure: see
[`docs/device_trust.md`](device_trust.md).

### Troubleshooting: MQTT Setup

| Symptom | Check |
|---------|-------|
| Device not appearing in HA | Verify Canary is connected to home WiFi (check serial output or WAP dashboard) |
| MQTT not connecting | Verify broker host/port in Canary config matches your Mosquitto setup |
| Sensors show "unavailable" | Check MQTT broker logs for connection attempts from `securacv-canary-*` client IDs |
| Discovery not working | Ensure HA MQTT integration is configured and `securacv/#` topics are not blocked |
| Tamper alerts not firing | Verify the `tamper` binary sensor entity exists and automations are enabled |
| Two "SecuraCV Canary" devices / entities ending in `_2` | Known overlap: the firmware announces core entities via native MQTT discovery *and* the integration creates its own (with PKI verification attributes). Both work; disable the ones you don't use from the entity settings, and prefer the integration's set if you use signature verification |

### MQTT Topic Reference

| Topic | Direction | Content |
|-------|-----------|---------|
| `securacv/{device_id}/status` | Device → HA | Device state, GPS, chain sequence (every 30s) |
| `securacv/{device_id}/health` | Device → HA | System metrics (every 60s) |
| `securacv/{device_id}/events` | Device → HA | Witness record events |
| `securacv/{device_id}/chain` | Device → HA | Hash chain state |
| `securacv/{device_id}/tamper` | Device → HA | Tamper alerts (immediate) |
| `securacv/{device_id}/availability` | Device → HA | Online/offline (LWT, retained) |
| `securacv/{device_id}/update/state` | Device → HA | Firmware update entity state (retained) |
| `securacv/{device_id}/update/cmd` | HA → Device | `install` — start a firmware update |
| `homeassistant/*/securacv_*/config` | Device → HA | HA MQTT Discovery config (retained) |

### Transport catalog

The integration models multi-transport resilience (`custom_components/securacv/const.py`).
Status reflects what the code actually does today — **Implemented** means the transport
carries witness data end-to-end, **Experimental** means partially wired (one side shipped,
not verified end-to-end), **Planned** means the vocabulary is reserved and nothing emits it
yet.

> **Per-transport health entities do not appear yet.** The integration's
> `SecuraCVCanaryTransportSensor` is only created when a device publishes
> `securacv/{device_id}/transport` — and no current firmware calls its
> `mqtt_publish_transport()` helper, so the *health sensors* below are pending a firmware
> publisher for every row. The Status column describes the transport itself (does witness
> data actually flow over it), not the health entity.

| Transport | Description | Transport status | Health sensor |
|-----------|-------------|------------------|---------------|
| `wifi_sta` | WiFi station to your router (normal operation; carries all MQTT traffic) | Implemented | Pending firmware publisher |
| `wifi_ap` | Direct WiFi AP mode (device dashboard / setup) | Implemented | Pending firmware publisher |
| `mqtt` | MQTT broker connection — the primary witness-data path | Implemented | Pending firmware publisher |
| `ble` | Bluetooth Low Energy (device-side scanning, chirp alerts) | Experimental | Pending firmware publisher |
| `mesh` | Opera mesh network, peer-to-peer ([spec v0](../spec/canary_mesh_network_v0.md)) | Experimental | Pending firmware publisher |
| `chirp` | Community alert network ([spec v0](../spec/chirp_channel_v0.md)) | Experimental | Pending firmware publisher |
| `lora` | LoRa radio (see [Meshtastic notes](meshtastic_integration.md)) | Planned | — |
| `audio` | SCQCS audio squawks | Planned | — |

### Per-tamper-type sensor catalog

Each tamper type below gets its own HA binary sensor (`binary_sensor.py`). Status is based
on whether current firmware actually emits the corresponding signal, not on the sensor
existing: the integration listens for several signals no firmware publishes yet.

| Tamper type | HA sensor | Firmware signal today | Status |
|-------------|-----------|----------------------|--------|
| `sd_remove` | SD Removed | Canary WAP publishes `sd_mounted` in health | Implemented |
| `sd_error` | SD Error | Canary publishes `sd_errors` in health | Implemented |
| `memory_critical` | Memory Critical | derived HA-side from published `free_heap` | Implemented |
| `enclosure` | Enclosure Open | capacitive-touch tamper published on the tamper topic (as `enclosure_tamper`) | Experimental |
| `power_loss` | Power Loss | canary base publishes `{"type":"power_loss"}` on the tamper topic at boot when the power-events classifier reports a restored outage or brownout (`canary_power_events.h`; the WAP and other trees are still pending their boot-path wiring) | Implemented (canary base) |
| `gps_jamming` | GPS Jamming | none found | Experimental |
| `motion` | Unexpected Motion | none found (accelerometer signal not published) | Experimental |
| `gpio` | GPIO Tamper | none found | Experimental |
| `watchdog` | Watchdog Timeout | none found | Experimental |
| `unexpected_reboot` | Unexpected Reboot | canary base publishes `{"type":"unexpected_reboot"}` on the tamper topic at boot after a fault reset (same power-events path as `power_loss`) | Implemented (canary base) |
| `battery_remove` | — (no sensor) | none | Planned |
| `gps_spoof` | — (no sensor) | none | Planned |
| `capacitive` | — (no sensor; folded into `enclosure` on-device) | touch pad tamper | Planned |
| `audio_anomaly` | — (no sensor) | none | Planned |

---

## Firmware Updates from Home Assistant

Canaries with the signed pull-OTA firmware expose a native **Firmware**
update entity (plus an **Auto Update** switch) via MQTT Discovery — no
extra setup needed.

- When a new release is published, the device's update entity shows
  "Update available" with the release notes. Press **Install**: the device
  downloads the update over HTTPS, verifies its SHA-256 and Ed25519
  release signature, installs it to the inactive partition, restarts, and
  confirms itself healthy. A live progress bar tracks the whole cycle.
- If the update fails for any reason, the device restores its previous
  firmware automatically and reports what happened — it cannot be bricked
  by a bad update, and a forged image can never pass the signature check.
- Turn on the **Auto Update** switch (per device) to install new releases
  hands-free within a day of publication. It's off by default — a witness
  device never restarts unattended unless you choose that.
- Every update is signed into the device's witness chain
  (`fw_update_started` / `fw_update_applied` / `fw_update_rolled_back`),
  so the audit trail proves when firmware changed.

See `docs/firmware_ota.md` for the release pipeline, local/air-gapped
hosting, and the full security model.

---

## Alternative Setup Modes

The integration supports three modes. The MQTT-only mode above is recommended for most users.

| Mode | Best For | How It Works |
|------|----------|--------------|
| **MQTT only** | Canary device users | Auto-discover devices via MQTT (recommended) |
| **Kernel only** | PWK API users | Poll the Witness Kernel HTTP API |
| **Both** | Advanced users | MQTT for Canary + HTTP for the kernel |

---

## Legacy: Witness Kernel Setup

### Choose Your Mode

| Mode | Best For | How It Works |
|------|----------|--------------|
| **frigate** | Users with Frigate NVR | Subscribe to Frigate's MQTT events |
| **standalone** | Users without Frigate | Process RTSP streams directly |

### Frigate Mode (Recommended if you use Frigate)

```
Cameras → Frigate (detection) → MQTT → PWK (privacy logging)
```

- Uses Frigate's superior ML detection (Coral TPU, TensorFlow)
- PWK receives event notifications, not video
- Best accuracy, minimal resource usage
- The app always starts the Event API service in Frigate mode, even if MQTT publishing is disabled

#### Frigate + MQTT Configuration Checklist

- [ ] **Mode set to frigate**: `mode: "frigate"` is configured in the app options.
- [ ] **Frigate MQTT broker details match**: `frigate.mqtt_host`, `frigate.mqtt_port`, `frigate.mqtt_username`, and `frigate.mqtt_password` match the broker that Frigate uses.
- [ ] **Frigate event topic is correct**: `frigate.mqtt_topic` matches Frigate’s configured event topic (default `frigate/events`).
- [ ] **Home Assistant MQTT publish settings are aligned**: if you enable `mqtt_publish.enabled`, ensure `mqtt_publish.host`, `mqtt_publish.port`, `mqtt_publish.username`, and `mqtt_publish.password` match the same broker.
- [ ] **Topic + discovery prefixes are consistent**: `mqtt_publish.topic_prefix` is the prefix you expect for PWK events, and `mqtt_publish.discovery_prefix` matches Home Assistant’s discovery prefix (default `homeassistant`).
- [ ] **Required app options from the Configuration tab are configured**: `device_key_seed` is set, `mode` is still `frigate`, and any Frigate-specific options (`frigate.cameras`, `frigate.labels`, `frigate.min_confidence`) are configured as needed.
- [ ] **MQTT transport expectations are understood**: the current bridges speak MQTT 3.1.1 over TCP with no TLS support.

**Follow-up task**: If you require TLS or MQTT v5, the bridge code must be modified to use a standard MQTT client library that supports these features. When making this change, ensure the bridge still avoids introducing new privacy metadata.

### Standalone Mode

```
Cameras → go2rtc → PWK (detection + logging)
```

- PWK processes camera streams directly
- Simpler setup without Frigate
- Built-in motion detection

---

## Quick Start (HACS + Kernel)

### Step 1: Install the HACS Integration

1. Open **HACS → Integrations**
2. Click **⋮ → Custom repositories**
3. Add `https://github.com/kmay89/securacv-homeassistant` (the integration's
   distribution repository) as an **Integration**
4. Install **SecuraCV** and restart Home Assistant when prompted

### Step 2: Install the Kernel

Choose one runtime option (you'll configure it in **frigate** or **standalone** mode in Step 4):

**Option A: Home Assistant app (custom repository)**
1. Go to **Settings → Apps → App Store**
2. Click **⋮ → Repositories** → Add: `https://github.com/kmay89/securaCV`
3. Install **Privacy Witness Kernel**

**Option B: Docker / another host**
1. Run the kernel using your preferred deployment method
2. Ensure the Event API is reachable from Home Assistant

### Step 3: Generate a Device Key

The device key establishes your kernel's cryptographic identity and is
required before the kernel will start:

```bash
openssl rand -hex 32
```

Save this key - it protects your event signatures.

### Step 4: Configure the Kernel

**For Frigate users:**
```yaml
mode: "frigate"
device_key_seed: "your-64-char-key"
frigate:
  mqtt_host: "core-mosquitto"
  min_confidence: 0.5
mqtt_publish:
  enabled: true  # Optional: HA MQTT discovery
```

**For standalone users:**
```yaml
mode: "standalone"
device_key_seed: "your-64-char-key"
go2rtc_discovery: true
mqtt_publish:
  enabled: true  # Optional: HA MQTT discovery
```

### Step 5: Start the Kernel

Click **Start** (or start your container). Check logs for any errors.

### Step 6: Add the Integration

1. Go to **Settings → Devices & Services**
2. Click **Add Integration** and select **SecuraCV**
3. Provide the Event API URL and authentication (MQTT is optional):
   - **API token file (recommended):** the kernel rotates its capability token
     every 10 minutes and rewrites the token file. When the kernel runs as the
     app, that file is `/config/api_token` (the default), which Home
     Assistant can read directly — the integration re-reads it automatically
     whenever the token rotates.
   - **Static token:** only for kernels on another host whose token file Home
     Assistant cannot read. A pasted token goes stale at the next rotation
     (≤10 minutes), so for remote kernels either share the token file (e.g. a
     mounted volume) or expect to re-authenticate.

---

## Distribution

- **HACS integration (current):** install the SecuraCV integration in Home Assistant.
- **Kernel runtime:** run the Privacy Witness Kernel as an app, container, or service. The integration connects to its Event API.

---

## Kernel Installation (Optional App)

### Option 1: App Repository

1. In Home Assistant, go to **Settings → Apps → App Store**
2. Click the menu (⋮) → **Repositories**
3. Add: `https://github.com/kmay89/securaCV`
4. Find "Privacy Witness Kernel" and click **Install**

### Option 2: Local Installation

```bash
# Clone the repository
git clone https://github.com/kmay89/securaCV.git
cd securaCV

# Build the app container (from the repo root)
docker build -f privacy_witness_kernel/Dockerfile -t privacy-witness-kernel .

# Copy to the HA /addons folder (the path keeps the historical name)
cp -r privacy_witness_kernel /addons/privacy_witness_kernel
```

---

## Configuration

### Step 1: Generate Device Key

The device key is a secret that establishes your device's cryptographic identity. Generate one:

```bash
openssl rand -hex 32
```

Save this key securely. If you lose it, you cannot verify old event signatures.

### Step 2: Configure the App

In the app configuration panel:

```yaml
# Required: Your unique device key (generate with openssl rand -hex 32)
device_key_seed: "your-64-character-hex-key-here"

# Camera discovery from go2rtc (recommended)
go2rtc_discovery: true
go2rtc_url: "http://homeassistant.local:1984"

# Or manual camera configuration
cameras:
  - name: front_door
    url: rtsp://admin:password@192.168.1.100:554/stream1
    zone_id: zone:front_door
    fps: 10
    width: 640
    height: 480

  - name: driveway
    url: rtsp://admin:password@192.168.1.101:554/stream1
    zone_id: zone:driveway

# How long to keep events (days)
retention_days: 7

# Time bucket size (minutes) - events are grouped into buckets
# Larger = more privacy, smaller = finer granularity
time_bucket_minutes: 10

# Logging verbosity
log_level: info
```

### Step 3: Start the App

Click **Start**. Check the logs for any errors.

---

## go2rtc Integration

[go2rtc](https://github.com/AlexxIT/go2rtc) is the recommended way to connect cameras in Home Assistant. The Privacy Witness Kernel can auto-discover cameras from go2rtc.

### If You Already Use go2rtc

1. Enable discovery in the app config:
   ```yaml
   go2rtc_discovery: true
   go2rtc_url: "http://homeassistant.local:1984"
   ```

2. The app will automatically find your cameras

### If You Don't Use go2rtc

Either:
1. Install the [go2rtc app](https://github.com/AlexxIT/go2rtc) (recommended)
2. Or configure cameras manually in the PWK app

### Frigate Users

If you use Frigate, it includes go2rtc. Point the discovery URL to Frigate:
```yaml
go2rtc_url: "http://frigate:1984"
```

---

## MQTT Discovery (Automatic Sensors)

PWK supports **Home Assistant MQTT Discovery**, which automatically creates sensors without any manual configuration.

### Enable MQTT Publishing

```yaml
mqtt_publish:
  enabled: true
  host: "core-mosquitto"    # HA's built-in MQTT broker
  port: 1883
  username: ""              # Optional: MQTT auth
  password: ""
  topic_prefix: "witness"   # State topics: witness/zone/*/event
  discovery_prefix: "homeassistant"  # HA discovery prefix
```

#### TLS Settings (Optional)

If your broker requires TLS, enable it via the bridge environment (or CLI flags):

| Environment Variable | Description |
|---|---|
| `MQTT_USE_TLS` | Enable TLS (required for `mqtts://` brokers) |
| `MQTT_TLS_CA_PATH` | Path to PEM-encoded CA certificate |
| `MQTT_TLS_CLIENT_CERT_PATH` | Path to PEM-encoded client certificate (mutual TLS) |
| `MQTT_TLS_CLIENT_KEY_PATH` | Path to PEM-encoded client private key (mutual TLS) |

### Auto-Created Entities

When enabled, PWK automatically creates these entities for each zone:

| Entity | Type | Description |
|--------|------|-------------|
| `sensor.pwk_<zone>_events` | Sensor | Total event count (state_class: total_increasing) |
| `binary_sensor.pwk_<zone>_motion` | Binary Sensor | Motion detected (auto-off after 10 min) |
| `sensor.pwk_last_event` | Sensor | Most recent event with full attributes |

### Entity Attributes

The `sensor.pwk_last_event` entity includes these attributes:
Time is reported as coarse buckets (typically 10 minutes), not precise timestamps.

```yaml
event_type: "BoundaryCrossingObjectLarge"
zone_id: "zone:front_door"
time_bucket_start: 1706140800
time_bucket_size: 600
confidence: 0.85
published_bucket_start: 1706141400
published_bucket_size: 600
```

### Availability Tracking

PWK publishes to `witness/status` with Last Will Testament (LWT):
- **Online**: PWK is running and publishing events
- **Offline**: PWK has disconnected (set automatically by MQTT broker)

All entities use this availability topic, so they show "unavailable" when PWK is offline.

### MQTT Topics Published

| Topic | Payload | Retained |
|-------|---------|----------|
| `witness/status` | `online` / `offline` | Yes |
| `witness/last_event` | JSON event details | Yes |
| `witness/zone/<name>/count` | Event count integer | Yes |
| `witness/zone/<name>/motion` | `ON` | No |
| `witness/zone/<name>/event` | Full event JSON | No |
| `witness/events` | All events (firehose) | No |

### Example Automation with MQTT Sensors

```yaml
# automations.yaml
automation:
  - alias: "Notify on Front Door Motion"
    trigger:
      - platform: state
        entity_id: binary_sensor.pwk_front_door_motion
        to: "on"
    action:
      - service: notify.mobile_app
        data:
          title: "Motion Detected"
          message: >
            Motion at front door.
            Confidence: {{ state_attr('sensor.pwk_last_event', 'confidence') }}
```

---

## Manual Sensors (Alternative)

If you prefer not to use MQTT Discovery, you can create sensors manually using the REST API.

The app exposes an Event API on port 8799. When Home Assistant is running
alongside the app, use the app hostname (its slug) so HA can reach it over
the Supervisor network. The default slug for this repository is
`privacy_witness_kernel`, which results in `http://privacy_witness_kernel:8799`.
You can confirm the hostname in **Settings → Apps → Privacy Witness Kernel →
Info**, where Home Assistant lists the app hostname/slug.

### REST Sensor (Basic)

```yaml
# configuration.yaml
sensor:
  - platform: rest
    name: "PWK Last Event"
    resource: http://privacy_witness_kernel:8799/events/latest
    headers:
      Authorization: Bearer YOUR_API_TOKEN
    value_template: "{{ value_json.event_type }}"
    json_attributes:
      - zone_id
      - time_bucket
      - confidence
      - kernel_version
      - ruleset_id
    scan_interval: 30
```

### Template Sensor (Event Count)

```yaml
# configuration.yaml
template:
  - sensor:
      - name: "Boundary Crossings Today"
        state: "{{ states('sensor.pwk_boundary_crossing_count') | int }}"
        icon: mdi:walk
```

### Automation Example

```yaml
# automations.yaml
automation:
  - alias: "Notify on Large Object Detection"
    trigger:
      - platform: state
        entity_id: sensor.pwk_last_event
        to: "BoundaryCrossingObjectLarge"
    action:
      - service: notify.mobile_app
        data:
          title: "Motion Detected"
          message: "Large object detected in {{ state_attr('sensor.pwk_last_event', 'zone_id') }}"
```

---

## API Reference

The Event API is available at `http://privacy_witness_kernel:8799` when running
as a Home Assistant app (or your configured port), as this hostname is
automatically resolved by the Supervisor. If the kernel runs elsewhere, replace
the hostname with the reachable IP/DNS name for that host.

### Authentication

The API uses short-lived capability tokens as **Bearer** credentials. The token is written to `/config/api_token` when the app starts and rotates every 10 minutes; read it from the configured token file whenever you need to authenticate. If you run the kernel elsewhere, use the token path or secrets location configured for that deployment. The SecuraCV integration handles rotation automatically when configured with the token-file path (its default); scripts and other clients must re-read the file on every `401`.
The `/health` endpoint is unauthenticated and only reachable on the local loopback interface. Query-string tokens are rejected—send the token only in the `Authorization: Bearer` header.

```bash
# Read the token
TOKEN=$(cat /config/api_token)

# Make authenticated requests (Bearer token only)
curl -H "Authorization: Bearer $TOKEN" http://privacy_witness_kernel:8799/events
```

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/events` | GET | Export events as batched buckets |
| `/events/latest` | GET | Get the most recent event (single event JSON) |
| `/digest` | GET | Coarse activity digest for dashboards (bucketed counts, no per-event detail) |
| `/status` | GET | Daemon status snapshot (retention, verify state) |
| `/verify` | POST | Run sealed-log verification and return the `VerifyReport` |
| `/health` | GET | Check daemon health (unauthenticated) |

### `/events/latest` Response (Event)

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "time_bucket": {
    "start_epoch_s": 1706140800,
    "size_s": 600
  },
  "zone_id": "zone:front_door",
  "confidence": 0.85,
  "kernel_version": "0.4.2",
  "ruleset_id": "baseline"
}
```

### `/events` Response (Export Artifact)

```json
{
  "batches": [
    {
      "buckets": [
        {
          "time_bucket": {
            "start_epoch_s": 1706140800,
            "size_s": 600
          },
          "events": [
            {
              "event_type": "BoundaryCrossingObjectLarge",
              "time_bucket": {
                "start_epoch_s": 1706140800,
                "size_s": 600
              },
              "zone_id": "zone:front_door",
              "confidence": 0.85,
              "kernel_version": "0.4.2",
              "ruleset_id": "baseline"
            }
          ]
        }
      ]
    }
  ],
  "max_events_per_batch": 50,
  "jitter_s": 120,
  "jitter_step_s": 60
}
```

### `/health` Response

```json
{
  "status": "ok"
}
```

---

## Privacy Features

### What the App DOES:
- Detects motion/boundary crossing events
- Records coarse-grained event claims (zone + 10-minute bucket)
- Stores cryptographically signed event log locally
- Expires old events according to retention policy

### What the App DOES NOT:
- Export raw video frames
- Record faces, license plates, or identifying features
- Send data to any cloud service
- Allow bulk historical queries
- Create searchable recordings

### Break-Glass Access

In emergency situations (e.g., law enforcement request with warrant), raw frames can be accessed through a quorum-based break-glass process. This requires:
1. Multiple trustees to approve
2. A specific time window
3. Immutable audit logging

Operationally, the flow is:
1. Store the quorum policy (`break_glass policy set`)
2. Create a request (`break_glass request`) and share the request hash
3. Trustees sign approvals (`break_glass approve`)
4. Authorize and emit a token file (`break_glass authorize --output-token ...`)
5. Unseal with the token (`break_glass unseal`)

See [Break-Glass Documentation](../spec/break_glass.md) for details.

---

## Troubleshooting

### App Won't Start

1. Check logs: **Settings → Apps → Privacy Witness Kernel → Logs**
2. Verify `device_key_seed` is set and valid (64 hex characters)
3. Ensure cameras are reachable

### No Cameras Discovered

1. Verify go2rtc is running: `curl http://homeassistant.local:1984/api/streams`
2. Check go2rtc URL is correct
3. Try manual camera configuration

### Events Not Appearing

1. Check the camera is streaming (view in HA)
2. Verify zone_id format: `zone:[a-z0-9_-]{1,64}`
3. Check app logs for errors

### High Resource Usage

1. Reduce camera resolution (use sub-stream)
2. Lower FPS to 5
3. Increase time bucket size

---

## Architecture Notes

The Privacy Witness Kernel runs as a separate process (app) for isolation:

1. **No HA core access**: The app cannot access HA internals
2. **Local storage only**: All data stays on your HA instance
3. **Sandboxed modules**: Detection modules run in seccomp sandbox
4. **API isolation**: Only the Event API is exposed

This design means even if the app is compromised, it cannot:
- Access other HA apps
- Export raw video
- Modify HA configuration

---

## Updates

The app updates independently of Home Assistant:

1. Go to **Settings → Apps → Privacy Witness Kernel**
2. Click **Update** when available

Update notes are in the [CHANGELOG](../CHANGELOG.md).

---

## Getting Help

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
- [Home Assistant Community](https://community.home-assistant.io/)
- [Documentation](../README.md)
