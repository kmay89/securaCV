# Demo: end-to-end witness loop

## Purpose

The demo exercises a complete, local witness loop: synthetic frames/events are
recorded, the log and vault are sealed, an export bundle is produced, and the
results are verified. It is meant as a stable “hello world” path for the
Privacy Witness Kernel without changing any invariants.

## Commands

1) Run the demo:

```bash
cargo run --bin demo
```

2) Verify the log independently:

```bash
cargo run --bin log_verify -- --db demo_witness.db
```

## Real input (file or RTSP)

Use these when you want to drive the demo from a real video source.

**Local file ingest (requires `ingest-file-ffmpeg` feature):**

```bash
cargo run --features ingest-file-ffmpeg --bin demo -- \
  --file /path/to/video.mp4
```

**RTSP ingest (requires `rtsp-gstreamer` or `rtsp-ffmpeg` feature):**

```bash
cargo run --features rtsp-gstreamer --bin demo -- \
  --rtsp rtsp://user:pass@camera.example/stream
```

3) Optional (not needed for the demo): export from the DB with `export_events`
if you already have a break-glass token (issued via the policy → request →
approve → authorize flow):

```bash
DEVICE_KEY_SEED=devkey:demo \
  cargo run --bin export_events -- \
  --db-path demo_witness.db \
  --break-glass-token /path/to/break_glass.token \
  --output demo_out/export_bundle.json
```

## Tract backend demo (ONNX, optional)

The tract backend requires a **local** ONNX model and a build with the
`backend-tract` feature enabled.

1) Download the recommended model (Apache-2.0 licensed, small/CPU-friendly):

```bash
mkdir -p vendor/models
curl -L \
  https://github.com/onnx/models/raw/main/vision/object_detection_segmentation/ssdlite_mobilenet_v2/model/ssdlite_mobilenet_v2_12.onnx \
  -o vendor/models/ssdlite_mobilenet_v2_12.onnx
echo "ad6303f1ca2c3dcc0d86a87c36892be9b97b02a0105faa5cc3cfae79a2b11a31  vendor/models/ssdlite_mobilenet_v2_12.onnx" | sha256sum -c -
```

2) Run the demo with the tract backend:

```bash
cargo run --features backend-tract --bin demo -- \
  --backend tract \
  --tract-model vendor/models/ssdlite_mobilenet_v2_12.onnx
```

**Exact CLI fields required for tract in the demo:**

- `--backend tract` (select the backend)
- `--tract-model <path>` (local ONNX model path)
- **Input size**: the demo feeds frames at **320x320**; the model must accept
  `1x3x320x320` RGB inputs.
- **Thresholds**: the tract backend uses a fixed confidence threshold of **0.5**
  (not configurable via CLI today).

## Expected artifacts

After a successful run you should see:

- `./demo_witness.db` (sealed log + receipts)
- `./vault/envelopes/` (or your `--vault` path)
- `./demo_out/export_bundle.json` (export bundle)

Example summary output (values may vary):

```
demo summary:
  input: synthetic (stub://demo)
  events written: 42
  log db: demo_witness.db
  vault path: vault/envelopes
  export bundle: demo_out/export_bundle.json
  verify: OK
```

## Tamper test (manual)

1. Run the demo once to generate artifacts.
2. Flip a byte in the export bundle:

```bash
python - <<'PY'
from pathlib import Path
path = Path("demo_out/export_bundle.json")
data = bytearray(path.read_bytes())
data[len(data) // 2] ^= 0xFF
path.write_bytes(data)
print("tampered", path)
PY
```

3. Re-run the demo in verify-only mode (no new events):

```bash
cargo run --bin demo -- --seconds 0 --fps 1
```

The demo should report `verify: FAIL` and exit non-zero.

## Troubleshooting

- If you see permission errors, choose a writable `--vault` or `--out` path.
- If verification fails unexpectedly, ensure you are using the original
  `demo_out/export_bundle.json` and `demo_witness.db` from the same run.
- If `demo_out/export_bundle.json` is missing, rerun the demo to regenerate it.
- If you need deterministic artifacts for debugging, pass `--seed`.
- For real RTSP streams, ensure GStreamer dependencies are installed and use the
  `rtsp-gstreamer` feature (see README).
