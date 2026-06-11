//! log_verify - External verifier for PWK sealed log integrity
//!
//! This tool verifies:
//! - The sealed event log is hash-chained (tamper-evident)
//! - The break-glass receipt log is hash-chained (tamper-evident)
//! - The export receipt log is hash-chained (tamper-evident)
//! - Each entry is signed by the device key (Ed25519)
//! - Checkpoints preserve verifiability across retention pruning
//!
//! This is not a convenience feature.
//! It is a core anti-erosion mechanism: integrity must be verifiable without trusting the runtime.

use anyhow::Result;
use clap::{Parser, ValueEnum};
use rusqlite::Connection;
use std::io::IsTerminal;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::verify_runner::{run_full_verify, VerifiedItem};
use witness_kernel::{verify, verify_explain, verify_helpers};

#[path = "../ui.rs"]
mod ui;

#[derive(Parser, Debug)]
#[command(
    name = "log_verify",
    about = "Verify PWK sealed log integrity (hash-chain + signatures)"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db")]
    db: String,

    /// Device public key (hex-encoded Ed25519 verifying key)
    #[arg(long, value_name = "HEX", conflicts_with = "public_key_file")]
    public_key: Option<String>,

    /// Path to file containing hex-encoded device public key
    #[arg(long, value_name = "PATH", conflicts_with = "public_key")]
    public_key_file: Option<String>,

    /// Verbose output
    #[arg(short, long)]
    verbose: bool,
    /// Print the full machine-readable VerifyReport as JSON instead of the
    /// human report (exit 1 on chain failure, 2 on --strict warnings)
    #[arg(long)]
    json: bool,
    /// Treat timeline-audit warnings (stale tail, missing heartbeats,
    /// timestamp regressions) as failures (non-zero exit)
    #[arg(long)]
    strict: bool,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
    /// Signature verification mode (compat = Ed25519 OR PQ, strict = Ed25519 AND PQ)
    #[arg(long, value_enum, default_value = "compat")]
    sig_mode: SignatureModeArg,
    /// PQ public key (hex-encoded)
    #[arg(long, value_name = "HEX", conflicts_with = "pq_public_key_file")]
    pq_public_key: Option<String>,
    /// Path to file containing PQ public key (hex-encoded)
    #[arg(long, value_name = "PATH", conflicts_with = "pq_public_key")]
    pq_public_key_file: Option<String>,
    /// SQLCipher database encryption key (hex-encoded, 32 bytes)
    #[arg(long, value_name = "HEX", env = "SECURACV_DB_KEY")]
    db_key: Option<String>,

    /// Device key seed (as used by the kernel/bridges). Convenience for
    /// operators: derives the SQLCipher key (when --db-key is not given)
    /// and the verifying key (when no --public-key/--public-key-file is
    /// given), so `DEVICE_KEY_SEED=... log_verify --db witness.db` works
    /// against a bridge-produced encrypted log.
    #[arg(long, value_name = "SEED", env = "DEVICE_KEY_SEED")]
    device_key_seed: Option<String>,
}

#[derive(ValueEnum, Clone, Debug)]
enum SignatureModeArg {
    Compat,
    Strict,
}

impl From<SignatureModeArg> for SignatureMode {
    fn from(value: SignatureModeArg) -> Self {
        match value {
            SignatureModeArg::Compat => SignatureMode::Compat,
            SignatureModeArg::Strict => SignatureMode::Strict,
        }
    }
}

