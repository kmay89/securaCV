//! Tests for the hot-reloadable adapter config primitives (increment 6): live route swaps and the
//! host confidence-floor setter that SIGHUP reload builds on.
//!
//! Run with: `cargo test --test adapter_increment6 \
//!   --features adapter-webhook,adapter-mqtt-sensor`

#![cfg(all(feature = "adapter-mqtt-sensor", feature = "adapter-webhook"))]

use std::time::Duration;

use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig, ClaimKind, SensorAdapter};
use witness_kernel::{ExportOptions, Kernel, KernelConfig, ZonePolicy};

fn setup_host(min_confidence: f32) -> (AdapterHost, [u8; 32]) {
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:incr6");
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:incr6".to_string(),
        ruleset_hash: hash,
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:incr6_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    let host = AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:incr6".to_string(),
            ruleset_hash: hash,
            min_confidence,
        },
    );
    (host, hash)
}

#[test]
fn routes_hot_reload_changes_poll_output() {
    // Start with an empty routing table: the message routes to nothing.
    let (mut adapter, tx) = MqttSensorAdapter::new(vec![]);
    let handle = adapter.routes_handle();

    tx.send((
        "sensors/garage/acoustic".to_string(),
        br#"{"confidence":0.9}"#.to_vec(),
    ))
    .unwrap();
    assert!(
        adapter.poll().expect("poll").is_empty(),
        "no routes yet => no claims"
    );

    // Hot-reload: install a matching route via the shared handle (what SIGHUP does in the binary).
    *handle.lock().unwrap() = vec![SensorRoute::new(
        "sensors/garage/acoustic",
        ClaimKind::AcousticImpulseInZone,
        "garage",
    )];

    tx.send((
        "sensors/garage/acoustic".to_string(),
        br#"{"confidence":0.9}"#.to_vec(),
    ))
    .unwrap();
    let claims = adapter.poll().expect("poll");
    assert_eq!(claims.len(), 1, "route added at runtime => claim produced");
    assert_eq!(claims[0].kind, ClaimKind::AcousticImpulseInZone);

    // And reloading back to empty stops production again.
    handle.lock().unwrap().clear();
    tx.send((
        "sensors/garage/acoustic".to_string(),
        br#"{"confidence":0.9}"#.to_vec(),
    ))
    .unwrap();
    assert!(adapter.poll().expect("poll").is_empty());
}

#[test]
fn host_min_confidence_reload_changes_filtering() {
    // Floor above the claim's confidence: nothing seals.
    let (mut host, hash) = setup_host(0.9);
    let (adapter, tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
        "sensors/lobby/pir",
        ClaimKind::PresenceInRestrictedZone,
        "lobby",
    )]);
    let send = |tx: &std::sync::mpsc::Sender<(String, Vec<u8>)>| {
        tx.send((
            "sensors/lobby/pir".to_string(),
            br#"{"confidence":0.5}"#.to_vec(),
        ))
        .unwrap();
    };
    send(&tx);
    host.register(adapter);
    assert_eq!(
        host.run_once().expect("run"),
        0,
        "0.5 < floor 0.9 => filtered"
    );

    // Lower the floor (what SIGHUP reload does), then the same claim seals.
    host.set_min_confidence(0.0);
    send(&tx);
    assert_eq!(
        host.run_once().expect("run"),
        1,
        "0.5 >= floor 0.0 => sealed"
    );

    let artifact = host
        .kernel_mut()
        .export_events_for_api(hash, ExportOptions::default())
        .expect("export");
    let sealed: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();
    assert_eq!(sealed, 1);
}
