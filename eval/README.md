# Perception Eval Harness

The kernel's cryptography proves that a sealed event was **not forged** (hash chains +
signatures). This harness measures the other half of trustworthiness: whether the detector
**actually witnessed the event in the first place**, and at what false-alarm rate.

It scores any `DetectorBackend` against a labeled dataset and reports:

- **precision / recall / F1** per class and micro-averaged,
- **average precision (AP)** per class and **mAP**,
- a **confidence-threshold sweep** — the data-driven alternative to the kernel's hardcoded
  `0.5` default,
- a **size-class confusion** matrix (a wrong size = a wrong emitted event:
  `BoundaryCrossingObjectLarge` vs `…Small`),
- **per-frame latency** (mean / p50 / p95 / p99) for on-device feasibility,
- a plain-language **witness summary**: "witnessed X% of labeled events, false alarms on Y% of
  detections."

## Build

The harness is behind the `detect-eval` feature (pulls in `backend-tract` + `image`):

```bash
cargo build --features detect-eval --bin detect_eval
```

## Quick run (synthetic sample)

```bash
cargo run --features detect-eval --bin detect_eval -- \
    --model tests/fixtures/test_detector.onnx --format postnms \
    --dataset eval/datasets/sample --width 4 --height 4 \
    --json report.json
```

`eval/datasets/sample/` is **fully synthetic** (colored boxes on noise) and contains **no real
imagery** — the eval corpus must itself honor the project's privacy premise. Regenerate it with
the stdlib-only script (no Pillow/numpy needed):

```bash
python3 eval/datasets/sample/generate.py
```

> The deterministic `test_detector.onnx` ignores pixels and emits fixed boxes, so it is **not**
> expected to score well on the synthetic frames. That run is a plumbing smoke test; the exact
> numeric correctness check is `tests/detect_eval_integration.rs`.

## Real measurement

For a meaningful number, fetch the real model and point the harness at your **own,
locally-held** labeled dataset (kept out of the repo for privacy):

```bash
bash scripts/fetch_detection_model.sh          # one-time, operator-initiated (tiny-YOLOv2)
cargo run --features detect-eval --bin detect_eval -- \
    --model vendor/models/tinyyolov2-8.onnx --format yolov2 \
    --dataset /path/to/your/dataset \
    --confidence-sweep 0.1:0.9:0.1 \
    --json report.json
```

> **HOST-ONLY:** the tract detector runs in `witnessd` on a Pi/x86 host. It does **not** run on
> the ESP32-S3 — that path uses the Grove Vision AI V2 (SSCMA) board for on-device inference
> (`firmware/projects/canary-vision`). A 60 MB ONNX model cannot fit on an MCU, and tract does
> not target microcontrollers.
>
> **Why tiny-YOLOv2:** the TF-exported SSD/SSDLite models use ONNX `Loop` ops for in-graph NMS
> that tract does not implement, so tract cannot run them. tiny-YOLOv2 emits a raw grid that is
> decoded + NMS'd on the host (`--format yolov2`, the default), and tract runs it cleanly.

## Dataset format

A directory with a `labels.json` manifest plus the referenced images. Boxes are **normalized
0..1** `x, y, w, h` — identical to `detect::Detection`, so there is no coordinate drift between
ground truth and predictions.

```json
{
  "frames": [
    {
      "image": "frame_0001.png",
      "objects": [
        { "class": "person", "x": 0.25, "y": 0.20, "w": 0.50, "h": 0.60 }
      ],
      "expected_size_class": "large"
    }
  ]
}
```

- `class` ∈ `person | vehicle | animal | package | unknown`
- `expected_size_class` (optional) ∈ `small | large | unknown` — feeds the size-class matrix.

## CLI flags

| flag | default | meaning |
|------|---------|---------|
| `--model` | `vendor/models/tinyyolov2-8.onnx` | ONNX model path |
| `--dataset` | (required) | dataset directory |
| `--format` | `yolov2` | `yolov2` (raw grid decoded on host) or `postnms` (model emits final boxes) |
| `--width` / `--height` | `416` | input resolution (PostNms only; YOLOv2 has a fixed 416×416 input) |
| `--iou-threshold` | `0.5` | IoU for matching a prediction to ground truth |
| `--threshold` | `0.5` | operating confidence for headline metrics |
| `--confidence-sweep` | `0.05:0.95:0.05` | `start:end:step` sweep |
| `--json` | — | write the full report as JSON |
| `--min-recall` / `--min-precision` | — | fail (non-zero exit) below this value — use as a CI gate |

## Scope / future work

- **Frame-level** detection + size class only. Temporal/zone-crossing **event-sequence** eval
  (does a sequence of frames produce the right `BoundaryCrossing…` event over time?) is not yet
  covered and is the natural next step.
- On-device latency should be measured on the real target (Raspberry Pi), not just CI runners.
