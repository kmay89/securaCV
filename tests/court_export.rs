//! End-to-end tests for the `court_export` disclosure-kit assembler: it must
//! package only verified, chain-anchored evidence (fail closed on tampering or
//! a foreign database), attest every kit file by hash in the manifest, and
//! say honestly whether any RFC 3161 token covers the evidence bytes.

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
