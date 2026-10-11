//! Integration tests for Frigate MQTT → PWK pipeline.
//!
//! These tests verify that:
//! 1. Frigate event JSON is correctly parsed, for every payload shape Frigate
//!    0.14 through 0.17 publishes (`tests/fixtures/frigate/`)
//! 2. Every identity-bearing field is discarded at parse time (spec D7):
//!    each fixture populates every discarded field with a recognizable
//!    sentinel, and the sealed payload, the export and the parsed claim are
//!    asserted to contain none of them
//! 3. `face` / `license_plate` labels are refused outright
//! 4. Reviews mode seals an alert exactly once, at the transition
//! 5. Field mapping complies with spec/event_contract.md
//! 6. Time coarsening is applied correctly
//! 7. Malformed JSON is rejected gracefully
//! 8. The camera slug matches the add-on wizard's rule (contract C8) and the
//!    subscription list is exactly `<prefix>/events` or `<prefix>/reviews`
//!
//! Fixture hygiene: a discarded value must be a string starting with
//! `SENTINEL_` or a number at or above 7770, so a containment check can
//! never pass by accident (a `1` would match any digit) — the fixture test
//! asserts that shape before it asserts absence.

use std::collections::BTreeSet;
use std::path::Path;
use std::time::Duration;

use serde_json::Value;
use witness_kernel::transport::frigate::{
    camera_slug, frigate_subscriptions, is_identity_label, map_label_to_event_type,
    parse_frigate_event, parse_review_event, sanitize_zone_name, select_event_type,
    FrigateClaimKind, FrigateMode, ParsedFrigateEvent, DEFAULT_LABELS, DISCARDED_EVENT_FIELDS,
    DISCARDED_REVIEW_FIELDS, KEPT_EVENT_FIELDS, KEPT_REVIEW_FIELDS,
};
use witness_kernel::{
    Attestation, CandidateEvent, Event, EventType, ExportOptions, InferenceBackend, Kernel,
    KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy, MIN_BUCKET_SIZE_S,
};

/// Sample Frigate event JSON for testing - "new" event type
const FRIGATE_EVENT_NEW: &str = r#"{
    "before": null,
    "after": {
        "id": "1234567890.abc123",
        "camera": "front_door",
        "label": "person",
        "sub_label": null,
        "score": 0.75,
        "top_score": 0.92,
        "current_zones": ["porch"],
        "entered_zones": ["driveway", "porch"],
        "false_positive": false,
        "has_clip": true,
        "has_snapshot": true
    },
    "type": "new"
}"#;

/// Frigate event for car detection
const FRIGATE_EVENT_CAR: &str = r#"{
    "before": null,
    "after": {
        "id": "9876543210.xyz789",
        "camera": "driveway_cam",
        "label": "car",
        "sub_label": null,
        "score": 0.88,
        "top_score": 0.95,
        "current_zones": [],
        "entered_zones": ["street", "driveway"],
        "false_positive": false,
        "has_clip": false,
        "has_snapshot": true
    },
    "type": "new"
}"#;

/// Frigate event marked as false positive (should be skipped)
const FRIGATE_EVENT_FALSE_POSITIVE: &str = r#"{
    "before": null,
    "after": {
        "id": "fp12345.test",
        "camera": "backyard",
        "label": "dog",
        "score": 0.60,
        "false_positive": true
    },
    "type": "new"
}"#;

/// Frigate "update" event type (should be skipped to prevent duplicates)
const FRIGATE_EVENT_UPDATE: &str = r#"{
    "before": {"id": "1234567890.abc123"},
    "after": {
        "id": "1234567890.abc123",
        "camera": "front_door",
        "label": "person",
        "score": 0.78,
        "current_zones": ["porch"],
        "false_positive": false
    },
    "type": "update"
}"#;

/// Frigate "end" event type (should be skipped)
const FRIGATE_EVENT_END: &str = r#"{
    "before": {"id": "1234567890.abc123"},
    "after": {
        "id": "1234567890.abc123",
        "camera": "front_door",
        "label": "person"
    },
    "type": "end"
}"#;

/// Malformed JSON (missing required fields)
const FRIGATE_EVENT_MALFORMED_NO_AFTER: &str = r#"{
    "before": null,
    "type": "new"
}"#;

/// Invalid JSON syntax
const FRIGATE_EVENT_INVALID_JSON: &str = r#"{
    "before": null,
    "after": {not valid json}
}"#;

/// The flat shape an early version of this bridge accepted on
/// `frigate/reviews`. Frigate never published it (the topic has carried
/// before/after items since it appeared in 0.14), so it is no longer sealed.
const FRIGATE_REVIEW_FLAT_LEGACY: &str = r#"{
    "type": "new",
    "id": "review123",
    "camera": "garage",
    "data": {
        "objects": ["motorcycle"],
        "score": 0.85,
        "zones": ["garage_zone"]
    }
}"#;

