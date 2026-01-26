//! grove_vision2_ingest - Ingest Grove Vision 2 event-only payloads.
//!
//! This ingestion path accepts only event-contract-compatible fields:
//! event_type, time_bucket, zone_id, confidence.
//! Any extra fields are rejected and logged as conformance alarms.

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use std::io::{self, BufRead};
use std::path::Path;
use std::time::Duration;

use witness_kernel::module_runtime::event_payload;
use witness_kernel::{EventType, Kernel, KernelConfig, ModuleDescriptor, ZonePolicy};

const INGEST_NAME: &str = "grove_vision2_ingest";

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "Ingest Grove Vision 2 event-only payloads into the Privacy Witness Kernel"
)]
struct Args {
    /// Path to witness database.
    #[arg(long, env = "WITNESS_DB_PATH", default_value = "witness.db")]
    db_path: String,

    /// Ruleset identifier for logged events.
    #[arg(
        long,
        env = "WITNESS_RULESET_ID",
        default_value = "ruleset:grove_vision2_v1"
    )]
    ruleset_id: String,

    /// Optional serial device path to read directly (reconnects on disconnect).
    #[arg(long, env = "WITNESS_SERIAL_DEVICE")]
    serial_device: Option<String>,

    /// Delay between reconnect attempts when using --serial-device.
    #[arg(long, env = "WITNESS_RECONNECT_DELAY_SECS", default_value = "2")]
    reconnect_delay_secs: u64,
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let device_key_seed =
        std::env::var("DEVICE_KEY_SEED").map_err(|_| anyhow!("DEVICE_KEY_SEED must be set"))?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);
    let cfg = KernelConfig {
        db_path: args.db_path.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let mut kernel = Kernel::open(&cfg)?;

    let module_desc = ModuleDescriptor {
        id: INGEST_NAME,
        allowed_event_types: &[
            EventType::BoundaryCrossingObjectLarge,
            EventType::BoundaryCrossingObjectSmall,
        ],
        requested_capabilities: &[],
    };

    if let Some(serial_device) = args.serial_device.as_deref() {
        let reconnect_delay = Duration::from_secs(args.reconnect_delay_secs);
        let device_path = Path::new(serial_device);
        loop {
            match std::fs::File::open(device_path)
                .with_context(|| format!("failed to open serial device {}", device_path.display()))
            {
                Ok(file) => {
                    let reader = io::BufReader::new(file);
                    if let Err(err) = ingest_lines(
                        reader,
                        &mut kernel,
                        &cfg,
                        &module_desc,
                        &format!("serial:{}", device_path.display()),
                    ) {
                        log::warn!("Serial input ended: {}", err);
                    } else {
                        log::warn!("Serial input ended (EOF)");
                    }
                }
                Err(err) => {
                    log::warn!("Serial device unavailable: {}", err);
                }
            }

            log::info!(
                "Reconnecting to serial device in {}s",
                reconnect_delay.as_secs()
            );
            std::thread::sleep(reconnect_delay);
        }
    } else {
        let stdin = io::stdin();
        ingest_lines(stdin.lock(), &mut kernel, &cfg, &module_desc, "stdin")?;
    }

    Ok(())
}

fn ingest_lines<R: BufRead>(
    reader: R,
    kernel: &mut Kernel,
    cfg: &KernelConfig,
    module_desc: &ModuleDescriptor,
    source: &str,
) -> Result<()> {
    for line in reader.lines() {
        let line = line.with_context(|| format!("failed to read {}", source))?;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        let value: serde_json::Value = match serde_json::from_str(trimmed) {
            Ok(value) => value,
            Err(err) => {
                kernel.log_alarm(
                    "CONFORMANCE_GROVE_VISION2_INPUT_REJECT",
                    &format!("conformance: invalid json payload: {}", err),
                )?;
                continue;
            }
        };

        let candidate = match event_payload::parse_event_payload(&value) {
            Ok(candidate) => candidate,
            Err(err) => {
                kernel.log_alarm(
                    "CONFORMANCE_GROVE_VISION2_INPUT_REJECT",
                    &format!("{}", err),
                )?;
                continue;
            }
        };

        if let Err(err) = kernel.append_event_checked(
            module_desc,
            candidate,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        ) {
            log::warn!("Failed to append Grove Vision 2 event: {}", err);
        } else {
            log::info!("Grove Vision 2 event appended");
        }
    }

    Ok(())
}
