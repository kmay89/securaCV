//! RTSP frame source.
//!
//! This module provides `RtspSource` for ingesting frames from IP cameras via RTSP.
//!
//! MVP: This is a stub that generates synthetic frames for testing.
//! Production: Would use gstreamer or ffmpeg bindings for real RTSP decode.
//!
//! The RTSP source is responsible for:
//! - Connecting to camera streams
//! - Decoding video frames
//! - Coarsening timestamps at capture time
//! - Computing feature hashes at capture time
//! - Producing `RawFrame` instances
//!
//! The RTSP source MUST NOT:
//! - Store decoded frames to disk
//! - Forward raw frames over network
//! - Retain frames beyond handoff to FrameBuffer

use anyhow::Result;
use sha2::{Digest, Sha256};

use crate::frame::RawFrame;
use crate::TimeBucket;

/// Configuration for an RTSP source.
#[derive(Clone, Debug)]
pub struct RtspConfig {
    /// RTSP URL (e.g., "rtsp://192.168.1.100:554/stream")
    pub url: String,
    /// Target frame rate (frames per second). Source will decimate to this rate.
    pub target_fps: u32,
    /// Frame width (for synthetic frames in MVP).
    pub width: u32,
    /// Frame height (for synthetic frames in MVP).
    pub height: u32,
}

impl Default for RtspConfig {
    fn default() -> Self {
        Self {
            url: "rtsp://localhost:554/stream".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
        }
    }
}

/// RTSP frame source.
///
/// MVP: Generates synthetic frames for testing.
/// Production: Would decode actual RTSP streams.
pub struct RtspSource {
    config: RtspConfig,
    frame_count: u64,
    /// Simulated "scene" state for synthetic motion detection.
    scene_state: u8,
}

impl RtspSource {
    pub fn new(config: RtspConfig) -> Self {
        Self {
            config,
            frame_count: 0,
            scene_state: 0,
        }
    }

    /// Connect to the RTSP stream.
    ///
    /// MVP: No-op (synthetic frames don't need connection).
    /// Production: Would establish RTSP session.
    pub fn connect(&mut self) -> Result<()> {
        // MVP: synthetic source is always "connected"
        log::info!("RtspSource: connected to {} (synthetic)", self.config.url);
        Ok(())
    }

    /// Capture the next frame.
    ///
    /// This method:
    /// 1. Decodes the next frame from the stream (or generates synthetic)
    /// 2. Coarsens the timestamp into a bucket
    /// 3. Computes a non-invertible feature hash
    /// 4. Returns a `RawFrame`
    ///
    /// The returned `RawFrame` has private pixel data that modules cannot access directly.
    pub fn next_frame(&mut self) -> Result<RawFrame> {
        self.frame_count += 1;

        // Coarsen timestamp at capture time (10-minute buckets)
        let timestamp_bucket = TimeBucket::now_10min()?;

        // Generate synthetic pixel data
        // In production, this would be actual decoded pixels
        let pixels = self.generate_synthetic_pixels();

        // Compute non-invertible feature hash
        // In production, this would derive from color histograms, motion vectors, etc.
        // NOT from identity-bearing features like faces or plates.
        let features_hash = self.compute_features_hash(&pixels);

        Ok(RawFrame::new(
            pixels,
            self.config.width,
            self.config.height,
            timestamp_bucket,
            features_hash,
        ))
    }

