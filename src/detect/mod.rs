mod backend;
mod backends;
mod registry;
mod result;

pub use backend::{DetectionCapability, DetectorBackend};
pub use backends::StubBackend;
pub use registry::BackendRegistry;
pub use result::{Detection, DetectionResult, SizeClass};
