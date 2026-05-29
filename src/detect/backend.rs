use anyhow::Result;

use crate::detect::result::DetectionResult;

/// Detection capabilities supported by backends.
///
/// Capabilities are intentionally limited to privacy-preserving primitives.
/// Identity-linked outputs (faces, plates, re-ID vectors) are forbidden.
#[non_exhaustive]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DetectionCapability {
    Motion,
    ObjectDetection,
    Classification,
}

/// Detector backend trait.
///
/// # Audit Boundary
///
/// This trait defines an AUDIT BOUNDARY, not a security boundary.
/// Implementations MUST be manually audited to ensure they:
/// - Do not store raw pixels beyond the `detect` call
/// - Do not write to disk
/// - Do not make network requests
/// - Do not compute identity-linked outputs
///
/// Backends execute with full process privileges until sandboxing is implemented.
pub trait DetectorBackend: Send {
    /// Backend identifier.
    fn name(&self) -> &'static str;

    /// Returns true when the backend supports a capability.
    fn supports(&self, capability: DetectionCapability) -> bool;

    /// Run detection on a frame.
    ///
    /// Implementations must treat the pixel slice as read-only and ephemeral.
    /// Any violation of the audit boundary is a conformance failure.
    fn detect(&mut self, pixels: &[u8], width: u32, height: u32) -> Result<DetectionResult>;

    /// Optional warm-up hook.
    fn warm_up(&mut self) -> Result<()> {
        Ok(())
    }

    /// Export cross-frame detector state so it can survive the sandbox boundary.
    ///
    /// `detect` runs inside a forked, seccomp-restricted child process. Any
    /// mutation it makes to `self` (e.g. a previous-frame hash for motion
    /// detection) lives only in the child's copy-on-write memory and is lost
    /// when the child exits. The runtime calls `export_state` in the child
    /// after `detect` and `import_state` in the parent to carry that state
    /// forward. Stateless backends keep the default (no state).
    fn export_state(&self) -> Option<Vec<u8>> {
        None
    }

    /// Restore state previously produced by `export_state`. Default: no-op.
    fn import_state(&mut self, _state: &[u8]) -> Result<()> {
        Ok(())
    }
}
