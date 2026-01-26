//! event_mqtt_bridge - Publish PWK events to MQTT with Home Assistant Discovery.
//!
//! This bridge enables seamless Home Assistant integration:
//! 1. Publishes HA MQTT Discovery config for auto-entity creation
//! 2. Publishes events with QoS 1 for reliable delivery
//! 3. Uses Last Will Testament (LWT) for availability tracking
//! 4. Supports both one-shot and daemon modes
//!
//! Entities created in Home Assistant:
//! - sensor.pwk_<zone>_events: Event count per zone
//! - binary_sensor.pwk_<zone>_motion: Motion state per zone
//! - sensor.pwk_last_event: Most recent event details

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::v5::{mqttbytes::QoS, Client, Connection, Event, MqttOptions};
use rumqttc::Transport;
use serde::Serialize;
use std::collections::{HashMap, HashSet};
use std::io::IsTerminal;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpStream};
use std::path::PathBuf;
use std::time::Duration;
use witness_kernel::{ExportArtifact, ExportEvent, TimeBucket};

#[path = "../ui.rs"]
mod ui;

const BRIDGE_NAME: &str = "event_mqtt_bridge";
const EVENTS_PATH: &str = "/events";
const DEFAULT_DISCOVERY_PREFIX: &str = "homeassistant";
const DEFAULT_STATE_PREFIX: &str = "witness";
const AVAILABILITY_TOPIC_SUFFIX: &str = "status";
const PAYLOAD_ONLINE: &str = "online";
const PAYLOAD_OFFLINE: &str = "offline";

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "Publish PWK events to MQTT with Home Assistant Discovery"
)]
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

    /// MQTT broker address.
    #[arg(long, env = "MQTT_BROKER_ADDR", default_value = "127.0.0.1:1883")]
    mqtt_broker_addr: String,

    /// Allow non-loopback MQTT connections.
    /// Use in trusted environments like Home Assistant containers.
    #[arg(long, env = "ALLOW_REMOTE_MQTT")]
    allow_remote_mqtt: bool,

    /// MQTT username for authentication.
    #[arg(long, env = "MQTT_USERNAME")]
    mqtt_username: Option<String>,

    /// MQTT password for authentication.
    #[arg(long, env = "MQTT_PASSWORD")]
    mqtt_password: Option<String>,

    /// Enable TLS for MQTT (required for mqtts:// brokers).
    #[arg(long, env = "MQTT_USE_TLS")]
    mqtt_use_tls: bool,

    /// Path to a PEM-encoded CA certificate to trust for MQTT TLS.
    #[arg(long, env = "MQTT_TLS_CA_PATH")]
    mqtt_tls_ca_path: Option<PathBuf>,

    /// Path to a PEM-encoded client certificate for MQTT TLS.
    #[arg(long, env = "MQTT_TLS_CLIENT_CERT_PATH")]
    mqtt_tls_client_cert_path: Option<PathBuf>,

    /// Path to a PEM-encoded client private key for MQTT TLS.
    #[arg(long, env = "MQTT_TLS_CLIENT_KEY_PATH")]
    mqtt_tls_client_key_path: Option<PathBuf>,

    /// Home Assistant MQTT discovery prefix.
    #[arg(long, env = "HA_DISCOVERY_PREFIX", default_value = DEFAULT_DISCOVERY_PREFIX)]
    ha_discovery_prefix: String,

    /// MQTT topic prefix for state updates.
    #[arg(long, env = "MQTT_TOPIC_PREFIX", default_value = DEFAULT_STATE_PREFIX)]
    mqtt_topic_prefix: String,

    /// MQTT client identifier.
    #[arg(long, env = "MQTT_CLIENT_ID", default_value = BRIDGE_NAME)]
    mqtt_client_id: String,

    /// Device identifier for Home Assistant (derived from device_key_seed if not set).
    #[arg(long, env = "HA_DEVICE_ID")]
    ha_device_id: Option<String>,

    /// Run as daemon, polling for new events periodically.
    #[arg(long, env = "DAEMON_MODE")]
    daemon: bool,

    /// Poll interval in seconds (daemon mode only).
    #[arg(long, env = "POLL_INTERVAL", default_value_t = 30)]
    poll_interval: u64,

    /// Disable Home Assistant discovery (publish raw events only).
    #[arg(long, env = "NO_DISCOVERY")]
    no_discovery: bool,

    /// UI mode for stderr progress (auto|plain|pretty).
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

