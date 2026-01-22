//! log_verify - External verifier for PWK sealed log integrity
//!
//! This tool proves:
//! - The sealed event log is hash-chained (tamper-evident)
//! - The break-glass receipt log is hash-chained (tamper-evident)
//! - Each entry is signed by the device key (MVP placeholder signing)
//! - Checkpoints preserve verifiability across retention pruning
//!
//! This is not a convenience feature.
//! It is a core anti-erosion mechanism: integrity must be provable without trusting the runtime.

use anyhow::{anyhow, Result};
use clap::Parser;
use rusqlite::{Connection, Row};
use sha2::{Digest, Sha256};

use witness_kernel::{hash_entry, sign_mvp};

#[derive(Parser, Debug)]
#[command(
    name = "log_verify",
    about = "Verify PWK sealed log integrity (hash-chain + signatures)"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db")]
    db: String,

    /// MVP device key seed (must match witnessd)
    #[arg(long, default_value = "devkey:mvp")]
    device_key_seed: String,

    /// Verbose output
    #[arg(short, long)]
    verbose: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();
    let conn = Connection::open(&args.db)?;

    // Derive MVP device key from seed (do not ship).
    let device_key: [u8; 32] = Sha256::digest(args.device_key_seed.as_bytes()).into();

    println!("log_verify: checking {}", args.db);
    println!();

    // === Sealed Events ===
    println!("=== Sealed Events ===");

    let (checkpoint_hash, checkpoint_sig, cutoff_event_id) = latest_checkpoint(&conn)?;

    if let (Some(head), Some(sig), Some(cutoff_id)) =
        (checkpoint_hash, checkpoint_sig, cutoff_event_id)
    {
        let expected = sign_mvp(&device_key, &head);
        if expected != sig {
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

    verify_events(&conn, &device_key, checkpoint_hash, args.verbose)?;
    println!();

    // === Break-Glass Receipts ===
    verify_break_glass_receipts(&conn, &device_key, args.verbose)?;

    println!("OK: all chains verified.");
    Ok(())
}

fn latest_checkpoint(
    conn: &Connection,
) -> Result<(Option<[u8; 32]>, Option<[u8; 32]>, Option<i64>)> {
    let mut stmt = conn.prepare(
        "SELECT chain_head_hash, signature, cutoff_event_id FROM checkpoints ORDER BY id DESC LIMIT 1",
    )?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let head = blob32(row, 0)?;
        let sig = blob32(row, 1)?;
        let cutoff_id: i64 = row.get(2)?;
        Ok((Some(head), Some(sig), Some(cutoff_id)))
    } else {
        Ok((None, None, None))
    }
}

fn verify_events(
    conn: &Connection,
    device_key: &[u8; 32],
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
        let sig = blob32(row, 4)?;

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

        let expected_sig = sign_mvp(device_key, &entry_hash);
        if expected_sig != sig {
            return Err(anyhow!(
                "signature mismatch at id {}: expected={}, stored={}",
                id,
                hex(&expected_sig),
                hex(&sig)
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
    device_key: &[u8; 32],
    verbose: bool,
) -> Result<()> {
    println!("=== Break-Glass Receipts ===");

    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, prev_hash, entry_hash, signature FROM break_glass_receipts ORDER BY id ASC",
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
        let prev_hash = blob32(row, 3)?;
        let entry_hash = blob32(row, 4)?;
        let sig = blob32(row, 5)?;

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

        let expected_sig = sign_mvp(device_key, &entry_hash);
        if expected_sig != sig {
            return Err(anyhow!(
                "receipt signature mismatch at id {}: expected={}, stored={}",
                id,
                hex(&expected_sig),
                hex(&sig)
            ));
        }

        // Parse to count outcomes
        if let Ok(receipt) = serde_json::from_str::<witness_kernel::BreakGlassReceipt>(&payload) {
            match receipt.outcome {
                witness_kernel::BreakGlassOutcome::Granted => granted += 1,
                witness_kernel::BreakGlassOutcome::Denied { .. } => denied += 1,
            }
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

fn hex(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
