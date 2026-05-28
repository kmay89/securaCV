//! adapter_host — run many vendor-neutral sensor adapters behind one daemon.
//!
//! Reads a TOML config describing a set of adapters, connects each to its MQTT source on a
//! background thread, and runs the shared [`AdapterHost`] loop. Every claim flows through the
//! same `Kernel::append_event_checked` choke point, so privacy invariants hold regardless of how
//! many vendors/sensors are wired in. This replaces the need for N bespoke bridge binaries.
//!
//! Example config (see `config.example.toml`):
//!
//! ```toml
//! db_path = "witness.db"
//! ruleset_id = "ruleset:adapter_host_v1"
//! bucket_size_secs = 600
//! poll_interval_secs = 1
//! min_confidence = 0.5
//!
//! [[adapter]]
//! type = "frigate"
//! mqtt_broker_addr = "127.0.0.1:1883"
//! topic = "frigate/events"
//!
//! [[adapter]]
//! type = "mqtt_sensor"
//! mqtt_broker_addr = "127.0.0.1:1883"
//! [[adapter.route]]
//! topic = "sensors/garage/acoustic"
//! kind = "acoustic_impulse_in_zone"
//! zone = "garage"
//! ```

use std::sync::mpsc::Sender;
use std::thread;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::v5::mqttbytes::QoS;
use rumqttc::v5::{Client, Event, Incoming, MqttOptions};
use serde::Deserialize;

use witness_kernel::adapter::frigate::FrigateAdapter;
use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::adapter::{AdapterHost, AdapterHostConfig, ClaimKind};
use witness_kernel::{KernelConfig, ZonePolicy, MIN_BUCKET_SIZE_S};

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "Run vendor-neutral sensor adapters into the witness kernel"
)]
struct Args {
    /// Path to the adapter host TOML config.
    #[arg(long, env = "ADAPTER_HOST_CONFIG", default_value = "adapter_host.toml")]
    config: String,
}

fn default_bucket() -> u32 {
    600
}
fn default_poll() -> u64 {
    1
}
fn default_frigate_topic() -> String {
    "frigate/events".to_string()
}
fn default_broker() -> String {
    "127.0.0.1:1883".to_string()
}

#[derive(Debug, Deserialize)]
struct FileConfig {
    db_path: String,
    ruleset_id: String,
    #[serde(default = "default_bucket")]
    bucket_size_secs: u32,
    #[serde(default = "default_poll")]
    poll_interval_secs: u64,
    #[serde(default)]
    min_confidence: f32,
    #[serde(default)]
    adapter: Vec<AdapterCfg>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum AdapterCfg {
    Frigate(FrigateCfg),
    MqttSensor(MqttSensorCfg),
}

#[derive(Debug, Deserialize)]
struct FrigateCfg {
    #[serde(default = "default_broker")]
    mqtt_broker_addr: String,
    #[serde(default = "default_frigate_topic")]
    topic: String,
    #[serde(default)]
    cameras: Option<Vec<String>>,
    #[serde(default)]
    labels: Vec<String>,
    #[serde(default)]
    min_confidence: Option<f64>,
    #[serde(default)]
    mqtt_username: Option<String>,
    #[serde(default)]
    mqtt_password: Option<String>,
}

#[derive(Debug, Deserialize)]
struct MqttSensorCfg {
    #[serde(default = "default_broker")]
    mqtt_broker_addr: String,
    #[serde(default)]
    route: Vec<RouteCfg>,
    #[serde(default)]
    mqtt_username: Option<String>,
    #[serde(default)]
    mqtt_password: Option<String>,
}

#[derive(Debug, Deserialize)]
struct RouteCfg {
    topic: String,
    kind: String,
    zone: String,
    #[serde(default)]
    min_confidence: f32,
    #[serde(default)]
    require_truthy_state: bool,
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let raw = std::fs::read_to_string(&args.config)
        .with_context(|| format!("reading adapter host config {}", args.config))?;
    let file: FileConfig = toml::from_str(&raw).context("parsing adapter host config")?;

    if file.bucket_size_secs < MIN_BUCKET_SIZE_S {
        return Err(anyhow!(
            "bucket_size_secs {} is below the {}s minimum (spec/event_contract.md §3)",
            file.bucket_size_secs,
            MIN_BUCKET_SIZE_S
        ));
    }
    if file.adapter.is_empty() {
        return Err(anyhow!(
            "no [[adapter]] entries configured in {}",
            args.config
        ));
    }

    let device_key_seed =
        std::env::var("DEVICE_KEY_SEED").map_err(|_| anyhow!("DEVICE_KEY_SEED must be set"))?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&file.ruleset_id);
    let kernel_cfg = KernelConfig {
        db_path: file.db_path.clone(),
        ruleset_id: file.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };
    let kernel = witness_kernel::Kernel::open(&kernel_cfg)?;
    log::info!("Kernel opened: {}", file.db_path);

