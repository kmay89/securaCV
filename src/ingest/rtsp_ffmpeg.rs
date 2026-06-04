//! RTSP frame source using FFmpeg.
//!
//! This module provides an FFmpeg-backed RTSP decoder that mirrors the invariants of
//! the GStreamer implementation: frames are processed in-memory, timestamps are
//! coarsened at capture time, and feature hashes are computed at capture time.

use anyhow::{Context, Result};
use ffmpeg_next as ffmpeg;
use std::time::{Duration, Instant};

use crate::frame::RawFrame;

use super::rtsp::RtspConfig;

pub(crate) struct FfmpegRtspSource {
    config: RtspConfig,
    input: ffmpeg::format::context::Input,
    stream_index: usize,
    decoder: ffmpeg::codec::decoder::Video,
    scaler: ffmpeg::software::scaling::Context,
    frame_count: u64,
    last_frame_at: Option<Instant>,
    connected_at: Option<Instant>,
    last_error: Option<String>,
}

impl FfmpegRtspSource {
    pub(crate) fn new(config: RtspConfig) -> Result<Self> {
        ffmpeg::init().context("initialize ffmpeg")?;
        // Optionally force the RTSP lower transport (e.g. "tcp"/"udp") from
        // config. Many IP cameras and NVRs only stream reliably over interleaved
        // TCP. When `transport` is None the libav default is used (UDP-first with
        // TCP fallback), so behavior is unchanged for existing deployments.
        let input = match config.transport.as_deref().map(str::trim) {
            Some(transport) if !transport.is_empty() => {
                let mut opts = ffmpeg::Dictionary::new();
                opts.set("rtsp_transport", transport);
                ffmpeg::format::input_with_dictionary(&config.url, opts)
                    .context("open RTSP input with ffmpeg")?
            }
            _ => ffmpeg::format::input(&config.url).context("open RTSP input with ffmpeg")?,
        };
        let input_stream = input
            .streams()
            .best(ffmpeg::media::Type::Video)
            .ok_or_else(|| anyhow::anyhow!("RTSP stream has no video track"))?;
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
        log::info!("RtspSource: connected to {} (ffmpeg)", self.config.url);
        Ok(())
    }

    pub(crate) fn next_frame(&mut self) -> Result<RawFrame> {
        self.poll_timeout();

        let mut decoded = ffmpeg::frame::Video::empty();
        let mut rgb_frame = ffmpeg::frame::Video::empty();

        // Drain any already-decoded frame before pulling new packets, so the
        // decoder's internal output queue stays bounded — otherwise feeding a
        // fresh packet on every call while frames sit buffered (B-frames /
        // multi-frame packets) accumulates latency and memory on long-running
        // live streams.
        if self.decoder.receive_frame(&mut decoded).is_ok() {
            return self.emit_frame(&decoded, &mut rgb_frame);
        }

        for (stream, packet) in self.input.packets() {
            if stream.index() != self.stream_index {
                continue;
            }

            self.decoder
                .send_packet(&packet)
                .context("send packet to ffmpeg decoder")?;

            if self.decoder.receive_frame(&mut decoded).is_ok() {
                return self.emit_frame(&decoded, &mut rgb_frame);
            }
        }

        self.last_error = Some("ffmpeg stream ended without frames".to_string());
        anyhow::bail!("RTSP stream ended without frames")
    }

    /// Scale a decoded frame to RGB and wrap it as a `RawFrame`, advancing the
    /// capture counters. `rgb_frame` is a scratch buffer reused across calls.
    fn emit_frame(
        &mut self,
        decoded: &ffmpeg::frame::Video,
        rgb_frame: &mut ffmpeg::frame::Video,
    ) -> Result<RawFrame> {
        self.scaler
            .run(decoded, rgb_frame)
            .context("scale frame to RGB")?;
        let (pixels, width, height) = frame_to_pixels(rgb_frame)?;

        self.frame_count += 1;
        self.last_frame_at = Some(Instant::now());

        super::raw_frame_at_capture(pixels, width, height, self.frame_count)
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

    pub(crate) fn stats(&self) -> super::rtsp::RtspStats {
        super::rtsp::RtspStats {
            frames_captured: self.frame_count,
            url: self.config.url.clone(),
        }
    }

    fn frame_timeout(&self) -> Duration {
        let base_ms = 1000u32
            .checked_div(self.config.target_fps)
            .map(|hz| hz.saturating_mul(4))
            .unwrap_or(500);
        Duration::from_millis(base_ms.max(500) as u64)
    }

    fn health_grace(&self) -> Duration {
        let base_ms = 1000u32
            .checked_div(self.config.target_fps)
            .map(|hz| hz.saturating_mul(6))
            .unwrap_or(2_000);
        Duration::from_millis(base_ms.max(2_000) as u64)
    }

    fn poll_timeout(&mut self) {
        if let Some(last_frame_at) = self.last_frame_at {
            if last_frame_at.elapsed() > self.frame_timeout() {
                self.last_error = Some("ffmpeg stream stalled".to_string());
            }
        }
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
