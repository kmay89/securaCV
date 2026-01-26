//! Frame ingestion sources.
//!
//! This module provides different sources for raw frames:
//! - RTSP streams (IP cameras)
//! - Local video files (feature: ingest-file-ffmpeg)
//! - USB/V4L2 devices (feature: ingest-v4l2)
//! - ESP32-S3 HTTP/UDP streams (feature: ingest-esp32)
//! - Stub source (testing)
//!
//! v1 deployments should assume local-only ingestion. Network sources are optional
//! and feature-gated, with no stability guarantees in v1.
//!
//! All sources produce `RawFrame` instances that flow into the frame buffer.
//! The ingestion layer is responsible for:
//! - Coarsening timestamps into buckets at capture time
//! - Computing non-invertible feature hashes at capture time
//! - Rate limiting / frame decimation
//!
//! The ingestion layer MUST NOT:
//! - Store raw frames to disk
//! - Transmit raw frames over network
//! - Log raw frame content

#[cfg(feature = "ingest-esp32")]
pub mod esp32;
mod features;
pub mod file;
#[cfg(feature = "ingest-file-ffmpeg")]
pub(crate) mod file_ffmpeg;
#[cfg(feature = "ingest-v4l2")]
mod normalize;
pub mod rtsp;
#[cfg(feature = "rtsp-ffmpeg")]
pub(crate) mod rtsp_ffmpeg;
#[cfg(feature = "ingest-v4l2")]
pub mod v4l2;

#[cfg(feature = "ingest-esp32")]
pub use esp32::Esp32Source;
pub(crate) use features::compute_features_hash;
pub use file::FileSource;
pub use rtsp::RtspSource;
#[cfg(feature = "ingest-v4l2")]
pub use v4l2::V4l2Source;
