//! Integration tests for the Meshtastic adapter: JSON-mode mesh frames round-trip through the
//! SAME kernel choke point (`append_event_checked`) as every other source, per-bucket dedup
//! collapses repeated detections, and — the load-bearing privacy property — no Meshtastic
//! identifier or RF metadata survives into the sealed log or its export.
//!
//! Run with: `cargo test --test adapter_meshtastic --features adapter-meshtastic`

#![cfg(feature = "adapter-meshtastic")]

use std::time::Duration;

use witness_kernel::adapter::meshtastic::{MeshNode, MeshtasticAdapter};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig, ClaimKind, SensorAdapter};
use witness_kernel::{EventType, ExportOptions, Kernel, KernelConfig, ZonePolicy};

const TOPIC: &str = "msh/US/2/json/SecuraCV/!aabbccdd";
/// The configured sensor node, 0x7d3a9f7f.
const NODE_DECIMAL: &str = "2100993919";
const NODE_HEX: &str = "7d3a9f7f";

fn setup_host() -> AdapterHost {
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:lora_adapter_test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:lora_adapter_test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:lora_adapter_test:0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    AdapterHost::new(
        kernel,
        AdapterHostConfig {
            bucket_size_secs: 600,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:lora_adapter_test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:lora_adapter_test"),
            min_confidence: 0.0,
        },
    )
}

fn adapter() -> (
    MeshtasticAdapter,
    std::sync::mpsc::Sender<(String, Vec<u8>)>,
) {
    MeshtasticAdapter::new(vec![MeshNode::new(
        0x7d3a9f7f,
        ClaimKind::PresenceInRestrictedZone,
        "back_gate",
    )])
}

fn detection_frame() -> Vec<u8> {
    format!(
        r#"{{"channel":0,"from":{NODE_DECIMAL},"id":987654,"payload":{{"text":"Motion detected"}},"rssi":-61,"sender":"!{NODE_HEX}","snr":12.25,"timestamp":1721239944,"to":4294967295,"type":"detection"}}"#
    )
    .into_bytes()
}

#[test]
fn detection_frame_seals_a_coarse_presence_event() {
    let mut host = setup_host();
    let (adapter, _tx) = adapter();
    let claim = adapter
        .message_to_claim(TOPIC, &detection_frame())
        .expect("claim");

    let event = host
        .process_claim(adapter.descriptor(), &claim)
        .expect("process")
        .expect("event written");

    assert_eq!(event.event_type, EventType::PresenceInRestrictedZone);
    assert_eq!(event.zone_id, "zone:back_gate");
    // Time is coarsened to a 10-minute bucket; the packet timestamp was never even parsed.
    assert_eq!(event.time_bucket.size_s, 600);
    assert_eq!(event.time_bucket.start_epoch_s % 600, 0);
    assert!(event.correlation_token.is_none());
}

#[test]
fn repeated_detections_in_a_bucket_collapse_to_one_event() {
    let mut host = setup_host();
    let (mut adapter, tx) = adapter();
    // A nervous PIR (or a replayed packet) fires three times in quick succession.
    for _ in 0..3 {
        tx.send((TOPIC.to_string(), detection_frame())).unwrap();
    }
    let claims = adapter.poll().expect("poll");
    assert_eq!(claims.len(), 3);
    let desc = adapter.descriptor();
    let mut sealed = 0;
    for claim in &claims {
        if host.process_claim(desc, claim).expect("process").is_some() {
            sealed += 1;
        }
    }
    assert_eq!(sealed, 1, "per-bucket dedup must collapse repeats");
}

#[test]
fn export_carries_no_node_id_or_rf_metadata() {
    let mut host = setup_host();
    let (adapter, _tx) = adapter();
    let claim = adapter
        .message_to_claim(TOPIC, &detection_frame())
        .expect("claim");
    host.process_claim(adapter.descriptor(), &claim)
        .expect("process")
        .expect("event written");

    let hash = KernelConfig::ruleset_hash_from_id("ruleset:lora_adapter_test");
    let artifact = host
        .kernel_mut()
        .export_events_for_api(hash, ExportOptions::default())
        .expect("export");
    let json = serde_json::to_string(&artifact).expect("serialize export");

    // The stable Meshtastic node id, in any display form, must not survive into the export…
    assert!(!json.contains(NODE_DECIMAL), "decimal node id leaked");
    assert!(!json.contains(NODE_HEX), "hex node id leaked");
    // …and neither may RF metadata or the alert text.
    for forbidden in ["snr", "rssi", "Motion detected", "meshtastic"] {
        assert!(
            !json.to_lowercase().contains(forbidden),
            "{forbidden} leaked"
        );
    }
    // The coarse zone is the only attribution that remains.
    assert!(json.contains("zone:back_gate"));
}
