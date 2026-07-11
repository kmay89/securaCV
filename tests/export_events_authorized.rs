use anyhow::Result;
use ed25519_dalek::SigningKey;
use witness_kernel::{
    verify_export_bundle, Approval, BreakGlass, BreakGlassTokenFile, CandidateEvent, EventType,
    ExportAuthMode, ExportOptions, ExportReceipt, ExportWindow, InferenceBackend, Kernel,
    KernelConfig, ModuleDescriptor, QuorumPolicy, TimeBucket, TrusteeEntry, TrusteeId,
    UnlockRequest, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

fn add_test_event(kernel: &mut Kernel, cfg: &KernelConfig) -> Result<()> {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        },
        zone_id: "zone:test".to_string(),
        confidence: 0.5,
        correlation_token: None,
        attestation: None,
    };
    kernel.append_event_checked(
        &desc,
        cand,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;
    Ok(())
}

fn authorize_export(
    cfg: &KernelConfig,
    bucket: TimeBucket,
) -> Result<(UnlockRequest, Approval, QuorumPolicy)> {
    let signing_key = SigningKey::from_bytes(&[7u8; 32]);
    let request = UnlockRequest::new(
        EXPORT_EVENTS_ENVELOPE_ID,
        cfg.ruleset_hash,
        "export_events",
        bucket,
    )?;
    let approval = Approval::signed(
        TrusteeId::new("alice"),
        request.request_hash(),
        &signing_key,
    );
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: signing_key.verifying_key().to_bytes(),
        }],
    )?;
    Ok((request, approval, policy))
}

#[test]
fn export_fails_without_valid_token() -> Result<()> {
    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
    let mut token = result.expect("token should be issued before signing");

    let result =
        kernel.export_events_authorized(cfg.ruleset_hash, ExportOptions::default(), &mut token);
    assert!(result.is_err());
    Ok(())
}

#[test]
fn export_succeeds_with_break_glass_token() -> Result<()> {
    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    // Persist the quorum policy as the real flow does: the runtime unseal /
    // export gate re-derives the quorum from the configured policy (Invariant
    // V) and refuses a Granted receipt it cannot corroborate.
    kernel.set_break_glass_policy(&policy)?;
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result.expect("token");
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;

    let artifact = kernel.export_events_authorized(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &mut token,
    )?;
    assert!(!artifact.batches.is_empty());

    let count: i64 = kernel
        .conn
        .query_row("SELECT COUNT(*) FROM export_receipts", [], |row| row.get(0))?;
    assert_eq!(count, 1);
    Ok(())
}

#[test]
fn export_replay_of_token_file_is_refused_across_invocations() -> Result<()> {
    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    // Persist the quorum policy as the real flow does: the runtime unseal /
    // export gate re-derives the quorum from the configured policy (Invariant
    // V) and refuses a Granted receipt it cannot corroborate.
    kernel.set_break_glass_policy(&policy)?;
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result.expect("token");
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;

    // The replay vector: a token FILE always re-parses as unconsumed —
    // the in-memory `consumed` flag protects nothing across invocations.
    let token_file = BreakGlassTokenFile::from_token(&token)?;
    let mut replayed = token_file.into_token()?;

    // First use succeeds and burns the nonce in the durable ledger.
    kernel.export_events_authorized(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &mut token,
    )?;

    // Second use with the re-parsed (fresh, unconsumed-looking) token must
    // be refused by the persisted nonce even inside the validity bucket.
    let err = kernel
        .export_events_authorized(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                ..ExportOptions::default()
            },
            &mut replayed,
        )
        .expect_err("replayed token file must be refused");
    assert!(
        err.to_string().contains("already consumed"),
        "unexpected error: {err}"
    );

    // Exactly one export happened.
    let count: i64 = kernel
        .conn
        .query_row("SELECT COUNT(*) FROM export_receipts", [], |row| row.get(0))?;
    assert_eq!(count, 1);
    Ok(())
}

#[test]
fn export_bundle_verifies_and_detects_tampering() -> Result<()> {
    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    // Persist the quorum policy as the real flow does: the runtime unseal /
    // export gate re-derives the quorum from the configured policy (Invariant
    // V) and refuses a Granted receipt it cannot corroborate.
    kernel.set_break_glass_policy(&policy)?;
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result.expect("token");
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;

    let bundle = kernel.export_events_bundle_authorized(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &mut token,
    )?;
    verify_export_bundle(&bundle)?;

    let mut tampered = bundle.clone();
    tampered.artifact.batches.clear();
    assert!(verify_export_bundle(&tampered).is_err());
    Ok(())
}

