//! Persistent native serial monitor and machine-readable boot receipt parser.
//!
//! Shared by both desktop apps (the Flasher and the Lab's native flash path):
//! each app keeps one [`SerialMonitorState`] in Tauri's managed state and its
//! three `*_serial_monitor` commands are thin wrappers over
//! [`SerialMonitorState::start`] / [`send`](SerialMonitorState::send) /
//! [`stop`](SerialMonitorState::stop). Everything the frontend sees arrives as
//! the same four events in both apps — `serial:log`, `serial:status`,
//! `serial:receipt`, `serial:vision` — through the app's [`Emit`] sink.

use crate::host::Emit;
use serde::Serialize;
use serde_json::Value;
use serialport::SerialPortType;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

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

/// The outcome of looking for the board among the ports present right now.
#[derive(Debug, PartialEq)]
enum PortChoice {
    /// This one. Carries the port path and its USB vendor ID when visible.
    Found(String, Option<u16>),
    /// The board isn't here (yet) — keep waiting; it may be mid-re-enumeration.
    Absent,
    /// More than one port could plausibly be the board and nothing breaks the
    /// tie. Guessing here means attaching the console to somebody else's
    /// Canary on a two-board bench, so the caller says so instead.
    Ambiguous,
}

/// Pick the board's port from what the OS reports — pure, so the reconnect
/// rules live in tests rather than in a story about what probably happens.
///
/// The rules encode what a flash does to a native-USB board. After espflash's
/// reset the whole USB device drops off the bus and re-enumerates: the path
/// can change (macOS renumbers `usbmodem` freely), and the PID can change
/// too, because the ROM bootloader (0x303A:0x1001) and the application are
/// different USB devices wearing the same vendor ID. A monitor that demands
/// the exact identity it saw before the flash therefore waits forever for a
/// device that no longer exists — which is precisely the "it never connects
/// until I replug it" ritual. So:
///
///   * the exact path we were on wins outright — nothing re-enumerated;
///   * an Espressif-tracked board otherwise matches only as the SOLE
///     Espressif-VID port present. Not "the port wearing the remembered
///     VID:PID": an identity names a product, not a device, and after a flash
///     our board may be wearing any Espressif identity — so with two such
///     ports the one carrying the old identity may well be the OTHER board;
///   * a bridge-tracked board matches its exact VID:PID, but only when
///     unique — the bridge's identity is stable across the ESP's resets, and
///     duplicated the moment a second board of the same model is attached;
///   * anything plural → Ambiguous, said out loud, never a guess.
fn choose_port(
    preferred: &str,
    want_vid: Option<u16>,
    want_pid: Option<u16>,
    ports: &[(String, Option<u16>, Option<u16>)],
) -> PortChoice {
    if let Some((name, vid, _)) = ports.iter().find(|(name, _, _)| name == preferred) {
        return PortChoice::Found(name.clone(), *vid);
    }
    // Past the exact path, everything is identity — and a USB identity names a
    // PRODUCT, not a device. Two boards of the same model are
    // indistinguishable by VID:PID, and a just-flashed native-USB board can
    // come back wearing ANY Espressif identity (bootloader or app). So an
    // identity match is only evidence when it is the ONLY candidate: for an
    // Espressif-tracked board, the only trustworthy facts are "the exact path
    // survived" and "there is exactly one Espressif device here". Letting a
    // unique-looking VID:PID match win while a second Espressif port sat on
    // the bus was the review finding: the OTHER board could be the one
    // wearing the remembered identity, and the first connection carries the
    // one-shot post-flash reset — a coin-flip that reboots and certifies
    // somebody else's Canary.
    if want_vid == Some(ESPRESSIF_USB_VID) {
        let espressif: Vec<_> = ports
            .iter()
            .filter(|(_, vid, _)| *vid == Some(ESPRESSIF_USB_VID))
            .collect();
        match espressif.as_slice() {
            [] => PortChoice::Absent,
            [(name, vid, _)] => PortChoice::Found(name.clone(), *vid),
            _ => PortChoice::Ambiguous,
        }
    } else if want_vid.is_some() {
        // A bridge board: its identity is stable (the bridge chip never
        // re-enumerates when the ESP behind it resets), so an exact VID:PID
        // match is meaningful — but still only when unique, because two
        // boards on the same bridge chip share it.
        let matches: Vec<_> = ports
            .iter()
            .filter(|(_, vid, pid)| *vid == want_vid && *pid == want_pid)
            .collect();
        match matches.as_slice() {
            [] => PortChoice::Absent,
            [(name, vid, _)] => PortChoice::Found(name.clone(), *vid),
            _ => PortChoice::Ambiguous,
        }
    } else {
        PortChoice::Absent
    }
}

