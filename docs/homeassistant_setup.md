# Home Assistant Integration Guide

The Privacy Witness Kernel can run as a Home Assistant add-on, providing privacy-preserving event detection for your existing camera setup.

## Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     Home Assistant                           │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────┐  │
│  │   go2rtc    │───▶│   PWK       │───▶│  HA Sensors     │  │
│  │  (streams)  │    │  (add-on)   │    │  (events)       │  │
│  └─────────────┘    └─────────────┘    └─────────────────┘  │
│         ▲                                                    │
│         │                                                    │
│  ┌──────┴──────┐                                            │
│  │   Cameras   │                                            │
│  └─────────────┘                                            │
└─────────────────────────────────────────────────────────────┘
```

The add-on:
- Discovers cameras from go2rtc automatically
- Produces privacy-preserving event claims (no raw video export)
- Stores all data locally on your Home Assistant instance
- Exposes events via a local API for HA integration

---

## Installation

### Option 1: Add-on Repository (Recommended)

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

## Creating Home Assistant Sensors

The add-on exposes an Event API on port 8799. You can create sensors to track events.

### REST Sensor (Basic)

```yaml
# configuration.yaml
sensor:
  - platform: rest
    name: "PWK Last Event"
    resource: http://localhost:8799/events/latest
    headers:
      Authorization: Bearer YOUR_API_TOKEN
    value_template: "{{ value_json.event_type }}"
    json_attributes:
      - zone_id
      - time_bucket
      - confidence
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

The Event API is available at `http://localhost:8799` (or your configured port).

### Authentication

The API uses capability tokens. The token is written to `/config/api_token` when the add-on starts.

```bash
# Read the token
TOKEN=$(cat /config/api_token)

# Make authenticated requests
curl -H "Authorization: Bearer $TOKEN" http://localhost:8799/events
```

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/events` | GET | List recent events (paginated) |
| `/events/latest` | GET | Get the most recent event |
| `/health` | GET | Check daemon health |

### Event Schema

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": {
    "start_epoch_s": 1706140800,
    "size_s": 600
  },
  "confidence": 0.85,
  "correlation_token": null
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
