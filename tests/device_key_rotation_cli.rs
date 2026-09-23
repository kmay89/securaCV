//! The operator-facing device-key ceremony (CEREMONY_RUNBOOK C7):
//! `break_glass rotate-identity` and `break_glass rekey-db`, driven as the
//! real binaries with the environment an operator would have. Every
//! assertion about output also checks the negative space: no seed and no
//! key material may ever appear on stdout or stderr.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::time::Duration;

use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy,
};

const GENESIS_SEED: &str = "devkey:rotation-cli:genesis-00112233445566";
const SUCCESSOR_SEED: &str = "devkey:rotation-cli:successor-998877665544";
const DB_SECRET: &str = "independent-db-key-secret-for-the-rotation-test";
const DB_SECRET_2: &str = "second-independent-db-key-secret-after-rotation";

/// Every variable a child could inherit from the developer's shell and that
/// would change what these commands do. Removed on every spawn so the test
/// controls each one explicitly.
const CONTROLLED_ENV: &[&str] = &[
    "DEVICE_KEY_SEED",
    "NEW_DEVICE_KEY_SEED",
    "SECURACV_DB_KEY_SEED",
    "SECURACV_NEW_DB_KEY_SEED",
    "SECURACV_DB_KEY",
    "SECURACV_HWM_PATH",
    "RUST_LOG",
];

/// One event-contract line, as a Grove Vision 2 module would emit it.
const EVENT_LINE: &str = r#"{"event_type":"boundary_crossing_object_large","time_bucket":{"start_epoch_s":1200,"size_s":600},"zone_id":"zone:a","confidence":0.5}"#;

fn cfg(db: &Path, seed: &str) -> KernelConfig {
    KernelConfig {
        db_path: db.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: seed.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig) {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    kernel
        .append_event_checked(
            &desc,
            CandidateEvent {
                event_type: EventType::BoundaryCrossingObjectLarge,
                time_bucket: TimeBucket {
                    start_epoch_s: 600,
                    size_s: 600,
                },
                zone_id: "zone:a".to_string(),
                confidence: 0.5,
                correlation_token: None,
                attestation: None,
            },
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("seal event");
}

/// A log the way witnessd leaves it: one sealed event and a checkpoint under
/// the genesis identity, database key derived from the signing key.
fn build_db(db: &Path) {
    let cfg = cfg(db, GENESIS_SEED);
    let mut kernel = Kernel::open(&cfg).expect("open kernel");
    seal_event(&mut kernel, &cfg);
    kernel
        .enforce_retention_with_checkpoint(Duration::from_secs(0))
        .expect("checkpoint");
}

fn seed_file_for(db: &Path) -> PathBuf {
    witness_kernel::crypto::device_key_path_for_db(db.to_str().expect("utf8 path"))
        .expect("seed path")
}

/// The seed file witnessd would have written on first start (mode 0600).
fn write_genesis_seed_file(db: &Path) -> PathBuf {
    let path = seed_file_for(db);
    witness_kernel::crypto::load_or_create_device_seed(&path, Some(GENESIS_SEED))
        .expect("write seed file");
    path
}

fn read_seed(path: &Path) -> String {
    std::fs::read_to_string(path)
        .expect("read seed file")
        .trim()
        .to_string()
}

#[cfg(unix)]
fn mode_of(path: &Path) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).expect("stat").permissions().mode() & 0o777
}

fn command(bin: &str, args: &[&str], envs: &[(&str, &str)]) -> Command {
    let mut cmd = Command::new(bin);
    cmd.args(args);
    for var in CONTROLLED_ENV {
        cmd.env_remove(var);
    }
    cmd.envs(envs.iter().copied());
    cmd
}

fn run(bin: &str, args: &[&str], envs: &[(&str, &str)]) -> Output {
    command(bin, args, envs)
        .stdin(Stdio::null())
        .output()
        .expect("spawn binary")
}

