//! Optional read-only HTTP stats endpoint for an adapter host.
//!
//! Exposes the host's per-adapter counters ([`AdapterHost::stats_snapshot`](crate::adapter::host::AdapterHost::stats_snapshot))
//! so operators — Home Assistant's `rest` sensor, or a Prometheus scraper — can see each sensor's
//! health at a glance. It serves **operational counters only** (polls/sealed/rejected + a coarse
//! timestamp), never event content, so it does not widen the privacy surface. Read-only; answers
//! any method:
//!
//! - `GET /healthz` → `200 ok` (liveness)
//! - `GET /metrics` → `200` Prometheus text exposition format
//! - any other path → `200` with the latest stats JSON
//!
//! The snapshot is held behind a shared [`SharedStats`] that the host loop refreshes each cycle, so
//! this server never touches the kernel.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;

use crate::adapter::host::AdapterStats;

/// Latest per-adapter snapshot, shared between the host loop (writer) and the stats server.
pub type SharedStats = Arc<Mutex<BTreeMap<String, AdapterStats>>>;

/// Create an empty shared-stats handle.
pub fn shared_stats() -> SharedStats {
    Arc::new(Mutex::new(BTreeMap::new()))
}

const READ_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_LINE: u64 = 1024;

/// Counter metrics (monotonic) and their help text.
const COUNTERS: &[(&str, &str)] = &[
    ("polls", "Times the adapter was polled."),
    ("claims_emitted", "Claims produced by the adapter."),
    ("claims_sealed", "Claims sealed into the log."),
    (
        "claims_filtered",
        "Claims dropped by the host (confidence/dedup).",
    ),
    ("claims_rejected", "Claims rejected by the kernel gates."),
    ("poll_errors", "Poll calls that returned an error."),
];

/// Bind and serve the stats endpoint forever (intended to run on its own thread).
pub fn serve_stats(addr: impl ToSocketAddrs, stats: SharedStats) -> Result<()> {
    let listener = TcpListener::bind(addr)?;
    if let Ok(local) = listener.local_addr() {
        log::info!("adapter stats endpoint bound to {local}");
    }
    // Bounded like the webhook listener: a connection flood must not spawn
    // unbounded threads. Past the cap a connection is dropped unanswered — a
    // scraper simply retries — instead of costing a thread for READ_TIMEOUT.
    const MAX_LIVE_CONNECTIONS: usize = 32;
    let live = Arc::new(AtomicUsize::new(0));
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if live.fetch_add(1, Ordering::AcqRel) >= MAX_LIVE_CONNECTIONS {
                    live.fetch_sub(1, Ordering::AcqRel);
                    log::debug!("stats endpoint at capacity; dropping a connection");
                    drop(s);
                    continue;
                }
                // One thread per connection so a slow/idle client cannot block other readers
                // (e.g. Home Assistant) for up to READ_TIMEOUT.
                let stats = Arc::clone(&stats);
                let live = Arc::clone(&live);
                std::thread::spawn(move || {
                    if let Err(e) = handle(s, &stats) {
                        log::debug!("stats connection error: {e}");
                    }
                    live.fetch_sub(1, Ordering::AcqRel);
                });
            }
            Err(e) => log::warn!("stats accept error: {e}"),
        }
    }
    Ok(())
}

fn snapshot(stats: &SharedStats) -> BTreeMap<String, AdapterStats> {
    stats.lock().map(|g| g.clone()).unwrap_or_default()
}

fn handle(stream: TcpStream, stats: &SharedStats) -> Result<()> {
    stream.set_read_timeout(Some(READ_TIMEOUT))?;
    stream.set_write_timeout(Some(READ_TIMEOUT))?;
    let mut reader = BufReader::new(stream);
    let mut request_line = String::new();
    reader
        .by_ref()
        .take(MAX_LINE)
        .read_line(&mut request_line)?;
    let path = request_line.split_whitespace().nth(1).unwrap_or("/");

    let (content_type, body) = if path.starts_with("/healthz") {
        ("text/plain", "ok".to_string())
    } else if path.starts_with("/metrics") {
        (
            "text/plain; version=0.0.4",
            render_prometheus(&snapshot(stats)),
        )
    } else {
        let json = serde_json::to_string(&snapshot(stats)).unwrap_or_else(|_| "{}".to_string());
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

/// Escape a Prometheus label value (`\`, `"`, newline).
fn escape_label(s: &str) -> String {
    s.replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}

/// Render the snapshot in Prometheus text exposition format.
fn render_prometheus(stats: &BTreeMap<String, AdapterStats>) -> String {
    let mut out = String::new();
    let field = |s: &AdapterStats, name: &str| -> u64 {
        match name {
            "polls" => s.polls,
            "claims_emitted" => s.claims_emitted,
            "claims_sealed" => s.claims_sealed,
            "claims_filtered" => s.claims_filtered,
            "claims_rejected" => s.claims_rejected,
            "poll_errors" => s.poll_errors,
            _ => 0,
        }
    };
    for (counter, help) in COUNTERS {
        let metric = format!("securacv_adapter_{counter}");
        out.push_str(&format!(
            "# HELP {metric} {help}\n# TYPE {metric} counter\n"
        ));
        for (adapter, s) in stats {
            out.push_str(&format!(
                "{metric}{{adapter=\"{}\"}} {}\n",
                escape_label(adapter),
                field(s, counter)
            ));
        }
    }
    let metric = "securacv_adapter_last_seal_epoch_seconds";
    out.push_str(&format!(
        "# HELP {metric} Unix time of the most recently sealed event (0 if none).\n# TYPE {metric} gauge\n"
    ));
    for (adapter, s) in stats {
        out.push_str(&format!(
            "{metric}{{adapter=\"{}\"}} {}\n",
            escape_label(adapter),
            s.last_seal_epoch_s
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpStream;

    fn sample() -> SharedStats {
        let stats = shared_stats();
        stats.lock().unwrap().insert(
            "webhook_adapter#0".to_string(),
            AdapterStats {
                polls: 3,
                claims_sealed: 2,
                ..Default::default()
            },
        );
        stats
    }

    fn get(addr: std::net::SocketAddr, path: &str) -> String {
        let mut s = TcpStream::connect(addr).unwrap();
        s.write_all(format!("GET {path} HTTP/1.1\r\nConnection: close\r\n\r\n").as_bytes())
            .unwrap();
        let mut resp = String::new();
        s.read_to_string(&mut resp).unwrap();
        resp
    }

    #[test]
    fn serves_json_metrics_and_healthz() {
        let stats = sample();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let stats_clone = Arc::clone(&stats);
        std::thread::spawn(move || {
            for stream in listener.incoming().flatten() {
                let _ = handle(stream, &stats_clone);
            }
        });

        let json = get(addr, "/stats");
        assert!(json.contains("200 OK"));
        assert!(json.contains(r#""polls":3"#));

        let metrics = get(addr, "/metrics");
        assert!(metrics.contains("# TYPE securacv_adapter_claims_sealed counter"));
        assert!(
            metrics.contains(r#"securacv_adapter_claims_sealed{adapter="webhook_adapter#0"} 2"#)
        );
        assert!(metrics.contains("securacv_adapter_last_seal_epoch_seconds"));

        let health = get(addr, "/healthz");
        assert!(health.trim_end().ends_with("ok"));
    }

    #[test]
    fn prometheus_escapes_label_values() {
        assert_eq!(escape_label("a\"b\\c"), "a\\\"b\\\\c");
    }
}
