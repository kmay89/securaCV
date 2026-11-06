//! Fleet book backend: find every Canary on this network, then speak each
//! device's own local API for status, identify, and over-the-air updates.
//!
//! Discovery is a real mDNS browse of `_securacv._tcp` — the service every
//! networked Canary advertises with a canonical TXT schema (device_id, name,
//! fw, model, dt, role; see docs/onboarding_unified_wizard.md). Browsing TXT
//! records reaches ALL variants, including vision/sense which run no HTTP
//! server at all, and it names each device's firmware version without a
//! single connection — the fleet book's "who's here and who's behind" column
//! comes straight from this.
//!
//! Device calls are a fixed verb set against a fixed path set — never an
//! arbitrary URL/path proxy — and only ever to a private/local host. The OTA
//! trigger is the device's own `POST /api/ota/install`: the DEVICE downloads
//! the signed manifest and image, verifies Ed25519 + SHA-256 against its
//! pinned key, and A/B-swaps with rollback (docs/firmware_ota.md). This app
//! never serves firmware bytes to a device — it only rings the bell, with
//! the bearer credential the flasher itself seeded at flash time.

use serde::Serialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};

const SERVICE_TYPE: &str = "_securacv._tcp.local.";

/// One device seen on the network, straight from its mDNS announcement.
#[derive(Serialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub struct FleetSighting {
    pub device_id: String,
    pub name: String,
    /// The device's own mDNS hostname (e.g. `canary-sense-001-a1b2c3.local.`).
    pub host: String,
    /// Best resolved address (IPv4 preferred — the LAN case).
    pub ip: Option<String>,
    pub port: u16,
    pub fw: String,
    pub model: String,
    /// Canonical device type from TXT `dt` (`canary-wap`, `canary-sense`, …).
    pub device_type: String,
    /// `witness` or `display`.
    pub role: String,
}

/// Browse `_securacv._tcp` for a bounded window and return every device that
/// answered, one entry per device_id (latest announcement wins).
#[tauri::command]
pub async fn fleet_scan(timeout_ms: Option<u64>) -> Result<Vec<FleetSighting>, String> {
    let wait = timeout_ms.unwrap_or(2500).clamp(500, 8000);
    tauri::async_runtime::spawn_blocking(move || scan_blocking(wait))
        .await
        .map_err(|e| format!("scan thread failed: {e}"))?
}

fn scan_blocking(wait_ms: u64) -> Result<Vec<FleetSighting>, String> {
    let daemon =
        mdns_sd::ServiceDaemon::new().map_err(|e| format!("couldn't open an mDNS browser: {e}"))?;
    let receiver = daemon
        .browse(SERVICE_TYPE)
        .map_err(|e| format!("couldn't browse for Canaries: {e}"))?;

    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(wait_ms);
    let mut seen: HashMap<String, FleetSighting> = HashMap::new();
    loop {
        let left = deadline.saturating_duration_since(std::time::Instant::now());
        if left.is_zero() {
            break;
        }
        match receiver.recv_timeout(left) {
            Ok(mdns_sd::ServiceEvent::ServiceResolved(info)) => {
                let txt = |key: &str| {
                    info.get_property_val_str(key)
                        .unwrap_or_default()
                        .to_string()
                };
                let device_id = txt("device_id");
                let name = txt("name");
                // Prefer IPv4 — that's what the LAN HTTP calls below want.
                // mdns-sd 0.21 hands back interface-scoped addresses; the
                // fleet book only needs the bare IP for its LAN HTTP calls.
                let mut addrs: Vec<IpAddr> = info
                    .get_addresses()
                    .iter()
                    .map(|a| a.to_ip_addr())
                    .collect();
                addrs.sort_by_key(|a| match a {
                    IpAddr::V4(_) => 0,
                    IpAddr::V6(_) => 1,
                });
                let sighting = FleetSighting {
                    // The older PIO canary announces without device_id in TXT;
                    // its instance name still identifies it well enough to list.
                    device_id: if device_id.is_empty() {
                        info.get_fullname()
                            .split('.')
                            .next()
                            .unwrap_or_default()
                            .to_string()
                    } else {
                        device_id
                    },
                    name,
                    host: info.get_hostname().trim_end_matches('.').to_string(),
                    ip: addrs.first().map(|a| a.to_string()),
                    port: info.get_port(),
                    fw: txt("fw"),
                    model: txt("model"),
                    device_type: txt("dt"),
                    role: txt("role"),
                };
                seen.insert(sighting.device_id.clone(), sighting);
            }
            Ok(_) => {}
            Err(_) => break, // window elapsed or channel closed — either way, done
        }
    }
    let _ = daemon.shutdown();
    let mut list: Vec<FleetSighting> = seen.into_values().collect();
    list.sort_by(|a, b| a.device_id.cmp(&b.device_id));
    Ok(list)
}

