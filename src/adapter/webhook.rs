//! HTTP webhook ingress adapter.
//!
//! The lowest-friction way to wire a sensor into the witness mesh: any device or script that can
//! make an HTTP `POST` becomes a witness source, with **no MQTT broker required**. A council or
//! hospital can register a sensor with a single `curl`:
//!
//! ```text
//! curl -X POST http://127.0.0.1:8800/sensors/garage/acoustic -d '{"confidence":0.8}'
//! ```
//!
//! The request *path* plays the role of the routing "topic"; the body is parsed exactly like the
//! generic MQTT sensor adapter (shared [`route_message`]). A tiny std-only HTTP/1.1 listener
//! ([`serve`]) accepts POST/PUT requests and forwards `(path, body)` into the adapter's channel;
//! the adapter drains and parses them in [`poll`](WebhookAdapter::poll), optionally inside the
//! seccomp sandbox. The listener is intentionally separate from the trusted read-only Event API,
//! so untrusted inbound bodies never touch that surface.
//!
//! Gated behind the `adapter-webhook` feature (which implies `adapter-mqtt-sensor` for the shared
//! routing types).

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::sync::mpsc::{channel, sync_channel, Receiver, Sender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::Result;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::mqtt_sensor::{parse_messages, route_message, SensorMessage, SensorRoute};
use crate::adapter::SensorAdapter;
use crate::EventType;

/// Maximum accepted request body, in bytes. Webhook payloads are tiny status messages.
const MAX_BODY: usize = 64 * 1024;
/// Maximum bytes read for the request line or any single header line (anti-OOM).
const MAX_LINE: u64 = 1024;
/// Maximum total bytes across all header lines (anti-OOM).
const MAX_HEADER_BYTES: usize = 8 * 1024;
/// Per-connection read timeout so a slow/stuck client cannot tie up its handler forever.
const READ_TIMEOUT: Duration = Duration::from_secs(10);
/// Default size of the connection worker pool.
const DEFAULT_WORKERS: usize = 16;

/// A bidirectional stream a worker can serve over (plain `TcpStream` or a TLS session).
trait ReadWrite: Read + Write {}
impl<T: Read + Write> ReadWrite for T {}

/// Converts an accepted `TcpStream` into a served stream (identity for plaintext, a TLS handshake
/// wrapper when TLS is enabled). Run in the worker, not the accept loop, so handshakes don't block
/// `accept`.
type StreamWrap =
    Arc<dyn Fn(TcpStream) -> std::io::Result<Box<dyn ReadWrite + Send>> + Send + Sync>;

/// Identity wrap: serve the TCP stream directly (no TLS).
fn plain_wrap() -> StreamWrap {
    Arc::new(|tcp| Ok(Box::new(tcp) as Box<dyn ReadWrite + Send>))
}

/// Authentication required of inbound webhook requests.
///
/// The webhook is the one untrusted, network-facing ingress, so on anything but loopback an
/// operator should require a secret. Both schemes are checked in constant time.
#[derive(Clone, Default)]
pub enum WebhookAuth {
    /// No authentication (suitable only for loopback / trusted-LAN deployments).
    #[default]
    None,
    /// Require `Authorization: Bearer <token>` matching this shared token.
    Bearer(String),
    /// Require `X-Signature: sha256=<hex>` HMAC of the request, keyed by `secret`.
    ///
    /// With `replay_window = None` the signature covers the body only (simplest scheme). With
    /// `replay_window = Some(w)` the request must also carry `X-Timestamp` (unix seconds, within
    /// `±w` of now) and a unique `X-Nonce`, and the signature covers
    /// `"<timestamp>.<nonce>.<path>.<body>"` — binding the timestamp, nonce, and request path into
    /// the MAC so a captured request cannot be replayed (the nonce is rejected on reuse, the
    /// timestamp expires, and a signature for one route can't be reused against another).
    Hmac {
        secret: Vec<u8>,
        replay_window: Option<Duration>,
    },
}

/// A simple per-key token bucket: `capacity` burst, refilled at `refill_per_sec`.
#[derive(Clone, Copy)]
pub struct RateLimit {
    pub capacity: f64,
    pub refill_per_sec: f64,
}

/// Tuning for [`serve_with_options`] / [`serve_listener_with_options`].
#[derive(Clone, Default)]
pub struct WebhookOptions {
    /// Authentication scheme (default: none).
    pub auth: WebhookAuth,
    /// Optional per-path rate limit (default: unlimited).
    pub rate_limit: Option<RateLimit>,
    /// Worker pool size; `0` selects [`DEFAULT_WORKERS`].
    pub workers: usize,
}

/// Headers relevant to authentication, extracted from a request.
#[derive(Default)]
struct AuthHeaders {
    authorization: Option<String>,
    signature: Option<String>,
    timestamp: Option<String>,
    nonce: Option<String>,
}

/// Shared state for connection workers.
struct Shared {
    tx: Sender<SensorMessage>,
    auth: WebhookAuth,
    rate_limit: Option<RateLimit>,
    /// per-path token buckets: path -> (tokens, last_refill).
    buckets: Mutex<HashMap<String, (f64, Instant)>>,
    /// recently-seen HMAC nonces (for replay rejection): nonce -> first-seen instant.
    seen_nonces: Mutex<HashMap<String, Instant>>,
}

impl Shared {
    /// Token-bucket admission for one request on `path`. Returns `true` if allowed.
    fn rate_ok(&self, path: &str) -> bool {
        let Some(rl) = self.rate_limit else {
            return true;
        };
        let now = Instant::now();
        let mut map = self.buckets.lock().expect("rate bucket mutex");
        // Hard cap (docs/strategy/12, K6): paths are attacker-controlled, so
        // an unbounded per-path map is a memory leak on demand — the same
        // trap the API's RateLimiter already closes. A bucket idle for 60 s
        // has refilled to capacity anyway, so sweeping it loses nothing; if
        // rotation keeps everything fresh, evict the stalest bucket.
        const MAX_TRACKED_PATHS: usize = 1024;
        if !map.contains_key(path) && map.len() >= MAX_TRACKED_PATHS {
            map.retain(|_, (_, last)| now.duration_since(*last) < Duration::from_secs(60));
            while map.len() >= MAX_TRACKED_PATHS {
                let stalest = map
                    .iter()
                    .min_by_key(|(_, (_, last))| *last)
                    .map(|(path, _)| path.clone());
                match stalest {
                    Some(path) => map.remove(&path),
                    None => break,
                };
            }
        }
        let entry = map.entry(path.to_string()).or_insert((rl.capacity, now));
        let elapsed = now.duration_since(entry.1).as_secs_f64();
        entry.1 = now;
        entry.0 = (entry.0 + elapsed * rl.refill_per_sec).min(rl.capacity);
        if entry.0 >= 1.0 {
            entry.0 -= 1.0;
            true
        } else {
            false
        }
    }

    /// Verify the HMAC scheme for a request, including replay protection when configured.
    ///
    /// In the replay-protected mode the request `path` is bound into the MAC, so a signed payload
    /// captured for one route cannot be replayed against a different route sharing the same secret.
    fn verify_hmac(&self, h: &AuthHeaders, path: &str, body: &[u8]) -> bool {
        let WebhookAuth::Hmac {
            secret,
            replay_window,
        } = &self.auth
        else {
            return false;
        };
        let Some(provided) = h
            .signature
            .as_deref()
            .and_then(|s| s.trim().strip_prefix("sha256="))
            .map(str::trim)
        else {
            return false;
        };

        let Some(window) = replay_window else {
            // Body-only signature.
            let expected = hex::encode(hmac_sha256(secret, body));
            return ct_eq_str(provided, &expected);
        };

        // Replay-protected: require a fresh timestamp and an unused nonce, both bound into the MAC.
        let (Some(ts_raw), Some(nonce)) = (h.timestamp.as_deref(), h.nonce.as_deref()) else {
            return false;
        };
        let (ts_raw, nonce) = (ts_raw.trim(), nonce.trim());
        if nonce.is_empty() {
            return false;
        }
        let Ok(ts) = ts_raw.parse::<u64>() else {
            return false;
        };
        if now_epoch_s().abs_diff(ts) > window.as_secs() {
            return false;
        }

        // Sign "<timestamp>.<nonce>.<path>.<body>" — binding the route so a captured signature
        // cannot be replayed against a different path.
        let mut signed =
            Vec::with_capacity(ts_raw.len() + nonce.len() + path.len() + body.len() + 3);
        signed.extend_from_slice(ts_raw.as_bytes());
        signed.push(b'.');
        signed.extend_from_slice(nonce.as_bytes());
        signed.push(b'.');
        signed.extend_from_slice(path.as_bytes());
        signed.push(b'.');
        signed.extend_from_slice(body);
        let expected = hex::encode(hmac_sha256(secret, &signed));
        if !ct_eq_str(provided, &expected) {
            return false;
        }

        // Signature is valid: reject if the nonce was already used inside the window.
        let mut seen = self.seen_nonces.lock().expect("nonce cache mutex");
        let now = Instant::now();
        seen.retain(|_, &mut t| now.saturating_duration_since(t) < window.saturating_mul(2));
        if seen.contains_key(nonce) {
            return false;
        }
        seen.insert(nonce.to_string(), now);
        true
    }
}

/// Current unix time in seconds (0 on clock error).
fn now_epoch_s() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Constant-time comparison of two equal-purpose strings (used for fixed-length hex digests).
fn ct_eq_str(a: &str, b: &str) -> bool {
    a.as_bytes().ct_eq(b.as_bytes()).into()
}

/// HMAC-SHA256 over `msg` keyed by `key` (RFC 2104), using the always-present `sha2` dependency.
fn hmac_sha256(key: &[u8], msg: &[u8]) -> [u8; 32] {
    const BLOCK: usize = 64;
    let mut k = [0u8; BLOCK];
    if key.len() > BLOCK {
        k[..32].copy_from_slice(&Sha256::digest(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut ipad = [0x36u8; BLOCK];
    let mut opad = [0x5cu8; BLOCK];
    for i in 0..BLOCK {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    let inner = {
        let mut h = Sha256::new();
        h.update(ipad);
        h.update(msg);
        h.finalize()
    };
    let mut outer = Sha256::new();
    outer.update(opad);
    outer.update(inner);
    outer.finalize().into()
}

/// Stateless auth check for the schemes that don't need the body or shared state (None, Bearer).
/// HMAC is handled by [`Shared::verify_hmac`].
fn authorized(auth: &WebhookAuth, authorization: Option<&str>) -> bool {
    match auth {
        WebhookAuth::None => true,
        WebhookAuth::Bearer(expected) => {
            let token = authorization.and_then(|h| {
                let h = h.trim();
                h.strip_prefix("Bearer ")
                    .or_else(|| h.strip_prefix("bearer "))
            });
            match token {
                // Compare SHA-256 digests so the comparison is over fixed-length inputs and does
                // not leak the expected token's length via an early-out on a length mismatch.
                Some(t) => {
                    let got = Sha256::digest(t.trim().as_bytes());
                    let want = Sha256::digest(expected.as_bytes());
                    got.ct_eq(&want).into()
                }
                None => false,
            }
        }
        WebhookAuth::Hmac { .. } => false,
    }
}

static WEBHOOK_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "webhook_adapter",
    allowed_claim_kinds: &[
        ClaimKind::LargeObjectBoundaryCrossing,
        ClaimKind::SmallObjectBoundaryCrossing,
        ClaimKind::AcousticImpulseInZone,
        ClaimKind::PresenceInRestrictedZone,
        ClaimKind::VehiclePresenceAfterHours,
        ClaimKind::ContactStateChange,
        ClaimKind::ObjectRemovedFromZone,
        ClaimKind::TamperDetected,
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
        EventType::AcousticImpulseInZone,
        EventType::PresenceInRestrictedZone,
        EventType::VehiclePresenceAfterHours,
        EventType::ContactStateChange,
        EventType::ObjectRemovedFromZone,
        EventType::TamperDetected,
    ],
    requested_capabilities: &[],
};

/// HTTP webhook adapter. Construct with [`WebhookAdapter::new`], spawn [`serve`] with the returned
/// [`Sender`] on a background thread, and register the adapter with the host.
pub struct WebhookAdapter {
    rx: Receiver<SensorMessage>,
    routes: crate::adapter::mqtt_sensor::SharedRoutes,
    sandbox: bool,
}

impl WebhookAdapter {
    /// Build the adapter from a routing table (route `topic` is matched against the request path);
    /// returns the feeding [`Sender`].
    pub fn new(routes: Vec<SensorRoute>) -> (Self, Sender<SensorMessage>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                routes: Arc::new(Mutex::new(routes)),
                sandbox: false,
            },
            tx,
        )
    }

    /// Opt in to running payload parsing inside the seccomp sandbox (requires the
    /// `adapter-sandbox` feature; no effect otherwise).
    pub fn with_sandbox(mut self, enabled: bool) -> Self {
        self.sandbox = enabled;
        self
    }

    /// Handle to the live routing table, for hot-reload by the host.
    pub fn routes_handle(&self) -> crate::adapter::mqtt_sensor::SharedRoutes {
        Arc::clone(&self.routes)
    }

    /// Pure transform: map one request `(path, body)` to at most one claim.
    pub fn message_to_claim(&self, path: &str, body: &[u8]) -> Option<Claim> {
        route_message(&self.routes.lock().expect("routes mutex"), path, body)
    }
}

impl SensorAdapter for WebhookAdapter {
    fn name(&self) -> &'static str {
        "webhook_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &WEBHOOK_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let mut msgs = Vec::new();
        while let Ok(msg) = self.rx.try_recv() {
            msgs.push(msg);
        }
        if msgs.is_empty() {
            return Ok(Vec::new());
        }
        let routes = self.routes.lock().expect("routes mutex");
        parse_messages(&routes, &msgs, self.sandbox)
    }
}

/// Bind a TCP listener for the webhook (separate from [`serve_listener`] so callers/tests can
/// learn the bound address, e.g. when binding to port 0).
pub fn bind(addr: impl ToSocketAddrs) -> Result<TcpListener> {
    let listener = TcpListener::bind(addr)?;
    if let Ok(local) = listener.local_addr() {
        log::info!("webhook listener bound to {local}");
    }
    Ok(listener)
}

/// Run a blocking HTTP listener that forwards each `POST`/`PUT` request as `(path, body)` into
/// `tx`, with no authentication or rate limiting. Returns only on a fatal bind error.
pub fn serve(addr: impl ToSocketAddrs, tx: Sender<SensorMessage>) -> Result<()> {
    serve_with_options(addr, tx, WebhookOptions::default())
}

/// Like [`serve`], with authentication / rate-limit / worker-pool tuning.
pub fn serve_with_options(
    addr: impl ToSocketAddrs,
    tx: Sender<SensorMessage>,
    options: WebhookOptions,
) -> Result<()> {
    serve_listener_with_options(bind(addr)?, tx, options)
}

/// Serve an already-bound listener (see [`bind`]) with default options.
pub fn serve_listener(listener: TcpListener, tx: Sender<SensorMessage>) -> Result<()> {
    serve_listener_with_options(listener, tx, WebhookOptions::default())
}

/// Serve an already-bound listener with explicit [`WebhookOptions`].
///
/// Connections are dispatched to a fixed-size worker pool over a bounded queue, so a slow or
/// stuck client cannot block delivery of webhooks from other sensors (head-of-line blocking) and
/// a connection flood cannot spawn unbounded threads. When the queue is saturated the listener
/// fast-rejects with `503`. Per-connection reads are bounded (line/header/body limits + timeout),
/// and each request is authenticated and rate-limited per [`WebhookOptions`].
pub fn serve_listener_with_options(
    listener: TcpListener,
    tx: Sender<SensorMessage>,
    options: WebhookOptions,
) -> Result<()> {
    let shared = Arc::new(Shared {
        tx,
        auth: options.auth,
        rate_limit: options.rate_limit,
        buckets: Mutex::new(HashMap::new()),
        seen_nonces: Mutex::new(HashMap::new()),
    });
    run_pool(listener, shared, options.workers, plain_wrap())
}

/// Dispatch accepted connections to a fixed worker pool over a bounded queue. `wrap` adapts each
/// raw `TcpStream` into the served stream (plaintext or TLS).
fn run_pool(
    listener: TcpListener,
    shared: Arc<Shared>,
    workers: usize,
    wrap: StreamWrap,
) -> Result<()> {
    let workers = if workers == 0 {
        DEFAULT_WORKERS
    } else {
        workers
    };

    // Bounded hand-off queue: the accept loop is the producer, the pool are consumers.
    let (job_tx, job_rx) = sync_channel::<TcpStream>(workers.saturating_mul(2));
    let job_rx = Arc::new(Mutex::new(job_rx));
    for _ in 0..workers {
        let job_rx = Arc::clone(&job_rx);
        let shared = Arc::clone(&shared);
        let wrap = Arc::clone(&wrap);
        thread::spawn(move || loop {
            // Hold the lock only across recv; release before handling so peers can pick up work.
            let job = {
                let guard = job_rx.lock().expect("webhook job queue mutex");
                guard.recv()
            };
            let tcp = match job {
                Ok(tcp) => tcp,
                Err(_) => break, // listener dropped the sender
            };
            // Bound reads, writes, and the TLS handshake on the underlying socket so a client that
            // stalls mid-read or refuses to read the response cannot tie up a worker forever.
            if let Err(e) = tcp.set_read_timeout(Some(READ_TIMEOUT)) {
                log::debug!("webhook set_read_timeout failed: {e}");
                continue;
            }
            if let Err(e) = tcp.set_write_timeout(Some(READ_TIMEOUT)) {
                log::debug!("webhook set_write_timeout failed: {e}");
                continue;
            }
            match wrap(tcp) {
                Ok(stream) => {
                    if let Err(e) = handle_connection(stream, &shared) {
                        log::debug!("webhook connection error: {e}");
                    }
                }
                Err(e) => log::debug!("webhook stream setup failed: {e}"),
            }
        });
    }

    for stream in listener.incoming() {
        match stream {
            Ok(s) => match job_tx.try_send(s) {
                Ok(()) => {}
                Err(TrySendError::Full(mut s)) => {
                    log::warn!("webhook overloaded; rejecting connection with 503");
                    let _ = write_response(&mut s, 503, "server busy");
                }
                Err(TrySendError::Disconnected(_)) => break,
            },
            Err(e) => log::warn!("webhook accept error: {e}"),
        }
    }
    Ok(())
}

fn handle_connection<S: Read + Write>(stream: S, shared: &Shared) -> Result<()> {
    let mut reader = BufReader::new(stream);

    // Request line: "METHOD SP PATH SP HTTP/1.1". Bounded read to prevent OOM from a client that
    // never sends a newline.
    let mut request_line = String::new();
    if reader
        .by_ref()
        .take(MAX_LINE)
        .read_line(&mut request_line)?
        == 0
    {
        return Ok(()); // client closed before sending anything
    }
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let raw_path = parts.next().unwrap_or("/").to_string();

    // Headers. Match names case-insensitively but preserve values (tokens/signatures are
    // case-sensitive). Each line and the total header size are bounded.
    let mut content_length: usize = 0;
    let mut headers = AuthHeaders::default();
    let mut headers_read: usize = 0;
    loop {
        let mut line = String::new();
        let n = reader.by_ref().take(MAX_LINE).read_line(&mut line)?;
        if n == 0 || line == "\r\n" || line == "\n" {
            break;
        }
        headers_read += n;
        if headers_read > MAX_HEADER_BYTES {
            return write_response(reader.get_mut(), 400, "headers too large");
        }
        if let Some((name, value)) = line.split_once(':') {
            let value = value.trim();
            match name.trim().to_ascii_lowercase().as_str() {
                "content-length" => content_length = value.parse().unwrap_or(0),
                "authorization" => headers.authorization = Some(value.to_string()),
                "x-signature" => headers.signature = Some(value.to_string()),
                "x-timestamp" => headers.timestamp = Some(value.to_string()),
                "x-nonce" => headers.nonce = Some(value.to_string()),
                _ => {}
            }
        }
    }

    if method != "POST" && method != "PUT" {
        return write_response(reader.get_mut(), 405, "method not allowed");
    }
    if content_length > MAX_BODY {
        return write_response(reader.get_mut(), 413, "payload too large");
    }

    // Bearer/None auth does not need the body, so reject before reading it: otherwise an
    // unauthenticated client could advertise a large body, drip it until READ_TIMEOUT, and tie up
    // a worker. HMAC must defer (it signs the body) and is checked after the read.
    let is_hmac = matches!(shared.auth, WebhookAuth::Hmac { .. });
    if !is_hmac && !authorized(&shared.auth, headers.authorization.as_deref()) {
        return write_response(reader.get_mut(), 401, "unauthorized");
    }

    let mut body = vec![0u8; content_length];
    reader.read_exact(&mut body)?;

    // Strip any query string so routing (and the replay-protected signature) keys on the bare path.
    let path = raw_path.split('?').next().unwrap_or(&raw_path).to_string();

    if is_hmac && !shared.verify_hmac(&headers, &path, &body) {
        return write_response(reader.get_mut(), 401, "unauthorized");
    }

    if !shared.rate_ok(&path) {
        return write_response(reader.get_mut(), 429, "too many requests");
    }

    // A full channel/dropped receiver is not fatal for the request.
    let _ = shared.tx.send((path, body));
    write_response(reader.get_mut(), 204, "")
}

fn write_response<W: Write>(stream: &mut W, code: u16, body: &str) -> Result<()> {
    let reason = match code {
        204 => "No Content",
        400 => "Bad Request",
        401 => "Unauthorized",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        429 => "Too Many Requests",
        503 => "Service Unavailable",
        _ => "OK",
    };
    let response = format!(
        "HTTP/1.1 {code} {reason}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(response.as_bytes())?;
    stream.flush()?;
    Ok(())
}

/// TLS support for the webhook listener (feature `adapter-webhook-tls`). Bearer tokens cross the
/// network in plaintext otherwise, so TLS is the natural complement to webhook authentication for
/// any non-loopback deployment.
#[cfg(feature = "adapter-webhook-tls")]
pub mod tls {
    use super::*;
    use std::fs::File;
    use std::io::BufReader as IoBufReader;
    use std::path::Path;
    use std::sync::Arc;

    use anyhow::Context;
    use rustls::pki_types::CertificateDer;
    use rustls::server::WebPkiClientVerifier;
    use rustls::{RootCertStore, ServerConfig};

    fn load_certs(path: &Path) -> Result<Vec<CertificateDer<'static>>> {
        let mut rd = IoBufReader::new(
            File::open(path).with_context(|| format!("opening {}", path.display()))?,
        );
        let certs = rustls_pemfile::certs(&mut rd).collect::<std::result::Result<Vec<_>, _>>()?;
        if certs.is_empty() {
            anyhow::bail!("no certificates found in {}", path.display());
        }
        Ok(certs)
    }

    fn load_key(path: &Path) -> Result<rustls::pki_types::PrivateKeyDer<'static>> {
        let mut rd = IoBufReader::new(
            File::open(path).with_context(|| format!("opening TLS key {}", path.display()))?,
        );
        rustls_pemfile::private_key(&mut rd)?
            .ok_or_else(|| anyhow::anyhow!("no private key found in {}", path.display()))
    }

    /// Load a rustls [`ServerConfig`] from PEM cert-chain and private-key files (no client auth).
    pub fn load_server_config(cert_pem: &Path, key_pem: &Path) -> Result<Arc<ServerConfig>> {
        let config = ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(load_certs(cert_pem)?, load_key(key_pem)?)
            .context("building TLS server config")?;
        Ok(Arc::new(config))
    }

    /// Load a mutual-TLS [`ServerConfig`]: clients must present a certificate chaining to one of the
    /// certs in `client_ca_pem`. This authenticates machine-to-machine senders by certificate, with
    /// no shared secret crossing the wire.
    pub fn load_server_config_mtls(
        cert_pem: &Path,
        key_pem: &Path,
        client_ca_pem: &Path,
    ) -> Result<Arc<ServerConfig>> {
        let mut roots = RootCertStore::empty();
        for ca in load_certs(client_ca_pem)? {
            roots
                .add(ca)
                .with_context(|| format!("adding client CA from {}", client_ca_pem.display()))?;
        }
        let verifier = WebPkiClientVerifier::builder(Arc::new(roots))
            .build()
            .context("building client-certificate verifier")?;
        let config = ServerConfig::builder()
            .with_client_cert_verifier(verifier)
            .with_single_cert(load_certs(cert_pem)?, load_key(key_pem)?)
            .context("building mTLS server config")?;
        Ok(Arc::new(config))
    }

    /// Like [`serve_with_options`](super::serve_with_options) but over TLS.
    pub fn serve_tls(
        addr: impl ToSocketAddrs,
        tx: Sender<SensorMessage>,
        options: WebhookOptions,
        config: Arc<ServerConfig>,
    ) -> Result<()> {
        serve_listener_tls(super::bind(addr)?, tx, options, config)
    }

    /// Serve an already-bound listener over TLS. The handshake runs in the worker (not the accept
    /// loop) and is bounded by the per-connection read timeout.
    pub fn serve_listener_tls(
        listener: TcpListener,
        tx: Sender<SensorMessage>,
        options: WebhookOptions,
        config: Arc<ServerConfig>,
    ) -> Result<()> {
        let shared = Arc::new(Shared {
            tx,
            auth: options.auth,
            rate_limit: options.rate_limit,
            buckets: Mutex::new(HashMap::new()),
            seen_nonces: Mutex::new(HashMap::new()),
        });
        let wrap: StreamWrap = Arc::new(move |tcp: TcpStream| {
            let conn = rustls::ServerConnection::new(Arc::clone(&config))
                .map_err(std::io::Error::other)?;
            Ok(Box::new(rustls::StreamOwned::new(conn, tcp)) as Box<dyn ReadWrite + Send>)
        });
        run_pool(listener, shared, options.workers, wrap)
    }
}

