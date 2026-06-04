//! Pure detection-quality metrics for the perception eval harness.
//!
//! This module is intentionally free of any model, image, or feature-gated dependency so the
//! metric math can be unit-tested with a plain `cargo test` against hand-computed values. The
//! eval *binary* and *runner* (feature `detect-eval`) feed real detections in; everything here
//! operates on plain structs.
//!
//! Why this exists: the kernel proves the *integrity* of every event (hash chains, signatures)
//! but, before this harness, never measured the *truth* of the perception underneath. These
//! metrics answer the safety-critical question "what fraction of real events does the detector
//! actually witness, and at what false-alarm rate?"

use serde::Serialize;

use crate::detect::{ObjectClass, SizeClass};

/// The object classes the kernel can emit events for. `Unknown` is deliberately excluded from
/// per-class scoring (it is never a valid witnessed event), but predictions tagged `Unknown`
/// still count as false positives against real classes during matching.
pub const EVAL_CLASSES: [ObjectClass; 4] = [
    ObjectClass::Person,
    ObjectClass::Vehicle,
    ObjectClass::Animal,
    ObjectClass::Package,
];

/// Upper bound on confidence-sweep points, guarding against a degenerate `step` that never
/// advances (which would otherwise loop forever — see [`parse_sweep_spec`]).
pub const MAX_SWEEP_STEPS: usize = 100_000;

/// Box-area threshold above which an object is "Large". Mirrors `TractBackend`'s
/// `LARGE_AREA_THRESHOLD` so the eval's size-class outcome matches what the kernel would emit.
pub const LARGE_AREA_THRESHOLD: f32 = 0.2;

/// Stable, lowercase name for an [`ObjectClass`] (used in reports and dataset labels).
pub fn class_name(class: ObjectClass) -> &'static str {
    match class {
        ObjectClass::Person => "person",
        ObjectClass::Vehicle => "vehicle",
        ObjectClass::Animal => "animal",
        ObjectClass::Package => "package",
        ObjectClass::Unknown => "unknown",
    }
}

/// Stable, lowercase name for a [`SizeClass`].
pub fn size_class_name(size: SizeClass) -> &'static str {
    match size {
        SizeClass::Unknown => "unknown",
        SizeClass::Small => "small",
        SizeClass::Large => "large",
    }
}

/// Normalized bounding box (coordinates in 0..1), matching `detect::Detection` geometry.
#[derive(Clone, Copy, Debug)]
pub struct EvalBox {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl EvalBox {
    pub fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Self { x, y, w, h }
    }

    fn area(&self) -> f32 {
        self.w.max(0.0) * self.h.max(0.0)
    }
}

/// Intersection-over-union of two normalized boxes. Returns 0.0 for non-overlapping or
/// degenerate boxes.
pub fn iou(a: &EvalBox, b: &EvalBox) -> f32 {
    let (ax2, ay2) = (a.x + a.w, a.y + a.h);
    let (bx2, by2) = (b.x + b.w, b.y + b.h);
    let ix1 = a.x.max(b.x);
    let iy1 = a.y.max(b.y);
    let ix2 = ax2.min(bx2);
    let iy2 = ay2.min(by2);
    let iw = (ix2 - ix1).max(0.0);
    let ih = (iy2 - iy1).max(0.0);
    let inter = iw * ih;
    let union = a.area() + b.area() - inter;
    if union <= 0.0 {
        0.0
    } else {
        inter / union
    }
}

/// A ground-truth object in a frame.
#[derive(Clone, Copy, Debug)]
pub struct GtObject {
    pub class: ObjectClass,
    pub bbox: EvalBox,
}

/// A predicted detection in a frame.
#[derive(Clone, Copy, Debug)]
pub struct Prediction {
    pub class: ObjectClass,
    pub bbox: EvalBox,
    pub confidence: f32,
}

/// One evaluated frame: its ground truth and the detector's predictions.
#[derive(Clone, Debug, Default)]
pub struct FrameSample {
    pub gts: Vec<GtObject>,
    pub preds: Vec<Prediction>,
    /// Per-frame size-class outcome (expected from labels, predicted by the backend). Both must
    /// be present to contribute to the size-class confusion matrix.
    pub expected_size: Option<SizeClass>,
    pub predicted_size: Option<SizeClass>,
}

