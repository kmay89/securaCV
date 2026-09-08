//! RFC 3161 interop tests against OpenSSL-generated fixtures.
//!
//! Fixtures in tests/fixtures/tsa/ were produced by a throwaway local TSA
//! (see the README there for the exact commands): DER queries from
//! `openssl ts -query` and granted responses from `openssl ts -reply`,
//! all over sha256("securacv-fixture"). Parsing them proves the hand-rolled
//! DER code agrees with an independent implementation, not just with itself.

use sha2::{Digest, Sha256};
use witness_kernel::tsa::{self, AnchorSubject, HashLocation, HistoryWitness};

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
    // Identity is cached from the token; the ledger position from the chain.
    assert_eq!(
        anchors[0].signer_fingerprint.as_deref(),
        Some("fbf1c838f80923a01badb6030b9d708c5ef1a65b7d23f1f53a6aa274d1b99542")
    );
    assert_eq!(
        anchors[0].signer_sid.as_deref(),
        Some("d806acd919f81e613c74e1b025e3aab8d47502181d30a00bf761bf67aa7ce3ef")
    );
    assert_eq!(anchors[0].ledger_id, Some(1));
    assert_eq!(anchors[0].tsa_name, None);
    assert_eq!(anchors[0].subject_kind(), Some(AnchorSubject::ChainHead));

    // The other ledgers, hand-made with the shapes from src/lib.rs.
    conn.execute_batch(
        "CREATE TABLE export_receipts (id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL, payload_json TEXT NOT NULL,
            prev_hash BLOB NOT NULL, entry_hash BLOB NOT NULL, signature BLOB NOT NULL);
         CREATE TABLE break_glass_receipts (id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL, payload_json TEXT NOT NULL,
            approvals_json TEXT NOT NULL DEFAULT '[]',
            prev_hash BLOB NOT NULL, entry_hash BLOB NOT NULL, signature BLOB NOT NULL);
         CREATE TABLE policy_change_history (id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL, payload_json TEXT NOT NULL,
            approvals_json TEXT NOT NULL DEFAULT '[]',
            prev_hash BLOB NOT NULL, entry_hash BLOB NOT NULL, signature BLOB NOT NULL,
            pq_signature BLOB, pq_scheme TEXT);",
    )
    .unwrap();
    // Empty ledgers: no head, nothing classified.
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::ExportReceiptHead).unwrap(),
        None
    );
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::BreakGlassReceiptHead).unwrap(),
        None
    );
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::PolicyHead).unwrap(),
        None
    );
    assert!(tsa::ledger_head(&conn, AnchorSubject::Digest).is_err());

    let export_h = [0xe1u8; 32];
    let bg_h = [0xb2u8; 32];
    let policy_h = [0xc3u8; 32];
    conn.execute(
        "INSERT INTO export_receipts (created_at, payload_json, prev_hash, entry_hash, signature)
         VALUES (0, '{}', ?1, ?2, x'00')",
        rusqlite::params![[0u8; 32].as_slice(), export_h.as_slice()],
    )
    .unwrap();
    conn.execute(
        "INSERT INTO break_glass_receipts (created_at, payload_json, prev_hash, entry_hash, signature)
         VALUES (0, '{}', ?1, ?2, x'00')",
        rusqlite::params![[0u8; 32].as_slice(), bg_h.as_slice()],
    )
    .unwrap();
    conn.execute(
        "INSERT INTO policy_change_history (created_at, payload_json, prev_hash, entry_hash, signature)
         VALUES (0, '{}', ?1, ?2, x'00')",
        rusqlite::params![[0u8; 32].as_slice(), policy_h.as_slice()],
    )
    .unwrap();

    // classify_hash names the ledger each hash lives in.
    let loc = |h: &[u8; 32]| tsa::classify_hash(&conn, h).unwrap();
    assert_eq!(
        loc(&head),
        Some(HashLocation {
            subject: AnchorSubject::ChainHead,
            witness: HistoryWitness::LiveRow { id: 1 }
        })
    );
    assert_eq!(
        loc(&export_h),
        Some(HashLocation {
            subject: AnchorSubject::ExportReceiptHead,
            witness: HistoryWitness::LiveRow { id: 1 }
        })
    );
    assert_eq!(
        loc(&bg_h),
        Some(HashLocation {
            subject: AnchorSubject::BreakGlassReceiptHead,
            witness: HistoryWitness::LiveRow { id: 1 }
        })
    );
    assert_eq!(
        loc(&policy_h),
        Some(HashLocation {
            subject: AnchorSubject::PolicyHead,
            witness: HistoryWitness::LiveRow { id: 1 }
        })
    );
    assert_eq!(loc(&[0u8; 32]), None);
    for kind in AnchorSubject::HEADS {
        let sentinel = kind.empty_ledger_sentinel().unwrap();
        assert_eq!(loc(&sentinel), None, "sentinels are in no ledger");
    }
    // Membership is checked in the DECLARED ledger only.
    assert_eq!(
        tsa::hash_in_ledger(&conn, &export_h, AnchorSubject::ChainHead).unwrap(),
        None
    );
    assert_eq!(
        tsa::hash_in_ledger(&conn, &head, AnchorSubject::ExportReceiptHead).unwrap(),
        None
    );
    assert_eq!(
        tsa::hash_in_ledger(&conn, &export_h, AnchorSubject::Digest).unwrap(),
        None
    );
    // Heads and positions per ledger.
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::ChainHead).unwrap(),
        Some(head)
    );
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::ExportReceiptHead).unwrap(),
        Some(export_h)
    );
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::BreakGlassReceiptHead).unwrap(),
        Some(bg_h)
    );
    assert_eq!(
        tsa::ledger_head(&conn, AnchorSubject::PolicyHead).unwrap(),
        Some(policy_h)
    );
    assert_eq!(
        tsa::ledger_position(&conn, AnchorSubject::PolicyHead, &policy_h).unwrap(),
        Some(1)
    );
    assert_eq!(
        tsa::ledger_position(&conn, AnchorSubject::PolicyHead, &export_h).unwrap(),
        None
    );
    // A checkpoint head is history but has no live position.
    let cp_head = [0xccu8; 32];
    conn.execute(
        "INSERT INTO checkpoints (created_at, cutoff_event_id, chain_head_hash, signature)
         VALUES (0, 0, ?1, x'00')",
        rusqlite::params![cp_head.as_slice()],
    )
    .unwrap();
    assert_eq!(
        tsa::hash_in_ledger(&conn, &cp_head, AnchorSubject::ChainHead).unwrap(),
        Some(HistoryWitness::CheckpointHead { checkpoint_id: 1 })
    );
    assert_eq!(
        tsa::ledger_position(&conn, AnchorSubject::ChainHead, &cp_head).unwrap(),
        None
    );
    // A typed insert records the declared name and the ledger position.
    let id2 = tsa::insert_anchor_declared(
        &conn,
        AnchorSubject::ExportReceiptHead,
        &export_h,
        "https://example/tsr",
        Some("alpha"),
        &token,
    )
    .unwrap();
    let rows = tsa::list_anchors(&conn).unwrap();
    let row = rows.iter().find(|r| r.id == id2).unwrap();
    assert_eq!(row.subject, "export_receipt_head");
    assert_eq!(row.tsa_name.as_deref(), Some("alpha"));
    assert_eq!(row.ledger_id, Some(1));
}

