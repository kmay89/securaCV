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

/// A source URL as it may be logged: any userinfo (`user:password@`) is
/// replaced with `***`. Camera URLs routinely embed the camera's own
/// credentials (`rtsp://admin:secret@192.168.1.10:554/stream`), and a
/// connect line is written on every connect and reconnect — into journald,
/// `docker logs`, the Home Assistant add-on log — so the URL itself must
/// never reach a log line or a stats summary.
///
/// The userinfo is everything between `://` and the last `@` of the
/// authority (the part before the first `/`, `?` or `#`). A URL whose
/// authority has no `@` but which carries one later is redacted through its
/// last `@` as well: a password pasted with a raw `/` or `?` in it puts the
/// `@` there, and hiding part of a path that really did contain `@` costs a
/// log line some detail, never a secret. A string with no `://`, or no `@`
/// after it, is returned unchanged.
pub(crate) fn redact_url_userinfo(url: &str) -> String {
    let Some(scheme_end) = url.find("://") else {
        return url.to_string();
    };
    let after_scheme = scheme_end + 3;
    let rest = &url[after_scheme..];
    let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let Some(at) = rest[..authority_end].rfind('@').or_else(|| rest.rfind('@')) else {
        return url.to_string();
    };
    format!("{}***{}", &url[..after_scheme], &rest[at..])
}

#[cfg(feature = "ingest-esp32")]
pub use esp32::Esp32Source;
// The single capture-time privacy gate: every frame source (RTSP, file, esp32, v4l2)
// emits through it, so no backend calls the feature hash directly (F-11).
pub(crate) use features::raw_frame_at_capture;
pub use file::FileSource;
pub use rtsp::RtspSource;
#[cfg(feature = "ingest-v4l2")]
pub use v4l2::V4l2Source;

#[cfg(test)]
mod tests {
    use super::redact_url_userinfo;

    #[test]
    fn url_userinfo_is_redacted_for_logging() {
        // The documented camera URL shape: the password never survives.
        assert_eq!(
            redact_url_userinfo("rtsp://admin:P%40ss@10.0.0.1:554/x"),
            "rtsp://***@10.0.0.1:554/x"
        );
        // A user name alone is still userinfo.
        assert_eq!(
            redact_url_userinfo("rtsp://viewer@cam.local/stream"),
            "rtsp://***@cam.local/stream"
        );
        // A raw `@` in the password: cut at the LAST one of the authority.
        assert_eq!(
            redact_url_userinfo("rtsp://admin:p@ss@10.0.0.1/x"),
            "rtsp://***@10.0.0.1/x"
        );
        // An `@` later in the path is not userinfo once the authority had one.
        assert_eq!(
            redact_url_userinfo("rtsp://admin:pw@10.0.0.1/cam@2?x=1"),
            "rtsp://***@10.0.0.1/cam@2?x=1"
        );
        // A password pasted with a raw `/` or `?`: nothing before the last
        // `@` is kept.
        for leaky in [
            "rtsp://admin:S3/cret@192.168.1.10:554/stream",
            "rtsp://admin:S3?cret@192.168.1.10:554/stream",
        ] {
            let redacted = redact_url_userinfo(leaky);
            assert_eq!(redacted, "rtsp://***@192.168.1.10:554/stream");
            assert!(!redacted.contains("admin") && !redacted.contains("cret"));
        }
    }

    #[test]
    fn url_without_userinfo_is_unchanged() {
        for plain in [
            "rtsp://192.168.1.100:554/stream",
            "rtsp://camera-1",
            "stub://test",
            "http://[fe80::1]:81/stream?fps=5",
            "/dev/video0",
            "",
        ] {
            assert_eq!(redact_url_userinfo(plain), plain);
        }
    }
}