/// Verbatim-shaped Frigate 0.14–0.17 `frigate/reviews` payload: a change
/// feed with before/after items carrying camera, severity, and a data
/// block (detections/objects/sub_labels/zones/audio — and NO score).
const FRIGATE_REVIEW_017_NEW: &str = r#"{
    "type": "new",
    "before": {
        "id": "1719000000.612943-bbrpj1",
        "camera": "front_door",
        "start_time": 1719000000.612943,
        "end_time": null,
        "severity": "detection",
        "thumb_path": "/media/frigate/clips/review/thumb-front_door-1719000000.612943-bbrpj1.webp",
        "data": {
            "detections": ["1719000000.580339-emqk1y"],
            "objects": ["person"],
            "sub_labels": [],
            "zones": [],
            "audio": []
        }
    },
    "after": {
        "id": "1719000000.612943-bbrpj1",
        "camera": "front_door",
        "start_time": 1719000000.612943,
        "end_time": null,
        "severity": "alert",
        "thumb_path": "/media/frigate/clips/review/thumb-front_door-1719000000.612943-bbrpj1.webp",
        "data": {
            "detections": ["1719000000.580339-emqk1y"],
            "objects": ["person"],
            "sub_labels": [],
            "zones": ["porch"],
            "audio": []
        }
    }
}"#;

/// Verbatim-shaped Frigate 0.16/0.17 `frigate/events` "new" payload,
/// including the 0.16+ attribute/recognition fields the bridge must ignore.
const FRIGATE_EVENT_017_NEW: &str = r#"{
    "before": null,
    "after": {
        "id": "1719000000.580339-emqk1y",
        "camera": "front_door",
        "frame_time": 1719000000.580339,
        "snapshot": {
            "frame_time": 1719000000.580339,
            "box": [928, 326, 1124, 712],
            "area": 75576,
            "region": [741, 109, 1445, 813],
            "score": 0.84,
            "attributes": []
        },
        "label": "person",
        "sub_label": null,
        "top_score": 0.87,
        "false_positive": false,
        "start_time": 1719000000.580339,
        "end_time": null,
        "score": 0.84,
        "box": [928, 326, 1124, 712],
        "area": 75576,
        "ratio": 0.51,
        "region": [741, 109, 1445, 813],
        "stationary": false,
        "motionless_count": 0,
        "position_changes": 1,
        "current_zones": ["porch"],
        "entered_zones": ["driveway", "porch"],
        "has_clip": true,
        "has_snapshot": true,
        "attributes": {},
        "current_attributes": [],
        "recognized_license_plate": null,
        "recognized_license_plate_score": null
    },
    "type": "new"
}"#;

/// One fixture per Frigate `frigate/events` shape, every discarded field
/// populated with a sentinel (see the module doc).
const EVENT_FIXTURES: &[&str] = &[
    "events_0.14_new.json",
    "events_0.15_new.json",
    "events_0.16_new.json",
    "events_0.17_new.json",
];

/// One fixture per Frigate `frigate/reviews` shape that seals: a `new`
/// alert (0.14), an `update` promoting a detection to an alert (0.16), and
/// a 0.17 `new` alert carrying the GenAI review fields.
const REVIEW_FIXTURES: &[&str] = &[
    "reviews_0.14_new_alert.json",
    "reviews_0.16_update_to_alert.json",
    "reviews_0.17_new_alert_genai.json",
];

fn fixture(name: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/frigate")
        .join(name);
    std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()))
}

fn fixture_json(name: &str) -> Value {
    serde_json::from_str(&fixture(name)).unwrap_or_else(|e| panic!("{name} is not JSON: {e}"))
}

/// A discarded value is recognizable: a `SENTINEL_` string or a number at
/// or above 7770 (the bucket starts, confidences and byte arrays in a
/// sealed payload never spell one of those).
fn is_sentinel_shaped(leaf: &str) -> bool {
    leaf.starts_with("SENTINEL_") || leaf.parse::<f64>().is_ok_and(|n| n >= 7770.0)
}

/// Every string and number under `value`, plus every object key that is
/// itself a sentinel (an `attributes` dict keys on the recognized label).
fn collect_leaves(value: &Value, out: &mut Vec<String>) {
    match value {
        Value::String(s) => out.push(s.clone()),
        Value::Number(n) => out.push(n.to_string()),
        Value::Array(items) => items.iter().for_each(|v| collect_leaves(v, out)),
        Value::Object(map) => {
            for (key, v) in map {
                if key.starts_with("SENTINEL_") {
                    out.push(key.clone());
                }
                collect_leaves(v, out);
            }
        }
        Value::Bool(_) | Value::Null => {}
    }
}

/// The discard surface of an events fixture: every value outside the kept
/// fields of `after` (and of `before`, which mirrors its shape and is never
/// read at all).
fn event_sentinels(fixture: &Value) -> Vec<String> {
    let mut out = Vec::new();
    for (key, value) in fixture.as_object().expect("fixture object") {
        match (key.as_str(), value) {
            ("type", _) => {}
            ("after" | "before", Value::Object(item)) => {
                for (field, v) in item {
                    if KEPT_EVENT_FIELDS.contains(&field.as_str()) {
                        continue;
                    }
                    if field.starts_with("SENTINEL_") {
                        out.push(field.clone());
                    }
                    collect_leaves(v, &mut out);
                }
            }
            (_, v) => collect_leaves(v, &mut out),
        }
    }
    out
}

