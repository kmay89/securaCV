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
use std::time::{Duration, Instant};

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
    /// Require `X-Signature: sha256=<hex>` = HMAC-SHA256(secret, body). Also authenticates the
    /// body, so a replayed body cannot be altered.
    Hmac(Vec<u8>),
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

/// Shared state for connection workers.
struct Shared {
    tx: Sender<SensorMessage>,
    auth: WebhookAuth,
    rate_limit: Option<RateLimit>,
    /// per-path token buckets: path -> (tokens, last_refill).
    buckets: Mutex<HashMap<String, (f64, Instant)>>,
}

impl Shared {
    /// Token-bucket admission for one request on `path`. Returns `true` if allowed.
    fn rate_ok(&self, path: &str) -> bool {
        let Some(rl) = self.rate_limit else {
            return true;
        };
        let now = Instant::now();
        let mut map = self.buckets.lock().expect("rate bucket mutex");
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

/// Constant-time authentication check. `body` is only needed for the HMAC scheme.
fn authorized(
    auth: &WebhookAuth,
    authorization: Option<&str>,
    signature: Option<&str>,
    body: &[u8],
) -> bool {
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
        WebhookAuth::Hmac(secret) => {
            let provided = signature
                .and_then(|h| h.trim().strip_prefix("sha256="))
                .map(str::trim);
            match provided {
                Some(hexsig) => {
                    let expected_hex = hex::encode(hmac_sha256(secret, body));
                    hexsig.as_bytes().ct_eq(expected_hex.as_bytes()).into()
                }
                None => false,
            }
        }
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
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
        EventType::AcousticImpulseInZone,
        EventType::PresenceInRestrictedZone,
        EventType::VehiclePresenceAfterHours,
        EventType::ContactStateChange,
        EventType::ObjectRemovedFromZone,
    ],
    requested_capabilities: &[],
};

/// HTTP webhook adapter. Construct with [`WebhookAdapter::new`], spawn [`serve`] with the returned
/// [`Sender`] on a background thread, and register the adapter with the host.
pub struct WebhookAdapter {
    rx: Receiver<SensorMessage>,
    routes: Vec<SensorRoute>,
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
                routes,
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

    /// Pure transform: map one request `(path, body)` to at most one claim.
    pub fn message_to_claim(&self, path: &str, body: &[u8]) -> Option<Claim> {
        route_message(&self.routes, path, body)
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
        parse_messages(&self.routes, &msgs, self.sandbox)
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
    let workers = if options.workers == 0 {
        DEFAULT_WORKERS
    } else {
        options.workers
    };
    let shared = Arc::new(Shared {
        tx,
        auth: options.auth,
        rate_limit: options.rate_limit,
        buckets: Mutex::new(HashMap::new()),
    });

    // Bounded hand-off queue: the accept loop is the producer, the pool are consumers.
    let (job_tx, job_rx) = sync_channel::<TcpStream>(workers.saturating_mul(2));
    let job_rx = Arc::new(Mutex::new(job_rx));
    for _ in 0..workers {
        let job_rx = Arc::clone(&job_rx);
        let shared = Arc::clone(&shared);
        thread::spawn(move || loop {
            // Hold the lock only across recv; release before handling so peers can pick up work.
            let job = {
                let guard = job_rx.lock().expect("webhook job queue mutex");
                guard.recv()
            };
            match job {
                Ok(stream) => {
                    if let Err(e) = handle_connection(stream, &shared) {
                        log::debug!("webhook connection error: {e}");
                    }
                }
                Err(_) => break, // listener dropped the sender
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

fn handle_connection(stream: TcpStream, shared: &Shared) -> Result<()> {
    stream.set_read_timeout(Some(READ_TIMEOUT))?;
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
    let mut authorization: Option<String> = None;
    let mut signature: Option<String> = None;
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
                "authorization" => authorization = Some(value.to_string()),
                "x-signature" => signature = Some(value.to_string()),
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

    // Bearer auth does not need the body, so reject before reading it: otherwise an
    // unauthenticated client could advertise a large body, drip it until READ_TIMEOUT, and tie up
    // a worker. HMAC must defer (it signs the body) and is checked after the read.
    if matches!(shared.auth, WebhookAuth::Bearer(_))
        && !authorized(&shared.auth, authorization.as_deref(), None, &[])
    {
        return write_response(reader.get_mut(), 401, "unauthorized");
    }

    let mut body = vec![0u8; content_length];
    reader.read_exact(&mut body)?;

    if matches!(shared.auth, WebhookAuth::Hmac(_))
        && !authorized(&shared.auth, None, signature.as_deref(), &body)
    {
        return write_response(reader.get_mut(), 401, "unauthorized");
    }

    // Strip any query string so routing keys on the bare path.
    let path = raw_path.split('?').next().unwrap_or(&raw_path).to_string();

    if !shared.rate_ok(&path) {
        return write_response(reader.get_mut(), 429, "too many requests");
    }

    // A full channel/dropped receiver is not fatal for the request.
    let _ = shared.tx.send((path, body));
    write_response(reader.get_mut(), 204, "")
}

fn write_response(stream: &mut TcpStream, code: u16, body: &str) -> Result<()> {
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

    #[test]
    fn auth_none_allows_everything() {
        assert!(authorized(&WebhookAuth::None, None, None, b""));
    }

    #[test]
    fn bearer_auth_requires_matching_token() {
        let auth = WebhookAuth::Bearer("s3cret".to_string());
        assert!(authorized(&auth, Some("Bearer s3cret"), None, b""));
        assert!(authorized(&auth, Some("bearer s3cret"), None, b"")); // case-insensitive scheme
        assert!(!authorized(&auth, Some("Bearer wrong"), None, b""));
        assert!(!authorized(&auth, Some("s3cret"), None, b"")); // missing scheme
        assert!(!authorized(&auth, None, None, b"")); // missing header
    }

    #[test]
    fn hmac_auth_validates_body_signature() {
        let secret = b"shared-key".to_vec();
        let body = br#"{"confidence":0.8}"#;
        let sig = format!("sha256={}", hex::encode(hmac_sha256(&secret, body)));
        let auth = WebhookAuth::Hmac(secret);
        assert!(authorized(&auth, None, Some(&sig), body));
        // Tampered body invalidates the signature.
        assert!(!authorized(
            &auth,
            None,
            Some(&sig),
            br#"{"confidence":0.9}"#
        ));
        // Missing / malformed signature header.
        assert!(!authorized(&auth, None, None, body));
        assert!(!authorized(&auth, None, Some("deadbeef"), body));
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
        let shared = Shared {
            tx: channel().0,
            auth: WebhookAuth::None,
            rate_limit: Some(RateLimit {
                capacity: 2.0,
                refill_per_sec: 1000.0,
            }),
            buckets: Mutex::new(HashMap::new()),
        };
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
