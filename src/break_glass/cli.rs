//! break_glass - CLI for quorum-gated evidence access
//!
//! Implements Invariant V (Break-Glass by Quorum).
//!
//! Notes:
//! - Trustee approvals are signed with local Ed25519 keys.
//! - This CLI demonstrates authorization + receipt logging.
//! - Vault unsealing is implemented via `break_glass unseal` and writes a clear
//!   envelope to the specified output directory.

use anyhow::{anyhow, Result};
use clap::{Parser, Subcommand};
use ed25519_dalek::{SigningKey, VerifyingKey};
use rand::TryRng;
use std::io::IsTerminal;
use std::io::Write;
use zeroize::Zeroize;

use crate::crypto::signatures::{SignatureMode, SignatureSet, DOMAIN_BREAK_GLASS_RECEIPT};
use crate::{
    approvals_commitment, break_glass::BreakGlassTokenFile,
    break_glass_receipt_outcome_for_verifier, device_public_key_from_db, hash_entry,
    verify_entry_signature, Approval, BreakGlass, BreakGlassOutcome, BreakGlassToken, Kernel,
    KernelConfig, TimeBucket, TrusteeId, UnlockRequest, Vault, VaultConfig, ZonePolicy,
};

#[path = "../ui.rs"]
mod ui;

#[derive(Parser, Debug)]
#[command(
    name = "break_glass",
    about = "Quorum-gated evidence access (Invariant V)"
)]
struct Args {
    #[command(subcommand)]
    command: Command,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
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
        /// Also write the full request context to a file trustees can approve
        /// from (`approve --request`), so they sign what they can SEE — not a
        /// bare hash someone read to them.
        #[arg(long, value_name = "PATH")]
        output_request: Option<String>,
    },

    /// Create an approval file for an unlock request (Ed25519 signature).
    /// Prefer `--request <file>`: your own tool recomputes the hash from the
    /// displayed fields, so you sign what you see. `--request-hash` alone is
    /// blind signing and prints a warning.
    Approve {
        /// Request-context file written by `request --output-request`.
        /// The hash is recomputed locally from its fields before signing.
        #[arg(long, value_name = "PATH", conflicts_with = "request_hash")]
        request: Option<String>,
        /// Bare request hash (blind signing — you cannot verify what this
        /// authorizes; prefer --request)
        #[arg(long, value_name = "HEX")]
        request_hash: Option<String>,
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

    /// Manage break-glass quorum policy stored in the kernel database
    Policy {
        #[command(subcommand)]
        command: PolicyCommand,
    },

    /// Health-check the break-glass setup: quorum policy, device identity, and
    /// vault key material. Exits non-zero if anything is missing or invalid, so
    /// it can gate a deploy. Read-only.
    Doctor {
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(
            long,
            default_value = "vault/envelopes",
            help = "Vault directory (where master.key lives)"
        )]
        vault_path: String,
        /// Device key seed (must match witnessd) — needed to open the encrypted
        /// kernel database.
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
    },

    /// Rehearse a full break-glass (request → approve → authorize → seal →
    /// unseal) in a throwaway sandbox — temp database, temp vault, and ephemeral
    /// trustee keys, all discarded afterward. Proves the machinery works on this
    /// build before you need it in a real incident. Touches nothing real.
    Drill {
        /// Quorum threshold to rehearse (n)
        #[arg(long, default_value_t = 2)]
        threshold: u8,
        /// Number of trustees in the rehearsed quorum (m)
        #[arg(long, default_value_t = 3)]
        trustees: u8,
    },

    /// Start a guided break-glass setup: pin the device identity and open a
    /// draft for an n-of-m quorum. Enroll trustees with `trustee enroll`; the
    /// quorum policy commits automatically once ALL m are enrolled (changing a
    /// live policy afterwards needs current-quorum approvals). Idempotent.
    Init {
        /// Quorum threshold (n) — how many trustees must approve
        #[arg(long)]
        threshold: u8,
        /// Target number of trustees (m) you plan to enroll
        #[arg(long)]
        trustees: u8,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
        /// Setup-draft file (defaults to <db>.setup-draft.json)
        #[arg(long)]
        draft: Option<String>,
    },

    /// Manage trustees during setup
    Trustee {
        #[command(subcommand)]
        command: TrusteeCommand,
    },
}

#[derive(Subcommand, Debug)]
enum TrusteeCommand {
    /// Enroll one trustee into the setup draft. Either import an existing public
    /// key (`--public-key`) or mint a fresh keypair (`--generate --output`). The
    /// quorum policy commits automatically once the roster is complete.
    Enroll {
        /// Trustee id (a short human label, unique within the quorum)
        #[arg(long)]
        id: String,
        /// Mint a fresh Ed25519 keypair for this trustee (writes the signing key
        /// to --output). Mutually exclusive with --public-key.
        #[arg(long, conflicts_with = "public_key")]
        generate: bool,
        /// Where to write the newly-minted signing key (required with --generate)
        #[arg(long)]
        output: Option<String>,
        /// Import an existing 32-byte Ed25519 public key (hex)
        #[arg(long, value_name = "HEX")]
        public_key: Option<String>,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
        /// Setup-draft file (defaults to <db>.setup-draft.json)
        #[arg(long)]
        draft: Option<String>,
    },
}

#[derive(Subcommand, Debug)]
enum PolicyCommand {
    /// Store a quorum policy in the kernel database. On an empty database
    /// this is the bootstrap and needs no approvals; when a policy already
    /// exists, changing it requires current-quorum approvals (collect them
    /// with `policy propose` + `policy approve`, then pass --approvals).
    Set {
        /// Quorum threshold (n)
        #[arg(long)]
        threshold: u8,
        /// Trustee entry in the format "id:HEX_PUBLIC_KEY"
        #[arg(long = "trustee", value_name = "ID:HEX", action = clap::ArgAction::Append)]
        trustees: Vec<String>,
        /// Vault crypto mode (classical|pq|hybrid). Defaults to the stored
        /// policy's mode when one exists, else classical — so a roster change
        /// can never silently downgrade the vault crypto.
        #[arg(long, value_name = "MODE")]
        crypto_mode: Option<String>,
        /// Comma-separated policy-change approval files from CURRENT trustees
        /// (required when replacing an existing policy)
        #[arg(long, value_name = "FILES")]
        approvals: Option<String>,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
    },

    /// Propose a change to the stored quorum policy: writes a proposal file
    /// carrying the FULL current and proposed policies, for trustees to
    /// review and approve with `policy approve`.
    Propose {
        /// Proposed quorum threshold (n)
        #[arg(long)]
        threshold: u8,
        /// Proposed trustee entry in the format "id:HEX_PUBLIC_KEY"
        #[arg(long = "trustee", value_name = "ID:HEX", action = clap::ArgAction::Append)]
        trustees: Vec<String>,
        /// Proposed vault crypto mode (classical|pq|hybrid); defaults to the
        /// stored policy's mode
        #[arg(long, value_name = "MODE")]
        crypto_mode: Option<String>,
        /// Where to write the proposal file
        #[arg(long, value_name = "PATH")]
        output: String,
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
    },

    /// Approve a proposed policy change as a CURRENT trustee. Recomputes the
    /// change hash locally from the proposal's full contents and displays
    /// exactly what is changing before signing — you sign what you see.
    Approve {
        /// Proposal file written by `policy propose`
        #[arg(long, value_name = "PATH")]
        proposal: String,
        #[arg(long)]
        trustee: String,
        #[arg(long)]
        signing_key: String,
        #[arg(long)]
        output: String,
    },

    /// Print the stored quorum policy
    Show {
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
    },

    /// Print the policy-change history ledger (chained, device-signed)
    History {
        #[arg(long, default_value = "witness.db")]
        db: String,
        #[arg(long, default_value = "ruleset:v0.3.0")]
        ruleset_id: String,
        /// Device key seed (must match witnessd)
        #[arg(long, env = "DEVICE_KEY_SEED")]
        device_key_seed: String,
    },
}

pub fn run() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let disable_pretty = !stdout_is_tty
        || matches!(
            args.command,
            Command::Policy {
                command: PolicyCommand::Show { .. }
            }
        );
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, disable_pretty);

    match args.command {
        Command::Request {
            envelope,
            purpose,
            db: _,
            ruleset_id,
            output_request,
        } => {
            let _stage = ui.stage("Create unlock request");
            cmd_request(&envelope, &purpose, &ruleset_id, output_request.as_deref())
        }
        Command::Approve {
            request,
            request_hash,
            trustee,
            signing_key,
            output,
        } => {
            let _stage = ui.stage("Sign approval");
            cmd_approve(
                request.as_deref(),
                request_hash.as_deref(),
                &trustee,
                &signing_key,
                &output,
            )
        }
        Command::Authorize {
            envelope,
            purpose,
            approvals,
            db,
            ruleset_id,
            device_key_seed,
            output_token,
        } => {
            let _stage = ui.stage("Authorize break-glass");
            cmd_authorize(
                &envelope,
                &purpose,
                &approvals,
                &db,
                &ruleset_id,
                &device_key_seed,
                output_token.as_deref(),
            )
        }
        Command::Receipts {
            db,
            public_key,
            public_key_file,
            verbose,
        } => {
            let _stage = ui.stage("Verify receipts");
            cmd_receipts(
                &db,
                public_key.as_deref(),
                public_key_file.as_deref(),
                verbose,
            )
        }
        Command::Unseal {
            envelope,
            token,
            db,
            ruleset_id,
            vault_path,
            output_dir,
        } => {
            let _stage = ui.stage("Unseal envelope");
            cmd_unseal(
                &envelope,
                &token,
                &db,
                &ruleset_id,
                &vault_path,
                &output_dir,
            )
        }
        Command::Policy { command } => match command {
            PolicyCommand::Set {
                threshold,
                trustees,
                crypto_mode,
                approvals,
                db,
                ruleset_id,
                device_key_seed,
            } => {
                let _stage = ui.stage("Set quorum policy");
                cmd_policy_set(
                    threshold,
                    &trustees,
                    crypto_mode.as_deref(),
                    approvals.as_deref(),
                    &db,
                    &ruleset_id,
                    &device_key_seed,
                )
            }
            PolicyCommand::Propose {
                threshold,
                trustees,
                crypto_mode,
                output,
                db,
                ruleset_id,
                device_key_seed,
            } => {
                let _stage = ui.stage("Propose policy change");
                cmd_policy_propose(
                    threshold,
                    &trustees,
                    crypto_mode.as_deref(),
                    &output,
                    &db,
                    &ruleset_id,
                    &device_key_seed,
                )
            }
            PolicyCommand::Approve {
                proposal,
                trustee,
                signing_key,
                output,
            } => {
                let _stage = ui.stage("Approve policy change");
                cmd_policy_change_approve(&proposal, &trustee, &signing_key, &output)
            }
            PolicyCommand::Show {
                db,
                ruleset_id,
                device_key_seed,
            } => {
                let _stage = ui.stage("Show quorum policy");
                cmd_policy_show(&db, &ruleset_id, &device_key_seed)
            }
            PolicyCommand::History {
                db,
                ruleset_id,
                device_key_seed,
            } => {
                let _stage = ui.stage("Policy-change history");
                cmd_policy_history(&db, &ruleset_id, &device_key_seed)
            }
        },
        Command::Doctor {
            db,
            vault_path,
            device_key_seed,
        } => {
            let _stage = ui.stage("Vault doctor");
            cmd_doctor(&db, &vault_path, &device_key_seed)
        }
        Command::Drill {
            threshold,
            trustees,
        } => {
            let _stage = ui.stage("Break-glass drill");
            cmd_drill(threshold, trustees)
        }
        Command::Init {
            threshold,
            trustees,
            db,
            ruleset_id,
            device_key_seed,
            draft,
        } => {
            let _stage = ui.stage("Break-glass init");
            cmd_init(
                threshold,
                trustees,
                &db,
                &ruleset_id,
                &device_key_seed,
                draft.as_deref(),
            )
        }
        Command::Trustee { command } => match command {
            TrusteeCommand::Enroll {
                id,
                generate,
                output,
                public_key,
                db,
                ruleset_id,
                device_key_seed,
                draft,
            } => {
                let _stage = ui.stage("Enroll trustee");
                cmd_trustee_enroll(
                    &id,
                    generate,
                    output.as_deref(),
                    public_key.as_deref(),
                    &db,
                    &ruleset_id,
                    &device_key_seed,
                    draft.as_deref(),
                )
            }
        },
    }
}

