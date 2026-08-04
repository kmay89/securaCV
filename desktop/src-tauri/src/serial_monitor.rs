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

/// Espressif's own USB vendor ID. A port enumerating under it is the chip's
/// native USB peripheral rather than a USB-UART bridge, which is the single
/// fact that decides whether touching DTR is helpful or dangerous — see
/// [`wants_dtr`].
const ESPRESSIF_USB_VID: u16 = 0x303A;

/// Whether we should assert DTR on this port.
///
/// Only for a NATIVE USB port, and the distinction is not cosmetic:
///
///   * Native USB (VID 0x303A) — `Serial` is the chip's own USB peripheral.
///     Arduino's USBCDC treats "host asserted DTR" as "a terminal is attached"
///     and DISCARDS its output until it sees that, so without this the console
///     connects and stays blank forever. There is no reset circuit on these
///     lines, so asserting DTR cannot disturb the board.
///
///   * A USB-UART bridge (CP210x, CH340, FTDI) — DTR and RTS are wired to EN
///     and IO0 through the standard two-transistor circuit, and the board
///     resets or drops into the bootloader whenever the two lines DIFFER.
///     serialport-rs offers no way to move both at once, so ANY sequence we
///     write passes through a mismatched state and can reset a board that a
///     monitor has no business resetting. These boards also don't need it:
///     their `Serial` is a plain UART that transmits regardless.
///
/// So: assert where it is required and harmless, and don't touch the lines at
/// all where it is unnecessary and risky.
fn wants_dtr(vid: Option<u16>) -> bool {
    vid == Some(ESPRESSIF_USB_VID)
}

