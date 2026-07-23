//! The Sensor Adapter contract: the vendor-neutral types every adapter speaks.
//!
//! A [`Claim`] is deliberately *narrower* than [`CandidateEvent`] so that an adapter
//! cannot smuggle forbidden fields across the boundary:
//!
//! - It has **no timestamp** — the [`AdapterHost`](crate::adapter::AdapterHost) stamps a coarse
//!   [`TimeBucket`], which the kernel's `ContractEnforcer` re-coarsens to 10 minutes.
//! - It has **no pre-built `zone:` string** — only a raw `zone_label` that the host runs through
//!   [`sanitize_zone_name`] and the kernel validates against
//!   the strict `^zone:[a-z0-9_-]{1,64}$` allowlist.
//! - It has **no media/blob field** — raw bytes are structurally incapable of crossing.
//! - It has **no correlation token** — token policy is the host's, never the adapter's.
//!
//! See `spec/sensor_adapter_contract_v0.md`. This is an AUDIT boundary, mirroring
//! `detect::backend::DetectorBackend`; the *security* boundary remains the kernel's three gates
//! inside `Kernel::append_event_checked`.

use crate::transport::sanitize_zone_name;
use crate::{
    Attestation, CandidateEvent, EventType, InferenceBackend, ModuleCapability, ModuleDescriptor,
    TimeBucket,
};
use serde::{Deserialize, Serialize};

/// A vendor-neutral, semantic claim kind. Intentionally a *closed* vocabulary of coarse
/// claims: there is no plate/face/embedding variant and no free-form text, by construction.
///
/// Each `ClaimKind` maps 1:1 to an [`EventType`] via [`claim_kind_to_event_type`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ClaimKind {
    /// A vehicle-sized object crossed a zone boundary.
    LargeObjectBoundaryCrossing,
    /// A small object (animal, package, bicycle) crossed a zone boundary.
    SmallObjectBoundaryCrossing,
    /// A coarse acoustic impulse was sensed in a zone (no waveform, no direction).
    AcousticImpulseInZone,
    /// A presence was sensed in an operator-designated restricted zone (no identity).
    PresenceInRestrictedZone,
    /// A vehicle-sized presence was sensed during operator-configured "after hours".
    VehiclePresenceAfterHours,
    /// A binary contact/open-close state change (door, gate, window, enclosure).
    ContactStateChange,
    /// An object previously present in a zone is no longer present.
    ObjectRemovedFromZone,
    /// Tampering with the witnessing device itself (enclosure opened, camera
    /// covered/blinded, thermal-attack temperature drift). Coarse claim only.
    TamperDetected,
    /// A vehicle arrived at or departed a zone (e.g. ignition on/off sensed
    /// passively off a vehicle's own CAN bus). No plate, make, model, trip
    /// data, or GPS trail — a binary state change scoped to a zone.
    VehicleArrivalDeparture,
}

impl ClaimKind {
    /// Stable, lowercase identifier used for dedup keys and logging. Never written to the log.
    pub fn as_str(&self) -> &'static str {
        match self {
            ClaimKind::LargeObjectBoundaryCrossing => "large_object_boundary_crossing",
            ClaimKind::SmallObjectBoundaryCrossing => "small_object_boundary_crossing",
            ClaimKind::AcousticImpulseInZone => "acoustic_impulse_in_zone",
            ClaimKind::PresenceInRestrictedZone => "presence_in_restricted_zone",
            ClaimKind::VehiclePresenceAfterHours => "vehicle_presence_after_hours",
            ClaimKind::ContactStateChange => "contact_state_change",
            ClaimKind::ObjectRemovedFromZone => "object_removed_from_zone",
            ClaimKind::TamperDetected => "tamper_detected",
            ClaimKind::VehicleArrivalDeparture => "vehicle_arrival_departure",
        }
    }

    /// Parse a `ClaimKind` from its stable string identifier (for config files).
    pub fn from_str_opt(s: &str) -> Option<Self> {
        match s.trim().to_lowercase().as_str() {
            "large_object_boundary_crossing" => Some(ClaimKind::LargeObjectBoundaryCrossing),
            "small_object_boundary_crossing" => Some(ClaimKind::SmallObjectBoundaryCrossing),
            "acoustic_impulse_in_zone" => Some(ClaimKind::AcousticImpulseInZone),
            "presence_in_restricted_zone" => Some(ClaimKind::PresenceInRestrictedZone),
            "vehicle_presence_after_hours" => Some(ClaimKind::VehiclePresenceAfterHours),
            "contact_state_change" => Some(ClaimKind::ContactStateChange),
            "object_removed_from_zone" => Some(ClaimKind::ObjectRemovedFromZone),
            "tamper_detected" => Some(ClaimKind::TamperDetected),
            "vehicle_arrival_departure" => Some(ClaimKind::VehicleArrivalDeparture),
            _ => None,
        }
    }
}

