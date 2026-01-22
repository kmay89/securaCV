//! Module runtime boundary.
//!
//! This module defines the capability boundary for untrusted modules.
//! Modules must declare requested capabilities up front and the runtime
//! MUST refuse any module that asks for filesystem or network access.

use anyhow::{anyhow, Result};

use crate::ModuleDescriptor;

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
