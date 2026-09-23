//! Native USB flashing in the Lab — the Flasher's flash path, on the same
//! engine.
//!
//! Everything a flash decides and says lives in the tauri-free flash engine
//! (`desktop/flash-engine`), which the SecuraCV Flasher runs too: the catalog
//! chip guard, the closed set of manifest URLs, the release-origin and
//! size/SHA-256/Ed25519 checks, NVS provisioning, the first-contact erase, the
//! espflash invocation, the receipt, and the boot-receipt serial monitor. This
//! file is the Lab's thin Tauri layer over it, and it is deliberately the
//! Flasher's layer (`desktop/src-tauri/src/lib.rs`, `serial_monitor.rs`,
//! `host.rs`) over again: the SAME command names, the SAME argument names and
//! DTOs, the SAME event names (`flash:log`,
//! `flash:progress`, `flash:changemap`, `serial:log`, `serial:status`,
//! `serial:receipt`, `serial:vision`). `canary-local/tests/desktop_parity.test.js`
//! holds the two apps' wrappers equal by text.
//!
//! What the Lab does NOT port is the Flasher's launch guard
//! (`desktop/src-tauri/src/launch_guard.rs`), which writes each sidecar PID to
//! disk and reaps survivors on the next launch. The Lab keeps a smaller
//! promise: [`Sidecars`] holds every running espflash, and a normal quit
//! (the window, Cmd-Q, the tray's Quit) kills whatever is still running, so a
//! closed Lab never leaves espflash holding a board's serial port. A force
//! quit or a crash skips that exit path; the orphan then holds the port until
//! it finishes or the board is unplugged.
//!
//! Desktop only, like everything that reaches a USB port (MOBILE.md).

use flash_engine::catalog::Catalog;
use flash_engine::flash::{ChipInfo, FlashReceipt, FlashRequest};
use flash_engine::host::{Emit, FlashHost};
use flash_engine::monitor::SerialMonitorState;
use flash_engine::ports::PortDto;
use flash_engine::provisioning::Provisioning;
use flash_engine::sidecar::ESPFLASH;
use serde::Serialize;
use serde_json::Value;
use std::collections::HashMap;
use std::future::Future;
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_shell::process::{CommandChild, CommandEvent};
use tauri_plugin_shell::ShellExt;

// The flasher catalog, baked in at compile time exactly as the Flasher bakes
// it: build.rs copies the ONE canonical canary-local/devices/flash.json into
// OUT_DIR on every build. The webview reads the same file from the staged web
// root, but the chip guard and the manifest allow-list must not depend on
// anything the webview can hand over — they read this copy.
const EMBEDDED_CATALOG: &str = include_str!(concat!(env!("OUT_DIR"), "/flash.json"));

/// The embedded catalog and everything the flash engine derives from it,
/// computed once per launch.
fn bundled_catalog() -> &'static Catalog {
    static CATALOG: std::sync::OnceLock<Catalog> = std::sync::OnceLock::new();
    CATALOG.get_or_init(|| Catalog::new(EMBEDDED_CATALOG))
}

/// Every espflash the Lab is running right now, by PID. See the module docs
/// for what this does and does not cover.
#[derive(Default)]
pub struct Sidecars(Mutex<HashMap<u32, CommandChild>>);

impl Sidecars {
    /// Kill every espflash still running. Called once, on app exit.
    pub fn kill_all(app: &AppHandle) {
        let Some(state) = app.try_state::<Sidecars>() else {
            return;
        };
        let children: Vec<CommandChild> = match state.0.lock() {
            Ok(mut running) => running.drain().map(|(_, child)| child).collect(),
            Err(_) => return,
        };
        for child in children {
            let _ = child.kill();
        }
    }
}

/// Removes a sidecar from [`Sidecars`] when its run ends, however it ends.
struct Tracked {
    app: AppHandle,
    pid: u32,
}

impl Drop for Tracked {
    fn drop(&mut self) {
        if let Some(state) = self.app.try_state::<Sidecars>() {
            if let Ok(mut running) = state.0.lock() {
                running.remove(&self.pid);
            }
        }
    }
}

/// The flash engine's host over the Lab's handle: Tauri's emitter, and the
/// bundled espflash spawned through Tauri's shell plugin (Rust-side — the
/// webview holds no shell grant; see capabilities/default.json).
pub struct TauriHost(pub AppHandle);

impl Emit for TauriHost {
    fn emit<P: Serialize + Clone>(&self, event: &str, payload: P) {
        let _ = self.0.emit(event, payload);
    }
}

impl FlashHost for TauriHost {
    const USER_AGENT: &'static str = "SecuraCV-Lab";

    fn espflash<F>(
        &self,
        args: Vec<String>,
        mut on_output: F,
    ) -> impl Future<Output = Result<i32, String>> + Send
    where
        F: FnMut(&[u8]) + Send,
    {
        let app = self.0.clone();
        async move {
            let cmd = app
                .shell()
                .sidecar(ESPFLASH)
                .map_err(|e| format!("bundled espflash missing: {e}"))?
                .args(args);
            let (mut rx, child) = cmd
                .spawn()
                .map_err(|e| flash_engine::sidecar::spawn_error(ESPFLASH, &e.to_string()))?;
            let pid = child.pid();
            if let Some(state) = app.try_state::<Sidecars>() {
                if let Ok(mut running) = state.0.lock() {
                    running.insert(pid, child);
                }
            }
            let _tracked = Tracked {
                app: app.clone(),
                pid,
            };
            let mut code = -1;
            while let Some(event) = rx.recv().await {
                match event {
                    CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                        on_output(&bytes);
                    }
                    CommandEvent::Terminated(payload) => {
                        code = payload.code.unwrap_or(-1);
                    }
                    _ => {}
                }
            }
            Ok(code)
        }
    }
}

