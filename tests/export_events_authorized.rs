use anyhow::Result;
use ed25519_dalek::{Signer, SigningKey};
use witness_kernel::{
    Approval, BreakGlass, CandidateEvent, EventType, ExportOptions, Kernel, KernelConfig,
    ModuleDescriptor, QuorumPolicy, TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest,
    EXPORT_EVENTS_ENVELOPE_ID,
};

fn add_test_event(kernel: &mut Kernel, cfg: &KernelConfig) -> Result<()> {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
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
    let approval = Approval::new(
        TrusteeId::new("alice"),
        request.request_hash(),
        signing_key.sign(&request.request_hash()).to_vec(),
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
        device_key_seed: "devkey:test".to_string(),
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
        device_key_seed: "devkey:test".to_string(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;

    let bucket = TimeBucket::now(600)?;
    let (request, approval, policy) = authorize_export(&cfg, bucket)?;
    let (result, receipt) = BreakGlass::authorize(&policy, &request, &[approval.clone()], bucket);
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