/// A write-side daemon fed one event on stdin (it exits at EOF). It resolves
/// its seed the way every write-side process does: DEVICE_KEY_SEED, else the
/// seed file beside the database, else a freshly generated one.
fn grove_ingest(db: &str, envs: &[(&str, &str)]) -> Output {
    let mut envs = envs.to_vec();
    envs.push(("RUST_LOG", "info"));
    let mut child = command(
        env!("CARGO_BIN_EXE_grove_vision2_ingest"),
        &["--db-path", db, "--ruleset-id", "ruleset:test"],
        &envs,
    )
    .stdin(Stdio::piped())
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .expect("spawn grove_vision2_ingest");
    {
        let mut stdin = child.stdin.take().expect("stdin");
        writeln!(stdin, "{EVENT_LINE}").expect("write event line");
    }
    child.wait_with_output().expect("wait grove_vision2_ingest")
}

fn break_glass(args: &[&str], envs: &[(&str, &str)]) -> Output {
    run(env!("CARGO_BIN_EXE_break_glass"), args, envs)
}

fn log_verify(args: &[&str], envs: &[(&str, &str)]) -> Output {
    run(env!("CARGO_BIN_EXE_log_verify"), args, envs)
}

fn export_events(args: &[&str], envs: &[(&str, &str)]) -> Output {
    run(env!("CARGO_BIN_EXE_export_events"), args, envs)
}

