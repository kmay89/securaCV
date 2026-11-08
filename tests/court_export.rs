//! End-to-end tests for the `court_export` disclosure-kit assembler: it must
//! package only verified, chain-anchored evidence (fail closed on tampering or
//! a foreign database), attest every kit file by hash in the manifest, and
//! say honestly whether any RFC 3161 token covers the evidence bytes.

#[path = "common/mod.rs"]
mod common;

use anyhow::Result;
use sha2::{Digest, Sha256};
use std::path::Path;
use std::process::Command;
use witness_kernel::{
    tsa, CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";
const OTHER_SEED: &str = "devkey:test:ffffffffffffffffffff";

fn test_cfg_with_seed(db_path: &Path, seed: &str) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60 * 60),
        device_key_seed: seed.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn test_cfg(db_path: &Path) -> KernelConfig {
    test_cfg_with_seed(db_path, SEED)
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

/// Build a real database with one event and one self-exported bundle on disk.
fn make_db_and_bundle(dir: &Path) -> Result<(std::path::PathBuf, std::path::PathBuf)> {
    make_db_and_bundle_with_seed(dir, SEED)
}

fn make_db_and_bundle_with_seed(
    dir: &Path,
    seed: &str,
) -> Result<(std::path::PathBuf, std::path::PathBuf)> {
    let db_path = dir.join("witness.db");
    let cfg = test_cfg_with_seed(&db_path, seed);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    let bundle = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let bundle_path = dir.join("witness_export.json");
    std::fs::write(&bundle_path, serde_json::to_vec(&bundle)?)?;
    Ok((db_path, bundle_path))
}

fn run_court_export(db: &Path, bundle: &Path, out: &Path) -> std::process::Output {
    run_court_export_with_seed(db, bundle, out, SEED)
}

fn run_court_export_with_seed(
    db: &Path,
    bundle: &Path,
    out: &Path,
    seed: &str,
) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_court_export"))
        .args([
            "--bundle",
            &bundle.to_string_lossy(),
            "--db",
            &db.to_string_lossy(),
            "--output-dir",
            &out.to_string_lossy(),
            "--device-key-seed",
            seed,
            "--ui",
            "plain",
        ])
        .output()
        .expect("court_export runs")
}

#[test]
fn assembles_kit_and_manifest_hashes_are_correct() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let kit = temp.path().join("kit");

    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );

    // Every advertised document exists.
    for name in [
        "README.md",
        "SYSTEM_DESCRIPTION.md",
        "CUSTODY_AND_CONTROL.md",
        "VERIFICATION.md",
        "CERTIFICATION_FRE_902_13.md",
        "CERTIFICATION_FRE_902_14.md",
        "MANIFEST.json",
        "evidence/witness_export.json",
    ] {
        assert!(kit.join(name).exists(), "missing kit file {}", name);
    }

    // The evidence copy is byte-identical to the input bundle.
    assert_eq!(
        std::fs::read(&bundle)?,
        std::fs::read(kit.join("evidence/witness_export.json"))?
    );

    // Every manifest entry's hash matches the bytes on disk (the manifest
    // attests the kit as built), and the evidence digest is the real one.
    let manifest: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(kit.join("MANIFEST.json"))?)?;
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();
    let bundle_hex = hex::encode(bundle_sha);
    assert_eq!(manifest["evidence"]["sha256"], bundle_hex.as_str());
    assert_eq!(manifest["evidence"]["auth_mode"], "self_export");
    assert_eq!(manifest["anchored"], false, "no anchors were recorded");
    for file in manifest["files"].as_array().expect("files array") {
        let path = kit.join(file["path"].as_str().expect("path"));
        let bytes = std::fs::read(&path)?;
        let digest: [u8; 32] = Sha256::digest(&bytes).into();
        let hex = hex::encode(digest);
        assert_eq!(
            file["sha256"],
            hex,
            "manifest hash mismatch for {}",
            path.display()
        );
    }

    // An unanchored kit says so, loudly, with the exact remediation command.
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(stdout.contains("WARNING: no RFC 3161 anchor covers"));
    // The remediation command must be complete enough to copy-paste:
    // log_anchor's --url is required, so its absence would make the printed
    // command a guaranteed usage error.
    assert!(stdout.contains("log_anchor request"));
    assert!(stdout.contains("--url "));
    assert!(stdout.contains(&bundle_hex));
    Ok(())
}

#[test]
fn tampered_bundle_is_refused_and_nothing_is_packaged() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;

    // Flip the disclosed artifact (a jitter parameter survives JSON reparse
    // but changes the artifact hash) while keeping the signed receipt.
    let mut parsed: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&bundle)?)?;
    parsed["artifact"]["jitter_s"] = serde_json::json!(999);
    let tampered = temp.path().join("tampered.json");
    std::fs::write(&tampered, serde_json::to_vec(&parsed)?)?;

    let kit = temp.path().join("kit_tampered");
    let out = run_court_export(&db, &tampered, &kit);
    assert!(!out.status.success(), "a tampered bundle must be refused");
    assert!(
        !kit.join("MANIFEST.json").exists(),
        "no kit may be assembled from unverified evidence"
    );
    Ok(())
}

#[test]
fn bundle_from_a_foreign_database_is_refused_as_custody_break() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let dir_a = temp.path().join("a");
    let dir_b = temp.path().join("b");
    std::fs::create_dir_all(&dir_a)?;
    std::fs::create_dir_all(&dir_b)?;
    let (_db_a, bundle_a) = make_db_and_bundle(&dir_a)?;
    // Database B belongs to a DIFFERENT device (different seed). Even if two
    // devices exported identical content in the same bucket (colliding entry
    // hashes — the hashed payload carries no device identity), the identity
    // and signature checks must refuse the cross-device package.
    let (db_b, _bundle_b) = make_db_and_bundle_with_seed(&dir_b, OTHER_SEED)?;

    // Bundle from device A packaged against device B's database: custody
    // break, refused.
    let kit = temp.path().join("kit_foreign");
    let out = run_court_export_with_seed(&db_b, &bundle_a, &kit, OTHER_SEED);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("custody break"));
    Ok(())
}

/// The committed OpenSSL-generated fixture token (covers
/// sha256("securacv-fixture"), NOT any live bundle).
fn fixture_token() -> tsa::TimestampToken {
    let path = format!(
        "{}/tests/fixtures/tsa/reply.tsr",
        env!("CARGO_MANIFEST_DIR")
    );
    tsa::parse_response(&std::fs::read(&path).expect("reading TSA fixture"))
        .expect("fixture token parses")
}

