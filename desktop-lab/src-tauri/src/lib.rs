// SecuraCV Lab — native shell around the local-first `canary-local` Lab.
//
// v1 wraps the existing web Lab so it ships as a Mac/Linux app that runs
// entirely on your machine. The commands below are the seam where native
// capabilities plug in next — the biggest win being reliable USB flashing
// (replacing the browser's flaky WebSerial). See ../README.md.
//
// Self-update (desktop only; iOS/iPadOS updates ride the App Store): the app
// checks its release channel at launch and on a six-hour routine while it
// stays open — see src/self_update.rs for the shape, copied from the Flasher.
// Local-first still means local-first: the only thing the Lab ever fetches on
// its own is its update manifest, from the project's releases.

#[cfg(desktop)]
mod self_update;

#[tauri::command]
fn app_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

/// What build is actually running — the same DTO the Flasher's About panel
/// reads (`desktop/src-tauri/src/lib.rs:app_info`), so the two apps answer
/// "which version am I on?" identically. Stamped at compile time by build.rs;
/// `build_rev` is "source" when built outside a git checkout.
#[derive(serde::Serialize)]
struct AppInfo {
    version: String,
    build_rev: String,
    build_epoch: u64,
}

#[tauri::command]
fn app_info() -> AppInfo {
    AppInfo {
        version: env!("CARGO_PKG_VERSION").to_string(),
        build_rev: env!("SECURACV_BUILD_REV").to_string(),
        build_epoch: env!("SECURACV_BUILD_EPOCH").parse::<u64>().unwrap_or(0),
    }
}

// --- roadmap seam (Phase 2): native device capabilities -----------------
// Reliable serial flashing and LAN discovery are why a native app earns its
// keep. When we add the `serialport` crate, this becomes the real thing:
//
//   #[tauri::command]
//   fn list_serial_ports() -> Vec<String> {
//       serialport::available_ports()
//           .map(|ports| ports.into_iter().map(|p| p.port_name).collect())
//           .unwrap_or_default()
//   }
//
// For now we expose a stub so the frontend can feature-detect the native
// shell and light up the "Flash over USB (native)" path when it's present.
#[tauri::command]
fn native_capabilities() -> serde_json::Value {
    serde_json::json!({
        "shell": "tauri",
        "serial": false,     // Phase 2: serialport
        // LAN fleet discovery is live: witness_discover polls /api/fleet on
        // the LAN (the DISCOVERY.md contract). mDNS browse + BLE stay future.
        "discovery": true,
        "notifications": false,
        // Signed self-update via the rolling lab-latest pointer (desktop
        // builds only — the App Store owns updates on iOS/iPadOS).
        "self_update": cfg!(desktop)
    })
}

