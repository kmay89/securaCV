mod backend;
mod backends;
mod registry;
mod result;

pub use backend::{DetectionCapability, DetectorBackend};
pub use backends::{CpuBackend, StubBackend};
#[cfg(feature = "backend-tract")]
pub use backends::TractBackend;
pub use registry::BackendRegistry;
pub use result::{Detection, DetectionResult, SizeClass};
