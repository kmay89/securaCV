# Frigate NVR Integration

This guide explains how to integrate the Privacy Witness Kernel with [Frigate NVR](https://frigate.video) for Home Assistant users.

## Why Use Both?

| System | Strengths | Limitations |
|--------|-----------|-------------|
| **Frigate** | Excellent ML detection (TensorFlow, Coral TPU), real-time alerts, recordings | Stores full recordings, detailed object tracking |
| **PWK** | Privacy-preserving logging, tamper-evident, no raw video export | Simpler detection (without Frigate) |

**Together**: Use Frigate's superior detection, but log events through PWK for privacy-preserving long-term storage.

---

## Quick Start (Home Assistant)

### 1. Install the Add-on

1. Go to **Settings → Add-ons → Add-on Store**
2. Add repository: `https://github.com/kmay89/securaCV`
3. Install "Privacy Witness Kernel"

### 2. Configure for Frigate

```yaml
# Mode: Use Frigate's detection instead of processing RTSP directly
mode: "frigate"

# Your unique device key (generate with: openssl rand -hex 32)
device_key_seed: "your-64-character-hex-key-here"

# Frigate MQTT settings (defaults work for most HA setups)
frigate:
  mqtt_host: "core-mosquitto"   # HA's built-in broker
  mqtt_port: 1883
  mqtt_topic: "frigate/events"
  mqtt_username: ""             # Optional: MQTT authentication
  mqtt_password: ""
  min_confidence: 0.5           # Ignore low-confidence detections
  cameras: []                   # Empty = all cameras
  labels: ["person", "car", "dog", "cat"]

# Enable MQTT publishing for automatic HA sensor creation
mqtt_publish:
  enabled: true

# How long to keep privacy-preserving events
retention_days: 30
```

### 3. Start the Add-on

That's it! The add-on will:
1. Subscribe to Frigate's MQTT events
2. Strip identity data (object IDs, coordinates, thumbnails)
3. Coarsen timestamps to 10-minute buckets
4. Write sanitized events to the sealed log

---

## Architectural Validity

**Question**: Does this integration violate PWK's privacy invariants?

**Answer**: No. Here's why:

### What the frigate_bridge Does

```
Frigate publishes:                    PWK logs:
┌─────────────────────────┐          ┌─────────────────────────┐
│ id: "abc123"            │    →     │ (removed)               │
│ timestamp: 1706140823.4 │    →     │ time_bucket: 1706140800 │
│ camera: "front_door"    │    →     │ zone_id: "zone:front_d" │
│ label: "person"         │    →     │ event_type: Large       │
│ sub_label: "ups"        │    →     │ (category only)         │
│ score: 0.92             │    →     │ confidence: 0.92        │
│ box: [100,200,300,400]  │    →     │ (removed)               │
│ thumbnail: "base64..."  │    →     │ (removed)               │
│ current_zones: ["yard"] │    →     │ zone_id: "zone:porch"   │
│ entered_zones: ["porch"]│    →     │ (uses entered_zones)    │
│ has_clip: true          │    →     │ (not logged)            │
│ has_snapshot: true      │    →     │ (not logged)            │
└─────────────────────────┘          └─────────────────────────┘
```

### Frigate Fields Parsed

| Field | Usage |
|-------|-------|
| `camera` | Maps to zone_id if no zones specified |
| `label` | Maps to EventType (person→Large, car→Large, etc.) |
| `sub_label` | Logged for categorization but not exposed |
| `score`/`top_score` | Uses top_score if available |
| `current_zones` | Used for zone_id (fallback) |
| `entered_zones` | Preferred for zone_id (more complete) |
| `false_positive` | Events marked false_positive are skipped |
| `has_clip`/`has_snapshot` | Logged for debugging, not stored |

### Invariant Compliance

| Invariant | How It's Enforced |
|-----------|-------------------|
| **I. No Raw Export** | frigate_bridge never receives raw video - only MQTT event metadata |
| **II. No Identity** | Object tracking IDs are stripped; only category labels kept |
| **III. Metadata Min** | Timestamps coarsened to 10-minute buckets |
| **IV. Local Ownership** | Sealed log stored locally on HA device |
| **V. Break-Glass** | Vault access still requires quorum (unchanged) |
| **VI. No Retroactive** | Events bound to ruleset at creation |
| **VII. Non-Queryable** | No bulk search interfaces added |

### Trust Boundary

The frigate_bridge is classified as an **external tool** per `kernel/architecture.md` §3.3:
- It cannot access raw media (Frigate doesn't publish it to MQTT)
- It cannot bypass Contract Enforcement (`append_event_checked` validates all events)
- It cannot access the sealed log directly (uses kernel API)

---

## When to Use Frigate Mode vs Standalone

### Use Frigate Mode When:
- You already have Frigate set up
- You want better detection accuracy (Coral TPU, TensorFlow)
- You want real-time alerts AND privacy-preserving long-term logging
- You want to reduce Frigate's recording retention but keep event history

### Use Standalone Mode When:
- You don't have Frigate
- You want a simpler setup with just cameras
- You want complete independence from other systems

---

## Configuration Reference

### Add-on Options (frigate mode)

| Option | Default | Description |
|--------|---------|-------------|
| `frigate.mqtt_host` | `core-mosquitto` | MQTT broker hostname |
| `frigate.mqtt_port` | `1883` | MQTT broker port |
| `frigate.mqtt_topic` | `frigate/events` | Frigate event topic |
| `frigate.mqtt_username` | (empty) | MQTT authentication username |
| `frigate.mqtt_password` | (empty) | MQTT authentication password |
| `frigate.min_confidence` | `0.5` | Minimum detection confidence |
| `frigate.cameras` | `[]` (all) | Camera names to process |
| `frigate.labels` | `[person,car,dog,cat]` | Object types to process |

### MQTT Publishing Options (HA Discovery)

| Option | Default | Description |
|--------|---------|-------------|
| `mqtt_publish.enabled` | `false` | Enable MQTT publishing to HA |
| `mqtt_publish.host` | `core-mosquitto` | MQTT broker for publishing |
| `mqtt_publish.port` | `1883` | MQTT port |
| `mqtt_publish.username` | (empty) | MQTT auth username |
| `mqtt_publish.password` | (empty) | MQTT auth password |
| `mqtt_publish.topic_prefix` | `witness` | Prefix for state topics |
| `mqtt_publish.discovery_prefix` | `homeassistant` | HA discovery prefix |

**MQTT protocol note:** The current bridges use MQTT v3.1.1 over an unencrypted TCP connection; TLS is not yet supported.

When `mqtt_publish.enabled` is `true`, PWK will:
1. Publish HA MQTT Discovery configs for automatic entity creation
2. Create sensors for each zone (event count, motion state)
3. Publish availability status with LWT (Last Will Testament)
4. Use QoS 1 for reliable message delivery

**Follow-up task (if TLS or MQTT v5 is required):** Replace the custom MQTT implementation in the bridges with a standard client library that supports TLS and MQTT v5, while preserving existing privacy guarantees (i.e., do not introduce any new privacy metadata).

### Standalone CLI Usage

```bash
# Generate device key
export DEVICE_KEY_SEED=$(openssl rand -hex 32)

# Run with Frigate (loopback MQTT)
cargo run --bin frigate_bridge -- \
  --mqtt-broker-addr 127.0.0.1:1883 \
  --frigate-topic frigate/events \
  --db-path witness.db

# For HA addon (non-loopback MQTT) - explicitly allow
cargo run --bin frigate_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --mqtt-username homeassistant \
  --mqtt-password your_password \
  --frigate-topic frigate/events \
  --db-path witness.db
```

### Publish Events to HA with MQTT Discovery

```bash
# One-shot mode (publish current events)
cargo run --bin event_mqtt_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --api-token-path /config/api_token

# Daemon mode (continuous publishing)
cargo run --bin event_mqtt_bridge -- \
  --daemon \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --ha-discovery-prefix homeassistant \
  --mqtt-topic-prefix witness \
  --api-token-path /config/api_token
```

---

## Example: Privacy-First Setup

Here's how to configure Frigate + PWK for maximum privacy:

### Frigate Config (frigate.yml)

```yaml
mqtt:
  enabled: true
  host: core-mosquitto

cameras:
  front_door:
    ffmpeg:
      inputs:
        - path: rtsp://admin:pass@192.168.1.100:554/stream
          roles: [detect]
    detect:
      enabled: true

# Minimal recording - just for real-time viewing
record:
  enabled: true
  retain:
    days: 1  # Delete raw video after 1 day

# Disable snapshots to reduce stored data
snapshots:
  enabled: false
```

### PWK Add-on Config

```yaml
mode: "frigate"
device_key_seed: "..."
frigate:
  min_confidence: 0.6
  labels: ["person", "car"]  # Only track these
retention_days: 90  # Keep events for 90 days
```

**Result**:
- Frigate handles detection and 1-day video retention
- PWK keeps privacy-preserving event log for 90 days
- No long-term raw video storage
- No object tracking IDs in long-term storage

---

## Troubleshooting

### Events Not Appearing

1. **Check Frigate MQTT is enabled**:
   ```yaml
   # frigate.yml
   mqtt:
     enabled: true
   ```

2. **Verify MQTT connectivity**:
   ```bash
   mosquitto_sub -h core-mosquitto -t "frigate/events" -v
   ```
   You should see events when objects are detected.

3. **Check add-on logs**:
   Go to **Settings → Add-ons → Privacy Witness Kernel → Logs**

### "MQTT broker must be loopback"

This error occurs when running frigate_bridge standalone without `--allow-remote-mqtt`. Either:
- Use `--allow-remote-mqtt` flag for trusted networks
- Or run MQTT broker on localhost

### Low Detection Rate

- Check `min_confidence` - lower it to catch more events
- Verify camera names match Frigate's config exactly

---

## Security Considerations

### Why `--allow-remote-mqtt` is Safe in HA

The `--allow-remote-mqtt` flag is needed because HA runs services in separate containers. This is safe because:

1. **No raw media flows through MQTT** - Only sanitized event metadata
2. **Events are still validated** - Contract Enforcer rejects non-conforming events
3. **HA network is trusted** - All containers run on the same host
4. **No new attack surface** - The data received is already public to any HA addon

### What's NOT Sent to PWK

Even with Frigate integration, PWK never receives:
- Video frames or thumbnails
- Object bounding boxes or positions
- Face embeddings or license plates
- Object tracking trajectories
- Precise timestamps

---

## Further Reading

- [Home Assistant Setup](homeassistant_setup.md) - Full HA guide
- [RTSP Camera Setup](rtsp_setup.md) - Standalone mode with cameras
- [Invariants Specification](../spec/invariants.md) - Privacy guarantees
- [Architecture](../kernel/architecture.md) - System design
