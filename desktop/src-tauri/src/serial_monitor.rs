//! The serial monitor's Tauri commands. The monitor itself — the reconnect
//! rules, the DTR policy, the one post-flash reset, the silence advice and the
//! machine-readable boot receipt — is the flash engine's
//! (flash_engine::monitor), shared with the Lab; these wrappers only hand it
//! this app's event emitter and keep the command names and arguments the
//! frontend has always used.

use crate::host::TauriHost;
pub use flash_engine::monitor::SerialMonitorState;
use tauri::{AppHandle, State};

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
