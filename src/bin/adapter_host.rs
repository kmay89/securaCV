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

use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::thread;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::{Client, Event, Incoming, MqttOptions, QoS};
use serde::Deserialize;

use witness_kernel::adapter::ble_presence::{BlePresenceAdapter, BleRoom};
use witness_kernel::adapter::can_bus::{self, CanBusAdapter, CanRoute};
use witness_kernel::adapter::frigate::{FrigateAdapter, FrigateFilter};
use witness_kernel::adapter::meshtastic::{self, MeshNode, MeshtasticAdapter};
use witness_kernel::adapter::mqtt_sensor::{MqttSensorAdapter, SensorRoute};
use witness_kernel::Attestation;

/// Set by the SIGHUP handler; the run loop drains it to reload the config.
static RELOAD_REQUESTED: AtomicBool = AtomicBool::new(false);

/// A closure that applies a matching new [`AdapterCfg`] to a live adapter (hot-reload).
type Reloader = Box<dyn Fn(&AdapterCfg) -> Result<()>>;

/// Install a SIGHUP handler that requests a config reload (Linux only; no-op elsewhere).
fn install_reload_handler() {
    #[cfg(target_os = "linux")]
    {
        extern "C" fn on_sighup(_sig: libc::c_int) {
            RELOAD_REQUESTED.store(true, Ordering::SeqCst);
        }
        // Cast through the fn-pointer type before the integer sighandler_t.
        let handler = on_sighup as extern "C" fn(libc::c_int);
        unsafe {
            libc::signal(libc::SIGHUP, handler as libc::sighandler_t);
        }
        log::info!("SIGHUP will reload adapter routes/filters and min_confidence");
    }
}

/// Re-read the config and apply the hot-reloadable parts (host `min_confidence` and each adapter's
/// routes/rooms/filters). Topology changes (count/type/listener/auth) require a restart and are
/// logged, not applied. Never fatal: a bad file is logged and the daemon keeps running.
fn apply_reload(config_path: &str, host: &mut AdapterHost, reloaders: &[Reloader]) {
    let file = match std::fs::read_to_string(config_path)
        .map_err(anyhow::Error::from)
        .and_then(|raw| toml::from_str::<FileConfig>(&raw).map_err(anyhow::Error::from))
    {
        Ok(f) => f,
        Err(e) => {
            log::warn!("config reload failed (keeping current config): {e}");
            return;
        }
    };

    host.set_min_confidence(file.min_confidence);
    if file.adapter.len() != reloaders.len() {
        log::warn!(
            "config reload: adapter count changed ({} -> {}); restart to apply topology changes",
            reloaders.len(),
            file.adapter.len()
        );
    } else {
        for (idx, (reloader, cfg)) in reloaders.iter().zip(file.adapter.iter()).enumerate() {
            if let Err(e) = reloader(cfg) {
                log::warn!("config reload: adapter #{idx} not live-reloadable: {e}");
            }
        }
    }
    log::info!("config reloaded (min_confidence + adapter routes/filters)");
}
use witness_kernel::adapter::webhook::{
    self, RateLimit, WebhookAdapter, WebhookAuth, WebhookOptions,
};
use witness_kernel::adapter::{observability, AdapterHost, AdapterHostConfig, ClaimKind};
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
#[serde(deny_unknown_fields)]
struct FileConfig {
    db_path: String,
    ruleset_id: String,
    #[serde(default = "default_bucket")]
    bucket_size_secs: u32,
    #[serde(default = "default_poll")]
    poll_interval_secs: u64,
    #[serde(default)]
    min_confidence: f32,
    /// Optional `host:port` for the read-only adapter stats/health endpoint.
    #[serde(default)]
    stats_addr: Option<String>,
    #[serde(default)]
    adapter: Vec<AdapterCfg>,
}

fn default_webhook_addr() -> String {
    "127.0.0.1:8800".to_string()
}

fn default_espresense_topic() -> String {
    "espresense/devices/#".to_string()
}

