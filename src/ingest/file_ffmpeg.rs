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
    eof_sent: bool,
}

impl FfmpegFileSource {
    pub(crate) fn new(config: FileConfig) -> Result<Self> {
        ffmpeg::init().context("initialize ffmpeg")?;
        let input = ffmpeg::format::input(&config.path)
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
            eof_sent: false,
        })
    }

    pub(crate) fn connect(&mut self) -> Result<()> {
        self.connected_at = Some(Instant::now());
        log::info!("FileSource: connected to {} (ffmpeg)", self.config.path);
        Ok(())
    }

    pub(crate) fn next_frame(&mut self) -> Result<RawFrame> {
        // No inter-frame stall timeout here: this is a file, not a live
        // stream. The elapsed time between next_frame() calls reflects how
        // fast the *caller* consumes frames (per-frame sandbox fork +
        // signing), not a stalled source, so timing it out would spuriously
        // abort a slow but healthy run.
        let mut decoded = ffmpeg::frame::Video::empty();

        // Canonical ffmpeg decode loop: drain decoded frames first, only
        // sending a new packet when the decoder needs more input. At EOF the
        // decoder is flushed once so its buffered tail frames are not lost.
        loop {
            if self.decoder.receive_frame(&mut decoded).is_ok() {
                return self.build_frame(&decoded);
            }

            match self.read_video_packet()? {
                Some(packet) => {
                    self.decoder
                        .send_packet(&packet)
                        .context("send packet to ffmpeg decoder")?;
                }
                None => {
                    if !self.eof_sent {
                        self.decoder
                            .send_eof()
                            .context("flush ffmpeg decoder at EOF")?;
                        self.eof_sent = true;
                        continue;
                    }
                    self.last_error = Some("file ended without frames".to_string());
                    anyhow::bail!("file ended without frames");
                }
            }
        }
    }

    /// Read the next packet belonging to the selected video stream, skipping
    /// packets from other streams. Returns `None` at end of file.
    fn read_video_packet(&mut self) -> Result<Option<ffmpeg::codec::packet::Packet>> {
        let mut packets = self.input.packets();
        loop {
            match packets.next() {
                Some((stream, packet)) => {
                    if stream.index() == self.stream_index {
                        return Ok(Some(packet));
                    }
                }
                None => return Ok(None),
            }
        }
    }

    /// Scale a decoded frame to RGB24 and wrap it in a `RawFrame` with a
    /// coarsened capture timestamp and feature hash.
    fn build_frame(&mut self, decoded: &ffmpeg::frame::Video) -> Result<RawFrame> {
        let mut rgb_frame = ffmpeg::frame::Video::empty();
        self.scaler
            .run(decoded, &mut rgb_frame)
            .context("scale frame to RGB")?;
        let (pixels, width, height) = frame_to_pixels(&rgb_frame)?;

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

    fn health_grace(&self) -> Duration {
        let base_ms = 1000u32
            .checked_div(self.config.target_fps)
            .map(|per_frame| per_frame.saturating_mul(6))
            .unwrap_or(2_000);
        Duration::from_millis(base_ms.max(2_000) as u64)
    }
}

fn frame_to_pixels(frame: &ffmpeg::frame::Video) -> Result<(Vec<u8>, u32, u32)> {
    let width = frame.width();
    let height = frame.height();
    let row_bytes = (width as usize) * 3;
    let stride = frame.stride(0);
    let data = frame.data(0);

    if stride == row_bytes {
        return Ok((data.to_vec(), width, height));
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

    Ok((pixels, width, height))
}