/// True/false positive and false negative tallies.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
pub struct Counts {
    pub true_positives: u32,
    pub false_positives: u32,
    pub false_negatives: u32,
}

impl Counts {
    fn add(&mut self, other: Counts) {
        self.true_positives += other.true_positives;
        self.false_positives += other.false_positives;
        self.false_negatives += other.false_negatives;
    }
}

pub fn precision(c: &Counts) -> f32 {
    let denom = c.true_positives + c.false_positives;
    if denom == 0 {
        0.0
    } else {
        c.true_positives as f32 / denom as f32
    }
}

pub fn recall(c: &Counts) -> f32 {
    let denom = c.true_positives + c.false_negatives;
    if denom == 0 {
        0.0
    } else {
        c.true_positives as f32 / denom as f32
    }
}

pub fn f1(precision: f32, recall: f32) -> f32 {
    if precision + recall <= 0.0 {
        0.0
    } else {
        2.0 * precision * recall / (precision + recall)
    }
}

/// Greedy match of one frame's predictions (already filtered + sorted by confidence desc)
/// against its ground truth for a single class. Highest-IoU unmatched GT wins.
fn match_frame_class(gts: &[EvalBox], preds_sorted: &[EvalBox], iou_threshold: f32) -> Counts {
    let mut matched = vec![false; gts.len()];
    let mut counts = Counts::default();
    for pred in preds_sorted {
        let mut best_iou = iou_threshold;
        let mut best_j: Option<usize> = None;
        for (j, gt) in gts.iter().enumerate() {
            if matched[j] {
                continue;
            }
            let v = iou(pred, gt);
            if v >= best_iou {
                best_iou = v;
                best_j = Some(j);
            }
        }
        match best_j {
            Some(j) => {
                matched[j] = true;
                counts.true_positives += 1;
            }
            None => counts.false_positives += 1,
        }
    }
    counts.false_negatives = matched.iter().filter(|m| !**m).count() as u32;
    counts
}

/// Counts for a single class across all frames at a fixed confidence threshold.
pub fn counts_at_threshold(
    frames: &[FrameSample],
    class: ObjectClass,
    conf_threshold: f32,
    iou_threshold: f32,
) -> Counts {
    let mut total = Counts::default();
    for frame in frames {
        let gts: Vec<EvalBox> = frame
            .gts
            .iter()
            .filter(|g| g.class == class)
            .map(|g| g.bbox)
            .collect();
        let mut preds: Vec<(f32, EvalBox)> = frame
            .preds
            .iter()
            .filter(|p| p.class == class && p.confidence >= conf_threshold)
            .map(|p| (p.confidence, p.bbox))
            .collect();
        preds.sort_by(|a, b| b.0.total_cmp(&a.0));
        let boxes: Vec<EvalBox> = preds.into_iter().map(|(_, b)| b).collect();
        total.add(match_frame_class(&gts, &boxes, iou_threshold));
    }
    total
}

/// Micro-averaged counts (summed across the supplied classes) at a fixed threshold.
pub fn micro_counts_at_threshold(
    frames: &[FrameSample],
    classes: &[ObjectClass],
    conf_threshold: f32,
    iou_threshold: f32,
) -> Counts {
    let mut total = Counts::default();
    for &class in classes {
        total.add(counts_at_threshold(
            frames,
            class,
            conf_threshold,
            iou_threshold,
        ));
    }
    total
}

/// Count predictions tagged `Unknown` at/above the threshold. An `Unknown` detection can never
/// match real-class ground truth, so each is an unconditional false alarm. Counting them keeps
/// the harness honest when a model emits classes outside the kernel's taxonomy (e.g. VOC
/// `chair`/`tvmonitor`) — otherwise a frame of unmapped-but-confident detections would report
/// perfect precision and a 0% false-alarm rate.
pub fn unknown_false_positives(frames: &[FrameSample], conf_threshold: f32) -> u32 {
    frames
        .iter()
        .flat_map(|f| f.preds.iter())
        .filter(|p| p.class == ObjectClass::Unknown && p.confidence >= conf_threshold)
        .count() as u32
}

