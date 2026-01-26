use anyhow::Result;
use sha2::{Digest, Sha256};

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::result::{DetectionResult, SizeClass};

/// CPU backend for motion detection.
#[derive(Default)]
pub struct CpuBackend {
    last_hash: Option<[u8; 32]>,
}

impl CpuBackend {
    pub fn new() -> Self {
        Self::default()
    }
}

impl DetectorBackend for CpuBackend {
    fn name(&self) -> &'static str {
        "cpu"
    }

    fn supports(&self, capability: DetectionCapability) -> bool {
        matches!(capability, DetectionCapability::Motion)
    }

    fn detect(&mut self, pixels: &[u8], _width: u32, _height: u32) -> Result<DetectionResult> {
        let current_hash: [u8; 32] = Sha256::digest(pixels).into();

        let motion = self.last_hash.is_some_and(|prev| prev != current_hash);

        self.last_hash = Some(current_hash);

        if motion {
            Ok(DetectionResult {
                motion_detected: true,
                detections: vec![],
                confidence: 0.85,
                size_class: SizeClass::Large,
            })
        } else {
            Ok(DetectionResult::default())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::backend::DetectorBackend;

    #[test]
    fn cpu_backend_detects_motion() {
        let mut backend = CpuBackend::default();

        let r1 = backend.detect(b"frame1", 10, 10).unwrap();
        assert!(!r1.motion_detected);
        assert_eq!(r1.confidence, 0.0);
        assert_eq!(r1.size_class, SizeClass::Unknown);

        let r2 = backend.detect(b"frame2", 10, 10).unwrap();
        assert!(r2.motion_detected);
        assert_eq!(r2.confidence, 0.85);
        assert_eq!(r2.size_class, SizeClass::Large);

        let r3 = backend.detect(b"frame2", 10, 10).unwrap();
        assert!(!r3.motion_detected);
        assert_eq!(r3.confidence, 0.0);
        assert_eq!(r3.size_class, SizeClass::Unknown);
    }
}
