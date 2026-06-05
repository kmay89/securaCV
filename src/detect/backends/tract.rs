#![cfg(feature = "backend-tract")]

use std::path::Path;
use std::sync::Arc;

use anyhow::{anyhow, Context, Result};
use tract_onnx::prelude::*;

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::result::{Detection, DetectionResult, ObjectClass, SizeClass};
use crate::detect::yolo::{decode_yolov2, nms, YoloV2Spec};

const LARGE_AREA_THRESHOLD: f32 = 0.2;
const ABSOLUTE_COORD_THRESHOLD: f32 = 1.5;

/// tiny-YOLOv2's fixed square input dimension (416×416).
pub const TINY_YOLOV2_INPUT: u32 = 416;
/// IoU threshold for tiny-YOLOv2 non-max suppression.
const YOLO_NMS_IOU: f32 = 0.45;

/// Default minimum confidence for a detection to be reported. Overridable via
/// [`TractBackend::with_threshold`]; witnessd wires this from `detect.confidence`
/// (config file / `WITNESS_DETECT_CONFIDENCE`). Keep in sync with
/// `config`'s `DEFAULT_DETECT_CONFIDENCE`.
pub const DEFAULT_CONFIDENCE_THRESHOLD: f32 = 0.5;

/// How the raw model output is turned into detections.
enum PostProcess {
    /// The model already produced final boxes (`[N,6]` or separate boxes/scores/classes
    /// tensors); see [`TractBackend::extract_detections`].
    PostNms,
    /// A raw YOLOv2 grid (`[1, B*(5+C), G, G]`) decoded + NMS-suppressed on the host.
    YoloV2(YoloV2Spec),
}

/// Tract-based backend for ONNX inference.
///
/// Loads a local model file and runs inference on RGB frames. No network I/O and no disk writes
/// beyond model loading.
///
/// HOST-ONLY: runs in `witnessd` on a Pi/x86 host. It does NOT run on the ESP32-S3 — that path
/// uses the Grove Vision AI V2 (SSCMA) board (`firmware/projects/canary-vision`).
pub struct TractBackend {
    model: Arc<TypedRunnableModel>,
    width: u32,
    height: u32,
    confidence_threshold: f32,
    /// Per-pixel scale applied when building the input tensor (`1/255` to normalize, `1.0` for
    /// models like tiny-YOLOv2 that expect raw 0–255 values).
    input_scale: f32,
    /// When true, incoming frames are resized to the model's fixed input (e.g. tiny-YOLOv2's
    /// 416×416) instead of being required to match it exactly.
    resize: bool,
    post: PostProcess,
}

impl TractBackend {
    fn load_runnable(
        model_path: &Path,
        width: u32,
        height: u32,
    ) -> Result<Arc<TypedRunnableModel>> {
        tract_onnx::onnx()
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
            .context("failed to build runnable ONNX model")
    }

    /// Load an ONNX model whose output is already post-NMS (`[N,6]` or separate
    /// boxes/scores/classes tensors). Frames must match `width`×`height` exactly.
    pub fn new<P: AsRef<Path>>(model_path: P, width: u32, height: u32) -> Result<Self> {
        let model = Self::load_runnable(model_path.as_ref(), width, height)?;
        Ok(Self {
            model,
            width,
            height,
            confidence_threshold: DEFAULT_CONFIDENCE_THRESHOLD,
            input_scale: 1.0 / 255.0,
            resize: false,
            post: PostProcess::PostNms,
        })
    }

    /// Load the tiny-YOLOv2 (VOC) detector: fixed 416×416 input, raw 0–255 pixel values, and
    /// host-side YOLOv2 grid decode + NMS. Incoming frames of any size are resized to 416×416.
    ///
    /// This is the model fetched by `scripts/fetch_detection_model.sh`.
    pub fn tiny_yolov2<P: AsRef<Path>>(model_path: P) -> Result<Self> {
        let model = Self::load_runnable(model_path.as_ref(), TINY_YOLOV2_INPUT, TINY_YOLOV2_INPUT)?;
        Ok(Self {
            model,
            width: TINY_YOLOV2_INPUT,
            height: TINY_YOLOV2_INPUT,
            confidence_threshold: DEFAULT_CONFIDENCE_THRESHOLD,
            input_scale: 1.0,
            resize: true,
            post: PostProcess::YoloV2(YoloV2Spec::tiny_yolov2_voc()),
        })
    }

