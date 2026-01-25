//! V4L2 frame source.
//!
//! This module provides `V4l2Source` for ingesting frames from local V4L2 devices.
//!
//! The V4L2 source is responsible for:
//! - Connecting to a local device node (e.g., /dev/video0)
//! - Capturing frames in-memory
//! - Coarsening timestamps at capture time
//! - Computing feature hashes at capture time
//! - Producing `RawFrame` instances
//!
//! The V4L2 source MUST NOT:
//! - Store captured frames to disk
//! - Forward raw frames over network
//! - Retain frames beyond handoff to FrameBuffer

use anyhow::{Context, Result};
use ouroboros::self_referencing;
use std::time::{Duration, Instant};

use super::compute_features_hash;
use crate::frame::RawFrame;
use crate::TimeBucket;

/// Configuration for a V4L2 source.
#[derive(Clone, Debug)]
pub struct V4l2Config {
    /// Device path (e.g., "/dev/video0")
    pub device: String,
    /// Target frame rate (frames per second). Source will decimate to this rate.
    pub target_fps: u32,
    /// Preferred frame width.
    pub width: u32,
    /// Preferred frame height.
    pub height: u32,
}

impl Default for V4l2Config {
    fn default() -> Self {
        Self {
            device: "/dev/video0".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
        }
    }
}

/// V4L2 frame source.
///
/// Uses libv4l for real devices, with a synthetic fallback for `stub://` paths.
pub struct V4l2Source {
    backend: V4l2Backend,
}

enum V4l2Backend {
    Synthetic(SyntheticV4l2Source),
    Device(DeviceV4l2Source),
}

impl V4l2Source {
    pub fn new(config: V4l2Config) -> Result<Self> {
        if config.device.starts_with("stub://") {
            Ok(Self {
                backend: V4l2Backend::Synthetic(SyntheticV4l2Source::new(config)),
            })
        } else {
            Ok(Self {
                backend: V4l2Backend::Device(DeviceV4l2Source::new(config)?),
            })
        }
    }

    /// Connect to the V4L2 device.
    pub fn connect(&mut self) -> Result<()> {
        match &mut self.backend {
            V4l2Backend::Synthetic(source) => source.connect(),
            V4l2Backend::Device(source) => source.connect(),
        }
    }

    /// Capture the next frame.
    ///
    /// This method:
    /// 1. Captures the next frame from the device (or generates synthetic)
    /// 2. Coarsens the timestamp into a bucket
    /// 3. Computes a non-invertible feature hash
    /// 4. Returns a `RawFrame`
    ///
    /// The returned `RawFrame` has private pixel data that modules cannot access directly.
    pub fn next_frame(&mut self) -> Result<RawFrame> {
        match &mut self.backend {
            V4l2Backend::Synthetic(source) => source.next_frame(),
            V4l2Backend::Device(source) => source.next_frame(),
        }
    }

    /// Check if the source is healthy.
    pub fn is_healthy(&self) -> bool {
        match &self.backend {
            V4l2Backend::Synthetic(source) => source.is_healthy(),
            V4l2Backend::Device(source) => source.is_healthy(),
        }
    }

    /// Get frame statistics.
    pub fn stats(&self) -> V4l2Stats {
        match &self.backend {
            V4l2Backend::Synthetic(source) => source.stats(),
            V4l2Backend::Device(source) => source.stats(),
        }
    }
}

/// Statistics for a V4L2 source.
#[derive(Clone, Debug)]
pub struct V4l2Stats {
    pub frames_captured: u64,
    pub device: String,
}

// ----------------------------------------------------------------------------
// Synthetic source (stub://) for tests
// ----------------------------------------------------------------------------

struct SyntheticV4l2Source {
    config: V4l2Config,
    frame_count: u64,
    /// Simulated "scene" state for synthetic motion detection.
    scene_state: u8,
}

impl SyntheticV4l2Source {
    fn new(config: V4l2Config) -> Self {
        Self {
            config,
            frame_count: 0,
            scene_state: 0,
        }
    }