fn main() -> Result<()> {
    let args = Args::parse();
    let signature_mode: SignatureMode = args.sig_mode.into();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);

    // SQLCipher key: explicit --db-key wins; otherwise derive it from the
    // device key seed exactly as the kernel/bridges do.
    let db_key: Option<String> = match (&args.db_key, &args.device_key_seed) {
        (Some(key), _) => Some(key.clone()),
        (None, Some(seed)) => {
            let signing_key = witness_kernel::signing_key_from_seed(seed)?;
            let seed_env = witness_kernel::db_key_seed_from_env();
            Some(
                witness_kernel::resolve_db_encryption_key(
                    &signing_key,
                    seed_env.as_ref().map(|s| s.as_str()),
                )
                .to_string(),
            )
        }
        (None, None) => None,
    };

    let conn = {
        let _stage = ui.stage("Open database");
        let conn = Connection::open(&args.db)?;
        if let Some(ref key) = db_key {
            conn.pragma_update(None, "key", format!("x'{}'", key))?;
        }
        conn
    };

    // Verifying key precedence: explicit hex > key file > device key seed >
    // the DB-stored device public key (resolved inside the runner).
    let public_key_hex: Option<String> = match (&args.public_key, &args.public_key_file) {
        (Some(hex), _) => Some(hex.clone()),
        (None, Some(path)) => Some(
            std::fs::read_to_string(path)
                .map_err(|e| anyhow::anyhow!("failed to read public key file {}: {}", path, e))?
                .trim()
                .to_string(),
        ),
        (None, None) => args
            .device_key_seed
            .as_deref()
            .map(|seed| {
                witness_kernel::verifying_key_from_seed(seed).map(|key| hex::encode(key.to_bytes()))
            })
            .transpose()?,
    };
    let pq_public_key_hex: Option<String> = match (&args.pq_public_key, &args.pq_public_key_file) {
        (Some(hex), _) => Some(hex.clone()),
        (None, Some(path)) => Some(
            std::fs::read_to_string(path)
                .map_err(|e| anyhow::anyhow!("failed to read pq public key file {}: {}", path, e))?
                .trim()
                .to_string(),
        ),
        (None, None) => None,
    };

    if !args.json {
        println!("log_verify: checking {}", args.db);
        println!();
    }

    let report = {
        let _stage = ui.stage("Verify sealed log");
        run_full_verify(
            &conn,
            public_key_hex.as_deref(),
            pq_public_key_hex.as_deref(),
            signature_mode,
            |item| {
                if args.verbose {
                    match item {
                        VerifiedItem::Event { id, entry_hash } => println!(
                            "  event {}: hash={} OK",
                            id,
                            &verify_helpers::hex32(&entry_hash)[..16]
                        ),
                        VerifiedItem::BreakGlassReceipt { id, entry_hash }
                        | VerifiedItem::ExportReceipt { id, entry_hash } => println!(
                            "  receipt {}: hash={} OK",
                            id,
                            &verify_helpers::hex32(&entry_hash)[..16]
                        ),
                    }
                }
            },
        )?
    };

    if args.json {
        println!("{}", serde_json::to_string_pretty(&report)?);
        if !report.chain_valid {
            std::process::exit(1);
        }
        if args.strict && !report.warnings.is_empty() {
            std::process::exit(2);
        }
        return Ok(());
    }

    println!("=== Sealed Events ===");
    match (
        &report.checkpoint_head_hash,
        report.checkpoint_cutoff_event_id,
    ) {
        (Some(head), Some(cutoff_id)) => println!(
            "checkpoint: cutoff_event_id={}, chain_head_hash={}",
            cutoff_id, head
        ),
        _ => println!("checkpoint: none (genesis chain)"),
    }
    if report.alarm_count > 0 {
        println!(
            "WARNING: {} conformance alarms recorded",
            report.alarm_count
        );
        if args.verbose {
            for alarm in verify::load_alarms(&conn)? {
                println!(
                    "  ALARM @{}: {} - {}",
                    alarm.created_at, alarm.code, alarm.message
                );
            }
        }
    }
    println!("verified {} event entries", report.events_verified);
    println!();

    println!("=== Break-Glass Receipts ===");
    println!(
        "verified {} receipt entries ({} granted, {} denied)",
        report.break_glass_receipts_verified, report.break_glass_granted, report.break_glass_denied
    );
    println!();

    println!("=== Export Receipts ===");
    println!(
        "verified {} export receipt entries",
        report.export_receipts_verified
    );

    if !report.chain_valid {
        if let Some(failure) = &report.failure {
            println!();
            println!("=== Diagnosis ===");
            println!("{}", verify_explain::format_failure_diagnosis(failure));
        }
        return Err(anyhow::anyhow!(
            "verification FAILED: {}",
            report
                .error
                .unwrap_or_else(|| "unknown verification error".to_string())
        ));
    }

    if !report.warnings.is_empty() {
        println!();
        println!("=== Timeline Audit ===");
        println!("The hash chain itself is valid; these flag anomalies the chain cannot see:");
        for warning in &report.warnings {
            println!("WARNING: {warning}");
            if let Some(hint) = verify_explain::warning_hint(verify::classify_warning(warning)) {
                println!("         hint: {hint}");
            }
        }
        if args.strict {
            return Err(anyhow::anyhow!(
                "verification passed but --strict treats {} timeline warning(s) as failure",
                report.warnings.len()
            ));
        }
    }

    println!("OK: all chains verified.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::verify_helpers::load_verifying_key;
    use ed25519_dalek::{Signer, SigningKey};
    use std::path::{Path, PathBuf};
    use witness_kernel::crypto::signatures::SignatureMode;
    use witness_kernel::{
        derive_db_encryption_key, signing_key_from_seed, Approval, BreakGlass, CandidateEvent,
        EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, QuorumPolicy,
        TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest, ZonePolicy,
    };

    const TEST_SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";

    fn open_encrypted_test_db(path: &Path) -> Connection {
        let signing_key = signing_key_from_seed(TEST_SEED).unwrap();
        let db_key = derive_db_encryption_key(&signing_key);
        let conn = Connection::open(path).unwrap();
        conn.pragma_update(None, "key", format!("x'{}'", &*db_key))
            .unwrap();
        conn
    }

    struct TempDb {
        path: PathBuf,
    }

    impl TempDb {
        fn new() -> Self {
            let mut path = std::env::temp_dir();
            let suffix: u64 = rand::random();
            path.push(format!("log_verify_test_{}.db", suffix));
            Self { path }
        }

        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempDb {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }

    fn write_test_event(kernel: &mut Kernel) -> Result<()> {
        let module = ModuleDescriptor {
            id: "test-module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket::now(600)?,
            zone_id: "zone:test".to_string(),
            confidence: 0.9,
            correlation_token: None,
        };
        kernel.append_event_checked(
            &module,
            cand,
            env!("CARGO_PKG_VERSION"),
            "ruleset:test",
            KernelConfig::ruleset_hash_from_id("ruleset:test"),
        )?;
        Ok(())
    }

    #[test]
    fn log_verify_succeeds_with_public_key() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = verify::latest_checkpoint(&conn)?;
        verify::verify_events_with(
            &conn,
            &verifying_key,
            checkpoint.chain_head_hash,
            SignatureMode::Compat,
            None,
            |_, _| {},
        )?;
        let policy = verify::load_break_glass_policy(&conn)?;
        verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            SignatureMode::Compat,
            None,
            |_, _| {},
        )?;
        verify::verify_export_receipts_with(
            &conn,
            &verifying_key,
            SignatureMode::Compat,
            None,
            |_, _| {},
        )?;

        Ok(())
    }

    #[test]
    fn log_verify_fails_without_public_key() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        kernel
            .conn
            .execute("DELETE FROM device_metadata WHERE id = 1", [])?;
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let result = load_verifying_key(&conn, None, None);
        assert!(result.is_err());

        Ok(())
    }

    #[test]
    fn log_verify_fails_with_mismatched_public_key() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        drop(kernel);

        let wrong_signing_key = SigningKey::from_bytes(&[42u8; 32]);
        let public_key_hex = hex::encode(wrong_signing_key.verifying_key().to_bytes());

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = verify::latest_checkpoint(&conn)?;
        let result = verify::verify_events_with(
            &conn,
            &verifying_key,
            checkpoint.chain_head_hash,
            SignatureMode::Compat,
            None,
            |_, _| {},
        );
        assert!(result.is_err());

        Ok(())
    }

    #[test]
    fn log_verify_rejects_tampered_event_payload() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        kernel.conn.execute(
            "UPDATE sealed_events SET payload_json = '{\"tampered\":true}' WHERE id = 1",
            [],
        )?;
        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = verify::latest_checkpoint(&conn)?;
        let result = verify::verify_events_with(
            &conn,
            &verifying_key,
            checkpoint.chain_head_hash,
            SignatureMode::Compat,
            None,
            |_, _| {},
        );
        assert!(result.is_err());

        Ok(())
    }

    #[test]
    fn log_verify_rejects_tampered_break_glass_approvals() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;

        let bucket = TimeBucket::now(600)?;
        let request = UnlockRequest::new("vault:1", [9u8; 32], "audit", bucket)?;
        let signing_key = SigningKey::from_bytes(&[3u8; 32]);
        let signature = signing_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )?;
        let (_, receipt) =
            BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
        let _entry_hash =
            kernel.append_break_glass_receipt(&receipt, std::slice::from_ref(&approval))?;

        kernel
            .conn
            .execute("UPDATE break_glass_receipts SET approvals_json = '[]'", [])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            SignatureMode::Compat,
            None,
            |_, _| {},
        );
        assert!(result.is_err());

        Ok(())
    }

    #[test]
    fn log_verify_rejects_unknown_trustee_approvals() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;

        let alice_key = SigningKey::from_bytes(&[11u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice_key.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&policy)?;

        let bucket = TimeBucket::now(600)?;
        let request = UnlockRequest::new("vault:1", [9u8; 32], "audit", bucket)?;
        let bob_key = SigningKey::from_bytes(&[12u8; 32]);
        let bob_signature = bob_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("bob"),
            request.request_hash(),
            bob_signature.to_vec(),
        );
        let (_, receipt) =
            BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
        let _entry_hash = kernel.append_break_glass_receipt(&receipt, &[approval])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            SignatureMode::Compat,
            None,
            |_, _| {},
        );
        assert!(result.is_err());

        Ok(())
    }

    #[test]
    fn log_verify_rejects_invalid_trustee_signature() -> Result<()> {
        let db = TempDb::new();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })?;

        let alice_key = SigningKey::from_bytes(&[21u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice_key.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&policy)?;

        let bucket = TimeBucket::now(600)?;
        let request = UnlockRequest::new("vault:1", [9u8; 32], "audit", bucket)?;
        let wrong_key = SigningKey::from_bytes(&[22u8; 32]);
        let bad_signature = wrong_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            bad_signature.to_vec(),
        );
        let (_, receipt) =
            BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
        let _entry_hash = kernel.append_break_glass_receipt(&receipt, &[approval])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = open_encrypted_test_db(db.path());
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            SignatureMode::Compat,
            None,
            |_, _| {},
        );
        assert!(result.is_err());

        Ok(())
    }
}
