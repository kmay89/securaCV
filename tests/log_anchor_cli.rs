//! End-to-end tests for the `log_anchor` CLI: typed subjects, offline
//! import classification, read-only observer verbs, `anchor-all`'s
//! constant per-run request pattern, `relabel`, and — when openssl is on
//! PATH — countersignature attribution under `--ca` and `--policy`. Every
//! test is offline: tokens come from the committed fixture (byte-spliced
//! over the hash under test) or from a throwaway local TSA.

#[path = "common/mod.rs"]
mod common;

use anyhow::Result;
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use witness_kernel::{
    tsa, tsa::AnchorSubject, CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel,
    KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";

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

fn add_test_event(kernel: &mut Kernel, cfg: &KernelConfig) -> Result<()> {
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
    kernel.append_event_checked(
        &desc,
        cand,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;
    Ok(())
}

/// A real database with one sealed event.
fn make_db(dir: &Path) -> Result<PathBuf> {
    let db_path = dir.join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    Ok(db_path)
}

fn log_anchor(db: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_log_anchor"))
        .args(["--db", &db.to_string_lossy(), "--device-key-seed", SEED])
        .args(args)
        .output()
        .expect("log_anchor runs")
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn assert_ok(out: &Output) {
    assert!(
        out.status.success(),
        "expected success\nstdout: {}\nstderr: {}",
        stdout(out),
        stderr(out)
    );
}

fn assert_exit1(out: &Output) {
    assert_eq!(
        out.status.code(),
        Some(1),
        "expected exit 1\nstdout: {}\nstderr: {}",
        stdout(out),
        stderr(out)
    );
}

fn write_response(dir: &Path, name: &str, hash: &[u8; 32]) -> String {
    let path = dir.join(name);
    std::fs::write(&path, common::response_with_imprint(hash)).unwrap();
    path.to_string_lossy().into_owned()
}

fn policy_json(entries: &[(&str, &str, &str, &[String])], subjects: &[&str]) -> String {
    let tsas = entries
        .iter()
        .map(|(name, role, ca, pins)| {
            let pins = pins
                .iter()
                .map(|p| format!("\"{p}\""))
                .collect::<Vec<_>>()
                .join(",");
            format!(
                r#"{{"name":"{name}","role":"{role}","url":"https://{name}.example/tsr","ca":"{ca}","cert_sha256":[{pins}]}}"#
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    let subjects = subjects
        .iter()
        .map(|s| format!("\"{s}\""))
        .collect::<Vec<_>>()
        .join(",");
    format!(r#"{{"format":"securacv-anchor-policy:v1","tsas":[{tsas}],"subjects":[{subjects}]}}"#)
}

fn anchor_rows(db: &Path) -> Vec<tsa::AnchorRecord> {
    let cfg = test_cfg(db);
    let kernel = Kernel::open(&cfg).expect("reopen kernel");
    tsa::list_anchors(&kernel.conn).expect("list anchors")
}

#[test]
fn import_classifies_each_head_kind() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let mut kernel = Kernel::open(&cfg)?;
    let chain_head = tsa::chain_head(&kernel.conn)?;
    let bundle = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let export_head = bundle.receipt_entry.entry_hash;
    let policy_head = common::bootstrap_policy_history(&mut kernel);
    drop(kernel);

    let chain = write_response(temp.path(), "chain.tsr", &chain_head);
    let export = write_response(temp.path(), "export.tsr", &export_head);
    let policy = write_response(temp.path(), "policy.tsr", &policy_head);
    let out = log_anchor(
        &db,
        &[
            "import",
            "--response",
            &chain,
            "--response",
            &export,
            "--response",
            &policy,
        ],
    );
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains(&format!(
            "anchor #1 stored: chain_head {}",
            hex::encode(chain_head)
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "anchor #2 stored: export_receipt_head {}",
            hex::encode(export_head)
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "anchor #3 stored: policy_head {}",
            hex::encode(policy_head)
        )),
        "{text}"
    );
    assert!(stderr(&out).is_empty(), "{}", stderr(&out));

    // An unrelated digest: the exact note, stored as `digest`.
    let other = [0x42u8; 32];
    let other_path = write_response(temp.path(), "other.tsr", &other);
    let out = log_anchor(&db, &["import", "--response", &other_path]);
    assert_ok(&out);
    assert!(
        stderr(&out).contains(&format!(
            "note: imprint {} is not in this DB's chain or receipt-ledger history; storing as a \
             generic digest anchor (use this for export-bundle digests; a ledger head pruned by \
             retention before import also lands here)",
            hex::encode(other)
        )),
        "{}",
        stderr(&out)
    );
    assert!(stdout(&out).contains("anchor #4 stored: digest "));

    // A sentinel: named as such, stored as `digest`.
    let sentinel = AnchorSubject::BreakGlassReceiptHead
        .empty_ledger_sentinel()
        .unwrap();
    let sentinel_path = write_response(temp.path(), "sentinel.tsr", &sentinel);
    let out = log_anchor(&db, &["import", "--response", &sentinel_path]);
    assert_ok(&out);
    assert!(
        stderr(&out).contains(&format!(
            "note: imprint {}… is the empty-ledger sentinel for break_glass_receipt_head; stored as a digest anchor",
            &hex::encode(sentinel)[..16]
        )),
        "{}",
        stderr(&out)
    );
    assert!(stdout(&out).contains("anchor #5 stored: digest "));

    let rows = anchor_rows(&db);
    assert_eq!(rows.len(), 5);
    assert_eq!(rows[0].subject, "chain_head");
    assert_eq!(rows[0].ledger_id, Some(1));
    assert_eq!(rows[1].subject, "export_receipt_head");
    assert_eq!(rows[1].ledger_id, Some(1));
    assert_eq!(rows[2].subject, "policy_head");
    assert_eq!(rows[2].ledger_id, Some(1));
    assert_eq!(rows[3].subject, "digest");
    assert_eq!(rows[3].ledger_id, None);
    assert_eq!(rows[4].subject, "digest");
    assert_eq!(rows[4].subject_hash, sentinel);
    Ok(())
}

#[test]
fn import_records_identity_from_fixture_token_offline() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let out = log_anchor(
        &db,
        &["import", "--response", &common::fixture_path("reply.tsr")],
    );
    assert_ok(&out);
    assert!(stdout(&out).contains("  signer cert sha256:fbf1c838f80923a0…"));
    let out = log_anchor(&db, &["list"]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains("signer cert sha256:fbf1c838f80923a0… (SecuraCV Test TSA)"),
        "{text}"
    );
    assert!(text.contains("via (offline)"), "{text}");
    assert!(text.contains("tsa (undeclared)"), "{text}");
    assert!(text.starts_with("#1  digest  "), "{text}");
    Ok(())
}

#[test]
fn list_verify_query_are_read_only() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let before = std::fs::read(&db)?;

    let out = log_anchor(&db, &["list"]);
    assert_ok(&out);
    assert_eq!(stdout(&out).trim(), "no anchors stored");

    let out = log_anchor(&db, &["verify"]);
    assert_ok(&out);
    assert!(stdout(&out).contains("no anchors stored"));

    let tsq = temp.path().join("chain.tsq");
    let out = log_anchor(&db, &["query", "--out", &tsq.to_string_lossy()]);
    assert_ok(&out);
    assert!(tsq.exists());
    assert!(
        stdout(&out).contains(
            "--url <TSA>  (within the retention window: after the head is pruned the token can only be stored as a digest anchor)"
        ),
        "{}",
        stdout(&out)
    );

    // No table was created and the file is byte-identical.
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    assert!(!tsa::anchor_table_exists(&kernel.conn)?);
    drop(kernel);
    assert_eq!(std::fs::read(&db)?, before, "observer verbs must not write");

    // The typed subjects refuse an empty ledger with the ledger's name.
    let out = log_anchor(
        &db,
        &[
            "query",
            "--out",
            &tsq.to_string_lossy(),
            "--subject",
            "export_receipt_head",
        ],
    );
    assert_exit1(&out);
    assert!(
        stderr(&out).contains("export-receipt chain is empty: nothing to anchor"),
        "{}",
        stderr(&out)
    );
    Ok(())
}

#[test]
fn verify_reports_membership_per_kind() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let mut kernel = Kernel::open(&cfg)?;
    let chain_head = tsa::chain_head(&kernel.conn)?;
    let bundle = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let export_head = bundle.receipt_entry.entry_hash;
    let policy_head = common::bootstrap_policy_history(&mut kernel);
    drop(kernel);
    let sentinel = AnchorSubject::BreakGlassReceiptHead
        .empty_ledger_sentinel()
        .unwrap();

    let files: Vec<String> = [
        ("chain.tsr", chain_head),
        ("export.tsr", export_head),
        ("policy.tsr", policy_head),
        ("sentinel.tsr", sentinel),
    ]
    .iter()
    .map(|(n, h)| write_response(temp.path(), n, h))
    .collect();
    let mut args = vec!["import"];
    for f in &files {
        args.push("--response");
        args.push(f);
    }
    assert_ok(&log_anchor(&db, &args));

    let out = log_anchor(&db, &["verify"]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains("anchor #1: UNVERIFIED (imprint matches, in chain history;"),
        "{text}"
    );
    assert!(
        text.contains("anchor #2: UNVERIFIED (imprint matches, in export-receipt chain history;"),
        "{text}"
    );
    assert!(
        text.contains("anchor #3: UNVERIFIED (imprint matches, in policy-change history;"),
        "{text}"
    );
    assert!(
        text.contains(
            "anchor #4: UNVERIFIED (imprint matches, empty-ledger sentinel for break_glass_receipt_head, chain membership not applicable;"
        ),
        "{text}"
    );
    assert!(
        text.contains("(untrusted) signer cert sha256:fbf1c838f80923a0… (unverified)"),
        "{text}"
    );
    assert!(
        text.contains("--ca <tsa-ca.pem> or --policy <anchor-policy.json>"),
        "{text}"
    );
    assert!(!text.contains(": OK ("), "no OK without openssl: {text}");

    // Truncate the export-receipt ledger: the export head anchor now FAILs.
    let kernel = Kernel::open(&cfg)?;
    kernel.conn.execute("DELETE FROM export_receipts", [])?;
    drop(kernel);
    let out = log_anchor(&db, &["verify"]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(text.contains("anchor #2: FAIL"), "{text}");
    assert!(
        text.contains("    anchored hash is not in export-receipt chain history"),
        "{text}"
    );
    assert!(stderr(&out).contains("1 anchor(s) failed verification"));
    Ok(())
}

#[test]
fn file_flag_hashes_bytes() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let bundle = temp.path().join("bundle.json");
    std::fs::write(&bundle, b"{\"not\":\"really a bundle\"}")?;
    let expected: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();
    let tsq = temp.path().join("bundle.tsq");
    let out = log_anchor(
        &db,
        &[
            "query",
            "--file",
            &bundle.to_string_lossy(),
            "--out",
            &tsq.to_string_lossy(),
        ],
    );
    assert_ok(&out);
    assert_eq!(
        std::fs::read(&tsq)?,
        tsa::build_request(&expected, None, true)
    );
    assert!(
        stderr(&out).contains(&format!(
            "digest of {}: {}",
            bundle.display(),
            hex::encode(expected)
        )),
        "{}",
        stderr(&out)
    );
    assert!(stdout(&out).contains(&format!("query for digest {}", hex::encode(expected))));

    // --subject / --digest / --file are mutually exclusive (clap usage, exit 2).
    let out = log_anchor(
        &db,
        &[
            "query",
            "--file",
            &bundle.to_string_lossy(),
            "--digest",
            &hex::encode(expected),
            "--out",
            &tsq.to_string_lossy(),
        ],
    );
    assert_eq!(out.status.code(), Some(2));
    Ok(())
}

#[test]
fn anchor_all_offline_writes_one_query_per_subject_including_empty() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let mut kernel = Kernel::open(&cfg)?;
    let chain_head = tsa::chain_head(&kernel.conn)?;
    let bundle = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let export_head = bundle.receipt_entry.entry_hash;
    let policy_head = common::bootstrap_policy_history(&mut kernel);
    drop(kernel);
    let before = std::fs::read(&db)?;

    let policy_path = temp.path().join("anchor-policy.json");
    std::fs::write(
        &policy_path,
        policy_json(
            &[
                ("alpha", "qualified", "alpha.pem", &[]),
                ("beta", "independent", "beta.pem", &[]),
            ],
            &[
                "chain_head",
                "export_receipt_head",
                "break_glass_receipt_head",
                "policy_head",
            ],
        ),
    )?;
    let out_dir = temp.path().join("out");
    let run = || {
        log_anchor(
            &db,
            &[
                "anchor-all",
                "--policy",
                &policy_path.to_string_lossy(),
                "--offline-dir",
                &out_dir.to_string_lossy(),
            ],
        )
    };
    let out = run();
    assert_ok(&out);
    let text = stdout(&out);
    let err = stderr(&out);

    let mut files: Vec<String> = std::fs::read_dir(&out_dir)?
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    files.sort();
    assert_eq!(
        files,
        vec![
            "break_glass_receipt_head.tsq",
            "chain_head.tsq",
            "export_receipt_head.tsq",
            "policy_head.tsq"
        ]
    );
    let sentinel = AnchorSubject::BreakGlassReceiptHead
        .empty_ledger_sentinel()
        .unwrap();
    assert_eq!(
        std::fs::read(out_dir.join("break_glass_receipt_head.tsq"))?,
        tsa::build_request(&sentinel, None, true)
    );
    assert_eq!(
        std::fs::read(out_dir.join("chain_head.tsq"))?,
        tsa::build_request(&chain_head, None, true)
    );
    assert_eq!(
        std::fs::read(out_dir.join("export_receipt_head.tsq"))?,
        tsa::build_request(&export_head, None, true)
    );
    assert_eq!(
        std::fs::read(out_dir.join("policy_head.tsq"))?,
        tsa::build_request(&policy_head, None, true)
    );
    assert!(
        err.contains(
            "anchor-all: break_glass_receipt_head: ledger is empty — anchoring the empty-ledger sentinel"
        ),
        "{err}"
    );
    assert!(text.contains("(4×2 submissions per run, always)"), "{text}");
    assert!(
        text.contains("anchor-all (offline): 4 query file(s) written to"),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "break_glass_receipt_head (ledger empty) sentinel {}",
            hex::encode(sentinel)
        )),
        "{text}"
    );
    assert!(text.contains("within the retention window"), "{text}");
    assert!(
        text.contains(&format!(
            "curl -s -H 'Content-Type: application/timestamp-query' --data-binary @{}/chain_head.tsq https://alpha.example/tsr > {}/chain_head.alpha.tsr",
            out_dir.display(),
            out_dir.display()
        )),
        "{text}"
    );
    assert!(text.contains("policy_head.beta.tsr"), "{text}");
    assert!(
        text.contains(&format!(
            "log_anchor --db {} import --policy {} --response {}/*.tsr",
            db.display(),
            policy_path.display(),
            out_dir.display()
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "log_anchor --db {} verify --policy {}",
            db.display(),
            policy_path.display()
        )),
        "{text}"
    );
    assert_eq!(
        std::fs::read(&db)?,
        before,
        "offline anchor-all must not write the DB"
    );

    // A second run is identical: no skip for already-written subjects.
    let first: Vec<(String, Vec<u8>)> = files
        .iter()
        .map(|f| (f.clone(), std::fs::read(out_dir.join(f)).unwrap()))
        .collect();
    let out = run();
    assert_ok(&out);
    assert_eq!(stdout(&out), text);
    for (f, bytes) in first {
        assert_eq!(std::fs::read(out_dir.join(&f))?, bytes, "{f}");
    }
    Ok(())
}

