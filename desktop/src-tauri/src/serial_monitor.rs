//! Persistent native serial monitor and machine-readable boot receipt parser.

use serde::Serialize;
use serde_json::Value;
use serialport::SerialPortType;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, State};

struct MonitorControl {
    cancel: Arc<AtomicBool>,
    send: mpsc::Sender<Vec<u8>>,
}

#[derive(Default)]
pub struct SerialMonitorState(Mutex<Option<MonitorControl>>);

#[derive(Clone, Serialize)]
struct HostReceipt {
    target: &'static str,
    ready: bool,
    manifest: Value,
    vision: Option<Value>,
}

fn matching_port(preferred: &str, vid: Option<u16>, pid: Option<u16>) -> Option<String> {
    let ports = serialport::available_ports().ok()?;
    if ports.iter().any(|port| port.port_name == preferred) {
        return Some(preferred.to_string());
    }
    ports.into_iter().find_map(|port| match port.port_type {
        SerialPortType::UsbPort(info)
            if vid.is_some() && info.vid == vid.unwrap() && pid == Some(info.pid) =>
        {
            Some(port.port_name)
        }
        _ => None,
    })
}

fn receipt_ready(manifest: &Value, vision: Option<&Value>) -> bool {
    let board = manifest.get("board").and_then(Value::as_str).unwrap_or("");
    if board != "canary-vision" {
        return true;
    }
    let Some(vision) = vision else {
        return false;
    };
    vision.get("i2c_ready").and_then(Value::as_bool) == Some(true)
        && vision.get("module_id").and_then(Value::as_i64).unwrap_or(0) > 0
}

fn inspect_line(
    app: &AppHandle,
    line: &str,
    manifest: &mut Option<Value>,
    vision: &mut Option<Value>,
) {
    let trimmed = line.trim();
    if let Some(raw_id) = trimmed.strip_prefix("Grove Vision AI ID=") {
        if let Ok(module_id) = raw_id.trim().parse::<i64>() {
            let proof = serde_json::json!({
                "schema": "securacv.vision.proof/v1",
                "module_id": module_id,
                "i2c_ready": module_id > 0,
                "source": "live-boot-line"
            });
            *vision = Some(proof.clone());
            let _ = app.emit("serial:vision", proof);
        }
    }
    if !trimmed.starts_with('{') || !trimmed.ends_with('}') {
        return;
    }
    let Ok(value) = serde_json::from_str::<Value>(trimmed) else {
        return;
    };
    match value.get("schema").and_then(Value::as_str) {
        Some("securacv.canary.manifest/v1") => *manifest = Some(value),
        Some("securacv.vision.proof/v1") => *vision = Some(value),
        _ => return,
    }
    if let Some(device) = manifest.clone() {
        let receipt = HostReceipt {
            target: "esp32-host",
            ready: receipt_ready(&device, vision.as_ref()),
            manifest: device,
            vision: vision.clone(),
        };
        let _ = app.emit("serial:receipt", receipt);
    }
}