/// The discard surface of a review fixture: every value outside the kept
/// fields of the `after` and `before` items (`before.severity` is read for
/// the transition, so it is a kept field there too).
fn review_sentinels(fixture: &Value) -> Vec<String> {
    let mut out = Vec::new();
    for (key, value) in fixture.as_object().expect("fixture object") {
        match (key.as_str(), value) {
            ("type", _) => {}
            ("after" | "before", Value::Object(item)) => {
                for (field, v) in item {
                    match (field.as_str(), v) {
                        ("camera" | "severity", _) => {}
                        ("data", Value::Object(data)) => {
                            for (data_field, dv) in data {
                                let dotted = format!("data.{data_field}");
                                if KEPT_REVIEW_FIELDS.contains(&dotted.as_str()) {
                                    continue;
                                }
                                collect_leaves(dv, &mut out);
                            }
                        }
                        (_, v) => {
                            if field.starts_with("SENTINEL_") {
                                out.push(field.clone());
                            }
                            collect_leaves(v, &mut out);
                        }
                    }
                }
            }
            (_, v) => collect_leaves(v, &mut out),
        }
    }
    out
}

fn setup_test_kernel() -> (Kernel, KernelConfig) {
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:frigate_test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:frigate_test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:frigate_test:a1b2c3d4e5f6".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    (kernel, cfg)
}

fn make_module_descriptor() -> ModuleDescriptor {
    ModuleDescriptor {
        id: "frigate_bridge_test",
        allowed_event_types: &[
            EventType::BoundaryCrossingObjectSmall,
            EventType::BoundaryCrossingObjectLarge,
        ],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    }
}

fn default_labels() -> Vec<String> {
    DEFAULT_LABELS.iter().map(|l| l.to_string()).collect()
}

fn zone_id_for(parsed: &ParsedFrigateEvent) -> String {
    match parsed.zones.first() {
        Some(zone) => format!("zone:{}", sanitize_zone_name(zone)),
        None => format!("zone:{}", sanitize_zone_name(&parsed.camera)),
    }
}

