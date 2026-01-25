//! ESP32-S3 frame source.
//!
//! This module provides `Esp32Source` for ingesting frames from ESP32-S3 cameras
//! that stream MJPEG/JPEG over HTTP or JPEG over RTP/UDP.
//!
//! The ESP32 source is responsible for:
//! - Connecting to HTTP MJPEG/JPEG or UDP RTP streams
//! - Decoding JPEG frames in-memory
//! - Coarsening timestamps at capture time
//! - Computing feature hashes at capture time
//! - Producing `RawFrame` instances
//!
//! The ESP32 source MUST NOT:
//! - Store decoded frames to disk
//! - Forward raw frames over network
//! - Retain frames beyond handoff to FrameBuffer

use anyhow::{anyhow, Context, Result};
use std::io::Read;
use std::net::UdpSocket;
use std::time::{Duration, Instant};

use image::GenericImageView;
use url::Url;

use super::compute_features_hash;
use crate::frame::RawFrame;
use crate::TimeBucket;

const MAX_JPEG_BYTES: usize = 5 * 1024 * 1024;
const RTP_JPEG_PAYLOAD_TYPE: u8 = 26;

/// Configuration for an ESP32-S3 source.
#[derive(Clone, Debug)]
pub struct Esp32Config {
    /// Stream URL. Supported schemes: http(s):// for MJPEG/JPEG, udp:// for RTP/JPEG.
    pub url: String,
    /// Target frame rate (frames per second). Source will decimate to this rate.
    pub target_fps: u32,
}

impl Default for Esp32Config {
    fn default() -> Self {
        Self {
            url: "http://127.0.0.1:81/stream".to_string(),
            target_fps: 10,
        }
    }
}

/// ESP32-S3 frame source.
///
/// Uses HTTP MJPEG/JPEG or UDP RTP/JPEG, depending on the URL scheme.
pub struct Esp32Source {
    backend: Esp32Backend,
}

enum Esp32Backend {
    Http(HttpEsp32Source),
    Udp(UdpEsp32Source),
}

impl Esp32Source {
    pub fn new(config: Esp32Config) -> Result<Self> {
        let url = Url::parse(&config.url).context("parse esp32 url")?;
        let backend = match url.scheme() {
            "http" | "https" => Esp32Backend::Http(HttpEsp32Source::new(config)),
            "udp" => Esp32Backend::Udp(UdpEsp32Source::new(config, url)?),
            other => {
                return Err(anyhow!(
                    "unsupported esp32 scheme '{}'; expected http(s) or udp",
                    other
                ))
            }
        };
        Ok(Self { backend })
    }

    /// Connect to the ESP32 stream.
    pub fn connect(&mut self) -> Result<()> {
        match &mut self.backend {
            Esp32Backend::Http(source) => source.connect(),
            Esp32Backend::Udp(source) => source.connect(),
        }
    }

    /// Capture the next frame.
    pub fn next_frame(&mut self) -> Result<RawFrame> {
        match &mut self.backend {
            Esp32Backend::Http(source) => source.next_frame(),
            Esp32Backend::Udp(source) => source.next_frame(),
        }
    }

    /// Check if the source is healthy.
    pub fn is_healthy(&self) -> bool {
        match &self.backend {
            Esp32Backend::Http(source) => source.is_healthy(),
            Esp32Backend::Udp(source) => source.is_healthy(),
        }
    }

    /// Get frame statistics.
    pub fn stats(&self) -> Esp32Stats {
        match &self.backend {
            Esp32Backend::Http(source) => source.stats(),
            Esp32Backend::Udp(source) => source.stats(),
        }
    }
}

/// Statistics for an ESP32 source.
#[derive(Clone, Debug)]
pub struct Esp32Stats {
    pub frames_captured: u64,
    pub source: String,
}

struct HttpEsp32Source {
    config: Esp32Config,
    stream: Option<HttpStream>,
    last_frame_at: Option<Instant>,
    connected_at: Option<Instant>,
    frame_count: u64,
    last_error: Option<String>,
}

enum HttpStream {
    Mjpeg(MjpegStream),
    SingleJpeg,
}

impl HttpEsp32Source {
    fn new(config: Esp32Config) -> Self {
        Self {
            config,
            stream: None,
            last_frame_at: None,
            connected_at: None,
            frame_count: 0,
            last_error: None,
        }
    }