// ── the device's own local API ──────────────────────────────────────────────

/// Is this one address on a network this computer can be on? RFC 1918,
/// 100.64/10 (carrier-grade NAT, which some home routers hand out on the LAN
/// side), loopback and link-local; for IPv6 `::1`, fe80::/10, fc00::/7 —
/// and the IPv4-mapped form (`::ffff:a.b.c.d`) of any of the IPv4 ranges,
/// judged as the IPv4 it carries: without that, `::ffff:8.8.8.8` is neither
/// loopback nor link-local and would pass. `pub(crate)` because the hub
/// commands (hub.rs) classify the addresses they dial the same way.
pub(crate) fn ip_is_local(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => v4_is_local(v4),
        IpAddr::V6(v6) => {
            if let Some(v4) = v6.to_ipv4_mapped() {
                return v4_is_local(v4);
            }
            v6.is_loopback()
                // fe80::/10 link-local and fc00::/7 unique-local
                || (v6.segments()[0] & 0xffc0) == 0xfe80
                || (v6.segments()[0] & 0xfe00) == 0xfc00
        }
    }
}

fn v4_is_local(v4: std::net::Ipv4Addr) -> bool {
    let o = v4.octets();
    v4.is_private()
        || v4.is_loopback()
        || v4.is_link_local()
        // 100.64.0.0/10 (RFC 6598)
        || (o[0] == 100 && (o[1] & 0xc0) == 64)
}

/// The NAME gate: only ever talk to a host that can be on this network —
/// `.local`-style names, single-label LAN hostnames, or private/loopback/
/// link-local IP literals. Mirrors the firmware's own local-transport policy
/// (docs/firmware_ota.md §Transport) — a fleet-book call must never be
/// steerable at an internet host.
///
/// A name is only half the answer: `.lan`, `.internal`, `.home.arpa` and a
/// single label all resolve through whatever resolver the OS has, which can
/// be unicast DNS answering anything at all. So no connection is dialed on
/// this verdict alone — `resolve_local` resolves the name and judges every
/// address, and the clients pin to those addresses. `pub(crate)` because the
/// hub commands (hub.rs) carry the owner's typed credentials and hold to the
/// same rule.
pub(crate) fn host_is_local(host: &str) -> bool {
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() {
        return false;
    }
    if let Ok(ip) = host.parse::<IpAddr>() {
        return ip_is_local(ip);
    }
    let lower = host.to_ascii_lowercase();
    for suffix in [".local", ".lan", ".internal", ".home.arpa"] {
        if lower.ends_with(suffix) && lower.len() > suffix.len() {
            return true;
        }
    }
    // A single-label hostname ("canary-3f2a") can only resolve locally.
    !lower.contains('.')
}

/// Every address a name resolved to must be local, or the name is refused:
/// one public answer among local ones is still a connection that leaves the
/// LAN (and a resolver that answers both is exactly a rebinding setup). An
/// empty answer is refused too — there is nothing to pin a connection to.
/// Pure over the resolver's answer, so the rule is unit-tested.
pub(crate) fn addrs_all_local(
    host: &str,
    addrs: Vec<SocketAddr>,
) -> Result<Vec<SocketAddr>, String> {
    if addrs.is_empty() {
        return Err(format!("{host} didn't resolve to any address"));
    }
    if let Some(bad) = addrs.iter().find(|a| !ip_is_local(a.ip())) {
        return Err(format!(
            "{host} resolves to {}, which is not on this network — refusing to connect",
            bad.ip()
        ));
    }
    Ok(addrs)
}