/// Overall (micro) counts at a threshold: per-class matches over [`EVAL_CLASSES`] plus every
/// `Unknown` prediction as a false positive.
pub fn overall_counts_at_threshold(
    frames: &[FrameSample],
    conf_threshold: f32,
    iou_threshold: f32,
) -> Counts {
    let mut counts =
        micro_counts_at_threshold(frames, &EVAL_CLASSES, conf_threshold, iou_threshold);
    counts.false_positives += unknown_false_positives(frames, conf_threshold);
    counts
}

/// The size class the kernel would emit for these predictions at `conf_threshold`, mirroring
/// `TractBackend::size_class_for` (max box area >= [`LARGE_AREA_THRESHOLD`] => Large). Size is
/// class-agnostic, so all predictions (including `Unknown`) contribute. Recomputing at the
/// operating threshold — rather than reusing the backend's `DetectionResult.size_class`, which
/// is produced at the lower sweep threshold — ensures the size-class outcome reflects what would
/// actually be emitted.
pub fn size_class_for_predictions(preds: &[Prediction], conf_threshold: f32) -> SizeClass {
    let max_area = preds
        .iter()
        .filter(|p| p.confidence >= conf_threshold)
        .map(|p| p.bbox.area())
        .fold(0.0_f32, f32::max);
    if max_area <= 0.0 {
        SizeClass::Unknown
    } else if max_area >= LARGE_AREA_THRESHOLD {
        SizeClass::Large
    } else {
        SizeClass::Small
    }
}

/// A point on the precision/recall vs. confidence-threshold sweep.
#[derive(Clone, Copy, Debug, Serialize)]
pub struct SweepPoint {
    pub threshold: f32,
    pub precision: f32,
    pub recall: f32,
    pub f1: f32,
    pub counts: Counts,
}

/// Sweep confidence thresholds, reporting overall (micro) precision/recall/F1 at each. This is
/// the curve that turns the kernel's hardcoded `0.5` into a data-driven operating-point choice.
pub fn confidence_sweep(
    frames: &[FrameSample],
    thresholds: &[f32],
    iou_threshold: f32,
) -> Vec<SweepPoint> {
    thresholds
        .iter()
        .map(|&t| {
            let counts = overall_counts_at_threshold(frames, t, iou_threshold);
            let p = precision(&counts);
            let r = recall(&counts);
            SweepPoint {
                threshold: t,
                precision: p,
                recall: r,
                f1: f1(p, r),
                counts,
            }
        })
        .collect()
}

/// Average precision for one class via all-point interpolation over the PR curve. Returns
/// `None` when the class has no ground truth (AP is undefined and must not pollute mAP).
pub fn average_precision(
    frames: &[FrameSample],
    class: ObjectClass,
    iou_threshold: f32,
) -> Option<f32> {
    let gt_per_frame: Vec<Vec<EvalBox>> = frames
        .iter()
        .map(|f| {
            f.gts
                .iter()
                .filter(|g| g.class == class)
                .map(|g| g.bbox)
                .collect()
        })
        .collect();
    let total_gt: usize = gt_per_frame.iter().map(|v| v.len()).sum();
    if total_gt == 0 {
        return None;
    }

    // All predictions for this class across the dataset, ranked by confidence desc.
    let mut ranked: Vec<(usize, f32, EvalBox)> = Vec::new();
    for (fi, frame) in frames.iter().enumerate() {
        for p in frame.preds.iter().filter(|p| p.class == class) {
            ranked.push((fi, p.confidence, p.bbox));
        }
    }
    ranked.sort_by(|a, b| b.1.total_cmp(&a.1));

    let mut matched: Vec<Vec<bool>> = gt_per_frame.iter().map(|v| vec![false; v.len()]).collect();
    let mut tp = 0u32;
    let mut fp = 0u32;
    let mut points: Vec<(f32, f32)> = Vec::with_capacity(ranked.len());
    for (fi, _conf, bbox) in ranked {
        let mut best_iou = iou_threshold;
        let mut best_j: Option<usize> = None;
        for (j, gt) in gt_per_frame[fi].iter().enumerate() {
            if matched[fi][j] {
                continue;
            }
            let v = iou(&bbox, gt);
            if v >= best_iou {
                best_iou = v;
                best_j = Some(j);
            }
        }
        if let Some(j) = best_j {
            matched[fi][j] = true;
            tp += 1;
        } else {
            fp += 1;
        }
        let recall = tp as f32 / total_gt as f32;
        let precision = tp as f32 / (tp + fp) as f32;
        points.push((recall, precision));
    }

    Some(average_precision_from_points(&points))
}