    /// Connect to the V4L2 device.
    ///
    /// Synthetic sources are always "connected".
    fn connect(&mut self) -> Result<()> {
        log::info!(
            "V4l2Source: connected to {} (synthetic)",
            self.config.device
        );
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
        if self.frame_count % 50 == 0 {
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

    fn stats(&self) -> V4l2Stats {
        V4l2Stats {
            frames_captured: self.frame_count,
            device: self.config.device.clone(),
        }
    }
}

// ----------------------------------------------------------------------------
// Production V4L2 source using libv4l
// ----------------------------------------------------------------------------

struct DeviceV4l2Source {
    config: V4l2Config,
    state: Option<DeviceV4l2State>,
    frame_count: u64,
    last_frame_at: Option<Instant>,
    last_error: Option<String>,
    active_width: u32,
    active_height: u32,
}

#[self_referencing]
struct DeviceV4l2State {
    device: v4l::Device,
    #[borrows(mut device)]
    #[covariant]
    stream: v4l::prelude::MmapStream<'this, v4l::Device>,
}

impl DeviceV4l2Source {
    fn new(config: V4l2Config) -> Result<Self> {
        Ok(Self {
            active_width: config.width,
            active_height: config.height,
            config,
            state: None,
            frame_count: 0,
            last_frame_at: None,
            last_error: None,
        })
    }

    fn connect(&mut self) -> Result<()> {
        use v4l::buffer::Type;
        use v4l::video::Capture;

        let mut device = v4l::Device::with_path(&self.config.device)
            .with_context(|| format!("open v4l2 device {}", self.config.device))?;
        let mut format = device.format().context("read v4l2 format")?;
        format.width = self.config.width;
        format.height = self.config.height;
        format.fourcc = v4l::FourCC::new(b"RGB3");

        let format = match device.set_format(&format) {
            Ok(format) => format,
            Err(err) => {
                log::warn!(
                    "V4l2Source: failed to set format on {}: {}",
                    self.config.device,
                    err
                );
                device
                    .format()
                    .context("read v4l2 format after set failure")?
            }
        };

        if self.config.target_fps > 0 {
            let params = v4l::video::capture::Parameters::with_fps(self.config.target_fps);
            if let Err(err) = device.set_params(&params) {
                log::warn!(
                    "V4l2Source: failed to set fps on {}: {}",
                    self.config.device,
                    err
                );
            }
        }

        self.active_width = format.width;
        self.active_height = format.height;
        self.last_error = None;

        let state = DeviceV4l2StateBuilder {
            device,
            stream_builder: |device| {
                v4l::prelude::MmapStream::with_buffers(device, Type::VideoCapture, 4)
                    .map_err(|err| anyhow::Error::new(err).context("create v4l2 buffer stream"))
            },
        }
        .try_build()
        .map_err(|err| {
            self.last_error = Some(err.to_string());
            err
        })?;
        self.state = Some(state);

        log::info!(
            "V4l2Source: connected to {} ({}x{})",
            self.config.device,
            self.active_width,
            self.active_height
        );
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        use v4l::io::traits::CaptureStream;

        let state = self.state.as_mut().context("v4l2 device not connected")?;
        let (buf, _meta) = state
            .with_mut(|fields| fields.stream.next())
            .map_err(|err| {
                self.last_error = Some(err.to_string());
                anyhow::Error::new(err).context("capture v4l2 frame")
            })?;

        self.frame_count += 1;
        self.last_frame_at = Some(Instant::now());

        let timestamp_bucket = TimeBucket::now_10min()?;
        let features_hash = compute_features_hash(buf, self.frame_count);

        Ok(RawFrame::new(
            buf.to_vec(),
            self.active_width,
            self.active_height,
            timestamp_bucket,
            features_hash,
        ))
    }

    fn is_healthy(&self) -> bool {
        if self.last_error.is_some() {
            return false;
        }
        let Some(last_frame_at) = self.last_frame_at else {
            return true;
        };
        last_frame_at.elapsed() <= self.health_grace()
    }

    fn stats(&self) -> V4l2Stats {
        V4l2Stats {
            frames_captured: self.frame_count,
            device: self.config.device.clone(),
        }
    }

    fn health_grace(&self) -> Duration {
        let base_ms = if self.config.target_fps == 0 {
            2_000
        } else {
            (1000 / self.config.target_fps).saturating_mul(6)
        };
        Duration::from_millis(base_ms.max(2_000) as u64)
    }
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn stub_config() -> V4l2Config {
        V4l2Config {
            device: "stub://test".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
        }
    }

    #[test]
    fn v4l2_source_produces_frames() -> Result<()> {
        let config = stub_config();
        let mut source = V4l2Source::new(config)?;
        source.connect()?;

        let frame = source.next_frame()?;
        assert_eq!(frame.width, 640);
        assert_eq!(frame.height, 480);

        Ok(())
    }

    #[test]
    fn v4l2_source_frames_have_coarse_timestamps() -> Result<()> {
        let config = stub_config();
        let mut source = V4l2Source::new(config)?;
        source.connect()?;

        let frame = source.next_frame()?;

        // Timestamp bucket should be at least 5 minutes (300s)
        assert!(frame.timestamp_bucket.size_s >= 300);

        Ok(())
    }

    #[test]
    fn v4l2_source_feature_hashes_are_not_stable() -> Result<()> {
        let config = stub_config();
        let mut source = V4l2Source::new(config)?;
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