/// Resolve a host (a bare name or IP literal; IPv6 brackets allowed) into
/// the addresses a connection may be pinned to. The name gate first
/// (`host_is_local`), then the OS resolver (Bonjour / avahi answer `.local`,
/// the same way the connection would have resolved it), then every answer
/// judged (`addrs_all_local`). A literal resolves to itself.
pub(crate) async fn resolve_local(host: &str) -> Result<Vec<SocketAddr>, String> {
    let bare = host.trim_matches(['[', ']']).to_string();
    if !host_is_local(&bare) {
        return Err(format!("{host} is not a local/private host"));
    }
    if let Ok(ip) = bare.parse::<IpAddr>() {
        return Ok(vec![SocketAddr::new(ip, 0)]);
    }
    let name = bare.clone();
    let addrs = tauri::async_runtime::spawn_blocking(move || {
        use std::net::ToSocketAddrs;
        (name.as_str(), 0u16)
            .to_socket_addrs()
            .map(|it| it.collect::<Vec<_>>())
            .map_err(|e| format!("couldn't resolve {name}: {e}"))
    })
    .await
    .map_err(|e| format!("resolver thread failed: {e}"))??;
    addrs_all_local(&bare, addrs)
}

/// The host of `http://host[:port][/…]` (or https), brackets kept on an
/// IPv6 literal. `None` for anything that isn't such a URL.
pub(crate) fn base_host(base: &str) -> Option<&str> {
    let rest = base
        .strip_prefix("http://")
        .or_else(|| base.strip_prefix("https://"))?;
    let host_port = rest.split('/').next().unwrap_or_default();
    if host_port.is_empty() {
        return None;
    }
    // Split a trailing :port (careful with bracketed IPv6).
    let host = if let Some(end) = host_port.rfind(']') {
        &host_port[..=end]
    } else if let Some((h, port)) = host_port.rsplit_once(':') {
        if port.chars().all(|c| c.is_ascii_digit()) && !port.is_empty() {
            h
        } else {
            host_port
        }
    } else {
        host_port
    };
    Some(host)
}

/// `http://host[:port]` (or https) with a local host — anything else is
/// refused before a socket opens. `pub(crate)` because `witness_discover`
/// (lib.rs) enforces the same policy over its candidate bases.
pub(crate) fn base_ok(base: &str) -> bool {
    base_host(base).is_some_and(host_is_local)
}

/// A client for ONE local base: proxy-free, timeout-bounded, and pinned to
/// the addresses `resolve_local` just vetted, so hyper never resolves the
/// name a second time — what was judged is what is dialed. Errors when the
/// base isn't a local URL or its name resolves anywhere off this network.
///
/// `.no_proxy()`: a LAN probe must never be routed through an OS-configured
/// proxy. reqwest defaults `auto_sys_proxy` on, and hyper-util's
/// client-proxy-system feature — which tauri-plugin-updater can switch on
/// from the other side of the graph, since Cargo unifies features on the one
/// shared hyper-util — makes that read the system proxy on macOS/Windows.
/// macOS applies no bypass list there, so `.local` and 192.168.x.x addresses
/// would leave the machine. The URL gate stops nothing on its own; only the
/// connection's own settings do.
pub(crate) async fn pinned_client(
    base: &str,
    user_agent: &'static str,
    timeout: std::time::Duration,
) -> Result<reqwest::Client, String> {
    let host =
        base_host(base).ok_or_else(|| "device address must be a local/private host".to_string())?;
    let addrs = resolve_local(host).await?;
    let bare = host.trim_matches(['[', ']']);
    let mut builder = reqwest::Client::builder()
        .user_agent(user_agent)
        .timeout(timeout)
        .no_proxy();
    if bare.parse::<IpAddr>().is_err() {
        // reqwest keys the override on the URL's host and ignores the port
        // in the pinned addresses (the URL's own port is used).
        builder = builder.resolve_to_addrs(bare, &addrs);
    }
    builder.build().map_err(|e| e.to_string())
}

