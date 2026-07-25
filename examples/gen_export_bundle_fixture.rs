//! Regenerate the cross-language export-bundle fixture in
//! `tests/fixtures/export_bundle/`. The fixture is the exact artifact
//! `export_events --self-export` writes (an `ExportBundle`), and is consumed
//! by both the Rust verifier (`verify_export_bundle`, pinned in
//! `tests/export_bundle_fixtures.rs`) and the offline JS verifier
//! (`viewer/verify_core.js`, pinned in `viewer/verify_core.test.js`) to
//! guarantee byte-for-byte parity.
//!
//! Run with:  `cargo run --example gen_export_bundle_fixture`
//!
//! Deliberately separate from `gen_envelope_fixtures` so regenerating this
//! fixture never churns the committed envelope fixtures.
//!
//! NOTE: uses the default (non-PQ) build on purpose, so the browser/WebCrypto
//! verifier can fully check the fixture.

use std::fs;
use std::path::Path;
use std::time::Duration;

use witness_kernel::{
    CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
};

fn main() -> anyhow::Result<()> {
    let out_dir = Path::new("tests/fixtures/export_bundle");
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
    // Varied confidences, including an integer-valued 1.0, so the fixture
    // exercises the cross-language f32 formatting edge cases in the JS
    // artifact-binding check (same sample set as the envelope fixtures).
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

    // Owner self-export, jitter 0 so bucket times in the artifact stay the
    // exact appended values; the receipt itself is signed over whatever it
    // says, so the fixture verifies forever regardless of when it was cut.
    let bundle = kernel.export_events_bundle_self(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
    )?;

    let path = out_dir.join("valid_bundle.json");
    let json = serde_json::to_string_pretty(&bundle)?;
    fs::write(&path, json + "\n")?;
    println!("wrote {}", path.display());
    Ok(())
}
