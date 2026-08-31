use crate::storage_health::SharedStorageHealth;
use crate::verify_runner::VerifyReport;
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

const MAX_REQUEST_BYTES: usize = 8192;

/// TLS material (PEM cert chain + private key) for the token-bearing HTTP
/// surfaces (event API, break-glass).
///
/// What configuring it *means* depends on the build:
/// - With the `api-tls` feature compiled in, the material is terminated
///   **in-process** by rustls — every accepted connection is served through a
///   TLS session, and a non-loopback bind becomes acceptable because the
///   capability token never crosses the network in cleartext.
/// - Without the feature, the break-glass server treats it as an *attestation*
///   that an external terminator fronts the socket (see
///   `break_glass::server::validate_exposure`), and the event API refuses it
///   outright at startup — material a build cannot terminate must not look
///   like protection.
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

    /// Load TLS materials from the `WITNESS_API_TLS_CERT` /
    /// `WITNESS_API_TLS_KEY` environment variables (paths to PEM files).
    /// Both-or-neither: a lone half is an error rather than a silent fall
    /// back to plaintext.
    pub fn from_env() -> Result<Self> {
        match (
            std::env::var_os(API_TLS_CERT_ENV),
            std::env::var_os(API_TLS_KEY_ENV),
        ) {
            (Some(cert), Some(key)) => Self::load(Path::new(&cert), Path::new(&key)),
            (None, None) => Ok(Self::default()),
            _ => Err(anyhow!(
                "{API_TLS_CERT_ENV} and {API_TLS_KEY_ENV} must be set together"
            )),
        }
    }

    pub fn is_configured(&self) -> bool {
        self.cert_pem.is_some() && self.key_pem.is_some()
    }
}

/// Environment variable naming a PEM certificate-chain file for the event API.
pub const API_TLS_CERT_ENV: &str = "WITNESS_API_TLS_CERT";
/// Environment variable naming a PEM private-key file for the event API.
pub const API_TLS_KEY_ENV: &str = "WITNESS_API_TLS_KEY";

/// A bidirectional stream a request can be served over (plain `TcpStream` or
/// an in-process TLS session).
pub(crate) trait ReadWrite: Read + Write {}
impl<T: Read + Write> ReadWrite for T {}

/// Converts an accepted `TcpStream` into a served stream (identity for
/// plaintext, a rustls session when `api-tls` is active). The wrap only
/// constructs the session — the TLS handshake itself runs lazily on the first
/// read/write through the same socket, so it is bounded by the read/write
/// timeouts set on the `TcpStream` before wrapping.
pub(crate) type StreamWrap =
    Arc<dyn Fn(TcpStream) -> std::io::Result<Box<dyn ReadWrite + Send>> + Send + Sync>;

/// Identity wrap: serve the TCP stream directly (no TLS).
pub(crate) fn plain_wrap() -> StreamWrap {
    Arc::new(|tcp| Ok(Box::new(tcp) as Box<dyn ReadWrite + Send>))
}

/// In-process TLS for the token-bearing HTTP surfaces (feature `api-tls`).
/// The capability token crosses the network in plaintext otherwise, which is
/// why the exposure gates below only relax for TLS that is actually ACTIVE.
#[cfg(feature = "api-tls")]
pub(crate) mod tls {
    use super::{ReadWrite, StreamWrap};
    use anyhow::{anyhow, Context, Result};
    use rustls::ServerConfig;
    use std::net::TcpStream;
    use std::sync::Arc;

    /// Build a rustls server config (no client auth) from in-memory PEM
    /// bytes — [`super::ApiTlsConfig`] holds material as bytes, not paths.
    pub(crate) fn server_config_from_pem(
        cert_pem: &[u8],
        key_pem: &[u8],
    ) -> Result<Arc<ServerConfig>> {
        let certs = rustls_pemfile::certs(&mut &cert_pem[..])
            .collect::<std::result::Result<Vec<_>, _>>()
            .context("parsing TLS certificate PEM")?;
        if certs.is_empty() {
            return Err(anyhow!("no certificates found in TLS cert PEM"));
        }
        let key = rustls_pemfile::private_key(&mut &key_pem[..])
            .context("parsing TLS key PEM")?
            .ok_or_else(|| anyhow!("no private key found in TLS key PEM"))?;
        let config = ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(certs, key)
            .context("building TLS server config")?;
        Ok(Arc::new(config))
    }

