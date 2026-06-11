//! Regenerate the cross-language evidence-envelope fixtures in
//! `tests/fixtures/envelope/`. These are consumed by both the Rust verifier
//! (`envelope_verify` / `verify_envelope`) and the offline JS verifier
//! (`viewer/verify_core.js`) to guarantee byte-for-byte parity.
//!
//! Run with:  `cargo run --example gen_envelope_fixtures`
//!
//! Fixtures produced (Ed25519-only so the browser/WebCrypto verifier can fully
//! check them — no PQ key is present):
//!   valid_envelope.json              a well-formed, fully verifiable envelope
//!   valid_envelope_self_export.json  envelope produced by owner self-export
//!   tampered_payload.json            a sealed-event payload mutated (chain break)
//!   tampered_digest.json             whole_envelope_digest corrupted
//!   domain_separation_vectors.json   inputs/outputs of domain_separated_hash
//!
//! `valid_envelope_legacy.json` is NOT regenerated: it pins the pre-`auth_mode`
//! receipt format (no auth_mode/window fields) so both verifiers keep accepting
//! bundles exported before those fields existed.
//!
//! NOTE: this example uses the default (non-PQ) build on purpose.

use std::fs;
use std::path::Path;
use std::time::Duration;

use witness_kernel::crypto::signatures::{
    domain_separated_hash, DOMAIN_BREAK_GLASS_RECEIPT, DOMAIN_CHECKPOINT, DOMAIN_EXPORT_RECEIPT,
    DOMAIN_SEALED_LOG_ENTRY,
};
use witness_kernel::{
    envelope, CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
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
    // Use varied confidences, including an integer-valued 1.0, so the fixtures exercise the
    // cross-language f32 formatting edge cases in the JS artifact-binding check.
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
        };
        kernel.append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
    }

    let envelope = kernel.build_evidence_envelope_for_api(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &cfg.ruleset_id,
        &cfg.kernel_version,
    )?;

    // valid
    write_json(out_dir.join("valid_envelope.json"), &envelope)?;

    // tampered payload: mutate a sealed-event payload and refresh the digest so the failure
    // surfaces at chain verification rather than the digest check.
    let mut tp = envelope.clone();
    tp.ledgers.sealed_events.entries[0].payload_json.push(' ');
    tp.whole_envelope_digest = envelope::compute_whole_envelope_digest(&tp)?;
    write_json(out_dir.join("tampered_payload.json"), &tp)?;

    // tampered digest: corrupt the fingerprint directly.
    let mut td = envelope.clone();
    td.whole_envelope_digest = "0".repeat(64);
    write_json(out_dir.join("tampered_digest.json"), &td)?;

    // owner self-export: same kernel, no quorum — exercises the `self_export`
    // auth_mode and a bucket-aligned disclosure window on the signed receipt.
    let self_envelope = kernel.build_evidence_envelope_self(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            window: Some(witness_kernel::ExportWindow {
                start_epoch_s: 600,
                end_epoch_s: 2400,
            }),
            ..ExportOptions::default()
        },
        &cfg.ruleset_id,
        &cfg.kernel_version,
    )?;
    write_json(
        out_dir.join("valid_envelope_self_export.json"),
        &self_envelope,
    )?;

    // domain separation vectors
    let domains = [
        ("sealed_log_entry", DOMAIN_SEALED_LOG_ENTRY),
        ("checkpoint", DOMAIN_CHECKPOINT),
        ("break_glass", DOMAIN_BREAK_GLASS_RECEIPT),
        ("export_receipt", DOMAIN_EXPORT_RECEIPT),
    ];
    let entry_hashes = [[0u8; 32], [0xffu8; 32], {
        let mut h = [0u8; 32];
        for (i, b) in h.iter_mut().enumerate() {
            *b = i as u8;
        }
        h
    }];
    let mut vectors = Vec::new();
    for (name, domain) in domains {
        for eh in &entry_hashes {
            let signing_hash = domain_separated_hash(domain, eh);
            vectors.push(serde_json::json!({
                "domain_name": name,
                "domain": domain,
                "entry_hash": hex::encode(eh),
                "signing_hash": hex::encode(signing_hash),
            }));
        }
    }
    let vectors_json = serde_json::json!({
        "description": "Inputs/outputs of domain_separated_hash = SHA256(le32(len(domain))||domain||entry_hash)",
        "vectors": vectors,
    });
    write_json(
        out_dir.join("domain_separation_vectors.json"),
        &vectors_json,
    )?;

    println!("wrote fixtures to {}", out_dir.display());
    Ok(())
}

fn write_json<T: serde::Serialize>(path: std::path::PathBuf, value: &T) -> anyhow::Result<()> {
    let json = serde_json::to_string_pretty(value)?;
    fs::write(&path, json + "\n")?;
    println!("  {}", path.display());
    Ok(())
}
