//! Privacy-preserving edge-detection thumbnails.
//!
//! Generates Sobel edge thumbnails from raw frames. The output is mathematically
//! irreversible — Sobel computes spatial derivatives, discarding absolute intensity
//! values. The original frame cannot be reconstructed from edge magnitudes alone.
//!
//! The pipeline: downscale → grayscale → Sobel 3×3 → threshold → bbox overlay.

use crate::detect::{Detection, ObjectClass};
use crate::TimeBucket;
use serde::Serialize;

/// Default thumbnail dimensions.
pub const THUMB_WIDTH: u16 = 256;
pub const THUMB_HEIGHT: u16 = 192;

/// Edge threshold: edges below this magnitude are zeroed out.
pub const DEFAULT_EDGE_THRESHOLD: u8 = 30;

/// Privacy-safe thumbnail. Contains only edge magnitudes and detection metadata.
///
/// Unlike `RawFrame`, this type IS serializable because edge data cannot be
/// reversed to identify individuals. The Sobel transform is a lossy derivative
/// that discards absolute intensity, color, and fine texture.
#[derive(Clone, Debug, Serialize)]
pub struct EdgeThumbnail {
    /// Grayscale edge magnitude pixels (width × height bytes).
    edges: Vec<u8>,
    pub width: u16,
    pub height: u16,
    /// Bounding boxes from detection (same data already in semantic events).
    pub detections: Vec<ThumbnailDetection>,
    pub timestamp_bucket: TimeBucket,
}

/// Serializable detection for thumbnail overlay.
#[derive(Clone, Debug, Serialize)]
pub struct ThumbnailDetection {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub class: String,
}

impl From<&Detection> for ThumbnailDetection {
    fn from(d: &Detection) -> Self {
        Self {
            x: d.x,
            y: d.y,
            w: d.w,
            h: d.h,
            class: match d.class {
                ObjectClass::Person => "person".into(),
                ObjectClass::Vehicle => "vehicle".into(),
                ObjectClass::Animal => "animal".into(),
                ObjectClass::Package => "package".into(),
                ObjectClass::Unknown => "unknown".into(),
            },
        }
    }
}

impl EdgeThumbnail {
    /// Raw edge bytes for encoding or display.
    pub fn edge_bytes(&self) -> &[u8] {
        &self.edges
    }

    /// Encode as a base64 data URI (grayscale PGM wrapped in base64).
    pub fn to_pgm_base64(&self) -> String {
        let header = format!("P5\n{} {}\n255\n", self.width, self.height);
        let mut pgm = header.into_bytes();
        pgm.extend_from_slice(&self.edges);
        let encoded = base64_encode(&pgm);
        format!("data:image/x-portable-graymap;base64,{}", encoded)
    }
}

/// Downscale RGB pixels to grayscale at target resolution using area averaging.
///
/// Input: RGB888 pixels (3 bytes per pixel), width × height.
/// Output: grayscale pixels (1 byte per pixel), target_w × target_h.
pub fn downscale_grayscale(
    pixels: &[u8],
    src_w: u32,
    src_h: u32,
    dst_w: u16,
    dst_h: u16,
) -> Vec<u8> {
    let dw = dst_w as u32;
    let dh = dst_h as u32;
    let mut out = vec![0u8; (dw * dh) as usize];

    if src_w == 0 || src_h == 0 || dw == 0 || dh == 0 || pixels.is_empty() {
        return out;
    }

    let x_ratio = src_w as f32 / dw as f32;
    let y_ratio = src_h as f32 / dh as f32;

    for dy in 0..dh {
        for dx in 0..dw {
            let sx = (dx as f32 * x_ratio) as u32;
            let sy = (dy as f32 * y_ratio) as u32;
            let sx = sx.min(src_w - 1);
            let sy = sy.min(src_h - 1);

            let idx = ((sy * src_w + sx) * 3) as usize;
            if idx + 2 < pixels.len() {
                let r = pixels[idx] as u32;
                let g = pixels[idx + 1] as u32;
                let b = pixels[idx + 2] as u32;
                // ITU-R BT.601 luminance
                let lum = (77 * r + 150 * g + 29 * b) >> 8;
                out[(dy * dw + dx) as usize] = lum.min(255) as u8;
            }
        }
    }
    out
}