    /// Wrap every accepted connection in an in-process rustls session.
    pub(crate) fn tls_wrap(config: Arc<ServerConfig>) -> StreamWrap {
        Arc::new(move |tcp: TcpStream| {
            let conn = rustls::ServerConnection::new(Arc::clone(&config))
                .map_err(std::io::Error::other)?;
            Ok(Box::new(rustls::StreamOwned::new(conn, tcp)) as Box<dyn ReadWrite + Send>)
        })
    }

    /// Resolve an [`super::ApiTlsConfig`] into a server config, or `None`
    /// when no material is configured. Mismatched halves are an error.
    pub(crate) fn resolve(cfg: &super::ApiTlsConfig) -> Result<Option<Arc<ServerConfig>>> {
        match (&cfg.cert_pem, &cfg.key_pem) {
            (Some(cert), Some(key)) => Ok(Some(server_config_from_pem(cert, key)?)),
            (None, None) => Ok(None),
            _ => Err(anyhow!("TLS cert and key must be configured together")),
        }
    }
}

#[derive(Clone, Debug)]
pub struct ApiConfig {
    pub addr: String,
    pub export_options: ExportOptions,
    pub token_path: Option<PathBuf>,
    /// Allow plaintext HTTP without TLS. Logs a conformance alarm if true.
    pub allow_insecure: bool,
    /// Max requests per client IP per minute on every endpoint except
    /// `/health` (fixed window). Applies before token validation, so a
    /// stolen-or-not client cannot hammer `/events` or `POST /verify`
    /// (which walks the whole sealed log). 0 disables the limit.
    pub rate_limit_per_minute: u32,
    /// In-process TLS material for the event API socket. With the `api-tls`
    /// feature compiled in, a configured cert/key means every accepted
    /// connection is served through an in-process rustls session (and a
    /// non-loopback bind no longer needs the insecure override). Without the
    /// feature, configuring this is a startup error.
    pub tls: ApiTlsConfig,
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
            allow_insecure: false,
            rate_limit_per_minute: DEFAULT_API_RATE_LIMIT_PER_MINUTE,
            tls: ApiTlsConfig::default(),
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
        // Resolve in-process TLS up front so bad material fails startup, not
        // the first connection. `tls_config` is Some only when this BUILD can
        // actually terminate it — the exposure gate below keys off that, never
        // off configuration alone.
        #[cfg(feature = "api-tls")]
        let tls_config = tls::resolve(&self.cfg.tls)?;
        #[cfg(not(feature = "api-tls"))]
        let tls_config: Option<()> = match (&self.cfg.tls.cert_pem, &self.cfg.tls.key_pem) {
            (None, None) => None,
            // Material a build cannot terminate must not look like protection.
            _ => {
                return Err(anyhow!(
                    "API TLS cert/key configured, but this build has no in-process TLS \
                     (feature `api-tls` is not compiled in). Rebuild with --features api-tls, \
                     or remove the cert/key and keep the loopback bind behind a TLS reverse \
                     proxy or SSH tunnel."
                ))
            }
        };
        let tls_active = tls_config.is_some();

        // Firmware alignment: DEFAULT_TLS_REQUIRED = 1. When in-process TLS is
        // not active, the honest remedies are: keep the default loopback bind
        // and put a TLS reverse proxy or SSH tunnel in front of it, provide
        // cert/key to an `api-tls` build, or accept plaintext exposure
        // explicitly with WITNESS_API_ALLOW_INSECURE=1.
        if !tls_active && !self.cfg.allow_insecure {
            log::warn!(
                "CONFORMANCE: event API serves plaintext HTTP (in-process TLS not \
                 active; firmware policy DEFAULT_TLS_REQUIRED=1). Keep the bind \
                 loopback behind a TLS reverse proxy or SSH tunnel, configure \
                 cert/key on an `api-tls` build, or set \
                 WITNESS_API_ALLOW_INSECURE=1 to accept plaintext exposure."
            );
        }
        if tls_active {
            log::info!("event API terminating TLS in-process");
        }

