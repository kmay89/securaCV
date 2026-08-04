//! The accessory server: discovery, connections, routing, and the metronome
//! that drives them.
//!
//! # The shape of this module
//!
//! The protocol lives in [`Connection`], which is a **pure state machine** —
//! bytes in, bytes out, no sockets. That is deliberate: it means the entire
//! pairing-through-notification path is exercised in unit tests at full
//! fidelity, including the encrypted-session transition, without binding a
//! port or waiting on a timer. [`serve`] is the thin socket loop around it.
//!
//! # The metronome is the only thing that publishes
//!
//! [`Projection::tick`](crate::bridge::homekit::Projection::tick) is called
//! on a fixed cadence by [`Fleet::tick`], and characteristic values change
//! *only* there. A connection never reads kernel state directly, and no code
//! path turns an event into a notification without passing through a tick.
//! That is Invariant III surviving the trip to the wire: because the
//! accessory's outbound traffic is paced, what a controller (or anyone
//! watching the LAN) can learn about *when* something happened is bounded by
//! the tick, not by the event.

use std::collections::{BTreeSet, HashMap};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use super::accessory::{accessories_json, CanaryAccessory, BRIDGE_AID};
use super::http::{self, content_type, HttpError, Request};
use super::pairing::{AccessoryIdentity, PairError, PairSetup, PairVerify, Pairing, PairingStore};
use super::session::Session;
use super::tlv8::{error as tlv_error, method, ty, Tlv};
use crate::bridge::homekit::{HomeSignal, PacingConfig, Projection};

/// HAP accessory category 2 — a bridge. This is what makes the Home app
/// offer "Add Accessory" for a thing that owns other things.
pub const CATEGORY_BRIDGE: u8 = 2;

/// The mDNS service type every HAP accessory advertises under.
pub const HAP_SERVICE: &str = "_hap._tcp.local.";

/// How many controllers may be connected at once (FR-4).
pub const MAX_CONNECTIONS: usize = 16;

/// How long a connection may sit idle before the accessory closes it. Also
/// bounds the "peer promised bytes it never sent" case in the framing layer.
pub const IDLE_TIMEOUT: Duration = Duration::from_secs(60);

/// Everything the server shares across connections.
pub struct ServerState {
    /// This accessory's long-term identity.
    pub identity: AccessoryIdentity,
    /// Controllers allowed to talk to us.
    pub pairings: PairingStore,
    /// The fleet, as bridged accessories.
    pub fleet: Vec<CanaryAccessory>,
    /// The bridge accessory's display name.
    pub bridge_name: String,
    /// The setup code a controller must be told out of band.
    pub setup_code: String,
    /// Bumped whenever the accessory database changes shape, so controllers
    /// know to re-read `/accessories` instead of trusting their cache.
    pub config_number: u64,
    /// Failed pair-setup attempts, accessory-wide.
    ///
    /// Lives here rather than on a [`Connection`] because a per-connection
    /// counter is no limit at all: an attacker reconnects and starts over.
    /// See [`PairSetup::handle`].
    pub setup_attempts: u32,
}

impl ServerState {
    /// The `/accessories` document for the current fleet.
    fn accessories(&self) -> String {
        accessories_json(&self.bridge_name, self.identity.pairing_id(), &self.fleet)
    }

    /// Resolve `(aid, iid)` to a JSON value.
    fn value_at(&self, aid: u64, iid: u64) -> Option<String> {
        self.fleet
            .iter()
            .find(|a| a.aid == aid)
            .and_then(|a| a.value_at(iid))
    }

    /// Every `(aid, iid)` the fleet currently exposes.
    fn all_characteristics(&self) -> Vec<(u64, u64)> {
        self.fleet
            .iter()
            .flat_map(|a| {
                a.characteristics()
                    .into_iter()
                    .map(move |(iid, _)| (a.aid, iid))
            })
            .collect()
    }
}

/// A fleet of paced projections — one [`Projection`] per Canary, ticked
/// together so every accessory publishes on the same metronome.
pub struct Fleet {
    projections: Vec<(u64, Projection)>,
}

impl Fleet {
    /// Build a fleet with one projection per accessory aid.
    pub fn new(aids: impl IntoIterator<Item = u64>, cfg: PacingConfig) -> Result<Self, PairError> {
        let mut projections = Vec::new();
        for aid in aids {
            let p = Projection::new(cfg).map_err(|_| PairError::Unknown)?;
            projections.push((aid, p));
        }
        Ok(Fleet { projections })
    }

    /// The projection for one accessory, to drive from the kernel.
    pub fn projection_mut(&mut self, aid: u64) -> Option<&mut Projection> {
        self.projections
            .iter_mut()
            .find(|(a, _)| *a == aid)
            .map(|(_, p)| p)
    }

    /// Advance every projection one beat and copy the results into
    /// `state.fleet`.
    ///
    /// Returns the `(aid, iid)` pairs whose value changed. That list is for
    /// observability and tests only — deliberately **not** for deciding
    /// whether to notify. [`Connection::notification`] sends the full
    /// subscribed set every tick regardless, because gating frames on
    /// "something changed" makes socket traffic proportional to event
    /// occurrence, which is the leak the pacing exists to remove.
    pub fn tick(&mut self, state: &mut ServerState) -> Vec<(u64, u64)> {
        let mut changed = Vec::new();
        for (aid, projection) in &mut self.projections {
            let publication = projection.tick();
            if let Some(acc) = state.fleet.iter_mut().find(|a| a.aid == *aid) {
                let before: Vec<(u64, String)> = acc
                    .characteristics()
                    .into_iter()
                    .filter_map(|(iid, _)| acc.value_at(iid).map(|v| (iid, v)))
                    .collect();
                acc.enabled = projection.enabled();
                acc.last = publication;
                for (iid, old) in before {
                    if acc.value_at(iid).as_deref() != Some(old.as_str()) {
                        changed.push((*aid, iid));
                    }
                }
            }
        }
        changed
    }
}

/// One controller connection, as a pure state machine.
pub struct Connection {
    session: Option<Session>,
    /// A session that Pair Verify has established but that must not take
    /// effect until the M4 response has been written. HAP sends M4 in the
    /// clear and encrypts everything *after* it; promoting the session one
    /// step early encrypts M4 itself, and the controller — still reading
    /// plaintext — sees garbage and abandons the pairing.
    pending_session: Option<Session>,
    pair_setup: PairSetup,
    pair_verify: PairVerify,
    /// `(aid, iid)` pairs this controller asked to be notified about.
    /// Bounded by the accessory database, which is itself bounded.
    subscriptions: BTreeSet<(u64, u64)>,
    /// Undecrypted bytes from the socket.
    raw: Vec<u8>,
    /// Decrypted (or pre-session plaintext) bytes awaiting HTTP parsing.
    plain: Vec<u8>,
}

