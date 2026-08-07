# Frigate NVR Integration

This guide explains how to integrate the Privacy Witness Kernel with [Frigate NVR](https://frigate.video) for Home Assistant users.

## Frigate Compatibility

Tested against the Frigate 0.14–0.17 MQTT schema:

| Topic | Supported | Notes |
|-------|-----------|-------|
| `<prefix>/events` | All versions | Default. "new" events only; updates/ends/false-positives skipped |
| `<prefix>/reviews` | Frigate 0.14+ | Opt-in (`enable_reviews`). Real before/after schema with `severity` |

The topic prefix follows Frigate's `mqtt.topic_prefix` (default `frigate`) —
set `frigate.topic_prefix` (app) or `FRIGATE_TOPIC_PREFIX` (Docker
sidecar) if you changed it. The camera+label-per-bucket dedup means
enabling reviews on top of events never double-logs a detection.

## Why Use Both?

| System | Strengths | Limitations |
|--------|-----------|-------------|
| **Frigate** | Excellent ML detection (TensorFlow, Coral TPU), real-time alerts, recordings | Stores full recordings, detailed object tracking |
| **PWK** | Privacy-preserving logging, tamper-evident, no raw video export | Simpler detection (without Frigate) |

**Together**: Use Frigate's superior detection, but log events through PWK for privacy-preserving long-term storage.

---

## Quick Start (Home Assistant)

### 1. Install the App

1. Go to **Settings → Apps → App Store**
2. Add repository: `https://github.com/kmay89/securaCV`
3. Install "Privacy Witness Kernel"

### 2. Configure for Frigate

Open the app Web UI and click through the setup wizard — that's the
whole configuration for most installs:

- The **device key is generated automatically** (and persisted to
  `/config/.securacv/device_key`; back it up — HA backups include it).
- The **MQTT broker is auto-discovered** from the Supervisor when you run
  the Mosquitto app: host, port, and credentials, nothing to type.
- **HA sensors are on by default** (`mqtt_publish.enabled: true`).

Manual YAML is only needed for an external broker or non-default Frigate
topics:

```yaml
# Mode: Use Frigate's detection instead of processing RTSP directly
mode: "frigate"

# Optional — auto-generated when left empty
device_key_seed: ""

# Frigate MQTT settings. Empty host/credentials = auto-discover from the
# Supervisor MQTT service (Mosquitto app).
frigate:
  mqtt_host: ""                 # set only for an external broker
  mqtt_port: 1883
  topic_prefix: "frigate"       # match Frigate's mqtt.topic_prefix
  enable_reviews: false         # also ingest <prefix>/reviews (0.14+)
  mqtt_username: ""
  mqtt_password: ""
  min_confidence: 0.5           # Ignore low-confidence detections
  cameras: []                   # Empty = all cameras
  labels: ["person", "car", "dog", "cat"]

# HA sensor creation via MQTT Discovery (on by default)
mqtt_publish:
  enabled: true

# How long to keep privacy-preserving events
retention_days: 30

# Hours between automatic sealed-log verifications (0 disables)
verify_interval_hours: 24
```

### 3. Start the App

That's it! The app will:
1. Subscribe to Frigate's MQTT events (and reviews, if enabled)
2. Strip identity data (object IDs, coordinates, thumbnails)
3. Coarsen timestamps to 10-minute buckets
4. Write sanitized events to the sealed log
5. Create HA entities automatically, including:
   - `sensor.pwk_daily_digest` — rolling 24h summary (counts per zone,
     coarse day periods)
   - `binary_sensor.pwk_chain_problem` — sealed-log integrity (verified
     automatically every `verify_interval_hours`)
   - `button.pwk_verify_now` — one-click verification from any dashboard

The app Web UI doubles as a status panel after setup: chain-integrity
badge, 24h digest, a Verify Now button, and a **dashboard generator** that
emits Lovelace YAML for your actual zones. For the morning summary on your
phone, import the
[daily digest blueprint](blueprints/securacv_daily_digest.yaml).

---

## Quick Start (Docker, no Home Assistant)

Already running Frigate under Docker without HA? The sidecar image wraps
witness_api + frigate_bridge + event_mqtt_bridge in one container:

```bash
# 1. Grab the quickstart compose file
curl -fsSLO https://raw.githubusercontent.com/kmay89/securaCV/main/docker/sidecar/quickstart.compose.yml

# 2. Set FRIGATE_MQTT_HOST in it to the broker Frigate publishes to, then:
docker compose -f quickstart.compose.yml up -d

# 3. Diagnose the integration end-to-end (broker, auth, Frigate traffic,
#    sealed-log verification):
docker compose -f quickstart.compose.yml run --rm securacv doctor
```

The device key is generated on first start into the `securacv_data` volume
(back it up). No broker yet? Use
`docker/sidecar/quickstart-with-broker.compose.yml`, which bundles
Mosquitto. The bundled broker is reachable only from that compose network
and from the Docker host (loopback port binding); to let a Frigate on
another machine connect, set `SECURACV_MQTT_PASSWORD` and
`SECURACV_MQTT_BIND=0.0.0.0` in a `.env` file — the compose file's header
walks through it. The full environment-variable contract is documented at
the top of [`docker/sidecar/entrypoint.sh`](../docker/sidecar/entrypoint.sh).

Verify the sealed log from the host at any time:

```bash
docker compose exec securacv sh -c 'DEVICE_KEY_SEED=$(cat /data/device_key) log_verify --db /data/witness.db'
```

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

### App Options (frigate mode)

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

### MQTT TLS Options (Bridges)

Both `frigate_bridge` (subscribe) and `event_mqtt_bridge` (publish) support **MQTT v5** and TLS.
Use a `mqtts://` broker address or set `MQTT_USE_TLS=1` to enable TLS, then configure certificates
as needed:

| Environment Variable | Description |
|---|---|
| `MQTT_USE_TLS` | Enable TLS (required for `mqtts://` brokers) |
| `MQTT_TLS_CA_PATH` | Path to PEM-encoded CA certificate |
| `MQTT_TLS_CLIENT_CERT_PATH` | Path to PEM-encoded client certificate (mutual TLS) |
| `MQTT_TLS_CLIENT_KEY_PATH` | Path to PEM-encoded client private key (mutual TLS) |

When `mqtt_publish.enabled` is `true`, PWK will:
1. Publish HA MQTT Discovery configs for automatic entity creation
2. Create sensors for each zone (event count, motion state)
3. Publish availability status with LWT (Last Will Testament)
4. Use QoS 1 for reliable message delivery

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

# TLS example (mqtts:// broker with custom CA)
cargo run --bin frigate_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr mqtts://core-mosquitto:8883 \
  --mqtt-use-tls \
  --mqtt-tls-ca-path /config/mqtt/ca.pem \
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

# TLS example (mqtts:// broker with mutual TLS)
cargo run --bin event_mqtt_bridge -- \
  --daemon \
  --allow-remote-mqtt \
  --mqtt-broker-addr mqtts://core-mosquitto:8883 \
  --mqtt-use-tls \
  --mqtt-tls-ca-path /config/mqtt/ca.pem \
  --mqtt-tls-client-cert-path /config/mqtt/client.crt \
  --mqtt-tls-client-key-path /config/mqtt/client.key \
  --api-token-path /config/api_token
```

---

## Example: Privacy-First Setup

Here's how to configure Frigate + PWK for maximum privacy:

### Frigate Config

> **Where this file lives:** the Frigate **Home Assistant app** (0.16+) reads
> `/addon_configs/ccab4aaf_frigate/config.yml` (editable from the Frigate Web UI's
> configuration editor or a file editor app) — *not* `/config/frigate.yml`, which
> SecuraCV only uses as a generated template. In a **Docker** deployment it's whatever
> config file you mount into the Frigate container.

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

### PWK App Config

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
   # Frigate config (see "Where this file lives" above)
   mqtt:
     enabled: true
   ```

2. **Verify MQTT connectivity**:
   ```bash
   mosquitto_sub -h core-mosquitto -t "frigate/events" -v
   ```
   You should see events when objects are detected.

3. **Check app logs**:
   Go to **Settings → Apps → Privacy Witness Kernel → Logs**

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