        let configured_addr: SocketAddr = self.cfg.addr.parse()?;
        // Fail closed on off-host exposure: unless in-process TLS actually
        // wraps the accepted sockets, a non-loopback bind puts the bearer
        // capability token on the wire in cleartext, so it needs the explicit
        // operator override. Gated on TLS being ACTIVE (feature compiled in
        // AND material loaded), not on config being present. Mirrors the
        // break-glass server's validate_exposure; the loopback path (behind a
        // TLS reverse proxy or SSH tunnel) is the default.
        if !configured_addr.ip().is_loopback() && !tls_active && !self.cfg.allow_insecure {
            return Err(anyhow!(
                "refusing to bind non-loopback address '{configured_addr}': the event API \
                 capability token would cross the network in cleartext. Terminate TLS \
                 in-process (build with --features api-tls and configure cert/key), bind a \
                 loopback address behind a TLS reverse proxy or SSH tunnel, or set \
                 WITNESS_API_ALLOW_INSECURE=1 to accept plaintext exposure explicitly."
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

        #[cfg(feature = "api-tls")]
        let wrap: StreamWrap = match tls_config {
            Some(config) => tls::tls_wrap(config),
            None => plain_wrap(),
        };
        #[cfg(not(feature = "api-tls"))]
        let wrap: StreamWrap = plain_wrap();

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
                wrap,
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

#[allow(clippy::too_many_arguments)]
fn run_api(
    listener: TcpListener,
    cfg: ApiConfig,
    kernel_cfg: KernelConfig,
    storage_health: Option<SharedStorageHealth>,
    token_mgr: &mut CapabilityTokenManager,
    shutdown: Arc<AtomicBool>,
    wrap: StreamWrap,
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
                    &wrap,
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
    stream: TcpStream,
    wrap: &StreamWrap,
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
    // Addresses and socket timeouts are taken on the TCP stream BEFORE any
    // TLS wrap: rustls performs its handshake lazily on the first read/write
    // through the same socket, so these bounds cover the handshake too — a
    // client that stalls mid-handshake cannot wedge this single-threaded
    // accept loop (same K6 reasoning as the write timeout below).
    let peer = stream.peer_addr()?;
    let local = stream.local_addr()?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    // A response write can block forever against a peer that stops reading;
    // on this single-threaded accept loop that wedges every endpoint
    // including /health (docs/strategy/12, K6).
    stream.set_write_timeout(Some(Duration::from_secs(2)))?;
    let mut stream = wrap(stream)?;
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
        // `/api/sealed-log` (the checkpoint-anchored chain tail for read-only
        // verifiers) is token-gated like `/events`. It takes NO query
        // parameters — the sealed log is non-queryable (Invariant VII), so a
        // query string is ignored the same way `/events` ignores one.
        ("GET", "/events")
        | ("GET", "/events/latest")
        | ("GET", "/digest")
        | ("GET", "/status")
        | ("GET", "/export/bundle")
        | ("GET", "/api/sealed-log") => {}
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
        | (_, "/export/bundle")
        | (_, "/api/sealed-log") => {
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
        // Route through the kernel so a configured high-water-mark is checked
        // here too (mirrors witnessd's boot verify) — a rolled-back log fails
        // closed rather than reporting valid.
        let report = kernel.verify_sealed_log()?;
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

    if request.path == "/api/sealed-log" {
        // The checkpoint-anchored sealed-log tail as a self-contained
        // document (verifying key + entries exactly as stored) for read-only
        // chain verifiers such as the Witness Wall's `tvos/witness-core`.
        // A rotation-crossing tail is never served — the walk re-anchors at
        // the newest rotation record so a single-key verifier verifies
        // everything it receives (see Kernel::sealed_log_document).
        //
        // Deliberate, and different from `/export/bundle`: the payloads are
        // the STORED bytes, so nothing can be redacted from them — not even
        // `correlation_token`, which the export path's reshaped rows strip —
        // because any byte removed breaks the entry-hash walk that is this
        // endpoint's entire purpose. The capability token therefore reads
        // the chain as the chain, and redaction remains the export lane's
        // job. No query parameters exist for this route (Invariant VII,
        // non-queryable): any query string is ignored, apart from the
        // `?token=` rejection above.
        let doc = kernel.sealed_log_document()?;
        let payload = serde_json::to_vec(&doc)?;
        write_response(&mut stream, 200, "application/json", &payload)?;
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

/// Read and parse one HTTP request (header section only). Generic over the
/// stream so it serves both plaintext and in-process-TLS connections; the
/// socket timeouts bounding this read are set in `handle_connection` before
/// any TLS wrap.
fn read_request<S: Read>(stream: &mut S) -> Result<HttpRequest> {
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

fn write_json_response<S: Write>(stream: &mut S, status: u16, body: &str) -> Result<()> {
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
fn write_download_response<S: Write>(
    stream: &mut S,
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
    // Flush matters under TLS: buffered records must reach the socket before
    // the connection is dropped (no-op for a plain TcpStream).
    stream.flush()?;
    Ok(())
}

fn write_response<S: Write>(
    stream: &mut S,
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
    // Flush matters under TLS: buffered records must reach the socket before
    // the connection is dropped (no-op for a plain TcpStream).
    stream.flush()?;
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

    // ---------------------------------------------------------------
    // GET /api/sealed-log — the checkpoint-anchored chain tail served
    // to read-only verifiers. The harness mirrors the crate's API
    // integration tests (tests/api_export_bundle.rs): a real Kernel on
    // a temp DB, a spawned ApiServer, raw HTTP over loopback.
    // ---------------------------------------------------------------

    use crate::crypto::signatures::{
        domain_separated_hash, SignatureKeys, DOMAIN_SEALED_LOG_ENTRY,
    };
    use crate::{
        CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
        SealedLogDocument, TimeBucket, ZonePolicy,
    };
    use anyhow::{anyhow, Result};
    use std::net::TcpStream;

    fn sealed_log_kernel_config(db_path: &std::path::Path) -> KernelConfig {
        KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        }
    }

    fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) -> Result<()> {
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        kernel.append_event_checked(
            &desc,
            CandidateEvent {
                event_type: EventType::BoundaryCrossingObjectLarge,
                time_bucket: TimeBucket::now(600)?,
                zone_id: zone.to_string(),
                confidence: 0.5,
                correlation_token: None,
                attestation: None,
            },
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        Ok(())
    }

    /// Append one correctly chained and signed row whose `payload_json` is
    /// the EXACT given string — spacing, key order, tabs and all — bypassing
    /// serde so the byte-verbatim serving contract is actually exercised.
    fn plant_verbatim_payload(kernel: &Kernel, cfg: &KernelConfig, payload: &str) -> Result<i64> {
        let signing_key = crate::signing_key_from_seed(&cfg.device_key_seed)?;
        let keys = SignatureKeys::new(&signing_key);
        let prev = kernel.last_event_hash_or_checkpoint_head()?;
        let entry_hash = crate::log::hash_entry(&prev, payload.as_bytes());
        let signatures = crate::log::sign_entry(&keys, &entry_hash, DOMAIN_SEALED_LOG_ENTRY)?;
        kernel.conn.execute(
            "INSERT INTO sealed_events(created_at, payload_json, prev_hash, entry_hash, signature) \
             VALUES (?1, ?2, ?3, ?4, ?5)",
            rusqlite::params![
                0i64,
                payload,
                prev.to_vec(),
                entry_hash.to_vec(),
                signatures.ed25519_signature,
            ],
        )?;
        Ok(kernel.conn.last_insert_rowid())
    }

    struct SealedLogTestApi {
        _dir: tempfile::TempDir,
        api_handle: Option<ApiHandle>,
    }

    impl SealedLogTestApi {
        /// Spawn the API server over a kernel DB prepared by `prepare`.
        fn spawn(prepare: impl FnOnce(&mut Kernel, &KernelConfig) -> Result<()>) -> Result<Self> {
            let dir = tempfile::tempdir()?;
            let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
            let mut kernel = Kernel::open(&cfg)?;
            prepare(&mut kernel, &cfg)?;
            drop(kernel);

            let api_config = ApiConfig {
                addr: "127.0.0.1:0".to_string(),
                ..ApiConfig::default()
            };
            let api_handle = ApiServer::new(api_config, cfg).spawn()?;
            Ok(Self {
                _dir: dir,
                api_handle: Some(api_handle),
            })
        }

        fn handle(&self) -> &ApiHandle {
            self.api_handle
                .as_ref()
                .expect("test API handle should be initialized")
        }

        fn request(&self, method: &str, path: &str, with_token: bool) -> Result<(String, String)> {
            let mut stream = TcpStream::connect(self.handle().addr)?;
            let token_header = if with_token {
                format!("X-Witness-Token: {}\r\n", self.handle().token)
            } else {
                String::new()
            };
            let request =
                format!("{method} {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}\r\n");
            stream.write_all(request.as_bytes())?;
            let mut response = String::new();
            stream.read_to_string(&mut response)?;
            let mut parts = response.splitn(2, "\r\n\r\n");
            let headers = parts.next().unwrap_or("").to_string();
            let body = parts.next().unwrap_or("").to_string();
            Ok((headers, body))
        }

        fn get(&self, path: &str, with_token: bool) -> Result<(String, String)> {
            self.request("GET", path, with_token)
        }
    }

    impl Drop for SealedLogTestApi {
        fn drop(&mut self) {
            if let Some(handle) = self.api_handle.take() {
                if let Err(err) = handle.stop() {
                    eprintln!("Failed to stop API server: {err}");
                }
            }
        }
    }

    /// Walk a served document exactly the way a read-only verifier does
    /// (`tvos/witness-core::verify_chain`), but using the KERNEL's own
    /// hashing and domain separation — proving the served fields reproduce
    /// the stored chain. Returns how many entries verified before the walk
    /// stopped.
    fn walk_with_kernel_hashing(doc: &SealedLogDocument) -> Result<u64> {
        use ed25519_dalek::{Signature, Verifier, VerifyingKey};
        let key_bytes: [u8; 32] = hex::decode(&doc.verifying_key)?
            .try_into()
            .map_err(|_| anyhow!("verifying_key is not 32 bytes"))?;
        let key = VerifyingKey::from_bytes(&key_bytes)?;
        let mut expected_prev: [u8; 32] = match &doc.checkpoint_head {
            Some(head) => hex::decode(head)?
                .try_into()
                .map_err(|_| anyhow!("checkpoint_head is not 32 bytes"))?,
            None => [0u8; 32],
        };
        let mut verified = 0u64;
        for entry in &doc.entries {
            let prev_hash: [u8; 32] = hex::decode(&entry.prev_hash)?
                .try_into()
                .map_err(|_| anyhow!("prev_hash is not 32 bytes"))?;
            if prev_hash != expected_prev {
                break;
            }
            let stored_hash: [u8; 32] = hex::decode(&entry.entry_hash)?
                .try_into()
                .map_err(|_| anyhow!("entry_hash is not 32 bytes"))?;
            if crate::log::hash_entry(&expected_prev, entry.payload.as_bytes()) != stored_hash {
                break;
            }
            let signature: [u8; 64] = hex::decode(&entry.signature)?
                .try_into()
                .map_err(|_| anyhow!("signature is not 64 bytes"))?;
            let signing_hash = domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, &stored_hash);
            if key
                .verify(&signing_hash, &Signature::from_bytes(&signature))
                .is_err()
            {
                break;
            }
            expected_prev = stored_hash;
            verified += 1;
        }
        Ok(verified)
    }

    #[test]
    fn sealed_log_requires_capability_token() -> Result<()> {
        let api = SealedLogTestApi::spawn(|kernel, cfg| seal_event(kernel, cfg, "zone:a"))?;

        let (headers, body) = api.get("/api/sealed-log", false)?;
        assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
        assert!(body.contains("missing_token"), "body: {body}");

        let (headers, body) = api.get("/api/sealed-log", true)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        let doc: SealedLogDocument = serde_json::from_str(&body)?;
        assert_eq!(doc.entries.len(), 1);

        let (headers, _) = api.request("POST", "/api/sealed-log", true)?;
        assert!(
            headers.contains("405 Method Not Allowed"),
            "headers: {headers}"
        );
        Ok(())
    }

    #[test]
    fn sealed_log_serves_stored_payload_bytes_verbatim() -> Result<()> {
        // Distinctive spacing, a tab, and non-alphabetical key order: any
        // parse-and-re-serialize (serde_json::Value) would normalize all
        // three and the verifier's entry hash would stop matching.
        const VERBATIM: &str =
            "{\"record_type\": \"event\",   \"zz_last\": 1,\t\"aa_first\": \"kept   verbatim\"}";
        let mut planted_id = 0i64;
        let api = SealedLogTestApi::spawn(|kernel, cfg| {
            seal_event(kernel, cfg, "zone:a")?;
            planted_id = plant_verbatim_payload(kernel, cfg, VERBATIM)?;
            Ok(())
        })?;

        let (headers, body) = api.get("/api/sealed-log", true)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        let doc: SealedLogDocument = serde_json::from_str(&body)?;
        let planted = doc
            .entries
            .iter()
            .find(|e| e.id == planted_id)
            .expect("planted row is served");
        assert_eq!(
            planted.payload, VERBATIM,
            "served payload must be byte-identical to storage"
        );
        // And those exact bytes reproduce the stored entry hash.
        let prev: [u8; 32] = hex::decode(&planted.prev_hash)?
            .try_into()
            .map_err(|_| anyhow!("prev_hash is not 32 bytes"))?;
        assert_eq!(
            hex::encode(crate::log::hash_entry(&prev, planted.payload.as_bytes())),
            planted.entry_hash
        );
        Ok(())
    }

    #[test]
    fn sealed_log_tail_is_checkpoint_anchored_and_bounded() -> Result<()> {
        let api = SealedLogTestApi::spawn(|kernel, cfg| {
            seal_event(kernel, cfg, "zone:a")?;
            seal_event(kernel, cfg, "zone:b")?;
            // Age the sealed rows past retention (created_at is bookkeeping,
            // not part of the hashed payload), then checkpoint-and-prune.
            kernel.conn.execute(
                "UPDATE sealed_events SET created_at = created_at - 86400",
                [],
            )?;
            kernel.enforce_retention_with_checkpoint(std::time::Duration::from_secs(3600))?;
            seal_event(kernel, cfg, "zone:c")
        })?;

        let (headers, body) = api.get("/api/sealed-log", true)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        let doc: SealedLogDocument = serde_json::from_str(&body)?;

        // Bounded: only the post-checkpoint tail, never the whole chain.
        assert_eq!(doc.entries.len(), 1, "entries: {:?}", doc.entries);
        let anchor = doc
            .checkpoint_head
            .as_ref()
            .expect("checkpoint anchor served");
        assert_eq!(&doc.entries[0].prev_hash, anchor);

        // The anchored tail verifies end to end with the kernel's hashing.
        assert_eq!(walk_with_kernel_hashing(&doc)?, 1);
        Ok(())
    }

    #[test]
    fn sealed_log_document_verifies_with_kernel_hashing() -> Result<()> {
        let api = SealedLogTestApi::spawn(|kernel, cfg| {
            seal_event(kernel, cfg, "zone:a")?;
            seal_event(kernel, cfg, "zone:b")?;
            seal_event(kernel, cfg, "zone:c")
        })?;

        let (headers, body) = api.get("/api/sealed-log", true)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        let doc: SealedLogDocument = serde_json::from_str(&body)?;
        assert!(doc.checkpoint_head.is_none(), "no checkpoint yet");
        assert_eq!(doc.entries.len(), 3);
        assert_eq!(
            walk_with_kernel_hashing(&doc)?,
            3,
            "a verifier must be able to walk the whole served tail"
        );
        Ok(())
    }

    #[test]
    fn sealed_log_has_no_query_surface() -> Result<()> {
        let api = SealedLogTestApi::spawn(|kernel, cfg| seal_event(kernel, cfg, "zone:a"))?;

        // Non-queryable (Invariant VII): a query string neither filters nor
        // errors — the response is identical to the plain request, exactly
        // as `/events` treats an unexpected query.
        let (_, plain) = api.get("/api/sealed-log", true)?;
        let (headers, queried) = api.get("/api/sealed-log?last=24h&zone=zone:a", true)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        assert_eq!(
            plain, queried,
            "a query string must not change the served document"
        );

        // The shared token-in-query guard still applies.
        let (headers, body) = api.get("/api/sealed-log?token=abc", true)?;
        assert!(headers.contains("400 Bad Request"), "headers: {headers}");
        assert!(
            body.contains("token_query_param_not_allowed"),
            "body: {body}"
        );
        Ok(())
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

    // ---------------------------------------------------------------
    // Exposure policy + in-process TLS (`api-tls`).
    // ---------------------------------------------------------------

    /// The non-loopback fail-closed gate holds whenever in-process TLS is not
    /// ACTIVE — on every build when nothing is configured.
    #[test]
    fn spawn_refuses_non_loopback_plaintext_bind() {
        let dir = tempfile::tempdir().unwrap();
        let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
        let api_config = ApiConfig {
            addr: "0.0.0.0:0".to_string(),
            ..ApiConfig::default()
        };
        let err = ApiServer::new(api_config, cfg)
            .spawn()
            .expect_err("plaintext non-loopback bind must be refused");
        assert!(
            err.to_string().contains("refusing to bind non-loopback"),
            "unexpected error: {err}"
        );
    }

    /// A build without `api-tls` must refuse configured material outright:
    /// cert/key that cannot be terminated must not look like protection.
    #[test]
    #[cfg(not(feature = "api-tls"))]
    fn spawn_refuses_tls_material_without_feature() {
        let dir = tempfile::tempdir().unwrap();
        let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            tls: ApiTlsConfig {
                cert_pem: Some(b"cert".to_vec()),
                key_pem: Some(b"key".to_vec()),
            },
            ..ApiConfig::default()
        };
        let err = ApiServer::new(api_config, cfg)
            .spawn()
            .expect_err("TLS material on a non-TLS build must be refused");
        assert!(
            err.to_string().contains("api-tls"),
            "unexpected error: {err}"
        );
    }

    #[cfg(feature = "api-tls")]
    mod tls_active {
        use super::*;
        use rustls::pki_types::ServerName;

        /// A fresh self-signed identity for `localhost` (PEM cert + key).
        pub(crate) fn self_signed_identity() -> (Vec<u8>, Vec<u8>) {
            let rcgen::CertifiedKey { cert, signing_key } =
                rcgen::generate_simple_self_signed(vec!["localhost".to_string()])
                    .expect("generate self-signed certificate");
            (
                cert.pem().into_bytes(),
                signing_key.serialize_pem().into_bytes(),
            )
        }

        /// Read until EOF, tolerating the missing-close_notify error a rustls
        /// client reports when the server drops the TCP stream after its
        /// response (this single-connection server does not send close_notify).
        fn read_lenient(stream: &mut impl Read) -> Vec<u8> {
            let mut out = Vec::new();
            let mut buf = [0u8; 2048];
            loop {
                match stream.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => out.extend_from_slice(&buf[..n]),
                }
            }
            out
        }

        fn spawn_tls_api(addr: &str) -> (tempfile::TempDir, ApiHandle) {
            let dir = tempfile::tempdir().unwrap();
            let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
            let (cert_pem, key_pem) = self_signed_identity();
            let api_config = ApiConfig {
                addr: addr.to_string(),
                tls: ApiTlsConfig {
                    cert_pem: Some(cert_pem),
                    key_pem: Some(key_pem),
                },
                ..ApiConfig::default()
            };
            let handle = ApiServer::new(api_config, cfg)
                .spawn()
                .expect("TLS API server should spawn");
            (dir, handle)
        }

        #[test]
        fn garbage_pem_is_rejected_at_startup() {
            let err = tls::server_config_from_pem(b"not a cert", b"not a key")
                .expect_err("garbage PEM must not build a server config");
            assert!(
                err.to_string().contains("certificate"),
                "unexpected error: {err}"
            );
        }

        #[test]
        fn mismatched_tls_halves_are_rejected() {
            let dir = tempfile::tempdir().unwrap();
            let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
            let api_config = ApiConfig {
                addr: "127.0.0.1:0".to_string(),
                tls: ApiTlsConfig {
                    cert_pem: Some(b"cert only".to_vec()),
                    key_pem: None,
                },
                ..ApiConfig::default()
            };
            let err = ApiServer::new(api_config, cfg)
                .spawn()
                .expect_err("half-configured TLS must be refused");
            assert!(
                err.to_string().contains("together"),
                "unexpected error: {err}"
            );
        }

        /// End to end: the socket actually speaks TLS — a rustls client that
        /// trusts the server certificate completes a handshake and reads
        /// `/health` over the encrypted session. One identity is generated in
        /// the test and shared by both sides: the server terminates with it,
        /// the client pins it as its only trust root.
        #[test]
        fn health_served_over_in_process_tls() {
            let dir = tempfile::tempdir().unwrap();
            let cfg = sealed_log_kernel_config(&dir.path().join("witness.db"));
            let (cert_pem, key_pem) = self_signed_identity();
            let api_config = ApiConfig {
                addr: "127.0.0.1:0".to_string(),
                tls: ApiTlsConfig {
                    cert_pem: Some(cert_pem.clone()),
                    key_pem: Some(key_pem),
                },
                ..ApiConfig::default()
            };
            let handle = ApiServer::new(api_config, cfg)
                .spawn()
                .expect("TLS API server should spawn");

            let mut roots = rustls::RootCertStore::empty();
            for cert in rustls_pemfile::certs(&mut &cert_pem[..]) {
                roots.add(cert.expect("parse test cert")).expect("add root");
            }
            let client_config = rustls::ClientConfig::builder()
                .with_root_certificates(roots)
                .with_no_client_auth();
            let server_name = ServerName::try_from("localhost").expect("server name");
            let conn =
                rustls::ClientConnection::new(std::sync::Arc::new(client_config), server_name)
                    .expect("client connection");
            let tcp = TcpStream::connect(handle.addr).expect("connect");
            let mut tls_stream = rustls::StreamOwned::new(conn, tcp);
            tls_stream
                .write_all(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                .expect("write request over TLS");
            let response = read_lenient(&mut tls_stream);
            let response = String::from_utf8_lossy(&response);
            assert!(response.contains("200 OK"), "response: {response}");
            assert!(
                response.contains(r#""status":"ok""#),
                "response: {response}"
            );

            handle.stop().expect("stop API server");
        }

        /// A plaintext client against the TLS listener gets no HTTP response —
        /// the capability token surface never falls back to cleartext.
        #[test]
        fn plaintext_client_gets_no_http_from_tls_listener() {
            let (_dir, handle) = spawn_tls_api("127.0.0.1:0");

            let mut stream = TcpStream::connect(handle.addr).expect("connect");
            stream
                .write_all(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                .expect("write plaintext request");
            let response = read_lenient(&mut stream);
            let response = String::from_utf8_lossy(&response);
            assert!(
                !response.contains("200 OK"),
                "TLS listener must not answer plaintext HTTP: {response}"
            );

            handle.stop().expect("stop API server");
        }

        /// With in-process TLS active, a non-loopback bind is legitimate —
        /// the token never crosses the network in cleartext.
        #[test]
        fn non_loopback_bind_allowed_when_tls_active() {
            let (_dir, handle) = spawn_tls_api("0.0.0.0:0");
            handle.stop().expect("stop API server");
        }
    }
}
