//! export_events - sequential event export bundle to a local artifact

use anyhow::{anyhow, Result};
use clap::Parser;
use std::time::Duration;
use witness_kernel::break_glass::BreakGlassTokenFile;
use witness_kernel::{ExportOptions, Kernel, KernelConfig};

#[derive(Parser, Debug)]
#[command(author, version, about)]
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
    /// Path to a break-glass token file authorizing export.
    #[arg(long)]
    break_glass_token: String,
    /// Maximum events per export batch.
    #[arg(long, default_value_t = 50)]
    max_events_per_batch: usize,
    /// Jitter in seconds applied to exported time buckets.
    #[arg(long, default_value_t = 120)]
    jitter_s: u64,
    /// Jitter step in seconds (granularity of jitter).
    #[arg(long, default_value_t = 60)]
    jitter_step_s: u64,
}

fn main() -> Result<()> {
    let args = Args::parse();
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);

    let cfg = KernelConfig {
        db_path: args.db_path,
        ruleset_id: args.ruleset_id,
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: args.device_key_seed.trim().to_string(),
    };

    if cfg.device_key_seed.is_empty() {
        return Err(anyhow!("DEVICE_KEY_SEED must be set"));
    }

    let mut kernel = Kernel::open(&cfg)?;
    let token_json = std::fs::read_to_string(&args.break_glass_token).map_err(|e| {
        anyhow!(
            "failed to read token file {}: {}",
            args.break_glass_token,
            e
        )
    })?;
    let token_file: BreakGlassTokenFile =
        serde_json::from_str(&token_json).map_err(|e| anyhow!("invalid token file: {}", e))?;
    let mut token = token_file.into_token()?;
    let options = ExportOptions {
        max_events_per_batch: args.max_events_per_batch,
        jitter_s: args.jitter_s,
        jitter_step_s: args.jitter_step_s,
    };
    let bundle = kernel.export_events_bundle_authorized(cfg.ruleset_hash, options, &mut token)?;
    let json = serde_json::to_vec(&bundle)?;
    std::fs::write(&args.output, json)?;
    println!("export bundle written to {}", args.output);
    Ok(())
}
