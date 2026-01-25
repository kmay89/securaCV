# V4L2 Camera Setup Guide

This guide covers connecting the Privacy Witness Kernel to local USB/V4L2 cameras on Linux.
The V4L2 backend runs entirely in-memory and preserves the same invariants as RTSP ingestion:
timestamp coarsening, non-invertible feature hashing, and zero raw-frame persistence.

## Build with V4L2 Support

The V4L2 backend is gated behind the `ingest-v4l2` feature:

```bash
cargo build --release --features ingest-v4l2
```

## Configure `witnessd`

Create a `witness.toml` with the V4L2 backend selected:

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

For testing without a physical camera, use the synthetic stub source:

```toml
[v4l2]
device = "stub://test"
```

Run the daemon:

```bash
export DEVICE_KEY_SEED=$(openssl rand -hex 32)
WITNESS_CONFIG=witness.toml cargo run --release --features ingest-v4l2 --bin witnessd
```

## Invariant Notes

- Raw frames remain in-memory only and are dropped immediately after inference.
- No raw frames are written to disk or exposed over the network.
- Time buckets are coarsened at capture time (`TimeBucket::now_10min`).
- Feature hashes are lossy and intentionally unstable across frames.

For the canonical invariants and threat model, see `spec/invariants.md` and `spec/threat_model.md`.