#[test]
fn invalid_anchor_tokens_are_excluded_never_trusted() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;

    // 1. A row claiming to cover the bundle whose "token" is not a token at
    //    all: the anchored claim must rest on the token, not the row.
    let garbage = tsa::TimestampToken {
        status: 0,
        gen_time: "20260818000000Z".to_string(),
        policy_oid: "1.2.3.4".to_string(),
        serial_hex: "01".to_string(),
        imprint: bundle_sha.to_vec(),
        nonce: None,
        token_der: b"test-token-der".to_vec(),
    };
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &bundle_sha,
        "https://tsa.example/tsr",
        &garbage,
    )?;
    // 2. A REAL token (the fixture) on a row that claims it covers the bundle
    //    bytes — but the token's embedded imprint is the fixture digest.
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &bundle_sha,
        "https://tsa.example/tsr",
        &fixture_token(),
    )?;
    // 3. The same real token on a self-consistent row (subject hash == the
    //    token's imprint) claiming chain-head status — but that hash is not
    //    in this database's chain history.
    let fixture_digest: [u8; 32] = Sha256::digest(b"securacv-fixture").into();
    tsa::insert_anchor(
        &kernel.conn,
        "chain_head",
        &fixture_digest,
        "https://tsa.example/tsr",
        &fixture_token(),
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_anchored");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );

    // All three rows are excluded, each with its own named reason, and the
    // kit honestly reports itself unanchored.
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("does not parse"), "stderr: {stderr}");
    assert!(
        stderr.contains("not the digest the anchor row claims"),
        "stderr: {stderr}"
    );
    assert!(
        stderr.contains("not in this database's chain history"),
        "stderr: {stderr}"
    );
    let manifest: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(kit.join("MANIFEST.json"))?)?;
    assert_eq!(manifest["anchored"], false);
    assert_eq!(manifest["anchor_count"], 0);
    assert!(
        !kit.join("anchors").exists(),
        "no excluded token may be packaged"
    );
    assert!(String::from_utf8_lossy(&out.stdout).contains("WARNING: no RFC 3161 anchor covers"));
    Ok(())
}

/// Full positive path: a token really minted over the live bundle bytes (by a
/// throwaway local TSA, the same way the committed fixtures were made) is
/// packaged, marked as covering the bundle, and flips the kit to anchored.
/// Skipped when openssl is not on PATH.
#[test]
fn real_token_minted_over_the_bundle_bytes_anchors_the_kit() -> Result<()> {
    if Command::new("openssl").arg("version").output().is_err() {
        eprintln!("skipping: openssl not available");
        return Ok(());
    }
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();

    // Throwaway TSA (fixtures README recipe): key + cert + serial in a
    // scratch dir; it signs this test's artifacts only.
    let tsa_dir = temp.path().join("tsa");
    std::fs::create_dir_all(&tsa_dir)?;
    std::fs::write(
        tsa_dir.join("tsa.cnf"),
        "[ req ]\ndistinguished_name = dn\nprompt = no\n[ dn ]\nCN = SecuraCV Test TSA\nO = Fixture Only\n\
         [ tsa_cert ]\nextendedKeyUsage = critical,timeStamping\nkeyUsage = critical,digitalSignature\nbasicConstraints = CA:false\n\
         [ tsa ]\ndefault_tsa = tsa_config1\n[ tsa_config1 ]\nserial = ./serial\ncrypto_device = builtin\n\
         signer_cert = ./tsa.crt\nsigner_key = ./tsa.key\ndefault_policy = 1.3.6.1.4.1.13762.3\ndigests = sha256\n\
         accuracy = secs:1\nordering = no\ntsa_name = no\ness_cert_id_chain = no\nsigner_digest = sha256\n",
    )?;
    std::fs::write(tsa_dir.join("serial"), "01\n")?;
    let keygen = Command::new("openssl")
        .current_dir(&tsa_dir)
        .args([
            "req",
            "-x509",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:P-256",
            "-keyout",
            "tsa.key",
            "-out",
            "tsa.crt",
            "-days",
            "2",
            "-nodes",
            "-config",
            "tsa.cnf",
            "-extensions",
            "tsa_cert",
        ])
        .output()?;
    assert!(
        keygen.status.success(),
        "openssl req failed: {}",
        String::from_utf8_lossy(&keygen.stderr)
    );

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    let chain_head = tsa::chain_head(&kernel.conn)?;

    // Mint one token over the bundle bytes and one over the real chain head.
    for (name, digest, kind) in [
        ("bundle", bundle_sha, "digest"),
        ("head", chain_head, "chain_head"),
    ] {
        std::fs::write(
            tsa_dir.join(format!("{name}.tsq")),
            tsa::build_request(&digest, None, true),
        )?;
        let reply = Command::new("openssl")
            .current_dir(&tsa_dir)
            .env("OPENSSL_CONF", "tsa.cnf")
            .args([
                "ts",
                "-reply",
                "-queryfile",
                &format!("{name}.tsq"),
                "-out",
                &format!("{name}.tsr"),
                "-section",
                "tsa_config1",
            ])
            .output()?;
        assert!(
            reply.status.success(),
            "openssl ts -reply failed: {}",
            String::from_utf8_lossy(&reply.stderr)
        );
        let token = tsa::parse_response(&std::fs::read(tsa_dir.join(format!("{name}.tsr")))?)?;
        tsa::verify_match(&token, &digest, None)?;
        tsa::insert_anchor(
            &kernel.conn,
            kind,
            &digest,
            "https://tsa.example/tsr",
            &token,
        )?;
    }
    drop(kernel);

    let kit = temp.path().join("kit_anchored");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );

    let manifest: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(kit.join("MANIFEST.json"))?)?;
    assert_eq!(manifest["anchored"], true);
    assert_eq!(manifest["anchor_count"], 2);
    // Both tokens travel verbatim, and the verification instructions name the
    // exact openssl invocation for the covering one.
    let anchor_files: Vec<_> = std::fs::read_dir(kit.join("anchors"))?
        .map(|e| e.unwrap().path())
        .collect();
    assert_eq!(anchor_files.len(), 2);
    for f in &anchor_files {
        assert_eq!(
            f.extension().and_then(|e| e.to_str()),
            Some("der"),
            "kit anchors are bare tokens and must not be named like responses: {f:?}"
        );
    }
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    let printed = verification
        .lines()
        .find(|l| {
            l.starts_with(&format!(
                "openssl ts -verify -digest {}",
                hex::encode(bundle_sha)
            ))
        })
        .expect("VERIFICATION.md names the exact openssl invocation for the covering token");
    // The instructions must WORK for a recipient with nothing but openssl: run
    // the exact printed line (CA placeholder filled in) from the kit directory.
    // This is the check that catches a wrong `-token_in` form — the string
    // assertion alone let a non-working command ship.
    let ca = tsa_dir.join("tsa.crt").to_string_lossy().to_string();
    let filled = printed.replace("<tsa-ca.pem>", &ca);
    let argv: Vec<&str> = filled.split_whitespace().collect();
    assert_eq!(argv[0], "openssl");
    let run = Command::new(argv[0])
        .current_dir(&kit)
        .args(&argv[1..])
        .output()?;
    assert!(
        run.status.success() && String::from_utf8_lossy(&run.stdout).contains("Verification: OK"),
        "the kit's own openssl instruction must verify the covering token: `{filled}` -> stdout {} stderr {}",
        String::from_utf8_lossy(&run.stdout),
        String::from_utf8_lossy(&run.stderr)
    );
    assert!(!String::from_utf8_lossy(&out.stdout).contains("WARNING"));
    assert!(!String::from_utf8_lossy(&out.stderr).contains("excluded"));
    Ok(())
}

