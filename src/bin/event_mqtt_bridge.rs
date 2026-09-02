//! event_mqtt_bridge - Publish PWK events to MQTT with Home Assistant Discovery.
//!
//! This bridge enables seamless Home Assistant integration:
//! 1. Publishes HA MQTT Discovery config for auto-entity creation
//! 2. Publishes events with QoS 1 for reliable delivery
//! 3. Uses Last Will Testament (LWT) for availability tracking
//! 4. Supports both one-shot and daemon modes
//!
//! Entities created in Home Assistant:
//! - `sensor.pwk_<zone>_events`: Event count per zone
//! - `binary_sensor.pwk_<zone>_motion`: Motion state per zone
//! - `sensor.pwk_last_event`: Most recent event details
//! - `sensor.pwk_daily_digest`: Rolling 24h summary (daemon mode)
//! - `binary_sensor.pwk_chain_problem`: Sealed-log integrity (daemon mode)
//! - `button.pwk_verify_now`: One-click verification (daemon mode)

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::{Client, Connection, Event, Incoming, MqttOptions, QoS};
use serde::Serialize;
use std::collections::{HashMap, HashSet};
use std::io::IsTerminal;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpStream};
use std::path::PathBuf;
use std::sync::mpsc;
use std::time::{Duration, Instant};
use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
use witness_kernel::{ExportArtifact, ExportEvent, TimeBucket};

#[path = "../ui.rs"]
mod ui;

const BRIDGE_NAME: &str = "event_mqtt_bridge";
const EVENTS_PATH: &str = "/events";
const DIGEST_PATH: &str = "/digest";
const VERIFY_PATH: &str = "/verify";
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

    /// TLS backend: 'classic' (default) or 'hybrid_pq' (post-quantum).
    /// hybrid_pq requires the pqc-tls feature and a PQ-capable MQTT broker.
    #[arg(long, env = "MQTT_TLS_BACKEND", default_value = "classic")]
    mqtt_tls_backend: String,

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

    /// Interval between automatic full sealed-log verifications in daemon
    /// mode (seconds; 0 disables). One verification also runs at daemon
    /// start so the chain-integrity sensor is populated immediately.
    #[arg(long, env = "VERIFY_INTERVAL_SECS", default_value_t = 86_400)]
    verify_interval_secs: u64,

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

