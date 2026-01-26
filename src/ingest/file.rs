//! Local file frame source.
//!
//! This module provides `FileSource` for ingesting frames from local video files.
//! The file source is responsible for:
//! - Reading frames from a local video file (no network access)
//! - Decoding video frames in-memory
//! - Coarsening timestamps at capture time
//! - Computing feature hashes at capture time
//! - Producing `RawFrame` instances
//!
//! The file source MUST NOT:
//! - Fetch remote URLs
//! - Store decoded frames to disk
//! - Retain frames beyond handoff to FrameBuffer

use anyhow::{anyhow, Result};

use super::compute_features_hash;
#[cfg(feature = "ingest-file-ffmpeg")]
use super::file_ffmpeg::FfmpegFileSource;
use crate::frame::RawFrame;
use crate::TimeBucket;

/// Configuration for a local file source.
#[derive(Clone, Debug)]
pub struct FileConfig {
    /// Local file path (e.g., "/var/lib/witness/video.mp4").
    pub path: String,
    /// Target frame rate (frames per second). Source may decimate to this rate.
    pub target_fps: u32,
}

impl Default for FileConfig {
    fn default() -> Self {
        Self {
            path: String::new(),
            target_fps: 10,
        }
    }
}

/// Local file frame source.
pub struct FileSource {
    backend: FileBackend,
}

enum FileBackend {
    Synthetic(SyntheticFileSource),
    #[cfg(feature = "ingest-file-ffmpeg")]
    Ffmpeg(FfmpegFileSource),
}

impl FileSource {
    pub fn new(config: FileConfig) -> Result<Self> {
        if !is_local_file_path(&config.path) {
            return Err(anyhow!(
                "file ingestion only supports local paths (no URL schemes)"
            ));
        }
        if config.path.starts_with("stub://") {
            Ok(Self {
                backend: FileBackend::Synthetic(SyntheticFileSource::new(config)),
            })
        } else {
            #[cfg(feature = "ingest-file-ffmpeg")]
            {
                Ok(Self {
                    backend: FileBackend::Ffmpeg(FfmpegFileSource::new(config)?),
                })
            }
            #[cfg(not(feature = "ingest-file-ffmpeg"))]
            {
                Err(anyhow!(
                    "file ingestion requires the ingest-file-ffmpeg feature"
                ))
            }
        }
    }

    /// Connect to the file source.
    pub fn connect(&mut self) -> Result<()> {
        match &mut self.backend {
            FileBackend::Synthetic(source) => source.connect(),
            #[cfg(feature = "ingest-file-ffmpeg")]
            FileBackend::Ffmpeg(source) => source.connect(),
        }
    }

    /// Capture the next frame.
    pub fn next_frame(&mut self) -> Result<RawFrame> {
        match &mut self.backend {
            FileBackend::Synthetic(source) => source.next_frame(),
            #[cfg(feature = "ingest-file-ffmpeg")]
            FileBackend::Ffmpeg(source) => source.next_frame(),
        }
    }

    /// Check if the source is healthy.
    pub fn is_healthy(&self) -> bool {
        match &self.backend {
            FileBackend::Synthetic(source) => source.is_healthy(),
            #[cfg(feature = "ingest-file-ffmpeg")]
            FileBackend::Ffmpeg(source) => source.is_healthy(),
        }
    }

    /// Get frame statistics.
    pub fn stats(&self) -> FileStats {
        match &self.backend {
            FileBackend::Synthetic(source) => source.stats(),
            #[cfg(feature = "ingest-file-ffmpeg")]
            FileBackend::Ffmpeg(source) => source.stats(),
        }
    }
}

/// Statistics for a file source.
#[derive(Clone, Debug)]
pub struct FileStats {
    pub frames_captured: u64,
    pub path: String,
}

// ----------------------------------------------------------------------------
// Synthetic source (stub://) for tests
// ----------------------------------------------------------------------------

struct SyntheticFileSource {
    config: FileConfig,
    frame_count: u64,
    scene_state: u8,
}

impl SyntheticFileSource {
    fn new(config: FileConfig) -> Self {
        Self {
            config,
            frame_count: 0,
            scene_state: 0,
        }
    }

    fn connect(&mut self) -> Result<()> {
        log::info!("FileSource: connected to {} (synthetic)", self.config.path);
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        self.frame_count += 1;

        let timestamp_bucket = TimeBucket::now_10min()?;
        let pixels = self.generate_synthetic_pixels();
        let features_hash = compute_features_hash(&pixels, self.frame_count);

        Ok(RawFrame::new(
            pixels,
            640,
            480,
            timestamp_bucket,
            features_hash,
        ))
    }

    fn generate_synthetic_pixels(&mut self) -> Vec<u8> {
        let pixel_count = (640 * 480 * 3) as usize;
        if self.frame_count % 50 == 0 {
            self.scene_state = self.scene_state.wrapping_add(1);
        }
        let mut pixels = vec![0u8; pixel_count];
        for (i, pixel) in pixels.iter_mut().enumerate() {
            *pixel = ((i as u64 + self.frame_count + self.scene_state as u64) % 256) as u8;
        }
        pixels
    }

    fn is_healthy(&self) -> bool {
        true
    }

    fn stats(&self) -> FileStats {
        FileStats {
            frames_captured: self.frame_count,
            path: self.config.path.clone(),
        }
    }
}

fn is_local_file_path(path: &str) -> bool {
    if path.trim().is_empty() {
        return false;
    }
    if path.starts_with("stub://") {
        return true;
    }
    !path.contains("://")
}