fn monitor_thread(
    app: AppHandle,
    preferred_port: String,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
    cancel: Arc<AtomicBool>,
    receive: mpsc::Receiver<Vec<u8>>,
) {
    let connect_deadline = Instant::now() + Duration::from_secs(20);
    let mut opened = None;
    while !cancel.load(Ordering::Relaxed) && Instant::now() < connect_deadline {
        if let Some(name) = matching_port(&preferred_port, vid, pid) {
            match serialport::new(&name, baud)
                .timeout(Duration::from_millis(100))
                .open()
            {
                Ok(port) => {
                    opened = Some((name, port));
                    break;
                }
                Err(_) => std::thread::sleep(Duration::from_millis(350)),
            }
        } else {
            std::thread::sleep(Duration::from_millis(350));
        }
    }
    let Some((name, mut port)) = opened else {
        let _ = app.emit(
            "serial:status",
            "Serial monitor could not reopen the board after its reboot.",
        );
        return;
    };
    let _ = app.emit(
        "serial:status",
        format!("Serial monitor connected to {name} at {baud} baud."),
    );

    let mut read_buffer = [0u8; 2048];
    let mut line_buffer = String::new();
    let mut manifest = None;
    let mut vision = None;
    let mut next_manifest_request = Instant::now() + Duration::from_millis(700);

    while !cancel.load(Ordering::Relaxed) {
        while let Ok(bytes) = receive.try_recv() {
            if let Err(error) = port.write_all(&bytes) {
                let _ = app.emit("serial:status", format!("Serial write failed: {error}"));
                return;
            }
            let _ = port.flush();
        }
        if manifest.is_none() && Instant::now() >= next_manifest_request {
            let _ = port.write_all(b"j\n");
            let _ = port.flush();
            next_manifest_request = Instant::now() + Duration::from_secs(3);
        }

        match port.read(&mut read_buffer) {
            Ok(count) if count > 0 => {
                let text = String::from_utf8_lossy(&read_buffer[..count]);
                let _ = app.emit("serial:log", text.to_string());
                for character in text.chars() {
                    if character == '\n' {
                        inspect_line(
                            &app,
                            line_buffer.trim_end_matches('\r'),
                            &mut manifest,
                            &mut vision,
                        );
                        line_buffer.clear();
                    } else if line_buffer.len() < 16 * 1024 {
                        line_buffer.push(character);
                    } else {
                        line_buffer.clear();
                    }
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::TimedOut => {}
            Err(error) => {
                let _ = app.emit("serial:status", format!("Serial monitor stopped: {error}"));
                return;
            }
        }
    }
    let _ = app.emit("serial:status", "Serial monitor stopped.");
}

#[tauri::command]
pub fn start_serial_monitor(
    app: AppHandle,
    state: State<'_, SerialMonitorState>,
    port: String,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
) -> Result<(), String> {
    if !(1_200..=2_000_000).contains(&baud) {
        return Err("unsupported serial-monitor baud rate".into());
    }
    let mut active = state
        .0
        .lock()
        .map_err(|_| "serial-monitor state is unavailable".to_string())?;
    if let Some(old) = active.take() {
        old.cancel.store(true, Ordering::Relaxed);
    }
    let cancel = Arc::new(AtomicBool::new(false));
    let (send, receive) = mpsc::channel();
    *active = Some(MonitorControl {
        cancel: cancel.clone(),
        send,
    });
    std::thread::spawn(move || {
        monitor_thread(app, port, vid, pid, baud, cancel, receive);
    });
    Ok(())
}

#[tauri::command]
pub fn serial_monitor_send(
    state: State<'_, SerialMonitorState>,
    command: String,
) -> Result<(), String> {
    if command.len() > 128 || command.chars().any(|c| c == '\0') {
        return Err("serial command is too long or contains NUL".into());
    }
    let active = state
        .0
        .lock()
        .map_err(|_| "serial-monitor state is unavailable".to_string())?;
    let control = active
        .as_ref()
        .ok_or_else(|| "serial monitor is not running".to_string())?;
    let mut bytes = command.into_bytes();
    if !bytes.ends_with(b"\n") && !bytes.ends_with(b"\r") {
        bytes.push(b'\n');
    }
    control
        .send
        .send(bytes)
        .map_err(|_| "serial monitor has already stopped".to_string())
}

#[tauri::command]
pub fn stop_serial_monitor(state: State<'_, SerialMonitorState>) -> Result<(), String> {
    let mut active = state
        .0
        .lock()
        .map_err(|_| "serial-monitor state is unavailable".to_string())?;
    if let Some(control) = active.take() {
        control.cancel.store(true, Ordering::Relaxed);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vision_requires_both_manifest_and_live_i2c_module() {
        let manifest = serde_json::json!({"board":"canary-vision"});
        assert!(!receipt_ready(&manifest, None));
        assert!(!receipt_ready(
            &manifest,
            Some(&serde_json::json!({"i2c_ready":true,"module_id":0}))
        ));
        assert!(receipt_ready(
            &manifest,
            Some(&serde_json::json!({"i2c_ready":true,"module_id":7}))
        ));
    }

    #[test]
    fn other_canaries_need_their_manifest_only() {
        assert!(receipt_ready(&serde_json::json!({"board":"canary"}), None));
    }
}
