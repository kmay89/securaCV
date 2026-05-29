//! Optional read-only HTTP stats endpoint for an adapter host.
//!
//! Exposes the host's per-adapter counters ([`AdapterHost::stats_snapshot`]) as JSON so operators
//! — and Home Assistant's built-in `rest` sensor platform — can see each sensor's health at a
//! glance. It serves **operational counters only** (polls/sealed/rejected + a coarse timestamp),
//! never event content, so it does not widen the privacy surface. It is read-only and answers any
//! method:
//!
//! - `GET /healthz` → `200 ok` (liveness)
//! - any other path → `200` with the latest stats JSON
//!
//! The served JSON is held behind a shared [`SharedStats`] that the host loop refreshes each
//! cycle, so this server never touches the kernel.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;

/// Latest stats JSON, shared between the host loop (writer) and the stats server (reader).
pub type SharedStats = Arc<Mutex<String>>;

/// Create an empty shared-stats handle.
pub fn shared_stats() -> SharedStats {
    Arc::new(Mutex::new("{}".to_string()))
}

const READ_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_LINE: u64 = 1024;

/// Bind and serve the stats endpoint forever (intended to run on its own thread).
pub fn serve_stats(addr: impl ToSocketAddrs, stats: SharedStats) -> Result<()> {
    let listener = TcpListener::bind(addr)?;
    if let Ok(local) = listener.local_addr() {
        log::info!("adapter stats endpoint bound to {local}");
    }
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                // One thread per connection so a slow/idle client cannot block other readers
                // (e.g. Home Assistant) for up to READ_TIMEOUT.
                let stats = Arc::clone(&stats);
                std::thread::spawn(move || {
                    if let Err(e) = handle(s, &stats) {
                        log::debug!("stats connection error: {e}");
                    }
                });
            }
            Err(e) => log::warn!("stats accept error: {e}"),
        }
    }
    Ok(())
}

fn handle(stream: TcpStream, stats: &SharedStats) -> Result<()> {
    stream.set_read_timeout(Some(READ_TIMEOUT))?;
    let mut reader = BufReader::new(stream);
    let mut request_line = String::new();
    reader
        .by_ref()
        .take(MAX_LINE)
        .read_line(&mut request_line)?;
    let path = request_line.split_whitespace().nth(1).unwrap_or("/");

    let (content_type, body) = if path.starts_with("/healthz") {
        ("text/plain", "ok".to_string())
    } else {
        let json = stats.lock().map(|g| g.clone()).unwrap_or_default();
        ("application/json", json)
    };

    let response = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    let stream = reader.get_mut();
    stream.write_all(response.as_bytes())?;
    stream.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpStream;

    #[test]
    fn serves_stats_json_and_healthz() {
        let stats = shared_stats();
        *stats.lock().unwrap() = r#"{"webhook_adapter":{"polls":3}}"#.to_string();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let stats_clone = Arc::clone(&stats);
        std::thread::spawn(move || {
            for stream in listener.incoming().flatten() {
                let _ = handle(stream, &stats_clone);
            }
        });

        let mut s = TcpStream::connect(addr).unwrap();
        s.write_all(b"GET /stats HTTP/1.1\r\nConnection: close\r\n\r\n")
            .unwrap();
        let mut resp = String::new();
        s.read_to_string(&mut resp).unwrap();
        assert!(resp.contains("200 OK"));
        assert!(resp.contains(r#""polls":3"#));

        let mut s = TcpStream::connect(addr).unwrap();
        s.write_all(b"GET /healthz HTTP/1.1\r\nConnection: close\r\n\r\n")
            .unwrap();
        let mut resp = String::new();
        s.read_to_string(&mut resp).unwrap();
        assert!(resp.trim_end().ends_with("ok"));
    }
}