/// The portable request-context file `request --output-request` writes and
/// `approve --request` consumes. Carries every field the request hash binds,
/// so a trustee's own tool can recompute the hash and display the decoded
/// request before signing (WYSIWYS — spec/quorum_unseal_v2.md §3.2).
#[derive(serde::Serialize, serde::Deserialize)]
struct UnlockRequestFile {
    format: String,
    envelope: String,
    purpose: String,
    ruleset_id: String,
    ruleset_hash: String,
    time_bucket: TimeBucket,
    request_hash: String,
}

const UNLOCK_REQUEST_FILE_FORMAT: &str = "securacv-unlock-request:v1";

/// The portable policy-change proposal file `policy propose` writes and
/// `policy approve` consumes. Carries the FULL current and proposed policies
/// so a trustee reviews the actual rosters, not a summary someone narrated.
#[derive(serde::Serialize, serde::Deserialize)]
struct PolicyChangeProposalFile {
    format: String,
    prev_policy: Option<crate::break_glass::QuorumPolicy>,
    new_policy: crate::break_glass::QuorumPolicy,
    time_bucket: TimeBucket,
    change_hash: String,
}

const POLICY_CHANGE_PROPOSAL_FORMAT: &str = "securacv-policy-change-proposal:v1";

/// Render an epoch-seconds instant as a UTC timestamp for human review
/// (display only — never part of any hashed or signed material).
fn utc_string(epoch_s: u64) -> String {
    let days = (epoch_s / 86_400) as i64;
    let secs = epoch_s % 86_400;
    // Civil-from-days (Howard Hinnant's algorithm), valid for the Unix era.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
        y,
        m,
        d,
        secs / 3600,
        (secs % 3600) / 60,
        secs % 60
    )
}

fn bucket_window(bucket: &TimeBucket) -> String {
    format!(
        "{} .. {} (bucket start {}, size {}s)",
        utc_string(bucket.start_epoch_s),
        utc_string(bucket.start_epoch_s + bucket.size_s as u64),
        bucket.start_epoch_s,
        bucket.size_s
    )
}

fn cmd_request(
    envelope: &str,
    purpose: &str,
    ruleset_id: &str,
    output_request: Option<&str>,
) -> Result<()> {
    let bucket = TimeBucket::now_10min()?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    let request = UnlockRequest::new(envelope, ruleset_hash, purpose, bucket)?;
    let request_hash = request.request_hash();

    println!("=== Unlock Request ===");
    println!("Envelope:     {}", envelope);
    println!("Purpose:      {}", purpose);
    println!("Ruleset:      {}", ruleset_id);
    println!("Valid window: {}", bucket_window(&bucket));
    println!("Request hash: {}", hex32(&request_hash));
    println!();
    if let Some(path) = output_request {
        let file = UnlockRequestFile {
            format: UNLOCK_REQUEST_FILE_FORMAT.to_string(),
            envelope: envelope.trim().to_string(),
            purpose: purpose.trim().to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash: hex32(&ruleset_hash),
            time_bucket: bucket,
            request_hash: hex32(&request_hash),
        };
        std::fs::write(path, format!("{}\n", serde_json::to_string_pretty(&file)?))?;
        println!("Request context written to: {}", path);
        println!(
            "Send the FILE to each trustee (full context, never a bare hash). Each trustee runs:"
        );
        println!(
            "  break_glass approve --request {} --trustee <name> --signing-key <hex-key-file> --output <name>.approval",
            path
        );
    } else {
        println!("Share the request hash with trustees. Each trustee runs:");
        println!(
            "  break_glass approve --request-hash {} --trustee <name> --signing-key <hex-key-file> --output <name>.approval",
            hex32(&request_hash)
        );
        println!(
            "Better: re-run with --output-request <file> and send trustees the full context \
             file instead, so their tool can show them what they are signing."
        );
    }
    println!("Trustee roster is loaded from the canonical policy in the kernel database.");
    Ok(())
}

fn read_signing_key(signing_key_path: &str) -> Result<SigningKey> {
    let signing_key_hex = std::fs::read_to_string(signing_key_path)
        .map_err(|e| anyhow!("failed to read signing key {}: {}", signing_key_path, e))?;
    let signing_key_bytes = parse_hex32(signing_key_hex.trim())?;
    Ok(SigningKey::from_bytes(&signing_key_bytes))
}

