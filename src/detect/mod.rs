mod backend;
mod backends;
mod registry;
mod result;

pub use backend::{DetectionCapability, DetectorBackend};
#[cfg(feature = "backend-tract")]
pub use backends::TractBackend;
pub use backends::{CpuBackend, StubBackend};
pub use registry::BackendRegistry;
pub use result::{Detection, DetectionResult, ObjectClass, SizeClass};