#[test]
fn relabel_downgrades_legacy_and_refuses_truncation() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db_path = temp.path().join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    let hashes: Vec<[u8; 32]> = {
        let mut stmt = kernel
            .conn
            .prepare("SELECT entry_hash FROM sealed_events ORDER BY id")?;
        let rows = stmt.query_map([], |r| r.get::<_, Vec<u8>>(0))?;
        rows.map(|r| r.unwrap().try_into().unwrap()).collect()
    };
    let (h1, _h2) = (hashes[0], hashes[1]);
    // (a) Prune BEFORE the anchor row exists: h1 is gone (the checkpoint
    // preserves h2, the cutoff head), and the row's ledger_id is NULL — the
    // legacy shape relabel exists for.
    kernel.enforce_retention_with_checkpoint(std::time::Duration::from_secs(0))?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    let legacy = tsa::insert_anchor(
        &kernel.conn,
        "chain_head",
        &h1,
        "(offline)",
        &common::spliced_token(&h1),
    )?;
    assert_eq!(tsa::list_anchors(&kernel.conn)?[0].ledger_id, None);

    // (b) A head sealed after the prune, anchored with its position, then
    // deleted: truncation, not a legacy prune.
    add_test_event(&mut kernel, &cfg)?;
    let (id3, h3): (i64, Vec<u8>) = kernel.conn.query_row(
        "SELECT id, entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1",
        [],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    let h3: [u8; 32] = h3.try_into().unwrap();
    let truncated = tsa::insert_anchor(
        &kernel.conn,
        "chain_head",
        &h3,
        "(offline)",
        &common::spliced_token(&h3),
    )?;
    assert_eq!(tsa::list_anchors(&kernel.conn)?[1].ledger_id, Some(id3));
    kernel
        .conn
        .execute("DELETE FROM sealed_events WHERE id = ?1", [id3])?;
    drop(kernel);

    let legacy_id = legacy.to_string();
    let out = log_anchor(&db_path, &["verify"]);
    assert_exit1(&out);
    assert!(stdout(&out).contains("anchored hash is not in chain history"));

    let out = log_anchor(
        &db_path,
        &["relabel", "--id", &legacy_id, "--subject", "digest"],
    );
    assert_ok(&out);
    assert_eq!(
        stdout(&out).trim(),
        format!(
            "anchor #{legacy} relabeled chain_head -> digest (the token is unchanged; ledger membership is no longer asserted)"
        )
    );
    let rows = anchor_rows(&db_path);
    assert_eq!(rows[0].subject, "digest");
    assert_eq!(rows[0].token_der, common::token_with_imprint(&h1));
    // Never upgrades, and a digest row has nothing to relabel.
    let out = log_anchor(
        &db_path,
        &["relabel", "--id", &legacy_id, "--subject", "chain_head"],
    );
    assert_exit1(&out);
    let out = log_anchor(
        &db_path,
        &["relabel", "--id", &legacy_id, "--subject", "digest"],
    );
    assert_exit1(&out);
    assert!(stderr(&out).contains("already a digest anchor"));

    let truncated_id = truncated.to_string();
    let out = log_anchor(
        &db_path,
        &["relabel", "--id", &truncated_id, "--subject", "digest"],
    );
    assert_exit1(&out);
    assert!(
        stderr(&out).contains(&format!(
            "anchor #{truncated}: the anchored head (ledger row {id3}) is newer than the retention cutoff — that is truncation or rollback, not a legacy prune; refusing to relabel"
        )),
        "{}",
        stderr(&out)
    );
    assert_eq!(anchor_rows(&db_path)[1].subject, "chain_head");

    let out = log_anchor(&db_path, &["relabel", "--id", "99", "--subject", "digest"]);
    assert_exit1(&out);
    assert!(stderr(&out).contains("anchor #99 not found"));
    Ok(())
}

