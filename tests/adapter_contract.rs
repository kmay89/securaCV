//! Integration tests for the Sensor Adapter contract and host.
//!
//! These verify that claims produced by adapters round-trip through the SAME kernel choke point
//! (`append_event_checked`) used by `frigate_bridge`, that zone labels are sanitized, that the
//! coarse vocabulary maps to allowed event types, and that the kernel — not the adapter — has the
//! final say on the event contract.
//!
//! Requires the adapter feature so the host/contract are compiled in:
//! `cargo test --test adapter_contract --features adapter-frigate,adapter-mqtt-sensor`

#![cfg(feature = "adapter-mqtt-sensor")]

use std::time::Duration;

use witness_kernel::adapter::contract::claim_kind_to_event_type;
use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig, Claim, ClaimKind};
use witness_kernel::{EventType, ExportOptions, Kernel, KernelConfig, ZonePolicy};

fn setup_host(min_confidence: f32) -> AdapterHost {
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:adapter_test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:adapter_test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:adapter_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:adapter_test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:adapter_test"),
            min_confidence,
        },
    )
}

/// The generic MQTT adapter is authorized for the full vocabulary; use its descriptor for
/// round-trip tests of any claim kind.
fn permissive_descriptor() -> &'static witness_kernel::adapter::AdapterDescriptor {
    let (adapter, _tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
        "x",
        ClaimKind::AcousticImpulseInZone,
        "z",
    )]);
    use witness_kernel::adapter::SensorAdapter;
    adapter.descriptor()
}

fn exported_event_count(host: &mut AdapterHost) -> usize {
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:adapter_test");
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
fn every_claim_kind_maps_into_the_generic_allowlist() {
    let desc = permissive_descriptor();
    for kind in [
        ClaimKind::LargeObjectBoundaryCrossing,
        ClaimKind::SmallObjectBoundaryCrossing,
        ClaimKind::AcousticImpulseInZone,
        ClaimKind::PresenceInRestrictedZone,
        ClaimKind::VehiclePresenceAfterHours,
        ClaimKind::ContactStateChange,
        ClaimKind::ObjectRemovedFromZone,
        ClaimKind::TamperDetected,
    ] {
        let et = claim_kind_to_event_type(kind);
        assert!(
            desc.allowed_event_types.contains(&et),
            "kind {:?} maps to {:?} which is not in the allowlist",
            kind,
            et
        );
    }
}

#[test]
fn tamper_claim_round_trips_into_sealed_log() {
    // The Canary firmware publishes enclosure/camera/temp-drift tamper on
    // securacv/<id>/tamper; the mqtt_sensor route turns it into this claim.
    // It must seal as a TamperDetected event through the standard gates.
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::TamperDetected, "Canary 1", 0.93);

    let event = host
        .process_claim(desc, &claim)
        .expect("process")
        .expect("event written");

    assert_eq!(event.event_type, EventType::TamperDetected);
    assert_eq!(event.zone_id, "zone:canary_1");
    assert_eq!(event.time_bucket.size_s, 600);
    assert!(event.correlation_token.is_none());

    assert_eq!(exported_event_count(&mut host), 1);
}

#[test]
fn acoustic_claim_round_trips_into_sealed_log() {
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::AcousticImpulseInZone, "Garage Bay", 0.8);

    let event = host
        .process_claim(desc, &claim)
        .expect("process")
        .expect("event written");

    assert_eq!(event.event_type, EventType::AcousticImpulseInZone);
    assert_eq!(event.zone_id, "zone:garage_bay");
    // Time is coarsened to a 10-minute bucket regardless of adapter input.
    assert_eq!(event.time_bucket.size_s, 600);
    assert_eq!(event.time_bucket.start_epoch_s % 600, 0);
    assert!(event.correlation_token.is_none());

    assert_eq!(exported_event_count(&mut host), 1);
}

#[test]
fn zone_label_with_punctuation_is_sanitized() {
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::PresenceInRestrictedZone, "Front Door!!", 0.9);
    let event = host
        .process_claim(desc, &claim)
        .expect("process")
        .expect("event");
    assert_eq!(event.zone_id, "zone:front_door__");
}