fn cmd_approve(
    request_file: Option<&str>,
    request_hash_hex: Option<&str>,
    trustee: &str,
    signing_key_path: &str,
    output: &str,
) -> Result<()> {
    let request_hash = match (request_file, request_hash_hex) {
        (Some(path), _) => {
            let json = std::fs::read_to_string(path)
                .map_err(|e| anyhow!("failed to read request file {}: {}", path, e))?;
            let file: UnlockRequestFile = serde_json::from_str(&json)
                .map_err(|e| anyhow!("invalid request file {}: {}", path, e))?;
            if file.format != UNLOCK_REQUEST_FILE_FORMAT {
                return Err(anyhow!(
                    "unrecognized request file format '{}' (expected {})",
                    file.format,
                    UNLOCK_REQUEST_FILE_FORMAT
                ));
            }
            // Recompute everything locally from the request's FIELDS. The
            // file's own claimed hashes are cross-checked, never trusted:
            // the signature is over what this tool derived from what it is
            // about to display.
            let derived_ruleset = KernelConfig::ruleset_hash_from_id(&file.ruleset_id);
            let claimed_ruleset = parse_hex32(&file.ruleset_hash)?;
            if derived_ruleset != claimed_ruleset {
                return Err(anyhow!(
                    "request file inconsistent: ruleset_hash does not match ruleset_id '{}' — refusing to sign",
                    file.ruleset_id
                ));
            }
            let request = UnlockRequest::new(
                &file.envelope,
                derived_ruleset,
                &file.purpose,
                file.time_bucket,
            )?;
            let recomputed = request.request_hash();
            let claimed = parse_hex32(&file.request_hash)?;
            if recomputed != claimed {
                return Err(anyhow!(
                    "request file inconsistent: recomputed request hash {} does not match its claimed hash {} — refusing to sign",
                    hex32(&recomputed),
                    hex32(&claimed)
                ));
            }

            println!("=== Unlock request — review before signing ===");
            println!("Envelope:     {}", file.envelope);
            println!("Purpose:      {}", file.purpose);
            println!("Ruleset:      {}", file.ruleset_id);
            println!("Valid window: {}", bucket_window(&file.time_bucket));
            println!("Request hash: {} (recomputed locally)", hex32(&recomputed));
            if let Ok(now) = TimeBucket::now_10min() {
                if now.start_epoch_s != file.time_bucket.start_epoch_s {
                    println!(
                        "WARNING: this request's time window is not the current one — \
                         authorization only succeeds inside the request's own window."
                    );
                }
            }
            println!();
            recomputed
        }
        (None, Some(hex_str)) => {
            eprintln!(
                "WARNING: signing a bare request hash is BLIND SIGNING — this tool cannot \
                 show you what it authorizes. Ask the operator for the request context file \
                 (`request --output-request`) and use --request instead."
            );
            parse_hex32(hex_str)?
        }
        (None, None) => {
            return Err(anyhow!(
                "provide --request <file> (preferred) or --request-hash <hex>"
            ))
        }
    };

    let signing_key = read_signing_key(signing_key_path)?;
    // Domain-separated to DOMAIN_TRUSTEE_APPROVAL so this consent cannot be
    // replayed as (or from) any other signature context.
    let approval = Approval::signed(TrusteeId::new(trustee), request_hash, &signing_key);
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
    let cfg = kernel_config(args.db_path, args.ruleset_id, args.device_key_seed);
    let mut kernel = Kernel::open(&cfg)?;
    let policy = kernel
        .break_glass_policy()
        .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?
        .clone();

    let request = UnlockRequest::new(args.envelope, cfg.ruleset_hash, args.purpose, args.bucket)?;

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

/// Read a comma-separated list of approval files (the same format
/// `authorize --approvals` consumes).
fn read_approval_files(arg: &str) -> Result<Vec<Approval>> {
    let mut approvals: Vec<Approval> = Vec::new();
    for file in arg.split(',').map(|s| s.trim()).filter(|s| !s.is_empty()) {
        let json =
            std::fs::read_to_string(file).map_err(|e| anyhow!("failed to read {}: {}", file, e))?;
        let a: Approval =
            serde_json::from_str(&json).map_err(|e| anyhow!("failed to parse {}: {}", file, e))?;
        approvals.push(a);
    }
    Ok(approvals)
}

/// Build the proposed policy for `policy set` / `policy propose`: parse the
/// roster, then resolve the vault crypto mode — explicit flag wins, else the
/// STORED policy's mode is preserved (a roster change must never silently
/// downgrade the vault crypto), else the default.
fn build_proposed_policy(
    threshold: u8,
    trustees: &[String],
    crypto_mode: Option<&str>,
    current: Option<&crate::break_glass::QuorumPolicy>,
) -> Result<crate::break_glass::QuorumPolicy> {
    let entries = trustees
        .iter()
        .map(|entry| parse_trustee_entry(entry))
        .collect::<Result<Vec<_>>>()?;
    let mut policy = crate::break_glass::QuorumPolicy::new(threshold, entries)?;
    policy.vault.crypto_mode = match crypto_mode {
        Some(mode) => mode.parse()?,
        None => match current {
            Some(cur) => cur.vault.crypto_mode,
            None => Default::default(),
        },
    };
    Ok(policy)
}

fn print_policy_roster(label: &str, policy: &crate::break_glass::QuorumPolicy) {
    println!(
        "{}: {}-of-{} (crypto mode: {})",
        label, policy.n, policy.m, policy.vault.crypto_mode
    );
    for trustee in &policy.trustees {
        println!(
            "  trustee {} {}",
            trustee.id.0,
            hex_vec(&trustee.public_key)
        );
    }
}

fn cmd_policy_set(
    threshold: u8,
    trustees: &[String],
    crypto_mode: Option<&str>,
    approvals_arg: Option<&str>,
    db_path: &str,
    ruleset_id: &str,
    device_key_seed: &str,
) -> Result<()> {
    let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
    let mut kernel = Kernel::open(&cfg)?;
    let current = kernel.break_glass_policy().cloned();
    let policy = build_proposed_policy(threshold, trustees, crypto_mode, current.as_ref())?;

    let approvals = match approvals_arg {
        Some(arg) => read_approval_files(arg)?,
        None => Vec::new(),
    };
    let bucket = TimeBucket::now_10min()?;

    // Quorum-gated (Invariant V): replacing a live policy needs the CURRENT
    // quorum's consent; only the bootstrap on an empty database does not.
    let outcome = kernel.set_break_glass_policy_gated(&policy, &approvals, bucket)?;
    match outcome {
        crate::PolicyChangeOutcome::Bootstrapped => {
            println!("Stored break-glass policy (bootstrap — no prior policy existed):");
        }
        crate::PolicyChangeOutcome::Replaced => {
            println!("Replaced break-glass policy with current-quorum consent:");
        }
        crate::PolicyChangeOutcome::Unchanged => {
            println!("Stored policy is identical — nothing changed.");
            return Ok(());
        }
    }
    print_policy_roster("Policy", &policy);
    Ok(())
}

fn cmd_policy_propose(
    threshold: u8,
    trustees: &[String],
    crypto_mode: Option<&str>,
    output: &str,
    db_path: &str,
    ruleset_id: &str,
    device_key_seed: &str,
) -> Result<()> {
    let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
    let kernel = Kernel::open(&cfg)?;
    let Some(current) = kernel.break_glass_policy().cloned() else {
        return Err(anyhow!(
            "no policy is stored yet — the first policy is a bootstrap and needs no proposal; \
             run `break_glass policy set` directly"
        ));
    };
    let new_policy = build_proposed_policy(threshold, trustees, crypto_mode, Some(&current))?;
    if current.full_commitment() == new_policy.full_commitment() {
        return Err(anyhow!(
            "the proposed policy is identical to the stored policy — nothing to change"
        ));
    }

    let bucket = TimeBucket::now_10min()?;
    let proposal = crate::break_glass::PolicyChangeProposal {
        prev_policy_commitment: current.full_commitment(),
        new_policy: new_policy.clone(),
        time_bucket: bucket,
    };
    let change_hash = proposal.change_hash();
    let file = PolicyChangeProposalFile {
        format: POLICY_CHANGE_PROPOSAL_FORMAT.to_string(),
        prev_policy: Some(current.clone()),
        new_policy: new_policy.clone(),
        time_bucket: bucket,
        change_hash: hex32(&change_hash),
    };
    std::fs::write(
        output,
        format!("{}\n", serde_json::to_string_pretty(&file)?),
    )?;

    println!("=== Policy change proposal ===");
    print_policy_roster("Current", &current);
    print_policy_roster("Proposed", &new_policy);
    println!("Valid window: {}", bucket_window(&bucket));
    println!("Change hash:  {}", hex32(&change_hash));
    println!("Proposal written to: {}", output);
    println!();
    println!(
        "Send the FILE to {} current trustee(s). Each runs:",
        current.n
    );
    println!(
        "  break_glass policy approve --proposal {} --trustee <name> --signing-key <hex-key-file> --output <name>.policy-approval",
        output
    );
    println!("Then apply within the window: break_glass policy set … --approvals <files>");
    Ok(())
}

fn cmd_policy_change_approve(
    proposal_path: &str,
    trustee: &str,
    signing_key_path: &str,
    output: &str,
) -> Result<()> {
    let json = std::fs::read_to_string(proposal_path)
        .map_err(|e| anyhow!("failed to read proposal {}: {}", proposal_path, e))?;
    let file: PolicyChangeProposalFile = serde_json::from_str(&json)
        .map_err(|e| anyhow!("invalid proposal file {}: {}", proposal_path, e))?;
    if file.format != POLICY_CHANGE_PROPOSAL_FORMAT {
        return Err(anyhow!(
            "unrecognized proposal format '{}' (expected {})",
            file.format,
            POLICY_CHANGE_PROPOSAL_FORMAT
        ));
    }
    let Some(prev_policy) = &file.prev_policy else {
        return Err(anyhow!(
            "proposal carries no current policy — a bootstrap needs no approvals"
        ));
    };
    prev_policy.validate()?;
    file.new_policy.validate()?;

    // Recompute the change hash from the proposal's FULL contents; the file's
    // claimed hash is cross-checked, never trusted. What is displayed below
    // is exactly what the recomputed hash commits to.
    let proposal = crate::break_glass::PolicyChangeProposal {
        prev_policy_commitment: prev_policy.full_commitment(),
        new_policy: file.new_policy.clone(),
        time_bucket: file.time_bucket,
    };
    let recomputed = proposal.change_hash();
    let claimed = parse_hex32(&file.change_hash)?;
    if recomputed != claimed {
        return Err(anyhow!(
            "proposal inconsistent: recomputed change hash {} does not match its claimed hash {} — refusing to sign",
            hex32(&recomputed),
            hex32(&claimed)
        ));
    }

    println!("=== Policy change — review before signing ===");
    print_policy_roster("Current", prev_policy);
    print_policy_roster("Proposed", &file.new_policy);
    let removed: Vec<&str> = prev_policy
        .trustees
        .iter()
        .filter(|t| {
            !file
                .new_policy
                .trustees
                .iter()
                .any(|n| n.public_key == t.public_key)
        })
        .map(|t| t.id.0.as_str())
        .collect();
    let added: Vec<&str> = file
        .new_policy
        .trustees
        .iter()
        .filter(|t| {
            !prev_policy
                .trustees
                .iter()
                .any(|p| p.public_key == t.public_key)
        })
        .map(|t| t.id.0.as_str())
        .collect();
    if !removed.is_empty() {
        println!("Removes trustee key(s): {}", removed.join(", "));
    }
    if !added.is_empty() {
        println!("Adds trustee key(s):    {}", added.join(", "));
    }
    if prev_policy.vault.crypto_mode != file.new_policy.vault.crypto_mode {
        println!(
            "Changes vault crypto mode: {} -> {}",
            prev_policy.vault.crypto_mode, file.new_policy.vault.crypto_mode
        );
    }
    println!("Valid window: {}", bucket_window(&file.time_bucket));
    println!("Change hash:  {} (recomputed locally)", hex32(&recomputed));
    println!(
        "Verify the CURRENT-policy fingerprint out of band with the operator: {}",
        hex32(&prev_policy.full_commitment())
    );
    println!();

    let signing_key = read_signing_key(signing_key_path)?;
    // Signed under DOMAIN_POLICY_CHANGE_APPROVAL — disjoint from unlock
    // approvals, so this consent authorizes exactly one thing.
    let signature =
        crate::break_glass::sign_policy_change_approval(&signing_key, &recomputed).to_vec();
    let approval = Approval::new(TrusteeId::new(trustee), recomputed, signature);
    std::fs::write(output, serde_json::to_string_pretty(&approval)?)?;

    println!("Policy-change approval written to: {}", output);
    println!(
        "Trustee public key: {}",
        hex32(&signing_key.verifying_key().to_bytes())
    );
    Ok(())
}

fn cmd_policy_history(db_path: &str, ruleset_id: &str, device_key_seed: &str) -> Result<()> {
    let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
    let kernel = Kernel::open(&cfg)?;
    let device_vk = kernel.device_verifying_key();
    let entries = kernel.policy_change_history()?;
    println!("=== Policy-change history ===");
    if entries.is_empty() {
        println!("(no policy changes recorded)");
        return Ok(());
    }
    let mut expected_prev = [0u8; 32];
    let mut prev_new_commitment: Option<[u8; 32]> = None;
    let mut invalid = 0usize;
    for (i, entry) in entries.iter().enumerate() {
        let mut issues = Vec::new();
        if entry.prev_hash != expected_prev {
            issues.push("prev_hash mismatch".to_string());
        }
        let computed = hash_entry(&expected_prev, entry.payload_json.as_bytes());
        if computed != entry.entry_hash {
            issues.push("entry_hash mismatch".to_string());
        }
        let sig_ok = <[u8; 64]>::try_from(entry.signature.as_slice())
            .ok()
            .map(|sig| {
                crate::crypto::signatures::verify_ed25519_only(
                    crate::crypto::signatures::DOMAIN_POLICY_CHANGE_RECORD,
                    &device_vk,
                    &entry.entry_hash,
                    &sig,
                )
                .is_ok()
            })
            .unwrap_or(false);
        if !sig_ok {
            issues.push("device signature invalid".to_string());
        }
        // Approvals binding: the stored approvals must match the commitment
        // inside the signed payload.
        if approvals_commitment(&entry.approvals) != entry.record.approvals_commitment {
            issues.push("approvals do not match the signed commitment".to_string());
        }
        // Per-transition quorum consent: a non-bootstrap change must carry the
        // prior era's N-of-M consent, chain from the prior era, and commit to
        // the right change hash. Only the first row may be a bootstrap.
        if entry.record.bootstrap {
            if i != 0 {
                issues.push("bootstrap after the first entry".to_string());
            }
        } else if let Some(prev) = &entry.record.prev_policy {
            if let Some(prev_commitment) = prev_new_commitment {
                if prev.full_commitment() != prev_commitment {
                    issues.push("prev_policy is not the prior era".to_string());
                }
            }
            let expected_change_hash = crate::break_glass::PolicyChangeProposal {
                prev_policy_commitment: prev.full_commitment(),
                new_policy: entry.record.new_policy.clone(),
                time_bucket: entry.record.time_bucket,
            }
            .change_hash();
            if expected_change_hash != entry.record.change_hash {
                issues.push("change_hash does not match its policies".to_string());
            }
            let valid = crate::break_glass::count_valid_distinct_policy_change_approvals(
                prev,
                &entry.record.change_hash,
                &entry.approvals,
            );
            if valid < prev.n as usize {
                issues.push(format!(
                    "only {} of {} required prior-quorum approvals valid",
                    valid, prev.n
                ));
            }
        } else {
            issues.push("non-bootstrap change without a prev_policy".to_string());
        }
        let status = if issues.is_empty() {
            "VALID"
        } else {
            "INVALID"
        };
        if !issues.is_empty() {
            invalid += 1;
        }
        let kind = if entry.record.bootstrap {
            "bootstrap"
        } else {
            "change"
        };
        println!(
            "[{}] {} {} — {}-of-{} (crypto mode: {}), {} approval(s), window {}{}",
            i + 1,
            status,
            kind,
            entry.record.new_policy.n,
            entry.record.new_policy.m,
            entry.record.new_policy.vault.crypto_mode,
            entry.approvals.len(),
            bucket_window(&entry.record.time_bucket),
            if issues.is_empty() {
                String::new()
            } else {
                format!(" [{}]", issues.join("; "))
            }
        );
        expected_prev = entry.entry_hash;
        prev_new_commitment = Some(entry.record.new_policy.full_commitment());
    }
    if invalid > 0 {
        return Err(anyhow!(
            "policy-change history INVALID: {} of {} entries failed verification",
            invalid,
            entries.len()
        ));
    }
    println!("History chain VALID ({} entries).", entries.len());
    Ok(())
}

fn cmd_policy_show(db_path: &str, ruleset_id: &str, device_key_seed: &str) -> Result<()> {
    let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
    let kernel = Kernel::open(&cfg)?;
    let policy = kernel
        .break_glass_policy()
        .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?;
    println!("{}", serde_json::to_string_pretty(policy)?);
    Ok(())
}

// Doctor status glyphs (ASCII-safe meaning is clear even if a terminal drops the
// codepoint): check / cross / warning.
const DR_OK: &str = "\u{2713}"; // ✓
const DR_BAD: &str = "\u{2717}"; // ✗
const DR_WARN: &str = "\u{26a0}"; // ⚠

/// Short, human-comparable fingerprint of a 32-byte key (first 8 bytes, hex).
fn short_fp(bytes: &[u8; 32]) -> String {
    bytes[..8].iter().map(|b| format!("{:02x}", b)).collect()
}

fn count_receipts(conn: &rusqlite::Connection) -> Result<i64> {
    Ok(
        conn.query_row("SELECT COUNT(*) FROM break_glass_receipts", [], |r| {
            r.get(0)
        })?,
    )
}

/// Open the SQLCipher-encrypted kernel database, deriving the key from the
/// device seed exactly as the kernel and `log_verify` do. Fails if the seed is
/// wrong or the database is corrupt (the key check reads a page). Read-only use.
fn open_kernel_db_keyed(db_path: &str, device_key_seed: &str) -> Result<rusqlite::Connection> {
    let signing_key = crate::signing_key_from_seed(device_key_seed)?;
    let seed_env = crate::db_key_seed_from_env();
    let db_key =
        crate::resolve_db_encryption_key(&signing_key, seed_env.as_ref().map(|s| s.as_str()));
    let conn = rusqlite::Connection::open(db_path)?;
    conn.pragma_update(None, "key", format!("x'{}'", db_key.as_str()))?;
    conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
        .map_err(|_| {
            anyhow!(
                "could not open the kernel database — wrong device key seed or corrupt database"
            )
        })?;
    Ok(conn)
}

/// Inspect the break-glass setup and report health. Read-only; never writes.
/// Exits non-zero (via the returned `Err`) if anything is missing or invalid so
/// it can gate a deploy. Warnings (e.g. the master key being plaintext on disk,
/// which is the honest state until hardware-backed keys land) do not fail.
fn cmd_doctor(db_path: &str, vault_path: &str, device_key_seed: &str) -> Result<()> {
    let mut problems = 0usize;
    let mut warnings = 0usize;

    println!("=== Break-glass vault doctor ===");
    println!("Kernel DB:  {}", db_path);
    println!("Vault path: {}", vault_path);
    println!();

    // --- quorum policy + device identity + receipts (all live in the DB) ---
    println!("[ quorum policy ]");
    if !std::path::Path::new(db_path).exists() {
        println!(
            "  {} no kernel database at {} — set a policy with `break_glass policy set`",
            DR_BAD, db_path
        );
        problems += 1;
    } else {
        match open_kernel_db_keyed(db_path, device_key_seed)
            .and_then(|conn| load_break_glass_policy(&conn).map(|p| (conn, p)))
        {
            Ok((conn, Some(policy))) => {
                println!(
                    "  {} quorum policy configured: {}-of-{}",
                    DR_OK, policy.n, policy.m
                );
                let mut bad_keys = 0usize;
                for t in &policy.trustees {
                    if VerifyingKey::from_bytes(&t.public_key).is_ok() {
                        println!("    · {}  {}", t.id.0, short_fp(&t.public_key));
                    } else {
                        println!(
                            "    {} trustee {} has an invalid public key",
                            DR_BAD, t.id.0
                        );
                        bad_keys += 1;
                    }
                }
                if bad_keys == 0 {
                    println!(
                        "  {} {} trustee key(s), all well-formed",
                        DR_OK,
                        policy.trustees.len()
                    );
                } else {
                    problems += bad_keys;
                }
                println!("    crypto mode: {:?}", policy.vault.crypto_mode);

                println!();
                println!("[ device identity ]");
                // Verify the supplied seed actually derives the pinned identity —
                // not just that *some* key is pinned. When SECURACV_DB_KEY_SEED is
                // set, the DB key is independent of the device seed, so the DB can
                // open with the WRONG device seed; without this check `doctor`
                // would report HEALTHY while `authorize`/`witnessd` (which pin the
                // identity via Kernel::open) reject the same seed.
                let seed_vk =
                    crate::signing_key_from_seed(device_key_seed).map(|sk| sk.verifying_key());
                match (device_public_key_from_db(&conn), seed_vk) {
                    (Ok(pinned), Ok(vk)) if pinned.to_bytes() == vk.to_bytes() => println!(
                        "  {} device public key pinned, matches the supplied seed: {}",
                        DR_OK,
                        short_fp(&pinned.to_bytes())
                    ),
                    (Ok(pinned), Ok(vk)) => {
                        println!(
                            "  {} device seed mismatch: DB is pinned to {} but the supplied \
                             seed derives {} — authorize/witnessd will reject this seed",
                            DR_BAD,
                            short_fp(&pinned.to_bytes()),
                            short_fp(&vk.to_bytes())
                        );
                        problems += 1;
                    }
                    (Ok(pinned), Err(_)) => {
                        println!(
                            "  {} device public key pinned ({}) but the supplied seed is unusable",
                            DR_BAD,
                            short_fp(&pinned.to_bytes())
                        );
                        problems += 1;
                    }
                    (Err(_), _) => {
                        println!("  {} no device public key pinned in the database", DR_BAD);
                        problems += 1;
                    }
                }

                println!();
                println!("[ history ]");
                match count_receipts(&conn) {
                    Ok(n) if n > 0 => println!("    break-glass receipts logged: {}", n),
                    Ok(_) => println!(
                        "    no break-glass receipts yet — rehearse the flow before you need it"
                    ),
                    Err(_) => println!("    (receipts table not initialized yet)"),
                }
            }
            Ok((_conn, None)) => {
                println!(
                    "  {} no quorum policy configured — set one with `break_glass policy set`",
                    DR_BAD
                );
                problems += 1;
            }
            Err(e) => {
                // Either the database wouldn't open with this seed, or the stored
                // policy failed validation — both are hard problems.
                println!("  {} {}", DR_BAD, e);
                problems += 1;
            }
        }
    }

    // --- vault master key ---
    println!();
    println!("[ vault master key ]");
    let master = std::path::Path::new(vault_path).join("master.key");
    if !master.exists() {
        println!(
            "  {} master.key not found at {} — it is created on first seal; \
             this vault directory has not been initialized yet",
            DR_WARN,
            master.display()
        );
        warnings += 1;
    } else {
        match std::fs::metadata(&master) {
            Ok(meta) => {
                if meta.len() == 32 {
                    println!(
                        "  {} master.key present ({} bytes) at {}",
                        DR_OK,
                        meta.len(),
                        master.display()
                    );
                } else {
                    println!(
                        "  {} master.key at {} is {} bytes, expected 32",
                        DR_BAD,
                        master.display(),
                        meta.len()
                    );
                    problems += 1;
                }
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let mode = meta.permissions().mode() & 0o777;
                    if mode == 0o600 {
                        println!("  {} permissions are 0600 (owner-only)", DR_OK);
                    } else if mode & 0o077 != 0 {
                        // Group/other can read the vault master secret — a real
                        // hole: a local user could decrypt sealed evidence with no
                        // trustee quorum (Invariant V). Hard failure, not a warning.
                        println!(
                            "  {} permissions are {:o} — the master key is readable beyond its \
                             owner; anyone with that access can decrypt the vault WITHOUT quorum. \
                             Run `chmod 600 {}`",
                            DR_BAD,
                            mode,
                            master.display()
                        );
                        problems += 1;
                    } else {
                        // Owner-only but not exactly 0600 (e.g. 0400/0700): not a
                        // leak, just unusual — nudge toward the canonical mode.
                        println!(
                            "  {} permissions are {:o} (owner-only, but not 0600) — run `chmod 600 {}`",
                            DR_WARN,
                            mode,
                            master.display()
                        );
                        warnings += 1;
                    }
                }
            }
            Err(e) => {
                println!("  {} cannot read master.key metadata: {}", DR_BAD, e);
                problems += 1;
            }
        }
        println!(
            "  {} the master key is plaintext on disk — no hardware backing yet (file backend)",
            DR_WARN
        );
        println!("    hardware-backed keys are v1.1; see docs/design/vault_operator_ux_v1_1.md");
        warnings += 1;
    }

    // --- key store backend ---
    println!();
    println!("[ key store ]");
    println!("    backend: file (the only backend today)");

    // --- summary + exit gate ---
    println!();
    if problems == 0 {
        println!("Result: HEALTHY ({} warning(s))", warnings);
        Ok(())
    } else {
        println!("Result: {} PROBLEM(S), {} warning(s)", problems, warnings);
        Err(anyhow!(
            "vault doctor found {} problem(s) — see the report above",
            problems
        ))
    }
}

