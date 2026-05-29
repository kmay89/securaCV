//! HTTP webhook ingress adapter.
//!
//! The lowest-friction way to wire a sensor into the witness mesh: any device or script that can
//! make an HTTP `POST` becomes a witness source, with **no MQTT broker required**. A council or
//! hospital can register a sensor with a single `curl`:
//!
//! ```text
//! curl -X POST http://127.0.0.1:8800/sensors/garage/acoustic -d '{"confidence":0.8}'
//! ```
//!
//! The request *path* plays the role of the routing "topic"; the body is parsed exactly like the
//! generic MQTT sensor adapter (shared [`route_message`]). A tiny std-only HTTP/1.1 listener
//! ([`serve`]) accepts POST/PUT requests and forwards `(path, body)` into the adapter's channel;
//! the adapter drains and parses them in [`poll`](WebhookAdapter::poll), optionally inside the
//! seccomp sandbox. The listener is intentionally separate from the trusted read-only Event API,
//! so untrusted inbound bodies never touch that surface.
//!
//! Gated behind the `adapter-webhook` feature (which implies `adapter-mqtt-sensor` for the shared
//! routing types).

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::thread;
use std::time::Duration;

use anyhow::Result;

use crate::adapter::contract::{AdapterDescriptor, Claim, ClaimKind};
use crate::adapter::mqtt_sensor::{parse_messages, route_message, SensorMessage, SensorRoute};
use crate::adapter::SensorAdapter;
use crate::EventType;

/// Maximum accepted request body, in bytes. Webhook payloads are tiny status messages.
const MAX_BODY: usize = 64 * 1024;
/// Maximum bytes read for the request line or any single header line (anti-OOM).
const MAX_LINE: u64 = 1024;
/// Maximum total bytes across all header lines (anti-OOM).
const MAX_HEADER_BYTES: usize = 8 * 1024;
/// Per-connection read timeout so a slow/stuck client cannot tie up its handler forever.
const READ_TIMEOUT: Duration = Duration::from_secs(10);

static WEBHOOK_DESCRIPTOR: AdapterDescriptor = AdapterDescriptor {
    id: "webhook_adapter",
    allowed_claim_kinds: &[
        ClaimKind::LargeObjectBoundaryCrossing,
        ClaimKind::SmallObjectBoundaryCrossing,
        ClaimKind::AcousticImpulseInZone,
        ClaimKind::PresenceInRestrictedZone,
        ClaimKind::VehiclePresenceAfterHours,
        ClaimKind::ContactStateChange,
        ClaimKind::ObjectRemovedFromZone,
    ],
    allowed_event_types: &[
        EventType::BoundaryCrossingObjectLarge,
        EventType::BoundaryCrossingObjectSmall,
        EventType::AcousticImpulseInZone,
        EventType::PresenceInRestrictedZone,
        EventType::VehiclePresenceAfterHours,
        EventType::ContactStateChange,
        EventType::ObjectRemovedFromZone,
    ],
    requested_capabilities: &[],
};

/// HTTP webhook adapter. Construct with [`WebhookAdapter::new`], spawn [`serve`] with the returned
/// [`Sender`] on a background thread, and register the adapter with the host.
pub struct WebhookAdapter {
    rx: Receiver<SensorMessage>,
    routes: Vec<SensorRoute>,
    sandbox: bool,
}

impl WebhookAdapter {
    /// Build the adapter from a routing table (route `topic` is matched against the request path);
    /// returns the feeding [`Sender`].
    pub fn new(routes: Vec<SensorRoute>) -> (Self, Sender<SensorMessage>) {
        let (tx, rx) = channel();
        (
            Self {
                rx,
                routes,
                sandbox: false,
            },
            tx,
        )
    }

    /// Opt in to running payload parsing inside the seccomp sandbox (requires the
    /// `adapter-sandbox` feature; no effect otherwise).
    pub fn with_sandbox(mut self, enabled: bool) -> Self {
        self.sandbox = enabled;
        self
    }

    /// Pure transform: map one request `(path, body)` to at most one claim.
    pub fn message_to_claim(&self, path: &str, body: &[u8]) -> Option<Claim> {
        route_message(&self.routes, path, body)
    }
}

impl SensorAdapter for WebhookAdapter {
    fn name(&self) -> &'static str {
        "webhook_adapter"
    }

    fn descriptor(&self) -> &'static AdapterDescriptor {
        &WEBHOOK_DESCRIPTOR
    }

    fn poll(&mut self) -> Result<Vec<Claim>> {
        let mut msgs = Vec::new();
        while let Ok(msg) = self.rx.try_recv() {
            msgs.push(msg);
        }
        if msgs.is_empty() {
            return Ok(Vec::new());
        }
        parse_messages(&self.routes, &msgs, self.sandbox)
    }
}

