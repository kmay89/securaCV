use crate::crypto::signatures::SignatureMode;
use crate::storage_health::SharedStorageHealth;
use crate::verify_runner::{run_full_verify, VerifyReport};
use crate::{ExportArtifact, ExportOptions, Kernel, KernelConfig, TimeBucket};
use anyhow::{anyhow, Result};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;
use subtle::ConstantTimeEq;

#[cfg(feature = "pqc-tls")]
use std::io::{BufReader, BufWriter};

const MAX_REQUEST_BYTES: usize = 8192;

/// TLS configuration for the kernel API server.
/// Mirrors firmware's DEFAULT_TLS_REQUIRED policy.
#[derive(Clone, Debug, Default)]
pub struct ApiTlsConfig {
    /// PEM-encoded certificate chain.
    pub cert_pem: Option<Vec<u8>>,
    /// PEM-encoded private key.
    pub key_pem: Option<Vec<u8>>,
}

impl ApiTlsConfig {
    /// Load TLS materials from file paths.
    pub fn load(cert_path: &Path, key_path: &Path) -> Result<Self> {
        let cert_pem = std::fs::read(cert_path).map_err(|e| {
            anyhow!(
                "failed to read API TLS cert '{}': {}",
                cert_path.display(),
                e
            )
        })?;
        let key_pem = std::fs::read(key_path)
            .map_err(|e| anyhow!("failed to read API TLS key '{}': {}", key_path.display(), e))?;
        Ok(Self {
            cert_pem: Some(cert_pem),
            key_pem: Some(key_pem),
        })
    }

    pub fn is_configured(&self) -> bool {
        self.cert_pem.is_some() && self.key_pem.is_some()
    }
}

#[derive(Clone, Debug)]
pub struct ApiConfig {
    pub addr: String,
    pub export_options: ExportOptions,
    pub token_path: Option<PathBuf>,
    /// TLS configuration. When set, the API server uses TLS.
    pub tls: Option<ApiTlsConfig>,
    /// Allow plaintext HTTP without TLS. Logs a conformance alarm if true.
    pub allow_insecure: bool,
    /// Max requests per client IP per minute on every endpoint except
    /// `/health` (fixed window). Applies before token validation, so a
    /// stolen-or-not client cannot hammer `/events` or `POST /verify`
    /// (which walks the whole sealed log). 0 disables the limit.
    pub rate_limit_per_minute: u32,
}

/// Generous for legitimate clients — the HA coordinator polls every 30 s and
/// the panel is human-driven — while capping what a leaked capability token
/// is worth for resource exhaustion.
pub const DEFAULT_API_RATE_LIMIT_PER_MINUTE: u32 = 120;

impl Default for ApiConfig {
    fn default() -> Self {
        Self {
            addr: "127.0.0.1:8799".to_string(),
            export_options: ExportOptions::default(),
            token_path: None,
            tls: None,
            allow_insecure: false,
            rate_limit_per_minute: DEFAULT_API_RATE_LIMIT_PER_MINUTE,
        }
    }
}

