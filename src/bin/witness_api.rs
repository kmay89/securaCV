//! witness_api - API-only service for Privacy Witness Kernel
//!
//! This daemon:
//! 1. Opens the kernel database
//! 2. Serves the Event API (with a storage sampler behind `GET /status`)
//! 3. Does NOT ingest RTSP streams
//!
//! Operator subcommands (`mint-viewer-token`, `issue-pairing-code`, …) run
//! against the same config and never serve; see the usage block below.

use anyhow::Result;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, RwLock};
use std::time::Duration;

use witness_kernel::{
    api::{ApiConfig, ApiServer, ApiTlsConfig},
    storage_health::{MonitorSettings, SharedStorageHealth, StorageHealthMonitor},
    KernelConfig, ZonePolicy,
};

/// How often the storage sampler behind `GET /status` re-reads free space
/// (and whatever else the host exposes) for the database's directory. The
/// same cadence witnessd's monitor defaults to; one small state-file write
/// per sample, negligible wear.
const STATUS_SAMPLE_INTERVAL: Duration = Duration::from_secs(600);

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
    let config = witness_kernel::config::WitnessApiConfig::load()?;

    // Operator subcommands run to completion against the same config and
    // never serve; with no argument the daemon path below is unchanged. They
    // are dispatched BEFORE the seed is resolved: that resolution creates and
    // persists a device identity on a host that has none, and `help`, a
    // mint or a revoke must never be what mints one.
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some((command, rest)) = args.split_first() {
        return run_subcommand(command, rest, &config, kernel_version);
    }

    // DEVICE_KEY_SEED, else the seed file witnessd keeps beside the database,
    // else a fresh one generated there. Only the source is logged.
    let device_key_seed = {
        let env_seed = std::env::var("DEVICE_KEY_SEED").ok();
        let resolved =
            witness_kernel::crypto::resolve_device_seed(&config.db_path, env_seed.as_deref())?;
        log::info!("device key seed: {}", resolved.source);
        resolved.seed
    };
    let cfg = kernel_config(&config, kernel_version, device_key_seed)?;

    let api_config = ApiConfig {
        addr: config.api_addr.clone(),
        token_path: config.api_token_path.clone(),
        fleet_peers_path: config.api_fleet_peers_path.clone(),
        viewer_token_path: config.api_viewer_token_path.clone(),
        pairing: config.api_access.pairing_paths(),
        fleet_rollcall: config.api_access.fleet_rollcall,
        rate_limit_per_minute: config.api_rate_limit_per_minute,
        // Explicit opt-in required to expose the plaintext API off-loopback.
        allow_insecure: env_flag("WITNESS_API_ALLOW_INSECURE"),
        // In-process TLS material (WITNESS_API_TLS_CERT / WITNESS_API_TLS_KEY,
        // paths to PEM files). Terminated by the server only on an `api-tls`
        // build; configuring it on any other build is a startup error.
        tls: ApiTlsConfig::from_env()?,
        ..ApiConfig::default()
    };
    if api_config.pairing.is_some() {
        log::info!(
            "pairing enabled: codes from `witness_api issue-pairing-code`, bearers in {}",
            config
                .api_access
                .paired_token_path
                .as_deref()
                .map(|p| p.display().to_string())
                .unwrap_or_default()
        );
    }
    log::info!(
        "fleet roll-call: {}",
        config.api_access.fleet_rollcall.as_str()
    );

    // `GET /status` needs a sampler. witnessd runs one inside its main
    // loop; this daemon has no loop, so a thread samples the database
    // directory's free space (and the card wear counters where the host
    // exposes them) into the snapshot the route serves — the Home
    // Assistant storage entities read what is known and show `unknown`
    // for the rest. Without it the sidecar and the Frigate-mode add-on
    // answered 404 forever.
    let storage_health: SharedStorageHealth = Arc::new(RwLock::new(None));
    let sampler_stop = Arc::new(AtomicBool::new(false));
    let sampler = spawn_status_sampler(
        &config.db_path,
        storage_health.clone(),
        sampler_stop.clone(),
    );

    let api_handle = ApiServer::new(api_config, cfg.clone())
        .with_storage_health(storage_health)
        .spawn()?;
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
    sampler_stop.store(true, Ordering::SeqCst);
    api_handle.stop()?;
    let _ = sampler.join();

    Ok(())
}

