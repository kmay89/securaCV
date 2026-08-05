//! The native **live bench** for the Grove Vision AI V2 — look through the
//! module's camera without reflashing anything. Mirrors the browser Lab's
//! bench (`canary-local/assets/we2-flash.js` mountBench) protocol exactly:
//! continuous `AT+INVOKE=-1,0,0` for frames-with-boxes, `AT+BREAK` to stop,
//! and the two on-module thresholds (`AT+TSCORE` / `AT+TIOU`) set-then-read-
//! back so the sliders never lie.
//!
//! Shape: one background thread owns the serial port (the same pattern as
//! `serial_monitor.rs`), streams every SSCMA type-1 INVOKE event to the
//! frontend over the `we2:bench` Tauri event, and services slider commands
//! from an mpsc channel — replies are matched back by the same rule
//! `we2.rs::at_command` uses (`sscma::reply_matches`). The wire framing is the
//! host-tested `sscma::FrameScanner`.
//!
//! Privacy: camera frames go from the module, over the user's own USB cable,
//! to this window — and nowhere else. Nothing here writes to module flash.

use base64::Engine as _;
use serde_json::{json, Value};
use serialport::{ClearBuffer, SerialPort};
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, State};

use crate::sscma;
use crate::we2;

/// One slider/query command in flight to the module, with its reply channel.
pub struct BenchCmd {
    body: String,
    reply: mpsc::Sender<Result<Value, String>>,
}

struct BenchControl {
    cancel: Arc<AtomicBool>,
    send: mpsc::Sender<BenchCmd>,
    /// The worker's handle — stop/replace JOIN it, so the serial port is
    /// actually free before whatever needs it next (an immediate restart, a
    /// module flash) tries to open it.
    handle: std::thread::JoinHandle<()>,
}

#[derive(Default)]
pub struct We2BenchState(Mutex<Option<BenchControl>>);

/// Monotonic bench-session id. Every event a worker emits carries its session,
/// so a dying worker's tail (its `stopped`, a late frame) can never be mistaken
/// for — or tear down — a replacement bench the frontend has since started.
static SESSION: AtomicU64 = AtomicU64::new(0);

fn open_port(port_name: &str) -> Result<Box<dyn SerialPort>, String> {
    serialport::new(port_name, we2::BAUD)
        .timeout(Duration::from_millis(15))
        .open()
        .map_err(|e| {
            let mut message = format!("could not open the Vision module port: {e}");
            if cfg!(target_os = "linux") {
                if let Some(hint) = crate::port_hint::linux_open_hint(&message) {
                    message = format!("{message}\n{hint}");
                }
            }
            message
        })
}

fn write_line(port: &mut Box<dyn SerialPort>, body: &str) -> Result<(), String> {
    port.write_all(sscma::at_line(body).as_bytes())
        .and_then(|_| port.flush())
        .map_err(|e| format!("module serial write failed: {e}"))
}

/// Send one command BEFORE the stream starts and scan for its type-0 reply.
/// Returns Ok(None) on a quiet module (timeout), Err only on a wire failure.
fn do_cmd(
    port: &mut Box<dyn SerialPort>,
    body: &str,
    timeout: Duration,
) -> Result<Option<Value>, String> {
    write_line(port, body)?;
    let deadline = Instant::now() + timeout;
    let mut scanner = sscma::FrameScanner::new();
    let mut byte = [0u8; 1];
    while Instant::now() < deadline {
        match port.read(&mut byte) {
            Ok(1) => {
                if let Some(frame) = scanner.push(byte[0]) {
                    if let Ok(value) = serde_json::from_slice::<Value>(&frame) {
                        let name = value.get("name").and_then(Value::as_str).unwrap_or("");
                        if value.get("type").and_then(Value::as_i64) == Some(0)
                            && sscma::reply_matches(name, body)
                        {
                            return Ok(Some(value));
                        }
                    }
                }
            }
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(format!("module serial read failed: {e}")),
        }
    }
    Ok(None)
}