#[derive(Debug)]
pub struct ApiHandle {
    pub addr: SocketAddr,
    pub token: String,
    pub token_path: Option<PathBuf>,
    shutdown: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl ApiHandle {
    pub fn stop(mut self) -> Result<()> {
        self.shutdown.store(true, Ordering::SeqCst);
        if let Some(join) = self.join.take() {
            join.join()
                .map_err(|_| anyhow!("api server thread panicked"))?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug)]
pub struct CapabilityTokenManager {
    current_bucket: Option<TimeBucket>,
    token: [u8; 32],
}

impl CapabilityTokenManager {
    pub fn new(bucket: TimeBucket) -> Result<Self> {
        let mut token = [0u8; 32];
        rand::fill(&mut token[..]);
        Ok(Self {
            current_bucket: Some(bucket),
            token,
        })
    }

    pub fn rotate_if_needed(&mut self, bucket: TimeBucket) -> Result<bool> {
        if self.current_bucket == Some(bucket) {
            return Ok(false);
        }
        let mut token = [0u8; 32];
        rand::fill(&mut token[..]);
        self.current_bucket = Some(bucket);
        self.token = token;
        Ok(true)
    }

    pub fn token_hex(&self) -> String {
        hex::encode(self.token)
    }

    pub fn validate(&self, presented: &str, bucket: TimeBucket) -> Result<()> {
        if self.current_bucket != Some(bucket) {
            return Err(anyhow!("capability token expired"));
        }
        let presented = parse_hex32(presented)?;
        // SECURITY: Use constant-time comparison to prevent timing side-channel
        // attacks that could leak token bytes. The `!=` operator on byte arrays
        // short-circuits on the first differing byte, enabling byte-by-byte
        // extraction via response-time measurement.
        if presented.ct_eq(&self.token).unwrap_u8() != 1 {
            return Err(anyhow!("capability token invalid"));
        }
        Ok(())
    }
}

pub struct ApiServer {
    cfg: ApiConfig,
    kernel_cfg: KernelConfig,
    storage_health: Option<SharedStorageHealth>,
}

impl ApiServer {
    pub fn new(cfg: ApiConfig, kernel_cfg: KernelConfig) -> Self {
        Self {
            cfg,
            kernel_cfg,
            storage_health: None,
        }
    }

    /// Attach a storage-health snapshot shared with the witnessd main loop.
    /// When attached, `GET /status` (token-gated) serves the latest report.
    pub fn with_storage_health(mut self, snapshot: SharedStorageHealth) -> Self {
        self.storage_health = Some(snapshot);
        self
    }

    pub fn spawn(self) -> Result<ApiHandle> {
        // Firmware alignment: DEFAULT_TLS_REQUIRED = 1
        // Log a conformance alarm if running without TLS and not explicitly allowed.
        if (self.cfg.tls.is_none() || !self.cfg.tls.as_ref().is_some_and(|t| t.is_configured()))
            && !self.cfg.allow_insecure
        {
            log::warn!(
                "CONFORMANCE: API server starting WITHOUT TLS. \
                 This violates firmware policy DEFAULT_TLS_REQUIRED=1. \
                 Use --api-tls-cert and --api-tls-key to enable TLS, \
                 or --allow-insecure-api to suppress this warning."
            );
        }

        let configured_addr: SocketAddr = self.cfg.addr.parse()?;
        // Fail closed on off-host exposure. The event API terminates no TLS
        // in-process, so a non-loopback bind would put the bearer capability
        // token and event metadata on the wire in cleartext. Require an
        // explicit operator override before exposing it — mirroring the
        // break-glass server's validate_exposure. The loopback path (reached
        // behind a TLS reverse proxy or SSH tunnel) stays the default.
        let tls_configured = self.cfg.tls.as_ref().is_some_and(|t| t.is_configured());
        if !configured_addr.ip().is_loopback() && !tls_configured && !self.cfg.allow_insecure {
            return Err(anyhow!(
                "refusing to bind non-loopback address '{configured_addr}': the event API \
                 capability token would cross the network in cleartext. Bind a loopback address \
                 behind a TLS reverse proxy or SSH tunnel, or set WITNESS_API_ALLOW_INSECURE=1 to \
                 accept plaintext exposure explicitly."
            ));
        }
        let listener = TcpListener::bind(configured_addr)?;
        let addr = listener.local_addr()?;
        if configured_addr.ip().is_loopback() && !addr.ip().is_loopback() {
            return Err(anyhow!(
                "api configured for loopback address '{}', but bound to non-loopback address '{}'",
                configured_addr,
                addr
            ));
        }
        listener.set_nonblocking(true)?;

        let bucket = TimeBucket::now_10min()?;
        let mut token_mgr = CapabilityTokenManager::new(bucket)?;
        let token = token_mgr.token_hex();
        if let Some(path) = &self.cfg.token_path {
            write_token_file(path, &token)?;
        }

        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_thread = shutdown.clone();
        let cfg = self.cfg.clone();
        let kernel_cfg = self.kernel_cfg.clone();
        let storage_health = self.storage_health.clone();
        let token_path = cfg.token_path.clone();
        let join = std::thread::spawn(move || {
            if let Err(err) = run_api(
                listener,
                cfg,
                kernel_cfg,
                storage_health,
                &mut token_mgr,
                shutdown_thread,
            ) {
                log::error!("event api stopped: {}", err);
            }
        });

        Ok(ApiHandle {
            addr,
            token,
            token_path,
            shutdown,
            join: Some(join),
        })
    }
}

/// Hard bound on tracked failing IPs — mirrors `RATE_MAX_TRACKED_IPS`.
const AUTH_MAX_TRACKED_IPS: usize = 4096;

/// Per-IP auth failure tracker with exponential backoff.
/// Aligned with firmware's DEFAULT_AUTH_LOCKOUT_BASE_SEC / DEFAULT_AUTH_LOCKOUT_CAP_SEC.
struct AuthFailureTracker {
    entries: HashMap<std::net::IpAddr, AuthFailureEntry>,
    base_lockout_ms: u64,
    cap_lockout_ms: u64,
    max_attempts: u32,
}

struct AuthFailureEntry {
    /// Failures since last lockout (or since start).
    failure_count: u32,
    /// Number of times this IP has been locked out (drives exponential backoff).
    lockout_count: u32,
    /// Epoch ms when lockout expires.
    locked_until_ms: u64,
}

impl AuthFailureTracker {
    fn new() -> Self {
        Self {
            entries: HashMap::new(),
            base_lockout_ms: 2_000, // firmware: DEFAULT_AUTH_LOCKOUT_BASE_SEC = 2
            cap_lockout_ms: 300_000, // firmware: DEFAULT_AUTH_LOCKOUT_CAP_SEC = 300
            max_attempts: 5,        // firmware: DEFAULT_AUTH_MAX_ATTEMPTS = 5
        }
    }

    fn now_ms() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64
    }

    fn is_locked(&self, ip: &std::net::IpAddr) -> Option<u64> {
        if let Some(entry) = self.entries.get(ip) {
            let now = Self::now_ms();
            if entry.locked_until_ms > now {
                return Some((entry.locked_until_ms - now).div_ceil(1000));
            }
        }
        None
    }

    fn record_failure(&mut self, ip: std::net::IpAddr) {
        // Same hard cap the RateLimiter already has (docs/strategy/12, K6):
        // entries are added per distinct failing IP and previously removed
        // only on auth *success*, so a source cycling addresses grew this
        // map without bound. Sweep expired lockouts first; if rotation kept
        // everything live, evict the soonest-expiring entry — that client
        // merely restarts its failure count, which beats unbounded memory.
        if !self.entries.contains_key(&ip) && self.entries.len() >= AUTH_MAX_TRACKED_IPS {
            let now = Self::now_ms();
            self.entries.retain(|_, e| e.locked_until_ms > now);
            while self.entries.len() >= AUTH_MAX_TRACKED_IPS {
                let soonest = self
                    .entries
                    .iter()
                    .min_by_key(|(_, e)| e.locked_until_ms)
                    .map(|(ip, _)| *ip);
                match soonest {
                    Some(ip) => self.entries.remove(&ip),
                    None => break,
                };
            }
        }
        let entry = self.entries.entry(ip).or_insert(AuthFailureEntry {
            failure_count: 0,
            lockout_count: 0,
            locked_until_ms: 0,
        });
        entry.failure_count += 1;
        if entry.failure_count >= self.max_attempts {
            entry.lockout_count += 1;
            let lockout_ms = std::cmp::min(
                self.base_lockout_ms * 2u64.saturating_pow(entry.lockout_count.saturating_sub(1)),
                self.cap_lockout_ms,
            );
            entry.locked_until_ms = Self::now_ms() + lockout_ms;
            // Reset failure count so the next round requires max_attempts again
            entry.failure_count = 0;
        }
    }

    fn clear_on_success(&mut self, ip: &std::net::IpAddr) {
        self.entries.remove(ip);
    }
}

/// Per-IP fixed-window request limiter for everything except `/health`.
///
/// The auth-failure tracker above throttles clients that *fail* auth; this
/// caps how fast a client that *holds* the capability token can drive the
/// single-threaded API (each `/events` re-exports, `POST /verify` walks the
/// whole sealed log). A fixed one-minute window is deliberately simple: the
/// worst-case burst is 2× the configured rate at a window boundary.
struct RateLimiter {
    limit_per_minute: u32,
    windows: HashMap<std::net::IpAddr, RateWindow>,
}

struct RateWindow {
    window_start_ms: u64,
    count: u32,
}

const RATE_WINDOW_MS: u64 = 60_000;
/// Backstop against per-IP state growth from address-rotating clients.
const RATE_MAX_TRACKED_IPS: usize = 4096;

impl RateLimiter {
    fn new(limit_per_minute: u32) -> Self {
        Self {
            limit_per_minute,
            windows: HashMap::new(),
        }
    }

