//! Load-bearing proof: an adapter cannot bypass the kernel's contract enforcement.
//!
//! The security argument is structural — the only egress from an adapter to the sealed log is
//! `AdapterHost::process_claim` building a `CandidateEvent` and calling
//! `Kernel::append_event_checked`, which runs three independent fail-closed gates. There is no
//! method on `AdapterHost` that writes an `Event`/`CandidateEvent` to the log bypassing the
//! enforcer. These tests exercise the gates by driving a deliberately misbehaving adapter through
//! the host.
//!
//! Run with: `cargo test --test adapter_cannot_bypass_enforcer --features adapter-mqtt-sensor`

#![cfg(feature = "adapter-mqtt-sensor")]

use std::time::Duration;

use witness_kernel::adapter::{
    AdapterDescriptor, AdapterHost, AdapterHostConfig, Claim, ClaimKind, SensorAdapter,
};
use witness_kernel::{EventType, ExportOptions, Kernel, KernelConfig, ZonePolicy};

/// An adapter whose descriptor authorizes ONLY large-object boundary crossings, but which tries
/// to emit an acoustic-impulse claim. The kernel allowlist gate must reject it.
struct OverclaimingAdapter;

static OVERCLAIM_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "overclaiming_adapter",
    // Declares only the large-crossing kind...
    allowed_claim_kinds: &[ClaimKind::LargeObjectBoundaryCrossing],
    // ...and only the corresponding event type.
    allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
    requested_capabilities: &[],
};

impl SensorAdapter for OverclaimingAdapter {
    fn name(&self) -> &'static str {
        "overclaiming_adapter"
    }
    fn descriptor(&self) -> &'static AdapterDescriptor {
        &OVERCLAIM_DESCRIPTOR
    }
    fn poll(&mut self) -> anyhow::Result<Vec<Claim>> {
        // Emits a claim OUTSIDE its declared vocabulary.
        Ok(vec![Claim::new(
            ClaimKind::AcousticImpulseInZone,
            "garage",
            0.9,
        )])
    }
}

fn setup_host() -> (AdapterHost, [u8; 32]) {
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:bypass_test");
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:bypass_test".to_string(),
        ruleset_hash: hash,
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:bypass_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    let host = AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:bypass_test".to_string(),
            ruleset_hash: hash,
            min_confidence: 0.0,
        },
    );
    (host, hash)
}

fn sealed_event_count(host: &mut AdapterHost, hash: [u8; 32]) -> usize {
    let artifact = host
        .kernel_mut()
        .export_events_for_api(hash, ExportOptions::default())
        .expect("export");
    artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum()
}

#[test]
fn adapter_emitting_disallowed_claim_kind_is_rejected_via_run_once() {
    let (mut host, hash) = setup_host();
    host.register(OverclaimingAdapter);

    // run_once logs and skips the rejected claim; zero events are sealed.
    let written = host.run_once().expect("run_once");
    assert_eq!(written, 0, "over-claimed event must not be sealed");
    assert_eq!(sealed_event_count(&mut host, hash), 0);
}

#[test]
fn process_claim_surfaces_allowlist_rejection_as_error() {
    let (mut host, hash) = setup_host();
    // Drive process_claim directly with the restrictive descriptor + an out-of-vocabulary claim.
    let claim = Claim::new(ClaimKind::AcousticImpulseInZone, "garage", 0.9);
    let result = host.process_claim(&OVERCLAIM_DESCRIPTOR, &claim);
    assert!(
        result.is_err(),
        "kernel allowlist gate must reject a claim outside the adapter's declared event types"
    );
    assert_eq!(sealed_event_count(&mut host, hash), 0);
}

#[test]
fn an_allowed_claim_from_the_same_adapter_still_succeeds() {
    let (mut host, hash) = setup_host();
    // The large-crossing kind IS in the descriptor's allowlist, so it seals normally.
    let claim = Claim::new(ClaimKind::LargeObjectBoundaryCrossing, "driveway", 0.7);
    let event = host
        .process_claim(&OVERCLAIM_DESCRIPTOR, &claim)
        .expect("process")
        .expect("event");
    assert_eq!(event.event_type, EventType::BoundaryCrossingObjectLarge);
    assert_eq!(sealed_event_count(&mut host, hash), 1);
}
