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
//! a challenge is bound to. A token cannot change the posture, but it does
//! read it: `GET /v1/posture` tells its holder the household posture and the
//! minute it last changed. `pair --reset` unpairs every client while the
//! server runs. **Every rejection on these routes is the same `401` with the
//! same body**, whether the token was wrong, `home` was refused during a
//! cooldown, the challenge was used, the handle unknown, the signature bad or
//! the class not allowed: no response says whether a handle exists.
//! `not_after` is `null` for a statement that never lapses.
//!
//! A missing or wrong token is refused before anything is counted: it never
//! reaches the request cap or the door limiter, so a client with no token
//! cannot use up the request cap or start a cooldown (it can still take
//! connection slots, below). A paired client's body that does not parse is
//! charged to the request cap but never reaches the door limiter. Only a
//! response is counted toward a cooldown, from the challenge lookup on
//! ([`crate::ratelimit`]), and even then a cooldown refuses only `home`.
//!
//! Patterned on the kernel's `src/api/mod.rs`: a request is at most
//! [`MAX_REQUEST_BYTES`] including a body of at most [`MAX_BODY_BYTES`]; every
//! connection has an absolute budget of [`CONNECTION_BUDGET`] to send its
//! request, each read timeout cut to what is left, and its response is
//! written by the end of that budget or [`RESPONSE_WINDOW`] after routing,
//! whichever is later (the request may already have changed the posture);
//! one request per connection; a request-rate
//! cap for paired clients; no chunked bodies. Connections are served on
//! their own threads, at most [`MAX_CONNECTIONS`] at once and
//! [`MAX_CONNECTIONS_PER_PEER`] from one peer (an IPv4 address or an IPv6
//! /64). When a newcomer finds a cap reached, the oldest connection that has
//! not finished sending its request is shut down to make room, so idle or
//! trickling connections never keep out a client that sends its request at
//! once; a newcomer is turned away only when every counted connection is
//! already serving a whole request. Requests reach the checker in the order
//! of their timestamps: the clock is read under the state lock.
//!
//! What a network attacker can still do: a host that opens connections as
//! fast as the accept loop takes them can still contend with a client for a
//! slot, and anyone holding a paired token can spend the shared request cap
//! (refusing every route for a minute) or start a signature-lane cooldown
//! (refusing `home`). Each holds the current posture; a `home` still lapses
//! on time, on both the wall and the monotonic clock, so a short
//! `--home-ttl` bounds it.
//!
//! It binds loopback unless the caller explicitly allows a LAN address, and
//! on any bind it answers only peers on the home network
//! ([`is_home_network`]): loopback, private and link-local addresses. A peer
//! from anywhere else, Tailscale's ranges included (the IPv4 shared address
//! space and its IPv6 `fd7a:115c:a1e0::/48`), is dropped at accept. A proxy,
//! VPN or ingress that re-originates traffic from a private address defeats
//! this, so the LAN API must not sit behind one.
//!
//! There is no TLS in v0: on a LAN bind every request crosses the network in
//! cleartext, including each client's bearer token (stable per client) and,
//! in a command, the member handle. A passive listener on the LAN can
//! therefore link acts to a device and to a member. A signature still has to
//! come from an enrolled key.
//!
//! The server logs nothing per request: no handles, no credential ids, no
//! per-command lines, no peer addresses. Its only log lines are startup,
//! shutdown, reload trouble and storage failures.

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
use std::net::{IpAddr, SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Largest request (head and body) read.
pub const MAX_REQUEST_BYTES: usize = 8192;
/// Largest body accepted.
pub const MAX_BODY_BYTES: usize = 4096;
/// Longest a single read or write may block (a read is always cut to what is
/// left of the connection budget; the write, to its own deadline).
pub const IO_TIMEOUT: Duration = Duration::from_secs(3);
/// Absolute budget for one connection to send its request. The response's
/// write gets until this budget ends or [`RESPONSE_WINDOW`] after routing,
/// whichever is later.
pub const CONNECTION_BUDGET: Duration = Duration::from_secs(5);
/// Most requests served per minute (all paired clients together).
pub const MAX_REQUESTS_PER_MINUTE: usize = 600;
/// Most connections served at once.
pub const MAX_CONNECTIONS: usize = 16;
/// Most connections served at once for one peer: one IPv4 address, or one
/// IPv6 /64.
pub const MAX_CONNECTIONS_PER_PEER: usize = 4;
/// How long a response may take to write once its request has been routed,
/// even past [`CONNECTION_BUDGET`].
pub const RESPONSE_WINDOW: Duration = Duration::from_secs(1);
/// The one rejection body.
pub const REJECTED_BODY: &str = r#"{"error":"rejected"}"#;

/// Where log lines go.
pub type LogSink = Arc<dyn Fn(&str) + Send + Sync>;

/// A log sink that writes to standard error.
pub fn stderr_log() -> LogSink {
    Arc::new(|line: &str| eprintln!("household_checker: {line}"))
}

/// Whether `ip` is on the home network: loopback, a private IPv4 range (RFC
/// 1918), IPv4 link-local, or an IPv6 unique-local or link-local address
/// (IPv4-mapped IPv6 counts as its IPv4 address). Everything else is
/// refused, the shared address space 100.64.0.0/10 (Tailscale, carrier NAT)
/// included, so from outside the home network a key does nothing even on a
/// LAN bind. That is enforced by peer address: a strong default, not a
/// cryptographic guarantee, since a proxy, VPN or ingress that re-originates
/// traffic from a private or loopback address defeats it. A home LAN reached
/// only over public IPv6 addresses is refused too: bind an IPv4 LAN address.
pub fn is_home_network(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => v4.is_loopback() || v4.is_private() || v4.is_link_local(),
        IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
            Some(v4) => is_home_network(IpAddr::V4(v4)),
            None => {
                (v6.is_loopback() || v6.is_unique_local() || v6.is_unicast_link_local())
                    && !is_tailscale_v6(&v6)
            }
        },
    }
}