/// The fixed verb set the fleet book may ring on a device. An enum-by-name,
/// not a path proxy: the frontend can only ever name one of these.
fn action_route(action: &str) -> Option<(reqwest::Method, &'static str)> {
    match action {
        "ota-status" => Some((reqwest::Method::GET, "/api/ota/status")),
        "ota-check" => Some((reqwest::Method::POST, "/api/ota/check")),
        "ota-install" => Some((reqwest::Method::POST, "/api/ota/install")),
        "status" => Some((reqwest::Method::GET, "/api/status")),
        "identify" => Some((reqwest::Method::POST, "/api/identify")),
        // The one deliberately unauthenticated read (the endpoint itself is
        // public by design): the fleet book probes it on :80 to recognize a
        // display that announced the old port-1 "formality" advert but does
        // in fact serve its glass page + /api/fleet there.
        "fleet" => Some((reqwest::Method::GET, "/api/fleet")),
        _ => None,
    }
}

// ── the sealed credential ───────────────────────────────────────────────────
// An mDNS announcement is just a packet: any host on the LAN can announce a
// victim's device_id with its own address, and a fresh signed challenge does
// not settle who holds the socket, because a peer that can reach the genuine
// board relays the challenge (whoami.rs). So a token never rides in the
// clear at all. Before a credential goes to a (device_id, address) pair the
// app agrees a session key with the device over X25519, signed by the
// identity key the fleet book pinned at flash time, and sends the token
// sealed under that key, bound to the method, the path and a counter
// (seal.rs; the firmware's api_seal.h). A host that is not the device gets
// a ciphertext it cannot open; one that relays gets one request it cannot
// read, replay or redirect. There is no plain-bearer fallback: a device whose
// firmware predates the handshake is told to update once over USB.

/// The sealed sessions this app holds, one per (device_id, base). A session
/// outlives nothing important: the device drops it after its TTL or a
/// reboot, and the next 401 that says so opens a new one.
#[derive(Default)]
pub struct SealState(std::sync::Mutex<HashMap<(String, String), crate::seal::Session>>);

/// What a handshake came to: a session, or the device's answer judged and
/// refused. A refusal is an ANSWER the row shows (the handshake's own
/// sentence, which always says the token stayed here), carried back to the
/// frontend as `refused` on the call's result rather than thrown, so the
/// OTA loop never mistakes it for a lost socket. Only a transport failure
/// is an `Err`.
enum Handshake {
    Session(crate::seal::Session),
    Refused(String),
}

/// The handshake, judged: `GET /api/device/seal?nonce=&client_pub=` at
/// `base`, the answer checked against the recorded fingerprint, a session
/// derived.
async fn seal_handshake(
    base: &str,
    device_id: &str,
    expected_fp: &str,
) -> Result<Handshake, String> {
    let hs = crate::seal::Handshake::new()?;
    // The route and field names below are the FIRMWARE's (seal_api.h,
    // api_seal::write_answer), pinned by canary-local/tests/desktop_parity.test.js
    // against its source: a drift fails as "no seal", which reads like old
    // firmware forever.
    let url = format!(
        "{}{}?{}",
        base.trim_end_matches('/'),
        crate::seal::ROUTE,
        hs.query()
    );
    let client = pinned_client(base, "SecuraCV-Flasher", std::time::Duration::from_secs(6)).await?;
    let resp = client
        .get(&url)
        .send()
        .await
        .map_err(|e| format!("Couldn't reach the device: {e}"))?;
    let status = resp.status().as_u16();
    if status == 404 {
        return Ok(Handshake::Refused(
            crate::seal::Refusal::Unsupported("no handshake route".into()).detail(),
        ));
    }
    if status != 200 {
        return Ok(Handshake::Refused(format!(
            "The device answered the handshake with HTTP {status}. The stored key wasn't sent."
        )));
    }
    let body: Value = match resp.json().await {
        Ok(b) => b,
        Err(e) => {
            return Ok(Handshake::Refused(format!(
                "The device's handshake answer wasn't JSON ({e}). The stored key wasn't sent."
            )))
        }
    };
    let field = |v: &Value, k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
    let seal = body.get("seal").cloned().unwrap_or(Value::Null);
    let answer = crate::seal::Answer {
        pubkey_hex: field(&body, "pubkey_hex"),
        sid: field(&seal, "sid"),
        device_pub_hex: field(&seal, "device_pub_hex"),
        sig_hex: field(&seal, "sig_hex"),
    };
    // Bind to the device_id WE expected, not the one the answer claims.
    Ok(match hs.accept(device_id, expected_fp, &answer) {
        Ok(session) => Handshake::Session(session),
        Err(r) => Handshake::Refused(r.detail()),
    })
}