impl Connection {
    /// A new connection, before anything has been said on it.
    pub fn new(setup_code: &str) -> Result<Self, PairError> {
        Ok(Connection {
            session: None,
            pending_session: None,
            pair_setup: PairSetup::new(setup_code)?,
            pair_verify: PairVerify::new(),
            subscriptions: BTreeSet::new(),
            raw: Vec::new(),
            plain: Vec::new(),
        })
    }

    /// Whether this connection has completed Pair Verify.
    pub fn is_secure(&self) -> bool {
        self.session.is_some()
    }

    /// What this controller is subscribed to.
    pub fn subscriptions(&self) -> &BTreeSet<(u64, u64)> {
        &self.subscriptions
    }

    /// Feed bytes from the socket; get bytes to write back.
    ///
    /// Handles as many complete requests as the input contains, so a
    /// pipelined controller is answered fully in one pass.
    pub fn feed(&mut self, bytes: &[u8], state: &mut ServerState) -> Vec<u8> {
        self.raw.extend_from_slice(bytes);
        let mut out = Vec::new();

        // Move everything currently readable into the plaintext buffer.
        // Before a session exists that is a straight move; after, it is a
        // decrypt of each complete frame.
        let mut frame_failed = false;
        if let Some(session) = self.session.as_mut() {
            // Three outcomes, not two: a complete frame, "nothing whole yet"
            // (the normal stream case), and a frame that did not
            // authenticate. Only the last is fatal, so this cannot collapse
            // into a `while let`.
            loop {
                match session.open_frame(&mut self.raw) {
                    Ok(Some(frame)) => self.plain.extend_from_slice(&frame),
                    Ok(None) => break,
                    Err(_) => {
                        frame_failed = true;
                        break;
                    }
                }
            }
        } else {
            self.plain.append(&mut self.raw);
        }
        if frame_failed {
            // A frame that fails to authenticate ends the connection: there
            // is no way to resynchronize a counter-based stream, and
            // pretending otherwise would accept attacker-chosen framing.
            self.session = None;
            self.raw.clear();
            self.plain.clear();
            return out;
        }

        loop {
            let parsed = match Request::parse(&self.plain) {
                Ok(Some((req, used))) => {
                    self.plain.drain(..used);
                    req
                }
                Ok(None) => break,
                Err(e) => {
                    let status = match e {
                        HttpError::HeadersTooLarge | HttpError::BodyTooLarge => 400,
                        HttpError::Malformed => 400,
                    };
                    self.plain.clear();
                    out.extend_from_slice(&self.wrap(http::empty_response(status)));
                    break;
                }
            };
            let response = self.route(&parsed, state);
            out.extend_from_slice(&self.wrap(response));
            // Promote a session armed by Pair Verify only now, after its M4
            // has been written in the clear.
            if let Some(session) = self.pending_session.take() {
                self.session = Some(session);
            }
        }
        out
    }

    /// Encrypt a response if a session is up; otherwise send it as-is.
    fn wrap(&mut self, plaintext: Vec<u8>) -> Vec<u8> {
        match self.session.as_mut() {
            Some(s) => s.seal(&plaintext),
            None => plaintext,
        }
    }