/// Home Assistant MQTT Discovery config for a sensor.
#[derive(Serialize)]
struct HaSensorConfig {
    name: String,
    unique_id: String,
    state_topic: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    json_attributes_topic: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    value_template: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    unit_of_measurement: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    device_class: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    state_class: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    icon: Option<String>,
    availability_topic: String,
    payload_available: String,
    payload_not_available: String,
    device: HaDeviceInfo,
}

/// Home Assistant MQTT Discovery config for a binary sensor.
#[derive(Serialize)]
struct HaBinarySensorConfig {
    name: String,
    unique_id: String,
    state_topic: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    json_attributes_topic: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    value_template: Option<String>,
    device_class: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    off_delay: Option<u32>,
    availability_topic: String,
    payload_available: String,
    payload_not_available: String,
    device: HaDeviceInfo,
}

/// Home Assistant device info for entity grouping.
#[derive(Clone, Serialize)]
struct HaDeviceInfo {
    identifiers: Vec<String>,
    name: String,
    manufacturer: String,
    model: String,
    sw_version: String,
}

/// Event state payload for Home Assistant.
#[derive(Serialize)]
struct EventStatePayload {
    event_type: String,
    zone_id: String,
    time_bucket_start: u64,
    time_bucket_size: u32,
    confidence: f32,
    timestamp: u64, // When we published this
}

/// Zone state for tracking event counts.
#[derive(Default)]
struct ZoneState {
    event_count: u64,
    last_event_time: u64,
}

struct MqttRuntime {
    client: Client,
    connection_handle: Option<std::thread::JoinHandle<()>>,
}

impl MqttRuntime {
    fn new(client: Client, mut connection: Connection) -> Self {
        let handle = std::thread::spawn(move || {
            for event in connection.iter() {
                match event {
                    Ok(Event::Incoming(_)) | Ok(Event::Outgoing(_)) => {}
                    Err(e) => {
                        log::warn!("MQTT connection error: {}", e);
                        break;
                    }
                }
            }
        });

        Self {
            client,
            connection_handle: Some(handle),
        }
    }

    fn disconnect(mut self) -> Result<()> {
        self.client.disconnect()?;
        if let Some(handle) = self.connection_handle.take() {
            let _ = handle.join();
        }
        Ok(())
    }
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);

    // Validate addresses
    let api_addr = parse_loopback_socket_addr(&args.api_addr)
        .with_context(|| "api addr must be loopback-only")?;
    let mqtt_endpoint = parse_mqtt_endpoint(&args.mqtt_broker_addr, args.mqtt_use_tls)?;
    let tls_materials = load_tls_materials(
        args.mqtt_tls_ca_path.as_ref(),
        args.mqtt_tls_client_cert_path.as_ref(),
        args.mqtt_tls_client_key_path.as_ref(),
    )?;

    if !args.allow_remote_mqtt {
        validate_loopback_addr(&mqtt_endpoint, &args.mqtt_broker_addr)?;
    } else {
        log::warn!("Remote MQTT enabled - ensure broker is in a trusted network");
    }

    let token = {
        let _stage = ui.stage("Load capability token");
        load_token(args.api_token_path.clone(), args.api_token.clone())?
    };

    // Generate device ID from environment or use provided
    let device_id = args.ha_device_id.clone().unwrap_or_else(|| {
        std::env::var("DEVICE_KEY_SEED")
            .map(|s| format!("pwk_{}", &s[..8]))
            .unwrap_or_else(|_| "pwk_default".to_string())
    });

    let device_info = HaDeviceInfo {
        identifiers: vec![device_id.clone()],
        name: "Privacy Witness Kernel".to_string(),
        manufacturer: "securaCV".to_string(),
        model: "PWK".to_string(),
        sw_version: env!("CARGO_PKG_VERSION").to_string(),
    };

    let availability_topic = format!("{}/{}", args.mqtt_topic_prefix, AVAILABILITY_TOPIC_SUFFIX);

    let run_ctx = RunContext {
        args: &args,
        api_addr,
        mqtt_endpoint: &mqtt_endpoint,
        tls_materials: &tls_materials,
        token: &token,
        device_id: &device_id,
        device_info: &device_info,
        availability_topic: &availability_topic,
        ui: &ui,
    };

    if args.daemon {
        run_daemon(&run_ctx)
    } else {
        run_oneshot(&run_ctx)
    }
}