#[cfg(feature = "adapter-webhook-tls")]
pub use tls::{load_server_config, load_server_config_mtls, serve_listener_tls, serve_tls};

#[cfg(test)]
mod tests {
    use super::*;

    fn route() -> SensorRoute {
        SensorRoute::new(
            "/sensors/garage/acoustic",
            ClaimKind::AcousticImpulseInZone,
            "garage",
        )
    }

    #[test]
    fn post_body_maps_to_claim_by_path() {
        let (adapter, _tx) = WebhookAdapter::new(vec![route()]);
        let claim = adapter
            .message_to_claim("/sensors/garage/acoustic", br#"{"confidence":0.7}"#)
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::AcousticImpulseInZone);
        assert_eq!(claim.zone_label, "garage");
    }

    #[test]
    fn poll_drains_fed_requests() {
        let (mut adapter, tx) = WebhookAdapter::new(vec![route()]);
        tx.send((
            "/sensors/garage/acoustic".to_string(),
            br#"{"confidence":0.7}"#.to_vec(),
        ))
        .unwrap();
        let claims = adapter.poll().expect("poll");
        assert_eq!(claims.len(), 1);
        assert!(adapter.poll().expect("poll2").is_empty());
    }

    #[test]
    fn unrouted_path_yields_nothing() {
        let (adapter, _tx) = WebhookAdapter::new(vec![route()]);
        assert!(adapter.message_to_claim("/unknown", b"{}").is_none());
    }

