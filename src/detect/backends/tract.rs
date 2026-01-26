#![cfg(feature = "backend-tract")]

use std::path::Path;

use anyhow::{anyhow, Context, Result};
use tract_onnx::prelude::*;

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::result::{Detection, DetectionResult, ObjectClass, SizeClass};

const LARGE_AREA_THRESHOLD: f32 = 0.2;
const ABSOLUTE_COORD_THRESHOLD: f32 = 1.5;

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

    fn validate_threshold(&self) -> Result<()> {
        if (0.0..=1.0).contains(&self.confidence_threshold) {
            Ok(())
        } else {
            Err(anyhow!(
                "confidence threshold {} must be within 0..=1",
                self.confidence_threshold
            ))
        }
    }

    fn extract_detections(
        &self,
        outputs: TVec<Tensor>,
        frame_width: u32,
        frame_height: u32,
    ) -> Result<Vec<Detection>> {
        if outputs.is_empty() {
            return Err(anyhow!("model produced no outputs"));
        }

        match outputs.len() {
            1 => self.parse_combined_output(&outputs[0], frame_width, frame_height),
            3.. => self.parse_separate_outputs(&outputs, frame_width, frame_height),
            _ => Err(anyhow!(
                "expected either 1 or 3+ output tensors, got {}",
                outputs.len()
            )),
        }
    }

    fn parse_combined_output(
        &self,
        output: &Tensor,
        frame_width: u32,
        frame_height: u32,
    ) -> Result<Vec<Detection>> {
        let shape = output.shape();
        let data = output
            .to_array_view::<f32>()
            .context("combined output tensor was not f32")?
            .as_slice()
            .ok_or_else(|| anyhow!("combined output tensor is not contiguous"))?;

        let (rows, cols) = match shape {
            [1, n, 6] => (*n, 6),
            [n, 6] => (*n, 6),
            _ => {
                return Err(anyhow!(
                    "combined output tensor must have shape [N,6] or [1,N,6], got {:?}",
                    shape
                ))
            }
        };

        if data.len() != rows.saturating_mul(cols) {
            return Err(anyhow!(
                "combined output tensor has {} values, expected {}",
                data.len(),
                rows.saturating_mul(cols)
            ));
        }

        let mut detections = Vec::new();
        for chunk in data.chunks(cols) {
            let confidence = chunk[4];
            if !confidence.is_finite() {
                return Err(anyhow!("combined output confidence was not finite"));
            }
            if confidence < self.confidence_threshold {
                continue;
            }
            let class_id = chunk[5].round() as i64;
            if let Some((x, y, w, h)) = self.normalize_box(
                [chunk[0], chunk[1], chunk[2], chunk[3]],
                frame_width,
                frame_height,
            )? {
                detections.push(Detection {
                    x,
                    y,
                    w,
                    h,
                    confidence,
                    class: Self::map_class_id(class_id),
                });
            }
        }

        Ok(detections)
    }

    fn parse_separate_outputs(
        &self,
        outputs: &TVec<Tensor>,
        frame_width: u32,
        frame_height: u32,
    ) -> Result<Vec<Detection>> {
        let boxes = Self::extract_boxes(&outputs[0])?;
        let scores = Self::extract_scores(&outputs[1])?;
        let classes = Self::extract_class_ids(&outputs[2])?;

        if boxes.len() != scores.len() || boxes.len() != classes.len() {
            return Err(anyhow!(
                "output tensor lengths mismatch: boxes {}, scores {}, classes {}",
                boxes.len(),
                scores.len(),
                classes.len()
            ));
        }

        let mut detections = Vec::new();
        for ((raw_box, confidence), class_id) in boxes
            .into_iter()
            .zip(scores.into_iter())
            .zip(classes.into_iter())
        {
            if !confidence.is_finite() {
                return Err(anyhow!("score was not finite"));
            }
            if confidence < self.confidence_threshold {
                continue;
            }
            if let Some((x, y, w, h)) = self.normalize_box(raw_box, frame_width, frame_height)? {
                detections.push(Detection {
                    x,
                    y,
                    w,
                    h,
                    confidence,
                    class: Self::map_class_id(class_id),
                });
            }
        }

        Ok(detections)
    }

    fn extract_boxes(output: &Tensor) -> Result<Vec<[f32; 4]>> {
        let shape = output.shape();
        let data = output
            .to_array_view::<f32>()
            .context("boxes tensor was not f32")?
            .as_slice()
            .ok_or_else(|| anyhow!("boxes tensor is not contiguous"))?;
        let rows = match shape {
            [1, n, 4] => *n,
            [n, 4] => *n,
            _ => {
                return Err(anyhow!(
                    "boxes tensor must have shape [N,4] or [1,N,4], got {:?}",
                    shape
                ))
            }
        };
        let expected = rows.saturating_mul(4);
        if data.len() != expected {
            return Err(anyhow!(
                "boxes tensor has {} values, expected {}",
                data.len(),
                expected
            ));
        }
        Ok(data
            .chunks(4)
            .map(|chunk| [chunk[0], chunk[1], chunk[2], chunk[3]])
            .collect())
    }

    fn extract_scores(output: &Tensor) -> Result<Vec<f32>> {
        let shape = output.shape();
        let data = output
            .to_array_view::<f32>()
            .context("scores tensor was not f32")?
            .as_slice()
            .ok_or_else(|| anyhow!("scores tensor is not contiguous"))?;
        let len = match shape {
            [1, n] => *n,
            [n] => *n,
            [1, n, 1] => *n,
            _ => {
                return Err(anyhow!(
                    "scores tensor must have shape [N], [1,N], or [1,N,1], got {:?}",
                    shape
                ))
            }
        };
        if data.len() != len {
            return Err(anyhow!(
                "scores tensor has {} values, expected {}",
                data.len(),
                len
            ));
        }
        Ok(data.to_vec())
    }

    fn extract_class_ids(output: &Tensor) -> Result<Vec<i64>> {
        let shape = output.shape();
        let len = match shape {
            [1, n] => *n,
            [n] => *n,
            [1, n, 1] => *n,
            _ => {
                return Err(anyhow!(
                    "class tensor must have shape [N], [1,N], or [1,N,1], got {:?}",
                    shape
                ))
            }
        };

        if let Ok(view) = output.to_array_view::<i64>() {
            let data = view
                .as_slice()
                .ok_or_else(|| anyhow!("class tensor (i64) is not contiguous"))?;
            if data.len() != len {
                return Err(anyhow!(
                    "class tensor (i64) has {} values, expected {}",
                    data.len(),
                    len
                ));
            }
            Ok(data.to_vec())
        } else if let Ok(view) = output.to_array_view::<f32>() {
            let data = view
                .as_slice()
                .ok_or_else(|| anyhow!("class tensor (f32) is not contiguous"))?;
            if data.len() != len {
                return Err(anyhow!(
                    "class tensor (f32) has {} values, expected {}",
                    data.len(),
                    len
                ));
            }
            Ok(data.iter().map(|v| v.round() as i64).collect())
        } else {
            Err(anyhow!(
                "class tensor must be i64 or f32, but was {:?}",
                output.datum_type()
            ))
        }
    }

    fn normalize_box(
        &self,
        raw: [f32; 4],
        frame_width: u32,
        frame_height: u32,
    ) -> Result<Option<(f32, f32, f32, f32)>> {
        if raw.iter().any(|v| !v.is_finite()) {
            return Err(anyhow!("box coordinates were not finite"));
        }

        let absolute = raw.iter().any(|v| *v > ABSOLUTE_COORD_THRESHOLD);
        let (mut x1, mut y1, mut x2, mut y2) = (raw[0], raw[1], raw[2], raw[3]);
        if absolute {
            let width = frame_width as f32;
            let height = frame_height as f32;
            if width <= 0.0 || height <= 0.0 {
                return Err(anyhow!("frame dimensions must be positive"));
            }
            x1 /= width;
            x2 /= width;
            y1 /= height;
            y2 /= height;
        }

        x1 = x1.clamp(0.0, 1.0);
        y1 = y1.clamp(0.0, 1.0);
        x2 = x2.clamp(0.0, 1.0);
        y2 = y2.clamp(0.0, 1.0);

        if x2 <= x1 || y2 <= y1 {
            return Ok(None);
        }

        Ok(Some((x1, y1, x2 - x1, y2 - y1)))
    }

    fn map_class_id(class_id: i64) -> ObjectClass {
        match class_id {
            0 => ObjectClass::Person,
            1 => ObjectClass::Vehicle,
            2 => ObjectClass::Animal,
            3 => ObjectClass::Package,
            _ => ObjectClass::Unknown,
        }
    }

    fn size_class_for(detections: &[Detection]) -> SizeClass {
        let max_area = detections.iter().map(|d| d.w * d.h).fold(0.0_f32, f32::max);
        if max_area == 0.0 {
            SizeClass::Unknown
        } else if max_area >= LARGE_AREA_THRESHOLD {
            SizeClass::Large
        } else {
            SizeClass::Small
        }
    }
}

impl DetectorBackend for TractBackend {
    fn name(&self) -> &'static str {
        "tract"
    }

    fn supports(&self, capability: DetectionCapability) -> bool {
        matches!(
            capability,
            DetectionCapability::Classification | DetectionCapability::ObjectDetection
        )
    }

    fn detect(&mut self, pixels: &[u8], width: u32, height: u32) -> Result<DetectionResult> {
        self.validate_threshold()?;
        let input = self.build_input(pixels, width, height)?;
        let outputs = self
            .model
            .run(tvec!(input))
            .context("ONNX inference failed")?;
        let detections = self.extract_detections(outputs, width, height)?;
        let confidence = detections
            .iter()
            .map(|d| d.confidence)
            .fold(0.0_f32, f32::max);

        Ok(DetectionResult {
            motion_detected: confidence >= self.confidence_threshold,
            detections,
            confidence,
            size_class: Self::size_class_for(&detections),
        })
    }
}
