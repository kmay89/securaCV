//! LAN discovery by mDNS: a real browse of `_securacv._tcp`, the service
//! every networked Canary board advertises with a canonical TXT schema
//! (device_id, name, fw, model, dt, role; see
//! docs/onboarding_unified_wizard.md). Browsing reaches ALL variants,
//! including vision/sense which run no HTTP server at all.
//!
//! A LOCKSTEP TWIN of the Flasher's browse (`desktop/src-tauri/src/fleet.rs`):
//! the same command name, the same `FleetSighting` DTO and the same service
//! constant, with `fleet_scan`/`scan_blocking` copied verbatim (their comments
//! included) — canary-local/tests/desktop_parity.test.js holds the two files
//! equal, so a frontend written against either app reads the other's answer
//! unchanged. Only the browse is ported: the Flasher's device calls
//! (`fleet_device_call`, `device_whoami`) carry bearer tokens from its secret
//! drawer, and the Lab has no secret drawer.
//!
//! The browse finds BOARDS. The kernel / Home Assistant add-on advertises no
//! `_securacv._tcp`, so `witness_discover` (lib.rs) keeps polling a typed
//! kernel base beside it — the two coexist. Desktop only: an iOS browse needs
//! the multicast entitlement and `NSBonjourServices`, and the mobile shell is
//! scaffold-only (desktop-lab/MOBILE.md).

use serde::Serialize;
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

#[cfg(test)]
mod tests {
    use super::*;

    /// No Canary answers in CI or a sandbox, and some hosts refuse the
    /// multicast socket outright: either way the browse must come back as a
    /// plain `Result` (an empty list or a typed error), never a panic.
    #[test]
    fn a_browse_with_nobody_answering_fails_closed() {
        match scan_blocking(500) {
            Ok(found) => assert!(found.iter().all(|s| !s.device_id.is_empty())),
            Err(e) => assert!(!e.is_empty()),
        }
    }

    #[test]
    fn the_service_is_the_one_every_board_advertises() {
        assert_eq!(SERVICE_TYPE, "_securacv._tcp.local.");
    }
}