#[cfg(not(feature = "tsa"))]
#[test]
fn request_stub_names_offline_flow() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let out = log_anchor(&db, &["request", "--url", "https://tsa.example/tsr"]);
    assert_exit1(&out);
    let err = stderr(&out);
    assert!(err.contains("rebuild with `--features tsa`"), "{err}");
    assert!(
        err.contains("`log_anchor anchor-all --policy <file> --offline-dir <dir>`"),
        "{err}"
    );
    Ok(())
}

#[cfg(not(feature = "tsa"))]
#[test]
fn anchor_all_online_stub() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let policy_path = temp.path().join("anchor-policy.json");
    std::fs::write(
        &policy_path,
        policy_json(
            &[
                ("alpha", "qualified", "alpha.pem", &[]),
                ("beta", "independent", "beta.pem", &[]),
            ],
            &["chain_head"],
        ),
    )?;
    let out = log_anchor(
        &db,
        &["anchor-all", "--policy", &policy_path.to_string_lossy()],
    );
    assert_exit1(&out);
    assert!(stderr(&out).contains("rebuild with `--features tsa`"));
    Ok(())
}

#[test]
fn verify_policy_without_openssl_refuses() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db = make_db(temp.path())?;
    let policy_path = temp.path().join("anchor-policy.json");
    std::fs::write(
        &policy_path,
        policy_json(
            &[
                ("alpha", "qualified", "alpha.pem", &[]),
                ("beta", "independent", "beta.pem", &[]),
            ],
            &["chain_head"],
        ),
    )?;
    let out = Command::new(env!("CARGO_BIN_EXE_log_anchor"))
        .env("PATH", "")
        .args(["--db", &db.to_string_lossy(), "--device-key-seed", SEED])
        .args(["verify", "--policy", &policy_path.to_string_lossy()])
        .output()?;
    assert_exit1(&out);
    assert!(
        stderr(&out).contains(
            "verify --policy requires the openssl CLI to check countersignatures under each TSA's CA (not found on PATH)"
        ),
        "{}",
        stderr(&out)
    );
    // --ca and --policy are mutually exclusive (clap usage, exit 2).
    let out = log_anchor(
        &db,
        &[
            "verify",
            "--ca",
            "x.pem",
            "--policy",
            &policy_path.to_string_lossy(),
        ],
    );
    assert_eq!(out.status.code(), Some(2));
    Ok(())
}

