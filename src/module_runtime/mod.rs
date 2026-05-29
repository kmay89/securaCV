//! Module runtime boundary.
//!
//! This module defines the capability boundary for untrusted modules.
//! Modules must declare requested capabilities up front and the runtime
//! MUST refuse any module that asks for filesystem or network access.
//!
//! Runtime execution is isolated behind a hardened sandbox boundary that
//! denies filesystem and network syscalls even if a module attempts them.

use anyhow::{anyhow, Result};

use crate::{
    detect::BackendRegistry, BucketKeyManager, CandidateEvent, InferenceView, Module,
    ModuleDescriptor, TimeBucket,
};

pub mod event_payload;
pub(crate) mod sandbox;

/// Capabilities a module may request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ModuleCapability {
    Filesystem,
    Network,
}

/// Runtime capability boundary enforcement.
#[derive(Default)]
pub struct CapabilityBoundaryRuntime;

impl CapabilityBoundaryRuntime {
    pub fn new() -> Self {
        Self
    }

    /// Ensure a module requests no forbidden capabilities.
    pub fn validate_descriptor(&self, desc: &ModuleDescriptor) -> Result<()> {
        if let Some(cap) = desc.requested_capabilities.first() {
            return Err(anyhow!(
                "conformance: module {} requested forbidden capability {:?}",
                desc.id,
                cap
            ));
        }
        Ok(())
    }

    /// Execute a module inside the hardened sandbox boundary.
    ///
    /// Detection backends carry per-frame state (e.g. the previous frame's
    /// hash for motion detection), but `module.process` runs in a forked,
    /// seccomp-restricted child: any state it mutates lives only in the
    /// child's copy-on-write memory and dies with the child. We therefore
    /// export the default backend's post-detect state from the child and
    /// re-import it into the parent's backend, so cross-frame detection works
    /// without weakening the sandbox (detection still runs isolated).
    pub fn execute_sandboxed<M: Module>(
        &self,
        module: &mut M,
        view: &InferenceView<'_>,
        bucket: TimeBucket,
        token_mgr: &BucketKeyManager,
        registry: &BackendRegistry,
    ) -> Result<Vec<CandidateEvent>> {
        let (candidates, exported_state) = sandbox::run_in_sandbox(|| {
            let candidates = module.process(view, bucket, token_mgr, registry)?;
            let state = match registry.default_backend() {
                Some(backend) => {
                    let guard = backend
                        .lock()
                        .map_err(|_| anyhow!("default backend lock poisoned"))?;
                    guard.export_state()
                }
                None => None,
            };
            Ok((candidates, state))
        })?;

        if let Some(state) = exported_state {
            if let Some(backend) = registry.default_backend() {
                let mut guard = backend
                    .lock()
                    .map_err(|_| anyhow!("default backend lock poisoned"))?;
                guard.import_state(&state)?;
            }
        }

        Ok(candidates)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::detect::{CpuBackend, StubBackend};
    use crate::frame::RawFrame;
    use crate::{BackendSelection, DeviceCapabilities};
    use crate::{EventType, InferenceBackend, ZoneCrossingModule};
    use sha2::{Digest, Sha256};

    fn frame_from(data: &[u8]) -> RawFrame {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let features: [u8; 32] = Sha256::digest(data).into();
        RawFrame::new(data.to_vec(), 8, 8, bucket, features)
    }

    // Regression: detection backends are stateful (motion = "this frame's
    // hash differs from the previous frame's"), but `process` runs in a
    // forked sandbox child whose mutations would otherwise be discarded.
    // execute_sandboxed must carry that state back so the second, different
    // frame is detected as motion. Without the export/import fix this loops
    // forever producing zero candidates.
    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_preserves_backend_state_across_frames() {
        let runtime = CapabilityBoundaryRuntime::new();
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let token_mgr = BucketKeyManager::new();

        let mut module = ZoneCrossingModule::with_backend_selection(
            "zone:test",
            BackendSelection::Require(InferenceBackend::Cpu),
            &DeviceCapabilities::cpu_only(),
        )
        .unwrap()
        .with_tokens(false);

        let mut registry = BackendRegistry::new();
        registry.register(StubBackend::new());
        registry.register(CpuBackend::new());
        registry.set_default("cpu").unwrap();

        // First frame establishes the baseline: no prior hash -> no motion.
        let f1 = frame_from(&[1u8; 192]);
        let c1 = runtime
            .execute_sandboxed(
                &mut module,
                &f1.inference_view(),
                bucket,
                &token_mgr,
                &registry,
            )
            .unwrap();
        assert!(c1.is_empty(), "first frame should not produce motion");

        // Second, different frame: motion only if the baseline survived the
        // sandbox boundary.
        let f2 = frame_from(&[2u8; 192]);
        let c2 = runtime
            .execute_sandboxed(
                &mut module,
                &f2.inference_view(),
                bucket,
                &token_mgr,
                &registry,
            )
            .unwrap();
        assert_eq!(
            c2.len(),
            1,
            "second frame must be detected as motion (backend state lost across sandbox)"
        );

        // Identical repeat: no motion -> proves the state advanced to f2.
        let c3 = runtime
            .execute_sandboxed(
                &mut module,
                &f2.inference_view(),
                bucket,
                &token_mgr,
                &registry,
            )
            .unwrap();
        assert!(c3.is_empty(), "repeated frame should not produce motion");
    }

    #[test]
    fn capability_boundary_denies_filesystem_or_network_access() {
        let runtime = CapabilityBoundaryRuntime::new();

        let fs_desc = ModuleDescriptor {
            id: "test_fs",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[ModuleCapability::Filesystem],
            supported_backends: &[crate::InferenceBackend::Stub],
        };
        assert!(runtime.validate_descriptor(&fs_desc).is_err());

        let net_desc = ModuleDescriptor {
            id: "test_net",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[ModuleCapability::Network],
            supported_backends: &[crate::InferenceBackend::Stub],
        };
        assert!(runtime.validate_descriptor(&net_desc).is_err());
    }
}
