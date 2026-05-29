//! End-to-end tests for the webhook ingress adapter and the optional sandbox parse path.
//!
//! Run with:
//! `cargo test --test adapter_webhook_sandbox --features adapter-webhook,adapter-sandbox`

#![cfg(all(feature = "adapter-webhook", feature = "adapter-sandbox"))]

use std::io::{Read, Write};
use std::net::TcpStream;
use std::thread;
use std::time::Duration;

use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::adapter::webhook::{self, WebhookAdapter};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig};
use witness_kernel::{EventType, ExportOptions, Kernel, KernelConfig, ZonePolicy};

fn setup_host() -> (AdapterHost, [u8; 32]) {
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:webhook_test");
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:webhook_test".to_string(),
        ruleset_hash: hash,
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:webhook_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    let host = AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:webhook_test".to_string(),
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
fn webhook_post_round_trips_into_sealed_log() {
    let (mut host, hash) = setup_host();

    let routes = vec![SensorRoute::new(
        "/sensors/garage/acoustic",
        witness_kernel::adapter::ClaimKind::AcousticImpulseInZone,
        "garage",
    )];
    let (adapter, tx) = WebhookAdapter::new(routes);

    // Bind to an ephemeral port and serve on a background thread.
    let listener = webhook::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    thread::spawn(move || {
        let _ = webhook::serve_listener(listener, tx);
    });
    host.register(adapter);

    // POST a sensor reading over real HTTP.
    let body = r#"{"confidence":0.8}"#;
    let request = format!(
        "POST /sensors/garage/acoustic HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        body.len(),
        body
    );
    let mut stream = TcpStream::connect(addr).expect("connect");
    stream.write_all(request.as_bytes()).expect("write");
    let mut response = String::new();
    stream.read_to_string(&mut response).expect("read");
    assert!(response.contains("204"), "expected 204, got: {response}");

    // The server enqueues before responding, so the claim is available now.
    let written = host.run_once().expect("run_once");
    assert_eq!(written, 1, "webhook POST should seal exactly one event");
    assert_eq!(sealed_event_count(&mut host, hash), 1);

    // A non-POST method is rejected and seals nothing.
    let mut stream = TcpStream::connect(addr).expect("connect2");
    stream
        .write_all(b"GET /sensors/garage/acoustic HTTP/1.1\r\nConnection: close\r\n\r\n")
        .expect("write2");
    let mut response = String::new();
    stream.read_to_string(&mut response).expect("read2");
    assert!(response.contains("405"), "expected 405, got: {response}");
}

#[test]
fn sandboxed_mqtt_adapter_seals_event_through_host() {
    let (mut host, hash) = setup_host();
    let (adapter, tx) = MqttSensorAdapter::new(vec![SensorRoute::new(
        "sensors/lobby/pir",
        witness_kernel::adapter::ClaimKind::PresenceInRestrictedZone,
        "lobby",
    )]);
    // Parse inside the seccomp sandbox.
    let adapter = adapter.with_sandbox(true);
    tx.send((
        "sensors/lobby/pir".to_string(),
        br#"{"confidence":0.9}"#.to_vec(),
    ))
    .unwrap();
    host.register(adapter);

    let written = host.run_once().expect("run_once");
    assert_eq!(written, 1, "sandboxed parse should still seal one event");
    assert_eq!(sealed_event_count(&mut host, hash), 1);

    // Sanity: the sealed event is the expected coarse claim.
    let artifact = host
        .kernel_mut()
        .export_events_for_api(hash, ExportOptions::default())
        .expect("export");
    let types: Vec<EventType> = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .flat_map(|bucket| bucket.events.iter())
        .map(|e| e.event_type.clone())
        .collect();
    assert_eq!(types, vec![EventType::PresenceInRestrictedZone]);
}