// Throwaway device seed for the drill's temp database. Fixed is fine: every drill
// runs in its own fresh tempdir, so the DB (and this seed's derived keys) never
// escape the sandbox or collide with a real deployment.
const DRILL_SEED: &str = "devkey:drill:00112233445566778899";

/// Run one request → approve → authorize cycle against the (temp) kernel and
/// return a signed break-glass token, logging a receipt in the process — exactly
/// what `authorize` does, minus the file shuffling. `approver_keys` are the first
/// `n` trustee keys (their ids taken from the policy in the same order).
fn drill_authorize(
    kernel: &mut Kernel,
    policy: &crate::break_glass::QuorumPolicy,
    envelope: &str,
    ruleset_hash: [u8; 32],
    approver_keys: &[SigningKey],
) -> Result<BreakGlassToken> {
    let bucket = TimeBucket::now_10min()?;
    let request = UnlockRequest::new(
        envelope,
        ruleset_hash,
        "DRILL — rehearsal, not a real disclosure",
        bucket,
    )?;
    let approvals: Vec<Approval> = approver_keys
        .iter()
        .zip(policy.trustees.iter())
        .map(|(key, trustee)| Approval::signed(trustee.id.clone(), request.request_hash(), key))
        .collect();
    let (result, receipt) = BreakGlass::authorize(policy, &request, &approvals, bucket);
    let receipt_entry_hash = kernel.log_break_glass_receipt(&receipt, &approvals)?;
    let mut token = result.map_err(|e| anyhow!("drill quorum authorization failed: {}", e))?;
    kernel.sign_break_glass_token(&mut token, receipt_entry_hash)?;
    Ok(token)
}