/// Integrate an all-point-interpolated PR curve. `points` are `(recall, precision)` in order of
/// decreasing confidence (so recall is non-decreasing).
fn average_precision_from_points(points: &[(f32, f32)]) -> f32 {
    if points.is_empty() {
        return 0.0;
    }
    let mut prec: Vec<f32> = points.iter().map(|p| p.1).collect();
    let rec: Vec<f32> = points.iter().map(|p| p.0).collect();
    // Make precision monotonically non-increasing as recall grows (interpolation).
    for i in (0..prec.len() - 1).rev() {
        prec[i] = prec[i].max(prec[i + 1]);
    }
    let mut ap = 0.0;
    let mut prev_recall = 0.0;
    for i in 0..rec.len() {
        let dr = rec[i] - prev_recall;
        if dr > 0.0 {
            ap += dr * prec[i];
        }
        prev_recall = rec[i];
    }
    ap
}

/// Latency summary in milliseconds. All fields are finite (zeros when no samples).
#[derive(Clone, Copy, Debug, Default, Serialize)]
pub struct LatencyStats {
    pub count: usize,
    pub mean_ms: f64,
    pub p50_ms: f64,
    pub p95_ms: f64,
    pub p99_ms: f64,
    pub min_ms: f64,
    pub max_ms: f64,
}

pub fn latency_stats(mut samples_ms: Vec<f64>) -> LatencyStats {
    if samples_ms.is_empty() {
        return LatencyStats::default();
    }
    samples_ms.sort_by(|a, b| a.total_cmp(b));
    let count = samples_ms.len();
    let sum: f64 = samples_ms.iter().sum();
    let pct = |p: f64| -> f64 {
        let idx = ((p / 100.0) * (count as f64 - 1.0)).round() as usize;
        samples_ms[idx.min(count - 1)]
    };
    LatencyStats {
        count,
        mean_ms: sum / count as f64,
        p50_ms: pct(50.0),
        p95_ms: pct(95.0),
        p99_ms: pct(99.0),
        min_ms: samples_ms[0],
        max_ms: samples_ms[count - 1],
    }
}

/// One cell of the size-class confusion matrix.
#[derive(Clone, Debug, Serialize)]
pub struct SizeClassCell {
    pub expected: String,
    pub predicted: String,
    pub count: u32,
}

/// Confusion between the expected and predicted per-frame size class. A wrong size means the
/// kernel would emit the wrong event (`BoundaryCrossingObjectLarge` vs `...Small`).
#[derive(Clone, Debug, Default, Serialize)]
pub struct SizeClassConfusion {
    pub cells: Vec<SizeClassCell>,
    pub correct: u32,
    pub total: u32,
    pub accuracy: f32,
}

fn size_class_confusion(frames: &[FrameSample]) -> SizeClassConfusion {
    let order = [SizeClass::Unknown, SizeClass::Small, SizeClass::Large];
    let mut grid = [[0u32; 3]; 3];
    let idx = |s: SizeClass| order.iter().position(|o| *o == s).unwrap_or(0);
    let mut correct = 0u32;
    let mut total = 0u32;
    for frame in frames {
        if let (Some(exp), Some(pred)) = (frame.expected_size, frame.predicted_size) {
            grid[idx(exp)][idx(pred)] += 1;
            total += 1;
            if exp == pred {
                correct += 1;
            }
        }
    }
    let mut cells = Vec::new();
    for (ei, exp) in order.iter().enumerate() {
        for (pi, pred) in order.iter().enumerate() {
            if grid[ei][pi] > 0 {
                cells.push(SizeClassCell {
                    expected: size_class_name(*exp).to_string(),
                    predicted: size_class_name(*pred).to_string(),
                    count: grid[ei][pi],
                });
            }
        }
    }
    let accuracy = if total == 0 {
        0.0
    } else {
        correct as f32 / total as f32
    };
    SizeClassConfusion {
        cells,
        correct,
        total,
        accuracy,
    }
}