/// Seal a parsed claim exactly as `frigate_bridge` does: the coarsest allowed
/// class, the first zone (else the camera), the current bucket, no token,
/// adapter attestation.
fn seal_parsed(kernel: &mut Kernel, cfg: &KernelConfig, parsed: &ParsedFrigateEvent) -> Event {
    let event_type =
        select_event_type(&parsed.labels, &default_labels()).expect("an allowed label");
    let candidate = CandidateEvent {
        event_type,
        time_bucket: TimeBucket::now(600).expect("time bucket"),
        zone_id: zone_id_for(parsed),
        confidence: parsed.confidence as f32,
        correlation_token: None, // Frigate IDs are stripped
        attestation: Some(Attestation::Adapter),
    };
    kernel
        .append_event_checked(
            &make_module_descriptor(),
            candidate,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("append event")
}

fn sealed_payloads(kernel: &Kernel) -> Vec<String> {
    kernel
        .sealed_log_document()
        .expect("sealed log document")
        .entries
        .into_iter()
        .map(|entry| entry.payload)
        .collect()
}

fn export_json(kernel: &mut Kernel, cfg: &KernelConfig) -> String {
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");
    serde_json::to_string(&artifact).expect("serialize")
}

/// Seal `parsed` and assert none of `sentinels` reaches the parsed claim,
/// the stored `payload_json`, or the export.
fn assert_sealed_without(
    name: &str,
    parsed: &ParsedFrigateEvent,
    sentinels: &[String],
) -> Vec<String> {
    assert!(!sentinels.is_empty(), "{name}: fixture carries no sentinel");
    for sentinel in sentinels {
        assert!(
            is_sentinel_shaped(sentinel),
            "{name}: discarded value {sentinel:?} is not sentinel-shaped; every discarded \
             value in a fixture must be recognizable (SENTINEL_… or a number >= 7770)"
        );
    }

    let (mut kernel, cfg) = setup_test_kernel();
    seal_parsed(&mut kernel, &cfg, parsed);
    let payloads = sealed_payloads(&kernel);
    assert_eq!(payloads.len(), 1, "{name}: one sealed record");
    let export = export_json(&mut kernel, &cfg);
    let parsed_text = format!("{parsed:?}");

    for sentinel in sentinels {
        assert!(
            !parsed_text.contains(sentinel.as_str()),
            "{name}: {sentinel:?} survived into the parsed claim: {parsed_text}"
        );
        assert!(
            !payloads[0].contains(sentinel.as_str()),
            "{name}: {sentinel:?} reached the sealed payload: {}",
            payloads[0]
        );
        assert!(
            !export.contains(sentinel.as_str()),
            "{name}: {sentinel:?} reached the export"
        );
    }

    // The sealed payload is the claim and nothing else.
    let payload: Value = serde_json::from_str(&payloads[0]).expect("payload json");
    let keys: BTreeSet<&str> = payload
        .as_object()
        .expect("payload object")
        .keys()
        .map(String::as_str)
        .collect();
    let expected: BTreeSet<&str> = [
        "record_type",
        "event_type",
        "time_bucket",
        "zone_id",
        "confidence",
        "correlation_token",
        "attestation",
        "kernel_version",
        "ruleset_id",
        "ruleset_hash",
    ]
    .into_iter()
    .collect();
    assert_eq!(keys, expected, "{name}: sealed payload keys");
    assert_eq!(payload["correlation_token"], Value::Null);
    payloads
}

// ==================== Time Bucket Tests ====================

#[test]
fn time_bucket_rejects_below_minimum_5_minutes() {
    // Per spec/event_contract.md §3: minimum bucket 5 minutes (300 seconds)
    let result = TimeBucket::now(299);
    assert!(result.is_err());
    let err = result.unwrap_err().to_string();
    assert!(
        err.contains("below minimum"),
        "Error should mention minimum: {}",
        err
    );
    assert!(
        err.contains("300"),
        "Error should mention 300 seconds: {}",
        err
    );
}

#[test]
fn time_bucket_accepts_minimum_5_minutes() {
    // 5 minutes = 300 seconds is the minimum allowed
    let result = TimeBucket::now(MIN_BUCKET_SIZE_S);
    assert!(result.is_ok(), "5-minute bucket should be allowed");
}

#[test]
fn time_bucket_accepts_10_minutes() {
    // 10 minutes = 600 seconds is the typical default
    let result = TimeBucket::now(600);
    assert!(result.is_ok(), "10-minute bucket should be allowed");
}

#[test]
fn time_bucket_accepts_15_minutes() {
    // 15 minutes = 900 seconds is within typical range
    let result = TimeBucket::now(900);
    assert!(result.is_ok(), "15-minute bucket should be allowed");
}

#[test]
fn time_bucket_coarsen_to_rejects_below_minimum() {
    let bucket = TimeBucket::now(600).expect("10-minute bucket");
    let result = bucket.coarsen_to(200);
    assert!(result.is_err());
    assert!(result.unwrap_err().to_string().contains("below minimum"));
}

#[test]
fn time_bucket_validate_bucket_size_works() {
    assert!(TimeBucket::validate_bucket_size(MIN_BUCKET_SIZE_S).is_ok());
    assert!(TimeBucket::validate_bucket_size(600).is_ok());
    assert!(TimeBucket::validate_bucket_size(299).is_err());
    assert!(TimeBucket::validate_bucket_size(0).is_err());
}

// ==================== Frigate Field Mapping Tests ====================

#[test]
fn frigate_event_parsing_new_event_succeeds() {
    let result = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes());
    assert!(result.is_ok());

    let event = result.unwrap();
    assert_eq!(event.camera, "front_door");
    assert_eq!(event.labels, vec!["person"]);
    assert_eq!(event.kind, FrigateClaimKind::TrackedObject);
    // Should use top_score (0.92) not score (0.75)
    assert!((event.confidence - 0.92).abs() < 0.001);
    // Should prefer entered_zones over current_zones
    assert_eq!(event.zones, vec!["driveway", "porch"]);
}

#[test]
fn frigate_event_parsing_uses_top_score_when_available() {
    let event = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes()).unwrap();
    // top_score is 0.92, score is 0.75 - should use top_score
    assert!((event.confidence - 0.92).abs() < 0.001);
}

#[test]
fn frigate_event_parsing_car_event() {
    let result = parse_frigate_event(FRIGATE_EVENT_CAR.as_bytes());
    assert!(result.is_ok());

    let event = result.unwrap();
    assert_eq!(event.camera, "driveway_cam");
    assert_eq!(event.labels, vec!["car"]);
    assert!((event.confidence - 0.95).abs() < 0.001);
    assert_eq!(event.zones, vec!["street", "driveway"]);
}

#[test]
fn frigate_event_false_positive_is_rejected() {
    let result = parse_frigate_event(FRIGATE_EVENT_FALSE_POSITIVE.as_bytes());
    assert!(result.is_err());
    assert!(result.unwrap_err().to_string().contains("false positive"));
}

#[test]
fn frigate_event_update_type_is_rejected() {
    let result = parse_frigate_event(FRIGATE_EVENT_UPDATE.as_bytes());
    assert!(result.is_err());
    assert!(result
        .unwrap_err()
        .to_string()
        .contains("already processed"));
}

#[test]
fn frigate_event_end_type_is_rejected() {
    let result = parse_frigate_event(FRIGATE_EVENT_END.as_bytes());
    assert!(result.is_err());
    assert!(result
        .unwrap_err()
        .to_string()
        .contains("already processed"));
}