#[test]
fn nonempty_output_dir_is_refused() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;

    // Any pre-existing content — not just a previous manifest — must refuse:
    // MANIFEST.json attests every file in the directory, so a stray file
    // would sit inside the kit unlisted.
    let kit = temp.path().join("kit_dirty");
    std::fs::create_dir_all(&kit)?;
    std::fs::write(kit.join("stray.txt"), b"leftover")?;
    let out = run_court_export(&db, &bundle, &kit);
    assert!(!out.status.success(), "a nonempty output dir must refuse");
    assert!(String::from_utf8_lossy(&out.stderr).contains("not empty"));
    assert!(!kit.join("MANIFEST.json").exists());
    assert_eq!(std::fs::read(kit.join("stray.txt"))?, b"leftover");

    // An explicitly-created EMPTY directory is fine.
    let kit_empty = temp.path().join("kit_empty");
    std::fs::create_dir_all(&kit_empty)?;
    let out = run_court_export(&db, &bundle, &kit_empty);
    assert!(
        out.status.success(),
        "an empty output dir must be accepted: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    Ok(())
}

#[test]
fn custody_position_is_ordinal_not_rowid() -> Result<()> {
    // The custody record claims "receipt N of M in the signed export chain".
    // N must be the receipt's ORDINAL position in the chain, not its SQLite
    // rowid — those diverge whenever the ledger has a rowid gap. AUTOINCREMENT
    // never reuses ids, so deleting the two newest receipts (tail truncation,
    // which still leaves a chain that verifies) and then exporting again yields
    // a receipt whose rowid is 4 but whose ordinal position is 2. "receipt 4
    // of 2" would be a false statement in a court document.
    let temp = tempfile::tempdir()?;
    let db_path = temp.path().join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg)?;
    for _ in 0..3 {
        add_test_event(&mut kernel, &cfg)?;
        let _ = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    }
    // Delete receipts 2 and 3. The surviving head is receipt 1; the next
    // append (read from the DB, ORDER BY id DESC) chains onto it, so the chain
    // stays valid — but AUTOINCREMENT hands the new row id 4, not 2.
    kernel
        .conn
        .execute("DELETE FROM export_receipts WHERE id IN (2, 3)", [])?;
    add_test_event(&mut kernel, &cfg)?;
    let latest = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let bundle_path = temp.path().join("witness_export.json");
    std::fs::write(&bundle_path, serde_json::to_vec(&latest)?)?;
    drop(kernel);

    let kit = temp.path().join("kit_ordinal");
    let out = run_court_export(&db_path, &bundle_path, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );

    // Two receipts survive (ordinals 1 and 2); the packaged bundle is the
    // second one in the chain even though its rowid is 4.
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("2 of 2"),
        "custody must report the ordinal position (2 of 2), got: {custody}"
    );
    assert!(
        !custody.contains("4 of 2"),
        "custody must not report the SQLite rowid as the chain position"
    );
    assert!(String::from_utf8_lossy(&out.stdout).contains("receipt 2 of 2"));
    Ok(())
}

#[test]
fn tampered_interior_receipt_row_is_refused() -> Result<()> {
    // The custody record says "receipt N of M in the signed export chain" —
    // so the WHOLE chain must verify, not just the bundle's own row. Tamper
    // with an EARLIER receipt and package a later bundle: refused.
    let temp = tempfile::tempdir()?;
    let db_path = temp.path().join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    let _first = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let second = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let bundle_path = temp.path().join("witness_export.json");
    std::fs::write(&bundle_path, serde_json::to_vec(&second)?)?;
    kernel.conn.execute(
        "UPDATE export_receipts SET payload_json = '{}' WHERE id = 1",
        [],
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_broken_chain");
    let out = run_court_export(&db_path, &bundle_path, &kit);
    assert!(
        !out.status.success(),
        "a broken export-receipt chain must refuse packaging"
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("export-receipt chain does not verify"),
        "stderr: {stderr}"
    );
    assert!(!kit.join("MANIFEST.json").exists());
    Ok(())
}

// ---------------------------------------------------------------------------
// Receipt-head packaging (hash-keyed) and the privacy rule for other kinds.
// ---------------------------------------------------------------------------

fn bundle_entry_hash(bundle: &Path) -> Result<[u8; 32]> {
    let parsed: witness_kernel::ExportBundle = serde_json::from_slice(&std::fs::read(bundle)?)?;
    Ok(parsed.receipt_entry.entry_hash)
}

fn read_manifest(kit: &Path) -> Result<serde_json::Value> {
    Ok(serde_json::from_str(&std::fs::read_to_string(
        kit.join("MANIFEST.json"),
    )?)?)
}

#[test]
fn receipt_head_anchor_is_packaged_and_marked() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let entry_hash = bundle_entry_hash(&bundle)?;
    let entry_hash_hex = hex::encode(entry_hash);

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &entry_hash,
        "https://tsa.example/tsr",
        &common::spliced_token(&entry_hash),
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_receipt");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(kit
        .join("anchors/anchor-0001-export_receipt_head.der")
        .exists());
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["anchored"], false);
    assert_eq!(manifest["receipt_anchored"], true);
    assert_eq!(manifest["anchor_count"], 1);
    assert_eq!(manifest["anchors"][0]["covers"], "receipt_entry");
    assert_eq!(manifest["anchors"][0]["subject"], "export_receipt_head");
    assert_eq!(
        manifest["anchors"][0]["file"],
        "anchors/anchor-0001-export_receipt_head.der"
    );
    assert_eq!(
        manifest["anchors"][0]["tsa"]["declared_name"],
        serde_json::Value::Null
    );
    assert_eq!(
        manifest["anchors"][0]["tsa"]["signer_cert_sha256"],
        "fbf1c838f80923a01badb6030b9d708c5ef1a65b7d23f1f53a6aa274d1b99542"
    );

    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("covers this disclosure's export receipt"),
        "{custody}"
    );
    assert!(
        custody.contains("by TSA certificate sha256:fbf1c838f80923a0… (SecuraCV Test TSA)"),
        "{custody}"
    );
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    let line = verification
        .lines()
        .find(|l| l.starts_with(&format!("openssl ts -verify -digest {entry_hash_hex}")))
        .expect("VERIFICATION.md names the openssl invocation over the entry hash");
    assert!(line.contains("-token_in"), "{line}");
    assert!(
        verification.contains("no producing database is involved"),
        "{verification}"
    );
    assert!(
        verification.contains("need the producing database"),
        "{verification}"
    );
    assert!(
        verification.contains("A token over this disclosure's export receipt is present below"),
        "{verification}"
    );
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        stdout.contains("1 covering this disclosure's export receipt"),
        "{stdout}"
    );
    assert!(
        stdout.contains("WARNING: no RFC 3161 anchor covers"),
        "{stdout}"
    );
    assert!(
        stdout.contains("(offline: log_anchor query --db"),
        "{stdout}"
    );
    Ok(())
}

