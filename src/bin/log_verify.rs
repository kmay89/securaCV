//! log_verify - External verifier for PWK sealed log integrity
//!
//! This tool proves:
//! - The sealed event log is hash-chained (tamper-evident)
//! - The break-glass receipt log is hash-chained (tamper-evident)
//! - Each entry is signed by the device key (Ed25519)
//! - Checkpoints preserve verifiability across retention pruning
//!
//! This is not a convenience feature.
//! It is a core anti-erosion mechanism: integrity must be provable without trusting the runtime.

use anyhow::{anyhow, Result};
use clap::Parser;
use ed25519_dalek::VerifyingKey;
use rusqlite::{Connection, Row};

use witness_kernel::{
    approvals_commitment, device_public_key_from_db, hash_entry, verify_entry_signature, Approval,
};

#[derive(Clone, Copy, Debug)]
struct CheckpointData {
    chain_head_hash: Option<[u8; 32]>,
    signature: Option<[u8; 64]>,
    cutoff_event_id: Option<i64>,
}

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
}

fn main() -> Result<()> {
    let args = Args::parse();
    let conn = Connection::open(&args.db)?;
    let verifying_key = load_verifying_key(
        &conn,
        args.public_key.as_deref(),
        args.public_key_file.as_deref(),
    )?;

    println!("log_verify: checking {}", args.db);
    println!();

    // === Sealed Events ===
    println!("=== Sealed Events ===");

    let checkpoint = latest_checkpoint(&conn)?;

    if let (Some(head), Some(sig), Some(cutoff_id)) = (
        checkpoint.chain_head_hash,
        checkpoint.signature,
        checkpoint.cutoff_event_id,
    )
    {
        if verify_entry_signature(&verifying_key, &head, &sig).is_err() {
            return Err(anyhow!("checkpoint signature mismatch"));
        }
        println!(
            "checkpoint: cutoff_event_id={}, chain_head_hash={}",
            cutoff_id,
            hex(&head)
        );
    } else {
        println!("checkpoint: none (genesis chain)");
    }

    let alarm_count = count_alarms(&conn)?;
    if alarm_count > 0 {
        println!("WARNING: {} conformance alarms recorded", alarm_count);
        if args.verbose {
            print_alarms(&conn)?;
        }
    }

    verify_events(&conn, &verifying_key, checkpoint.chain_head_hash, args.verbose)?;
    println!();

    // === Break-Glass Receipts ===
    verify_break_glass_receipts(&conn, &verifying_key, args.verbose)?;

    println!("OK: all chains verified.");
    Ok(())
}

fn load_verifying_key(
    conn: &Connection,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
) -> Result<VerifyingKey> {
    if let Some(hex) = public_key_hex {
        return verifying_key_from_hex(hex);
    }
    if let Some(path) = public_key_file {
        let key_hex = std::fs::read_to_string(path)
            .map_err(|e| anyhow!("failed to read public key file {}: {}", path, e))?;
        return verifying_key_from_hex(key_hex.trim());
    }
    device_public_key_from_db(conn).map_err(|e| {
        anyhow!(
            "{} (provide --public-key or --public-key-file if the database has no key)",
            e
        )
    })
}

fn verifying_key_from_hex(hex_str: &str) -> Result<VerifyingKey> {
    let bytes = hex::decode(hex_str.trim()).map_err(|e| anyhow!("invalid hex: {}", e))?;
    let mut key_bytes = [0u8; 32];
    if bytes.len() != 32 {
        return Err(anyhow!(
            "invalid public key length: expected 32 bytes, got {}",
            bytes.len()
        ));
    }
    key_bytes.copy_from_slice(&bytes);
    VerifyingKey::from_bytes(&key_bytes).map_err(|e| anyhow!("invalid public key bytes: {}", e))
}

fn latest_checkpoint(conn: &Connection) -> Result<CheckpointData> {
    let mut stmt = conn.prepare(
        "SELECT chain_head_hash, signature, cutoff_event_id FROM checkpoints ORDER BY id DESC LIMIT 1",
    )?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let head = blob32(row, 0)?;
        let sig = blob64(row, 1)?;
        let cutoff_id: i64 = row.get(2)?;
        Ok(CheckpointData {
            chain_head_hash: Some(head),
            signature: Some(sig),
            cutoff_event_id: Some(cutoff_id),
        })
    } else {
        Ok(CheckpointData {
            chain_head_hash: None,
            signature: None,
            cutoff_event_id: None,
        })
    }
}

fn verify_events(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    checkpoint_head: Option<[u8; 32]>,
    verbose: bool,
) -> Result<()> {
    let mut stmt = conn.prepare(
        "SELECT id, payload_json, prev_hash, entry_hash, signature FROM sealed_events ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev: [u8; 32] = checkpoint_head.unwrap_or([0u8; 32]);
    let mut count = 0u64;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let payload: String = row.get(1)?;
        let prev_hash = blob32(row, 2)?;
        let entry_hash = blob32(row, 3)?;
        let sig = blob64(row, 4)?;

        if prev_hash != expected_prev {
            return Err(anyhow!(
                "chain break at id {}: prev_hash={}, expected_prev={}",
                id,
                hex(&prev_hash),
                hex(&expected_prev)
            ));
        }

        let computed = hash_entry(&prev_hash, payload.as_bytes());
        if computed != entry_hash {
            return Err(anyhow!(
                "hash mismatch at id {}: computed={}, stored={}",
                id,
                hex(&computed),
                hex(&entry_hash)
            ));
        }

        if verify_entry_signature(verifying_key, &entry_hash, &sig).is_err() {
            return Err(anyhow!(
                "signature mismatch at id {}: stored={}",
                id,
                hex64(&sig)
            ));
        }

        if verbose {
            println!("  event {}: hash={} OK", id, &hex(&entry_hash)[..16]);
        }

        expected_prev = entry_hash;
        count += 1;
    }

    println!("verified {} event entries", count);
    Ok(())
}

