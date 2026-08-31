//! `break_glass_serve` — kernel-served break-glass web UI (request → approve →
//! unseal) over HTTP.
//!
//! Operator-mediated and loopback-default: binds `127.0.0.1` unless a routable
//! address is given *with* TLS materials — terminated in-process on an
//! `api-tls` build, by a front proxy otherwise. Every request needs the
//! rotating capability token written to `--token-path`. See
//! `src/break_glass/server.rs` for the security model.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use clap::Parser;

use witness_kernel::api::ApiTlsConfig;
use witness_kernel::break_glass::server::{BreakGlassServer, BreakGlassServerConfig};
use witness_kernel::break_glass::KernelVaultOps;
use witness_kernel::{KernelConfig, ZonePolicy};

#[derive(Parser, Debug)]
#[command(
    name = "break_glass_serve",
    about = "Serve the break-glass web UI (Invariant V)"
)]
struct Args {
    /// Address to bind. Loopback by default; a routable address requires TLS.
    #[arg(long, default_value = "127.0.0.1:8800")]
    addr: String,
    #[arg(long, default_value = "witness.db")]
    db: String,
    #[arg(long, default_value = "ruleset:v0.3.0")]
    ruleset_id: String,
    /// Device key seed (must match witnessd).
    #[arg(long, env = "DEVICE_KEY_SEED")]
    device_key_seed: String,
    #[arg(long, default_value = "vault/envelopes")]
    vault_path: String,
    /// Directory unsealed envelopes are written to (0600).
    #[arg(long, default_value = "vault/unsealed")]
    output_dir: String,
    /// Path to persist the rotating capability token (mode 0600).
    #[arg(long, env = "BREAK_GLASS_TOKEN_PATH")]
    token_path: Option<PathBuf>,
    /// TLS certificate chain (PEM). Required with TLS key to bind non-loopback.
    #[arg(long, requires = "tls_key")]
    tls_cert: Option<PathBuf>,
    /// TLS private key (PEM).
    #[arg(long, requires = "tls_cert")]
    tls_key: Option<PathBuf>,
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);
    let kernel_cfg = KernelConfig {
        db_path: args.db.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: args.device_key_seed.clone(),
        zone_policy: ZonePolicy::default(),
    };

    let ops = KernelVaultOps::open(kernel_cfg, args.vault_path.clone())?;

    let tls = match (args.tls_cert.as_ref(), args.tls_key.as_ref()) {
        (Some(cert), Some(key)) => Some(ApiTlsConfig::load(cert, key)?),
        _ => None,
    };
    let tls_configured = tls.as_ref().is_some_and(|t| t.is_configured());
    let server_cfg = BreakGlassServerConfig {
        addr: args.addr.clone(),
        output_dir: args.output_dir.clone(),
        token_path: args.token_path.clone(),
        tls,
    };

    let server = BreakGlassServer::bind(server_cfg)?;
    let addr = server.local_addr();
    log::info!("break-glass server bound to {addr}");
    // On an `api-tls` build with material configured, the listener itself
    // terminates TLS — advertise https:// directly. Otherwise only advertise
    // the plaintext URL for loopback: on a routable bind the listener is
    // plaintext behind a TLS terminator, so pointing operators at
    // http://<addr> directly would bypass the proxy and leak the token.
    let in_process_tls = cfg!(feature = "api-tls") && tls_configured;
    if in_process_tls {
        log::info!("operator console: https://{addr}/breakglass (TLS terminated in-process)");
    } else if addr.ip().is_loopback() {
        log::info!("operator console: http://{addr}/breakglass");
    } else {
        log::info!(
            "operator console served at /breakglass — reach it via your HTTPS terminator, not http://{addr} directly"
        );
    }
    match &args.token_path {
        Some(path) => log::info!("capability token written to {}", path.display()),
        None => log::warn!(
            "no --token-path set; the capability token is not retrievable — clients cannot authenticate"
        ),
    }

    let shutdown = Arc::new(AtomicBool::new(false));
    let shutdown_handler = shutdown.clone();
    ctrlc::set_handler(move || {
        shutdown_handler.store(true, Ordering::SeqCst);
    })
    .expect("error setting Ctrl-C handler");

    log::info!("break-glass server running (Ctrl-C to stop)...");
    server.serve(ops, shutdown)?;
    log::info!("break-glass server stopped");
    Ok(())
}