// ---------------------------------------------------------------------------
// openssl-backed tests (skipped when openssl is absent)
// ---------------------------------------------------------------------------

fn write_policy_file(dir: &Path, entries: &[(&str, &str, &str, &[String])]) -> PathBuf {
    let path = dir.join("anchor-policy.json");
    std::fs::write(&path, policy_json(entries, &["chain_head"])).unwrap();
    path
}

#[test]
fn verify_multi_ca_attributes_each_row() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(a) = common::ThrowawayTsa::new(&temp.path().join("a"), "TSA Alpha") else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let b = common::ThrowawayTsa::new(&temp.path().join("b"), "TSA Beta").unwrap();
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let head = tsa::chain_head(&kernel.conn)?;
    drop(kernel);

    let (ra, _) = a.mint_response(&head);
    let (rb, _) = b.mint_response(&head);
    let pa = temp.path().join("a.tsr");
    let pb = temp.path().join("b.tsr");
    std::fs::write(&pa, ra)?;
    std::fs::write(&pb, rb)?;
    assert_ok(&log_anchor(
        &db,
        &[
            "import",
            "--response",
            &pa.to_string_lossy(),
            "--response",
            &pb.to_string_lossy(),
            "--url",
            "https://tsa.example/tsr",
        ],
    ));

    let out = log_anchor(&db, &["verify", "--ca", &a.ca_path(), "--ca", &b.ca_path()]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains(&format!(
            "anchor #1: OK (imprint matches, in chain history, countersignature OK under {})",
            a.ca_path()
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "anchor #2: OK (imprint matches, in chain history, countersignature OK under {})",
            b.ca_path()
        )),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "head chain_head {}…: countersigned by 2 distinct TSA signer certificates",
            &hex::encode(head)[..16]
        )),
        "{text}"
    );
    assert!(!text.contains("UNVERIFIED"), "{text}");

    let out = log_anchor(&db, &["verify", "--ca", &a.ca_path()]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(text.contains("anchor #1: OK"), "{text}");
    assert!(text.contains("anchor #2: FAIL"), "{text}");
    assert!(
        text.contains("    openssl ts -verify failed under every CA given (1 tried):"),
        "{text}"
    );
    assert!(
        text.contains("countersigned by 1 distinct TSA signer certificate\n"),
        "{text}"
    );
    Ok(())
}

