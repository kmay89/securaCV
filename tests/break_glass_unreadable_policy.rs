//! A stored break-glass policy that cannot be loaded, through the real
//! `break_glass` binary (reader hardening, D7): `doctor` names it
//! `policy: UNREADABLE` and exits 1, `policy set` and the enrollment that
//! completes a setup roster refuse with the recorded error instead of
//! bootstrapping over it, and the open logs one pinned error line.
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
    database_with_policy_cell(db, &UNREADABLE_POLICY);
}

/// A kernel database whose bootstrapped policy cell was overwritten with
/// `cell`.
fn database_with_policy_cell(db: &Path, cell: &dyn rusqlite::ToSql) {
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
            [cell],
        )
        .expect("overwrite the policy row");
    kernel
        .conn
        .execute_batch("PRAGMA wal_checkpoint(TRUNCATE);")
        .expect("checkpoint");
}

fn break_glass(args: &[&str]) -> Output {
    break_glass_with_log(args, "off")
}

/// The binary with `RUST_LOG` set to `rust_log` (the other tests keep it
/// `off`, so their stderr carries only the command's own lines).
fn break_glass_with_log(args: &[&str], rust_log: &str) -> Output {
    Command::new(env!("CARGO_BIN_EXE_break_glass"))
        .args(args)
        .env_remove("DEVICE_KEY_SEED")
        .env_remove("SECURACV_DB_KEY_SEED")
        .env_remove("SECURACV_DB_KEY")
        .env_remove("SECURACV_HWM_PATH")
        .env_remove("RUST_BACKTRACE")
        .env_remove("RUST_LIB_BACKTRACE")
        .env("RUST_LOG", rust_log)
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

/// stderr's lines with the UI stage's two progress lines taken out, after
/// checking each appears exactly once: `==> {stage}` and `✔ {stage} (…)`,
/// whose duration is wall-clock time and so is not pinned.
fn without_stage(out: &Output, stage: &str) -> Vec<String> {
    let stderr = String::from_utf8_lossy(&out.stderr);
    let start = format!("==> {stage}");
    let done = format!("\u{2714} {stage} (");
    let mut starts = 0usize;
    let mut dones = 0usize;
    let mut rest = Vec::new();
    for line in stderr.lines() {
        if line == start {
            starts += 1;
        } else if line.starts_with(&done) && line.ends_with(')') {
            dones += 1;
        } else {
            rest.push(line.to_string());
        }
    }
    assert_eq!((starts, dones), (1, 1), "{}", text(out));
    rest
}

/// The refusal `policy set` and the roster-completing enrollment print: the
/// accessor's recorded-error string (`Kernel::break_glass_policy_checked`).
fn accessor_refusal() -> String {
    format!(
        "Error: break-glass policy could not be loaded: {UNREADABLE_POLICY_ERROR} — this build \
         refuses every break-glass action on this database (run log_verify; upgrade if the \
         policy was written by a newer release)"
    )
}

/// Enrolling the trustee that completes a setup roster over an unreadable
/// policy refuses with the recorded load error, not the "a policy is already
/// live … use `policy propose`" guidance (no policy is live, and `policy
/// propose` refuses too). Nothing is committed.
#[test]
fn trustee_enroll_refuses_over_an_unreadable_policy() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    database_with_unreadable_policy(&db);
    let db_arg = db.to_string_lossy().to_string();

    let init = break_glass(&[
        "init",
        "--threshold",
        "1",
        "--trustees",
        "1",
        "--db",
        &db_arg,
        "--ruleset-id",
        "ruleset:test",
        "--device-key-seed",
        SEED,
    ]);
    assert_eq!(init.status.code(), Some(0), "{}", text(&init));

    let key = public_hex(0x22);
    let out = break_glass(&[
        "trustee",
        "enroll",
        "--id",
        "bob",
        "--public-key",
        &key,
        "--db",
        &db_arg,
        "--ruleset-id",
        "ruleset:test",
        "--device-key-seed",
        SEED,
    ]);
    assert_eq!(out.status.code(), Some(1), "{}", text(&out));
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert_eq!(
        stdout.lines().collect::<Vec<_>>(),
        ["  \u{2713} enrolled trustee 'bob' (1/1)"],
        "{}",
        text(&out)
    );
    assert_eq!(
        without_stage(&out, "Enroll trustee"),
        [accessor_refusal()],
        "{}",
        text(&out)
    );
    assert!(
        !String::from_utf8_lossy(&out.stderr).contains("already live"),
        "{}",
        text(&out)
    );

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

/// `doctor` on an existing database with no kernel schema (an empty
/// pre-created `witness.db`) reports the failed read as itself, not as an
/// UNREADABLE stored policy: no policy is stored, and `Kernel::open` would
/// provision the schema. Still a problem; still exit 1.
#[test]
fn doctor_on_an_unprovisioned_database_is_not_unreadable() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    let vault = dir.0.join("vault");
    std::fs::write(&db, b"").expect("pre-create an empty database file");

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
        "  \u{2717} no such table: break_glass_policy".to_string(),
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
    assert!(!stdout.contains("UNREADABLE"), "{}", text(&out));
    assert_eq!(
        without_stage(&out, "Vault doctor"),
        ["Error: vault doctor found 1 problem(s) — see the report above"],
        "{}",
        text(&out)
    );
}

