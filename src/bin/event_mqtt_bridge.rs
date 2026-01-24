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

/// MQTT connection with protocol support.
struct MqttConnection {
    stream: TcpStream,
    packet_id: u16,
}

impl MqttConnection {
    fn new(stream: TcpStream) -> Self {
        Self {
            stream,
            packet_id: 0,
        }
    }

    fn next_packet_id(&mut self) -> u16 {
        self.packet_id = self.packet_id.wrapping_add(1);
        if self.packet_id == 0 {
            self.packet_id = 1;
        }
        self.packet_id
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

    if !args.allow_remote_mqtt {
        validate_loopback_addr(&args.mqtt_broker_addr)?;
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

    if args.daemon {
        run_daemon(
            &args,
            api_addr,
            &token,
            &device_id,
            &device_info,
            &availability_topic,
            &ui,
        )
    } else {
        run_oneshot(
            &args,
            api_addr,
            &token,
            &device_id,
            &device_info,
            &availability_topic,
            &ui,
        )
    }
}

fn run_oneshot(
    args: &Args,
    api_addr: std::net::SocketAddr,
    token: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    availability_topic: &str,
    ui: &ui::Ui,
) -> Result<()> {
    let artifact = {
        let _stage = ui.stage("Fetch export artifact");
        fetch_export_artifact(api_addr, token)?
    };

    let events = flatten_export_events(&artifact);
    if events.is_empty() {
        log::info!("No events to publish");
        return Ok(());
    }

    let mut conn = {
        let _stage = ui.stage("Connect to MQTT broker");
        connect_mqtt(
            &args.mqtt_broker_addr,
            &args.mqtt_client_id,
            args.mqtt_username.as_deref(),
            args.mqtt_password.as_deref(),
            availability_topic,
        )?
    };

    // Publish availability online
    mqtt_publish_qos1(
        &mut conn,
        availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;

    if !args.no_discovery {
        let _stage = ui.stage("Publish HA discovery configs");
        publish_discovery_configs(
            &mut conn,
            &args.ha_discovery_prefix,
            &args.mqtt_topic_prefix,
            availability_topic,
            device_id,
            device_info,
            &events,
        )?;
    }

    {
        let _stage = ui.stage("Publish events");
        publish_events(&mut conn, &args.mqtt_topic_prefix, &events)?;
    }

    mqtt_disconnect(&mut conn)?;
    log::info!("Published {} events", events.len());
    Ok(())
}

fn run_daemon(
    args: &Args,
    api_addr: std::net::SocketAddr,
    token: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    availability_topic: &str,
    ui: &ui::Ui,
) -> Result<()> {
    log::info!(
        "Starting daemon mode (poll interval: {}s)",
        args.poll_interval
    );

    let mut conn = {
        let _stage = ui.stage("Connect to MQTT broker");
        connect_mqtt(
            &args.mqtt_broker_addr,
            &args.mqtt_client_id,
            args.mqtt_username.as_deref(),
            args.mqtt_password.as_deref(),
            availability_topic,
        )?
    };

    // Publish availability online (retained)
    mqtt_publish_qos1(
        &mut conn,
        availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;
    log::info!("Published online status to {}", availability_topic);

    let mut discovered_zones: HashSet<String> = HashSet::new();
    let mut zone_states: HashMap<String, ZoneState> = HashMap::new();
    let mut last_bucket_seen: Option<TimeBucket> = None;

    loop {
        match fetch_export_artifact(api_addr, token) {
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
                    if !args.no_discovery {
                        for event in &new_events {
                            let zone = extract_zone_name(&event.zone_id);
                            if !discovered_zones.contains(&zone) {
                                publish_zone_discovery(
                                    &mut conn,
                                    &args.ha_discovery_prefix,
                                    &args.mqtt_topic_prefix,
                                    availability_topic,
                                    device_id,
                                    device_info,
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
                        publish_single_event(&mut conn, &args.mqtt_topic_prefix, event, &zone)?;

                        // Publish zone count
                        let count_topic = format!("{}/zone/{}/count", args.mqtt_topic_prefix, zone);
                        mqtt_publish_qos1(
                            &mut conn,
                            &count_topic,
                            state.event_count.to_string().as_bytes(),
                            true,
                        )?;

                        // Trigger motion sensor
                        let motion_topic =
                            format!("{}/zone/{}/motion", args.mqtt_topic_prefix, zone);
                        mqtt_publish_qos1(&mut conn, &motion_topic, b"ON", false)?;
                    }

                    // Update last event state
                    if let Some(last) = new_events.last() {
                        let state_topic = format!("{}/last_event", args.mqtt_topic_prefix);
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
                        mqtt_publish_qos1(&mut conn, &state_topic, &json, true)?;

                        last_bucket_seen = Some(last.time_bucket);
                    }

                    log::info!("Published {} new events", new_events.len());
                }
            }
            Err(e) => {
                log::warn!("Failed to fetch events: {}", e);
            }
        }

        std::thread::sleep(Duration::from_secs(args.poll_interval));
    }
}

fn publish_discovery_configs(
    conn: &mut MqttConnection,
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
            conn,
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
    mqtt_publish_qos1(conn, &config_topic, &config_json, true)?;

    log::info!(
        "Published HA discovery for {} zones + last_event sensor",
        zones.len()
    );
    Ok(())
}

fn publish_zone_discovery(
    conn: &mut MqttConnection,
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
    mqtt_publish_qos1(conn, &config_topic, &config_json, true)?;

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
    mqtt_publish_qos1(conn, &config_topic, &config_json, true)?;

    log::debug!("Published HA discovery for zone: {}", zone);
    Ok(())
}

fn publish_events(
    conn: &mut MqttConnection,
    topic_prefix: &str,
    events: &[ExportEvent],
) -> Result<()> {
    let mut zone_counts: HashMap<String, u64> = HashMap::new();

    for event in events {
        let zone = extract_zone_name(&event.zone_id);
        *zone_counts.entry(zone.clone()).or_default() += 1;

        publish_single_event(conn, topic_prefix, event, &zone)?;
    }

    // Publish final counts (retained)
    for (zone, count) in &zone_counts {
        let count_topic = format!("{}/zone/{}/count", topic_prefix, zone);
        mqtt_publish_qos1(conn, &count_topic, count.to_string().as_bytes(), true)?;

        // Trigger motion
        let motion_topic = format!("{}/zone/{}/motion", topic_prefix, zone);
        mqtt_publish_qos1(conn, &motion_topic, b"ON", false)?;
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
        mqtt_publish_qos1(conn, &state_topic, &json, true)?;
    }

    Ok(())
}

fn publish_single_event(
    conn: &mut MqttConnection,
    topic_prefix: &str,
    event: &ExportEvent,
    zone: &str,
) -> Result<()> {
    // Publish to zone-specific topic
    let topic = format!("{}/zone/{}/event", topic_prefix, zone);
    let payload = serde_json::to_vec(event)?;
    mqtt_publish_qos1(conn, &topic, &payload, false)?;

    // Publish to firehose topic
    let firehose_topic = format!("{}/events", topic_prefix);
    mqtt_publish_qos1(conn, &firehose_topic, &payload, false)?;

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

fn validate_loopback_addr(addr: &str) -> Result<()> {
    let addr = addr.strip_prefix("mqtt://").unwrap_or(addr);
    if let Some((host, _)) = addr.rsplit_once(':') {
        if host == "localhost" || host == "127.0.0.1" || host == "::1" {
            return Ok(());
        }
        if let Ok(ip) = host.parse::<std::net::IpAddr>() {
            if ip.is_loopback() {
                return Ok(());
            }
        }
    }
    Err(anyhow!(
        "MQTT broker must be loopback for security: {} (use --allow-remote-mqtt to override)",
        addr
    ))
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

fn connect_mqtt(
    addr: &str,
    client_id: &str,
    username: Option<&str>,
    password: Option<&str>,
    will_topic: &str,
) -> Result<MqttConnection> {
    let addr = addr.strip_prefix("mqtt://").unwrap_or(addr);
    let mut stream = TcpStream::connect(addr).context("connect to MQTT broker")?;
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;
    stream.set_write_timeout(Some(Duration::from_secs(10)))?;

    // Build CONNECT packet with LWT
    let mut variable = Vec::new();
    encode_string(&mut variable, "MQTT");
    variable.push(0x04); // Protocol level (MQTT 3.1.1)

    // Connect flags: clean session + will + will retain + optional auth
    let mut connect_flags = 0x02u8; // Clean session
    connect_flags |= 0x04; // Will flag
    connect_flags |= 0x20; // Will retain
                           // Will QoS = 1 (bits 3-4 = 01)
    connect_flags |= 0x08;
    if username.is_some() {
        connect_flags |= 0x80; // Username flag
    }
    if password.is_some() {
        connect_flags |= 0x40; // Password flag
    }
    variable.push(connect_flags);
    variable.extend_from_slice(&60u16.to_be_bytes()); // Keep alive (60s)

    // Payload: client_id, will topic, will message, optional auth
    let mut payload = Vec::new();
    encode_string(&mut payload, client_id);
    encode_string(&mut payload, will_topic);
    encode_string(&mut payload, PAYLOAD_OFFLINE);
    if let Some(user) = username {
        encode_string(&mut payload, user);
    }
    if let Some(pass) = password {
        encode_string(&mut payload, pass);
    }

    let remaining = variable.len() + payload.len();
    let mut packet = vec![0x10];
    encode_remaining_length(&mut packet, remaining);
    packet.extend_from_slice(&variable);
    packet.extend_from_slice(&payload);
    stream.write_all(&packet)?;

    // Read CONNACK
    let mut connack = [0u8; 4];
    stream.read_exact(&mut connack)?;
    if connack[0] != 0x20 || connack[3] != 0x00 {
        let reason = match connack[3] {
            0x01 => "unacceptable protocol version",
            0x02 => "identifier rejected",
            0x03 => "server unavailable",
            0x04 => "bad username or password",
            0x05 => "not authorized",
            _ => "unknown error",
        };
        return Err(anyhow!("MQTT connection rejected: {}", reason));
    }

    log::info!(
        "Connected to MQTT broker (LWT: {}, auth: {})",
        will_topic,
        username.is_some()
    );
    Ok(MqttConnection::new(stream))
}

fn mqtt_publish_qos1(
    conn: &mut MqttConnection,
    topic: &str,
    payload: &[u8],
    retain: bool,
) -> Result<()> {
    let packet_id = conn.next_packet_id();

    let mut variable = Vec::new();
    encode_string(&mut variable, topic);
    variable.extend_from_slice(&packet_id.to_be_bytes());

    let remaining_len = variable.len() + payload.len();

    // PUBLISH with QoS 1 (0x32) + retain flag if set
    let mut header = 0x32u8; // PUBLISH + QoS 1
    if retain {
        header |= 0x01;
    }

    let mut packet = vec![header];
    encode_remaining_length(&mut packet, remaining_len);
    packet.extend_from_slice(&variable);
    packet.extend_from_slice(payload);
    conn.stream.write_all(&packet)?;

    // Wait for PUBACK
    let mut puback = [0u8; 4];
    conn.stream.read_exact(&mut puback)?;
    if puback[0] != 0x40 {
        return Err(anyhow!("expected PUBACK, got 0x{:02x}", puback[0]));
    }
    let ack_id = u16::from_be_bytes([puback[2], puback[3]]);
    if ack_id != packet_id {
        return Err(anyhow!(
            "PUBACK packet ID mismatch: expected {}, got {}",
            packet_id,
            ack_id
        ));
    }

    Ok(())
}

fn mqtt_disconnect(conn: &mut MqttConnection) -> Result<()> {
    conn.stream.write_all(&[0xE0, 0x00])?;
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
        let err = validate_loopback_addr("192.168.1.10:1883").unwrap_err();
        assert!(format!("{err}").contains("loopback"));
    }

    #[test]
    fn broker_accepts_loopback_hosts() {
        assert!(validate_loopback_addr("127.0.0.1:1883").is_ok());
        assert!(validate_loopback_addr("localhost:1883").is_ok());
        assert!(validate_loopback_addr("::1:1883").is_ok());
    }
}