#[test]
fn verify_policy_satisfied_and_not() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(alpha) = common::ThrowawayTsa::new(&temp.path().join("alpha"), "TSA Alpha") else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let beta = common::ThrowawayTsa::new(&temp.path().join("beta"), "TSA Beta").unwrap();
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let head = tsa::chain_head(&kernel.conn)?;
    drop(kernel);
    let policy_path = write_policy_file(
        temp.path(),
        &[
            ("alpha", "qualified", &alpha.ca_path(), &[]),
            ("beta", "independent", &beta.ca_path(), &[]),
        ],
    );
    let policy_arg = policy_path.to_string_lossy().into_owned();

    let import_from = |tsa: &common::ThrowawayTsa, name: &str, digest: &[u8; 32]| {
        let (bytes, _) = tsa.mint_response(digest);
        let path = temp
            .path()
            .join(format!("{name}-{}.tsr", hex::encode(&digest[..4])));
        std::fs::write(&path, bytes).unwrap();
        let out = log_anchor(
            &db,
            &[
                "import",
                "--policy",
                &policy_arg,
                "--tsa",
                name,
                "--response",
                &path.to_string_lossy(),
            ],
        );
        assert_ok(&out);
        out
    };
    import_from(&alpha, "alpha", &head);
    import_from(&beta, "beta", &head);

    let note = format!(
        "note: 'qualified' and 'independent' are the operator's declarations in {}; this tool checks countersignatures, count and distinctness, not legal status.",
        policy_path.display()
    );
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains("anchor #1: OK (imprint matches, in chain history, countersignature OK under alpha's CA)"),
        "{text}"
    );
    assert!(
        text.contains("policy: chain_head: covered by alpha (qualified, declared), beta (independent, declared)"),
        "{text}"
    );
    assert!(text.contains("anchors #1, #2, bucket "), "{text}");
    assert!(text.contains("current head: yes"), "{text}");
    assert!(text.contains(&note), "{text}");
    assert!(
        text.contains(&format!(
            "anchor policy {}: SATISFIED",
            policy_path.display()
        )),
        "{text}"
    );
    assert!(
        text.contains("countersigned by 2 distinct TSA policy entries"),
        "{text}"
    );
    // The recorded URL is the policy entry's.
    let rows = anchor_rows(&db);
    assert_eq!(rows[0].tsa_url, "https://alpha.example/tsr");
    assert_eq!(rows[0].tsa_name.as_deref(), Some("alpha"));
    assert_eq!(rows[1].tsa_name.as_deref(), Some("beta"));

    // Drop beta's row: the independent role is missing.
    let kernel = Kernel::open(&cfg)?;
    kernel
        .conn
        .execute("DELETE FROM tsa_anchors WHERE tsa_name = 'beta'", [])?;
    drop(kernel);
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(
        text.contains("policy: chain_head: NOT covered — anchored by alpha (qualified, declared) only; missing role(s): independent"),
        "{text}"
    );
    assert!(
        text.contains(&format!(
            "  anchor now: log_anchor --db {} anchor-all --policy {}",
            db.display(),
            policy_path.display()
        )),
        "{text}"
    );
    assert!(text.contains(&note), "{text}");
    assert!(
        stderr(&out).contains(&format!(
            "anchor policy {}: NOT SATISFIED (1 subject(s) uncovered)",
            policy_path.display()
        )),
        "{}",
        stderr(&out)
    );

    // Re-cover, then advance the ledger: covered, but not the current head.
    import_from(&beta, "beta", &head);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    drop(kernel);
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains("current head: NO (ledger advanced since)"),
        "{text}"
    );
    assert!(text.contains("SATISFIED"), "{text}");
    assert!(text.contains(&note), "{text}");
    let out = log_anchor(
        &db,
        &["verify", "--policy", &policy_arg, "--require-current"],
    );
    assert_exit1(&out);
    assert!(
        stdout(&out).contains("  anchor now: log_anchor --db"),
        "{}",
        stdout(&out)
    );
    assert!(
        stderr(&out).contains("NOT SATISFIED (0 subject(s) uncovered, 1 not current)"),
        "{}",
        stderr(&out)
    );

    // A cert_sha256 pin that differs from the token's certificate contradicts
    // the row even though alpha's CA validated it: the pin is a declaration
    // the chain-validated certificate must agree with.
    let bogus = "0".repeat(64);
    std::fs::write(
        &policy_path,
        policy_json(
            &[
                ("alpha", "qualified", &alpha.ca_path(), &[bogus]),
                ("beta", "independent", &beta.ca_path(), &[]),
            ],
            &["chain_head"],
        ),
    )?;
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(text.contains("anchor #1: FAIL"), "{text}");
    assert!(
        text.contains(&format!(
            "    signer certificate sha256:{}… is not among the cert_sha256 pins declared for 'alpha'",
            &alpha.cert_sha256_hex()[..16]
        )),
        "{text}"
    );
    assert!(text.contains("policy: chain_head: NOT covered"), "{text}");
    Ok(())
}