/// Pull the model card out of an `INFO?` reply: the data is a base64 JSON
/// card, either bare or nested as `data.info` (both shapes exist in the
/// field — the browser bench documents the same). Returns the card's name and
/// whether it pins the `person` class (which is what earns "person" labels —
/// honesty about an unknown model's classes, mirroring the browser).
fn model_card(reply: &Value) -> (Option<String>, bool) {
    let raw = match reply.get("data") {
        Some(Value::String(s)) => Some(s.as_str()),
        Some(other) => other.get("info").and_then(Value::as_str),
        None => None,
    };
    let Some(raw) = raw else {
        return (None, false);
    };
    let Ok(bytes) = base64::engine::general_purpose::STANDARD.decode(raw) else {
        return (None, false);
    };
    let Ok(card) = serde_json::from_slice::<Value>(&bytes) else {
        return (None, false);
    };
    let name = card
        .get("name")
        .and_then(Value::as_str)
        .map(ToOwned::to_owned);
    let pinned = card
        .get("classes")
        .and_then(Value::as_array)
        .map(|c| c.iter().any(|v| v.as_str() == Some("person")))
        .unwrap_or(false);
    (name, pinned)
}

struct Pending {
    body: String,
    reply: mpsc::Sender<Result<Value, String>>,
    deadline: Instant,
}

/// Wake the module and return its `VER?` reply, or Ok(None) if it never spoke.
///
/// Opening the CH343 port wiggles the modem lines — the very RTS line the
/// flasher drives as the module's reset — so the module is usually mid-boot
/// when the bench's first bytes leave the host. A single probe here made a
/// freshly flashed, provably working module look dead. So: hold reset
/// released explicitly (open() leaves the line wherever the OS put it),
/// probe a few times while a booting module comes up, and if it stays quiet,
/// pulse reset once ourselves — the "power-cycle it" advice, automated —
/// and probe again before giving up.
fn wake_module(
    port: &mut Box<dyn SerialPort>,
    emit: &impl Fn(Value),
) -> Result<Option<Value>, String> {
    let _ = port.write_request_to_send(true); // release reset if open() left it held
    for _ in 0..3 {
        if let Some(reply) = do_cmd(port, "VER?", Duration::from_millis(1200))? {
            return Ok(Some(reply));
        }
    }
    emit(json!({
        "kind": "log",
        "line": "quiet — resetting the module and waiting for it to boot…"
    }));
    port.write_request_to_send(false)
        .map_err(|e| format!("could not assert module reset: {e}"))?;
    std::thread::sleep(Duration::from_millis(100));
    port.write_request_to_send(true)
        .map_err(|e| format!("could not release module reset: {e}"))?;
    std::thread::sleep(Duration::from_millis(1200));
    let _ = port.clear(ClearBuffer::Input);
    for _ in 0..3 {
        if let Some(reply) = do_cmd(port, "VER?", Duration::from_millis(1500))? {
            return Ok(Some(reply));
        }
    }
    Ok(None)
}

