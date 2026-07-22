// SecuraCV Lab — native shell around the local-first `canary-local` Lab.
//
// v1 wraps the existing web Lab so it ships as a Mac/Linux app that runs
// entirely on your machine. The commands below are the seam where native
// capabilities plug in next — the biggest win being reliable USB flashing
// (replacing the browser's flaky WebSerial). See ../README.md.

#[tauri::command]
fn app_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
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
        "discovery": false,  // Phase 2: mDNS + BLE
        "notifications": false
    })
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![app_version, native_capabilities])
        .run(tauri::generate_context!())
        .expect("error while running the SecuraCV Lab");
}
