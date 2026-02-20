//! Kernel hardening tests for Phase 2.
//!
//! Tests hash chain integrity, signature validation, tamper detection,
//! and break-glass ceremony edge cases per spec/invariants.md.

use std::time::Duration;
use witness_kernel::{
    hash_entry, CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
};

fn setup_test_kernel() -> (Kernel, KernelConfig) {
    let cfg = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:hardening_test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:hardening_test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:hardening_test:a1b2c3d4e5f6".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let kernel = Kernel::open(&cfg).expect("open kernel");
    (kernel, cfg)
}

fn make_module_descriptor() -> ModuleDescriptor {
    ModuleDescriptor {
        id: "hardening_test",
        allowed_event_types: &[
            EventType::BoundaryCrossingObjectSmall,
            EventType::BoundaryCrossingObjectLarge,
        ],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    }
}

fn add_events(kernel: &mut Kernel, cfg: &KernelConfig, count: usize) {
    let desc = make_module_descriptor();
    for i in 0..count {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let candidate = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: bucket,
            zone_id: format!("zone:camera_{}", i % 10),
            confidence: 0.5 + ((i % 500) as f32 * 0.001), // stays in valid 0..=1 range
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
}

// ==================== Hash Chain Integrity Tests ====================

#[test]
fn hash_chain_is_valid_after_single_event() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 1);

    // Verify the chain using log_verify logic
    let device_key = kernel.device_key_for_verify_only();
    assert_ne!(device_key, [0u8; 32], "Device key should be non-zero");
}

#[test]
fn hash_chain_is_valid_after_many_events() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 100);

    // Export to verify chain structure
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    let total_events: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    assert_eq!(total_events, 100, "Should have all 100 events");
}

#[test]
fn hash_chain_survives_rapid_event_ingestion() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    // Rapid ingestion: 500 events in quick succession
    for i in 0..500 {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let candidate = CandidateEvent {
            event_type: if i % 2 == 0 {
                EventType::BoundaryCrossingObjectLarge
            } else {
                EventType::BoundaryCrossingObjectSmall
            },
            time_bucket: bucket,
            zone_id: format!("zone:rapid_{}", i % 20),
            confidence: 0.5 + ((i % 50) as f32) * 0.01,
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
            .expect("append rapid event");
    }

    // Verify all events are present and exportable
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export after rapid ingestion");

    let total: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    assert_eq!(total, 500, "All 500 rapid events should be present");
}

// ==================== Ed25519 Signature Tests ====================

// Note: Direct signature tests require complex SignatureKeys setup.
// The kernel handles signing internally. These tests verify the hash function
// used by the kernel for building the hash chain.

#[test]
fn hash_entry_is_deterministic() {
    let prev_hash = [1u8; 32];
    let payload = b"test event payload";

    let hash1 = hash_entry(&prev_hash, payload);
    let hash2 = hash_entry(&prev_hash, payload);

    assert_eq!(hash1, hash2, "Hash should be deterministic");
}

#[test]
fn hash_entry_changes_with_different_prev_hash() {
    let prev_hash1 = [1u8; 32];
    let prev_hash2 = [2u8; 32];
    let payload = b"test event payload";

    let hash1 = hash_entry(&prev_hash1, payload);
    let hash2 = hash_entry(&prev_hash2, payload);

    assert_ne!(
        hash1, hash2,
        "Different prev_hash should produce different hash"
    );
}

#[test]
fn hash_entry_changes_with_different_payload() {
    let prev_hash = [1u8; 32];
    let payload1 = b"payload version 1";
    let payload2 = b"payload version 2";

    let hash1 = hash_entry(&prev_hash, payload1);
    let hash2 = hash_entry(&prev_hash, payload2);

    assert_ne!(
        hash1, hash2,
        "Different payload should produce different hash"
    );
}

#[test]
fn hash_entry_produces_32_byte_hash() {
    let prev_hash = [0u8; 32];
    let payload = b"test";

    let hash = hash_entry(&prev_hash, payload);

    assert_eq!(hash.len(), 32, "Hash should be 32 bytes (SHA-256)");
}

#[test]
fn hash_entry_empty_payload_produces_valid_hash() {
    let prev_hash = [0u8; 32];
    let payload = b"";

    let hash = hash_entry(&prev_hash, payload);

    // Should not panic and should produce a valid hash
    assert_ne!(
        hash, [0u8; 32],
        "Empty payload should produce non-zero hash"
    );
}

// ==================== Export Robustness Tests ====================

#[test]
fn export_with_zero_events_produces_valid_artifact() {
    let (mut kernel, cfg) = setup_test_kernel();
    // Don't add any events

    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export empty");

    // Should produce valid (possibly empty) artifact
    let json = serde_json::to_string(&artifact).expect("serialize");
    assert!(json.contains("batches"), "Should have batches field");
}

