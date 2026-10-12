//! End-to-end gate for the Frigate → MQTT → Privacy Witness Kernel pipeline.
//!
//! Two layers, both deterministic and Docker-free:
//!
//! 1. The pipeline, in-process: exactly what the `frigate_bridge` binary does
//!    with a `frigate/events` payload — parse, strip identity, coarsen the
//!    timestamp, map the label, sanitize the zone, and `append_event_checked`
//!    — against a **real, SQLCipher-encrypted** on-disk `witness.db`, then
//!    the **real `log_verify` binary** proves the sealed log verifies.
//! 2. The process: the real `frigate_bridge` binary against an in-test MQTT
//!    v5 broker stub. It proves the appending process owns its housekeeping
//!    (spec D6: lifecycle + heartbeat records, the retention pass pruning an
//!    expired row and checkpointing the cutoff, the signed high-water mark at
//!    `SECURACV_HWM_PATH`), publishes the retained `witness/frigate_link`
//!    payload (spec D14), compares cameras by the shared slug (C8), subscribes
//!    to exactly `<prefix>/events`, and — with a sentinel-laden Frigate 0.17
//!    payload fed through the socket — leaks no discarded value into the
//!    sealed log or onto MQTT (D7).
//!
//! The live-broker variant (a mosquitto service + the real binary) lives in CI
//! (`.github/workflows/frigate_mqtt_e2e.yml`); these tests pin the logic so a
//! regression is caught by `cargo test`, with no broker or container required.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::Command;
use std::time::Duration;

