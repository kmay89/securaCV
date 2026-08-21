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
use std::net::IpAddr;

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
                let mut addrs: Vec<&IpAddr> = info.get_addresses().iter().collect();
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

/// Only ever talk to a host that can be on this network: `.local`-style
/// names, single-label LAN hostnames, or private/loopback/link-local IP
/// literals. Mirrors the firmware's own local-transport policy
/// (docs/firmware_ota.md §Transport) — a fleet-book call must never be
/// steerable at an internet host.
fn host_is_local(host: &str) -> bool {
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() {
        return false;
    }
    if let Ok(ip) = host.parse::<IpAddr>() {
        return match ip {
            IpAddr::V4(v4) => v4.is_private() || v4.is_loopback() || v4.is_link_local(),
            IpAddr::V6(v6) => {
                v6.is_loopback()
                    // fe80::/10 link-local and fc00::/7 unique-local
                    || (v6.segments()[0] & 0xffc0) == 0xfe80
                    || (v6.segments()[0] & 0xfe00) == 0xfc00
            }
        };
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

/// `http://host[:port]` (or https) with a local host — anything else is
/// refused before a socket opens. `pub(crate)` because `witness_discover`
/// (lib.rs) enforces the same policy over its candidate bases.
pub(crate) fn base_ok(base: &str) -> bool {
    let rest = if let Some(r) = base.strip_prefix("http://") {
        r
    } else if let Some(r) = base.strip_prefix("https://") {
        r
    } else {
        return false;
    };
    let host_port = rest.split('/').next().unwrap_or_default();
    if host_port.is_empty() {
        return false;
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
    host_is_local(host)
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

/// Call one device-API action with the device's bearer credential. Returns
/// `{ httpStatus, body }` — 401 is an ANSWER (the stored token isn't this
/// device's), not a transport error, so the UI can say the right thing.
#[tauri::command]
pub async fn fleet_device_call(
    base: String,
    token: String,
    action: String,
) -> Result<Value, String> {
    let (method, path) =
        action_route(&action).ok_or_else(|| format!("unknown device action {action:?}"))?;
    if !base_ok(&base) {
        return Err("device address must be a local/private host".into());
    }
    let url = format!("{}{}", base.trim_end_matches('/'), path);
    // Installs make the device fetch + verify a whole image; its HTTP answer
    // can lag or the socket can drop at the reboot. Generous but bounded —
    // the UI keeps polling ota-status afterwards either way.
    let timeout = if action == "ota-install" { 20 } else { 6 };
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .timeout(std::time::Duration::from_secs(timeout))
        .build()
        .map_err(|e| e.to_string())?;
    let mut req = client.request(method, &url);
    if !token.is_empty() {
        req = req.bearer_auth(token);
    }
    let resp = req
        .send()
        .await
        .map_err(|e| format!("couldn't reach the device: {e}"))?;
    let status = resp.status().as_u16();
    let body: Value = resp.json().await.unwrap_or(Value::Null);
    Ok(json!({ "httpStatus": status, "body": body }))
}

/// Ask a device to prove it holds its identity key, by signing a nonce we
/// mint right now. Reuses the same private-host guard as every other device
/// call — this must never reach off the LAN.
///
/// Returns `{ proof, detail, seenFp }`; `proof` is one of `answered`,
/// `wrong-key`, `bad-signature`, `unavailable`.
///
/// The caller SHOWS this and gates nothing on it. See whoami.rs for why: the
/// proof is relay-able, so treating it as authorization would authenticate
/// the key while saying nothing about the socket that receives a token.
#[tauri::command]
pub async fn device_whoami(
    base: String,
    device_id: String,
    expected_fp: String,
) -> Result<Value, String> {
    if !base_ok(&base) {
        return Err("device address must be a local/private host".into());
    }
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
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .timeout(std::time::Duration::from_secs(6))
        .build()
        .map_err(|e| e.to_string())?;
    let resp = match client.get(&url).send().await {
        Ok(r) => r,
        Err(e) => {
            return Ok(json!({
                "proof": "unavailable",
                "detail": format!("couldn't reach the device: {e}"),
            }))
        }
    };
    let status = resp.status().as_u16();
    if status == 404 {
        // Older firmware without the endpoint. Absence of proof, and it must
        // read as exactly that rather than as a failed check.
        return Ok(json!({
            "proof": "unavailable",
            "detail": "this firmware doesn't offer the challenge endpoint yet",
        }));
    }
    if status != 200 {
        return Ok(json!({
            "proof": "unavailable",
            "detail": format!("the device answered HTTP {status}"),
        }));
    }
    let body: Value = match resp.json().await {
        Ok(b) => b,
        Err(e) => {
            return Ok(json!({
                "proof": "unavailable",
                "detail": format!("the device's answer wasn't JSON: {e}"),
            }))
        }
    };
    let field = |k: &str| body.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
    // Bind to the device_id WE expected, not the one the answer claims —
    // otherwise a responder could sign for whatever identity it liked and
    // the canonical would happily agree with it.
    let verdict = crate::whoami::check_answer(
        &device_id,
        &nonce,
        &field("pubkey_hex"),
        &field("sig_hex"),
        &expected_fp,
    );
    let seen_fp = match &verdict {
        crate::whoami::Proof::WrongKey { seen_fp, .. } => seen_fp.clone(),
        _ => {
            let pk = field("pubkey_hex");
            if pk.len() == 64 {
                crate::whoami::pubkey_fingerprint(
                    &(0..64)
                        .step_by(2)
                        .filter_map(|i| u8::from_str_radix(&pk[i..i + 2], 16).ok())
                        .collect::<Vec<u8>>(),
                )
            } else {
                String::new()
            }
        }
    };
    let detail = match &verdict {
        crate::whoami::Proof::Answered => {
            "answered a fresh challenge with its identity key".to_string()
        }
        crate::whoami::Proof::WrongKey { seen_fp, expected_fp } => format!(
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
            "::1",
            "fe80::1",
            "fd00::abcd",
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
            "2001:4860:4860::8888",
            "",
            ".local",
        ] {
            assert!(!host_is_local(h), "{h} must be refused");
        }
    }

    #[test]
    fn base_urls_are_gated() {
        assert!(base_ok("http://192.168.1.40"));
        assert!(base_ok("http://canary-3f2a.local:80"));
        assert!(base_ok("https://canary.local"));
        assert!(base_ok("http://10.0.0.7:8099/"));
        assert!(!base_ok("http://example.com"));
        assert!(!base_ok("ftp://192.168.1.40"));
        assert!(!base_ok("192.168.1.40"));
        assert!(!base_ok("http://"));
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