/// Rehearse the whole break-glass path in an isolated sandbox and report whether
/// it works end to end. Everything — the kernel DB, the vault, the trustee keys —
/// is ephemeral and discarded when the temp dir drops, so this touches nothing
/// real. Exits non-zero (via the returned `Err`) if any step fails.
fn cmd_drill(threshold: u8, trustees: u8) -> Result<()> {
    if threshold == 0 || trustees == 0 || threshold > trustees {
        return Err(anyhow!(
            "drill needs 1 <= threshold <= trustees (got {}-of-{})",
            threshold,
            trustees
        ));
    }

    println!("=== Break-glass drill ===");
    println!(
        "Rehearsing a {}-of-{} quorum end to end in a throwaway sandbox — temp DB, \
         temp vault, ephemeral keys, all discarded after. Nothing real is touched.",
        threshold, trustees
    );
    println!();

    let temp = tempfile::tempdir()?;
    let db_str = temp.path().join("witness.db").to_string_lossy().to_string();
    let vault_path = temp.path().join("vault");

    let cfg = kernel_config(&db_str, "ruleset:drill", DRILL_SEED);
    let ruleset_hash = cfg.ruleset_hash;
    let envelope = "drill-rehearsal";

    // Authorize both tokens (seal + unseal) up front, then drop the kernel so the
    // vault side reads the receipts through its own connection — no two writers.
    let device_vk;
    let crypto_mode;
    let mut seal_token;
    let mut unseal_token;
    {
        let mut kernel = Kernel::open(&cfg)?;
        device_vk = kernel.device_verifying_key();

        let trustee_keys: Vec<SigningKey> = (0..trustees)
            .map(|i| SigningKey::from_bytes(&[0xA0u8.wrapping_add(i); 32]))
            .collect();
        let entries: Vec<crate::break_glass::TrusteeEntry> = trustee_keys
            .iter()
            .enumerate()
            .map(|(i, key)| crate::break_glass::TrusteeEntry {
                id: TrusteeId::new(&format!("drill-trustee-{}", i + 1)),
                public_key: key.verifying_key().to_bytes(),
            })
            .collect();
        let policy = crate::break_glass::QuorumPolicy::new(threshold, entries)?;
        kernel.set_break_glass_policy(&policy)?;
        crypto_mode = policy.vault.crypto_mode;
        println!(
            "  {} set up a {}-of-{} sandbox quorum",
            DR_OK, policy.n, policy.m
        );

        let approvers = &trustee_keys[..threshold as usize];
        seal_token = drill_authorize(&mut kernel, &policy, envelope, ruleset_hash, approvers)?;
        unseal_token = drill_authorize(&mut kernel, &policy, envelope, ruleset_hash, approvers)?;
        println!(
            "  {} {} trustees approved; two break-glass tokens minted (one to seal, one to unseal)",
            DR_OK, threshold
        );
    }

    let vault = Vault::new(VaultConfig {
        local_path: vault_path,
        crypto_mode,
        key_material: crate::VaultKeyMaterial::from_env(),
    })?;
    let conn = open_kernel_db_keyed(&db_str, DRILL_SEED)?;
    #[cfg(feature = "pqc-signatures")]
    let pq_pk = crate::device_pq_public_key_from_db(&conn).ok();
    #[cfg(not(feature = "pqc-signatures"))]
    let pq_pk: Option<crate::crypto::signatures::PqPublicKey> = None;

    let payload =
        b"SecuraCV break-glass drill \xE2\x80\x94 throwaway payload, safe to discard".to_vec();
    let mut raw = payload.clone();
    vault.seal(
        envelope,
        &mut seal_token,
        ruleset_hash,
        &mut raw,
        &device_vk,
        |hash| {
            break_glass_receipt_outcome_for_verifier(
                &conn,
                &device_vk,
                envelope,
                ruleset_hash,
                hash,
                pq_pk.as_ref(),
            )
        },
    )?;
    println!("  {} sealed a throwaway envelope", DR_OK);

    // Burn-first, exactly as `unseal` does, then recover.
    crate::consume_break_glass_token_durably(&conn, &unseal_token)?;
    let clear = vault.unseal(
        envelope,
        &mut unseal_token,
        ruleset_hash,
        &device_vk,
        |hash| {
            break_glass_receipt_outcome_for_verifier(
                &conn,
                &device_vk,
                envelope,
                ruleset_hash,
                hash,
                pq_pk.as_ref(),
            )
        },
    )?;
    println!("  {} unsealed it with a fresh quorum token", DR_OK);

    println!();
    if clear == payload {
        println!(
            "  {} recovered payload matches the original, byte for byte",
            DR_OK
        );
        println!();
        println!("Result: DRILL PASSED — break-glass works end to end on this build.");
        Ok(())
    } else {
        println!(
            "  {} recovered payload does NOT match what was sealed",
            DR_BAD
        );
        Err(anyhow!(
            "drill failed: the unsealed payload did not match the sealed payload"
        ))
    }
}

// --------------------------------------------------------------------------- //
// Guided setup: `init` + `trustee enroll`, backed by a plaintext draft file
// that is kept SEPARATE from the committed QuorumPolicy. QuorumPolicy::validate
// rejects an empty/partial roster by design (Invariant V), so the draft is where
// a half-finished enrollment lives until it can commit a valid n-of-m — the
// validator never sees a partial roster.
// --------------------------------------------------------------------------- //

#[derive(serde::Serialize, serde::Deserialize)]
struct DraftTrustee {
    id: String,
    public_key_hex: String,
}

#[derive(serde::Serialize, serde::Deserialize)]
struct SetupDraft {
    threshold: u8,
    target_trustees: u8,
    trustees: Vec<DraftTrustee>,
}

/// The draft lives next to the database unless overridden. It holds only public
/// keys and counts — no secrets — so it is a plain (non-encrypted) file.
fn draft_path(db_path: &str, draft_override: Option<&str>) -> String {
    match draft_override {
        Some(p) => p.to_string(),
        None => format!("{}.setup-draft.json", db_path),
    }
}

fn read_draft(path: &str) -> Result<Option<SetupDraft>> {
    if !std::path::Path::new(path).exists() {
        return Ok(None);
    }
    let json = std::fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read setup draft {}: {}", path, e))?;
    let draft: SetupDraft =
        serde_json::from_str(&json).map_err(|e| anyhow!("invalid setup draft {}: {}", path, e))?;
    Ok(Some(draft))
}

fn write_draft(path: &str, draft: &SetupDraft) -> Result<()> {
    let json = format!("{}\n", serde_json::to_string_pretty(draft)?);
    std::fs::write(path, json).map_err(|e| anyhow!("failed to write setup draft {}: {}", path, e))
}

/// Write secret bytes to `path` with restricted permissions, hardened against
/// two failure modes the plain `OpenOptions().mode(0o600)` pattern has.
///
/// 1. **Symlink following.** `O_NOFOLLOW` makes the open fail if the final
///    path component is a symlink, so a pre-planted symlink cannot redirect
///    the write (or, on read-back, leak the secret through an attacker's link).
/// 2. **Mode ignored on a pre-existing file.** `.mode()` applies only when the
///    file is *created*; if the path already exists (perhaps mode 0644), the
///    requested 0600 is silently ignored. An explicit post-open
///    `set_permissions(0600)` guarantees the mode regardless.
///
/// The file is created if absent and truncated if present (secrets are
/// overwritten wholesale, never appended).
#[cfg(unix)]
fn write_secret_file(path: &str, bytes: &[u8]) -> Result<()> {
    use std::fs::{OpenOptions, Permissions};
    use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
    let mut file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)
        .map_err(|e| anyhow!("failed to open {} for a secret write: {}", path, e))?;
    // Enforce 0600 even if the file already existed with laxer permissions.
    file.set_permissions(Permissions::from_mode(0o600))?;
    file.write_all(bytes)?;
    Ok(())
}

#[cfg(not(unix))]
fn write_secret_file(path: &str, bytes: &[u8]) -> Result<()> {
    std::fs::write(path, bytes).map_err(|e| anyhow!("failed to write {}: {}", path, e))
}

/// Write a freshly-minted trustee signing key (hex) to `path`, mode 0600 — this
/// IS a secret, unlike the draft. Matches the format `approve --signing-key`
/// reads.
fn write_signing_key_file(path: &str, signing_key: &SigningKey) -> Result<()> {
    write_secret_file(
        path,
        format!("{}\n", hex_vec(&signing_key.to_bytes())).as_bytes(),
    )
}

fn print_setup_next_steps(draft: &SetupDraft) {
    let n = draft.trustees.len();
    let target = draft.target_trustees as usize;
    println!();
    if n < target {
        println!(
            "Next: enroll {} more trustee(s) — the policy goes live when all {} are enrolled:",
            target - n,
            target
        );
    } else {
        println!("Setup complete. Rehearse and verify:");
        println!(
            "  break_glass drill --threshold {} --trustees {}",
            draft.threshold, draft.target_trustees
        );
        println!("  break_glass doctor");
        return;
    }
    println!("  break_glass trustee enroll --id <name> --public-key <HEX>");
    println!("  break_glass trustee enroll --id <name> --generate --output <name>.key");
}

fn cmd_init(
    threshold: u8,
    trustees: u8,
    db_path: &str,
    ruleset_id: &str,
    device_key_seed: &str,
    draft_override: Option<&str>,
) -> Result<()> {
    if threshold == 0 || trustees == 0 || threshold > trustees {
        return Err(anyhow!(
            "init needs 1 <= threshold <= trustees (got {}-of-{})",
            threshold,
            trustees
        ));
    }

    println!("=== Break-glass setup — init ===");

    // Open the kernel once: this creates the (encrypted) database and pins the
    // device identity, so `authorize`/`witnessd` recognize this seed later.
    let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
    let kernel = Kernel::open(&cfg)?;
    let device_vk = kernel.device_verifying_key();
    drop(kernel);
    println!(
        "  {} device identity pinned: {}",
        DR_OK,
        short_fp(&device_vk.to_bytes())
    );

    let dpath = draft_path(db_path, draft_override);
    if let Some(existing) = read_draft(&dpath)? {
        // Idempotent: report state rather than clobbering an in-progress setup.
        println!(
            "  {} setup already in progress: {} of {} trustees enrolled toward a {}-of-{} quorum",
            DR_OK,
            existing.trustees.len(),
            existing.target_trustees,
            existing.threshold,
            existing.target_trustees
        );
        print_setup_next_steps(&existing);
        return Ok(());
    }

    let draft = SetupDraft {
        threshold,
        target_trustees: trustees,
        trustees: Vec::new(),
    };
    write_draft(&dpath, &draft)?;
    println!(
        "  {} started a {}-of-{} setup draft ({})",
        DR_OK, threshold, trustees, dpath
    );
    println!(
        "    the quorum policy commits automatically once all {} trustees are enrolled",
        trustees
    );
    print_setup_next_steps(&draft);
    Ok(())
}