fn default_meshtastic_topic() -> String {
    "msh/+/2/json/+/+".to_string()
}

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum AdapterCfg {
    Frigate(FrigateCfg),
    MqttSensor(MqttSensorCfg),
    Webhook(WebhookCfg),
    BlePresence(BleCfg),
    Meshtastic(MeshtasticCfg),
    CanBus(CanBusCfg),
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
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
    #[serde(default)]
    sandbox: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct MqttSensorCfg {
    #[serde(default = "default_broker")]
    mqtt_broker_addr: String,
    #[serde(default)]
    route: Vec<RouteCfg>,
    #[serde(default)]
    mqtt_username: Option<String>,
    #[serde(default)]
    mqtt_password: Option<String>,
    #[serde(default)]
    sandbox: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct WebhookCfg {
    #[serde(default = "default_webhook_addr")]
    listen_addr: String,
    #[serde(default)]
    route: Vec<RouteCfg>,
    #[serde(default)]
    sandbox: bool,
    /// Require `Authorization: Bearer <token>` matching this value.
    #[serde(default)]
    auth_token: Option<String>,
    /// Require `X-Signature: sha256=<hmac>` over the body, keyed by this secret. Takes precedence
    /// over `auth_token` if both are set.
    #[serde(default)]
    hmac_secret: Option<String>,
    /// Enable HMAC replay protection with this window (seconds): requires `X-Timestamp` + `X-Nonce`
    /// bound into the signature. Only meaningful with `hmac_secret`.
    #[serde(default)]
    hmac_replay_window_secs: Option<u64>,
    /// Per-path sustained rate (requests/minute). Enables rate limiting when set.
    #[serde(default)]
    rate_limit_per_min: Option<f64>,
    /// Per-path burst capacity (defaults to one minute's worth of `rate_limit_per_min`).
    #[serde(default)]
    rate_burst: Option<f64>,
    /// Connection worker-pool size (0 / unset = default).
    #[serde(default)]
    workers: Option<usize>,
    /// PEM cert chain for TLS (must be set together with `tls_key`).
    #[serde(default)]
    tls_cert: Option<String>,
    /// PEM private key for TLS.
    #[serde(default)]
    tls_key: Option<String>,
    /// PEM CA bundle for mutual TLS: clients must present a cert chaining to it. Requires TLS.
    #[serde(default)]
    tls_client_ca: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct BleCfg {
    #[serde(default = "default_broker")]
    mqtt_broker_addr: String,
    #[serde(default = "default_espresense_topic")]
    topic: String,
    #[serde(default)]
    room: Vec<BleRoomCfg>,
    #[serde(default)]
    mqtt_username: Option<String>,
    #[serde(default)]
    mqtt_password: Option<String>,
    #[serde(default)]
    sandbox: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct BleRoomCfg {
    room: String,
    zone: String,
    max_distance: f64,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct MeshtasticCfg {
    #[serde(default = "default_broker")]
    mqtt_broker_addr: String,
    /// Meshtastic JSON-mode uplink topic (gateway node: mqtt.enabled + mqtt.json_enabled).
    #[serde(default = "default_meshtastic_topic")]
    topic: String,
    #[serde(default)]
    node: Vec<MeshNodeCfg>,
    #[serde(default)]
    mqtt_username: Option<String>,
    #[serde(default)]
    mqtt_password: Option<String>,
    #[serde(default)]
    sandbox: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct MeshNodeCfg {
    /// Node id as `!hex`, bare hex, or decimal (the originating sensor node, not the gateway).
    node_id: String,
    kind: String,
    zone: String,
    #[serde(default)]
    detection_name: Option<String>,
    #[serde(default)]
    min_snr: Option<f32>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct CanBusCfg {
    /// Linux SocketCAN interface name (e.g. "can0"). Bring it up first, outside
    /// this process: `sudo ip link set can0 up type can bitrate 500000`.
    interface: String,
    #[serde(default)]
    route: Vec<CanRouteCfg>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct CanRouteCfg {
    /// Arbitration ID: decimal or 0x-prefixed hex (e.g. "0x3E8" or "1000").
    can_id: String,
    /// Byte index into the frame's payload to inspect (0-based).
    byte_offset: usize,
    /// Masked value that triggers this route: decimal or 0x-prefixed hex.
    equals: String,
    /// Bitmask applied before comparison (default 0xFF — whole byte).
    #[serde(default)]
    mask: Option<String>,
    kind: String,
    zone: String,
    /// Confidence stamped on the claim (default 0.9 — CAN frames carry no
    /// natural score).
    #[serde(default)]
    confidence: Option<f32>,
}

/// Parse a decimal or `0x`-prefixed hex string into an integer (route configs use this for
/// CAN IDs and byte values so a `candump` hex dump can be pasted in directly).
fn parse_can_int<T>(s: &str, field: &str) -> Result<T>
where
    T: TryFrom<u64>,
{
    let s = s.trim();
    let v = if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16)
    } else {
        s.parse::<u64>()
    }
    .with_context(|| format!("parsing {field} '{s}' as decimal or 0x-hex"))?;
    T::try_from(v).map_err(|_| anyhow!("{field} '{s}' out of range"))
}

/// Build a `CanRoute` list from config, validating claim kinds and ID/byte fields.
fn build_can_routes(routes: &[CanRouteCfg]) -> Result<Vec<CanRoute>> {
    routes
        .iter()
        .map(|r| {
            let kind = ClaimKind::from_str_opt(&r.kind)
                .ok_or_else(|| anyhow!("unknown claim kind '{}'", r.kind))?;
            let can_id: u32 = parse_can_int(&r.can_id, "can_id")?;
            let equals: u8 = parse_can_int(&r.equals, "equals")?;
            let mut route = CanRoute::new(can_id, r.byte_offset, equals, kind, r.zone.clone());
            if let Some(mask) = &r.mask {
                route = route.with_mask(parse_can_int(mask, "mask")?);
            }
            if let Some(confidence) = r.confidence {
                route = route.with_confidence(confidence);
            }
            Ok(route)
        })
        .collect()
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RouteCfg {
    topic: String,
    kind: String,
    zone: String,
    #[serde(default)]
    min_confidence: f32,
    #[serde(default)]
    require_truthy_state: bool,
    #[serde(default)]
    numeric_min: Option<f32>,
    /// Read the gating state from this JSON field instead of `state` (for multiplexed
    /// topics like a Canary's `sensing` stream). Fails closed when the field is absent.
    #[serde(default)]
    state_field: Option<String>,
    /// The state must equal this string exactly to emit a claim. Replaces the
    /// truthy/numeric gates — configuring it alongside them is a config error.
    #[serde(default)]
    state_equals: Option<String>,
    /// "adapter" (default when omitted) or "ha-bridged" for routes fed by an
    /// HA mqtt_statestream bridge.
    #[serde(default)]
    attestation: Option<String>,
}

/// Translate webhook config into runtime [`WebhookOptions`].
fn build_webhook_options(wc: &WebhookCfg) -> WebhookOptions {
    let auth = if let Some(secret) = &wc.hmac_secret {
        WebhookAuth::Hmac {
            secret: secret.clone().into_bytes(),
            replay_window: wc.hmac_replay_window_secs.map(Duration::from_secs),
        }
    } else if let Some(token) = &wc.auth_token {
        WebhookAuth::Bearer(token.clone())
    } else {
        WebhookAuth::None
    };
    let rate_limit = wc.rate_limit_per_min.map(|rpm| RateLimit {
        capacity: wc.rate_burst.unwrap_or_else(|| rpm.max(1.0)),
        refill_per_sec: rpm / 60.0,
    });
    WebhookOptions {
        auth,
        rate_limit,
        workers: wc.workers.unwrap_or(0),
    }
}

/// Build a `MeshNode` list from config, validating node ids and claim kinds against the
/// adapter's deliberately narrow allowlist.
fn build_mesh_nodes(nodes: &[MeshNodeCfg]) -> Result<Vec<MeshNode>> {
    nodes
        .iter()
        .map(|n| {
            let node_num = meshtastic::parse_node_id(&n.node_id)
                .ok_or_else(|| anyhow!("invalid meshtastic node_id '{}'", n.node_id))?;
            let kind = ClaimKind::from_str_opt(&n.kind)
                .ok_or_else(|| anyhow!("unknown claim kind '{}'", n.kind))?;
            if !meshtastic::allowed_kinds().contains(&kind) {
                return Err(anyhow!(
                    "claim kind '{}' is not in the meshtastic adapter allowlist ({})",
                    n.kind,
                    meshtastic::allowed_kinds()
                        .iter()
                        .map(|k| k.as_str())
                        .collect::<Vec<_>>()
                        .join(", ")
                ));
            }
            let mut node = MeshNode::new(node_num, kind, n.zone.clone());
            node.detection_name = n.detection_name.clone();
            node.min_snr = n.min_snr;
            Ok(node)
        })
        .collect()
}

/// Build a `SensorRoute` list from config, validating claim kinds.
fn build_routes(routes: &[RouteCfg]) -> Result<Vec<SensorRoute>> {
    routes
        .iter()
        .map(|r| {
            let kind = ClaimKind::from_str_opt(&r.kind)
                .ok_or_else(|| anyhow!("unknown claim kind '{}'", r.kind))?;
            if r.state_equals.is_some() && (r.require_truthy_state || r.numeric_min.is_some()) {
                return Err(anyhow!(
                    "route '{}': state_equals replaces the truthy/numeric gates — configure only one",
                    r.topic
                ));
            }
            let mut route = SensorRoute::new(r.topic.clone(), kind, r.zone.clone());
            route.min_confidence = r.min_confidence;
            route.require_truthy_state = r.require_truthy_state;
            route.numeric_min = r.numeric_min;
            route.state_field = r.state_field.clone();
            route.state_equals = r.state_equals.clone();
            route.attestation = match r.attestation.as_deref() {
                None | Some("adapter") => None, // adapter provenance is the path default
                Some("ha-bridged") => Some(Attestation::HaBridged),
                Some(other) => {
                    return Err(anyhow!(
                        "unknown attestation '{}' (expected 'adapter' or 'ha-bridged')",
                        other
                    ))
                }
            };
            Ok(route)
        })
        .collect()
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

    let mut reloaders: Vec<Reloader> = Vec::new();
    for (idx, cfg) in file.adapter.into_iter().enumerate() {
        match cfg {
            AdapterCfg::Frigate(fc) => {
                let (adapter, tx) = FrigateAdapter::new(
                    fc.cameras.clone(),
                    fc.labels.clone(),
                    fc.min_confidence.unwrap_or(0.5),
                );
                let adapter = adapter.with_sandbox(fc.sandbox);
                let filter = adapter.filter_handle();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::Frigate(fc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    // Recover a poisoned lock: reload overwrites the state wholesale,
                    // so SIGHUP is exactly how an operator un-wedges a poisoned adapter.
                    *filter.lock().unwrap_or_else(|p| p.into_inner()) = FrigateFilter::new(
                        fc.cameras.clone(),
                        fc.labels.clone(),
                        fc.min_confidence.unwrap_or(0.5),
                    );
                    Ok(())
                }));
                spawn_mqtt_forwarder(
                    format!("adapter_host_frigate_{idx}"),
                    fc.mqtt_broker_addr,
                    fc.mqtt_username,
                    fc.mqtt_password,
                    vec![fc.topic],
                    tx,
                );
                host.register(adapter);
                log::info!("registered frigate adapter #{idx} (sandbox={})", fc.sandbox);
            }
            AdapterCfg::MqttSensor(mc) => {
                let routes = build_routes(&mc.route)?;
                let (adapter, tx) = MqttSensorAdapter::new(routes);
                let adapter = adapter.with_sandbox(mc.sandbox);
                let handle = adapter.routes_handle();
                // Topics subscribed at startup. The MQTT forwarder subscribes once on connect, so a
                // reload can change route *attributes* (kind/zone/confidence/truthy) live, but
                // adding/removing/renaming a topic needs a restart to (un)subscribe.
                let subscribed: std::collections::BTreeSet<String> =
                    mc.route.iter().map(|r| r.topic.clone()).collect();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::MqttSensor(mc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    let new_topics: std::collections::BTreeSet<String> =
                        mc.route.iter().map(|r| r.topic.clone()).collect();
                    if new_topics != subscribed {
                        log::warn!(
                            "mqtt_sensor topic set changed on reload; added/removed topics require \
                             a restart to (un)subscribe (attributes for existing topics applied)"
                        );
                    }
                    *handle.lock().unwrap_or_else(|p| p.into_inner()) = build_routes(&mc.route)?;
                    Ok(())
                }));
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
                log::info!(
                    "registered mqtt_sensor adapter #{idx} (sandbox={})",
                    mc.sandbox
                );
            }
            AdapterCfg::Webhook(wc) => {
                let routes = build_routes(&wc.route)?;
                let (adapter, tx) = WebhookAdapter::new(routes);
                let adapter = adapter.with_sandbox(wc.sandbox);
                let handle = adapter.routes_handle();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::Webhook(wc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    *handle.lock().unwrap_or_else(|p| p.into_inner()) = build_routes(&wc.route)?;
                    Ok(())
                }));
                let options = build_webhook_options(&wc);
                let authed = !matches!(options.auth, WebhookAuth::None);
                let rate_limited = options.rate_limit.is_some();
                let listen_addr = wc.listen_addr.clone();

                // Fail closed: a webhook seals its claims into the witness log,
                // so an unauthenticated listener on a routable interface lets
                // anyone on the network forge witness events. Require token/HMAC
                // auth (or mutual TLS, which authenticates the client) before a
                // non-loopback bind. An explicit operator override is honored
                // for deliberately network-isolated deployments.
                let authenticated = authed || wc.tls_client_ca.is_some();
                let allow_insecure = std::env::var("ADAPTER_WEBHOOK_ALLOW_INSECURE")
                    .map(|v| {
                        let v = v.trim();
                        v == "1" || v.eq_ignore_ascii_case("true")
                    })
                    .unwrap_or(false);
                // Resolve exactly as webhook::bind does (ToSocketAddrs), so a
                // hostname listen_addr that resolves to a routable interface is
                // caught too — not only numeric SocketAddr literals. If the name
                // can't resolve here, the subsequent bind fails anyway.
                use std::net::ToSocketAddrs;
                let resolves_non_loopback = listen_addr
                    .to_socket_addrs()
                    .map(|addrs| addrs.into_iter().any(|sa| !sa.ip().is_loopback()))
                    .unwrap_or(false);
                if resolves_non_loopback && !authenticated && !allow_insecure {
                    return Err(anyhow!(
                        "webhook adapter #{idx}: refusing to bind non-loopback address '{}' \
                         without authentication — an unauthenticated webhook can forge witness \
                         events. Set auth_token/hmac_secret (or mutual TLS via tls_client_ca), \
                         bind a loopback address, or set ADAPTER_WEBHOOK_ALLOW_INSECURE=1 to \
                         override explicitly.",
                        listen_addr
                    ));
                }

                // TLS when both cert and key are configured; a client-CA adds mutual TLS.
                let tls = match (&wc.tls_cert, &wc.tls_key) {
                    (Some(cert), Some(key)) => {
                        let config = match &wc.tls_client_ca {
                            Some(ca) => webhook::load_server_config_mtls(
                                Path::new(cert),
                                Path::new(key),
                                Path::new(ca),
                            )
                            .with_context(|| {
                                format!("loading webhook mTLS materials for adapter #{idx}")
                            })?,
                            None => webhook::load_server_config(Path::new(cert), Path::new(key))
                                .with_context(|| {
                                    format!("loading webhook TLS materials for adapter #{idx}")
                                })?,
                        };
                        let label = if wc.tls_client_ca.is_some() {
                            "mutual"
                        } else {
                            "yes"
                        };
                        thread::spawn(move || {
                            if let Err(e) = webhook::serve_tls(&listen_addr, tx, options, config) {
                                log::error!("webhook TLS listener on {listen_addr} exited: {e}");
                            }
                        });
                        label
                    }
                    (None, None) => {
                        if wc.tls_client_ca.is_some() {
                            return Err(anyhow!(
                                "webhook adapter #{idx}: tls_client_ca requires tls_cert and tls_key"
                            ));
                        }
                        thread::spawn(move || {
                            if let Err(e) = webhook::serve_with_options(&listen_addr, tx, options) {
                                log::error!("webhook listener on {listen_addr} exited: {e}");
                            }
                        });
                        "no"
                    }
                    _ => {
                        return Err(anyhow!(
                            "webhook adapter #{idx}: tls_cert and tls_key must be set together"
                        ))
                    }
                };
                host.register(adapter);
                log::info!(
                    "registered webhook adapter #{idx} on {} (sandbox={}, auth={}, rate_limited={}, tls={})",
                    wc.listen_addr,
                    wc.sandbox,
                    authed,
                    rate_limited,
                    tls
                );
            }
            AdapterCfg::BlePresence(bc) => {
                let rooms: Vec<BleRoom> = bc
                    .room
                    .iter()
                    .map(|r| BleRoom::new(r.room.clone(), r.zone.clone(), r.max_distance))
                    .collect();
                let (adapter, tx) = BlePresenceAdapter::new(rooms);
                let adapter = adapter.with_sandbox(bc.sandbox);
                let handle = adapter.rooms_handle();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::BlePresence(bc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    *handle.lock().unwrap_or_else(|p| p.into_inner()) = bc
                        .room
                        .iter()
                        .map(|r| BleRoom::new(r.room.clone(), r.zone.clone(), r.max_distance))
                        .collect();
                    Ok(())
                }));
                spawn_mqtt_forwarder(
                    format!("adapter_host_ble_{idx}"),
                    bc.mqtt_broker_addr,
                    bc.mqtt_username,
                    bc.mqtt_password,
                    vec![bc.topic],
                    tx,
                );
                host.register(adapter);
                log::info!(
                    "registered ble_presence adapter #{idx} (sandbox={})",
                    bc.sandbox
                );
            }
            AdapterCfg::Meshtastic(mc) => {
                let nodes = build_mesh_nodes(&mc.node)?;
                let (adapter, tx) = MeshtasticAdapter::new(nodes);
                let adapter = adapter.with_sandbox(mc.sandbox);
                let handle = adapter.nodes_handle();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::Meshtastic(mc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    *handle.lock().unwrap_or_else(|p| p.into_inner()) = build_mesh_nodes(&mc.node)?;
                    Ok(())
                }));
                spawn_mqtt_forwarder(
                    format!("adapter_host_meshtastic_{idx}"),
                    mc.mqtt_broker_addr,
                    mc.mqtt_username,
                    mc.mqtt_password,
                    vec![mc.topic],
                    tx,
                );
                host.register(adapter);
                log::info!(
                    "registered meshtastic adapter #{idx} (sandbox={})",
                    mc.sandbox
                );
            }
            AdapterCfg::CanBus(cc) => {
                let routes = build_can_routes(&cc.route)?;
                let (adapter, tx) = CanBusAdapter::new(routes);
                let handle = adapter.routes_handle();
                reloaders.push(Box::new(move |c| {
                    let AdapterCfg::CanBus(cc) = c else {
                        return Err(anyhow!("adapter type changed at this position"));
                    };
                    *handle.lock().unwrap_or_else(|p| p.into_inner()) =
                        build_can_routes(&cc.route)?;
                    Ok(())
                }));
                spawn_socketcan_reader(cc.interface.clone(), tx);
                host.register(adapter);
                log::info!(
                    "registered can_bus adapter #{idx} on {} (read-only, no sandbox — see \
                     can_bus.rs's CanBusAdapter doc comment for why)",
                    cc.interface
                );
            }
        }
    }

    log::info!(
        "adapter host running: {} adapters, bucket {}s, poll {}s",
        host.adapter_count(),
        file.bucket_size_secs,
        file.poll_interval_secs
    );

    let poll = Duration::from_secs(file.poll_interval_secs);

    // SIGHUP-triggered config reload (routes/filters + min_confidence) without restarting.
    install_reload_handler();

    // Optional read-only stats endpoint, refreshed each cycle from the host snapshot.
    let stats = file.stats_addr.clone().map(|stats_addr| {
        let stats = observability::shared_stats();
        let serve_stats = stats.clone();
        thread::spawn(move || {
            if let Err(e) = observability::serve_stats(&stats_addr, serve_stats) {
                log::error!("adapter stats endpoint on {stats_addr} exited: {e}");
            }
        });
        stats
    });

    let log_every = (60 / file.poll_interval_secs.max(1)).max(1);
    let mut cycle: u64 = 0;
    loop {
        if RELOAD_REQUESTED.swap(false, Ordering::SeqCst) {
            apply_reload(&args.config, &mut host, &reloaders);
        }
        if let Err(e) = host.run_once() {
            log::warn!("adapter host poll cycle failed: {e}");
        }
        if let Some(stats) = &stats {
            if let Ok(mut g) = stats.lock() {
                *g = host.stats_snapshot();
            }
        }
        cycle += 1;
        if cycle.is_multiple_of(log_every) {
            host.log_stats_summary();
        }
        thread::sleep(poll);
    }
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
                let mut sub_failed = false;
                for topic in &topics {
                    if let Err(e) = client.subscribe(topic, QoS::AtLeastOnce) {
                        log::warn!("[{client_id}] subscribe to {topic} failed: {e}");
                        sub_failed = true;
                        break;
                    }
                    log::info!("[{client_id}] subscribed to {topic}");
                }
                // Reconnect rather than run with missing subscriptions.
                if !sub_failed {
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
) -> Result<(Client, rumqttc::Connection)> {
    let (host, port) = broker_addr
        .rsplit_once(':')
        .ok_or_else(|| anyhow!("broker addr must be host:port, got {broker_addr}"))?;
    let port: u16 = port.parse().context("parsing broker port")?;
    // Strip IPv6 brackets (e.g. "[::1]") for the loopback check and the rumqttc host.
    let host = host
        .strip_prefix('[')
        .and_then(|h| h.strip_suffix(']'))
        .unwrap_or(host);
    if host != "127.0.0.1" && host != "localhost" && host != "::1" {
        log::warn!("connecting to non-loopback broker {host}; ensure the network is trusted");
    }
    let mut options = MqttOptions::new(client_id, (host, port));
    options.set_keep_alive(60);
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default().to_string());
    }
    let (client, connection) = rumqttc::ClientBuilder::new(options).capacity(10).build();
    Ok((client, connection))
}

