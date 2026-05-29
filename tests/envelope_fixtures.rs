//! Pin the committed evidence-envelope fixtures to the Rust verifier. The same fixtures are
//! checked by the offline JS verifier (`viewer/verify_core.test.js`), guaranteeing that the
//! Rust and JavaScript verifiers agree byte-for-byte. Regenerate with:
//!   cargo run --example gen_envelope_fixtures

use std::fs;
use std::path::Path;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{verify_envelope, EvidenceEnvelope};

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
}

#[test]
fn tampered_payload_fixture_is_rejected() {
    let envelope = load("tampered_payload.json");
    let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
    assert!(
        format!("{err}").contains("sealed_events"),
        "unexpected error: {err}"
    );
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