    /// Build this tick's `EVENT/1.0` notification: the current value of every
    /// characteristic this controller subscribed to.
    ///
    /// # This is the cover traffic, and it is why nothing here looks at what
    /// # changed
    ///
    /// An earlier version sent only *changed* characteristics and returned
    /// `None` otherwise. That was a real leak, and a subtle one: the
    /// projection was paced, but the **socket** was not. An observer on the
    /// LAN — or the hub relaying onward — sees a frame on ticks where
    /// something happened and silence on ticks where nothing did, which is
    /// network behavior varying in proportion to event occurrence. That is
    /// precisely what Invariant III forbids, and pacing the layer above it
    /// does not help if this layer re-introduces it.
    ///
    /// So: one frame per tick per subscribed connection, carrying current
    /// values whether or not they moved. Redundant notifications are legal
    /// HAP and controllers coalesce them.
    ///
    /// **Frame length is held constant too.** `true` is one byte shorter
    /// than `false`, so an unpadded body would shrink and grow with the
    /// values inside it, and ChaCha20-Poly1305 is a stream cipher — the
    /// ciphertext length would leak exactly what the payload refused to say.
    /// One trailing space per `true` pads every frame to the width it would
    /// have had with all values false, which is constant for a given
    /// subscription set. JSON ignores trailing whitespace.
    ///
    /// Returns `None` only when there is no session or no subscription —
    /// both static properties of the connection, not event-correlated.
    pub fn notification(&mut self, state: &ServerState) -> Option<Vec<u8>> {
        // No session, no notification: an unauthenticated peer is told
        // nothing about the fleet's state.
        self.session.as_ref()?;
        let mut entries = Vec::new();
        for (aid, iid) in &self.subscriptions {
            if let Some(value) = state.value_at(*aid, *iid) {
                entries.push(format!(r#"{{"aid":{aid},"iid":{iid},"value":{value}}}"#));
            }
        }
        if entries.is_empty() {
            return None;
        }
        let mut body = format!(r#"{{"characteristics":[{}]}}"#, entries.join(","));
        let short_by = body.matches(":true").count();
        body.extend(std::iter::repeat_n(' ', short_by));
        Some(self.wrap(http::event(body.as_bytes())))
    }

    fn route(&mut self, req: &Request, state: &mut ServerState) -> Vec<u8> {
        match (req.method.as_str(), req.path()) {
            ("POST", "/pair-setup") => {
                let body = self.pair_setup.handle(
                    &req.body,
                    &state.identity,
                    &mut state.pairings,
                    &mut state.setup_attempts,
                );
                // A completed setup changes what the accessory advertises
                // (it is no longer discoverable), so the config number moves.
                if self.pair_setup.is_complete() {
                    state.config_number = state.config_number.saturating_add(1);
                }
                http::response(200, content_type::TLV8, &body)
            }
            ("POST", "/pair-verify") => {
                let body = self
                    .pair_verify
                    .handle(&req.body, &state.identity, &state.pairings);
                // Establish the session only once M4 has actually verified
                // the controller. `session_keys` returns None until then, so
                // this cannot fire early.
                if let Some(keys) = self.pair_verify.session_keys() {
                    // Arm the session, but let `feed` promote it once M4 has
                    // been wrapped — M4 itself is plaintext.
                    self.pending_session = Some(Session::new(keys));
                }
                http::response(200, content_type::TLV8, &body)
            }
            ("GET", "/accessories") => {
                if !self.is_secure() {
                    return http::empty_response(470);
                }
                http::response(200, content_type::JSON, state.accessories().as_bytes())
            }
            ("GET", "/characteristics") => {
                if !self.is_secure() {
                    return http::empty_response(470);
                }
                self.read_characteristics(req, state)
            }
            ("PUT", "/characteristics") => {
                if !self.is_secure() {
                    return http::empty_response(470);
                }
                self.write_characteristics(req, state)
            }
            ("POST", "/pairings") => {
                if !self.is_secure() {
                    return http::empty_response(470);
                }
                self.pairings(req, state)
            }
            ("GET", "/") => http::empty_response(404),
            _ => http::empty_response(404),
        }
    }

    /// `GET /characteristics?id=2.11,2.13`
    fn read_characteristics(&self, req: &Request, state: &ServerState) -> Vec<u8> {
        let query = req.query().unwrap_or("");
        let ids = query
            .split('&')
            .find_map(|kv| kv.strip_prefix("id="))
            .unwrap_or("");

        let mut entries = Vec::new();
        for pair in ids.split(',').filter(|s| !s.is_empty()) {
            let Some((aid, iid)) = parse_aid_iid(pair) else {
                continue;
            };
            match state.value_at(aid, iid) {
                Some(value) => {
                    entries.push(format!(r#"{{"aid":{aid},"iid":{iid},"value":{value}}}"#))
                }
                // HAP's own "no such characteristic" status, rather than
                // omitting the entry and leaving the controller to guess.
                None => entries.push(format!(r#"{{"aid":{aid},"iid":{iid},"status":-70409}}"#)),
            }
        }
        let body = format!(r#"{{"characteristics":[{}]}}"#, entries.join(","));
        http::response(200, content_type::JSON, body.as_bytes())
    }

    /// `PUT /characteristics` — subscriptions, and the writes we accept.
    ///
    /// The only writable characteristic in this projection is Identify. Every
    /// signal is read-only by construction: a home automation ecosystem is an
    /// audience for witness state, never a control surface for it. Inbound
    /// control is the epic's open decision #4 and deliberately absent until
    /// it has been designed as a trust surface.
    fn write_characteristics(&mut self, req: &Request, state: &ServerState) -> Vec<u8> {
        let Ok(parsed) = serde_json::from_slice::<serde_json::Value>(&req.body) else {
            return http::empty_response(400);
        };
        let Some(items) = parsed.get("characteristics").and_then(|c| c.as_array()) else {
            return http::empty_response(400);
        };

        let known = state.all_characteristics();
        for item in items {
            let (Some(aid), Some(iid)) = (
                item.get("aid").and_then(|v| v.as_u64()),
                item.get("iid").and_then(|v| v.as_u64()),
            ) else {
                continue;
            };
            if let Some(ev) = item.get("ev").and_then(|v| v.as_bool()) {
                // Only subscribe to characteristics that exist, so the
                // subscription set stays bounded by the accessory database
                // rather than by what a peer asks for (FR-4).
                if ev {
                    if known.contains(&(aid, iid)) {
                        self.subscriptions.insert((aid, iid));
                    }
                } else {
                    self.subscriptions.remove(&(aid, iid));
                }
            }
            // A write to Identify is accepted and does nothing observable
            // here; a witness has no light to blink from this layer.
        }
        http::empty_response(204)
    }

    /// `POST /pairings` — add, remove and list, per HAP.
    fn pairings(&mut self, req: &Request, state: &mut ServerState) -> Vec<u8> {
        let Ok(request) = Tlv::decode(&req.body) else {
            return http::response(
                200,
                content_type::TLV8,
                &Tlv::error_response(2, tlv_error::UNKNOWN),
            );
        };

        // Only an admin controller may change the pairing list. Without this
        // check, any paired controller could add another — which is how a
        // guest becomes a permanent resident.
        let caller_is_admin = self
            .pair_verify
            .verified_controller()
            .and_then(|id| state.pairings.get(id))
            .map(|p| p.admin)
            .unwrap_or(false);
        if !caller_is_admin {
            return http::response(
                200,
                content_type::TLV8,
                &Tlv::error_response(2, tlv_error::AUTHENTICATION),
            );
        }

        let body = match request.get_u8(ty::METHOD) {
            Some(method::ADD_PAIRING) => {
                let id = request.get(ty::IDENTIFIER).unwrap_or_default();
                let ltpk = request.get(ty::PUBLIC_KEY).unwrap_or_default();
                let admin = request.get_u8(ty::PERMISSIONS).unwrap_or(0) == 1;
                match (String::from_utf8(id.to_vec()), <[u8; 32]>::try_from(ltpk)) {
                    (Ok(id), Ok(ltpk)) => match state.pairings.add(Pairing { id, ltpk, admin }) {
                        Ok(()) => {
                            let mut t = Tlv::new();
                            t.push_u8(ty::STATE, 2);
                            t.encode()
                        }
                        Err(_) => Tlv::error_response(2, tlv_error::MAX_PEERS),
                    },
                    _ => Tlv::error_response(2, tlv_error::UNKNOWN),
                }
            }
            Some(method::REMOVE_PAIRING) => {
                if let Ok(id) =
                    String::from_utf8(request.get(ty::IDENTIFIER).unwrap_or_default().to_vec())
                {
                    state.pairings.remove(&id);
                }
                // Removing the last pairing makes the accessory discoverable
                // again, which controllers learn from the advertisement.
                state.config_number = state.config_number.saturating_add(1);
                let mut t = Tlv::new();
                t.push_u8(ty::STATE, 2);
                t.encode()
            }
            Some(method::LIST_PAIRINGS) => {
                let mut t = Tlv::new();
                t.push_u8(ty::STATE, 2);
                for (i, p) in state.pairings.list().iter().enumerate() {
                    if i > 0 {
                        t.push(ty::SEPARATOR, Vec::new());
                    }
                    t.push(ty::IDENTIFIER, p.id.as_bytes().to_vec())
                        .push(ty::PUBLIC_KEY, p.ltpk.to_vec())
                        .push_u8(ty::PERMISSIONS, u8::from(p.admin));
                }
                t.encode()
            }
            _ => Tlv::error_response(2, tlv_error::UNKNOWN),
        };
        http::response(200, content_type::TLV8, &body)
    }
}

/// Parse an `aid.iid` pair from a `/characteristics` query.
fn parse_aid_iid(s: &str) -> Option<(u64, u64)> {
    let (a, i) = s.split_once('.')?;
    Some((a.parse().ok()?, i.parse().ok()?))
}

/// The `X-HM://` setup URI a controller can scan.
///
/// The 9-character payload is a base-36 encoding of a packed integer:
/// version, reserved bits, accessory category, a flags nibble (2 = pairs
/// over IP) and the eight-digit setup code. It is not a secret — it *is* the
/// setup code, in the form a camera can read — so it belongs on a sticker or
/// a screen, never in a log.
pub fn setup_uri(setup_code: &str, setup_id: &str, category: u8) -> String {
    let digits: String = setup_code.chars().filter(|c| c.is_ascii_digit()).collect();
    let code: u64 = digits.parse().unwrap_or(0);

    // The packed layout, most significant field first. Version and the
    // reserved field are both zero today; they are named and shifted anyway
    // so the field boundaries stay visible if either ever moves.
    const VERSION: u64 = 0;
    const RESERVED: u64 = 0;
    const FLAG_PAIRS_OVER_IP: u64 = 2;

    let mut payload: u64 = VERSION;
    payload <<= 4;
    payload |= RESERVED;
    payload <<= 8;
    payload |= u64::from(category);
    payload <<= 4;
    payload |= FLAG_PAIRS_OVER_IP;
    payload <<= 27;
    payload |= code & 0x7FFF_FFFF;

    let mut encoded = String::new();
    let mut n = payload;
    const ALPHABET: &[u8] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if n == 0 {
        encoded.push('0');
    }
    while n > 0 {
        let d = (n % 36) as usize;
        // `d` is always < 36, so this index is in bounds.
        encoded.push(ALPHABET[d] as char);
        n /= 36;
    }
    let encoded: String = encoded.chars().rev().collect();
    format!("X-HM://{:0>9}{}", encoded, setup_id)
}

/// The mDNS TXT records that describe this accessory to the network.
pub fn txt_records(
    device_id: &str,
    config_number: u64,
    paired: bool,
    category: u8,
) -> HashMap<String, String> {
    let mut txt = HashMap::new();
    txt.insert("c#".into(), config_number.to_string());
    txt.insert("ff".into(), "0".into()); // no MFi coprocessor
    txt.insert("id".into(), device_id.to_string());
    txt.insert("md".into(), "SecuraCV Bridge".into());
    txt.insert("pv".into(), "1.1".into());
    txt.insert("s#".into(), "1".into());
    // The bit a controller reads to decide whether to offer pairing: 1 means
    // "not yet paired, come and get me", 0 means "already someone's".
    txt.insert("sf".into(), if paired { "0" } else { "1" }.into());
    txt.insert("ci".into(), category.to_string());
    txt
}

/// A live connection: its protocol state, its socket, and whether the thread
/// serving it is still running.
///
/// # Lock order
///
/// Every thread that takes more than one of these locks takes them in this
/// order, and never any other:
///
/// > `ServerState` → the connection list → [`LiveConnection::proto`] → [`LiveConnection::write`]
///
/// This is not style. The reader thread needs the state (to route a request)
/// and the connection (to feed it); the metronome needs the state (to read
/// values) and every connection (to notify it). Taking them in opposite
/// orders is a textbook inversion, and the deadlock it produces is permanent
/// — the tick thread and a request thread wedge holding one lock each, and
/// all HAP traffic stops until the process is restarted.
///
/// `write` is deliberately a separate, leaf-level lock so a slow or wedged
/// socket cannot hold the protocol state hostage.
struct LiveConnection {
    proto: Mutex<Connection>,
    write: Mutex<TcpStream>,
    /// Cleared when the serving thread exits. A dropped connection's mutex
    /// still locks perfectly well, so liveness has to be tracked explicitly
    /// rather than inferred from whether a lock succeeds — otherwise dead
    /// entries accumulate and, after `MAX_CONNECTIONS` lifetime reconnects,
    /// the server refuses every new controller until restart.
    alive: AtomicBool,
}

impl LiveConnection {
    fn is_alive(&self) -> bool {
        self.alive.load(Ordering::Relaxed)
    }

    fn retire(&self) {
        self.alive.store(false, Ordering::Relaxed);
    }
}

/// Every connection the server is currently serving.
type Connections = Arc<Mutex<Vec<Arc<LiveConnection>>>>;

/// A running server's handle.
pub struct HapServer {
    state: Arc<Mutex<ServerState>>,
    stop: Arc<AtomicBool>,
    addr: SocketAddr,
}

impl HapServer {
    /// The address the listener actually bound, which matters when the
    /// configured port was 0.
    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }

    /// Shared state, for the kernel side to update the fleet.
    pub fn state(&self) -> Arc<Mutex<ServerState>> {
        Arc::clone(&self.state)
    }

    /// Ask the server to stop. Connections close as they time out.
    pub fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

/// Bind, advertise over mDNS, and serve until stopped.
///
/// The `tick` closure is called on the pacing cadence; it is where the
/// caller advances [`Fleet`] and hands back the changed characteristics.
pub fn serve(
    bind: SocketAddr,
    state: ServerState,
    pacing: PacingConfig,
    mut tick: impl FnMut(&mut ServerState) -> Vec<(u64, u64)> + Send + 'static,
) -> std::io::Result<HapServer> {
    let listener = TcpListener::bind(bind)?;
    let addr = listener.local_addr()?;
    let setup_code = state.setup_code.clone();
    let device_id = state.identity.pairing_id().to_string();
    let paired = !state.pairings.is_empty();
    let config_number = state.config_number;

    let state = Arc::new(Mutex::new(state));
    let stop = Arc::new(AtomicBool::new(false));

    // Discovery. A failure here is logged and tolerated: the accessory is
    // still reachable by IP, and refusing to serve because Bonjour is
    // unavailable would be a worse outcome than being hard to find.
    match mdns_sd::ServiceDaemon::new() {
        Ok(daemon) => {
            let txt = txt_records(&device_id, config_number, paired, CATEGORY_BRIDGE);
            let instance = device_id.replace(':', "");
            match mdns_sd::ServiceInfo::new(
                HAP_SERVICE,
                &instance,
                &format!("{instance}.local."),
                (),
                addr.port(),
                txt,
            ) {
                Ok(info) => {
                    let info = info.enable_addr_auto();
                    if let Err(e) = daemon.register(info) {
                        log::warn!("HAP: mDNS registration failed ({e}); reachable by IP only");
                    }
                }
                Err(e) => log::warn!("HAP: could not build mDNS record ({e})"),
            }
            // The daemon must outlive the server or the advertisement stops.
            std::mem::forget(daemon);
        }
        Err(e) => log::warn!("HAP: mDNS unavailable ({e}); reachable by IP only"),
    }

    let connections: Connections = Arc::new(Mutex::new(Vec::new()));

    // The metronome. It ticks whether or not anything is connected, because
    // a cadence that starts and stops with an audience is itself a signal.
    {
        let state = Arc::clone(&state);
        let connections = Arc::clone(&connections);
        let stop = Arc::clone(&stop);
        let period = Duration::from_millis(u64::from(pacing.tick_ms));
        std::thread::spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                std::thread::sleep(period);

                // Lock order: state, then the list, then each connection.
                let Ok(mut s) = state.lock() else { break };
                tick(&mut s);
                let Ok(mut conns) = connections.lock() else {
                    break;
                };
                conns.retain(|c| c.is_alive());

                // Build every frame under the locks, write none of them: a
                // blocked socket must not stall the metronome, and a stalled
                // metronome would be a visible gap in the cadence.
                let mut outbound: Vec<(Arc<LiveConnection>, Vec<u8>)> = Vec::new();
                for conn in conns.iter() {
                    let Ok(mut proto) = conn.proto.lock() else {
                        conn.retire();
                        continue;
                    };
                    if let Some(bytes) = proto.notification(&s) {
                        outbound.push((Arc::clone(conn), bytes));
                    }
                }
                drop(conns);
                drop(s);

                for (conn, bytes) in outbound {
                    let Ok(mut stream) = conn.write.lock() else {
                        conn.retire();
                        continue;
                    };
                    if stream.write_all(&bytes).is_err() {
                        conn.retire();
                    }
                }
            }
        });
    }

    {
        let state = Arc::clone(&state);
        let connections = Arc::clone(&connections);
        let stop = Arc::clone(&stop);
        std::thread::spawn(move || {
            for incoming in listener.incoming() {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                let Ok(stream) = incoming else { continue };
                if stream.set_read_timeout(Some(IDLE_TIMEOUT)).is_err() {
                    continue;
                }
                // Refuse rather than queue past the cap, so a peer opening
                // sockets cannot grow our thread count without bound.
                let Ok(mut conns) = connections.lock() else {
                    break;
                };
                // Drop entries whose thread has exited before testing the
                // cap, or routine controller reconnects would fill it.
                conns.retain(|c| c.is_alive());
                if conns.len() >= MAX_CONNECTIONS {
                    continue;
                }
                let Ok(connection) = Connection::new(&setup_code) else {
                    continue;
                };
                let Ok(peer) = stream.try_clone() else {
                    continue;
                };
                let live = Arc::new(LiveConnection {
                    proto: Mutex::new(connection),
                    write: Mutex::new(stream),
                    alive: AtomicBool::new(true),
                });
                conns.push(Arc::clone(&live));
                drop(conns);

                let state = Arc::clone(&state);
                std::thread::spawn(move || {
                    let mut reader = peer;
                    let mut buf = [0u8; 4096];
                    loop {
                        let n = match reader.read(&mut buf) {
                            Ok(0) | Err(_) => break,
                            Ok(n) => n,
                        };
                        // Same order as the metronome: state, then proto.
                        // Both are released before the socket write, which
                        // takes only the leaf `write` lock.
                        let out = {
                            let Ok(mut s) = state.lock() else { break };
                            let Ok(mut proto) = live.proto.lock() else {
                                break;
                            };
                            proto.feed(&buf[..n], &mut s)
                        };
                        if !out.is_empty() {
                            let Ok(mut stream) = live.write.lock() else {
                                break;
                            };
                            if stream.write_all(&out).is_err() {
                                break;
                            }
                        }
                    }
                    live.retire();
                });
            }
        });
    }

    Ok(HapServer { state, stop, addr })
}

/// Build the default fleet state for a set of named Canaries.
pub fn state_for(
    identity: AccessoryIdentity,
    bridge_name: impl Into<String>,
    setup_code: impl Into<String>,
    canaries: &[(&str, &str)],
) -> ServerState {
    let fleet = canaries
        .iter()
        .enumerate()
        .map(|(i, (name, serial))| {
            // aid 1 is the bridge, so bridged accessories start at 2.
            CanaryAccessory::new(BRIDGE_AID + 1 + i as u64, *name, *serial)
        })
        .collect();
    ServerState {
        identity,
        pairings: PairingStore::new(),
        fleet,
        bridge_name: bridge_name.into(),
        setup_code: setup_code.into(),
        config_number: 1,
        setup_attempts: 0,
    }
}

/// Which signals the projection publishes, for the Doctor card and the app.
pub fn enabled_summary(state: &ServerState) -> Vec<(u64, Vec<&'static str>)> {
    state
        .fleet
        .iter()
        .map(|a| {
            let mut sigs: Vec<&'static str> = HomeSignal::ALL
                .into_iter()
                .filter(|s| a.enabled.contains(*s))
                .map(|s| s.as_str())
                .collect();
            sigs.sort_unstable();
            (a.aid, sigs)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bridge::homekit::HomeSignal;
    use ed25519_dalek::{Signer, SigningKey};
    use sha2::Sha512;
    use srp::ClientG3072;
    use x25519_dalek::{PublicKey as XPublicKey, StaticSecret};

    use super::super::crypto::{self, kdf, nonce};
    use super::super::pairing::SessionKeys;

    const CODE: &str = "123-45-678";

    /// A controller that speaks the whole stack — HTTP, TLV8, pairing and
    /// the encrypted session — written against the spec rather than against
    /// the server's internals, so a shared misunderstanding cannot make a
    /// test pass.
    struct Controller {
        id: String,
        signing: SigningKey,
        session: Option<Session>,
    }

    impl Controller {
        fn new(id: &str) -> Self {
            Controller {
                id: id.into(),
                signing: SigningKey::from_bytes(&[9u8; 32]),
                session: None,
            }
        }

        fn ltpk(&self) -> [u8; 32] {
            self.signing.verifying_key().to_bytes()
        }

        /// Build a request, encrypting it if the session is up.
        fn request(&mut self, method: &str, target: &str, ct: &str, body: &[u8]) -> Vec<u8> {
            let mut raw = format!(
                "{method} {target} HTTP/1.1\r\nContent-Type: {ct}\r\nContent-Length: {}\r\n\r\n",
                body.len()
            )
            .into_bytes();
            raw.extend_from_slice(body);
            match self.session.as_mut() {
                Some(s) => s.seal(&raw),
                None => raw,
            }
        }

        /// Decrypt a response if the session is up, and return its body.
        fn read_body(&mut self, wire: &[u8]) -> Vec<u8> {
            let plain = match self.session.as_mut() {
                Some(s) => {
                    let mut buf = wire.to_vec();
                    let mut out = Vec::new();
                    while let Ok(Some(frame)) = s.open_frame(&mut buf) {
                        out.extend_from_slice(&frame);
                    }
                    out
                }
                None => wire.to_vec(),
            };
            Request::parse(&plain)
                .ok()
                .flatten()
                .map(|(r, _)| r.body)
                // A response is not a request; parse it loosely by finding
                // the header terminator.
                .unwrap_or_else(|| match plain.windows(4).position(|w| w == b"\r\n\r\n") {
                    Some(i) => plain[i + 4..].to_vec(),
                    None => Vec::new(),
                })
        }

        fn status(wire: &[u8]) -> u16 {
            std::str::from_utf8(wire)
                .ok()
                .and_then(|s| s.split(' ').nth(1).and_then(|c| c.parse().ok()))
                .unwrap_or(0)
        }

        fn pair_setup(&mut self, conn: &mut Connection, state: &mut ServerState, code: &str) {
            let mut m1 = Tlv::new();
            m1.push_u8(ty::STATE, 1)
                .push_u8(ty::METHOD, method::PAIR_SETUP);
            let wire = self.request("POST", "/pair-setup", content_type::TLV8, &m1.encode());
            let body = self.read_body(&conn.feed(&wire, state));
            let m2 = Tlv::decode(&body).expect("m2");
            let b_pub = m2.get(ty::PUBLIC_KEY).expect("B").to_vec();
            let salt = m2.get(ty::SALT).expect("salt").to_vec();

            let a = [5u8; 64];
            let client = ClientG3072::<Sha512>::new();
            let a_pub = client.compute_public_ephemeral(&a);
            let cv = client
                .process_reply(&a, b"Pair-Setup", code.as_bytes(), &salt, &b_pub)
                .expect("srp");
            let mut m3 = Tlv::new();
            m3.push_u8(ty::STATE, 3)
                .push(ty::PUBLIC_KEY, a_pub)
                .push(ty::PROOF, cv.proof().to_vec());
            let wire = self.request("POST", "/pair-setup", content_type::TLV8, &m3.encode());
            let body = self.read_body(&conn.feed(&wire, state));
            let m4 = Tlv::decode(&body).expect("m4");
            let srp_key = cv
                .verify_server(m4.get(ty::PROOF).expect("proof"))
                .expect("server proof")
                .to_vec();

            let sk = crypto::hkdf_sha512(
                &srp_key,
                kdf::PAIR_SETUP_ENCRYPT_SALT,
                kdf::PAIR_SETUP_ENCRYPT_INFO,
            );
            let x = crypto::hkdf_sha512(
                &srp_key,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_SALT,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_INFO,
            );
            let mut info = Vec::new();
            info.extend_from_slice(&x);
            info.extend_from_slice(self.id.as_bytes());
            info.extend_from_slice(&self.ltpk());
            let sig = self.signing.sign(&info).to_bytes();
            let mut sub = Tlv::new();
            sub.push(ty::IDENTIFIER, self.id.as_bytes().to_vec())
                .push(ty::PUBLIC_KEY, self.ltpk().to_vec())
                .push(ty::SIGNATURE, sig.to_vec());
            let mut m5 = Tlv::new();
            m5.push_u8(ty::STATE, 5).push(
                ty::ENCRYPTED_DATA,
                crypto::seal(&sk, nonce::PS_MSG05, b"", &sub.encode()),
            );
            let wire = self.request("POST", "/pair-setup", content_type::TLV8, &m5.encode());
            let body = self.read_body(&conn.feed(&wire, state));
            let m6 = Tlv::decode(&body).expect("m6");
            assert_eq!(m6.get_u8(ty::STATE), Some(6), "pair setup completed");
        }

        fn pair_verify(&mut self, conn: &mut Connection, state: &mut ServerState) {
            let mut scalar = [0u8; 32];
            scalar[0] = 77;
            let secret = StaticSecret::from(scalar);
            let my_pk = XPublicKey::from(&secret).to_bytes();

            let mut m1 = Tlv::new();
            m1.push_u8(ty::STATE, 1)
                .push(ty::PUBLIC_KEY, my_pk.to_vec());
            let wire = self.request("POST", "/pair-verify", content_type::TLV8, &m1.encode());
            let body = self.read_body(&conn.feed(&wire, state));
            let m2 = Tlv::decode(&body).expect("m2");
            let acc_pk: [u8; 32] = m2.get(ty::PUBLIC_KEY).expect("pk").try_into().unwrap();
            let shared = secret.diffie_hellman(&XPublicKey::from(acc_pk)).to_bytes();
            let sk = crypto::hkdf_sha512(
                &shared,
                kdf::PAIR_VERIFY_ENCRYPT_SALT,
                kdf::PAIR_VERIFY_ENCRYPT_INFO,
            );

            let mut info = Vec::new();
            info.extend_from_slice(&my_pk);
            info.extend_from_slice(self.id.as_bytes());
            info.extend_from_slice(&acc_pk);
            let sig = self.signing.sign(&info).to_bytes();
            let mut sub = Tlv::new();
            sub.push(ty::IDENTIFIER, self.id.as_bytes().to_vec())
                .push(ty::SIGNATURE, sig.to_vec());
            let mut m3 = Tlv::new();
            m3.push_u8(ty::STATE, 3).push(
                ty::ENCRYPTED_DATA,
                crypto::seal(&sk, nonce::PV_MSG03, b"", &sub.encode()),
            );
            let wire = self.request("POST", "/pair-verify", content_type::TLV8, &m3.encode());
            let body = self.read_body(&conn.feed(&wire, state));
            let m4 = Tlv::decode(&body).expect("m4");
            assert_eq!(m4.get_u8(ty::STATE), Some(4), "pair verify completed");

            // From here on the controller speaks encrypted, mirroring the
            // accessory's key directions.
            self.session = Some(Session::new(SessionKeys {
                read: crypto::hkdf_sha512(&shared, kdf::CONTROL_SALT, kdf::CONTROL_WRITE_INFO),
                write: crypto::hkdf_sha512(&shared, kdf::CONTROL_SALT, kdf::CONTROL_READ_INFO),
            }));
        }
    }

    fn fixture() -> (ServerState, Connection) {
        let identity = AccessoryIdentity::from_seed("AA:BB:CC:DD:EE:FF", [3u8; 32]);
        let state = state_for(identity, "SecuraCV", CODE, &[("Porch Canary", "CNRY-1")]);
        let conn = Connection::new(CODE).expect("connection");
        (state, conn)
    }

    /// The whole thing, end to end: pair, verify, read the accessory
    /// database over the encrypted session, subscribe, tick, get notified.
    #[test]
    fn a_controller_pairs_reads_subscribes_and_is_notified() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");

        ctrl.pair_setup(&mut conn, &mut state, CODE);
        assert_eq!(state.pairings.len(), 1);
        ctrl.pair_verify(&mut conn, &mut state);
        assert!(conn.is_secure(), "session established after M4");

        // /accessories, now encrypted.
        let wire = ctrl.request("GET", "/accessories", content_type::JSON, b"");
        let body = ctrl.read_body(&conn.feed(&wire, &mut state));
        let doc = String::from_utf8(body).expect("utf8");
        assert!(doc.contains("Porch Canary"), "got: {doc}");
        assert!(doc.contains(r#""aid":2"#));

        // Subscribe to the motion characteristic.
        let motion_iid = state.fleet[0]
            .characteristics()
            .into_iter()
            .find(|(_, s)| *s == HomeSignal::Motion)
            .map(|(i, _)| i)
            .expect("motion");
        let put = format!(r#"{{"characteristics":[{{"aid":2,"iid":{motion_iid},"ev":true}}]}}"#);
        let wire = ctrl.request(
            "PUT",
            "/characteristics",
            content_type::JSON,
            put.as_bytes(),
        );
        let out = conn.feed(&wire, &mut state);
        assert!(!out.is_empty());
        // Consume the response: every sealed frame must be opened, or the
        // two ends' nonce counters drift apart and nothing decrypts after.
        ctrl.read_body(&out);
        assert!(conn.subscriptions().contains(&(2, motion_iid)));

        // Drive a motion event through the projection, then tick.
        let mut fleet = Fleet::new([2u64], PacingConfig::default()).expect("fleet");
        fleet
            .projection_mut(2)
            .expect("projection")
            .set_level(HomeSignal::Occupancy, true);
        let changed = fleet.tick(&mut state);
        assert!(!changed.is_empty(), "occupancy became true on the tick");

        // A subscribed controller is notified; the notification is encrypted.
        fleet
            .projection_mut(2)
            .expect("projection")
            .observe_event(crate::EventType::BoundaryCrossingObjectLarge, None);
        let changed = fleet.tick(&mut state);
        assert!(
            changed.iter().any(|(aid, _)| *aid == 2),
            "the event must have moved a characteristic, or the assertion below is vacuous"
        );
        let note = conn.notification(&state).expect("motion notification");
        let decoded = ctrl.read_body(&note);
        let text = String::from_utf8(decoded).expect("utf8");
        assert!(text.contains(r#""value":true"#), "got: {text}");
    }

    /// Everything past pairing must refuse an unauthenticated connection
    /// with HAP's own 470, not a 401 that controllers give up on.
    #[test]
    fn unauthenticated_requests_are_refused_with_470() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("nobody");
        for (m, path) in [
            ("GET", "/accessories"),
            ("GET", "/characteristics?id=2.11"),
            ("PUT", "/characteristics"),
            ("POST", "/pairings"),
        ] {
            let wire = ctrl.request(m, path, content_type::JSON, b"{}");
            let out = conn.feed(&wire, &mut state);
            assert_eq!(Controller::status(&out), 470, "{m} {path}");
        }
    }

    /// A controller that never paired must not be able to verify, so the
    /// session never opens and the fleet stays private.
    #[test]
    fn an_unpaired_controller_never_gets_a_session() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("stranger");

        let mut scalar = [0u8; 32];
        scalar[0] = 5;
        let secret = StaticSecret::from(scalar);
        let my_pk = XPublicKey::from(&secret).to_bytes();
        let mut m1 = Tlv::new();
        m1.push_u8(ty::STATE, 1)
            .push(ty::PUBLIC_KEY, my_pk.to_vec());
        let wire = ctrl.request("POST", "/pair-verify", content_type::TLV8, &m1.encode());
        let body = ctrl.read_body(&conn.feed(&wire, &mut state));
        let m2 = Tlv::decode(&body).expect("tlv");
        // M2 succeeds (the accessory does not yet know who this is), but M3
        // cannot, and no session is ever established.
        assert!(m2.get(ty::PUBLIC_KEY).is_some());
        assert!(!conn.is_secure());
    }

    /// Notifications only go to controllers that asked for them.
    #[test]
    fn an_unsubscribed_controller_is_not_notified() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);

        let mut fleet = Fleet::new([2u64], PacingConfig::default()).expect("fleet");
        fleet
            .projection_mut(2)
            .expect("p")
            .set_level(HomeSignal::Occupancy, true);
        let changed = fleet.tick(&mut state);
        assert!(!changed.is_empty());
        assert!(
            conn.notification(&state).is_none(),
            "no subscription, no notification"
        );
    }

    /// A subscription to a characteristic that does not exist must not grow
    /// the subscription set (FR-4).
    #[test]
    fn subscriptions_are_bounded_to_real_characteristics() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);

        let put = r#"{"characteristics":[{"aid":9,"iid":9999,"ev":true},{"aid":2,"iid":424242,"ev":true}]}"#;
        let wire = ctrl.request(
            "PUT",
            "/characteristics",
            content_type::JSON,
            put.as_bytes(),
        );
        conn.feed(&wire, &mut state);
        assert!(conn.subscriptions().is_empty());
    }

    /// A class-scoped signal must not appear in the accessory database until
    /// a human turns it on — the dumb-PIR bar, enforced at the wire.
    #[test]
    fn class_scoped_signals_are_absent_from_the_database_by_default() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);

        let wire = ctrl.request("GET", "/accessories", content_type::JSON, b"");
        let doc = String::from_utf8(ctrl.read_body(&conn.feed(&wire, &mut state))).expect("utf8");
        assert!(!doc.contains("Person"), "got: {doc}");
        assert!(doc.contains("Motion"));
    }

    /// A corrupt frame must end the session rather than resynchronize.
    #[test]
    fn a_corrupt_frame_tears_down_the_session() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);
        assert!(conn.is_secure());

        let mut wire = ctrl.request("GET", "/accessories", content_type::JSON, b"");
        let n = wire.len();
        wire[n - 1] ^= 0xFF;
        let out = conn.feed(&wire, &mut state);
        assert!(out.is_empty(), "no response to an unauthenticated frame");
        assert!(!conn.is_secure(), "session torn down");
    }

    #[test]
    fn reading_an_unknown_characteristic_returns_a_hap_status() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);

