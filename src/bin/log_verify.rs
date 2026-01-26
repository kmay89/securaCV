//! log_verify - External verifier for PWK sealed log integrity
//!
//! This tool proves:
//! - The sealed event log is hash-chained (tamper-evident)
//! - The break-glass receipt log is hash-chained (tamper-evident)
//! - The export receipt log is hash-chained (tamper-evident)
//! - Each entry is signed by the device key (Ed25519)
//! - Checkpoints preserve verifiability across retention pruning
//!
//! This is not a convenience feature.
//! It is a core anti-erosion mechanism: integrity must be provable without trusting the runtime.

use anyhow::{anyhow, Result};
use clap::Parser;
use rusqlite::Connection;
use std::io::IsTerminal;

use witness_kernel::{verify, verify_entry_signature, verify_helpers};

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
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

fn main() -> Result<()> {
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);
    let conn = {
        let _stage = ui.stage("Open database");
        Connection::open(&args.db)?
    };
    let verifying_key = {
        let _stage = ui.stage("Load verifying key");
        verify_helpers::load_verifying_key(
            &conn,
            args.public_key.as_deref(),
            args.public_key_file.as_deref(),
        )?
    };

    println!("log_verify: checking {}", args.db);
    println!();

    // === Sealed Events ===
    println!("=== Sealed Events ===");

    {
        let _stage = ui.stage("Verify sealed events");
        let checkpoint = verify::latest_checkpoint(&conn)?;
        if let (Some(head), Some(sig), Some(cutoff_id)) = (
            checkpoint.chain_head_hash,
            checkpoint.signature,
            checkpoint.cutoff_event_id,
        ) {
            if verify_entry_signature(&verifying_key, &head, &sig).is_err() {
                return Err(anyhow!("checkpoint signature mismatch"));
            }
            println!(
                "checkpoint: cutoff_event_id={}, chain_head_hash={}",
                cutoff_id,
                verify_helpers::hex32(&head)
            );
        } else {
            println!("checkpoint: none (genesis chain)");
        }

        let alarm_count = verify::count_alarms(&conn)?;
        if alarm_count > 0 {
            println!("WARNING: {} conformance alarms recorded", alarm_count);
            if args.verbose {
                for alarm in verify::load_alarms(&conn)? {
                    println!(
                        "  ALARM @{}: {} - {}",
                        alarm.created_at, alarm.code, alarm.message
                    );
                }
            }
        }

        let count = verify::verify_events_with(
            &conn,
            &verifying_key,
            checkpoint.chain_head_hash,
            |id, entry_hash| {
                if args.verbose {
                    println!(
                        "  event {}: hash={} OK",
                        id,
                        &verify_helpers::hex32(&entry_hash)[..16]
                    );
                }
            },
        )?;
        println!("verified {} event entries", count);
    };
    println!();

    // === Break-Glass Receipts ===
    {
        let _stage = ui.stage("Verify break-glass receipts");
        println!("=== Break-Glass Receipts ===");
        let policy = verify::load_break_glass_policy(&conn)?;
        let counts = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            |id, entry_hash| {
                if args.verbose {
                    println!(
                        "  receipt {}: hash={} OK",
                        id,
                        &verify_helpers::hex32(&entry_hash)[..16]
                    );
                }
            },
        )?;
        println!(
            "verified {} receipt entries ({} granted, {} denied)",
            counts.total, counts.granted, counts.denied
        );
    }
    println!();

    // === Export Receipts ===
    {
        let _stage = ui.stage("Verify export receipts");
        println!("=== Export Receipts ===");
        let count =
            verify::verify_export_receipts_with(&conn, &verifying_key, |id, entry_hash| {
                if args.verbose {
                    println!(
                        "  receipt {}: hash={} OK",
                        id,
                        &verify_helpers::hex32(&entry_hash)[..16]
                    );
                }
            })?;
        println!("verified {} export receipt entries", count);
    }

    println!("OK: all chains verified.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::verify_helpers::load_verifying_key;
    use ed25519_dalek::{Signer, SigningKey};
    use std::path::PathBuf;
    use witness_kernel::{
        Approval, BreakGlass, CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig,
        ModuleDescriptor, QuorumPolicy, TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest,
        ZonePolicy,
    };

    fn temp_db_path() -> PathBuf {
        let mut path = std::env::temp_dir();
        let suffix: u64 = rand::random();
        path.push(format!("log_verify_test_{}.db", suffix));
        path
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
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = verify::latest_checkpoint(&conn)?;
        verify::verify_events_with(&conn, &verifying_key, checkpoint.chain_head_hash, |_, _| {})?;
        let policy = verify::load_break_glass_policy(&conn)?;
        verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            |_, _| {},
        )?;
        verify::verify_export_receipts_with(&conn, &verifying_key, |_, _| {})?;

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }

    #[test]
    fn log_verify_fails_without_public_key() -> Result<()> {
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        kernel
            .conn
            .execute("DELETE FROM device_metadata WHERE id = 1", [])?;
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let result = load_verifying_key(&conn, None, None);
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }

    #[test]
    fn log_verify_fails_with_mismatched_public_key() -> Result<()> {
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_test_event(&mut kernel)?;
        drop(kernel);

        let wrong_signing_key = SigningKey::from_bytes(&[42u8; 32]);
        let public_key_hex = hex::encode(wrong_signing_key.verifying_key().to_bytes());

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = verify::latest_checkpoint(&conn)?;
        let result = verify::verify_events_with(
            &conn,
            &verifying_key,
            checkpoint.chain_head_hash,
            |_, _| {},
        );
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }

    #[test]
    fn log_verify_rejects_tampered_break_glass_approvals() -> Result<()> {
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
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
        let (_, receipt) = BreakGlass::authorize(&policy, &request, &[approval.clone()], bucket);
        let _entry_hash = kernel.append_break_glass_receipt(&receipt, &[approval.clone()])?;

        kernel
            .conn
            .execute("UPDATE break_glass_receipts SET approvals_json = '[]'", [])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            |_, _| {},
        );
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }

    #[test]
    fn log_verify_rejects_unknown_trustee_approvals() -> Result<()> {
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
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
        let (_, receipt) = BreakGlass::authorize(&policy, &request, &[approval.clone()], bucket);
        let _entry_hash = kernel.append_break_glass_receipt(&receipt, &[approval])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            |_, _| {},
        );
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }

    #[test]
    fn log_verify_rejects_invalid_trustee_signature() -> Result<()> {
        let db_path = temp_db_path();
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
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
        let (_, receipt) = BreakGlass::authorize(&policy, &request, &[approval.clone()], bucket);
        let _entry_hash = kernel.append_break_glass_receipt(&receipt, &[approval])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let policy = verify::load_break_glass_policy(&conn)?;
        let result = verify::verify_break_glass_receipts_with(
            &conn,
            &verifying_key,
            policy.as_ref(),
            |_, _| {},
        );
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }
}
