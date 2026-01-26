//! Raw media isolation layer.
//!
//! This module enforces Invariant I (No Raw Export) at the type level.
//!
//! - `RawFrame`: Opaque container for raw pixel data. Bytes are private.
//! - `InferenceView`: Restricted view that modules receive. Can run inference, cannot export bytes.
//! - `FrameBuffer`: Bounded ring buffer for pre-roll (vault sealing only).
//!
//! The ONLY path to raw bytes is `RawMediaBoundary::export_for_vault()`, which enforces
//! break-glass validation before vault sealing. Normal operation cannot extract raw media.

use anyhow::Result;
use sha2::{Digest, Sha256};
use std::collections::VecDeque;
use std::time::Instant;
use zeroize::Zeroize;

use crate::{break_glass::BreakGlassToken, RawMediaBoundary, TimeBucket};

/// Build-time maximum pre-roll duration in seconds.
/// This is a hard cap on how much raw media can be buffered for vault sealing.
pub const MAX_PREROLL_SECS: u64 = 30;

/// Build-time maximum frame buffer capacity.
/// At 10 fps, 30 seconds = 300 frames.
pub const MAX_BUFFER_FRAMES: usize = 300;

// ----------------------------------------------------------------------------
// RawFrame: Opaque raw media container
// ----------------------------------------------------------------------------

/// Opaque raw frame. Bytes are private; there is no `.as_bytes()`, no `Clone`, no `AsRef<[u8]>`.
///
/// Modules cannot access raw pixel data directly. They receive an `InferenceView` instead.
/// The only extraction path is through the vault/break-glass mechanism.
pub struct RawFrame {
    /// Private pixel data. MUST NOT be exposed via any public API.
    data: Vec<u8>,

    /// Frame dimensions (modules may see these for inference setup).
    pub width: u32,
    pub height: u32,

    /// Coarsened timestamp bucket (not precise capture time).
    pub timestamp_bucket: TimeBucket,

    /// Monotonic capture instant (for buffer TTL, not exported).
    capture_instant: Instant,

    /// Non-invertible feature hash derived at capture time.
    /// This is what modules use for correlation tokens.
    features_hash: [u8; 32],
}

// Explicitly NOT implementing Clone, AsRef<[u8]>, or any byte-exposing trait.
// This is enforced by simply not writing those impls.

impl RawFrame {
    /// Create a new raw frame. Called only by ingestion layer.
    ///
    /// The `features_hash` should be derived from non-identity-bearing embeddings
    /// computed at capture time (e.g., color histogram, motion vectors).
    pub(crate) fn new(
        data: Vec<u8>,
        width: u32,
        height: u32,
        timestamp_bucket: TimeBucket,
        features_hash: [u8; 32],
    ) -> Self {
        Self {
            data,
            width,
            height,
            timestamp_bucket,
            capture_instant: Instant::now(),
            features_hash,
        }
    }

    /// Modules get a restricted view for inference. Cannot extract bytes.
    pub fn inference_view(&self) -> InferenceView<'_> {
        InferenceView { frame: self }
    }

    /// Internal: age of this frame for TTL enforcement.
    pub(crate) fn age_secs(&self) -> u64 {
        self.capture_instant.elapsed().as_secs()
    }

    /// Internal: raw byte length (for buffer memory tracking).
    pub(crate) fn byte_len(&self) -> usize {
        self.data.len()
    }

    /// Export for vault sealing. REQUIRES a valid BreakGlassToken.
    ///
    /// This is the ONLY path to raw bytes. If you don't have a token, you can't call this.
    /// In normal operation, no token exists, so this path is unreachable.
    /// `RawMediaBoundary::export_for_vault` enforces token validity and consumption.
    pub fn export_for_vault(
        mut self,
        token: &mut BreakGlassToken,
        envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &ed25519_dalek::VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<crate::BreakGlassOutcome>,
    ) -> Result<Vec<u8>> {
        RawMediaBoundary::export_for_vault(
            &mut self.data,
            token,
            envelope_id,
            expected_ruleset_hash,
            verifying_key,
            receipt_lookup,
        )
    }
}

impl Drop for RawFrame {
    fn drop(&mut self) {
        // Zeroize raw pixel data on drop to limit exposure window.
        self.data.zeroize();
    }
}

// ----------------------------------------------------------------------------
// InferenceView: Restricted interface for modules
// ----------------------------------------------------------------------------

/// Restricted view of a frame for inference. Modules receive this, not `RawFrame`.
///
/// `InferenceView` provides:
/// - Dimensions (width, height)
/// - Time bucket (coarse)
/// - Feature hash (for correlation tokens)
/// - Ability to run a detector (which receives pixels internally)
///
/// `InferenceView` does NOT provide:
/// - Raw byte access
/// - Serialization
/// - Cloning of underlying data
pub struct InferenceView<'a> {
    frame: &'a RawFrame,
}