/// Sample storage health for `db_path`'s directory into `snapshot` now and
/// every [`STATUS_SAMPLE_INTERVAL`], until `stop` is raised. Default
/// thresholds and endurance rating (this daemon's config has no
/// `storage_health` block); a probe the host lacks reads as `None`, never
/// as a failure of the sampler.
fn spawn_status_sampler(
    db_path: &str,
    snapshot: SharedStorageHealth,
    stop: Arc<AtomicBool>,
) -> std::thread::JoinHandle<()> {
    let defaults = witness_kernel::config::StorageHealthSettings::default();
    let mut monitor = StorageHealthMonitor::new(
        MonitorSettings {
            endurance_tbw: defaults.endurance_tbw,
            block_device: None,
            state_path: None,
            thresholds: defaults.thresholds,
        },
        db_path,
    );
    std::thread::spawn(move || {
        while !stop.load(Ordering::SeqCst) {
            match monitor.sample() {
                Ok(report) => {
                    if let Ok(mut guard) = snapshot.write() {
                        *guard = Some(report);
                    }
                }
                Err(err) => log::warn!("storage health sample failed: {err:#}"),
            }
            // Sleep in slices so shutdown does not wait out the interval.
            let mut slept = Duration::ZERO;
            while slept < STATUS_SAMPLE_INTERVAL && !stop.load(Ordering::SeqCst) {
                let slice = Duration::from_secs(1).min(STATUS_SAMPLE_INTERVAL - slept);
                std::thread::sleep(slice);
                slept += slice;
            }
        }
    })
}