/// Host-controlled mapping from a vendor-neutral [`ClaimKind`] to a kernel [`EventType`].
///
/// This mapping lives in the trusted host, never in an adapter, so an adapter cannot choose
/// which `EventType` it emits except through the closed `ClaimKind` vocabulary.
pub fn claim_kind_to_event_type(kind: ClaimKind) -> EventType {
    match kind {
        ClaimKind::LargeObjectBoundaryCrossing => EventType::BoundaryCrossingObjectLarge,
        ClaimKind::SmallObjectBoundaryCrossing => EventType::BoundaryCrossingObjectSmall,
        ClaimKind::AcousticImpulseInZone => EventType::AcousticImpulseInZone,
        ClaimKind::PresenceInRestrictedZone => EventType::PresenceInRestrictedZone,
        ClaimKind::VehiclePresenceAfterHours => EventType::VehiclePresenceAfterHours,
        ClaimKind::ContactStateChange => EventType::ContactStateChange,
        ClaimKind::ObjectRemovedFromZone => EventType::ObjectRemovedFromZone,
        ClaimKind::TamperDetected => EventType::TamperDetected,
        ClaimKind::VehicleArrivalDeparture => EventType::VehicleArrivalDeparture,
    }
}

/// A pre-sanitized, vendor-neutral claim produced by an adapter.
///
/// The only egress for a `Claim` is the host handing a derived [`CandidateEvent`] to
/// `Kernel::append_event_checked`. Adapters never write to the log.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Claim {
    /// What kind of coarse event occurred.
    pub kind: ClaimKind,
    /// Raw, human-friendly zone label (e.g. "Front Door"). The host sanitizes and prefixes it.
    pub zone_label: String,
    /// Adapter's confidence (0..=1). Passed through to the kernel, which bounds-checks it.
    pub confidence: f32,
    /// Optional hint used only for in-bucket deduplication. **Never logged or exported.**
    pub dedup_hint: Option<String>,
    /// Provenance override. `None` means plain adapter provenance — every claim
    /// on this path is kernel-signed at ingest, so the mapped event is stamped
    /// [`Attestation::Adapter`] unless the route declares it transited Home
    /// Assistant first ([`Attestation::HaBridged`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attestation: Option<Attestation>,
}

impl Claim {
    /// Construct a claim with no dedup hint.
    pub fn new(kind: ClaimKind, zone_label: impl Into<String>, confidence: f32) -> Self {
        Self {
            kind,
            zone_label: zone_label.into(),
            confidence,
            dedup_hint: None,
            attestation: None,
        }
    }

    /// Attach a dedup hint (consumed by the host, never logged).
    pub fn with_dedup_hint(mut self, hint: impl Into<String>) -> Self {
        self.dedup_hint = Some(hint.into());
        self
    }

    /// Declare non-default provenance (e.g. HA-statestream-bridged routes).
    pub fn with_attestation(mut self, attestation: Attestation) -> Self {
        self.attestation = Some(attestation);
        self
    }
}