#[test]
fn import_policy_refuses_wrong_ca() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(alpha) = common::ThrowawayTsa::new(&temp.path().join("alpha"), "TSA Alpha") else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let beta = common::ThrowawayTsa::new(&temp.path().join("beta"), "TSA Beta").unwrap();
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let head = tsa::chain_head(&kernel.conn)?;
    drop(kernel);
    let policy_path = write_policy_file(
        temp.path(),
        &[
            ("alpha", "qualified", &alpha.ca_path(), &[]),
            ("beta", "independent", &beta.ca_path(), &[]),
        ],
    );
    let (bytes, _) = beta.mint_response(&head);
    let path = temp.path().join("wrong.tsr");
    std::fs::write(&path, bytes)?;
    let out = log_anchor(
        &db,
        &[
            "import",
            "--policy",
            &policy_path.to_string_lossy(),
            "--tsa",
            "alpha",
            "--response",
            &path.to_string_lossy(),
        ],
    );
    assert_exit1(&out);
    let err = stderr(&out);
    assert!(
        err.contains(&format!(
            "response {} from alpha does not verify under its declared CA {}: not stored",
            path.display(),
            alpha.ca_path()
        )),
        "{err}"
    );
    assert!(err.contains("1 of 1 response(s) failed to import"), "{err}");
    assert_eq!(
        stdout(&log_anchor(&db, &["list"])).trim(),
        "no anchors stored"
    );
    Ok(())
}

