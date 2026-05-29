# Operator Guide (CLI & Ingest Reference)

Deep reference for operators and developers. The top-level [README](../README.md) covers the
consumer/Home-Assistant path; this guide holds the command-line workflows and the per-source
ingest setup that used to live inline in the README.

See also: [`docs/rtsp_setup.md`](rtsp_setup.md), [`docs/v4l2_setup.md`](v4l2_setup.md),
[`docs/esp32_s3_setup.md`](esp32_s3_setup.md), [`docs/container.md`](container.md),
[`docs/homeassistant_setup.md`](homeassistant_setup.md), [`docs/frigate_integration.md`](frigate_integration.md).

---

## Demo & tamper proof

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
```

The demo writes `./demo_witness.db`, `./demo_out/export_bundle.json`, and the vault directory
at `./vault/envelopes` (override with `--out` / `--vault`).

### Tamper demo

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json
printf "\n" >> demo_out/export_bundle.json
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json  # should FAIL
```

## Device public key location

The device Ed25519 **verifying key** is stored locally in the witness database at
`device_metadata.public_key` (row `id = 1`). External verification tools like `log_verify` read
this public key from the database by default, or accept an explicit key via `--public-key` /
`--public-key-file` as documented in `log_verify_README.md`.

## Break-glass policy management

The break-glass CLI stores the quorum policy in the kernel database so `break_glass authorize`
can evaluate approvals against a persistent trustee roster.

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

Trustee entries use the format `id:HEX_PUBLIC_KEY`, where the public key is the hex-encoded
32-byte Ed25519 verifying key.

## Break-glass unseal workflow

Ensure a quorum policy is stored first (`break_glass policy set`). Then create an unlock request,
gather trustee approvals, and authorize the request before unsealing. The authorization step logs
a receipt (granted or denied) and issues a sensitive token file via `--output-token`. The unseal
command writes the clear envelope to `--output-dir` (default: `vault/unsealed`).

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

cargo run --bin break_glass -- unseal \
  --envelope <envelope_id> \
  --token /path/to/break_glass.token \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --vault-path vault/envelopes \
  --output-dir vault/unsealed
```

## Event export

Write a local artifact with coarse time buckets and batched events (no precise timestamps or
identity selectors):

Export is gated on a break-glass authorization: `--break-glass-token` is required and takes the
token file produced by the break-glass `authorize` step above (`--output-token`).

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- \
  --db-path witness.db \
  --break-glass-token /path/to/break_glass.token \
  --output witness_export.json
```

`export_events` emits a single JSON artifact with batched buckets, applying default jitter and
batching unless overridden by CLI flags.

---

## RTSP camera setup

1. Install RTSP dependencies (GStreamer or FFmpeg):
   ```bash
   # GStreamer backend
   sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
       gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libseccomp-dev
   # OR FFmpeg backend
   sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libseccomp-dev
   ```
2. Build with RTSP support:
   ```bash
   cargo build --release --features rtsp-gstreamer   # or rtsp-ffmpeg
   ```
3. Configure your camera (`witness.toml`):
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
4. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features rtsp-gstreamer --bin witnessd
   ```

See [`docs/rtsp_setup.md`](rtsp_setup.md) for camera URL patterns and troubleshooting.

**Architecture:** `witnessd` decodes RTSP in-memory and produces `RawFrame` values without
writing frames to disk. Time coarsening and non-invertible feature hashing happen at capture
time. GStreamer is gated behind `rtsp-gstreamer`, FFmpeg behind `rtsp-ffmpeg`; `auto` prefers
GStreamer. The `stub://` scheme is test/dev only and is rejected in release builds.

## V4L2 camera setup (USB / local devices)

1. Build: `cargo build --release --features ingest-v4l2`
2. Configure (`witness.toml`):
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

See [`docs/v4l2_setup.md`](v4l2_setup.md). The V4L2 backend never writes raw frames to disk and
never exposes them over the network.

## ESP32-S3 camera setup (HTTP MJPEG/JPEG or UDP RTP)

1. Build: `cargo build --release --features ingest-esp32`
2. Configure (`witness.toml`):
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

See [`docs/esp32_s3_setup.md`](esp32_s3_setup.md).

## Tract (ONNX) detection backend

Local ONNX inference for object detection. Feature-gated (`backend-tract`); requires a **local**
model file and explicit width/height (RTSP or V4L2 ingest only).

Recommended small model (Apache-2.0):

```bash
mkdir -p vendor/models
curl -L \
  https://github.com/onnx/models/raw/main/vision/object_detection_segmentation/ssdlite_mobilenet_v2/model/ssdlite_mobilenet_v2_12.onnx \
  -o vendor/models/ssdlite_mobilenet_v2_12.onnx
echo "ad6303f1ca2c3dcc0d86a87c36892be9b97b02a0105faa5cc3cfae79a2b11a31  vendor/models/ssdlite_mobilenet_v2_12.onnx" | sha256sum -c -
```

```toml
[detect]
backend = "tract"
tract_model = "vendor/models/ssdlite_mobilenet_v2_12.onnx"

[rtsp]      # or [v4l2]
width = 320
height = 320
```

Environment overrides: `WITNESS_DETECT_BACKEND=tract`,
`WITNESS_TRACT_MODEL=/absolute/path/to/model.onnx`. Frame dimensions must match the model input.
The tract backend currently uses a fixed confidence threshold of `0.5` (no override yet).

## Container deployment

See [`docs/container.md`](container.md) for building and running the containerized `witnessd`
artifact with RTSP configuration and Event-API-only exposure.

## Home Assistant entity reference

Full sensor/binary-sensor/service catalog (kernel sensors, Canary MQTT sensors, per-tamper-type
sensors, multi-transport status, MQTT Discovery details) lives in
[`docs/homeassistant_setup.md`](homeassistant_setup.md).