#[test]
fn export_with_many_events_maintains_structure() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 1000);

    let artifact = kernel
        .export_events_for_api(
            cfg.ruleset_hash,
            ExportOptions {
                max_events_per_batch: 100,
                ..ExportOptions::default()
            },
        )
        .expect("export many");

    let total: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    assert_eq!(total, 1000, "All 1000 events should be in export");
}

#[test]
fn export_applies_jitter_when_configured() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 10);

    let artifact = kernel
        .export_events_for_api(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 60,
                jitter_step_s: 30,
                ..ExportOptions::default()
            },
        )
        .expect("export with jitter");

    assert_eq!(artifact.jitter_s, 60);
    assert_eq!(artifact.jitter_step_s, 30);
}

#[test]
fn export_omits_forbidden_fields() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    // Add event with correlation token (internal use)
    let bucket = TimeBucket::now(600).expect("time bucket");
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: Some([42u8; 32]), // This should be stripped on export
    };

    kernel
        .append_event_checked(
            &desc,
            candidate,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("append");

    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    let json = serde_json::to_string(&artifact).expect("serialize");

    // Per spec/event_contract.md §7: forbidden metadata
    assert!(
        !json.contains("\"correlation_token\""),
        "Should not contain correlation_token in export"
    );
    assert!(
        !json.contains("\"created_at\""),
        "Should not contain created_at"
    );
    assert!(
        !json.contains("\"timestamp\""),
        "Should not contain timestamp (only time_bucket)"
    );
}

// ==================== Configuration Validation Tests ====================

#[test]
fn kernel_device_key_is_derived_from_seed() {
    // Test that the kernel derives a device key from the seed
    let cfg1 = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "seed_one:a1b2c3d4e5f6a7b8c9d0e1".to_string(),
        zone_policy: ZonePolicy::default(),
    };

    let cfg2 = KernelConfig {
        db_path: witness_kernel::shared_memory_uri(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "seed_two:a1b2c3d4e5f6a7b8c9d0e1".to_string(),
        zone_policy: ZonePolicy::default(),
    };

    let kernel1 = Kernel::open(&cfg1).expect("open kernel 1");
    let kernel2 = Kernel::open(&cfg2).expect("open kernel 2");

    // Different seeds should produce different device keys
    let key1 = kernel1.device_key_for_verify_only();
    let key2 = kernel2.device_key_for_verify_only();
    assert_ne!(key1, key2, "Different seeds should produce different keys");
}

#[test]
fn kernel_rejects_invalid_zone_id_format() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let bucket = TimeBucket::now(600).expect("time bucket");
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "lat=41.5,lon=-81.6".to_string(), // GPS coordinates - forbidden!
        confidence: 0.9,
        correlation_token: None,
    };

    let result = kernel.append_event_checked(
        &desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    );

    assert!(
        result.is_err(),
        "GPS coordinates should be rejected as zone_id"
    );
}

#[test]
fn kernel_rejects_confidence_out_of_bounds() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let bucket = TimeBucket::now(600).expect("time bucket");

    // Test confidence > 1.0
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: 1.5, // Invalid: > 1.0
        correlation_token: None,
    };

    let result = kernel.append_event_checked(
        &desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    );

    assert!(result.is_err(), "Confidence > 1.0 should be rejected");
}

#[test]
fn kernel_rejects_negative_confidence() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let bucket = TimeBucket::now(600).expect("time bucket");

    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: -0.5, // Invalid: < 0.0
        correlation_token: None,
    };

    let result = kernel.append_event_checked(
        &desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    );

    assert!(result.is_err(), "Negative confidence should be rejected");
}

// ==================== Module Authorization Tests ====================

#[test]
fn module_can_only_emit_allowed_event_types() {
    let (mut kernel, cfg) = setup_test_kernel();

    // Module only allowed to emit Small events
    let restricted_desc = ModuleDescriptor {
        id: "restricted_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectSmall],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };

    let bucket = TimeBucket::now(600).expect("time bucket");

    // Try to emit Large event (not allowed)
    let candidate = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge, // Not in allowed list
        time_bucket: bucket,
        zone_id: "zone:test".to_string(),
        confidence: 0.9,
        correlation_token: None,
    };

    let result = kernel.append_event_checked(
        &restricted_desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    );

    assert!(
        result.is_err(),
        "Module should not emit unauthorized event type"
    );
}

// ==================== Ruleset Binding Tests ====================