#[test]
fn import_infers_tsa_from_filename() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(alpha) = common::ThrowawayTsa::new(&temp.path().join("alpha"), "TSA Alpha") else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let beta = common::ThrowawayTsa::new(&temp.path().join("beta"), "TSA Beta").unwrap();
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let head = tsa::chain_head(&kernel.conn)?;
    drop(kernel);
    let policy_path = write_policy_file(
        temp.path(),
        &[
            ("alpha", "qualified", &alpha.ca_path(), &[]),
            ("beta", "independent", &beta.ca_path(), &[]),
        ],
    );
    let (bytes, _) = beta.mint_response(&head);
    let path = temp.path().join("chain_head.beta.tsr");
    std::fs::write(&path, bytes)?;
    let out = log_anchor(
        &db,
        &[
            "import",
            "--policy",
            &policy_path.to_string_lossy(),
            "--response",
            &path.to_string_lossy(),
        ],
    );
    assert_ok(&out);
    assert!(stdout(&out).contains("anchor #1 stored: chain_head "));
    let rows = anchor_rows(&db);
    assert_eq!(rows[0].tsa_name.as_deref(), Some("beta"));
    assert_eq!(rows[0].tsa_url, "https://beta.example/tsr");

    // A name that is not in the policy cannot be inferred.
    let (bytes, _) = alpha.mint_response(&head);
    let unknown = temp.path().join("chain_head.gamma.tsr");
    std::fs::write(&unknown, bytes)?;
    let out = log_anchor(
        &db,
        &[
            "import",
            "--policy",
            &policy_path.to_string_lossy(),
            "--response",
            &unknown.to_string_lossy(),
        ],
    );
    assert_exit1(&out);
    assert!(
        stderr(&out).contains(&format!(
            "cannot infer the TSA for {}: pass --tsa <name>",
            unknown.display()
        )),
        "{}",
        stderr(&out)
    );
    Ok(())
}

