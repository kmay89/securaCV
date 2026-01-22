//! break_glass - CLI for quorum-gated evidence access
//!
//! Implements Invariant V (Break-Glass by Quorum).
//!
//! MVP notes:
//! - Trustee signatures are placeholders (non-empty), not real Ed25519.
//! - This CLI demonstrates authorization + receipt logging.
//! - Vault decryption is not implemented here.

use anyhow::{anyhow, Result};
use clap::{Parser, Subcommand};
use sha2::{Digest, Sha256};

use crate::{
    Approval, BreakGlass, BreakGlassOutcome, Kernel, KernelConfig, QuorumPolicy, TimeBucket,
    TrusteeId, UnlockRequest,
};

#[derive(Parser, Debug)]
#[command(name = "break_glass", about = "Quorum-gated evidence access (Invariant V)")]
struct Args {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Create an unlock request for a vault envelope (prints request hash)
    Request {
        #[arg(long)]
        envelope: String,
        #[arg(long)]
        purpose: String,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
    },

    /// Create an approval file for a request hash (MVP placeholder signature)
    Approve {
        #[arg(long)]
        request_hash: String,
        #[arg(long)]
        trustee: String,
        #[arg(long)]
        output: String,
    },

    /// Authorize unlock using collected approval files
    Authorize {
        #[arg(long)]
        envelope: String,
        #[arg(long)]
        purpose: String,
        #[arg(long)]
        approvals: String,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        #[arg(long, default_value_t = 2)]
        threshold: u8,
        #[arg(long, default_value = "alice,bob,carol")]
        trustees: String,
    },

    /// List break-glass receipts
    Receipts {
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(short, long)]
        verbose: bool,
    },
}

pub fn run() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    match args.command {
        Command::Request {
            envelope,
            purpose,
            db: _,
            ruleset_id,
        } => cmd_request(&envelope, &purpose, &ruleset_id),
        Command::Approve {
            request_hash,
            trustee,
            output,
        } => cmd_approve(&request_hash, &trustee, &output),
        Command::Authorize {
            envelope,
            purpose,
            approvals,
            db,
            ruleset_id,
            threshold,
            trustees,
        } => cmd_authorize(
            &envelope,
            &purpose,
            &approvals,
            &db,
            &ruleset_id,
            threshold,
            &trustees,
        ),
        Command::Receipts { db, verbose } => cmd_receipts(&db, verbose),
    }
}

fn cmd_request(envelope: &str, purpose: &str, ruleset_id: &str) -> Result<()> {
    let bucket = TimeBucket::now_10min()?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    let request = UnlockRequest::new(envelope, ruleset_hash, purpose, bucket)?;
    let request_hash = request.request_hash();

    println!("=== Unlock Request ===");
    println!("Envelope:     {}", envelope);
    println!("Purpose:      {}", purpose);
    println!("Ruleset:      {}", ruleset_id);
    println!("Time bucket:  {} (size {}s)", bucket.start_epoch_s, bucket.size_s);
    println!("Request hash: {}", hex32(&request_hash));
    println!();
    println!("Share the request hash with trustees. Each trustee runs:");
    println!(
        "  break_glass approve --request-hash {} --trustee <name> --output <name>.approval",
        hex32(&request_hash)
    );
    Ok(())
}

fn cmd_approve(request_hash_hex: &str, trustee: &str, output: &str) -> Result<()> {
    let request_hash = parse_hex32(request_hash_hex)?;

    // MVP placeholder: deterministic non-empty "signature"
    let sig = Sha256::digest(
        format!(
            "mvp:{}:{:x}",
            trustee,
            u64::from_le_bytes(request_hash[0..8].try_into().unwrap())
        )
        .as_bytes(),
    );

    let approval = Approval::new(TrusteeId::new(trustee), request_hash, sig.to_vec());
    let json = serde_json::to_string_pretty(&approval)?;
    std::fs::write(output, json)?;

    println!("Approval written to: {}", output);
    Ok(())
}

