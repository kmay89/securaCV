//! A stored break-glass policy that cannot be loaded, through the real
//! `break_glass` binary (reader hardening, D7): `doctor` names it
//! `policy: UNREADABLE` and exits 1, and `policy set` refuses with the
//! recorded error instead of bootstrapping over it.
//!
//! Offline: the policy is bootstrapped at a fixed bucket, then overwritten
//! with a row this build cannot parse.

use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::time::Duration;

use ed25519_dalek::SigningKey;
use witness_kernel::break_glass::{QuorumPolicy, TrusteeEntry, TrusteeId};
use witness_kernel::{Kernel, KernelConfig, TimeBucket, ZonePolicy};

const SEED: &str = "devkey:unreadable-policy:0011223344556677";

const UNREADABLE_POLICY: &str = r#"{"n":1,"m":1,"trustees":"x"}"#;
const UNREADABLE_POLICY_ERROR: &str =
    "invalid type: string \"x\", expected a sequence at line 1 column 27";

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        let dir = std::env::temp_dir().join(format!(
            "bg_unreadable_policy_{:016x}",
            rand::random::<u64>()
        ));
        std::fs::create_dir_all(&dir).expect("temp dir");
        Self(dir)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn cfg(db: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn public_hex(byte: u8) -> String {
    hex::encode(
        SigningKey::from_bytes(&[byte; 32])
            .verifying_key()
            .to_bytes(),
    )
}

/// A kernel database whose bootstrapped policy was overwritten with a row
/// this build cannot parse.
fn database_with_unreadable_policy(db: &Path) {
    let mut kernel = Kernel::open(&cfg(db)).expect("open");
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: SigningKey::from_bytes(&[0x11; 32])
                .verifying_key()
                .to_bytes(),
        }],
    )
    .expect("policy");
    let bucket = TimeBucket {
        start_epoch_s: 1_794_312_000,
        size_s: 600,
    };
    kernel
        .set_break_glass_policy_gated(&policy, &[], bucket)
        .expect("bootstrap");
    kernel
        .conn
        .execute(
            "UPDATE break_glass_policy SET policy_json = ?1 WHERE id = 1",
            [UNREADABLE_POLICY],
        )
        .expect("overwrite the policy row");
    kernel
        .conn
        .execute_batch("PRAGMA wal_checkpoint(TRUNCATE);")
        .expect("checkpoint");
}

fn break_glass(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_break_glass"))
        .args(args)
        .env_remove("DEVICE_KEY_SEED")
        .env_remove("SECURACV_DB_KEY_SEED")
        .env_remove("SECURACV_DB_KEY")
        .env_remove("SECURACV_HWM_PATH")
        .env_remove("RUST_BACKTRACE")
        .env_remove("RUST_LIB_BACKTRACE")
        .env("RUST_LOG", "off")
        .stdin(Stdio::null())
        .output()
        .expect("spawn break_glass")
}

fn text(out: &Output) -> String {
    format!(
        "--- stdout ---\n{}\n--- stderr ---\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    )
}

/// Test 8. `doctor` reports the unreadable policy as a problem on its own
/// line and exits 1. Every stdout line is pinned (the two path lines echo
/// this test's own paths).
#[test]
fn doctor_reports_unreadable_policy() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    let vault = dir.0.join("vault");
    database_with_unreadable_policy(&db);

    let db_arg = db.to_string_lossy().to_string();
    let vault_arg = vault.to_string_lossy().to_string();
    let out = break_glass(&[
        "doctor",
        "--db",
        &db_arg,
        "--vault-path",
        &vault_arg,
        "--device-key-seed",
        SEED,
    ]);
    assert_eq!(out.status.code(), Some(1), "{}", text(&out));

    let stdout = String::from_utf8_lossy(&out.stdout);
    let expected = [
        "=== Break-glass vault doctor ===".to_string(),
        format!("Kernel DB:  {db_arg}"),
        format!("Vault path: {vault_arg}"),
        String::new(),
        "[ quorum policy ]".to_string(),
        format!("  \u{2717} policy: UNREADABLE — {UNREADABLE_POLICY_ERROR}"),
        String::new(),
        "[ vault master key ]".to_string(),
        format!(
            "  \u{26a0} master.key not found at {} — it is created on first seal; this vault \
             directory has not been initialized yet",
            vault.join("master.key").display()
        ),
        String::new(),
        "[ key store ]".to_string(),
        "    backend: file (the only backend today)".to_string(),
        String::new(),
        "Result: 1 PROBLEM(S), 1 warning(s)".to_string(),
    ];
    assert_eq!(
        stdout.lines().collect::<Vec<_>>(),
        expected.iter().map(String::as_str).collect::<Vec<_>>(),
        "{}",
        text(&out)
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr
            .lines()
            .any(|l| l == "Error: vault doctor found 1 problem(s) — see the report above"),
        "{}",
        text(&out)
    );
}

/// `policy set` against an unreadable policy refuses with the recorded load
/// error; nothing is bootstrapped over it.
#[test]
fn policy_set_refuses_over_an_unreadable_policy() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    database_with_unreadable_policy(&db);

    let db_arg = db.to_string_lossy().to_string();
    let trustee = format!("mallory:{}", public_hex(0x44));
    let out = break_glass(&[
        "policy",
        "set",
        "--threshold",
        "1",
        "--trustee",
        &trustee,
        "--db",
        &db_arg,
        "--ruleset-id",
        "ruleset:test",
        "--device-key-seed",
        SEED,
    ]);
    assert_eq!(out.status.code(), Some(1), "{}", text(&out));
    assert!(out.stdout.is_empty(), "{}", text(&out));
    let refusal = format!(
        "Error: break-glass policy could not be loaded: {UNREADABLE_POLICY_ERROR} — this build \
         refuses every break-glass action on this database (run log_verify; upgrade if the \
         policy was written by a newer release)"
    );
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.lines().any(|l| l == refusal), "{}", text(&out));

    // The stored row and the history are exactly as they were.
    let kernel = Kernel::open(&cfg(&db)).expect("open");
    let stored: String = kernel
        .conn
        .query_row(
            "SELECT policy_json FROM break_glass_policy WHERE id = 1",
            [],
            |r| r.get(0),
        )
        .expect("policy row");
    assert_eq!(stored, UNREADABLE_POLICY);
    assert_eq!(kernel.policy_change_history().expect("history").len(), 1);
}