/// Bind a TCP listener for the webhook (separate from [`serve_listener`] so callers/tests can
/// learn the bound address, e.g. when binding to port 0).
pub fn bind(addr: impl ToSocketAddrs) -> Result<TcpListener> {
    let listener = TcpListener::bind(addr)?;
    if let Ok(local) = listener.local_addr() {
        log::info!("webhook listener bound to {local}");
    }
    Ok(listener)
}

/// Run a blocking HTTP listener that forwards each `POST`/`PUT` request as `(path, body)` into
/// `tx`. Intended to be spawned on its own thread. Returns only on a fatal bind error.
pub fn serve(addr: impl ToSocketAddrs, tx: Sender<SensorMessage>) -> Result<()> {
    serve_listener(bind(addr)?, tx)
}

/// Serve an already-bound listener (see [`bind`]).
///
/// Each connection is handled on its own thread so a slow or stuck client cannot block delivery
/// of webhooks from other sensors (avoids head-of-line blocking / a trivial DoS). Per-connection
/// reads are bounded (line/header/body limits + a read timeout).
pub fn serve_listener(listener: TcpListener, tx: Sender<SensorMessage>) -> Result<()> {
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let tx = tx.clone();
                thread::spawn(move || {
                    if let Err(e) = handle_connection(s, &tx) {
                        log::debug!("webhook connection error: {e}");
                    }
                });
            }
            Err(e) => log::warn!("webhook accept error: {e}"),
        }
    }
    Ok(())
}

fn handle_connection(stream: TcpStream, tx: &Sender<SensorMessage>) -> Result<()> {
    stream.set_read_timeout(Some(READ_TIMEOUT))?;
    let mut reader = BufReader::new(stream);

    // Request line: "METHOD SP PATH SP HTTP/1.1". Bounded read to prevent OOM from a client that
    // never sends a newline.
    let mut request_line = String::new();
    if reader
        .by_ref()
        .take(MAX_LINE)
        .read_line(&mut request_line)?
        == 0
    {
        return Ok(()); // client closed before sending anything
    }
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let raw_path = parts.next().unwrap_or("/").to_string();

    // Headers — we only need Content-Length. Each line and the total header size are bounded.
    let mut content_length: usize = 0;
    let mut headers_read: usize = 0;
    loop {
        let mut line = String::new();
        let n = reader.by_ref().take(MAX_LINE).read_line(&mut line)?;
        if n == 0 || line == "\r\n" || line == "\n" {
            break;
        }
        headers_read += n;
        if headers_read > MAX_HEADER_BYTES {
            return write_response(reader.get_mut(), 400, "headers too large");
        }
        let lower = line.to_lowercase();
        if let Some(value) = lower.strip_prefix("content-length:") {
            content_length = value.trim().parse().unwrap_or(0);
        }
    }

    if method != "POST" && method != "PUT" {
        return write_response(reader.get_mut(), 405, "method not allowed");
    }
    if content_length > MAX_BODY {
        return write_response(reader.get_mut(), 413, "payload too large");
    }

    let mut body = vec![0u8; content_length];
    reader.read_exact(&mut body)?;

    // Strip any query string so routing keys on the bare path.
    let path = raw_path.split('?').next().unwrap_or(&raw_path).to_string();

    // A full channel/dropped receiver is not fatal for the request.
    let _ = tx.send((path, body));
    write_response(reader.get_mut(), 204, "")
}

fn write_response(stream: &mut TcpStream, code: u16, body: &str) -> Result<()> {
    let reason = match code {
        204 => "No Content",
        400 => "Bad Request",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        _ => "OK",
    };
    let response = format!(
        "HTTP/1.1 {code} {reason}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    stream.write_all(response.as_bytes())?;
    stream.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn route() -> SensorRoute {
        SensorRoute::new(
            "/sensors/garage/acoustic",
            ClaimKind::AcousticImpulseInZone,
            "garage",
        )
    }

    #[test]
    fn post_body_maps_to_claim_by_path() {
        let (adapter, _tx) = WebhookAdapter::new(vec![route()]);
        let claim = adapter
            .message_to_claim("/sensors/garage/acoustic", br#"{"confidence":0.7}"#)
            .expect("claim");
        assert_eq!(claim.kind, ClaimKind::AcousticImpulseInZone);
        assert_eq!(claim.zone_label, "garage");
    }

    #[test]
    fn poll_drains_fed_requests() {
        let (mut adapter, tx) = WebhookAdapter::new(vec![route()]);
        tx.send((
            "/sensors/garage/acoustic".to_string(),
            br#"{"confidence":0.7}"#.to_vec(),
        ))
        .unwrap();
        let claims = adapter.poll().expect("poll");
        assert_eq!(claims.len(), 1);
        assert!(adapter.poll().expect("poll2").is_empty());
    }

    #[test]
    fn unrouted_path_yields_nothing() {
        let (adapter, _tx) = WebhookAdapter::new(vec![route()]);
        assert!(adapter.message_to_claim("/unknown", b"{}").is_none());
    }
}
