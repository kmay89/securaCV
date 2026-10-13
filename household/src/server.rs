//! The LAN API: a small, bounded HTTP/1.1 server on `std::net`.
//!
//! | Method | Path            | Body | Result |
//! |--------|-----------------|------|--------|
//! | `POST` | `/v1/challenge` | `{"purpose": "home" \| "away" \| "night"}` | `{"challenge_id", "msg_b64", "expires_in": 10}` |
//! | `POST` | `/v1/command`   | `{"challenge_id", "handle", "signature_b64"}` (FIDO2 adds `authenticator_data_b64`) | `{"posture", "not_after"}` |
//! | `GET`  | `/v1/posture`   | — | `{"statement_b64", "posture", "not_after"}` |
//!
//! Every route needs a paired bearer token (`Authorization: Bearer …`,
//! paired with `household_checker pair`); the token's SHA-256 is the session
//! a challenge is bound to. **Every rejection on these routes is the same
//! `401` with the same body**, whether the token was wrong, the door was
//! cooling down, the challenge was used, the handle unknown, the signature
//! bad or the class not allowed: no response says whether a handle exists.
//! `not_after` is `null` for a statement that never lapses.
//!
//! Patterned on the kernel's `src/api/mod.rs`: a request is at most
//! [`MAX_REQUEST_BYTES`] including a body of at most [`MAX_BODY_BYTES`]; every
//! connection has read/write timeouts and an absolute budget; one request per
//! connection; a global request-rate cap; no chunked bodies. It binds
//! loopback unless the caller explicitly allows a LAN address. There is no
//! TLS in v0: on a LAN bind the bearer token and the posture cross the
//! network in cleartext (a signature still has to come from an enrolled key).
//!
//! The server logs nothing per request: no handles, no credential ids, no
//! per-command lines. Its only log lines are startup, shutdown and storage
//! failures.

use crate::challenge::{SessionId, CHALLENGE_TTL_SECS};
use crate::checker::{Checker, CommandRequest};
use crate::posture::{Statement, NEVER};
use crate::roster::Roster;
use crate::secrets::CardStore;
use crate::wire::{Purpose, LAN_DOOR_ID};
use crate::{hex, Error, Result};
use base64::engine::general_purpose::STANDARD as B64;
use base64::Engine;
use serde::Deserialize;
use serde_json::json;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Largest request (head and body) read.
pub const MAX_REQUEST_BYTES: usize = 8192;
/// Largest body accepted.
pub const MAX_BODY_BYTES: usize = 4096;
/// Per-operation socket timeout.
pub const IO_TIMEOUT: Duration = Duration::from_secs(3);
/// Absolute budget for one connection.
pub const CONNECTION_BUDGET: Duration = Duration::from_secs(5);
/// Most requests served per minute (all clients together).
pub const MAX_REQUESTS_PER_MINUTE: usize = 600;
/// The one rejection body.
pub const REJECTED_BODY: &str = r#"{"error":"rejected"}"#;

/// Where log lines go.
pub type LogSink = Arc<dyn Fn(&str) + Send + Sync>;

/// A log sink that writes to standard error.
pub fn stderr_log() -> LogSink {
    Arc::new(|line: &str| eprintln!("household_checker: {line}"))
}

/// The server's clocks.
pub trait Clock: Send + Sync {
    /// Monotonic milliseconds.
    fn mono_ms(&self) -> u64;
    /// Wall clock, Unix seconds.
    fn unix(&self) -> u64;
}

/// The system clocks.
#[derive(Debug)]
pub struct SystemClock(Instant);

impl Default for SystemClock {
    fn default() -> Self {
        SystemClock(Instant::now())
    }
}

impl Clock for SystemClock {
    fn mono_ms(&self) -> u64 {
        self.0.elapsed().as_millis() as u64
    }
    fn unix(&self) -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0)
    }
}