/// Home Assistant MQTT Discovery config for a button.
#[derive(Serialize)]
struct HaButtonConfig {
    name: String,
    unique_id: String,
    command_topic: String,
    payload_press: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    icon: Option<String>,
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

/// Capability token source. The witness API rotates its token every
/// 10-minute bucket (rewriting the token file), so daemon-mode consumers
/// must re-read the file before each request rather than caching the value
/// loaded at startup.
struct TokenSource {
    path: Option<PathBuf>,
    fixed: Option<String>,
}

impl TokenSource {
    fn current(&self) -> Result<String> {
        load_token(self.path.clone(), self.fixed.clone())
    }
}

/// Event state payload for Home Assistant.
#[derive(Serialize)]
struct EventStatePayload {
    event_type: String,
    zone_id: String,
    time_bucket_start: u64,
    time_bucket_size: u32,
    confidence: f32,
    #[serde(skip_serializing_if = "Option::is_none")]
    attestation: Option<String>,
    published_bucket_start: u64,
    published_bucket_size: u32,
}

/// Zone state for tracking event counts.
/// Command publishes forwarded to the daemon poll loop as (topic, payload).
type CommandTx = mpsc::Sender<(String, Vec<u8>)>;

/// Daemon-mode eventloop wiring. Bundled so the reconnect handler can, on
/// every ConnAck, both re-subscribe the command filter (clean-start drops
/// subscriptions across reconnects) and re-assert the retained availability
/// `online` — the broker published the retained LWT `offline` when the link
/// dropped, and `run_daemon` publishes `online` only once at startup, so
/// without this HA would show the device permanently unavailable after the
/// first reconnect.
struct DaemonWiring {
    commands: CommandTx,
    cmd_filter: String,
    availability_topic: String,
}

#[derive(Default)]
struct ZoneState {
    event_count: u64,
    last_event_time: u64,
}

struct MqttRuntime {
    client: Client,
    connection_handle: Option<std::thread::JoinHandle<()>>,
    shutdown: std::sync::Arc<std::sync::atomic::AtomicBool>,
    /// Set by the eventloop thread: true on ConnAck, false on connection
    /// error. The daemon poll loop reads this to skip its fetch+publish
    /// cycle while the broker is down, so a blocking publish into the full
    /// bounded request queue can't wedge the loop (docs/strategy/12 FR-4).
    connected: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

impl MqttRuntime {
    /// Drive the MQTT event loop on a background thread. When `incoming` is
    /// set, received publishes (e.g. on the subscribed `<prefix>/cmd/#`
    /// command topics) are forwarded as `(topic, payload)` to the daemon's
    /// poll loop; everything else is drained as before.
    ///
    /// Reconnect supervision (docs/strategy/12, finding K3): rumqttc's
    /// automatic reconnect lives *inside* this iterator — continuing after
    /// an `Err` is what retries the connection. The previous `break` ended
    /// the thread on the first broker hiccup, silencing HA egress until a
    /// process restart, while its sibling `frigate_bridge` self-healed.
    /// Outage and recovery are each logged once (log-when-unavailable);
    /// backoff keeps a dead broker from being hammered. Because the client
    /// connects with clean-start, subscriptions do not survive a reconnect,
    /// so `subscribe_filter` is (re)subscribed on every ConnAck — including
    /// the first.
    fn new(client: Client, mut connection: Connection, daemon: Option<DaemonWiring>) -> Self {
        let shutdown = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let connected = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let thread_shutdown = std::sync::Arc::clone(&shutdown);
        let thread_connected = std::sync::Arc::clone(&connected);
        let thread_client = client.clone();
        let handle = std::thread::spawn(move || {
            const BACKOFF_START: Duration = Duration::from_secs(2);
            const BACKOFF_CAP: Duration = Duration::from_secs(60);
            let mut backoff = BACKOFF_START;
            let mut outage_logged = false;
            for event in connection.iter() {
                if thread_shutdown.load(std::sync::atomic::Ordering::SeqCst) {
                    break;
                }
                match event {
                    Ok(Event::Incoming(Incoming::ConnAck(_))) => {
                        if outage_logged {
                            log::info!("MQTT broker connection restored");
                            outage_logged = false;
                        }
                        backoff = BACKOFF_START;
                        thread_connected.store(true, std::sync::atomic::Ordering::SeqCst);
                        if let Some(w) = &daemon {
                            // Re-assert availability (broker holds the retained
                            // LWT `offline` from the drop) and re-subscribe the
                            // command filter, both lost across a clean-start
                            // reconnect. Retained state topics self-heal on the
                            // next poll cycle, so only these two need replaying.
                            if let Err(e) = thread_client.publish(
                                &w.availability_topic,
                                QoS::AtLeastOnce,
                                true,
                                PAYLOAD_ONLINE.as_bytes().to_vec(),
                            ) {
                                log::warn!("MQTT availability re-publish failed: {}", e);
                            }
                            if let Err(e) = thread_client.subscribe(&w.cmd_filter, QoS::AtLeastOnce)
                            {
                                log::warn!("MQTT command (re)subscribe failed: {}", e);
                            }
                        }
                    }
                    Ok(Event::Incoming(Incoming::Publish(publish))) => {
                        if let Some(w) = &daemon {
                            let topic = match std::str::from_utf8(&publish.topic) {
                                Ok(topic) => topic.to_string(),
                                Err(_) => continue,
                            };
                            if w.commands.send((topic, publish.payload.to_vec())).is_err() {
                                break;
                            }
                        }
                    }
                    Ok(_) => {}
                    Err(e) => {
                        thread_connected.store(false, std::sync::atomic::Ordering::SeqCst);
                        if !outage_logged {
                            log::warn!("MQTT connection error (retrying with backoff): {}", e);
                            outage_logged = true;
                        }
                        // Sleep in slices so a shutdown request doesn't have
                        // to wait out a full backoff window.
                        let mut slept = Duration::ZERO;
                        while slept < backoff
                            && !thread_shutdown.load(std::sync::atomic::Ordering::SeqCst)
                        {
                            let slice = std::cmp::min(Duration::from_millis(250), backoff - slept);
                            std::thread::sleep(slice);
                            slept += slice;
                        }
                        // Break here on shutdown rather than looping back to
                        // connection.iter().next(), which would block trying
                        // to reconnect to a dead broker and hang disconnect()'s
                        // join. The top-of-loop check only guards the path
                        // where the disconnect nudge unblocks next(); a
                        // shutdown that lands mid-backoff has no such nudge.
                        if thread_shutdown.load(std::sync::atomic::Ordering::SeqCst) {
                            break;
                        }
                        backoff = std::cmp::min(backoff * 2, BACKOFF_CAP);
                    }
                }
            }
        });

        Self {
            client,
            connection_handle: Some(handle),
            shutdown,
            connected,
        }
    }