/// A table written by an older build (the frozen 8-column DDL) lists from a
/// READ-ONLY connection — with `None` for the newer columns and no migration
/// — and migrates in place, idempotently, when a writer runs
/// `ensure_anchor_table`.
#[test]
fn legacy_anchor_table_lists_read_only_and_migrates() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("legacy.db");
    let conn = rusqlite::Connection::open(&path).unwrap();
    conn.execute_batch(
        "CREATE TABLE tsa_anchors (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          created_bucket_start INTEGER NOT NULL,
          created_bucket_size INTEGER NOT NULL,
          subject TEXT NOT NULL,
          subject_hash BLOB NOT NULL,
          tsa_url TEXT NOT NULL,
          gen_time TEXT NOT NULL,
          token_der BLOB NOT NULL
        );",
    )
    .unwrap();
    let token = tsa::parse_response(&fixture("reply.tsr")).unwrap();
    conn.execute(
        "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject, subject_hash,
            tsa_url, gen_time, token_der) VALUES (600, 600, 'chain_head', ?1, '(offline)',
            '20260610123324Z', ?2)",
        rusqlite::params![fixture_digest().as_slice(), token.token_der],
    )
    .unwrap();
    drop(conn);

    let ro = rusqlite::Connection::open_with_flags(
        &path,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .unwrap();
    assert!(tsa::anchor_table_exists(&ro).unwrap());
    let rows = tsa::list_anchors(&ro).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].subject, "chain_head");
    assert_eq!(rows[0].tsa_name, None);
    assert_eq!(rows[0].signer_fingerprint, None);
    assert_eq!(rows[0].signer_sid, None);
    assert_eq!(rows[0].ledger_id, None);
    let cols: i64 = ro
        .query_row(
            "SELECT count(*) FROM pragma_table_info('tsa_anchors')",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(cols, 8, "a read-only reader never migrates");
    drop(ro);

    let rw = rusqlite::Connection::open(&path).unwrap();
    tsa::ensure_anchor_table(&rw).unwrap();
    tsa::ensure_anchor_table(&rw).unwrap(); // idempotent
    let cols: i64 = rw
        .query_row(
            "SELECT count(*) FROM pragma_table_info('tsa_anchors')",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(cols, 12);
    let rows = tsa::list_anchors(&rw).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].ledger_id, None);
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