// A CLI handler whose parameters map one-to-one to `trustee enroll` flags; the
// arguments are all distinct and independently meaningful, so a bag struct would
// obscure more than it helps.
#[allow(clippy::too_many_arguments)]
fn cmd_trustee_enroll(
    id: &str,
    generate: bool,
    output: Option<&str>,
    public_key: Option<&str>,
    db_path: &str,
    ruleset_id: &str,
    device_key_seed: &str,
    draft_override: Option<&str>,
) -> Result<()> {
    let dpath = draft_path(db_path, draft_override);
    let mut draft = read_draft(&dpath)?
        .ok_or_else(|| anyhow!("no setup in progress — run `break_glass init` first"))?;

    let id = id.trim();
    if id.is_empty() {
        return Err(anyhow!("trustee id cannot be empty"));
    }
    if draft.trustees.iter().any(|t| t.id == id) {
        return Err(anyhow!("trustee id '{}' is already enrolled", id));
    }
    if draft.trustees.len() >= draft.target_trustees as usize {
        return Err(anyhow!(
            "the draft already has its target of {} trustees — re-init to change the target",
            draft.target_trustees
        ));
    }

    // Resolve the trustee's public key: mint a fresh keypair, or import one.
    let public_key_bytes: [u8; 32] = match (generate, public_key) {
        (true, _) => {
            let out = output.ok_or_else(|| {
                anyhow!("--generate requires --output <file> to save the trustee's signing key")
            })?;
            let mut seed = [0u8; 32];
            rand::rngs::SysRng
                .try_fill_bytes(&mut seed[..])
                .map_err(|_| anyhow!("OS RNG unavailable"))?;
            let signing_key = SigningKey::from_bytes(&seed);
            seed.zeroize();
            write_signing_key_file(out, &signing_key)?;
            println!(
                "  {} minted a signing key for '{}' → {} (secret, mode 0600 — hand it to the trustee)",
                DR_OK, id, out
            );
            signing_key.verifying_key().to_bytes()
        }
        (false, Some(hex_str)) => {
            let bytes = hex::decode(hex_str.trim())
                .map_err(|e| anyhow!("invalid public key hex: {}", e))?;
            if bytes.len() != 32 {
                return Err(anyhow!("public key must be 32 bytes, got {}", bytes.len()));
            }
            let mut pk = [0u8; 32];
            pk.copy_from_slice(&bytes);
            VerifyingKey::from_bytes(&pk).map_err(|_| anyhow!("not a valid Ed25519 public key"))?;
            pk
        }
        (false, None) => {
            return Err(anyhow!(
                "provide either --public-key <HEX> or --generate --output <file>"
            ));
        }
    };

    // Reject a reused key: quorum independence is a property of the KEYS, not
    // just the ids (Invariant V) — mirrors QuorumPolicy::validate, but fails now
    // with a clear message instead of at commit.
    let public_key_hex = hex_vec(&public_key_bytes);
    if draft
        .trustees
        .iter()
        .any(|t| t.public_key_hex == public_key_hex)
    {
        return Err(anyhow!(
            "that public key is already enrolled under another trustee — quorum independence requires distinct keys"
        ));
    }

    draft.trustees.push(DraftTrustee {
        id: id.to_string(),
        public_key_hex,
    });
    write_draft(&dpath, &draft)?;
    println!(
        "  {} enrolled trustee '{}' ({}/{})",
        DR_OK,
        id,
        draft.trustees.len(),
        draft.target_trustees
    );

    // Commit the real policy once the roster is COMPLETE. Committing earlier
    // (at the threshold) made every later enrollment a silent mutation of a
    // LIVE policy — exactly the roster-rewrite path the quorum gate closes
    // (Invariant V; spec/quorum_unseal_v2.md §3.1). A complete-roster commit
    // is a bootstrap on a fresh database and needs no approvals.
    if draft.trustees.len() == draft.target_trustees as usize {
        let entries = draft
            .trustees
            .iter()
            .map(|t| {
                let bytes = hex::decode(&t.public_key_hex)
                    .map_err(|e| anyhow!("corrupt draft key for {}: {}", t.id, e))?;
                let mut pk = [0u8; 32];
                if bytes.len() != 32 {
                    return Err(anyhow!("corrupt draft key for {}", t.id));
                }
                pk.copy_from_slice(&bytes);
                Ok(crate::break_glass::TrusteeEntry {
                    id: TrusteeId::new(&t.id),
                    public_key: pk,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let policy = crate::break_glass::QuorumPolicy::new(draft.threshold, entries)?;
        let cfg = kernel_config(db_path, ruleset_id, device_key_seed);
        let mut kernel = Kernel::open(&cfg)?;
        match kernel.set_break_glass_policy_gated(&policy, &[], TimeBucket::now_10min()?) {
            Ok(_) => println!(
                "  {} quorum policy is live: {}-of-{}",
                DR_OK, policy.n, policy.m
            ),
            Err(e) => {
                return Err(anyhow!(
                    "a policy is already live, so this enrollment is a policy CHANGE and needs \
                     current-quorum consent: {} — use `break_glass policy propose` / \
                     `policy approve` / `policy set --approvals`",
                    e
                ));
            }
        }
    }

    print_setup_next_steps(&draft);
    Ok(())
}

fn cmd_receipts(
    db_path: &str,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
    verbose: bool,
) -> Result<()> {
    let conn = rusqlite::Connection::open(db_path)?;
    let verifying_key = load_verifying_key(&conn, public_key_hex, public_key_file)?;
    let policy = load_break_glass_policy(&conn)?;
    // Ground truth for a receipt's declared `policy_commitment`: the
    // authenticated policy-change history (chain + device signature + approvals
    // binding + per-transition quorum consent). See verify::verify_receipt_quorum.
    let policy_history =
        crate::verify::load_authenticated_policy_eras(&conn, &verifying_key, None)?;
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, approvals_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM break_glass_receipts ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;

    println!("=== Break-Glass Receipts ===");
    let mut n = 0usize;
    let mut expected_prev = [0u8; 32];
    let mut invalid_count = 0usize;
    while let Some(row) = rows.next()? {
        let policy = policy
            .as_ref()
            .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?;
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let approvals_json: String = row.get(3)?;
        let prev_hash_bytes: Vec<u8> = row.get(4)?;
        let entry_hash_bytes: Vec<u8> = row.get(5)?;
        let signature_bytes: Vec<u8> = row.get(6)?;
        let pq_signature: Option<Vec<u8>> = row.get(7)?;
        let pq_scheme: Option<String> = row.get(8)?;
        let prev_hash = blob32_vec(prev_hash_bytes, "break_glass_receipts.prev_hash")?;
        let entry_hash = blob32_vec(entry_hash_bytes, "break_glass_receipts.entry_hash")?;
        let signature_set = SignatureSet::from_storage(&signature_bytes, pq_signature, pq_scheme)?;
        let signature = signature_set.ed25519_signature_array()?;
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
        if verify_entry_signature(
            &verifying_key,
            &entry_hash,
            &signature_set,
            SignatureMode::Compat,
            None,
            DOMAIN_BREAK_GLASS_RECEIPT,
        )
        .is_err()
        {
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
                // Quorum re-derivation (Invariant V): shared with log_verify /
                // API verify so the CLI cannot report a forged or under-quorum
                // Granted receipt as VALID. Resolves the receipt's policy era
                // against the signed policy-change history.
                if let Err(err) = crate::verify::verify_receipt_quorum(
                    policy,
                    &policy_history,
                    &receipt,
                    &approvals,
                ) {
                    status = "INVALID";
                    issues.push(format!("approvals invalid: {}", err));
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
    let token_file = BreakGlassTokenFile::from_token(token)?;
    let payload = format!("{}\n", serde_json::to_string_pretty(&token_file)?);
    write_secret_file(path, payload.as_bytes())
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
    let conn = rusqlite::Connection::open(db_path)?;
    let crypto_mode = load_break_glass_policy(&conn)?
        .map(|policy| policy.vault.crypto_mode)
        .unwrap_or_default();
    let vault = Vault::new(VaultConfig {
        local_path: vault_path.into(),
        crypto_mode,
        key_material: crate::VaultKeyMaterial::from_env(),
    })?;
    let verifying_key = device_public_key_from_db(&conn)?;
    #[cfg(feature = "pqc-signatures")]
    let pq_pk = crate::device_pq_public_key_from_db(&conn).ok();
    #[cfg(not(feature = "pqc-signatures"))]
    let pq_pk: Option<crate::crypto::signatures::PqPublicKey> = None;
    // Burn-first (durable anti-replay): the token file just re-parsed as
    // unconsumed, so this is THE gate that stops a granted token from
    // authorizing repeated unseals across separate CLI runs within its
    // validity bucket. Record the nonce before any cleartext exists.
    crate::consume_break_glass_token_durably(&conn, &token)?;
    let clear = vault.unseal(envelope, &mut token, ruleset_hash, &verifying_key, |hash| {
        break_glass_receipt_outcome_for_verifier(
            &conn,
            &verifying_key,
            envelope,
            ruleset_hash,
            hash,
            pq_pk.as_ref(),
        )
    })?;

    let sanitized = crate::vault::sanitize_envelope_id(envelope)?;
    let output_dir_path = std::path::Path::new(output_dir);
    std::fs::create_dir_all(output_dir_path)?;
    let output_path = output_dir_path.join(format!("{}.raw", sanitized));

    // Unsealed raw media is the most sensitive artifact break-glass produces —
    // write it symlink-safe and 0600-guaranteed, then zeroize the plaintext.
    let mut clear = clear;
    let write_result = write_secret_file(&output_path.to_string_lossy(), &clear);
    clear.zeroize();
    write_result?;

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

fn load_break_glass_policy(
    conn: &rusqlite::Connection,
) -> Result<Option<crate::break_glass::QuorumPolicy>> {
    let mut stmt = conn.prepare("SELECT policy_json FROM break_glass_policy WHERE id = 1")?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let policy_json: String = row.get(0)?;
        let policy: crate::break_glass::QuorumPolicy = serde_json::from_str(&policy_json)?;
        policy.validate()?;
        Ok(Some(policy))
    } else {
        Ok(None)
    }
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

fn parse_trustee_entry(entry: &str) -> Result<crate::break_glass::TrusteeEntry> {
    let (id, hex_key) = entry
        .split_once(':')
        .ok_or_else(|| anyhow!("invalid trustee entry {} (expected id:hex)", entry))?;
    let bytes = hex::decode(hex_key.trim()).map_err(|e| anyhow!("invalid trustee key: {}", e))?;
    if bytes.len() != 32 {
        return Err(anyhow!(
            "invalid trustee key length: expected 32 bytes, got {}",
            bytes.len()
        ));
    }
    let mut public_key = [0u8; 32];
    public_key.copy_from_slice(&bytes);
    Ok(crate::break_glass::TrusteeEntry {
        id: TrusteeId::new(id),
        public_key,
    })
}

fn kernel_config(db_path: &str, ruleset_id: &str, device_key_seed: &str) -> KernelConfig {
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: ruleset_id.to_string(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: std::time::Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: device_key_seed.to_string(),
        zone_policy: ZonePolicy::default(),
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::SigningKey;
    use hex::encode as hex_encode;
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
            policy_commitment: [0u8; 32],
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
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
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
        let approval = Approval::signed(TrusteeId::new("bob"), request.request_hash(), &bob_key);
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

    #[test]
    fn policy_persists_and_authorize_uses_stored_policy() -> Result<()> {
        let temp_dir = std::env::temp_dir().join(format!(
            "secura_break_glass_policy_{}",
            rand::random::<u64>()
        ));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");

        let ruleset_id = "ruleset:test";
        let device_key_seed = "devkey:test:a1b2c3d4e5f6a7b8c9d0";
        let trustee_key = SigningKey::from_bytes(&[7u8; 32]);
        let trustee_entry = format!(
            "alice:{}",
            hex_encode(trustee_key.verifying_key().to_bytes())
        );

        cmd_policy_set(
            1,
            std::slice::from_ref(&trustee_entry),
            None,
            None,
            &db_path.to_string_lossy(),
            ruleset_id,
            device_key_seed,
        )?;

        let cfg = kernel_config(&db_path.to_string_lossy(), ruleset_id, device_key_seed);
        let kernel = Kernel::open(&cfg)?;
        let policy = kernel
            .break_glass_policy()
            .expect("policy should be loaded from db");
        assert_eq!(policy.n, 1);
        assert_eq!(policy.m, 1);
        assert_eq!(policy.trustees[0].id.0, "alice");

        let bucket = TimeBucket {
            start_epoch_s: 1234,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:1", cfg.ruleset_hash, "audit", bucket)?;
        let approval = Approval::signed(
            TrusteeId::new("alice"),
            request.request_hash(),
            &trustee_key,
        );
        let approval_path = temp_dir.join("alice.approval");
        std::fs::write(&approval_path, serde_json::to_string(&approval)?)?;

        let approval_arg = approval_path.to_string_lossy();
        let args = AuthorizeArgs {
            envelope: "vault:1",
            purpose: "audit",
            approvals_arg: approval_arg.as_ref(),
            db_path: cfg.db_path.as_str(),
            ruleset_id,
            device_key_seed,
            output_token: None,
            bucket,
        };
        cmd_authorize_with_bucket(args)?;
        Ok(())
    }

    fn trustee_entry(id: &str, seed: u8) -> String {
        let key = SigningKey::from_bytes(&[seed; 32]);
        format!("{}:{}", id, hex_encode(key.verifying_key().to_bytes()))
    }

    #[test]
    fn doctor_healthy_for_configured_policy() -> Result<()> {
        let temp_dir =
            std::env::temp_dir().join(format!("secura_doctor_ok_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");
        let vault_path = temp_dir.join("vault");
        std::fs::create_dir_all(&vault_path)?;

        let entries = vec![
            trustee_entry("alice", 7),
            trustee_entry("bob", 8),
            trustee_entry("carol", 9),
        ];
        cmd_policy_set(
            2,
            &entries,
            None,
            None,
            &db_path.to_string_lossy(),
            "ruleset:test",
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        )?;

        // Policy present + device key pinned by `policy set`. master.key is absent
        // (created on first seal), which doctor treats as a warning, not a problem,
        // so the overall result is healthy (Ok).
        cmd_doctor(
            &db_path.to_string_lossy(),
            &vault_path.to_string_lossy(),
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        )?;
        Ok(())
    }

    #[test]
    fn doctor_fails_without_policy() {
        let temp_dir =
            std::env::temp_dir().join(format!("secura_doctor_nopolicy_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir).unwrap();
        // DB does not exist -> "no kernel database" is a hard problem -> non-zero exit.
        let db_path = temp_dir.join("witness.db");
        let vault_path = temp_dir.join("vault");
        let result = cmd_doctor(
            &db_path.to_string_lossy(),
            &vault_path.to_string_lossy(),
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        );
        assert!(
            result.is_err(),
            "doctor must fail (non-zero) when no policy is configured"
        );
    }

    #[test]
    fn doctor_flags_wrong_size_master_key() -> Result<()> {
        let temp_dir =
            std::env::temp_dir().join(format!("secura_doctor_badkey_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");
        let vault_path = temp_dir.join("vault");
        std::fs::create_dir_all(&vault_path)?;

        let entry = trustee_entry("solo", 3);
        cmd_policy_set(
            1,
            std::slice::from_ref(&entry),
            None,
            None,
            &db_path.to_string_lossy(),
            "ruleset:test",
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        )?;

        // A truncated master.key is a hard problem even though the policy is valid.
        std::fs::write(vault_path.join("master.key"), [0u8; 10])?;
        let result = cmd_doctor(
            &db_path.to_string_lossy(),
            &vault_path.to_string_lossy(),
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        );
        assert!(
            result.is_err(),
            "doctor must fail on a wrong-size master.key"
        );
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn doctor_flags_group_readable_master_key() -> Result<()> {
        use std::os::unix::fs::PermissionsExt;
        let temp_dir =
            std::env::temp_dir().join(format!("secura_doctor_perms_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");
        let vault_path = temp_dir.join("vault");
        std::fs::create_dir_all(&vault_path)?;

        let entry = trustee_entry("solo", 5);
        cmd_policy_set(
            1,
            std::slice::from_ref(&entry),
            None,
            None,
            &db_path.to_string_lossy(),
            "ruleset:test",
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        )?;

        // Correctly-sized master key, but group/world-readable (0644) — a hard
        // problem, because it lets a local user decrypt the vault without quorum.
        let master = vault_path.join("master.key");
        std::fs::write(&master, [7u8; 32])?;
        std::fs::set_permissions(&master, std::fs::Permissions::from_mode(0o644))?;
        let result = cmd_doctor(
            &db_path.to_string_lossy(),
            &vault_path.to_string_lossy(),
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        );
        assert!(
            result.is_err(),
            "a group/world-readable master.key must be a hard failure"
        );
        Ok(())
    }

    #[test]
    fn doctor_fails_with_wrong_device_seed() -> Result<()> {
        let temp_dir =
            std::env::temp_dir().join(format!("secura_doctor_wrongseed_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp_dir)?;
        let db_path = temp_dir.join("witness.db");
        let vault_path = temp_dir.join("vault");
        std::fs::create_dir_all(&vault_path)?;

        let entry = trustee_entry("solo", 6);
        cmd_policy_set(
            1,
            std::slice::from_ref(&entry),
            None,
            None,
            &db_path.to_string_lossy(),
            "ruleset:test",
            "devkey:test:a1b2c3d4e5f6a7b8c9d0",
        )?;

        // A different device seed can't open the (seed-keyed) DB, so doctor fails
        // rather than falsely reporting healthy with the wrong identity.
        let result = cmd_doctor(
            &db_path.to_string_lossy(),
            &vault_path.to_string_lossy(),
            "devkey:test:ffffffffffffffffffff",
        );
        assert!(
            result.is_err(),
            "doctor must fail when the supplied device seed doesn't match the vault"
        );
        Ok(())
    }

    #[test]
    fn drill_completes_end_to_end() -> Result<()> {
        // A 2-of-3 rehearsal runs the whole request→approve→authorize→seal→unseal
        // path in a throwaway sandbox and must succeed on a healthy build.
        cmd_drill(2, 3)?;
        Ok(())
    }

    #[test]
    fn drill_with_single_trustee_quorum() -> Result<()> {
        // 1-of-1 is the smallest valid quorum and must also round-trip.
        cmd_drill(1, 1)?;
        Ok(())
    }

    #[test]
    fn drill_rejects_impossible_quorum() {
        assert!(cmd_drill(0, 3).is_err(), "threshold 0 is invalid");
        assert!(
            cmd_drill(4, 3).is_err(),
            "threshold greater than trustee count is invalid"
        );
    }

    fn pub_hex(seed: u8) -> String {
        hex_encode(
            SigningKey::from_bytes(&[seed; 32])
                .verifying_key()
                .to_bytes(),
        )
    }

    const TEST_SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";

    fn policy_of(db: &str) -> Option<(u8, u8)> {
        let cfg = kernel_config(db, "ruleset:test", TEST_SEED);
        let kernel = Kernel::open(&cfg).unwrap();
        kernel.break_glass_policy().map(|p| (p.n, p.m))
    }

    #[test]
    fn init_and_enroll_commit_policy_when_roster_is_complete() -> Result<()> {
        let temp = std::env::temp_dir().join(format!("secura_init_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let db = temp.join("witness.db").to_string_lossy().to_string();

        cmd_init(2, 3, &db, "ruleset:test", TEST_SEED, None)?;
        // init is idempotent — a second call reports state, doesn't error/clobber.
        cmd_init(2, 3, &db, "ruleset:test", TEST_SEED, None)?;
        // draft only: no committed policy yet (validate would reject a partial roster).
        assert_eq!(policy_of(&db), None);

        cmd_trustee_enroll(
            "alice",
            false,
            None,
            Some(&pub_hex(1)),
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        )?;
        assert_eq!(policy_of(&db), None, "one trustee is below the target of 3");

        cmd_trustee_enroll(
            "bob",
            false,
            None,
            Some(&pub_hex(2)),
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        )?;
        // Committing at the threshold would make every later enrollment a
        // silent mutation of a LIVE policy — the roster-rewrite path the
        // quorum gate exists to close. So no policy yet:
        assert_eq!(
            policy_of(&db),
            None,
            "policy must not go live before the roster is complete"
        );

        cmd_trustee_enroll(
            "carol",
            false,
            None,
            Some(&pub_hex(3)),
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        )?;
        assert_eq!(
            policy_of(&db),
            Some((2, 3)),
            "policy goes live once all target trustees are enrolled"
        );
        Ok(())
    }

    #[test]
    fn enroll_rejects_duplicates_and_bad_input() -> Result<()> {
        let temp =
            std::env::temp_dir().join(format!("secura_enroll_dup_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let db = temp.join("witness.db").to_string_lossy().to_string();
        cmd_init(2, 3, &db, "ruleset:test", TEST_SEED, None)?;
        cmd_trustee_enroll(
            "alice",
            false,
            None,
            Some(&pub_hex(1)),
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        )?;

        let enroll = |id: &str, generate: bool, output: Option<&str>, pk: Option<&str>| {
            cmd_trustee_enroll(
                id,
                generate,
                output,
                pk,
                &db,
                "ruleset:test",
                TEST_SEED,
                None,
            )
        };
        assert!(
            enroll("alice", false, None, Some(&pub_hex(9))).is_err(),
            "duplicate id"
        );
        assert!(
            enroll("alice2", false, None, Some(&pub_hex(1))).is_err(),
            "duplicate key"
        );
        assert!(
            enroll("bob", false, None, None).is_err(),
            "no key and no --generate"
        );
        assert!(
            enroll("bob", true, None, None).is_err(),
            "--generate without --output"
        );
        assert!(
            enroll("bob", false, None, Some("nothex")).is_err(),
            "bad hex"
        );
        Ok(())
    }

    #[test]
    fn enroll_before_init_fails() {
        let temp = std::env::temp_dir().join(format!("secura_noinit_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp).unwrap();
        let db = temp.join("witness.db").to_string_lossy().to_string();
        let r = cmd_trustee_enroll(
            "alice",
            false,
            None,
            Some(&pub_hex(1)),
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        );
        assert!(r.is_err(), "enroll must fail before init");
    }

    #[test]
    fn init_rejects_impossible_quorum() {
        let temp = std::env::temp_dir().join(format!("secura_initbad_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp).unwrap();
        let db = temp.join("witness.db").to_string_lossy().to_string();
        assert!(cmd_init(0, 3, &db, "ruleset:test", TEST_SEED, None).is_err());
        assert!(cmd_init(4, 3, &db, "ruleset:test", TEST_SEED, None).is_err());
    }

    #[cfg(unix)]
    #[test]
    fn enroll_generate_writes_0600_signing_key() -> Result<()> {
        use std::os::unix::fs::PermissionsExt;
        let temp = std::env::temp_dir().join(format!("secura_gen_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let db = temp.join("witness.db").to_string_lossy().to_string();
        let keyfile = temp.join("dave.key").to_string_lossy().to_string();
        cmd_init(1, 1, &db, "ruleset:test", TEST_SEED, None)?;
        cmd_trustee_enroll(
            "dave",
            true,
            Some(&keyfile),
            None,
            &db,
            "ruleset:test",
            TEST_SEED,
            None,
        )?;

        let mode = std::fs::metadata(&keyfile)?.permissions().mode() & 0o777;
        assert_eq!(
            mode, 0o600,
            "a minted signing key must be written mode 0600"
        );
        let hex = std::fs::read_to_string(&keyfile)?;
        assert_eq!(
            hex::decode(hex.trim()).unwrap().len(),
            32,
            "key file holds 32-byte hex"
        );
        assert_eq!(
            policy_of(&db),
            Some((1, 1)),
            "1-of-1 commits on the single enrollment"
        );
        Ok(())
    }

    // ─── WYSIWYS: approve from a request-context file ─────────────────────

    #[test]
    fn utc_string_formats_known_instants() {
        assert_eq!(utc_string(0), "1970-01-01 00:00:00 UTC");
        assert_eq!(utc_string(86_400), "1970-01-02 00:00:00 UTC");
        assert_eq!(utc_string(1_000_000_000), "2001-09-09 01:46:40 UTC");
    }

    #[test]
    fn approve_from_request_file_recomputes_hash_and_signs() -> Result<()> {
        let temp = std::env::temp_dir().join(format!("secura_wysiwys_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let req_path = temp.join("unlock.request").to_string_lossy().to_string();
        let key_path = temp.join("alice.key").to_string_lossy().to_string();
        let out_path = temp.join("alice.approval").to_string_lossy().to_string();

        cmd_request(
            "vault:wysiwys",
            "incident response",
            "ruleset:test",
            Some(&req_path),
        )?;

        let alice = SigningKey::from_bytes(&[41u8; 32]);
        std::fs::write(&key_path, format!("{}\n", hex_vec(&alice.to_bytes())))?;

        // The trustee tool recomputes the hash from the file's FIELDS and
        // signs that — producing a valid, domain-separated approval.
        cmd_approve(Some(&req_path), None, "alice", &key_path, &out_path)?;
        let approval: Approval = serde_json::from_str(&std::fs::read_to_string(&out_path)?)?;
        let file: UnlockRequestFile = serde_json::from_str(&std::fs::read_to_string(&req_path)?)?;
        let expected = parse_hex32(&file.request_hash)?;
        assert_eq!(approval.request_hash, expected);
        assert!(crate::verify_approval(
            &alice.verifying_key().to_bytes(),
            &expected,
            &approval.signature
        ));
        Ok(())
    }

    #[test]
    fn approve_refuses_request_file_whose_fields_do_not_match_its_hash() -> Result<()> {
        let temp = std::env::temp_dir().join(format!("secura_wysiwys_t_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let req_path = temp.join("unlock.request").to_string_lossy().to_string();
        let key_path = temp.join("alice.key").to_string_lossy().to_string();
        let out_path = temp.join("alice.approval").to_string_lossy().to_string();

        cmd_request(
            "vault:tamper",
            "routine export",
            "ruleset:test",
            Some(&req_path),
        )?;
        let alice = SigningKey::from_bytes(&[42u8; 32]);
        std::fs::write(&key_path, format!("{}\n", hex_vec(&alice.to_bytes())))?;

        // A compromised relay swaps the displayed purpose but keeps the hash
        // (the Bybit shape): the trustee tool recomputes, detects the
        // mismatch, and refuses to sign.
        let mut file: UnlockRequestFile =
            serde_json::from_str(&std::fs::read_to_string(&req_path)?)?;
        file.purpose = "totally routine, nothing to see".to_string();
        std::fs::write(&req_path, serde_json::to_string_pretty(&file)?)?;

        let result = cmd_approve(Some(&req_path), None, "alice", &key_path, &out_path);
        assert!(
            result.is_err(),
            "a request file whose fields do not hash to its claimed request hash must be refused"
        );
        assert!(
            !std::path::Path::new(&out_path).exists(),
            "no approval may be written for a tampered request"
        );
        Ok(())
    }

    // ─── quorum-gated policy mutation, end to end through the CLI ─────────

    #[test]
    fn policy_set_mutation_requires_and_accepts_current_quorum_approvals() -> Result<()> {
        let temp = std::env::temp_dir().join(format!("secura_polgate_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let db = temp.join("witness.db").to_string_lossy().to_string();
        let seed = "devkey:test:a1b2c3d4e5f6a7b8c9d0";

        // Bootstrap: 1-of-1 alice, no approvals needed on an empty database.
        let alice_key = SigningKey::from_bytes(&[7u8; 32]);
        let alice_entry = trustee_entry("alice", 7);
        cmd_policy_set(
            1,
            std::slice::from_ref(&alice_entry),
            None,
            None,
            &db,
            "ruleset:test",
            seed,
        )?;
        assert_eq!(policy_of(&db), Some((1, 1)));

        // Replacing the roster WITHOUT current-quorum approvals must fail…
        let mallory_entry = trustee_entry("mallory", 66);
        assert!(cmd_policy_set(
            1,
            std::slice::from_ref(&mallory_entry),
            None,
            None,
            &db,
            "ruleset:test",
            seed,
        )
        .is_err());
        assert_eq!(policy_of(&db), Some((1, 1)), "policy must be unchanged");

        // …and must succeed with the propose → approve → set flow. The
        // propose/set pair must land in one 10-minute freshness window, so
        // retry once if the boundary was straddled (the same rule a real
        // ceremony follows).
        let bob_entry = trustee_entry("bob", 8);
        let alice_key_path = temp.join("alice.key").to_string_lossy().to_string();
        std::fs::write(
            &alice_key_path,
            format!("{}\n", hex_vec(&alice_key.to_bytes())),
        )?;
        let mut applied = false;
        for attempt in 0..2 {
            let proposal_path = temp
                .join(format!("change-{}.proposal", attempt))
                .to_string_lossy()
                .to_string();
            let approval_path = temp
                .join(format!("alice-{}.policy-approval", attempt))
                .to_string_lossy()
                .to_string();
            cmd_policy_propose(
                2,
                &[alice_entry.clone(), bob_entry.clone()],
                None,
                &proposal_path,
                &db,
                "ruleset:test",
                seed,
            )?;
            cmd_policy_change_approve(&proposal_path, "alice", &alice_key_path, &approval_path)?;
            match cmd_policy_set(
                2,
                &[alice_entry.clone(), bob_entry.clone()],
                None,
                Some(&approval_path),
                &db,
                "ruleset:test",
                seed,
            ) {
                Ok(()) => {
                    applied = true;
                    break;
                }
                Err(_) if attempt == 0 => continue,
                Err(e) => return Err(e),
            }
        }
        assert!(applied, "gated policy change with valid consent must apply");
        assert_eq!(policy_of(&db), Some((2, 2)));

        // The history ledger records bootstrap + change and verifies.
        cmd_policy_history(&db, "ruleset:test", seed)?;
        Ok(())
    }

    #[test]
    fn policy_change_approve_refuses_tampered_proposal() -> Result<()> {
        let temp = std::env::temp_dir().join(format!("secura_polprop_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&temp)?;
        let db = temp.join("witness.db").to_string_lossy().to_string();
        let seed = "devkey:test:a1b2c3d4e5f6a7b8c9d0";

        let alice_entry = trustee_entry("alice", 7);
        cmd_policy_set(
            1,
            std::slice::from_ref(&alice_entry),
            None,
            None,
            &db,
            "ruleset:test",
            seed,
        )?;

        let proposal_path = temp.join("change.proposal").to_string_lossy().to_string();
        let bob_entry = trustee_entry("bob", 8);
        cmd_policy_propose(
            1,
            std::slice::from_ref(&bob_entry),
            None,
            &proposal_path,
            &db,
            "ruleset:test",
            seed,
        )?;

        // Swap the displayed roster while keeping the claimed change hash:
        // the approving tool recomputes from the full contents and refuses.
        let mut file: PolicyChangeProposalFile =
            serde_json::from_str(&std::fs::read_to_string(&proposal_path)?)?;
        let mallory = SigningKey::from_bytes(&[66u8; 32]);
        file.new_policy.trustees[0].public_key = mallory.verifying_key().to_bytes();
        std::fs::write(&proposal_path, serde_json::to_string_pretty(&file)?)?;

        let alice_key = SigningKey::from_bytes(&[7u8; 32]);
        let key_path = temp.join("alice.key").to_string_lossy().to_string();
        std::fs::write(&key_path, format!("{}\n", hex_vec(&alice_key.to_bytes())))?;
        let out_path = temp
            .join("alice.policy-approval")
            .to_string_lossy()
            .to_string();
        let result = cmd_policy_change_approve(&proposal_path, "alice", &key_path, &out_path);
        assert!(
            result.is_err(),
            "a proposal whose contents do not hash to its claimed change hash must be refused"
        );
        Ok(())
    }
}
