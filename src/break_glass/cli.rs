//! break_glass - CLI for quorum-gated evidence access
//!
//! Implements Invariant V (Break-Glass by Quorum).
//!
//! Notes:
//! - Trustee approvals are signed with local Ed25519 keys.
//! - This CLI demonstrates authorization + receipt logging.
//! - Vault decryption is not implemented here.

use anyhow::{anyhow, Result};
use clap::{Parser, Subcommand};
use ed25519_dalek::{Signer, SigningKey};

use crate::{
    Approval, BreakGlass, BreakGlassOutcome, BreakGlassToken, Kernel, KernelConfig, QuorumPolicy,
    TimeBucket, TrusteeEntry, TrusteeId, UnlockRequest,
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

    /// Create an approval file for a request hash (Ed25519 signature)
    Approve {
        #[arg(long)]
        request_hash: String,
        #[arg(long)]
        trustee: String,
        #[arg(long)]
        signing_key: String,
        #[arg(long)]
        output: String,
    },

    /// Authorize unlock using collected approval files (token output is sensitive)
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
        #[arg(long, help = "Comma-separated <id>:<pubkey-hex> entries")]
        trustees: String,
        #[arg(
            long,
            help = "Write token nonce to a file (0600). Handle securely; token output is sensitive."
        )]
        output_token: Option<String>,
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
            signing_key,
            output,
        } => cmd_approve(&request_hash, &trustee, &signing_key, &output),
        Command::Authorize {
            envelope,
            purpose,
            approvals,
            db,
            ruleset_id,
            threshold,
            trustees,
            output_token,
        } => cmd_authorize(
            &envelope,
            &purpose,
            &approvals,
            &db,
            &ruleset_id,
            threshold,
            &trustees,
            output_token.as_deref(),
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
        "  break_glass approve --request-hash {} --trustee <name> --signing-key <hex-key-file> --output <name>.approval",
        hex32(&request_hash)
    );
    println!("Collect trustee public keys and pass them to authorize as:");
    println!("  --trustees <name>:<pubkey-hex>,<name>:<pubkey-hex>");
    Ok(())
}

fn cmd_approve(
    request_hash_hex: &str,
    trustee: &str,
    signing_key_path: &str,
    output: &str,
) -> Result<()> {
    let request_hash = parse_hex32(request_hash_hex)?;

    let signing_key_hex = std::fs::read_to_string(signing_key_path)
        .map_err(|e| anyhow!("failed to read signing key {}: {}", signing_key_path, e))?;
    let signing_key_bytes = parse_hex32(signing_key_hex.trim())?;
    let signing_key = SigningKey::from_bytes(&signing_key_bytes);
    let signature = signing_key.sign(&request_hash);

    let approval = Approval::new(
        TrusteeId::new(trustee),
        request_hash,
        signature.to_vec(),
    );
    let json = serde_json::to_string_pretty(&approval)?;
    std::fs::write(output, json)?;

    println!("Approval written to: {}", output);
    println!(
        "Trustee public key: {}",
        hex32(&signing_key.verifying_key().to_bytes())
    );
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
    output_token: Option<&str>,
) -> Result<()> {
    let trustees = parse_trustees(trustees_arg)?;
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
            for line in granted_lines(&receipt, &token) {
                println!("{line}");
            }
            if let Some(path) = output_token {
                write_token_to_file(path, &token)?;
                eprintln!(
                    "WARNING: Token nonce written to {}. Treat this file as highly sensitive.",
                    path
                );
            }
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

fn parse_trustees(s: &str) -> Result<Vec<TrusteeEntry>> {
    let mut trustees = Vec::new();
    for entry in s.split(',').map(|v| v.trim()).filter(|v| !v.is_empty()) {
        let (id, key_hex) = entry
            .split_once(':')
            .ok_or_else(|| anyhow!("trustee entry must be <id>:<pubkey-hex>"))?;
        let public_key = parse_hex32(key_hex.trim())?;
        trustees.push(TrusteeEntry {
            id: TrusteeId::new(id),
            public_key,
        });
    }
    if trustees.is_empty() {
        return Err(anyhow!("no trustees provided"));
    }
    Ok(trustees)
}

fn granted_lines(receipt: &crate::BreakGlassReceipt, token: &BreakGlassToken) -> Vec<String> {
    vec![
        "GRANTED".to_string(),
        format!("Expires:     bucket {}", token.expires_bucket.start_epoch_s),
        format!(
            "Trustees:    {:?}",
            receipt
                .trustees_used
                .iter()
                .map(|t| t.0.clone())
                .collect::<Vec<_>>()
        ),
    ]
}

fn write_token_to_file(path: &str, token: &BreakGlassToken) -> Result<()> {
    use std::io::Write;

    let payload = format!(
        "{{\"token_nonce\":\"{}\",\"expires_bucket\":{}}}\n",
        hex32(&token.token_nonce),
        token.expires_bucket.start_epoch_s
    );

    #[cfg(unix)]
    {
        use std::fs::OpenOptions;
        use std::os::unix::fs::OpenOptionsExt;
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .mode(0o600)
            .open(path)?;
        file.write_all(payload.as_bytes())?;
        return Ok(());
    }

    #[cfg(not(unix))]
    {
        std::fs::write(path, payload)?;
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn granted_lines_do_not_include_token_nonce() {
        let receipt = crate::BreakGlassReceipt {
            vault_envelope_id: "env".to_string(),
            request_hash: [2u8; 32],
            ruleset_hash: [3u8; 32],
            time_bucket: TimeBucket {
                start_epoch_s: 1234,
                size_s: 600,
            },
            trustees_used: vec![TrusteeId::new("trustee")],
            outcome: BreakGlassOutcome::Granted,
        };
        let mut token = BreakGlassToken::test_token();
        token.token_nonce = [1u8; 32];
        token.expires_bucket = TimeBucket {
            start_epoch_s: 1234,
            size_s: 600,
        };
        token.vault_envelope_id = "env".to_string();
        token.ruleset_hash = [3u8; 32];

        let output = granted_lines(&receipt, &token).join("\n");
        assert!(!output.contains("Token nonce"));
        assert!(!output.contains(&hex32(&token.token_nonce)));
    }
}
