//! YOLOv2 output decoding, non-max suppression, and class mapping — the pure, host-side
//! post-processing for the tiny-YOLOv2 ONNX detector.
//!
//! tiny-YOLOv2 (ONNX model zoo, VOC) emits a raw grid tensor `[1, B*(5+C), G, G]` (no in-graph
//! NMS — which is exactly why tract can run it, unlike the TF-SSD family whose `Loop`-based NMS
//! tract does not implement). This module turns that grid into normalized [`Detection`]s and
//! suppresses duplicates.
//!
//! HOST-ONLY: this runs in `witnessd` on a Pi/x86 host via tract. It does NOT run on the
//! ESP32-S3 — that path uses the Grove Vision AI V2 (SSCMA) board for on-device inference. See
//! `firmware/projects/canary-vision`.
//!
//! Kept free of tract/model/image dependencies so the math is unit-tested in a plain
//! `cargo test`.

use crate::detect::result::{Detection, ObjectClass};

/// Number of anchor boxes per grid cell in YOLOv2.
pub const NUM_ANCHORS: usize = 5;

/// tiny-YOLOv2 (VOC) anchor box dimensions, in grid-cell units. From the canonical model.
pub const TINY_YOLOV2_VOC_ANCHORS: [(f32, f32); NUM_ANCHORS] = [
    (1.08, 1.19),
    (3.42, 4.41),
    (6.63, 11.38),
    (9.42, 5.11),
    (16.62, 10.52),
];

/// Number of VOC classes tiny-YOLOv2 predicts.
pub const VOC_NUM_CLASSES: usize = 20;

/// Decoding spec for a YOLOv2 grid output.
#[derive(Clone, Debug)]
pub struct YoloV2Spec {
    pub anchors: Vec<(f32, f32)>,
    pub num_classes: usize,
}

impl YoloV2Spec {
    /// The tiny-YOLOv2 VOC configuration (5 anchors, 20 classes).
    pub fn tiny_yolov2_voc() -> Self {
        Self {
            anchors: TINY_YOLOV2_VOC_ANCHORS.to_vec(),
            num_classes: VOC_NUM_CLASSES,
        }
    }

    /// Channels per grid cell: `anchors * (5 box/obj values + num_classes)`.
    pub fn channels(&self) -> usize {
        self.anchors.len() * (5 + self.num_classes)
    }
}

/// Map a VOC class index (tiny-YOLOv2 ordering) to the kernel's coarse [`ObjectClass`].
///
/// VOC order: aeroplane, bicycle, bird, boat, bottle, bus, car, cat, chair, cow, diningtable,
/// dog, horse, motorbike, person, pottedplant, sheep, sofa, train, tvmonitor.
pub fn voc20_to_object_class(idx: usize) -> ObjectClass {
    match idx {
        14 => ObjectClass::Person,
        // aeroplane, bicycle, boat, bus, car, motorbike, train
        0 | 1 | 3 | 5 | 6 | 13 | 18 => ObjectClass::Vehicle,
        // bird, cat, cow, dog, horse, sheep
        2 | 7 | 9 | 11 | 12 | 16 => ObjectClass::Animal,
        // bottle, chair, diningtable, pottedplant, sofa, tvmonitor (and anything else)
        _ => ObjectClass::Unknown,
    }
}

fn sigmoid(x: f32) -> f32 {
    1.0 / (1.0 + (-x).exp())
}

/// Numerically stable softmax over a slice (returns a new Vec).
fn softmax(logits: &[f32]) -> Vec<f32> {
    let max = logits.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
    let exps: Vec<f32> = logits.iter().map(|v| (v - max).exp()).collect();
    let sum: f32 = exps.iter().sum();
    if sum <= 0.0 {
        return vec![0.0; logits.len()];
    }
    exps.into_iter().map(|e| e / sum).collect()
}