struct RunContext<'a> {
    args: &'a Args,
    api_addr: std::net::SocketAddr,
    mqtt_endpoint: &'a MqttEndpoint,
    tls_materials: &'a TlsMaterials,
    token: &'a str,
    device_id: &'a str,
    device_info: &'a HaDeviceInfo,
    availability_topic: &'a str,
    ui: &'a ui::Ui,
}

fn run_oneshot(ctx: &RunContext<'_>) -> Result<()> {
    let artifact = {
        let _stage = ctx.ui.stage("Fetch export artifact");
        fetch_export_artifact(ctx.api_addr, ctx.token)?
    };

    let events = flatten_export_events(&artifact);
    if events.is_empty() {
        log::info!("No events to publish");
        return Ok(());
    }

    let conn = {
        let _stage = ctx.ui.stage("Connect to MQTT broker");
        connect_mqtt(
            ctx.mqtt_endpoint,
            ctx.tls_materials,
            &ctx.args.mqtt_client_id,
            ctx.args.mqtt_username.as_deref(),
            ctx.args.mqtt_password.as_deref(),
            ctx.availability_topic,
        )?
    };

    // Publish availability online
    mqtt_publish_qos1(
        &conn.client,
        ctx.availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;

    if !ctx.args.no_discovery {
        let _stage = ctx.ui.stage("Publish HA discovery configs");
        publish_discovery_configs(
            &conn.client,
            &ctx.args.ha_discovery_prefix,
            &ctx.args.mqtt_topic_prefix,
            ctx.availability_topic,
            ctx.device_id,
            ctx.device_info,
            &events,
        )?;
    }

    {
        let _stage = ctx.ui.stage("Publish events");
        publish_events(&conn.client, &ctx.args.mqtt_topic_prefix, &events)?;
    }

    conn.disconnect()?;
    log::info!("Published {} events", events.len());
    Ok(())
}

fn run_daemon(ctx: &RunContext<'_>) -> Result<()> {
    log::info!(
        "Starting daemon mode (poll interval: {}s)",
        ctx.args.poll_interval
    );

    let conn = {
        let _stage = ctx.ui.stage("Connect to MQTT broker");
        connect_mqtt(
            ctx.mqtt_endpoint,
            ctx.tls_materials,
            &ctx.args.mqtt_client_id,
            ctx.args.mqtt_username.as_deref(),
            ctx.args.mqtt_password.as_deref(),
            ctx.availability_topic,
        )?
    };

    // Publish availability online (retained)
    mqtt_publish_qos1(
        &conn.client,
        ctx.availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;
    log::info!("Published online status to {}", ctx.availability_topic);

    let mut discovered_zones: HashSet<String> = HashSet::new();
    let mut zone_states: HashMap<String, ZoneState> = HashMap::new();
    let mut last_bucket_seen: Option<TimeBucket> = None;

    loop {
        match fetch_export_artifact(ctx.api_addr, ctx.token) {
            Ok(artifact) => {
                let events = flatten_export_events(&artifact);

                // Filter to only new events
                let new_events: Vec<_> = events
                    .iter()
                    .filter(|e| {
                        last_bucket_seen
                            .as_ref()
                            .map(|lb| {
                                e.time_bucket.start_epoch_s > lb.start_epoch_s
                                    || (e.time_bucket.start_epoch_s == lb.start_epoch_s
                                        && e.time_bucket.size_s >= lb.size_s)
                            })
                            .unwrap_or(true)
                    })
                    .collect();

                if !new_events.is_empty() {
                    // Discover new zones
                    if !ctx.args.no_discovery {
                        for event in &new_events {
                            let zone = extract_zone_name(&event.zone_id);
                            if !discovered_zones.contains(&zone) {
                                publish_zone_discovery(
                                    &conn.client,
                                    &ctx.args.ha_discovery_prefix,
                                    &ctx.args.mqtt_topic_prefix,
                                    ctx.availability_topic,
                                    ctx.device_id,
                                    ctx.device_info,
                                    &zone,
                                )?;
                                discovered_zones.insert(zone);
                            }
                        }
                    }

                    // Publish events and update zone states
                    for event in &new_events {
                        let zone = extract_zone_name(&event.zone_id);

                        // Update zone state
                        let state = zone_states.entry(zone.clone()).or_default();
                        state.event_count += 1;
                        state.last_event_time = std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .map(|d| d.as_secs())
                            .unwrap_or(0);

                        // Publish event
                        publish_single_event(
                            &conn.client,
                            &ctx.args.mqtt_topic_prefix,
                            event,
                            &zone,
                        )?;

                        // Publish zone count
                        let count_topic =
                            format!("{}/zone/{}/count", ctx.args.mqtt_topic_prefix, zone);
                        mqtt_publish_qos1(
                            &conn.client,
                            &count_topic,
                            state.event_count.to_string().as_bytes(),
                            true,
                        )?;

                        // Trigger motion sensor
                        let motion_topic =
                            format!("{}/zone/{}/motion", ctx.args.mqtt_topic_prefix, zone);
                        mqtt_publish_qos1(&conn.client, &motion_topic, b"ON", false)?;
                    }

                    // Update last event state
                    if let Some(last) = new_events.last() {
                        let state_topic = format!("{}/last_event", ctx.args.mqtt_topic_prefix);
                        let payload = EventStatePayload {
                            event_type: format!("{:?}", last.event_type),
                            zone_id: last.zone_id.clone(),
                            time_bucket_start: last.time_bucket.start_epoch_s,
                            time_bucket_size: last.time_bucket.size_s,
                            confidence: last.confidence,
                            timestamp: std::time::SystemTime::now()
                                .duration_since(std::time::UNIX_EPOCH)
                                .map(|d| d.as_secs())
                                .unwrap_or(0),
                        };
                        let json = serde_json::to_vec(&payload)?;
                        mqtt_publish_qos1(&conn.client, &state_topic, &json, true)?;

                        last_bucket_seen = Some(last.time_bucket);
                    }

                    log::info!("Published {} new events", new_events.len());
                }
            }
            Err(e) => {
                log::warn!("Failed to fetch events: {}", e);
            }
        }

        std::thread::sleep(Duration::from_secs(ctx.args.poll_interval));
    }
}

fn publish_discovery_configs(
    client: &Client,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    events: &[ExportEvent],
) -> Result<()> {
    // Collect unique zones
    let zones: HashSet<String> = events
        .iter()
        .map(|e| extract_zone_name(&e.zone_id))
        .collect();

    // Publish discovery for each zone
    for zone in &zones {
        publish_zone_discovery(
            client,
            discovery_prefix,
            state_prefix,
            availability_topic,
            device_id,
            device_info,
            zone,
        )?;
    }

    // Publish last_event sensor discovery
    let last_event_config = HaSensorConfig {
        name: "PWK Last Event".to_string(),
        unique_id: format!("{}_last_event", device_id),
        state_topic: format!("{}/last_event", state_prefix),
        json_attributes_topic: Some(format!("{}/last_event", state_prefix)),
        value_template: Some("{{ value_json.event_type }}".to_string()),
        unit_of_measurement: None,
        device_class: None,
        state_class: None,
        icon: Some("mdi:motion-sensor".to_string()),
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };

    let config_topic = format!(
        "{}/sensor/{}/last_event/config",
        discovery_prefix, device_id
    );
    let config_json = serde_json::to_vec(&last_event_config)?;
    mqtt_publish_qos1(client, &config_topic, &config_json, true)?;

    log::info!(
        "Published HA discovery for {} zones + last_event sensor",
        zones.len()
    );
    Ok(())
}

fn publish_zone_discovery(
    client: &Client,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zone: &str,
) -> Result<()> {
    let zone_clean = sanitize_for_id(zone);

    // Event count sensor
    let count_config = HaSensorConfig {
        name: format!("PWK {} Events", zone),
        unique_id: format!("{}_{}_events", device_id, zone_clean),
        state_topic: format!("{}/zone/{}/count", state_prefix, zone),
        json_attributes_topic: None,
        value_template: None,
        unit_of_measurement: Some("events".to_string()),
        device_class: None,
        state_class: Some("total_increasing".to_string()),
        icon: Some("mdi:counter".to_string()),
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };

    let config_topic = format!(
        "{}/sensor/{}/{}_events/config",
        discovery_prefix, device_id, zone_clean
    );
    let config_json = serde_json::to_vec(&count_config)?;
    mqtt_publish_qos1(client, &config_topic, &config_json, true)?;

    // Motion binary sensor
    let motion_config = HaBinarySensorConfig {
        name: format!("PWK {} Motion", zone),
        unique_id: format!("{}_{}_motion", device_id, zone_clean),
        state_topic: format!("{}/zone/{}/motion", state_prefix, zone),
        json_attributes_topic: None,
        value_template: None,
        device_class: "motion".to_string(),
        off_delay: Some(600), // Auto-off after 10 minutes (matches time bucket)
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };

    let config_topic = format!(
        "{}/binary_sensor/{}/{}_motion/config",
        discovery_prefix, device_id, zone_clean
    );
    let config_json = serde_json::to_vec(&motion_config)?;
    mqtt_publish_qos1(client, &config_topic, &config_json, true)?;

    log::debug!("Published HA discovery for zone: {}", zone);
    Ok(())
}

fn publish_events(client: &Client, topic_prefix: &str, events: &[ExportEvent]) -> Result<()> {
    let mut zone_counts: HashMap<String, u64> = HashMap::new();

    for event in events {
        let zone = extract_zone_name(&event.zone_id);
        *zone_counts.entry(zone.clone()).or_default() += 1;

        publish_single_event(client, topic_prefix, event, &zone)?;
    }

    // Publish final counts (retained)
    for (zone, count) in &zone_counts {
        let count_topic = format!("{}/zone/{}/count", topic_prefix, zone);
        mqtt_publish_qos1(client, &count_topic, count.to_string().as_bytes(), true)?;

        // Trigger motion
        let motion_topic = format!("{}/zone/{}/motion", topic_prefix, zone);
        mqtt_publish_qos1(client, &motion_topic, b"ON", false)?;
    }

    // Publish last event (retained)
    if let Some(last) = events.last() {
        let state_topic = format!("{}/last_event", topic_prefix);
        let payload = EventStatePayload {
            event_type: format!("{:?}", last.event_type),
            zone_id: last.zone_id.clone(),
            time_bucket_start: last.time_bucket.start_epoch_s,
            time_bucket_size: last.time_bucket.size_s,
            confidence: last.confidence,
            timestamp: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
        };
        let json = serde_json::to_vec(&payload)?;
        mqtt_publish_qos1(client, &state_topic, &json, true)?;
    }

    Ok(())
}

fn publish_single_event(
    client: &Client,
    topic_prefix: &str,
    event: &ExportEvent,
    zone: &str,
) -> Result<()> {
    // Publish to zone-specific topic
    let topic = format!("{}/zone/{}/event", topic_prefix, zone);
    let payload = serde_json::to_vec(event)?;
    mqtt_publish_qos1(client, &topic, &payload, false)?;

    // Publish to firehose topic
    let firehose_topic = format!("{}/events", topic_prefix);
    mqtt_publish_qos1(client, &firehose_topic, &payload, false)?;

    Ok(())
}

fn extract_zone_name(zone_id: &str) -> String {
    zone_id.strip_prefix("zone:").unwrap_or(zone_id).to_string()
}

fn sanitize_for_id(s: &str) -> String {
    s.chars()
        .map(|c| if c.is_alphanumeric() { c } else { '_' })
        .collect()
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
    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_secs(5))?;
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;

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

#[derive(Clone, Debug)]
struct MqttEndpoint {
    host: String,
    port: u16,
    use_tls: bool,
}

#[derive(Clone, Debug, Default)]
struct TlsMaterials {
    ca: Option<Vec<u8>>,
    client_auth: Option<(Vec<u8>, Vec<u8>)>,
}

fn parse_mqtt_endpoint(addr: &str, tls_override: bool) -> Result<MqttEndpoint> {
    let mut use_tls = tls_override;
    let mut remainder = addr.trim();

    if let Some((scheme, rest)) = remainder.split_once("://") {
        match scheme {
            "mqtt" | "tcp" => {}
            "mqtts" | "ssl" => use_tls = true,
            other => return Err(anyhow!("unsupported MQTT scheme: {}", other)),
        }
        remainder = rest;
    }

    let (host, port) = split_host_port(remainder)?;
    Ok(MqttEndpoint {
        host,
        port,
        use_tls,
    })
}

fn split_host_port(addr: &str) -> Result<(String, u16)> {
    if let Some(rest) = addr.strip_prefix('[') {
        let (host, rest) = rest
            .split_once(']')
            .ok_or_else(|| anyhow!("invalid MQTT address: {}", addr))?;
        let port = rest
            .strip_prefix(':')
            .ok_or_else(|| anyhow!("missing MQTT port in {}", addr))?;
        let port: u16 = port.parse().context("invalid MQTT port")?;
        return Ok((host.to_string(), port));
    }

    let (host, port) = addr
        .rsplit_once(':')
        .ok_or_else(|| anyhow!("missing MQTT port in {}", addr))?;
    let port: u16 = port.parse().context("invalid MQTT port")?;
    Ok((host.to_string(), port))
}

fn validate_loopback_addr(endpoint: &MqttEndpoint, original: &str) -> Result<()> {
    let host = endpoint.host.as_str();
    if host == "localhost" || host == "127.0.0.1" || host == "::1" {
        return Ok(());
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        if ip.is_loopback() {
            return Ok(());
        }
    }
    Err(anyhow!(
        "MQTT broker must be loopback for security: {} (use --allow-remote-mqtt to override)",
        original
    ))
}

fn load_tls_materials(
    ca_path: Option<&PathBuf>,
    client_cert_path: Option<&PathBuf>,
    client_key_path: Option<&PathBuf>,
) -> Result<TlsMaterials> {
    let ca = match ca_path {
        Some(path) => Some(
            std::fs::read(path)
                .with_context(|| format!("failed to read MQTT TLS CA {}", path.display()))?,
        ),
        None => None,
    };

    let client_auth = match (client_cert_path, client_key_path) {
        (Some(cert_path), Some(key_path)) => {
            let cert = std::fs::read(cert_path).with_context(|| {
                format!(
                    "failed to read MQTT TLS client cert {}",
                    cert_path.display()
                )
            })?;
            let key = std::fs::read(key_path).with_context(|| {
                format!("failed to read MQTT TLS client key {}", key_path.display())
            })?;
            Some((cert, key))
        }
        (None, None) => None,
        _ => {
            return Err(anyhow!(
                "MQTT TLS client cert and key must be provided together"
            ))
        }
    };

    Ok(TlsMaterials { ca, client_auth })
}

fn build_transport(endpoint: &MqttEndpoint, tls: &TlsMaterials) -> Result<Transport> {
    if !endpoint.use_tls {
        if tls.ca.is_some() || tls.client_auth.is_some() {
            return Err(anyhow!(
                "MQTT TLS materials provided but TLS is disabled (use --mqtt-use-tls or mqtts://)"
            ));
        }
        return Ok(Transport::tcp());
    }

    if tls.ca.is_none() && tls.client_auth.is_none() {
        return Ok(Transport::tls_with_default_config());
    }

    let ca = tls.ca.clone().ok_or_else(|| {
        anyhow!("MQTT TLS CA certificate is required when providing client certificates")
    })?;
    Ok(Transport::tls(ca, tls.client_auth.clone(), None))
}

fn connect_mqtt(
    endpoint: &MqttEndpoint,
    tls: &TlsMaterials,
    client_id: &str,
    username: Option<&str>,
    password: Option<&str>,
    will_topic: &str,
) -> Result<MqttRuntime> {
    let mut options = MqttOptions::new(client_id, &endpoint.host, endpoint.port);
    options.set_keep_alive(Duration::from_secs(60));
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default());
    }
    let will = rumqttc::v5::mqttbytes::v5::LastWill::new(
        will_topic,
        PAYLOAD_OFFLINE.as_bytes().to_vec(),
        QoS::AtLeastOnce,
        true,
        None,
    );
    options.set_last_will(will);
    options.set_transport(build_transport(endpoint, tls)?);

    let (client, connection) = Client::new(options, 10);
    log::info!(
        "Connected to MQTT broker (TLS: {}, auth: {})",
        endpoint.use_tls,
        username.is_some()
    );
    Ok(MqttRuntime::new(client, connection))
}