/// The open-time log line (§2.1, the line an operator sees on stderr with the
/// default filter) is pinned: `policy set` opens the kernel with logging on,
/// and every stderr line besides the UI stage's two is either that error
/// record or the refusal. The
/// record's message is matched after env_logger's `[<timestamp> ERROR
/// <module>] ` prefix, which carries the wall clock.
#[test]
fn unreadable_policy_open_logs_the_refusal_line() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    database_with_unreadable_policy(&db);

    let db_arg = db.to_string_lossy().to_string();
    let trustee = format!("mallory:{}", public_hex(0x44));
    let out = break_glass_with_log(
        &[
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
        ],
        "error",
    );
    assert_eq!(out.status.code(), Some(1), "{}", text(&out));
    assert!(out.stdout.is_empty(), "{}", text(&out));

    let logged = format!(
        "break-glass policy could not be loaded: {UNREADABLE_POLICY_ERROR} — every break-glass \
         action on this database is refused; run log_verify"
    );
    let refusal = accessor_refusal();
    let mut records = 0usize;
    let mut refusals = 0usize;
    for line in without_stage(&out, "Set quorum policy") {
        if line == refusal {
            refusals += 1;
            continue;
        }
        let message = line
            .strip_prefix('[')
            .and_then(|rest| rest.split_once("] "))
            .filter(|(prefix, _)| prefix.contains(" ERROR "))
            .map(|(_, message)| message);
        assert_eq!(message, Some(logged.as_str()), "{}", text(&out));
        records += 1;
    }
    assert!(records >= 1, "{}", text(&out));
    assert_eq!(refusals, 1, "{}", text(&out));
}

/// A stored policy cell that is not text (a blob) was read but cannot be read
/// as a policy: `doctor` still names it `policy: UNREADABLE`, as it names a
/// parse or validate failure; only a failed SQL read keeps its own message.
#[test]
fn doctor_reports_a_non_text_policy_cell_as_unreadable() {
    let dir = TempDir::new();
    let db = dir.0.join("witness.db");
    let vault = dir.0.join("vault");
    database_with_policy_cell(&db, &vec![0u8, 1, 2]);

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
    let policy_lines: Vec<&str> = stdout
        .lines()
        .skip_while(|l| *l != "[ quorum policy ]")
        .skip(1)
        .take_while(|l| !l.is_empty())
        .collect();
    assert_eq!(
        policy_lines,
        ["  \u{2717} policy: UNREADABLE — Invalid column type Blob at index: 0, name: policy_json"],
        "{}",
        text(&out)
    );
    assert_eq!(
        without_stage(&out, "Vault doctor"),
        ["Error: vault doctor found 1 problem(s) — see the report above"],
        "{}",
        text(&out)
    );
}