/// Tailscale's IPv6 range, `fd7a:115c:a1e0::/48`: unique-local in form, but
/// a tailnet reaches it from anywhere, so it is not the home network.
fn is_tailscale_v6(v6: &std::net::Ipv6Addr) -> bool {
    let s = v6.segments();
    s[0] == 0xfd7a && s[1] == 0x115c && s[2] == 0xa1e0
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

/// Changes picked up between requests. Each part is `None` when its file did
/// not change, and an error when it changed but did not load; a part that
/// fails never holds up another.
#[derive(Default)]
pub struct Reload {
    /// A newer roster that passed the chain checks, or why the changed chain
    /// did not load. On an error the checker refuses `home` until a roster
    /// verifies ([`Checker::mark_roster_stale`]); `away` and `night` keep
    /// working against the last roster that verified.
    pub roster: Option<Result<Roster>>,
    /// A newer card store, or why it did not open (the old keys are kept;
    /// revocation does not depend on them, the roster does).
    pub cards: Option<Result<CardStore>>,
    /// A newer set of bearer hashes, or why they did not load (the old
    /// pairings are kept).
    pub bearers: Option<Result<Vec<[u8; 32]>>>,
}

/// Something that can report roster, card and pairing changes.
pub trait ReloadSource: Send {
    /// Check for changes. A part that failed to load is reported again on the
    /// next poll until it loads.
    fn poll(&mut self) -> Reload;
}

/// Server configuration.
#[derive(Clone, Debug)]
pub struct ServerConfig {
    /// Address to bind.
    pub bind: SocketAddr,
    /// Allow a non-loopback bind (cleartext on the LAN).
    pub allow_lan: bool,
}

/// Which reload parts are failing (so trouble is logged once, not every
/// second).
#[derive(Default)]
struct ReloadTrouble {
    roster: bool,
    cards: bool,
    bearers: bool,
}

/// Log `line` only when `failing` was not already set, and set it.
fn first_trouble(failing: &mut bool, log: &LogSink, line: &str) {
    if !std::mem::replace(failing, true) {
        log(line);
    }
}

struct State {
    checker: Checker,
    bearers: Vec<[u8; 32]>,
    reload: Option<Box<dyn ReloadSource>>,
    trouble: ReloadTrouble,
    window: std::collections::VecDeque<u64>,
}

/// What the accept loop and the connection threads share.
struct Shared {
    state: Mutex<State>,
    clock: Arc<dyn Clock>,
    log: LogSink,
    /// Open connections. Never more than [`MAX_CONNECTIONS`] entries, and
    /// never logged.
    conns: Mutex<ConnTable>,
}

/// Which connections count together toward [`MAX_CONNECTIONS_PER_PEER`]:
/// one IPv4 address, or one IPv6 /64 (a single IPv6 host can hold any
/// number of addresses in its /64).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum PeerGroup {
    V4(std::net::Ipv4Addr),
    V6Prefix(u64),
}

impl PeerGroup {
    fn of(ip: IpAddr) -> PeerGroup {
        match ip {
            IpAddr::V4(v4) => PeerGroup::V4(v4),
            IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
                Some(v4) => PeerGroup::V4(v4),
                None => PeerGroup::V6Prefix((v6.to_bits() >> 64) as u64),
            },
        }
    }
}