#[test]
fn kit_identity_comes_only_from_the_packaged_token() -> Result<()> {
    // The row's cache columns were written from the token at insert time;
    // if the stored token later cannot support them, the kit must not
    // repeat them — a recipient can only reproduce what the token embeds.
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let entry_hash = bundle_entry_hash(&bundle)?;

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    // Row 1: cache populated, token rewritten without its certificate set.
    let id1 = tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &entry_hash,
        "https://tsa.example/tsr",
        &common::spliced_token(&entry_hash),
    )?;
    kernel.conn.execute(
        "UPDATE tsa_anchors SET token_der = ?1 WHERE id = ?2",
        rusqlite::params![common::token_without_certificates(&entry_hash), id1],
    )?;
    let rows = tsa::list_anchors(&kernel.conn)?;
    assert!(
        rows[0].signer_fingerprint.is_some(),
        "the cache still remembers the certificate the token no longer embeds"
    );
    drop(kernel);

    let kit = temp.path().join("kit_cache");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("by TSA (no certificate embedded; signer id d806acd919f81e61…)"),
        "{custody}"
    );
    assert!(!custody.contains("TSA certificate sha256:"), "{custody}");
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["anchors"][0]["covers"], "receipt_entry");
    assert_eq!(
        manifest["anchors"][0]["tsa"]["signer_cert_sha256"],
        serde_json::Value::Null
    );
    assert!(!std::fs::read_to_string(kit.join("MANIFEST.json"))?.contains("fbf1c838"));

    // Row 2 (a second kit): the token's signer is not readable at all and a
    // bogus cache was planted; neither the bogus value nor the fixture's
    // certificate hash may reach the kit.
    let kernel = Kernel::open(&cfg)?;
    kernel.conn.execute("DELETE FROM tsa_anchors", [])?;
    let id2 = tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &entry_hash,
        "https://tsa.example/tsr",
        &common::spliced_token(&entry_hash),
    )?;
    let bogus = "ab".repeat(32);
    kernel.conn.execute(
        "UPDATE tsa_anchors SET token_der = ?1, signer_cert_sha256 = ?2 WHERE id = ?3",
        rusqlite::params![
            common::token_with_unreadable_signer(&entry_hash),
            bogus,
            id2
        ],
    )?;
    drop(kernel);
    let kit2 = temp.path().join("kit_unreadable");
    let out = run_court_export(&db, &bundle, &kit2);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let custody = std::fs::read_to_string(kit2.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("by TSA (identity not readable from the token)"),
        "{custody}"
    );
    let manifest_text = std::fs::read_to_string(kit2.join("MANIFEST.json"))?;
    for leaked in [bogus.as_str(), "fbf1c838"] {
        assert!(!custody.contains(leaked), "custody leaks {leaked}");
        assert!(!manifest_text.contains(leaked), "manifest leaks {leaked}");
    }
    Ok(())
}
#[test]
fn legacy_digest_row_over_own_receipt_is_packaged_by_hash() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let db_path = temp.path().join("witness.db");
    let cfg = test_cfg(&db_path);
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    let first = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let second = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let bundle_path = temp.path().join("witness_export.json");
    std::fs::write(&bundle_path, serde_json::to_vec(&second)?)?;
    let own = second.receipt_entry.entry_hash;
    let other = first.receipt_entry.entry_hash;
    assert_ne!(own, other);

    tsa::ensure_anchor_table(&kernel.conn)?;
    // The pre-typed-subject runbook shape: the receipt hash anchored by hand
    // as a plain digest.
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &own,
        "https://tsa.example/tsr",
        &common::spliced_token(&own),
    )?;
    // A digest row over a DIFFERENT receipt's entry hash: not this
    // disclosure's business.
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &other,
        "https://tsa.example/tsr",
        &common::spliced_token(&other),
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_legacy");
    let out = run_court_export(&db_path, &bundle_path, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["anchor_count"], 1);
    assert_eq!(manifest["receipt_anchored"], true);
    assert_eq!(manifest["anchored"], false);
    assert_eq!(manifest["anchors"][0]["covers"], "receipt_entry");
    assert_eq!(manifest["anchors"][0]["subject"], "digest");
    assert!(kit.join("anchors/anchor-0001-digest.der").exists());
    assert!(!kit.join("anchors/anchor-0002-digest.der").exists());
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(!stderr.contains("excluded"), "{stderr}");
    assert!(!stderr.contains(&hex::encode(other)), "{stderr}");
    Ok(())
}

/// A token really minted over the receipt entry hash verifies with the exact
/// line the kit prints. Skipped when openssl is not on PATH.
#[test]
fn real_receipt_token_verifies_from_kit() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(tsa_srv) = common::ThrowawayTsa::new(&temp.path().join("tsa"), "SecuraCV Test TSA")
    else {
        eprintln!("skipping: openssl not available");
        return Ok(());
    };
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let entry_hash = bundle_entry_hash(&bundle)?;
    let token = tsa_srv.mint(&entry_hash);

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &entry_hash,
        "https://tsa.example/tsr",
        &token,
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_real_receipt");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    let printed = verification
        .lines()
        .find(|l| {
            l.starts_with(&format!(
                "openssl ts -verify -digest {}",
                hex::encode(entry_hash)
            ))
        })
        .expect("VERIFICATION.md names the openssl invocation for the receipt token");
    let filled = printed.replace("<tsa-ca.pem>", &tsa_srv.ca_path());
    let argv: Vec<&str> = filled.split_whitespace().collect();
    let run = Command::new(argv[0])
        .current_dir(&kit)
        .args(&argv[1..])
        .output()?;
    assert!(
        run.status.success() && String::from_utf8_lossy(&run.stdout).contains("Verification: OK"),
        "`{filled}` -> stdout {} stderr {}",
        String::from_utf8_lossy(&run.stdout),
        String::from_utf8_lossy(&run.stderr)
    );
    Ok(())
}

