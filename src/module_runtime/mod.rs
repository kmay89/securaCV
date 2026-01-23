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
    BucketKeyManager, CandidateEvent, InferenceView, Module, ModuleDescriptor, TimeBucket,
};

mod sandbox;

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
    pub fn execute_sandboxed<M: Module>(
        &self,
        module: &mut M,
        view: &InferenceView<'_>,
        bucket: TimeBucket,
        token_mgr: &BucketKeyManager,
    ) -> Result<Vec<CandidateEvent>> {
        sandbox::run_in_sandbox(|| module.process(view, bucket, token_mgr))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::EventType;

    #[test]
    fn capability_boundary_denies_filesystem_or_network_access() {
        let runtime = CapabilityBoundaryRuntime::new();

        let fs_desc = ModuleDescriptor {
            id: "test_fs",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[ModuleCapability::Filesystem],
        };
        assert!(runtime.validate_descriptor(&fs_desc).is_err());

        let net_desc = ModuleDescriptor {
            id: "test_net",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[ModuleCapability::Network],
        };
        assert!(runtime.validate_descriptor(&net_desc).is_err());
    }
}