#[test]
fn frigate_event_missing_after_section_is_rejected() {
    let result = parse_frigate_event(FRIGATE_EVENT_MALFORMED_NO_AFTER.as_bytes());
    assert!(result.is_err());
    assert!(result.unwrap_err().to_string().contains("missing"));
}

#[test]
fn frigate_event_invalid_json_is_rejected() {
    let result = parse_frigate_event(FRIGATE_EVENT_INVALID_JSON.as_bytes());
    assert!(result.is_err());
    assert!(result.unwrap_err().to_string().contains("parse error"));
}

#[test]
fn label_to_event_type_mapping_person_is_large() {
    for label in ["person", "Person", "PERSON"] {
        assert_eq!(
            map_label_to_event_type(label),
            Some(EventType::BoundaryCrossingObjectLarge)
        );
    }
}

#[test]
fn label_to_event_type_mapping_vehicles_are_large() {
    for label in ["car", "truck", "bus", "motorcycle"] {
        assert_eq!(
            map_label_to_event_type(label),
            Some(EventType::BoundaryCrossingObjectLarge),
            "{label}"
        );
    }
}

#[test]
fn label_to_event_type_mapping_animals_are_small() {
    for label in ["dog", "cat", "bird"] {
        assert_eq!(
            map_label_to_event_type(label),
            Some(EventType::BoundaryCrossingObjectSmall),
            "{label}"
        );
    }
}

#[test]
fn label_to_event_type_mapping_unknown_defaults_to_small() {
    assert_eq!(
        map_label_to_event_type("unknown_object"),
        Some(EventType::BoundaryCrossingObjectSmall)
    );
}

#[test]
fn label_to_event_type_refuses_identity_labels() {
    for label in ["face", "license_plate", "Face", "LICENSE_PLATE"] {
        assert!(is_identity_label(label), "{label}");
        assert_eq!(map_label_to_event_type(label), None, "{label}");
    }
    assert!(!is_identity_label("person"));
}

#[test]
fn sub_label_is_never_folded_into_the_label() {
    // Frigate publishes sub_label as null or a [name, score] pair on
    // frigate/events; an early version of this bridge accepted a bare string
    // and folded it into "label:sub". Every shape now leaves the base label
    // untouched — and never fails the message.
    for sub_label in [
        "null",
        r#""amazon""#,
        r#"["Someone Known", 0.79]"#,
        r#"[null, null]"#,
        r#"{"name": "Someone Known"}"#,
        "7770",
    ] {
        let payload = format!(
            r#"{{"after": {{"camera": "porch", "label": "package", "top_score": 0.9,
                "sub_label": {sub_label}}}, "type": "new"}}"#
        );
        let event = parse_frigate_event(payload.as_bytes())
            .unwrap_or_else(|e| panic!("sub_label {sub_label} failed the message: {e}"));
        assert_eq!(event.labels, vec!["package"], "sub_label {sub_label}");
        assert_eq!(
            select_event_type(&event.labels, &["package".to_string()]),
            Some(EventType::BoundaryCrossingObjectSmall)
        );
    }
}

#[test]
fn identity_labeled_objects_yield_no_event() {
    for label in ["face", "license_plate"] {
        let payload = FRIGATE_EVENT_NEW.replace("\"person\"", &format!("\"{label}\""));
        let err = parse_frigate_event(payload.as_bytes()).unwrap_err();
        assert!(err.to_string().contains("identity label"), "{label}: {err}");
    }
}

#[test]
fn unknown_keys_at_every_level_are_ignored() {
    let payload = r#"{
        "SENTINEL_TOP": "SENTINEL_TOP_VALUE",
        "before": {"SENTINEL_BEFORE": [1, 2, {"deep": true}]},
        "after": {
            "camera": "front_door", "label": "person", "top_score": 0.9,
            "SENTINEL_AFTER": {"nested": {"deeper": "SENTINEL_DEEP"}},
            "entered_zones": ["porch"]
        },
        "type": "new"
    }"#;
    let event = parse_frigate_event(payload.as_bytes()).expect("unknown keys are ignored");
    assert_eq!(event.labels, vec!["person"]);
    assert_eq!(event.zones, vec!["porch"]);
}

#[test]
fn zone_name_sanitization_removes_special_chars() {
    assert_eq!(sanitize_zone_name("Front Door"), "front_door");
    assert_eq!(sanitize_zone_name("Zone #1"), "zone__1");
    assert_eq!(sanitize_zone_name("porch-area"), "porch-area");
}

#[test]
fn zone_name_sanitization_limits_length() {
    let long_name = "a".repeat(100);
    let sanitized = sanitize_zone_name(&long_name);
    assert_eq!(sanitized.len(), 64);
}

// ==================== Camera slug + subscriptions (contracts C8, D7) ====================