use witness_kernel::api::{ApiConfig, ApiServer};
use witness_kernel::transport::frigate::{camera_slug, select_event_type, DEFAULT_LABELS};
use witness_kernel::transport::{parse_frigate_event, sanitize_zone_name};
use witness_kernel::{
    derive_db_encryption_key, signing_key_from_seed, verifying_key_from_seed, CandidateEvent,
    EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:frigate_e2e:00112233445566778899aabbccddeeff";
const RULESET_ID: &str = "ruleset:frigate_v1";

/// A realistic Frigate `frigate/events` payload (a "new" person detection that
/// carries an object id, a recognized name, precise zones, and a clip/snapshot
/// — all of which the bridge must strip). `top_score` is above the 0.5
/// default confidence floor. The camera is named with a space, the way an
/// operator types it into the add-on wizard.
const FRIGATE_EVENT: &str = r#"{
    "before": null,
    "after": {
        "id": "1719000000.abc123-frigate-object-id",
        "camera": "Front Door",
        "label": "person",
        "sub_label": ["Someone Known", 0.79],
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

fn kernel_config(db_path: &str, retention: Duration) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: RULESET_ID.to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention,
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

/// Run the real `log_verify` binary over `db_path` both ways the operators do
/// (an explicit public key, and the seed alone) and assert it passes.
fn assert_log_verify_passes(db_path: &str) {
    let pubkey_hex = hex::encode(verifying_key_from_seed(SEED).expect("vk").to_bytes());
    let db_key = derive_db_encryption_key(&signing_key_from_seed(SEED).expect("sk"));

    let output = Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", db_path, "--public-key", &pubkey_hex])
        .env("SECURACV_DB_KEY", db_key.as_str())
        .output()
        .expect("spawn log_verify");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "log_verify must succeed on the bridge-produced log\n--- stdout ---\n{stdout}\n--- stderr ---\n{stderr}"
    );

    // `log_verify --device-key-seed` (the sidecar/doctor path) derives both
    // the SQLCipher key and the verifying key from the seed.
    let output = Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", db_path])
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
}

#[test]
fn frigate_event_flows_to_a_verifiable_sealed_log() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path_str = db_path.to_str().expect("utf8 path").to_string();
    let cfg = kernel_config(&db_path_str, Duration::from_secs(60 * 60 * 24 * 7));

    // ── Stage 1: the bridge pipeline writes one event to the sealed log ──
    {
        let mut kernel = Kernel::open(&cfg).expect("open encrypted kernel");
        let desc = module_desc();

        let parsed = parse_frigate_event(FRIGATE_EVENT.as_bytes()).expect("parse frigate event");
        assert_eq!(parsed.camera, "Front Door");
        assert_eq!(parsed.labels, vec!["person"]);
        assert!(
            parsed.confidence >= 0.5,
            "fixture must clear the default confidence floor"
        );

        // The operator's allowlist names the camera the way the wizard
        // writes it; the bridge compares slugs on both sides (contract C8).
        let allowlist = ["front-door"];
        assert!(allowlist
            .iter()
            .any(|entry| camera_slug(entry) == camera_slug(&parsed.camera)));
        assert!(!["garage"]
            .iter()
            .any(|entry| camera_slug(entry) == camera_slug(&parsed.camera)));

        let allowed: Vec<String> = DEFAULT_LABELS.iter().map(|l| l.to_string()).collect();
        let event_type = select_event_type(&parsed.labels, &allowed).expect("person is allowed");
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
        let payload = serde_json::to_string(&event).expect("event json");
        assert!(!payload.contains("Someone Known"), "sub_label name leaked");
        assert!(!payload.contains("abc123"), "object id leaked");

        // The retention pass the bridge runs on its timer. A fresh event is
        // inside retention, so nothing is pruned and no checkpoint is
        // written; `log_verify` then walks from genesis.
        kernel
            .enforce_retention_with_checkpoint(cfg.retention)
            .expect("retention pass");
    } // kernel dropped → connection closed, db flushed to disk

    // ── Stage 2: the real `log_verify` binary verifies the encrypted log ──
    assert_log_verify_passes(&db_path_str);

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

/// A minimal MQTT v5 broker for one client at a time: enough of the protocol
/// for rumqttc to connect, subscribe, publish and disconnect, recording what
/// the client did and injecting canned Frigate messages after its first
/// SUBACK. Everything the client publishes is kept verbatim, so a test can
/// grep the bridge's whole MQTT output.
mod stub_broker {
    use std::io::{ErrorKind, Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    #[derive(Debug, Clone)]
    pub struct Published {
        pub topic: String,
        pub qos: u8,
        pub retain: bool,
        pub payload: Vec<u8>,
    }

    #[derive(Debug, Default)]
    pub struct Log {
        pub connects: usize,
        pub subscriptions: Vec<String>,
        pub published: Vec<Published>,
        pub disconnects: usize,
    }

    pub struct Broker {
        pub port: u16,
        pub log: Arc<Mutex<Log>>,
        stop: Arc<AtomicBool>,
        thread: Option<std::thread::JoinHandle<()>>,
    }

    impl Broker {
        /// Bind an ephemeral loopback port and serve on a background thread.
        /// `inject` is sent to the client, in order, right after its first
        /// SUBACK, as QoS 0 PUBLISH packets.
        pub fn start(inject: Vec<(String, Vec<u8>)>) -> Broker {
            let listener = TcpListener::bind("127.0.0.1:0").expect("bind stub broker");
            let port = listener.local_addr().expect("local addr").port();
            listener.set_nonblocking(true).expect("nonblocking accept");
            let log = Arc::new(Mutex::new(Log::default()));
            let stop = Arc::new(AtomicBool::new(false));
            let thread = {
                let log = Arc::clone(&log);
                let stop = Arc::clone(&stop);
                std::thread::spawn(move || serve(listener, log, stop, inject))
            };
            Broker {
                port,
                log,
                stop,
                thread: Some(thread),
            }
        }

        pub fn snapshot(&self) -> Log {
            let log = self.log.lock().expect("broker log");
            Log {
                connects: log.connects,
                subscriptions: log.subscriptions.clone(),
                published: log.published.clone(),
                disconnects: log.disconnects,
            }
        }

        pub fn stop(mut self) -> Log {
            self.stop.store(true, Ordering::SeqCst);
            if let Some(thread) = self.thread.take() {
                thread.join().expect("stub broker thread");
            }
            self.snapshot()
        }
    }

    fn serve(
        listener: TcpListener,
        log: Arc<Mutex<Log>>,
        stop: Arc<AtomicBool>,
        inject: Vec<(String, Vec<u8>)>,
    ) {
        let mut injected = false;
        while !stop.load(Ordering::SeqCst) {
            match listener.accept() {
                Ok((stream, _)) => {
                    stream.set_nonblocking(false).expect("blocking stream");
                    stream
                        .set_read_timeout(Some(Duration::from_millis(200)))
                        .expect("read timeout");
                    handle(stream, &log, &stop, &inject, &mut injected);
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(20));
                }
                Err(e) => panic!("stub broker accept: {e}"),
            }
        }
    }

    fn encode_varint(mut value: usize) -> Vec<u8> {
        let mut out = Vec::new();
        loop {
            let mut byte = (value % 128) as u8;
            value /= 128;
            if value > 0 {
                byte |= 0x80;
            }
            out.push(byte);
            if value == 0 {
                return out;
            }
        }
    }

    fn decode_varint(bytes: &[u8]) -> (usize, usize) {
        let mut value = 0usize;
        let mut multiplier = 1usize;
        for (i, byte) in bytes.iter().enumerate().take(4) {
            value += usize::from(byte & 0x7f) * multiplier;
            if byte & 0x80 == 0 {
                return (value, i + 1);
            }
            multiplier *= 128;
        }
        (value, 4)
    }

    /// Read one byte, retrying on the read timeout until `stop`. `None` on
    /// EOF, stop, or a socket error.
    fn read_byte(stream: &mut TcpStream, stop: &AtomicBool) -> Option<u8> {
        let mut byte = [0u8; 1];
        loop {
            if stop.load(Ordering::SeqCst) {
                return None;
            }
            match stream.read(&mut byte) {
                Ok(0) => return None,
                Ok(_) => return Some(byte[0]),
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                Err(_) => return None,
            }
        }
    }

    /// Read one MQTT control packet: `(first byte, body)`.
    fn read_packet(stream: &mut TcpStream, stop: &AtomicBool) -> Option<(u8, Vec<u8>)> {
        let first = read_byte(stream, stop)?;
        let mut remaining = 0usize;
        let mut multiplier = 1usize;
        loop {
            let byte = read_byte(stream, stop)?;
            remaining += usize::from(byte & 0x7f) * multiplier;
            if byte & 0x80 == 0 {
                break;
            }
            multiplier *= 128;
        }
        let mut body = vec![0u8; remaining];
        let mut filled = 0;
        while filled < remaining {
            if stop.load(Ordering::SeqCst) {
                return None;
            }
            match stream.read(&mut body[filled..]) {
                Ok(0) => return None,
                Ok(n) => filled += n,
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                Err(_) => return None,
            }
        }
        Some((first, body))
    }

    fn write_packet(stream: &mut TcpStream, first: u8, body: &[u8]) {
        let mut packet = vec![first];
        packet.extend(encode_varint(body.len()));
        packet.extend_from_slice(body);
        stream.write_all(&packet).expect("stub broker write");
    }

    fn utf8_string(bytes: &[u8]) -> (String, usize) {
        let len = usize::from(u16::from_be_bytes([bytes[0], bytes[1]]));
        let text = String::from_utf8_lossy(&bytes[2..2 + len]).into_owned();
        (text, 2 + len)
    }

    fn handle(
        mut stream: TcpStream,
        log: &Mutex<Log>,
        stop: &AtomicBool,
        inject: &[(String, Vec<u8>)],
        injected: &mut bool,
    ) {
        while let Some((first, body)) = read_packet(&mut stream, stop) {
            match first >> 4 {
                // CONNECT → CONNACK (session present 0, success, no properties)
                1 => {
                    log.lock().expect("log").connects += 1;
                    write_packet(&mut stream, 0x20, &[0x00, 0x00, 0x00]);
                }
                // SUBSCRIBE → SUBACK granting QoS 1 per filter, then the injection
                8 => {
                    let pkid = [body[0], body[1]];
                    let (props_len, props_len_bytes) = decode_varint(&body[2..]);
                    let mut i = 2 + props_len_bytes + props_len;
                    let mut filters = Vec::new();
                    while i + 2 <= body.len() {
                        let (filter, used) = utf8_string(&body[i..]);
                        i += used + 1; // + subscription options byte
                        filters.push(filter);
                    }
                    let mut suback = vec![pkid[0], pkid[1], 0x00];
                    suback.extend(filters.iter().map(|_| 0x01u8));
                    log.lock()
                        .expect("log")
                        .subscriptions
                        .extend(filters.iter().cloned());
                    write_packet(&mut stream, 0x90, &suback);
                    if !*injected {
                        for (topic, payload) in inject {
                            let mut publish = Vec::new();
                            publish.extend((topic.len() as u16).to_be_bytes());
                            publish.extend_from_slice(topic.as_bytes());
                            publish.push(0x00); // no properties
                            publish.extend_from_slice(payload);
                            write_packet(&mut stream, 0x30, &publish);
                        }
                        *injected = true;
                    }
                }
                // PUBLISH from the client → record, PUBACK when QoS 1
                3 => {
                    let flags = first & 0x0f;
                    let qos = (flags >> 1) & 0x03;
                    let retain = flags & 0x01 == 1;
                    let (topic, mut i) = utf8_string(&body);
                    let mut pkid = [0u8; 2];
                    if qos > 0 {
                        pkid = [body[i], body[i + 1]];
                        i += 2;
                    }
                    let (props_len, props_len_bytes) = decode_varint(&body[i..]);
                    i += props_len_bytes + props_len;
                    let payload = body[i..].to_vec();
                    log.lock().expect("log").published.push(Published {
                        topic,
                        qos,
                        retain,
                        payload,
                    });
                    if qos == 1 {
                        write_packet(&mut stream, 0x40, &pkid);
                    }
                }
                // PINGREQ → PINGRESP
                12 => write_packet(&mut stream, 0xd0, &[]),
                // DISCONNECT
                14 => {
                    log.lock().expect("log").disconnects += 1;
                    return;
                }
                _ => {}
            }
        }
    }
}

/// The id every producer derives for a seed: `pwk_` + sha256("securacv:ha-device-id:v1:" || seed)[..4] hex.
fn expected_device_id(seed: &str) -> String {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(format!("securacv:ha-device-id:v1:{seed}").as_bytes());
    format!("pwk_{}", hex::encode(&digest[..4]))
}

fn frigate_fixture(name: &str) -> Vec<u8> {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/frigate")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()))
}

/// Every string and number under `value`, plus every object key (an
/// `attributes` dict keys on the recognized label), as text.
fn json_leaves(value: &serde_json::Value, out: &mut Vec<String>) {
    match value {
        serde_json::Value::String(s) => out.push(s.clone()),
        serde_json::Value::Number(n) => out.push(n.to_string()),
        serde_json::Value::Array(items) => items.iter().for_each(|v| json_leaves(v, out)),
        serde_json::Value::Object(map) => {
            for (key, v) in map {
                out.push(key.clone());
                json_leaves(v, out);
            }
        }
        serde_json::Value::Bool(_) | serde_json::Value::Null => {}
    }
}

/// The discarded values a fixture carries, recognizably: every `SENTINEL_`
/// string or key and every number at or above 7770 (the convention
/// tests/frigate_integration.rs pins — scores above 1, ids and epochs from
/// 1970, never a bucket start, a confidence or a count). Numbers are kept
/// as their JSON text, so a leak is matched as a whole value.
fn fixture_sentinels(fixture: &[u8]) -> std::collections::BTreeSet<String> {
    let value: serde_json::Value = serde_json::from_slice(fixture).expect("fixture is JSON");
    let mut leaves = Vec::new();
    json_leaves(&value, &mut leaves);
    leaves
        .into_iter()
        .filter(|leaf| {
            leaf.starts_with("SENTINEL_") || leaf.parse::<f64>().is_ok_and(|n| n >= 7770.0)
        })
        .collect()
}

/// The first value in `json` (a bridge payload or a sealed record) that is
/// a discarded fixture value: a string or key carrying `SENTINEL_`, or a
/// number whose JSON text is one of the fixture's numeric sentinels. Whole
/// values only — a digit-substring test once failed this gate because a
/// wall-clock bucket start (1791777000) happened to contain "777".
fn leaked_sentinel(json: &str, sentinels: &std::collections::BTreeSet<String>) -> Option<String> {
    let value: serde_json::Value =
        serde_json::from_str(json).unwrap_or_else(|e| panic!("output is not JSON ({e}): {json}"));
    let mut leaves = Vec::new();
    json_leaves(&value, &mut leaves);
    leaves
        .into_iter()
        .find(|leaf| leaf.contains("SENTINEL_") || sentinels.contains(leaf))
}

#[test]
fn sentinel_sweep_matches_whole_values_not_digit_substrings() {
    // Negative control for the sweep below: a discarded fixture value that
    // reaches an output is flagged whichever shape it takes, and a legitimate
    // value that merely shares digits with one is not.
    let sentinels = fixture_sentinels(&frigate_fixture("events_0.17_new.json"));
    assert!(
        sentinels.len() > 60,
        "the 0.17 fixture carries {} distinct sentinel values, too few to be the widest shape",
        sentinels.len()
    );
    assert!(sentinels.contains("7770620") && sentinels.contains("7770.86"));
    assert!(
        sentinels.contains("SENTINEL_ID_017") && sentinels.contains("SENTINEL_UNKNOWN_KEY_017")
    );

    let leaked_number = r#"{"record_type":"event","zone_id":"zone:driveway","area":7770620}"#;
    assert_eq!(
        leaked_sentinel(leaked_number, &sentinels).as_deref(),
        Some("7770620")
    );
    let leaked_score = r#"{"confidence":7770.86}"#;
    assert_eq!(
        leaked_sentinel(leaked_score, &sentinels).as_deref(),
        Some("7770.86")
    );
    let leaked_string = r#"{"v":1,"nested":{"deep":["SENTINEL_PLATE_017"]}}"#;
    assert_eq!(
        leaked_sentinel(leaked_string, &sentinels).as_deref(),
        Some("SENTINEL_PLATE_017")
    );
    let leaked_key = r#"{"attributes":{"SENTINEL_ATTRIBUTE_FACE_017":0.5}}"#;
    assert_eq!(
        leaked_sentinel(leaked_key, &sentinels).as_deref(),
        Some("SENTINEL_ATTRIBUTE_FACE_017")
    );
    let leaked_in_text = r#"{"note":"plate SENTINEL_PLATE_017 seen"}"#;
    assert!(leaked_sentinel(leaked_in_text, &sentinels).is_some());

    // The CI run that motivated this: buckets 1791777000 and 1791777600, and
    // an April 2026 bucket (1777062000) that contains the `area` sentinel
    // 7770620 as a digit substring. Neither is a discarded value.
    let legitimate = r#"{"v":1,"device_id":"pwk_0123abcd","connected":true,"topic_prefix":"frigate","last_message_bucket_start":1791777000,"last_message_bucket_size":600,"messages_since_start":2,"mode":"events","other":1791777600,"april":1777062000,"confidence":0.87}"#;
    assert_eq!(leaked_sentinel(legitimate, &sentinels), None);
}

fn wait_until(timeout: Duration, mut condition: impl FnMut() -> bool) -> bool {
    let deadline = std::time::Instant::now() + timeout;
    while std::time::Instant::now() < deadline {
        if condition() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    condition()
}

#[cfg(unix)]
#[test]
fn frigate_bridge_run_seals_heartbeats_prunes_and_publishes_frigate_link() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path_str = db_path.to_str().expect("utf8 path").to_string();
    let hwm_path = dir.path().join("witness.hwm");
    let stderr_path = dir.path().join("frigate_bridge.stderr");
    let cfg = kernel_config(&db_path_str, Duration::from_secs(3600));

    // ── A row the bridge's boot retention pass must prune: sealed before the
    // bridge starts, then aged past the one-hour retention. Retention ages
    // rows by their sealed `created_at` (the coarse bucket start, a column
    // outside the hash), which is how the store's own tests age rows too. ──
    {
        let mut kernel = Kernel::open(&cfg).expect("open kernel");
        let candidate = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectSmall,
            time_bucket: TimeBucket::now(600).expect("bucket"),
            zone_id: "zone:expired_row".to_string(),
            confidence: 0.6,
            correlation_token: None,
            attestation: None,
        };
        kernel
            .append_event_checked(
                &module_desc(),
                candidate,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
            .expect("seed expired row");
        kernel
            .conn
            .execute(
                "UPDATE sealed_events SET created_at = created_at - 7200",
                [],
            )
            .expect("age the seeded row");
    }

    // ── The stub broker injects a sentinel-laden Frigate 0.17 tracked object
    // (sealed) and a face-labeled variant of it (refused, but still "Frigate
    // heard") after the bridge subscribes. ──
    let event_017 = frigate_fixture("events_0.17_new.json");
    let face = String::from_utf8(event_017.clone())
        .expect("utf8 fixture")
        .replacen("\"label\": \"person\"", "\"label\": \"face\"", 2)
        .into_bytes();
    let broker = stub_broker::Broker::start(vec![
        ("frigate/events".to_string(), event_017.clone()),
        ("frigate/events".to_string(), face),
    ]);

    let stderr_file = std::fs::File::create(&stderr_path).expect("stderr file");
    let mut child = Command::new(env!("CARGO_BIN_EXE_frigate_bridge"))
        .args([
            "--mqtt-broker-addr",
            &format!("127.0.0.1:{}", broker.port),
            "--db-path",
            &db_path_str,
            "--retention-secs",
            "3600",
            "--cameras",
            "Front Door", // the wizard's spelling of the fixture's `front_door`
            "--mqtt-topic-prefix",
            "witness",
        ])
        .env("DEVICE_KEY_SEED", SEED)
        .env("SECURACV_HWM_PATH", &hwm_path)
        .env("RUST_LOG", "info,frigate_bridge=debug")
        .stdout(std::process::Stdio::null())
        .stderr(stderr_file)
        .spawn()
        .expect("spawn frigate_bridge");

    let frigate_link_payloads = |log: &stub_broker::Log| -> Vec<serde_json::Value> {
        log.published
            .iter()
            .filter(|p| p.topic == "witness/frigate_link")
            .map(|p| serde_json::from_slice(&p.payload).expect("frigate_link json"))
            .collect()
    };

    // The bridge publishes frigate_link on ConnAck (no message yet) and again
    // once Frigate has been heard (the last-message bucket changed). The
    // second message lands in the same bucket, so it is NOT a republish — the
    // counter rides along on the next one (here, the shutdown payload); the
    // bridge's own log line for the refused label says it was processed.
    let bridge_log = || std::fs::read_to_string(&stderr_path).unwrap_or_default();
    let heard = wait_until(Duration::from_secs(30), || {
        frigate_link_payloads(&broker.snapshot())
            .iter()
            .any(|p| !p["last_message_bucket_start"].is_null())
            && bridge_log().contains("identity label refused")
    });
    assert!(
        heard,
        "bridge never reported Frigate heard on witness/frigate_link and refused the face; broker saw {:?}\n--- frigate_bridge stderr ---\n{}",
        broker.snapshot(),
        bridge_log()
    );

    // ── Clean shutdown: SIGTERM, the way s6 / docker stop deliver it. ──
    // SAFETY: `child.id()` is the live pid of a process this test spawned and
    // still owns; SIGTERM asks it to shut down cleanly.
    let killed = unsafe { libc::kill(child.id() as libc::pid_t, libc::SIGTERM) };
    assert_eq!(killed, 0, "SIGTERM delivered");
    let exited = wait_until(Duration::from_secs(20), || {
        matches!(child.try_wait(), Ok(Some(_)))
    });
    if !exited {
        let _ = child.kill();
    }
    let status = child.wait().expect("wait frigate_bridge");
    assert!(
        exited && status.success(),
        "frigate_bridge must exit cleanly on SIGTERM: {status:?}\n--- frigate_bridge stderr ---\n{}",
        bridge_log()
    );
    let log = broker.stop();

    // ── MQTT side: exactly one subscription, retained link health, no sentinel. ──
    let describe = |log: &stub_broker::Log| -> String {
        let published: Vec<String> = log
            .published
            .iter()
            .map(|p| {
                format!(
                    "{} (qos {}, retain {}): {}",
                    p.topic,
                    p.qos,
                    p.retain,
                    String::from_utf8_lossy(&p.payload)
                )
            })
            .collect();
        format!(
            "connects {}, subscriptions {:?}, disconnects {}, published {:#?}\n--- frigate_bridge stderr ---\n{}",
            log.connects,
            log.subscriptions,
            log.disconnects,
            published,
            bridge_log()
        )
    };
    assert_eq!(log.connects, 1, "one connection: {}", describe(&log));
    assert_eq!(
        log.subscriptions,
        vec!["frigate/events".to_string()],
        "the bridge subscribes to exactly <prefix>/events"
    );
    assert_eq!(
        log.disconnects,
        1,
        "a DISCONNECT closed the session cleanly: {}",
        describe(&log)
    );
    let topics: std::collections::BTreeSet<&str> =
        log.published.iter().map(|p| p.topic.as_str()).collect();
    assert_eq!(
        topics.into_iter().collect::<Vec<_>>(),
        vec!["witness/frigate_link"],
        "frigate_link is the bridge's only MQTT output"
    );
    let sentinels = fixture_sentinels(&event_017);
    for published in &log.published {
        assert!(published.retain, "frigate_link is retained");
        assert_eq!(published.qos, 1, "frigate_link is QoS 1");
        let text = String::from_utf8_lossy(&published.payload);
        if let Some(leak) = leaked_sentinel(&text, &sentinels) {
            panic!("a discarded Frigate value ({leak}) reached MQTT: {text}");
        }
    }
    let links = frigate_link_payloads(&log);
    assert!(links.len() >= 3, "connect, heard, shutdown: {links:?}");
    let device_id = expected_device_id(SEED);
    for link in &links {
        assert_eq!(link["v"], 1);
        assert_eq!(link["device_id"], device_id);
        assert_eq!(link["topic_prefix"], "frigate");
        assert_eq!(link["mode"], "events");
        assert_eq!(link["last_message_bucket_size"], 600);
    }
    let first = &links[0];
    assert_eq!(first["connected"], true);
    assert_eq!(first["last_message_bucket_start"], serde_json::Value::Null);
    assert_eq!(first["messages_since_start"], 0);
    let heard = links
        .iter()
        .find(|p| !p["last_message_bucket_start"].is_null())
        .expect("a payload once Frigate was heard");
    assert_eq!(heard["connected"], true);
    assert!(heard["messages_since_start"]
        .as_u64()
        .is_some_and(|n| n >= 1));
    let bucket_start = heard["last_message_bucket_start"]
        .as_u64()
        .expect("a bucket start once Frigate was heard");
    assert_eq!(bucket_start % 600, 0, "coarse: a 10-minute bucket start");
    // Two messages in one bucket are one change: no publish per message.
    assert_eq!(
        links.iter().filter(|p| p["connected"] == true).count(),
        2,
        "connect + heard, nothing per message: {links:?}"
    );
    let last = links.last().expect("final payload");
    assert_eq!(
        last["connected"], false,
        "clean shutdown says the link is down on purpose"
    );
    assert_eq!(last["messages_since_start"], 2);

    // ── Sealed log: lifecycle + heartbeat records, the expired row pruned
    // and checkpointed, the one sealed claim, and no sentinel anywhere. ──
    let kernel = Kernel::open(&cfg).expect("reopen kernel");
    let payloads: Vec<String> = kernel
        .sealed_log_document()
        .expect("sealed log document")
        .entries
        .into_iter()
        .map(|entry| entry.payload)
        .collect();
    let count = |record_type: &str| {
        let tag = format!("{{\"record_type\":\"{record_type}\"");
        payloads.iter().filter(|p| p.starts_with(&tag)).count()
    };
    assert_eq!(
        count("lifecycle"),
        2,
        "start + shutdown_clean: {payloads:#?}"
    );
    assert!(
        payloads.iter().any(|p| p.contains("\"phase\":\"start\""))
            && payloads
                .iter()
                .any(|p| p.contains("\"phase\":\"shutdown_clean\"")),
        "{payloads:#?}"
    );
    assert!(
        count("heartbeat") >= 2,
        "a heartbeat at start and a final one at shutdown: {payloads:#?}"
    );
    assert_eq!(
        count("event"),
        1,
        "the person was sealed once; the face refused; the expired row pruned: {payloads:#?}"
    );
    assert_eq!(count("failure"), 0, "{payloads:#?}");
    assert!(
        !payloads.iter().any(|p| p.contains("expired_row")),
        "the boot retention pass must prune the expired row: {payloads:#?}"
    );
    let sealed = payloads
        .iter()
        .find(|p| p.starts_with("{\"record_type\":\"event\""))
        .expect("the sealed claim");
    assert!(sealed.contains("\"zone_id\":\"zone:driveway\""), "{sealed}");
    assert!(sealed.contains("BoundaryCrossingObjectLarge"), "{sealed}");
    assert!(sealed.contains("\"attestation\":\"adapter\""), "{sealed}");
    for payload in &payloads {
        if let Some(leak) = leaked_sentinel(payload, &sentinels) {
            panic!("a discarded Frigate value ({leak}) reached the sealed log: {payload}");
        }
    }
    let checkpoints: i64 = kernel
        .conn
        .query_row("SELECT COUNT(*) FROM checkpoints", [], |r| r.get(0))
        .expect("count checkpoints");
    assert_eq!(checkpoints, 1, "the prune checkpointed its cutoff");

    // ── The signed high-water mark followed every append. ──
    let newest_id: i64 = kernel
        .conn
        .query_row("SELECT MAX(id) FROM sealed_events", [], |r| r.get(0))
        .expect("max id");
    let mark = witness_kernel::log::high_water_mark::load(&hwm_path)
        .expect("decode high-water mark")
        .expect("the bridge wrote the high-water mark");
    assert_eq!(
        mark.seq,
        u64::try_from(newest_id).expect("id"),
        "the mark names the newest sealed row"
    );
    drop(kernel);

    // ── And the whole thing verifies with the real verifier. ──
    assert_log_verify_passes(&db_path_str);
}