#[test]
fn break_glass_policy_sentinel_and_later_receipt_heads_are_never_packaged() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let fixture_digest: [u8; 32] = Sha256::digest(b"securacv-fixture").into();

    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    let url = "https://tsa.example/tsr";
    tsa::insert_anchor(
        &kernel.conn,
        "break_glass_receipt_head",
        &fixture_digest,
        url,
        &fixture_token(),
    )?;
    tsa::insert_anchor(
        &kernel.conn,
        "policy_head",
        &fixture_digest,
        url,
        &fixture_token(),
    )?;
    let sentinel = witness_kernel::tsa::AnchorSubject::BreakGlassReceiptHead
        .empty_ledger_sentinel()
        .unwrap();
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &sentinel,
        url,
        &common::spliced_token(&sentinel),
    )?;
    let later = [0x77u8; 32];
    tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &later,
        url,
        &common::spliced_token(&later),
    )?;
    drop(kernel);

    let kit = temp.path().join("kit_private");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(!kit.join("anchors").exists(), "nothing may be packaged");
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["anchor_count"], 0);
    assert_eq!(manifest["receipt_anchored"], false);
    assert_eq!(manifest["anchors"].as_array().unwrap().len(), 0);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(!stderr.contains("excluded"), "{stderr}");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(!stdout.contains("break_glass"), "{stdout}");
    assert!(!stdout.contains("policy"), "{stdout}");
    assert!(!stdout.contains("sentinel"), "{stdout}");
    assert!(
        stdout.contains(
            "0 token(s), 0 covering the bundle bytes, 0 covering this disclosure's export receipt"
        ),
        "{stdout}"
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// Clock provenance: the device-signed record of every packaged token travels
// with it (`clock_provenance/clock-NNNN.json` beside the verbatim signed
// bytes), checked under the kit's lineage and bound by the packaged token's
// hash. Clock readings are injected, keyed to the fixture token's genTime.
// ---------------------------------------------------------------------------

const GEN: u64 = 1_781_094_804;

fn observe(db: &Path, wall: u64, gap: Option<u32>) -> usize {
    let mut kernel = Kernel::open(&test_cfg(db)).expect("open kernel");
    kernel
        .observe_anchors_for_clock_provenance(&witness_kernel::ClockObservation {
            observed_wall_s: wall,
            poll_gap_s: gap,
            kernel_version: "0.0.0-test",
        })
        .expect("observe")
        .len()
}

/// A database whose export-receipt head is anchored (so the token is
/// packaged) and measured +2 s by a running daemon; plus a digest anchor
/// that is not this kit's and has a record of its own. Returns db, bundle.
fn clock_kit_db(dir: &Path) -> Result<(std::path::PathBuf, std::path::PathBuf)> {
    let (db, bundle) = make_db_and_bundle(dir)?;
    let entry_hash = bundle_entry_hash(&bundle)?;
    assert_eq!(
        observe(&db, GEN, None),
        0,
        "first poll initializes the cursor"
    );
    let kernel = Kernel::open(&test_cfg(&db))?;
    tsa::ensure_anchor_table(&kernel.conn)?;
    tsa::insert_anchor(
        &kernel.conn,
        "export_receipt_head",
        &entry_hash,
        "https://tsa.example/tsr",
        &common::spliced_token(&entry_hash),
    )?;
    let other = [0x42u8; 32];
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &other,
        "https://tsa.example/tsr",
        &common::spliced_token(&other),
    )?;
    // Both rows stored in the bucket of the token's own time (a prompt
    // online anchoring), not at the test's wall-clock bucket.
    kernel.conn.execute(
        "UPDATE tsa_anchors SET created_bucket_start = 1781094600",
        [],
    )?;
    drop(kernel);
    assert_eq!(observe(&db, GEN + 2, Some(5)), 2);
    Ok((db, bundle))
}

fn export_ok(db: &Path, bundle: &Path, kit: &Path) -> std::process::Output {
    let out = run_court_export(db, bundle, kit);
    assert!(
        out.status.success(),
        "court_export failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

#[test]
fn clock_provenance_records_are_packaged_by_token_hash() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = clock_kit_db(temp.path())?;
    let kit = temp.path().join("kit");
    let out = export_ok(&db, &bundle, &kit);

    // Only the packaged token's record: #1 (the digest anchor's #2 is not
    // this kit's).
    let dir = kit.join("clock_provenance");
    let mut names: Vec<String> = std::fs::read_dir(&dir)?
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(names, vec!["clock-0001.json", "clock-0001.payload.json"]);

    // The payload file is the stored literal, byte for byte.
    let kernel = Kernel::open(&test_cfg(&db))?;
    let (stored_payload, prev, entry): (String, Vec<u8>, Vec<u8>) = kernel.conn.query_row(
        "SELECT payload_json, prev_hash, entry_hash FROM clock_provenance WHERE id = 1",
        [],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
    )?;
    drop(kernel);
    let payload = std::fs::read(dir.join("clock-0001.payload.json"))?;
    assert_eq!(payload, stored_payload.as_bytes());

    // Every fact re-derives from kit files alone: the token hash, the entry
    // hash over prev ‖ payload, and the Ed25519 signature under the domain.
    let token = std::fs::read(kit.join("anchors/anchor-0001-export_receipt_head.der"))?;
    let record: serde_json::Value = serde_json::from_slice(&payload)?;
    assert_eq!(record["token_sha256"], hex::encode(Sha256::digest(&token)));
    assert_eq!(record["observed_offset_s"], 2);
    assert_eq!(record["poll_gap_s"], 5);
    let sidecar: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(dir.join("clock-0001.json"))?)?;
    assert_eq!(sidecar["format"], "securacv-clock-provenance-record:v1");
    assert_eq!(
        sidecar["anchor_file"],
        "anchors/anchor-0001-export_receipt_head.der"
    );
    assert_eq!(
        sidecar["signing_domain"],
        "securacv:pwk:clock-provenance:v1"
    );
    assert_eq!(sidecar["prev_hash"], hex::encode(&prev));
    assert_eq!(prev, vec![0u8; 32], "the first record links to genesis");
    let mut h = Sha256::new();
    h.update(&prev);
    h.update(&payload);
    let derived: [u8; 32] = h.finalize().into();
    assert_eq!(derived.to_vec(), entry);
    assert_eq!(sidecar["entry_hash"], hex::encode(derived));
    let key_bytes: [u8; 32] = hex::decode(sidecar["signer_public_key"].as_str().unwrap())?
        .try_into()
        .unwrap();
    let key = ed25519_dalek::VerifyingKey::from_bytes(&key_bytes)?;
    let sig_bytes: [u8; 64] = hex::decode(sidecar["signature"]["ed25519"].as_str().unwrap())?
        .try_into()
        .unwrap();
    let message = witness_kernel::crypto::signatures::domain_separated_hash(
        "securacv:pwk:clock-provenance:v1",
        &derived,
    );
    key.verify_strict(&message, &ed25519_dalek::Signature::from_bytes(&sig_bytes))?;

    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        stdout.contains(
            "  clock:     1 clock-provenance record(s) packaged; ledger self-consistent; identity unverified"
        ),
        "{stdout}"
    );
    Ok(())
}

