// Rationale: detectors may receive pixels for inference but cannot return or export them.
use witness_kernel::{DetectionResult, Detector};

struct BadDetector;

impl Detector for BadDetector {
    fn detect_internal(&mut self, pixels: &[u8], _width: u32, _height: u32) -> DetectionResult {
        // Attempt to return/cloned pixel bytes should be rejected by the type system.
        return pixels.to_vec();
    }
}

fn main() {}
