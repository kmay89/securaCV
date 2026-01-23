//! export_verify - Verify export bundle bytes against export receipts.
//!
//! This tool proves:
//! - The export receipt log is hash-chained (tamper-evident)
//! - Each receipt entry is signed by the device key (Ed25519)
//! - The bundle file bytes hash to a receipt recorded in the DB
//!
//! It does not attempt to reserialize or canonicalize JSON: raw file bytes are hashed.

use anyhow::{anyhow, Result};
use clap::Parser;
use rusqlite::Connection;
use sha2::{Digest, Sha256};
use std::io::IsTerminal;

use witness_kernel::{verify, ExportReceipt};

#[path = "../ui.rs"]
mod ui;
mod verify_helpers;

#[derive(Parser, Debug)]
#[command(
    name = "export_verify",
    about = "Verify export bundle bytes against export receipts"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db")]
    db: String,

    /// Path to export bundle file
    #[arg(long, value_name = "PATH")]
    bundle: String,

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

    println!("export_verify: checking {}", args.bundle);
    println!();

    {
        let _stage = ui.stage("Verify export receipts");
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
    println!();

    let bundle_bytes = {
        let _stage = ui.stage("Read bundle");
        std::fs::read(&args.bundle)
            .map_err(|e| anyhow!("failed to read bundle {}: {}", args.bundle, e))?
    };
    let bundle_hash: [u8; 32] = Sha256::digest(&bundle_bytes).into();
    if args.verbose {
        println!("bundle hash: {}", verify_helpers::hex32(&bundle_hash));
    }

    let found = {
        let _stage = ui.stage("Match bundle hash");
        bundle_hash_in_export_receipts(&conn, &bundle_hash)?
    };
    if !found {
        return Err(anyhow!("TAMPER: bundle hash not found in export receipts"));
    }

    println!("OK: export bundle hash verified.");
    Ok(())
}

fn bundle_hash_in_export_receipts(conn: &Connection, bundle_hash: &[u8; 32]) -> Result<bool> {
    let mut stmt = conn.prepare("SELECT payload_json FROM export_receipts ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    while let Some(row) = rows.next()? {
        let payload: String = row.get(0)?;
        let receipt: ExportReceipt = serde_json::from_str(&payload)?;
        if &receipt.artifact_hash == bundle_hash {
            return Ok(true);
        }
    }
    Ok(false)
}
