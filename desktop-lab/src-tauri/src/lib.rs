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
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Lab")
        .timeout(std::time::Duration::from_secs(2))
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
        .run(tauri::generate_context!())
        .expect("error while running the SecuraCV Lab")
}
