//! Pin the committed export-bundle fixtures to the Rust verifier. The same
//! fixtures are checked by the offline JS verifier
//! (`viewer/verify_core.test.js`), guaranteeing that the Rust and JavaScript
//! export-bundle verifiers agree byte-for-byte. Regenerate with:
//!   cargo run --example gen_export_bundle_fixture

use std::fs;
use std::path::Path;

use witness_kernel::{verify_export_bundle, ExportAuthMode, ExportBundle};

fn load_named(name: &str) -> ExportBundle {
    let path = Path::new("tests/fixtures/export_bundle").join(name);
    let bytes = fs::read(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()));
    serde_json::from_slice(&bytes).unwrap_or_else(|e| panic!("parse {}: {e}", path.display()))
}

fn load() -> ExportBundle {
    load_named("valid_bundle.json")
}

/// The bundle a paired bearer (the Home Assistant integration's credential)
/// downloads from `GET /export/bundle`: the receipt names the credential
/// class AND the bearer (`auth_mode: paired`, `token_id`), inside the signed
/// bytes, after the disclosure window.
fn load_paired() -> ExportBundle {
    load_named("valid_bundle_paired.json")
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

#[test]
fn paired_bundle_fixture_verifies_and_names_its_bearer() {
    let bundle = load_paired();
    verify_export_bundle(&bundle).expect("valid paired fixture must verify");
    let receipt = &bundle.receipt_entry.receipt;
    assert_eq!(receipt.auth_mode, Some(ExportAuthMode::Paired));
    assert_eq!(receipt.token_id.as_deref(), Some("0badf00d"));
    let window = receipt.window.expect("the paired export was windowed");
    assert_eq!((window.start_epoch_s, window.end_epoch_s), (0, 2400));
    let events: usize = bundle
        .artifact
        .batches
        .iter()
        .flat_map(|b| &b.buckets)
        .map(|bk| bk.events.len())
        .sum();
    assert_eq!(events, 3);

    // The wire form the offline verifiers re-serialize: `token_id` is the
    // LAST receipt field, after `auth_mode` and `window`.
    let json = serde_json::to_string(receipt).expect("serialize receipt");
    assert!(
        json.ends_with(r#""token_id":"0badf00d"}"#),
        "token_id must be the trailing receipt field: {json}"
    );
    let auth_pos = json.find(r#""auth_mode":"paired""#).expect("auth_mode");
    let window_pos = json.find(r#""window":"#).expect("window");
    let token_pos = json.find(r#""token_id":"#).expect("token_id");
    assert!(auth_pos < window_pos && window_pos < token_pos, "{json}");
}

#[test]
fn paired_bundle_with_an_edited_bearer_is_rejected() {
    // Which bearer exported is signed: re-attributing the disclosure to
    // another token id, or scrubbing the id, breaks the receipt entry hash.
    let mut bundle = load_paired();
    bundle.receipt_entry.receipt.token_id = Some("deadbeef".to_string());
    let err = verify_export_bundle(&bundle).expect_err("edited token_id must fail");
    assert!(
        err.to_string().contains("entry hash mismatch"),
        "got: {err}"
    );

    let mut bundle = load_paired();
    bundle.receipt_entry.receipt.token_id = None;
    let err = verify_export_bundle(&bundle).expect_err("scrubbed token_id must fail");
    assert!(
        err.to_string().contains("entry hash mismatch"),
        "got: {err}"
    );
}