fn text(output: &Output) -> String {
    format!(
        "--- stdout ---\n{}\n--- stderr ---\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    )
}

/// No seed and no DB-key material may appear on either stream, ever.
fn assert_no_secrets(output: &Output, secrets: &[&str]) {
    let all = text(output);
    for secret in secrets {
        assert!(
            !all.contains(secret),
            "output leaked secret material\n{all}"
        );
    }
    assert!(
        !all.contains("devkey:"),
        "output must never carry a device seed\n{all}"
    );
}

fn genesis_public_key_hex() -> String {
    hex::encode(
        witness_kernel::verifying_key_from_seed(GENESIS_SEED)
            .expect("genesis key")
            .to_bytes(),
    )
}

#[test]
fn rotate_identity_generate_rotates_replaces_seed_file_and_keeps_the_log_verifiable() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    let seed_file = write_genesis_seed_file(&db);
    let db_str = db.to_str().expect("utf8 path");

    // The retiring seed is taken from the seed file (no DEVICE_KEY_SEED); the
    // database key is decoupled in the same ceremony (--rekey-db-to).
    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
            "--generate",
            "--rekey-db-to",
            DB_SECRET,
        ],
        &[],
    );
    assert!(
        out.status.success(),
        "rotate-identity failed\n{}",
        text(&out)
    );
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        stdout.contains(&format!(
            "retiring public key: {}",
            genesis_public_key_hex()
        )),
        "{stdout}"
    );
    assert!(stdout.contains("current public key:"), "{stdout}");
    assert!(
        stdout.contains(&format!(
            "genesis public key:  {}",
            genesis_public_key_hex()
        )),
        "the ceremony prints the pin verifiers need\n{stdout}"
    );
    assert!(stdout.contains("lineage epoch 1"), "{stdout}");
    assert!(
        stdout.contains("seed file:") && stdout.contains("replaced"),
        "{stdout}"
    );

    // The seed file now holds a fresh successor, still 0600, no staging file left.
    let successor = read_seed(&seed_file);
    assert!(successor.starts_with("devkey:"), "successor format");
    assert_ne!(successor, GENESIS_SEED, "seed file must hold the successor");
    assert_eq!(successor.len(), "devkey:".len() + 64);
    #[cfg(unix)]
    assert_eq!(mode_of(&seed_file), 0o600, "seed file must stay 0600");
    let staged = seed_file.with_file_name("witness.ed25519.seed.new");
    assert!(!staged.exists(), "no .new file may be left behind");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // A verifier with no flag at all finds the seed file for the database
    // key, and the chain verifies ACROSS the rotation — self-anchored,
    // because a file beside the database is not an out-of-band identity.
    let out = log_verify(&["--db", db_str], &[("SECURACV_DB_KEY_SEED", DB_SECRET)]);
    assert!(
        out.status.success(),
        "log_verify (seed file) failed\n{}",
        text(&out)
    );
    let all = text(&out);
    assert!(all.contains("database key from the seed file"), "{all}");
    assert!(all.contains("SELF-CONSISTENT"), "{all}");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // Pinned to the GENESIS key out-of-band, it is VALID across the boundary.
    let genesis_hex = genesis_public_key_hex();
    let out = log_verify(
        &["--db", db_str, "--public-key", &genesis_hex],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(
        out.status.success(),
        "log_verify (genesis pin) failed\n{}",
        text(&out)
    );
    assert!(text(&out).contains("VALID"), "{}", text(&out));

    // The retired seed can no longer open the log.
    let export_path = dir.path().join("export.json");
    let export_str = export_path.to_str().expect("utf8 path");
    let out = export_events(
        &[
            "--db-path",
            db_str,
            "--self-export",
            "--output",
            export_str,
            "--device-key-seed",
            GENESIS_SEED,
        ],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(
        !out.status.success(),
        "a retired seed must be refused\n{}",
        text(&out)
    );
    assert!(text(&out).contains("retired"), "{}", text(&out));

    // A signing CLI with no seed flag finds the seed file and signs under the
    // successor (an export receipt is a post-rotation signed row).
    let out = export_events(
        &["--db-path", db_str, "--self-export", "--output", export_str],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(
        out.status.success(),
        "export under the seed file failed\n{}",
        text(&out)
    );
    assert!(export_path.is_file(), "export bundle written");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // A write-side daemon starts with ONLY the seed file present, logs the
    // seed's source (never its value) and appends under the successor.
    let out = grove_ingest(db_str, &[("SECURACV_DB_KEY_SEED", DB_SECRET)]);
    assert!(out.status.success(), "grove ingest failed\n{}", text(&out));
    let all = text(&out);
    assert!(all.contains("device key seed: seed file"), "{all}");
    assert!(all.contains("Grove Vision 2 event appended"), "{all}");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // Rows signed by both epochs verify from the genesis anchor.
    let out = log_verify(
        &["--db", db_str, "--public-key", &genesis_hex],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(out.status.success(), "{}", text(&out));
    let all = text(&out);
    assert!(all.contains("verified 1 export receipt entries"), "{all}");
    assert!(all.contains("VALID"), "{all}");

    // The lineage inspector shows the new epoch.
    let out = log_verify(
        &["--db", db_str, "--lineage", "--public-key", &genesis_hex],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(out.status.success(), "{}", text(&out));
    assert!(text(&out).contains("epoch 1"), "{}", text(&out));

    // The operator does what "Next:" says and exports the NEW seed, then
    // verifies with it. The seed derives epoch 1's key, not genesis: the run
    // must not fail as if the log were tampered, and must not claim identity
    // either — it verifies self-anchored and names the epoch.
    for envs in [
        vec![
            ("DEVICE_KEY_SEED", successor.as_str()),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
        vec![("SECURACV_DB_KEY_SEED", DB_SECRET)],
    ] {
        let args: Vec<&str> = if envs.len() == 2 {
            vec!["--db", db_str]
        } else {
            vec!["--db", db_str, "--device-key-seed", successor.as_str()]
        };
        let out = log_verify(&args, &envs);
        assert!(
            out.status.success(),
            "log_verify with the successor seed failed\n{}",
            text(&out)
        );
        let all = text(&out);
        assert!(all.contains("lineage epoch 1 key"), "{all}");
        assert!(all.contains("SELF-CONSISTENT"), "{all}");
        assert!(!all.contains("VALID (identity anchored"), "{all}");
        assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);
    }
    let out = log_verify(
        &["--db", db_str, "--json"],
        &[
            ("DEVICE_KEY_SEED", successor.as_str()),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(out.status.success(), "{}", text(&out));
    let report: serde_json::Value =
        serde_json::from_slice(&out.stdout).expect("--json prints the report");
    assert_eq!(report["chain_valid"], true, "{}", text(&out));
    assert_eq!(report["identity_verified"], false, "{}", text(&out));
    assert_eq!(
        report["verdict"],
        "self-consistent; identity unverified",
        "{}",
        text(&out)
    );

    // The genesis seed still derives the genesis key — the identity anchor —
    // so it verifies VALID (read-only; it can no longer open the log).
    let out = log_verify(
        &["--db", db_str],
        &[
            ("DEVICE_KEY_SEED", GENESIS_SEED),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(out.status.success(), "{}", text(&out));
    assert!(
        text(&out).contains("VALID (identity anchored"),
        "{}",
        text(&out)
    );
}

#[test]
fn rotate_identity_refuses_while_the_db_key_follows_the_signing_key() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    let seed_file = write_genesis_seed_file(&db);
    let db_str = db.to_str().expect("utf8 path");

    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
            "--generate",
        ],
        &[],
    );
    assert!(
        !out.status.success(),
        "must refuse a coupled DB key\n{}",
        text(&out)
    );
    let all = text(&out);
    assert!(all.contains("SECURACV_DB_KEY_SEED"), "{all}");
    assert!(all.contains("rekey-db"), "{all}");
    assert_no_secrets(&out, &[GENESIS_SEED]);

    // Nothing moved: the seed file is the genesis seed, nothing staged, and
    // the log still opens and verifies under it.
    assert_eq!(read_seed(&seed_file), GENESIS_SEED);
    assert!(!seed_file
        .with_file_name("witness.ed25519.seed.new")
        .exists());
    let out = log_verify(&["--db", db_str], &[("DEVICE_KEY_SEED", GENESIS_SEED)]);
    assert!(out.status.success(), "{}", text(&out));
    assert!(text(&out).contains("VALID"), "{}", text(&out));
}

#[test]
fn rotate_identity_with_explicit_seed_then_generate_and_a_loose_seed_file_is_refused() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    let db_str = db.to_str().expect("utf8 path");
    let seed_file = seed_file_for(&db);
    assert!(
        !seed_file.exists(),
        "this deployment keeps its seed in the environment"
    );

    // An environment-held seed: rotate to an operator-chosen successor. No
    // seed file exists and none is written; the operator updates the env.
    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
        ],
        &[
            ("DEVICE_KEY_SEED", GENESIS_SEED),
            ("NEW_DEVICE_KEY_SEED", SUCCESSOR_SEED),
            ("SECURACV_NEW_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(
        out.status.success(),
        "rotate-identity failed\n{}",
        text(&out)
    );
    assert!(text(&out).contains("seed file: none"), "{}", text(&out));
    assert!(
        !seed_file.exists(),
        "--new-seed without a seed file writes none"
    );
    assert_no_secrets(&out, &[GENESIS_SEED, SUCCESSOR_SEED, DB_SECRET]);

    // The successor opens the log; a second rotation (now --generate) proves
    // the reopen and always writes the seed file.
    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
            "--generate",
        ],
        &[
            ("DEVICE_KEY_SEED", SUCCESSOR_SEED),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(
        out.status.success(),
        "second rotation failed\n{}",
        text(&out)
    );
    assert!(text(&out).contains("lineage epoch 2"), "{}", text(&out));
    assert!(seed_file.is_file(), "--generate writes the seed file");
    let third = read_seed(&seed_file);
    assert_no_secrets(&out, &[GENESIS_SEED, SUCCESSOR_SEED, &third, DB_SECRET]);

    // Now the retired successor is refused, and the file-held seed works.
    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
            "--generate",
        ],
        &[
            ("DEVICE_KEY_SEED", SUCCESSOR_SEED),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(
        !out.status.success(),
        "retired seed must be refused\n{}",
        text(&out)
    );
    assert!(text(&out).contains("retired"), "{}", text(&out));
    assert_eq!(
        read_seed(&seed_file),
        third,
        "a refused rotation leaves the file alone"
    );
    assert!(!seed_file
        .with_file_name("witness.ed25519.seed.new")
        .exists());

    // A seed file another user can read is refused, naming the fix.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&seed_file, std::fs::Permissions::from_mode(0o644))
            .expect("chmod 644");
        let out = log_verify(&["--db", db_str], &[("SECURACV_DB_KEY_SEED", DB_SECRET)]);
        assert!(
            !out.status.success(),
            "loose seed file must be refused\n{}",
            text(&out)
        );
        assert!(text(&out).contains("chmod 600"), "{}", text(&out));
        assert_no_secrets(&out, &[third.as_str()]);
        std::fs::set_permissions(&seed_file, std::fs::Permissions::from_mode(0o600))
            .expect("chmod 600");
        let out = log_verify(&["--db", db_str], &[("SECURACV_DB_KEY_SEED", DB_SECRET)]);
        assert!(out.status.success(), "{}", text(&out));
    }
}

#[test]
fn rekey_db_moves_the_database_key_to_an_independent_secret() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    write_genesis_seed_file(&db);
    let db_str = db.to_str().expect("utf8 path");

    // Current key derived from the seed file (no flags), new key from the secret.
    let out = break_glass(
        &["rekey-db", "--db", db_str, "--new-db-key-seed", DB_SECRET],
        &[],
    );
    assert!(out.status.success(), "rekey-db failed\n{}", text(&out));
    assert!(
        text(&out).contains("SECURACV_DB_KEY_SEED"),
        "{}",
        text(&out)
    );
    assert_no_secrets(&out, &[GENESIS_SEED, DB_SECRET]);

    // The signing-derived key no longer opens the database ...
    let out = log_verify(&["--db", db_str], &[("DEVICE_KEY_SEED", GENESIS_SEED)]);
    assert!(
        !out.status.success(),
        "old key must not open the db\n{}",
        text(&out)
    );

    // ... the independent secret does, and identity is unchanged (VALID).
    let out = log_verify(
        &["--db", db_str],
        &[
            ("DEVICE_KEY_SEED", GENESIS_SEED),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(out.status.success(), "{}", text(&out));
    assert!(text(&out).contains("VALID"), "{}", text(&out));

    // Re-keying to the key it already has is refused as a no-op.
    let out = break_glass(
        &["rekey-db", "--db", db_str, "--new-db-key-seed", DB_SECRET],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(
        !out.status.success(),
        "no-op rekey must be refused\n{}",
        text(&out)
    );
    assert!(text(&out).contains("nothing to do"), "{}", text(&out));

    // Rotating the independent secret itself: the current key comes from
    // `db-key` (as an observer would hold it), never the signing seed.
    let out = break_glass(
        &["db-key", "--device-key-seed", GENESIS_SEED],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(out.status.success(), "{}", text(&out));
    let current_key = String::from_utf8_lossy(&out.stdout).trim().to_string();
    assert_eq!(current_key.len(), 64, "db-key prints the 32-byte hex key");
    let out = break_glass(
        &[
            "rekey-db",
            "--db",
            db_str,
            "--old-db-key",
            &current_key,
            "--new-db-key-seed",
            DB_SECRET_2,
        ],
        &[],
    );
    assert!(
        out.status.success(),
        "rekey-db (--old-db-key) failed\n{}",
        text(&out)
    );
    assert_no_secrets(&out, &[GENESIS_SEED, DB_SECRET, DB_SECRET_2, &current_key]);
    let out = log_verify(
        &["--db", db_str],
        &[
            ("DEVICE_KEY_SEED", GENESIS_SEED),
            ("SECURACV_DB_KEY_SEED", DB_SECRET_2),
        ],
    );
    assert!(out.status.success(), "{}", text(&out));
    assert!(text(&out).contains("VALID"), "{}", text(&out));
}

#[test]
fn a_write_side_daemon_generates_then_reuses_the_seed_file_and_refuses_a_loose_one() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    let db_str = db.to_str().expect("utf8 path");
    let seed_file = seed_file_for(&db);

    // First start with no seed anywhere: one is generated and persisted 0600;
    // only the source reaches the log.
    let out = grove_ingest(db_str, &[]);
    assert!(out.status.success(), "first start failed\n{}", text(&out));
    let all = text(&out);
    assert!(all.contains("device key seed: generated"), "{all}");
    assert!(all.contains("Grove Vision 2 event appended"), "{all}");
    assert!(seed_file.is_file(), "the generated seed is persisted");
    #[cfg(unix)]
    assert_eq!(mode_of(&seed_file), 0o600, "generated seed file is 0600");
    let generated = read_seed(&seed_file);
    assert!(generated.starts_with("devkey:"));
    assert_no_secrets(&out, &[&generated]);

    // Second start: the same identity, from the file.
    let out = grove_ingest(db_str, &[]);
    assert!(out.status.success(), "second start failed\n{}", text(&out));
    assert!(
        text(&out).contains("device key seed: seed file"),
        "{}",
        text(&out)
    );
    assert_no_secrets(&out, &[&generated]);

    // An environment seed that disagrees with the file is refused (two
    // identities on one log), and the file is left alone.
    let out = grove_ingest(db_str, &[("DEVICE_KEY_SEED", SUCCESSOR_SEED)]);
    assert!(
        !out.status.success(),
        "mismatch must be refused\n{}",
        text(&out)
    );
    assert!(text(&out).contains("mismatch"), "{}", text(&out));
    assert_no_secrets(&out, &[&generated, SUCCESSOR_SEED]);
    assert_eq!(read_seed(&seed_file), generated);

    // A seed file another user can read stops the daemon, naming the fix.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&seed_file, std::fs::Permissions::from_mode(0o640))
            .expect("chmod 640");
        let out = grove_ingest(db_str, &[]);
        assert!(
            !out.status.success(),
            "loose seed file must be refused\n{}",
            text(&out)
        );
        assert!(text(&out).contains("chmod 600"), "{}", text(&out));
        assert_no_secrets(&out, &[&generated]);
    }
}

#[test]
fn rotate_identity_replaces_a_custom_seed_file_and_the_one_beside_the_database() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    let db_str = db.to_str().expect("utf8 path");
    // An add-on style deployment: its own key file, exported as
    // DEVICE_KEY_SEED at start, which witnessd also persisted beside the db.
    let beside_db = write_genesis_seed_file(&db);
    let key_file = dir.path().join("keys").join("device_key_seed");
    witness_kernel::crypto::load_or_create_device_seed(&key_file, Some(GENESIS_SEED))
        .expect("write key file");
    let key_file_str = key_file.to_str().expect("utf8 path");

    let out = break_glass(
        &["rekey-db", "--db", db_str, "--new-db-key-seed", DB_SECRET],
        &[("DEVICE_KEY_SEED", GENESIS_SEED)],
    );
    assert!(out.status.success(), "rekey-db failed\n{}", text(&out));

    // The current seed is read from --seed-file (no DEVICE_KEY_SEED).
    let out = break_glass(
        &[
            "rotate-identity",
            "--db",
            db_str,
            "--ruleset-id",
            "ruleset:test",
            "--generate",
            "--seed-file",
            key_file_str,
        ],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(
        out.status.success(),
        "rotate-identity failed\n{}",
        text(&out)
    );
    let successor = read_seed(&key_file);
    assert_ne!(successor, GENESIS_SEED);
    assert_eq!(
        read_seed(&beside_db),
        successor,
        "the seed file beside the database follows the identity too"
    );
    #[cfg(unix)]
    {
        assert_eq!(mode_of(&key_file), 0o600);
        assert_eq!(mode_of(&beside_db), 0o600);
    }
    let all = text(&out);
    assert!(all.contains(key_file_str), "{all}");
    assert!(all.contains(beside_db.to_str().expect("utf8")), "{all}");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // The add-on restarts: it exports the key file's (new) seed, which agrees
    // with the file beside the database, so the daemon opens under it.
    let out = grove_ingest(
        db_str,
        &[
            ("DEVICE_KEY_SEED", successor.as_str()),
            ("SECURACV_DB_KEY_SEED", DB_SECRET),
        ],
    );
    assert!(
        out.status.success(),
        "restart under the successor failed\n{}",
        text(&out)
    );
    assert!(
        text(&out).contains("Grove Vision 2 event appended"),
        "{}",
        text(&out)
    );
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);

    // And the chain still verifies from the genesis anchor.
    let out = log_verify(
        &["--db", db_str, "--public-key", &genesis_public_key_hex()],
        &[("SECURACV_DB_KEY_SEED", DB_SECRET)],
    );
    assert!(out.status.success(), "{}", text(&out));
    assert!(text(&out).contains("VALID"), "{}", text(&out));
}

/// Every environment variable that carries a secret VALUE (a seed, a
/// database key or its secret, a password, a bearer token). A clap argument
/// bound to one must set `hide_env_values = true`, or `--help` prints the
/// live value from the environment (`[env: DEVICE_KEY_SEED=devkey:…]`).
/// `*_PATH` variables name a file and are not secrets.
const SECRET_ENV_VARS: &[&str] = &[
    "DEVICE_KEY_SEED",
    "NEW_DEVICE_KEY_SEED",
    "SECURACV_DB_KEY",
    "SECURACV_DB_KEY_SEED",
    "SECURACV_NEW_DB_KEY_SEED",
    "MQTT_PASSWORD",
    "BUSYBAR_TOKEN",
    "WITNESS_API_TOKEN",
];

fn rust_sources(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in std::fs::read_dir(dir).expect("read src dir") {
        let path = entry.expect("dir entry").path();
        if path.is_dir() {
            rust_sources(&path, out);
        } else if path.extension().is_some_and(|ext| ext == "rs") {
            out.push(path);
        }
    }
}

/// The class guard: no `#[arg(...)]` anywhere under `src/` binds a secret
/// environment variable without hiding its value from `--help`.
#[test]
fn no_cli_argument_prints_a_secret_env_value_in_help() {
    let mut files = Vec::new();
    rust_sources(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("src"),
        &mut files,
    );
    let mut offenders = Vec::new();
    let mut checked = 0usize;
    for file in &files {
        let source = std::fs::read_to_string(file).expect("read source");
        let mut rest = source.as_str();
        while let Some(start) = rest.find("#[arg(") {
            let tail = &rest[start..];
            let end = tail.find(")]").map(|i| i + 2).unwrap_or(tail.len());
            let attribute = &tail[..end];
            for var in SECRET_ENV_VARS {
                if attribute.contains(&format!("env = \"{var}\"")) {
                    checked += 1;
                    if !attribute.contains("hide_env_values = true") {
                        offenders.push(format!("{}: {var}", file.display()));
                    }
                }
            }
            rest = &tail[end..];
        }
    }
    assert!(
        checked >= 30,
        "the scan found only {checked} secret-bound arguments; the attribute parser is broken"
    );
    assert!(
        offenders.is_empty(),
        "these arguments print a secret env value in --help (add `hide_env_values = true`):\n{}",
        offenders.join("\n")
    );
}

/// The behavior the guard protects: `--help` with every secret variable set
/// names the variables but prints none of their values.
#[test]
fn help_never_prints_a_secret_from_the_environment() {
    const DB_KEY_HEX: &str = "5ec2e75ec2e75ec2e75ec2e75ec2e75ec2e75ec2e75ec2e75ec2e75ec2e75ec2";
    let envs = [
        ("DEVICE_KEY_SEED", GENESIS_SEED),
        ("NEW_DEVICE_KEY_SEED", SUCCESSOR_SEED),
        ("SECURACV_NEW_DB_KEY_SEED", DB_SECRET),
        ("SECURACV_DB_KEY_SEED", DB_SECRET_2),
        ("SECURACV_DB_KEY", DB_KEY_HEX),
    ];
    let secrets = [
        GENESIS_SEED,
        SUCCESSOR_SEED,
        DB_SECRET,
        DB_SECRET_2,
        DB_KEY_HEX,
    ];
    for (bin, args) in [
        (
            env!("CARGO_BIN_EXE_break_glass"),
            &["rotate-identity", "--help"][..],
        ),
        (
            env!("CARGO_BIN_EXE_break_glass"),
            &["rekey-db", "--help"][..],
        ),
        (env!("CARGO_BIN_EXE_break_glass"), &["db-key", "--help"][..]),
        (
            env!("CARGO_BIN_EXE_break_glass"),
            &["receipts", "--help"][..],
        ),
        (env!("CARGO_BIN_EXE_log_verify"), &["--help"][..]),
        (env!("CARGO_BIN_EXE_export_events"), &["--help"][..]),
        (env!("CARGO_BIN_EXE_break_glass_serve"), &["--help"][..]),
    ] {
        let out = run(bin, args, &envs);
        assert!(out.status.success(), "{bin} {args:?}\n{}", text(&out));
        let all = text(&out);
        assert!(
            all.contains("[env: DEVICE_KEY_SEED]"),
            "{bin} {args:?} should still name the variable\n{all}"
        );
        assert_no_secrets(&out, &secrets);
    }
}

/// A write-side daemon given DEVICE_KEY_SEED uses it and writes NOTHING: a
/// seed kept in a secret store (the sidecar's Docker secret, an add-on
/// option) must not be copied onto the data volume beside the database.
#[test]
fn a_write_side_daemon_never_copies_an_environment_seed_to_disk() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    let db_str = db.to_str().expect("utf8 path");
    let seed_file = seed_file_for(&db);

    for start in ["first", "second"] {
        let out = grove_ingest(db_str, &[("DEVICE_KEY_SEED", GENESIS_SEED)]);
        assert!(out.status.success(), "{start} start failed\n{}", text(&out));
        let all = text(&out);
        assert!(all.contains("device key seed: DEVICE_KEY_SEED"), "{all}");
        assert!(all.contains("Grove Vision 2 event appended"), "{all}");
        assert!(
            !seed_file.exists(),
            "{start} start copied the environment seed to {}",
            seed_file.display()
        );
        assert!(
            !seed_file
                .with_file_name("witness.ed25519.seed.new")
                .exists(),
            "no staging file either"
        );
        assert_no_secrets(&out, &[GENESIS_SEED]);
    }
}

/// `--seed-file ./witness.ed25519.seed` beside `--db witness.db` names the
/// seed file beside the database a second time: it is ONE target, staged and
/// replaced once. And a staging refusal (a `.new` left by an interrupted
/// ceremony) happens before `--rekey-db-to` touches the database.
#[test]
fn rotate_identity_treats_two_spellings_of_one_seed_file_as_one_and_stages_before_rekey() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    build_db(&db);
    let seed_file = write_genesis_seed_file(&db);
    let staged = seed_file.with_file_name("witness.ed25519.seed.new");

    // A leftover staged file refuses the ceremony — and the database is not
    // re-keyed: the genesis-derived key still opens it.
    std::fs::write(&staged, "devkey:left-by-an-interrupted-ceremony-0000\n").expect("stale .new");
    let out = command(
        env!("CARGO_BIN_EXE_break_glass"),
        &[
            "rotate-identity",
            "--db",
            "witness.db",
            "--ruleset-id",
            "ruleset:test",
            "--generate",
            "--rekey-db-to",
            DB_SECRET,
        ],
        &[],
    )
    .current_dir(dir.path())
    .stdin(Stdio::null())
    .output()
    .expect("spawn break_glass");
    assert!(!out.status.success(), "{}", text(&out));
    let all = text(&out);
    assert!(all.contains("a staged seed file already exists"), "{all}");
    assert!(!all.contains("database re-keyed"), "{all}");
    let db_str = db.to_str().expect("utf8 path");
    let out = log_verify(&["--db", db_str], &[("DEVICE_KEY_SEED", GENESIS_SEED)]);
    assert!(
        out.status.success(),
        "the refused ceremony must leave the database key alone\n{}",
        text(&out)
    );
    std::fs::remove_file(&staged).expect("clear the stale .new");

    // Two spellings of the seed file beside the database: one target.
    let out = command(
        env!("CARGO_BIN_EXE_break_glass"),
        &[
            "rotate-identity",
            "--db",
            "witness.db",
            "--ruleset-id",
            "ruleset:test",
            "--generate",
            "--rekey-db-to",
            DB_SECRET,
            "--seed-file",
            "./witness.ed25519.seed",
        ],
        &[],
    )
    .current_dir(dir.path())
    .stdin(Stdio::null())
    .output()
    .expect("spawn break_glass");
    assert!(
        out.status.success(),
        "rotate-identity failed\n{}",
        text(&out)
    );
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert_eq!(
        stdout.matches("seed file:").count(),
        1,
        "one file, replaced once\n{stdout}"
    );
    let successor = read_seed(&seed_file);
    assert_ne!(successor, GENESIS_SEED);
    assert!(!staged.exists(), "no .new file may be left behind");
    assert_no_secrets(&out, &[GENESIS_SEED, &successor, DB_SECRET]);
}
