// SecuraCV Lab — native shell around the local-first `canary-local` Lab.
//
// It wraps the existing web Lab so it ships as a Mac/Linux app that runs
// entirely on your machine. The commands below are the seam where native
// capabilities plug in — USB flashing through the Flasher's bundled espflash
// engine (src/flash.rs, desktop/flash-engine) replaces the WebSerial the OS
// webview doesn't have. See ../README.md.
//
// Self-update (desktop only; iOS/iPadOS updates ride the App Store): the app
// checks its release channel at launch and on a six-hour routine while it
// stays open — see src/self_update.rs for the shape, copied from the Flasher.
// Local-first still means local-first: the Lab reaches the internet only for
// the project's GitHub releases — its update manifest, and, on the Flash
// page, which signed firmware is published (read once a board is connected)
// and the image the user presses Flash for (src/flash.rs, the engine's
// net.rs).

#[cfg(desktop)]
mod companion;
#[cfg(desktop)]
mod flash;
#[cfg(desktop)]
mod fleet;
#[cfg(desktop)]
mod self_update;

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

// --- native device capabilities -----------------------------------------
// Reliable serial flashing and LAN discovery are why a native app earns its
// keep. Native FLASHING is live on macOS and Linux: the flash commands
// (src/flash.rs — the Flasher's commands over the shared
// desktop/flash-engine) run the espflash sidecar that the release bundles for
// exactly those two platforms (tauri.{macos,linux}.conf.json externalBin,
// desktop-release.yml "Bundle espflash sidecar"), and the Flash page mounts
// its native bench (canary-local/assets/flash-native.js) when `serial` says
// so. Anywhere else — the iPad shell, a Windows build nobody ships — `serial`
// is false, so the page never lights a path that can only fail. On macOS and
// Linux it is ALSO a runtime answer (espflash_bundled): the platform bundles
// espflash, and the file is really there next to this binary.
// `serial_list` advertises the port list (flash::list_ports) on every desktop
// build. LAN discovery is two live
// commands on desktop: an mDNS browse that finds the boards (fleet_scan,
// src/fleet.rs) and the /api/fleet poll that finds a kernel
// (witness_discover). Bluetooth LE discovery is still future.
#[tauri::command]
fn native_capabilities(app: tauri::AppHandle) -> serde_json::Value {
    serde_json::json!({
        "shell": "tauri",
        // Native FLASHING (src/flash.rs): only where the release bundles the
        // espflash sidecar, AND only while that sidecar is really there to
        // run. desktop_parity.test.js refuses this unless the sidecar, its
        // bundling step and the frontend path all exist.
        "serial": cfg!(any(target_os = "macos", target_os = "linux")) && espflash_bundled(&app),
        // Native port enumeration (flash::list_ports). Desktop only:
        // MOBILE.md's contract is that generic USB serial does not exist on
        // iOS/iPadOS, so a mobile build neither registers the command nor
        // advertises it — a capability must never light a path that can
        // only fail.
        "serial_list": cfg!(desktop),
        // LAN fleet discovery is live: witness_discover polls /api/fleet on
        // the LAN (the DISCOVERY.md contract), on every build.
        "discovery": true,
        // The mDNS browse of `_securacv._tcp` (fleet_scan, the Flasher's
        // twin). Desktop only — iOS needs the multicast entitlement and
        // NSBonjourServices first (MOBILE.md), so a mobile build neither
        // registers the command nor advertises it. BLE is still future.
        "mdns": cfg!(desktop),
        // The menubar fleet companion (src/companion.rs): a tray icon and
        // native notifications on fleet changes, posted from Rust — coarse
        // presence words only, never sealed-log content, never "verified".
        // Desktop only; the webview holds no notification grant.
        "notifications": cfg!(desktop),
        // Signed self-update via the rolling lab-latest pointer (desktop
        // builds only — the App Store owns updates on iOS/iPadOS).
        "self_update": cfg!(desktop)
    })
}