    /// Override the default confidence threshold.
    pub fn with_threshold(mut self, threshold: f32) -> Self {
        self.confidence_threshold = threshold;
        self
    }

    /// Bilinear-resize a packed RGB frame to the model's input dimensions. Used by the YOLO path
    /// so a camera frame of any resolution can feed tiny-YOLOv2's fixed 416×416 input.
    fn resize_to_input(&self, pixels: &[u8], width: u32, height: u32) -> Result<Vec<u8>> {
        let expected = (width as usize)
            .checked_mul(height as usize)
            .and_then(|v| v.checked_mul(3))
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;
        if pixels.len() != expected {
            return Err(anyhow!(
                "expected {} RGB bytes for {}x{} frame, received {}",
                expected,
                width,
                height,
                pixels.len()
            ));
        }
        Ok(resize_rgb(
            pixels,
            width as usize,
            height as usize,
            self.width as usize,
            self.height as usize,
        ))
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

        let scale = self.input_scale;
        let width = width as usize;
        let input = tract_ndarray::Array4::from_shape_fn(
            (1, 3, height as usize, width),
            |(_, channel, y, x)| {
                let idx = (y * width + x) * 3 + channel;
                pixels[idx] as f32 * scale
            },
        );

        Ok(input.into_tensor())
    }

