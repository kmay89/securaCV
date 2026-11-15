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
        let (child_candidates, exported_state) = sandbox::run_in_sandbox(|| {
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

        admit_candidates(module, view, bucket, token_mgr, child_candidates)
    }
}

/// Most candidates one frame may carry out of the sandbox.
pub const MAX_CANDIDATES_PER_FRAME: usize = 16;

/// Confidence leaves the sandbox at this granularity (two decimals).
const CONFIDENCE_STEP: f32 = 100.0;

/// What a sandboxed module may say, and nothing else.
///
/// The child is a fork of a parent that holds the device signing key; the
/// seccomp denylist keeps it from *sending* anything, but its one legitimate
/// output — the candidate list — crossed back as free-form data: `zone_id`
/// was any string the child chose and `correlation_token` any 32 bytes, both
/// sealed verbatim and republished by the bridges. A hostile model library
/// could have read the key out of its own copy-on-write heap and walked it
/// out through either field. So the parent rebuilds every candidate from
/// what it already knows: the module's declared zone ids, its allowed event
/// types, the frame's bucket, and a token it mints itself from the view's
/// features. The child's say is reduced to *which* declared zone, *which*
/// allowed type, whether a token applies, and a two-decimal confidence.
fn admit_candidates<M: Module>(
    module: &M,
    view: &InferenceView<'_>,
    bucket: TimeBucket,
    token_mgr: &BucketKeyManager,
    child_candidates: Vec<CandidateEvent>,
) -> Result<Vec<CandidateEvent>> {
    if child_candidates.len() > MAX_CANDIDATES_PER_FRAME {
        return Err(anyhow!(
            "conformance: module returned {} candidates for one frame (max {})",
            child_candidates.len(),
            MAX_CANDIDATES_PER_FRAME
        ));
    }
    let desc = module.descriptor();
    let zones = module.declared_zone_ids();
    let mut admitted = Vec::with_capacity(child_candidates.len());
    for cand in child_candidates {
        if !desc.allowed_event_types.contains(&cand.event_type) {
            return Err(anyhow!(
                "conformance: module {} returned event type {:?} it did not declare",
                desc.id,
                cand.event_type
            ));
        }
        let zone_id = match zones.iter().find(|z| **z == cand.zone_id) {
            Some(zone) => zone.clone(),
            None => {
                return Err(anyhow!(
                    "conformance: module {} returned a zone id it did not declare",
                    desc.id
                ))
            }
        };
        if cand.time_bucket != bucket {
            return Err(anyhow!(
                "conformance: module {} returned a time bucket other than the frame's",
                desc.id
            ));
        }
        if cand.attestation.is_some() {
            return Err(anyhow!(
                "conformance: module {} is the device itself and may not claim an attestation",
                desc.id
            ));
        }
        if !cand.confidence.is_finite() || !(0.0..=1.0).contains(&cand.confidence) {
            return Err(anyhow!(
                "conformance: module {} returned a confidence outside 0..=1",
                desc.id
            ));
        }
        let correlation_token = match cand.correlation_token {
            Some(_) => Some(token_mgr.token_for_features(view.features_hash())?),
            None => None,
        };
        admitted.push(CandidateEvent {
            event_type: cand.event_type,
            time_bucket: bucket,
            zone_id,
            confidence: (cand.confidence * CONFIDENCE_STEP).round() / CONFIDENCE_STEP,
            correlation_token,
            attestation: None,
        });
    }
    Ok(admitted)
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
