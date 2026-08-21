//! Pin the committed evidence-envelope fixtures to the Rust verifier. The same fixtures are
//! checked by the offline JS verifier (`viewer/verify_core.test.js`), guaranteeing that the
//! Rust and JavaScript verifiers agree byte-for-byte. Regenerate with:
//!   cargo run --example gen_envelope_fixtures

use std::fs;
use std::path::Path;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::verify::{FailedLedger, FailureKind, VerifyFailure};
use witness_kernel::{verify_envelope, EvidenceEnvelope, ExportAuthMode};

fn load(name: &str) -> EvidenceEnvelope {
    let path = Path::new("tests/fixtures/envelope").join(name);
    let bytes = fs::read(&path).unwrap_or_else(|e| panic!("read {}: {e}", path.display()));
    serde_json::from_slice(&bytes).unwrap_or_else(|e| panic!("parse {}: {e}", path.display()))
}

#[test]
fn valid_fixture_verifies() {
    let envelope = load("valid_envelope.json");
    let report =
        verify_envelope(&envelope, SignatureMode::Compat).expect("valid fixture must verify");
    assert_eq!(report.sealed_events, 3);
    assert_eq!(report.export_receipts, 1);
    assert_eq!(
        envelope.export_receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::Api)
    );
}

/// Pins rotation-aware verification cross-language: the ledgers mix signers
/// (a device-key rotation mid-history) and the lineage is proven by the
/// envelope's own key_rotation record. Regenerate with:
///   cargo run --example gen_rotated_envelope_fixture
#[test]
fn rotated_fixture_verifies() {
    let envelope = load("valid_envelope_rotated.json");
    let report =
        verify_envelope(&envelope, SignatureMode::Compat).expect("rotated fixture must verify");
    assert_eq!(
        report.sealed_events, 3,
        "pre-event, rotation record, post-event"
    );
    assert_eq!(report.export_receipts, 1);
}

/// Pins the pre-`auth_mode` receipt format: bundles exported before the field
/// existed must keep verifying forever. This fixture is never regenerated.
#[test]
fn legacy_fixture_without_auth_mode_verifies() {
    let envelope = load("valid_envelope_legacy.json");
    let report =
        verify_envelope(&envelope, SignatureMode::Compat).expect("legacy fixture must verify");
    assert_eq!(report.sealed_events, 3);
    assert_eq!(report.export_receipts, 1);
    assert_eq!(envelope.export_receipt_entry.receipt.auth_mode, None);
    assert_eq!(envelope.export_receipt_entry.receipt.window, None);
}

#[test]
fn self_export_fixture_verifies() {
    let envelope = load("valid_envelope_self_export.json");
    let report =
        verify_envelope(&envelope, SignatureMode::Compat).expect("self-export fixture must verify");
    assert_eq!(report.sealed_events, 3);
    // The same kernel produced an API export first, so the receipts ledger
    // carries both generations chained together.
    assert_eq!(report.export_receipts, 2);
    assert_eq!(
        envelope.export_receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::SelfExport)
    );
    let window = envelope
        .export_receipt_entry
        .receipt
        .window
        .expect("self-export fixture records its disclosure window");
    assert_eq!(window.start_epoch_s, 600);
    assert_eq!(window.end_epoch_s, 2400);
}

#[test]
fn tampered_payload_fixture_is_rejected() {
    let envelope = load("tampered_payload.json");
    let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
    assert!(
        format!("{err}").contains("sealed_events"),
        "unexpected error: {err}"
    );
    // Structured-failure parity pin: the JS verifier asserts the identical
    // {ledger, entry_id, kind} triple for this fixture (verify_core.test.js).
    let failure = err
        .downcast_ref::<VerifyFailure>()
        .expect("chain failure carries a structured VerifyFailure");
    assert_eq!(failure.ledger, FailedLedger::SealedEvents);
    assert_eq!(failure.entry_id, Some(0));
    assert_eq!(failure.kind, FailureKind::EntryHashMismatch);
}

#[test]
fn tampered_digest_fixture_is_rejected() {
    let envelope = load("tampered_digest.json");
    let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
    assert!(
        format!("{err}").contains("whole_envelope_digest"),
        "unexpected error: {err}"
    );
}