fn test_cfg() -> KernelConfig {
    KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn add_event_at_bucket(kernel: &mut Kernel, cfg: &KernelConfig, start_epoch_s: u64) -> Result<()> {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket {
            start_epoch_s,
            size_s: 600,
        },
        zone_id: "zone:test".to_string(),
        confidence: 0.5,
        correlation_token: None,
        attestation: None,
    };
    kernel.append_event_checked(
        &desc,
        cand,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;
    Ok(())
}

#[test]
fn self_export_bundle_verifies_and_labels_receipt() -> Result<()> {
    let cfg = test_cfg();
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    // No break-glass token anywhere in this path.
    let bundle = kernel.export_events_bundle_self(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
    )?;
    assert!(!bundle.artifact.batches.is_empty());
    verify_export_bundle(&bundle)?;

    // The export still leaves a chained receipt, labeled with its auth mode.
    assert_eq!(
        bundle.receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::SelfExport)
    );
    let count: i64 = kernel
        .conn
        .query_row("SELECT COUNT(*) FROM export_receipts", [], |row| row.get(0))?;
    assert_eq!(count, 1);
    Ok(())
}

#[test]
fn break_glass_export_receipt_is_labeled_break_glass() -> Result<()> {
    let cfg = test_cfg();
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    // Persist the quorum policy as the real flow does: the runtime unseal /
    // export gate re-derives the quorum from the configured policy (Invariant
    // V) and refuses a Granted receipt it cannot corroborate.
    kernel.set_break_glass_policy(&policy)?;
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result.expect("token");
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;

    kernel.export_events_authorized(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &mut token,
    )?;
    assert_eq!(
        kernel.latest_export_receipt_entry()?.receipt.auth_mode,
        Some(ExportAuthMode::BreakGlass)
    );
    Ok(())
}

#[test]
fn windowed_export_filters_on_true_buckets_and_records_window() -> Result<()> {
    let cfg = test_cfg();
    let mut kernel = Kernel::open(&cfg)?;
    for start in [0u64, 600, 1200, 1800] {
        add_event_at_bucket(&mut kernel, &cfg, start)?;
    }

    let window = ExportWindow {
        start_epoch_s: 600,
        end_epoch_s: 1800,
    };
    let bundle = kernel.export_events_bundle_self(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            window: Some(window),
            ..ExportOptions::default()
        },
    )?;
    verify_export_bundle(&bundle)?;

    // Half-open [600, 1800): buckets 600 and 1200 only.
    let bucket_starts: Vec<u64> = bundle
        .artifact
        .batches
        .iter()
        .flat_map(|b| &b.buckets)
        .map(|b| b.time_bucket.start_epoch_s)
        .collect();
    assert_eq!(bucket_starts, vec![600, 1200]);
    // The signed receipt records the disclosure scope.
    assert_eq!(bundle.receipt_entry.receipt.window, Some(window));
    Ok(())
}

#[test]
fn unaligned_or_inverted_windows_are_rejected() -> Result<()> {
    let cfg = test_cfg();
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    for (start, end) in [(601u64, 1800u64), (600, 1801), (1800, 600), (600, 600)] {
        let result = kernel.export_events_self(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                window: Some(ExportWindow {
                    start_epoch_s: start,
                    end_epoch_s: end,
                }),
                ..ExportOptions::default()
            },
        );
        assert!(
            result.is_err(),
            "window {}..{} must be rejected",
            start,
            end
        );
    }
    Ok(())
}

/// Pins the receipt wire format both ways: a legacy receipt (no auth_mode /
/// window) must round-trip byte-identically, and a new receipt must keep the
/// optional fields last — the envelope verifiers (Rust + JS) re-serialize this
/// struct to recompute the signed entry hash.
#[test]
fn export_receipt_serde_round_trips_both_generations() -> Result<()> {
    let legacy = r#"{"time_bucket":{"start_epoch_s":600,"size_s":600},"ruleset_hash":[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"batch_size":50,"artifact_hash":[2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2]}"#;
    let parsed: ExportReceipt = serde_json::from_str(legacy)?;
    assert_eq!(parsed.auth_mode, None);
    assert_eq!(parsed.window, None);
    assert_eq!(serde_json::to_string(&parsed)?, legacy);

    let modern = ExportReceipt {
        auth_mode: Some(ExportAuthMode::SelfExport),
        window: Some(ExportWindow {
            start_epoch_s: 600,
            end_epoch_s: 1200,
        }),
        ..parsed
    };
    let json = serde_json::to_string(&modern)?;
    assert!(
        json.ends_with(
            r#""auth_mode":"self_export","window":{"start_epoch_s":600,"end_epoch_s":1200}}"#
        ),
        "optional receipt fields must stay last: {json}"
    );
    let reparsed: ExportReceipt = serde_json::from_str(&json)?;
    assert_eq!(serde_json::to_string(&reparsed)?, json);
    Ok(())
}