#[test]
fn camera_slug_matches_wizard_rule() {
    // The examples the contract states; serve_wizard.py's camera_slug must agree.
    let cases = [
        ("Front Door", "front_door"),
        ("front-door", "front_door"),
        ("Front  Door", "front__door"),
        ("Garage_2", "garage_2"),
        ("Café", "caf_"),
        ("porch", "porch"),
        ("BACK yard!", "back_yard_"),
    ];
    for (input, expected) in cases {
        assert_eq!(camera_slug(input), expected, "{input:?}");
    }
    // Per code point, truncated to 64 — never trimmed or collapsed.
    assert_eq!(camera_slug(&"ab ".repeat(40)).len(), 64);
    assert_eq!(camera_slug(""), "");
    // The slug and the zone rule are different rules: a hyphenated camera
    // keeps its hyphen in the zone id, so existing sealed logs do not move.
    assert_eq!(sanitize_zone_name("front-door"), "front-door");
    assert_eq!(camera_slug("front-door"), "front_door");
}

#[test]
fn subscriptions_are_exactly_events_or_reviews() {
    assert_eq!(
        frigate_subscriptions(FrigateMode::Events, "frigate", None),
        vec!["frigate/events".to_string()]
    );
    assert_eq!(
        frigate_subscriptions(FrigateMode::Reviews, "frigate", None),
        vec!["frigate/reviews".to_string()]
    );
    assert_eq!(
        frigate_subscriptions(FrigateMode::Events, "nvr", None),
        vec!["nvr/events".to_string()]
    );
    // The legacy full-topic override applies to events mode only.
    assert_eq!(
        frigate_subscriptions(FrigateMode::Events, "frigate", Some("custom/events")),
        vec!["custom/events".to_string()]
    );
    assert_eq!(
        frigate_subscriptions(FrigateMode::Reviews, "frigate", Some("custom/events")),
        vec!["frigate/reviews".to_string()]
    );
    // Never a snapshot, tracked_object_update, camera_activity or audio topic.
    for mode in [FrigateMode::Events, FrigateMode::Reviews] {
        for topic in frigate_subscriptions(mode, "frigate", None) {
            for forbidden in [
                "snapshot",
                "tracked_object_update",
                "camera_activity",
                "audio",
                "#",
                "+",
            ] {
                assert!(!topic.contains(forbidden), "{mode}: {topic}");
            }
        }
    }
    assert_eq!(FrigateMode::Events.as_str(), "events");
    assert_eq!(FrigateMode::Reviews.as_str(), "reviews");
}

// ==================== Identity stripping fixtures (D7) ====================

#[test]
fn discard_lists_are_exercised_by_the_fixtures() {
    // Every discarded events field appears in at least one fixture's
    // `after`, and the 0.17 fixture (the widest shape) carries them all.
    let mut seen: BTreeSet<String> = BTreeSet::new();
    for name in EVENT_FIXTURES {
        let value = fixture_json(name);
        let after = value["after"].as_object().expect("after");
        for key in after.keys() {
            seen.insert(key.clone());
            assert!(
                KEPT_EVENT_FIELDS.contains(&key.as_str())
                    || DISCARDED_EVENT_FIELDS.contains(&key.as_str())
                    || key.starts_with("SENTINEL_"),
                "{name}: after.{key} is neither kept, in the discard list, nor an unknown-key sentinel"
            );
        }
    }
    for field in DISCARDED_EVENT_FIELDS {
        assert!(seen.contains(*field), "no events fixture populates {field}");
    }
    let widest = fixture_json("events_0.17_new.json");
    let widest_after = widest["after"].as_object().expect("after");
    for field in DISCARDED_EVENT_FIELDS {
        assert!(
            widest_after.contains_key(*field),
            "events_0.17_new.json does not populate {field}"
        );
    }
    for field in KEPT_EVENT_FIELDS {
        assert!(
            widest_after.contains_key(*field),
            "events_0.17_new.json does not carry kept field {field}"
        );
    }

    // Likewise for reviews, with `data.` fields nested.
    let mut seen: BTreeSet<String> = BTreeSet::new();
    for name in REVIEW_FIXTURES {
        let value = fixture_json(name);
        let after = value["after"].as_object().expect("after");
        for (key, v) in after {
            if key == "data" {
                for data_key in v.as_object().expect("data").keys() {
                    seen.insert(format!("data.{data_key}"));
                }
            } else {
                seen.insert(key.clone());
            }
        }
    }
    for field in DISCARDED_REVIEW_FIELDS {
        assert!(seen.contains(*field), "no review fixture populates {field}");
    }
    for field in KEPT_REVIEW_FIELDS {
        assert!(
            seen.contains(*field),
            "no review fixture carries kept field {field}"
        );
    }
    // The GenAI fields are nested under data.metadata in 0.17 too.
    let genai = fixture_json("reviews_0.17_new_alert_genai.json");
    for field in ["title", "description", "scene", "other_concerns"] {
        assert!(
            genai["after"]["data"]["metadata"].get(field).is_some(),
            "data.metadata.{field}"
        );
    }
}

