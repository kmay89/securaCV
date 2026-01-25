# ESP32-S3 Camera Setup Guide

This guide covers connecting the Privacy Witness Kernel to ESP32-S3 camera modules that
stream MJPEG/JPEG over HTTP or JPEG over RTP/UDP. The ESP32 ingestion backend runs
entirely in-memory and preserves the same invariants as RTSP/V4L2 ingestion: timestamp
coarsening, non-invertible feature hashing, and zero raw-frame persistence.

For Seeed XIAO Vision AI devices, see the dedicated guide:
[`docs/seeed_xiao_vision_ai_setup.md`](seeed_xiao_vision_ai_setup.md).

## Build with ESP32-S3 Support

The ESP32-S3 backend is gated behind the `ingest-esp32` feature:

```bash
cargo build --release --features ingest-esp32
```

## Configure `witnessd`

Create a `witness.toml` with the ESP32 backend selected:

```toml
[ingest]
backend = "esp32"

[esp32]
url = "http://192.168.1.50:81/stream"
target_fps = 10

[zones]
module_zone_id = "zone:front_door"
```

Run the daemon:

```bash
export DEVICE_KEY_SEED=$(openssl rand -hex 32)
WITNESS_CONFIG=witness.toml cargo run --release --features ingest-esp32 --bin witnessd
```

## Supported ESP32-S3 Camera Configurations

### HTTP MJPEG stream

Common ESP32-S3 MJPEG URL patterns:

- `http://<camera-ip>:81/stream`
- `http://<camera-ip>/stream`

### HTTP JPEG snapshot

Common ESP32-S3 JPEG snapshot URL patterns:

- `http://<camera-ip>/capture`
- `http://<camera-ip>/jpg`

### UDP RTP (JPEG payload)

The ESP32 backend listens on a local UDP port for incoming RTP packets that contain
JPEG payloads (payload type 26). Configure the ESP32-S3 firmware to send RTP/JPEG to
the host running `witnessd` and bind the listener using a URL like:

```toml
[esp32]
url = "udp://0.0.0.0:5004"
```

The adapter expects:

- RTP version 2 packets
- Payload type 26 (JPEG)
- Marker bit set on the final packet of each JPEG frame
- Payload bytes containing a complete JPEG bitstream (concatenated if fragmented)

## Invariant Notes

- Raw frames remain in-memory only and are dropped immediately after inference.
- No raw frames are written to disk or exposed over the network.
- Time buckets are coarsened at capture time (`TimeBucket::now_10min`).
- Feature hashes are lossy and intentionally unstable across frames.

For the canonical invariants and threat model, see `spec/invariants.md` and
`spec/threat_model.md`.