    /// True while the eventloop holds a live broker connection (last saw a
    /// ConnAck, no error since). The daemon loop gates publishing on this.
    fn is_connected(&self) -> bool {
        self.connected.load(std::sync::atomic::Ordering::SeqCst)
    }

    fn disconnect(mut self) -> Result<()> {
        // Raise the flag before disconnecting: the eventloop thread no
        // longer exits on connection errors, so this is what lets join()
        // return when the broker is already gone. The disconnect itself is
        // best-effort for the same reason — it can fail against a dead
        // broker, and the thread must still be joined.
        self.shutdown
            .store(true, std::sync::atomic::Ordering::SeqCst);
        let disconnect_result = self.client.disconnect();
        if let Some(handle) = self.connection_handle.take() {
            let _ = handle.join();
        }
        disconnect_result?;
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
    let tls_backend: TlsBackend = args.mqtt_tls_backend.parse()?;
    tls_backend.validate_feature_support()?;
    let tls_materials = TlsMaterials::load(
        args.mqtt_tls_ca_path.as_ref(),
        args.mqtt_tls_client_cert_path.as_ref(),
        args.mqtt_tls_client_key_path.as_ref(),
    )?;
    let tls_config = TlsConfig {
        backend: tls_backend,
        materials: tls_materials,
    };

    if !args.allow_remote_mqtt {
        validate_loopback_addr(&mqtt_endpoint, &args.mqtt_broker_addr)?;
    } else {
        log::warn!("Remote MQTT enabled - ensure broker is in a trusted network");
    }

    let tokens = TokenSource {
        path: args.api_token_path.clone(),
        fixed: args.api_token.clone(),
    };
    {
        // Fail fast on a missing/empty token at startup; the daemon re-reads
        // the file per request because the API rotates it every 10 minutes.
        let _stage = ui.stage("Load capability token");
        tokens.current()?;
    }

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
        tls_config: &tls_config,
        tokens: &tokens,
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
    tls_config: &'a TlsConfig,
    tokens: &'a TokenSource,
    device_id: &'a str,
    device_info: &'a HaDeviceInfo,
    availability_topic: &'a str,
    ui: &'a ui::Ui,
}

fn run_oneshot(ctx: &RunContext<'_>) -> Result<()> {
    let artifact = {
        let _stage = ctx.ui.stage("Fetch export artifact");
        let token = ctx.tokens.current()?;
        fetch_export_artifact(ctx.api_addr, &token)?
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
            ctx.tls_config,
            &ctx.args.mqtt_client_id,
            ctx.args.mqtt_username.as_deref(),
            ctx.args.mqtt_password.as_deref(),
            ctx.availability_topic,
            None,
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
        "Starting daemon mode (poll interval: {}s, verify interval: {}s)",
        ctx.args.poll_interval,
        ctx.args.verify_interval_secs
    );

    let (cmd_tx, cmd_rx) = mpsc::channel::<(String, Vec<u8>)>();
    // Scoped to the command subtree only — this daemon must never consume
    // event or sensor traffic. Subscribed from the eventloop thread on every
    // ConnAck so it survives broker reconnects.
    let cmd_filter = format!("{}/cmd/#", ctx.args.mqtt_topic_prefix);
    let conn = {
        let _stage = ctx.ui.stage("Connect to MQTT broker");
        connect_mqtt(
            ctx.mqtt_endpoint,
            ctx.tls_config,
            &ctx.args.mqtt_client_id,
            ctx.args.mqtt_username.as_deref(),
            ctx.args.mqtt_password.as_deref(),
            ctx.availability_topic,
            Some(DaemonWiring {
                commands: cmd_tx,
                cmd_filter,
                availability_topic: ctx.availability_topic.to_string(),
            }),
        )?
    };

    let verify_cmd_topic = format!("{}/cmd/verify", ctx.args.mqtt_topic_prefix);

    // Publish availability online (retained)
    mqtt_publish_qos1(
        &conn.client,
        ctx.availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;
    log::info!("Published online status to {}", ctx.availability_topic);

    // Publish the zone-independent entities up front so the device appears
    // in HA immediately, not only after the first event fires.
    if !ctx.args.no_discovery {
        publish_static_discovery(
            &conn.client,
            &ctx.args.ha_discovery_prefix,
            &ctx.args.mqtt_topic_prefix,
            ctx.availability_topic,
            ctx.device_id,
            ctx.device_info,
        )?;
    }

    // Initial verification populates the chain-integrity sensor right away.
    let verify_interval = ctx.args.verify_interval_secs;
    let mut last_auto_verify = Instant::now();
    if verify_interval > 0 {
        if let Err(e) = run_verify_and_publish(ctx, &conn.client) {
            log::warn!("Initial verification failed to run: {}", e);
        }
    }

    let mut discovered_zones: HashSet<String> = HashSet::new();
    let mut zone_states: HashMap<String, ZoneState> = HashMap::new();
    let mut publish_cursor: Option<PublishCursor> = None;

    loop {
        // Only fetch+publish while the broker is reachable. During an outage
        // the eventloop thread is reconnecting with backoff and cannot drain
        // the bounded request queue, so a blocking publish would wedge this
        // loop after ~10 queued messages (docs/strategy/12 FR-4). Skipping the
        // cycle is safe: retained state topics self-heal on the next poll, and
        // events accumulated during the outage publish as a catch-up once the
        // link (and, via ConnAck, availability) is restored. Command handling
        // and shutdown stay responsive through the wait loop below.
        if conn.is_connected() {
            match ctx
                .tokens
                .current()
                .and_then(|token| fetch_export_artifact(ctx.api_addr, &token))
            {
                Ok(artifact) => {
                    let events = flatten_export_events(&artifact);

                    // Filter to only new events. The cursor remembers how many
                    // events of the newest bucket were already published, so a
                    // 30 s poll inside a 10-minute bucket does not republish the
                    // whole bucket (and re-count every zone) every time.
                    let new_events = select_new_events(&events, &mut publish_cursor);

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
                            let payload = build_event_state_payload(last)?;
                            let json = serde_json::to_vec(&payload)?;
                            mqtt_publish_qos1(&conn.client, &state_topic, &json, true)?;
                        }

                        log::info!("Published {} new events", new_events.len());
                    }
                }
                Err(e) => {
                    log::warn!("Failed to fetch events: {}", e);
                }
            }

            // Refresh the retained 24h digest each cycle.
            match ctx
                .tokens
                .current()
                .and_then(|token| fetch_digest(ctx.api_addr, &token))
            {
                Ok(digest) => {
                    let digest_topic = format!("{}/digest", ctx.args.mqtt_topic_prefix);
                    mqtt_publish_qos1(&conn.client, &digest_topic, &digest, true)?;
                }
                Err(e) => {
                    log::warn!("Failed to fetch digest: {}", e);
                }
            }

            // Scheduled re-verification keeps the integrity sensor fresh.
            if verify_interval > 0
                && last_auto_verify.elapsed() >= Duration::from_secs(verify_interval)
            {
                last_auto_verify = Instant::now();
                if let Err(e) = run_verify_and_publish(ctx, &conn.client) {
                    log::warn!("Scheduled verification failed to run: {}", e);
                }
            }
        } // end `if conn.is_connected()`

        // Sleep until the next poll, waking early for button presses.
        let deadline = Instant::now() + Duration::from_secs(ctx.args.poll_interval);
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                break;
            }
            match cmd_rx.recv_timeout(remaining) {
                Ok((topic, _payload)) if topic == verify_cmd_topic => {
                    log::info!("Verify command received via {}", topic);
                    if let Err(e) = run_verify_and_publish(ctx, &conn.client) {
                        log::warn!("Verification failed to run: {}", e);
                    }
                    last_auto_verify = Instant::now();
                }
                Ok((topic, _)) => {
                    log::debug!("Ignoring unknown command topic {}", topic);
                }
                Err(mpsc::RecvTimeoutError::Timeout) => break,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    // The eventloop thread has exited (only on shutdown now
                    // that it survives broker outages). Finish the wait on a
                    // plain sleep; the next is_connected() check gates work.
                    std::thread::sleep(remaining);
                    break;
                }
            }
        }
    }
}