/// Decode a YOLOv2 grid output (`[1, B*(5+C), G, G]`, NCHW, row-major) into normalized
/// detections (top-left `x,y` + `w,h` in 0..1), keeping those at/above `conf_threshold`.
///
/// `data` is the flat output tensor; `grid_w`/`grid_h` are the grid dimensions (13×13 for
/// tiny-YOLOv2 at 416×416). Detections are NOT yet NMS-suppressed.
pub fn decode_yolov2(
    data: &[f32],
    grid_w: usize,
    grid_h: usize,
    spec: &YoloV2Spec,
    conf_threshold: f32,
) -> Vec<Detection> {
    let b = spec.anchors.len();
    let c = spec.num_classes;
    let per_anchor = 5 + c;
    let plane = grid_w * grid_h;
    let expected = b * per_anchor * plane;
    if data.len() != expected {
        // Caller passed a mismatched tensor; refuse to misread it.
        return Vec::new();
    }

    let mut out = Vec::new();
    // Channel `ch` at cell (cx, cy): index = ch * plane + cy * grid_w + cx (NCHW, batch 1).
    let at = |ch: usize, cx: usize, cy: usize| data[ch * plane + cy * grid_w + cx];

    for cy in 0..grid_h {
        for cx in 0..grid_w {
            for (ai, &(anchor_w, anchor_h)) in spec.anchors.iter().enumerate() {
                let base = ai * per_anchor;
                let tx = at(base, cx, cy);
                let ty = at(base + 1, cx, cy);
                let tw = at(base + 2, cx, cy);
                let th = at(base + 3, cx, cy);
                let to = at(base + 4, cx, cy);

                let objectness = sigmoid(to);
                if objectness < conf_threshold {
                    // Cheap early-out: class score can only lower this.
                    continue;
                }

                let logits: Vec<f32> = (0..c).map(|k| at(base + 5 + k, cx, cy)).collect();
                let probs = softmax(&logits);
                let (best, &best_p) = probs
                    .iter()
                    .enumerate()
                    .max_by(|a, b| a.1.total_cmp(b.1))
                    .unwrap_or((0, &0.0));
                let score = objectness * best_p;
                if score < conf_threshold {
                    continue;
                }

                // Center in grid units → normalized 0..1.
                let bx = (cx as f32 + sigmoid(tx)) / grid_w as f32;
                let by = (cy as f32 + sigmoid(ty)) / grid_h as f32;
                let bw = (anchor_w * tw.exp()) / grid_w as f32;
                let bh = (anchor_h * th.exp()) / grid_h as f32;

                let x = (bx - bw / 2.0).clamp(0.0, 1.0);
                let y = (by - bh / 2.0).clamp(0.0, 1.0);
                let w = bw.clamp(0.0, 1.0 - x);
                let h = bh.clamp(0.0, 1.0 - y);
                if w <= 0.0 || h <= 0.0 {
                    continue;
                }

                out.push(Detection {
                    x,
                    y,
                    w,
                    h,
                    confidence: score,
                    class: voc20_to_object_class(best),
                });
            }
        }
    }
    out
}

fn iou(a: &Detection, b: &Detection) -> f32 {
    let ax2 = a.x + a.w;
    let ay2 = a.y + a.h;
    let bx2 = b.x + b.w;
    let by2 = b.y + b.h;
    let ix1 = a.x.max(b.x);
    let iy1 = a.y.max(b.y);
    let ix2 = ax2.min(bx2);
    let iy2 = ay2.min(by2);
    let iw = (ix2 - ix1).max(0.0);
    let ih = (iy2 - iy1).max(0.0);
    let inter = iw * ih;
    let union = a.w * a.h + b.w * b.h - inter;
    if union <= 0.0 {
        0.0
    } else {
        inter / union
    }
}

