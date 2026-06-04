use sha2::{Digest, Sha256};

use anyhow::{Context, Result};

use crate::frame::RawFrame;
use crate::TimeBucket;

/// Compute non-invertible feature hash from pixels.
///
/// This hash is used for correlation tokens and MUST NOT:
/// - Be invertible back to pixel data
/// - Contain identity-bearing information (faces, plates, etc.)
/// - Be stable across long time windows
///
/// In production, this would derive from:
/// - Color histograms (coarse)
/// - Motion vector magnitudes (not directions)
/// - Texture energy (not structure)
pub(crate) fn compute_features_hash(pixels: &[u8], frame_count: u64) -> [u8; 32] {
    let mut hasher = Sha256::new();

    // Coarse color histogram (very lossy)
    let mut histogram = [0u32; 8]; // 8 bins
    for &p in pixels.iter().step_by(300) {
        // Sample every 100th pixel
        histogram[(p / 32) as usize] += 1;
    }
    for count in &histogram {
        hasher.update(count.to_le_bytes());
    }

    // Add frame-local noise to prevent cross-frame stability
    hasher.update(frame_count.to_le_bytes());
    hasher.update(rand::random::<u64>().to_le_bytes());

    hasher.finalize().into()
}

/// The single capture-time privacy gate every frame source funnels through.
///
/// Coarsens wall-clock time into a 10-minute `TimeBucket` and computes the
/// non-invertible feature hash *at capture time*, then assembles the `RawFrame`.
/// Centralizing this is the guard against the RTSP (GStreamer / FFmpeg) and file
/// (`file` / `file_ffmpeg`) backends silently diverging on the privacy contract
/// (flag report F-11): change the bucket granularity or the hash inputs here and
/// every backend that routes through this helper moves together, rather than each
/// backend re-implementing the sequence and drifting apart.
///
/// (The feature-gated `esp32` / `v4l2` sources still inline the equivalent sequence
/// and should be migrated onto this helper too — tracked under F-11.)
pub(crate) fn raw_frame_at_capture(
    pixels: Vec<u8>,
    width: u32,
    height: u32,
    frame_count: u64,
) -> Result<RawFrame> {
    // Every backend hands us tightly-packed RGB24 (3 bytes/pixel). Validate that here,
    // at the single capture gate, so a decoder that ever produces a mis-sized buffer
    // (wrong pixel format, mishandled stride) fails cleanly instead of panicking or
    // mis-indexing downstream in detection / `InferenceView`.
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|wh| wh.checked_mul(3))
        .context("frame dimensions overflow usize")?;
    if pixels.len() != expected {
        anyhow::bail!(
            "frame buffer size {} does not match {}x{} RGB24 ({} bytes)",
            pixels.len(),
            width,
            height,
            expected
        );
    }

    let timestamp_bucket = TimeBucket::now_10min()?;
    let features_hash = compute_features_hash(&pixels, frame_count);
    Ok(RawFrame::new(
        pixels,
        width,
        height,
        timestamp_bucket,
        features_hash,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn raw_frame_at_capture_enforces_the_capture_contract() {
        let pixels = vec![7u8; 640 * 480 * 3];
        let frame = raw_frame_at_capture(pixels.clone(), 640, 480, 1).expect("capture");

        // Dimensions and pixels are carried through unchanged.
        assert_eq!(frame.width, 640);
        assert_eq!(frame.height, 480);
        assert_eq!(frame.byte_len(), pixels.len());

        // Timestamp is coarsened to at least a 5-minute bucket (no fine timestamps).
        assert!(frame.timestamp_bucket.size_s >= 300);

        // The feature hash is frame-local (random salt), so two captures of identical
        // pixels do not yield a stable, cross-frame-comparable correlation token.
        let again = raw_frame_at_capture(pixels, 640, 480, 1).expect("capture");
        assert_ne!(
            frame.inference_view().features_hash(),
            again.inference_view().features_hash()
        );
    }

    #[test]
    fn raw_frame_at_capture_rejects_mismatched_buffer() {
        // 10x10 RGB24 needs 300 bytes; a buffer of any other size is malformed and
        // must be rejected at the gate rather than flowing downstream.
        // `RawFrame` has no `Debug` (no byte exposure), so match rather than unwrap_err.
        match raw_frame_at_capture(vec![0u8; 299], 10, 10, 1) {
            Ok(_) => panic!("expected mismatched-buffer rejection"),
            Err(e) => assert!(e.to_string().contains("does not match"), "{e}"),
        }
    }
}