/// POST /verify on the witness API and publish the outcome:
/// `<prefix>/chain_problem` (retained ON/OFF for the `problem`-class binary
/// sensor — ON means verification failed) and the full report JSON on
/// `<prefix>/chain_problem/attrs` (retained, surfaced as HA attributes).
fn run_verify_and_publish(ctx: &RunContext<'_>, client: &Client) -> Result<()> {
    let token = ctx.tokens.current()?;
    let report = post_verify(ctx.api_addr, &token)?;
    let chain_valid = serde_json::from_slice::<serde_json::Value>(&report)
        .ok()
        .and_then(|v| v.get("chain_valid").and_then(|b| b.as_bool()))
        .unwrap_or(false);

    let state_topic = format!("{}/chain_problem", ctx.args.mqtt_topic_prefix);
    let attrs_topic = format!("{}/chain_problem/attrs", ctx.args.mqtt_topic_prefix);
    mqtt_publish_qos1(
        client,
        &state_topic,
        if chain_valid { b"OFF" } else { b"ON" },
        true,
    )?;
    mqtt_publish_qos1(client, &attrs_topic, &report, true)?;
    log::info!(
        "Sealed-log verification published: chain_valid={}",
        chain_valid
    );
    Ok(())
}

/// Discovery configs for the entities that exist regardless of which zones
/// have produced events: last-event sensor, daily digest, chain integrity,
/// and the verify button.
fn publish_static_discovery(
    client: &Client,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
) -> Result<()> {
    publish_last_event_discovery(
        client,
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;

    let digest_config = HaSensorConfig {
        name: "PWK Daily Digest".to_string(),
        unique_id: format!("{}_daily_digest", device_id),
        state_topic: format!("{}/digest", state_prefix),
        json_attributes_topic: Some(format!("{}/digest", state_prefix)),
        value_template: Some("{{ value_json.total_events }}".to_string()),
        unit_of_measurement: Some("events".to_string()),
        device_class: None,
        state_class: None,
        icon: Some("mdi:calendar-today".to_string()),
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };
    let config_topic = format!(
        "{}/sensor/{}/daily_digest/config",
        discovery_prefix, device_id
    );
    mqtt_publish_qos1(
        client,
        &config_topic,
        &serde_json::to_vec(&digest_config)?,
        true,
    )?;

    let chain_config = HaBinarySensorConfig {
        name: "PWK Chain Problem".to_string(),
        unique_id: format!("{}_chain_problem", device_id),
        state_topic: format!("{}/chain_problem", state_prefix),
        json_attributes_topic: Some(format!("{}/chain_problem/attrs", state_prefix)),
        value_template: None,
        device_class: "problem".to_string(),
        off_delay: None,
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };
    let config_topic = format!(
        "{}/binary_sensor/{}/chain_problem/config",
        discovery_prefix, device_id
    );
    mqtt_publish_qos1(
        client,
        &config_topic,
        &serde_json::to_vec(&chain_config)?,
        true,
    )?;

    let button_config = HaButtonConfig {
        name: "PWK Verify Now".to_string(),
        unique_id: format!("{}_verify_now", device_id),
        command_topic: format!("{}/cmd/verify", state_prefix),
        payload_press: "PRESS".to_string(),
        icon: Some("mdi:shield-search".to_string()),
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };
    let config_topic = format!(
        "{}/button/{}/verify_now/config",
        discovery_prefix, device_id
    );
    mqtt_publish_qos1(
        client,
        &config_topic,
        &serde_json::to_vec(&button_config)?,
        true,
    )?;

    log::info!("Published HA discovery for digest, chain integrity, and verify button");
    Ok(())
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
    publish_last_event_discovery(
        client,
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;

    log::info!(
        "Published HA discovery for {} zones + last_event sensor",
        zones.len()
    );
    Ok(())
}

fn publish_last_event_discovery(
    client: &Client,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
) -> Result<()> {
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
        let payload = build_event_state_payload(last)?;
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

fn build_event_state_payload(event: &ExportEvent) -> Result<EventStatePayload> {
    let published_bucket = TimeBucket::now_10min()?;
    Ok(EventStatePayload {
        event_type: format!("{:?}", event.event_type),
        zone_id: event.zone_id.clone(),
        time_bucket_start: event.time_bucket.start_epoch_s,
        time_bucket_size: event.time_bucket.size_s,
        confidence: event.confidence,
        // serde renames carry the HA attestation contract values
        // ("adapter" / "ha-bridged"); absent means device/kernel-attested.
        attestation: event
            .attestation
            .and_then(|a| serde_json::to_value(a).ok())
            .and_then(|v| v.as_str().map(str::to_string)),
        published_bucket_start: published_bucket.start_epoch_s,
        published_bucket_size: published_bucket.size_s,
    })
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
    let body = api_request(addr, token, "GET", EVENTS_PATH)?;
    let artifact: ExportArtifact =
        serde_json::from_slice(&body).context("failed to parse /events response")?;
    Ok(artifact)
}

/// Fetch the rolling 24h digest as raw JSON (republished verbatim to MQTT).
fn fetch_digest(addr: std::net::SocketAddr, token: &str) -> Result<Vec<u8>> {
    api_request(addr, token, "GET", DIGEST_PATH)
}

/// Trigger a full sealed-log verification; returns the VerifyReport JSON.
fn post_verify(addr: std::net::SocketAddr, token: &str) -> Result<Vec<u8>> {
    api_request(addr, token, "POST", VERIFY_PATH)
}

fn api_request(
    addr: std::net::SocketAddr,
    token: &str,
    method: &str,
    path: &str,
) -> Result<Vec<u8>> {
    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_secs(5))?;
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;

    let request = format!(
        "{method} {path} HTTP/1.1\r\nHost: {host}\r\nx-witness-token: {token}\r\nConnection: close\r\n\r\n",
        host = addr,
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
        return Err(anyhow!(
            "event api returned status {} for {}",
            status_code,
            path
        ));
    }

    Ok(body.to_vec())
}

/// Where publication stopped: the newest bucket seen and how many of its
/// events (in export order) have already gone out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct PublishCursor {
    bucket_start_epoch_s: u64,
    published_in_bucket: usize,
}

/// The events not yet published, in export order, advancing the cursor.
///
/// Sealed events all carry the same bucket size, so "bucket start > last
/// seen" is the only ordering the artifact gives us — and within the
/// current bucket every event compares equal. The first bridge kept only
/// the last bucket and re-emitted its entire contents on every poll
/// (duplicate events, motion re-triggered, zone counts inflated by one
/// bucket-load per 30 s). Counting what was already published within that
/// bucket is what makes a second poll over the same artifact yield nothing.
fn select_new_events<'a>(
    events: &'a [ExportEvent],
    cursor: &mut Option<PublishCursor>,
) -> Vec<&'a ExportEvent> {
    let mut out = Vec::new();
    let mut seen_in_newest = 0usize;
    let mut newest: Option<u64> = None;
    for event in events {
        let start = event.time_bucket.start_epoch_s;
        // Track the newest bucket and how many events it holds in this artifact.
        match newest {
            Some(n) if start == n => seen_in_newest += 1,
            Some(n) if start > n => {
                newest = Some(start);
                seen_in_newest = 1;
            }
            Some(_) => {}
            None => {
                newest = Some(start);
                seen_in_newest = 1;
            }
        }
        let is_new = match cursor {
            None => true,
            Some(c) => {
                start > c.bucket_start_epoch_s
                    || (start == c.bucket_start_epoch_s
                        && seen_in_newest > c.published_in_bucket
                        && newest == Some(start))
            }
        };
        if is_new {
            out.push(event);
        }
    }
    if let Some(n) = newest {
        let published = match cursor {
            Some(c) if c.bucket_start_epoch_s == n => c.published_in_bucket.max(seen_in_newest),
            _ => seen_in_newest,
        };
        *cursor = Some(PublishCursor {
            bucket_start_epoch_s: n,
            published_in_bucket: published,
        });
    }
    out
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

