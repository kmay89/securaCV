//! `log_verify` with RFC 3161 anchors folded into the verdict (structural):
//! the `anchors:` line and `anchors_checked` field, the `=== Anchor Audit ===`
//! block for damaged rows, `--strict` counting timeline and anchor warnings
//! apart, a rolled-back anchored head as a structured failure, and a
//! database with no anchors table reading exactly as before. Every test is
//! offline: tokens are the committed fixture byte-spliced over the hash
//! under test.

#[path = "common/mod.rs"]
mod common;

use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use witness_kernel::{
    tsa, tsa::AnchorSubject, CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:log_verify_anchors:0011223344556677";

fn test_cfg(db_path: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60 * 60),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn add_test_event(kernel: &mut Kernel, cfg: &KernelConfig) {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        },
        zone_id: "zone:test".to_string(),
        confidence: 0.5,
        correlation_token: None,
        attestation: None,
    };
    kernel
        .append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("seal event");
}

fn event_hash(kernel: &Kernel, id: i64) -> [u8; 32] {
    let bytes: Vec<u8> = kernel
        .conn
        .query_row(
            "SELECT entry_hash FROM sealed_events WHERE id = ?1",
            [id],
            |r| r.get(0),
        )
        .unwrap();
    bytes.try_into().unwrap()
}

fn anchor(kernel: &Kernel, subject: AnchorSubject, hash: &[u8; 32]) -> i64 {
    tsa::ensure_anchor_table(&kernel.conn).unwrap();
    tsa::insert_anchor_declared(
        &kernel.conn,
        subject,
        hash,
        "(offline)",
        None,
        &common::spliced_token(hash),
    )
    .unwrap()
}

/// A damaged row: a 31-byte subject hash. Returns its id.
fn malformed_anchor(kernel: &Kernel) -> i64 {
    tsa::ensure_anchor_table(&kernel.conn).unwrap();
    kernel
        .conn
        .execute(
            "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
                subject_hash, tsa_url, gen_time, token_der)
             VALUES (0, 600, 'chain_head', ?1, '(offline)', '20260610123324Z', x'00')",
            [vec![0u8; 31]],
        )
        .unwrap();
    kernel.conn.last_insert_rowid()
}

/// Two sealed events in a fresh database; the open kernel for the caller.
fn make_db(dir: &Path) -> (PathBuf, Kernel) {
    let db_path = dir.join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg).expect("open kernel");
    add_test_event(&mut kernel, &cfg);
    add_test_event(&mut kernel, &cfg);
    (db_path, kernel)
}

