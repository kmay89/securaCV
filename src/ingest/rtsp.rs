//! RTSP frame source.
//!
//! This module provides `RtspSource` for ingesting frames from IP cameras via RTSP.
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

#[cfg(feature = "rtsp-gstreamer")]
use anyhow::Context;
use anyhow::Result;
use sha2::{Digest, Sha256};
#[cfg(feature = "rtsp-gstreamer")]
use std::time::{Duration, Instant};

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
/// Uses GStreamer for real RTSP decode, with a synthetic fallback for `stub://` URLs.
pub struct RtspSource {
    backend: RtspBackend,
}

enum RtspBackend {
    Synthetic(SyntheticRtspSource),
    #[cfg(feature = "rtsp-gstreamer")]
    Gstreamer(GstreamerRtspSource),
}

impl RtspSource {
    pub fn new(config: RtspConfig) -> Result<Self> {
        if config.url.starts_with("stub://") {
            Ok(Self {
                backend: RtspBackend::Synthetic(SyntheticRtspSource::new(config)),
            })
        } else {
            #[cfg(feature = "rtsp-gstreamer")]
            {
                Ok(Self {
                    backend: RtspBackend::Gstreamer(GstreamerRtspSource::new(config)?),
                })
            }
            #[cfg(not(feature = "rtsp-gstreamer"))]
            {
                anyhow::bail!("RTSP requires the rtsp-gstreamer feature")
            }
        }
    }

    /// Connect to the RTSP stream.
    pub fn connect(&mut self) -> Result<()> {
        match &mut self.backend {
            RtspBackend::Synthetic(source) => source.connect(),
            #[cfg(feature = "rtsp-gstreamer")]
            RtspBackend::Gstreamer(source) => source.connect(),
        }
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
        match &mut self.backend {
            RtspBackend::Synthetic(source) => source.next_frame(),
            #[cfg(feature = "rtsp-gstreamer")]
            RtspBackend::Gstreamer(source) => source.next_frame(),
        }
    }

    /// Check if the source is healthy.
    pub fn is_healthy(&self) -> bool {
        match &self.backend {
            RtspBackend::Synthetic(source) => source.is_healthy(),
            #[cfg(feature = "rtsp-gstreamer")]
            RtspBackend::Gstreamer(source) => source.is_healthy(),
        }
    }