fn connect_mqtt(
    endpoint: &MqttEndpoint,
    tls_config: &TlsConfig,
    client_id: &str,
    username: Option<&str>,
    password: Option<&str>,
    will_topic: &str,
    daemon: Option<DaemonWiring>,
) -> Result<MqttRuntime> {
    let mut options = MqttOptions::new(client_id, (endpoint.host.as_str(), endpoint.port));
    options.set_keep_alive(60);
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default().to_string());
    } else {
        log::warn!("MQTT connecting without authentication; set --mqtt-username/--mqtt-password for production use");
    }
    let will = rumqttc::LastWill::new(
        will_topic,
        PAYLOAD_OFFLINE.as_bytes().to_vec(),
        QoS::AtLeastOnce,
        true,
        None,
    );
    options.set_last_will(will);
    options.set_transport(tls_config.build_transport(endpoint)?);

    let (client, connection) = rumqttc::ClientBuilder::new(options).capacity(10).build();
    log::info!(
        "Connected to MQTT broker (TLS: {}, backend: {}, auth: {})",
        endpoint.use_tls,
        tls_config.backend,
        username.is_some()
    );
    Ok(MqttRuntime::new(client, connection, daemon))
}

fn mqtt_publish_qos1(client: &Client, topic: &str, payload: &[u8], retain: bool) -> Result<()> {
    client.publish(topic, QoS::AtLeastOnce, retain, payload.to_vec())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(start: u64, zone: &str) -> ExportEvent {
        let mut e: ExportEvent = serde_json::from_value(serde_json::json!({
            "event_type": "BoundaryCrossingObjectLarge",
            "zone_id": format!("zone:{zone}"),
            "time_bucket": { "start_epoch_s": start, "size_s": 600 },
            "confidence": 0.9
        }))
        .expect("export event shape");
        e.time_bucket = TimeBucket { start_epoch_s: start, size_s: 600 };
        e
    }

    #[test]
    fn select_new_events_does_not_republish_the_current_bucket() {
        let mut cursor = None;
        let artifact = vec![ev(1_000_200, "a"), ev(1_000_200, "b")];
        assert_eq!(select_new_events(&artifact, &mut cursor).len(), 2);
        // Same artifact 30 s later: nothing new.
        assert!(select_new_events(&artifact, &mut cursor).is_empty());
        // One more event lands in the same bucket: exactly that one.
        let grown = vec![ev(1_000_200, "a"), ev(1_000_200, "b"), ev(1_000_200, "c")];
        let fresh = select_new_events(&grown, &mut cursor);
        assert_eq!(fresh.len(), 1);
        assert_eq!(fresh[0].zone_id, "zone:c");
        assert!(select_new_events(&grown, &mut cursor).is_empty());
        // The bucket advances: only the new bucket's events, and the cursor moves.
        let next = vec![ev(1_000_200, "a"), ev(1_000_200, "b"), ev(1_000_200, "c"), ev(1_000_800, "d")];
        let fresh = select_new_events(&next, &mut cursor);
        assert_eq!(fresh.len(), 1);
        assert_eq!(fresh[0].zone_id, "zone:d");
        assert_eq!(cursor, Some(PublishCursor { bucket_start_epoch_s: 1_000_800, published_in_bucket: 1 }));
        // Retention drops the old bucket from the artifact: still nothing new.
        let trimmed = vec![ev(1_000_800, "d")];
        assert!(select_new_events(&trimmed, &mut cursor).is_empty());
    }

    #[test]
    fn serialize_event_state_payload() {
        let payload = EventStatePayload {
            event_type: "BoundaryCrossingObjectLarge".to_string(),
            zone_id: "zone:front_door".to_string(),
            attestation: None,
            time_bucket_start: 1_700_000_000,
            time_bucket_size: 600,
            confidence: 0.9,
            published_bucket_start: 1_700_000_200,
            published_bucket_size: 600,
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
    fn token_source_rereads_rotated_file() {
        let mut path = std::env::temp_dir();
        path.push(format!("event_mqtt_bridge_token_{}", rand::random::<u64>()));
        std::fs::write(&path, "token-one\n").expect("write token");

        let tokens = TokenSource {
            path: Some(path.clone()),
            fixed: None,
        };
        assert_eq!(tokens.current().expect("first read"), "token-one");

        // The witness API rotates the token every 10-minute bucket by
        // rewriting this file; the source must pick the new value up.
        std::fs::write(&path, "token-two\n").expect("rotate token");
        assert_eq!(tokens.current().expect("second read"), "token-two");

        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn token_source_fixed_value_wins() {
        let tokens = TokenSource {
            path: Some(PathBuf::from("/nonexistent/token")),
            fixed: Some("fixed-token".to_string()),
        };
        assert_eq!(tokens.current().expect("fixed"), "fixed-token");
    }

    #[test]
    fn ha_button_config_serializes_correctly() {
        let device = HaDeviceInfo {
            identifiers: vec!["pwk_test".to_string()],
            name: "PWK Test".to_string(),
            manufacturer: "securaCV".to_string(),
            model: "PWK".to_string(),
            sw_version: "0.5.0".to_string(),
        };
        let config = HaButtonConfig {
            name: "PWK Verify Now".to_string(),
            unique_id: "pwk_test_verify_now".to_string(),
            command_topic: "witness/cmd/verify".to_string(),
            payload_press: "PRESS".to_string(),
            icon: Some("mdi:shield-search".to_string()),
            availability_topic: "witness/status".to_string(),
            payload_available: "online".to_string(),
            payload_not_available: "offline".to_string(),
            device,
        };

        let json = serde_json::to_string(&config).expect("serialize");
        assert!(json.contains("command_topic"));
        assert!(json.contains("witness/cmd/verify"));
        assert!(json.contains("payload_press"));
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
