//! Frame ingestion sources.
//!
//! This module provides different sources for raw frames:
//! - RTSP streams (IP cameras)
//! - USB/V4L2 devices (future)
//! - Stub source (testing)
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

pub mod rtsp;

pub use rtsp::RtspSource;