fn mqtt_publish_qos1(client: &Client, topic: &str, payload: &[u8], retain: bool) -> Result<()> {
    client.publish(topic, QoS::AtLeastOnce, retain, payload.to_vec())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serialize_event_state_payload() {
        let payload = EventStatePayload {
            event_type: "BoundaryCrossingObjectLarge".to_string(),
            zone_id: "zone:front_door".to_string(),
            time_bucket_start: 1_700_000_000,
            time_bucket_size: 600,
            confidence: 0.9,
            timestamp: 1_700_000_100,
        };

        let json = serde_json::to_string(&payload).expect("serialize");
        assert!(json.contains("BoundaryCrossingObjectLarge"));
        assert!(json.contains("front_door"));
    }

    #[test]
    fn extract_zone_name_strips_prefix() {
        assert_eq!(extract_zone_name("zone:front_door"), "front_door");
        assert_eq!(extract_zone_name("driveway"), "driveway");
    }

    #[test]
    fn sanitize_for_id_replaces_special_chars() {
        assert_eq!(sanitize_for_id("front-door"), "front_door");
        assert_eq!(sanitize_for_id("zone:test"), "zone_test");
        assert_eq!(sanitize_for_id("camera_1"), "camera_1");
    }

    #[test]
    fn ha_sensor_config_serializes_correctly() {
        let device = HaDeviceInfo {
            identifiers: vec!["pwk_test".to_string()],
            name: "PWK Test".to_string(),
            manufacturer: "securaCV".to_string(),
            model: "PWK".to_string(),
            sw_version: "0.3.1".to_string(),
        };

        let config = HaSensorConfig {
            name: "Test Sensor".to_string(),
            unique_id: "pwk_test_sensor".to_string(),
            state_topic: "witness/test".to_string(),
            json_attributes_topic: None,
            value_template: None,
            unit_of_measurement: Some("events".to_string()),
            device_class: None,
            state_class: Some("total_increasing".to_string()),
            icon: Some("mdi:counter".to_string()),
            availability_topic: "witness/status".to_string(),
            payload_available: "online".to_string(),
            payload_not_available: "offline".to_string(),
            device,
        };

        let json = serde_json::to_string(&config).expect("serialize");
        assert!(json.contains("unique_id"));
        assert!(json.contains("state_topic"));
        assert!(json.contains("availability_topic"));
        assert!(json.contains("device"));
    }

    #[test]
    fn broker_rejects_non_loopback_without_flag() {
        let endpoint = parse_mqtt_endpoint("192.168.1.10:1883", false).expect("endpoint");
        let err = validate_loopback_addr(&endpoint, "192.168.1.10:1883").unwrap_err();
        assert!(format!("{err}").contains("loopback"));
    }

    #[test]
    fn broker_accepts_loopback_hosts() {
        let endpoint = parse_mqtt_endpoint("127.0.0.1:1883", false).expect("endpoint");
        assert!(validate_loopback_addr(&endpoint, "127.0.0.1:1883").is_ok());
        let endpoint = parse_mqtt_endpoint("localhost:1883", false).expect("endpoint");
        assert!(validate_loopback_addr(&endpoint, "localhost:1883").is_ok());
        let endpoint = parse_mqtt_endpoint("::1:1883", false).expect("endpoint");
        assert!(validate_loopback_addr(&endpoint, "::1:1883").is_ok());
    }
}