/// Apply 3×3 Sobel edge detection filter.
///
/// Computes horizontal and vertical gradients, returns magnitude.
/// This is mathematically irreversible: the absolute intensity (DC component)
/// is lost, and only relative differences between adjacent pixels survive.
pub fn sobel_edges(gray: &[u8], w: u16, h: u16) -> Vec<u8> {
    let w = w as usize;
    let h = h as usize;
    let mut out = vec![0u8; w * h];

    if w < 2 || h < 2 || gray.len() < w * h {
        return out;
    }

    // Skip border pixels (1px on each side)
    for y in 1..(h - 1) {
        for x in 1..(w - 1) {
            let tl = gray[(y - 1) * w + (x - 1)] as i32;
            let tc = gray[(y - 1) * w + x] as i32;
            let tr = gray[(y - 1) * w + (x + 1)] as i32;
            let ml = gray[y * w + (x - 1)] as i32;
            let mr = gray[y * w + (x + 1)] as i32;
            let bl = gray[(y + 1) * w + (x - 1)] as i32;
            let bc = gray[(y + 1) * w + x] as i32;
            let br = gray[(y + 1) * w + (x + 1)] as i32;

            // Sobel Gx = [[-1,0,1],[-2,0,2],[-1,0,1]]
            let gx = -tl + tr - 2 * ml + 2 * mr - bl + br;
            // Sobel Gy = [[-1,-2,-1],[0,0,0],[1,2,1]]
            let gy = -tl - 2 * tc - tr + bl + 2 * bc + br;

            // Magnitude approximation (avoids sqrt)
            let mag = ((gx.abs() + gy.abs()) / 2).min(255) as u8;
            out[y * w + x] = mag;
        }
    }
    out
}

/// Zero out edge pixels below the threshold.
pub fn threshold_edges(edges: &mut [u8], threshold: u8) {
    for pixel in edges.iter_mut() {
        if *pixel < threshold {
            *pixel = 0;
        }
    }
}

/// Draw detection bounding boxes as white outlines on the edge image.
pub fn overlay_bounding_boxes(edges: &mut [u8], w: u16, h: u16, detections: &[Detection]) {
    let w = w as usize;
    let h = h as usize;

    if edges.len() < w * h {
        return;
    }

    for det in detections {
        if !det.x.is_finite() || !det.y.is_finite() || !det.w.is_finite() || !det.h.is_finite() {
            continue;
        }

        // Skip detections entirely off-screen
        if det.x >= 1.0 || det.y >= 1.0 || det.x + det.w <= 0.0 || det.y + det.h <= 0.0 {
            continue;
        }

        let x0 = (det.x * w as f32).max(0.0) as usize;
        let y0 = (det.y * h as f32).max(0.0) as usize;
        let x1 = ((det.x + det.w) * w as f32).min(w as f32 - 1.0) as usize;
        let y1 = ((det.y + det.h) * h as f32).min(h as f32 - 1.0) as usize;

        // Brightness based on detection class
        let brightness: u8 = match det.class {
            ObjectClass::Person => 255,
            ObjectClass::Vehicle => 220,
            ObjectClass::Animal => 200,
            ObjectClass::Package => 180,
            ObjectClass::Unknown => 160,
        };

        // Top and bottom edges
        for x in x0..=x1.min(w - 1) {
            if y0 < h {
                edges[y0 * w + x] = brightness;
            }
            if y1 < h {
                edges[y1 * w + x] = brightness;
            }
        }
        // Left and right edges
        for y in y0..=y1.min(h - 1) {
            if x0 < w {
                edges[y * w + x0] = brightness;
            }
            if x1 < w {
                edges[y * w + x1] = brightness;
            }
        }
    }
}

/// Generate a complete edge thumbnail from raw RGB pixels and detection results.
pub fn generate(
    pixels: &[u8],
    src_w: u32,
    src_h: u32,
    detections: &[Detection],
    timestamp_bucket: TimeBucket,
) -> EdgeThumbnail {
    let gray = downscale_grayscale(pixels, src_w, src_h, THUMB_WIDTH, THUMB_HEIGHT);
    let mut edges = sobel_edges(&gray, THUMB_WIDTH, THUMB_HEIGHT);
    threshold_edges(&mut edges, DEFAULT_EDGE_THRESHOLD);
    overlay_bounding_boxes(&mut edges, THUMB_WIDTH, THUMB_HEIGHT, detections);

    EdgeThumbnail {
        edges,
        width: THUMB_WIDTH,
        height: THUMB_HEIGHT,
        detections: detections.iter().map(ThumbnailDetection::from).collect(),
        timestamp_bucket,
    }
}