        let wire = ctrl.request(
            "GET",
            "/characteristics?id=2.99999",
            content_type::JSON,
            b"",
        );
        let text = String::from_utf8(ctrl.read_body(&conn.feed(&wire, &mut state))).expect("utf8");
        assert!(text.contains("-70409"), "got: {text}");
    }

    /// Pairing twice must be refused — the accessory belongs to one home.
    #[test]
    fn a_second_pair_setup_is_refused() {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);

        let mut conn2 = Connection::new(CODE).expect("conn");
        let mut m1 = Tlv::new();
        m1.push_u8(ty::STATE, 1)
            .push_u8(ty::METHOD, method::PAIR_SETUP);
        let mut ctrl2 = Controller::new("controller-2");
        let wire = ctrl2.request("POST", "/pair-setup", content_type::TLV8, &m1.encode());
        let body = ctrl2.read_body(&conn2.feed(&wire, &mut state));
        let reply = Tlv::decode(&body).expect("tlv");
        assert_eq!(reply.get_u8(ty::ERROR), Some(tlv_error::UNAVAILABLE));
    }

    /// The setup URI is what a QR code carries. The encoding is fixed by the
    /// spec, so it is asserted rather than eyeballed.
    #[test]
    fn the_setup_uri_encodes_the_code_and_category() {
        let uri = setup_uri("123-45-678", "7OSX", CATEGORY_BRIDGE);
        assert!(uri.starts_with("X-HM://"));
        // 7 for the scheme, 9 payload characters, then the 4-character
        // setup id.
        assert_eq!(uri.len(), 7 + 9 + 4);
        assert!(uri.ends_with("7OSX"));
        // Different codes must not collide.
        assert_ne!(uri, setup_uri("123-45-679", "7OSX", CATEGORY_BRIDGE));
        // A code with no dashes is the same code.
        assert_eq!(uri, setup_uri("12345678", "7OSX", CATEGORY_BRIDGE));
    }

    /// `sf` is the flag a controller reads to decide whether to offer
    /// pairing at all; getting it backwards makes the accessory invisible.
    #[test]
    fn the_discoverable_flag_follows_the_pairing_state() {
        let unpaired = txt_records("AA:BB", 1, false, CATEGORY_BRIDGE);
        assert_eq!(unpaired.get("sf").map(String::as_str), Some("1"));
        let paired = txt_records("AA:BB", 2, true, CATEGORY_BRIDGE);
        assert_eq!(paired.get("sf").map(String::as_str), Some("0"));
        assert_eq!(paired.get("c#").map(String::as_str), Some("2"));
        assert_eq!(paired.get("ci").map(String::as_str), Some("2"));
        assert_eq!(paired.get("pv").map(String::as_str), Some("1.1"));
    }

    /// Pairing changes what the accessory advertises, so the config number
    /// must move or controllers keep a stale cache.
    #[test]
    fn the_config_number_advances_when_pairing_completes() {
        let (mut state, mut conn) = fixture();
        let before = state.config_number;
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        assert!(state.config_number > before);
    }

    #[test]
    fn the_fleet_ticks_every_accessory_together() {
        let identity = AccessoryIdentity::from_seed("AA:BB", [3u8; 32]);
        let mut state = state_for(
            identity,
            "SecuraCV",
            CODE,
            &[("Porch", "P-1"), ("Garage", "G-1")],
        );
        assert_eq!(state.fleet.len(), 2);
        assert_eq!(state.fleet[0].aid, 2);
        assert_eq!(state.fleet[1].aid, 3);

        let mut fleet = Fleet::new([2u64, 3], PacingConfig::default()).expect("fleet");
        for _ in 0..3 {
            fleet.tick(&mut state);
        }
        assert_eq!(state.fleet[0].last.seq, 3, "both ticked the same number");
        assert_eq!(state.fleet[1].last.seq, 3);
    }

    #[test]
    fn the_summary_reports_the_default_signal_set() {
        let identity = AccessoryIdentity::from_seed("AA:BB", [3u8; 32]);
        let state = state_for(identity, "SecuraCV", CODE, &[("Porch", "P-1")]);
        let summary = enabled_summary(&state);
        assert_eq!(summary.len(), 1);
        let (aid, signals) = &summary[0];
        assert_eq!(*aid, 2);
        assert!(signals.contains(&"motion"));
        assert!(signals.contains(&"tamper"));
        assert!(
            !signals.contains(&"motion_person"),
            "class-scoped signals are off by default"
        );
    }

    #[test]
    fn junk_on_the_socket_does_not_panic() {
        let (mut state, mut conn) = fixture();
        for junk in [
            &b"not http at all"[..],
            &b"GET\r\n\r\n"[..],
            &[0xFF; 64][..],
        ] {
            let _ = conn.feed(junk, &mut state);
        }
    }

    /// A paired, verified controller subscribed to this Canary's motion
    /// characteristic — the starting point for the cover-traffic tests.
    fn paired_and_subscribed() -> (ServerState, Connection, Controller) {
        let (mut state, mut conn) = fixture();
        let mut ctrl = Controller::new("controller-1");
        ctrl.pair_setup(&mut conn, &mut state, CODE);
        ctrl.pair_verify(&mut conn, &mut state);

        let motion_iid = state.fleet[0]
            .characteristics()
            .into_iter()
            .find(|(_, s)| *s == HomeSignal::Motion)
            .map(|(i, _)| i)
            .expect("motion");
        let put = format!(r#"{{"characteristics":[{{"aid":2,"iid":{motion_iid},"ev":true}}]}}"#);
        let wire = ctrl.request(
            "PUT",
            "/characteristics",
            content_type::JSON,
            put.as_bytes(),
        );
        let out = conn.feed(&wire, &mut state);
        ctrl.read_body(&out);
        (state, conn, ctrl)
    }

    /// The property the pacer exists for, asserted at the **socket** layer
    /// rather than at the projection layer.
    ///
    /// A previous version sent a frame only when a subscribed value changed,
    /// so an observer could tell an event-bearing tick from an idle one just
    /// by watching for traffic. Both the frame *count* and the frame *length*
    /// must be identical whether or not anything happened.
    #[test]
    fn every_tick_emits_one_frame_of_constant_size() {
        let (mut state, mut conn, _ctrl) = paired_and_subscribed();
        let mut fleet = Fleet::new([2u64], PacingConfig::default()).expect("fleet");

        let mut quiet = Vec::new();
        for _ in 0..10 {
            fleet.tick(&mut state);
            quiet.push(
                conn.notification(&state)
                    .expect("a frame on every tick, even an idle one")
                    .len(),
            );
        }

        fleet
            .projection_mut(2)
            .expect("p")
            .observe_event(crate::EventType::BoundaryCrossingObjectLarge, None);
        fleet.tick(&mut state);
        let busy = conn.notification(&state).expect("frame").len();
        assert!(
            state.fleet[0].last.asserted.contains(HomeSignal::Motion),
            "the event must actually have asserted motion, or this proves nothing"
        );

        fleet
            .projection_mut(2)
            .expect("p")
            .set_level(HomeSignal::Occupancy, true);
        fleet.tick(&mut state);
        let busier = conn.notification(&state).expect("frame").len();

        let baseline = quiet[0];
        assert!(
            quiet.iter().all(|n| *n == baseline),
            "idle ticks vary in size: {quiet:?}"
        );
        assert_eq!(
            busy, baseline,
            "an event-bearing tick is distinguishable by frame size"
        );
        assert_eq!(
            busier, baseline,
            "a second asserted signal is distinguishable by frame size"
        );
    }

    /// The padding must not break the JSON a controller has to parse.
    #[test]
    fn padded_notifications_are_still_valid_json() {
        let (mut state, mut conn, mut ctrl) = paired_and_subscribed();
        let mut fleet = Fleet::new([2u64], PacingConfig::default()).expect("fleet");
        fleet
            .projection_mut(2)
            .expect("p")
            .observe_event(crate::EventType::BoundaryCrossingObjectLarge, None);
        fleet.tick(&mut state);

        let frame = conn.notification(&state).expect("frame");
        let text = String::from_utf8(ctrl.read_body(&frame)).expect("utf8");
        let parsed: serde_json::Value =
            serde_json::from_str(&text).expect("a controller must be able to parse this");
        assert!(parsed["characteristics"].is_array(), "got: {text:?}");
        assert!(text.ends_with(' '), "expected trailing pad, got: {text:?}");
    }
}
