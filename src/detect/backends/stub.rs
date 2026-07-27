use anyhow::Result;

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::backends::motion::FrameHashMotion;
use crate::detect::result::DetectionResult;

/// Stub backend for testing. Uses frame-difference motion detection
/// (via the shared frame-hash motion engine); shares its implementation with [`super::CpuBackend`].
#[derive(Default)]
pub struct StubBackend {
    motion: FrameHashMotion,
}

impl StubBackend {
    pub fn new() -> Self {
        Self::default()
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
        Ok(self.motion.detect(pixels))
    }

    fn export_state(&self) -> Option<Vec<u8>> {
        self.motion.export_state()
    }

    fn import_state(&mut self, state: &[u8]) -> Result<()> {
        self.motion.import_state(state, "stub")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::backend::DetectorBackend;
    use crate::detect::result::SizeClass;

    #[test]
    fn stub_backend_detects_motion() {
        let mut backend = StubBackend::default();

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