/// The runtime half of `serial`: is the bundled espflash really next to this
/// binary (src/flash.rs `espflash_bundled` — a non-empty executable file,
/// resolved where the spawn will look)? Compile-time `cfg!` alone says only
/// that the RELEASE bundles one; a dev build on the empty compile-only stub,
/// a repackaged binary or a deleted file would still advertise a flash path
/// whose every board read fails at spawn.
#[cfg(any(target_os = "macos", target_os = "linux"))]
fn espflash_bundled(app: &tauri::AppHandle) -> bool {
    flash::espflash_bundled(app)
}

/// No espflash is bundled here (the iPad shell, an unshipped Windows build).
#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn espflash_bundled(_app: &tauri::AppHandle) -> bool {
    false
}

/// Is this one address on a network this computer can be on? RFC 1918,
/// 100.64/10 (carrier-grade NAT), loopback and link-local; for IPv6 `::1`,
/// fe80::/10, fc00::/7 — and the IPv4-mapped form of any IPv4 range, judged
/// as the IPv4 it carries. Lockstep twin of the Flasher's
/// `fleet::ip_is_local` (`desktop/src-tauri/src/fleet.rs`).
fn ip_is_local(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(v4) => v4_is_local(v4),
        std::net::IpAddr::V6(v6) => {
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
/// link-local IP literals. Keep in lockstep with `desktop/src-tauri/src/fleet.rs`
/// (`host_is_local`) — one policy, two crates: a fleet call must never be
/// steerable at an internet host (docs/firmware_ota.md §Transport). A name
/// is only half the answer (`.lan`, `.internal`, a single label can resolve
/// through unicast DNS to anything): `resolve_local` resolves it, judges
/// every address, and the client pins to those addresses.
fn host_is_local(host: &str) -> bool {
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() {
        return false;
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
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

/// Every address a name resolved to must be local, or the name is refused
/// (one public answer among local ones is still a connection off the LAN);
/// an empty answer is refused too. Twin of the Flasher's `addrs_all_local`.
fn addrs_all_local(
    host: &str,
    addrs: Vec<std::net::SocketAddr>,
) -> Result<Vec<std::net::SocketAddr>, String> {
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

/// Resolve a host (bare name or IP literal, brackets allowed) into the
/// addresses a connection may be pinned to: the name gate, then the OS
/// resolver (Bonjour / avahi answer `.local`), then every answer judged.
async fn resolve_local(host: &str) -> Result<Vec<std::net::SocketAddr>, String> {
    let bare = host.trim_matches(['[', ']']).to_string();
    if !host_is_local(&bare) {
        return Err(format!("{host} is not a local/private host"));
    }
    if let Ok(ip) = bare.parse::<std::net::IpAddr>() {
        return Ok(vec![std::net::SocketAddr::new(ip, 0)]);
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

/// The host a connection to `base` would dial, as the url crate reads it
/// (`reqwest::Url`, the parse reqwest dials with): lowercased,
/// percent-decoded, punycoded, an IPv4 in any spelling written dotted, an
/// IPv6 literal kept in its brackets. `None` unless `base` is an `http` or
/// `https` URL with a host, no user or password, no query, no fragment, no
/// space or control character (the calls append to `base` as written, so a
/// base the parser had to clean is refused), and a name host DNS-shaped
/// (`dns_shaped`). Lockstep twin of the Flasher's `fleet::base_host`
/// (`desktop/src-tauri/src/fleet.rs`), whose doc says why a hand split on
/// `/` was not the judge: it read `http://8.8.8.8?.local:8080` as a `.local`
/// name while the connection dialed 8.8.8.8:80.
fn base_host(base: &str) -> Option<String> {
    if base.chars().any(|c| c == ' ' || c.is_ascii_control()) {
        return None;
    }
    let url = reqwest::Url::parse(base).ok()?;
    if !matches!(url.scheme(), "http" | "https")
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
        || url.domain().is_some_and(|name| !dns_shaped(name))
    {
        return None;
    }
    url.host_str().map(str::to_string)
}

/// A name a request can carry on either scheme: dot-separated labels of 1
/// to 63 ASCII letters, digits, `-` or `_`, none starting or ending with
/// `-`, 253 bytes at most (rustls's server-name rule). The url crate accepts
/// more in a host, which hyper's `http::Uri` or rustls then refuses, so such
/// a base would pass the gate and build no request. Lockstep twin of the
/// Flasher's `fleet::dns_shaped` (`desktop/src-tauri/src/fleet.rs`).
fn dns_shaped(name: &str) -> bool {
    name.len() <= 253
        && name.split('.').all(|label| {
            (1..=63).contains(&label.len())
                && !label.starts_with('-')
                && !label.ends_with('-')
                && label
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        })
}

/// `http://host[:port]` (or https) with a local host — anything else is
/// refused before a socket opens. The host is `base_host`'s, the one the
/// connection dials. Lockstep twin of the Flasher's `fleet::base_ok`
/// (`desktop/src-tauri/src/fleet.rs`).
fn base_ok(base: &str) -> bool {
    base_host(base).is_some_and(|h| host_is_local(&h))
}

/// The redirect policy every pinned client carries: a redirect is followed
/// only to the host the base was judged by, within reqwest's default limit
/// of 10 hops; any other target stops the chain and the 3xx is the answer
/// (reqwest's default follows any Location, off the judged and pinned host).
/// Lockstep twin of the Flasher's `fleet::same_host_redirects`
/// (`desktop/src-tauri/src/fleet.rs`), whose doc says why the scheme and
/// port may change.
fn same_host_redirects(judged: &str) -> reqwest::redirect::Policy {
    let judged = judged.to_string();
    reqwest::redirect::Policy::custom(move |attempt| {
        if attempt.url().host_str() == Some(judged.as_str()) {
            reqwest::redirect::Policy::default().redirect(attempt)
        } else {
            attempt.stop()
        }
    })
}

/// A client for ONE local base: proxy-free, timeout-bounded, pinned to the
/// addresses `resolve_local` just vetted, so hyper never resolves the name a
/// second time, and following a redirect only back to that host
/// (`same_host_redirects`). Twin of the Flasher's `fleet::pinned_client`.
///
/// `.no_proxy()`: a LAN probe must never be routed through an OS-configured
/// proxy. reqwest defaults `auto_sys_proxy` on, and hyper-util's
/// client-proxy-system feature — which tauri-plugin-updater can switch on
/// from the other side of the graph, since Cargo unifies features on the one
/// shared hyper-util — makes that read the system proxy on macOS/Windows.
/// macOS applies no bypass list there, so `.local` and 192.168.x.x addresses
/// would leave the machine. The URL gate stops nothing on its own; only the
/// connection's own settings do.
async fn pinned_client(
    base: &str,
    timeout: std::time::Duration,
) -> Result<reqwest::Client, String> {
    let host =
        base_host(base).ok_or_else(|| "device address must be a local/private host".to_string())?;
    let addrs = resolve_local(&host).await?;
    let bare = host.trim_matches(['[', ']']);
    let mut builder = reqwest::Client::builder()
        .user_agent("SecuraCV-Lab")
        .timeout(timeout)
        .no_proxy()
        .redirect(same_host_redirects(&host));
    if bare.parse::<std::net::IpAddr>().is_err() {
        // Keyed on the URL's host as `base_host` read it, by the parser
        // reqwest dials with, so the override is the one hyper's lookup finds.
        builder = builder.resolve_to_addrs(bare, &addrs);
    }
    builder.build().map_err(|e| e.to_string())
}

/// Find Canaries on the LAN and return the first kernel fleet that answers.
///
/// Byte-for-byte the same command as the Flasher's
/// (`desktop/src-tauri/src/lib.rs`) — the two apps must discover the SAME way
/// (`canary-local/tests/desktop_parity.test.js` pins that). Unlike the
/// browser Lab (which can't scan a LAN), the native shell can reach it
/// directly; `.local` hostnames resolve through the OS resolver (Bonjour /
/// avahi), so this command needs no mDNS of its own — on desktop the
/// frontend adds the boards `fleet_scan` browsed (src/fleet.rs) to the
/// candidates, and the typed kernel base stays first because the kernel
/// advertises no `_securacv._tcp`. ONE pass over the candidate bases,
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
    for base in &bases {
        // One pinned client per base: the name is resolved, every address
        // judged local, and the connection pinned to those addresses. A name
        // that resolves off the LAN is skipped like any other candidate that
        // failed the gate.
        let Ok(client) = pinned_client(base, std::time::Duration::from_secs(2)).await else {
            continue;
        };
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
        .plugin(tauri_plugin_notification::init())
        // The espflash sidecar is spawned from Rust (src/flash.rs); the shell
        // plugin's state must exist for that, and the webview gets no grant.
        .plugin(tauri_plugin_shell::init())
        .manage(std::sync::Mutex::new(self_update::UpdateGate::default()))
        .manage(companion::Companion::default())
        .manage(flash_engine::monitor::SerialMonitorState::default())
        .manage(flash::Sidecars::default())
        // Every command here has a caller in the Lab's own pages, and every
        // page's invoke() has a command here: canary-local/tests/
        // lab_bundle.test.js holds both directions, so the webview's IPC
        // surface is exactly what the Lab uses.
        .invoke_handler(tauri::generate_handler![
            app_info,
            native_capabilities,
            flash::list_ports,
            flash::detect_chip,
            flash::fetch_manifest,
            flash::flash,
            flash::start_serial_monitor,
            flash::serial_monitor_send,
            flash::stop_serial_monitor,
            witness_discover,
            fleet::fleet_scan,
            companion::companion_set_bases,
            self_update::check_update,
            self_update::install_update,
            self_update::read_update_journal,
            self_update::update_journal_path
        ]);
    #[cfg(not(desktop))]
    let builder = builder.invoke_handler(tauri::generate_handler![
        app_info,
        native_capabilities,
        witness_discover
    ]);

    // If a self-update is REPLACING the app bundle when the user closes the
    // window, hold the close: nothing in the bundle is guaranteed complete
    // until the install returns, and quitting mid-write is the one thing that
    // can leave the Lab unable to open at all (the Flasher's guard, ported —
    // desktop/src-tauri/src/lib.rs). Not negotiable, so no "quit anyway";
    // the install takes seconds and the app relaunches itself. That guard
    // runs FIRST; then, where the menu bar always shows the companion's
    // tray (companion::keeps_running), closing hides the window instead
    // of quitting — the fleet watch keeps running, the tray reopens it, and
    // a flash in flight is untouched. Only where the close really quits
    // does the flash guard ask (the Flasher's hub guard, ported): a quit
    // kills the running espflash mid-write. Advisory, with a Quit — a
    // half-written board is re-flashed, nothing is bricked.
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
            } else if companion::keeps_running(window.app_handle()) {
                api.prevent_close();
                let _ = window.hide();
                companion::told_hidden_once(window.app_handle());
            } else if flash::Sidecars::any_running(window.app_handle()) {
                api.prevent_close();
                let app = window.app_handle().clone();
                window
                    .dialog()
                    .message(FLASH_QUIT_QUESTION)
                    .title("Quit while flashing?")
                    .buttons(MessageDialogButtons::OkCancelCustom(
                        "Quit".into(),
                        "Keep flashing".into(),
                    ))
                    .show(move |quit| {
                        if quit {
                            quit_through_flash(&app, 0);
                        }
                    });
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
                // The menubar fleet companion: tray + a 30 s fleet watch.
                companion::start(_app.handle());
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("error while building the SecuraCV Lab")
        .run(|_app, _event| {
            // The window may be hidden behind the menu bar companion: a Dock
            // click brings it back.
            #[cfg(target_os = "macos")]
            if let tauri::RunEvent::Reopen {
                has_visible_windows: false,
                ..
            } = &_event
            {
                companion::show_main(_app);
            }
            // A quitting Lab takes its running espflash with it, so no sidecar
            // is left holding a board's serial port (src/flash.rs: Sidecars).
            #[cfg(desktop)]
            if let tauri::RunEvent::Exit = &_event {
                flash::Sidecars::kill_all(_app);
            }
            // Cmd-Q / the app menu's Quit never pass through CloseRequested —
            // they request an application exit directly, and this is the only
            // place that can stop them (the Flasher's guard, ported). The
            // tray's Quit and the updater's restart come through here too
            // (both are request_exit), so one flash guard covers them all.
            #[cfg(desktop)]
            if let tauri::RunEvent::ExitRequested { code, api, .. } = &_event {
                use tauri_plugin_dialog::{DialogExt as _, MessageDialogButtons};
                if self_update::install_in_progress(_app) {
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
                } else if flash::Sidecars::any_running(_app) {
                    api.prevent_exit();
                    let app = _app.clone();
                    // The exit code this request carried is kept.
                    let code = code.unwrap_or(0);
                    _app.dialog()
                        .message(FLASH_QUIT_QUESTION)
                        .title("Quit while flashing?")
                        .buttons(MessageDialogButtons::OkCancelCustom(
                            "Quit".into(),
                            "Keep flashing".into(),
                        ))
                        .show(move |quit| {
                            if quit {
                                quit_through_flash(&app, code);
                            }
                        });
                }
            }
        })
}

/// What the quit guards ask while an espflash is running. Advisory on
/// purpose: a board left half-written is flashed again, nothing is bricked
/// (the ESP32's first-stage bootloader is in ROM).
#[cfg(desktop)]
const FLASH_QUIT_QUESTION: &str = "A flash is still writing to the board. Quitting now leaves it \
     unfinished — safe, you would flash it again. Quit anyway?";

/// The Quit the guards offer: end the running espflash first, THEN ask for
/// the exit again with the same code. The map is empty by then, so the exit
/// guard lets this second request through (an exit requested while the
/// map is still full would only be asked about again), and the Exit
/// handler's `kill_all` finds nothing left to do.
#[cfg(desktop)]
fn quit_through_flash(app: &tauri::AppHandle, code: i32) {
    flash::Sidecars::kill_all(app);
    app.exit(code);
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirrors the Flasher's fleet.rs tests — the lockstep twin must refuse
    // and accept the same hosts, or the two apps' discovery policies drift.
    #[test]
    fn local_hosts_pass_and_public_hosts_are_refused() {
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
            "::1",
            "fe80::1",
            "fd00::abcd",
            "::ffff:192.168.1.40",
        ] {
            assert!(host_is_local(h), "{h} should be local");
        }
        for h in [
            "example.com",
            "github.com",
            "evil.local.example.com",
            "8.8.8.8",
            "172.32.0.1",
            "2001:4860:4860::8888",
            "::ffff:8.8.8.8",
            "100.128.0.0",
            "",
            ".local",
        ] {
            assert!(!host_is_local(h), "{h} must be refused");
        }
    }

    #[test]
    fn a_resolved_name_is_local_only_when_every_answer_is() {
        let sa = |ip: &str| std::net::SocketAddr::new(ip.parse().unwrap(), 80);
        assert_eq!(
            addrs_all_local("hub.lan", vec![sa("192.168.1.9"), sa("fe80::1")])
                .unwrap()
                .len(),
            2
        );
        assert!(addrs_all_local("hub.lan", vec![sa("192.168.1.9"), sa("93.184.216.34")]).is_err());
        assert!(addrs_all_local("hub.lan", vec![sa("::ffff:93.184.216.34")]).is_err());
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
        assert_eq!(
            base_host("http://canary.local:80/x").as_deref(),
            Some("canary.local")
        );
        assert_eq!(
            base_host("http://[fe80::1]:80").as_deref(),
            Some("[fe80::1]")
        );
    }

    /// The Flasher's `fleet::tests::SPLIT_BASES`, row for row: bases whose
    /// text reads as a local host while the url crate, the parser reqwest
    /// dials with, reads another. Every one must be refused.
    const SPLIT_BASES: [&str; 15] = [
        "http://8.8.8.8?.local:8080",
        "http://example.com#.local",
        "http://example.com\\.local",
        "http://8.8.8.8#.local",
        "http://8.8.8.8\\.local",
        "http://1.2.3.4:25?.local:80",
        "http://8.8.8.8@offlan.local:8080",
        "http://user:pw@canary.local",
        "http://8%2E8%2E8%2E8:8080",
        "http://134744072",
        "http://0x08080808",
        "http://[fe80::1%25eth0]",
        "http://canary\t.local",
        "http://canary.local ",
        " http://canary.local",
    ];

    /// The Flasher's `fleet::tests::UNSENDABLE_BASES`, row for row: hosts the
    /// url crate accepts and hyper or rustls then refuses.
    const UNSENDABLE_BASES: [&str; 10] = [
        "http://canary\".local",
        "http://canary`.local",
        "http://canary{.local",
        "http://canary}.local",
        "https://canary*.local",
        "https://1.1.1.1;.local",
        "https://canary~1.local",
        "https://..local",
        "https://-canary.local",
        "https://canary-.local",
    ];

    #[test]
    fn a_base_is_judged_by_the_host_it_dials() {
        let passed: Vec<&str> = SPLIT_BASES.into_iter().filter(|b| base_ok(b)).collect();
        assert!(
            passed.is_empty(),
            "these bases passed the gate while reqwest dials a host it never judged: {passed:?}"
        );
        assert_eq!(base_host("http://134744072").as_deref(), Some("8.8.8.8"));
        assert_eq!(
            base_host("http://8%2E8%2E8%2E8.local:8080").as_deref(),
            Some("8.8.8.8.local"),
            "the pin must be keyed on the name hyper looks up, not the encoded text"
        );
        assert_eq!(
            base_host("http://C%41NARY.Local:80/").as_deref(),
            Some("canary.local")
        );
        assert_eq!(base_host("http://[FE80::1]").as_deref(), Some("[fe80::1]"));
        assert_eq!(
            base_host("http://3232235777").as_deref(),
            Some("192.168.1.1")
        );
    }

    #[test]
    fn a_passed_base_dials_the_host_it_was_judged_by() {
        let client = reqwest::Client::new();
        for base in [
            "http://192.168.1.40",
            "http://canary-3f2a.local:80",
            "https://canary.local",
            "http://10.0.0.7:8099/",
            "http://[fe80::1]:80/",
            "http://8%2E8%2E8%2E8.local:8080",
            "http://C%41NARY.Local:80/",
            "http://3232235777",
        ]
        .into_iter()
        .chain(SPLIT_BASES)
        .chain(UNSENDABLE_BASES)
        {
            if !base_ok(base) {
                continue;
            }
            let url = format!("{}/api/fleet", base.trim_end_matches('/'));
            let req = client
                .get(&url)
                .build()
                .expect("a passed base builds a request");
            assert_eq!(
                req.url().host_str(),
                base_host(base).as_deref(),
                "{base}: judged one host, dials another"
            );
            assert!(
                req.url().as_str().parse::<tauri::http::Uri>().is_ok(),
                "{base}: passed the gate, but hyper cannot carry its URL"
            );
        }
    }

    #[test]
    fn a_passed_base_is_one_a_request_can_carry() {
        let passed: Vec<&str> = UNSENDABLE_BASES
            .into_iter()
            .filter(|b| base_ok(b))
            .collect();
        assert!(
            passed.is_empty(),
            "these bases passed the gate, yet hyper or rustls refuses their host: {passed:?}"
        );
        // Inside the rule: a name the firmware announces, an underscore, a
        // single label, and an IDNA name judged as its punycode.
        for base in [
            "http://canary-sense-001-a1b2c3.local",
            "http://my_pc.local",
            "https://canary-3f2a",
            "http://café.local",
        ] {
            assert!(base_ok(base), "{base} must pass");
        }
        assert_eq!(
            base_host("http://café.local").as_deref(),
            Some("xn--caf-dma.local")
        );
    }

    /// A loopback HTTP stand-in for one address: every request gets
    /// `answer` and is counted. The Flasher's `fleet::tests::stub`.
    fn stub(answer: String) -> (u16, std::sync::Arc<std::sync::atomic::AtomicUsize>) {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let hits = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let counted = hits.clone();
        std::thread::spawn(move || {
            for mut conn in listener.incoming().flatten() {
                let mut got = Vec::new();
                let mut buf = [0u8; 1024];
                while !got.windows(4).any(|w| w == b"\r\n\r\n") {
                    match conn.read(&mut buf) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => got.extend_from_slice(&buf[..n]),
                    }
                }
                counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                let _ = conn.write_all(answer.as_bytes());
            }
        });
        (port, hits)
    }

    fn found(location: &str) -> String {
        format!(
            "HTTP/1.1 302 Found\r\nLocation: {location}\r\n\
             Content-Length: 0\r\nConnection: close\r\n\r\n"
        )
    }

    fn fleet_json(said: &str) -> String {
        let body = format!("{{\"fleet\":\"{said}\"}}");
        format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\
             Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
    }

    #[test]
    fn a_redirect_off_the_judged_host_is_not_followed() {
        // The Flasher's test of the same name: a judged, pinned base that
        // answers 302 to another host must not have that host dialed, and
        // discovery must not return its answer as the fleet.
        let (off, off_hits) = stub(fleet_json("from a host nobody judged"));
        let (vetted, vetted_hits) = stub(found(&format!("http://localhost:{off}/api/fleet")));
        let base = format!("http://127.0.0.1:{vetted}");
        tauri::async_runtime::block_on(async {
            let client = pinned_client(&base, std::time::Duration::from_secs(5))
                .await
                .unwrap();
            let resp = client
                .get(format!("{base}/api/fleet"))
                .send()
                .await
                .unwrap();
            assert_eq!(
                (resp.status().as_u16(), resp.url().host_str()),
                (302, Some("127.0.0.1")),
                "the pinned client followed a redirect off the host it judged"
            );
            assert_eq!(
                witness_discover(vec![base.clone()]).await.ok(),
                None,
                "witness_discover returned an unjudged host's answer as the fleet"
            );
        });
        let off_hits = off_hits.load(std::sync::atomic::Ordering::SeqCst);
        assert_eq!(off_hits, 0, "a host nobody judged was dialed");
        assert_eq!(vetted_hits.load(std::sync::atomic::Ordering::SeqCst), 2);
    }

    #[test]
    fn a_redirect_back_to_the_judged_host_is_still_followed() {
        let (next, next_hits) = stub(fleet_json("same host"));
        let (vetted, _) = stub(found(&format!("http://127.0.0.1:{next}/api/fleet")));
        let base = format!("http://127.0.0.1:{vetted}");
        tauri::async_runtime::block_on(async {
            let client = pinned_client(&base, std::time::Duration::from_secs(5))
                .await
                .unwrap();
            let resp = client
                .get(format!("{base}/api/fleet"))
                .send()
                .await
                .unwrap();
            assert_eq!(resp.status().as_u16(), 200);
            assert_eq!(resp.url().port(), Some(next));
        });
        assert_eq!(next_hits.load(std::sync::atomic::Ordering::SeqCst), 1);
    }
}