    let host_cfg = AdapterHostConfig {
        bucket_size_secs: file.bucket_size_secs,
        kernel_version: kernel_cfg.kernel_version.clone(),
        ruleset_id: file.ruleset_id.clone(),
        ruleset_hash,
        min_confidence: file.min_confidence,
    };
    let mut host = AdapterHost::new(kernel, host_cfg);

    for (idx, cfg) in file.adapter.into_iter().enumerate() {
        match cfg {
            AdapterCfg::Frigate(fc) => {
                let (adapter, tx) = FrigateAdapter::new(
                    fc.cameras.clone(),
                    fc.labels.clone(),
                    fc.min_confidence.unwrap_or(0.5),
                );
                spawn_mqtt_forwarder(
                    format!("adapter_host_frigate_{idx}"),
                    fc.mqtt_broker_addr,
                    fc.mqtt_username,
                    fc.mqtt_password,
                    vec![fc.topic],
                    tx,
                );
                host.register(adapter);
                log::info!("registered frigate adapter #{idx}");
            }
            AdapterCfg::MqttSensor(mc) => {
                let mut routes = Vec::new();
                for r in &mc.route {
                    let kind = ClaimKind::from_str_opt(&r.kind)
                        .ok_or_else(|| anyhow!("unknown claim kind '{}'", r.kind))?;
                    let mut route = SensorRoute::new(r.topic.clone(), kind, r.zone.clone());
                    route.min_confidence = r.min_confidence;
                    route.require_truthy_state = r.require_truthy_state;
                    routes.push(route);
                }
                let (adapter, tx) = MqttSensorAdapter::new(routes);
                let topics = adapter.topics();
                spawn_mqtt_forwarder(
                    format!("adapter_host_sensor_{idx}"),
                    mc.mqtt_broker_addr,
                    mc.mqtt_username,
                    mc.mqtt_password,
                    topics,
                    tx,
                );
                host.register(adapter);
                log::info!("registered mqtt_sensor adapter #{idx}");
            }
        }
    }

    log::info!(
        "adapter host running: {} adapters, bucket {}s, poll {}s",
        host.adapter_count(),
        file.bucket_size_secs,
        file.poll_interval_secs
    );
    host.run_loop(Duration::from_secs(file.poll_interval_secs))
}

/// Spawn a background thread that connects to MQTT, subscribes to `topics`, and forwards every
/// publish `(topic, payload)` into `tx`. Reconnects on error with a fixed backoff.
fn spawn_mqtt_forwarder(
    client_id: String,
    broker_addr: String,
    username: Option<String>,
    password: Option<String>,
    topics: Vec<String>,
    tx: Sender<(String, Vec<u8>)>,
) {
    thread::spawn(move || loop {
        match connect(
            &client_id,
            &broker_addr,
            username.as_deref(),
            password.as_deref(),
        ) {
            Ok((client, mut connection)) => {
                for topic in &topics {
                    if let Err(e) = client.subscribe(topic, QoS::AtLeastOnce) {
                        log::warn!("subscribe to {topic} failed: {e}");
                    } else {
                        log::info!("[{client_id}] subscribed to {topic}");
                    }
                }
                for event in connection.iter() {
                    match event {
                        Ok(Event::Incoming(Incoming::Publish(p))) => {
                            let topic = String::from_utf8_lossy(&p.topic).to_string();
                            if tx.send((topic, p.payload.to_vec())).is_err() {
                                return; // host dropped the adapter; stop forwarding.
                            }
                        }
                        Ok(_) => {}
                        Err(e) => {
                            log::warn!("[{client_id}] mqtt error: {e}");
                            break;
                        }
                    }
                }
            }
            Err(e) => log::warn!("[{client_id}] mqtt connect failed: {e}"),
        }
        thread::sleep(Duration::from_secs(3));
    });
}

fn connect(
    client_id: &str,
    broker_addr: &str,
    username: Option<&str>,
    password: Option<&str>,
) -> Result<(Client, rumqttc::v5::Connection)> {
    let (host, port) = broker_addr
        .rsplit_once(':')
        .ok_or_else(|| anyhow!("broker addr must be host:port, got {broker_addr}"))?;
    let port: u16 = port.parse().context("parsing broker port")?;
    if host != "127.0.0.1" && host != "localhost" && host != "::1" {
        log::warn!("connecting to non-loopback broker {host}; ensure the network is trusted");
    }
    let mut options = MqttOptions::new(client_id, host, port);
    options.set_keep_alive(Duration::from_secs(60));
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default());
    }
    let (client, connection) = Client::new(options, 10);
    Ok((client, connection))
}