impl<'a> InferenceView<'a> {
    pub fn width(&self) -> u32 {
        self.frame.width
    }

    pub fn height(&self) -> u32 {
        self.frame.height
    }

    pub fn timestamp_bucket(&self) -> TimeBucket {
        self.frame.timestamp_bucket
    }

    /// Non-invertible feature hash for correlation token derivation.
    pub fn features_hash(&self) -> [u8; 32] {
        self.frame.features_hash
    }

    /// Run a detector on this frame. The detector receives pixels via internal callback.
    ///
    /// The detector trait is designed so that:
    /// - Pixels flow IN to the detector
    /// - Only non-extractive results (detections) flow OUT
    /// - The detector cannot store/export raw bytes
    pub fn run_detector<D: Detector>(&self, detector: &mut D) -> DetectionResult {
        // Detector receives raw bytes via internal-only callback.
        // The callback signature prevents the detector from capturing the slice.
        detector.detect_internal(&self.frame.data, self.frame.width, self.frame.height)
    }

    /// Attempt to export raw bytes. This MUST fail in normal operation.
    pub fn try_export_bytes(&self) -> Result<Vec<u8>> {
        RawMediaBoundary::deny_export("InferenceView cannot export raw bytes")
    }
}

// ----------------------------------------------------------------------------
// Detector trait: how modules run inference
// ----------------------------------------------------------------------------

/// Result of running detection on a frame.
#[derive(Clone, Debug, Default)]
pub struct DetectionResult {
    /// Did we detect motion/presence?
    pub motion_detected: bool,
    /// Bounding boxes (normalized 0..1 coordinates).
    pub detections: Vec<Detection>,
    /// Confidence of primary detection.
    pub confidence: f32,
    /// Size class (large/small object).
    pub size_class: SizeClass,
}

#[derive(Clone, Debug)]
pub struct Detection {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub confidence: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SizeClass {
    #[default]
    Unknown,
    Small,
    Large,
}

/// Detector trait for running inference on frames.
///
/// The `detect_internal` method receives raw bytes but:
/// - Takes `&[u8]` not `Vec<u8>` (cannot take ownership)
/// - Returns only `DetectionResult` (non-extractive)
/// - The slice lifetime prevents capture across calls
pub trait Detector {
    /// Internal detection method. Modules implement this.
    ///
    /// SAFETY CONTRACT: Implementations MUST NOT:
    /// - Store the pixel slice beyond this call
    /// - Copy pixels to external storage
    /// - Transmit pixels over network
    ///
    /// Violations are conformance failures.
    fn detect_internal(&mut self, pixels: &[u8], width: u32, height: u32) -> DetectionResult;
}

/// Stub detector for MVP. Does simple "motion detection" via pixel variance.
pub struct StubDetector {
    last_hash: Option<[u8; 32]>,
}

impl StubDetector {
    pub fn new() -> Self {
        Self { last_hash: None }
    }
}

impl Default for StubDetector {
    fn default() -> Self {
        Self::new()
    }
}

impl Detector for StubDetector {
    fn detect_internal(&mut self, pixels: &[u8], _width: u32, _height: u32) -> DetectionResult {
        // Simple "motion detection": hash pixels and compare to last frame.
        let current_hash: [u8; 32] = Sha256::digest(pixels).into();

        let motion = match self.last_hash {
            Some(prev) => prev != current_hash,
            None => false,
        };

        self.last_hash = Some(current_hash);

        DetectionResult {
            motion_detected: motion,
            detections: vec![],
            confidence: if motion { 0.85 } else { 0.0 },
            size_class: if motion {
                SizeClass::Large
            } else {
                SizeClass::Unknown
            },
        }
    }
}

// ----------------------------------------------------------------------------
// BreakGlassToken: Proof of quorum authorization
// ----------------------------------------------------------------------------
// A token proving that break-glass authorization has been granted.
//
// This token is:
// - Created only by the quorum authorization system
// - Required to export raw media from the vault
// - Logged immutably upon creation
// - Single-use (consumed on export)
//
// Authorization requires quorum logic via break-glass receipts and approvals.

// ----------------------------------------------------------------------------
// FrameBuffer: Bounded ring buffer for pre-roll
// ----------------------------------------------------------------------------

/// Bounded ring buffer for raw frames.
///
/// Used for "pre-roll" when sealing evidence to the vault:
/// - Keeps the last N seconds of frames in memory
/// - Enforces MAX_PREROLL_SECS and MAX_BUFFER_FRAMES at build time
/// - Zeroizes old frames when evicted
/// - Only accessible for vault sealing (requires BreakGlassToken to extract)
pub struct FrameBuffer {
    buffer: VecDeque<RawFrame>,
    max_frames: usize,
    max_age_secs: u64,
}

impl FrameBuffer {
    pub fn new() -> Self {
        Self {
            buffer: VecDeque::with_capacity(MAX_BUFFER_FRAMES),
            max_frames: MAX_BUFFER_FRAMES,
            max_age_secs: MAX_PREROLL_SECS,
        }
    }

