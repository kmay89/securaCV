//! This app's side of the flash engine's seam (flash_engine::host): Tauri's
//! event emitter, and the bundled espflash spawned through the launch guard.
//!
//! The engine owns everything the flash path decides and says; this file is
//! the only place the Flasher's sidecar spawn for it lives, so every espflash
//! run — detect, flash, local file, rescue bench, health reads — is tracked
//! by `launch_guard::spawn_tracked` exactly as before the engine existed.

use crate::launch_guard;
use flash_engine::host::{Emit, FlashHost};
use flash_engine::sidecar::ESPFLASH;
use serde::Serialize;
use std::future::Future;
use tauri::{AppHandle, Emitter};
use tauri_plugin_shell::process::CommandEvent;
use tauri_plugin_shell::ShellExt;

/// The engine's host over this app's handle.
pub struct TauriHost(pub AppHandle);

impl Emit for TauriHost {
    fn emit<P: Serialize + Clone>(&self, event: &str, payload: P) {
        let _ = self.0.emit(event, payload);
    }
}

impl FlashHost for TauriHost {
    const USER_AGENT: &'static str = "SecuraCV-Flasher";

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
            // Tracked, not bare: espflash's PID is on disk for as long as it
            // runs, so a force quit can't strand it holding the board's serial
            // port with nothing left to clean it up. `_ticket` un-records it on
            // the way out.
            let (mut rx, _child, _ticket) = launch_guard::spawn_tracked(&app, cmd, ESPFLASH)?;
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