/// The call's result when the handshake refused: no HTTP exchange carried
/// the token, and `refused` is the sentence for the row.
fn refused(detail: String) -> Value {
    json!({ "httpStatus": 0, "body": Value::Null, "sealed": true, "refused": detail })
}

/// Call one device-API action. Returns `{ httpStatus, body, sealed }`, or
/// `{ httpStatus: 0, refused }` when the handshake refused to seal the
/// token to what answered — a 401 or 403 and a refusal are ANSWERS (the
/// stored token isn't this device's; the device isn't the one the book
/// knows), not transport errors, so the UI can say the right thing.
///
/// With a non-empty `token`, `device_id` and `expected_fp` (the fingerprint
/// the fleet book recorded) are required, and the token is sent only sealed
/// under a session agreed with that key (`seal_handshake`), opened once per
/// (device, address) and again when the device says the session is gone
/// (`seal::on_refused`). The handshake itself carries no credential, and
/// nothing here ever sends a plain bearer.
#[tauri::command]
pub async fn fleet_device_call(
    state: tauri::State<'_, SealState>,
    base: String,
    token: String,
    action: String,
    device_id: Option<String>,
    expected_fp: Option<String>,
) -> Result<Value, String> {
    let (method, path) =
        action_route(&action).ok_or_else(|| format!("unknown device action {action:?}"))?;
    if !base_ok(&base) {
        return Err("device address must be a local/private host".into());
    }
    let device_id = device_id.unwrap_or_default();
    let expected_fp = expected_fp.unwrap_or_default();
    // The preflight refusals (a token with no device id, or no recorded
    // fingerprint to seal it to) are answers too, carried as `refused` like
    // the handshake's, never thrown: a throw is a lost socket to the OTA
    // loop, which would then narrate a reboot for an install never sent.
    let sealed = match crate::seal::seal_need(&token, &device_id, &expected_fp) {
        Ok(sealed) => sealed,
        Err(detail) => return Ok(refused(detail)),
    };
    let url = format!("{}{}", base.trim_end_matches('/'), path);
    // Installs make the device fetch + verify a whole image; its HTTP answer
    // can lag or the socket can drop at the reboot. Generous but bounded —
    // the UI keeps polling ota-status afterwards either way.
    let timeout = if action == "ota-install" { 20 } else { 6 };
    let client = pinned_client(
        &base,
        "SecuraCV-Flasher",
        std::time::Duration::from_secs(timeout),
    )
    .await?;
    let pair = (device_id.clone(), base.clone());
    // At most one re-handshake: the device rebooted (the OTA it just ran),
    // the session aged out, or it refused our counter. A second refusal
    // after a fresh session is an answer, not a transport hiccup.
    let mut rehandshaked = false;
    loop {
        let mut req = client.request(method.clone(), &url);
        if sealed {
            // The lock is never held across an await: the handshake runs
            // unlocked, and the session is sealed with in one short hold.
            let held = state
                .0
                .lock()
                .map_err(|_| "session cache poisoned")?
                .contains_key(&pair);
            if !held {
                let fresh = match seal_handshake(&base, &device_id, &expected_fp).await? {
                    Handshake::Session(s) => s,
                    Handshake::Refused(detail) => return Ok(refused(detail)),
                };
                state
                    .0
                    .lock()
                    .map_err(|_| "session cache poisoned")?
                    .insert(pair.clone(), fresh);
            }
            let header = {
                let mut sessions = state.0.lock().map_err(|_| "session cache poisoned")?;
                let session = sessions
                    .get_mut(&pair)
                    .ok_or_else(|| "the session was dropped while in use".to_string())?;
                session.seal(method.as_str(), path, &token)?
            };
            req = req.header(reqwest::header::AUTHORIZATION, header);
        }
        let resp = req
            .send()
            .await
            .map_err(|e| format!("Couldn't reach the device: {e}"))?;
        let status = resp.status().as_u16();
        let body: Value = resp.json().await.unwrap_or(Value::Null);
        if sealed && status == 401 {
            match crate::seal::on_refused(status, &body) {
                crate::seal::OnRefused::Rehandshake if !rehandshaked => {
                    rehandshaked = true;
                    state
                        .0
                        .lock()
                        .map_err(|_| "session cache poisoned")?
                        .remove(&pair);
                    continue;
                }
                crate::seal::OnRefused::Unsupported => {
                    state
                        .0
                        .lock()
                        .map_err(|_| "session cache poisoned")?
                        .remove(&pair);
                    return Ok(refused(
                        crate::seal::Refusal::Unsupported(
                            "its gate cannot open a sealed credential".into(),
                        )
                        .detail(),
                    ));
                }
                _ => {}
            }
        }
        return Ok(json!({ "httpStatus": status, "body": body, "sealed": sealed }));
    }
}

