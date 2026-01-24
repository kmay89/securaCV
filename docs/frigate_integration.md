# Frigate NVR Integration

The Privacy Witness Kernel can integrate with [Frigate NVR](https://frigate.video) to leverage its excellent ML-based object detection while adding a privacy-preserving event log.

## How It Works

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Frigate NVR                                  │
│  ┌──────────┐   ┌──────────────┐   ┌───────────────┐               │
│  │  Camera  │──▶│  Detection   │──▶│  MQTT Events  │               │
│  │  Streams │   │  (TensorFlow)│   │  (detailed)   │               │
│  └──────────┘   └──────────────┘   └───────┬───────┘               │
└─────────────────────────────────────────────┼───────────────────────┘
                                              │
                                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   Privacy Witness Kernel                             │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    frigate_bridge                             │   │
│  │  ┌─────────────┐   ┌─────────────┐   ┌──────────────────┐   │   │
│  │  │ Subscribe   │──▶│  Sanitize   │──▶│  Sealed Log      │   │   │
│  │  │ MQTT Events │   │  - Strip ID │   │  (privacy-safe)  │   │   │
│  │  └─────────────┘   │  - Coarsen  │   └──────────────────┘   │   │
│  │                    │    time     │                           │   │
│  │                    └─────────────┘                           │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### What Gets Stripped (Privacy Sanitization)

| Frigate Data | PWK Handling |
|--------------|--------------|
| Object tracking ID | **Removed** - no cross-event correlation |
| Precise timestamp | **Coarsened** - 10-minute buckets |
| Bounding box coordinates | **Removed** - no position tracking |
| Thumbnail/snapshot | **Removed** - no raw images |
| Camera coordinates | **Removed** - zone ID only |
| Top score | **Kept** as confidence |
| Object label | **Kept** as event type |
| Zone name | **Kept** as zone_id |

### What Gets Logged

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": { "start_epoch_s": 1706140800, "size_s": 600 },
  "confidence": 0.85
}
```

---

## Setup Options

### Option 1: Home Assistant Add-on (Recommended)

1. Install the Privacy Witness Kernel add-on
2. Set mode to `frigate`:

```yaml
mode: "frigate"
device_key_seed: "your-64-char-hex-key"

frigate:
  mqtt_host: "core-mosquitto"
  mqtt_port: 1883
  mqtt_topic: "frigate/events"
  min_confidence: 0.5
  cameras: []  # Empty = all cameras
  labels: ["person", "car", "dog", "cat"]
```

3. Start the add-on

### Option 2: Standalone frigate_bridge

```bash
# Generate device key
export DEVICE_KEY_SEED=$(openssl rand -hex 32)

# Run the Frigate bridge
cargo run --bin frigate_bridge -- \
  --mqtt-broker-addr 127.0.0.1:1883 \
  --frigate-topic frigate/events \
  --db-path witness.db \
  --min-confidence 0.5
