//! Pin the committed export-bundle fixture to the Rust verifier. The same
//! fixture is checked by the offline JS verifier
//! (`viewer/verify_core.test.js`), guaranteeing that the Rust and JavaScript
//! export-bundle verifiers agree byte-for-byte. Regenerate with:
//!   cargo run --example gen_export_bundle_fixture

use std::fs;
use std::path::Path;

use witness_kernel::{verify_export_bundle, ExportAuthMode, ExportBundle};

fn load() -> ExportBundle {
    let path = Path::new("tests/fixtures/export_bundle/valid_bundle.json");
    let bytes = fs::read(path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()));
    serde_json::from_slice(&bytes).unwrap_or_else(|e| panic!("parse {}: {e}", path.display()))
}

#[test]
fn valid_bundle_fixture_verifies() {
    let bundle = load();
    verify_export_bundle(&bundle).expect("valid fixture must verify");
    assert_eq!(
        bundle.receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::SelfExport)
    );
    let events: usize = bundle
        .artifact
        .batches
        .iter()
        .flat_map(|b| &b.buckets)
        .map(|bk| bk.events.len())
        .sum();
    assert_eq!(events, 3);
}

#[test]
fn tampered_artifact_is_rejected() {
    let mut bundle = load();
    bundle.artifact.batches[0].buckets[0].events[0].zone_id = "zone:forged".to_string();
    let err = verify_export_bundle(&bundle).expect_err("tampered artifact must fail");
    assert!(
        err.to_string().contains("artifact hash mismatch"),
        "got: {err}"
    );
}

#[test]
fn tampered_receipt_is_rejected() {
    let mut bundle = load();
    bundle.receipt_entry.receipt.batch_size += 1;
    let err = verify_export_bundle(&bundle).expect_err("tampered receipt must fail");
    assert!(
        err.to_string().contains("entry hash mismatch"),
        "got: {err}"
    );
}

#[test]
fn swapped_device_key_is_rejected() {
    let mut bundle = load();
    bundle.device_public_key[0] ^= 0x01;
    // A different (even invalid-point) key must never verify the signature.
    assert!(verify_export_bundle(&bundle).is_err());
}