fn cmd_authorize(
    envelope: &str,
    purpose: &str,
    approvals_arg: &str,
    db_path: &str,
    ruleset_id: &str,
    threshold: u8,
    trustees_arg: &str,
) -> Result<()> {
    let trustees: Vec<TrusteeId> = trustees_arg.split(',').map(|s| TrusteeId::new(s)).collect();
    let policy = QuorumPolicy::new(threshold, trustees)?;

    let bucket = TimeBucket::now_10min()?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    let request = UnlockRequest::new(envelope, ruleset_hash, purpose, bucket)?;

    let mut approvals: Vec<Approval> = Vec::new();
    for file in approvals_arg
        .split(',')
        .map(|s| s.trim())
        .filter(|s| !s.is_empty())
    {
        let json =
            std::fs::read_to_string(file).map_err(|e| anyhow!("failed to read {}: {}", file, e))?;
        let a: Approval =
            serde_json::from_str(&json).map_err(|e| anyhow!("failed to parse {}: {}", file, e))?;
        approvals.push(a);
    }

    println!("=== Break-Glass Authorization ===");
    println!("Envelope:  {}", envelope);
    println!("Policy:    {}-of-{}", policy.n, policy.m);
    println!("Approvals: {}", approvals.len());

    let (result, receipt) = BreakGlass::authorize(&policy, &request, &approvals, bucket);

    // Log receipt regardless of outcome
    let cfg = KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: ruleset_id.to_string(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: std::time::Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: "devkey:mvp".to_string(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    kernel.append_break_glass_receipt(&receipt)?;

    match result {
        Ok(mut token) => {
            println!("GRANTED");
            println!("Token nonce: {}", hex32(&token.token_nonce));
            println!("Expires:     bucket {}", token.expires_bucket.start_epoch_s);
            println!(
                "Trustees:    {:?}",
                receipt
                    .trustees_used
                    .iter()
                    .map(|t| t.0.clone())
                    .collect::<Vec<_>>()
            );
            // Demonstrate single-use consume path (no vault wired here)
            token.consume()?;
            Ok(())
        }
        Err(e) => {
            println!("DENIED: {}", e);
            if let BreakGlassOutcome::Denied { reason } = &receipt.outcome {
                println!("Reason: {}", reason);
            }
            Err(e)
        }
    }
}

fn cmd_receipts(db_path: &str, verbose: bool) -> Result<()> {
    let conn = rusqlite::Connection::open(db_path)?;
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, entry_hash FROM break_glass_receipts ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;

    println!("=== Break-Glass Receipts ===");
    let mut n = 0usize;
    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let entry_hash: Vec<u8> = row.get(3)?;
        let receipt: crate::BreakGlassReceipt = serde_json::from_str(&payload)?;
        let outcome = match &receipt.outcome {
            BreakGlassOutcome::Granted => "GRANTED",
            BreakGlassOutcome::Denied { .. } => "DENIED",
        };
        println!(
            "#{} @{}: {} envelope={} trustees={:?}",
            id,
            created_at,
            outcome,
            receipt.vault_envelope_id,
            receipt
                .trustees_used
                .iter()
                .map(|t| t.0.clone())
                .collect::<Vec<_>>()
        );
        if verbose {
            println!("  entry_hash: {}", hex_vec(&entry_hash));
            if let BreakGlassOutcome::Denied { reason } = &receipt.outcome {
                println!("  reason: {}", reason);
            }
        }
        n += 1;
    }
    println!("Total: {}", n);
    Ok(())
}

fn parse_hex32(s: &str) -> Result<[u8; 32]> {
    let b = hex::decode(s).map_err(|e| anyhow!("invalid hex: {}", e))?;
    if b.len() != 32 {
        return Err(anyhow!("expected 32 bytes, got {}", b.len()));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&b);
    Ok(out)
}

fn hex32(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn hex_vec(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
