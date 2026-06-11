//! export_events - sequential event export bundle to a local artifact
//!
//! Two authorization modes:
//!   --self-export          owner self-export; possession of the device key seed
//!                          (which decrypts the database and signs the receipt)
//!                          is the credential. Exports only the privacy-filtered
//!                          artifact the local API already serves.
//!   --break-glass-token    trustee-quorum authorization for the same artifact.
//!
//! Either way a signed, hash-chained export receipt is appended to the sealed
//! log (Invariant IV), labeled with the authorization mode. Sealed-vault
//! evidence and unsealing remain quorum-only (`break_glass unseal`).

use anyhow::{anyhow, Result};
use clap::Parser;
use std::io::IsTerminal;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use witness_kernel::break_glass::BreakGlassTokenFile;
use witness_kernel::{ExportOptions, ExportWindow, Kernel, KernelConfig, ZonePolicy};

#[path = "../ui.rs"]
mod ui;

const BUCKET_S: u64 = 600;

#[derive(Parser, Debug)]
#[command(author, version, about)]
#[command(after_help = "\
Exported times are coarsened to 10-minute buckets and jittered by design \
(see spec/event_contract.md); raw media never leaves the kernel. Every export \
appends a signed receipt to the tamper-evident log, so disclosures are always \
auditable.")]
struct Args {
    /// Path to the witness database.
    #[arg(long, default_value = "witness.db")]
    db_path: String,
    /// Ruleset identifier to bind export interpretation.
    #[arg(long, default_value = "ruleset:v0.1")]
    ruleset_id: String,
    /// Device key seed (required).
    #[arg(long, env = "DEVICE_KEY_SEED")]
    device_key_seed: String,
    /// Output file path for the export artifact.
    #[arg(long, default_value = "witness_export.json")]
    output: String,
    /// Path to a break-glass token file authorizing export (trustee quorum).
    #[arg(long, conflicts_with = "self_export")]
    break_glass_token: Option<String>,
    /// Owner self-export: authorize with the device key seed alone.
    /// The export receipt is labeled `self_export` so it stays distinguishable
    /// from quorum-authorized disclosures.
    #[arg(long)]
    self_export: bool,
    /// Export window start (Unix epoch seconds, floored to a 600s bucket boundary).
    #[arg(long, requires = "end", conflicts_with = "last")]
    start: Option<u64>,
    /// Export window end (Unix epoch seconds, ceiled to a 600s bucket boundary).
    #[arg(long, requires = "start", conflicts_with = "last")]
    end: Option<u64>,
    /// Export only the trailing duration, e.g. `--last 24h`, `--last 7d`,
    /// `--last 90m` (aligned outward to 600s bucket boundaries).
    #[arg(long)]
    last: Option<String>,
    /// Maximum events per export batch.
    #[arg(long, default_value_t = 50)]
    max_events_per_batch: usize,
    /// Jitter in seconds applied to exported time buckets.
    #[arg(long, default_value_t = 120)]
    jitter_s: u64,
    /// Jitter step in seconds (granularity of jitter).
    #[arg(long, default_value_t = 60)]
    jitter_step_s: u64,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

/// Parse `24h` / `7d` / `90m` / `3600s` / `3600` into seconds.
fn parse_duration_s(s: &str) -> Result<u64> {
    let s = s.trim();
    let (num, mult) = match s.chars().last() {
        Some('s') => (&s[..s.len() - 1], 1),
        Some('m') => (&s[..s.len() - 1], 60),
        Some('h') => (&s[..s.len() - 1], 3600),
        Some('d') => (&s[..s.len() - 1], 86_400),
        Some(c) if c.is_ascii_digit() => (s, 1),
        _ => return Err(anyhow!("invalid duration '{}': use e.g. 24h, 7d, 90m", s)),
    };
    let n: u64 = num
        .parse()
        .map_err(|_| anyhow!("invalid duration '{}': use e.g. 24h, 7d, 90m", s))?;
    n.checked_mul(mult)
        .ok_or_else(|| anyhow!("duration '{}' overflows", s))
}

/// Resolve --start/--end/--last into a bucket-aligned window (floor start,
/// ceil end) so the window itself never encodes finer-than-bucket timing.
fn resolve_window(args: &Args) -> Result<Option<ExportWindow>> {
    let (raw_start, raw_end) = match (&args.last, args.start, args.end) {
        (Some(last), None, None) => {
            let dur = parse_duration_s(last)?;
            let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
            (now.saturating_sub(dur), now)
        }
        (None, Some(start), Some(end)) => (start, end),
        (None, None, None) => return Ok(None),
        // clap's `requires`/`conflicts_with` make the remaining combinations unreachable.
        _ => return Err(anyhow!("--start/--end must be given together")),
    };
    if raw_start >= raw_end {
        return Err(anyhow!("export window start must be before end"));
    }
    let end_epoch_s = raw_end
        .div_ceil(BUCKET_S)
        .checked_mul(BUCKET_S)
        .ok_or_else(|| anyhow!("export window end is too large"))?;
    Ok(Some(ExportWindow {
        start_epoch_s: raw_start / BUCKET_S * BUCKET_S,
        end_epoch_s,
    }))
}

fn main() -> Result<()> {
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);