/// Read raw frames off a Linux SocketCAN interface (`can0`, etc.) and forward them into the
/// adapter's channel. **Read-only**: this function never calls `write`/`send` on the socket —
/// see `docs/hardware/canary_vehicle_can.md` §3 for why passive-only is a deliberate design
/// choice, not just an MVP gap. Reconnects (re-opens the interface) on any read error, exactly
/// like `spawn_mqtt_forwarder` reconnects on a dropped broker.
#[cfg(target_os = "linux")]
fn spawn_socketcan_reader(iface: String, tx: Sender<can_bus::CanFrame>) {
    thread::spawn(move || loop {
        match open_socketcan(&iface) {
            Ok(fd) => {
                log::info!("[can_bus:{iface}] listening (read-only)");
                let mut buf = [0u8; std::mem::size_of::<libc::can_frame>()];
                loop {
                    let n =
                        unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
                    if n < 0 {
                        log::warn!(
                            "[can_bus:{iface}] read error: {}",
                            std::io::Error::last_os_error()
                        );
                        break;
                    }
                    if n as usize != buf.len() {
                        continue; // short/partial read (e.g. a CAN FD frame) — skip, not fatal
                    }
                    // SAFETY: `buf` is exactly `size_of::<can_frame>()` bytes, just filled by a
                    // full `read()` from a CAN_RAW socket, which only ever delivers whole frames.
                    let frame: libc::can_frame =
                        unsafe { std::ptr::read(buf.as_ptr() as *const libc::can_frame) };
                    if frame.can_id & libc::CAN_ERR_FLAG != 0 {
                        continue; // bus error frame, not vehicle data
                    }
                    if frame.can_id & libc::CAN_RTR_FLAG != 0 {
                        continue; // remote-request frame, carries no data to match
                    }
                    let mask = if frame.can_id & libc::CAN_EFF_FLAG != 0 {
                        libc::CAN_EFF_MASK
                    } else {
                        libc::CAN_SFF_MASK
                    };
                    let can_id = frame.can_id & mask;
                    let dlc = (frame.can_dlc as usize).min(frame.data.len());
                    let data = frame.data[..dlc].to_vec();
                    if tx.send(can_bus::CanFrame { can_id, data }).is_err() {
                        unsafe { libc::close(fd) };
                        return; // host dropped the adapter; stop forwarding.
                    }
                }
                unsafe { libc::close(fd) };
            }
            Err(e) => log::warn!("[can_bus:{iface}] open failed: {e}"),
        }
        thread::sleep(Duration::from_secs(3));
    });
}

