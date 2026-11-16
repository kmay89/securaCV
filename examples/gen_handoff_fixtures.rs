//! Regenerate the handoff-sidecar-era cross-language fixtures — and ONLY
//! these three files, so the committed export-bundle and envelope fixtures
//! never churn:
//!
//! - `tests/fixtures/export_bundle/valid_bundle_self_context.json` — an owner
//!   self-export whose receipt carries a declared `context` (the exporting
//!   operator and a salted recipient commitment).
//! - `tests/fixtures/export_bundle/valid_bundle_quorum.json` — a quorum
//!   export: `auth_mode: break_glass`, a window, `context` with only a
//!   recipient commitment, and the trailing `authorizing_receipt` naming the
//!   `Granted` receipt whose token it spent.
//! - `tests/fixtures/envelope/valid_envelope_unsealed.json` — an evidence
//!   envelope whose break-glass ledger holds a grant, its redemption and the
//!   device-signed `Unsealed` row (with a recipient commitment), then a
//!   second grant and redemption for `export:events`, and whose export ledger
//!   ends in the quorum export receipt carrying `authorizing_receipt`.
//!
//! Every fixture is consumed by both the Rust verifiers
//! (`tests/export_bundle_fixtures.rs`, `tests/envelope_fixtures.rs`) and the
//! offline JS verifier (`viewer/verify_core.test.js`).
//!
//! Run with:  `cargo run --example gen_handoff_fixtures`
//!
//! The salts are the pinned test vectors (`22`×32 for the plaintext, `11`×32
//! for the recipient), never production salts. Uses the default (non-PQ)
//! build on purpose, so the browser/WebCrypto verifier can fully check the
//! fixtures.

use std::fs;
use std::path::Path;
use std::time::Duration;

use ed25519_dalek::SigningKey;
use witness_kernel::break_glass::{
    Approval, BreakGlass, OperatorContext, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
use witness_kernel::handoff::{self, HandoffRecipient};
use witness_kernel::{
    CandidateEvent, EventType, ExportContext, ExportOptions, ExportWindow, InferenceBackend,
    Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

fn main() -> anyhow::Result<()> {
    let bundle_dir = Path::new("tests/fixtures/export_bundle");
    let envelope_dir = Path::new("tests/fixtures/envelope");
    fs::create_dir_all(bundle_dir)?;
    fs::create_dir_all(envelope_dir)?;

    let recipient =
        HandoffRecipient::new("Dana Reyes", Some("Example County Records Office"), None)?;
    let recipient_commitment = handoff::recipient_commitment(&[0x11; 32], &recipient);

    // Self-export with a declared operator context and a recipient.
    let (mut kernel, cfg, _) = seeded_kernel()?;
    let operator = OperatorContext::new("Dana Owner", "owner-recovery", Some("HOA-114"), None)?;
    let bundle = kernel.export_events_bundle_self_with(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        ExportContext::new(Some(operator), Some(recipient_commitment))?,
    )?;
    write_json(&bundle_dir.join("valid_bundle_self_context.json"), &bundle)?;

    // The unseal + quorum-export kernel.
    let (mut kernel, cfg, alice) = seeded_kernel()?;
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: alice.verifying_key().to_bytes(),
        }],
    )?;
    kernel.set_break_glass_policy(&policy)?;

    // 1. Grant, redemption and the device-signed unseal record.
    let unseal_token = grant(&mut kernel, &cfg, &policy, &alice, "vault:1")?;
    let redemption = kernel.redeem_token_durably(&unseal_token)?;
    let plaintext_commitment = handoff::plaintext_commitment(&[0x22; 32], b"securacv-fixture");
    kernel.record_unsealed(redemption, plaintext_commitment, Some(recipient_commitment))?;

    // 2. A second grant for export:events, spent by a quorum export that
    //    names it and commits to the recipient.
    let mut export_token = grant(
        &mut kernel,
        &cfg,
        &policy,
        &alice,
        witness_kernel::EXPORT_EVENTS_ENVELOPE_ID,
    )?;
    let bundle = kernel.export_events_bundle_authorized_with(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            window: Some(ExportWindow::aligned(0, 2400)?),
            ..ExportOptions::default()
        },
        &mut export_token,
        Some(recipient_commitment),
    )?;
    write_json(&bundle_dir.join("valid_bundle_quorum.json"), &bundle)?;

    let envelope = kernel.evidence_envelope_from_bundle(
        bundle,
        &cfg.ruleset_id,
        &cfg.kernel_version,
        cfg.ruleset_hash,
    )?;
    write_json(
        &envelope_dir.join("valid_envelope_unsealed.json"),
        &envelope,
    )?;
    Ok(())
}

/// A 1-of-1 grant for `envelope`, logged and with its token device-signed.
fn grant(
    kernel: &mut Kernel,
    cfg: &KernelConfig,
    policy: &QuorumPolicy,
    alice: &SigningKey,
    envelope: &str,
) -> anyhow::Result<witness_kernel::break_glass::BreakGlassToken> {
    let bucket = TimeBucket::now(600)?;
    let request = UnlockRequest::new(envelope, cfg.ruleset_hash, "incident", bucket)?;
    let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), alice);
    let (result, receipt) =
        BreakGlass::authorize(policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result?;
    let granted = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, granted)?;
    Ok(token)
}

fn write_json<T: serde::Serialize>(path: &Path, value: &T) -> anyhow::Result<()> {
    let json = serde_json::to_string_pretty(value)?;
    fs::write(path, json + "\n")?;
    println!("wrote {}", path.display());
    Ok(())
}

/// An in-memory kernel with the three sample events sealed — the same
/// sample set as the other fixtures — and the fixture trustee key.
fn seeded_kernel() -> anyhow::Result<(Kernel, KernelConfig, SigningKey)> {
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
    let samples = [("zone:a", 1.0f32), ("zone:b", 0.85), ("zone:a", 0.8734212)];
    for (i, (zone, confidence)) in samples.iter().enumerate() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 600 * (i as u64 + 1),
                size_s: 600,
            },
            zone_id: zone.to_string(),
            confidence: *confidence,
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
    }
    Ok((kernel, cfg, SigningKey::from_bytes(&[41u8; 32])))
}