    if !args.self_export && args.break_glass_token.is_none() {
        return Err(anyhow!(
            "choose an authorization mode: --self-export (owner, device key seed) \
             or --break-glass-token <FILE> (trustee quorum)"
        ));
    }

    let cfg = KernelConfig {
        db_path: args.db_path.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: args.device_key_seed.trim().to_string(),
        zone_policy: ZonePolicy::default(),
    };

    if cfg.device_key_seed.is_empty() {
        return Err(anyhow!("DEVICE_KEY_SEED must be set"));
    }

    let window = resolve_window(&args)?;
    let mut kernel = {
        let _stage = ui.stage("Open kernel");
        Kernel::open(&cfg)?
    };
    let options = ExportOptions {
        max_events_per_batch: args.max_events_per_batch,
        jitter_s: args.jitter_s,
        jitter_step_s: args.jitter_step_s,
        window,
    };
    let (bundle, mode) = if args.self_export {
        let _stage = ui.stage("Export events (self)");
        (
            kernel.export_events_bundle_self(cfg.ruleset_hash, options)?,
            "self-export (device key seed)",
        )
    } else {
        let token_path = args.break_glass_token.as_deref().expect("checked above");
        let mut token = {
            let _stage = ui.stage("Load break-glass token");
            let token_json = std::fs::read_to_string(token_path)
                .map_err(|e| anyhow!("failed to read token file {}: {}", token_path, e))?;
            let token_file: BreakGlassTokenFile = serde_json::from_str(&token_json)
                .map_err(|e| anyhow!("invalid token file: {}", e))?;
            token_file.into_token()?
        };
        let _stage = ui.stage("Export events");
        (
            kernel.export_events_bundle_authorized(cfg.ruleset_hash, options, &mut token)?,
            "break-glass (trustee quorum)",
        )
    };
    let json = serde_json::to_vec(&bundle)?;
    {
        let _stage = ui.stage("Write export bundle");
        std::fs::write(&args.output, json)?;
    }
    let bucket_count: usize = bundle
        .artifact
        .batches
        .iter()
        .map(|b| b.buckets.len())
        .sum();
    println!("export bundle written to {}", args.output);
    println!("  authorization: {}", mode);
    match window {
        Some(w) => println!(
            "  window (bucket-aligned): {}..{} ({} bucket(s) in {} batch(es))",
            w.start_epoch_s,
            w.end_epoch_s,
            bucket_count,
            bundle.artifact.batches.len()
        ),
        None => println!(
            "  window: full retained history ({} bucket(s) in {} batch(es))",
            bucket_count,
            bundle.artifact.batches.len()
        ),
    }
    println!(
        "  receipt: signed export receipt {}… appended to the tamper-evident log",
        &hex::encode(bundle.receipt_entry.entry_hash)[..16]
    );
    if args.jitter_s > 0 {
        println!(
            "  note: exported times are coarsened to {}-minute buckets and jittered ±{}s \
             (step {}s) by design — see spec/event_contract.md",
            BUCKET_S / 60,
            args.jitter_s,
            args.jitter_step_s
        );
    } else {
        println!(
            "  note: exported times are coarsened to {}-minute buckets by design — \
             see spec/event_contract.md",
            BUCKET_S / 60
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{parse_duration_s, resolve_window, Args, BUCKET_S};
    use clap::Parser;

    #[test]
    fn windows_align_outward_and_reject_overflow() {
        let args = |start: &str, end: &str| {
            Args::parse_from([
                "export_events",
                "--device-key-seed",
                "devkey:test:seed",
                "--self-export",
                "--start",
                start,
                "--end",
                end,
            ])
        };
        let w = resolve_window(&args("601", "1799")).unwrap().unwrap();
        assert_eq!(w.start_epoch_s, 600);
        assert_eq!(w.end_epoch_s, 1800);
        // End near u64::MAX must error instead of overflowing on bucket ceil.
        let max = u64::MAX.to_string();
        assert!(resolve_window(&args("600", &max)).is_err());
        // Exact multiple of the bucket stays put.
        let w = resolve_window(&args("1200", "2400")).unwrap().unwrap();
        assert_eq!((w.start_epoch_s, w.end_epoch_s), (1200, 2400));
        assert_eq!(w.end_epoch_s % BUCKET_S, 0);
    }

    #[test]
    fn durations_parse() {
        assert_eq!(parse_duration_s("24h").unwrap(), 86_400);
        assert_eq!(parse_duration_s("7d").unwrap(), 604_800);
        assert_eq!(parse_duration_s("90m").unwrap(), 5_400);
        assert_eq!(parse_duration_s("3600s").unwrap(), 3_600);
        assert_eq!(parse_duration_s("3600").unwrap(), 3_600);
        assert!(parse_duration_s("yesterday").is_err());
        assert!(parse_duration_s("h").is_err());
    }
}