// ── the commands: the Flasher's names, arguments and DTOs ───────────────────

/// Serial ports the OS can see this instant — the Flasher's `list_ports`,
/// under its own name so either frontend's port picker works here unchanged.
/// (`list_serial_ports` in lib.rs is the same list under the name this seam
/// always promised.)
#[tauri::command]
pub fn list_ports() -> Result<Vec<PortDto>, String> {
    flash_engine::ports::list_ports()
}

/// Ask the connected board which ESP32 it is (and how much flash it carries).
#[tauri::command]
pub async fn detect_chip(app: AppHandle, port: String) -> Result<ChipInfo, String> {
    flash_engine::flash::detect_chip(&TauriHost(app), bundled_catalog(), port).await
}

/// Fetch the live release manifest so the UI can show what version is
/// currently published for each product — only ever one of the bundled
/// manifest URLs (the gate is the engine's, before any socket).
#[tauri::command]
pub async fn fetch_manifest(app: AppHandle, manifest_url: String) -> Result<Value, String> {
    flash_engine::flash::fetch_manifest(&TauriHost(app), bundled_catalog(), manifest_url).await
}

/// Resolve → download → flash. Streams every line espflash prints over the
/// `flash:log` event so the UI is a live console, then returns Ok on a clean
/// exit or a human error otherwise. The pipeline is the flash engine's
/// (flash_engine::flash::flash) — the Flasher runs the same one.
// The argument list IS the frontend's invoke contract (and the Flasher's twin
// takes the same nine), so it stays flat rather than becoming a struct.
#[allow(clippy::too_many_arguments)]
#[tauri::command]
pub async fn flash(
    app: AppHandle,
    port: String,
    product_id: String,
    manifest_url: String,
    baud: u32,
    detected_chip: String,
    provisioning: Option<Provisioning>,
    erase_first: Option<bool>,
    // The safety copy taken moments ago, if there is one. Present = draw the
    // change map from it; absent = the user skipped the copy or the board
    // wouldn't read, and no map is the truthful outcome.
    backup_path: Option<String>,
) -> Result<FlashReceipt, String> {
    let request = FlashRequest {
        port,
        product_id,
        manifest_url,
        baud,
        detected_chip,
        provisioning,
        erase_first,
        backup_path,
    };
    flash_engine::flash::flash(&TauriHost(app), bundled_catalog(), request).await
}

#[tauri::command]
pub fn start_serial_monitor(
    app: AppHandle,
    state: State<'_, SerialMonitorState>,
    port: String,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
    // True only when the flash flow starts the monitor: permits ONE deliberate
    // reboot of a native-USB board so its boot streams from the first line
    // (and a board espflash left stranded in its bootloader gets un-stuck).
    // Every other caller attaches as a pure observer.
    post_flash: Option<bool>,
) -> Result<(), String> {
    state.start(TauriHost(app), port, vid, pid, baud, post_flash)
}

#[tauri::command]
pub fn serial_monitor_send(
    state: State<'_, SerialMonitorState>,
    command: String,
) -> Result<(), String> {
    state.send(command)
}

#[tauri::command]
pub fn stop_serial_monitor(state: State<'_, SerialMonitorState>) -> Result<(), String> {
    state.stop()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    // The chip guard and the manifest allow-list the Lab's commands apply come
    // from the catalog build.rs embedded — the same file the Flasher embeds,
    // so the two apps refuse and accept the same things.

    #[test]
    fn the_engine_catalog_wraps_the_embedded_catalog() {
        assert_eq!(bundled_catalog().raw(), EMBEDDED_CATALOG);
        let embedded: Value = serde_json::from_str(EMBEDDED_CATALOG).unwrap();
        let chips = embedded["chips"].as_object().expect("catalog has chips");
        assert_eq!(bundled_catalog().chip_table().len(), chips.len());
        let manifest_url = embedded["manifest_url"].as_str().unwrap();
        let origin = bundled_catalog()
            .release_origin()
            .expect("catalog carries a releases/download manifest_url");
        assert!(manifest_url.starts_with(origin));
    }

    #[test]
    fn only_the_bundled_manifest_urls_are_fetchable() {
        let embedded: Value = serde_json::from_str(EMBEDDED_CATALOG).unwrap();
        let bundled = bundled_catalog();
        assert!(bundled.manifest_url_allowed(embedded["manifest_url"].as_str().unwrap()));
        assert!(bundled.manifest_url_allowed(flash_engine::catalog::DEV_FLASH_MANIFEST_URL));
        assert!(!bundled.manifest_url_allowed("https://example.com/manifest-flash.json"));
        assert!(!bundled.manifest_url_allowed(""));
    }

    #[test]
    fn the_chip_guard_names_every_catalog_chip_canonically() {
        let embedded: Value = serde_json::from_str(EMBEDDED_CATALOG).unwrap();
        for canon in embedded["chips"].as_object().unwrap().keys() {
            let sloppy = canon.to_lowercase().replace('-', "_");
            assert_eq!(
                bundled_catalog()
                    .canonical_chip(&format!("Chip type: {sloppy} (rev 0)"))
                    .as_deref(),
                Some(canon.as_str())
            );
        }
    }
}
