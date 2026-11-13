//! `export_verify` driven as the real binary. Its last line has to say what
//! the verdict rests on, by the rule `log_verify` applies (its B3 label): a
//! key from `--public-key`, `--public-key-file`, `--device-key-seed` or
//! `DEVICE_KEY_SEED` anchors identity; with only `--db-key`, or with the seed
//! file beside the database (which only opens it), the verifying key is read
//! from the database under audit. A database rewritten wholesale under another
//! key passes that way too, so the run must say `SELF-CONSISTENT, IDENTITY
//! UNVERIFIED`, never "verified" bare.
//!
//! Every row `check_rows` takes runs `log_verify` on the same database with
//! the same key source and requires the same label from it, so the two
//! verifiers cannot drift apart on what anchors identity. Two runs sit outside
//! it: the rotated database's current seed, the one documented divergence
//! (`log_verify` verifies self-anchored, `export_verify` refuses; the test
//! holds only that it never reads as anchored), and the C2PA test, which
//! drives `export_verify` alone because `log_verify` has no sidecar flags.

use anyhow::Result;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use witness_kernel::{
    CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy,
};

const GENUINE_SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";
/// Someone who rewrites the whole database under a key of their own: its
/// `device_metadata.public_key`, every event and every export receipt.
const FORGER_SEED: &str = "devkey:test:ffffffffffffffffffff";
/// The key a rotated database's genesis identity hands over to.
const SUCCESSOR_SEED: &str = "devkey:test:successor-00112233445566";

const ANCHORED_LINE: &str = "OK: export bundle hash verified (identity anchored out-of-band).";
const SELF_CONSISTENT_LINE: &str = "OK: export bundle hash matches the receipt chain — \
     SELF-CONSISTENT, IDENTITY UNVERIFIED. The verifying key was taken from the database under \
     audit, so this proves internal consistency, not identity. Supply --public-key / \
     --public-key-file / --device-key-seed from an out-of-band source for an evidentiary \
     verdict.";
/// What `export_verify` printed on every pass before it labeled the verdict.
const OLD_UNLABELED_LINE: &str = "OK: export bundle hash verified.";

/// Variables a developer's shell could hand the children that would change
/// which key they use. `SECURACV_DB_KEY_SEED` is left alone: the kernel that
/// builds these databases and the children that open them read it the same
/// way, and `db_key` below derives through it too.
const CONTROLLED_ENV: &[&str] = &["DEVICE_KEY_SEED", "SECURACV_DB_KEY"];

