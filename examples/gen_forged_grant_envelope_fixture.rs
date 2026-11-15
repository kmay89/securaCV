//! Generate ONLY `tests/fixtures/envelope/forged_grant_envelope.json`: an
//! envelope whose break-glass ledger holds a `Granted` row no trustee approved.
//! The row is device-signed and its approvals commitment matches, but the one
//! approval carries a filler signature. Both offline verifiers must report it
//! `valid_with_warnings`, never `ok`: they cannot check trustee signatures.
//!
//! Separate from `gen_envelope_fixtures`, which rewrites every fixture.
//!
//! Run with:  `cargo run --example gen_forged_grant_envelope_fixture`

use std::fs;
use std::path::Path;
use std::time::Duration;

use witness_kernel::{
    approvals_commitment, Approval, BreakGlassOutcome, BreakGlassReceipt, CandidateEvent,
    EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, TimeBucket,
    TrusteeId, UnlockRequest, ZonePolicy,
};

fn main() -> anyhow::Result<()> {
    let out_dir = Path::new("tests/fixtures/envelope");
    fs::create_dir_all(out_dir)?;

    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:fixture".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:fixture"),
        kernel_version: "fixture-0".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: "devkey:fixture:0123456789abcdef0123".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;

    let desc = ModuleDescriptor {
        id: "fixture_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let bucket = TimeBucket {
        start_epoch_s: 600,
        size_s: 600,
    };
    kernel.append_event_checked(
        &desc,
        CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: bucket,
            zone_id: "zone:a".to_string(),
            confidence: 0.9,
            correlation_token: None,
            attestation: None,
        },
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;

    // The forged grant: one approval with a filler signature, and the
    // commitment recomputed over it. Only the device key signs the row.
    let request = UnlockRequest::new("vault:forged", cfg.ruleset_hash, "forged", bucket)?;
    let approvals = [Approval::new(
        TrusteeId::new("nobody"),
        request.request_hash(),
        vec![0xAA; 64],
    )];
    let receipt = BreakGlassReceipt {
        vault_envelope_id: request.vault_envelope_id.clone(),
        request_hash: request.request_hash(),
        ruleset_hash: cfg.ruleset_hash,
        time_bucket: bucket,
        trustees_used: vec![TrusteeId::new("nobody")],
        approvals_commitment: approvals_commitment(&approvals),
        policy_commitment: [0u8; 32],
        outcome: BreakGlassOutcome::Granted,
        purpose: Some(request.purpose.clone()),
        context: None,
        request_bucket: Some(bucket),
    };
    kernel.log_break_glass_receipt(&receipt, &approvals)?;

    let envelope = kernel.build_evidence_envelope_for_api(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &cfg.ruleset_id,
        &cfg.kernel_version,
    )?;

    let path = out_dir.join("forged_grant_envelope.json");
    let json = serde_json::to_string_pretty(&envelope)?;
    fs::write(&path, json + "\n")?;
    println!("  {}", path.display());
    Ok(())
}