fn verify_break_glass_receipts(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    verbose: bool,
) -> Result<()> {
    println!("=== Break-Glass Receipts ===");

    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, approvals_json, prev_hash, entry_hash, signature FROM break_glass_receipts ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev = [0u8; 32]; // genesis
    let mut count = 0u64;
    let mut granted = 0u64;
    let mut denied = 0u64;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let _created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let approvals_json: String = row.get(3)?;
        let prev_hash = blob32(row, 4)?;
        let entry_hash = blob32(row, 5)?;
        let sig = blob64(row, 6)?;

        if prev_hash != expected_prev {
            return Err(anyhow!(
                "receipt chain break at id {}: prev_hash={}, expected_prev={}",
                id,
                hex(&prev_hash),
                hex(&expected_prev)
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(anyhow!(
                "receipt hash mismatch at id {}: computed={}, stored={}",
                id,
                hex(&computed),
                hex(&entry_hash)
            ));
        }

        if verify_entry_signature(verifying_key, &entry_hash, &sig).is_err() {
            return Err(anyhow!(
                "receipt signature mismatch at id {}: stored={}",
                id,
                hex64(&sig)
            ));
        }

        let receipt: witness_kernel::BreakGlassReceipt = serde_json::from_str(&payload)?;
        match receipt.outcome {
            witness_kernel::BreakGlassOutcome::Granted => granted += 1,
            witness_kernel::BreakGlassOutcome::Denied { .. } => denied += 1,
        }
        let approvals: Vec<Approval> = serde_json::from_str(&approvals_json)?;
        let commitment = approvals_commitment(&approvals);
        if commitment != receipt.approvals_commitment {
            return Err(anyhow!(
                "receipt approvals commitment mismatch at id {}: stored={}, expected={}",
                id,
                hex(&receipt.approvals_commitment),
                hex(&commitment)
            ));
        }

        if verbose {
            println!("  receipt {}: hash={} OK", id, &hex(&entry_hash)[..16]);
        }

        expected_prev = entry_hash;
        count += 1;
    }

    println!(
        "verified {} receipt entries ({} granted, {} denied)",
        count, granted, denied
    );
    Ok(())
}

fn count_alarms(conn: &Connection) -> Result<i64> {
    let mut stmt = conn.prepare("SELECT COUNT(*) FROM conformance_alarms")?;
    let count: i64 = stmt.query_row([], |row| row.get(0))?;
    Ok(count)
}

fn print_alarms(conn: &Connection) -> Result<()> {
    let mut stmt =
        conn.prepare("SELECT created_at, code, message FROM conformance_alarms ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    while let Some(row) = rows.next()? {
        let created_at: i64 = row.get(0)?;
        let code: String = row.get(1)?;
        let message: String = row.get(2)?;
        println!("  ALARM @{}: {} - {}", created_at, code, message);
    }
    Ok(())
}

fn blob32(row: &Row<'_>, idx: usize) -> Result<[u8; 32]> {
    let bytes: Vec<u8> = row.get(idx)?;
    if bytes.len() != 32 {
        return Err(anyhow!("expected 32-byte blob at col {}", idx));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn blob64(row: &Row<'_>, idx: usize) -> Result<[u8; 64]> {
    let bytes: Vec<u8> = row.get(idx)?;
    if bytes.len() != 64 {
        return Err(anyhow!("expected 64-byte blob at col {}", idx));
    }
    let mut out = [0u8; 64];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn hex(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn hex64(b: &[u8; 64]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use std::path::PathBuf;
    use witness_kernel::{
        Approval, BreakGlass, CandidateEvent, EventType, Kernel, KernelConfig, ModuleDescriptor,
        QuorumPolicy, TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest,
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
        })?;
        write_test_event(&mut kernel)?;
        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = latest_checkpoint(&conn)?;
        verify_events(&conn, &verifying_key, checkpoint.chain_head_hash, false)?;
        verify_break_glass_receipts(&conn, &verifying_key, false)?;

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
        })?;
        write_test_event(&mut kernel)?;
        drop(kernel);

        let wrong_signing_key = SigningKey::from_bytes(&[42u8; 32]);
        let public_key_hex = hex::encode(wrong_signing_key.verifying_key().to_bytes());

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let checkpoint = latest_checkpoint(&conn)?;
        let result = verify_events(&conn, &verifying_key, checkpoint.chain_head_hash, false);
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
        kernel.append_break_glass_receipt(&receipt, &[approval.clone()])?;

        kernel
            .conn
            .execute("UPDATE break_glass_receipts SET approvals_json = '[]'", [])?;

        let public_key_hex = hex::encode(kernel.device_key_for_verify_only());
        drop(kernel);

        let conn = Connection::open(&db_path)?;
        let verifying_key = load_verifying_key(&conn, Some(&public_key_hex), None)?;
        let result = verify_break_glass_receipts(&conn, &verifying_key, false);
        assert!(result.is_err());

        let _ = std::fs::remove_file(&db_path);
        Ok(())
    }
}
