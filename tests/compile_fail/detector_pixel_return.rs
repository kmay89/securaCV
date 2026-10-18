// Rationale: detectors may receive pixels for inference but `detect` cannot return them.
// Written against the crate-root `DetectorBackend`, the trait every real backend implements
// (StubBackend, CpuBackend, TractBackend), so the root name is pinned to that trait too.
use witness_kernel::detect::DetectionCapability;
use witness_kernel::{DetectionResult, DetectorBackend};

struct BadDetector;

impl DetectorBackend for BadDetector {
    fn name(&self) -> &'static str {
        "bad"
    }

    fn supports(&self, _capability: DetectionCapability) -> bool {
        false
    }

    fn detect(&mut self, pixels: &[u8], _width: u32, _height: u32) -> anyhow::Result<DetectionResult> {
        // Attempt to return/cloned pixel bytes should be rejected by the type system.
        Ok(pixels.to_vec())
    }
}

fn main() {}