#[test]
fn clock_provenance_time_source_section_and_manifest_keys() -> Result<()> {
    let temp = tempfile::tempdir()?;
    // Absent: a database with no record says so and packages nothing.
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let kit = temp.path().join("kit_absent");
    export_ok(&db, &bundle, &kit);
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["clock_provenance_ledger"], "absent");
    assert!(!kit.join("clock_provenance").exists());
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("Clock-provenance ledger: **absent**"),
        "{custody}"
    );

    let temp = tempfile::tempdir()?;
    let (db, bundle) = clock_kit_db(temp.path())?;
    let kit = temp.path().join("kit");
    export_ok(&db, &bundle, &kit);
    let manifest = read_manifest(&kit)?;
    assert_eq!(
        manifest["clock_provenance_ledger"],
        "self-consistent; identity unverified"
    );
    let text = serde_json::to_string(&manifest)?;
    assert!(!text.contains("\"verified\""), "{text}");
    let anchor = &manifest["anchors"][0];
    assert_eq!(
        anchor["clock_provenance"]["file"],
        "clock_provenance/clock-0001.json"
    );
    assert_eq!(
        anchor["clock_provenance"]["payload_file"],
        "clock_provenance/clock-0001.payload.json"
    );
    assert_eq!(anchor["clock_provenance"]["record_id"], 1);
    assert_eq!(anchor["clock_provenance"]["observed_offset_s"], 2);
    assert_eq!(anchor["clock_provenance"]["poll_gap_s"], 5);
    assert_eq!(anchor["clock_provenance"]["binding_only"], false);
    assert_eq!(anchor["clock_provenance"]["signer_is_device_key"], true);
    let files = manifest["files"].to_string();
    assert!(
        files.contains("clock_provenance/clock-0001.json"),
        "{files}"
    );
    assert!(
        files.contains("clock_provenance/clock-0001.payload.json"),
        "{files}"
    );

    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(custody.contains("## Time source"), "{custody}");
    assert!(
        custody.contains(
            "an event's bucket under a device offset is the same width shifted — a bias correction adds no precision"
        ),
        "{custody}"
    );
    assert!(
        custody.contains("Clock-provenance ledger: **self-consistent; identity unverified**"),
        "{custody}"
    );
    assert!(
        custody.contains("binds `anchors/anchor-0001-export_receipt_head.der` by SHA-256"),
        "{custody}"
    );
    assert!(
        custody.contains("device clock +2 s from the token's time, the row first seen within 5 s of its storage (within one bucket (600 s)); the row was stored in the bucket of issuance."),
        "{custody}"
    );
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(
        verification.contains("## 3b. Verify the clock-provenance records"),
        "{verification}"
    );
    assert!(
        verification.contains("sha256sum anchors/anchor-0001-export_receipt_head.der"),
        "{verification}"
    );
    assert!(
        verification.contains(
            "openssl pkeyutl -verify -pubin -inkey device.pem -rawin -in msg.bin -sigfile sig.bin"
        ),
        "{verification}"
    );
    assert!(
        verification.contains("printf '\\040\\000\\000\\000securacv:pwk:clock-provenance:v1'"),
        "{verification}"
    );
    // No JSON parser, and no key read from the file under check: the device
    // key the custody record names is written into the recipe.
    assert!(!verification.contains("jq "), "{verification}");
    assert!(!verification.contains("<<<"), "{verification}");
    let device_key = manifest["evidence"]["device_public_key"]
        .as_str()
        .unwrap()
        .to_string();
    assert!(
        verification.contains(&format!(
            "printf %s {device_key} | xxd -r -p; }} | openssl pkey"
        )),
        "{verification}"
    );
    assert!(
        custody.contains(&device_key),
        "the recipe's key is the custody record's device key"
    );
    // The measured record's step 2 names its bucket and offset.
    assert!(
        verification.contains("G + (2) is what the device\n#       clock read, and must lie in [1781094600, 1781095200)"),
        "{verification}"
    );
    let readme = std::fs::read_to_string(kit.join("README.md"))?;
    assert!(readme.contains("7. `clock_provenance/`"), "{readme}");
    Ok(())
}

#[test]
fn clock_provenance_ledger_that_fails_packages_nothing() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = clock_kit_db(temp.path())?;
    let kernel = Kernel::open(&test_cfg(&db))?;
    kernel.conn.execute(
        "UPDATE clock_provenance SET payload_json = replace(payload_json, '\"observed_offset_s\":2', '\"observed_offset_s\":0') WHERE id = 1",
        [],
    )?;
    drop(kernel);
    let kit = temp.path().join("kit");
    let out = export_ok(&db, &bundle, &kit);
    assert!(!kit.join("clock_provenance").exists(), "fail closed");
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["clock_provenance_ledger"], "verification failed");
    assert_eq!(
        manifest["anchors"][0]["clock_provenance"],
        serde_json::Value::Null
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("WARNING: the clock-provenance ledger does not verify"),
        "{stderr}"
    );
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody.contains("Clock-provenance ledger: **verification failed**"),
        "{custody}"
    );
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(
        verification.contains("**The clock-provenance ledger did not verify at packaging time**"),
        "{verification}"
    );
    // The README agrees with the manifest and the custody record.
    let readme = std::fs::read_to_string(kit.join("README.md"))?;
    assert!(
        readme.contains("7. `clock_provenance/` — not packaged: the clock-provenance ledger did not verify when the kit was assembled"),
        "{readme}"
    );
    assert!(!readme.contains("recorded none"), "{readme}");
    Ok(())
}

#[test]
fn a_record_binding_a_replaced_token_is_withheld_and_named() -> Result<()> {
    // The packaged anchor's token is replaced after the device bound it:
    // the record is left out, named on stderr, counted in the README and
    // the custody record, and the manifest's per-anchor object is null.
    let temp = tempfile::tempdir()?;
    let (db, bundle) = clock_kit_db(temp.path())?;
    let entry_hash = bundle_entry_hash(&bundle)?;
    let kernel = Kernel::open(&test_cfg(&db))?;
    let mut der = common::spliced_token(&entry_hash).token_der;
    // Same imprint (the anchor's own line still holds), different bytes.
    let last = der.len() - 1;
    der[last] ^= 0x01;
    kernel
        .conn
        .execute("UPDATE tsa_anchors SET token_der = ?1 WHERE id = 1", [der])?;
    drop(kernel);
    let kit = temp.path().join("kit");
    let out = export_ok(&db, &bundle, &kit);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains(
            "WARNING: clock-provenance record #1 for anchor #1 is not packaged: it binds a token"
        ),
        "{stderr}"
    );
    assert!(!kit.join("clock_provenance").exists());
    let manifest = read_manifest(&kit)?;
    assert_eq!(
        manifest["clock_provenance_ledger"],
        "self-consistent; identity unverified"
    );
    assert_eq!(
        manifest["anchors"][0]["clock_provenance"],
        serde_json::Value::Null
    );
    let readme = std::fs::read_to_string(kit.join("README.md"))?;
    assert!(
        readme.contains("absent: no packaged token has a clock-provenance record that matches it"),
        "{readme}"
    );
    assert!(
        readme.contains(
            "1 record(s) were left out because they bind a token other than the one packaged"
        ),
        "{readme}"
    );
    assert!(!readme.contains("recorded none"), "{readme}");
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(
        custody
            .contains("1 clock-provenance record(s) bind a token other than the one packaged here"),
        "{custody}"
    );
    Ok(())
}