/// Mint a nonce, ask the device at `base` to sign it, and judge the answer.
/// Returns the verdict and the fingerprint of whatever key answered (empty
/// when none did). `Err` only when the challenge couldn't be built at all;
/// a device that doesn't answer is `Proof::Unavailable`, which the gate
/// treats as closed and the badge shows as absence of proof.
async fn challenge(
    base: &str,
    device_id: &str,
    expected_fp: &str,
) -> Result<(crate::whoami::Proof, String), String> {
    // 32 hex chars of OS randomness: inside the firmware's 16-64 gate, and
    // far past any birthday concern for a per-session challenge.
    let mut raw = [0u8; 16];
    getrandom::fill(&mut raw).map_err(|e| format!("no system randomness: {e}"))?;
    let nonce: String = raw.iter().map(|b| format!("{b:02x}")).collect();
    debug_assert!(crate::whoami::nonce_ok(&nonce));

    // The route and field names below are the FIRMWARE's, verified against
    // canary_wap.ino's httpd_register_uri_handler and render_enroll_json --
    // not guessed from the endpoint's informal name. Getting either wrong
    // fails silently as "unavailable", which is indistinguishable from old
    // firmware, so nothing would ever look broken.
    let url = format!(
        "{}/api/device/enroll?nonce={}",
        base.trim_end_matches('/'),
        nonce
    );
    let client = pinned_client(base, "SecuraCV-Flasher", std::time::Duration::from_secs(6)).await?;
    let unavailable =
        |detail: String| Ok((crate::whoami::Proof::Unavailable(detail), String::new()));
    let resp = match client.get(&url).send().await {
        Ok(r) => r,
        Err(e) => return unavailable(format!("couldn't reach the device: {e}")),
    };
    let status = resp.status().as_u16();
    if status == 404 {
        // Older firmware without the endpoint. Absence of proof, and it must
        // read as exactly that rather than as a failed check.
        return unavailable("this firmware doesn't offer the challenge endpoint yet".into());
    }
    if status != 200 {
        return unavailable(format!("the device answered HTTP {status}"));
    }
    let body: Value = match resp.json().await {
        Ok(b) => b,
        Err(e) => return unavailable(format!("the device's answer wasn't JSON: {e}")),
    };
    let field = |k: &str| {
        body.get(k)
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string()
    };
    // Bind to the device_id WE expected, not the one the answer claims —
    // otherwise a responder could sign for whatever identity it liked and
    // the canonical would happily agree with it.
    let verdict = crate::whoami::check_answer(
        device_id,
        &nonce,
        &field("pubkey_hex"),
        &field("sig_hex"),
        expected_fp,
    );
    let seen_fp = match &verdict {
        crate::whoami::Proof::WrongKey { seen_fp, .. } => seen_fp.clone(),
        // The device's own answer is untrusted bytes: decode_hex refuses anything
        // that is not pure hex before slicing, so a multi-byte character at an even
        // offset cannot turn a byte-index slice into a panic (and an aborted app).
        _ => crate::whoami::decode_hex(&field("pubkey_hex"))
            .filter(|key| key.len() == 32)
            .map(|key| crate::whoami::pubkey_fingerprint(&key))
            .unwrap_or_default(),
    };
    Ok((verdict, seen_fp))
}

