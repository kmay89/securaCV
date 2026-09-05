//! End-to-end gate for the Frigate → MQTT → Privacy Witness Kernel pipeline.
//!
//! This is the deterministic, Docker-free core of the
//! `integrations/ha_frigate_mqtt` release gate. It reproduces exactly what the
//! `frigate_bridge` binary does with a `frigate/events` payload — parse,
//! strip identity, coarsen the timestamp, map the label, sanitize the zone, and
//! `append_event_checked` — against a **real, SQLCipher-encrypted** on-disk
//! `witness.db`, then runs the **real `log_verify` binary** to prove the sealed
//! log verifies (checkpoint signature + per-event signatures + hash chain).
//!
//! The live-broker variant (a mosquitto service + the real binary) lives in CI
//! (`.github/workflows/frigate_mqtt_e2e.yml`); this test pins the logic so a
//! regression is caught by `cargo test`, with no broker or container required.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::Command;
use std::time::Duration;

use witness_kernel::api::{ApiConfig, ApiServer};
use witness_kernel::transport::{map_label_to_event_type, parse_frigate_event, sanitize_zone_name};
use witness_kernel::{
    derive_db_encryption_key, signing_key_from_seed, verifying_key_from_seed, CandidateEvent,
    EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:frigate_e2e:00112233445566778899aabbccddeeff";
const RULESET_ID: &str = "ruleset:frigate_v1";

/// A realistic Frigate `frigate/events` payload (a "new" person detection that
/// carries an object id, precise zones, and a clip/snapshot — all of which the
/// bridge must strip). `top_score` is above the 0.5 default confidence floor.
const FRIGATE_EVENT: &str = r#"{
    "before": null,
    "after": {
        "id": "1719000000.abc123-frigate-object-id",
        "camera": "front_door",
        "label": "person",
        "sub_label": null,
        "score": 0.81,
        "top_score": 0.92,
        "current_zones": ["porch"],
        "entered_zones": ["driveway", "porch"],
        "false_positive": false,
        "has_clip": true,
        "has_snapshot": true
    },
    "type": "new"
}"#;

fn module_desc() -> ModuleDescriptor {
    // Mirrors the descriptor `frigate_bridge` registers.
    ModuleDescriptor {
        id: "frigate_bridge",
        allowed_event_types: &[
            EventType::BoundaryCrossingObjectSmall,
            EventType::BoundaryCrossingObjectLarge,
        ],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    }
}

#[test]
fn frigate_event_flows_to_a_verifiable_sealed_log() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path_str = db_path.to_str().expect("utf8 path").to_string();

    let cfg = KernelConfig {
        db_path: db_path_str.clone(),
        ruleset_id: RULESET_ID.to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    };

    // ── Stage 1: the bridge pipeline writes one event to the sealed log ──
    {
        let mut kernel = Kernel::open(&cfg).expect("open encrypted kernel");
        let desc = module_desc();

        let parsed = parse_frigate_event(FRIGATE_EVENT.as_bytes()).expect("parse frigate event");
        assert_eq!(parsed.camera, "front_door");
        assert_eq!(parsed.label, "person");
        assert!(
            parsed.confidence >= 0.5,
            "fixture must clear the default confidence floor"
        );

        let event_type = map_label_to_event_type(&parsed.label);
        let zone_id = match parsed.zones.first() {
            Some(zone) => format!("zone:{}", sanitize_zone_name(zone)),
            None => format!("zone:{}", sanitize_zone_name(&parsed.camera)),
        };

        let candidate = CandidateEvent {
            event_type,
            time_bucket: TimeBucket::now(600).expect("time bucket"),
            zone_id: zone_id.clone(),
            confidence: parsed.confidence as f32,
            correlation_token: None, // Frigate object ids are never carried through
            attestation: None,
        };

        let event = kernel
            .append_event_checked(
                &desc,
                candidate,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
            .expect("append_event_checked");

        // Privacy invariants the bridge promises.
        assert_eq!(event.event_type, EventType::BoundaryCrossingObjectLarge); // person → large
        assert_eq!(event.zone_id, zone_id);
        assert!(event.correlation_token.is_none(), "no tracking id may leak");

        // A checkpoint is required for external verification; write one over the head.
        kernel
            .enforce_retention_with_checkpoint(cfg.retention)
            .expect("write checkpoint");
    } // kernel dropped → connection closed, db flushed to disk

    // ── Stage 2: the real `log_verify` binary verifies the encrypted log ──
    let pubkey_hex = hex::encode(verifying_key_from_seed(SEED).expect("vk").to_bytes());
    let db_key = derive_db_encryption_key(&signing_key_from_seed(SEED).expect("sk"));

    let output = Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", &db_path_str, "--public-key", &pubkey_hex])
        .env("SECURACV_DB_KEY", db_key.as_str())
        .output()
        .expect("spawn log_verify");

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "log_verify must succeed on the bridge-produced log\n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );

    // ── Stage 2b: `log_verify --device-key-seed` (the sidecar/doctor path)
    // derives both the SQLCipher key and the verifying key from the seed ──
    let output = Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", &db_path_str])
        .env("DEVICE_KEY_SEED", SEED)
        .env_remove("SECURACV_DB_KEY")
        .output()
        .expect("spawn log_verify with seed");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "log_verify must succeed via DEVICE_KEY_SEED alone\n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );

    // ── Stage 3: the event API's POST /verify reports the same outcome
    // (this is what the HA "Verify Now" button drives) ──
    let api = ApiServer::new(
        ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            ..ApiConfig::default()
        },
        cfg.clone(),
    )
    .spawn()
    .expect("spawn api");

    let mut stream = TcpStream::connect(api.addr).expect("connect api");
    let request = format!(
        "POST /verify HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {}\r\n\r\n",
        api.token
    );
    stream.write_all(request.as_bytes()).expect("send request");
    let mut response = String::new();
    stream.read_to_string(&mut response).expect("read response");
    let body = response
        .split_once("\r\n\r\n")
        .map(|(_, body)| body)
        .expect("response body");
    let report: serde_json::Value = serde_json::from_str(body).expect("verify report json");
    assert_eq!(
        report["chain_valid"], true,
        "API verification must pass on the bridge-produced log: {report}"
    );
    assert!(report["events_verified"].as_u64().unwrap_or(0) >= 1);
    api.stop().expect("stop api");
}