/// Static description of an adapter: what claim kinds it may emit, the corresponding kernel
/// event-type allowlist, and the capabilities it requests. Mirrors [`ModuleDescriptor`].
///
/// `allowed_claim_kinds` and `allowed_event_types` SHOULD be consistent: every kind in
/// `allowed_claim_kinds` must map (via [`claim_kind_to_event_type`]) into `allowed_event_types`.
/// The kernel enforces `allowed_event_types` regardless; the claim-kind list is documentation
/// and an audit aid.
pub struct AdapterDescriptor {
    /// Stable adapter identifier (also used as the [`ModuleDescriptor::id`]).
    pub id: &'static str,
    /// The closed set of claim kinds this adapter is permitted to produce.
    pub allowed_claim_kinds: &'static [ClaimKind],
    /// The kernel event-type allowlist this adapter is authorized to emit.
    pub allowed_event_types: &'static [EventType],
    /// Capabilities requested. The host rejects any adapter requesting forbidden capabilities,
    /// exactly as `CapabilityBoundaryRuntime` does for modules.
    pub requested_capabilities: &'static [ModuleCapability],
}

impl AdapterDescriptor {
    /// Translate to a [`ModuleDescriptor`] so the *unchanged* kernel allowlist gate
    /// (`enforce_module_event_allowlist`) governs adapter output identically to module output.
    pub fn to_module_descriptor(&self) -> ModuleDescriptor {
        ModuleDescriptor {
            id: self.id,
            allowed_event_types: self.allowed_event_types,
            requested_capabilities: self.requested_capabilities,
            supported_backends: &[InferenceBackend::Stub],
        }
    }
}

/// Build the kernel-facing [`CandidateEvent`] from a [`Claim`] and a coarse [`TimeBucket`].
///
/// This is where the raw `zone_label` is sanitized and prefixed. Confidence is passed through
/// unchanged so the kernel's `ContractEnforcer` performs the authoritative bounds check.
pub fn claim_to_candidate(claim: &Claim, bucket: TimeBucket) -> CandidateEvent {
    CandidateEvent {
        event_type: claim_kind_to_event_type(claim.kind),
        time_bucket: bucket,
        zone_id: format!("zone:{}", sanitize_zone_name(&claim.zone_label)),
        confidence: claim.confidence,
        correlation_token: None,
        // Everything on the adapter path is kernel-signed at ingest, never
        // device-signed: stamp Adapter unless the claim declared it also
        // transited Home Assistant. A claim cannot opt UP to device-attested —
        // Attestation has no such variant by construction.
        attestation: Some(claim.attestation.unwrap_or(Attestation::Adapter)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adapter_claims_default_to_adapter_attestation() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        // The adapter path is never device-signed: an undeclared claim maps
        // to Adapter provenance, and a declared HA hop is preserved.
        let plain = claim_to_candidate(
            &Claim::new(ClaimKind::PresenceInRestrictedZone, "bedroom", 0.9),
            bucket,
        );
        assert_eq!(plain.attestation, Some(Attestation::Adapter));

        let bridged = claim_to_candidate(
            &Claim::new(ClaimKind::PresenceInRestrictedZone, "bedroom", 0.9)
                .with_attestation(Attestation::HaBridged),
            bucket,
        );
        assert_eq!(bridged.attestation, Some(Attestation::HaBridged));
    }

    #[test]
    fn every_claim_kind_round_trips_to_string() {
        for kind in [
            ClaimKind::LargeObjectBoundaryCrossing,
            ClaimKind::SmallObjectBoundaryCrossing,
            ClaimKind::AcousticImpulseInZone,
            ClaimKind::PresenceInRestrictedZone,
            ClaimKind::VehiclePresenceAfterHours,
            ClaimKind::ContactStateChange,
            ClaimKind::ObjectRemovedFromZone,
            ClaimKind::TamperDetected,
            ClaimKind::VehicleArrivalDeparture,
        ] {
            assert_eq!(ClaimKind::from_str_opt(kind.as_str()), Some(kind));
        }
    }

    #[test]
    fn claim_to_candidate_sanitizes_and_prefixes_zone() {
        let bucket = TimeBucket::now(600).expect("bucket");
        let claim = Claim::new(ClaimKind::AcousticImpulseInZone, "Front Door!!", 0.9);
        let cand = claim_to_candidate(&claim, bucket);
        assert_eq!(cand.zone_id, "zone:front_door__");
        assert_eq!(cand.event_type, EventType::AcousticImpulseInZone);
        assert!(cand.correlation_token.is_none());
    }
}