    /// Get frame statistics.
    pub fn stats(&self) -> RtspStats {
        match &self.backend {
            RtspBackend::Synthetic(source) => source.stats(),
            #[cfg(feature = "rtsp-gstreamer")]
            RtspBackend::Gstreamer(source) => source.stats(),
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
// Synthetic source (stub://) for tests
// ----------------------------------------------------------------------------

struct SyntheticRtspSource {
    config: RtspConfig,
    frame_count: u64,
    /// Simulated "scene" state for synthetic motion detection.
    scene_state: u8,
}

impl SyntheticRtspSource {
    fn new(config: RtspConfig) -> Self {
        Self {
            config,
            frame_count: 0,
            scene_state: 0,
        }
    }

    /// Connect to the RTSP stream.
    ///
    /// Synthetic sources are always "connected".
    fn connect(&mut self) -> Result<()> {
        log::info!("RtspSource: connected to {} (synthetic)", self.config.url);
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        self.frame_count += 1;

        // Coarsen timestamp at capture time (10-minute buckets)
        let timestamp_bucket = TimeBucket::now_10min()?;

        // Generate synthetic pixel data
        let pixels = self.generate_synthetic_pixels();

        // Compute non-invertible feature hash
        let features_hash = compute_features_hash(&pixels, self.frame_count);

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

    fn is_healthy(&self) -> bool {
        true
    }

    fn stats(&self) -> RtspStats {
        RtspStats {
            frames_captured: self.frame_count,
            url: self.config.url.clone(),
        }
    }
}

// ----------------------------------------------------------------------------
// Production RTSP source using GStreamer
// ----------------------------------------------------------------------------

#[cfg(feature = "rtsp-gstreamer")]
struct GstreamerRtspSource {
    config: RtspConfig,
    pipeline: gstreamer::Pipeline,
    appsink: gstreamer_app::AppSink,
    frame_count: u64,
    last_frame_at: Option<Instant>,
    connected_at: Option<Instant>,
    last_error: Option<String>,
}

#[cfg(feature = "rtsp-gstreamer")]
impl GstreamerRtspSource {
    /// Create a new GStreamer RTSP source.
    ///
    /// Production implementation:
    /// 1. Build GStreamer pipeline: rtspsrc ! decodebin ! videoconvert ! appsink
    /// 2. Configure appsink for RGB output
    /// 3. Handle reconnection on stream errors
    fn new(config: RtspConfig) -> Result<Self> {
        gstreamer::init().context("initialize gstreamer")?;

        let pipeline_description = format!(
            "rtspsrc location={} latency=0 ! decodebin ! videoconvert ! video/x-raw,format=RGB ! \
             appsink name=appsink sync=false max-buffers=1 drop=true",
            config.url
        );
        let pipeline = gstreamer::parse_launch(&pipeline_description)
            .context("build RTSP pipeline")?
            .downcast::<gstreamer::Pipeline>()
            .map_err(|_| anyhow::anyhow!("RTSP pipeline is not a Pipeline"))?;

        let appsink = pipeline
            .by_name("appsink")
            .context("appsink element missing from pipeline")?
            .downcast::<gstreamer_app::AppSink>()
            .map_err(|_| anyhow::anyhow!("appsink element has unexpected type"))?;

        let caps = gstreamer::Caps::builder("video/x-raw")
            .field("format", "RGB")
            .build();
        appsink.set_caps(Some(&caps));
        appsink.set_max_buffers(1);
        appsink.set_drop(true);
        appsink.set_sync(false);

        Ok(Self {
            config,
            pipeline,
            appsink,
            frame_count: 0,
            last_frame_at: None,
            connected_at: None,
            last_error: None,
        })
    }

    fn connect(&mut self) -> Result<()> {
        self.pipeline
            .set_state(gstreamer::State::Playing)
            .context("set RTSP pipeline to Playing")?;
        self.connected_at = Some(Instant::now());
        log::info!("RtspSource: connected to {}", self.config.url);
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        self.poll_bus();

        let timeout = self.frame_timeout();
        let sample = self
            .appsink
            .try_pull_sample(timeout)
            .context("pull RTSP sample")?
            .ok_or_else(|| anyhow::anyhow!("RTSP stream stalled"))?;

        let (pixels, width, height) = sample_to_pixels(&sample)?;

        self.frame_count += 1;
        self.last_frame_at = Some(Instant::now());

        let timestamp_bucket = TimeBucket::now_10min()?;
        let features_hash = compute_features_hash(&pixels, self.frame_count);

        Ok(RawFrame::new(
            pixels,
            width,
            height,
            timestamp_bucket,
            features_hash,
        ))
    }

    fn is_healthy(&self) -> bool {
        if self.last_error.is_some() {
            return false;
        }
        let Some(connected_at) = self.connected_at else {
            return false;
        };
        let Some(last_frame_at) = self.last_frame_at else {
            return connected_at.elapsed() <= Duration::from_secs(5);
        };
        last_frame_at.elapsed() <= self.health_grace()
    }

    fn stats(&self) -> RtspStats {
        RtspStats {
            frames_captured: self.frame_count,
            url: self.config.url.clone(),
        }
    }

    fn frame_timeout(&self) -> Duration {
        let base_ms = if self.config.target_fps == 0 {
            500
        } else {
            (1000 / self.config.target_fps).saturating_mul(4)
        };
        Duration::from_millis(base_ms.max(500) as u64)
    }

    fn health_grace(&self) -> Duration {
        let base_ms = if self.config.target_fps == 0 {
            2_000
        } else {
            (1000 / self.config.target_fps).saturating_mul(6)
        };
        Duration::from_millis(base_ms.max(2_000) as u64)
    }

    fn poll_bus(&mut self) {
        let Some(bus) = self.pipeline.bus() else {
            return;
        };
        while let Some(message) = bus.timed_pop(Duration::from_millis(0)) {
            use gstreamer::MessageView;
            match message.view() {
                MessageView::Error(err) => {
                    self.last_error = Some(format!(
                        "gstreamer error from {:?}: {}",
                        err.src().map(|s| s.path_string()),
                        err.error()
                    ));
                }
                MessageView::Eos(..) => {
                    self.last_error = Some("gstreamer reached EOS".to_string());
                }
                _ => {}
            }
        }
    }
}

#[cfg(feature = "rtsp-gstreamer")]
fn sample_to_pixels(sample: &gstreamer::Sample) -> Result<(Vec<u8>, u32, u32)> {
    let buffer = sample.buffer().context("RTSP sample missing buffer")?;
    let caps = sample.caps().context("RTSP sample missing caps")?;
    let info =
        gstreamer_video::VideoInfo::from_caps(caps).context("parse RTSP caps as video info")?;

    let width = info.width() as u32;
    let height = info.height() as u32;
    let row_bytes = (width as usize) * 3;
    let stride = info.stride(0) as usize;

    let map = buffer.map_readable().context("map RTSP buffer")?;
    let data = map.as_slice();

    if stride == row_bytes {
        return Ok((data.to_vec(), width, height));
    }

    let mut pixels = Vec::with_capacity(row_bytes * height as usize);
    for row in 0..height as usize {
        let start = row * stride;
        let end = start + row_bytes;
        pixels.extend_from_slice(
            data.get(start..end)
                .context("RTSP buffer row is out of bounds")?,
        );
    }

    Ok((pixels, width, height))
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
fn compute_features_hash(pixels: &[u8], frame_count: u64) -> [u8; 32] {
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
    hasher.update(frame_count.to_le_bytes());
    hasher.update(rand::random::<u64>().to_le_bytes());

    hasher.finalize().into()
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn stub_config() -> RtspConfig {
        RtspConfig {
            url: "stub://test".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
        }
    }

    #[test]
    fn rtsp_source_produces_frames() -> Result<()> {
        let config = stub_config();
        let mut source = RtspSource::new(config)?;
        source.connect()?;

        let frame = source.next_frame()?;
        assert_eq!(frame.width, 640);
        assert_eq!(frame.height, 480);

        Ok(())
    }

    #[test]
    fn rtsp_source_frames_have_coarse_timestamps() -> Result<()> {
        let config = stub_config();
        let mut source = RtspSource::new(config)?;
        source.connect()?;

        let frame = source.next_frame()?;

        // Timestamp bucket should be at least 5 minutes (300s)
        assert!(frame.timestamp_bucket.size_s >= 300);

        Ok(())
    }

    #[test]
    fn rtsp_source_feature_hashes_are_not_stable() -> Result<()> {
        let config = stub_config();
        let mut source = RtspSource::new(config)?;
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