fn bench_thread(
    app: AppHandle,
    port_name: String,
    session: u64,
    cancel: Arc<AtomicBool>,
    commands: mpsc::Receiver<BenchCmd>,
) {
    let emit = |payload: Value| {
        let mut payload = payload;
        if let Some(map) = payload.as_object_mut() {
            map.insert("session".into(), json!(session));
        }
        let _ = app.emit("we2:bench", payload);
    };

    let mut port = match open_port(&port_name) {
        Ok(p) => p,
        Err(e) => {
            emit(json!({ "kind": "error", "message": e }));
            return;
        }
    };

    // Identity first, so the preview starts honest: SSCMA build + what model
    // the module carries (its card decides whether labels may read "person").
    emit(json!({ "kind": "log", "line": "→ asking the module what it carries…" }));
    let ver = match wake_module(&mut port, &emit) {
        Ok(v) => v,
        Err(e) => {
            emit(json!({ "kind": "error", "message": e }));
            return;
        }
    };
    if ver.is_none() {
        emit(json!({
            "kind": "error",
            "message": "The module didn't answer AT, even after an automatic reset. Check the cable is in the MODULE's own USB-C port (the CH343, \"USB Single Serial\"), unplug/replug, and try again; if it keeps refusing, flash the model first."
        }));
        return;
    }
    let software = ver
        .as_ref()
        .and_then(|v| v.get("data"))
        .and_then(|d| d.get("software"))
        .and_then(Value::as_str)
        .map(ToOwned::to_owned);
    let info = do_cmd(&mut port, "INFO?", Duration::from_millis(2000))
        .ok()
        .flatten();
    let (model_name, pinned) = info.as_ref().map(model_card).unwrap_or((None, false));
    emit(json!({ "kind": "id", "software": software, "model": model_name, "pinned": pinned }));

    // Continuous invoke, with frames (result_only=0). The stream arrives as
    // type-1 INVOKE events; a type-0 ack may or may not precede it depending
    // on the SSCMA build — success is either signal (same as the browser and
    // we2.rs::invoke_event), so only an explicit error ack is fatal here.
    if let Err(e) = write_line(&mut port, "INVOKE=-1,0,0") {
        emit(json!({ "kind": "error", "message": e }));
        return;
    }
    emit(json!({ "kind": "log", "line": "→ streaming — frames stay on this computer." }));

    let mut scanner = sscma::FrameScanner::new();
    let mut pending: Option<Pending> = None;
    let mut byte = [0u8; 1];
    while !cancel.load(Ordering::Relaxed) {
        // One command in flight at a time; the rest wait in the channel.
        if pending.is_none() {
            if let Ok(cmd) = commands.try_recv() {
                match write_line(&mut port, &cmd.body) {
                    Ok(()) => {
                        pending = Some(Pending {
                            body: cmd.body,
                            reply: cmd.reply,
                            deadline: Instant::now() + Duration::from_millis(3000),
                        });
                    }
                    Err(e) => {
                        let _ = cmd.reply.send(Err(e));
                    }
                }
            }
        } else if pending
            .as_ref()
            .is_some_and(|p| Instant::now() > p.deadline)
        {
            if let Some(p) = pending.take() {
                let _ = p
                    .reply
                    .send(Err("the module didn't answer in time".to_string()));
            }
        }

        match port.read(&mut byte) {
            Ok(1) => {
                let Some(frame) = scanner.push(byte[0]) else {
                    continue;
                };
                let Ok(value) = serde_json::from_slice::<Value>(&frame) else {
                    continue;
                };
                let kind = value.get("type").and_then(Value::as_i64);
                let name = value.get("name").and_then(Value::as_str).unwrap_or("");
                if kind == Some(1) && name == "INVOKE" {
                    emit(json!({
                        "kind": "event",
                        "data": value.get("data").cloned().unwrap_or(Value::Null),
                    }));
                } else if kind == Some(0) {
                    let code = value.get("code").and_then(Value::as_i64).unwrap_or(0);
                    if name == "INVOKE" && code != 0 {
                        emit(json!({
                            "kind": "error",
                            "message": "the module answered AT but refused INVOKE — it likely has no model. Flash the model first, then reopen the bench."
                        }));
                        break;
                    }
                    let answers = pending
                        .as_ref()
                        .is_some_and(|p| sscma::reply_matches(name, &p.body));
                    if answers {
                        if let Some(p) = pending.take() {
                            let _ = p.reply.send(Ok(value));
                        }
                    }
                }
            }
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => {
                emit(json!({
                    "kind": "error",
                    "message": format!("the module went away mid-stream: {e}. Reconnect and reopen the bench.")
                }));
                break;
            }
        }
    }

    // Leave the module quiet — BREAK ends the continuous invoke.
    let _ = write_line(&mut port, "BREAK");
    std::thread::sleep(Duration::from_millis(150));
    emit(json!({ "kind": "stopped" }));
}

/// Cancel a bench worker and WAIT for it to finish — i.e. to send `BREAK` and
/// drop the serial port. Off the async runtime (the join is bounded: the read
/// loop wakes every 15 ms and the shutdown sleep is 150 ms).
async fn retire(control: BenchControl) {
    control.cancel.store(true, Ordering::Relaxed);
    let _ = tauri::async_runtime::spawn_blocking(move || {
        let _ = control.handle.join();
    })
    .await;
}

