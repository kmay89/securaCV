//! End-to-end tests for webhook auth + rate limiting, the BLE presence adapter, and per-adapter
//! observability counters.
//!
//! Run with: `cargo test --test adapter_increment4 \
//!   --features adapter-webhook,adapter-ble-presence,adapter-sandbox`

#![cfg(all(feature = "adapter-webhook", feature = "adapter-ble-presence"))]

use std::io::{Read, Write};
use std::net::TcpStream;
use std::thread;
use std::time::Duration;

use witness_kernel::adapter::ble_presence::{BlePresenceAdapter, BleRoom};
use witness_kernel::adapter::webhook::{
    self, RateLimit, WebhookAdapter, WebhookAuth, WebhookOptions,
};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig, ClaimKind};
use witness_kernel::{EventType, ExportOptions, Kernel, KernelConfig, ZonePolicy};

fn setup_host() -> (AdapterHost, [u8; 32]) {
    let hash = KernelConfig::ruleset_hash_from_id("ruleset:incr4_test");
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:incr4_test".to_string(),
        ruleset_hash: hash,
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:incr4_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    let host = AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:incr4_test".to_string(),
            ruleset_hash: hash,
            min_confidence: 0.0,
        },
    );
    (host, hash)
}

fn acoustic_route() -> witness_kernel::adapter::mqtt_sensor::SensorRoute {
    witness_kernel::adapter::mqtt_sensor::SensorRoute::new(
        "/sensors/garage/acoustic",
        ClaimKind::AcousticImpulseInZone,
        "garage",
    )
}

/// Send a POST and return the raw HTTP response.
fn post(addr: std::net::SocketAddr, headers: &str, body: &str) -> String {
    let request = format!(
        "POST /sensors/garage/acoustic HTTP/1.1\r\nHost: x\r\n{headers}Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    let mut stream = TcpStream::connect(addr).expect("connect");
    stream.write_all(request.as_bytes()).expect("write");
    let mut response = String::new();
    stream.read_to_string(&mut response).expect("read");
    response
}

#[test]
fn webhook_bearer_auth_is_enforced() {
    let (mut host, _hash) = setup_host();
    let (adapter, tx) = WebhookAdapter::new(vec![acoustic_route()]);
    let listener = webhook::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    let options = WebhookOptions {
        auth: WebhookAuth::Bearer("topsecret".to_string()),
        ..Default::default()
    };
    thread::spawn(move || {
        let _ = webhook::serve_listener_with_options(listener, tx, options);
    });
    host.register(adapter);

    // No token → 401, nothing sealed.
    let resp = post(addr, "", r#"{"confidence":0.8}"#);
    assert!(resp.contains("401"), "missing token should 401: {resp}");
    assert_eq!(host.run_once().expect("run"), 0);

    // Wrong token → 401.
    let resp = post(
        addr,
        "Authorization: Bearer nope\r\n",
        r#"{"confidence":0.8}"#,
    );
    assert!(resp.contains("401"), "wrong token should 401: {resp}");
    assert_eq!(host.run_once().expect("run"), 0);

    // Correct token → 204 and a sealed event.
    let resp = post(
        addr,
        "Authorization: Bearer topsecret\r\n",
        r#"{"confidence":0.8}"#,
    );
    assert!(resp.contains("204"), "valid token should 204: {resp}");
    assert_eq!(host.run_once().expect("run"), 1);
}

#[test]
fn webhook_rate_limit_returns_429() {
    let (mut host, _hash) = setup_host();
    let (adapter, tx) = WebhookAdapter::new(vec![acoustic_route()]);
    let listener = webhook::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    let options = WebhookOptions {
        // Burst of 1, negligible refill → the second immediate request is throttled.
        rate_limit: Some(RateLimit {
            capacity: 1.0,
            refill_per_sec: 0.0001,
        }),
        ..Default::default()
    };
    thread::spawn(move || {
        let _ = webhook::serve_listener_with_options(listener, tx, options);
    });
    host.register(adapter);

    let first = post(addr, "", r#"{"confidence":0.8}"#);
    assert!(first.contains("204"), "first request should pass: {first}");
    let second = post(addr, "", r#"{"confidence":0.8}"#);
    assert!(
        second.contains("429"),
        "second should be throttled: {second}"
    );
}

#[test]
fn ble_presence_seals_and_records_stats() {
    let (mut host, hash) = setup_host();
    let (adapter, tx) = BlePresenceAdapter::new(vec![BleRoom::new("lobby", "lobby", 4.0)]);
    // Two devices seen close in the same room within one bucket.
    tx.send((
        "espresense/devices/aabbcc/lobby".to_string(),
        br#"{"distance":1.2}"#.to_vec(),
    ))
    .unwrap();
    tx.send((
        "espresense/devices/ddeeff/lobby".to_string(),
        br#"{"distance":2.0}"#.to_vec(),
    ))
    .unwrap();
    host.register(adapter);

    // First device seals; the second is collapsed by per-bucket dedup → exactly one event.
    let written = host.run_once().expect("run_once");
    assert_eq!(written, 1, "presence should seal one event per bucket");

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

    // Observability: counters reflect what happened. The registry keys adapters as
    // "{name}#{index}", so match by prefix.
    let stats = host.stats_snapshot();
    let s = stats
        .iter()
        .find(|(k, _)| k.starts_with("ble_presence_adapter"))
        .map(|(_, v)| v)
        .expect("ble adapter stats present");
    assert_eq!(s.polls, 1);
    assert_eq!(s.claims_emitted, 2);
    assert_eq!(s.claims_sealed, 1);
    assert_eq!(s.claims_filtered, 1); // the deduped second claim
    assert!(s.last_seal_epoch_s > 0);
}