/// Per-class metrics at the operating threshold.
#[derive(Clone, Debug, Serialize)]
pub struct ClassMetrics {
    pub class: String,
    pub ground_truth: u32,
    pub counts: Counts,
    pub precision: f32,
    pub recall: f32,
    pub f1: f32,
    pub average_precision: Option<f32>,
}

/// Headline micro-averaged metrics at the operating threshold.
#[derive(Clone, Debug, Serialize)]
pub struct OverallMetrics {
    pub counts: Counts,
    pub precision: f32,
    pub recall: f32,
    pub f1: f32,
    /// Fraction of real events the witness missed at the operating threshold (`1 - recall`).
    pub witness_miss_rate: f32,
}

/// The full, serde-serializable evaluation report. Contains no NaN/Infinity, so it is always
/// valid JSON.
#[derive(Clone, Debug, Serialize)]
pub struct EvalReport {
    pub model: String,
    pub backend: String,
    pub iou_threshold: f32,
    pub operating_threshold: f32,
    pub frames: usize,
    pub ground_truth_objects: usize,
    pub predictions: usize,
    pub overall: OverallMetrics,
    pub per_class: Vec<ClassMetrics>,
    pub mean_average_precision: f32,
    pub confidence_sweep: Vec<SweepPoint>,
    pub size_class_confusion: SizeClassConfusion,
    pub latency: LatencyStats,
}

/// Metadata threaded through into the report header.
pub struct ReportMeta {
    pub model: String,
    pub backend: String,
}

/// Assemble a full [`EvalReport`] from evaluated frames. Kept here (not in the binary) so the
/// entire computation is unit-testable without a model, image decoder, or feature flag.
pub fn build_report(
    frames: &[FrameSample],
    meta: ReportMeta,
    operating_threshold: f32,
    iou_threshold: f32,
    sweep_thresholds: &[f32],
    latencies_ms: Vec<f64>,
) -> EvalReport {
    let mut per_class = Vec::new();
    let mut ap_values = Vec::new();
    for &class in EVAL_CLASSES.iter() {
        let gt = frames
            .iter()
            .flat_map(|f| f.gts.iter())
            .filter(|g| g.class == class)
            .count() as u32;
        let counts = counts_at_threshold(frames, class, operating_threshold, iou_threshold);
        let ap = average_precision(frames, class, iou_threshold);
        if let Some(v) = ap {
            ap_values.push(v);
        }
        let p = precision(&counts);
        let r = recall(&counts);
        per_class.push(ClassMetrics {
            class: class_name(class).to_string(),
            ground_truth: gt,
            counts,
            precision: p,
            recall: r,
            f1: f1(p, r),
            average_precision: ap,
        });
    }

    let overall_counts = overall_counts_at_threshold(frames, operating_threshold, iou_threshold);
    let op = precision(&overall_counts);
    let orr = recall(&overall_counts);
    let mean_ap = if ap_values.is_empty() {
        0.0
    } else {
        ap_values.iter().sum::<f32>() / ap_values.len() as f32
    };

    EvalReport {
        model: meta.model,
        backend: meta.backend,
        iou_threshold,
        operating_threshold,
        frames: frames.len(),
        ground_truth_objects: frames.iter().map(|f| f.gts.len()).sum(),
        predictions: frames.iter().map(|f| f.preds.len()).sum(),
        overall: OverallMetrics {
            counts: overall_counts,
            precision: op,
            recall: orr,
            f1: f1(op, orr),
            witness_miss_rate: 1.0 - orr,
        },
        per_class,
        mean_average_precision: mean_ap,
        confidence_sweep: confidence_sweep(frames, sweep_thresholds, iou_threshold),
        size_class_confusion: size_class_confusion(frames),
        latency: latency_stats(latencies_ms),
    }
}