/// The kernel configuration both the daemon and the subcommands open.
fn kernel_config(
    config: &witness_kernel::config::WitnessApiConfig,
    kernel_version: &str,
    device_key_seed: String,
) -> Result<KernelConfig> {
    Ok(KernelConfig {
        db_path: config.db_path.clone(),
        ruleset_id: config.ruleset_id.clone(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(&config.ruleset_id),
        kernel_version: kernel_version.to_string(),
        retention: config.retention,
        device_key_seed,
        zone_policy: ZonePolicy::new(config.sensitive_zones.clone())?,
    })
}

// ---------------------------------------------------------------------------
// Operator subcommands — viewer credentials for the Witness Wall, pairing
// for the Home Assistant integration.
//
// `witness_api mint-viewer-token --label <s> [--base-url <url>]` prints a
// pairing receipt ONCE and keeps only the token's sha256 in the viewer
// token file; `witness_api revoke-viewer-token <id>` removes it.
// `witness_api issue-pairing-code [--ttl-secs <n>] [--label <s>]` prints a
// 6-digit code ONCE (the file keeps its sha256 and a 5-guess budget);
// `list-paired-tokens` / `revoke-paired-token <id>` manage the bearers
// `POST /api/pair` minted. Kept apart from the serving path on purpose.
// (`anyhow::anyhow!` is spelled out below so this block leaves the file's
// top-level imports alone.)
// ---------------------------------------------------------------------------

use witness_kernel::{
    api::{
        pairing::{key_fingerprint, PairingCodeFile},
        ViewerTokenSet,
    },
    config::WitnessApiConfig,
    Kernel,
};

const SUBCOMMAND_USAGE: &str = "\
usage: witness_api                                    serve the event API
       witness_api mint-viewer-token --label <s> [--base-url <url>]
                                                      mint a long-lived token good for GET /api/sealed-log only;
                                                      prints the pairing receipt ONCE (the file keeps only its sha256)
       witness_api revoke-viewer-token <id>           revoke one (the id on its receipt, or in the file)
       witness_api issue-pairing-code [--ttl-secs <n>] [--label <s>]
                                                      issue the 6-digit code Home Assistant redeems at POST /api/pair;
                                                      prints it ONCE with the kernel key fingerprint (5 guesses, 10 min)
       witness_api list-paired-tokens                 list the paired bearers (id, label, minted — never the token)
       witness_api revoke-paired-token <id>           revoke one paired bearer; its next request is refused
The viewer token file is api.viewer_token_path / WITNESS_API_VIEWER_TOKEN_PATH,
defaulting to viewer_tokens.json beside the capability token (api.token_path).
The pairing files are api.pairing_code_path / WITNESS_API_PAIRING_CODE_PATH and
api.paired_token_path / WITNESS_API_PAIRED_TOKEN_PATH, defaulting to pairing_code
and paired_tokens.json beside the capability token.";

fn run_subcommand(
    command: &str,
    args: &[String],
    config: &WitnessApiConfig,
    kernel_version: &str,
) -> Result<()> {
    match command {
        "mint-viewer-token" => mint_viewer_token(args, config, kernel_version),
        "revoke-viewer-token" => revoke_viewer_token(args, config),
        "issue-pairing-code" => issue_pairing_code(args, config, kernel_version),
        "list-paired-tokens" => list_paired_tokens(args, config),
        "revoke-paired-token" => revoke_paired_token(args, config),
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

fn pairing_paths(config: &WitnessApiConfig) -> Result<witness_kernel::api::PairingPaths> {
    config.api_access.pairing_paths().ok_or_else(|| {
        anyhow::anyhow!(
            "pairing is not configured: set api.token_path (the pairing files then sit beside \
             the capability token) or WITNESS_API_PAIRING_CODE_PATH and \
             WITNESS_API_PAIRED_TOKEN_PATH"
        )
    })
}

/// The kernel this host serves, opened so a receipt or a pairing code can
/// only ever name the key `/api/sealed-log` actually serves, and the seed
/// it resolved to. Find, never create: a host with no seed has no log to
/// pair with.
fn open_existing_kernel(
    config: &WitnessApiConfig,
    kernel_version: &str,
) -> Result<(Kernel, String)> {
    let env_seed = std::env::var("DEVICE_KEY_SEED").ok();
    let found = witness_kernel::crypto::find_device_seed(&config.db_path, env_seed.as_deref())?
        .ok_or_else(|| {
            anyhow::anyhow!(
                "this host has no device identity yet (no DEVICE_KEY_SEED and no seed file \
                 beside {}): start witness_api once so the kernel creates it, then retry",
                config.db_path
            )
        })?;
    let cfg = kernel_config(config, kernel_version, found.seed.clone())?;
    // Open the kernel rather than deriving from the seed: a retired seed
    // cannot reopen the log, so the receipt can only ever carry the key
    // /api/sealed-log actually serves.
    Ok((Kernel::open(&cfg)?, found.seed))
}

fn mint_viewer_token(
    args: &[String],
    config: &WitnessApiConfig,
    kernel_version: &str,
) -> Result<()> {
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
    let (kernel, _seed) = open_existing_kernel(config, kernel_version)?;
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

/// What `issue-pairing-code` prints on stdout, once: the code, when it
/// dies, and the identity Home Assistant will pin — so the operator can
/// compare the fingerprint the pairing step shows against this one.
#[derive(serde::Serialize)]
struct PairingCodeReceipt<'a> {
    kernel: &'static str,
    pairing_code: &'a str,
    expires: u64,
    attempts: u32,
    device_id: String,
    verifying_key: String,
    fingerprint: String,
}

fn issue_pairing_code(
    args: &[String],
    config: &WitnessApiConfig,
    kernel_version: &str,
) -> Result<()> {
    let mut ttl_secs = config.api_access.pairing_ttl_secs;
    let mut label = "witness_api issue-pairing-code".to_string();
    let mut it = args.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--ttl-secs" => {
                let raw = it
                    .next()
                    .ok_or_else(|| anyhow::anyhow!("--ttl-secs needs a value"))?;
                ttl_secs = raw
                    .parse::<u64>()
                    .ok()
                    .filter(|n| *n > 0)
                    .ok_or_else(|| anyhow::anyhow!("--ttl-secs must be a positive integer"))?;
            }
            "--label" => {
                label = it
                    .next()
                    .cloned()
                    .ok_or_else(|| anyhow::anyhow!("--label needs a value"))?;
            }
            other => {
                return Err(anyhow::anyhow!(
                    "unknown argument {other:?}\n{SUBCOMMAND_USAGE}"
                ))
            }
        }
    }
    let paths = pairing_paths(config)?;
    let (kernel, seed) = open_existing_kernel(config, kernel_version)?;
    let verifying_key = hex::encode(kernel.device_verifying_key().to_bytes());
    drop(kernel);
    let device_id = witness_kernel::ha_device_id(&seed);
    let fingerprint = key_fingerprint(&verifying_key)?;

    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let issued = PairingCodeFile::issue(&paths.code_path, ttl_secs, &label, now)?;
    let receipt = PairingCodeReceipt {
        kernel: witness_kernel::api::KERNEL_NAME,
        pairing_code: &issued.code,
        expires: issued.expires,
        attempts: witness_kernel::api::pairing::PAIRING_CODE_ATTEMPTS,
        device_id,
        verifying_key,
        fingerprint: fingerprint.clone(),
    };
    // stdout: the receipt, once. stderr: the human sentence. The code is
    // never logged and never written anywhere but this stream.
    println!("{}", serde_json::to_string(&receipt)?);
    eprintln!(
        "pairing code {} issued (valid {} minutes, {} attempts); kernel key fingerprint {}. \
         Home Assistant shows the same fingerprint before it pairs — compare them. \
         {} keeps the code's sha256 only; the code is consumed on first success, burned on the \
         {}th wrong guess, and replaced by running this again.",
        issued.code,
        ttl_secs.div_ceil(60),
        witness_kernel::api::pairing::PAIRING_CODE_ATTEMPTS,
        fingerprint,
        paths.code_path.display(),
        witness_kernel::api::pairing::PAIRING_CODE_ATTEMPTS,
    );
    Ok(())
}