    /// Generate synthetic pixel data for testing.
    ///
    /// Simulates a scene with occasional "motion events":
    /// - Most frames are static background
    /// - Occasionally the scene changes (simulating object entry)
    fn generate_synthetic_pixels(&mut self) -> Vec<u8> {
        let pixel_count = (self.config.width * self.config.height * 3) as usize; // RGB

        // Change scene state occasionally to simulate motion
        if self.frame_count.is_multiple_of(50) {
            self.scene_state = self.scene_state.wrapping_add(1);
        }

        // Generate pixels based on scene state
        // This is intentionally simple - just fills with a pattern
        let mut pixels = vec![0u8; pixel_count];
        for (i, pixel) in pixels.iter_mut().enumerate() {
            // Mix frame count, scene state, and position for variation
            *pixel = ((i as u64 + self.frame_count + self.scene_state as u64) % 256) as u8;
        }

        pixels
    }

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
    ///
    /// For MVP, we use a simple hash with added noise to prevent stability.
    fn compute_features_hash(&self, pixels: &[u8]) -> [u8; 32] {
        let mut hasher = Sha256::new();

        // Coarse color histogram (very lossy)
        let mut histogram = [0u32; 8]; // 8 bins
        for &p in pixels.iter().step_by(100) {
            // Sample every 100th pixel
            histogram[(p / 32) as usize] += 1;
        }
        for count in &histogram {
            hasher.update(count.to_le_bytes());
        }

        // Add frame-local noise to prevent cross-frame stability
        hasher.update(self.frame_count.to_le_bytes());
        hasher.update(rand::random::<u64>().to_le_bytes());

        hasher.finalize().into()
    }

    /// Check if the source is healthy.
    pub fn is_healthy(&self) -> bool {
        // MVP: synthetic source is always healthy
        true
    }

    /// Get frame statistics.
    pub fn stats(&self) -> RtspStats {
        RtspStats {
            frames_captured: self.frame_count,
            url: self.config.url.clone(),
        }
    }
}

/// Statistics for an RTSP source.
#[derive(Clone, Debug)]
pub struct RtspStats {
    pub frames_captured: u64,
    pub url: String,
}

// ----------------------------------------------------------------------------
// Placeholder for production RTSP (gstreamer/ffmpeg)
// ----------------------------------------------------------------------------

/// Production RTSP source using GStreamer.
///
/// NOT IMPLEMENTED in MVP. This is a placeholder showing the intended interface.
#[allow(dead_code)]
pub struct GstreamerRtspSource {
    // Would contain:
    // - GStreamer pipeline
    // - Appsink for frame extraction
    // - Decoder state
}

#[allow(dead_code)]
impl GstreamerRtspSource {
    /// Create a new GStreamer RTSP source.
    ///
    /// Production implementation would:
    /// 1. Build GStreamer pipeline: rtspsrc ! decodebin ! videoconvert ! appsink
    /// 2. Configure appsink for RGB output
    /// 3. Handle reconnection on stream errors
    pub fn new(_config: RtspConfig) -> Result<Self> {
        anyhow::bail!("GStreamer RTSP not implemented in MVP")
    }
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rtsp_source_produces_frames() -> Result<()> {
        let config = RtspConfig::default();
        let mut source = RtspSource::new(config);
        source.connect()?;

        let frame = source.next_frame()?;
        assert_eq!(frame.width, 640);
        assert_eq!(frame.height, 480);

        Ok(())
    }

    #[test]
    fn rtsp_source_frames_have_coarse_timestamps() -> Result<()> {
        let config = RtspConfig::default();
        let mut source = RtspSource::new(config);
        source.connect()?;

        let frame = source.next_frame()?;

        // Timestamp bucket should be at least 5 minutes (300s)
        assert!(frame.timestamp_bucket.size_s >= 300);

        Ok(())
    }

    #[test]
    fn rtsp_source_feature_hashes_are_not_stable() -> Result<()> {
        let config = RtspConfig::default();
        let mut source = RtspSource::new(config);
        source.connect()?;

        // Capture two frames
        let frame1 = source.next_frame()?;
        let frame2 = source.next_frame()?;

        // Feature hashes should differ (noise injection)
        let view1 = frame1.inference_view();
        let view2 = frame2.inference_view();

        assert_ne!(
            view1.features_hash(),
            view2.features_hash(),
            "feature hashes must not be stable across frames"
        );

        Ok(())
    }
}