#[test]
fn empty_zone_label_is_rejected_by_the_kernel() {
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    // Sanitizes to "zone:" which fails the kernel's `^zone:[a-z0-9_-]{1,64}$` allowlist.
    let claim = Claim::new(ClaimKind::ContactStateChange, "", 0.9);
    let result = host.process_claim(desc, &claim);
    assert!(result.is_err(), "empty zone must be rejected");
    // No event sealed, but the kernel recorded a fail-closed FailureEvent (not counted here).
    assert_eq!(exported_event_count(&mut host), 0);
}

#[test]
fn out_of_bounds_confidence_is_rejected_by_the_kernel() {
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::AcousticImpulseInZone, "garage", 1.5);
    let result = host.process_claim(desc, &claim);
    assert!(result.is_err(), "confidence 1.5 must be rejected");
    assert_eq!(exported_event_count(&mut host), 0);
}

#[test]
fn below_confidence_floor_is_dropped_silently() {
    let mut host = setup_host(0.5);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::AcousticImpulseInZone, "garage", 0.2);
    // Filtered by the host floor: Ok(None), not an error, nothing sealed.
    assert!(host.process_claim(desc, &claim).expect("process").is_none());
    assert_eq!(exported_event_count(&mut host), 0);
}

#[test]
fn multiple_adapters_of_same_type_all_register() {
    let mut host = setup_host(0.0);
    let (a1, _tx1) = MqttSensorAdapter::new(vec![SensorRoute::new(
        "s/1",
        ClaimKind::ContactStateChange,
        "z1",
    )]);
    let (a2, _tx2) = MqttSensorAdapter::new(vec![SensorRoute::new(
        "s/2",
        ClaimKind::ContactStateChange,
        "z2",
    )]);
    host.register(a1);
    host.register(a2);
    // Both instances coexist — the second does not replace the first under a shared name.
    assert_eq!(host.adapter_count(), 2);
    // Polling both is fine (no claims fed => zero sealed, no panic).
    assert_eq!(host.run_once().expect("run_once"), 0);
}

#[test]
fn below_floor_confidence_never_reaches_the_export() {
    // The numeric gates reject silently; this pins that a below-floor raw
    // value is ABSENT from the serialized export (the adapter_meshtastic
    // absence pattern), not merely that no event object was returned.
    let mut host = setup_host(0.8);
    let desc = permissive_descriptor();

    let mut route = SensorRoute::new(
        "sensors/garage/acoustic",
        ClaimKind::AcousticImpulseInZone,
        "garage",
    );
    route.min_confidence = 0.8;
    let (adapter, _tx) = MqttSensorAdapter::new(vec![route]);
    assert!(
        adapter
            .message_to_claim("sensors/garage/acoustic", br#"{"confidence":0.123}"#)
            .is_none(),
        "below-floor payload must be dropped at the adapter gate"
    );

    // A claim that reaches the host below ITS floor must not seal either.
    let sneaky = Claim::new(ClaimKind::AcousticImpulseInZone, "garage", 0.123);
    assert!(
        host.process_claim(desc, &sneaky)
            .expect("process")
            .is_none(),
        "host confidence floor must drop the claim"
    );

    assert_eq!(exported_event_count(&mut host), 0);
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:adapter_test");
    let artifact = host
        .kernel_mut()
        .export_events_for_api(hash, ExportOptions::default())
        .expect("export");
    let json = serde_json::to_string(&artifact).expect("serialize");
    assert!(
        !json.contains("0.123"),
        "raw below-floor confidence leaked into the export"
    );
}

#[test]
fn duplicate_claim_in_same_bucket_is_deduplicated() {
    let mut host = setup_host(0.0);
    let desc = permissive_descriptor();
    let claim = Claim::new(ClaimKind::PresenceInRestrictedZone, "lobby", 0.7);
    assert!(host.process_claim(desc, &claim).expect("p1").is_some());
    // Same kind+zone within the same bucket => deduped.
    assert!(host.process_claim(desc, &claim).expect("p2").is_none());
    assert_eq!(exported_event_count(&mut host), 1);
}
