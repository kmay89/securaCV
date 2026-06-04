//! End-to-end eval runner: decode a labeled dataset, run a detector over every frame, and
//! assemble an [`EvalReport`].
//!
//! Gated behind the `detect-eval` feature because it needs both the `image` decoder and the
//! `backend-tract` ONNX runtime. The pure metric math it calls lives in [`crate::eval::metrics`]
//! and is testable without this feature.

use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::{Context, Result};

use crate::detect::{DetectorBackend, TractBackend};
use crate::eval::dataset::load_manifest;
use crate::eval::metrics::{
    build_report, EvalBox, EvalReport, FrameSample, Prediction, ReportMeta,
};

/// Configuration for a single eval run.
pub struct EvalConfig {
    pub model_path: PathBuf,
    pub dataset_dir: PathBuf,
    pub width: u32,
    pub height: u32,
    pub iou_threshold: f32,
    pub operating_threshold: f32,
    pub sweep: Vec<f32>,
}

/// Decode an image to packed RGB8 at the model's input resolution. Shares the same crate as the
/// esp32 ingest decoder (`image`), resizing to the exact model input the backend requires.
fn decode_rgb8(path: &Path, width: u32, height: u32) -> Result<Vec<u8>> {
    let img =
        image::open(path).with_context(|| format!("decoding eval image {}", path.display()))?;
    let resized = img.resize_exact(width, height, image::imageops::FilterType::Triangle);
    Ok(resized.to_rgb8().into_raw())
}

/// Run the detector over the whole dataset and produce a report.
pub fn run_eval(cfg: &EvalConfig) -> Result<EvalReport> {
    let manifest = load_manifest(&cfg.dataset_dir)?;

    // Run the backend at the lowest threshold we will ever evaluate so the metrics layer — not
    // the backend — owns thresholding. This is what lets the sweep explore operating points
    // below the kernel's hardcoded 0.5 without re-running inference.
    let min_threshold = cfg
        .sweep
        .iter()
        .cloned()
        .fold(cfg.operating_threshold, f32::min)
        .max(0.0);

    let mut backend = TractBackend::new(&cfg.model_path, cfg.width, cfg.height)
        .with_context(|| format!("loading model {}", cfg.model_path.display()))?
        .with_threshold(min_threshold);
    backend.warm_up().ok();

    let mut frames = Vec::with_capacity(manifest.frames.len());
    let mut latencies_ms = Vec::with_capacity(manifest.frames.len());

    for frame in &manifest.frames {
        let image_path = cfg.dataset_dir.join(&frame.image);
        let pixels = decode_rgb8(&image_path, cfg.width, cfg.height)?;

        let start = Instant::now();
        let result = backend
            .detect(&pixels, cfg.width, cfg.height)
            .with_context(|| format!("running detector on {}", image_path.display()))?;
        latencies_ms.push(start.elapsed().as_secs_f64() * 1000.0);

        let preds = result
            .detections
            .iter()
            .map(|d| Prediction {
                class: d.class,
                bbox: EvalBox::new(d.x, d.y, d.w, d.h),
                confidence: d.confidence,
            })
            .collect();

        frames.push(FrameSample {
            gts: frame.ground_truth()?,
            preds,
            expected_size: frame.expected_size()?,
            predicted_size: Some(result.size_class),
        });
    }

    let model_name = cfg
        .model_path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("model")
        .to_string();

    Ok(build_report(
        &frames,
        ReportMeta {
            model: model_name,
            backend: backend.name().to_string(),
        },
        cfg.operating_threshold,
        cfg.iou_threshold,
        &cfg.sweep,
        latencies_ms,
    ))
}
