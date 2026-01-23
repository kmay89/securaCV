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
use ed25519_dalek::{Signer, SigningKey, VerifyingKey};
use std::io::Write;

use crate::{
    approvals_commitment, break_glass::BreakGlassTokenFile,
    break_glass_receipt_outcome_for_verifier, device_public_key_from_db, hash_entry,
    verify_entry_signature, Approval, BreakGlass, BreakGlassOutcome, BreakGlassToken, Kernel,
    KernelConfig, TimeBucket, TrusteeId, UnlockRequest, Vault, VaultConfig,
};

#[derive(Parser, Debug)]
#[command(
    name = "break_glass",
    about = "Quorum-gated evidence access (Invariant V)"
)]
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
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
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
        /// Device public key (hex-encoded Ed25519 verifying key)
        #[arg(long, value_name = "HEX", conflicts_with = "public_key_file")]
        public_key: Option<String>,
        /// Path to file containing hex-encoded device public key
        #[arg(long, value_name = "PATH", conflicts_with = "public_key")]
        public_key_file: Option<String>,
        #[arg(short, long)]
        verbose: bool,
    },

    /// Unseal a vault envelope using a valid break-glass token
    Unseal {
        #[arg(long)]
        envelope: String,
        #[arg(long)]
        token: String,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        #[arg(long, default_value = "vault/envelopes")]
        vault_path: String,
        #[arg(long, default_value = "vault/unsealed")]
        output_dir: String,
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
            device_key_seed,
            output_token,
        } => cmd_authorize(
            &envelope,
            &purpose,
            &approvals,
            &db,
            &ruleset_id,
            &device_key_seed,
            output_token.as_deref(),
        ),
        Command::Receipts {
            db,
            public_key,
            public_key_file,
            verbose,
        } => cmd_receipts(
            &db,
            public_key.as_deref(),
            public_key_file.as_deref(),
            verbose,
        ),
        Command::Unseal {
            envelope,
            token,
            db,
            ruleset_id,
            vault_path,
            output_dir,
        } => cmd_unseal(
            &envelope,
            &token,
            &db,
            &ruleset_id,
            &vault_path,
            &output_dir,
        ),
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
    println!(
        "Time bucket:  {} (size {}s)",
        bucket.start_epoch_s, bucket.size_s
    );
    println!("Request hash: {}", hex32(&request_hash));
    println!();
    println!("Share the request hash with trustees. Each trustee runs:");
    println!(
        "  break_glass approve --request-hash {} --trustee <name> --signing-key <hex-key-file> --output <name>.approval",
        hex32(&request_hash)
    );
    println!("Trustee roster is loaded from the canonical policy in the kernel database.");
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

    let approval = Approval::new(TrusteeId::new(trustee), request_hash, signature.to_vec());
    let json = serde_json::to_string_pretty(&approval)?;
    std::fs::write(output, json)?;

    println!("Approval written to: {}", output);
    println!(
        "Trustee public key: {}",
        hex32(&signing_key.verifying_key().to_bytes())
    );
    Ok(())
}

struct AuthorizeArgs<'a> {
    envelope: &'a str,
    purpose: &'a str,
    approvals_arg: &'a str,
    db_path: &'a str,
    ruleset_id: &'a str,
    device_key_seed: &'a str,
    output_token: Option<&'a str>,
    bucket: TimeBucket,
}

fn cmd_authorize(
    envelope: &str,
    purpose: &str,
    approvals_arg: &str,
    db_path: &str,
    ruleset_id: &str,
    device_key_seed: &str,
    output_token: Option<&str>,
) -> Result<()> {
    let bucket = TimeBucket::now_10min()?;
    let args = AuthorizeArgs {
        envelope,
        purpose,
        approvals_arg,
        db_path,
        ruleset_id,
        device_key_seed,
        output_token,
        bucket,
    };
    cmd_authorize_with_bucket(args)
}