/// A POSIX `sh` with `xxd` on its PATH: the real one, or (when the host has
/// none) a stand-in for exactly the `xxd -r -p [file]` the recipe uses.
fn recipe_shell(bin: &Path) -> Option<std::process::Command> {
    let path = std::env::var("PATH").unwrap_or_default();
    let has = |tool: &str| {
        path.split(':')
            .any(|d| !d.is_empty() && Path::new(d).join(tool).is_file())
    };
    if !has("openssl") || !has("sh") {
        return None;
    }
    let mut path_var = path.clone();
    if !has("xxd") {
        if !has("python3") {
            return None;
        }
        std::fs::create_dir_all(bin).ok()?;
        let shim = bin.join("xxd");
        std::fs::write(
            &shim,
            "#!/bin/sh\n[ \"$1\" = -r ] && [ \"$2\" = -p ] || exit 2\nshift 2\nexec python3 -c 'import sys,binascii,re; d=open(sys.argv[1]).read() if len(sys.argv)>1 else sys.stdin.read(); sys.stdout.buffer.write(binascii.unhexlify(re.sub(r\"[^0-9a-fA-F]\",\"\",d)))' \"$@\"\n",
        )
        .ok()?;
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&shim, std::fs::Permissions::from_mode(0o755)).ok()?;
        path_var = format!("{}:{path}", bin.display());
    }
    let mut cmd = std::process::Command::new("sh");
    cmd.env("PATH", path_var);
    Some(cmd)
}

#[test]
fn the_clock_verification_recipe_runs_in_posix_sh() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let Some(mut sh) = recipe_shell(&temp.path().join("bin")) else {
        eprintln!("skipping: openssl or sh not available");
        return Ok(());
    };
    let (db, bundle) = clock_kit_db(temp.path())?;
    let kit = temp.path().join("kit");
    export_ok(&db, &bundle, &kit);
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    let section = verification
        .split("## 3b. Verify the clock-provenance records")
        .nth(1)
        .unwrap();
    let block = section
        .split("```sh\n")
        .nth(1)
        .and_then(|b| b.split("```").next())
        .expect("one sh block");
    let out = sh.arg("-c").arg(block).current_dir(&kit).output()?;
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    let sidecar: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(
        kit.join("clock_provenance/clock-0001.json"),
    )?)?;
    let entry = sidecar["entry_hash"].as_str().unwrap();
    assert!(
        stdout.contains(entry),
        "step 3 re-derives the entry hash\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stdout.contains("Signature Verified Successfully"),
        "step 5\nstdout: {stdout}\nstderr: {stderr}"
    );
    // The same recipe over altered payload bytes fails.
    let payload = kit.join("clock_provenance/clock-0001.payload.json");
    let altered =
        std::fs::read_to_string(&payload)?.replace("\"poll_gap_s\":5", "\"poll_gap_s\":1");
    std::fs::write(&payload, altered)?;
    let mut sh = recipe_shell(&temp.path().join("bin")).unwrap();
    let out = sh.arg("-c").arg(block).current_dir(&kit).output()?;
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(!stdout.contains(entry), "{stdout}");
    assert!(
        !stdout.contains("Signature Verified Successfully"),
        "{stdout}"
    );
    Ok(())
}

#[test]
fn clock_provenance_head_anchors_are_never_packaged() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = clock_kit_db(temp.path())?;
    let kernel = Kernel::open(&test_cfg(&db))?;
    let head = tsa::ledger_head(
        &kernel.conn,
        witness_kernel::tsa::AnchorSubject::ClockProvenanceHead,
    )?
    .expect("ledger head");
    tsa::insert_anchor(
        &kernel.conn,
        "clock_provenance_head",
        &head,
        "https://tsa.example/tsr",
        &common::spliced_token(&head),
    )?;
    drop(kernel);
    let kit = temp.path().join("kit");
    export_ok(&db, &bundle, &kit);
    let manifest = read_manifest(&kit)?;
    assert_eq!(manifest["anchor_count"], 1);
    let anchors: Vec<String> = std::fs::read_dir(kit.join("anchors"))?
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert_eq!(anchors, vec!["anchor-0001-export_receipt_head.der"]);
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(
        verification.contains("`clock_provenance_head` anchors that fix the ledger itself in time stay in the producing database"),
        "{verification}"
    );
    Ok(())
}

#[test]
fn step_2_follows_each_record_clamped_and_binding_only() -> Result<()> {
    // One packaged record per kit, observed (a) 5000 s after genTime — the
    // offset clamps at +3600, a bound — and (b) at a process's first poll —
    // binding only, no measurement.
    for (wall, gap, expect) in [
        (
            GEN + 5_000,
            Some(5),
            "The offset +3600 is the\n#       clamp bound: the device read at least G + 3600, so check only that\n#       1781100000 > G + 3600",
        ),
        (
            GEN + 2,
            None,
            "#    2. binding only: the record carries no measurement, so step 2 does not apply;\n#       steps 1 and 3-5 do",
        ),
    ] {
        let temp = tempfile::tempdir()?;
        let (db, bundle) = make_db_and_bundle(temp.path())?;
        let entry_hash = bundle_entry_hash(&bundle)?;
        assert_eq!(observe(&db, GEN, None), 0);
        let kernel = Kernel::open(&test_cfg(&db))?;
        tsa::ensure_anchor_table(&kernel.conn)?;
        tsa::insert_anchor(
            &kernel.conn,
            "export_receipt_head",
            &entry_hash,
            "https://tsa.example/tsr",
            &common::spliced_token(&entry_hash),
        )?;
        drop(kernel);
        assert_eq!(observe(&db, wall, gap), 1);
        let kit = temp.path().join("kit");
        export_ok(&db, &bundle, &kit);
        let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
        assert!(verification.contains(expect), "{verification}");
        assert!(
            !verification.contains("must lie in ["),
            "no in-bucket check for this record: {verification}"
        );
    }
    Ok(())
}

// -------------------- OpenTimestamps proofs in the kit --------------------

use witness_kernel::ots;

const OTS_BOB: &str = "https://bob.btc.calendar.opentimestamps.org";

/// A pending proof over `hash`, composed from the fixture calendar body.
fn ots_proof(hash: &[u8; 32], nonce_byte: u8) -> ots::Proof {
    let body = std::fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ots/response-bob.otsr"),
    )
    .unwrap();
    let nonce = [nonce_byte; ots::OTS_NONCE_LEN];
    let c = ots::commitment(hash, &nonce);
    let node = ots::parse_timestamp(&body, &c).unwrap();
    ots::Proof::compose(hash, &nonce, node).unwrap()
}

fn insert_ots_row(db: &Path, subject: tsa::AnchorSubject, proof: &ots::Proof) -> i64 {
    let kernel = Kernel::open(&test_cfg(db)).unwrap();
    ots::insert_ots_anchor(
        &kernel.conn,
        TimeBucket {
            start_epoch_s: 600,
            size_s: 600,
        },
        subject,
        &proof.digest,
        OTS_BOB,
        proof,
    )
    .unwrap()
}

