//! envelope_verify - Verify a canonical evidence envelope file.
//!
//! Verifies a `securacv-evidence-envelope` JSON bundle entirely from its own contents
//! (no database, no network): the four hash-chained ledgers, their signatures, the
//! break-glass approvals commitments, the checkpoint, the artifact binding, the ledger
//! summaries, the manifest, and the `whole_envelope_digest`.
//!
//! This is the Rust side of the cross-language verifier pair; `viewer/verify_core.js`
//! implements the identical algorithm and both are pinned to the same fixtures.

use anyhow::{anyhow, Context, Result};
use clap::{Parser, ValueEnum};

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{verify_envelope_bytes, verify_explain, IntegrityStatus};

#[derive(Parser, Debug)]
#[command(
    name = "envelope_verify",
    about = "Verify a canonical evidence envelope (chain + signatures + digest)"
)]
struct Args {
    /// Path to the evidence envelope JSON file.
    #[arg(long, value_name = "PATH")]
    bundle: String,

    /// Signature verification mode (compat = Ed25519 OR PQ, strict = Ed25519 AND PQ).
    #[arg(long, value_enum, default_value = "compat")]
    sig_mode: SignatureModeArg,

    /// Emit the verification report as JSON on stdout.
    #[arg(long)]
    json: bool,

    /// The device's Ed25519 public key to pin (64 hex chars). With it, "verified"
    /// means every signature checked against THIS key. Without it the envelope is
    /// only self-consistent under the key it carries — an attacker's envelope
    /// under an attacker's key passes that bar — and the verdict says so.
    #[arg(long, value_name = "HEX")]
    public_key: Option<String>,
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
    let mode: SignatureMode = args.sig_mode.into();

    let pinned_key: Option<[u8; 32]> = match &args.public_key {
        Some(hex_key) => {
            let raw = hex::decode(hex_key.trim()).context("--public-key must be hex")?;
            let key: [u8; 32] = raw
                .as_slice()
                .try_into()
                .map_err(|_| anyhow!("--public-key must be 32 bytes (64 hex chars)"))?;
            Some(key)
        }
        None => None,
    };

    let bytes = std::fs::read(&args.bundle)
        .with_context(|| format!("failed to read envelope file {}", args.bundle))?;

    // Verified from the presented bytes, not a re-serialized struct: a field this
    // schema does not know must break the digest, exactly as it does in verify_core.js.
    match verify_envelope_bytes(&bytes, mode) {
        Ok((envelope, report)) => {
            if let Some(key) = pinned_key {
                if envelope.provenance.device_public_key != key {
                    let msg = format!(
                        "envelope is signed under device key {} but the pinned key is {}",
                        hex::encode(envelope.provenance.device_public_key),
                        hex::encode(key)
                    );
                    if args.json {
                        let out = serde_json::json!({ "status": "compromised", "error": msg });
                        println!("{}", serde_json::to_string_pretty(&out)?);
                    } else {
                        eprintln!("COMPROMISED: {msg}");
                    }
                    std::process::exit(1);
                }
            }
            let status = match report.status {
                IntegrityStatus::Ok => "ok",
                IntegrityStatus::ValidWithWarnings => "valid_with_warnings",
            };
            let device_key = if pinned_key.is_some() {
                "pinned"
            } else {
                "self-attested"
            };
            if args.json {
                let out = serde_json::json!({
                    "status": status,
                    "device_key": device_key,
                    "whole_envelope_digest": envelope.whole_envelope_digest,
                    "sealed_events": report.sealed_events,
                    "break_glass_granted": report.break_glass_granted,
                    "break_glass_denied": report.break_glass_denied,
                    "export_receipts": report.export_receipts,
                    "pq_checked": report.pq_checked,
                    "warnings": report.warnings,
                });
                println!("{}", serde_json::to_string_pretty(&out)?);
            } else {
                if pinned_key.is_some() {
                    println!(
                        "OK: evidence envelope verified against the pinned device key ({status})."
                    );
                } else {
                    println!(
                        "OK: evidence envelope is self-consistent ({status}) — every signature checks \
                         under the device key the envelope itself carries. Pass --public-key to \
                         verify against a pinned key."
                    );
                }
                println!("  device key:     {device_key}");
                println!("  digest:         {}", envelope.whole_envelope_digest);
                println!("  sealed events:  {}", report.sealed_events);
                println!(
                    "  break-glass:    {} granted, {} denied",
                    report.break_glass_granted, report.break_glass_denied
                );
                println!("  export receipts:{}", report.export_receipts);
                println!("  pq checked:     {}", report.pq_checked);
                for w in &report.warnings {
                    println!("  warning:        {w}");
                }
            }
            Ok(())
        }
        Err(e) => {
            let failure = e.downcast_ref::<witness_kernel::verify::VerifyFailure>();
            if args.json {
                let mut out =
                    serde_json::json!({ "status": "compromised", "error": e.to_string() });
                if let Some(failure) = failure {
                    out["failure"] = serde_json::to_value(failure)?;
                }
                println!("{}", serde_json::to_string_pretty(&out)?);
            } else {
                eprintln!("COMPROMISED: {e}");
                if let Some(failure) = failure {
                    eprintln!();
                    eprintln!("{}", verify_explain::format_failure_diagnosis(failure));
                }
            }
            // Exit non-zero without re-printing via anyhow (avoids a redundant backtrace).
            std::process::exit(1);
        }
    }
}