    /// Count one request. Returns `Some(retry_after_s)` when over the limit.
    fn check(&mut self, ip: std::net::IpAddr) -> Option<u64> {
        if self.limit_per_minute == 0 {
            return None;
        }
        let now = AuthFailureTracker::now_ms();
        if self.windows.len() >= RATE_MAX_TRACKED_IPS {
            self.windows
                .retain(|_, w| now.saturating_sub(w.window_start_ms) < RATE_WINDOW_MS);
            // Mass IP rotation can keep every tracked window active, so the
            // stale sweep alone doesn't bound the map. Evict the oldest
            // window to make the cap hard: the evicted client merely starts
            // a fresh count, which beats unbounded memory growth or turning
            // a full map into a lockout for new legitimate clients.
            while self.windows.len() >= RATE_MAX_TRACKED_IPS {
                let oldest = self
                    .windows
                    .iter()
                    .min_by_key(|(_, w)| w.window_start_ms)
                    .map(|(ip, _)| *ip);
                match oldest {
                    Some(ip) => self.windows.remove(&ip),
                    None => break,
                };
            }
        }
        let window = self.windows.entry(ip).or_insert(RateWindow {
            window_start_ms: now,
            count: 0,
        });
        if now.saturating_sub(window.window_start_ms) >= RATE_WINDOW_MS {
            window.window_start_ms = now;
            window.count = 0;
        }
        window.count = window.count.saturating_add(1);
        if window.count > self.limit_per_minute {
            let elapsed = now.saturating_sub(window.window_start_ms);
            Some((RATE_WINDOW_MS - elapsed).div_ceil(1000).max(1))
        } else {
            None
        }
    }
}

fn run_api(
    listener: TcpListener,
    cfg: ApiConfig,
    kernel_cfg: KernelConfig,
    storage_health: Option<SharedStorageHealth>,
    token_mgr: &mut CapabilityTokenManager,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let mut kernel = Kernel::open(&kernel_cfg)?;
    let expected_ruleset_hash = kernel_cfg.ruleset_hash;
    let kernel_version = kernel_cfg.kernel_version.clone();
    let retention = kernel_cfg.retention;
    let mut auth_tracker = AuthFailureTracker::new();
    let mut rate_limiter = RateLimiter::new(cfg.rate_limit_per_minute);
    // Most recent POST /verify outcome, surfaced in /digest so consumers
    // get chain status without re-running verification on every request.
    let mut last_verify: Option<VerifyReport> = None;
    loop {
        if shutdown.load(Ordering::SeqCst) {
            break;
        }
        match listener.accept() {
            Ok((stream, _)) => {
                if let Err(err) = handle_connection(
                    stream,
                    &mut kernel,
                    &cfg,
                    token_mgr,
                    expected_ruleset_hash,
                    &kernel_version,
                    storage_health.as_ref(),
                    retention,
                    &mut auth_tracker,
                    &mut rate_limiter,
                    &mut last_verify,
                ) {
                    log::warn!("event api request rejected: {}", err);
                }
            }
            Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
            Err(err) => return Err(err.into()),
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn handle_connection(
    mut stream: TcpStream,
    kernel: &mut Kernel,
    cfg: &ApiConfig,
    token_mgr: &mut CapabilityTokenManager,
    expected_ruleset_hash: [u8; 32],
    kernel_version: &str,
    storage_health: Option<&SharedStorageHealth>,
    retention: Duration,
    auth_tracker: &mut AuthFailureTracker,
    rate_limiter: &mut RateLimiter,
    last_verify: &mut Option<VerifyReport>,
) -> Result<()> {
    let peer = stream.peer_addr()?;
    let local = stream.local_addr()?;
    if local.ip().is_loopback() && !peer.ip().is_loopback() {
        write_json_response(&mut stream, 403, r#"{"error":"forbidden"}"#)?;
        return Ok(());
    }

    // Check auth lockout before processing request
    if let Some(retry_after) = auth_tracker.is_locked(&peer.ip()) {
        let body = format!(r#"{{"error":"auth_locked","retry_after":{}}}"#, retry_after);
        write_json_response(&mut stream, 429, &body)?;
        return Ok(());
    }

    let request = read_request(&mut stream)?;
    match (request.method.as_str(), request.path.as_str()) {
        ("GET", "/health") => {
            write_json_response(&mut stream, 200, r#"{"status":"ok"}"#)?;
            return Ok(());
        }
        // `/status` (storage health) is token-gated like `/events`: storage
        // metrics are operational metadata and stay behind the capability
        // token (Invariant III posture — only `/health` liveness is open).
        ("GET", "/events")
        | ("GET", "/events/latest")
        | ("GET", "/digest")
        | ("GET", "/status")
        | ("GET", "/export/bundle") => {}
        // POST /verify carries no body — verification has no parameters and
        // request parsing stays at the 8 KB header-only surface.
        ("POST", "/verify") => {
            if request
                .headers
                .get("content-length")
                .and_then(|v| v.parse::<usize>().ok())
                .is_some_and(|len| len > 0)
            {
                write_json_response(&mut stream, 400, r#"{"error":"body_not_allowed"}"#)?;
                return Ok(());
            }
        }
        (_, "/health")
        | (_, "/events")
        | (_, "/events/latest")
        | (_, "/digest")
        | (_, "/verify")
        | (_, "/status")
        | (_, "/export/bundle") => {
            write_json_response(&mut stream, 405, r#"{"error":"method_not_allowed"}"#)?;
            return Ok(());
        }
        _ => {
            write_json_response(&mut stream, 404, r#"{"error":"not_found"}"#)?;
            return Ok(());
        }
    }

    // Rate limit everything past `/health`, before any token work — the
    // constant-time compare and the handlers below are all budgeted.
    if let Some(retry_after) = rate_limiter.check(peer.ip()) {
        let body = format!(
            r#"{{"error":"rate_limited","retry_after":{}}}"#,
            retry_after
        );
        write_json_response(&mut stream, 429, &body)?;
        return Ok(());
    }

    if request.has_query_token() {
        write_json_response(
            &mut stream,
            400,
            r#"{"error":"token_query_param_not_allowed"}"#,
        )?;
        return Ok(());
    }

    let token = match request.bearer_token() {
        Some(token) => token,
        None => {
            auth_tracker.record_failure(peer.ip());
            write_json_response(&mut stream, 401, r#"{"error":"missing_token"}"#)?;
            return Ok(());
        }
    };

    let now_bucket = TimeBucket::now_10min()?;
    if token_mgr.rotate_if_needed(now_bucket)? {
        if let Some(path) = &cfg.token_path {
            write_token_file(path, &token_mgr.token_hex())?;
        } else {
            // SECURITY: Never log the token value, even to stderr.
            // Token material in logs can be captured by log aggregators,
            // process monitors, or shell history. Require explicit file path.
            log::warn!(
                "event api capability token rotated but WITNESS_API_TOKEN_PATH not set; \
                 new token is NOT accessible — configure the path to persist tokens"
            );
        }
    }

    if let Err(err) = token_mgr.validate(&token, now_bucket) {
        auth_tracker.record_failure(peer.ip());
        write_json_response(&mut stream, 401, r#"{"error":"invalid_token"}"#)?;
        return Err(err);
    }

    // Successful auth — clear any failure history for this IP
    auth_tracker.clear_on_success(&peer.ip());

    if request.path == "/status" {
        let report = storage_health
            .and_then(|shared| shared.read().ok().and_then(|guard| guard.as_ref().cloned()));
        match report {
            Some(report) => {
                #[derive(serde::Serialize)]
                struct StatusResponse<'a> {
                    kernel_version: &'a str,
                    #[serde(flatten)]
                    report: crate::storage_health::StorageHealthReport,
                }
                let payload = serde_json::to_vec(&StatusResponse {
                    kernel_version,
                    report,
                })?;
                write_response(&mut stream, 200, "application/json", &payload)?;
            }
            // No snapshot yet (monitoring disabled, or first sample pending):
            // 404 lets clients distinguish "not available" from an error.
            None => write_json_response(&mut stream, 404, r#"{"error":"not_available"}"#)?,
        }
        return Ok(());
    }

    if request.path == "/verify" {
        // Synchronous and read-only against the kernel's open connection.
        // The log holds coarse, deduplicated events, so a full pass is
        // cheap — but it does block this single-threaded API briefly.
        let report = run_full_verify(&kernel.conn, None, None, SignatureMode::Compat, |_| {})?;
        *last_verify = Some(report.clone());
        let payload = serde_json::to_vec(&report)?;
        write_response(&mut stream, 200, "application/json", &payload)?;
        return Ok(());
    }

    if request.path == "/export/bundle" {
        // One-click "download my events": the full signed ExportBundle
        // (artifact + chained receipt + verifying keys) so the file is
        // self-verifying offline. Optional window: ?last=24h or
        // ?start=<epoch_s>&end=<epoch_s>, aligned outward to bucket
        // boundaries. Receipt is labeled `api` (credential = capability
        // token) and records the window — same audit posture as /events.
        let window = match parse_window_query(&request.raw_path) {
            Ok(window) => window,
            Err(e) => {
                let body = serde_json::json!({"error": "bad_window", "detail": e.to_string()});
                write_json_response(&mut stream, 400, &body.to_string())?;
                return Ok(());
            }
        };
        let bundle = kernel.export_events_bundle_for_api(
            expected_ruleset_hash,
            ExportOptions {
                window,
                ..cfg.export_options
            },
        )?;
        let payload = serde_json::to_vec(&bundle)?;
        let filename = format!(
            "securacv-events-{}.json",
            TimeBucket::now_10min()?.start_epoch_s
        );
        write_download_response(&mut stream, "application/json", &payload, &filename)?;
        return Ok(());
    }

    let artifact = kernel.export_events_for_api(expected_ruleset_hash, cfg.export_options)?;

    if request.path == "/digest" {
        let digest = build_digest(&artifact, retention, last_verify.clone())?;
        let payload = serde_json::to_vec(&digest)?;
        write_response(&mut stream, 200, "application/json", &payload)?;
        return Ok(());
    }

    if request.path == "/events/latest" {
        if let Some(event) = latest_event(&artifact) {
            let payload = serde_json::to_vec(event)?;
            write_response(&mut stream, 200, "application/json", &payload)?;
        } else {
            write_json_response(&mut stream, 404, r#"{"error":"no_events"}"#)?;
        }
        return Ok(());
    }

    let payload = serde_json::to_vec(&artifact)?;
    write_response(&mut stream, 200, "application/json", &payload)?;
    Ok(())
}

/// Rolling 24h digest of the privacy-preserving event log.
///
/// Built exclusively from the already-exported artifact (privacy-filtered,
/// bucket-coarsened by `export_events_for_api`), so it can never carry more
/// than the export path does. Time-of-day breakdown uses four 6-hour UTC
/// periods — strictly coarser than the 10-minute buckets it is derived from
/// (invariants §5 metadata minimization).
#[derive(Debug, Clone, Serialize)]
pub struct DigestPayload {
    pub total_events: u64,
    pub per_zone: BTreeMap<String, u64>,
    pub per_period: BTreeMap<String, u64>,
    pub event_types: BTreeMap<String, u64>,
    pub window_secs: u64,
    pub retention_days: u64,
    pub generated_at_bucket_start: u64,
    pub generated_at_bucket_size: u32,
    /// Most recent POST /verify outcome, if verification has run.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_verify: Option<VerifyReport>,
}

const DIGEST_WINDOW_SECS: u64 = 24 * 60 * 60;

fn build_digest(
    artifact: &ExportArtifact,
    retention: Duration,
    last_verify: Option<VerifyReport>,
) -> Result<DigestPayload> {
    let now_bucket = TimeBucket::now_10min()?;
    let cutoff = now_bucket
        .start_epoch_s
        .saturating_sub(DIGEST_WINDOW_SECS.saturating_sub(u64::from(now_bucket.size_s)));

    let mut total_events = 0u64;
    let mut per_zone: BTreeMap<String, u64> = BTreeMap::new();
    let mut per_period: BTreeMap<String, u64> = BTreeMap::new();
    let mut event_types: BTreeMap<String, u64> = BTreeMap::new();

    for batch in &artifact.batches {
        for bucket in &batch.buckets {
            for event in &bucket.events {
                if event.time_bucket.start_epoch_s < cutoff {
                    continue;
                }
                total_events += 1;
                *per_zone.entry(event.zone_id.clone()).or_default() += 1;
                *per_period
                    .entry(period_of_day(event.time_bucket.start_epoch_s).to_string())
                    .or_default() += 1;
                *event_types
                    .entry(format!("{:?}", event.event_type))
                    .or_default() += 1;
            }
        }
    }

    Ok(DigestPayload {
        total_events,
        per_zone,
        per_period,
        event_types,
        window_secs: DIGEST_WINDOW_SECS,
        retention_days: retention.as_secs() / 86_400,
        generated_at_bucket_start: now_bucket.start_epoch_s,
        generated_at_bucket_size: now_bucket.size_s,
        last_verify,
    })
}

fn period_of_day(epoch_s: u64) -> &'static str {
    match (epoch_s % 86_400) / 3_600 {
        0..=5 => "night",
        6..=11 => "morning",
        12..=17 => "afternoon",
        _ => "evening",
    }
}

fn read_request(stream: &mut TcpStream) -> Result<HttpRequest> {
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    // A response write can block forever against a peer that stops reading;
    // on this single-threaded accept loop that wedges every endpoint
    // including /health (docs/strategy/12, K6).
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    let mut buf = [0u8; 1024];
    let mut data = Vec::new();
    loop {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            break;
        }
        data.extend_from_slice(&buf[..n]);
        if data.len() > MAX_REQUEST_BYTES {
            return Err(anyhow!("request too large"));
        }
        if data.windows(4).any(|w| w == b"\r\n\r\n") {
            break;
        }
    }
    let text = String::from_utf8_lossy(&data);
    let mut lines = text.split("\r\n");
    let request_line = lines.next().ok_or_else(|| anyhow!("empty request"))?;
    let mut parts = request_line.split_whitespace();
    let method = parts.next().ok_or_else(|| anyhow!("missing method"))?;
    let raw_path = parts.next().ok_or_else(|| anyhow!("missing path"))?;
    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            break;
        }
        if let Some((k, v)) = line.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }
    let path = raw_path.split('?').next().unwrap_or(raw_path).to_string();
    Ok(HttpRequest {
        method: method.to_string(),
        path,
        headers,
        raw_path: raw_path.to_string(),
    })
}

fn write_json_response(stream: &mut TcpStream, status: u16, body: &str) -> Result<()> {
    write_response(stream, status, "application/json", body.as_bytes())
}

/// Parse the optional export window from `/export/bundle` query parameters:
/// `?last=24h` (trailing duration) or `?start=<epoch_s>&end=<epoch_s>`.
/// Bounds are aligned outward to bucket boundaries; the filename-safe
/// duration grammar is the same one `export_events --last` accepts.
fn parse_window_query(raw_path: &str) -> Result<Option<crate::ExportWindow>> {
    let query = match raw_path.split('?').nth(1) {
        Some(query) if !query.is_empty() => query,
        _ => return Ok(None),
    };
    let mut last: Option<&str> = None;
    let mut start: Option<&str> = None;
    let mut end: Option<&str> = None;
    for pair in query.split('&') {
        match pair.split_once('=') {
            Some(("last", value)) => last = Some(value),
            Some(("start", value)) => start = Some(value),
            Some(("end", value)) => end = Some(value),
            _ => return Err(anyhow!("unknown query parameter (expected last|start&end)")),
        }
    }
    match (last, start, end) {
        (Some(last), None, None) => Ok(Some(crate::ExportWindow::last(crate::parse_duration_s(
            last,
        )?)?)),
        (None, Some(start), Some(end)) => {
            let start: u64 = start.parse().map_err(|_| anyhow!("invalid start"))?;
            let end: u64 = end.parse().map_err(|_| anyhow!("invalid end"))?;
            Ok(Some(crate::ExportWindow::aligned(start, end)?))
        }
        (None, None, None) => Ok(None),
        _ => Err(anyhow!("use either ?last=<dur> or ?start=&end=, not both")),
    }
}

/// As [`write_response`] but marked as a browser download. `filename` is
/// produced internally (timestamp pattern) — never derived from request input.
fn write_download_response(
    stream: &mut TcpStream,
    content_type: &str,
    body: &[u8],
    filename: &str,
) -> Result<()> {
    let header = format!(
        "HTTP/1.1 200 OK\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {len}\r\n\
         Content-Disposition: attachment; filename=\"{filename}\"\r\n\
         Cache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\n\
         X-Frame-Options: DENY\r\n\
         Referrer-Policy: no-referrer\r\n\
         Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n\
         \r\n",
        content_type = content_type,
        len = body.len(),
        filename = filename
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body)?;
    Ok(())
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> Result<()> {
    let status_line = match status {
        200 => "HTTP/1.1 200 OK",
        400 => "HTTP/1.1 400 Bad Request",
        401 => "HTTP/1.1 401 Unauthorized",
        403 => "HTTP/1.1 403 Forbidden",
        404 => "HTTP/1.1 404 Not Found",
        405 => "HTTP/1.1 405 Method Not Allowed",
        429 => "HTTP/1.1 429 Too Many Requests",
        _ => "HTTP/1.1 500 Internal Server Error",
    };
    // Security headers aligned with canary-vision/device-api/middleware/security-headers.js
    let header = format!(
        "{status_line}\r\n\
         Content-Type: {content_type}\r\n\
         Content-Length: {len}\r\n\
         Cache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\n\
         X-Frame-Options: DENY\r\n\
         Referrer-Policy: no-referrer\r\n\
         Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n\
         \r\n",
        status_line = status_line,
        content_type = content_type,
        len = body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body)?;
    Ok(())
}

#[derive(Debug)]
struct HttpRequest {
    method: String,
    path: String,
    headers: HashMap<String, String>,
    raw_path: String,
}

impl HttpRequest {
    fn bearer_token(&self) -> Option<String> {
        if let Some(value) = self.headers.get("authorization") {
            let parts: Vec<&str> = value.split_whitespace().collect();
            if parts.len() == 2 && parts[0].eq_ignore_ascii_case("bearer") {
                return Some(parts[1].to_string());
            }
        }
        if let Some(value) = self.headers.get("x-witness-token") {
            if !value.is_empty() {
                return Some(value.to_string());
            }
        }
        None
    }

    fn has_query_token(&self) -> bool {
        if let Some(query) = self.raw_path.split('?').nth(1) {
            for pair in query.split('&') {
                if let Some((k, _)) = pair.split_once('=') {
                    if k == "token" {
                        return true;
                    }
                }
            }
        }
        false
    }
}

fn write_token_file(path: &Path, token: &str) -> Result<()> {
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(path)?;
        f.write_all(format!("{token}\n").as_bytes())?;
    }
    #[cfg(not(unix))]
    {
        std::fs::write(path, format!("{token}\n"))?;
    }
    Ok(())
}

fn parse_hex32(value: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(value)?;
    if bytes.len() != 32 {
        return Err(anyhow!("token must be 32 bytes"));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn latest_event(artifact: &crate::ExportArtifact) -> Option<&crate::ExportEvent> {
    artifact
        .batches
        .iter()
        .flat_map(|batch| &batch.buckets)
        .flat_map(|bucket| &bucket.events)
        .last()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rate_limiter_map_is_hard_bounded_under_ip_rotation() {
        // Every request from a distinct IP inside one window: the stale sweep
        // frees nothing, so the oldest-entry eviction must hold the line.
        let mut limiter = RateLimiter::new(10);
        for i in 0..(RATE_MAX_TRACKED_IPS as u128 + 500) {
            let ip = std::net::IpAddr::V6(std::net::Ipv6Addr::from(0xfd00_0000_0000_0000_u128 + i));
            let _ = limiter.check(ip);
            assert!(
                limiter.windows.len() <= RATE_MAX_TRACKED_IPS,
                "tracked-IP map exceeded its cap at request {i}"
            );
        }
    }

    #[test]
    fn auth_failure_tracker_is_hard_bounded_under_ip_rotation() {
        // One failed auth from each of many distinct IPs. Entries were
        // previously removed only on auth success, so rotation grew the map
        // without bound; the cap must hold even though nothing ever succeeds.
        let mut tracker = AuthFailureTracker::new();
        for i in 0..(AUTH_MAX_TRACKED_IPS as u128 + 500) {
            let ip = std::net::IpAddr::V6(std::net::Ipv6Addr::from(0xfd00_0000_0000_1000_u128 + i));
            tracker.record_failure(ip);
            assert!(
                tracker.entries.len() <= AUTH_MAX_TRACKED_IPS,
                "auth-failure map exceeded its cap at request {i}"
            );
        }
    }

    #[test]
    #[cfg(unix)]
    fn token_file_created_with_restricted_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test-token");
        write_token_file(&path, "secret-token-value").unwrap();

        let metadata = std::fs::metadata(&path).unwrap();
        let mode = metadata.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "token file should be created with mode 0600");

        let contents = std::fs::read_to_string(&path).unwrap();
        assert_eq!(contents, "secret-token-value\n");
    }
}