/// One open connection.
struct ConnEntry {
    id: u64,
    group: PeerGroup,
    /// Set once the whole request has arrived. Until then the connection is
    /// the one evicted when a newcomer finds the caps reached.
    request_read: Arc<AtomicBool>,
    /// A handle to shut the connection down from outside its thread.
    stream: TcpStream,
}

#[derive(Default)]
struct ConnTable {
    next_id: u64,
    entries: Vec<ConnEntry>,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    match m.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    }
}

/// A connection slot, released on drop.
struct Slot {
    shared: Arc<Shared>,
    id: u64,
    request_read: Arc<AtomicBool>,
}

impl Slot {
    /// Admit a connection from `ip`. When a cap is reached, the oldest
    /// connection that has not finished sending its request (within the
    /// peer's group when the per-peer cap is the one reached, otherwise
    /// overall) is shut down to make room: an idle or trickling connection
    /// never keeps out a client that sends its request at once, which is
    /// what a paired client does. Only when every counted connection is
    /// already serving a whole request is the newcomer turned away.
    fn take(shared: &Arc<Shared>, ip: IpAddr, stream: &TcpStream) -> Option<Slot> {
        let handle = stream.try_clone().ok()?;
        let group = PeerGroup::of(ip);
        let mut t = lock(&shared.conns);
        let mine = t.entries.iter().filter(|e| e.group == group).count();
        let scope_is_group = mine >= MAX_CONNECTIONS_PER_PEER;
        if scope_is_group || t.entries.len() >= MAX_CONNECTIONS {
            // Entries are in admission order, so the first match is the
            // oldest.
            let victim = t.entries.iter().position(|e| {
                !e.request_read.load(Ordering::SeqCst) && (!scope_is_group || e.group == group)
            })?;
            let e = t.entries.remove(victim);
            let _ = e.stream.shutdown(std::net::Shutdown::Both);
            if t.entries.iter().filter(|e| e.group == group).count() >= MAX_CONNECTIONS_PER_PEER
                || t.entries.len() >= MAX_CONNECTIONS
            {
                return None;
            }
        }
        t.next_id += 1;
        let id = t.next_id;
        let request_read = Arc::new(AtomicBool::new(false));
        t.entries.push(ConnEntry {
            id,
            group,
            request_read: request_read.clone(),
            stream: handle,
        });
        Some(Slot {
            shared: shared.clone(),
            id,
            request_read,
        })
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        let mut t = lock(&self.shared.conns);
        // An evicted connection's entry is already gone.
        t.entries.retain(|e| e.id != self.id);
    }
}

/// A bound server.
pub struct Server {
    listener: TcpListener,
    shared: Arc<Shared>,
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
            shared: Arc::new(Shared {
                state: Mutex::new(State {
                    checker,
                    bearers,
                    reload,
                    trouble: ReloadTrouble::default(),
                    window: Default::default(),
                }),
                clock,
                log,
                conns: Mutex::new(ConnTable::default()),
            }),
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
            (self.shared.log)(&format!("LAN API listening on {scope} port {}", a.port()));
        }
        let mut last_tick = Instant::now() - Duration::from_secs(10);
        while !stop.load(Ordering::SeqCst) {
            if last_tick.elapsed() >= Duration::from_secs(1) {
                last_tick = Instant::now();
                self.shared.housekeeping();
            }
            match self.listener.accept() {
                Ok((stream, peer)) => self.admit(stream, peer),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(20));
                }
                Err(_) => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        (self.shared.log)("LAN API stopped");
    }

    /// Hand an accepted connection to its own thread, or drop it: a peer
    /// off the home network, or one over the connection caps, is closed
    /// without a response. Errors on one connection are that connection's
    /// problem and are not logged (a log line per request is exactly what
    /// this server must not write).
    fn admit(&self, stream: TcpStream, peer: SocketAddr) {
        if !is_home_network(peer.ip()) {
            return;
        }
        let Some(slot) = Slot::take(&self.shared, peer.ip(), &stream) else {
            return;
        };
        let shared = self.shared.clone();
        // If the thread cannot be spawned, the closure (slot and stream) is
        // dropped: the slot is released and the connection closed.
        let _ = std::thread::Builder::new()
            .name("household-lan-conn".into())
            .spawn(move || {
                let _ = shared.handle(stream, &slot.request_read);
                drop(slot);
            });
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
}