    /// Push a frame into the buffer. Evicts old frames as needed.
    pub fn push(&mut self, frame: RawFrame) {
        // Evict frames older than max_age_secs
        while let Some(oldest) = self.buffer.front() {
            if oldest.age_secs() > self.max_age_secs {
                self.buffer.pop_front(); // Drop triggers zeroize
            } else {
                break;
            }
        }

        // Evict if at capacity
        while self.buffer.len() >= self.max_frames {
            self.buffer.pop_front(); // Drop triggers zeroize
        }

        self.buffer.push_back(frame);
    }

    /// Get the most recent frame for processing (non-consuming).
    pub fn latest(&self) -> Option<&RawFrame> {
        self.buffer.back()
    }

    /// Current buffer length.
    pub fn len(&self) -> usize {
        self.buffer.len()
    }

    pub fn is_empty(&self) -> bool {
        self.buffer.is_empty()
    }

    /// Drain frames for vault sealing. REQUIRES BreakGlassToken.
    ///
    /// This is the only way to extract frames from the buffer.
    /// The buffer is cleared after drain.
    #[allow(dead_code)]
    pub fn drain_for_vault(
        &mut self,
        _token: &BreakGlassToken,
    ) -> impl Iterator<Item = RawFrame> + '_ {
        self.buffer.drain(..)
    }

    /// Memory usage estimate.
    pub fn memory_bytes(&self) -> usize {
        self.buffer.iter().map(|f| f.byte_len()).sum()
    }
}

impl Default for FrameBuffer {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for FrameBuffer {
    fn drop(&mut self) {
        // Clear triggers drop on each frame, which zeroizes.
        self.buffer.clear();
    }
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{
        Approval, BreakGlass, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
    };
    use crate::BreakGlassOutcome;
    use ed25519_dalek::{Signer, SigningKey};

    fn make_test_frame(data: &[u8]) -> RawFrame {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let features: [u8; 32] = Sha256::digest(data).into();
        RawFrame::new(data.to_vec(), 640, 480, bucket, features)
    }

    fn make_break_glass_token(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
    ) -> (BreakGlassToken, ed25519_dalek::VerifyingKey, [u8; 32]) {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let request = UnlockRequest::new(envelope_id, ruleset_hash, "test-export", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let signature = signing_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        let mut token = result.expect("break-glass token");
        let device_signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let receipt_hash = [8u8; 32];
        token
            .attach_receipt_signature(receipt_hash, &device_signing_key)
            .expect("attach receipt signature");
        (token, device_signing_key.verifying_key(), receipt_hash)
    }

    #[test]
    fn inference_view_cannot_export_bytes() {
        let frame = make_test_frame(b"test pixels");
        let view = frame.inference_view();

        // This MUST fail
        assert!(view.try_export_bytes().is_err());
    }

    #[test]
    fn inference_view_provides_metadata() {
        let frame = make_test_frame(b"test pixels");
        let view = frame.inference_view();

        assert_eq!(view.width(), 640);
        assert_eq!(view.height(), 480);
        assert_eq!(view.timestamp_bucket().size_s, 600);
    }

    #[test]
    fn frame_export_requires_token() {
        let frame = make_test_frame(b"test pixels");

        // With a break-glass token, export succeeds
        let envelope_id = "test-envelope";
        let ruleset_hash = [7u8; 32];
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let bytes = frame
            .export_for_vault(
                &mut token,
                envelope_id,
                ruleset_hash,
                &verifying_key,
                |hash| {
                    assert_eq!(hash, &receipt_hash);
                    Ok(BreakGlassOutcome::Granted)
                },
            )
            .unwrap();
        assert_eq!(bytes, b"test pixels");
    }

    #[test]
    fn frame_buffer_enforces_capacity() {
        let mut buf = FrameBuffer::new();

        // Push more than MAX_BUFFER_FRAMES
        for i in 0..(MAX_BUFFER_FRAMES + 10) {
            let data = format!("frame{}", i);
            buf.push(make_test_frame(data.as_bytes()));
        }

        // Buffer should be at max capacity
        assert!(buf.len() <= MAX_BUFFER_FRAMES);
    }

    #[test]
    fn stub_detector_detects_motion() {
        let mut detector = StubDetector::new();

        // First frame: no motion (no previous)
        let r1 = detector.detect_internal(b"frame1", 10, 10);
        assert!(!r1.motion_detected);

        // Second frame: different content = motion
        let r2 = detector.detect_internal(b"frame2", 10, 10);
        assert!(r2.motion_detected);

        // Third frame: same as second = no motion
        let r3 = detector.detect_internal(b"frame2", 10, 10);
        assert!(!r3.motion_detected);
    }
}
