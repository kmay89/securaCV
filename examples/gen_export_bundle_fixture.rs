//! Regenerate the cross-language export-bundle fixtures in
//! `tests/fixtures/export_bundle/`. Each fixture is an exact `ExportBundle`
//! as a kernel writes it, consumed by both the Rust verifier
//! (`verify_export_bundle`, pinned in `tests/export_bundle_fixtures.rs`) and
//! the offline JS verifier (`viewer/verify_core.js`, pinned in
//! `viewer/verify_core.test.js`) to guarantee byte-for-byte parity:
//!
//! - `valid_bundle.json` — the owner self-export (`export_events
//!   --self-export`): receipt `auth_mode: self_export`, no window.
//! - `valid_bundle_paired.json` — `GET /export/bundle` under a paired bearer
//!   (the Home Assistant integration's credential): receipt
//!   `auth_mode: paired`, a disclosure window, and the trailing `token_id`.
//!   Every optional receipt field is populated, so a verifier that
//!   re-serializes the receipt field by field is held to the full order.
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
    ApiCredential, CandidateEvent, EventType, ExportOptions, ExportWindow, InferenceBackend,
    Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

/// The paired bearer's id the paired fixture names (8 hex, as `POST
/// /api/pair` mints them). A fixed value: the fixture is a vector, not a
/// credential.
const PAIRED_TOKEN_ID: &str = "0badf00d";

fn main() -> anyhow::Result<()> {
    let out_dir = Path::new("tests/fixtures/export_bundle");
    fs::create_dir_all(out_dir)?;

    // Owner self-export, jitter 0 so bucket times in the artifact stay the
    // exact appended values; the receipt itself is signed over whatever it
    // says, so the fixture verifies forever regardless of when it was cut.
    let (mut kernel, cfg) = seeded_kernel()?;
    let bundle = kernel.export_events_bundle_self(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            ..ExportOptions::default()
        },
    )?;
    write_fixture(&out_dir.join("valid_bundle.json"), &bundle)?;

    // The same three events exported by a paired bearer over a disclosure
    // window — a fresh kernel, so each fixture stands alone (its receipt is
    // the first row of its own ledger). The window covers every sample
    // bucket; `token_id` follows it on the wire.
    let (mut kernel, cfg) = seeded_kernel()?;
    let bundle = kernel.export_events_bundle_for_api_with(
        cfg.ruleset_hash,
        ExportOptions {
            jitter_s: 0,
            window: Some(ExportWindow::aligned(0, 2400)?),
            ..ExportOptions::default()
        },
        ApiCredential::Paired {
            token_id: PAIRED_TOKEN_ID.to_string(),
        },
    )?;
    write_fixture(&out_dir.join("valid_bundle_paired.json"), &bundle)?;
    Ok(())
}

fn write_fixture(path: &Path, bundle: &witness_kernel::ExportBundle) -> anyhow::Result<()> {
    let json = serde_json::to_string_pretty(bundle)?;
    fs::write(path, json + "\n")?;
    println!("wrote {}", path.display());
    Ok(())
}

/// An in-memory kernel with the three sample events sealed — the same
/// sample set as the envelope fixtures.
fn seeded_kernel() -> anyhow::Result<(Kernel, KernelConfig)> {
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

    Ok((kernel, cfg))
}