impl Shared {
    fn housekeeping(&self) {
        let mut guard = lock(&self.state);
        let now = self.clock.unix();
        let now_ms = self.clock.mono_ms();
        let st = &mut *guard;
        if let Some(src) = st.reload.as_mut() {
            let r = src.poll();
            if let Some(roster) = r.roster {
                let ok = roster.and_then(|roster| st.checker.set_roster(roster));
                match ok {
                    Ok(()) => {
                        if std::mem::take(&mut st.trouble.roster) {
                            (self.log)("the roster on disk verifies again; home is accepted");
                        }
                    }
                    Err(_) => {
                        st.checker.mark_roster_stale();
                        if !std::mem::replace(&mut st.trouble.roster, true) {
                            (self.log)(
                                "the roster on disk did not load or verify; refusing home until it does",
                            );
                        }
                    }
                }
            }
            match r.cards {
                Some(Ok(c)) => {
                    st.checker.set_cards(c);
                    st.trouble.cards = false;
                }
                Some(Err(_)) => first_trouble(
                    &mut st.trouble.cards,
                    &self.log,
                    "the card store did not reload; keeping the previous card keys",
                ),
                None => {}
            }
            match r.bearers {
                Some(Ok(b)) => {
                    st.bearers = b;
                    st.trouble.bearers = false;
                }
                Some(Err(_)) => first_trouble(
                    &mut st.trouble.bearers,
                    &self.log,
                    "the pairings did not reload; keeping the previous pairings",
                ),
                None => {}
            }
        }
        if st.checker.tick(now, now_ms).is_err() {
            (self.log)("failed to publish a lapse statement");
        }
    }

