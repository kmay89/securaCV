//! Minimal HTTP server for the kernel-served break-glass web UI.
//!
//! Self-contained on purpose: it does **not** touch the read-only event-export
//! API server, so that surface is unchanged. It reuses the event API's
//! [`CapabilityTokenManager`] (rotating,
//! constant-time-validated bearer token) and mirrors its plaintext-bind + TLS
//! conformance model — TLS is terminated by a front proxy, never in-process.
//!
//! Security posture (operator-mediated, loopback-default):
//! - Binds `127.0.0.1` by default; a routable address is allowed **only** when
//!   TLS materials are configured (the operator's attestation that a terminator
//!   fronts it). Non-loopback without TLS is a hard refusal at bind time.
//! - Every request needs the rotating capability bearer token; per-IP failures
//!   are rate-limited with exponential lockout.
//! - Only the operator talks to the server. Trustees sign the request hash
//!   offline with their own keys and hand the operator the signature, which the
//!   operator submits — so the routable surface stays minimal and the capability
//!   token is never distributed.
//! - All routing/validation/unseal-gating lives in [`super::http`]; this module
//!   is transport only.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Result};

use crate::api::{ApiTlsConfig, CapabilityTokenManager};
use crate::break_glass::http::{handle_break_glass, BreakGlassOps};
use crate::break_glass::session::BreakGlassSession;
use crate::TimeBucket;

/// Cap on a single request (headers + body). Break-glass payloads are tiny
/// (a hex signature, a short envelope/purpose), so this is deliberately small.
const MAX_REQUEST_BYTES: usize = 64 * 1024;

/// The self-contained operator console (HTML + inline CSS/JS, no external assets).
const BREAK_GLASS_PAGE: &str = include_str!("breakglass.html");

/// Configuration for the break-glass HTTP server.
#[derive(Clone, Debug)]
pub struct BreakGlassServerConfig {
    /// Address to bind. Defaults to loopback.
    pub addr: String,
    /// Directory unsealed envelopes are written to (server-configured, never
    /// supplied by a client).
    pub output_dir: String,
    /// Optional path to persist the rotating capability token (mode 0600).
    pub token_path: Option<PathBuf>,
    /// TLS attestation. Required to bind a non-loopback address.
    pub tls: Option<ApiTlsConfig>,
}

impl Default for BreakGlassServerConfig {
    fn default() -> Self {
        Self {
            addr: "127.0.0.1:8800".to_string(),
            output_dir: "vault/unsealed".to_string(),
            token_path: None,
            tls: None,
        }
    }
}

/// Refuse to expose break-glass on a routable interface without TLS configured.
fn validate_exposure(addr: &SocketAddr, cfg: &BreakGlassServerConfig) -> Result<()> {
    let tls_configured = cfg.tls.as_ref().is_some_and(|t| t.is_configured());
    if !addr.ip().is_loopback() && !tls_configured {
        return Err(anyhow!(
            "refusing to bind non-loopback address {addr} without TLS: break-glass on a routable \
             interface must sit behind a TLS terminator — provide TLS cert/key to attest it, or \
             bind a loopback address and reach it over an SSH tunnel"
        ));
    }
    Ok(())
}

/// A bound break-glass server, ready to serve. Binding happens up front so the
/// caller can learn the actual address (and the capability token, for tests)
/// before the blocking serve loop starts.
pub struct BreakGlassServer {
    listener: TcpListener,
    addr: SocketAddr,
    cfg: BreakGlassServerConfig,
    token_mgr: CapabilityTokenManager,
}

impl BreakGlassServer {
    /// Bind the configured address, enforcing the exposure policy, and mint the
    /// initial capability token (persisting it if a token path is configured).
    pub fn bind(cfg: BreakGlassServerConfig) -> Result<Self> {
        let configured: SocketAddr = cfg
            .addr
            .parse()
            .map_err(|e| anyhow!("invalid break-glass server address '{}': {e}", cfg.addr))?;
        validate_exposure(&configured, &cfg)?;

        let listener = TcpListener::bind(configured)?;
        let addr = listener.local_addr()?;
        if configured.ip().is_loopback() && !addr.ip().is_loopback() {
            return Err(anyhow!(
                "configured loopback address '{configured}' but bound non-loopback '{addr}'"
            ));
        }
        listener.set_nonblocking(true)?;

        let token_mgr = CapabilityTokenManager::new(TimeBucket::now_10min()?)?;
        if let Some(path) = &cfg.token_path {
            write_token_file(path, &token_mgr.token_hex())?;
        }

        Ok(Self {
            listener,
            addr,
            cfg,
            token_mgr,
        })
    }

