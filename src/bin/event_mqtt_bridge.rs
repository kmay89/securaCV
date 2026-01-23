//! event_mqtt_bridge - publish exported events to a local MQTT broker.

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use std::io::IsTerminal;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpStream};
use std::path::PathBuf;
use std::time::Duration;
use witness_kernel::{ExportArtifact, ExportEvent};

#[path = "../ui.rs"]
mod ui;

const BRIDGE_NAME: &str = "event_mqtt_bridge";
const EVENTS_PATH: &str = "/events";

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    /// Loopback API address for witnessd.
    #[arg(long, env = "WITNESS_API_ADDR", default_value = "127.0.0.1:8799")]
    api_addr: String,
    /// Path to the capability token file.
    #[arg(long, env = "WITNESS_API_TOKEN_PATH")]
    api_token_path: Option<PathBuf>,
    /// Capability token value (overrides token path).
    #[arg(long, env = "WITNESS_API_TOKEN")]
    api_token: Option<String>,
    /// MQTT broker address (loopback-only).
    #[arg(long, env = "MQTT_BROKER_ADDR", default_value = "127.0.0.1:1883")]
    mqtt_broker_addr: String,
    /// MQTT topic to publish exported events to.
    #[arg(long, env = "MQTT_TOPIC", default_value = "witness/events")]
    mqtt_topic: String,
    /// MQTT client identifier.
    #[arg(long, env = "MQTT_CLIENT_ID", default_value = BRIDGE_NAME)]
    mqtt_client_id: String,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);

    let api_addr = parse_loopback_socket_addr(&args.api_addr)
        .with_context(|| "api addr must be loopback-only")?;
    let (broker_host, broker_port) = parse_loopback_host_port(&args.mqtt_broker_addr)?;
    let token = {
        let _stage = ui.stage("Load capability token");
        load_token(args.api_token_path, args.api_token)?
    };

    let artifact = {
        let _stage = ui.stage("Fetch export artifact");
        fetch_export_artifact(api_addr, &token)?
    };
    let events = flatten_export_events(&artifact);
    if events.is_empty() {
        log::info!("no events to publish");
        return Ok(());
    }

    {
        let _stage = ui.stage("Publish events");
        publish_events(
            &args.mqtt_client_id,
            &broker_host,
            broker_port,
            &args.mqtt_topic,
            &events,
        )?;
    }

    Ok(())
}

fn load_token(path: Option<PathBuf>, token: Option<String>) -> Result<String> {
    if let Some(token) = token {
        let trimmed = token.trim().to_string();
        if trimmed.is_empty() {
            return Err(anyhow!("WITNESS_API_TOKEN is empty"));
        }
        return Ok(trimmed);
    }
    let path =
        path.ok_or_else(|| anyhow!("WITNESS_API_TOKEN_PATH or WITNESS_API_TOKEN is required"))?;
    let contents = std::fs::read_to_string(&path)
        .with_context(|| format!("failed to read token file {}", path.display()))?;
    let token = contents.trim().to_string();
    if token.is_empty() {
        return Err(anyhow!("token file {} is empty", path.display()));
    }
    Ok(token)
}

fn fetch_export_artifact(addr: std::net::SocketAddr, token: &str) -> Result<ExportArtifact> {
    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_secs(2))?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;

    let request = format!(
        "GET {path} HTTP/1.1\r\nHost: {host}\r\nx-witness-token: {token}\r\nConnection: close\r\n\r\n",
        path = EVENTS_PATH,
        host = addr,
        token = token
    );
    stream.write_all(request.as_bytes())?;

    let mut response = Vec::new();
    stream.read_to_end(&mut response)?;
    let header_end = response
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or_else(|| anyhow!("invalid http response"))?;
    let (header, body) = response.split_at(header_end + 4);
    let header_text = String::from_utf8_lossy(header);
    let mut lines = header_text.lines();
    let status_line = lines.next().ok_or_else(|| anyhow!("missing status line"))?;
    let status_code = status_line
        .split_whitespace()
        .nth(1)
        .ok_or_else(|| anyhow!("missing status code"))?;
    if status_code != "200" {
        return Err(anyhow!("event api returned status {}", status_code));
    }

    let artifact: ExportArtifact =
        serde_json::from_slice(body).context("failed to parse /events response")?;
    Ok(artifact)
}

fn flatten_export_events(artifact: &ExportArtifact) -> Vec<ExportEvent> {
    let mut events = Vec::new();
    for batch in &artifact.batches {
        for bucket in &batch.buckets {
            for event in &bucket.events {
                events.push(event.clone());
            }
        }
    }
    events
}

fn publish_events(
    client_id: &str,
    broker_host: &str,
    broker_port: u16,
    topic: &str,
    events: &[ExportEvent],
) -> Result<()> {
    let addr = format!("{broker_host}:{broker_port}");
    let mut stream = TcpStream::connect(addr)?;
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    mqtt_connect(&mut stream, client_id)?;
    for event in events {
        let payload = serialize_event_payload(event)?;
        mqtt_publish(&mut stream, topic, &payload)?;
    }
    mqtt_disconnect(&mut stream)?;
    Ok(())
}

fn serialize_event_payload(event: &ExportEvent) -> Result<Vec<u8>> {
    serde_json::to_vec(event).context("serialize ExportEvent")
}