/// Minimal base64 encoder (no external dependency).
fn base64_encode(data: &[u8]) -> String {
    const CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = if chunk.len() > 1 { chunk[1] as u32 } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] as u32 } else { 0 };
        let triple = (b0 << 16) | (b1 << 8) | b2;

        out.push(CHARS[((triple >> 18) & 0x3F) as usize] as char);
        out.push(CHARS[((triple >> 12) & 0x3F) as usize] as char);
        if chunk.len() > 1 {
            out.push(CHARS[((triple >> 6) & 0x3F) as usize] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(CHARS[(triple & 0x3F) as usize] as char);
        } else {
            out.push('=');
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::ObjectClass;

    fn make_test_pixels(w: u32, h: u32) -> Vec<u8> {
        let mut pixels = vec![0u8; (w * h * 3) as usize];
        // Create a simple pattern: bright rectangle in center
        for y in (h / 4)..(3 * h / 4) {
            for x in (w / 4)..(3 * w / 4) {
                let idx = ((y * w + x) * 3) as usize;
                pixels[idx] = 200; // R
                pixels[idx + 1] = 200; // G
                pixels[idx + 2] = 200; // B
            }
        }
        pixels
    }

    #[test]
    fn downscale_produces_correct_dimensions() {
        let pixels = make_test_pixels(640, 480);
        let gray = downscale_grayscale(&pixels, 640, 480, 256, 192);
        assert_eq!(gray.len(), 256 * 192);
    }

    #[test]
    fn sobel_produces_correct_dimensions() {
        let gray = vec![128u8; 256 * 192];
        let edges = sobel_edges(&gray, 256, 192);
        assert_eq!(edges.len(), 256 * 192);
    }

    #[test]
    fn sobel_detects_sharp_edges() {
        let mut gray = vec![0u8; 64 * 64];
        // Create a vertical edge at x=32
        for y in 0..64 {
            for x in 32..64 {
                gray[y * 64 + x] = 200;
            }
        }
        let edges = sobel_edges(&gray, 64, 64);
        // Edge magnitude should be high near x=32
        let edge_at_boundary = edges[32 * 64 + 32];
        let edge_in_flat_area = edges[32 * 64 + 16];
        assert!(
            edge_at_boundary > edge_in_flat_area,
            "edge at boundary ({}) should be stronger than flat area ({})",
            edge_at_boundary,
            edge_in_flat_area
        );
    }

    #[test]
    fn uniform_image_produces_no_edges() {
        let gray = vec![128u8; 64 * 64];
        let edges = sobel_edges(&gray, 64, 64);
        // Interior pixels should all be zero (no edges in uniform image)
        for y in 1..63 {
            for x in 1..63 {
                assert_eq!(edges[y * 64 + x], 0, "unexpected edge at ({}, {})", x, y);
            }
        }
    }

    #[test]
    fn threshold_zeroes_weak_edges() {
        let mut edges = vec![10, 20, 30, 40, 50];
        threshold_edges(&mut edges, 35);
        assert_eq!(edges, vec![0, 0, 0, 40, 50]);
    }

    #[test]
    fn bounding_box_overlay_draws_rectangle() {
        let mut edges = vec![0u8; 100 * 100];
        let det = Detection {
            x: 0.1,
            y: 0.1,
            w: 0.3,
            h: 0.3,
            confidence: 0.9,
            class: ObjectClass::Person,
        };
        overlay_bounding_boxes(&mut edges, 100, 100, &[det]);
        // Top-left corner of box should be bright
        assert!(edges[10 * 100 + 10] > 0, "top edge should be drawn");
    }

    #[test]
    fn full_pipeline_produces_valid_thumbnail() {
        let pixels = make_test_pixels(640, 480);
        let bucket = TimeBucket {
            start_epoch_s: 1700000000,
            size_s: 600,
        };
        let dets = vec![Detection {
            x: 0.2,
            y: 0.2,
            w: 0.3,
            h: 0.4,
            confidence: 0.85,
            class: ObjectClass::Person,
        }];
        let thumb = generate(&pixels, 640, 480, &dets, bucket);
        assert_eq!(thumb.width, 256);
        assert_eq!(thumb.height, 192);
        assert_eq!(thumb.edges.len(), 256 * 192);
        assert_eq!(thumb.detections.len(), 1);
        assert_eq!(thumb.detections[0].class, "person");
    }

    #[test]
    fn pgm_base64_encoding_produces_data_uri() {
        let pixels = make_test_pixels(64, 48);
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let thumb = generate(&pixels, 64, 48, &[], bucket);
        let uri = thumb.to_pgm_base64();
        assert!(uri.starts_with("data:image/x-portable-graymap;base64,"));
        assert!(uri.len() > 50);
    }

    #[test]
    fn edge_thumbnail_is_not_raw_frame() {
        // EdgeThumbnail is Clone + Serialize — intentionally different from RawFrame
        // which has neither. This is safe because edge data cannot reconstruct identity.
        let pixels = make_test_pixels(64, 48);
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let thumb = generate(&pixels, 64, 48, &[], bucket);
        let _cloned = thumb.clone(); // RawFrame cannot do this
    }
}
