# Home Assistant Integration Guide

Install the SecuraCV Home Assistant integration via HACS, then connect it to a running Privacy Witness Kernel instance (add-on, Docker, or another host).

## Choose Your Mode

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
- The add-on always starts the Event API service in Frigate mode, even if MQTT publishing is disabled

#### Frigate + MQTT Configuration Checklist

- [ ] **Mode set to frigate**: `mode: "frigate"` is configured in the add-on options.
- [ ] **Frigate MQTT broker details match**: `frigate.mqtt_host`, `frigate.mqtt_port`, `frigate.mqtt_username`, and `frigate.mqtt_password` match the broker that Frigate uses.
- [ ] **Frigate event topic is correct**: `frigate.mqtt_topic` matches Frigate’s configured event topic (default `frigate/events`).
- [ ] **Home Assistant MQTT publish settings are aligned**: if you enable `mqtt_publish.enabled`, ensure `mqtt_publish.host`, `mqtt_publish.port`, `mqtt_publish.username`, and `mqtt_publish.password` match the same broker.
- [ ] **Topic + discovery prefixes are consistent**: `mqtt_publish.topic_prefix` is the prefix you expect for PWK events, and `mqtt_publish.discovery_prefix` matches Home Assistant’s discovery prefix (default `homeassistant`).
- [ ] **Required add-on options from the Configuration tab are configured**: `device_key_seed` is set, `mode` is still `frigate`, and any Frigate-specific options (`frigate.cameras`, `frigate.labels`, `frigate.min_confidence`) are configured as needed.
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
3. Add `https://github.com/kmay89/securaCV` as an **Integration**
4. Install **SecuraCV** and restart Home Assistant when prompted

### Step 2: Run the Kernel

Choose one runtime option and configure it in either **frigate** or **standalone** mode:

**Option A: Home Assistant add-on (custom repository)**
1. Go to **Settings → Add-ons → Add-on Store**
2. Click **⋮ → Repositories** → Add: `https://github.com/kmay89/securaCV`
3. Install **Privacy Witness Kernel**


### Step 2: Configure

First, generate a device key. This is required for the kernel configuration.
**Option B: Docker / another host**
1. Run the kernel using your preferred deployment method
2. Ensure the Event API is reachable from Home Assistant

### Step 3: Generate a Device Key (Kernel)

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

### Step 3: Start

Click **Start**. Check logs for any errors.

---
### Step 5: Add the Integration

1. Go to **Settings → Devices & Services**
2. Click **Add Integration** and select **SecuraCV**
3. Provide the Event API URL and token from your kernel instance (MQTT is optional)

---

## Distribution

- **HACS integration (current):** install the SecuraCV integration in Home Assistant.
- **Kernel runtime:** run the Privacy Witness Kernel as an add-on, container, or service. The integration connects to its Event API.

---

## Kernel Installation (Optional Add-on)

### Option 1: Add-on Repository

1. In Home Assistant, go to **Settings → Add-ons → Add-on Store**
2. Click the menu (⋮) → **Repositories**
3. Add: `https://github.com/kmay89/securaCV`
4. Find "Privacy Witness Kernel" and click **Install**

### Option 2: Local Installation

```bash
# Clone the repository
git clone https://github.com/kmay89/securaCV.git
cd securaCV

# Build the add-on container
docker build -f homeassistant/Dockerfile -t privacy-witness-kernel .

# Copy to HA add-ons folder
cp -r homeassistant /addons/privacy_witness_kernel
```

---

## Configuration

### Step 1: Generate Device Key

The device key is a secret that establishes your device's cryptographic identity. Generate one:

```bash
openssl rand -hex 32
```

Save this key securely. If you lose it, you cannot verify old event signatures.

### Step 2: Configure the Add-on

In the add-on configuration panel:

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

### Step 3: Start the Add-on

Click **Start**. Check the logs for any errors.

---

## go2rtc Integration