fn cmd_authorize_with_bucket(args: AuthorizeArgs<'_>) -> Result<()> {
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(args.ruleset_id);
    let cfg = KernelConfig {
        db_path: args.db_path.to_string(),
        ruleset_id: args.ruleset_id.to_string(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: std::time::Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: args.device_key_seed.to_string(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    let policy = kernel
        .break_glass_policy()
        .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?
        .clone();

    let request = UnlockRequest::new(args.envelope, ruleset_hash, args.purpose, args.bucket)?;

    let mut approvals: Vec<Approval> = Vec::new();
    for file in args
        .approvals_arg
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
    println!("Envelope:  {}", args.envelope);
    println!("Policy:    {}-of-{}", policy.n, policy.m);
    println!("Approvals: {}", approvals.len());

    let (result, receipt) = BreakGlass::authorize(&policy, &request, &approvals, args.bucket);

    // Log receipt regardless of outcome
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &approvals)?;

    match result {
        Ok(mut token) => {
            kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;
            for line in granted_lines(&receipt, &token) {
                println!("{line}");
            }
            if let Some(path) = args.output_token {
                write_token_to_file(path, &token)?;
                eprintln!(
                    "WARNING: Token nonce written to {}. Treat this file as highly sensitive.",
                    path
                );
            }
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

fn cmd_receipts(
    db_path: &str,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
    verbose: bool,
) -> Result<()> {
    let conn = rusqlite::Connection::open(db_path)?;
    let verifying_key = load_verifying_key(&conn, public_key_hex, public_key_file)?;
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, approvals_json, prev_hash, entry_hash, signature FROM break_glass_receipts ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;

    println!("=== Break-Glass Receipts ===");
    let mut n = 0usize;
    let mut expected_prev = [0u8; 32];
    let mut invalid_count = 0usize;
    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let approvals_json: String = row.get(3)?;
        let prev_hash_bytes: Vec<u8> = row.get(4)?;
        let entry_hash_bytes: Vec<u8> = row.get(5)?;
        let signature_bytes: Vec<u8> = row.get(6)?;
        let prev_hash = blob32_vec(prev_hash_bytes, "break_glass_receipts.prev_hash")?;
        let entry_hash = blob32_vec(entry_hash_bytes, "break_glass_receipts.entry_hash")?;
        let signature = blob64_vec(signature_bytes, "break_glass_receipts.signature")?;
        let receipt: crate::BreakGlassReceipt = serde_json::from_str(&payload)?;
        let outcome = match &receipt.outcome {
            BreakGlassOutcome::Granted => "GRANTED",
            BreakGlassOutcome::Denied { .. } => "DENIED",
        };

        let mut status = "VALID";
        let mut issues = Vec::new();
        if prev_hash != expected_prev {
            status = "INVALID";
            issues.push(format!(
                "prev_hash mismatch (stored={}, expected={})",
                hex_vec(&prev_hash),
                hex_vec(&expected_prev)
            ));
        }
        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            status = "INVALID";
            issues.push(format!(
                "entry_hash mismatch (computed={}, stored={})",
                hex_vec(&computed),
                hex_vec(&entry_hash)
            ));
        }
        if verify_entry_signature(&verifying_key, &entry_hash, &signature).is_err() {
            status = "INVALID";
            issues.push(format!(
                "signature mismatch (stored={})",
                hex_vec64(&signature)
            ));
        }
        match serde_json::from_str::<Vec<Approval>>(&approvals_json) {
            Ok(approvals) => {
                let commitment = approvals_commitment(&approvals);
                if commitment != receipt.approvals_commitment {
                    status = "INVALID";
                    issues.push(format!(
                        "approvals commitment mismatch (stored={}, expected={})",
                        hex_vec(&receipt.approvals_commitment),
                        hex_vec(&commitment)
                    ));
                }
            }
            Err(e) => {
                status = "INVALID";
                issues.push(format!("approvals parse error: {}", e));
            }
        }
        if status == "INVALID" {
            invalid_count += 1;
        }
        println!(
            "#{} @{}: {} envelope={} trustees={:?} [{}]",
            id,
            created_at,
            outcome,
            receipt.vault_envelope_id,
            receipt
                .trustees_used
                .iter()
                .map(|t| t.0.clone())
                .collect::<Vec<_>>(),
            status
        );
        if verbose {
            println!("  prev_hash: {}", hex_vec(&prev_hash));
            println!("  entry_hash: {}", hex_vec(&entry_hash));
            println!("  signature: {}", hex_vec64(&signature));
            if let BreakGlassOutcome::Denied { reason } = &receipt.outcome {
                println!("  reason: {}", reason);
            }
            if !issues.is_empty() {
                for issue in issues {
                    println!("  verification: {}", issue);
                }
            }
        } else if !issues.is_empty() {
            for issue in issues {
                println!("  verification: {}", issue);
            }
        }
        expected_prev = entry_hash;
        n += 1;
    }
    println!("Total: {}", n);
    if invalid_count > 0 {
        return Err(anyhow!(
            "receipt verification failed for {} entr{} (run log_verify for full audit)",
            invalid_count,
            if invalid_count == 1 { "y" } else { "ies" }
        ));
    }
    Ok(())
}

fn granted_lines(receipt: &crate::BreakGlassReceipt, token: &BreakGlassToken) -> Vec<String> {
    let expires_bucket = token.expires_bucket();
    vec![
        "GRANTED".to_string(),
        format!("Expires:     bucket {}", expires_bucket.start_epoch_s),
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

    let token_file = BreakGlassTokenFile::from_token(token);
    let payload = format!("{}\n", serde_json::to_string_pretty(&token_file)?);

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
    }

    #[cfg(not(unix))]
    {
        std::fs::write(path, payload)?;
    }
    Ok(())
}

fn read_token_from_file(path: &str) -> Result<BreakGlassTokenFile> {
    let json = std::fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read token file {}: {}", path, e))?;
    let token_file: BreakGlassTokenFile =
        serde_json::from_str(&json).map_err(|e| anyhow!("invalid token file: {}", e))?;
    Ok(token_file)
}

fn cmd_unseal(
    envelope: &str,
    token_path: &str,
    db_path: &str,
    ruleset_id: &str,
    vault_path: &str,
    output_dir: &str,
) -> Result<()> {
    let token_file = read_token_from_file(token_path)?;
    if token_file.vault_envelope_id != envelope {
        return Err(anyhow!(
            "token envelope id mismatch (token={}, requested={})",
            token_file.vault_envelope_id,
            envelope
        ));
    }
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    if token_file.ruleset_hash != hex::encode(ruleset_hash) {
        return Err(anyhow!(
            "token ruleset hash mismatch (token={}, expected={})",
            token_file.ruleset_hash,
            hex::encode(ruleset_hash)
        ));
    }
    let mut token = token_file.into_token()?;
    let vault = Vault::new(VaultConfig {
        local_path: vault_path.into(),
    })?;
    let conn = rusqlite::Connection::open(db_path)?;
    let verifying_key = device_public_key_from_db(&conn)?;
    let clear = vault.unseal(envelope, &mut token, ruleset_hash, &verifying_key, |hash| {
        break_glass_receipt_outcome_for_verifier(&conn, &verifying_key, hash)
    })?;

    let sanitized = crate::vault::sanitize_envelope_id(envelope)?;
    let output_dir_path = std::path::Path::new(output_dir);
    std::fs::create_dir_all(output_dir_path)?;
    let output_path = output_dir_path.join(format!("{}.raw", sanitized));

    #[cfg(unix)]
    {
        use std::fs::OpenOptions;
        use std::os::unix::fs::OpenOptionsExt;
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .mode(0o600)
            .open(&output_path)?;
        file.write_all(&clear)?;
    }

    #[cfg(not(unix))]
    {
        std::fs::write(&output_path, &clear)?;
    }

    println!("Unsealed envelope written to {}", output_path.display());
    Ok(())
}

fn load_verifying_key(
    conn: &rusqlite::Connection,
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

fn hex_vec64(b: &[u8; 64]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn blob32_vec(bytes: Vec<u8>, context: &str) -> Result<[u8; 32]> {
    if bytes.len() != 32 {
        return Err(anyhow!(
            "corrupt {}: expected 32 bytes, got {}",
            context,
            bytes.len()
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn blob64_vec(bytes: Vec<u8>, context: &str) -> Result<[u8; 64]> {
    if bytes.len() != 64 {
        return Err(anyhow!(
            "corrupt {}: expected 64 bytes, got {}",
            context,
            bytes.len()
        ));
    }
    let mut out = [0u8; 64];
    out.copy_from_slice(&bytes);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::SigningKey;
    use std::time::Duration;

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
            approvals_commitment: approvals_commitment(&[]),
            outcome: BreakGlassOutcome::Granted,
        };
        let token = BreakGlassToken::test_token_with(
            [1u8; 32],
            TimeBucket {
                start_epoch_s: 1234,
                size_s: 600,
            },
            "env",
            [3u8; 32],
        );
        let token_nonce = token.token_nonce();
        let expires_bucket = token.expires_bucket();
        assert_eq!(expires_bucket.start_epoch_s, 1234);
        assert_eq!(expires_bucket.size_s, 600);

        let output = granted_lines(&receipt, &token).join("\n");
        assert!(!output.contains("Token nonce"));
        assert!(!output.contains(&hex32(&token_nonce)));
    }

    #[test]
    fn test_token_matches_expected_fields() {
        let token = BreakGlassToken::test_token_with(
            [9u8; 32],
            TimeBucket {
                start_epoch_s: 1234,
                size_s: 600,
            },
            "env",
            [3u8; 32],
        );

        assert_eq!(token.vault_envelope_id(), "env");
        assert_eq!(token.ruleset_hash(), [3u8; 32]);
        assert_eq!(token.token_nonce(), [9u8; 32]);
        assert_eq!(token.expires_bucket().start_epoch_s, 1234);
    }

    #[test]
    fn authorize_rejects_unknown_trustee_from_db_policy() -> Result<()> {
        let temp_dir =
            std::env::temp_dir().join(format!("secura_break_glass_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");

        let ruleset_id = "ruleset:test";
        let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
        let cfg = KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash,
            kernel_version: "test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
        };
        let mut kernel = Kernel::open(&cfg)?;

        let alice_key = SigningKey::from_bytes(&[1u8; 32]);
        let policy = crate::break_glass::QuorumPolicy::new(
            1,
            vec![crate::break_glass::TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice_key.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&policy)?;

        let bucket = TimeBucket::now_10min()?;
        let request = UnlockRequest::new("vault:1", ruleset_hash, "audit", bucket)?;
        let bob_key = SigningKey::from_bytes(&[2u8; 32]);
        let signature = bob_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("bob"),
            request.request_hash(),
            signature.to_vec(),
        );
        let approval_path = temp_dir.join("bob.approval");
        std::fs::write(&approval_path, serde_json::to_string(&approval)?)?;

        let approval_arg = approval_path.to_string_lossy();
        let args = AuthorizeArgs {
            envelope: "vault:1",
            purpose: "audit",
            approvals_arg: approval_arg.as_ref(),
            db_path: cfg.db_path.as_str(),
            ruleset_id,
            device_key_seed: cfg.device_key_seed.as_str(),
            output_token: None,
            bucket,
        };
        let result = cmd_authorize_with_bucket(args);
        let err = result.expect_err("authorization should be denied");
        assert!(err.to_string().contains("unrecognized trustee approvals"));
        Ok(())
    }
}