#[test]
fn every_events_fixture_seals_without_a_discarded_value() {
    for name in EVENT_FIXTURES {
        let raw = fixture(name);
        let sentinels = event_sentinels(&fixture_json(name));
        let parsed = parse_frigate_event(raw.as_bytes())
            .unwrap_or_else(|e| panic!("{name} must parse: {e}"));
        assert_eq!(parsed.kind, FrigateClaimKind::TrackedObject, "{name}");
        assert_eq!(parsed.labels.len(), 1, "{name}");
        assert!(!is_identity_label(&parsed.labels[0]), "{name}");
        assert!(parsed.confidence > 0.5, "{name}: top_score is kept");
        assert!(!parsed.zones.is_empty(), "{name}: zones are kept");
        assert_sealed_without(name, &parsed, &sentinels);
    }
}

#[test]
fn events_fixtures_keep_exactly_the_claim_fields() {
    let parsed = parse_frigate_event(fixture("events_0.16_new.json").as_bytes()).expect("0.16");
    assert_eq!(parsed.camera, "Front Door");
    assert_eq!(parsed.labels, vec!["person"]);
    assert!((parsed.confidence - 0.87).abs() < 0.001, "top_score wins");
    assert_eq!(parsed.zones, vec!["driveway", "porch"]);

    let parsed = parse_frigate_event(fixture("events_0.15_new.json").as_bytes()).expect("0.15");
    assert_eq!(parsed.camera, "driveway_cam");
    assert_eq!(parsed.labels, vec!["car"]);
    assert!((parsed.confidence - 0.95).abs() < 0.001);
    assert_eq!(parsed.zones, vec!["street", "driveway"]);
}

#[test]
fn every_review_fixture_seals_its_alert_once_without_a_discarded_value() {
    for name in REVIEW_FIXTURES {
        let raw = fixture(name);
        let sentinels = review_sentinels(&fixture_json(name));
        let parsed =
            parse_review_event(raw.as_bytes()).unwrap_or_else(|e| panic!("{name} must seal: {e}"));
        assert_eq!(parsed.kind, FrigateClaimKind::ReviewAlert, "{name}");
        assert!((parsed.confidence - 1.0).abs() < f64::EPSILON, "{name}");
        assert!(
            parsed.labels.contains(&"person".to_string())
                || parsed.labels.contains(&"dog".to_string()),
            "{name}: {:?}",
            parsed.labels
        );
        assert!(
            parsed.labels.iter().all(|l| !is_identity_label(l)),
            "{name}: identity labels filtered from {:?}",
            parsed.labels
        );
        assert_eq!(parsed.zones, vec!["porch"], "{name}");
        assert_sealed_without(name, &parsed, &sentinels);
    }
}

#[test]
fn review_alert_is_sealed_at_the_transition_only() {
    // A `new` that is already an alert seals.
    let new_alert = fixture("reviews_0.14_new_alert.json");
    assert!(parse_review_event(new_alert.as_bytes()).is_ok());
    // A `new` detection does not.
    let new_detection =
        new_alert.replacen("\"severity\": \"alert\"", "\"severity\": \"detection\"", 1);
    assert!(parse_review_event(new_detection.as_bytes())
        .unwrap_err()
        .to_string()
        .contains("not an alert"));

    // An `update` that promotes detection → alert seals once…
    let promoted = fixture("reviews_0.16_update_to_alert.json");
    let parsed = parse_review_event(promoted.as_bytes()).expect("promotion seals");
    assert_eq!(
        parsed.labels,
        vec!["dog", "person"],
        "license_plate filtered out"
    );
    assert_eq!(
        select_event_type(&parsed.labels, &default_labels()),
        Some(EventType::BoundaryCrossingObjectLarge),
        "the coarsest allowed class"
    );
    // …a later `update` of an alert that was already one does not…
    let already = promoted.replacen("\"severity\": \"detection\"", "\"severity\": \"alert\"", 1);
    assert!(parse_review_event(already.as_bytes())
        .unwrap_err()
        .to_string()
        .contains("already"));
    // …an `update` still at detection does not…
    let still_detection =
        promoted.replace("\"severity\": \"alert\"", "\"severity\": \"detection\"");
    assert!(parse_review_event(still_detection.as_bytes()).is_err());
    // …and `end` never does.
    let ended = promoted.replacen("\"type\": \"update\"", "\"type\": \"end\"", 1);
    assert!(parse_review_event(ended.as_bytes())
        .unwrap_err()
        .to_string()
        .contains("ended"));

    // An alert naming only identity labels seals nothing.
    let faces_only = new_alert.replacen(
        "\"objects\": [\"person\"]",
        "\"objects\": [\"face\", \"license_plate\"]",
        2,
    );
    assert!(parse_review_event(faces_only.as_bytes())
        .unwrap_err()
        .to_string()
        .contains("no object class"));
}

// ==================== Kernel Integration Tests ====================