[go2rtc](https://github.com/AlexxIT/go2rtc) is the recommended way to connect cameras in Home Assistant. The Privacy Witness Kernel can auto-discover cameras from go2rtc.

### If You Already Use go2rtc

1. Enable discovery in the add-on config:
   ```yaml
   go2rtc_discovery: true
   go2rtc_url: "http://homeassistant.local:1984"
   ```

2. The add-on will automatically find your cameras

### If You Don't Use go2rtc

Either:
1. Install the [go2rtc add-on](https://github.com/AlexxIT/go2rtc) (recommended)
2. Or configure cameras manually in the PWK add-on

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

### Auto-Created Entities

When enabled, PWK automatically creates these entities for each zone:

| Entity | Type | Description |
|--------|------|-------------|
| `sensor.pwk_<zone>_events` | Sensor | Total event count (state_class: total_increasing) |
| `binary_sensor.pwk_<zone>_motion` | Binary Sensor | Motion detected (auto-off after 10 min) |
| `sensor.pwk_last_event` | Sensor | Most recent event with full attributes |

### Entity Attributes

The `sensor.pwk_last_event` entity includes these attributes:
Time is reported as a coarse `time_bucket` (typically 10–15 minutes), not a precise timestamp.

```yaml
event_type: "BoundaryCrossingObjectLarge"
zone_id: "zone:front_door"
time_bucket:
  start_epoch_s: 1706140800
  size_s: 600
confidence: 0.85
kernel_version: "0.4.2"
ruleset_id: "baseline"
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

The add-on exposes an Event API on port 8799. When Home Assistant is running
alongside the add-on, use the add-on hostname (its slug) so HA can reach it over
the Supervisor network. The default slug for this repository is
`privacy_witness_kernel`, which results in `http://privacy_witness_kernel:8799`.
You can confirm the hostname in **Settings → Add-ons → Privacy Witness Kernel →
Info**, where Home Assistant lists the add-on hostname/slug.

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
as a Home Assistant add-on (or your configured port), as this hostname is
automatically resolved by the Supervisor. If the kernel runs elsewhere, replace
the hostname with the reachable IP/DNS name for that host.

### Authentication

The API uses short-lived capability tokens as **Bearer** credentials. The token is written to `/config/api_token` when the add-on starts and rotates every 10 minutes; read it from the configured token file whenever you need to authenticate. If you run the kernel elsewhere, use the token path or secrets location configured for that deployment.
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
| `/health` | GET | Check daemon health |

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

### What the Add-on DOES:
- Detects motion/boundary crossing events
- Records coarse-grained event claims (zone + 10-minute bucket)
- Stores cryptographically signed event log locally
- Expires old events according to retention policy

### What the Add-on DOES NOT:
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
1. Create a request (`break_glass request`) and share the request hash
2. Trustees sign approvals (`break_glass approve`)
3. Authorize and emit a token file (`break_glass authorize --output-token ...`)
4. Unseal with the token (`break_glass unseal`)

See [Break-Glass Documentation](../spec/break_glass.md) for details.

---

## Troubleshooting

### Add-on Won't Start

1. Check logs: **Settings → Add-ons → Privacy Witness Kernel → Logs**
2. Verify `device_key_seed` is set and valid (64 hex characters)
3. Ensure cameras are reachable

### No Cameras Discovered

1. Verify go2rtc is running: `curl http://homeassistant.local:1984/api/streams`
2. Check go2rtc URL is correct
3. Try manual camera configuration

### Events Not Appearing

1. Check the camera is streaming (view in HA)
2. Verify zone_id format: `zone:[a-z0-9_-]{1,64}`
3. Check add-on logs for errors

### High Resource Usage

1. Reduce camera resolution (use sub-stream)
2. Lower FPS to 5
3. Increase time bucket size

---

## Architecture Notes

The Privacy Witness Kernel runs as a separate process (add-on) for isolation:

1. **No HA core access**: The add-on cannot access HA internals
2. **Local storage only**: All data stays on your HA instance
3. **Sandboxed modules**: Detection modules run in seccomp sandbox
4. **API isolation**: Only the Event API is exposed

This design means even if the add-on is compromised, it cannot:
- Access other HA add-ons
- Export raw video
- Modify HA configuration

---

## Updates

The add-on updates independently of Home Assistant:

1. Go to **Settings → Add-ons → Privacy Witness Kernel**
2. Click **Update** when available

Update notes are in the [CHANGELOG](../CHANGELOG.md).

---

## Getting Help

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
- [Home Assistant Community](https://community.home-assistant.io/)
- [Documentation](../README.md)
