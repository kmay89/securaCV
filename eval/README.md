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
    --model tests/fixtures/test_detector.onnx \
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
bash scripts/fetch_detection_model.sh          # one-time, operator-initiated
cargo run --features detect-eval --bin detect_eval -- \
    --model vendor/models/ssdlite_mobilenet_v2_12.onnx \
    --dataset /path/to/your/dataset \
    --confidence-sweep 0.1:0.9:0.1 \
    --json report.json
```

> **Known caveat (this is exactly what the harness surfaces):** `TractBackend::map_class_id`
> currently hardcodes `0→Person, 1→Vehicle, 2→Animal, 3→Package`, but the pinned SSDLite model
> is COCO-trained (different class IDs and I/O layout). Use this harness to *measure* whether the
> default model maps correctly; fixing the mapping is a separate follow-up.

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
| `--model` | `vendor/models/ssdlite_mobilenet_v2_12.onnx` | ONNX model path |
| `--dataset` | (required) | dataset directory |
| `--width` / `--height` | `300` | model input resolution (images are resized to match) |
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
