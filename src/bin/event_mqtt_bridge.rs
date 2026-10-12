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
//!
//! Two retained topics talk to the SecuraCV integration directly rather
//! than through discovery (daemon mode):
//! - `<prefix>/<device_id>/announce` — the signed announce: who this kernel
//!   is (device id, verifying key, version), how to reach its API (scheme,
//!   bind posture, port, optionally a host) and whether Home Assistant must
//!   pair. Signed by the kernel key through `POST /api/sign-announce`; the
//!   bridge holds no key. Cleared (empty retained payload) on a clean
//!   shutdown so a removed kernel stops offering a "discovered" card;
//!   `--no-announce` suppresses it.
//! - `<prefix>/claim/<device_id>` — published BY the integration once it
//!   owns this kernel's entities. While a claim fresher than 24 h is
//!   retained there, the bridge withdraws its own discovery configs (empty
//!   retained payloads; the state topics keep flowing, the integration
//!   consumes them) and republishes them when the claim is cleared or goes
//!   stale. `<prefix>/claim_state` (retained `claimed`/`unclaimed`) says
//!   which the bridge is doing.
//!
//! The event payload on `<prefix>/last_event`, `<prefix>/events` and
//! `<prefix>/zone/<zone>/event` is one shape, [`EventStatePayload`].
//!
//! Each discovery config asks for its id above with `default_entity_id`
//! (sweep HA16), `<zone>` being the zone name as Home Assistant slugs it.
//! Home Assistant 2025.10 and later honors that key when it first registers
//! an entity; an entity already in its registry keeps the id it has, and an
//! older release ignores the key and names the entity from the device name
//! and the entity name (`binary_sensor.privacy_witness_kernel_pwk_<zone>_motion`).
//!
//! With `--fleet-peers-path` (daemon mode) the bridge also keeps the fleet
//! roll-call for the kernel's `GET /api/fleet`: it subscribes to the Canaries'
//! `securacv/<device_id>/{availability,status,health,chain,state,meta}`
//! topics, pins each device's public key on first sight, verifies its signed
//! `chain` publishes, and writes the summary file the kernel projects into
//! fleet rows (`witness_kernel::fleet_peers`). Nothing heard there is ever
//! republished; `events` and `sensing` are deliberately not subscribed.

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
use witness_kernel::fleet_peers::{
    PeerSummaryFile, PeerTable, FLEET_PEER_CLOCK_JUMP_SECS, FLEET_PEER_FORGET_SECS, FLEET_PEER_MAX,
    FLEET_PEER_TOPIC_FILTERS,
};
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
const IDENTITY_PATH: &str = "/api/identity";
const SIGN_ANNOUNCE_PATH: &str = "/api/sign-announce";
const DEFAULT_DISCOVERY_PREFIX: &str = "homeassistant";
const DEFAULT_STATE_PREFIX: &str = "witness";
const AVAILABILITY_TOPIC_SUFFIX: &str = "status";
/// `<prefix>/claim_state`: what the bridge is doing about the claim.
const CLAIM_STATE_TOPIC_SUFFIX: &str = "claim_state";
const PAYLOAD_ONLINE: &str = "online";
const PAYLOAD_OFFLINE: &str = "offline";
const PAYLOAD_CLAIMED: &str = "claimed";
const PAYLOAD_UNCLAIMED: &str = "unclaimed";
/// How long after the first ConnAck the daemon waits for a retained claim
/// before deciding whether to publish its discovery configs — a claim that
/// arrives in that window is honored without a flash of `pwk_*` entities.
const CLAIM_SETTLE: Duration = Duration::from_millis(2000);
/// How long startup waits for that first ConnAck before carrying on
/// without one (the poll loop copes with an absent broker by itself).
const CONNACK_WAIT: Duration = Duration::from_secs(10);
/// A claim older than this (by its `claimed_bucket_start`, against the
/// bridge's own clock) is treated as cleared: the integration republishes
/// hourly and on every load, so a claim nobody renews is a claim nobody
/// holds any more.
const CLAIM_FRESH_SECS: u64 = 24 * 60 * 60;
/// Clock skew tolerated on a claim's bucket start (one bucket ahead).
const CLAIM_SKEW_SECS: u64 = 600;
/// How long a clean shutdown waits for the broker to acknowledge its last
/// publishes (the announce clear, `offline`) before disconnecting anyway.
const SHUTDOWN_ACK_TIMEOUT: Duration = Duration::from_secs(2);

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
    #[arg(long, env = "WITNESS_API_TOKEN", hide_env_values = true)]
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
    #[arg(long, env = "MQTT_PASSWORD", hide_env_values = true)]
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

    /// Device identifier for Home Assistant. Defaults to pwk_<8 hex chars of
    /// sha256("securacv:ha-device-id:v1:" || DEVICE_KEY_SEED)> — the derivation
    /// the add-on and the sidecar use too; an install that predates the hashed
    /// form can pin its old id here so its entities are not re-created.
    #[arg(long, env = "HA_DEVICE_ID")]
    ha_device_id: Option<String>,

    /// Run as daemon, polling for new events periodically.
    #[arg(long, env = "DAEMON_MODE")]
    daemon: bool,

    /// Poll interval in seconds (daemon mode only).
    #[arg(long, env = "POLL_INTERVAL", default_value_t = 30)]
    poll_interval: u64,

    /// The kernel's time bucket size in seconds (what `frigate_bridge
    /// --bucket-size-secs` seals with; the launchers pass both the same
    /// value). The publish cursor starts ONE such bucket back on daemon
    /// start, so a restart republishes at most the previous and the
    /// current bucket — never the whole retained log.
    #[arg(long, env = "WITNESS_BUCKET_SIZE", default_value_t = 600)]
    bucket_size_secs: u64,

    /// Publish the whole retained log on daemon start (every event since
    /// bucket zero, as non-retained messages on `<prefix>/events`) instead
    /// of starting one bucket back. A migration tool, not a restart
    /// posture: Home Assistant shows but never fires a row older than two
    /// buckets, and the bridge's own zone counters count from this start.
    #[arg(long, env = "REPLAY_FROM_GENESIS")]
    replay_from_genesis: bool,

    /// Interval between automatic full sealed-log verifications in daemon
    /// mode (seconds; 0 disables). One verification also runs at daemon
    /// start so the chain-integrity sensor is populated immediately.
    #[arg(long, env = "VERIFY_INTERVAL_SECS", default_value_t = 86_400)]
    verify_interval_secs: u64,

    /// Disable Home Assistant discovery (publish raw events only).
    #[arg(long, env = "NO_DISCOVERY")]
    no_discovery: bool,

    /// Do not publish the retained announce (daemon mode).
    #[arg(long, env = "NO_ANNOUNCE")]
    no_announce: bool,

    /// The LAN posture of the kernel API as the announce reports it:
    /// `loopback` or `all`. The bridge cannot observe the bind itself, so
    /// the launcher that knows it says so here.
    #[arg(long, env = "ANNOUNCE_API_BIND", default_value = "loopback")]
    announce_api_bind: String,

    /// A host name or address the announce names for the kernel API.
    /// Unset, the announce carries no host (a host is a network identifier)
    /// and the integration prefills its own guess.
    #[arg(long, env = "ANNOUNCE_API_HOST")]
    announce_api_host: Option<String>,

    /// `http` or `https`: the scheme the kernel API answers on.
    #[arg(long, env = "ANNOUNCE_API_SCHEME", default_value = "http")]
    announce_api_scheme: String,

    /// How Home Assistant gets a credential: `required` (it must pair with
    /// a code, the sidecar) or `token_file` (it can read the rotating token
    /// file, the add-on). A mode word only — never whether a code is
    /// pending.
    #[arg(long, env = "ANNOUNCE_PAIRING", default_value = "required")]
    announce_pairing: String,

    /// Keep the fleet roll-call for the kernel's `GET /api/fleet` in this
    /// file (daemon mode). Point witnessd's `api.fleet_peers_path` (or
    /// WITNESS_FLEET_PEERS_PATH — the same variable serves both) at the same
    /// path. Unset: the bridge never subscribes to any Canary topic.
    #[arg(long, env = "WITNESS_FLEET_PEERS_PATH")]
    fleet_peers_path: Option<PathBuf>,

    /// UI mode for stderr progress (auto|plain|pretty).
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

/// Home Assistant MQTT Discovery config for a sensor.
#[derive(Serialize)]
struct HaSensorConfig {
    name: String,
    unique_id: String,
    default_entity_id: String,
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
    default_entity_id: String,
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
    default_entity_id: String,
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

/// The ONE event payload, on every event topic (`<prefix>/last_event`
/// retained, `<prefix>/events` and `<prefix>/zone/<zone>/event` not).
/// Field order is the wire order. Every time in it is a bucket start.
#[derive(Serialize)]
struct EventStatePayload {
    /// PascalCase variant name (`BoundaryCrossingObjectLarge`).
    event_type: String,
    zone_id: String,
    /// The exported (jittered) bucket, nested…
    time_bucket: TimeBucket,
    /// …and flattened, same numbers, for templates that cannot index.
    time_bucket_start: u64,
    time_bucket_size: u32,
    confidence: f32,
    /// `adapter` / `ha-bridged`; absent means device/kernel-attested.
    #[serde(skip_serializing_if = "Option::is_none")]
    attestation: Option<String>,
    kernel_version: String,
    ruleset_id: String,
    /// The bridge's device id (`pwk_…`).
    device_id: String,
    published_bucket_start: u64,
    published_bucket_size: u32,
    /// The KERNEL's own last `POST /verify` (this process's), a self-report
    /// the integration shows as "kernel-reported", never as its own
    /// verification. All three absent until a verify has run.
    #[serde(skip_serializing_if = "Option::is_none")]
    verified_bucket_start: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    verified_bucket_size: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    verified_ok: Option<bool>,
    /// The sealed-log row id, when the kernel said (the state views carry
    /// it); absent otherwise.
    #[serde(skip_serializing_if = "Option::is_none")]
    seq: Option<u64>,
}

/// The kernel's last self-verify as this process saw it (`POST /verify`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct LastVerify {
    bucket_start: u64,
    bucket_size: u32,
    ok: bool,
}

/// What the integration pins a kernel by: `GET /api/identity`'s id, version
/// and key.
#[derive(Clone, Debug, PartialEq, Eq)]
struct KernelIdentity {
    device_id: String,
    kernel_version: String,
    verifying_key: String,
}

/// The retained announce (`<prefix>/<device_id>/announce`). Field order is
/// the wire order; `sig` is last and absent on the bytes the kernel signs,
/// so `serde_json::to_vec(&payload_without_sig)` is exactly the message
/// `POST /api/sign-announce` receives and the integration re-serializes.
#[derive(Clone, Debug, PartialEq, Serialize, serde::Deserialize)]
struct AnnouncePayload {
    v: u32,
    device_id: String,
    kernel_version: String,
    verifying_key: String,
    api: AnnounceApi,
    /// `required` | `token_file` — a mode word, never "a code is pending".
    pairing: String,
    mqtt_topic_prefix: String,
    discovery_prefix: String,
    availability_topic: String,
    announced_bucket_start: u64,
    announced_bucket_size: u32,
    /// Ed25519 by the kernel key over `securacv-announce|v1|` + the
    /// sig-less compact payload, 128 hex. Filled in by the kernel.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    sig: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, serde::Deserialize)]
struct AnnounceApi {
    scheme: String,
    /// `all` | `loopback`.
    bind: String,
    port: u16,
    /// Only when `--announce-api-host` named one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    host: Option<String>,
}

fn announce_topic(state_prefix: &str, device_id: &str) -> String {
    format!("{state_prefix}/{device_id}/announce")
}

fn claim_topic(state_prefix: &str, device_id: &str) -> String {
    format!("{state_prefix}/claim/{device_id}")
}

/// The retained announce payload: the latest bytes published, re-published
/// on every ConnAck (a clean-start reconnect keeps retained messages at the
/// broker, but a broker that lost them — or one the bridge was pointed at
/// anew — should hear the announce again), and cleared at shutdown.
type AnnounceSlot = std::sync::Arc<std::sync::Mutex<Option<(String, Vec<u8>)>>>;

/// The claim topics subscribed on every ConnAck: the bridge's own device
/// id, plus the kernel's when `/api/identity` names a different one (a
/// legacy `HA_DEVICE_ID` pin), so a claim addressed either way is heard.
type ClaimTopics = std::sync::Arc<std::sync::Mutex<Vec<String>>>;

/// What the retained claim topic last said.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ClaimRecord {
    claimed_bucket_start: u64,
}

