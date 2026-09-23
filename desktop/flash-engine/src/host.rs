//! The seam between the engine and an app shell.
//!
//! The engine never touches Tauri. What it needs from an app is exactly two
//! things — a place to send the events its frontend listens for, and a way to
//! run the bundled `espflash` — plus, with defaults every app uses, the two
//! HTTPS fetches a release flash makes. Each desktop app implements
//! [`FlashHost`] once over its `AppHandle`; the unit tests implement it over
//! an in-memory recorder, which is how the whole verify-provision-write
//! pipeline ([`crate::flash`]) is tested without a board, a webview or a
//! network.

use serde::Serialize;
use serde_json::Value;
use std::future::Future;

/// Somewhere to send an event the frontend listens for. Each app forwards it
/// to Tauri's `Emitter::emit`, so the event name and the JSON payload are
/// exactly what the engine hands over.
pub trait Emit {
    /// Deliver `payload` under `event`. Best-effort, like Tauri's own emit:
    /// a frontend that is not listening loses nothing the flash depends on.
    fn emit<P: Serialize + Clone>(&self, event: &str, payload: P);
}

/// Everything the flash pipeline needs from the app it runs in.
pub trait FlashHost: Emit + Sync {
    /// The User-Agent each release fetch carries ("SecuraCV-Flasher",
    /// "SecuraCV-Lab"), so a request in a server log names the app.
    const USER_AGENT: &'static str;

    /// Run the bundled `espflash` with `args` to completion, handing every
    /// stdout/stderr chunk to `on_output` as it arrives, and answer its exit
    /// code (-1 when the process reported none). `Err` is a spawn failure
    /// (see [`crate::sidecar::spawn_error`]).
    ///
    /// The app owns the spawn so it can record the sidecar's PID for as long
    /// as it runs — espflash must never be left holding a board's serial port
    /// after the app that started it has gone.
    fn espflash<F>(
        &self,
        args: Vec<String>,
        on_output: F,
    ) -> impl Future<Output = Result<i32, String>> + Send
    where
        F: FnMut(&[u8]) + Send;

    /// GET a release manifest as JSON. The default is the real fetch
    /// ([`crate::net::get_manifest`]); tests substitute a canned answer.
    fn get_manifest(&self, url: &str) -> impl Future<Output = Result<Value, String>> + Send {
        crate::net::get_manifest(url.to_string(), Self::USER_AGENT)
    }

    /// Download a release asset, calling `on_chunk(done, total)` after each
    /// chunk, and answer the bytes plus the total the progress was measured
    /// against. The default is the real download ([`crate::net::download`]).
    fn download<F>(
        &self,
        url: &str,
        expected_size: u64,
        on_chunk: F,
    ) -> impl Future<Output = Result<(Vec<u8>, u64), String>> + Send
    where
        F: FnMut(usize, u64) + Send,
    {
        crate::net::download(url.to_string(), expected_size, Self::USER_AGENT, on_chunk)
    }
}

/// Run espflash to completion, collecting stdout+stderr. Used for the short
/// `board-info` probe (and the health check's quiet region reads) where the
/// whole answer is wanted, not a live stream.
pub async fn run_capture<H: FlashHost>(
    host: &H,
    args: Vec<String>,
) -> Result<(i32, String), String> {
    let mut buf = String::new();
    let code = host
        .espflash(args, |bytes| buf.push_str(&String::from_utf8_lossy(bytes)))
        .await?;
    Ok((code, buf))
}

/// Run espflash, streaming each non-empty line over `event`, and return its
/// exit code.
pub async fn run_streaming<H: FlashHost>(
    host: &H,
    args: Vec<String>,
    event: &'static str,
) -> Result<i32, String> {
    host.espflash(args, |bytes| {
        let text = String::from_utf8_lossy(bytes);
        for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
            host.emit(event, line.to_string());
        }
    })
    .await
}

/// [`run_streaming`], keeping espflash's last words. They stream to the
/// console for the user to read, but the FAILURE needs them too: the
/// frontends classify errors by their text ("permission denied", "resource
/// busy", "no serial data"), and an error saying only "exited with code 1"
/// classifies as `unknown` — indistinguishable from a bad cable. That makes a
/// busy port look like a transport fault and get retried down the whole baud
/// ladder, re-erasing and re-downloading each time. A few lines of tail is the
/// difference between a diagnosis and a shrug.
pub async fn run_streaming_with_tail<H: FlashHost>(
    host: &H,
    args: Vec<String>,
    event: &'static str,
) -> Result<(i32, Vec<String>), String> {
    let mut tail: Vec<String> = Vec::new();
    let code = host
        .espflash(args, |bytes| {
            let text = String::from_utf8_lossy(bytes);
            for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                host.emit(event, line.to_string());
                tail.push(line.trim().to_string());
                if tail.len() > 8 {
                    tail.remove(0);
                }
            }
        })
        .await?;
    Ok((code, tail))
}