#[test]
fn kernel_accepts_frigate_event_and_produces_sealed_log() {
    let (mut kernel, cfg) = setup_test_kernel();

    // Parse and convert a Frigate event
    let parsed = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes()).expect("parse event");
    let zone_id = zone_id_for(&parsed);

    let event = seal_parsed(&mut kernel, &cfg, &parsed);
    assert_eq!(event.event_type, EventType::BoundaryCrossingObjectLarge);
    assert_eq!(event.zone_id, zone_id);
    assert!(event.correlation_token.is_none()); // Ensure no tracking ID
}

#[test]
fn kernel_strips_correlation_tokens_from_frigate_events() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let bucket = TimeBucket::now(600).expect("time bucket");
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: None, // Must always be None for Frigate events
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
        .expect("append event");

    // Verify no correlation token leaked through
    assert!(event.correlation_token.is_none());

    // Verify export also has no tokens
    let json = export_json(&mut kernel, &cfg);
    assert!(
        !json.contains("correlation_token"),
        "Export should not contain correlation_token"
    );
}

#[test]
fn export_carries_no_frigate_object_id_or_precise_timestamp() {
    // The Frigate object id ("1234567890.abc123") embeds a precise unix
    // timestamp. FrigateEventData structurally never deserializes it — this
    // pins that guarantee at the export boundary, the way adapter_meshtastic
    // pins the node id.
    let (mut kernel, cfg) = setup_test_kernel();

    let parsed = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes()).expect("parse event");
    let zone_id = zone_id_for(&parsed);
    seal_parsed(&mut kernel, &cfg, &parsed);

    let json = export_json(&mut kernel, &cfg);

    // The precise-timestamp id, in either half, must not survive into the export…
    assert!(
        !json.contains("1234567890"),
        "precise Frigate timestamp id leaked into export"
    );
    assert!(
        !json.contains("abc123"),
        "Frigate object id leaked into export"
    );
    // …and the raw camera name must not appear when a zone attribution exists.
    assert!(
        !json.contains("front_door"),
        "raw camera name leaked into export"
    );
    assert!(json.contains(&zone_id), "coarse zone must remain");
}

#[test]
fn multiple_frigate_events_create_hash_chain() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    // Add 5 events
    for i in 0..5 {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let candidate = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: bucket,
            zone_id: format!("zone:camera_{}", i),
            confidence: 0.8 + (i as f32 * 0.02),
            correlation_token: None,
            attestation: None,
        };

        kernel
            .append_event_checked(
                &desc,
                candidate,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
            .expect("append event");
    }

    // Export and verify structure
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    // Count total events in batches
    let total_events: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    assert_eq!(total_events, 5, "Should have 5 events in export");
}

#[test]
fn frigate_events_use_coarsened_timestamps_only() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let bucket = TimeBucket::now(600).expect("time bucket");
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: None,
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
        .expect("append event");

    // Time bucket start should be aligned to bucket boundary
    let bucket_start = event.time_bucket.start_epoch_s;
    let bucket_size = event.time_bucket.size_s as u64;
    assert_eq!(
        bucket_start % bucket_size,
        0,
        "Time bucket start must be aligned to bucket boundary"
    );
}

// ==================== Review Event Tests ====================

#[test]
fn flat_legacy_review_shape_is_not_sealed() {
    // No `after` item: not a Frigate review message, nothing to seal.
    let err = parse_review_event(FRIGATE_REVIEW_FLAT_LEGACY.as_bytes()).unwrap_err();
    assert!(err.to_string().contains("missing 'after'"), "{err}");
}

#[test]
fn frigate_review_017_schema_parses_from_after_section() {
    let event = parse_review_event(FRIGATE_REVIEW_017_NEW.as_bytes())
        .expect("real 0.14+ before/after review schema must parse");

    assert_eq!(event.camera, "front_door");
    assert_eq!(event.labels, vec!["person"]);
    assert_eq!(event.kind, FrigateClaimKind::ReviewAlert);
    // Real review payloads carry no score; an alert seals at full confidence.
    assert!((event.confidence - 1.0).abs() < f64::EPSILON);
    assert_eq!(event.zones, vec!["porch"]);
}

#[test]
fn frigate_review_017_update_of_an_alert_is_rejected() {
    // The fixture's `before` is a detection, so an `update` seals (the
    // promotion); with `before` already an alert it does not.
    let payload = FRIGATE_REVIEW_017_NEW.replacen("\"type\": \"new\"", "\"type\": \"update\"", 1);
    assert!(parse_review_event(payload.as_bytes()).is_ok());
    let payload = payload.replacen("\"severity\": \"detection\"", "\"severity\": \"alert\"", 1);
    assert!(parse_review_event(payload.as_bytes()).is_err());
}

#[test]
fn frigate_event_017_schema_parses_and_strips_extras() {
    let event = parse_frigate_event(FRIGATE_EVENT_017_NEW.as_bytes())
        .expect("verbatim 0.16/0.17 events payload must parse");

    assert_eq!(event.camera, "front_door");
    assert_eq!(event.labels, vec!["person"]);
    assert!((event.confidence - 0.87).abs() < 0.001); // top_score wins
                                                      // entered_zones preferred over current_zones
    assert_eq!(event.zones, vec!["driveway", "porch"]);
}