/// One message on the claim topic, as the bridge reads it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ClaimMessage {
    /// `{"v":1,"claimed":true,"nonce":…,"claimed_bucket_start":…}`.
    Claimed(ClaimRecord),
    /// The empty retained clear (or an explicit `claimed: false`).
    Cleared,
    /// Something else: ignored, state unchanged.
    Unparsable,
}

fn parse_claim_message(payload: &[u8]) -> ClaimMessage {
    if payload.is_empty() {
        return ClaimMessage::Cleared;
    }
    let Ok(value) = serde_json::from_slice::<serde_json::Value>(payload) else {
        return ClaimMessage::Unparsable;
    };
    match (
        value.get("claimed").and_then(|v| v.as_bool()),
        value.get("claimed_bucket_start").and_then(|v| v.as_u64()),
    ) {
        (Some(true), Some(claimed_bucket_start)) => ClaimMessage::Claimed(ClaimRecord {
            claimed_bucket_start,
        }),
        (Some(false), _) => ClaimMessage::Cleared,
        _ => ClaimMessage::Unparsable,
    }
}

/// Whether a claim is current against this clock: not older than
/// [`CLAIM_FRESH_SECS`], and not from a clock more than one bucket ahead.
fn claim_is_fresh(record: &ClaimRecord, now: u64) -> bool {
    record.claimed_bucket_start <= now.saturating_add(CLAIM_SKEW_SECS)
        && now.saturating_sub(record.claimed_bucket_start) < CLAIM_FRESH_SECS
}

/// The bridge's side of the claim: the last record heard, and whether the
/// discovery configs are currently withdrawn because of it. Honoring is
/// re-evaluated every settle (a fresh claim ages out at 24 h without any
/// message), so `settle` is what reports a transition to act on.
#[derive(Debug, Default)]
struct ClaimTracker {
    record: Option<ClaimRecord>,
    honored: bool,
}

/// A change the bridge must act on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ClaimTransition {
    /// Withdraw the discovery configs (a fresh claim appeared).
    Withdraw,
    /// Republish them (the claim was cleared, or went stale).
    Republish,
}

impl ClaimTracker {
    fn observe(&mut self, payload: &[u8]) {
        match parse_claim_message(payload) {
            ClaimMessage::Claimed(record) => self.record = Some(record),
            ClaimMessage::Cleared => self.record = None,
            ClaimMessage::Unparsable => {
                log::warn!("ignoring an unparsable claim payload; claim state unchanged")
            }
        }
    }

    fn honored_now(&self, now: u64) -> bool {
        self.record
            .is_some_and(|record| claim_is_fresh(&record, now))
    }

    /// Bring `honored` in line with the record and the clock; the
    /// transition, if that changed anything.
    fn settle(&mut self, now: u64) -> Option<ClaimTransition> {
        let should = self.honored_now(now);
        if should == self.honored {
            return None;
        }
        self.honored = should;
        Some(if should {
            ClaimTransition::Withdraw
        } else {
            ClaimTransition::Republish
        })
    }
}

/// Zone state for tracking event counts.
/// Command publishes forwarded to the daemon poll loop as (topic, payload).
/// `(topic, payload, retained)` — `retained` is the wire flag the broker sets
/// only when replaying a retained message to a new subscription.
///
/// Bounded: the eventloop thread `try_send`s and drops on a full queue, so a
/// broker that floods the fleet filters while the poll loop is inside an HTTP
/// round trip costs at most [`COMMAND_QUEUE_DEPTH`] messages of memory, not
/// an unbounded backlog. A dropped fleet publish is a missed observation the
/// next one repairs; a dropped verify command is re-issued by the interval.
type CommandTx = mpsc::SyncSender<(String, Vec<u8>, bool)>;

/// Depth of the eventloop → poll-loop queue. Sized for a burst of every
/// fleet topic from a full [`FLEET_PEER_MAX`]-device table with headroom.
const COMMAND_QUEUE_DEPTH: usize = 512;