    /// The actual bound address.
    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }

    /// The current capability token (hex). Exposed primarily for tests; in
    /// production the operator reads it from the token file.
    pub fn capability_token(&self) -> String {
        self.token_mgr.token_hex()
    }

    /// Serve until `shutdown` is set. Single-threaded: one in-memory
    /// [`BreakGlassSession`] is shared across connections without locking.
    pub fn serve<O: BreakGlassOps>(mut self, mut ops: O, shutdown: Arc<AtomicBool>) -> Result<()> {
        let mut session = BreakGlassSession::new();
        let mut lockout = Lockout::new();
        log::info!(
            "break-glass server listening on {} (loopback={})",
            self.addr,
            self.addr.ip().is_loopback()
        );
        loop {
            if shutdown.load(Ordering::SeqCst) {
                break;
            }
            match self.listener.accept() {
                Ok((stream, peer)) => {
                    if let Err(err) = handle_conn(
                        stream,
                        peer,
                        &self.cfg,
                        &mut ops,
                        &mut session,
                        &mut self.token_mgr,
                        &mut lockout,
                    ) {
                        log::warn!("break-glass request rejected: {err}");
                    }
                }
                Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(50));
                }
                Err(err) => return Err(err.into()),
            }
        }
        Ok(())
    }
}

#[allow(clippy::too_many_arguments)]
fn handle_conn<O: BreakGlassOps>(
    mut stream: TcpStream,
    peer: SocketAddr,
    cfg: &BreakGlassServerConfig,
    ops: &mut O,
    session: &mut BreakGlassSession,
    token_mgr: &mut CapabilityTokenManager,
    lockout: &mut Lockout,
) -> Result<()> {
    let local = stream.local_addr()?;
    // Defense in depth: even if bound to loopback, never serve a non-loopback peer.
    if local.ip().is_loopback() && !peer.ip().is_loopback() {
        return write_json(&mut stream, 403, r#"{"error":"forbidden"}"#);
    }
    if let Some(retry_after) = lockout.locked(&peer.ip()) {
        return write_json(
            &mut stream,
            429,
            &format!(r#"{{"error":"auth_locked","retry_after":{retry_after}}}"#),
        );
    }

    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let request = read_http_request(&mut stream)?;

    if request.has_query_token() {
        return write_json(
            &mut stream,
            400,
            r#"{"error":"token_query_param_not_allowed"}"#,
        );
    }

    // Serve the operator console without auth: a browser cannot attach a bearer
    // token to top-level navigation, and the page carries no secrets — every API
    // call it makes still requires the capability token the operator pastes in.
    if request.method == "GET"
        && matches!(request.path.as_str(), "/" | "/breakglass" | "/breakglass/")
    {
        return write_html(&mut stream, BREAK_GLASS_PAGE);
    }

    let now_bucket = TimeBucket::now_10min()?;
    if token_mgr.rotate_if_needed(now_bucket)? {
        if let Some(path) = &cfg.token_path {
            write_token_file(path, &token_mgr.token_hex())?;
        } else {
            log::warn!(
                "break-glass capability token rotated but no token path configured; \
                 the new token is not retrievable"
            );
        }
    }

    let token = match request.bearer_token() {
        Some(t) => t,
        None => {
            lockout.fail(peer.ip());
            return write_json(&mut stream, 401, r#"{"error":"missing_token"}"#);
        }
    };
    if token_mgr.validate(&token, now_bucket).is_err() {
        lockout.fail(peer.ip());
        return write_json(&mut stream, 401, r#"{"error":"invalid_token"}"#);
    }
    lockout.clear(&peer.ip());

    let reply = handle_break_glass(
        ops,
        session,
        &cfg.output_dir,
        now_bucket,
        &request.method,
        &request.path,
        &request.body,
    );
    write_json(&mut stream, reply.status, &reply.body)
}

/// A parsed HTTP request (method, path, headers, body).
#[derive(Debug)]
struct HttpRequest {
    method: String,
    path: String,
    raw_path: String,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

impl HttpRequest {
    fn bearer_token(&self) -> Option<String> {
        if let Some(value) = self.headers.get("authorization") {
            let parts: Vec<&str> = value.split_whitespace().collect();
            if parts.len() == 2 && parts[0].eq_ignore_ascii_case("bearer") {
                return Some(parts[1].to_string());
            }
        }
        match self.headers.get("x-witness-token") {
            Some(v) if !v.is_empty() => Some(v.to_string()),
            _ => None,
        }
    }

    fn has_query_token(&self) -> bool {
        self.raw_path
            .split('?')
            .nth(1)
            .into_iter()
            .flat_map(|q| q.split('&'))
            .filter_map(|pair| pair.split_once('='))
            .any(|(k, _)| k == "token")
    }
}

/// Read and parse one HTTP request, including a `Content-Length` body. Generic
/// over `Read` so it is unit-tested without a socket.
fn read_http_request<R: Read>(reader: &mut R) -> Result<HttpRequest> {
    let mut data = Vec::new();
    let mut buf = [0u8; 2048];
    let header_end = loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            return Err(anyhow!("connection closed before headers"));
        }
        data.extend_from_slice(&buf[..n]);
        if data.len() > MAX_REQUEST_BYTES {
            return Err(anyhow!("request too large"));
        }
        if let Some(pos) = data.windows(4).position(|w| w == b"\r\n\r\n") {
            break pos + 4;
        }
    };

    let header_text = String::from_utf8_lossy(&data[..header_end]);
    let mut lines = header_text.split("\r\n");
    let request_line = lines.next().ok_or_else(|| anyhow!("empty request"))?;
    let mut parts = request_line.split_whitespace();
    let method = parts
        .next()
        .ok_or_else(|| anyhow!("missing method"))?
        .to_string();
    let raw_path = parts
        .next()
        .ok_or_else(|| anyhow!("missing path"))?
        .to_string();

    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        if let Some((k, v)) = line.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }

    let content_length: usize = headers
        .get("content-length")
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(0);
    if content_length > MAX_REQUEST_BYTES {
        return Err(anyhow!("request body too large"));
    }

    let mut body = data[header_end..].to_vec();
    while body.len() < content_length {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&buf[..n]);
        if body.len() > MAX_REQUEST_BYTES {
            return Err(anyhow!("request body too large"));
        }
    }
    body.truncate(content_length);

    let path = raw_path.split('?').next().unwrap_or(&raw_path).to_string();
    Ok(HttpRequest {
        method,
        path,
        raw_path,
        headers,
        body,
    })
}

fn write_json(stream: &mut TcpStream, status: u16, body: &str) -> Result<()> {
    let status_line = match status {
        200 => "HTTP/1.1 200 OK",
        400 => "HTTP/1.1 400 Bad Request",
        401 => "HTTP/1.1 401 Unauthorized",
        403 => "HTTP/1.1 403 Forbidden",
        404 => "HTTP/1.1 404 Not Found",
        405 => "HTTP/1.1 405 Method Not Allowed",
        409 => "HTTP/1.1 409 Conflict",
        422 => "HTTP/1.1 422 Unprocessable Entity",
        429 => "HTTP/1.1 429 Too Many Requests",
        _ => "HTTP/1.1 500 Internal Server Error",
    };
    // Security headers mirror the event API server.
    let header = format!(
        "{status_line}\r\n\
         Content-Type: application/json\r\n\
         Content-Length: {len}\r\n\
         Cache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\n\
         X-Frame-Options: DENY\r\n\
         Referrer-Policy: no-referrer\r\n\
         Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n\
         \r\n",
        len = body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body.as_bytes())?;
    Ok(())
}

/// Serve the static operator console. A tight CSP confines it to inline
/// script/style and same-origin `fetch` — no external resources, framing, or
/// form submissions.
fn write_html(stream: &mut TcpStream, body: &str) -> Result<()> {
    let header = format!(
        "HTTP/1.1 200 OK\r\n\
         Content-Type: text/html; charset=utf-8\r\n\
         Content-Length: {len}\r\n\
         Cache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\n\
         X-Frame-Options: DENY\r\n\
         Referrer-Policy: no-referrer\r\n\
         Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n\
         Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; \
style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'\r\n\
         \r\n",
        len = body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body.as_bytes())?;
    Ok(())
}

fn write_token_file(path: &Path, token: &str) -> Result<()> {
    #[cfg(unix)]
    {
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

/// Per-IP auth failure lockout with exponential backoff, mirroring the event
/// API server's firmware-aligned constants.
struct Lockout {
    entries: HashMap<IpAddr, (u32, u32, u64)>, // failures, lockouts, locked_until_ms
}

impl Lockout {
    fn new() -> Self {
        Self {
            entries: HashMap::new(),
        }
    }

    fn now_ms() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64
    }

    fn locked(&self, ip: &IpAddr) -> Option<u64> {
        self.entries.get(ip).and_then(|&(_, _, until)| {
            let now = Self::now_ms();
            (until > now).then(|| (until - now).div_ceil(1000))
        })
    }

    /// Drop entries that are fully settled — neither currently locked nor
    /// mid-streak — so the table cannot accumulate one dead entry per source
    /// IP that ever failed once.
    fn prune(&mut self) {
        let now = Self::now_ms();
        self.entries
            .retain(|_, &mut (attempts, _, until)| attempts > 0 || until > now);
    }

    fn fail(&mut self, ip: IpAddr) {
        // Bound the table against a spoofed-source flood: without a cap, a peer
        // rotating source IPs could grow this map without limit (memory DoS).
        // The event-API rate tracker carries the same cap.
        const MAX_ENTRIES: usize = 4096;
        const MAX_ATTEMPTS: u32 = 5;
        const BASE_MS: u64 = 2_000;
        const CAP_MS: u64 = 300_000;
        self.prune();
        if !self.entries.contains_key(&ip) && self.entries.len() >= MAX_ENTRIES {
            // The table is full of live lockouts. Don't grow it; a not-yet-
            // tracked IP simply isn't rate-limited this round (the pre-existing
            // behavior for any untracked IP), which never wrongly locks a
            // legitimate peer.
            return;
        }
        let entry = self.entries.entry(ip).or_insert((0, 0, 0));
        entry.0 += 1;
        if entry.0 >= MAX_ATTEMPTS {
            entry.1 += 1;
            let backoff = BASE_MS.saturating_mul(2u64.saturating_pow(entry.1.saturating_sub(1)));
            entry.2 = Self::now_ms() + backoff.min(CAP_MS);
            entry.0 = 0;
        }
    }

    fn clear(&mut self, ip: &IpAddr) {
        self.entries.remove(ip);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn console_page_is_self_contained_and_drives_the_api() {
        assert!(BREAK_GLASS_PAGE.starts_with("<!doctype html>"));
        for path in [
            "/breakglass/policy",
            "/breakglass/request",
            "/breakglass/approve",
            "/breakglass/status",
            "/breakglass/unseal",
        ] {
            assert!(BREAK_GLASS_PAGE.contains(path), "page should call {path}");
        }
        // In-browser signer is present, and nothing is loaded from the network.
        assert!(BREAK_GLASS_PAGE.contains("Ed25519"));
        assert!(!BREAK_GLASS_PAGE.contains("http://"));
        assert!(!BREAK_GLASS_PAGE.contains("https://"));
    }

    #[test]
    fn refuses_non_loopback_without_tls() {
        let cfg = BreakGlassServerConfig {
            addr: "0.0.0.0:0".to_string(),
            ..Default::default()
        };
        let addr: SocketAddr = "203.0.113.5:8800".parse().unwrap();
        let err = validate_exposure(&addr, &cfg).unwrap_err();
        assert!(err.to_string().contains("without TLS"));
    }

    #[test]
    fn allows_loopback_without_tls() {
        let cfg = BreakGlassServerConfig::default();
        let addr: SocketAddr = "127.0.0.1:8800".parse().unwrap();
        assert!(validate_exposure(&addr, &cfg).is_ok());
    }

    #[test]
    fn parses_request_with_body() {
        let raw = b"POST /breakglass/approve HTTP/1.1\r\n\
            Host: localhost\r\n\
            Authorization: Bearer deadbeef\r\n\
            Content-Length: 13\r\n\
            \r\n\
            {\"trustee\":1}";
        let mut cur = Cursor::new(raw.to_vec());
        let req = read_http_request(&mut cur).unwrap();
        assert_eq!(req.method, "POST");
        assert_eq!(req.path, "/breakglass/approve");
        assert_eq!(req.body, b"{\"trustee\":1}");
        assert_eq!(req.bearer_token().as_deref(), Some("deadbeef"));
        assert!(!req.has_query_token());
    }

    #[test]
    fn strips_query_and_detects_token_param() {
        let raw = b"GET /breakglass/status?token=x&foo=1 HTTP/1.1\r\n\
            X-Witness-Token: abc123\r\n\
            \r\n";
        let mut cur = Cursor::new(raw.to_vec());
        let req = read_http_request(&mut cur).unwrap();
        assert_eq!(req.path, "/breakglass/status");
        assert!(req.has_query_token());
        // x-witness-token header is accepted as a bearer alternative.
        assert_eq!(req.bearer_token().as_deref(), Some("abc123"));
    }

    #[test]
    fn lockout_triggers_after_max_attempts() {
        let mut lk = Lockout::new();
        let ip: IpAddr = "127.0.0.1".parse().unwrap();
        assert!(lk.locked(&ip).is_none());
        for _ in 0..5 {
            lk.fail(ip);
        }
        assert!(
            lk.locked(&ip).is_some(),
            "5 failures should lock the IP out"
        );
        lk.clear(&ip);
        assert!(lk.locked(&ip).is_none());
    }
}
