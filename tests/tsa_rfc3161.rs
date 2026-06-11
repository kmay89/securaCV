//! RFC 3161 interop tests against OpenSSL-generated fixtures.
//!
//! Fixtures in tests/fixtures/tsa/ were produced by a throwaway local TSA
//! (see the README there for the exact commands): DER queries from
//! `openssl ts -query` and granted responses from `openssl ts -reply`,
//! all over sha256("securacv-fixture"). Parsing them proves the hand-rolled
//! DER code agrees with an independent implementation, not just with itself.

use sha2::{Digest, Sha256};
use witness_kernel::tsa;

fn fixture(name: &str) -> Vec<u8> {
    let path = format!("{}/tests/fixtures/tsa/{name}", env!("CARGO_MANIFEST_DIR"));
    std::fs::read(&path).unwrap_or_else(|e| panic!("reading {path}: {e}"))
}

fn fixture_digest() -> [u8; 32] {
    Sha256::digest(b"securacv-fixture").into()
}

#[test]
fn built_query_is_byte_identical_to_openssl() {
    let ours = tsa::build_request(&fixture_digest(), None, true);
    assert_eq!(ours, fixture("query_nononce.tsq"));
}

#[test]
fn parses_openssl_granted_response() {
    let token = tsa::parse_response(&fixture("reply.tsr")).unwrap();
    assert_eq!(token.status, 0);
    assert_eq!(token.imprint, fixture_digest());
    assert_eq!(token.gen_time, "20260610123324Z");
    assert_eq!(token.policy_oid, "1.3.6.1.4.1.13762.3");
    assert_eq!(token.serial_hex, "02");
    assert_eq!(token.nonce, None);
    tsa::verify_match(&token, &fixture_digest(), None).unwrap();

    // The captured token round-trips through the bare-token parser.
    let imprint = tsa::parse_token_imprint(&token.token_der).unwrap();
    assert_eq!(imprint, fixture_digest());
}

#[test]
fn nonce_is_extracted_and_checked() {
    let token = tsa::parse_response(&fixture("reply_nonce.tsr")).unwrap();
    let nonce = hex::decode("ac06d335ca6b0758").unwrap();
    assert_eq!(token.nonce.as_deref(), Some(nonce.as_slice()));
    tsa::verify_match(&token, &fixture_digest(), Some(&nonce)).unwrap();

    // A different nonce must be rejected as a possible replay.
    let wrong = hex::decode("0102030405060708").unwrap();
    let err = tsa::verify_match(&token, &fixture_digest(), Some(&wrong)).unwrap_err();
    assert!(err.to_string().contains("nonce mismatch"), "{err}");

    // As must a different digest.
    let err = tsa::verify_match(&token, &[0u8; 32], Some(&nonce)).unwrap_err();
    assert!(err.to_string().contains("imprint mismatch"), "{err}");
}

#[test]
fn anchor_rows_round_trip_and_track_chain_history() {
    let conn = rusqlite::Connection::open_in_memory().unwrap();
    tsa::ensure_anchor_table(&conn).unwrap();

    // Minimal chain tables so chain_head()/hash_in_history() have something
    // to query — same shapes as src/storage.rs.
    conn.execute_batch(
        "CREATE TABLE sealed_events (id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL, payload_json TEXT NOT NULL,
            prev_hash BLOB NOT NULL, entry_hash BLOB NOT NULL, signature BLOB NOT NULL);
         CREATE TABLE checkpoints (id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL, cutoff_event_id INTEGER NOT NULL,
            chain_head_hash BLOB NOT NULL, signature BLOB NOT NULL);",
    )
    .unwrap();

    // Empty log: nothing to anchor.
    assert!(tsa::chain_head(&conn).is_err());

    let head = fixture_digest();
    conn.execute(
        "INSERT INTO sealed_events (created_at, payload_json, prev_hash, entry_hash, signature)
         VALUES (0, '{}', ?1, ?2, x'00')",
        rusqlite::params![[0u8; 32].as_slice(), head.as_slice()],
    )
    .unwrap();
    assert_eq!(tsa::chain_head(&conn).unwrap(), head);
    assert!(tsa::hash_in_history(&conn, &head).unwrap());
    assert!(!tsa::hash_in_history(&conn, &[0u8; 32]).unwrap());

    let token = tsa::parse_response(&fixture("reply.tsr")).unwrap();
    let id = tsa::insert_anchor(&conn, "chain_head", &head, "https://example/tsr", &token).unwrap();
    let anchors = tsa::list_anchors(&conn).unwrap();
    assert_eq!(anchors.len(), 1);
    assert_eq!(anchors[0].id, id);
    assert_eq!(anchors[0].subject, "chain_head");
    assert_eq!(anchors[0].subject_hash, head);
    assert_eq!(anchors[0].gen_time, "20260610123324Z");
    assert_eq!(anchors[0].token_der, token.token_der);
    assert_eq!(anchors[0].created_bucket.size_s, 600);
}

/// Full cryptographic round trip: the token we store verifies under
/// `openssl ts -verify` against the fixture TSA's certificate — the same
/// independent check `log_anchor verify --ca` runs. Skipped when openssl
/// is not on PATH.
#[test]
fn stored_token_verifies_under_openssl() {
    if std::process::Command::new("openssl")
        .arg("version")
        .output()
        .is_err()
    {
        eprintln!("skipping: openssl not available");
        return;
    }
    let token = tsa::parse_response(&fixture("reply.tsr")).unwrap();
    let dir = tempfile::tempdir().unwrap();
    let token_path = dir.path().join("anchor.der");
    std::fs::write(&token_path, &token.token_der).unwrap();
    let ca_path = format!("{}/tests/fixtures/tsa/tsa.crt", env!("CARGO_MANIFEST_DIR"));
    let output = std::process::Command::new("openssl")
        .args([
            "ts",
            "-verify",
            "-digest",
            &hex::encode(fixture_digest()),
            "-in",
            token_path.to_str().unwrap(),
            "-token_in",
            "-CAfile",
            &ca_path,
        ])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "openssl ts -verify failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}