fn cfg(db_path: &Path, seed: &str) -> KernelConfig {
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

fn add_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) -> Result<()> {
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
        zone_id: zone.to_string(),
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

/// One database with one event and one self-exported bundle, written by the
/// kernel under `seed`, optionally rotated to `rotate_to` before the export
/// (so the export receipt is signed by the successor key).
fn make_db_and_bundle(
    dir: &Path,
    seed: &str,
    zone: &str,
    rotate_to: Option<&str>,
) -> Result<(PathBuf, PathBuf)> {
    std::fs::create_dir_all(dir)?;
    let db_path = dir.join("witness.db");
    let cfg = cfg(&db_path, seed);
    let mut kernel = Kernel::open(&cfg)?;
    add_event(&mut kernel, &cfg, zone)?;
    if let Some(successor) = rotate_to {
        kernel.rotate_device_identity(successor)?;
        add_event(&mut kernel, &cfg, zone)?;
    }
    let bundle = kernel.export_events_bundle_self(cfg.ruleset_hash, ExportOptions::default())?;
    let bundle_path = dir.join("witness_export.json");
    std::fs::write(&bundle_path, serde_json::to_vec(&bundle)?)?;
    drop(kernel);
    Ok((db_path, bundle_path))
}

/// The SQLCipher key the kernel derived for a database opened under `seed`.
fn db_key(seed: &str) -> String {
    let signing_key = witness_kernel::signing_key_from_seed(seed).expect("seed derives a key");
    let seed_env = witness_kernel::db_key_seed_from_env();
    witness_kernel::resolve_db_encryption_key(&signing_key, seed_env.as_ref().map(|s| s.as_str()))
        .to_string()
}

fn pubkey_hex(seed: &str) -> String {
    hex::encode(
        witness_kernel::verifying_key_from_seed(seed)
            .expect("seed derives a key")
            .to_bytes(),
    )
}

fn seed_file_beside(db: &Path) -> PathBuf {
    witness_kernel::crypto::device_key_path_for_db(db.to_str().expect("utf8 path"))
        .expect("seed path")
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Verdict {
    /// Exit 0 and the identity-anchored label.
    Anchored,
    /// Exit 0 and `SELF-CONSISTENT, IDENTITY UNVERIFIED`.
    SelfConsistent,
    /// Exit 1 and no `OK:` line.
    Refused,
}

/// One key source, as an operator would pass it.
struct KeySource {
    name: &'static str,
    args: Vec<String>,
    /// `DEVICE_KEY_SEED` in the child's environment.
    env_seed: Option<&'static str>,
    /// A seed file written beside the database (mode 0600) for this row only.
    seed_file: Option<&'static str>,
}

impl KeySource {
    fn new(name: &'static str, args: &[&str]) -> Self {
        Self {
            name,
            args: args.iter().map(|s| s.to_string()).collect(),
            env_seed: None,
            seed_file: None,
        }
    }
    fn env_seed(mut self, seed: &'static str) -> Self {
        self.env_seed = Some(seed);
        self
    }
    fn seed_file(mut self, seed: &'static str) -> Self {
        self.seed_file = Some(seed);
        self
    }
}

fn run(bin: &str, db: &Path, bundle: Option<&Path>, src: &KeySource) -> Output {
    let mut cmd = Command::new(bin);
    cmd.arg("--db").arg(db);
    if let Some(bundle) = bundle {
        cmd.arg("--bundle").arg(bundle);
    }
    cmd.args(&src.args).args(["--ui", "plain"]);
    for var in CONTROLLED_ENV {
        cmd.env_remove(var);
    }
    if let Some(seed) = src.env_seed {
        cmd.env("DEVICE_KEY_SEED", seed);
    }
    cmd.output().expect("verifier runs")
}

fn transcript(label: &str, out: &Output) -> String {
    format!(
        "{label}: exit {:?}\n--- stdout\n{}--- stderr\n{}",
        out.status.code(),
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    )
}

/// The verdict `export_verify` gave, checked line for line.
fn export_verdict(out: &Output) -> Verdict {
    let stdout = String::from_utf8_lossy(&out.stdout);
    let has_line = |want: &str| stdout.lines().any(|l| l == want);
    let why = transcript("export_verify", out);
    assert!(
        !has_line(OLD_UNLABELED_LINE),
        "the unlabeled verdict line is gone on every path\n{why}"
    );
    match out.status.code() {
        Some(0) => {
            // The per-ledger line stays as log_verify keeps its own.
            assert!(
                has_line("verified 1 export receipt entries"),
                "per-ledger line missing\n{why}"
            );
            match (has_line(ANCHORED_LINE), has_line(SELF_CONSISTENT_LINE)) {
                (true, false) => Verdict::Anchored,
                (false, true) => {
                    assert!(
                        !stdout.contains("identity anchored"),
                        "a self-consistent run must not mention an anchor\n{why}"
                    );
                    Verdict::SelfConsistent
                }
                _ => panic!("exactly one labeled OK line expected\n{why}"),
            }
        }
        Some(1) => {
            assert!(
                !stdout.lines().any(|l| l.starts_with("OK:")),
                "a refusal prints no OK line\n{why}"
            );
            Verdict::Refused
        }
        other => panic!("unexpected exit {other:?}\n{why}"),
    }
}

/// The verdict `log_verify` gave on the same database and key source.
fn log_verdict(out: &Output) -> Verdict {
    let stdout = String::from_utf8_lossy(&out.stdout);
    let why = transcript("log_verify", out);
    match out.status.code() {
        Some(0) => match (
            stdout.contains("OK: all chains verified — VALID (identity anchored out-of-band)."),
            stdout.contains("OK: all chains verified — SELF-CONSISTENT, IDENTITY UNVERIFIED."),
        ) {
            (true, false) => Verdict::Anchored,
            (false, true) => Verdict::SelfConsistent,
            _ => panic!("exactly one labeled OK line expected\n{why}"),
        },
        Some(1) => Verdict::Refused,
        other => panic!("unexpected exit {other:?}\n{why}"),
    }
}

/// Run both verifiers with `src` and return their verdicts.
fn verdicts(db: &Path, bundle: &Path, src: &KeySource) -> Result<(Verdict, Verdict)> {
    let seed_path = seed_file_beside(db);
    assert!(
        !seed_path.exists(),
        "{}: a stray seed file beside the database",
        src.name
    );
    if let Some(seed) = src.seed_file {
        witness_kernel::crypto::load_or_create_device_seed(&seed_path, Some(seed))?;
    }
    let export = run(env!("CARGO_BIN_EXE_export_verify"), db, Some(bundle), src);
    let log = run(env!("CARGO_BIN_EXE_log_verify"), db, None, src);
    if src.seed_file.is_some() {
        std::fs::remove_file(&seed_path)?;
    }
    let (export, log) = (export_verdict(&export), log_verdict(&log));
    // The matrix, for `cargo test -- --nocapture`.
    println!(
        "{:<52} export_verify {export:?}, log_verify {log:?}",
        src.name
    );
    Ok((export, log))
}

fn check_rows(db: &Path, bundle: &Path, rows: &[(KeySource, Verdict)]) -> Result<()> {
    for (src, want) in rows {
        let (export, log) = verdicts(db, bundle, src)?;
        assert_eq!(export, *want, "export_verify, {}", src.name);
        assert_eq!(
            log, *want,
            "log_verify disagrees with export_verify, {}",
            src.name
        );
    }
    Ok(())
}

#[test]
fn genuine_database_labels_each_key_source_as_log_verify_does() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(
        &temp.path().join("genuine"),
        GENUINE_SEED,
        "zone:front",
        None,
    )?;
    let key = pubkey_hex(GENUINE_SEED);
    let key_file = temp.path().join("genuine.pub");
    std::fs::write(&key_file, format!("{key}\n"))?;
    let key_file = key_file.to_string_lossy().to_string();
    let dbk = db_key(GENUINE_SEED);

    use Verdict::*;
    let rows = [
        (
            KeySource::new("--public-key", &["--public-key", &key, "--db-key", &dbk]),
            Anchored,
        ),
        (
            KeySource::new(
                "--public-key-file",
                &["--public-key-file", &key_file, "--db-key", &dbk],
            ),
            Anchored,
        ),
        (
            KeySource::new("--device-key-seed", &["--device-key-seed", GENUINE_SEED]),
            Anchored,
        ),
        (
            KeySource::new("DEVICE_KEY_SEED", &[]).env_seed(GENUINE_SEED),
            Anchored,
        ),
        (
            KeySource::new(
                "--public-key-file with a seed (the file is the key)",
                &[
                    "--public-key-file",
                    &key_file,
                    "--device-key-seed",
                    GENUINE_SEED,
                ],
            ),
            Anchored,
        ),
        // The seed file opens the database and nothing more: the key is the
        // database's own.
        (
            KeySource::new("the seed file beside the database", &[]).seed_file(GENUINE_SEED),
            SelfConsistent,
        ),
        (
            KeySource::new("--db-key only", &["--db-key", &dbk]),
            SelfConsistent,
        ),
        // Nothing to open the database with: both refuse before verifying.
        (KeySource::new("no key at all", &[]), Refused),
    ];
    check_rows(&db, &bundle, &rows)
}

#[test]
fn forged_database_passes_only_as_self_consistent_and_a_pinned_key_refuses_it() -> Result<()> {
    let temp = tempfile::tempdir()?;
    // A different device's history under a different key: an unpinned run
    // cannot tell it from the genuine one, which is why it must not say
    // "verified".
    let (db, bundle) = make_db_and_bundle(
        &temp.path().join("forged"),
        FORGER_SEED,
        "zone:backyard",
        None,
    )?;
    let genuine_key = pubkey_hex(GENUINE_SEED);
    let genuine_key_file = temp.path().join("genuine.pub");
    std::fs::write(&genuine_key_file, &genuine_key)?;
    let genuine_key_file = genuine_key_file.to_string_lossy().to_string();
    let forged_dbk = db_key(FORGER_SEED);

    use Verdict::*;
    let rows = [
        (
            KeySource::new("--db-key only", &["--db-key", &forged_dbk]),
            SelfConsistent,
        ),
        (
            KeySource::new("the forger's seed file beside the database", &[])
                .seed_file(FORGER_SEED),
            SelfConsistent,
        ),
        (
            KeySource::new(
                "the genuine --public-key",
                &["--public-key", &genuine_key, "--db-key", &forged_dbk],
            ),
            Refused,
        ),
        (
            KeySource::new(
                "the genuine --public-key-file",
                &[
                    "--public-key-file",
                    &genuine_key_file,
                    "--db-key",
                    &forged_dbk,
                ],
            ),
            Refused,
        ),
        (
            KeySource::new(
                "the genuine --device-key-seed",
                &["--device-key-seed", GENUINE_SEED, "--db-key", &forged_dbk],
            ),
            Refused,
        ),
        (
            KeySource::new("the genuine DEVICE_KEY_SEED", &["--db-key", &forged_dbk])
                .env_seed(GENUINE_SEED),
            Refused,
        ),
    ];
    check_rows(&db, &bundle, &rows)
}

#[test]
fn rotated_database_current_seed_never_reads_as_anchored() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(
        &temp.path().join("rotated"),
        GENUINE_SEED,
        "zone:front",
        Some(SUCCESSOR_SEED),
    )?;
    // Rotation leaves the database keyed by the genesis key.
    let dbk = db_key(GENUINE_SEED);
    let genesis_key = pubkey_hex(GENUINE_SEED);

    use Verdict::*;
    check_rows(
        &db,
        &bundle,
        &[
            (
                KeySource::new(
                    "the genesis --public-key",
                    &["--public-key", &genesis_key, "--db-key", &dbk],
                ),
                Anchored,
            ),
            (
                KeySource::new(
                    "the genesis --device-key-seed",
                    &["--device-key-seed", GENUINE_SEED],
                ),
                Anchored,
            ),
            (
                KeySource::new("--db-key only", &["--db-key", &dbk]),
                SelfConsistent,
            ),
        ],
    )?;

    // The current seed derives a later epoch's key. log_verify notices and
    // verifies self-anchored; export_verify has no such fallback yet and
    // refuses on the lineage check (the open decision in
    // docs/db_key_rotation.md, "Verifier CLIs"). Either way it must never
    // claim an anchor the seed does not give it.
    let current_seed = KeySource::new(
        "the current (post-rotation) --device-key-seed",
        &["--device-key-seed", SUCCESSOR_SEED, "--db-key", &dbk],
    );
    let (export, log) = verdicts(&db, &bundle, &current_seed)?;
    assert_ne!(export, Anchored, "export_verify, {}", current_seed.name);
    assert_eq!(log, SelfConsistent, "log_verify, {}", current_seed.name);
    Ok(())
}

