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

#[test]
fn digest_anchor_is_packaged_and_marked_as_covering_the_bundle() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(temp.path())?;

    // Record an anchor over the exact bundle bytes, the way
    // `log_anchor request --digest` does. The token bytes are opaque to the
    // packager (verification is openssl's job); what matters is the subject
    // hash binding.
    let bundle_sha: [u8; 32] = Sha256::digest(std::fs::read(&bundle)?).into();
    let cfg = test_cfg(&db);
    let kernel = Kernel::open(&cfg)?;
    let token = tsa::TimestampToken {
        status: 0,
        gen_time: "20260818000000Z".to_string(),
        policy_oid: "1.2.3.4".to_string(),
        serial_hex: "01".to_string(),
        imprint: bundle_sha.to_vec(),
        nonce: None,
        token_der: b"test-token-der".to_vec(),
    };
    tsa::ensure_anchor_table(&kernel.conn)?;
    tsa::insert_anchor(
        &kernel.conn,
        "digest",
        &bundle_sha,
        "https://tsa.example/tsr",
        &token,
    )?;
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
    assert_eq!(manifest["anchor_count"], 1);

    // The token bytes travel verbatim, and the verification instructions name
    // the exact openssl invocation for them.
    let anchor_files: Vec<_> = std::fs::read_dir(kit.join("anchors"))?
        .map(|e| e.unwrap().path())
        .collect();
    assert_eq!(anchor_files.len(), 1);
    assert_eq!(std::fs::read(&anchor_files[0])?, b"test-token-der");
    let verification = std::fs::read_to_string(kit.join("VERIFICATION.md"))?;
    assert!(verification.contains("openssl ts -verify -digest"));
    assert!(!String::from_utf8_lossy(&out.stdout).contains("WARNING"));
    Ok(())
}