/// Changes picked up between requests.
#[derive(Default)]
pub struct Reload {
    /// A newer roster that passed the chain checks.
    pub roster: Option<Roster>,
    /// A newer card store.
    pub cards: Option<CardStore>,
    /// A newer set of bearer hashes.
    pub bearers: Option<Vec<[u8; 32]>>,
}

/// Something that can report roster, card and pairing changes.
pub trait ReloadSource: Send {
    /// Check for changes. Errors are logged and the old state is kept.
    fn poll(&mut self) -> Result<Reload>;
}

/// Server configuration.
#[derive(Clone, Debug)]
pub struct ServerConfig {
    /// Address to bind.
    pub bind: SocketAddr,
    /// Allow a non-loopback bind (cleartext on the LAN).
    pub allow_lan: bool,
}

struct State {
    checker: Checker,
    bearers: Vec<[u8; 32]>,
    reload: Option<Box<dyn ReloadSource>>,
    window: std::collections::VecDeque<u64>,
}

/// A bound server.
pub struct Server {
    listener: TcpListener,
    state: Arc<Mutex<State>>,
    clock: Arc<dyn Clock>,
    log: LogSink,
}

/// A running server thread.
pub struct ServerHandle {
    addr: SocketAddr,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl ServerHandle {
    /// The bound address.
    pub fn addr(&self) -> SocketAddr {
        self.addr
    }

    /// Stop and join.
    pub fn stop(mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Server {
    /// Bind. Refuses a non-loopback address unless `allow_lan`.
    pub fn bind(
        cfg: &ServerConfig,
        checker: Checker,
        bearers: Vec<[u8; 32]>,
        reload: Option<Box<dyn ReloadSource>>,
        clock: Arc<dyn Clock>,
        log: LogSink,
    ) -> Result<Server> {
        if !cfg.bind.ip().is_loopback() && !cfg.allow_lan {
            return Err(Error::Invalid(
                "refusing a non-loopback bind without --allow-lan-plaintext",
            ));
        }
        let listener = TcpListener::bind(cfg.bind).map_err(Error::io("bind the LAN API"))?;
        listener
            .set_nonblocking(true)
            .map_err(Error::io("set the listener nonblocking"))?;
        Ok(Server {
            listener,
            state: Arc::new(Mutex::new(State {
                checker,
                bearers,
                reload,
                window: Default::default(),
            })),
            clock,
            log,
        })
    }

    /// The bound address.
    pub fn local_addr(&self) -> Result<SocketAddr> {
        self.listener
            .local_addr()
            .map_err(Error::io("read the bound address"))
    }

    /// Run until `stop` is set.
    pub fn run(&self, stop: &AtomicBool) {
        if let Ok(a) = self.local_addr() {
            let scope = if a.ip().is_loopback() {
                "loopback"
            } else {
                "LAN (cleartext)"
            };
            (self.log)(&format!("LAN API listening on {scope} port {}", a.port()));
        }
        let mut last_tick = Instant::now() - Duration::from_secs(10);
        while !stop.load(Ordering::SeqCst) {
            if last_tick.elapsed() >= Duration::from_secs(1) {
                last_tick = Instant::now();
                self.housekeeping();
            }
            match self.listener.accept() {
                Ok((stream, _peer)) => {
                    // Errors on one connection are that connection's problem
                    // and are not logged (a log line per request is exactly
                    // what this server must not write).
                    let _ = self.handle(stream);
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(20));
                }
                Err(_) => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        (self.log)("LAN API stopped");
    }

    /// Run on a new thread.
    pub fn spawn(self) -> Result<ServerHandle> {
        let addr = self.local_addr()?;
        let stop = Arc::new(AtomicBool::new(false));
        let s2 = stop.clone();
        let thread = std::thread::Builder::new()
            .name("household-lan-api".into())
            .spawn(move || self.run(&s2))
            .map_err(Error::io("spawn the server thread"))?;
        Ok(ServerHandle {
            addr,
            stop,
            thread: Some(thread),
        })
    }

    fn housekeeping(&self) {
        let now = self.clock.unix();
        let mut st = match self.state.lock() {
            Ok(g) => g,
            Err(p) => p.into_inner(),
        };
        if let Some(src) = st.reload.as_mut() {
            match src.poll() {
                Ok(r) => {
                    if let Some(roster) = r.roster {
                        if st.checker.set_roster(roster).is_err() {
                            (self.log)("refused a reloaded roster");
                        }
                    }
                    if let Some(c) = r.cards {
                        st.checker.set_cards(c);
                    }
                    if let Some(b) = r.bearers {
                        st.bearers = b;
                    }
                }
                Err(_) => {
                    (self.log)("reload failed; keeping the previous roster, cards and pairings")
                }
            }
        }
        if st.checker.tick(now).is_err() {
            (self.log)("failed to publish a lapse statement");
        }
    }

    fn handle(&self, stream: TcpStream) -> Result<()> {
        let deadline = Instant::now() + CONNECTION_BUDGET;
        stream
            .set_nonblocking(false)
            .map_err(Error::io("set blocking"))?;
        stream
            .set_read_timeout(Some(IO_TIMEOUT))
            .map_err(Error::io("set timeout"))?;
        stream
            .set_write_timeout(Some(IO_TIMEOUT))
            .map_err(Error::io("set timeout"))?;
        let mut stream = stream;
        let (status, body) = match read_request(&mut stream, deadline) {
            Ok(req) => self.route(&req),
            Err(RequestError::TooLarge) => (413, r#"{"error":"too_large"}"#.to_string()),
            Err(RequestError::Bad) => (400, r#"{"error":"bad_request"}"#.to_string()),
            Err(RequestError::Io) => return Ok(()),
        };
        write_response(&mut stream, status, &body)
    }

    fn route(&self, req: &Request) -> (u16, String) {
        let known = matches!(
            req.path.as_str(),
            "/v1/challenge" | "/v1/command" | "/v1/posture"
        );
        if !known {
            return (404, r#"{"error":"not_found"}"#.into());
        }
        let method_ok = matches!(
            (req.method.as_str(), req.path.as_str()),
            ("POST", "/v1/challenge") | ("POST", "/v1/command") | ("GET", "/v1/posture")
        );
        if !method_ok {
            return (405, r#"{"error":"method_not_allowed"}"#.into());
        }
        let now_ms = self.clock.mono_ms();
        let now_unix = self.clock.unix();
        let mut st = match self.state.lock() {
            Ok(g) => g,
            Err(p) => p.into_inner(),
        };
        // Global request cap.
        while st
            .window
            .front()
            .is_some_and(|t| now_ms.saturating_sub(*t) >= 60_000)
        {
            st.window.pop_front();
        }
        if st.window.len() >= MAX_REQUESTS_PER_MINUTE {
            return (401, REJECTED_BODY.into());
        }
        st.window.push_back(now_ms);
        let session = req
            .bearer
            .as_deref()
            .and_then(|t| crate::store::match_bearer(&st.bearers, t));
        let Some(session) = session else {
            st.checker.record_rejection(&LAN_DOOR_ID, now_ms);
            return (401, REJECTED_BODY.into());
        };
        let out = match req.path.as_str() {
            "/v1/challenge" => challenge(&mut st.checker, &req.body, session, now_ms),
            "/v1/command" => command(&mut st.checker, &req.body, session, now_ms, now_unix),
            _ => Ok(posture_body(&st.checker)),
        };
        match out {
            Ok(body) => (200, body),
            Err(RouteError::Rejected) => (401, REJECTED_BODY.into()),
            Err(RouteError::Malformed) => {
                st.checker.record_rejection(&LAN_DOOR_ID, now_ms);
                (401, REJECTED_BODY.into())
            }
        }
    }
}

enum RouteError {
    /// The checker refused (and already counted it).
    Rejected,
    /// The body did not parse (counted here).
    Malformed,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ChallengeBody {
    purpose: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CommandBody {
    challenge_id: String,
    handle: String,
    signature_b64: String,
    #[serde(default)]
    authenticator_data_b64: Option<String>,
}

fn challenge(
    c: &mut Checker,
    body: &[u8],
    session: SessionId,
    now_ms: u64,
) -> std::result::Result<String, RouteError> {
    let b: ChallengeBody = serde_json::from_slice(body).map_err(|_| RouteError::Malformed)?;
    let purpose = Purpose::from_word(&b.purpose).map_err(|_| RouteError::Malformed)?;
    let ch = c
        .issue_challenge(purpose, LAN_DOOR_ID, session, now_ms)
        .map_err(|_| RouteError::Rejected)?;
    Ok(json!({
        "challenge_id": hex::encode(&ch.id),
        "msg_b64": B64.encode(&ch.msg),
        "expires_in": CHALLENGE_TTL_SECS,
    })
    .to_string())
}

fn not_after_json(st: &Statement) -> serde_json::Value {
    if st.not_after == NEVER {
        serde_json::Value::Null
    } else {
        json!(st.not_after)
    }
}

fn command(
    c: &mut Checker,
    body: &[u8],
    session: SessionId,
    now_ms: u64,
    now_unix: u64,
) -> std::result::Result<String, RouteError> {
    let b: CommandBody = serde_json::from_slice(body).map_err(|_| RouteError::Malformed)?;
    let req = (|| {
        Some(CommandRequest {
            challenge_id: hex::decode_array(&b.challenge_id).ok()?,
            handle: hex::decode_array(&b.handle).ok()?,
            signature: B64.decode(&b.signature_b64).ok()?,
            authenticator_data: match &b.authenticator_data_b64 {
                Some(a) => Some(B64.decode(a).ok()?),
                None => None,
            },
        })
    })()
    .ok_or(RouteError::Malformed)?;
    let st = c
        .command(&req, &session, &LAN_DOOR_ID, now_ms, now_unix)
        .map_err(|_| RouteError::Rejected)?;
    Ok(json!({"posture": st.posture.word(), "not_after": not_after_json(&st)}).to_string())
}

fn posture_body(c: &Checker) -> String {
    let st = c.current();
    json!({
        "statement_b64": B64.encode(c.current_bytes()),
        "posture": st.posture.word(),
        "not_after": not_after_json(&st),
    })
    .to_string()
}

/// A parsed request.
#[derive(Debug)]
struct Request {
    method: String,
    path: String,
    bearer: Option<String>,
    body: Vec<u8>,
}

enum RequestError {
    TooLarge,
    Bad,
    Io,
}

fn find_head_end(b: &[u8]) -> Option<usize> {
    b.windows(4).position(|w| w == b"\r\n\r\n")
}

fn read_request(
    stream: &mut TcpStream,
    deadline: Instant,
) -> std::result::Result<Request, RequestError> {
    let mut data: Vec<u8> = Vec::with_capacity(1024);
    let mut buf = [0u8; 1024];
    let head_end = loop {
        if let Some(i) = find_head_end(&data) {
            break i;
        }
        if data.len() > MAX_REQUEST_BYTES {
            return Err(RequestError::TooLarge);
        }
        if Instant::now() >= deadline {
            return Err(RequestError::Io);
        }
        match stream.read(&mut buf) {
            Ok(0) => return Err(RequestError::Io),
            Ok(n) => data.extend_from_slice(&buf[..n]),
            Err(_) => return Err(RequestError::Io),
        }
    };
    let head = std::str::from_utf8(&data[..head_end]).map_err(|_| RequestError::Bad)?;
    let mut lines = head.split("\r\n");
    let request_line = lines.next().ok_or(RequestError::Bad)?;
    let mut parts = request_line.split(' ');
    let method = parts.next().ok_or(RequestError::Bad)?.to_string();
    let path = parts.next().ok_or(RequestError::Bad)?.to_string();
    let version = parts.next().ok_or(RequestError::Bad)?;
    if parts.next().is_some() || !(version == "HTTP/1.1" || version == "HTTP/1.0") {
        return Err(RequestError::Bad);
    }
    let mut content_length: Option<usize> = None;
    let mut bearer = None;
    for line in lines {
        let (name, value) = line.split_once(':').ok_or(RequestError::Bad)?;
        let name = name.trim().to_ascii_lowercase();
        let value = value.trim();
        match name.as_str() {
            "content-length" => {
                if content_length.is_some() {
                    return Err(RequestError::Bad);
                }
                content_length = Some(value.parse().map_err(|_| RequestError::Bad)?);
            }
            "transfer-encoding" => return Err(RequestError::Bad),
            "authorization" => {
                if bearer.is_some() {
                    return Err(RequestError::Bad);
                }
                bearer = value.strip_prefix("Bearer ").map(|t| t.trim().to_string());
            }
            _ => {}
        }
    }
    let len = content_length.unwrap_or(0);
    if len > MAX_BODY_BYTES || head_end + 4 + len > MAX_REQUEST_BYTES {
        return Err(RequestError::TooLarge);
    }
    let mut body = data[head_end + 4..].to_vec();
    if body.len() > len {
        return Err(RequestError::Bad);
    }
    while body.len() < len {
        if Instant::now() >= deadline {
            return Err(RequestError::Io);
        }
        match stream.read(&mut buf) {
            Ok(0) => return Err(RequestError::Io),
            Ok(n) => body.extend_from_slice(&buf[..n]),
            Err(_) => return Err(RequestError::Io),
        }
        if body.len() > len {
            return Err(RequestError::Bad);
        }
    }
    Ok(Request {
        method,
        path,
        bearer,
        body,
    })
}

fn write_response(stream: &mut TcpStream, status: u16, body: &str) -> Result<()> {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        _ => "Error",
    };
    let head = format!(
        "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream
        .write_all(head.as_bytes())
        .and_then(|_| stream.write_all(body.as_bytes()))
        .and_then(|_| stream.flush())
        .map_err(Error::io("write a response"))
}

/// (length, mtime) of a watched file.
type FileStamp = (u64, SystemTime);
/// What [`DirReload`] compares between polls: the newest chain file name,
/// the card store's stamp and the pairing file's stamp.
type Stamp = (Option<String>, Option<FileStamp>, Option<FileStamp>);

/// Reloads the roster chain, the sealed card store and the pairings from a
/// data directory when their files change.
pub struct DirReload {
    dir: crate::store::DataDir,
    kek: crate::secrets::Kek,
    seen: Stamp,
}

impl DirReload {
    /// Watch `dir`, opening the card store with `kek`.
    pub fn new(dir: crate::store::DataDir, kek: crate::secrets::Kek) -> Self {
        let mut r = DirReload {
            dir,
            kek,
            seen: (None, None, None),
        };
        r.seen = r.stamp();
        r
    }

    fn file_stamp(p: &std::path::Path) -> Option<FileStamp> {
        let m = std::fs::metadata(p).ok()?;
        Some((m.len(), m.modified().ok()?))
    }

    fn stamp(&self) -> Stamp {
        let last = std::fs::read_dir(self.dir.path("roster/chain"))
            .ok()
            .and_then(|it| {
                it.filter_map(|e| e.ok())
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .max()
            });
        (
            last,
            Self::file_stamp(&self.dir.cards_path()),
            Self::file_stamp(&self.dir.path("bearers.json")),
        )
    }
}

impl ReloadSource for DirReload {
    fn poll(&mut self) -> Result<Reload> {
        let now = self.stamp();
        let mut out = Reload::default();
        if now.0 != self.seen.0 {
            out.roster = self.dir.current_roster()?;
        }
        if now.1 != self.seen.1 {
            let p = self.dir.cards_path();
            let household = self.dir.household()?;
            out.cards = Some(if p.exists() {
                let b = crate::store::read_bounded(&p, crate::secrets::MAX_SEALED_BYTES)?;
                CardStore::open(&b, &self.kek, &household)?
            } else {
                CardStore::new()
            });
        }
        if now.2 != self.seen.2 {
            out.bearers = Some(self.dir.bearer_hashes()?);
        }
        self.seen = now;
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::checker::tests::roster;
    use crate::checker::MemoryPersist;

    fn checker() -> Checker {
        Checker::new(
            ed25519_dalek::SigningKey::from_bytes(&[9; 32]),
            roster(),
            CardStore::new(),
            0,
            3600,
            Box::new(MemoryPersist::default()),
            crate::checker::tests::T0,
        )
        .unwrap()
    }

    #[test]
    fn refuses_lan_bind_without_the_flag() {
        let cfg = ServerConfig {
            bind: "0.0.0.0:0".parse().unwrap(),
            allow_lan: false,
        };
        let r = Server::bind(
            &cfg,
            checker(),
            vec![],
            None,
            Arc::new(SystemClock::default()),
            stderr_log(),
        );
        assert!(r.is_err());
    }

    fn raw(addr: SocketAddr, req: &[u8]) -> String {
        let mut s = TcpStream::connect(addr).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        s.write_all(req).unwrap();
        let mut out = String::new();
        let _ = s.read_to_string(&mut out);
        out
    }

    #[test]
    fn bounded_and_uniform() {
        let cfg = ServerConfig {
            bind: "127.0.0.1:0".parse().unwrap(),
            allow_lan: false,
        };
        let token = "t0ken";
        let hash: [u8; 32] =
            sha2::Digest::finalize(<sha2::Sha256 as sha2::Digest>::new_with_prefix(token)).into();
        let srv = Server::bind(
            &cfg,
            checker(),
            vec![hash],
            None,
            Arc::new(SystemClock::default()),
            Arc::new(|_: &str| {}),
        )
        .unwrap();
        let h = srv.spawn().unwrap();
        let a = h.addr();
        // Unknown path, wrong method, malformed request line.
        assert!(raw(a, b"GET /nope HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404"));
        assert!(raw(a, b"GET /v1/command HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 405"));
        assert!(raw(a, b"garbage\r\n\r\n").starts_with("HTTP/1.1 400"));
        // Oversized body declaration and oversized head.
        assert!(raw(
            a,
            b"POST /v1/command HTTP/1.1\r\nContent-Length: 999999\r\n\r\n"
        )
        .starts_with("HTTP/1.1 413"));
        let mut big = b"GET /v1/posture HTTP/1.1\r\nX: ".to_vec();
        big.extend(std::iter::repeat_n(b'a', MAX_REQUEST_BYTES + 10));
        assert!(raw(a, &big).starts_with("HTTP/1.1 413"));
        // Chunked bodies are refused.
        assert!(raw(
            a,
            b"POST /v1/command HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        )
        .starts_with("HTTP/1.1 400"));
        // No token, wrong token, malformed body with a good token: one 401 body.
        let r1 = raw(a, b"GET /v1/posture HTTP/1.1\r\n\r\n");
        let r2 = raw(
            a,
            b"GET /v1/posture HTTP/1.1\r\nAuthorization: Bearer nope\r\n\r\n",
        );
        let r3 = raw(
            a,
            b"POST /v1/command HTTP/1.1\r\nAuthorization: Bearer t0ken\r\nContent-Length: 2\r\n\r\n{}",
        );
        for r in [&r1, &r2, &r3] {
            assert!(r.starts_with("HTTP/1.1 401"), "{r}");
            assert!(r.ends_with(REJECTED_BODY), "{r}");
        }
        h.stop();
    }
}
