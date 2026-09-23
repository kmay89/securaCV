//! witness_api - API-only service for Privacy Witness Kernel
//!
//! This daemon:
//! 1. Opens the kernel database
//! 2. Serves the Event API
//! 3. Does NOT ingest RTSP streams

use anyhow::{anyhow, Result};
use std::sync::mpsc;

use witness_kernel::{
    api::{ApiConfig, ApiServer, ApiTlsConfig},
    KernelConfig, ZonePolicy,
};

/// True when an environment variable is set to a truthy value (`1`/`true`).
fn env_flag(name: &str) -> bool {
    std::env::var(name)
        .map(|v| {
            let v = v.trim();
            v == "1" || v.eq_ignore_ascii_case("true")
        })
        .unwrap_or(false)
}

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

    // Operator subcommands run to completion against the same config and
    // never serve; with no argument the daemon path below is unchanged.
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some((command, rest)) = args.split_first() {
        return run_subcommand(command, rest, &config, &cfg);
    }

    let api_config = ApiConfig {
        addr: config.api_addr.clone(),
        token_path: config.api_token_path.clone(),
        fleet_peers_path: config.api_fleet_peers_path.clone(),
        viewer_token_path: config.api_viewer_token_path.clone(),
        rate_limit_per_minute: config.api_rate_limit_per_minute,
        // Explicit opt-in required to expose the plaintext API off-loopback.
        allow_insecure: env_flag("WITNESS_API_ALLOW_INSECURE"),
        // In-process TLS material (WITNESS_API_TLS_CERT / WITNESS_API_TLS_KEY,
        // paths to PEM files). Terminated by the server only on an `api-tls`
        // build; configuring it on any other build is a startup error.
        tls: ApiTlsConfig::from_env()?,
        ..ApiConfig::default()
    };
    let api_handle = ApiServer::new(api_config, cfg.clone()).spawn()?;
    log::info!("event api listening on {}", api_handle.addr);
    if let Some(path) = &api_handle.token_path {
        log::info!("event api capability token written to {}", path.display());
    } else {
        log::warn!(
            "event api capability token not written to file; use --api-token-path to persist it safely"
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

// ---------------------------------------------------------------------------
// Operator subcommands — viewer credentials for the Witness Wall.
//
// `witness_api mint-viewer-token --label <s> [--base-url <url>]` prints a
// pairing receipt ONCE and keeps only the token's sha256 in the viewer
// token file; `witness_api revoke-viewer-token <id>` removes it. Kept apart
// from the serving path above on purpose.
// (`anyhow::anyhow!` is spelled out below so this block leaves the file's
// top-level imports alone.)
// ---------------------------------------------------------------------------

use witness_kernel::{api::ViewerTokenSet, config::WitnessApiConfig, Kernel};

const SUBCOMMAND_USAGE: &str = "\
usage: witness_api                                    serve the event API
       witness_api mint-viewer-token --label <s> [--base-url <url>]
                                                      mint a long-lived token good for GET /api/sealed-log only;
                                                      prints the pairing receipt ONCE (the file keeps only its sha256)
       witness_api revoke-viewer-token <id>           revoke one (the id on its receipt, or in the file)
The viewer token file is api.viewer_token_path / WITNESS_API_VIEWER_TOKEN_PATH,
defaulting to viewer_tokens.json beside the capability token (api.token_path).";

fn run_subcommand(
    command: &str,
    args: &[String],
    config: &WitnessApiConfig,
    cfg: &KernelConfig,
) -> Result<()> {
    match command {
        "mint-viewer-token" => mint_viewer_token(args, config, cfg),
        "revoke-viewer-token" => revoke_viewer_token(args, config),
        "help" | "-h" | "--help" => {
            eprintln!("{SUBCOMMAND_USAGE}");
            Ok(())
        }
        other => Err(anyhow::anyhow!(
            "unknown subcommand {other:?}\n{SUBCOMMAND_USAGE}"
        )),
    }
}

fn viewer_token_path(config: &WitnessApiConfig) -> Result<&std::path::Path> {
    config.api_viewer_token_path.as_deref().ok_or_else(|| {
        anyhow::anyhow!(
            "no viewer token file is configured: set api.token_path (the viewer file then \
             sits beside the capability token) or WITNESS_API_VIEWER_TOKEN_PATH"
        )
    })
}

fn mint_viewer_token(args: &[String], config: &WitnessApiConfig, cfg: &KernelConfig) -> Result<()> {
    let mut label: Option<String> = None;
    let mut base_url: Option<String> = None;
    let mut it = args.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--label" => {
                label = Some(
                    it.next()
                        .cloned()
                        .ok_or_else(|| anyhow::anyhow!("--label needs a value"))?,
                )
            }
            "--base-url" => {
                base_url = Some(
                    it.next()
                        .cloned()
                        .ok_or_else(|| anyhow::anyhow!("--base-url needs a value"))?,
                )
            }
            other => {
                return Err(anyhow::anyhow!(
                    "unknown argument {other:?}\n{SUBCOMMAND_USAGE}"
                ))
            }
        }
    }
    let label = label.ok_or_else(|| {
        anyhow::anyhow!(
            "--label is required (which screen holds this token, e.g. \"living room tv\")"
        )
    })?;
    if let Some(url) = &base_url {
        if !(url.starts_with("http://") || url.starts_with("https://")) {
            return Err(anyhow::anyhow!(
                "--base-url must start with http:// or https:// (e.g. http://192.168.1.20:8799)"
            ));
        }
    }
    let path = viewer_token_path(config)?;
    // Open the kernel rather than deriving from the seed: a retired seed
    // cannot reopen the log, so the receipt can only ever carry the key
    // /api/sealed-log actually serves.
    let kernel = Kernel::open(cfg)?;
    let verifying_key = hex::encode(kernel.device_verifying_key().to_bytes());
    drop(kernel);

    let receipt = ViewerTokenSet::mint(path, &label, verifying_key, base_url)?;
    // stdout: the receipt, once. stderr: everything else, so a redirect
    // captures exactly the JSON.
    println!("{}", serde_json::to_string(&receipt)?);
    eprintln!(
        "viewer token {} minted for {:?}; this is the only time the token is shown — {} keeps its sha256. \
         Paste the line above into the Witness Wall's pairing field. Revoke it with revoke-viewer-token {}, run the way you ran this.",
        receipt.token_id,
        label.trim(),
        path.display(),
        receipt.token_id
    );
    if receipt.base_url.is_none() {
        eprintln!(
            "no --base-url given: the Wall connects to the address you typed into it (http://<this host>:{} — with the port)",
            config.api_addr.rsplit(':').next().unwrap_or("8799")
        );
    }
    Ok(())
}

fn revoke_viewer_token(args: &[String], config: &WitnessApiConfig) -> Result<()> {
    let id = match args {
        [id] => id,
        _ => {
            return Err(anyhow::anyhow!(
                "revoke-viewer-token takes exactly one argument, the token id\n{SUBCOMMAND_USAGE}"
            ))
        }
    };
    let path = viewer_token_path(config)?;
    let removed = ViewerTokenSet::revoke(path, id)?;
    eprintln!(
        "viewer token {} ({:?}, minted {}) revoked; the next request carrying it is refused",
        removed.id, removed.label, removed.minted
    );
    Ok(())
}
