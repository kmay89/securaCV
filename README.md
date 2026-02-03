# SecuraCV

[![HACS Badge](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://github.com/hacs/integration)
[![Validate with HACS](https://github.com/kmay89/securaCV/actions/workflows/validate.yml/badge.svg)](https://github.com/kmay89/securaCV/actions/workflows/validate.yml)

![SecuraCV logo animation](docs/securacv_logo_animation-2.gif)



---

**SecuraCV** is a privacy-preserving computer-vision witness system. 

# Witness Kernel

Core library and tools for the Privacy Witness Kernel (PWK).

## Quickstart: run the demo

```bash
cargo run --bin demo
```

This demo writes artifacts to `./demo_witness.db`, `./demo_out/export_bundle.json`, and the
vault directory at `./vault/envelopes` (unless you override `--out` / `--vault`). Verify the log
integrity with:

```bash
cargo run --bin log_verify -- --db demo_witness.db
```

### Tamper demo

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json
printf "\n" >> demo_out/export_bundle.json
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json  # should FAIL
```

Next: for a real RTSP stream or a local V4L2 device, see the ingestion sections below.

## Documentation

* Canonical specifications:
  * `spec/invariants.md`
  * `spec/event_contract.md`
  * `spec/threat_model.md`
  * `kernel/architecture.md`
* Other docs should link to the canonical specifications rather than duplicate them.
* Host-compromise limitations are documented in `docs/root_paradox.md`.
* Rationale for tamper-evident perception is in `docs/why_witnessing_matters.md`.
* See `kernel/architecture.md` for the canonical vault confidentiality policy and ruleset identifier guidance.
* Contribution guidance is in `CONTRIBUTING.md`.
* Security policy is in `SECURITY.md`.
* Release notes are in `CHANGELOG.md`.

## Firmware

Device firmware lives under `firmware/`.

- **Canary Vision (ESP32-C3 + Grove Vision AI V2)**: `firmware/projects/canary-vision/`  
  PlatformIO project that publishes privacy-preserving semantic events and Home Assistant MQTT discovery.

## Release Gate (v1 Tagging)

Before tagging any v1 release, the Home Assistant + Frigate MQTT pipeline must be verified end-to-end. Complete the v1 verification checklist in `docs/integrations/home-assistant-frigate-mqtt.md` and ensure `integrations/ha_frigate_mqtt/verify_pipeline.sh` succeeds with exit code `0` against a live stack. A v1 tag is blocked until this verification passes.

## Device public key location

The device Ed25519 **verifying key** is stored locally in the witness database at
`device_metadata.public_key` (row `id = 1`). External verification tools like
`log_verify` read this public key from the database by default, or accept an
explicit key via `--public-key` / `--public-key-file` as documented in
`log_verify_README.md`.

## Break-glass policy management

The break-glass CLI stores the quorum policy in the kernel database so
`break_glass authorize` can evaluate approvals against a persistent trustee
roster. Manage the policy with `break_glass policy` subcommands:

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy set \
  --threshold 2 \
  --trustee alice:0123... \
  --trustee bob:4567... \
  --db witness.db

DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy show --db witness.db
```

Trustee entries use the format `id:HEX_PUBLIC_KEY`, where the public key is the
hex-encoded 32-byte Ed25519 verifying key.

## Break-glass unseal workflow

Ensure a quorum policy is stored first (`break_glass policy set`). Then create an
unlock request, gather trustee approvals, and authorize the request before
unsealing. The authorization step logs a receipt (granted or denied) and issues
a sensitive token file via `--output-token`. Use that token to unseal the
envelope. The unseal command writes the clear envelope to `--output-dir`
(default: `vault/unsealed`) so operators can locate the recovered payload
explicitly rather than assuming the CLI lacks an unseal path.

```bash
cargo run --bin break_glass -- request \
  --envelope <envelope_id> \
  --purpose "incident response" \
  --ruleset-id ruleset:v0.3.0

cargo run --bin break_glass -- approve \
  --request-hash <request_hash> \
  --trustee alice \
  --signing-key /path/to/alice.signing.key \
  --output alice.approval

DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- authorize \
  --envelope <envelope_id> \
  --purpose "incident response" \
  --approvals alice.approval,bob.approval \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --output-token /path/to/break_glass.token
```

```bash
cargo run --bin break_glass -- unseal \
  --envelope <envelope_id> \
  --token /path/to/break_glass.token \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --vault-path vault/envelopes \
  --output-dir vault/unsealed
```

## Event export

Use the sequential export tool to write a local artifact with coarse time buckets
and batched events (no precise timestamps or identity selectors).

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- --db-path witness.db --output witness_export.json
```

`export_events` emits a single JSON artifact with batched buckets, applying
default jitter and batching unless overridden by CLI flags.

## Home Assistant Integration

SecuraCV integrates with Home Assistant through two connection methods:

1. **HTTP API** (required) - Connects to the Privacy Witness Kernel for vault storage, event queries, and management
2. **MQTT** (optional) - Real-time updates from Canary devices for multi-transport resilience

The integration surfaces semantic witness events, hash chain integrity, and device health - never raw video or identity data. Privacy by design.

### Requirements

- Home Assistant 2024.4.1 or later
- SecuraCV Privacy Witness Kernel running (Docker, add-on, or standalone)
- MQTT broker (optional, for Canary device real-time updates)

### HACS Installation (Recommended)

1. Open HACS in your Home Assistant instance
2. Click the three dots menu → **Custom repositories**
3. Add `https://github.com/kmay89/securaCV` as an **Integration**
4. Search for "SecuraCV" and install
5. Restart Home Assistant
6. Go to **Settings → Devices & Services → Add Integration → SecuraCV**
7. Enter kernel URL and API token (required)
8. Optionally enable MQTT for Canary device discovery

### Manual Installation

Copy `custom_components/securacv/` to your Home Assistant `config/custom_components/` directory and restart.

### Multi-Transport Resilience Architecture

Canary devices are designed to get witness data OUT by any means necessary before being silenced. The integration surfaces which communication paths are alive:

| Transport | Description | Sensor |
|-----------|-------------|--------|
| WiFi AP | Direct access point mode | `binary_sensor.securacv_{device}_transport_wifi_ap` |
| WiFi Station | Connection to home network | `binary_sensor.securacv_{device}_transport_wifi_sta` |
| MQTT | Broker-based messaging | `binary_sensor.securacv_{device}_transport_mqtt` |
| Bluetooth | BLE direct connection | `binary_sensor.securacv_{device}_transport_ble` |
| Mesh (Opera) | Ed25519 authenticated peer network | `binary_sensor.securacv_{device}_mesh_connected` |
| Chirp | Community alert network (ephemeral IDs) | `binary_sensor.securacv_{device}_chirp_active` |
| LoRa | Long-range radio (future) | - |
| SCQCS | Audio squawk alerts (future) | - |

### Tamper Detection

Each tamper type gets its own binary sensor for targeted automations:

| Threat | Sensor | Trigger |
|--------|--------|---------|
| Power Loss | `tamper_power_loss` | Power removed / brownout detected |
| SD Removed | `tamper_sd_remove` | Storage card physically removed |
| SD Error | `tamper_sd_error` | Storage write failures |
| GPS Jamming | `tamper_gps_jamming` | GPS signal lost or jammed |
| Motion | `tamper_motion` | Unexpected movement detected |
| Enclosure | `tamper_enclosure` | Physical enclosure opened |
| GPIO | `tamper_gpio` | Tamper detection pin triggered |
| Watchdog | `tamper_watchdog` | System hang / timeout |
| Reboot | `tamper_unexpected_reboot` | Unexpected device restart |
| Memory | `tamper_memory_critical` | Critical memory exhaustion |

### Entities Created

**Kernel Sensors (HTTP-based):**
- `sensor.securacv_last_event` - Latest event from the kernel
- `binary_sensor.securacv_kernel_online` - Kernel connectivity

**Canary Sensors (MQTT-based, when enabled):**
- `sensor.securacv_{device}_witness_count` - Total witness records
- `sensor.securacv_{device}_chain_length` - Hash chain length
- `sensor.securacv_{device}_last_event` - Last event type + timestamp
- `sensor.securacv_{device}_health_status` - Device health status
- `sensor.securacv_{device}_gps_fix` - GPS fix status

**Canary Binary Sensors:**
- `binary_sensor.securacv_{device}_online` - Device connectivity (MQTT LWT)
- `binary_sensor.securacv_{device}_chain_valid` - Hash chain integrity
- `binary_sensor.securacv_{device}_tamper` - General tamper detection
- Plus individual tamper type sensors (see above)
- Plus transport health sensors (see above)

**Services:**
- `securacv.export_chain` - Export tamper-evident witness chain
- `securacv.verify_chain` - Verify Ed25519 signatures and hash chain integrity

### MQTT Discovery (Auto-Sensors)

PWK supports **Home Assistant MQTT Discovery** for automatic entity creation:

```yaml
# Add-on configuration
mqtt_publish:
  enabled: true
  host: "core-mosquitto"
```

This automatically creates these entities for each zone:
- `sensor.pwk_<zone>_events` - Event count (state_class: total_increasing)
- `binary_sensor.pwk_<zone>_motion` - Motion state (auto-off after 10 min)
- `sensor.pwk_last_event` - Most recent event with full attributes

Features:
- **QoS 1** for reliable message delivery
- **Last Will Testament** for availability tracking
- **Retained messages** for state persistence across HA restarts

### Frigate Integration

For users with Frigate NVR:

```yaml
mode: "frigate"
frigate:
  mqtt_host: "core-mosquitto"
  mqtt_username: ""  # Optional MQTT auth
  mqtt_password: ""
  min_confidence: 0.5
mqtt_publish:
  enabled: true  # Enable HA Discovery
```

See `docs/homeassistant_setup.md` for the full guide and `docs/frigate_integration.md` for Frigate-specific setup.

For a combined Home Assistant + Frigate MQTT walkthrough, see `docs/integrations/home-assistant-frigate-mqtt.md`.

## RTSP Camera Setup

### Quick Start with Real Cameras

1. Install RTSP dependencies (GStreamer or FFmpeg):
   ```bash
   # Ubuntu/Debian
   sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
       gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libseccomp-dev
   ```

   ```bash
   # Ubuntu/Debian (FFmpeg backend)
   sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libseccomp-dev
   ```

2. Build with RTSP support:
   ```bash
   cargo build --release --features rtsp-gstreamer
   ```

   or

   ```bash
   cargo build --release --features rtsp-ffmpeg
   ```

3. Configure your camera (create `witness.toml`):
   ```toml
   [rtsp]
   url = "rtsp://admin:password@192.168.1.100:554/stream1"
   target_fps = 10
   width = 640
   height = 480
   backend = "auto" # auto, gstreamer, ffmpeg

   [zones]
   module_zone_id = "zone:front_door"
   ```

4. Run (matching the backend feature you built):
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features rtsp-gstreamer --bin witnessd
   ```

See `docs/rtsp_setup.md` for camera URL patterns, troubleshooting, and advanced configuration.

### RTSP Architecture

`witnessd` uses GStreamer or FFmpeg to decode RTSP streams in-memory. Configure an RTSP URL
in `RtspConfig` and the kernel will produce `RawFrame` values without writing
frames to disk. Time coarsening and non-invertible feature hashing happen at
capture time, and `RtspSource::is_healthy()` reports stream health.

GStreamer support is gated behind the `rtsp-gstreamer` feature; FFmpeg support
is gated behind `rtsp-ffmpeg`. The `stub://` scheme keeps the synthetic source
for tests and local development only; release builds reject it. The legacy
`StubFrameSource` helper is only compiled for tests or when the
`stub-frame-source` feature is enabled.

Select a backend with `rtsp.backend = "auto|gstreamer|ffmpeg"` in `witness.toml`
or the `WITNESS_RTSP_BACKEND` environment variable. `auto` prefers GStreamer
when both features are available.

## V4L2 Camera Setup (USB / local devices)

### Quick Start with Local Devices

1. Build with V4L2 support:
   ```bash
   cargo build --release --features ingest-v4l2
   ```

2. Configure your device (create `witness.toml`):
   ```toml
   [ingest]
   backend = "v4l2"

   [v4l2]
   device = "/dev/video0"
   target_fps = 10
   width = 640
   height = 480

   [zones]
   module_zone_id = "zone:front_door"
   ```

3. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features ingest-v4l2 --bin witnessd
   ```

See `docs/v4l2_setup.md` for more details, including stub devices and invariants.

### V4L2 Architecture

`witnessd` captures frames from local `/dev/video*` devices in-memory and produces
`RawFrame` values with coarsened time buckets and non-invertible feature hashes.
The V4L2 backend never writes raw frames to disk and never exposes them over the network.

## Tract (ONNX) backend

The tract backend enables local ONNX inference for object detection. It is
feature-gated (`backend-tract`) and requires a **local** model file. It only
supports RTSP or V4L2 ingest because those provide explicit width/height.

### Recommended small model (Apache-2.0)

Download the ONNX Model Zoo `ssdlite_mobilenet_v2_12` model deterministically:

```bash
mkdir -p vendor/models
curl -L \
  https://github.com/onnx/models/raw/main/vision/object_detection_segmentation/ssdlite_mobilenet_v2/model/ssdlite_mobilenet_v2_12.onnx \
  -o vendor/models/ssdlite_mobilenet_v2_12.onnx
echo "ad6303f1ca2c3dcc0d86a87c36892be9b97b02a0105faa5cc3cfae79a2b11a31  vendor/models/ssdlite_mobilenet_v2_12.onnx" | sha256sum -c -
```

### Required config/CLI fields

**Config (witness.toml):**

```toml
[detect]
backend = "tract"
tract_model = "vendor/models/ssdlite_mobilenet_v2_12.onnx"

[rtsp]
width = 320
height = 320
# OR, for V4L2:
[v4l2]
width = 320
height = 320
```

**Environment overrides:**

- `WITNESS_DETECT_BACKEND=tract`
- `WITNESS_TRACT_MODEL=/absolute/path/to/model.onnx`

**Input size requirement:** `tract` expects frame dimensions to match the model
input. Set `rtsp.width/height` or `v4l2.width/height` to the model input size.

**Thresholds:** the tract backend currently uses a fixed confidence threshold of
`0.5` (no CLI/config override yet).

## ESP32-S3 Camera Setup (HTTP MJPEG/JPEG or UDP RTP)

### Quick Start with ESP32-S3

1. Build with ESP32-S3 support:
   ```bash
   cargo build --release --features ingest-esp32
   ```

2. Configure your ESP32-S3 stream (create `witness.toml`):
   ```toml
   [ingest]
   backend = "esp32"

   [esp32]
   url = "http://192.168.1.50:81/stream"
   target_fps = 10

   [zones]
   module_zone_id = "zone:front_door"
   ```

3. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features ingest-esp32 --bin witnessd
   ```

See `docs/esp32_s3_setup.md` for supported ESP32-S3 URL patterns and RTP expectations.

## Container deployment

See `docs/container.md` for building and running the containerized `witnessd`
artifact with RTSP configuration and Event API-only exposure.