#[cfg(not(target_os = "linux"))]
fn spawn_socketcan_reader(iface: String, _tx: Sender<can_bus::CanFrame>) {
    log::error!(
        "[can_bus:{iface}] SocketCAN is Linux-only; the can_bus adapter cannot run on this platform"
    );
}

/// Open and bind a `CAN_RAW` socket on the named SocketCAN interface. Read-only by construction:
/// the caller only ever `read()`s the returned fd.
#[cfg(target_os = "linux")]
fn open_socketcan(iface: &str) -> Result<libc::c_int> {
    if iface.is_empty() || iface.len() >= libc::IFNAMSIZ {
        return Err(anyhow!(
            "interface name '{iface}' is empty or too long (max {} chars)",
            libc::IFNAMSIZ - 1
        ));
    }
    unsafe {
        let fd = libc::socket(libc::AF_CAN, libc::SOCK_RAW, libc::CAN_RAW);
        if fd < 0 {
            return Err(anyhow!(
                "socket(AF_CAN, SOCK_RAW, CAN_RAW) failed: {} — is the `can` kernel module loaded?",
                std::io::Error::last_os_error()
            ));
        }
        let mut ifr: libc::ifreq = std::mem::zeroed();
        for (dst, src) in ifr.ifr_name.iter_mut().zip(iface.bytes()) {
            *dst = src as libc::c_char;
        }
        if libc::ioctl(fd, libc::SIOCGIFINDEX, &mut ifr) < 0 {
            let err = std::io::Error::last_os_error();
            libc::close(fd);
            return Err(anyhow!(
                "ioctl(SIOCGIFINDEX) on '{iface}' failed: {err} — is it up? \
                 (sudo ip link set {iface} up type can bitrate 500000)"
            ));
        }
        let mut addr: libc::sockaddr_can = std::mem::zeroed();
        addr.can_family = libc::AF_CAN as libc::sa_family_t;
        addr.can_ifindex = ifr.ifr_ifru.ifru_ifindex;
        let addr_ptr = std::ptr::addr_of!(addr) as *const libc::sockaddr;
        if libc::bind(
            fd,
            addr_ptr,
            std::mem::size_of::<libc::sockaddr_can>() as libc::socklen_t,
        ) < 0
        {
            let err = std::io::Error::last_os_error();
            libc::close(fd);
            return Err(anyhow!("bind() on '{iface}' failed: {err}"));
        }
        Ok(fd)
    }
}