fn log_verify(db: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", &db.to_string_lossy()])
        .args(args)
        .env("DEVICE_KEY_SEED", SEED)
        .output()
        .expect("log_verify runs")
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn json(out: &Output) -> serde_json::Value {
    serde_json::from_str(&stdout(out)).unwrap_or_else(|e| panic!("{e}\n{}", stdout(out)))
}

fn has_64_hex(s: &str) -> bool {
    let mut run = 0usize;
    for b in s.bytes() {
        if b.is_ascii_hexdigit() {
            run += 1;
            if run >= 64 {
                return true;
            }
        } else {
            run = 0;
        }
    }
    false
}

#[test]
fn anchors_line_and_json_field_count_the_structural_checks() {
    let temp = tempfile::tempdir().unwrap();
    let (db, kernel) = make_db(temp.path());
    let head = event_hash(&kernel, 2);
    anchor(&kernel, AnchorSubject::ChainHead, &head);
    anchor(&kernel, AnchorSubject::Digest, &[9u8; 32]);
    drop(kernel);

    let out = log_verify(&db, &[]);
    assert!(out.status.success(), "{}\n{}", stdout(&out), stderr(&out));
    let text = stdout(&out);
    assert!(
        text.contains(&format!(
            "anchors: 2 checked (structural; countersignatures: log_anchor --db {} verify --ca \
             <tsa-ca.pem>)",
            db.display()
        )),
        "{text}"
    );
    assert!(!text.contains("=== Anchor Audit ==="), "{text}");
    // DEVICE_KEY_SEED derives the verifying key out of band → identity-bound.
    assert!(
        text.contains("OK: all chains verified — VALID (identity anchored out-of-band)."),
        "{text}"
    );

    let out = log_verify(&db, &["--json"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let report = json(&out);
    assert_eq!(report["anchors_checked"], 2);
    assert_eq!(report["chain_valid"], true);
    assert!(report.get("warnings").is_none(), "{report}");
}

#[test]
fn damaged_rows_print_under_anchor_audit_and_strict_counts_them_apart() {
    let temp = tempfile::tempdir().unwrap();
    let (db, kernel) = make_db(temp.path());
    let head = event_hash(&kernel, 2);
    anchor(&kernel, AnchorSubject::ChainHead, &head);
    let bad1 = malformed_anchor(&kernel);
    let bad2 = malformed_anchor(&kernel);
    drop(kernel);

    let out = log_verify(&db, &[]);
    assert!(out.status.success(), "{}\n{}", stdout(&out), stderr(&out));
    let text = stdout(&out);
    assert!(text.contains("anchors: 1 checked"), "{text}");
    assert!(text.contains("=== Anchor Audit ==="), "{text}");
    assert!(
        text.contains(
            "The ledgers verified and no anchored head is missing; these anchor rows cannot \
             vouch for anything:"
        ),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "WARNING: anchor #{bad1}: row is malformed (subject_hash size); run log_anchor verify"
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!("WARNING: anchor #{bad2}:")),
        "{text}"
    );
    assert!(
        text.contains("hint: this anchor row cannot vouch for anything"),
        "{text}"
    );
    // An anchor-only warning set opens no timeline block.
    assert!(!text.contains("=== Timeline Audit ==="), "{text}");
    assert!(text.contains("OK: all chains verified"), "{text}");

    let out = log_verify(&db, &["--strict"]);
    assert_eq!(
        out.status.code(),
        Some(2),
        "{}\n{}",
        stdout(&out),
        stderr(&out)
    );
    assert!(
        stderr(&out).contains(
            "verification passed but --strict treats 2 warning(s) as failure (0 timeline, 2 \
             anchor)"
        ),
        "{}",
        stderr(&out)
    );

    let out = log_verify(&db, &["--json", "--strict"]);
    assert_eq!(out.status.code(), Some(2), "{}", stderr(&out));
    let report = json(&out);
    assert_eq!(report["chain_valid"], true);
    assert_eq!(report["anchors_checked"], 1);
    let warnings = report["warnings"].as_array().expect("warnings");
    assert_eq!(warnings.len(), 2);
    for w in warnings {
        let w = w.as_str().unwrap();
        assert!(w.starts_with("anchor #"), "{w}");
        assert!(!has_64_hex(w), "{w}");
    }
}

#[test]
fn rolled_back_anchored_head_fails_the_verdict() {
    let temp = tempfile::tempdir().unwrap();
    let (db, kernel) = make_db(temp.path());
    let head = event_hash(&kernel, 2);
    let anchor_id = anchor(&kernel, AnchorSubject::ChainHead, &head);
    // Rollback: the anchored newest event is gone; the shorter chain still
    // hash-verifies, so only the anchor can see it.
    kernel
        .conn
        .execute("DELETE FROM sealed_events WHERE id = 2", [])
        .unwrap();
    drop(kernel);

    let out = log_verify(&db, &["--json"]);
    assert_eq!(out.status.code(), Some(1), "{}", stderr(&out));
    let report = json(&out);
    assert_eq!(report["chain_valid"], false);
    assert_eq!(report["verdict"], "verification failed");
    assert_eq!(report["anchors_checked"], 0);
    assert_eq!(report["failure"]["ledger"], "anchors");
    assert_eq!(report["failure"]["kind"], "anchor_regression");
    assert_eq!(report["failure"]["entry_id"], anchor_id);
    let error = report["error"].as_str().expect("error");
    assert!(
        error.starts_with(&format!(
            "anchor regression: anchor #{anchor_id} covers chain_head {}… which is not in this \
             database's chain history",
            &hex::encode(head)[..16]
        )),
        "{error}"
    );
    assert!(!has_64_hex(error), "{error}");
    assert!(
        !serde_json::to_string(&report)
            .unwrap()
            .contains(&hex::encode(head)),
        "the anchored hash must not leave in full: {report}"
    );

    let out = log_verify(&db, &[]);
    assert_eq!(out.status.code(), Some(1));
    let text = stdout(&out);
    assert!(text.contains("=== Diagnosis ==="), "{text}");
    assert!(
        text.contains(&format!(
            "Where:           RFC 3161 anchor table at entry id {anchor_id}"
        )),
        "{text}"
    );
    assert!(text.contains("relabel"), "{text}");
    assert!(stderr(&out).contains("verification FAILED: anchor regression"));
}

#[test]
fn database_without_an_anchors_table_reads_as_before() {
    let temp = tempfile::tempdir().unwrap();
    let (db, kernel) = make_db(temp.path());
    drop(kernel);

    let out = log_verify(&db, &[]);
    assert!(out.status.success(), "{}\n{}", stdout(&out), stderr(&out));
    let text = stdout(&out);
    assert!(!text.contains("anchors:"), "{text}");
    assert!(!text.contains("Anchor Audit"), "{text}");

    let out = log_verify(&db, &["--json"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let report = json(&out);
    assert!(report.get("anchors_checked").is_none(), "{report}");
    assert_eq!(report["chain_valid"], true);
}
