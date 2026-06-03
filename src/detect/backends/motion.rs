use anyhow::Result;
use sha2::{Digest, Sha256};

use crate::detect::result::{DetectionResult, SizeClass};

/// Shared frame-difference motion core for the `stub` and `cpu` backends.
///
/// Declares motion whenever the SHA-256 of the current frame differs from the
/// previous frame, emitting a fixed confidence and size class. The two backends
/// remain distinct registry entries (one is the test placeholder, one is the
/// CPU-capability path), but share this single implementation so they cannot
/// silently diverge. This is *motion* detection only — not object detection.
#[derive(Default)]
pub(crate) struct FrameHashMotion {
    last_hash: Option<[u8; 32]>,
}

impl FrameHashMotion {
    pub(crate) fn detect(&mut self, pixels: &[u8]) -> DetectionResult {
        let current_hash: [u8; 32] = Sha256::digest(pixels).into();

        let motion = self.last_hash.is_some_and(|prev| prev != current_hash);

        self.last_hash = Some(current_hash);

        if motion {
            DetectionResult {
                motion_detected: true,
                detections: vec![],
                confidence: 0.85,
                size_class: SizeClass::Large,
            }
        } else {
            DetectionResult::default()
        }
    }

    pub(crate) fn export_state(&self) -> Option<Vec<u8>> {
        self.last_hash.map(|hash| hash.to_vec())
    }

    pub(crate) fn import_state(&mut self, state: &[u8], backend: &str) -> Result<()> {
        let hash: [u8; 32] = state
            .try_into()
            .map_err(|_| anyhow::anyhow!("{backend} backend: invalid state length"))?;
        self.last_hash = Some(hash);
        Ok(())
    }
}
