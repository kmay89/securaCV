use anyhow::Result;

use crate::detect::backend::{DetectionCapability, DetectorBackend};
use crate::detect::backends::motion::FrameHashMotion;
use crate::detect::result::DetectionResult;

/// CPU backend for motion detection. Uses frame-difference motion detection
/// (see [`FrameHashMotion`]); shares its implementation with [`super::StubBackend`].
#[derive(Default)]
pub struct CpuBackend {
    motion: FrameHashMotion,
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
        Ok(self.motion.detect(pixels))
    }

    fn export_state(&self) -> Option<Vec<u8>> {
        self.motion.export_state()
    }

    fn import_state(&mut self, state: &[u8]) -> Result<()> {
        self.motion.import_state(state, "cpu")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::backend::DetectorBackend;
    use crate::detect::result::SizeClass;

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