```

---

## Configuration Reference

### frigate_bridge Options

| Option | Env Variable | Default | Description |
|--------|--------------|---------|-------------|
| `--mqtt-broker-addr` | `MQTT_BROKER_ADDR` | `127.0.0.1:1883` | MQTT broker address (must be loopback) |
| `--frigate-topic` | `FRIGATE_MQTT_TOPIC` | `frigate/events` | Frigate MQTT topic |
| `--db-path` | `WITNESS_DB_PATH` | `witness.db` | Path to witness database |
| `--ruleset-id` | `WITNESS_RULESET_ID` | `ruleset:frigate_v1` | Ruleset identifier |
| `--bucket-size-secs` | `WITNESS_BUCKET_SIZE` | `600` | Time bucket size (seconds) |
| `--min-confidence` | `FRIGATE_MIN_CONFIDENCE` | `0.5` | Minimum detection confidence |
| `--cameras` | `FRIGATE_CAMERAS` | (all) | Comma-separated camera names |
| `--labels` | `FRIGATE_LABELS` | (default set) | Comma-separated object labels |

### Supported Labels

Default labels that are processed:
- `person` → `BoundaryCrossingObjectLarge`
- `car`, `truck`, `bus`, `motorcycle` → `BoundaryCrossingObjectLarge`
- `dog`, `cat`, `bird` → `BoundaryCrossingObjectSmall`
- `bicycle` → `BoundaryCrossingObjectSmall`

---

## MQTT Topics

### frigate/events (Recommended)

Frigate publishes detailed events here. The bridge extracts:
- Camera name → zone_id
- Object label → event_type
- Top score → confidence
- Current zones → zone_id (if defined)

### frigate/reviews (Alternative)

For users who want to process only confirmed detections:
```bash
--frigate-topic frigate/reviews
```

---

## Privacy Comparison

| Feature | Frigate Alone | Frigate + PWK |
|---------|---------------|---------------|
| Real-time detection | Yes | Yes (via Frigate) |
| Object tracking | Yes (with IDs) | No (stripped) |
| Precise timestamps | Yes | No (10-min buckets) |
| Searchable recordings | Yes | No |
| Face/plate detection | Possible | Blocked |
| Long-term event log | No | Yes (sealed, signed) |
| Tamper evidence | No | Yes (cryptographic) |
| Break-glass access | No | Yes (quorum) |

---

## Architecture Benefits

### 1. Separation of Concerns
- **Frigate**: Optimized for real-time detection with GPU acceleration
- **PWK**: Optimized for privacy-preserving long-term logging

### 2. Best of Both Worlds
- Get Frigate's excellent TensorFlow/Coral detection
- Add PWK's privacy guarantees and tamper-evident logging

### 3. Frigate Recordings Are Optional
- With PWK, you can set Frigate's retention to very short (or disable recordings)
- PWK provides the long-term "witness" layer without raw video

### 4. Auditability
- All events are cryptographically signed
- Retention is automatically enforced
- Break-glass access requires quorum approval

---

## Example: Frigate + PWK Setup

### Frigate Configuration (frigate.yml)

```yaml
mqtt:
  enabled: true
  host: core-mosquitto
  port: 1883
  topic_prefix: frigate

cameras:
  front_door:
    ffmpeg:
      inputs:
        - path: rtsp://admin:password@192.168.1.100:554/stream1
          roles: [detect]
    detect:
      enabled: true
      width: 1280
      height: 720
    zones:
      front_porch:
        coordinates: 0,720,400,720,400,0,0,0

  driveway:
    ffmpeg:
      inputs:
        - path: rtsp://admin:password@192.168.1.101:554/stream1
          roles: [detect]
    detect:
      enabled: true

# Minimal recording retention (PWK handles long-term)
record:
  enabled: true
  retain:
    days: 1  # Keep raw video for only 1 day
```

### PWK Add-on Configuration

```yaml
mode: "frigate"
device_key_seed: "abc123..."  # Your generated key

frigate:
  mqtt_host: "core-mosquitto"
  mqtt_port: 1883
  mqtt_topic: "frigate/events"
  min_confidence: 0.6
  cameras: ["front_door", "driveway"]
  labels: ["person", "car", "dog"]

retention_days: 30  # Keep privacy-preserving events for 30 days
time_bucket_minutes: 10
```

---

## Troubleshooting

### No Events Being Logged

1. **Check Frigate is publishing to MQTT:**
   ```bash
   mosquitto_sub -h localhost -t "frigate/#" -v
   ```

2. **Check bridge is running:**
   ```bash
   # In HA add-on logs
   # Or standalone:
   RUST_LOG=debug cargo run --bin frigate_bridge -- ...
   ```

3. **Verify camera names match:**
   - Frigate camera names are case-sensitive
   - `--cameras front_door` won't match `Front_Door`

### MQTT Connection Refused

1. Ensure MQTT broker is running
2. For HA, use `core-mosquitto` as the host
3. Verify port is correct (default: 1883)

### Low Confidence Events Not Appearing

- Check `--min-confidence` setting
- Frigate's `min_score` in detect config affects what gets published

---

## Security Notes

1. **MQTT must be loopback-only** - The bridge refuses non-loopback connections
2. **Frigate credentials are never stored** - PWK only receives sanitized events
3. **Object IDs are never logged** - No cross-event correlation possible
4. **Thumbnails/snapshots are ignored** - Raw image data never enters PWK

---

## Next Steps

- [Home Assistant Integration](homeassistant_setup.md) - Full HA setup guide
- [Event Verification](../spec/verification.md) - Verify event integrity
- [Break-Glass Access](../spec/break_glass.md) - Emergency access procedures