#[cfg(test)]
mod tests {
    use super::FileConfig;

    // Every gate field in this config defaults permissive when absent
    // (auth off, TLS off, all cameras, floor 0.0). A misspelled key must
    // therefore be a parse error, not a silent gate-disable: the config
    // parser is itself a privacy/auth chokepoint and fails closed.

    fn parse(toml_str: &str) -> Result<FileConfig, toml::de::Error> {
        toml::from_str::<FileConfig>(toml_str)
    }

    const BASE: &str = "db_path = \"witness.db\"\nruleset_id = \"ruleset:test\"\n";

    #[test]
    fn shipped_example_config_parses() {
        let raw = std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/adapter_host.example.toml"
        ))
        .expect("read adapter_host.example.toml");
        parse(&raw).expect("shipped example config must parse under deny_unknown_fields");
    }

    #[test]
    fn valid_config_still_parses() {
        let cfg = parse(&format!(
            "{BASE}\
             [[adapter]]\n type = \"webhook\"\n auth_token = \"secret\"\n\
             [[adapter.route]]\n topic = \"/t\"\n kind = \"motion_detected\"\n zone = \"z\"\n min_confidence = 0.8\n"
        ))
        .expect("valid config must parse");
        assert_eq!(cfg.adapter.len(), 1);
    }

    #[test]
    fn unknown_root_key_is_rejected() {
        let err = parse(&format!("{BASE}min_confidnce = 0.9\n")).unwrap_err();
        assert!(err.to_string().contains("min_confidnce"), "{err}");
    }

    #[test]
    fn webhook_auth_token_typo_is_rejected_not_unauthenticated() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"webhook\"\n auth_toke = \"secret\"\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("auth_toke"), "{err}");
    }

    #[test]
    fn webhook_tls_key_typo_is_rejected_not_plaintext() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"webhook\"\n tls_cert = \"c.pem\"\n tls_kee = \"k.pem\"\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("tls_kee"), "{err}");
    }

    #[test]
    fn frigate_cameras_typo_is_rejected_not_all_cameras() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"frigate\"\n camera = [\"front\"]\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("camera"), "{err}");
    }

    #[test]
    fn route_min_confidence_typo_is_rejected_not_floor_zero() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"mqtt_sensor\"\n\
             [[adapter.route]]\n topic = \"/t\"\n kind = \"motion_detected\"\n zone = \"z\"\n min_confidnce = 0.8\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("min_confidnce"), "{err}");
    }

    #[test]
    fn meshtastic_min_snr_typo_is_rejected() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"meshtastic\"\n\
             [[adapter.node]]\n node_id = \"!aabbccdd\"\n kind = \"motion_detected\"\n zone = \"z\"\n min_snr_db = 5.0\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("min_snr_db"), "{err}");
    }

    #[test]
    fn ble_room_unknown_key_is_rejected() {
        let err = parse(&format!(
            "{BASE}[[adapter]]\n type = \"ble_presence\"\n\
             [[adapter.room]]\n room = \"r\"\n zone = \"z\"\n max_distance = 2.0\n max_distence = 3.0\n"
        ))
        .unwrap_err();
        assert!(err.to_string().contains("max_distence"), "{err}");
    }
}