/// Sign a C2PA sidecar over `bundle` with the credentials `seed` derives, the
/// way `export_events --c2pa` does, and write it plus that seed's device CA.
#[cfg(feature = "c2pa-export")]
fn sign_sidecar(bundle: &Path, seed: &str) -> Result<(PathBuf, PathBuf)> {
    use witness_kernel::c2pa_export;
    let bytes = std::fs::read(bundle)?;
    let parsed: witness_kernel::ExportBundle = serde_json::from_slice(&bytes)?;
    let credentials = c2pa_export::credentials_from_seed(seed)?;
    let binding = c2pa_export::WitnessBinding {
        receipt_entry_hash: hex::encode(parsed.receipt_entry.entry_hash),
        artifact_hash: hex::encode(parsed.receipt_entry.receipt.artifact_hash),
        device_public_key: pubkey_hex(seed),
        auth_mode: "self_export".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        kernel_version: "0.0.0-test".to_string(),
    };
    let manifest = c2pa_export::sign_export_sidecar(&bytes, &credentials, &binding, "test export")?;
    let manifest_path = bundle.with_extension("json.c2pa");
    std::fs::write(&manifest_path, manifest)?;
    let anchor_path = bundle.with_extension("ca.pem");
    std::fs::write(&anchor_path, credentials.ca_cert_pem.as_bytes())?;
    Ok((manifest_path, anchor_path))
}