    fn test_shared(auth: WebhookAuth, rate_limit: Option<RateLimit>) -> Shared {
        Shared {
            tx: channel().0,
            auth,
            rate_limit,
            buckets: Mutex::new(HashMap::new()),
            seen_nonces: Mutex::new(HashMap::new()),
        }
    }

    #[test]
    fn auth_none_allows_everything() {
        assert!(authorized(&WebhookAuth::None, None));
    }

    #[test]
    fn bearer_auth_requires_matching_token() {
        let auth = WebhookAuth::Bearer("s3cret".to_string());
        assert!(authorized(&auth, Some("Bearer s3cret")));
        assert!(authorized(&auth, Some("bearer s3cret"))); // case-insensitive scheme
        assert!(!authorized(&auth, Some("Bearer wrong")));
        assert!(!authorized(&auth, Some("s3cret"))); // missing scheme
        assert!(!authorized(&auth, None)); // missing header
    }

    #[test]
    fn hmac_auth_validates_body_signature() {
        let secret = b"shared-key".to_vec();
        let body = br#"{"confidence":0.8}"#;
        let sig = format!("sha256={}", hex::encode(hmac_sha256(&secret, body)));
        let shared = test_shared(
            WebhookAuth::Hmac {
                secret,
                replay_window: None,
            },
            None,
        );
        let headers = AuthHeaders {
            signature: Some(sig),
            ..Default::default()
        };
        // Body-only mode ignores the path argument.
        assert!(shared.verify_hmac(&headers, "/p", body));
        // Tampered body invalidates the signature.
        assert!(!shared.verify_hmac(&headers, "/p", br#"{"confidence":0.9}"#));
        // Missing / malformed signature header.
        assert!(!shared.verify_hmac(&AuthHeaders::default(), "/p", body));
        assert!(!shared.verify_hmac(
            &AuthHeaders {
                signature: Some("deadbeef".to_string()),
                ..Default::default()
            },
            "/p",
            body
        ));
    }

    #[test]
    fn hmac_replay_protection() {
        let secret = b"shared-key".to_vec();
        let body = br#"{"confidence":0.8}"#;
        let window = Duration::from_secs(300);
        let shared = test_shared(
            WebhookAuth::Hmac {
                secret: secret.clone(),
                replay_window: Some(window),
            },
            None,
        );

        let now = now_epoch_s();
        let path = "/sensors/garage/acoustic";
        let signed = |ts: u64, nonce: &str, path: &str| {
            let mut m = Vec::new();
            m.extend_from_slice(ts.to_string().as_bytes());
            m.push(b'.');
            m.extend_from_slice(nonce.as_bytes());
            m.push(b'.');
            m.extend_from_slice(path.as_bytes());
            m.push(b'.');
            m.extend_from_slice(body);
            format!("sha256={}", hex::encode(hmac_sha256(&secret, &m)))
        };
        let headers = |ts: u64, nonce: &str, path: &str| AuthHeaders {
            signature: Some(signed(ts, nonce, path)),
            timestamp: Some(ts.to_string()),
            nonce: Some(nonce.to_string()),
            ..Default::default()
        };

        // Fresh, signed request with a unique nonce is accepted once.
        assert!(shared.verify_hmac(&headers(now, "nonce-1", path), path, body));
        // Replaying the exact same request (same nonce) is rejected.
        assert!(!shared.verify_hmac(&headers(now, "nonce-1", path), path, body));
        // A different nonce is accepted.
        assert!(shared.verify_hmac(&headers(now, "nonce-2", path), path, body));
        // Stale timestamp (outside the window) is rejected even with a fresh nonce.
        assert!(!shared.verify_hmac(&headers(now - 1000, "nonce-3", path), path, body));
        // A signature for one path replayed against a different path is rejected.
        assert!(!shared.verify_hmac(&headers(now, "nonce-x", path), "/other", body));
        // Missing timestamp/nonce is rejected.
        assert!(!shared.verify_hmac(
            &AuthHeaders {
                signature: Some(signed(now, "nonce-4", path)),
                ..Default::default()
            },
            path,
            body
        ));
        // Tampered body invalidates the signature.
        assert!(!shared.verify_hmac(
            &headers(now, "nonce-5", path),
            path,
            br#"{"confidence":0.9}"#
        ));
    }

    #[cfg(feature = "adapter-webhook-tls")]
    #[test]
    fn tls_load_missing_files_errors() {
        use std::path::Path;
        // Fails closed when the cert/key are absent (never silently serves plaintext).
        assert!(super::tls::load_server_config(
            Path::new("/no/such.crt"),
            Path::new("/no/such.key")
        )
        .is_err());
        // mTLS likewise fails closed on a missing client CA.
        assert!(super::tls::load_server_config_mtls(
            Path::new("/no/such.crt"),
            Path::new("/no/such.key"),
            Path::new("/no/such.ca")
        )
        .is_err());
    }

    #[test]
    fn rfc2104_hmac_test_vector() {
        // RFC 4231 test case 2: key="Jefe", data="what do ya want for nothing?".
        let mac = hmac_sha256(b"Jefe", b"what do ya want for nothing?");
        assert_eq!(
            hex::encode(mac),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
    }

    #[test]
    fn token_bucket_limits_then_refills() {
        let shared = test_shared(
            WebhookAuth::None,
            Some(RateLimit {
                capacity: 2.0,
                refill_per_sec: 1000.0,
            }),
        );
        // Burst capacity of 2 on a path, then throttled.
        assert!(shared.rate_ok("/a"));
        assert!(shared.rate_ok("/a"));
        assert!(!shared.rate_ok("/a"));
        // A different path has its own independent bucket.
        assert!(shared.rate_ok("/b"));
        // After enough wall-clock time, /a refills.
        std::thread::sleep(Duration::from_millis(5));
        assert!(shared.rate_ok("/a"));
    }
}