/// The OS half of [`choose_port`]: snapshot what's plugged in and choose.
fn matching_port(preferred: &str, vid: Option<u16>, pid: Option<u16>) -> PortChoice {
    let Ok(ports) = serialport::available_ports() else {
        return PortChoice::Absent;
    };
    let ports: Vec<(String, Option<u16>, Option<u16>)> = ports
        .into_iter()
        .map(|p| match p.port_type {
            SerialPortType::UsbPort(info) => (p.port_name, Some(info.vid), Some(info.pid)),
            _ => (p.port_name, None, None),
        })
        .collect();
    choose_port(preferred, vid, pid, &ports)
}

fn receipt_ready(manifest: &Value, vision: Option<&Value>) -> bool {
    let board = manifest.get("board").and_then(Value::as_str).unwrap_or("");
    if board != "canary-vision" {
        return true;
    }
    let Some(vision) = vision else {
        return false;
    };
    vision_module_ok(vision)
}

/// Whether a Vision proof says its Grove module answered.
///
/// Every proof needs `i2c_ready` and a positive `module_id`. A Vision image
/// from sweep F384's fix on also sends `module_ok`, its own verdict (begin()
/// and a fresh NAME? round trip), and when it is present it must be `true`:
/// read it, never infer it. Older images send no `module_ok`, and their
/// `module_id` is a RAM address, non-zero whatever the module did; their
/// `i2c_ready` already carried begin()'s answer, so the rule for them is the
/// one it always was.
fn vision_module_ok(vision: &Value) -> bool {
    let i2c_ready = vision.get("i2c_ready").and_then(Value::as_bool) == Some(true);
    let id_positive = vision.get("module_id").and_then(Value::as_i64).unwrap_or(0) > 0;
    let module_ok = match vision.get("module_ok") {
        None => true,
        Some(value) => value.as_bool() == Some(true),
    };
    i2c_ready && id_positive && module_ok
}

/// The Vision host's I2C boot line (`canary-vision/src/vision/vision_mgr.cpp`
/// prints `Grove Vision AI ID=%d`; `canary-local/tools/gen_vision.py`
/// `must()`s that format, so it holds).
const VISION_ID_LINE: &str = "Grove Vision AI ID=";

/// What the same function prints just above that line when the module did
/// not answer (`gen_vision.py` `must()`s it too).
const VISION_DEAD_LINE: &str = "ERROR: Grove Vision AI V2 not responding";

/// Whether a `Grove Vision AI ID=` value says the module answered, given
/// whether its ERROR line was the line just before it (both are printed
/// back to back by one function, so anything between them, a reset
/// included, means the ERROR line belonged to something else).
///
/// An image from F384's fix on prints 1 (answered) or 0 (did not), and the
/// ERROR line exactly with 0. An older image printed the address of the SSCMA
/// library's id buffer, non-zero whether or not the module answered, so a
/// positive value proved nothing; the ERROR line it printed when begin()
/// failed was its only verdict. Reading both refuses a dead module from
/// either image.
fn boot_line_module_ok(module_id: i64, dead_line_seen: bool) -> bool {
    module_id > 0 && !dead_line_seen
}

/// What the monitor carries from one boot line to the next.
#[derive(Default)]
struct LineState {
    manifest: Option<Value>,
    vision: Option<Value>,
    /// The last non-empty line was `VISION_DEAD_LINE`.
    vision_dead_line: bool,
}