#[test]
fn verify_policy_shared_root_needs_pins() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(root) = common::SharedRoot::new(&temp.path().join("root")) else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let alpha =
        common::ThrowawayTsa::new_under_shared_root(&temp.path().join("alpha"), "TSA Alpha", &root)
            .unwrap();
    let beta =
        common::ThrowawayTsa::new_under_shared_root(&temp.path().join("beta"), "TSA Beta", &root)
            .unwrap();
    let db = make_db(temp.path())?;
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let head = tsa::chain_head(&kernel.conn)?;
    drop(kernel);
    let root_ca = root.ca_path();

    // Both entries point at the same root bundle, no pins.
    let write = |alpha_pins: &[String], beta_pins: &[String]| {
        write_policy_file(
            temp.path(),
            &[
                ("alpha", "qualified", &root_ca, alpha_pins),
                ("beta", "independent", &root_ca, beta_pins),
            ],
        )
    };
    let policy_path = write(&[], &[]);
    let policy_arg = policy_path.to_string_lossy().into_owned();
    for (tsa, name) in [(&alpha, "alpha"), (&beta, "beta")] {
        let (bytes, _) = tsa.mint_response(&head);
        let path = temp.path().join(format!("chain_head.{name}.tsr"));
        std::fs::write(&path, bytes)?;
        assert_ok(&log_anchor(
            &db,
            &[
                "import",
                "--policy",
                &policy_arg,
                "--response",
                &path.to_string_lossy(),
            ],
        ));
    }

    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(text.contains("anchor #1: FAIL"), "{text}");
    assert!(text.contains("anchor #2: FAIL"), "{text}");
    assert!(
        text.contains(
            "    verifies under the CA of both alpha and beta — their CA bundles overlap and no cert_sha256 pin singles one out; add cert_sha256 pins to disambiguate, or use leaf-issuer CA files"
        ),
        "{text}"
    );
    assert!(!text.contains(": OK ("), "{text}");

    // Pin each leaf to its entry: the chain-validated cert breaks the tie.
    let alpha_pin = alpha.cert_sha256_hex();
    let beta_pin = beta.cert_sha256_hex();
    assert_ne!(alpha_pin, beta_pin);
    write(
        std::slice::from_ref(&alpha_pin),
        std::slice::from_ref(&beta_pin),
    );
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_ok(&out);
    let text = stdout(&out);
    assert!(
        text.contains("anchor #1: OK (imprint matches, in chain history, countersignature OK under alpha's CA, attributed to alpha by cert_sha256 pin)"),
        "{text}"
    );
    assert!(
        text.contains("attributed to beta by cert_sha256 pin"),
        "{text}"
    );
    assert!(text.contains("SATISFIED"), "{text}");
    assert!(
        text.contains("countersigned by 2 distinct TSA policy entries"),
        "{text}"
    );

    // A pin that differs from the token's certificate contradicts it.
    let bogus = "0".repeat(64);
    write(&[bogus], std::slice::from_ref(&beta_pin));
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_exit1(&out);
    let text = stdout(&out);
    // With a shared root no pin can single alpha's row out, so it stays an
    // unresolved overlap; the direct pin contradiction is exercised under
    // leaf-issuer CAs in `verify_policy_satisfied_and_not`.
    assert!(text.contains("anchor #1: FAIL"), "{text}");
    assert!(text.contains("verifies under the CA of both"), "{text}");
    assert!(
        text.contains("attributed to beta by cert_sha256 pin"),
        "{text}"
    );

    // A pin on the WRONG entry (beta pins alpha's cert): still FAIL — the
    // pin attributes alpha's row to beta, which contradicts its declared
    // name, and beta's own row has no pin to resolve the overlap.
    write(&[], &[alpha_pin]);
    let out = log_anchor(&db, &["verify", "--policy", &policy_arg]);
    assert_exit1(&out);
    let text = stdout(&out);
    assert!(text.contains("anchor #1: FAIL"), "{text}");
    assert!(text.contains("anchor #2: FAIL"), "{text}");
    assert!(
        text.contains("declared TSA 'alpha' but the countersignature attributes to 'beta'"),
        "{text}"
    );
    assert!(!text.contains("SATISFIED\n"), "{text}");
    Ok(())
}