    fn handle(&self, stream: TcpStream, request_read: &AtomicBool) -> Result<()> {
        let deadline = Instant::now() + CONNECTION_BUDGET;
        stream
            .set_nonblocking(false)
            .map_err(Error::io("set blocking"))?;
        let mut stream = stream;
        let read = read_request(&mut stream, deadline);
        request_read.store(true, Ordering::SeqCst);
        let (status, body) = match read {
            Ok(req) => self.route(&req),
            Err(RequestError::TooLarge) => (413, r#"{"error":"too_large"}"#.to_string()),
            Err(RequestError::Bad) => (400, r#"{"error":"bad_request"}"#.to_string()),
            Err(RequestError::Io) => return Ok(()),
        };
        // The request may already have changed the posture, so its response
        // gets a write window of its own even if reading used up the budget
        // (it is a few hundred bytes: it fits the socket buffer).
        let write_by = deadline.max(Instant::now() + RESPONSE_WINDOW);
        write_response(&mut stream, status, &body, write_by)
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
        // The clock is read under the lock, so requests reach the checker in
        // the order of their timestamps (connections run on their own
        // threads).
        let mut guard = lock(&self.state);
        let now_ms = self.clock.mono_ms();
        let now_unix = self.clock.unix();
        let st = &mut *guard;
        // Paired clients only, before anything is counted.
        let session = req
            .bearer
            .as_deref()
            .and_then(|t| crate::store::match_bearer(&st.bearers, t));
        let Some(session) = session else {
            return (401, REJECTED_BODY.into());
        };
        // Request cap, paired clients together.
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
        let out = match req.path.as_str() {
            "/v1/challenge" => challenge(&mut st.checker, &req.body, session, now_ms),
            "/v1/command" => command(&mut st.checker, &req.body, session, now_ms, now_unix),
            _ => Ok(posture_body(&st.checker)),
        };
        match out {
            Ok(body) => (200, body),
            Err(Refused) => (401, REJECTED_BODY.into()),
        }
    }
}

/// A route refused the request (the checker counted it if it was a response
/// to a live challenge; nothing else is counted).
struct Refused;

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
) -> std::result::Result<String, Refused> {
    let b: ChallengeBody = serde_json::from_slice(body).map_err(|_| Refused)?;
    let purpose = Purpose::from_word(&b.purpose).map_err(|_| Refused)?;
    let ch = c
        .issue_challenge(purpose, LAN_DOOR_ID, session, now_ms)
        .map_err(|_| Refused)?;
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
) -> std::result::Result<String, Refused> {
    let b: CommandBody = serde_json::from_slice(body).map_err(|_| Refused)?;
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
    .ok_or(Refused)?;
    let st = c
        .command(&req, &session, &LAN_DOOR_ID, now_ms, now_unix)
        .map_err(|_| Refused)?;
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

/// What is left before `deadline` for one blocking operation: at most
/// [`IO_TIMEOUT`], and `None` once it has passed. Reads take the connection
/// budget's end, so no read outlasts [`CONNECTION_BUDGET`]; the response's
/// write takes its own deadline.
fn time_left(deadline: Instant) -> Option<Duration> {
    let left = deadline.checked_duration_since(Instant::now())?;
    if left.is_zero() {
        None
    } else {
        Some(left.min(IO_TIMEOUT))
    }
}

/// One bounded read.
fn read_some(
    stream: &mut TcpStream,
    buf: &mut [u8],
    deadline: Instant,
) -> std::result::Result<usize, RequestError> {
    let t = time_left(deadline).ok_or(RequestError::Io)?;
    stream
        .set_read_timeout(Some(t))
        .map_err(|_| RequestError::Io)?;
    match stream.read(buf) {
        Ok(0) | Err(_) => Err(RequestError::Io),
        Ok(n) => Ok(n),
    }
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
        let n = read_some(stream, &mut buf, deadline)?;
        data.extend_from_slice(&buf[..n]);
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
        let n = read_some(stream, &mut buf, deadline)?;
        body.extend_from_slice(&buf[..n]);
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

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    body: &str,
    deadline: Instant,
) -> Result<()> {
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
    let t = time_left(deadline).ok_or(Error::Invalid("the connection budget is spent"))?;
    stream
        .set_write_timeout(Some(t))
        .map_err(Error::io("set timeout"))?;
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
/// data directory when their files change. Each part loads on its own: a
/// card store or pairing file that fails never holds up a roster change (a
/// revocation), and a part that fails to load is retried on every poll until
/// it loads. A roster that loads but the checker refuses is not retried (its
/// files have not changed): the checker stays stale for `home` until the
/// chain changes again.
pub struct DirReload {
    dir: crate::store::DataDir,
    kek: crate::secrets::Kek,
    roster_key: ed25519_dalek::VerifyingKey,
    seen: Stamp,
    /// Parts (roster, cards, pairings) whose last load failed: retried on
    /// every poll, whatever their stamp says, until they load.
    failing: [bool; 3],
}

impl DirReload {
    /// Watch `dir`, opening the card store with `kek` and checking the
    /// roster chain against `roster_key`, the public half of the roster key
    /// the server opened under the KEK at startup (never re-read from the
    /// plaintext `hub_keys.json` while it runs).
    ///
    /// Nothing counts as seen yet, so the first poll loads every part: a
    /// change applied while the server was starting is never missed.
    pub fn new(
        dir: crate::store::DataDir,
        kek: crate::secrets::Kek,
        roster_key: ed25519_dalek::VerifyingKey,
    ) -> Self {
        DirReload {
            dir,
            kek,
            roster_key,
            seen: (None, None, None),
            failing: [false; 3],
        }
    }

    fn file_stamp(p: &std::path::Path) -> Option<FileStamp> {
        let m = std::fs::metadata(p).ok()?;
        Some((m.len(), m.modified().ok()?))
    }

    fn stamp(&self) -> Stamp {
        // Only names the chain loader reads count: a stray file sorting
        // above them would otherwise freeze the stamp.
        let last = std::fs::read_dir(self.dir.path("roster/chain"))
            .ok()
            .and_then(|it| {
                it.filter_map(|e| e.ok())
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .filter(|n| crate::store::is_chain_file_name(n))
                    .max()
            });
        (
            last,
            Self::file_stamp(&self.dir.cards_path()),
            Self::file_stamp(&self.dir.path("bearers.json")),
        )
    }

    fn load_cards(&self) -> Result<CardStore> {
        let p = self.dir.cards_path();
        if !p.exists() {
            return Ok(CardStore::new());
        }
        let b = crate::store::read_bounded(&p, crate::secrets::MAX_SEALED_BYTES)?;
        CardStore::open(&b, &self.kek, &self.dir.household()?)
    }
}

impl ReloadSource for DirReload {
    fn poll(&mut self) -> Reload {
        let now = self.stamp();
        let mut out = Reload::default();
        if now.0 != self.seen.0 || self.failing[0] {
            let r = self
                .dir
                .current_roster_with(&self.roster_key)
                .and_then(|r| r.ok_or(Error::Rejected("the roster chain is empty")));
            self.failing[0] = r.is_err();
            if r.is_ok() {
                self.seen.0 = now.0;
            }
            out.roster = Some(r);
        }
        if now.1 != self.seen.1 || self.failing[1] {
            let r = self.load_cards();
            self.failing[1] = r.is_err();
            if r.is_ok() {
                self.seen.1 = now.1;
            }
            out.cards = Some(r);
        }
        if now.2 != self.seen.2 || self.failing[2] {
            let r = self.dir.bearer_hashes();
            self.failing[2] = r.is_err();
            if r.is_ok() {
                self.seen.2 = now.2;
            }
            out.bearers = Some(r);
        }
        out
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
    fn home_network_is_private_loopback_and_link_local_only() {
        let yes = [
            "127.0.0.1",
            "10.1.2.3",
            "172.16.0.9",
            "172.31.255.1",
            "192.168.1.20",
            "169.254.3.4",
            "::1",
            "fd12:3456::1",
            "fe80::1",
            "::ffff:192.168.1.20",
        ];
        let no = [
            "100.64.0.1",
            "100.101.102.103",
            "8.8.8.8",
            "172.32.0.1",
            "192.169.0.1",
            "2001:db8::1",
            "2606:4700::1111",
            "::ffff:8.8.8.8",
            "::ffff:100.64.0.1",
            "fd7a:115c:a1e0::1",
            "fd7a:115c:a1e0:ab12::7",
        ];
        for a in yes {
            assert!(is_home_network(a.parse().unwrap()), "{a} refused");
        }
        for a in no {
            assert!(!is_home_network(a.parse().unwrap()), "{a} accepted");
        }
    }

    fn served(token: &str) -> (ServerHandle, SocketAddr) {
        let cfg = ServerConfig {
            bind: "127.0.0.1:0".parse().unwrap(),
            allow_lan: false,
        };
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
        (h, a)
    }

    #[test]
    fn a_silent_connection_does_not_hold_the_api() {
        let (h, a) = served("t0ken");
        // Two connections that never send a byte...
        let idle1 = TcpStream::connect(a).unwrap();
        let idle2 = TcpStream::connect(a).unwrap();
        std::thread::sleep(Duration::from_millis(100));
        // ...do not delay a real request behind them.
        let started = Instant::now();
        let r = raw(
            a,
            b"GET /v1/posture HTTP/1.1\r\nAuthorization: Bearer t0ken\r\n\r\n",
        );
        assert!(r.starts_with("HTTP/1.1 200"), "{r}");
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "served behind the idle connections"
        );
        drop((idle1, idle2));
        h.stop();
    }

    #[test]
    fn idle_connections_never_keep_out_a_real_request() {
        // The default deployment: every local client is 127.0.0.1, the
        // attacker included. Idle sockets at the per-peer cap are evicted,
        // oldest first, to admit a request.
        let (h, a) = served("t0ken");
        let idle: Vec<TcpStream> = (0..MAX_CONNECTIONS_PER_PEER)
            .map(|_| TcpStream::connect(a).unwrap())
            .collect();
        std::thread::sleep(Duration::from_millis(150));
        let r = raw(
            a,
            b"GET /v1/posture HTTP/1.1\r\nAuthorization: Bearer t0ken\r\n\r\n",
        );
        assert!(
            r.starts_with("HTTP/1.1 200"),
            "kept out by idle sockets: {r}"
        );
        // The oldest idle socket was the one closed.
        let mut first = idle.into_iter().next().unwrap();
        first
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(
            first.read(&mut buf).unwrap_or(0),
            0,
            "oldest idle socket kept"
        );
        h.stop();
    }

    /// Admission without the network: `Slot::take` with chosen peer
    /// addresses (any address can be named here, unlike a real connect).
    fn admission_harness() -> (Arc<Shared>, TcpListener, SocketAddr) {
        let srv = Server::bind(
            &ServerConfig {
                bind: "127.0.0.1:0".parse().unwrap(),
                allow_lan: false,
            },
            checker(),
            vec![],
            None,
            Arc::new(SystemClock::default()),
            Arc::new(|_: &str| {}),
        )
        .unwrap();
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let a = l.local_addr().unwrap();
        (srv.shared.clone(), l, a)
    }

    fn conn(l: &TcpListener, a: SocketAddr) -> (TcpStream, TcpStream) {
        let client = TcpStream::connect(a).unwrap();
        let (server_side, _) = l.accept().unwrap();
        (client, server_side)
    }

    #[test]
    fn admission_evicts_the_oldest_incomplete_connection_in_scope() {
        let (shared, l, a) = admission_harness();
        let ip = |s: &str| -> IpAddr { s.parse().unwrap() };
        // Fill the whole table: 4 peers x 4 connections.
        let mut held = Vec::new();
        for p in 1..=4 {
            for _ in 0..MAX_CONNECTIONS_PER_PEER {
                let (c, sv) = conn(&l, a);
                let slot = Slot::take(&shared, ip(&format!("10.0.0.{p}")), &sv).unwrap();
                held.push((c, sv, slot));
            }
        }
        assert_eq!(lock(&shared.conns).entries.len(), MAX_CONNECTIONS);
        // A fifth peer evicts the oldest incomplete connection overall.
        let (_c, sv) = conn(&l, a);
        let s5 = Slot::take(&shared, ip("10.0.0.9"), &sv).expect("newcomer refused");
        let ids: Vec<u64> = lock(&shared.conns).entries.iter().map(|e| e.id).collect();
        assert!(!ids.contains(&held[0].2.id), "oldest not evicted");
        let mut buf = [0u8; 1];
        held[0]
            .0
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        assert_eq!(
            held[0].0.read(&mut buf).unwrap_or(0),
            0,
            "evicted socket open"
        );
        // Once every connection is serving a whole request, a newcomer is
        // turned away instead.
        for (_, _, slot) in &held {
            slot.request_read.store(true, Ordering::SeqCst);
        }
        s5.request_read.store(true, Ordering::SeqCst);
        let (_c, sv) = conn(&l, a);
        assert!(Slot::take(&shared, ip("10.0.0.10"), &sv).is_none());
        drop(held);
        drop(s5);
        assert!(lock(&shared.conns).entries.is_empty(), "slots leaked");
    }

    #[test]
    fn one_ipv6_slash_64_counts_as_one_peer() {
        let (shared, l, a) = admission_harness();
        let ip = |s: &str| -> IpAddr { s.parse().unwrap() };
        let mut held = Vec::new();
        for i in 1..=MAX_CONNECTIONS_PER_PEER {
            let (c, sv) = conn(&l, a);
            let slot = Slot::take(&shared, ip(&format!("fd00:1:2:3::{i}")), &sv).unwrap();
            held.push((c, sv, slot));
        }
        // A fifth address in the same /64 is the same peer: it evicts one of
        // that peer's connections, not someone else's.
        let (_c, sv) = conn(&l, a);
        let other = Slot::take(&shared, ip("fd00:1:2:4::1"), &sv).unwrap();
        let (_c2, sv2) = conn(&l, a);
        let fifth = Slot::take(&shared, ip("fd00:1:2:3::99"), &sv2).unwrap();
        let ids: Vec<u64> = lock(&shared.conns).entries.iter().map(|e| e.id).collect();
        assert!(ids.contains(&other.id), "another /64 was evicted");
        assert!(!ids.contains(&held[0].2.id));
        assert!(ids.contains(&fifth.id));
    }

    #[test]
    fn the_connection_budget_is_absolute() {
        let (h, a) = served("t0ken");
        // Half a head at once, one more byte just before the budget ends,
        // then nothing. Checking the deadline only before each read would
        // let that last read block a full IO_TIMEOUT past the budget; each
        // read's timeout is cut to what is left instead.
        let mut s = TcpStream::connect(a).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        let started = Instant::now();
        s.write_all(b"GET /v1/posture HTTP/1.1\r\n").unwrap();
        std::thread::sleep(CONNECTION_BUDGET - Duration::from_millis(500));
        s.write_all(b"X").unwrap();
        let mut out = Vec::new();
        let _ = s.read_to_end(&mut out);
        let took = started.elapsed();
        assert!(
            took < CONNECTION_BUDGET + Duration::from_millis(700),
            "connection held for {took:?}"
        );
        h.stop();
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

    /// A data directory with a genesis roster and one member, for the
    /// reload tests.
    fn reload_dir(
        tag: &str,
    ) -> (
        crate::store::DataDir,
        crate::secrets::Kek,
        std::path::PathBuf,
    ) {
        use crate::admin::sign_cli;
        use crate::roster::tests::{ed_admin, phone};
        use crate::roster::CredentialClass;
        let p = crate::store::tests::tmpdir(tag);
        let kek = crate::secrets::Kek::generate().unwrap();
        let d = crate::store::DataDir::create(&p, &kek).unwrap();
        let hub = d.hub_keys(&kek).unwrap();
        let admin = ed25519_dalek::SigningKey::from_bytes(&[1; 32]);
        let g = Roster::genesis(d.household().unwrap(), vec![ed_admin(1, 1)], 1).unwrap();
        let a = vec![sign_cli(&admin, &[1; 16], &g.digest().unwrap())];
        d.accept(&hub, g.clone(), a, None).unwrap();
        let mut n = g.successor().unwrap();
        n.members.push(phone(0x10, 0x31, CredentialClass::Intent));
        let a = vec![sign_cli(&admin, &[1; 16], &n.digest().unwrap())];
        d.accept(&hub, n, a, None).unwrap();
        (d, kek, p)
    }

    fn revoke_all(d: &crate::store::DataDir, kek: &crate::secrets::Kek) -> u64 {
        let hub = d.hub_keys(kek).unwrap();
        let admin = ed25519_dalek::SigningKey::from_bytes(&[1; 32]);
        let cur = d.current_roster().unwrap().unwrap();
        let mut n = cur.successor().unwrap();
        n.members.clear();
        let a = vec![crate::admin::sign_cli(
            &admin,
            &[1; 16],
            &n.digest().unwrap(),
        )];
        d.accept(&hub, n.clone(), a, None).unwrap();
        n.epoch
    }

    #[test]
    fn the_first_poll_loads_everything() {
        let (d, kek, p) = reload_dir("reload-first");
        let key = d.roster_public_key().unwrap();
        let mut r = DirReload::new(d.clone(), kek, key);
        let first = r.poll();
        assert_eq!(first.roster.unwrap().unwrap().epoch, 2);
        assert!(first.cards.is_none(), "no card store yet, nothing to load");
        assert!(first.bearers.unwrap().is_ok());
        let second = r.poll();
        assert!(second.roster.is_none() && second.bearers.is_none());
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn a_stray_file_in_the_chain_never_freezes_a_revocation() {
        let (d, kek, p) = reload_dir("reload-stray");
        let key = d.roster_public_key().unwrap();
        let mut r = DirReload::new(d.clone(), kek.clone_for_test(), key);
        let _ = r.poll();
        std::fs::create_dir(d.path("roster/chain/old")).unwrap();
        std::fs::write(d.path("roster/chain/README.txt"), b"notes").unwrap();
        let epoch = revoke_all(&d, &kek);
        assert_eq!(r.poll().roster.unwrap().unwrap().epoch, epoch);
        std::fs::remove_dir_all(&p).unwrap();
    }

    #[test]
    fn a_roster_that_failed_to_load_is_retried_until_it_loads() {
        let (d, kek, p) = reload_dir("reload-retry");
        let key = d.roster_public_key().unwrap();
        let mut r = DirReload::new(d.clone(), kek.clone_for_test(), key);
        let _ = r.poll();
        // The newest chain file goes missing (the high-water mark makes the
        // shorter chain a refused rollback), then comes back: the stamp is
        // the one already seen, and the roster must still be reloaded.
        let newest = d.path("roster/chain/00000000000000000002.json");
        let aside = d.path("aside.json");
        std::fs::rename(&newest, &aside).unwrap();
        assert!(r.poll().roster.unwrap().is_err());
        std::fs::rename(&aside, &newest).unwrap();
        assert_eq!(r.poll().roster.unwrap().unwrap().epoch, 2);
        std::fs::remove_dir_all(&p).unwrap();
    }

    /// A ReloadSource that hands out scripted results.
    struct Scripted(std::collections::VecDeque<Reload>);
    impl ReloadSource for Scripted {
        fn poll(&mut self) -> Reload {
            self.0.pop_front().unwrap_or_default()
        }
    }

    #[test]
    fn housekeeping_applies_each_part_on_its_own_and_fails_closed_for_home() {
        let base = roster();
        let mut next = base.successor().unwrap();
        next.members
            .retain(|m| m.class != crate::roster::CredentialClass::Intent);
        let script = Scripted(
            [
                // A roster that does not load: stale, home refused.
                Reload {
                    roster: Some(Err(Error::Rejected("chain did not verify"))),
                    ..Default::default()
                },
                // A roster that loads beside a card store that does not: the
                // roster is applied anyway, and stale clears.
                Reload {
                    roster: Some(Ok(next.clone())),
                    cards: Some(Err(Error::Rejected("card store did not open"))),
                    ..Default::default()
                },
            ]
            .into(),
        );
        let srv = Server::bind(
            &ServerConfig {
                bind: "127.0.0.1:0".parse().unwrap(),
                allow_lan: false,
            },
            checker(),
            vec![],
            Some(Box::new(script)),
            Arc::new(SystemClock::default()),
            Arc::new(|_: &str| {}),
        )
        .unwrap();
        srv.shared.housekeeping();
        assert!(lock(&srv.shared.state).checker.roster_is_stale());
        srv.shared.housekeeping();
        let st = lock(&srv.shared.state);
        assert!(!st.checker.roster_is_stale());
        assert_eq!(
            st.checker.epoch(),
            next.epoch,
            "roster held up by the card store"
        );
    }
}
