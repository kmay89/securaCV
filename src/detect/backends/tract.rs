#![cfg(feature = "backend-tract")]

use std::path::Path;

use anyhow::{anyhow, Context, Result};
use tract_onnx::prelude::*;

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::result::{DetectionResult, SizeClass};

/// Tract-based backend for ONNX inference.
///
/// This backend loads a local model file and performs inference on RGB frames.
/// It does not perform any network I/O or write to disk beyond model loading.
pub struct TractBackend {
    model: SimplePlan<TypedFact, Box<dyn TypedOp>>,
    width: u32,
    height: u32,
    confidence_threshold: f32,
}

impl TractBackend {
    /// Load an ONNX model from disk and prepare it for inference.
    pub fn new<P: AsRef<Path>>(model_path: P, width: u32, height: u32) -> Result<Self> {
        let model_path = model_path.as_ref();
        let model = tract_onnx::onnx()
            .model_for_path(model_path)
            .with_context(|| format!("failed to load ONNX model from {}", model_path.display()))?
            .with_input_fact(
                0,
                InferenceFact::dt_shape(
                    f32::datum_type(),
                    tvec!(1, 3, height as usize, width as usize),
                ),
            )
            .context("failed to set input fact")?
            .into_optimized()
            .context("failed to optimize ONNX model")?
            .into_runnable()
            .context("failed to build runnable ONNX model")?;

        Ok(Self {
            model,
            width,
            height,
            confidence_threshold: 0.5,
        })
    }

    /// Override the default confidence threshold.
    pub fn with_threshold(mut self, threshold: f32) -> Self {
        self.confidence_threshold = threshold;
        self
    }

    fn build_input(&self, pixels: &[u8], width: u32, height: u32) -> Result<Tensor> {
        if width != self.width || height != self.height {
            return Err(anyhow!(
                "frame size {}x{} does not match model input {}x{}",
                width,
                height,
                self.width,
                self.height
            ));
        }

        let expected_len = (width as usize)
            .checked_mul(height as usize)
            .and_then(|v| v.checked_mul(3))
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;

        if pixels.len() != expected_len {
            return Err(anyhow!(
                "expected {} RGB bytes, received {}",
                expected_len,
                pixels.len()
            ));
        }

        let width = width as usize;
        let input = tract_ndarray::Array4::from_shape_fn(
            (1, 3, height as usize, width),
            |(_, channel, y, x)| {
                let idx = (y * width + x) * 3 + channel;
                pixels[idx] as f32 / 255.0
            },
        );

        Ok(input.into_tensor())
    }

    fn extract_confidence(&self, outputs: TVec<Tensor>) -> Result<f32> {
        let output = outputs
            .get(0)
            .ok_or_else(|| anyhow!("model produced no outputs"))?;
        let scores = output
            .to_array_view::<f32>()
            .context("model output tensor was not f32")?;
        let max_score = scores
            .iter()
            .cloned()
            .fold(f32::NEG_INFINITY, f32::max);
        if max_score.is_finite() {
            Ok(max_score)
        } else {
            Ok(0.0)
        }
    }
}

impl DetectorBackend for TractBackend {
    fn name(&self) -> &'static str {
        "tract"
    }

    fn supports(&self, capability: DetectionCapability) -> bool {
        matches!(capability, DetectionCapability::Classification)
    }

    fn detect(&mut self, pixels: &[u8], width: u32, height: u32) -> Result<DetectionResult> {
        let input = self.build_input(pixels, width, height)?;
        let outputs = self
            .model
            .run(tvec!(input))
            .context("ONNX inference failed")?;
        let confidence = self.extract_confidence(outputs)?;

        Ok(DetectionResult {
            motion_detected: confidence >= self.confidence_threshold,
            detections: Vec::new(),
            confidence,
            size_class: SizeClass::Unknown,
        })
    }
}
