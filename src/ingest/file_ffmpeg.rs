//! Local file frame source using FFmpeg.
//!
//! This module provides an FFmpeg-backed local file decoder. Frames are processed
//! in-memory, timestamps are coarsened at capture time, and feature hashes are
//! computed at capture time.

use anyhow::{Context, Result};
use ffmpeg_next as ffmpeg;
use std::time::{Duration, Instant};

use super::compute_features_hash;
use super::file::{FileConfig, FileStats};
use crate::frame::RawFrame;
use crate::TimeBucket;

pub(crate) struct FfmpegFileSource {
    config: FileConfig,
    input: ffmpeg::format::context::Input,
    stream_index: usize,
    decoder: ffmpeg::codec::decoder::Video,
    scaler: ffmpeg::software::scaling::Context,
    frame_count: u64,
    last_frame_at: Option<Instant>,
    connected_at: Option<Instant>,
    last_error: Option<String>,
}

impl FfmpegFileSource {
    pub(crate) fn new(config: FileConfig) -> Result<Self> {
        ffmpeg::init().context("initialize ffmpeg")?;
        let mut input = ffmpeg::format::input(&config.path)
            .with_context(|| format!("failed to open file input '{}' with ffmpeg", config.path))?;
        let input_stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| anyhow::anyhow!("file has no video track"))?;
        let stream_index = input_stream.index();
        let context = ffmpeg::codec::context::Context::from_parameters(input_stream.parameters())
            .context("load video decoder parameters")?;
        let decoder = context
            .decoder()
            .video()
            .context("open ffmpeg video decoder")?;

        let scaler = ffmpeg::software::scaling::context::Context::get(
            decoder.format(),
            decoder.width(),
            decoder.height(),
            ffmpeg::util::format::pixel::Pixel::RGB24,
            decoder.width(),
            decoder.height(),
            ffmpeg::software::scaling::flag::Flags::BILINEAR,
        )
        .context("create ffmpeg scaler")?;

        Ok(Self {
            config,
            input,
            stream_index,
            decoder,
            scaler,
            frame_count: 0,
            last_frame_at: None,
            connected_at: None,
            last_error: None,
        })
    }

    pub(crate) fn connect(&mut self) -> Result<()> {
        self.connected_at = Some(Instant::now());
        log::info!("FileSource: connected to {} (ffmpeg)", self.config.path);
        Ok(())
    }

    pub(crate) fn next_frame(&mut self) -> Result<RawFrame> {
        self.poll_timeout()?;

        let mut decoded = ffmpeg::frame::Video::empty();
        let mut rgb_frame = ffmpeg::frame::Video::empty();

        for (stream, packet) in self.input.packets() {
            if stream.index() != self.stream_index {
                continue;
            }

            self.decoder
                .send_packet(&packet)
                .context("send packet to ffmpeg decoder")?;

            while self.decoder.receive_frame(&mut decoded).is_ok() {
                self.scaler
                    .run(&decoded, &mut rgb_frame)
                    .context("scale frame to RGB")?;
                let (pixels, width, height) = frame_to_pixels(&rgb_frame)?;

                self.frame_count += 1;
                self.last_frame_at = Some(Instant::now());

                let timestamp_bucket = TimeBucket::now_10min()?;
                let features_hash = compute_features_hash(&pixels, self.frame_count);

                return Ok(RawFrame::new(
                    pixels,
                    width,
                    height,
                    timestamp_bucket,
                    features_hash,
                ));
            }
        }

        self.last_error = Some("file ended without frames".to_string());
        anyhow::bail!("file ended without frames")
    }

    pub(crate) fn is_healthy(&self) -> bool {
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

    pub(crate) fn stats(&self) -> FileStats {
        FileStats {
            frames_captured: self.frame_count,
            path: self.config.path.clone(),
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

    fn poll_timeout(&mut self) -> Result<()> {
        if let Some(last_frame_at) = self.last_frame_at {
            if last_frame_at.elapsed() > self.frame_timeout() {
                self.last_error = Some("file ingestion stalled".to_string());
                anyhow::bail!("file ingestion stalled");
            }
        }
        Ok(())
    }
}

fn frame_to_pixels(frame: &ffmpeg::frame::Video) -> Result<(Vec<u8>, u32, u32)> {
    let width = frame.width();
    let height = frame.height();
    let row_bytes = (width as usize) * 3;
    let stride = frame.stride(0) as usize;
    let data = frame.data(0);

    if stride == row_bytes {
        return Ok((data.to_vec(), width as u32, height as u32));
    }

    let mut pixels = Vec::with_capacity(row_bytes * height as usize);
    for row in 0..height as usize {
        let start = row * stride;
        let end = start + row_bytes;
        pixels.extend_from_slice(
            data.get(start..end)
                .context("ffmpeg frame row is out of bounds")?,
        );
    }

    Ok((pixels, width as u32, height as u32))
}
