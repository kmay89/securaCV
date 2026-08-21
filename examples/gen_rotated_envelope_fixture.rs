//! Generate ONLY `tests/fixtures/envelope/valid_envelope_rotated.json`: an
//! envelope from a device that rotated its signing key mid-history, so the
//! sealed-events ledger mixes signers and carries the KeyRotation record that
//! proves the lineage. Consumed by both verifiers (Rust `envelope_fixtures`,
//! JS `viewer/verify_core.test.js`) to pin rotation-aware verification
//! cross-language.
//!
//! Deliberately separate from `gen_envelope_fixtures` — regenerating that
//! example rewrites every fixture (receipt timestamps churn), and the other
//! fixtures don't need to move to add this one.
//!
//! Run with:  `cargo run --example gen_rotated_envelope_fixture`

use std::fs;
use std::path::Path;
use std::time::Duration;

use witness_kernel::{
    CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig, TimeBucket,
    ZonePolicy,
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

    let desc = ModuleDescriptorHolder::descriptor();
    let seal = |kernel: &mut Kernel, zone: &str, bucket_start: u64| -> anyhow::Result<()> {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: bucket_start,
                size_s: 600,
            },
            zone_id: zone.to_string(),
            confidence: 0.9,
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
    };

    seal(&mut kernel, "zone:pre", 600)?;
    kernel.rotate_device_identity("devkey:fixture:rotated:aabbccddeeff0011")?;
    seal(&mut kernel, "zone:post", 1200)?;

    let envelope = kernel.build_evidence_envelope_for_api(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
        &cfg.ruleset_id,
        &cfg.kernel_version,
    )?;

    let path = out_dir.join("valid_envelope_rotated.json");
    let json = serde_json::to_string_pretty(&envelope)?;
    fs::write(&path, json + "\n")?;
    println!("  {}", path.display());
    Ok(())
}

struct ModuleDescriptorHolder;
impl ModuleDescriptorHolder {
    fn descriptor() -> witness_kernel::ModuleDescriptor {
        witness_kernel::ModuleDescriptor {
            id: "fixture_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        }
    }
}