fn parse_loopback_socket_addr(value: &str) -> Result<std::net::SocketAddr> {
    if let Ok(addr) = value.parse::<std::net::SocketAddr>() {
        if addr.ip().is_loopback() {
            return Ok(addr);
        }
        return Err(anyhow!("address {} is not loopback", value));
    }
    if let Some(port) = value.strip_prefix("localhost:") {
        let port: u16 = port.parse().context("invalid port")?;
        return Ok(std::net::SocketAddr::new(
            IpAddr::from([127, 0, 0, 1]),
            port,
        ));
    }
    Err(anyhow!("unsupported api address {}", value))
}

fn parse_loopback_host_port(value: &str) -> Result<(String, u16)> {
    let value = value.strip_prefix("mqtt://").unwrap_or(value);
    if let Some((host, port)) = parse_host_port(value)? {
        if !is_loopback_host(host) {
            return Err(anyhow!(
                "broker host {} is not loopback; refusing off-host publish",
                host
            ));
        }
        return Ok((host.to_string(), port));
    }
    Err(anyhow!("invalid broker address {}", value))
}

fn parse_host_port(value: &str) -> Result<Option<(&str, u16)>> {
    if let Some(rest) = value.strip_prefix('[') {
        if let Some(end) = rest.find(']') {
            let host = &rest[..end];
            let port_part = &rest[end + 1..];
            let port = port_part
                .strip_prefix(':')
                .ok_or_else(|| anyhow!("missing port"))?;
            let port: u16 = port.parse().context("invalid port")?;
            return Ok(Some((host, port)));
        }
        return Err(anyhow!("invalid bracketed host"));
    }
    if let Some((host, port)) = value.rsplit_once(':') {
        let port: u16 = port.parse().context("invalid port")?;
        return Ok(Some((host, port)));
    }
    Ok(None)
}

fn is_loopback_host(host: &str) -> bool {
    if host.eq_ignore_ascii_case("localhost") {
        return true;
    }
    if let Ok(ip) = host.parse::<IpAddr>() {
        return ip.is_loopback();
    }
    false
}

fn mqtt_connect(stream: &mut TcpStream, client_id: &str) -> Result<()> {
    let mut variable = Vec::new();
    encode_string(&mut variable, "MQTT");
    variable.push(0x04);
    variable.push(0b0000_0010);
    variable.extend_from_slice(&5u16.to_be_bytes());

    let mut payload = Vec::new();
    encode_string(&mut payload, client_id);

    let remaining_len = variable.len() + payload.len();
    let mut packet = vec![0x10];
    encode_remaining_length(&mut packet, remaining_len);
    packet.extend_from_slice(&variable);
    packet.extend_from_slice(&payload);
    stream.write_all(&packet)?;

    let mut connack = [0u8; 4];
    stream.read_exact(&mut connack)?;
    if connack[0] != 0x20 || connack[1] != 0x02 {
        return Err(anyhow!("invalid connack from broker"));
    }
    if connack[3] != 0x00 {
        return Err(anyhow!("mqtt broker rejected connection"));
    }
    Ok(())
}

fn mqtt_publish(stream: &mut TcpStream, topic: &str, payload: &[u8]) -> Result<()> {
    let mut variable = Vec::new();
    encode_string(&mut variable, topic);
    let remaining_len = variable.len() + payload.len();

    let mut packet = vec![0x30];
    encode_remaining_length(&mut packet, remaining_len);
    packet.extend_from_slice(&variable);
    packet.extend_from_slice(payload);
    stream.write_all(&packet)?;
    Ok(())
}

fn mqtt_disconnect(stream: &mut TcpStream) -> Result<()> {
    stream.write_all(&[0xE0, 0x00])?;
    Ok(())
}

fn encode_string(out: &mut Vec<u8>, value: &str) {
    let len = value.len() as u16;
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(value.as_bytes());
}

fn encode_remaining_length(out: &mut Vec<u8>, mut value: usize) {
    loop {
        let mut encoded = (value % 128) as u8;
        value /= 128;
        if value > 0 {
            encoded |= 0x80;
        }
        out.push(encoded);
        if value == 0 {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use witness_kernel::{EventType, TimeBucket};

    #[test]
    fn serialize_payload_excludes_forbidden_fields() {
        let event = ExportEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 1_700_000_000,
                size_s: 600,
            },
            zone_id: "zone:front_boundary".to_string(),
            confidence: 0.9,
            kernel_version: "0.3.1".to_string(),
            ruleset_id: "ruleset:v0.1".to_string(),
            ruleset_hash: [0u8; 32],
        };

        let payload = serialize_event_payload(&event).expect("serialize payload");
        let value: serde_json::Value = serde_json::from_slice(&payload).expect("parse payload");

        let obj = value.as_object().expect("payload should be object");
        assert!(!obj.contains_key("correlation_token"));
        assert!(!obj.contains_key("created_at"));
        assert!(!obj.contains_key("event_id"));
    }

    #[test]
    fn broker_rejects_non_loopback_host() {
        let err = parse_loopback_host_port("mqtt://192.168.1.10:1883").unwrap_err();
        assert!(format!("{err}").contains("not loopback"));
    }

    #[test]
    fn broker_accepts_loopback_hosts() {
        let (host, port) = parse_loopback_host_port("127.0.0.1:1883").expect("loopback");
        assert_eq!(host, "127.0.0.1");
        assert_eq!(port, 1883);

        let (host, port) = parse_loopback_host_port("localhost:1883").expect("loopback");
        assert_eq!(host, "localhost");
        assert_eq!(port, 1883);

        let (host, port) = parse_loopback_host_port("[::1]:1883").expect("loopback");
        assert_eq!(host, "::1");
        assert_eq!(port, 1883);
    }
}
