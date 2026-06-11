//! export_verify - Verify export bundle bytes against export receipts.
//!
//! This tool proves:
//! - The export receipt log is hash-chained (tamper-evident)
//! - Each receipt entry is signed by the device key (Ed25519)
//! - The bundle file bytes hash to a receipt recorded in the DB
//!
//! It does not attempt to reserialize or canonicalize JSON: raw file bytes are hashed.

use anyhow::{anyhow, Result};
use clap::{Parser, ValueEnum};
use rusqlite::Connection;
use sha2::{Digest, Sha256};
use std::io::IsTerminal;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{verify, verify_export_bundle, verify_helpers, ExportBundle, ExportReceipt};

#[path = "../ui.rs"]
mod ui;

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
    /// Device key seed (as used by the kernel/bridges). Derives the SQLCipher
    /// key (when --db-key is not given) and the verifying key (when no
    /// --public-key/--public-key-file is given) — same semantics as
    /// log_verify, so `DEVICE_KEY_SEED=... export_verify --db witness.db
    /// --bundle ...` verifies an owner self-export with the seed alone.
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
    // device key seed exactly as the kernel/bridges do (same logic as
    // log_verify).
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
    let public_key_hex: Option<String> = match (&args.public_key, args.device_key_seed.as_deref()) {
        (Some(hex), _) => Some(hex.clone()),
        (None, Some(seed)) if args.public_key_file.is_none() => Some(hex::encode(
            witness_kernel::verifying_key_from_seed(seed)?.to_bytes(),
        )),
        _ => None,
    };
    let verifying_key = {
        let _stage = ui.stage("Load verifying key");
        verify_helpers::load_verifying_key(
            &conn,
            public_key_hex.as_deref(),
            args.public_key_file.as_deref(),
        )?
    };
    let pq_verifying_key = {
        let _stage = ui.stage("Load PQ verifying key (optional)");
        verify_helpers::load_pq_verifying_key(
            &conn,
            args.pq_public_key.as_deref(),
            args.pq_public_key_file.as_deref(),
        )?
    };

    println!("export_verify: checking {}", args.bundle);
    println!();

    // Entry hashes verified below against the TRUSTED key (from DB/CLI/seed).
    // Bundle files are later required to reference one of these entries, so a
    // bundle re-signed under an attacker-chosen key can never pass.
    let mut verified_entry_hashes: Vec<[u8; 32]> = Vec::new();
    {
        let _stage = ui.stage("Verify export receipts");
        let count = verify::verify_export_receipts_with(
            &conn,
            &verifying_key,
            signature_mode,
            pq_verifying_key.as_ref(),
            |id, entry_hash| {
                verified_entry_hashes.push(entry_hash);
                if args.verbose {
                    println!(
                        "  receipt {}: hash={} OK",
                        id,
                        &verify_helpers::hex32(&entry_hash)[..16]
                    );
                }
            },
        )?;
        println!("verified {} export receipt entries", count);
    }
    println!();

    let bundle_bytes = {
        let _stage = ui.stage("Read bundle");
        std::fs::read(&args.bundle)
            .map_err(|e| anyhow!("failed to read bundle {}: {}", args.bundle, e))?
    };

    // The file is normally an ExportBundle as written by `export_events`
    // (artifact + signed receipt + keys). The bundle carries its own device
    // key, which an attacker could swap alongside a re-signed receipt, so
    // bundle-internal checks (verify_export_bundle: artifact binding + receipt
    // signature) prove only internal consistency. Trust comes from requiring
    // the bundled receipt entry to be one of the entries verified above under
    // the trusted key — that entry hash commits to the full receipt payload,
    // including auth_mode and window, so none of it can be rewritten.
    // A bare-artifact file (no receipt wrapper) is matched by its whole-file
    // hash against the trusted chain, as older exports produced.
    let artifact_hash: [u8; 32] = match serde_json::from_slice::<ExportBundle>(&bundle_bytes) {
        Ok(bundle) => {
            let _stage = ui.stage("Verify bundle receipt + artifact binding");
            verify_export_bundle(&bundle)?;
            if !verified_entry_hashes.contains(&bundle.receipt_entry.entry_hash) {
                return Err(anyhow!(
                    "TAMPER: bundle receipt entry not found in the verified export-receipt chain"
                ));
            }
            if let Some(mode) = bundle.receipt_entry.receipt.auth_mode {
                println!("bundle authorization: {:?}", mode);
            }
            bundle.receipt_entry.receipt.artifact_hash
        }
        Err(_) => Sha256::digest(&bundle_bytes).into(),
    };
    if args.verbose {
        println!("artifact hash: {}", verify_helpers::hex32(&artifact_hash));
    }

    let found = {
        let _stage = ui.stage("Match bundle hash");
        bundle_hash_in_export_receipts(&conn, &artifact_hash)?
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