/// Parse a `start:end:step` sweep spec (inclusive of `end` within float tolerance) into a list
/// of thresholds. All values must be within 0..=1 and `step` > 0.
pub fn parse_sweep_spec(spec: &str) -> Result<Vec<f32>, String> {
    let parts: Vec<&str> = spec.split(':').collect();
    if parts.len() != 3 {
        return Err(format!("sweep spec must be 'start:end:step', got '{spec}'"));
    }
    let parse = |s: &str| s.trim().parse::<f32>().map_err(|e| e.to_string());
    let start = parse(parts[0])?;
    let end = parse(parts[1])?;
    let step = parse(parts[2])?;
    if step <= 0.0 {
        return Err("sweep step must be > 0".to_string());
    }
    if !(0.0..=1.0).contains(&start) || !(0.0..=1.0).contains(&end) {
        return Err("sweep start/end must be within 0..=1".to_string());
    }
    if end < start {
        return Err("sweep end must be >= start".to_string());
    }
    let mut out = Vec::new();
    let mut t = start;
    // Add a small epsilon so the inclusive endpoint survives float accumulation.
    while t <= end + 1e-6 {
        out.push((t.clamp(0.0, 1.0) * 1000.0).round() / 1000.0);
        t += step;
        // A `step` far smaller than `t`'s float resolution would never advance `t`, looping
        // forever and growing `out` without bound (a DoS via the `--confidence-sweep` CLI arg).
        // Cap the count: across 0..=1 even a fine 0.001 step is only ~1000 points.
        if out.len() > MAX_SWEEP_STEPS {
            return Err(format!(
                "sweep spec produces more than {MAX_SWEEP_STEPS} steps; use a larger step"
            ));
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b(x: f32, y: f32, w: f32, h: f32) -> EvalBox {
        EvalBox::new(x, y, w, h)
    }

    #[test]
    fn iou_identical_is_one() {
        let a = b(0.1, 0.1, 0.2, 0.2);
        assert!((iou(&a, &a) - 1.0).abs() < 1e-6);
    }

    #[test]
    fn iou_disjoint_is_zero() {
        let a = b(0.0, 0.0, 0.1, 0.1);
        let c = b(0.5, 0.5, 0.1, 0.1);
        assert_eq!(iou(&a, &c), 0.0);
    }

    #[test]
    fn iou_half_overlap() {
        // Two unit-ish boxes overlapping in exactly half their area.
        let a = b(0.0, 0.0, 0.2, 0.2);
        let c = b(0.1, 0.0, 0.2, 0.2);
        // intersection = 0.1 * 0.2 = 0.02; union = 0.04 + 0.04 - 0.02 = 0.06; iou = 1/3.
        assert!((iou(&a, &c) - 1.0 / 3.0).abs() < 1e-5);
    }

    #[test]
    fn perfect_detection_scores_one() {
        let gt = GtObject {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
        };
        let pred = Prediction {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
            confidence: 0.9,
        };
        let frames = vec![FrameSample {
            gts: vec![gt],
            preds: vec![pred],
            expected_size: None,
            predicted_size: None,
        }];
        let counts = counts_at_threshold(&frames, ObjectClass::Person, 0.5, 0.5);
        assert_eq!(
            counts,
            Counts {
                true_positives: 1,
                false_positives: 0,
                false_negatives: 0
            }
        );
        assert_eq!(precision(&counts), 1.0);
        assert_eq!(recall(&counts), 1.0);
        assert_eq!(
            average_precision(&frames, ObjectClass::Person, 0.5),
            Some(1.0)
        );
    }

    #[test]
    fn missed_detection_is_false_negative() {
        let gt = GtObject {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
        };
        let frames = vec![FrameSample {
            gts: vec![gt],
            preds: vec![],
            expected_size: None,
            predicted_size: None,
        }];
        let counts = counts_at_threshold(&frames, ObjectClass::Person, 0.5, 0.5);
        assert_eq!(counts.false_negatives, 1);
        assert_eq!(recall(&counts), 0.0);
        // Recall=0 → the witness missed the event entirely.
    }

    #[test]
    fn threshold_filters_low_confidence() {
        let gt = GtObject {
            class: ObjectClass::Vehicle,
            bbox: b(0.3, 0.4, 0.4, 0.4),
        };
        let pred = Prediction {
            class: ObjectClass::Vehicle,
            bbox: b(0.3, 0.4, 0.4, 0.4),
            confidence: 0.3,
        };
        let frames = vec![FrameSample {
            gts: vec![gt],
            preds: vec![pred],
            expected_size: None,
            predicted_size: None,
        }];
        // At 0.5 the 0.3-confidence prediction is dropped → a miss.
        let high = counts_at_threshold(&frames, ObjectClass::Vehicle, 0.5, 0.5);
        assert_eq!(high.true_positives, 0);
        assert_eq!(high.false_negatives, 1);
        // At 0.1 it is kept → a hit.
        let low = counts_at_threshold(&frames, ObjectClass::Vehicle, 0.1, 0.5);
        assert_eq!(low.true_positives, 1);
    }

    #[test]
    fn duplicate_prediction_is_false_positive() {
        let gt = GtObject {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
        };
        let p1 = Prediction {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
            confidence: 0.9,
        };
        let p2 = Prediction {
            class: ObjectClass::Person,
            bbox: b(0.1, 0.2, 0.4, 0.4),
            confidence: 0.8,
        };
        let frames = vec![FrameSample {
            gts: vec![gt],
            preds: vec![p1, p2],
            expected_size: None,
            predicted_size: None,
        }];
        let counts = counts_at_threshold(&frames, ObjectClass::Person, 0.5, 0.5);
        assert_eq!(counts.true_positives, 1);
        assert_eq!(counts.false_positives, 1);
        // Highest-confidence prediction is the TP; the duplicate is still AP=1 (found at top).
        assert_eq!(
            average_precision(&frames, ObjectClass::Person, 0.5),
            Some(1.0)
        );
    }

    #[test]
    fn average_precision_none_without_ground_truth() {
        let pred = Prediction {
            class: ObjectClass::Animal,
            bbox: b(0.1, 0.2, 0.4, 0.4),
            confidence: 0.9,
        };
        let frames = vec![FrameSample {
            gts: vec![],
            preds: vec![pred],
            expected_size: None,
            predicted_size: None,
        }];
        assert_eq!(average_precision(&frames, ObjectClass::Animal, 0.5), None);
    }

    #[test]
    fn latency_percentiles() {
        let stats = latency_stats((1..=100).map(|v| v as f64).collect());
        assert_eq!(stats.count, 100);
        assert!((stats.min_ms - 1.0).abs() < 1e-9);
        assert!((stats.max_ms - 100.0).abs() < 1e-9);
        assert!((stats.mean_ms - 50.5).abs() < 1e-9);
        // nearest-rank: p95 over 0-based index round(0.95*99)=94 → value 95.
        assert!((stats.p95_ms - 95.0).abs() < 1e-9);
    }

    #[test]
    fn size_confusion_counts_correct() {
        let frame = |exp: SizeClass, pred: SizeClass| FrameSample {
            gts: vec![],
            preds: vec![],
            expected_size: Some(exp),
            predicted_size: Some(pred),
        };
        let frames = vec![
            frame(SizeClass::Large, SizeClass::Large),
            frame(SizeClass::Large, SizeClass::Small),
            frame(SizeClass::Small, SizeClass::Small),
        ];
        let conf = size_class_confusion(&frames);
        assert_eq!(conf.total, 3);
        assert_eq!(conf.correct, 2);
        assert!((conf.accuracy - 2.0 / 3.0).abs() < 1e-6);
    }

    #[test]
    fn build_report_is_json_safe_and_correct() {
        // One frame: a person GT perfectly matched, a vehicle predicted below the 0.5 operating
        // threshold. Mirrors the deterministic `test_detector.onnx` fixture.
        let frames = vec![FrameSample {
            gts: vec![GtObject {
                class: ObjectClass::Person,
                bbox: b(0.1, 0.2, 0.4, 0.4),
            }],
            preds: vec![
                Prediction {
                    class: ObjectClass::Person,
                    bbox: b(0.1, 0.2, 0.4, 0.4),
                    confidence: 0.9,
                },
                Prediction {
                    class: ObjectClass::Vehicle,
                    bbox: b(0.3, 0.4, 0.4, 0.4),
                    confidence: 0.3,
                },
            ],
            expected_size: Some(SizeClass::Small),
            predicted_size: Some(SizeClass::Small),
        }];
        let report = build_report(
            &frames,
            ReportMeta {
                model: "test".into(),
                backend: "tract".into(),
            },
            0.5,
            0.5,
            &parse_sweep_spec("0.1:0.9:0.4").unwrap(),
            vec![1.0, 2.0, 3.0],
        );
        assert_eq!(report.overall.precision, 1.0);
        assert_eq!(report.overall.recall, 1.0);
        assert_eq!(report.overall.witness_miss_rate, 0.0);
        assert!((report.mean_average_precision - 1.0).abs() < 1e-6);
        // Must serialize cleanly (no NaN/Infinity).
        let json = serde_json::to_string(&report).expect("report serializes");
        assert!(json.contains("witness_miss_rate"));
    }

    #[test]
    fn sweep_spec_parses_inclusive_endpoint() {
        let ts = parse_sweep_spec("0.0:1.0:0.25").unwrap();
        assert_eq!(ts, vec![0.0, 0.25, 0.5, 0.75, 1.0]);
    }

    #[test]
    fn sweep_spec_rejects_bad_input() {
        assert!(parse_sweep_spec("0.1:0.9").is_err());
        assert!(parse_sweep_spec("0.1:0.9:0").is_err());
        assert!(parse_sweep_spec("0.9:0.1:0.1").is_err());
    }

    #[test]
    fn sweep_spec_rejects_too_many_steps() {
        // A vanishingly small step would loop forever / exhaust memory without the cap.
        assert!(parse_sweep_spec("0.0:1.0:0.000001").is_err());
    }

    #[test]
    fn unknown_predictions_count_as_false_alarms() {
        // A confident detection of an unmapped class (Unknown) on a frame with no ground truth
        // must register as a false alarm, not vanish into a perfect score.
        let frames = vec![FrameSample {
            gts: vec![],
            preds: vec![Prediction {
                class: ObjectClass::Unknown,
                bbox: b(0.1, 0.1, 0.2, 0.2),
                confidence: 0.9,
            }],
            expected_size: None,
            predicted_size: None,
        }];
        let counts = overall_counts_at_threshold(&frames, 0.5, 0.5);
        assert_eq!(counts.false_positives, 1);
        assert_eq!(counts.true_positives, 0);
        assert_eq!(precision(&counts), 0.0);
        // Below the operating threshold it is not counted.
        assert_eq!(
            overall_counts_at_threshold(&frames, 0.95, 0.5).false_positives,
            0
        );
    }

    #[test]
    fn size_class_recomputed_at_operating_threshold() {
        let preds = vec![
            // Large box (area 0.36) but low confidence — discarded at the operating threshold.
            Prediction {
                class: ObjectClass::Person,
                bbox: b(0.0, 0.0, 0.6, 0.6),
                confidence: 0.3,
            },
            // Small box (area 0.01), high confidence.
            Prediction {
                class: ObjectClass::Person,
                bbox: b(0.0, 0.0, 0.1, 0.1),
                confidence: 0.9,
            },
        ];
        // At 0.5 only the small high-confidence box survives → Small (the bug: reusing the
        // backend's size class would report Large from the discarded low-confidence box).
        assert_eq!(size_class_for_predictions(&preds, 0.5), SizeClass::Small);
        // At 0.1 the large box is included → Large.
        assert_eq!(size_class_for_predictions(&preds, 0.1), SizeClass::Large);
        // Above all confidences → no detection → Unknown.
        assert_eq!(size_class_for_predictions(&preds, 0.99), SizeClass::Unknown);
    }
}