    fn connect(&mut self) -> Result<()> {
        let response = ureq::get(&self.config.url)
            .call()
            .context("connect to esp32 http stream")?;
        let content_type = response.header("Content-Type").unwrap_or("");
        if content_type.to_lowercase().contains("multipart") {
            let reader = response.into_reader();
            self.stream = Some(HttpStream::Mjpeg(MjpegStream::new(reader)));
        } else {
            self.stream = Some(HttpStream::SingleJpeg);
        }
        self.connected_at = Some(Instant::now());
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        let stream = self
            .stream
            .as_mut()
            .ok_or_else(|| anyhow!("esp32 http source not connected; call connect() first"))?;
        let min_interval = frame_interval(self.config.target_fps);
        loop {
            let jpeg_bytes = match stream {
                HttpStream::Mjpeg(stream) => stream.read_next_jpeg(),
                HttpStream::SingleJpeg => fetch_single_jpeg(&self.config.url),
            }?;

            let now = Instant::now();
            if let Some(last) = self.last_frame_at {
                if now.duration_since(last) < min_interval {
                    continue;
                }
            }

            let (pixels, width, height) = decode_jpeg(&jpeg_bytes)?;
            self.frame_count += 1;
            self.last_frame_at = Some(now);

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
        last_frame_at.elapsed() <= health_grace(self.config.target_fps)
    }

    fn stats(&self) -> Esp32Stats {
        Esp32Stats {
            frames_captured: self.frame_count,
            source: self.config.url.clone(),
        }
    }
}

struct MjpegStream {
    reader: Box<dyn Read + Send>,
    buffer: Vec<u8>,
}

impl MjpegStream {
    fn new(reader: Box<dyn Read + Send>) -> Self {
        Self {
            reader,
            buffer: Vec::with_capacity(64 * 1024),
        }
    }

    fn read_next_jpeg(&mut self) -> Result<Vec<u8>> {
        let mut chunk = vec![0u8; 8192];
        loop {
            if let Some((start, end)) = find_jpeg_bounds(&self.buffer) {
                let frame = self.buffer[start..end].to_vec();
                self.buffer.drain(..end);
                return Ok(frame);
            }

            let read = self.reader.read(&mut chunk).context("read mjpeg chunk")?;
            if read == 0 {
                return Err(anyhow!("mjpeg stream ended"));
            }
            self.buffer.extend_from_slice(&chunk[..read]);

            if self.buffer.len() > MAX_JPEG_BYTES * 2 {
                let keep = 2.min(self.buffer.len());
                let drain_len = self.buffer.len() - keep;
                self.buffer.drain(..drain_len);
            }
        }
    }
}

struct UdpEsp32Source {
    config: Esp32Config,
    socket: Option<UdpSocket>,
    buffer: Vec<u8>,
    last_frame_at: Option<Instant>,
    connected_at: Option<Instant>,
    frame_count: u64,
    last_error: Option<String>,
}

impl UdpEsp32Source {
    fn new(config: Esp32Config, url: Url) -> Result<Self> {
        let host = url
            .host_str()
            .ok_or_else(|| anyhow!("udp url missing host"))?;
        let port = url.port().ok_or_else(|| anyhow!("udp url missing port"))?;
        let bind_addr = format!("{}:{}", host, port);
        Ok(Self {
            config,
            socket: Some(
                UdpSocket::bind(&bind_addr)
                    .with_context(|| format!("bind udp socket on {}", bind_addr))?,
            ),
            buffer: Vec::with_capacity(128 * 1024),
            last_frame_at: None,
            connected_at: None,
            frame_count: 0,
            last_error: None,
        })
    }

    fn connect(&mut self) -> Result<()> {
        if let Some(socket) = self.socket.as_ref() {
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .context("set udp read timeout")?;
        }
        self.connected_at = Some(Instant::now());
        Ok(())
    }