/// Greedy per-class non-max suppression: keep the highest-confidence box, drop same-class boxes
/// overlapping it by more than `iou_threshold`, repeat.
pub fn nms(mut dets: Vec<Detection>, iou_threshold: f32) -> Vec<Detection> {
    dets.sort_by(|a, b| b.confidence.total_cmp(&a.confidence));
    let mut keep: Vec<Detection> = Vec::new();
    for d in dets {
        let suppressed = keep
            .iter()
            .any(|k| k.class == d.class && iou(k, &d) > iou_threshold);
        if !suppressed {
            keep.push(d);
        }
    }
    keep
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_grid(spec: &YoloV2Spec, g: usize) -> Vec<f32> {
        vec![0.0; spec.channels() * g * g]
    }

    fn set(data: &mut [f32], g: usize, ch: usize, cx: usize, cy: usize, v: f32) {
        data[ch * g * g + cy * g + cx] = v;
    }

    #[test]
    fn voc_mapping_is_correct() {
        assert_eq!(voc20_to_object_class(14), ObjectClass::Person);
        assert_eq!(voc20_to_object_class(6), ObjectClass::Vehicle); // car
        assert_eq!(voc20_to_object_class(1), ObjectClass::Vehicle); // bicycle
        assert_eq!(voc20_to_object_class(11), ObjectClass::Animal); // dog
        assert_eq!(voc20_to_object_class(8), ObjectClass::Unknown); // chair
    }

    #[test]
    fn decodes_single_centered_person() {
        let spec = YoloV2Spec::tiny_yolov2_voc();
        let g = 13;
        let mut data = empty_grid(&spec, g);
        // Box at cell (6,6), anchor 0 (channel base 0): high objectness, person logit high.
        let base = 0;
        set(&mut data, g, base + 4, 6, 6, 10.0); // objectness logit (sigmoid≈1)
        set(&mut data, g, base + 5 + 14, 6, 6, 10.0); // person logit dominates softmax

        let dets = decode_yolov2(&data, g, g, &spec, 0.5);
        assert_eq!(dets.len(), 1, "exactly one detection expected");
        let d = &dets[0];
        assert_eq!(d.class, ObjectClass::Person);
        assert!(d.confidence > 0.9, "confidence {}", d.confidence);
        // Center ≈ (6.5/13, 6.5/13) = 0.5; box size from anchor 0 (1.08,1.19)/13.
        let cx = d.x + d.w / 2.0;
        let cy = d.y + d.h / 2.0;
        assert!((cx - 0.5).abs() < 0.02, "center x {cx}");
        assert!((cy - 0.5).abs() < 0.02, "center y {cy}");
        assert!((d.w - 1.08 / 13.0).abs() < 0.01, "w {}", d.w);
    }

    #[test]
    fn threshold_filters_weak_detections() {
        let spec = YoloV2Spec::tiny_yolov2_voc();
        let g = 13;
        let mut data = empty_grid(&spec, g);
        // Weak objectness (sigmoid(-1)≈0.27) → below a 0.5 threshold.
        set(&mut data, g, 4, 3, 3, -1.0);
        set(&mut data, g, 5 + 14, 3, 3, 10.0);
        assert!(decode_yolov2(&data, g, g, &spec, 0.5).is_empty());
    }

    #[test]
    fn mismatched_tensor_yields_nothing() {
        let spec = YoloV2Spec::tiny_yolov2_voc();
        assert!(decode_yolov2(&[0.0; 10], 13, 13, &spec, 0.5).is_empty());
    }

    #[test]
    fn nms_suppresses_overlapping_same_class() {
        let strong = Detection {
            x: 0.10,
            y: 0.10,
            w: 0.30,
            h: 0.30,
            confidence: 0.9,
            class: ObjectClass::Person,
        };
        let dup = Detection {
            x: 0.12,
            y: 0.12,
            w: 0.30,
            h: 0.30,
            confidence: 0.7,
            class: ObjectClass::Person,
        };
        // A different class at the same spot must survive.
        let other = Detection {
            class: ObjectClass::Vehicle,
            ..dup.clone()
        };
        let kept = nms(vec![strong.clone(), dup, other], 0.5);
        assert_eq!(kept.len(), 2);
        assert!((kept[0].confidence - 0.9).abs() < 1e-6);
    }

    #[test]
    fn nms_keeps_distant_boxes() {
        let a = Detection {
            x: 0.0,
            y: 0.0,
            w: 0.1,
            h: 0.1,
            confidence: 0.9,
            class: ObjectClass::Person,
        };
        let b = Detection {
            x: 0.7,
            y: 0.7,
            w: 0.1,
            h: 0.1,
            confidence: 0.8,
            class: ObjectClass::Person,
        };
        assert_eq!(nms(vec![a, b], 0.5).len(), 2);
    }
}