    /// Decode a YOLOv2 grid output into final detections (decode + per-class NMS).
    fn decode_yolo(&self, outputs: &TVec<TValue>, spec: &YoloV2Spec) -> Result<Vec<Detection>> {
        let out = outputs
            .first()
            .ok_or_else(|| anyhow!("yolo model produced no output"))?;
        let (grid_h, grid_w) = match out.shape() {
            [1, _channels, gh, gw] => (*gh, *gw),
            other => {
                return Err(anyhow!(
                    "unexpected yolo output shape {:?}, expected [1, C, H, W]",
                    other
                ))
            }
        };
        let view = out
            .to_plain_array_view::<f32>()
            .context("yolo output tensor was not f32")?;
        let data = view
            .as_slice()
            .ok_or_else(|| anyhow!("yolo output tensor is not contiguous"))?;
        let decoded = decode_yolov2(data, grid_w, grid_h, spec, self.confidence_threshold);
        Ok(nms(decoded, YOLO_NMS_IOU))
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
        outputs: TVec<TValue>,
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
        output: &TValue,
        frame_width: u32,
        frame_height: u32,
    ) -> Result<Vec<Detection>> {
        let shape = output.shape();
        let view = output
            .to_plain_array_view::<f32>()
            .context("combined output tensor was not f32")?;
        let data = view
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
        outputs: &TVec<TValue>,
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

    fn extract_boxes(output: &TValue) -> Result<Vec<[f32; 4]>> {
        let shape = output.shape();
        let view = output
            .to_plain_array_view::<f32>()
            .context("boxes tensor was not f32")?;
        let data = view
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

    fn extract_scores(output: &TValue) -> Result<Vec<f32>> {
        let shape = output.shape();
        let view = output
            .to_plain_array_view::<f32>()
            .context("scores tensor was not f32")?;
        let data = view
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

    fn extract_class_ids(output: &TValue) -> Result<Vec<i64>> {
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

        if let Ok(view) = output.to_plain_array_view::<i64>() {
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
        } else if let Ok(view) = output.to_plain_array_view::<f32>() {
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
        // The YOLO path resizes any camera frame to the model's fixed input; the post-NMS path
        // keeps strict size matching.
        let input = if self.resize {
            let resized = self.resize_to_input(pixels, width, height)?;
            self.build_input(&resized, self.width, self.height)?
        } else {
            self.build_input(pixels, width, height)?
        };
        let outputs = self
            .model
            .run(tvec!(input.into()))
            .context("ONNX inference failed")?;
        let detections = match &self.post {
            PostProcess::PostNms => self.extract_detections(outputs, width, height)?,
            PostProcess::YoloV2(spec) => self.decode_yolo(&outputs, spec)?,
        };
        let size_class = Self::size_class_for(&detections);
        let confidence = detections
            .iter()
            .map(|d| d.confidence)
            .fold(0.0_f32, f32::max);

        Ok(DetectionResult {
            // `detections` is already filtered to entries at/above the threshold, so the presence of
            // any detection is the signal. Deriving this from `confidence >= threshold` would
            // spuriously fire on an empty set when the threshold is 0.0 (the max-fold default).
            motion_detected: !detections.is_empty(),
            detections,
            confidence,
            size_class,
        })
    }
}

/// Bilinear-resize a packed RGB8 image (`src`, `sw`×`sh`) to `dw`×`dh`. Pure Rust (no image
/// crate) so the detection backend keeps a minimal dependency surface. Center-aligned sampling.
fn resize_rgb(src: &[u8], sw: usize, sh: usize, dw: usize, dh: usize) -> Vec<u8> {
    if (sw, sh) == (dw, dh) {
        return src.to_vec();
    }
    let mut out = vec![0u8; dw * dh * 3];
    let scale_x = sw as f32 / dw as f32;
    let scale_y = sh as f32 / dh as f32;
    for dy in 0..dh {
        let fy = ((dy as f32 + 0.5) * scale_y - 0.5).max(0.0);
        let y0 = (fy.floor() as usize).min(sh - 1);
        let y1 = (y0 + 1).min(sh - 1);
        let wy = fy - y0 as f32;
        for dx in 0..dw {
            let fx = ((dx as f32 + 0.5) * scale_x - 0.5).max(0.0);
            let x0 = (fx.floor() as usize).min(sw - 1);
            let x1 = (x0 + 1).min(sw - 1);
            let wx = fx - x0 as f32;
            for c in 0..3 {
                let p = |x: usize, y: usize| src[(y * sw + x) * 3 + c] as f32;
                let top = p(x0, y0) * (1.0 - wx) + p(x1, y0) * wx;
                let bot = p(x0, y1) * (1.0 - wx) + p(x1, y1) * wx;
                let v = top * (1.0 - wy) + bot * wy;
                out[(dy * dw + dx) * 3 + c] = v.round().clamp(0.0, 255.0) as u8;
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resize_identity_when_same_dims() {
        let src = vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]; // 2x2 RGB
        assert_eq!(resize_rgb(&src, 2, 2, 2, 2), src);
    }

    #[test]
    fn resize_downscale_preserves_channels_and_size() {
        // 4x4 solid red → 2x2 must stay solid red, correct length.
        let src: Vec<u8> = (0..16).flat_map(|_| [200u8, 10, 5]).collect();
        let out = resize_rgb(&src, 4, 4, 2, 2);
        assert_eq!(out.len(), 2 * 2 * 3);
        for px in out.chunks(3) {
            assert_eq!(px, [200, 10, 5]);
        }
    }

    #[test]
    fn resize_upscale_endpoints_match_source_corners() {
        // 2x2 with distinct corners; upscaled corners should match the source corners.
        let src = vec![
            10, 0, 0, /* (0,0) */ 20, 0, 0, /* (1,0) */
            30, 0, 0, /* (0,1) */ 40, 0, 0, /* (1,1) */
        ];
        let out = resize_rgb(&src, 2, 2, 4, 4);
        assert_eq!(out.len(), 4 * 4 * 3);
        let at = |x: usize, y: usize| out[(y * 4 + x) * 3];
        assert_eq!(at(0, 0), 10);
        assert_eq!(at(3, 0), 20);
        assert_eq!(at(0, 3), 30);
        assert_eq!(at(3, 3), 40);
    }
}