/// Daemon-mode eventloop wiring. Bundled so the reconnect handler can, on
/// every ConnAck, both re-subscribe the command filter (clean-start drops
/// subscriptions across reconnects) and re-assert the retained availability
/// `online` — the broker published the retained LWT `offline` when the link
/// dropped, and `run_daemon` publishes `online` only once at startup, so
/// without this HA would show the device permanently unavailable after the
/// first reconnect.
struct DaemonWiring {
    commands: CommandTx,
    /// `<prefix>/cmd/` — the subtree `cmd_filter` subscribes to, used to
    /// decide what is worth queueing.
    cmd_prefix: String,
    cmd_filter: String,
    availability_topic: String,
    /// The fleet roll-call filters (`FLEET_PEER_TOPIC_FILTERS`) when
    /// `--fleet-peers-path` is set; empty otherwise.
    fleet_filters: Vec<String>,
    /// `<prefix>/claim/` — what the claim topics below start with.
    claim_prefix: String,
    /// The exact claim topics to (re)subscribe on every ConnAck.
    claim_topics: ClaimTopics,
    /// The announce to re-publish on every ConnAck, once there is one.
    announce: AnnounceSlot,
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
    /// QoS 1 publishes the broker has acknowledged, counted by the
    /// eventloop thread. A clean shutdown waits on it so its last publishes
    /// (the announce clear, `offline`) are on the broker before the socket
    /// closes — `Client::publish` only queues.
    acked: std::sync::Arc<std::sync::atomic::AtomicU64>,
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
        let acked = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
        let thread_shutdown = std::sync::Arc::clone(&shutdown);
        let thread_connected = std::sync::Arc::clone(&connected);
        let thread_acked = std::sync::Arc::clone(&acked);
        let thread_client = client.clone();
        let handle = std::thread::spawn(move || {
            const BACKOFF_START: Duration = Duration::from_secs(2);
            const BACKOFF_CAP: Duration = Duration::from_secs(60);
            let mut backoff = BACKOFF_START;
            let mut outage_logged = false;
            // One warn/info pair per full episode of the inbound queue: set on
            // the first drop, cleared when a publish is accepted again after
            // QUEUE_DRAIN_QUIET of no drops (so a queue hovering at full does
            // not flap the pair on every message).
            let mut queue_full_logged = false;
            let mut queue_last_full: Option<Instant> = None;
            const QUEUE_DRAIN_QUIET: Duration = Duration::from_secs(5);
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
                        queue_full_logged = false;
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
                            for filter in &w.fleet_filters {
                                if let Err(e) = thread_client.subscribe(filter, QoS::AtLeastOnce) {
                                    log::warn!(
                                        "MQTT fleet (re)subscribe failed for {}: {}",
                                        filter,
                                        e
                                    );
                                }
                            }
                            // The claim topics (exact, no wildcard) and the
                            // announce: both are lost or stale across a
                            // reconnect the same way the command filter is.
                            let claim_topics =
                                w.claim_topics.lock().map(|t| t.clone()).unwrap_or_default();
                            for topic in claim_topics {
                                if let Err(e) = thread_client.subscribe(&topic, QoS::AtLeastOnce) {
                                    log::warn!(
                                        "MQTT claim (re)subscribe failed for {}: {}",
                                        topic,
                                        e
                                    );
                                }
                            }
                            let announce = w.announce.lock().map(|a| a.clone()).unwrap_or(None);
                            if let Some((topic, bytes)) = announce {
                                if let Err(e) =
                                    thread_client.publish(&topic, QoS::AtLeastOnce, true, bytes)
                                {
                                    log::warn!("MQTT announce re-publish failed: {}", e);
                                }
                            }
                        }
                    }
                    Ok(Event::Incoming(Incoming::PubAck(_))) => {
                        thread_acked.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                    }
                    Ok(Event::Incoming(Incoming::Publish(publish))) => {
                        if let Some(w) = &daemon {
                            let topic = match std::str::from_utf8(&publish.topic) {
                                Ok(topic) => topic.to_string(),
                                Err(_) => continue,
                            };
                            // Only what the poll loop handles is queued at
                            // all: the command subtree, the claim topic, and
                            // the fleet roll-call topics when that is on.
                            let wanted = topic.starts_with(&w.cmd_prefix)
                                || topic.starts_with(&w.claim_prefix)
                                || (!w.fleet_filters.is_empty() && topic.starts_with("securacv/"));
                            if !wanted {
                                continue;
                            }
                            match w.commands.try_send((
                                topic,
                                publish.payload.to_vec(),
                                publish.retain,
                            )) {
                                Ok(()) => {
                                    if queue_full_logged
                                        && queue_last_full
                                            .is_some_and(|t| t.elapsed() >= QUEUE_DRAIN_QUIET)
                                    {
                                        log::info!(
                                            "MQTT inbound queue drained; publishes are \
                                             being queued again"
                                        );
                                        queue_full_logged = false;
                                    }
                                }
                                Err(mpsc::TrySendError::Full(_)) => {
                                    queue_last_full = Some(Instant::now());
                                    if !queue_full_logged {
                                        log::warn!(
                                            "MQTT inbound queue full ({} messages); \
                                             dropping publishes until the poll loop \
                                             catches up",
                                            COMMAND_QUEUE_DEPTH
                                        );
                                        queue_full_logged = true;
                                    }
                                }
                                Err(mpsc::TrySendError::Disconnected(_)) => break,
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
            acked,
        }
    }

    /// True while the eventloop holds a live broker connection (last saw a
    /// ConnAck, no error since). The daemon loop gates publishing on this.
    fn is_connected(&self) -> bool {
        self.connected.load(std::sync::atomic::Ordering::SeqCst)
    }

    /// QoS 1 publishes acknowledged so far.
    fn acked(&self) -> u64 {
        self.acked.load(std::sync::atomic::Ordering::SeqCst)
    }

    /// Wait until `acked()` reaches `target`, or `timeout` passes. Returns
    /// whether it did — a clean shutdown logs the difference and moves on.
    fn wait_for_acks(&self, target: u64, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        while self.acked() < target {
            if Instant::now() >= deadline || !self.is_connected() {
                return false;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        true
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

    // Generate device ID from environment or use provided. Never a prefix of
    // the seed itself: this id lands in retained MQTT discovery topics that
    // every broker client can read, and a raw seed has no safe-to-publish
    // prefix (nor a guaranteed length). A short hash is stable per seed —
    // `witness_kernel::ha_device_id`, the one derivation the kernel's
    // `/api/identity`, the add-on and the sidecar share.
    let device_id = args.ha_device_id.clone().unwrap_or_else(|| {
        std::env::var("DEVICE_KEY_SEED")
            .map(|seed| witness_kernel::ha_device_id(&seed))
            .unwrap_or_else(|_| "pwk_default".to_string())
    });

    validate_announce_args(&args)?;

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

/// The announce flags take words, not free text; refuse anything else at
/// startup so a typo cannot publish a posture the integration misreads.
fn validate_announce_args(args: &Args) -> Result<()> {
    if !matches!(args.announce_api_bind.as_str(), "loopback" | "all") {
        return Err(anyhow!(
            "--announce-api-bind must be `loopback` or `all`, got {:?}",
            args.announce_api_bind
        ));
    }
    if !matches!(args.announce_api_scheme.as_str(), "http" | "https") {
        return Err(anyhow!(
            "--announce-api-scheme must be `http` or `https`, got {:?}",
            args.announce_api_scheme
        ));
    }
    if !matches!(args.announce_pairing.as_str(), "required" | "token_file") {
        return Err(anyhow!(
            "--announce-pairing must be `required` or `token_file`, got {:?}",
            args.announce_pairing
        ));
    }
    if let Some(host) = &args.announce_api_host {
        if host.trim().is_empty() || host.chars().any(|c| c.is_control() || c.is_whitespace()) {
            return Err(anyhow!(
                "--announce-api-host must be a host name or address without whitespace"
            ));
        }
    }
    Ok(())
}

fn run_oneshot(ctx: &RunContext<'_>) -> Result<()> {
    let artifact = {
        let _stage = ctx.ui.stage("Fetch event view");
        let token = ctx.tokens.current()?;
        // The receipt-less state view from the start of the retained log:
        // a bridge run is a projection onto the owner's broker, not a
        // disclosure act, so it leaves no export receipt behind.
        fetch_events_since(ctx.api_addr, &token, 0)?
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
        publish_events(
            &conn.client,
            &ctx.args.mqtt_topic_prefix,
            ctx.device_id,
            &events,
        )?;
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

    // SIGINT/SIGTERM end the loop cleanly: the retained announce is cleared
    // and `offline` is published BEFORE the socket closes (a clean DISCONNECT
    // suppresses the LWT, so the availability word has to be said here).
    let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    {
        let stop = std::sync::Arc::clone(&stop);
        ctrlc::set_handler(move || {
            stop.store(true, std::sync::atomic::Ordering::SeqCst);
        })
        .context("installing the shutdown handler")?;
    }

    let (cmd_tx, cmd_rx) = mpsc::sync_channel::<(String, Vec<u8>, bool)>(COMMAND_QUEUE_DEPTH);
    // Scoped to the command subtree only — this daemon must never consume
    // event or sensor traffic. Subscribed from the eventloop thread on every
    // ConnAck so it survives broker reconnects. Beyond it: the claim topic
    // (exact, this device's id), and the fleet roll-call
    // (`--fleet-peers-path`): presence/health/chain/meta topics only, never
    // `events` or `sensing` (see FLEET_PEER_TOPIC_FILTERS).
    let cmd_filter = format!("{}/cmd/#", ctx.args.mqtt_topic_prefix);
    let mut peers = ctx
        .args
        .fleet_peers_path
        .clone()
        .map(FleetPeerTracker::open);
    let fleet_filters: Vec<String> = if peers.is_some() {
        FLEET_PEER_TOPIC_FILTERS
            .iter()
            .map(|f| f.to_string())
            .collect()
    } else {
        Vec::new()
    };
    let claim_topics: ClaimTopics = std::sync::Arc::new(std::sync::Mutex::new(vec![claim_topic(
        &ctx.args.mqtt_topic_prefix,
        ctx.device_id,
    )]));
    let announce_slot: AnnounceSlot = std::sync::Arc::new(std::sync::Mutex::new(None));
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
                cmd_prefix: format!("{}/cmd/", ctx.args.mqtt_topic_prefix),
                cmd_filter,
                availability_topic: ctx.availability_topic.to_string(),
                fleet_filters,
                claim_prefix: format!("{}/claim/", ctx.args.mqtt_topic_prefix),
                claim_topics: std::sync::Arc::clone(&claim_topics),
                announce: std::sync::Arc::clone(&announce_slot),
            }),
        )?
    };

    let verify_cmd_topic = format!("{}/cmd/verify", ctx.args.mqtt_topic_prefix);
    let claim_prefix = format!("{}/claim/", ctx.args.mqtt_topic_prefix);

    // Publish availability online (retained)
    mqtt_publish_qos1(
        &conn.client,
        ctx.availability_topic,
        PAYLOAD_ONLINE.as_bytes(),
        true,
    )?;
    log::info!("Published online status to {}", ctx.availability_topic);

    // The signed announce: identity from the kernel, signature from the
    // kernel, bytes to the broker. A failed fetch is retried every poll
    // cycle — the announce is never published without the key.
    let mut announced = ctx.args.no_announce;
    let mut identity: Option<KernelIdentity> = None;
    if !announced {
        match try_announce(ctx, &conn, &announce_slot, &claim_topics) {
            Ok(id) => {
                identity = Some(id);
                announced = true;
            }
            Err(e) => log::warn!("announce deferred to the next poll cycle: {:#}", e),
        }
    }

    // Let a retained claim arrive before deciding whether to publish the
    // discovery configs: a kernel the integration already owns must not
    // flash its `pwk_*` entities into Home Assistant on every restart. The
    // settle window is counted from the first ConnAck — the claim topic is
    // subscribed there, and a retained claim is delivered right after — and
    // a broker that has not answered within `CONNACK_WAIT` does not hold
    // startup: the loop below handles an absent broker on its own.
    let mut claim = ClaimTracker::default();
    let mut discovered_zones: HashSet<String> = HashSet::new();
    let connack_until = Instant::now() + CONNACK_WAIT;
    let mut settle_until: Option<Instant> = None;
    loop {
        if stop.load(std::sync::atomic::Ordering::SeqCst) {
            break;
        }
        if settle_until.is_none() && conn.is_connected() {
            settle_until = Some(Instant::now() + CLAIM_SETTLE);
        }
        let deadline = settle_until.unwrap_or(connack_until);
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            break;
        }
        match cmd_rx.recv_timeout(remaining.min(Duration::from_millis(100))) {
            Ok((topic, payload, retained)) => {
                if topic.starts_with(&claim_prefix) {
                    claim.observe(&payload);
                } else if let Some(tracker) = peers.as_mut() {
                    if topic.starts_with("securacv/") {
                        tracker.observe(&topic, &payload, retained);
                    }
                }
                // A verify press during the settle window is served below,
                // by the initial verification.
            }
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    // The initial decision is a settle like any other, from `honored =
    // false`: a fresh claim withdraws nothing (nothing is published yet)
    // and no claim publishes the static set.
    claim.settle(unix_now_secs());
    if !ctx.args.no_discovery {
        if claim.honored {
            log::info!(
                "claimed by the SecuraCV integration; MQTT discovery withheld, state topics continue"
            );
        } else {
            publish_static_discovery(
                &conn.client,
                &ctx.args.ha_discovery_prefix,
                &ctx.args.mqtt_topic_prefix,
                ctx.availability_topic,
                ctx.device_id,
                ctx.device_info,
            )?;
        }
    }
    publish_claim_state(&conn.client, &ctx.args.mqtt_topic_prefix, claim.honored)?;

    // Initial verification populates the chain-integrity sensor right away.
    let verify_interval = ctx.args.verify_interval_secs;
    let mut last_auto_verify = Instant::now();
    let mut last_verify: Option<LastVerify> = None;
    if verify_interval > 0 {
        match run_verify_and_publish(ctx, &conn.client) {
            Ok(outcome) => last_verify = Some(outcome),
            Err(e) => log::warn!("Initial verification failed to run: {}", e),
        }
    }

    let mut zone_states: HashMap<String, ZoneState> = HashMap::new();
    let mut publish_cursor = initial_publish_cursor(
        unix_now_secs(),
        ctx.args.bucket_size_secs,
        ctx.args.replay_from_genesis,
    );
    match publish_cursor {
        Some(cursor) => log::info!(
            "Publishing events from bucket {} (one bucket back; --replay-from-genesis publishes the whole log)",
            cursor.bucket_start_epoch_s
        ),
        None => log::warn!(
            "--replay-from-genesis: every retained event is republished on {}/events as a non-retained message",
            ctx.args.mqtt_topic_prefix
        ),
    }

    while !stop.load(std::sync::atomic::Ordering::SeqCst) {
        // Each poll tick flushes whatever the roll-call heard since the last
        // one (the wait loop below also flushes, rate-limited, as it hears).
        if let Some(tracker) = peers.as_mut() {
            tracker.flush_if_due(true);
        }

        // A claim ages out at 24 h without a message; re-check each tick.
        if let Some(transition) = claim.settle(unix_now_secs()) {
            apply_claim_transition(ctx, &conn.client, transition, &discovered_zones)?;
        }

        // Only fetch+publish while the broker is reachable. During an outage
        // the eventloop thread is reconnecting with backoff and cannot drain
        // the bounded request queue, so a blocking publish would wedge this
        // loop after ~10 queued messages (docs/strategy/12 FR-4). Skipping the
        // cycle is safe: retained state topics self-heal on the next poll, and
        // events accumulated during the outage publish as a catch-up once the
        // link (and, via ConnAck, availability) is restored. Command handling
        // and shutdown stay responsive through the wait loop below.
        if conn.is_connected() {
            if !announced {
                match try_announce(ctx, &conn, &announce_slot, &claim_topics) {
                    Ok(id) => {
                        identity = Some(id);
                        announced = true;
                    }
                    Err(e) => log::warn!("announce still deferred: {:#}", e),
                }
            }

            // The receipt-less incremental view from the cursor's bucket: the
            // newest bucket already seen plus anything newer. The kernel
            // writes no export receipt for it, so a 30 s poll costs the
            // receipt ledger nothing (see `Kernel::view_events`).
            let since_bucket = publish_cursor.map(|c| c.bucket_start_epoch_s).unwrap_or(0);
            match ctx
                .tokens
                .current()
                .and_then(|token| fetch_events_since(ctx.api_addr, &token, since_bucket))
            {
                Ok(artifact) => {
                    let events = flatten_export_events(&artifact);

                    // Filter to only new events. The cursor remembers how many
                    // events of the newest bucket were already published, so a
                    // 30 s poll inside a 10-minute bucket does not republish the
                    // whole bucket (and re-count every zone) every time.
                    let new_events = select_new_events(&events, &mut publish_cursor);

                    if !new_events.is_empty() {
                        // Discover new zones. Tracked whether or not the
                        // integration holds the claim — a cleared claim
                        // republishes every zone seen — but published only
                        // while unclaimed.
                        if !ctx.args.no_discovery {
                            for event in &new_events {
                                let zone = extract_zone_name(&event.zone_id);
                                if discovered_zones.insert(zone.clone()) && !claim.honored {
                                    publish_zone_discovery(
                                        &conn.client,
                                        &ctx.args.ha_discovery_prefix,
                                        &ctx.args.mqtt_topic_prefix,
                                        ctx.availability_topic,
                                        ctx.device_id,
                                        ctx.device_info,
                                        &zone,
                                    )?;
                                }
                            }
                        }

                        // Publish events and update zone states
                        for event in &new_events {
                            let zone = extract_zone_name(&event.zone_id);

                            // Update zone state
                            let state = zone_states.entry(zone.clone()).or_default();
                            state.event_count += 1;
                            state.last_event_time = unix_now_secs();

                            // Publish event
                            publish_single_event(
                                &conn.client,
                                &ctx.args.mqtt_topic_prefix,
                                ctx.device_id,
                                event,
                                &zone,
                                last_verify,
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
                            let payload =
                                build_event_state_payload(last, ctx.device_id, last_verify)?;
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
                match run_verify_and_publish(ctx, &conn.client) {
                    Ok(outcome) => last_verify = Some(outcome),
                    Err(e) => log::warn!("Scheduled verification failed to run: {}", e),
                }
            }
        } // end `if conn.is_connected()`

        // Sleep until the next poll, waking early for button presses, claim
        // changes and the shutdown signal.
        let deadline = Instant::now() + Duration::from_secs(ctx.args.poll_interval);
        loop {
            if stop.load(std::sync::atomic::Ordering::SeqCst) {
                break;
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                break;
            }
            let slice = remaining.min(Duration::from_millis(250));
            match cmd_rx.recv_timeout(slice) {
                Ok((topic, _payload, _)) if topic == verify_cmd_topic => {
                    log::info!("Verify command received via {}", topic);
                    match run_verify_and_publish(ctx, &conn.client) {
                        Ok(outcome) => last_verify = Some(outcome),
                        Err(e) => log::warn!("Verification failed to run: {}", e),
                    }
                    last_auto_verify = Instant::now();
                }
                Ok((topic, payload, _)) if topic.starts_with(&claim_prefix) => {
                    claim.observe(&payload);
                    if let Some(transition) = claim.settle(unix_now_secs()) {
                        apply_claim_transition(ctx, &conn.client, transition, &discovered_zones)?;
                    }
                }
                Ok((topic, payload, retained)) => match peers.as_mut() {
                    Some(tracker) if topic.starts_with("securacv/") => {
                        tracker.observe(&topic, &payload, retained);
                        tracker.flush_if_due(false);
                    }
                    _ => log::debug!("Ignoring unknown command topic {}", topic),
                },
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
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

    log::info!("shutdown signal received; clearing the announce and going offline");
    shutdown_cleanly(ctx, conn, &announce_slot, identity.as_ref())
}

/// Fetch the kernel's identity, have the kernel sign the announce built
/// from it, publish it retained and remember it for ConnAck re-publishes.
/// Returns the identity so the claim topic can follow the kernel's id.
fn try_announce(
    ctx: &RunContext<'_>,
    conn: &MqttRuntime,
    slot: &AnnounceSlot,
    claim_topics: &ClaimTopics,
) -> Result<KernelIdentity> {
    let token = ctx.tokens.current()?;
    let identity = fetch_identity(ctx.api_addr, &token)?;
    if identity.device_id != ctx.device_id {
        log::warn!(
            "the kernel names itself {} but this bridge's device id is {}; the announce \
             carries the kernel's (set --ha-device-id to the kernel's id, or drop the override)",
            identity.device_id,
            ctx.device_id
        );
        // Hear a claim addressed to the kernel's id as well as to ours.
        let extra = claim_topic(&ctx.args.mqtt_topic_prefix, &identity.device_id);
        if let Ok(mut topics) = claim_topics.lock() {
            if !topics.contains(&extra) {
                topics.push(extra.clone());
                if let Err(e) = conn.client.subscribe(&extra, QoS::AtLeastOnce) {
                    log::warn!("MQTT claim subscribe failed for {}: {}", extra, e);
                }
            }
        }
    }
    let unsigned = build_announce_payload(ctx, &identity)?;
    let unsigned_bytes = serde_json::to_vec(&unsigned)?;
    let token = ctx.tokens.current()?;
    let sig = sign_announce(ctx.api_addr, &token, &unsigned_bytes)?;
    let signed = AnnouncePayload {
        sig: Some(sig),
        ..unsigned
    };
    let bytes = serde_json::to_vec(&signed)?;
    let topic = announce_topic(&ctx.args.mqtt_topic_prefix, &identity.device_id);
    mqtt_publish_qos1(&conn.client, &topic, &bytes, true)?;
    if let Ok(mut guard) = slot.lock() {
        *guard = Some((topic.clone(), bytes));
    }
    log::info!(
        "Published signed announce to {} (pairing: {}, api: {} on port {})",
        topic,
        signed.pairing,
        signed.api.bind,
        signed.api.port
    );
    Ok(identity)
}

/// The announce for this kernel, unsigned (`sig: None`).
fn build_announce_payload(
    ctx: &RunContext<'_>,
    identity: &KernelIdentity,
) -> Result<AnnouncePayload> {
    let announced = TimeBucket::now_10min()?;
    Ok(AnnouncePayload {
        v: 1,
        device_id: identity.device_id.clone(),
        kernel_version: identity.kernel_version.clone(),
        verifying_key: identity.verifying_key.clone(),
        api: AnnounceApi {
            scheme: ctx.args.announce_api_scheme.clone(),
            bind: ctx.args.announce_api_bind.clone(),
            port: ctx.api_addr.port(),
            host: ctx.args.announce_api_host.clone(),
        },
        pairing: ctx.args.announce_pairing.clone(),
        mqtt_topic_prefix: ctx.args.mqtt_topic_prefix.clone(),
        discovery_prefix: ctx.args.ha_discovery_prefix.clone(),
        availability_topic: ctx.availability_topic.to_string(),
        announced_bucket_start: announced.start_epoch_s,
        announced_bucket_size: announced.size_s,
        sig: None,
    })
}

/// Act on a claim transition: withdraw every discovery config this bridge
/// owns (empty retained payloads — the state topics are untouched), or
/// republish them all; then say which on `<prefix>/claim_state`. A no-op
/// for discovery under `--no-discovery` (nothing to withdraw), the state
/// topic is still kept honest.
fn apply_claim_transition<P: Publish>(
    ctx: &RunContext<'_>,
    publisher: &P,
    transition: ClaimTransition,
    zones: &HashSet<String>,
) -> Result<()> {
    if !ctx.args.no_discovery {
        match transition {
            ClaimTransition::Withdraw => {
                withdraw_discovery(
                    publisher,
                    &ctx.args.ha_discovery_prefix,
                    &ctx.args.mqtt_topic_prefix,
                    ctx.availability_topic,
                    ctx.device_id,
                    ctx.device_info,
                    zones,
                )?;
                log::info!(
                    "claimed by the SecuraCV integration; MQTT discovery withdrawn, state topics continue"
                );
            }
            ClaimTransition::Republish => {
                republish_discovery(
                    publisher,
                    &ctx.args.ha_discovery_prefix,
                    &ctx.args.mqtt_topic_prefix,
                    ctx.availability_topic,
                    ctx.device_id,
                    ctx.device_info,
                    zones,
                )?;
                log::info!("claim cleared; MQTT discovery republished");
            }
        }
    }
    publish_claim_state(
        publisher,
        &ctx.args.mqtt_topic_prefix,
        transition == ClaimTransition::Withdraw,
    )
}

fn publish_claim_state<P: Publish>(publisher: &P, state_prefix: &str, claimed: bool) -> Result<()> {
    let topic = format!("{state_prefix}/{CLAIM_STATE_TOPIC_SUFFIX}");
    let word = if claimed {
        PAYLOAD_CLAIMED
    } else {
        PAYLOAD_UNCLAIMED
    };
    publisher.publish_qos1(&topic, word.as_bytes(), true)
}

/// Every discovery config topic this bridge owns right now: the static
/// four plus the two per discovered zone — exactly the topics
/// `publish_static_discovery` and `publish_zone_discovery` publish, derived
/// from the same builders so the withdraw list cannot drift from them.
fn owned_discovery_topics(
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zones: &HashSet<String>,
) -> Result<Vec<String>> {
    let mut configs = static_discovery_configs(
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;
    let mut sorted: Vec<&String> = zones.iter().collect();
    sorted.sort();
    for zone in sorted {
        configs.extend(zone_discovery_configs(
            discovery_prefix,
            state_prefix,
            availability_topic,
            device_id,
            device_info,
            zone,
        )?);
    }
    Ok(configs.into_iter().map(|c| c.topic).collect())
}

/// An empty retained payload on every owned discovery config topic: Home
/// Assistant removes the entity, the state topics behind it keep flowing.
fn withdraw_discovery<P: Publish>(
    publisher: &P,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zones: &HashSet<String>,
) -> Result<()> {
    for topic in owned_discovery_topics(
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
        zones,
    )? {
        publisher.publish_qos1(&topic, b"", true)?;
    }
    Ok(())
}

/// The static set and every discovered zone, published again.
fn republish_discovery<P: Publish>(
    publisher: &P,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zones: &HashSet<String>,
) -> Result<()> {
    publish_static_discovery(
        publisher,
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;
    let mut sorted: Vec<&String> = zones.iter().collect();
    sorted.sort();
    for zone in sorted {
        publish_zone_discovery(
            publisher,
            discovery_prefix,
            state_prefix,
            availability_topic,
            device_id,
            device_info,
            zone,
        )?;
    }
    Ok(())
}

/// Clear the retained announce, say `offline` on the availability topic,
/// wait (bounded) for the broker to acknowledge both, then disconnect. The
/// LWT on the availability topic is unchanged — this is the clean-exit
/// path saying the same word the broker would have said for a crash.
fn shutdown_cleanly(
    ctx: &RunContext<'_>,
    conn: MqttRuntime,
    announce_slot: &AnnounceSlot,
    identity: Option<&KernelIdentity>,
) -> Result<()> {
    if let Ok(mut guard) = announce_slot.lock() {
        *guard = None;
    }
    let before = conn.acked();
    let mut expected = 0u64;
    if !ctx.args.no_announce {
        let device_id = identity
            .map(|id| id.device_id.as_str())
            .unwrap_or(ctx.device_id);
        let topic = announce_topic(&ctx.args.mqtt_topic_prefix, device_id);
        if let Err(e) = mqtt_publish_qos1(&conn.client, &topic, b"", true) {
            log::warn!("could not clear the retained announce on {}: {}", topic, e);
        } else {
            expected += 1;
        }
    }
    match mqtt_publish_qos1(
        &conn.client,
        ctx.availability_topic,
        PAYLOAD_OFFLINE.as_bytes(),
        true,
    ) {
        Ok(()) => expected += 1,
        Err(e) => log::warn!("could not publish offline availability: {}", e),
    }
    if expected > 0 && !conn.wait_for_acks(before + expected, SHUTDOWN_ACK_TIMEOUT) {
        log::warn!(
            "broker did not acknowledge the shutdown publishes within {:?}; disconnecting anyway \
             (recover a stale announce with `mosquitto_pub -r -n -t <prefix>/<device_id>/announce`)",
            SHUTDOWN_ACK_TIMEOUT
        );
    }
    conn.disconnect()
}

/// How often the roll-call summary is rewritten while messages keep
/// arriving. Every poll tick flushes regardless; this only bounds the write
/// rate under a chatty fleet.
const FLEET_PEERS_FLUSH_INTERVAL: Duration = Duration::from_secs(5);

/// The fleet roll-call (`--fleet-peers-path`): the [`PeerTable`] the
/// subscribed Canary topics feed, flushed atomically to the summary file the
/// kernel's `/api/fleet` reads. Rehydrated from that file on start so pins
/// survive a bridge restart.
struct FleetPeerTracker {
    path: PathBuf,
    table: PeerTable,
    dirty: bool,
    last_flush: Option<Instant>,
    /// Evictions already reported, so each displacement is logged once.
    evictions_logged: u64,
    /// A clock jump has been reported (once per process).
    jump_logged: bool,
}

impl FleetPeerTracker {
    fn open(path: PathBuf) -> Self {
        let table = match PeerSummaryFile::read(&path) {
            Ok(Some(summary)) => {
                // No expiry at startup: the file's pins are the one thing a
                // restart must keep. Expiry (flush time) can only ever forget
                // ids that hold neither a pin nor a verified length, and the
                // pass that straddles a clock jump is skipped besides — so a
                // host clock that is wrong at boot (a dead RTC, a bad NTP
                // answer) cannot cost a single pin.
                let table = PeerTable::from_summary(summary);
                log::info!(
                    "fleet roll-call: rehydrated {} peer(s) from {}",
                    table.len(),
                    path.display()
                );
                table
            }
            Ok(None) => PeerTable::default(),
            Err(err) => {
                log::warn!(
                    "fleet roll-call: ignoring unreadable {} ({err:#}); starting empty, \
                     so every device's key is pinned afresh on its next health publish",
                    path.display()
                );
                PeerTable::default()
            }
        };
        Self {
            path,
            table,
            // Write once at start even if nothing is heard, so a kernel
            // pointed at the path sees a valid (possibly empty) summary.
            dirty: true,
            last_flush: None,
            evictions_logged: 0,
            jump_logged: false,
        }
    }

    fn observe(&mut self, topic: &str, payload: &[u8], retained: bool) {
        if self
            .table
            .observe(topic, payload, retained, unix_now_secs())
        {
            self.dirty = true;
        }
        let evictions = self.table.evictions();
        // Each displacement drops that id's pin; say so, because a steady
        // stream of these is what a broker flood looks like — but a flood
        // must not turn into one warning per inbound publish, so the line is
        // emitted on the first displacement and then each time the count
        // doubles (1, 2, 4, 8, …), which keeps the journal readable while the
        // number stays honest.
        if evictions > self.evictions_logged && evictions >= self.evictions_logged * 2 {
            log::warn!(
                "fleet roll-call: at the {}-id cap, {} id(s) displaced so far \
                 (a displaced Canary is re-pinned on its next health publish)",
                FLEET_PEER_MAX,
                evictions
            );
            self.evictions_logged = evictions;
        }
    }

    fn flush_if_due(&mut self, force: bool) {
        let now = unix_now_secs();
        if self.table.clock_jumped_since(now) && !self.jump_logged {
            log::warn!(
                "fleet roll-call: the clock moved more than {} h since the table last saw it; \
                 no id is expired this round (a pinned id is never expired by the clock)",
                FLEET_PEER_CLOCK_JUMP_SECS / 3600
            );
            self.jump_logged = true;
        }
        let forgotten = self.table.prune(now);
        if forgotten > 0 {
            log::info!(
                "fleet roll-call: {} never-proven id(s) unheard for {} days forgotten",
                forgotten,
                FLEET_PEER_FORGET_SECS / 86_400
            );
            self.dirty = true;
        }
        if !self.dirty {
            return;
        }
        let due = force
            || self
                .last_flush
                .is_none_or(|at| at.elapsed() >= FLEET_PEERS_FLUSH_INTERVAL);
        if !due {
            return;
        }
        self.last_flush = Some(Instant::now());
        match self.table.summary(unix_now_secs()).write_atomic(&self.path) {
            Ok(()) => self.dirty = false,
            Err(err) => log::warn!(
                "fleet roll-call: could not write {}: {err:#}",
                self.path.display()
            ),
        }
    }
}

fn unix_now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// POST /verify on the witness API and publish the outcome:
/// `<prefix>/chain_problem` (retained ON/OFF for the `problem`-class binary
/// sensor — ON means verification failed) and the full report JSON on
/// `<prefix>/chain_problem/attrs` (retained, surfaced as HA attributes).
fn run_verify_and_publish(ctx: &RunContext<'_>, client: &Client) -> Result<LastVerify> {
    let token = ctx.tokens.current()?;
    let report = post_verify(ctx.api_addr, &token)?;
    let parsed = serde_json::from_slice::<serde_json::Value>(&report).ok();
    let field = |name: &str| parsed.as_ref().and_then(|v| v.get(name).cloned());
    let chain_valid = field("chain_valid")
        .and_then(|b| b.as_bool())
        .unwrap_or(false);
    let outcome = LastVerify {
        bucket_start: field("verified_at_bucket_start")
            .and_then(|v| v.as_u64())
            .unwrap_or(0),
        bucket_size: field("verified_at_bucket_size")
            .and_then(|v| v.as_u64())
            .and_then(|v| u32::try_from(v).ok())
            .unwrap_or(0),
        ok: chain_valid,
    };

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
    Ok(outcome)
}

/// One retained Home Assistant discovery config: its topic and its JSON body.
struct DiscoveryConfig {
    topic: String,
    body: Vec<u8>,
}

impl DiscoveryConfig {
    fn new<T: Serialize>(topic: String, config: &T) -> Result<Self> {
        Ok(Self {
            topic,
            body: serde_json::to_vec(config)?,
        })
    }
}

/// `s` the way Home Assistant's `slugify` treats an ASCII name: letters
/// lowercased, digits kept, every run of anything else one `_` (an `_`
/// included), none at either end. A non-ASCII character is kept as it is;
/// Home Assistant slugs whatever object id it is given, so it transliterates
/// that character itself.
fn ha_slug(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut gap = false;
    for c in s.chars() {
        if c.is_ascii() && !c.is_ascii_alphanumeric() {
            gap = !out.is_empty();
            continue;
        }
        if gap {
            out.push('_');
            gap = false;
        }
        out.push(c.to_ascii_lowercase());
    }
    out
}

/// The entity id a discovery config asks Home Assistant for, as its
/// `default_entity_id` (sweep HA16): `<domain>.<object id>`, the object id
/// slugged as Home Assistant would. Home Assistant 2025.10 and later keeps
/// the part after the `.` as the object id when it first registers the
/// entity (`homeassistant/components/mqtt/entity.py`); without it the id
/// came from the device name and the entity name.
fn ha_entity_id(domain: &str, object_id: &str) -> String {
    format!("{domain}.{}", ha_slug(object_id))
}

/// Where a QoS 1 publish goes: the broker client in production, a recorder
/// in tests (the claim's withdraw/republish are proven against the recorder).
trait Publish {
    fn publish_qos1(&self, topic: &str, payload: &[u8], retain: bool) -> Result<()>;
}

impl Publish for Client {
    fn publish_qos1(&self, topic: &str, payload: &[u8], retain: bool) -> Result<()> {
        mqtt_publish_qos1(self, topic, payload, retain)
    }
}

fn publish_configs<P: Publish>(publisher: &P, configs: &[DiscoveryConfig]) -> Result<()> {
    for config in configs {
        publisher.publish_qos1(&config.topic, &config.body, true)?;
    }
    Ok(())
}

/// Discovery configs for the entities that exist regardless of which zones
/// have produced events: last-event sensor, daily digest, chain integrity,
/// and the verify button, in that order.
fn static_discovery_configs(
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
) -> Result<Vec<DiscoveryConfig>> {
    let last_event = last_event_discovery_config(
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;

    let digest_config = HaSensorConfig {
        name: "PWK Daily Digest".to_string(),
        unique_id: format!("{}_daily_digest", device_id),
        default_entity_id: ha_entity_id("sensor", "pwk_daily_digest"),
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
    let digest = DiscoveryConfig::new(
        format!(
            "{}/sensor/{}/daily_digest/config",
            discovery_prefix, device_id
        ),
        &digest_config,
    )?;

    let chain_config = HaBinarySensorConfig {
        name: "PWK Chain Problem".to_string(),
        unique_id: format!("{}_chain_problem", device_id),
        default_entity_id: ha_entity_id("binary_sensor", "pwk_chain_problem"),
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
    let chain = DiscoveryConfig::new(
        format!(
            "{}/binary_sensor/{}/chain_problem/config",
            discovery_prefix, device_id
        ),
        &chain_config,
    )?;

    let button_config = HaButtonConfig {
        name: "PWK Verify Now".to_string(),
        unique_id: format!("{}_verify_now", device_id),
        default_entity_id: ha_entity_id("button", "pwk_verify_now"),
        command_topic: format!("{}/cmd/verify", state_prefix),
        payload_press: "PRESS".to_string(),
        icon: Some("mdi:shield-search".to_string()),
        availability_topic: availability_topic.to_string(),
        payload_available: PAYLOAD_ONLINE.to_string(),
        payload_not_available: PAYLOAD_OFFLINE.to_string(),
        device: device_info.clone(),
    };
    let button = DiscoveryConfig::new(
        format!(
            "{}/button/{}/verify_now/config",
            discovery_prefix, device_id
        ),
        &button_config,
    )?;

    Ok(vec![last_event, digest, chain, button])
}

fn publish_static_discovery<P: Publish>(
    publisher: &P,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
) -> Result<()> {
    publish_configs(
        publisher,
        &static_discovery_configs(
            discovery_prefix,
            state_prefix,
            availability_topic,
            device_id,
            device_info,
        )?,
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
    let last_event = last_event_discovery_config(
        discovery_prefix,
        state_prefix,
        availability_topic,
        device_id,
        device_info,
    )?;
    publish_configs(client, std::slice::from_ref(&last_event))?;

    log::info!(
        "Published HA discovery for {} zones + last_event sensor",
        zones.len()
    );
    Ok(())
}

fn last_event_discovery_config(
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
) -> Result<DiscoveryConfig> {
    let last_event_config = HaSensorConfig {
        name: "PWK Last Event".to_string(),
        unique_id: format!("{}_last_event", device_id),
        default_entity_id: ha_entity_id("sensor", "pwk_last_event"),
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
    DiscoveryConfig::new(
        format!(
            "{}/sensor/{}/last_event/config",
            discovery_prefix, device_id
        ),
        &last_event_config,
    )
}

/// One zone's discovery configs: its event-count sensor, then its motion
/// binary sensor.
fn zone_discovery_configs(
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zone: &str,
) -> Result<Vec<DiscoveryConfig>> {
    let zone_clean = sanitize_for_id(zone);

    // Event count sensor
    let count_config = HaSensorConfig {
        name: format!("PWK {} Events", zone),
        unique_id: format!("{}_{}_events", device_id, zone_clean),
        default_entity_id: ha_entity_id("sensor", &format!("pwk_{}_events", zone)),
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
    let count = DiscoveryConfig::new(
        format!(
            "{}/sensor/{}/{}_events/config",
            discovery_prefix, device_id, zone_clean
        ),
        &count_config,
    )?;

    // Motion binary sensor
    let motion_config = HaBinarySensorConfig {
        name: format!("PWK {} Motion", zone),
        unique_id: format!("{}_{}_motion", device_id, zone_clean),
        default_entity_id: ha_entity_id("binary_sensor", &format!("pwk_{}_motion", zone)),
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
    let motion = DiscoveryConfig::new(
        format!(
            "{}/binary_sensor/{}/{}_motion/config",
            discovery_prefix, device_id, zone_clean
        ),
        &motion_config,
    )?;

    Ok(vec![count, motion])
}

fn publish_zone_discovery<P: Publish>(
    publisher: &P,
    discovery_prefix: &str,
    state_prefix: &str,
    availability_topic: &str,
    device_id: &str,
    device_info: &HaDeviceInfo,
    zone: &str,
) -> Result<()> {
    publish_configs(
        publisher,
        &zone_discovery_configs(
            discovery_prefix,
            state_prefix,
            availability_topic,
            device_id,
            device_info,
            zone,
        )?,
    )?;
    log::debug!("Published HA discovery for zone: {}", zone);
    Ok(())
}

fn publish_events(
    client: &Client,
    topic_prefix: &str,
    device_id: &str,
    events: &[ExportEvent],
) -> Result<()> {
    let mut zone_counts: HashMap<String, u64> = HashMap::new();

    for event in events {
        let zone = extract_zone_name(&event.zone_id);
        *zone_counts.entry(zone.clone()).or_default() += 1;

        publish_single_event(client, topic_prefix, device_id, event, &zone, None)?;
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
        let payload = build_event_state_payload(last, device_id, None)?;
        let json = serde_json::to_vec(&payload)?;
        mqtt_publish_qos1(client, &state_topic, &json, true)?;
    }

    Ok(())
}

/// One event to its zone topic and the firehose, as the shared
/// [`EventStatePayload`] (the same shape `<prefix>/last_event` retains).
fn publish_single_event(
    client: &Client,
    topic_prefix: &str,
    device_id: &str,
    event: &ExportEvent,
    zone: &str,
    last_verify: Option<LastVerify>,
) -> Result<()> {
    let payload = serde_json::to_vec(&build_event_state_payload(event, device_id, last_verify)?)?;

    // Publish to zone-specific topic
    let topic = format!("{}/zone/{}/event", topic_prefix, zone);
    mqtt_publish_qos1(client, &topic, &payload, false)?;

    // Publish to firehose topic
    let firehose_topic = format!("{}/events", topic_prefix);
    mqtt_publish_qos1(client, &firehose_topic, &payload, false)?;

    Ok(())
}

fn build_event_state_payload(
    event: &ExportEvent,
    device_id: &str,
    last_verify: Option<LastVerify>,
) -> Result<EventStatePayload> {
    let published_bucket = TimeBucket::now_10min()?;
    Ok(EventStatePayload {
        event_type: format!("{:?}", event.event_type),
        zone_id: event.zone_id.clone(),
        time_bucket: event.time_bucket,
        time_bucket_start: event.time_bucket.start_epoch_s,
        time_bucket_size: event.time_bucket.size_s,
        confidence: event.confidence,
        // serde renames carry the HA attestation contract values
        // ("adapter" / "ha-bridged"); absent means device/kernel-attested.
        attestation: event
            .attestation
            .and_then(|a| serde_json::to_value(a).ok())
            .and_then(|v| v.as_str().map(str::to_string)),
        kernel_version: event.kernel_version.clone(),
        ruleset_id: event.ruleset_id.clone(),
        device_id: device_id.to_string(),
        published_bucket_start: published_bucket.start_epoch_s,
        published_bucket_size: published_bucket.size_s,
        verified_bucket_start: last_verify.map(|v| v.bucket_start),
        verified_bucket_size: last_verify.map(|v| v.bucket_size),
        verified_ok: last_verify.map(|v| v.ok),
        seq: event.seq,
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

/// The receipt-less event view from `since_bucket` (a true bucket start; 0
/// for the whole retained log): `GET /events?since_bucket=<n>`.
fn fetch_events_since(
    addr: std::net::SocketAddr,
    token: &str,
    since_bucket: u64,
) -> Result<ExportArtifact> {
    let body = api_request(addr, token, "GET", &events_since_path(since_bucket), None)?;
    let artifact: ExportArtifact =
        serde_json::from_slice(&body).context("failed to parse /events response")?;
    Ok(artifact)
}

fn events_since_path(since_bucket: u64) -> String {
    format!("{EVENTS_PATH}?since_bucket={since_bucket}")
}

/// Fetch the rolling 24h digest as raw JSON (republished verbatim to MQTT).
fn fetch_digest(addr: std::net::SocketAddr, token: &str) -> Result<Vec<u8>> {
    api_request(addr, token, "GET", DIGEST_PATH, None)
}

/// Trigger a full sealed-log verification; returns the VerifyReport JSON.
fn post_verify(addr: std::net::SocketAddr, token: &str) -> Result<Vec<u8>> {
    api_request(addr, token, "POST", VERIFY_PATH, None)
}

/// `GET /api/identity`: the id, version and key the announce carries.
fn fetch_identity(addr: std::net::SocketAddr, token: &str) -> Result<KernelIdentity> {
    let body = api_request(addr, token, "GET", IDENTITY_PATH, None)?;
    let value: serde_json::Value =
        serde_json::from_slice(&body).context("failed to parse /api/identity response")?;
    let field = |name: &str| -> Result<String> {
        value
            .get(name)
            .and_then(|v| v.as_str())
            .map(str::to_string)
            .ok_or_else(|| anyhow!("/api/identity response lacks {name}"))
    };
    let verifying_key = field("verifying_key")?;
    if hex::decode(&verifying_key).map(|k| k.len()) != Ok(32) {
        return Err(anyhow!(
            "/api/identity verifying_key is not 64 hex characters"
        ));
    }
    Ok(KernelIdentity {
        device_id: field("device_id")?,
        kernel_version: field("kernel_version")?,
        verifying_key,
    })
}

/// `POST /api/sign-announce` with the sig-less payload bytes; the kernel's
/// 128-hex signature over them.
fn sign_announce(addr: std::net::SocketAddr, token: &str, payload: &[u8]) -> Result<String> {
    let body = api_request(addr, token, "POST", SIGN_ANNOUNCE_PATH, Some(payload))?;
    let value: serde_json::Value =
        serde_json::from_slice(&body).context("failed to parse /api/sign-announce response")?;
    let sig = value
        .get("sig")
        .and_then(|v| v.as_str())
        .ok_or_else(|| anyhow!("/api/sign-announce response lacks sig"))?;
    if sig.len() != 128 || !sig.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(anyhow!("/api/sign-announce returned a malformed signature"));
    }
    Ok(sig.to_string())
}

fn api_request(
    addr: std::net::SocketAddr,
    token: &str,
    method: &str,
    path: &str,
    body: Option<&[u8]>,
) -> Result<Vec<u8>> {
    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_secs(5))?;
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;

    let body_headers = match body {
        Some(bytes) => format!(
            "Content-Type: application/json\r\nContent-Length: {}\r\n",
            bytes.len()
        ),
        None => String::new(),
    };
    let request = format!(
        "{method} {path} HTTP/1.1\r\nHost: {host}\r\nx-witness-token: {token}\r\n{body_headers}Connection: close\r\n\r\n",
        host = addr,
    );
    stream.write_all(request.as_bytes())?;
    if let Some(bytes) = body {
        stream.write_all(bytes)?;
    }

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

/// Where the publish cursor stands when the daemon starts: the bucket
/// before the current one, with nothing published in it, so the first poll
/// asks the kernel for `since_bucket=<that start>` and publishes the
/// previous and the current bucket only. The first bridge started from
/// bucket zero and republished every retained event as a non-retained
/// message on every restart — a fresh Home Assistant treated each as a
/// camera seeing someone now. `replay_from_genesis` restores that start
/// on purpose (a migration, never a restart). The grid is the kernel's
/// bucket size, so the cursor never lands inside the current bucket of a
/// kernel sealing wider buckets than the default (the kernel keeps only
/// records whose TRUE start is at or past the cursor).
fn initial_publish_cursor(
    now_epoch_s: u64,
    bucket_size_s: u64,
    replay_from_genesis: bool,
) -> Option<PublishCursor> {
    if replay_from_genesis {
        return None;
    }
    let size = bucket_size_s.max(1);
    let current = now_epoch_s / size * size;
    Some(PublishCursor {
        bucket_start_epoch_s: current.saturating_sub(size),
        published_in_bucket: 0,
    })
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
/// The bucket start BEFORE export jitter. Every `/events` export re-draws
/// each bucket's start by up to ±`jitter_s` (default 120 s, in 60 s steps)
/// around the true, size-aligned start, and a fresh draw lands on every
/// poll — so the raw start is not an identity: the same bucket compares
/// "newer" than the cursor one poll and "older" the next, and its events
/// republish. The true start is the nearest multiple of the bucket size,
/// which is recoverable exactly as long as the jitter stays below half the
/// bucket (120 s against a 600 s bucket, and the kernel's minimum bucket
/// is 300 s). The bridge is the kernel's own local companion, so undoing
/// the jitter for *ordering* here reveals nothing to anyone else; what it
/// publishes still carries the exported (jittered) bucket.
fn stable_bucket_start(bucket: &TimeBucket) -> u64 {
    let size = u64::from(bucket.size_s.max(1));
    let half = size / 2;
    bucket.start_epoch_s.saturating_add(half) / size * size
}

fn select_new_events<'a>(
    events: &'a [ExportEvent],
    cursor: &mut Option<PublishCursor>,
) -> Vec<&'a ExportEvent> {
    let mut out = Vec::new();
    let mut seen_in_newest = 0usize;
    let mut newest: Option<u64> = None;
    for event in events {
        let start = stable_bucket_start(&event.time_bucket);
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
            "confidence": 0.9,
            "kernel_version": "test",
            "ruleset_id": "test-ruleset",
            "ruleset_hash": vec![0u8; 32]
        }))
        .expect("export event shape");
        e.time_bucket = TimeBucket {
            start_epoch_s: start,
            size_s: 600,
        };
        e
    }

    #[test]
    fn the_daemon_starts_its_cursor_one_bucket_back_never_at_zero() {
        // 12:25 on a 600 s grid: the current bucket starts 12:20, the
        // cursor at 12:10 — the kernel then serves 12:10 and 12:20 only.
        let now = 1_760_140_800 + 25 * 60;
        let cursor = initial_publish_cursor(now, 600, false).expect("a cursor");
        assert_eq!(cursor.bucket_start_epoch_s, 1_760_140_800 + 10 * 60);
        assert_eq!(cursor.published_in_bucket, 0);
        // The kernel's own grid, not the default's: an 1800 s kernel at
        // 12:25 is in the 12:00 bucket, so the cursor is 11:30 — a 600 s
        // cursor (12:10) would have skipped the open 12:00 bucket entirely.
        let wide = initial_publish_cursor(now, 1800, false).expect("a cursor");
        assert_eq!(wide.bucket_start_epoch_s, 1_760_140_800 - 30 * 60);
        // Early in the epoch the cursor cannot go below zero; a zero size is
        // treated as one second rather than dividing by it.
        assert_eq!(
            initial_publish_cursor(100, 600, false).map(|c| c.bucket_start_epoch_s),
            Some(0)
        );
        assert_eq!(
            initial_publish_cursor(100, 0, false).map(|c| c.bucket_start_epoch_s),
            Some(99)
        );
        // --replay-from-genesis is the old start: no cursor, since_bucket=0.
        assert!(initial_publish_cursor(now, 600, true).is_none());
    }

    #[test]
    fn the_initial_cursor_publishes_the_previous_and_current_bucket_once() {
        // What the first poll sees with the cursor one bucket back: the
        // kernel serves the previous and the current bucket; every event
        // in both is new, once, and the cursor moves to the newest.
        let now = 1_760_140_800 + 25 * 60;
        let mut cursor = initial_publish_cursor(now, 600, false);
        let previous = 1_760_140_800 + 10 * 60;
        let current = 1_760_140_800 + 20 * 60;
        let served = vec![ev(previous, "a"), ev(previous, "b"), ev(current, "a")];
        let first = select_new_events(&served, &mut cursor);
        assert_eq!(first.len(), 3, "both buckets publish on the first poll");
        assert_eq!(cursor.map(|c| c.bucket_start_epoch_s), Some(current));
        assert_eq!(cursor.map(|c| c.published_in_bucket), Some(1));
        // The next poll (same bucket, one more event) publishes just that one.
        let again = vec![
            ev(previous, "a"),
            ev(previous, "b"),
            ev(current, "a"),
            ev(current, "b"),
        ];
        let second = select_new_events(&again, &mut cursor);
        assert_eq!(second.len(), 1);
        assert_eq!(second[0].zone_id, "zone:b");
    }

    #[test]
    fn select_new_events_is_immune_to_export_jitter() {
        // The same two events, exported twice: the first poll drew -120 s
        // for the bucket, the second +60 s. Raw starts differ; the bucket
        // is the same, so the second poll publishes nothing.
        let mut cursor = None;
        let first = vec![ev(1_000_200 - 120, "a"), ev(1_000_200 - 120, "b")];
        assert_eq!(select_new_events(&first, &mut cursor).len(), 2);
        let second = vec![ev(1_000_200 + 60, "a"), ev(1_000_200 + 60, "b")];
        assert!(
            select_new_events(&second, &mut cursor).is_empty(),
            "a re-jittered export of the same bucket must not republish"
        );
        // The cursor is keyed on the recovered true start, not the draw.
        assert_eq!(cursor.map(|c| c.bucket_start_epoch_s), Some(1_000_200));
        // A genuinely newer bucket, jittered downwards, still counts as new.
        let third = vec![
            ev(1_000_200 + 120, "a"),
            ev(1_000_200 + 120, "b"),
            ev(1_000_800 - 120, "c"),
        ];
        let fresh = select_new_events(&third, &mut cursor);
        assert_eq!(fresh.len(), 1);
        assert_eq!(fresh[0].zone_id, "zone:c");
    }

    #[test]
    fn stable_bucket_start_undoes_every_jitter_step() {
        for offset in [-120i64, -60, 0, 60, 120] {
            let start = (1_000_200i64 + offset) as u64;
            let b = TimeBucket {
                start_epoch_s: start,
                size_s: 600,
            };
            assert_eq!(stable_bucket_start(&b), 1_000_200, "offset {offset}");
        }
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
        let next = vec![
            ev(1_000_200, "a"),
            ev(1_000_200, "b"),
            ev(1_000_200, "c"),
            ev(1_000_800, "d"),
        ];
        let fresh = select_new_events(&next, &mut cursor);
        assert_eq!(fresh.len(), 1);
        assert_eq!(fresh[0].zone_id, "zone:d");
        assert_eq!(
            cursor,
            Some(PublishCursor {
                bucket_start_epoch_s: 1_000_800,
                published_in_bucket: 1
            })
        );
        // Retention drops the old bucket from the artifact: still nothing new.
        let trimmed = vec![ev(1_000_800, "d")];
        assert!(select_new_events(&trimmed, &mut cursor).is_empty());
    }

    // C5: one payload on every event topic — nested AND flattened bucket,
    // the kernel's self-verify as `verified_*` bucket fields (absent until a
    // verify ran), `seq` when the view carried it, and never a precise time.
    #[test]
    fn serialize_event_state_payload() {
        let mut event = ev(1_700_000_000, "front_door");
        event.seq = Some(42);
        let verified = LastVerify {
            bucket_start: 1_699_999_400,
            bucket_size: 600,
            ok: true,
        };
        let payload =
            build_event_state_payload(&event, "pwk_1a2b3c4d", Some(verified)).expect("payload");
        let json = serde_json::to_string(&payload).expect("serialize");
        let v: serde_json::Value = serde_json::from_str(&json).expect("json");
        assert_eq!(v["event_type"], "BoundaryCrossingObjectLarge");
        assert_eq!(v["zone_id"], "zone:front_door");
        assert_eq!(v["time_bucket"]["start_epoch_s"], 1_700_000_000u64);
        assert_eq!(v["time_bucket"]["size_s"], 600);
        assert_eq!(v["time_bucket_start"], 1_700_000_000u64);
        assert_eq!(v["time_bucket_size"], 600);
        assert_eq!(v["kernel_version"], "test");
        assert_eq!(v["ruleset_id"], "test-ruleset");
        assert_eq!(v["device_id"], "pwk_1a2b3c4d");
        assert_eq!(v["verified_bucket_start"], 1_699_999_400u64);
        assert_eq!(v["verified_bucket_size"], 600);
        assert_eq!(v["verified_ok"], true);
        assert_eq!(v["seq"], 42);
        assert!(
            v.get("attestation").is_none(),
            "device-attested omits the key"
        );
        assert!(
            v.get("ruleset_hash").is_none(),
            "the hash is not an HA attribute"
        );
        for forbidden in ["verified_at", "created_at", "correlation_token"] {
            assert!(
                v.get(forbidden).is_none(),
                "{forbidden} must not be on the wire"
            );
        }
        // Keys in contract (struct) order on the wire (a `Value` round trip
        // would sort them, so the positions are read off the string).
        let positions: Vec<usize> = [
            "event_type",
            "zone_id",
            "time_bucket",
            "time_bucket_start",
            "time_bucket_size",
            "confidence",
            "kernel_version",
            "ruleset_id",
            "device_id",
            "published_bucket_start",
            "published_bucket_size",
            "verified_bucket_start",
            "verified_bucket_size",
            "verified_ok",
            "seq",
        ]
        .iter()
        .map(|key| {
            json.find(&format!("\"{key}\":"))
                .unwrap_or_else(|| panic!("{key} missing from {json}"))
        })
        .collect();
        assert!(
            positions.windows(2).all(|w| w[0] < w[1]),
            "keys out of contract order: {json}"
        );

        // Before any verify, and without a seq: all four keys are absent.
        let bare = build_event_state_payload(&ev(1_700_000_000, "a"), "pwk_x", None).unwrap();
        let v: serde_json::Value = serde_json::to_value(&bare).unwrap();
        for absent in [
            "verified_bucket_start",
            "verified_bucket_size",
            "verified_ok",
            "seq",
        ] {
            assert!(v.get(absent).is_none(), "{absent}");
        }
    }

    // C1: the announce serializes in contract order, `sig` last and absent
    // until the kernel fills it, and `api.host` only when configured — so
    // the sig-less bytes the bridge sends to `/api/sign-announce` are what
    // the integration re-serializes to check the signature.
    #[test]
    fn announce_payload_serializes_in_contract_order() {
        let payload = AnnouncePayload {
            v: 1,
            device_id: "pwk_1a2b3c4d".to_string(),
            kernel_version: "2.4.16".to_string(),
            verifying_key: "7f".repeat(32),
            api: AnnounceApi {
                scheme: "http".to_string(),
                bind: "all".to_string(),
                port: 8799,
                host: None,
            },
            pairing: "required".to_string(),
            mqtt_topic_prefix: "witness".to_string(),
            discovery_prefix: "homeassistant".to_string(),
            availability_topic: "witness/status".to_string(),
            announced_bucket_start: 1_760_140_800,
            announced_bucket_size: 600,
            sig: None,
        };
        let unsigned = serde_json::to_string(&payload).expect("serialize");
        assert_eq!(
            unsigned,
            format!(
                r#"{{"v":1,"device_id":"pwk_1a2b3c4d","kernel_version":"2.4.16","verifying_key":"{}","api":{{"scheme":"http","bind":"all","port":8799}},"pairing":"required","mqtt_topic_prefix":"witness","discovery_prefix":"homeassistant","availability_topic":"witness/status","announced_bucket_start":1760140800,"announced_bucket_size":600}}"#,
                "7f".repeat(32)
            )
        );
        for forbidden in ["chain_length", "latest_hash", "token", "code", "seed"] {
            assert!(
                !unsigned.contains(forbidden),
                "{forbidden} never rides the announce"
            );
        }
        // Signing appends exactly one trailing key: the signed bytes minus
        // `,"sig":"…"` are the unsigned bytes.
        let signed = AnnouncePayload {
            sig: Some("ab".repeat(64)),
            ..payload.clone()
        };
        let signed_json = serde_json::to_string(&signed).expect("serialize");
        assert_eq!(
            signed_json,
            format!(
                "{},\"sig\":\"{}\"}}",
                &unsigned[..unsigned.len() - 1],
                "ab".repeat(64)
            )
        );
        // A host rides only when one was configured.
        let hosted = AnnouncePayload {
            api: AnnounceApi {
                host: Some("d0491a67-privacy-witness-kernel".to_string()),
                ..payload.api.clone()
            },
            ..payload
        };
        let hosted_json = serde_json::to_string(&hosted).expect("serialize");
        assert!(hosted_json.contains(r#""port":8799,"host":"d0491a67-privacy-witness-kernel"}"#));
        assert_eq!(
            announce_topic("witness", "pwk_1a2b3c4d"),
            "witness/pwk_1a2b3c4d/announce"
        );
        assert_eq!(
            claim_topic("witness", "pwk_1a2b3c4d"),
            "witness/claim/pwk_1a2b3c4d"
        );
        assert_eq!(
            events_since_path(1_760_140_800),
            "/events?since_bucket=1760140800"
        );
    }

    /// One HA seam fixture (tests/fixtures/ha_contract/, written by
    /// gen_ha_contract_fixtures.py): the SAME bytes the integration's own
    /// tests read, so both ends of a contract are held to one file.
    fn ha_contract_fixture(name: &str) -> serde_json::Value {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/ha_contract")
            .join(name);
        let text =
            std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        serde_json::from_str(&text).expect("fixture is JSON")
    }

    // C1 against the shared vector, byte for byte: the sig-less payload
    // round-trips through `AnnouncePayload` unchanged (struct order IS wire
    // order, which is what kernel_trust.py re-serializes), the kernel's
    // message is the prefix + those bytes, the deterministic Ed25519
    // signature under the vector's key is the vector's, and the published
    // bytes are payload + `sig` last.
    #[test]
    fn announce_vector_matches_the_integration_s_canonical_bytes() {
        use ed25519_dalek::{Signer, SigningKey};
        use witness_kernel::api::announce::{announce_message, verify_announce};
        let fixture = ha_contract_fixture("announce.json");
        let seed: [u8; 32] = hex::decode(fixture["signing_key_seed"].as_str().unwrap())
            .unwrap()
            .try_into()
            .unwrap();
        let key = SigningKey::from_bytes(&seed);
        let verifying = hex::encode(key.verifying_key().to_bytes());
        assert_eq!(fixture["verifying_key"], verifying.as_str());
        assert_eq!(
            fixture["fingerprint"],
            witness_kernel::api::pairing::key_fingerprint(&verifying)
                .unwrap()
                .as_str()
        );
        assert_eq!(
            fixture["topic"],
            announce_topic("witness", "pwk_1a2b3c4d").as_str()
        );
        let vectors = fixture["vectors"].as_array().expect("vectors");
        assert_eq!(vectors.len(), 2, "one per launcher posture");
        for vector in vectors {
            let name = vector["name"].as_str().unwrap();
            let payload_json = vector["payload"].as_str().unwrap();
            let parsed: AnnouncePayload =
                serde_json::from_str(payload_json).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert!(
                parsed.sig.is_none(),
                "{name}: the vector payload is sig-less"
            );
            assert_eq!(
                serde_json::to_string(&parsed).unwrap(),
                payload_json,
                "{name}: struct order is wire order"
            );
            let message = announce_message(payload_json.as_bytes());
            assert_eq!(
                message,
                vector["canonical_message"].as_str().unwrap().as_bytes(),
                "{name}: the signed message"
            );
            let sig = hex::encode(key.sign(&message).to_bytes());
            assert_eq!(
                vector["sig"],
                sig.as_str(),
                "{name}: Ed25519 is deterministic, so the vector's sig is reproducible"
            );
            verify_announce(&key.verifying_key(), payload_json.as_bytes(), &sig)
                .unwrap_or_else(|e| panic!("{name}: {e}"));
            let signed = AnnouncePayload {
                sig: Some(sig),
                ..parsed
            };
            assert_eq!(
                serde_json::to_string(&signed).unwrap(),
                vector["published"].as_str().unwrap(),
                "{name}: the retained bytes"
            );
            let republished: AnnouncePayload =
                serde_json::from_str(vector["published"].as_str().unwrap()).unwrap();
            assert_eq!(republished, signed, "{name}: published form parses back");
        }
        let hosted = &vectors[1]["payload"].as_str().unwrap();
        assert!(hosted.contains(r#""host":"d0491a67-privacy-witness-kernel"}"#));
        assert!(!vectors[0]["payload"].as_str().unwrap().contains("host"));
    }

    // C3 against the shared vector: the integration's compact claim parses
    // as a claim for its bucket, the empty retained payload is the clear,
    // the freshness constants are the ones the fixture states, and the
    // `claim_state` words are the ones the coordinator accepts.
    #[test]
    fn claim_vector_matches_the_integration_s_payload() {
        let fixture = ha_contract_fixture("claim.json");
        assert_eq!(
            fixture["topic"],
            claim_topic("witness", "pwk_1a2b3c4d").as_str()
        );
        let bucket = fixture["claimed_bucket_start"].as_u64().unwrap();
        assert_eq!(
            parse_claim_message(fixture["payload"].as_str().unwrap().as_bytes()),
            ClaimMessage::Claimed(ClaimRecord {
                claimed_bucket_start: bucket
            })
        );
        assert_eq!(
            parse_claim_message(fixture["withdrawal"].as_str().unwrap().as_bytes()),
            ClaimMessage::Cleared
        );
        assert_eq!(fixture["fresh_for_secs"], CLAIM_FRESH_SECS);
        assert_eq!(fixture["future_skew_secs"], CLAIM_SKEW_SECS);
        assert!(
            fixture["republish_interval_secs"].as_u64().unwrap() < CLAIM_FRESH_SECS,
            "the integration republishes well inside the freshness window"
        );
        assert_eq!(
            fixture["claim_state_topic"],
            format!("witness/{CLAIM_STATE_TOPIC_SUFFIX}").as_str()
        );
        assert_eq!(
            fixture["claim_state_values"],
            serde_json::json!([PAYLOAD_CLAIMED, PAYLOAD_UNCLAIMED])
        );
        let record = ClaimRecord {
            claimed_bucket_start: bucket,
        };
        assert!(claim_is_fresh(&record, bucket + 3600));
        assert!(!claim_is_fresh(&record, bucket + CLAIM_FRESH_SECS));
    }

    // C5 against the shared vector: the ONE event payload, byte for byte,
    // as the coordinator and `witness_event_attributes` parse it.
    #[test]
    fn event_state_vector_is_the_wire_the_integration_parses() {
        let fixture = ha_contract_fixture("event_state.json");
        let payload = EventStatePayload {
            event_type: "BoundaryCrossingObjectLarge".to_string(),
            zone_id: "zone:front_door".to_string(),
            time_bucket: witness_kernel::TimeBucket {
                start_epoch_s: 1_760_140_800,
                size_s: 600,
            },
            time_bucket_start: 1_760_140_800,
            time_bucket_size: 600,
            confidence: 0.87,
            attestation: Some("adapter".to_string()),
            kernel_version: "2.4.16".to_string(),
            ruleset_id: "ruleset:frigate_v1".to_string(),
            device_id: "pwk_1a2b3c4d".to_string(),
            published_bucket_start: 1_760_141_400,
            published_bucket_size: 600,
            verified_bucket_start: Some(1_760_140_200),
            verified_bucket_size: Some(600),
            verified_ok: Some(true),
            seq: Some(1287),
        };
        assert_eq!(
            serde_json::to_string(&payload).unwrap(),
            fixture["payload"].as_str().unwrap()
        );
        assert_eq!(
            fixture["topics"],
            serde_json::json!([
                "witness/last_event",
                "witness/events",
                "witness/zone/front_door/event"
            ])
        );
        assert_eq!(fixture["ha"]["zone_id"], "zone:front_door");
        assert_eq!(fixture["ha"]["time_bucket_start"], 1_760_140_800u64);
    }

    // C3: the claim payload's shapes, the freshness rule, and the tracker's
    // transitions (claimed → withdraw, cleared or stale → republish, noise
    // → nothing).
    #[test]
    fn claim_tracker_honors_fresh_claims_and_ages_them_out() {
        let now = 1_760_140_800u64;
        let claim = format!(
            r#"{{"v":1,"claimed":true,"nonce":"0123456789abcdef","claimed_bucket_start":{}}}"#,
            now - 600
        );
        assert_eq!(
            parse_claim_message(claim.as_bytes()),
            ClaimMessage::Claimed(ClaimRecord {
                claimed_bucket_start: now - 600
            })
        );
        assert_eq!(parse_claim_message(b""), ClaimMessage::Cleared);
        assert_eq!(
            parse_claim_message(br#"{"v":1,"claimed":false}"#),
            ClaimMessage::Cleared
        );
        assert_eq!(parse_claim_message(b"not json"), ClaimMessage::Unparsable);
        assert_eq!(
            parse_claim_message(br#"{"v":1,"entry_id":"abc","claimed_at":1}"#),
            ClaimMessage::Unparsable,
            "the v1 entry_id shape is not a claim"
        );

        let mut tracker = ClaimTracker::default();
        assert_eq!(tracker.settle(now), None, "nothing heard, nothing to do");
        tracker.observe(claim.as_bytes());
        assert_eq!(tracker.settle(now), Some(ClaimTransition::Withdraw));
        assert_eq!(tracker.settle(now), None, "already honored");
        tracker.observe(b"garbage");
        assert_eq!(tracker.settle(now), None, "noise leaves the state alone");
        // 24 h later with no renewal: stale, republish.
        assert_eq!(
            tracker.settle(now - 600 + CLAIM_FRESH_SECS),
            Some(ClaimTransition::Republish)
        );
        // A renewal brings it back; an explicit clear takes it away.
        let renewed = format!(
            r#"{{"v":1,"claimed":true,"nonce":"ff","claimed_bucket_start":{}}}"#,
            now + CLAIM_FRESH_SECS
        );
        tracker.observe(renewed.as_bytes());
        assert_eq!(
            tracker.settle(now + CLAIM_FRESH_SECS),
            Some(ClaimTransition::Withdraw)
        );
        tracker.observe(b"");
        assert_eq!(
            tracker.settle(now + CLAIM_FRESH_SECS),
            Some(ClaimTransition::Republish)
        );

        // Freshness: within 24 h, up to one bucket ahead of this clock.
        let fresh = |start: u64, at: u64| {
            claim_is_fresh(
                &ClaimRecord {
                    claimed_bucket_start: start,
                },
                at,
            )
        };
        assert!(fresh(now, now));
        assert!(fresh(now, now + CLAIM_FRESH_SECS - 1));
        assert!(!fresh(now, now + CLAIM_FRESH_SECS));
        assert!(fresh(now + CLAIM_SKEW_SECS, now));
        assert!(!fresh(now + CLAIM_SKEW_SECS + 1, now));
    }

    /// Records every publish for the claim tests.
    #[derive(Default)]
    struct Recorder(std::cell::RefCell<Vec<(String, Vec<u8>, bool)>>);

    impl Publish for Recorder {
        fn publish_qos1(&self, topic: &str, payload: &[u8], retain: bool) -> Result<()> {
            self.0
                .borrow_mut()
                .push((topic.to_string(), payload.to_vec(), retain));
            Ok(())
        }
    }

    // C3: a claim withdraws EVERY discovery config this bridge has published
    // — the static four and both per zone — with an empty retained payload
    // and touches no state topic; a clear republishes the same set with
    // full bodies; both say so on `<prefix>/claim_state`.
    #[test]
    fn claim_withdraws_every_published_config_and_clear_republishes() {
        let device = test_device();
        let zones: HashSet<String> = ["front_door", "driveway"]
            .into_iter()
            .map(str::to_string)
            .collect();
        let expected_topics: Vec<String> = {
            let mut t = vec![
                "homeassistant/sensor/pwk_1a2b3c4d/last_event/config",
                "homeassistant/sensor/pwk_1a2b3c4d/daily_digest/config",
                "homeassistant/binary_sensor/pwk_1a2b3c4d/chain_problem/config",
                "homeassistant/button/pwk_1a2b3c4d/verify_now/config",
                "homeassistant/sensor/pwk_1a2b3c4d/driveway_events/config",
                "homeassistant/binary_sensor/pwk_1a2b3c4d/driveway_motion/config",
                "homeassistant/sensor/pwk_1a2b3c4d/front_door_events/config",
                "homeassistant/binary_sensor/pwk_1a2b3c4d/front_door_motion/config",
            ]
            .into_iter()
            .map(str::to_string)
            .collect::<Vec<_>>();
            t.sort();
            t
        };
        let mut owned = owned_discovery_topics(
            "homeassistant",
            "witness",
            "witness/status",
            "pwk_1a2b3c4d",
            &device,
            &zones,
        )
        .expect("topics");
        owned.sort();
        assert_eq!(owned, expected_topics);

        let recorder = Recorder::default();
        withdraw_discovery(
            &recorder,
            "homeassistant",
            "witness",
            "witness/status",
            "pwk_1a2b3c4d",
            &device,
            &zones,
        )
        .expect("withdraw");
        let withdrawn = recorder.0.borrow();
        let mut topics: Vec<String> = withdrawn.iter().map(|(t, _, _)| t.clone()).collect();
        topics.sort();
        assert_eq!(topics, expected_topics);
        for (topic, payload, retain) in withdrawn.iter() {
            assert!(
                payload.is_empty(),
                "{topic}: an empty retained payload withdraws"
            );
            assert!(retain, "{topic}: must be retained to replace the config");
            assert!(
                topic.starts_with("homeassistant/") && topic.ends_with("/config"),
                "{topic}: only discovery configs are withdrawn, never a state topic"
            );
        }
        drop(withdrawn);

        let recorder = Recorder::default();
        republish_discovery(
            &recorder,
            "homeassistant",
            "witness",
            "witness/status",
            "pwk_1a2b3c4d",
            &device,
            &zones,
        )
        .expect("republish");
        let republished = recorder.0.borrow();
        let mut topics: Vec<String> = republished.iter().map(|(t, _, _)| t.clone()).collect();
        topics.sort();
        assert_eq!(topics, expected_topics);
        for (topic, payload, retain) in republished.iter() {
            let v: serde_json::Value = serde_json::from_slice(payload).expect("config body");
            assert!(v.get("unique_id").is_some(), "{topic}: a full config body");
            assert!(retain);
        }
        drop(republished);

        let recorder = Recorder::default();
        publish_claim_state(&recorder, "witness", true).unwrap();
        publish_claim_state(&recorder, "witness", false).unwrap();
        assert_eq!(
            *recorder.0.borrow(),
            vec![
                ("witness/claim_state".to_string(), b"claimed".to_vec(), true),
                (
                    "witness/claim_state".to_string(),
                    b"unclaimed".to_vec(),
                    true
                ),
            ]
        );
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

    fn test_device() -> HaDeviceInfo {
        HaDeviceInfo {
            identifiers: vec!["pwk_1a2b3c4d".to_string()],
            name: "Privacy Witness Kernel".to_string(),
            manufacturer: "securaCV".to_string(),
            model: "PWK".to_string(),
            sw_version: "0.7.0".to_string(),
        }
    }

    /// Each discovery config's topic, `default_entity_id` and `unique_id`.
    fn ids_of(configs: &[DiscoveryConfig]) -> Vec<(String, String, String)> {
        configs
            .iter()
            .map(|c| {
                let v: serde_json::Value = serde_json::from_slice(&c.body).expect("json body");
                let s = |k: &str| v[k].as_str().map(str::to_string).unwrap_or_default();
                (c.topic.clone(), s("default_entity_id"), s("unique_id"))
            })
            .collect()
    }

    // Sweep HA16: every config asks Home Assistant for the id this file's
    // header (and docs/homeassistant_setup.md, the Lovelace dashboard and
    // the HomeKit Bridge recipe) gives it, and keeps its unique id, so an
    // entity Home Assistant already registered keeps its registry entry.
    // The announce and claim topics are NOT discovery configs: they live
    // under the state prefix, never under `homeassistant/`.
    #[test]
    fn every_discovery_config_asks_for_its_documented_entity_id() {
        for topic in [
            announce_topic("witness", "pwk_1a2b3c4d"),
            claim_topic("witness", "pwk_1a2b3c4d"),
        ] {
            assert!(!topic.starts_with("homeassistant/"), "{topic}");
            assert!(!topic.ends_with("/config"), "{topic}");
        }
        let device = test_device();
        let mut configs = static_discovery_configs(
            "homeassistant",
            "witness",
            "witness/status",
            "pwk_1a2b3c4d",
            &device,
        )
        .expect("static configs");
        configs.extend(
            zone_discovery_configs(
                "homeassistant",
                "witness",
                "witness/status",
                "pwk_1a2b3c4d",
                &device,
                "front_door",
            )
            .expect("zone configs"),
        );
        let want = [
            ("sensor", "last_event", "sensor.pwk_last_event"),
            ("sensor", "daily_digest", "sensor.pwk_daily_digest"),
            (
                "binary_sensor",
                "chain_problem",
                "binary_sensor.pwk_chain_problem",
            ),
            ("button", "verify_now", "button.pwk_verify_now"),
            (
                "sensor",
                "front_door_events",
                "sensor.pwk_front_door_events",
            ),
            (
                "binary_sensor",
                "front_door_motion",
                "binary_sensor.pwk_front_door_motion",
            ),
        ];
        let got = ids_of(&configs);
        assert_eq!(got.len(), want.len());
        for ((topic, entity_id, unique_id), (component, object_id, want_id)) in got.iter().zip(want)
        {
            assert_eq!(
                topic,
                &format!("homeassistant/{component}/pwk_1a2b3c4d/{object_id}/config")
            );
            assert_eq!(entity_id, want_id, "{topic}");
            // The id's domain is the platform the topic names.
            assert!(entity_id.starts_with(&format!("{component}.")), "{topic}");
            assert_eq!(unique_id, &format!("pwk_1a2b3c4d_{object_id}"), "{topic}");
        }
    }

    // A zone name becomes its id the way Home Assistant slugs it; the unique
    // id and the topic keep sanitize_for_id's form, as before.
    #[test]
    fn zone_entity_ids_are_slugged_like_home_assistant() {
        let device = test_device();
        let cases = [
            ("Front Door", "Front_Door", "pwk_front_door"),
            ("back-yard", "back_yard", "pwk_back_yard"),
            ("Garage__2", "Garage__2", "pwk_garage_2"),
            ("-porch-", "_porch_", "pwk_porch"),
        ];
        for (zone, clean, slug) in cases {
            let got = ids_of(
                &zone_discovery_configs(
                    "homeassistant",
                    "witness",
                    "witness/status",
                    "pwk_x",
                    &device,
                    zone,
                )
                .expect("zone configs"),
            );
            assert_eq!(got[0].1, format!("sensor.{slug}_events"), "{zone}");
            assert_eq!(got[1].1, format!("binary_sensor.{slug}_motion"), "{zone}");
            assert_eq!(got[0].2, format!("pwk_x_{clean}_events"), "{zone}");
            assert_eq!(got[1].2, format!("pwk_x_{clean}_motion"), "{zone}");
        }
        assert_eq!(ha_slug("PWK Front--Door!"), "pwk_front_door");
        assert_eq!(ha_slug("_x_"), "x");
        assert_eq!(ha_slug(""), "");
        // Non-ASCII is left for Home Assistant's own slugify.
        assert_eq!(ha_slug("Caf\u{e9} 1"), "caf\u{e9}_1");
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
            default_entity_id: "sensor.pwk_test_sensor".to_string(),
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
        assert!(json.contains("\"default_entity_id\":\"sensor.pwk_test_sensor\""));
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
            default_entity_id: "button.pwk_verify_now".to_string(),
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
    fn fleet_tracker_writes_the_summary_the_kernel_reads_and_rehydrates_it() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("fleet_peers.json");
        let mut tracker = FleetPeerTracker::open(path.clone());
        tracker.flush_if_due(true);
        let empty = PeerSummaryFile::read(&path)
            .expect("read")
            .expect("written at start");
        assert!(empty.peers.is_empty());

        tracker.observe(
            "securacv/porch/status",
            br#"{"status":"online","device_type":"canary-wap"}"#,
            false,
        );
        // Rate-limited: a second write right after the first waits...
        tracker.flush_if_due(false);
        let still_empty = PeerSummaryFile::read(&path).expect("read").expect("exists");
        assert!(still_empty.peers.is_empty());
        // ...but the poll tick forces it.
        tracker.flush_if_due(true);
        let written = PeerSummaryFile::read(&path).expect("read").expect("exists");
        assert_eq!(written.peers.len(), 1);
        assert_eq!(written.peers[0].device_id, "porch");
        assert_eq!(written.peers[0].device_type.as_deref(), Some("canary-wap"));

        let reopened = FleetPeerTracker::open(path);
        assert_eq!(reopened.table.len(), 1);
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