/// Ask a device to prove it holds its identity key, by signing a nonce we
/// mint right now. Reuses the same private-host guard (and pinned client) as
/// every other device call — this must never reach off the LAN.
///
/// Returns `{ proof, detail, seenFp }`; `proof` is one of `answered`,
/// `wrong-key`, `bad-signature`, `unavailable`.
///
/// The caller SHOWS this as the row's identity badge and, on first sight,
/// records `seenFp` as the board's fingerprint. The badge itself gates
/// nothing: `fleet_device_call` seals the token to the recorded fingerprint
/// through its own handshake, rather than trusting a badge that may be
/// minutes old.
#[tauri::command]
pub async fn device_whoami(
    base: String,
    device_id: String,
    expected_fp: String,
) -> Result<Value, String> {
    if !base_ok(&base) {
        return Err("device address must be a local/private host".into());
    }
    let (verdict, seen_fp) = challenge(&base, &device_id, &expected_fp).await?;
    let detail = match &verdict {
        crate::whoami::Proof::Answered => {
            "answered a fresh challenge with its identity key".to_string()
        }
        crate::whoami::Proof::WrongKey {
            seen_fp,
            expected_fp,
        } => format!(
            "a DIFFERENT key answered for this device id (saw {seen_fp}, expected {expected_fp})"
        ),
        crate::whoami::Proof::BadSignature => {
            "the answer did not verify against the key it presented".to_string()
        }
        crate::whoami::Proof::Unavailable(why) => why.clone(),
    };
    Ok(json!({ "proof": verdict.as_str(), "detail": detail, "seenFp": seen_fp }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_hosts_pass() {
        for h in [
            "canary.local",
            "homeassistant.local",
            "hub.lan",
            "pi.home.arpa",
            "canary-3f2a",
            "192.168.1.40",
            "10.0.0.5",
            "172.16.9.9",
            "127.0.0.1",
            "169.254.10.10",
            "100.64.0.1",
            "100.127.255.254",
            "::1",
            "fe80::1",
            "fd00::abcd",
            "::ffff:192.168.1.40",
            "[fe80::1]",
        ] {
            assert!(host_is_local(h), "{h} should be local");
        }
    }

    #[test]
    fn public_hosts_are_refused() {
        for h in [
            "example.com",
            "github.com",
            "evil.local.example.com",
            "8.8.8.8",
            "172.32.0.1",
            "100.63.255.255",
            "100.128.0.0",
            "2001:4860:4860::8888",
            "::ffff:8.8.8.8",
            "64:ff9b::808:808",
            "",
            ".local",
        ] {
            assert!(!host_is_local(h), "{h} must be refused");
        }
    }

    fn sa(ip: &str) -> SocketAddr {
        SocketAddr::new(ip.parse().unwrap(), 80)
    }

    #[test]
    fn a_resolved_name_is_local_only_when_every_answer_is() {
        // The name gate passes "hub.lan"; the resolver decides the rest.
        let ok = addrs_all_local("hub.lan", vec![sa("192.168.1.9"), sa("fe80::1")]).unwrap();
        assert_eq!(ok.len(), 2);
        // One public answer among local ones is a connection off the LAN.
        let err =
            addrs_all_local("hub.lan", vec![sa("192.168.1.9"), sa("93.184.216.34")]).unwrap_err();
        assert!(err.contains("93.184.216.34"), "{err}");
        // A v4-mapped public answer is the same thing in a longer coat.
        assert!(addrs_all_local("hub.lan", vec![sa("::ffff:93.184.216.34")]).is_err());
        // Nothing resolved: nothing to pin, so nothing to dial.
        assert!(addrs_all_local("hub.lan", vec![]).is_err());
    }

    #[test]
    fn base_urls_are_gated() {
        assert!(base_ok("http://192.168.1.40"));
        assert!(base_ok("http://canary-3f2a.local:80"));
        assert!(base_ok("https://canary.local"));
        assert!(base_ok("http://10.0.0.7:8099/"));
        assert!(base_ok("http://[fe80::1]:80/"));
        assert!(!base_ok("http://example.com"));
        assert!(!base_ok("ftp://192.168.1.40"));
        assert!(!base_ok("192.168.1.40"));
        assert!(!base_ok("http://"));
        assert_eq!(base_host("http://canary.local:80/x"), Some("canary.local"));
        assert_eq!(base_host("http://[fe80::1]:80"), Some("[fe80::1]"));
    }

    #[test]
    fn the_verb_set_is_closed() {
        for a in [
            "ota-status",
            "ota-check",
            "ota-install",
            "status",
            "identify",
            "fleet",
        ] {
            assert!(action_route(a).is_some());
        }
        assert!(action_route("../../etc").is_none());
        assert!(
            action_route("config").is_none(),
            "ota-config is deliberately not exposed"
        );
    }
}