/// The port to talk to, and its USB vendor ID when we can see one.
fn matching_port(preferred: &str, vid: Option<u16>, pid: Option<u16>) -> Option<(String, Option<u16>)> {
    let ports = serialport::available_ports().ok()?;
    if let Some(found) = ports.iter().find(|port| port.port_name == preferred) {
        let seen = match &found.port_type {
            SerialPortType::UsbPort(info) => Some(info.vid),
            _ => None,
        };
        return Some((preferred.to_string(), seen));
    }
    ports.into_iter().find_map(|port| match port.port_type {
        SerialPortType::UsbPort(info)
            if vid.is_some() && info.vid == vid.unwrap() && pid == Some(info.pid) =>
        {
            Some((port.port_name, Some(info.vid)))
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

/// How long a freshly-connected board may say nothing before the console
/// explains itself. Long enough that an ordinary boot (or a board mid-reset)
/// isn't accused of being silent, short enough that nobody has decided their
/// hardware is dead first.
const SILENCE_SECS: u64 = 12;

/// Try to (re)open the board's port, retrying until it appears or we're
/// canceled. Returns None only when canceled while waiting — a USB-CDC board
/// takes a moment to re-enumerate after a reboot, so we wait it out rather than
/// give up. `first` only changes the status wording (connecting vs reconnecting).
fn connect(
    app: &AppHandle,
    preferred_port: &str,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
    cancel: &Arc<AtomicBool>,
    first: bool,
) -> Option<(String, Box<dyn serialport::SerialPort>)> {
    let mut announced = false;
    let mut open_hint_shown = false;
    while !cancel.load(Ordering::Relaxed) {
        if let Some((name, seen_vid)) = matching_port(preferred_port, vid, pid) {
            // DTR only where it is required AND harmless — see wants_dtr. On a
            // native-USB board the chip discards its output until a host raises
            // DTR, which is why this console was blank while `screen` worked.
            // On a bridge board the same line is half of a reset circuit, so we
            // leave both lines exactly as the driver left them: serialport-rs
            // cannot move DTR and RTS together, so every sequence we could write
            // passes through a mismatched state, and a mismatch is precisely
            // what resets the board or drops it into the bootloader. A monitor
            // must not reset the thing it is monitoring.
            let mut builder = serialport::new(&name, baud).timeout(Duration::from_millis(100));
            if wants_dtr(seen_vid) {
                builder = builder.dtr_on_open(true);
            }
            match builder.open() {
                Ok(port) => return Some((name, port)),
                Err(error) => {
                    // The retry loop is right for a port that's about to
                    // re-enumerate, but on Linux two failures deserve words
                    // instead of silence: permission denied never heals by
                    // itself, and "busy" is usually ModemManager holding the
                    // port (and possibly resetting the board). Without this,
                    // the user watches "Waiting…" forever with no clue.
                    if !open_hint_shown && cfg!(target_os = "linux") {
                        if let Some(hint) = crate::port_hint::linux_open_hint(&error.to_string()) {
                            open_hint_shown = true;
                            let _ = app.emit("serial:status", hint.to_string());
                        }
                    }
                    std::thread::sleep(Duration::from_millis(350));
                }
            }
        } else {
            if !announced {
                announced = true;
                let _ = app.emit(
                    "serial:status",
                    if first {
                        "Waiting for the board's serial port…".to_string()
                    } else {
                        "Board went away (reboot?) — waiting for it to come back…".to_string()
                    },
                );
            }
            std::thread::sleep(Duration::from_millis(350));
        }
    }
    None
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
    let mut read_buffer = [0u8; 2048];
    let mut line_buffer = String::new();
    let mut manifest = None;
    let mut vision = None;
    let mut first = true;

    // Outer loop: keep a live connection to the board across reboots and
    // cable blips. A read/write error (e.g. "Broken pipe" when the ESP32-S3's
    // USB-CDC port drops on reboot) is NOT the end of the monitor — we drop back
    // here and reopen, so the console "just works" without the user restarting.
    while !cancel.load(Ordering::Relaxed) {
        let Some((name, mut port)) = connect(&app, &preferred_port, vid, pid, baud, &cancel, first)
        else {
            break; // canceled while waiting
        };
        first = false;
        let _ = app.emit(
            "serial:status",
            format!("Serial monitor connected to {name} at {baud} baud."),
        );
        // Re-request the manifest on each (re)connect; a rebooted board reprints it.
        let mut next_manifest_request = Instant::now() + Duration::from_millis(700);
        line_buffer.clear();
        // Silence watch, armed per connection: a board that has said nothing at
        // all by the deadline gets one explanatory line (see below). Reset on
        // every reconnect, because a board that came back deserves a fresh
        // chance to be quiet for its own reasons.
        let mut heard_anything = false;
        let mut silence_noted = false;
        let silence_deadline = Instant::now() + Duration::from_secs(SILENCE_SECS);

        // Inner loop: pump the live port until it errors (→ reconnect) or we're
        // canceled (→ exit).
        let reconnect = loop {
            if cancel.load(Ordering::Relaxed) {
                break false;
            }
            let mut io_ok = true;
            while let Ok(bytes) = receive.try_recv() {
                if port.write_all(&bytes).is_err() {
                    io_ok = false;
                    break;
                }
                let _ = port.flush();
            }
            if !io_ok {
                break true; // write failed → the port dropped; reconnect
            }
            if manifest.is_none() && Instant::now() >= next_manifest_request {
                let _ = port.write_all(b"j\n");
                let _ = port.flush();
                next_manifest_request = Instant::now() + Duration::from_secs(3);
            }

            // A console that connects and then shows nothing is the worst shape
            // a diagnostic tool can take: it looks like the board is dead, when
            // usually the board is fine and the LINK is the problem. Say so
            // once, with the things actually worth trying, rather than leaving
            // someone staring at an empty pane deciding their hardware is
            // broken. Once only — a monitor that nags every ten seconds gets
            // ignored, taking the message that mattered with it.
            if !heard_anything && !silence_noted && Instant::now() >= silence_deadline {
                silence_noted = true;
                let _ = app.emit(
                    "serial:status",
                    format!(
                        "Connected to {name}, but the board hasn't said anything for \
                         {SILENCE_SECS}s. That usually isn't a dead board: it may simply not \
                         be printing (press EN/RESET to watch it boot), it may be running \
                         firmware built without a serial console, or another program — a \
                         second copy of this app, screen, or PlatformIO — may be holding \
                         the port. Try the reset button first."
                    ),
                );
            }

            match port.read(&mut read_buffer) {
                Ok(count) if count > 0 => {
                    // If we already accused the board of being silent, take it
                    // back the moment it speaks — otherwise the status line goes
                    // on saying "hasn't said anything" while its output visibly
                    // streams underneath, which is worse than never having
                    // warned. This is the common case after someone follows the
                    // advice and presses RESET.
                    if silence_noted && !heard_anything {
                        let _ = app.emit(
                            "serial:status",
                            format!("Serial monitor connected to {name} at {baud} baud."),
                        );
                    }
                    heard_anything = true;
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
                Err(_) => break true, // disconnect (Broken pipe, etc.) → reconnect
            }
        };

        if !reconnect {
            break;
        }
        let _ = app.emit("serial:status", "Reconnecting to the board…");
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
    // Send exactly what the caller asked for — the frontend owns the line
    // ending (its picker includes "No line ending" for raw keystrokes /
    // bootloader escape sequences), so the backend must not append one.
    control
        .send
        .send(command.into_bytes())
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

    // ── DTR is a per-board decision, not a global one ───────────────────
    //
    // Getting this backwards is destructive in one direction and merely
    // useless in the other, so it is pinned rather than reasoned about at the
    // call site.

    #[test]
    fn native_usb_boards_get_dtr_or_they_never_speak() {
        // ESP32-S3 targets build with ARDUINO_USB_CDC_ON_BOOT=1, so `Serial` is
        // the chip's own USB peripheral and Arduino's USBCDC discards output
        // until a host raises DTR. Without this the console connects and stays
        // blank forever — the bug this whole change exists to fix.
        assert!(wants_dtr(Some(ESPRESSIF_USB_VID)));
        assert_eq!(ESPRESSIF_USB_VID, 0x303A);
    }

    #[test]
    fn bridged_boards_are_left_completely_alone() {
        // CP210x, CH340 and FTDI wire DTR and RTS to EN and IO0 through the
        // standard two-transistor circuit: the board resets or drops into the
        // bootloader whenever the lines DIFFER. serialport-rs cannot move both
        // at once, so any sequence we write passes through a mismatched state.
        // A monitor must not reset the thing it is monitoring, and these boards
        // don't need DTR anyway — their Serial is a plain UART.
        for bridge in [0x10C4u16 /* CP210x */, 0x1A86 /* CH340 */, 0x0403 /* FTDI */] {
            assert!(!wants_dtr(Some(bridge)), "vid {bridge:#06x} must be left alone");
        }
    }

    #[test]
    fn an_unknown_port_is_left_alone_too() {
        // No USB descriptor (a real UART, a virtual port, anything we can't
        // identify) means we can't know whether the lines are wired to a reset
        // circuit. Doing nothing is the only safe default.
        assert!(!wants_dtr(None));
    }

    #[test]
    fn other_canaries_need_their_manifest_only() {
        assert!(receipt_ready(&serde_json::json!({"board":"canary"}), None));
    }
}