    fn next_frame(&mut self) -> Result<RawFrame> {
        let socket = self
            .socket
            .as_ref()
            .ok_or_else(|| anyhow!("udp socket not initialized"))?;
        let min_interval = frame_interval(self.config.target_fps);

        loop {
            let mut packet = vec![0u8; 64 * 1024];
            let (len, _) = socket.recv_from(&mut packet).context("recv udp packet")?;
            packet.truncate(len);
            let (payload, marker) = parse_rtp_payload(&packet)?;

            if payload.is_empty() {
                continue;
            }

            if self.buffer.len() + payload.len() > MAX_JPEG_BYTES {
                self.buffer.clear();
                return Err(anyhow!("rtp frame exceeded max jpeg size"));
            }

            self.buffer.extend_from_slice(payload);

            if !marker {
                continue;
            }

            let now = Instant::now();
            let jpeg_bytes = std::mem::take(&mut self.buffer);
            if let Some(last) = self.last_frame_at {
                if now.duration_since(last) < min_interval {
                    continue;
                }
            }

            let (pixels, width, height) = decode_jpeg(&jpeg_bytes)?;
            self.frame_count += 1;
            self.last_frame_at = Some(now);

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
        last_frame_at.elapsed() <= health_grace(self.config.target_fps)
    }

    fn stats(&self) -> Esp32Stats {
        Esp32Stats {
            frames_captured: self.frame_count,
            source: self.config.url.clone(),
        }
    }
}

fn fetch_single_jpeg(url: &str) -> Result<Vec<u8>> {
    let response = ureq::get(url)
        .call()
        .with_context(|| format!("fetch jpeg snapshot from {}", url))?;
    let mut bytes = Vec::new();
    response
        .into_reader()
        .read_to_end(&mut bytes)
        .context("read jpeg snapshot")?;
    if bytes.is_empty() {
        return Err(anyhow!("empty jpeg snapshot"));
    }
    Ok(bytes)
}

fn decode_jpeg(bytes: &[u8]) -> Result<(Vec<u8>, u32, u32)> {
    let image = image::load_from_memory(bytes).context("decode jpeg")?;
    let (width, height) = image.dimensions();
    let rgb = image.into_rgb8();
    Ok((rgb.into_raw(), width, height))
}

fn find_jpeg_bounds(buffer: &[u8]) -> Option<(usize, usize)> {
    let mut start = None;
    let mut i = 0;
    while i + 1 < buffer.len() {
        if buffer[i] == 0xFF && buffer[i + 1] == 0xD8 {
            start = Some(i);
            break;
        }
        i += 1;
    }
    let start = start?;
    let mut j = start + 2;
    while j + 1 < buffer.len() {
        if buffer[j] == 0xFF && buffer[j + 1] == 0xD9 {
            return Some((start, j + 2));
        }
        j += 1;
    }
    None
}

fn parse_rtp_payload(packet: &[u8]) -> Result<(&[u8], bool)> {
    if packet.len() < 12 {
        return Err(anyhow!("rtp packet too small"));
    }
    let b0 = packet[0];
    let b1 = packet[1];
    let version = b0 >> 6;
    if version != 2 {
        return Err(anyhow!("unsupported rtp version {}", version));
    }
    let padding = (b0 & 0x20) != 0;
    let extension = (b0 & 0x10) != 0;
    let csrc_count = (b0 & 0x0F) as usize;
    let marker = (b1 & 0x80) != 0;
    let payload_type = b1 & 0x7F;
    if payload_type != RTP_JPEG_PAYLOAD_TYPE {
        return Err(anyhow!(
            "unsupported rtp payload type {}; expected {}",
            payload_type,
            RTP_JPEG_PAYLOAD_TYPE
        ));
    }

    let mut offset = 12 + csrc_count * 4;
    if packet.len() < offset {
        return Err(anyhow!("rtp packet missing csrc entries"));
    }

    if extension {
        if packet.len() < offset + 4 {
            return Err(anyhow!("rtp extension header truncated"));
        }
        let ext_len = u16::from_be_bytes([packet[offset + 2], packet[offset + 3]]) as usize;
        offset += 4 + ext_len * 4;
    }

    if packet.len() < offset {
        return Err(anyhow!("rtp packet truncated"));
    }

    let mut payload_end = packet.len();
    if padding {
        let pad_len = *packet.last().unwrap_or(&0) as usize;
        if pad_len > payload_end - offset {
            return Err(anyhow!("invalid rtp padding"));
        }
        payload_end -= pad_len;
    }

    Ok((&packet[offset..payload_end], marker))
}

fn frame_interval(target_fps: u32) -> Duration {
    if target_fps == 0 {
        Duration::from_millis(0)
    } else {
        Duration::from_millis((1000 / target_fps).max(1) as u64)
    }
}

fn health_grace(target_fps: u32) -> Duration {
    let base_ms = if target_fps == 0 {
        2_000
    } else {
        (1000 / target_fps).saturating_mul(6)
    };
    Duration::from_millis(base_ms.max(2_000) as u64)
}