/// A trusted `--c2pa-anchor` binds the sidecar to the anchor's holder, but the
/// receipt chain it points into is still checked against whatever key the run
/// resolved. With no Ed25519 key source that is the database's own, so the
/// verdict stays SELF-CONSISTENT; a pinned key or the seed (which derives the
/// anchor and the key both) anchors it.
#[cfg(feature = "c2pa-export")]
#[test]
fn c2pa_anchor_alone_leaves_the_receipt_chain_self_consistent() -> Result<()> {
    let temp = tempfile::tempdir()?;
    let (db, bundle) = make_db_and_bundle(
        &temp.path().join("genuine"),
        GENUINE_SEED,
        "zone:front",
        None,
    )?;
    let (manifest, anchor) = sign_sidecar(&bundle, GENUINE_SEED)?;
    let manifest = manifest.to_string_lossy().to_string();
    let anchor = anchor.to_string_lossy().to_string();
    let dbk = db_key(GENUINE_SEED);
    let key = pubkey_hex(GENUINE_SEED);

    let rows = [
        (
            KeySource::new(
                "--c2pa-anchor with only --db-key",
                &[
                    "--c2pa-manifest",
                    &manifest,
                    "--c2pa-anchor",
                    &anchor,
                    "--db-key",
                    &dbk,
                ],
            ),
            Verdict::SelfConsistent,
        ),
        (
            KeySource::new(
                "--c2pa-anchor with --public-key",
                &[
                    "--c2pa-manifest",
                    &manifest,
                    "--c2pa-anchor",
                    &anchor,
                    "--public-key",
                    &key,
                    "--db-key",
                    &dbk,
                ],
            ),
            Verdict::Anchored,
        ),
        (
            KeySource::new(
                "the anchor derived from --device-key-seed",
                &[
                    "--c2pa-manifest",
                    &manifest,
                    "--device-key-seed",
                    GENUINE_SEED,
                ],
            ),
            Verdict::Anchored,
        ),
    ];
    for (src, want) in &rows {
        let out = run(
            env!("CARGO_BIN_EXE_export_verify"),
            &db,
            Some(bundle.as_path()),
            src,
        );
        assert_eq!(export_verdict(&out), *want, "export_verify, {}", src.name);
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(
            stdout.contains("c2pa: manifest valid, state Trusted")
                && stdout.contains(
                    "c2pa: witness binding matches this bundle's receipt in the chain checked above"
                ),
            "{}: the sidecar lines\n{}",
            src.name,
            transcript("export_verify", &out)
        );
    }

    // A forged database and bundle with the forger's own sidecar: well formed,
    // but it does not chain to the genuine anchor, so the run refuses.
    let (forged_db, forged_bundle) = make_db_and_bundle(
        &temp.path().join("forged"),
        FORGER_SEED,
        "zone:backyard",
        None,
    )?;
    let (forged_manifest, _) = sign_sidecar(&forged_bundle, FORGER_SEED)?;
    let forged_manifest = forged_manifest.to_string_lossy().to_string();
    let forged_dbk = db_key(FORGER_SEED);
    let src = KeySource::new(
        "a forged sidecar under the genuine --c2pa-anchor",
        &[
            "--c2pa-manifest",
            &forged_manifest,
            "--c2pa-anchor",
            &anchor,
            "--db-key",
            &forged_dbk,
        ],
    );
    let out = run(
        env!("CARGO_BIN_EXE_export_verify"),
        &forged_db,
        Some(forged_bundle.as_path()),
        &src,
    );
    assert_eq!(
        export_verdict(&out),
        Verdict::Refused,
        "export_verify, {}",
        src.name
    );
    Ok(())
}