/// Start the live bench on the module's port, replacing (and fully retiring)
/// any previous bench first. Returns the new bench's session id — the frontend
/// uses it to ignore a previous worker's late events.
#[tauri::command]
pub async fn we2_bench_start(
    app: AppHandle,
    state: State<'_, We2BenchState>,
    port: String,
) -> Result<u64, String> {
    if port.is_empty() {
        return Err("no module port".into());
    }
    let old = {
        let mut active = state
            .0
            .lock()
            .map_err(|_| "bench state is unavailable".to_string())?;
        active.take()
    };
    if let Some(old) = old {
        // The old worker still owns the port until it exits — wait, or the
        // fresh open below races it and fails busy.
        retire(old).await;
    }
    let session = SESSION.fetch_add(1, Ordering::Relaxed) + 1;
    let cancel = Arc::new(AtomicBool::new(false));
    let (send, receive) = mpsc::channel();
    let thread_cancel = cancel.clone();
    let handle = std::thread::spawn(move || {
        bench_thread(app, port, session, thread_cancel, receive);
    });
    let mut active = state
        .0
        .lock()
        .map_err(|_| "bench state is unavailable".to_string())?;
    *active = Some(BenchControl {
        cancel,
        send,
        handle,
    });
    Ok(session)
}

/// Send one AT body (the bench dialect only) and return the module's reply —
/// the sliders' set-then-read-back path.
#[tauri::command]
pub async fn we2_bench_cmd(state: State<'_, We2BenchState>, body: String) -> Result<Value, String> {
    if !sscma::valid_cmd_body(&body) {
        return Err("that isn't a bench command".into());
    }
    let sender = {
        let active = state
            .0
            .lock()
            .map_err(|_| "bench state is unavailable".to_string())?;
        let control = active
            .as_ref()
            .ok_or_else(|| "the bench is not running".to_string())?;
        control.send.clone()
    };
    let (tx, rx) = mpsc::channel();
    sender
        .send(BenchCmd { body, reply: tx })
        .map_err(|_| "the bench has already stopped".to_string())?;
    let reply = tauri::async_runtime::spawn_blocking(move || {
        match rx.recv_timeout(Duration::from_millis(3500)) {
            Ok(result) => result,
            Err(_) => Err("the module didn't answer in time".to_string()),
        }
    })
    .await
    .map_err(|e| format!("bench task failed: {e}"))?;
    reply
}

/// Stop the bench and WAIT for the worker to send `AT+BREAK` and release the
/// port — so when this returns, an immediate restart or a module flash can
/// open the port without racing the dying worker.
#[tauri::command]
pub async fn we2_bench_stop(state: State<'_, We2BenchState>) -> Result<(), String> {
    let control = {
        let mut active = state
            .0
            .lock()
            .map_err(|_| "bench state is unavailable".to_string())?;
        active.take()
    };
    if let Some(control) = control {
        retire(control).await;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn card_reply(card: Value, nested: bool) -> Value {
        let b64 = base64::engine::general_purpose::STANDARD.encode(card.to_string());
        if nested {
            json!({ "type": 0, "name": "INFO", "code": 0, "data": { "info": b64 } })
        } else {
            json!({ "type": 0, "name": "INFO", "code": 0, "data": b64 })
        }
    }

    #[test]
    fn model_card_reads_both_reply_shapes() {
        let card = json!({ "name": "Person Detection", "classes": ["person"] });
        for nested in [false, true] {
            let (name, pinned) = model_card(&card_reply(card.clone(), nested));
            assert_eq!(name.as_deref(), Some("Person Detection"));
            assert!(pinned);
        }
    }

    #[test]
    fn model_card_without_person_class_is_not_pinned() {
        let card = json!({ "name": "Traffic", "classes": ["car", "bus"] });
        let (name, pinned) = model_card(&card_reply(card, false));
        assert_eq!(name.as_deref(), Some("Traffic"));
        assert!(!pinned);
    }

    #[test]
    fn model_card_tolerates_garbage() {
        assert_eq!(
            model_card(&json!({ "data": "not base64 !!!" })),
            (None, false)
        );
        assert_eq!(model_card(&json!({ "code": 0 })), (None, false));
    }
}
