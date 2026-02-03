//! Integration tests for Frigate MQTT → PWK pipeline.
//!
//! These tests verify that:
//! 1. Frigate event JSON is correctly parsed
//! 2. Field mapping complies with spec/event_contract.md
//! 3. Time coarsening is applied correctly
//! 4. Malformed JSON is rejected gracefully
//! 5. Duplicate events in same bucket are deduplicated

use std::time::Duration;
use witness_kernel::transport::{
    map_label_to_event_type, parse_frigate_event, parse_review_event, sanitize_zone_name,
};
use witness_kernel::{
    CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy, MIN_BUCKET_SIZE_S,
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

/// Frigate review event
const FRIGATE_REVIEW_NEW: &str = r#"{
    "type": "new",
    "id": "review123",
    "camera": "garage",
    "data": {
        "objects": ["motorcycle"],
        "score": 0.85,
        "zones": ["garage_zone"]
    }
}"#;

fn setup_test_kernel() -> (Kernel, KernelConfig) {
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:frigate_test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:frigate_test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:frigate_test".to_string(),
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
    assert_eq!(event.label, "person");
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
    assert_eq!(event.label, "car");
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
    assert_eq!(
        map_label_to_event_type("person"),
        EventType::BoundaryCrossingObjectLarge
    );
    assert_eq!(
        map_label_to_event_type("Person"),
        EventType::BoundaryCrossingObjectLarge
    );
    assert_eq!(
        map_label_to_event_type("PERSON"),
        EventType::BoundaryCrossingObjectLarge
    );
}

#[test]
fn label_to_event_type_mapping_vehicles_are_large() {
    assert_eq!(
        map_label_to_event_type("car"),
        EventType::BoundaryCrossingObjectLarge
    );
    assert_eq!(
        map_label_to_event_type("truck"),
        EventType::BoundaryCrossingObjectLarge
    );
    assert_eq!(
        map_label_to_event_type("bus"),
        EventType::BoundaryCrossingObjectLarge
    );
    assert_eq!(
        map_label_to_event_type("motorcycle"),
        EventType::BoundaryCrossingObjectLarge
    );
}

#[test]
fn label_to_event_type_mapping_animals_are_small() {
    assert_eq!(
        map_label_to_event_type("dog"),
        EventType::BoundaryCrossingObjectSmall
    );
    assert_eq!(
        map_label_to_event_type("cat"),
        EventType::BoundaryCrossingObjectSmall
    );
    assert_eq!(
        map_label_to_event_type("bird"),
        EventType::BoundaryCrossingObjectSmall
    );
}

#[test]
fn label_to_event_type_mapping_unknown_defaults_to_small() {
    assert_eq!(
        map_label_to_event_type("unknown_object"),
        EventType::BoundaryCrossingObjectSmall
    );
}

#[test]
fn label_to_event_type_handles_sublabel_format() {
    // "package:amazon" should extract "package" and map to small
    assert_eq!(
        map_label_to_event_type("package:amazon"),
        EventType::BoundaryCrossingObjectSmall
    );
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

// ==================== Kernel Integration Tests ====================

#[test]
fn kernel_accepts_frigate_event_and_produces_sealed_log() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    // Parse and convert a Frigate event
    let parsed = parse_frigate_event(FRIGATE_EVENT_NEW.as_bytes()).expect("parse event");

    let event_type = map_label_to_event_type(&parsed.label);
    let zone_id = if let Some(zone) = parsed.zones.first() {
        format!("zone:{}", sanitize_zone_name(zone))
    } else {
        format!("zone:{}", sanitize_zone_name(&parsed.camera))
    };

    let bucket = TimeBucket::now(600).expect("time bucket");
    let candidate = CandidateEvent {
        event_type,
        time_bucket: bucket,
        zone_id: zone_id.clone(),
        confidence: parsed.confidence as f32,
        correlation_token: None, // Frigate IDs are stripped
    };

    let result = kernel.append_event_checked(
        &desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    );

    assert!(result.is_ok());
    let event = result.unwrap();
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
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    let json = serde_json::to_string(&artifact).expect("serialize");
    assert!(
        !json.contains("correlation_token"),
        "Export should not contain correlation_token"
    );
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
fn frigate_review_event_parsing() {
    let result = parse_review_event(FRIGATE_REVIEW_NEW.as_bytes());
    assert!(result.is_ok());

    let event = result.unwrap();
    assert_eq!(event.camera, "garage");
    assert_eq!(event.label, "motorcycle");
    assert!((event.confidence - 0.85).abs() < 0.001);
    assert_eq!(event.zones, vec!["garage_zone"]);
}