/// One paired bearer as `list-paired-tokens` prints it: everything in the
/// file except the hash.
#[derive(serde::Serialize)]
struct PairedTokenRow<'a> {
    id: &'a str,
    label: &'a str,
    minted: u64,
}

fn list_paired_tokens(args: &[String], config: &WitnessApiConfig) -> Result<()> {
    if !args.is_empty() {
        return Err(anyhow::anyhow!(
            "list-paired-tokens takes no arguments\n{SUBCOMMAND_USAGE}"
        ));
    }
    let paths = pairing_paths(config)?;
    let set = ViewerTokenSet::load_kind(&paths.tokens_path, "paired token")?;
    let rows: Vec<PairedTokenRow<'_>> = set
        .tokens
        .iter()
        .map(|entry| PairedTokenRow {
            id: &entry.id,
            label: &entry.label,
            minted: entry.minted,
        })
        .collect();
    println!("{}", serde_json::to_string(&rows)?);
    Ok(())
}

fn revoke_paired_token(args: &[String], config: &WitnessApiConfig) -> Result<()> {
    let id = match args {
        [id] => id,
        _ => {
            return Err(anyhow::anyhow!(
                "revoke-paired-token takes exactly one argument, the token id\n{SUBCOMMAND_USAGE}"
            ))
        }
    };
    let paths = pairing_paths(config)?;
    let removed = ViewerTokenSet::revoke_kind(&paths.tokens_path, id, "paired token")?;
    eprintln!(
        "paired token {} ({:?}, minted {}) revoked; the next request carrying it is refused",
        removed.id, removed.label, removed.minted
    );
    Ok(())
}