/// Only ever talk to a host that can be on this network: `.local`-style
/// names, single-label LAN hostnames, or private/loopback/link-local IP
/// literals. Keep in lockstep with `desktop/src-tauri/src/fleet.rs`
/// (`host_is_local`) — one policy, two crates: a fleet call must never be
/// steerable at an internet host (docs/firmware_ota.md §Transport).
fn host_is_local(host: &str) -> bool {
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() {
        return false;
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        return match ip {
            std::net::IpAddr::V4(v4) => v4.is_private() || v4.is_loopback() || v4.is_link_local(),
            std::net::IpAddr::V6(v6) => {
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
/// refused before a socket opens. Lockstep twin of the Flasher's
/// `fleet::base_ok` (`desktop/src-tauri/src/fleet.rs`).
fn base_ok(base: &str) -> bool {
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

/// Find Canaries on the LAN and return the first kernel fleet that answers.
///
/// Byte-for-byte the same command as the Flasher's
/// (`desktop/src-tauri/src/lib.rs`) — the two apps must discover the SAME way
/// (`canary-local/tests/desktop_parity.test.js` pins that). Unlike the
/// browser Lab (which can't scan a LAN), the native shell can reach it
/// directly; `.local` hostnames resolve through the OS resolver (Bonjour /
/// avahi), so no mDNS crate is needed. ONE pass over the candidate bases,
/// first `/api/fleet` that answers wins; the frontend polls while the Witness
/// Wall bench is open. Coarse presence/health only — see
/// `tvos/discovery/DISCOVERY.md`.
#[tauri::command]
async fn witness_discover(bases: Vec<String>) -> Result<serde_json::Value, String> {
    // Same transport policy as the Flasher's fleet book (fleet.rs base_ok): a
    // discovery probe must never be steerable at an internet host. Candidates
    // that fail the local-host gate are skipped, not fatal — the list is
    // best-effort by design and the `.local` defaults always qualify.
    let bases: Vec<&String> = bases.iter().filter(|b| base_ok(b)).collect();
    if bases.is_empty() {
        return Err(
            "no local device address to try — device addresses must be local/private hosts"
                .to_string(),
        );
    }
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Lab")
        .timeout(std::time::Duration::from_secs(2))
        // A LAN probe must never be routed through an OS-configured proxy.
        // reqwest defaults `auto_sys_proxy` on, and hyper-util's
        // client-proxy-system feature — which tauri-plugin-updater can switch
        // on from the other side of the graph, since Cargo unifies features on
        // the one shared hyper-util — makes that read the system proxy on
        // macOS/Windows. macOS applies no bypass list there, so `.local` and
        // 192.168.x.x addresses would leave the machine. base_ok gates the
        // URL; only .no_proxy() gates the connection.
        .no_proxy()
        .build()
        .map_err(|e| e.to_string())?;
    for base in &bases {
        let url = format!("{}/api/fleet", base.trim_end_matches('/'));
        if let Ok(resp) = client.get(&url).send().await {
            if resp.status().is_success() {
                if let Ok(v) = resp.json::<serde_json::Value>().await {
                    return Ok(v);
                }
            }
        }
    }
    Err("no kernel answered on the LAN yet".to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let builder = tauri::Builder::default().plugin(tauri_plugin_opener::init());

    #[cfg(desktop)]
    let builder = builder
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_process::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .manage(std::sync::Mutex::new(self_update::UpdateGate::default()))
        .invoke_handler(tauri::generate_handler![
            app_version,
            app_info,
            native_capabilities,
            witness_discover,
            self_update::check_update,
            self_update::install_update,
            self_update::read_update_journal,
            self_update::update_journal_path
        ]);
    #[cfg(not(desktop))]
    let builder = builder.invoke_handler(tauri::generate_handler![
        app_version,
        app_info,
        native_capabilities,
        witness_discover
    ]);

    // If a self-update is REPLACING the app bundle when the user closes the
    // window, hold the close: nothing in the bundle is guaranteed complete
    // until the install returns, and quitting mid-write is the one thing that
    // can leave the Lab unable to open at all (the Flasher's guard, ported —
    // desktop/src-tauri/src/lib.rs). Not negotiable, so no "quit anyway";
    // the install takes seconds and the app relaunches itself.
    #[cfg(desktop)]
    let builder = builder.on_window_event(|window, event| {
        use tauri::Manager as _;
        use tauri_plugin_dialog::{DialogExt as _, MessageDialogButtons};
        if let tauri::WindowEvent::CloseRequested { api, .. } = event {
            if self_update::install_in_progress(window.app_handle()) {
                api.prevent_close();
                window
                    .dialog()
                    .message(
                        "An update is being written over the app right now. \
                         Quitting mid-write is the one thing that can leave the \
                         Lab unable to open at all, so this has to finish — \
                         it takes a few seconds, then the app relaunches itself.",
                    )
                    .title("Finishing the update")
                    .buttons(MessageDialogButtons::OkCustom("OK".into()))
                    .show(|_| {});
            }
        }
    });

    builder
        .setup(|_app| {
            // The freshness routine: first check shortly after launch, then
            // every six hours for as long as the window stays open.
            #[cfg(desktop)]
            {
                let handle = _app.handle().clone();
                tauri::async_runtime::spawn(async move {
                    tokio::time::sleep(self_update::FIRST_CHECK_DELAY).await;
                    loop {
                        self_update::routine_check(handle.clone()).await;
                        tokio::time::sleep(self_update::RECHECK_EVERY).await;
                    }
                });
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("error while building the SecuraCV Lab")
        .run(|_app, _event| {
            // Cmd-Q / the app menu's Quit never pass through CloseRequested —
            // they request an application exit directly, and this is the only
            // place that can stop them (the Flasher's guard, ported).
            #[cfg(desktop)]
            if let tauri::RunEvent::ExitRequested { api, .. } = &_event {
                if self_update::install_in_progress(_app) {
                    use tauri_plugin_dialog::{DialogExt as _, MessageDialogButtons};
                    api.prevent_exit();
                    _app.dialog()
                        .message(
                            "An update is being written over the app right now. \
                             Quitting mid-write is the one thing that can leave the \
                             Lab unable to open at all, so this has to finish — \
                             it takes a few seconds, then the app relaunches itself.",
                        )
                        .title("Finishing the update")
                        .buttons(MessageDialogButtons::OkCustom("OK".into()))
                        .show(|_| {});
                }
            }
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirrors the Flasher's fleet.rs tests — the lockstep twin must refuse
    // and accept the same hosts, or the two apps' discovery policies drift.
    #[test]
    fn local_hosts_pass_and_public_hosts_are_refused() {
        for h in [
            "canary.local", "homeassistant.local", "hub.lan", "pi.home.arpa",
            "canary-3f2a", "192.168.1.40", "10.0.0.5", "172.16.9.9",
            "127.0.0.1", "169.254.10.10", "::1", "fe80::1", "fd00::abcd",
        ] {
            assert!(host_is_local(h), "{h} should be local");
        }
        for h in [
            "example.com", "github.com", "evil.local.example.com",
            "8.8.8.8", "172.32.0.1", "2001:4860:4860::8888", "", ".local",
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
}