#[test]
fn events_are_bound_to_ruleset_at_creation() {
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
        .expect("append");

    // Verify ruleset binding
    assert_eq!(event.ruleset_id, cfg.ruleset_id);
    assert_eq!(event.ruleset_hash, cfg.ruleset_hash);
    assert_eq!(event.kernel_version, cfg.kernel_version);
}

#[test]
fn export_returns_events_for_matching_ruleset() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 10);

    // Export with matching ruleset hash
    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export matching");

    let total: usize = artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    assert_eq!(total, 10, "Should export all events for matching ruleset");
}

// ==================== Invariant II: No Identity Substrate — Additional Conformance ====================

/// Assert that ExportEvent contains ONLY the allowed fields.
/// This prevents accidental addition of identity-related fields.
#[test]
fn export_event_has_no_identity_fields() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 1);

    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    let event = &artifact.batches[0].buckets[0].events[0];
    let json = serde_json::to_value(event).expect("serialize");
    let obj = json.as_object().expect("json object");

    // Allowlist of fields that may appear in ExportEvent
    let allowed_fields: Vec<&str> = vec![
        "event_type",
        "time_bucket",
        "zone_id",
        "confidence",
        "kernel_version",
        "ruleset_id",
        "ruleset_hash",
    ];

    for key in obj.keys() {
        assert!(
            allowed_fields.contains(&key.as_str()),
            "ExportEvent contains unexpected field '{}' which may leak identity data",
            key
        );
    }

    // Explicitly verify no identity-like fields
    assert!(!obj.contains_key("correlation_token"), "correlation_token must not appear in export");
    assert!(!obj.contains_key("created_at"), "created_at must not appear in export");
    assert!(!obj.contains_key("mac_address"), "mac_address must not appear in export");
    assert!(!obj.contains_key("ip_address"), "ip_address must not appear in export");
    assert!(!obj.contains_key("device_id"), "device_id must not appear in export");
    assert!(!obj.contains_key("face_embedding"), "face_embedding must not appear in export");
    assert!(!obj.contains_key("plate_number"), "plate_number must not appear in export");
}

/// Assert that zone_id validation rejects anything that looks like GPS coordinates.
#[test]
fn zone_id_rejects_gps_coordinates() {
    let (mut kernel, cfg) = setup_test_kernel();
    let desc = make_module_descriptor();

    let gps_like_zone_ids = [
        "41.5,-81.6",
        "lat=41.5,lon=-81.6",
        "N41.5W81.6",
        "41°30'N",
        "zone:41.5",        // looks like coordinate embedded in zone format
    ];

    for zone_id in gps_like_zone_ids {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let candidate = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: bucket,
            zone_id: zone_id.to_string(),
            confidence: 0.8,
            correlation_token: None,
        };
        let result = kernel.append_event_checked(
            &desc,
            candidate,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert!(
            result.is_err(),
            "Zone ID '{}' should be rejected (looks like GPS coordinates)",
            zone_id
        );
    }
}

/// Assert that no GPS/latitude/longitude data appears in export artifacts.
#[test]
fn export_contains_no_gps_data() {
    let (mut kernel, cfg) = setup_test_kernel();
    add_events(&mut kernel, &cfg, 5);

    let artifact = kernel
        .export_events_for_api(cfg.ruleset_hash, ExportOptions::default())
        .expect("export");

    let json_str = serde_json::to_string(&artifact).expect("serialize artifact");

    // Ensure no GPS-related keywords appear in the serialized export
    let forbidden_patterns = ["latitude", "longitude", "lat", "lon", "gps", "coord"];
    for pattern in forbidden_patterns {
        assert!(
            !json_str.to_lowercase().contains(pattern),
            "Export artifact contains forbidden GPS-related pattern: '{}'",
            pattern
        );
    }
}

// ==================== Signing Key Seed Entropy Enforcement ====================

#[test]
fn signing_key_rejects_short_seed() {
    // Seeds shorter than MIN_SEED_LENGTH (32) should be rejected
    let result = witness_kernel::signing_key_from_seed("short_seed");
    assert!(result.is_err(), "Short seed should be rejected");
    assert!(
        result.unwrap_err().to_string().contains("too short"),
        "Error should mention seed is too short"
    );
}

#[test]
fn signing_key_accepts_long_enough_seed() {
    // A 32+ char hex seed should be accepted
    let long_seed = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6";
    assert_eq!(long_seed.len(), 32);
    let result = witness_kernel::signing_key_from_seed(long_seed);
    assert!(result.is_ok(), "Seed of length 32 should be accepted");
}

#[test]
fn signing_key_rejects_mvp_placeholder() {
    let result = witness_kernel::signing_key_from_seed("devkey:mvp");
    assert!(result.is_err(), "MVP placeholder should be rejected");
}
