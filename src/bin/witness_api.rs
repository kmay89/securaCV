//! witness_api - API-only service for Privacy Witness Kernel
//!
//! This daemon:
//! 1. Opens the kernel database
//! 2. Serves the Event API
//! 3. Does NOT ingest RTSP streams

use anyhow::{anyhow, Result};
use std::sync::mpsc;

use witness_kernel::{
    api::{ApiConfig, ApiServer},
    KernelConfig, ZonePolicy,
};

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let kernel_version = env!("CARGO_PKG_VERSION");
    let device_key_seed =
        std::env::var("DEVICE_KEY_SEED").map_err(|_| anyhow!("DEVICE_KEY_SEED must be set"))?;
    let config = witness_kernel::config::WitnessApiConfig::load()?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&config.ruleset_id);

    let cfg = KernelConfig {
        db_path: config.db_path.clone(),
        ruleset_id: config.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: kernel_version.to_string(),
        retention: config.retention,
        device_key_seed,
        zone_policy: ZonePolicy::new(config.sensitive_zones.clone())?,
    };

    let api_config = ApiConfig {
        addr: config.api_addr.clone(),
        token_path: config.api_token_path.clone(),
        ..ApiConfig::default()
    };
    let api_handle = ApiServer::new(api_config, cfg.clone()).spawn()?;
    log::info!("event api listening on {}", api_handle.addr);
    if let Some(path) = &api_handle.token_path {
        log::info!("event api capability token written to {}", path.display());
    } else {
        log::warn!(
            "event api capability token (handle securely): {}",
            api_handle.token
        );
    }
    log::info!("witness_api running. serving {}", cfg.db_path);

    let (tx, rx) = mpsc::channel();
    ctrlc::set_handler(move || {
        let _ = tx.send(());
    })
    .expect("error setting Ctrl-C handler");

    log::info!("witness_api waiting for shutdown signal (Ctrl-C)...");
    let _ = rx.recv();
    log::info!("shutdown signal received, stopping API server...");
    api_handle.stop()?;

    Ok(())
}