#[test]
fn ots_proof_over_bundle_packaged() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();
    let proof = ots_proof(&bundle_sha, 1);
    let id = insert_ots_row(&db, tsa::AnchorSubject::Digest, &proof);
    let kit = temp.path().join("kit");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    let file = format!("ots-{id:04}-digest.ots");
    assert_eq!(std::fs::read(kit.join("anchors").join(&file))?, proof.to_bytes());
    let m = read_manifest(&kit)?;
    assert_eq!(m["ots_anchor_count"], 1);
    assert_eq!(m["ots_bundle_attestation"], "pending");
    assert_eq!(m["ots_receipt_attestation"], "none");
    let o = &m["ots_anchors"][0];
    assert_eq!(o["file"], format!("anchors/{file}"));
    assert_eq!(o["subject"], "digest");
    assert_eq!(o["covers"], "bundle_bytes");
    assert_eq!(o["calendar_url"], OTS_BOB);
    assert_eq!(o["attestation"], "pending");
    assert!(o["bitcoin_height"].is_null());
    assert!(o["merkle_root"].is_null());
    assert_eq!(o["pending_uris"][0], OTS_BOB);
    assert!(o.get("clock_provenance").is_none());
    // The RFC 3161 keys keep their exact meaning.
    assert_eq!(m["anchored"], false);
    assert_eq!(m["anchor_count"], 0);
    assert_eq!(m["receipt_anchored"], false);
    assert_eq!(m["anchors"].as_array().unwrap().len(), 0);
    assert!(m["files"]
        .as_array()
        .unwrap()
        .iter()
        .any(|f| f["path"] == format!("anchors/{file}")));
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(verification.contains(
        "The optional OpenTimestamps check in step 3 also needs the open-source `ots` client (`pip install opentimestamps-client`) and a Bitcoin node or block explorer you trust."
    ));
    assert!(verification.contains("### OpenTimestamps proofs (optional)"));
    assert!(verification.contains(&format!("ots info anchors/{file}")));
    assert!(verification.contains("This proof is pending (not yet in a Bitcoin block)"));
    let custody = std::fs::read_to_string(kit.join("CUSTODY_AND_CONTROL.md"))?;
    assert!(custody.contains(&format!(
        "- `anchors/{file}` — subject `digest`, OpenTimestamps proof via {OTS_BOB}; pending (not yet in a Bitcoin block) at {OTS_BOB} — **covers the evidence file's exact bytes**"
    )), "{custody}");
    let readme = std::fs::read_to_string(kit.join("README.md"))?;
    assert!(readme.contains("OpenTimestamps proofs in `anchors/*.ots` tie the same digests to the Bitcoin blockchain"));
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(stdout.contains("  ots:       1 proof(s), 0 attest a Bitcoin block, 1 pending"), "{stdout}");
    // An OTS proof never silences the RFC 3161 warning.
    assert!(stdout.contains("WARNING: no RFC 3161 anchor covers"));
    for doc in [&verification, &custody, &readme] {
        assert!(!doc.contains("confirmed in Bitcoin"), "{doc}");
    }

    if common::ots_available() {
        for f in [kit.join("anchors").join(&file)] {
            let o = Command::new("ots").arg("info").arg(&f).output()?;
            assert!(o.status.success(), "{}", String::from_utf8_lossy(&o.stderr));
        }
    }
    Ok(())
}

#[test]
fn ots_excluded_rows() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let sentinel = tsa::AnchorSubject::PolicyHead.empty_ledger_sentinel().unwrap();
    insert_ots_row(&db, tsa::AnchorSubject::Digest, &ots_proof(&sentinel, 1));
    insert_ots_row(&db, tsa::AnchorSubject::Digest, &ots_proof(&[9u8; 32], 2));
    let gone = insert_ots_row(&db, tsa::AnchorSubject::ChainHead, &ots_proof(&[8u8; 32], 3));
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();
    let broken = insert_ots_row(&db, tsa::AnchorSubject::Digest, &ots_proof(&bundle_sha, 4));
    {
        let kernel = Kernel::open(&test_cfg(&db))?;
        kernel
            .conn
            .execute("UPDATE ots_anchors SET proof = x'00' WHERE id = ?1", [broken])?;
    }
    let kit = temp.path().join("kit");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    let m = read_manifest(&kit)?;
    assert_eq!(m["ots_anchor_count"], 0);
    assert_eq!(m["ots_bundle_attestation"], "none");
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(err.contains(&format!(
        "WARNING: ots anchor #{gone} excluded from the kit: it claims to anchor a chain head, but the anchored hash is not in this database's chain history."
    )), "{err}");
    assert!(err.contains(&format!(
        "WARNING: ots anchor #{broken} excluded from the kit: its stored proof does not parse as an OpenTimestamps proof ("
    )), "{err}");
    assert_eq!(err.matches("WARNING: ots anchor").count(), 2, "sentinel and foreign digest stay out silently: {err}");
    assert!(!kit.join("anchors").exists() || std::fs::read_dir(kit.join("anchors"))?.next().is_none());
    Ok(())
}

#[test]
fn kit_without_ots_rows_has_constant_zero_keys() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;
    let kit = temp.path().join("kit");
    let out = run_court_export(&db, &bundle, &kit);
    assert!(out.status.success());
    let m = read_manifest(&kit)?;
    assert_eq!(m["ots_anchor_count"], 0);
    assert_eq!(m["ots_bundle_attestation"], "none");
    assert_eq!(m["ots_receipt_attestation"], "none");
    assert_eq!(m["ots_anchors"], serde_json::json!([]));
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(!verification.contains("OpenTimestamps"));
    let readme = std::fs::read_to_string(kit.join("README.md"))?;
    assert!(!readme.contains("OpenTimestamps"));
    let kernel = Kernel::open(&test_cfg(&db))?;
    assert!(!ots::ots_table_exists(&kernel.conn)?, "court_export never creates it");
    Ok(())
}

/// Cross-implementation check, skipped when the Python `ots` client is not
/// installed: `ots info` reads the fixtures and a proof our codec composed.
/// `ots info` makes no network call.
#[test]
fn ots_client_reads_fixtures_and_our_proofs() -> Result<()> {
    if !common::ots_available() {
        eprintln!("skipping: the `ots` client is not on PATH");
        return Ok(());
    }
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ots");
    for f in ["pending.ots", "bitcoin.ots", "pending-bob.ots", "genesis.ots"] {
        let o = Command::new("ots").arg("info").arg(dir.join(f)).output()?;
        assert!(o.status.success(), "{f}: {}", String::from_utf8_lossy(&o.stderr));
        if f == "bitcoin.ots" {
            let text = String::from_utf8_lossy(&o.stdout);
            assert!(text.contains("BitcoinBlockHeaderAttestation(449399)"), "{text}");
            assert!(text.contains("https://alice.btc.calendar.opentimestamps.org"));
            assert!(text.contains("https://bob.btc.calendar.opentimestamps.org"));
        }
    }
    let temp = tempfile::tempdir()?;
    let ours = temp.path().join("ours.ots");
    std::fs::write(&ours, ots_proof(&[0x5au8; 32], 7).to_bytes())?;
    let o = Command::new("ots").arg("info").arg(&ours).output()?;
    assert!(o.status.success(), "{}", String::from_utf8_lossy(&o.stderr));
    Ok(())
}