fn inspect_line<S: Emit>(sink: &S, line: &str, state: &mut LineState) {
    let trimmed = line.trim();
    let dead_line_before = state.vision_dead_line;
    if !trimmed.is_empty() {
        state.vision_dead_line = trimmed.starts_with(VISION_DEAD_LINE);
    }
    if let Some(raw_id) = trimmed.strip_prefix(VISION_ID_LINE) {
        if let Ok(module_id) = raw_id.trim().parse::<i64>() {
            let module_ok = boot_line_module_ok(module_id, dead_line_before);
            let proof = serde_json::json!({
                "schema": "securacv.vision.proof/v1",
                "module_id": module_id,
                "module_ok": module_ok,
                "i2c_ready": module_ok,
                "source": "live-boot-line"
            });
            state.vision = Some(proof.clone());
            sink.emit("serial:vision", proof);
        }
    }
    if !trimmed.starts_with('{') || !trimmed.ends_with('}') {
        return;
    }
    let Ok(value) = serde_json::from_str::<Value>(trimmed) else {
        return;
    };
    match value.get("schema").and_then(Value::as_str) {
        Some("securacv.canary.manifest/v1") => state.manifest = Some(value),
        Some("securacv.vision.proof/v1") => state.vision = Some(value),
        _ => return,
    }
    if let Some(device) = state.manifest.clone() {
        let receipt = HostReceipt {
            target: "esp32-host",
            ready: receipt_ready(&device, state.vision.as_ref()),
            manifest: device,
            vision: state.vision.clone(),
        };
        sink.emit("serial:receipt", receipt);
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
fn connect<S: Emit>(
    sink: &S,
    preferred_port: &str,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
    cancel: &Arc<AtomicBool>,
    first: bool,
) -> Option<(String, Option<u16>, Box<dyn serialport::SerialPort>)> {
    let mut announced = false;
    let mut open_hint_shown = false;
    let mut ambiguity_noted = false;
    // If the board hasn't reappeared by this deadline, escalate from the calm
    // "waiting…" to instructions. The wait is normal for the first few seconds
    // (a native-USB board takes a moment to re-enumerate after a reset); past
    // ~8s it almost never resolves by itself — the usual cause is espflash's
    // post-flash reset not taking on a board wired straight to the chip's own
    // USB, which leaves it sitting in the ROM bootloader or wedged, and only a
    // button press or a replug moves it. Say that ONCE, when it has become
    // true, not up front where it would teach a ritual that usually isn't
    // needed.
    let escalate_at = Instant::now() + Duration::from_secs(8);
    let mut escalated = false;
    while !cancel.load(Ordering::Relaxed) {
        match matching_port(preferred_port, vid, pid) {
            PortChoice::Found(name, seen_vid) => {
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
                    Ok(mut port) => {
                        // On a native-USB port, follow the DTR assert with RTS so the
                        // steady state is BOTH lines high. This is not cosmetic. The
                        // USB-Serial-JTAG peripheral emulates the classic two-
                        // transistor auto-reset circuit on these two lines, and that
                        // circuit's truth table releases both chip lines only when
                        // DTR and RTS AGREE. DTR high alone (the state #1431 left)
                        // virtually holds the BOOT strap low — harmless while the
                        // chip runs, but the moment someone presses the physical
                        // RESET the chip samples that strap and wakes in DOWNLOAD
                        // MODE instead of the app. Our own silence advice says
                        // "press the reset button", so without this line the tool
                        // would be recommending the exact action that strands the
                        // board. (Bridge boards never reach here — wants_dtr is
                        // false for them and both lines stay untouched.)
                        if wants_dtr(seen_vid) {
                            let _ = port.write_request_to_send(true);
                        }
                        return Some((name, seen_vid, port));
                    }
                    Err(error) => {
                        // The retry loop is right for a port that's about to
                        // re-enumerate, but on Linux two failures deserve words
                        // instead of silence: permission denied never heals by
                        // itself, and "busy" is usually ModemManager holding the
                        // port (and possibly resetting the board). Without this,
                        // the user watches "Waiting…" forever with no clue.
                        if !open_hint_shown && cfg!(target_os = "linux") {
                            if let Some(hint) =
                                crate::port_hint::linux_open_hint(&error.to_string())
                            {
                                open_hint_shown = true;
                                sink.emit("serial:status", hint.to_string());
                            }
                        }
                        std::thread::sleep(Duration::from_millis(350));
                    }
                }
            }
            PortChoice::Ambiguous => {
                // Two or more Espressif boards and no exact match: attaching to
                // whichever enumerated first would put someone else's Canary in
                // the console with no visible sign anything is wrong. Refuse,
                // and say why — this only clears when a human unplugs one.
                if !ambiguity_noted {
                    ambiguity_noted = true;
                    sink.emit(
                        "serial:status",
                        "More than one Espressif board is plugged in and I can't tell which \
                         one to watch. Unplug the other board (or replug the one you want) \
                         and I'll connect to it."
                            .to_string(),
                    );
                }
                std::thread::sleep(Duration::from_millis(350));
            }
            PortChoice::Absent => {
                if !announced {
                    announced = true;
                    sink.emit(
                        "serial:status",
                        if first {
                            "Waiting for the board's serial port…".to_string()
                        } else {
                            "Board went away (reboot?) — waiting for it to come back…".to_string()
                        },
                    );
                }
                if !escalated && Instant::now() >= escalate_at {
                    escalated = true;
                    sink.emit(
                        "serial:status",
                        "Still waiting for the board to come back. On boards wired straight \
                         to the chip's own USB, the flasher's automatic reset doesn't always \
                         take — press the board's RESET button, or unplug the cable and plug \
                         it back in. I'll connect the moment it reappears."
                            .to_string(),
                    );
                }
                std::thread::sleep(Duration::from_millis(350));
            }
        }
    }
    None
}

/// Reboot a native-USB board into its application, from the console's steady
/// line state (DTR and RTS both asserted).
///
/// This exists because espflash's own post-flash reset is best-effort on a
/// board wired straight to the chip's USB: when it doesn't take, the board is
/// left sitting in its ROM bootloader — which enumerates a perfectly healthy
/// serial port that will never print a byte. The console then connects to
/// that silence, and the only escape was the unplug/replug ritual. Resetting
/// deliberately, once, right after the flash, replaces the ritual with
/// "watch it boot".
///
/// The sequence walks the emulated two-transistor circuit (EN = !(RTS&!DTR),
/// IO0 = !(DTR&!RTS)) from (1,1) — everything released — through reset with
/// the BOOT strap high, back to released:
///
///   (1,1) → DTR low   ⇒ (0,1): EN pulled low, chip held in reset, IO0 high
///           hold      ⇒ the pulse is long enough to be unmissable
///   (0,1) → RTS low   ⇒ (0,0): EN released — the chip boots, samples IO0
///           high, and starts the APPLICATION, never the bootloader
///           hold      ⇒ let it out of reset before touching lines again
///   (0,0) → both high ⇒ back to the steady attached state the CDC gating
///           wants
///
/// On a board whose application speaks TinyUSB CDC instead, these line
/// changes reach the Arduino core's own line-state handler, which implements
/// the same reset semantics on purpose (it is how esptool resets those boards
/// without any buttons). Either way the device usually re-enumerates —
/// the caller's reconnect loop is the continuation of this function.
///
/// UNVERIFIED ON HARDWARE, stated plainly: the truth table is from the chip's
/// documented emulation of the classic circuit, and the sequence mirrors
/// esptool's hard-reset strategy, but no board has been in front of this
/// code. If it does nothing, nothing is lost — the escalation message still
/// teaches the button — and it can never fire on a bridge board at all.
fn reset_into_app(port: &mut dyn serialport::SerialPort) {
    let _ = port.write_data_terminal_ready(false);
    std::thread::sleep(Duration::from_millis(200));
    let _ = port.write_request_to_send(false);
    std::thread::sleep(Duration::from_millis(200));
    let _ = port.write_data_terminal_ready(true);
    let _ = port.write_request_to_send(true);
}

#[allow(clippy::too_many_arguments)]
fn monitor_thread<S: Emit>(
    sink: S,
    preferred_port: String,
    vid: Option<u16>,
    pid: Option<u16>,
    baud: u32,
    post_flash: bool,
    cancel: Arc<AtomicBool>,
    receive: mpsc::Receiver<Vec<u8>>,
) {
    let mut read_buffer = [0u8; 2048];
    let mut line_buffer = String::new();
    let mut lines = LineState::default();
    let mut first = true;
    // The post-flash reset happens at most ONCE per monitor. Without the
    // latch, the cycle would be: reset → device re-enumerates → reconnect →
    // reset → … forever, with the board never getting past its bootloader
    // messages. One deliberate reset, then the monitor is an observer again.
    let mut reset_pending = post_flash;

    // Outer loop: keep a live connection to the board across reboots and
    // cable blips. A read/write error (e.g. "Broken pipe" when the ESP32-S3's
    // USB-CDC port drops on reboot) is NOT the end of the monitor — we drop back
    // here and reopen, so the console "just works" without the user restarting.
    while !cancel.load(Ordering::Relaxed) {
        let Some((name, seen_vid, mut port)) =
            connect(&sink, &preferred_port, vid, pid, baud, &cancel, first)
        else {
            break; // canceled while waiting
        };
        first = false;
        // Right after a flash, make the "did it take?" question moot: reboot
        // the board ourselves and show its boot from the first line — the same
        // behavior the browser flasher has always had. Scoped to native-USB
        // Espressif ports, where espflash's own reset is the unreliable one
        // and where these lines cannot reach a physical reset circuit by
        // accident; a bridge board keeps espflash's reliable EN-line reset
        // and is never touched here.
        if reset_pending {
            // The latch clears on the FIRST connection no matter what kind of
            // port it turned out to be. A bridge board doesn't need us (its
            // espflash reset ran over a real EN line) — and a pending reset
            // that survived past this connection could fire on some entirely
            // different board plugged in later, which is exactly the class of
            // surprise a monitor must never produce.
            reset_pending = false;
            if wants_dtr(seen_vid) {
                sink.emit(
                    "serial:status",
                    "Rebooting the board so you can watch it start from the first line…"
                        .to_string(),
                );
                reset_into_app(port.as_mut());
            }
        }
        sink.emit(
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
            if lines.manifest.is_none() && Instant::now() >= next_manifest_request {
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
                // On a native-USB port there is one more silent-and-healthy
                // state worth naming: the ROM bootloader. It enumerates a
                // perfectly ordinary port and never prints, and it is where a
                // board lands when a flash's automatic reset doesn't take.
                let bootloader_hint = if wants_dtr(seen_vid) {
                    " If it was just flashed, it may still be sitting in its \
                     bootloader waiting for a real reset."
                } else {
                    ""
                };
                sink.emit(
                    "serial:status",
                    format!(
                        "Connected to {name}, but the board hasn't said anything for \
                         {SILENCE_SECS}s. That usually isn't a dead board: it may simply not \
                         be printing (press EN/RESET to watch it boot), it may be running \
                         firmware built without a serial console, or another program — a \
                         second copy of this app, screen, or PlatformIO — may be holding \
                         the port.{bootloader_hint} Try the reset button first."
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
                        sink.emit(
                            "serial:status",
                            format!("Serial monitor connected to {name} at {baud} baud."),
                        );
                    }
                    heard_anything = true;
                    let text = String::from_utf8_lossy(&read_buffer[..count]);
                    sink.emit("serial:log", text.to_string());
                    for character in text.chars() {
                        if character == '\n' {
                            inspect_line(&sink, line_buffer.trim_end_matches('\r'), &mut lines);
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
        sink.emit("serial:status", "Reconnecting to the board…");
    }
    sink.emit("serial:status", "Serial monitor stopped.");
}

impl SerialMonitorState {
    /// Start the monitor on `port` (replacing any monitor already running),
    /// reporting through `sink` from its own thread until [`stop`](Self::stop).
    ///
    /// `post_flash` is true only when the flash flow starts the monitor: it
    /// permits ONE deliberate reboot of a native-USB board so its boot streams
    /// from the first line (and a board espflash left stranded in its
    /// bootloader gets un-stuck). Every other caller attaches as a pure
    /// observer.
    pub fn start<S: Emit + Send + 'static>(
        &self,
        sink: S,
        port: String,
        vid: Option<u16>,
        pid: Option<u16>,
        baud: u32,
        post_flash: Option<bool>,
    ) -> Result<(), String> {
        if !(1_200..=2_000_000).contains(&baud) {
            return Err("unsupported serial-monitor baud rate".into());
        }
        let mut active = self
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
            monitor_thread(
                sink,
                port,
                vid,
                pid,
                baud,
                post_flash.unwrap_or(false),
                cancel,
                receive,
            );
        });
        Ok(())
    }

    /// Hand `command` to the running monitor, byte for byte.
    pub fn send(&self, command: String) -> Result<(), String> {
        if command.len() > 128 || command.chars().any(|c| c == '\0') {
            return Err("serial command is too long or contains NUL".into());
        }
        let active = self
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

    /// Stop the running monitor, if any. Stopping a stopped monitor is fine.
    pub fn stop(&self) -> Result<(), String> {
        let mut active = self
            .0
            .lock()
            .map_err(|_| "serial-monitor state is unavailable".to_string())?;
        if let Some(control) = active.take() {
            control.cancel.store(true, Ordering::Relaxed);
        }
        Ok(())
    }
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

    // ── the Vision module proof (sweep F384) ────────────────────────────
    //
    // An image before F384's fix printed `module_id` as the address of the
    // SSCMA library's id buffer: non-zero whether or not the module answered.
    // An image from the fix on prints 1 or 0 and adds `module_ok`. The
    // records below are each image's real shape (canary-vision/src/main.cpp's
    // `j` proof; vision_mgr.cpp's boot lines).

    /// An `Emit` that keeps every event, for reading back.
    #[derive(Default)]
    struct Events(Mutex<Vec<(String, Value)>>);

    impl Emit for Events {
        fn emit<P: Serialize + Clone>(&self, event: &str, payload: P) {
            let value = serde_json::to_value(payload).unwrap();
            self.0.lock().unwrap().push((event.to_string(), value));
        }
    }

    impl Events {
        fn last(&self, event: &str) -> Option<Value> {
            let events = self.0.lock().unwrap();
            events
                .iter()
                .rev()
                .find(|(e, _)| e == event)
                .map(|(_, v)| v.clone())
        }
    }

    /// Feed `lines` through the monitor's own line reader.
    fn read_lines(lines: &[&str]) -> (Events, LineState) {
        let sink = Events::default();
        let mut state = LineState::default();
        for line in lines {
            inspect_line(&sink, line, &mut state);
        }
        (sink, state)
    }

    const MANIFEST_LINE: &str =
        r#"{"schema":"securacv.canary.manifest/v1","board":"canary-vision","firmware":"2.4.16"}"#;
    // The address an older image printed for a module that answered and for
    // one that did not alike (an ESP32-C3 DRAM address, as `%d`).
    const RAM_ADDRESS: &str = "1070379020";

    #[test]
    fn an_older_proof_record_keeps_its_rule() {
        let manifest = serde_json::json!({"board":"canary-vision"});
        // An older image's `j` record for a module that answered: a RAM
        // address, i2c_ready from begin(), no module_ok. Still ready.
        let alive = serde_json::json!({
            "schema":"securacv.vision.proof/v1","module_id":1070379020,
            "i2c_ready":true,"witness_ready":true,
            "wifi_configured":true,"wifi_connected":true,"mqtt_connected":true});
        assert!(receipt_ready(&manifest, Some(&alive)));
        // ...and for a dead module: the same address, i2c_ready false.
        let dead = serde_json::json!({
            "schema":"securacv.vision.proof/v1","module_id":1070379020,
            "i2c_ready":false,"witness_ready":true,
            "wifi_configured":true,"wifi_connected":true,"mqtt_connected":true});
        assert!(!receipt_ready(&manifest, Some(&dead)));
    }

    #[test]
    fn a_new_proof_record_is_read_by_its_module_ok() {
        let manifest = serde_json::json!({"board":"canary-vision"});
        let alive = serde_json::json!({
            "schema":"securacv.vision.proof/v1","module_id":1,"module_ok":true,
            "i2c_ready":true,"witness_ready":true,
            "wifi_configured":false,"wifi_connected":false,"mqtt_connected":false});
        assert!(receipt_ready(&manifest, Some(&alive)));
        let dead = serde_json::json!({
            "schema":"securacv.vision.proof/v1","module_id":0,"module_ok":false,
            "i2c_ready":false,"witness_ready":true,
            "wifi_configured":false,"wifi_connected":false,"mqtt_connected":false});
        assert!(!receipt_ready(&manifest, Some(&dead)));
        // module_ok present decides, whatever the other two say, and only a
        // real `true` is a yes.
        for module_ok in [
            serde_json::json!(false),
            serde_json::json!("true"),
            serde_json::json!(1),
            Value::Null,
        ] {
            let record = serde_json::json!({
                "module_id":1,"module_ok":module_ok,"i2c_ready":true});
            assert!(
                !receipt_ready(&manifest, Some(&record)),
                "module_ok {module_ok} must refuse"
            );
        }
    }

    #[test]
    fn the_receipt_refuses_a_dead_module_from_its_proof_line() {
        // The whole `j` answer as the monitor reads it, from a new image whose
        // module did not answer: a receipt, and not a ready one.
        let proof = r#"{"schema":"securacv.vision.proof/v1","module_id":0,"module_ok":false,"i2c_ready":false,"witness_ready":true,"wifi_configured":true,"wifi_connected":true,"mqtt_connected":false}"#;
        let (sink, _) = read_lines(&[MANIFEST_LINE, proof]);
        let receipt = sink.last("serial:receipt").expect("a receipt");
        assert_eq!(receipt["ready"], Value::Bool(false));
        let proof = r#"{"schema":"securacv.vision.proof/v1","module_id":1,"module_ok":true,"i2c_ready":true,"witness_ready":true,"wifi_configured":true,"wifi_connected":true,"mqtt_connected":false}"#;
        let (sink, _) = read_lines(&[MANIFEST_LINE, proof]);
        assert_eq!(
            sink.last("serial:receipt").unwrap()["ready"],
            Value::Bool(true)
        );
    }

    #[test]
    fn the_boot_line_from_a_new_image() {
        let (sink, _) = read_lines(&["", "[I2C]", "Grove Vision AI ID=1"]);
        let proof = sink.last("serial:vision").expect("a boot-line proof");
        assert_eq!(proof["module_ok"], Value::Bool(true));
        assert_eq!(proof["i2c_ready"], Value::Bool(true));
        assert_eq!(proof["module_id"], serde_json::json!(1));
        assert_eq!(proof["source"], "live-boot-line");

        let (sink, _) = read_lines(&[
            "", "[I2C]",
            "ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)",
            "Grove Vision AI ID=0",
        ]);
        let proof = sink.last("serial:vision").expect("a boot-line proof");
        assert_eq!(proof["module_ok"], Value::Bool(false));
        assert_eq!(proof["i2c_ready"], Value::Bool(false));
    }

    #[test]
    fn the_boot_line_from_an_older_image_refuses_a_dead_module() {
        // The older image's dead module: its ERROR line, then the address.
        // Before F384's fix this proof said i2c_ready: true.
        let id_line = format!("Grove Vision AI ID={RAM_ADDRESS}");
        let (sink, state) = read_lines(&[
            "", "[I2C]",
            "ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)",
            &id_line,
        ]);
        let proof = sink.last("serial:vision").expect("a boot-line proof");
        assert_eq!(proof["module_ok"], Value::Bool(false));
        assert_eq!(proof["i2c_ready"], Value::Bool(false));
        assert!(
            !state.vision_dead_line,
            "the ERROR line is spent on its own ID line"
        );

        // An ERROR line, then anything else (here a reset's banner) before the
        // ID line: it was not this ID line's ERROR line.
        let (sink, _) = read_lines(&[
            "ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)",
            "              ,_,          Waking up...",
            "",
            "[I2C]",
            &id_line,
        ]);
        assert_eq!(
            sink.last("serial:vision").unwrap()["module_ok"],
            Value::Bool(true)
        );

        // The same image with a module that answered: no ERROR line.
        let (sink, _) = read_lines(&["", "[I2C]", &id_line]);
        assert_eq!(
            sink.last("serial:vision").unwrap()["module_ok"],
            Value::Bool(true)
        );

        // A dead boot, then a reboot whose module answered: the earlier ERROR
        // line does not carry over to the next ID line.
        let (sink, _) = read_lines(&[
            "ERROR: Grove Vision AI V2 not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)",
            &id_line,
            "              ,_,          Waking up...",
            "[I2C]",
            &id_line,
        ]);
        assert_eq!(
            sink.last("serial:vision").unwrap()["module_ok"],
            Value::Bool(true)
        );
    }

    #[test]
    fn a_boot_line_module_ok_needs_a_positive_id_and_no_error_line() {
        assert!(boot_line_module_ok(1, false));
        assert!(!boot_line_module_ok(1, true));
        assert!(!boot_line_module_ok(0, false));
        assert!(!boot_line_module_ok(0, true));
        assert!(!boot_line_module_ok(-5, false));
        assert!(boot_line_module_ok(1_070_379_020, false));
        assert!(!boot_line_module_ok(1_070_379_020, true));
    }

    // ── finding the board again after a flash ───────────────────────────
    //
    // A flash re-enumerates a native-USB board: new path, possibly a new PID
    // (ROM bootloader and application are different USB devices wearing the
    // same vendor ID). These pin the reconnect ladder — the old behavior of
    // demanding the exact pre-flash identity is what made the monitor wait
    // forever while the user performed the unplug/replug ritual.

    fn port(name: &str, vid: u16, pid: u16) -> (String, Option<u16>, Option<u16>) {
        (name.to_string(), Some(vid), Some(pid))
    }

    #[test]
    fn the_exact_path_wins_when_it_still_exists() {
        let ports = vec![port("/dev/cu.usbmodem101", 0x303A, 0x1001)];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &ports),
            PortChoice::Found("/dev/cu.usbmodem101".into(), Some(0x303A))
        );
    }

    #[test]
    fn a_moved_path_is_found_again_by_exact_identity() {
        // macOS renumbered usbmodem after the reboot; same device identity.
        let ports = vec![port("/dev/cu.usbmodem102", 0x303A, 0x1001)];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &ports),
            PortChoice::Found("/dev/cu.usbmodem102".into(), Some(0x303A))
        );
    }

    #[test]
    fn a_changed_pid_still_finds_the_sole_espressif_board() {
        // THE post-flash case: we remembered the bootloader (0x1001), the app
        // came back as its own CDC identity. One Espressif port present — it
        // can only be ours.
        let ports = vec![
            port("/dev/cu.usbmodem103", 0x303A, 0x0002),
            port("/dev/ttyUSB0", 0x10C4, 0xEA60), // someone's CP210x, ignored
        ];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &ports),
            PortChoice::Found("/dev/cu.usbmodem103".into(), Some(0x303A))
        );
    }

    #[test]
    fn two_espressif_boards_and_no_exact_match_is_a_refusal_not_a_guess() {
        // Attaching to whichever enumerated first would put someone else's
        // Canary in the console with no visible sign anything was wrong.
        let ports = vec![
            port("/dev/cu.usbmodem103", 0x303A, 0x0002),
            port("/dev/cu.usbmodem104", 0x303A, 0x1001),
        ];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x9999), &ports),
            PortChoice::Ambiguous
        );
    }

    #[test]
    fn a_remembered_identity_does_not_break_a_two_espressif_tie() {
        // The first version of this test asserted the OPPOSITE — that the
        // remembered VID:PID breaks the tie — and review overturned it. A USB
        // identity names a product, not a device: after a flash our board can
        // come back wearing any Espressif identity, so the port that still
        // wears the remembered one may well be the OTHER board. With the
        // one-shot post-flash reset riding the first connection, guessing
        // here reboots and certifies somebody else's Canary. Refuse.
        let ports = vec![
            port("/dev/cu.usbmodem103", 0x303A, 0x0002),
            port("/dev/cu.usbmodem104", 0x303A, 0x1001),
        ];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &ports),
            PortChoice::Ambiguous
        );
    }

    #[test]
    fn identical_twin_boards_are_ambiguous() {
        // The common real bench: two of the same product, byte-identical
        // identities. Any pick is a coin flip, and a silent coin flip is the
        // worst outcome a monitor can produce.
        let ports = vec![
            port("/dev/cu.usbmodem103", 0x303A, 0x1001),
            port("/dev/cu.usbmodem104", 0x303A, 0x1001),
        ];
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &ports),
            PortChoice::Ambiguous
        );
    }

    #[test]
    fn a_bridge_board_matches_by_identity_only_when_unique() {
        // A bridge chip's identity is stable across the ESP's resets, so a
        // unique match is meaningful — and a duplicated one still is not.
        let one = vec![
            port("/dev/ttyUSB0", 0x10C4, 0xEA60),
            port("/dev/cu.usbmodem103", 0x303A, 0x1001), // unrelated native board
        ];
        assert_eq!(
            choose_port("/dev/ttyUSB9", Some(0x10C4), Some(0xEA60), &one),
            PortChoice::Found("/dev/ttyUSB0".into(), Some(0x10C4))
        );
        let two = vec![
            port("/dev/ttyUSB0", 0x10C4, 0xEA60),
            port("/dev/ttyUSB1", 0x10C4, 0xEA60),
        ];
        assert_eq!(
            choose_port("/dev/ttyUSB9", Some(0x10C4), Some(0xEA60), &two),
            PortChoice::Ambiguous
        );
    }

    #[test]
    fn the_espressif_fallback_never_applies_to_bridge_boards() {
        // We remembered a CP210x; a random Espressif device appearing is NOT
        // evidence our board came back — a bridge board's identity never
        // changes across a flash, so anything else is a different device.
        let ports = vec![port("/dev/cu.usbmodem103", 0x303A, 0x1001)];
        assert_eq!(
            choose_port("/dev/ttyUSB0", Some(0x10C4), Some(0xEA60), &ports),
            PortChoice::Absent
        );
    }

    #[test]
    fn nothing_plugged_in_is_absent_and_keeps_waiting() {
        assert_eq!(
            choose_port("/dev/cu.usbmodem101", Some(0x303A), Some(0x1001), &[]),
            PortChoice::Absent
        );
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
        for bridge in [
            0x10C4u16, /* CP210x */
            0x1A86,    /* CH340 */
            0x0403,    /* FTDI */
        ] {
            assert!(
                !wants_dtr(Some(bridge)),
                "vid {bridge:#06x} must be left alone"
            );
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
