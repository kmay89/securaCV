use anyhow::Result;
use sha2::{Digest, Sha256};

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::result::{DetectionResult, SizeClass};

/// Stub backend for testing. Uses pixel hashing to detect motion.
pub struct StubBackend {
    last_hash: Option<[u8; 32]>,
}

impl StubBackend {
    pub fn new() -> Self {
        Self { last_hash: None }
    }
}

impl Default for StubBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl DetectorBackend for StubBackend {
    fn name(&self) -> &'static str {
        "stub"
    }

    fn supports(&self, capability: DetectionCapability) -> bool {
        matches!(capability, DetectionCapability::Motion)
    }

    fn detect(&mut self, pixels: &[u8], _width: u32, _height: u32) -> Result<DetectionResult> {
        let current_hash: [u8; 32] = Sha256::digest(pixels).into();

        let motion = match self.last_hash {
            Some(prev) => prev != current_hash,
            None => false,
        };

        self.last_hash = Some(current_hash);

        Ok(DetectionResult {
            motion_detected: motion,
            detections: vec![],
            confidence: if motion { 0.85 } else { 0.0 },
            size_class: if motion {
                SizeClass::Large
            } else {
                SizeClass::Unknown
            },
        })
    }
}
