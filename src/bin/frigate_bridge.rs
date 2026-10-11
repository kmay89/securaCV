//! frigate_bridge - Subscribe to Frigate MQTT events and convert to privacy-preserving PWK events.
//!
//! This bridge enables PWK to work with Frigate NVR:
//! 1. Subscribes to ONE Frigate MQTT topic: `<prefix>/events` (events mode,
//!    the default) or `<prefix>/reviews` (`--enable-reviews`)
//! 2. Strips identity data at parse time — recognized names and plates,
//!    object ids, boxes, trajectories, thumbnails, precise times, GenAI
//!    summaries (`witness_kernel::transport::frigate`, spec D7)
//! 3. Coarsens timestamps to PWK time buckets
//! 4. Maps Frigate object classes to PWK event types
//! 5. Writes sanitized events to the PWK sealed log
//!
//! It is the process that APPENDS in Frigate mode, so it also owns the
//! chain's housekeeping (spec D6): one heartbeat record per bucket, a
//! retention pass (prune + checkpoint) on a timer, lifecycle records at
//! start and clean shutdown, and the boot-time tail verification that
//! refuses to extend a chain that does not verify (honoring the signed
//! high-water mark at `SECURACV_HWM_PATH`, which the kernel also advances
//! on every append). It publishes one retained MQTT payload of its own,
//! `<state_prefix>/frigate_link` (spec D14), so Home Assistant can tell a
//! quiet night from a Frigate that stopped publishing.
//!
//! This allows users to leverage Frigate's excellent ML detection while
//! maintaining PWK's privacy guarantees for long-term event storage.

use anyhow::{anyhow, Result};
use clap::Parser;
use rumqttc::{Client, Connection, Event, Incoming, MqttOptions, QoS, RecvTimeoutError};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use witness_kernel::transport::frigate::{
    camera_slug, frigate_subscriptions, parse_frigate_event, parse_review_event,
    sanitize_zone_name, select_event_type, FrigateClaimKind, FrigateMode, ParsedFrigateEvent,
    DEFAULT_LABELS,
};
use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
use witness_kernel::{
    CandidateEvent, EventType, FailureType, InferenceBackend, Kernel, KernelConfig, LifecyclePhase,
    ModuleDescriptor, TimeBucket, ZonePolicy, MIN_BUCKET_SIZE_S,
};

const BRIDGE_NAME: &str = "frigate_bridge";

/// Heartbeat check cadence (spec D6: 300 s). A heartbeat is sealed once per
/// bucket — the kernel coarsens it to a 10-minute bucket at minimum — at the
/// first check after the bucket starts, so the chain tail is anchored within
/// this interval of a rollover; a second check inside the same bucket seals
/// nothing. One is also sealed at start and at clean shutdown.
const HEARTBEAT_INTERVAL: Duration = Duration::from_secs(300);

/// `<state_prefix>/frigate_link` is republished at least this often even
/// when nothing in it changed, so a stale retained payload is bounded.
const FRIGATE_LINK_REPUBLISH_INTERVAL: Duration = Duration::from_secs(300);

/// The bucket `last_message_bucket_start` is coarsened to. Pinned to ten
/// minutes by the contract, independent of `--bucket-size-secs`.
const FRIGATE_LINK_BUCKET_SIZE_S: u32 = 600;

const FRIGATE_LINK_TOPIC_SUFFIX: &str = "frigate_link";

/// How long the main loop waits for an MQTT event before running its timers.
const LOOP_TICK: Duration = Duration::from_millis(500);

/// How long clean shutdown lets the final retained publish and the
/// DISCONNECT drain before the process exits.
const SHUTDOWN_FLUSH: Duration = Duration::from_secs(2);

/// Exponential backoff helper for MQTT reconnection.
struct ExponentialBackoff {
    current_secs: u64,
}

impl ExponentialBackoff {
    const INITIAL_SECS: u64 = 2;
    const MAX_SECS: u64 = 60;

    fn new() -> Self {
        Self {
            current_secs: Self::INITIAL_SECS,
        }
    }

    fn current(&self) -> u64 {
        self.current_secs
    }

    fn reset(&mut self) {
        self.current_secs = Self::INITIAL_SECS;
    }

    /// Sleep out the current window in slices, so a shutdown request does
    /// not have to wait for the whole backoff, then widen it.
    fn wait_and_increment(&mut self, shutdown: &AtomicBool) {
        let window = Duration::from_secs(self.current_secs);
        let mut slept = Duration::ZERO;
        while slept < window && !shutdown.load(Ordering::SeqCst) {
            let slice = std::cmp::min(Duration::from_millis(250), window - slept);
            std::thread::sleep(slice);
            slept += slice;
        }
        self.current_secs = std::cmp::min(self.current_secs * 2, Self::MAX_SECS);
    }
}

#[derive(Parser, Debug)]
#[command(
    author,
    version,
    about = "Bridge Frigate events to Privacy Witness Kernel"
)]
struct Args {
    /// MQTT broker address.
    /// By default, only loopback addresses are allowed for security.
    /// Use --allow-remote-mqtt for trusted local network (e.g., Home Assistant).
    #[arg(long, env = "MQTT_BROKER_ADDR", default_value = "127.0.0.1:1883")]
    mqtt_broker_addr: String,

    /// Allow non-loopback MQTT connections.
    /// ONLY use this in trusted environments like Home Assistant where the
    /// MQTT broker (e.g., core-mosquitto) runs on a separate container.
    /// This does NOT weaken privacy guarantees - events are still sanitized.
    #[arg(long, env = "ALLOW_REMOTE_MQTT")]
    allow_remote_mqtt: bool,

    /// MQTT username for authentication.
    /// Required if your broker (like HA Mosquitto) requires authentication.
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

    /// Frigate MQTT topic to subscribe to. Deprecated full-topic override:
    /// when set to a non-default value it wins over --frigate-topic-prefix
    /// (events mode only).
    #[arg(long, env = "FRIGATE_MQTT_TOPIC", default_value = "frigate/events")]
    frigate_topic: String,

    /// Frigate's mqtt.topic_prefix. Subscribes to `<prefix>/events`, or to
    /// `<prefix>/reviews` with `--enable-reviews`.
    #[arg(long, env = "FRIGATE_TOPIC_PREFIX", default_value = "frigate")]
    frigate_topic_prefix: String,

    /// Reviews mode: subscribe to Frigate's review items (`<prefix>/reviews`,
    /// Frigate 0.14+) INSTEAD of `<prefix>/events`, and seal only review
    /// items of severity `alert` — once each, at the message that made them
    /// an alert, with confidence 1.0, one per camera per bucket. Without it
    /// (events mode) every tracked object is sealed at its `new` message,
    /// one per camera, object class and bucket.
    #[arg(long, env = "FRIGATE_ENABLE_REVIEWS")]
    enable_reviews: bool,

    /// MQTT client identifier.
    #[arg(long, env = "MQTT_CLIENT_ID", default_value = BRIDGE_NAME)]
    mqtt_client_id: String,

    /// Prefix of the kernel's own state topics (`witness`, the same prefix
    /// `event_mqtt_bridge` publishes under). This bridge publishes one
    /// retained payload there: `<prefix>/frigate_link`, the Frigate link
    /// health Home Assistant shows.
    #[arg(long, env = "MQTT_TOPIC_PREFIX", default_value = "witness")]
    mqtt_topic_prefix: String,

    /// Device identifier for Home Assistant, carried in `frigate_link`.
    /// Defaults to pwk_<8 hex chars of sha256("securacv:ha-device-id:v1:" ||
    /// device key seed)> — the derivation the add-on, the sidecar and
    /// `event_mqtt_bridge` use too, so every producer names one seed the
    /// same way.
    #[arg(long, env = "HA_DEVICE_ID")]
    ha_device_id: Option<String>,

    /// Path to witness database.
    #[arg(long, env = "WITNESS_DB_PATH", default_value = "witness.db")]
    db_path: String,

    /// Ruleset identifier for logged events.
    #[arg(long, env = "WITNESS_RULESET_ID", default_value = "ruleset:frigate_v1")]
    ruleset_id: String,

    /// Time bucket size in seconds (default: 10 minutes, minimum: 5 minutes per spec).
    /// Per spec/event_contract.md §3: bucket size is conformance-critical and MUST be >= 300.
    #[arg(long, env = "WITNESS_BUCKET_SIZE", default_value_t = 600)]
    bucket_size_secs: u64,

    /// Minimum confidence threshold (0.0-1.0). Events below this are ignored.
    #[arg(long, env = "FRIGATE_MIN_CONFIDENCE", default_value_t = 0.5)]
    min_confidence: f64,

    /// Comma-separated list of Frigate camera names to process.
    /// If empty, processes all cameras. Names are compared as slugs
    /// (ASCII-lowercased, every other character `_`), the rule the add-on
    /// wizard writes the list with, so `Front Door` matches `front_door`.
    #[arg(long, env = "FRIGATE_CAMERAS")]
    cameras: Option<String>,

    /// Comma-separated list of object labels to process (e.g., "person,car,dog").
    /// If empty, processes: person, car, dog, cat, bird, bicycle, motorcycle.
    /// `face` and `license_plate` seal nothing even when listed.
    #[arg(long, env = "FRIGATE_LABELS")]
    labels: Option<String>,

    /// Sealed-event retention in seconds (default: 7 days).
    #[arg(long, env = "WITNESS_RETENTION_SECS", default_value_t = 60 * 60 * 24 * 7)]
    retention_secs: u64,

    /// Seconds between retention passes in this process (default: 1 hour).
    /// A pass prunes events older than --retention-secs and writes a signed
    /// checkpoint over the prune cutoff. The first pass runs at start.
    #[arg(
        long,
        env = "WITNESS_RETENTION_CHECK_INTERVAL_SECS",
        default_value_t = 3600
    )]
    retention_check_interval_secs: u64,

    /// Seconds between checkpoint passes (default: 1 day). The store's only
    /// checkpoint primitive is the retention pass — it checkpoints the cutoff
    /// of what it prunes — so this timer runs that same pass; it matters
    /// when --retention-check-interval-secs is set longer than a day.
    #[arg(
        long,
        env = "WITNESS_CHECKPOINT_INTERVAL_SECS",
        default_value_t = 86400
    )]
    checkpoint_interval_secs: u64,
}

/// `pwk_` + the first 8 hex of sha256("securacv:ha-device-id:v1:" || seed):
/// the derivation the add-on (`run.sh`), the sidecar (`entrypoint.sh`) and
/// `event_mqtt_bridge` use, so every producer names one seed the same way.
/// Never a prefix of the seed itself — the id lands on a retained topic every
/// broker client can read.
fn ha_device_id_from_seed(seed: &str) -> String {
    let digest = Sha256::digest(format!("securacv:ha-device-id:v1:{seed}").as_bytes());
    format!("pwk_{}", hex::encode(&digest[..4]))
}

fn split_list(value: Option<&str>) -> Vec<String> {
    value
        .map(|s| {
            s.split(',')
                .map(str::trim)
                .filter(|c| !c.is_empty())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

/// What the bridge seals and what it skips: the operator's filters, normalized
/// once at start.
struct BridgeRules {
    mode: FrigateMode,
    /// Camera allowlist as slugs ([`camera_slug`]); `None` is every camera.
    allowed_camera_slugs: Option<Vec<String>>,
    /// Lowercase label allowlist.
    allowed_labels: Vec<String>,
    min_confidence: f64,
    bucket_size_s: u32,
}

/// Per-process counters the heartbeat reports as deltas.
#[derive(Debug, Default, Clone, Copy)]
struct Counters {
    /// Events appended to the sealed log by this process.
    events_sealed: u64,
    /// Failure records this process asked the kernel to seal.
    failures_recorded: u64,
}

/// Seals one heartbeat per bucket, carrying per-bucket counter deltas
/// (mirrors `witnessd`'s scheduler; see `HeartbeatRecord` in the kernel).
#[derive(Debug, Default)]
struct Heartbeats {
    last_bucket_start: Option<u64>,
    prev_events: u64,
    prev_failures: u64,
}

/// Timer bookkeeping for the chain housekeeping this process owns. Pure —
/// it answers "what is due at `now`" — so the schedule is testable with
/// synthetic instants; the kernel calls live on [`Bridge`].
struct Schedule {
    retention_interval: Duration,
    checkpoint_interval: Duration,
    /// `None` until the first check: a heartbeat is due at start.
    last_heartbeat: Option<Instant>,
    /// `None` until the first pass: retention runs at start, so a bridge
    /// that is restarted more often than its interval still prunes.
    last_retention: Option<Instant>,
    /// Starts "just run": the boot retention pass already checkpoints.
    last_checkpoint: Instant,
}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
struct Due {
    heartbeat: bool,
    retention: bool,
    checkpoint: bool,
}

impl Schedule {
    fn new(now: Instant, retention_interval: Duration, checkpoint_interval: Duration) -> Self {
        Self {
            retention_interval,
            checkpoint_interval,
            last_heartbeat: None,
            last_retention: None,
            last_checkpoint: now,
        }
    }

    /// What is due at `now`; each due item is marked as run so a failure is
    /// retried next interval, not every tick.
    fn due(&mut self, now: Instant) -> Due {
        let mut due = Due::default();
        if self
            .last_heartbeat
            .is_none_or(|at| now.duration_since(at) >= HEARTBEAT_INTERVAL)
        {
            due.heartbeat = true;
            self.last_heartbeat = Some(now);
        }
        if self
            .last_retention
            .is_none_or(|at| now.duration_since(at) >= self.retention_interval)
        {
            due.retention = true;
            self.last_retention = Some(now);
        }
        if now.duration_since(self.last_checkpoint) >= self.checkpoint_interval {
            due.checkpoint = true;
            self.last_checkpoint = now;
        }
        due
    }
}

/// The retained `<state_prefix>/frigate_link` payload (spec contract C5):
/// field order is the wire order. Coarse by construction — the last message
/// is a 10-minute bucket start, never the time a detection happened.
#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
struct FrigateLinkPayload {
    v: u32,
    device_id: String,
    connected: bool,
    topic_prefix: String,
    last_message_bucket_start: Option<u64>,
    last_message_bucket_size: u32,
    messages_since_start: u64,
    mode: &'static str,
}

/// Frigate link health, and when to republish it: on a change of
/// `connected` or of the last-message bucket, after every reconnect, and at
/// least every [`FRIGATE_LINK_REPUBLISH_INTERVAL`] — never per message, so a
/// busy camera does not turn into a retained publish per update.
struct FrigateLink {
    device_id: String,
    topic_prefix: String,
    mode: FrigateMode,
    connected: bool,
    last_message_bucket_start: Option<u64>,
    messages_since_start: u64,
    /// The payload the client last accepted, and when; `None` forces a publish.
    published: Option<(FrigateLinkPayload, Instant)>,
}

impl FrigateLink {
    fn new(device_id: String, topic_prefix: String, mode: FrigateMode) -> Self {
        Self {
            device_id,
            topic_prefix,
            mode,
            connected: false,
            last_message_bucket_start: None,
            messages_since_start: 0,
            published: None,
        }
    }

    fn payload(&self) -> FrigateLinkPayload {
        FrigateLinkPayload {
            v: 1,
            device_id: self.device_id.clone(),
            connected: self.connected,
            topic_prefix: self.topic_prefix.clone(),
            last_message_bucket_start: self.last_message_bucket_start,
            last_message_bucket_size: FRIGATE_LINK_BUCKET_SIZE_S,
            messages_since_start: self.messages_since_start,
            mode: self.mode.as_str(),
        }
    }

    fn set_connected(&mut self, connected: bool) {
        self.connected = connected;
    }

    /// A reconnect re-asserts the retained payload whatever it says.
    fn force_republish(&mut self) {
        self.published = None;
    }

    fn note_message(&mut self, bucket_start: u64) {
        self.messages_since_start = self.messages_since_start.saturating_add(1);
        self.last_message_bucket_start = Some(bucket_start);
    }

    fn publish_due(&self, now: Instant) -> bool {
        match &self.published {
            None => true,
            Some((prev, at)) => {
                prev.connected != self.connected
                    || prev.last_message_bucket_start != self.last_message_bucket_start
                    || now.duration_since(*at) >= FRIGATE_LINK_REPUBLISH_INTERVAL
            }
        }
    }

    fn mark_published(&mut self, payload: FrigateLinkPayload, now: Instant) {
        self.published = Some((payload, now));
    }
}

/// Outcome of one Frigate message.
#[derive(Debug, PartialEq, Eq)]
enum Processed {
    /// A claim was appended to the sealed log.
    Sealed,
    /// Nothing appended; the reason is a fixed word for the debug log, never
    /// payload content.
    Skipped(&'static str),
}

/// Dedup key for a claim. A tracked object dedupes per camera + base label
/// (the first and only label), a review alert per camera alone; the bucket
/// is the map's value, so one entry per key stays in the map.
fn dedup_key(parsed: &ParsedFrigateEvent) -> String {
    match parsed.kind {
        FrigateClaimKind::TrackedObject => format!(
            "{}:{}",
            parsed.camera,
            parsed
                .labels
                .first()
                .map(|label| label.to_lowercase())
                .unwrap_or_default()
        ),
        FrigateClaimKind::ReviewAlert => parsed.camera.clone(),
    }
}

/// The appending process: the kernel, the rules, and the chain housekeeping.
struct Bridge {
    kernel: Kernel,
    cfg: KernelConfig,
    module_desc: ModuleDescriptor,
    rules: BridgeRules,
    /// Dedup key → start of the bucket it was last sealed in.
    recent: HashMap<String, u64>,
    counters: Counters,
    heartbeats: Heartbeats,
    schedule: Schedule,
    link: FrigateLink,
}

impl Bridge {
    /// Parse one Frigate message by mode, apply the filters, dedupe, and
    /// seal. An `Err` is a message the parser could not turn into a claim
    /// (malformed, not a `new`, a detection, an identity label): logged at
    /// warn by the caller, never fatal.
    fn process_message(&mut self, payload: &[u8]) -> Result<Processed> {
        let parsed = match self.rules.mode {
            FrigateMode::Reviews => parse_review_event(payload)?,
            FrigateMode::Events => parse_frigate_event(payload)?,
        };

        // Filter by camera (slug rule shared with the wizard, contract C8)
        if let Some(allowed) = &self.rules.allowed_camera_slugs {
            if !allowed.contains(&camera_slug(&parsed.camera)) {
                log::debug!("Skipping camera: {}", parsed.camera);
                return Ok(Processed::Skipped("camera"));
            }
        }

        // Filter by label; identity labels never map to anything
        let Some(event_type) = select_event_type(&parsed.labels, &self.rules.allowed_labels) else {
            log::debug!("Skipping labels: {}", parsed.labels.join(","));
            return Ok(Processed::Skipped("label"));
        };

        // Filter by confidence
        if parsed.confidence < self.rules.min_confidence {
            log::debug!(
                "Skipping low confidence: {} < {}",
                parsed.confidence,
                self.rules.min_confidence
            );
            return Ok(Processed::Skipped("confidence"));
        }

        // Coarsen timestamp to bucket
        let bucket = TimeBucket::now(self.rules.bucket_size_s)?;

        // Deduplicate within the same bucket
        let key = dedup_key(&parsed);
        if self.recent.get(&key) == Some(&bucket.start_epoch_s) {
            log::debug!("Deduplicating claim in same bucket: {}", key);
            return Ok(Processed::Skipped("duplicate"));
        }
        self.recent.insert(key, bucket.start_epoch_s);

        // Clean old dedup entries
        let horizon = bucket
            .start_epoch_s
            .saturating_sub(2 * u64::from(self.rules.bucket_size_s));
        self.recent.retain(|_, &mut start| start >= horizon);

        // Determine zone_id (use Frigate zone or camera name)
        let zone_id = match parsed.zones.first() {
            Some(zone) => format!("zone:{}", sanitize_zone_name(zone)),
            None => format!("zone:{}", sanitize_zone_name(&parsed.camera)),
        };

        // Create candidate event (privacy-sanitized)
        let candidate = CandidateEvent {
            event_type,
            time_bucket: bucket,
            zone_id: zone_id.clone(),
            confidence: parsed.confidence as f32,
            correlation_token: None, // Never include Frigate's object IDs
            // Frigate claims are kernel-signed at ingest, like all adapter paths.
            attestation: Some(witness_kernel::Attestation::Adapter),
        };

        // Write to sealed log
        match self.kernel.append_event_checked(
            &self.module_desc,
            candidate,
            &self.cfg.kernel_version,
            &self.cfg.ruleset_id,
            self.cfg.ruleset_hash,
        ) {
            Ok(ev) => {
                self.counters.events_sealed += 1;
                log::info!(
                    "Event logged: {:?} zone={} conf={:.2}",
                    ev.event_type,
                    zone_id,
                    parsed.confidence
                );
                Ok(Processed::Sealed)
            }
            Err(e) => {
                log::warn!("Event rejected: {}", e);
                Ok(Processed::Skipped("rejected"))
            }
        }
    }

    /// Run whatever the schedule says is due at `now`.
    fn housekeeping(&mut self, now: Instant) {
        let due = self.schedule.due(now);
        if due.heartbeat {
            self.seal_heartbeat(false);
        }
        if due.retention || due.checkpoint {
            self.retention_pass();
        }
    }

    /// Seal the current bucket's heartbeat; with `force`, seal one even if
    /// this bucket already has one (the final heartbeat at clean shutdown
    /// covers the trailing partial bucket's activity).
    fn seal_heartbeat(&mut self, force: bool) {
        let bucket = match TimeBucket::now(self.rules.bucket_size_s) {
            Ok(bucket) => bucket,
            Err(e) => {
                log::error!("heartbeat skipped: {e}");
                return;
            }
        };
        if !force && self.heartbeats.last_bucket_start == Some(bucket.start_epoch_s) {
            return;
        }
        let events = self.counters.events_sealed;
        let failures = self.counters.failures_recorded;
        let healthy = self.link.connected;
        if let Err(e) = self.kernel.append_heartbeat(
            bucket,
            healthy,
            // The bridge sees Frigate's claims, never frames.
            0,
            events.saturating_sub(self.heartbeats.prev_events),
            failures.saturating_sub(self.heartbeats.prev_failures),
            &self.cfg.kernel_version,
            &self.cfg.ruleset_id,
            self.cfg.ruleset_hash,
        ) {
            // Heartbeats anchor the chain; a failed write is a storage fault.
            log::error!("failed to seal heartbeat record: {e}");
            self.report_storage_failure(&format!("heartbeat append failed: {e}"));
        } else {
            log::debug!(
                "heartbeat sealed for bucket {} (frigate link healthy={})",
                bucket.start_epoch_s,
                healthy
            );
        }
        self.heartbeats.last_bucket_start = Some(bucket.start_epoch_s);
        self.heartbeats.prev_events = events;
        self.heartbeats.prev_failures = failures;
    }

    /// Prune expired events and checkpoint the cutoff. A failure here is a
    /// storage fault to witness (sealed record), not a reason to exit.
    fn retention_pass(&mut self) {
        match self
            .kernel
            .enforce_retention_with_checkpoint(self.cfg.retention)
        {
            Ok(()) => log::debug!("retention pass complete"),
            Err(e) => {
                log::error!("retention enforcement failed: {e}");
                self.report_storage_failure(&format!("retention enforcement failed: {e}"));
            }
        }
    }

    fn report_storage_failure(&mut self, details: &str) {
        self.kernel.report_storage_failure(
            details,
            &self.cfg.kernel_version,
            &self.cfg.ruleset_id,
            self.cfg.ruleset_hash,
        );
        self.counters.failures_recorded += 1;
    }

    fn append_lifecycle(&mut self, phase: LifecyclePhase) -> Result<()> {
        self.kernel.append_lifecycle(
            phase,
            &self.cfg.kernel_version,
            &self.cfg.ruleset_id,
            self.cfg.ruleset_hash,
        )
    }

    /// Publish the retained link-health payload if it is due; `true` when
    /// the client accepted one. `try_publish` never blocks: a full request
    /// queue defers to the next tick rather than wedging the loop that
    /// drains it.
    fn publish_link_if_due(&mut self, client: &Client, topic: &str, now: Instant) -> bool {
        if !self.link.publish_due(now) {
            return false;
        }
        let payload = self.link.payload();
        let bytes = match serde_json::to_vec(&payload) {
            Ok(bytes) => bytes,
            Err(e) => {
                log::error!("frigate_link payload did not serialize: {e}");
                return false;
            }
        };
        match client.try_publish(topic, QoS::AtLeastOnce, true, bytes) {
            Ok(()) => {
                self.link.mark_published(payload, now);
                true
            }
            Err(e) => {
                log::debug!("frigate_link publish deferred: {e}");
                false
            }
        }
    }
}

/// Report-but-not-append safe mode: idle until shutdown, re-logging the boot
/// verification failure every minute. `witness_api` keeps serving the
/// (untrusted) log for inspection; this process seals nothing, because
/// extending an unverified tail would launder tampering into the chain.
fn run_safe_mode(shutdown: &AtomicBool, reason: &str) {
    let mut last_warn = Instant::now();
    while !shutdown.load(Ordering::SeqCst) {
        if last_warn.elapsed() >= Duration::from_secs(60) {
            log::error!("SAFE MODE active — not appending (reason: {reason})");
            last_warn = Instant::now();
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

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

    // Validate broker address
    if !args.allow_remote_mqtt {
        validate_loopback_addr(&mqtt_endpoint, &args.mqtt_broker_addr)?;
    } else {
        log::warn!("Remote MQTT enabled - ensure broker is in a trusted network");
    }

    // Validate bucket size per spec/event_contract.md §3
    if args.bucket_size_secs < MIN_BUCKET_SIZE_S as u64 {
        return Err(anyhow!(
            "bucket size {} seconds is below minimum {} seconds (5 minutes) per spec/event_contract.md §3",
            args.bucket_size_secs,
            MIN_BUCKET_SIZE_S
        ));
    }
    let bucket_size_s = u32::try_from(args.bucket_size_secs)
        .map_err(|_| anyhow!("bucket size {} seconds does not fit", args.bucket_size_secs))?;
    for (name, value) in [
        (
            "--retention-check-interval-secs",
            args.retention_check_interval_secs,
        ),
        ("--checkpoint-interval-secs", args.checkpoint_interval_secs),
    ] {
        if value == 0 {
            return Err(anyhow!("{name} must be at least 1"));
        }
    }

    // Parse allowed cameras (as slugs) and labels (lowercase)
    let allowed_camera_slugs: Option<Vec<String>> = args.cameras.as_deref().map(|list| {
        split_list(Some(list))
            .iter()
            .map(|camera| camera_slug(camera))
            .collect()
    });
    let mut allowed_labels: Vec<String> = split_list(args.labels.as_deref())
        .iter()
        .map(|label| label.to_lowercase())
        .collect();
    if allowed_labels.is_empty() {
        allowed_labels = DEFAULT_LABELS
            .iter()
            .map(|label| label.to_string())
            .collect();
    }

    let mode = if args.enable_reviews {
        FrigateMode::Reviews
    } else {
        FrigateMode::Events
    };
    // Topic selection: an explicitly overridden --frigate-topic (legacy,
    // full topic) wins in events mode; otherwise topics derive from
    // --frigate-topic-prefix. Exactly one topic per mode.
    let legacy_topic =
        (args.frigate_topic != "frigate/events").then_some(args.frigate_topic.as_str());
    let topics = frigate_subscriptions(mode, &args.frigate_topic_prefix, legacy_topic);

    log::info!("Frigate bridge starting");
    log::info!(
        "  MQTT broker: {}:{} (TLS: {})",
        mqtt_endpoint.host,
        mqtt_endpoint.port,
        mqtt_endpoint.use_tls
    );
    log::info!("  Mode: {} (topics: {})", mode, topics.join(", "));
    log::info!("  Database: {}", args.db_path);
    log::info!("  Min confidence: {}", args.min_confidence);
    log::info!("  Bucket size: {}s", bucket_size_s);
    log::info!(
        "  Cameras: {}",
        allowed_camera_slugs
            .as_ref()
            .map(|c| c.join(", "))
            .unwrap_or_else(|| "all".to_string())
    );
    log::info!("  Labels: {}", allowed_labels.join(", "));
    log::info!(
        "  Housekeeping: heartbeat every {}s, retention every {}s, checkpoint every {}s",
        HEARTBEAT_INTERVAL.as_secs(),
        args.retention_check_interval_secs,
        args.checkpoint_interval_secs
    );

    // Open kernel. The seed comes from DEVICE_KEY_SEED, else the seed file
    // witnessd keeps beside the database, else a fresh one is generated there
    // (never logged — only its source is).
    let device_key_seed = {
        let env_seed = std::env::var("DEVICE_KEY_SEED").ok();
        let resolved =
            witness_kernel::crypto::resolve_device_seed(&args.db_path, env_seed.as_deref())?;
        log::info!("device key seed: {}", resolved.source);
        resolved.seed
    };
    let device_id = args
        .ha_device_id
        .clone()
        .unwrap_or_else(|| ha_device_id_from_seed(&device_key_seed));
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);
    let cfg = KernelConfig {
        db_path: args.db_path.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(args.retention_secs),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let kernel = Kernel::open(&cfg)?;
    log::info!("Kernel opened: {} (device {})", args.db_path, device_id);

    // Graceful shutdown: SIGINT/SIGTERM set the flag; the main loop seals a
    // final heartbeat and a clean-shutdown record before exiting.
    let shutdown = Arc::new(AtomicBool::new(false));
    {
        let shutdown = Arc::clone(&shutdown);
        ctrlc::set_handler(move || shutdown.store(true, Ordering::SeqCst))
            .map_err(|e| anyhow!("failed to install shutdown signal handler: {e}"))?;
    }

    // Boot-time chain-tail verification, as witnessd does: verify the latest
    // checkpoint and the chain through the tail — against the signed
    // high-water mark when SECURACV_HWM_PATH is set — BEFORE extending it. A
    // tail that does not verify puts this process in report-but-not-append
    // safe mode; appending onto an untrusted tail would launder the tampering.
    let safe_mode_reason: Option<String> = match kernel.verify_sealed_log() {
        Ok(report) if report.chain_valid => None,
        Ok(report) => Some(
            report
                .error
                .unwrap_or_else(|| "sealed-log chain did not verify".to_string()),
        ),
        Err(e) => Some(format!("sealed log could not be verified at boot: {e:#}")),
    };
    if let Some(reason) = &safe_mode_reason {
        log::error!(
            "SAFE MODE: sealed-log tail failed boot verification — {reason}. \
             frigate_bridge will NOT append to the chain. Investigate and restore a \
             trusted log, then restart."
        );
        run_safe_mode(&shutdown, reason);
        log::info!("frigate_bridge stopped (safe mode)");
        return Ok(());
    }

    // Module descriptor for Frigate events
    let module_desc = ModuleDescriptor {
        id: "frigate_bridge",
        allowed_event_types: &[
            EventType::BoundaryCrossingObjectSmall,
            EventType::BoundaryCrossingObjectLarge,
        ],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };

    let now = Instant::now();
    let mut bridge = Bridge {
        kernel,
        cfg,
        module_desc,
        rules: BridgeRules {
            mode,
            allowed_camera_slugs,
            allowed_labels,
            min_confidence: args.min_confidence,
            bucket_size_s,
        },
        recent: HashMap::new(),
        counters: Counters::default(),
        heartbeats: Heartbeats::default(),
        schedule: Schedule::new(
            now,
            Duration::from_secs(args.retention_check_interval_secs),
            Duration::from_secs(args.checkpoint_interval_secs),
        ),
        link: FrigateLink::new(device_id, args.frigate_topic_prefix.clone(), mode),
    };

    // Witness the process lifecycle. A trailing `start` with no clean
    // shutdown means the previous run died uncleanly — seal a PowerLoss
    // failure record (proxy for power loss, crash, or kill; see
    // docs/failure_semantics.md). Then seal this run's start record —
    // fail-closed: if we cannot witness our own startup we must not run.
    match bridge.kernel.last_lifecycle_phase() {
        Ok(Some(LifecyclePhase::Start)) => {
            log::warn!("previous run did not shut down cleanly; sealing PowerLoss failure record");
            if let Err(e) = bridge.kernel.append_failure_event(
                FailureType::PowerLoss,
                TimeBucket::now_10min()?,
                Some("unclean_shutdown".to_string()),
                &bridge.cfg.kernel_version,
                &bridge.cfg.ruleset_id,
                bridge.cfg.ruleset_hash,
            ) {
                log::error!("failed to seal PowerLoss record: {e}");
            }
        }
        Ok(_) => {}
        Err(e) => log::error!("could not determine previous lifecycle phase: {e}"),
    }
    bridge.append_lifecycle(LifecyclePhase::Start)?;

    // The boot pass: a heartbeat for this bucket and the first retention pass.
    bridge.housekeeping(Instant::now());

    let frigate_link_topic = format!("{}/{}", args.mqtt_topic_prefix, FRIGATE_LINK_TOPIC_SUFFIX);

    // Exponential backoff for reconnection
    let mut backoff = ExponentialBackoff::new();

    // Main loop - process MQTT messages with automatic reconnection. The
    // eventloop is polled with a timeout so the housekeeping timers run
    // between messages and while the broker is down.
    'connect: loop {
        if shutdown.load(Ordering::SeqCst) {
            break;
        }

        let connect_result = connect_mqtt(
            &mqtt_endpoint,
            &tls_config,
            &args.mqtt_client_id,
            args.mqtt_username.as_deref(),
            args.mqtt_password.as_deref(),
        );

        let (client, mut connection) = match connect_result {
            Ok((c, conn)) => (c, conn),
            Err(e) => {
                log::error!(
                    "Failed to connect to MQTT broker: {}. Retrying in {} seconds...",
                    e,
                    backoff.current()
                );
                bridge.housekeeping(Instant::now());
                backoff.wait_and_increment(&shutdown);
                continue;
            }
        };

        // Subscribe with QoS 1 (AtLeastOnce) for reliable message delivery.
        // Queued now, sent on the wire once the connection is up.
        let mut subscribe_failed = false;
        for topic in &topics {
            if let Err(e) = client.subscribe(topic, QoS::AtLeastOnce) {
                log::error!(
                    "Failed to subscribe to {}: {}. Retrying in {} seconds...",
                    topic,
                    e,
                    backoff.current()
                );
                subscribe_failed = true;
                break;
            }
            log::info!("Subscribed to {} (QoS 1)", topic);
        }
        if subscribe_failed {
            backoff.wait_and_increment(&shutdown);
            continue;
        }

        loop {
            if shutdown.load(Ordering::SeqCst) {
                finish_connection(&client, &mut connection, &mut bridge, &frigate_link_topic);
                break 'connect;
            }

            let now = Instant::now();
            bridge.housekeeping(now);
            if bridge.link.connected {
                bridge.publish_link_if_due(&client, &frigate_link_topic, now);
            }

            match connection.recv_timeout(LOOP_TICK) {
                Ok(Ok(Event::Incoming(Incoming::ConnAck(_)))) => {
                    backoff.reset();
                    log::info!("MQTT broker connected");
                    bridge.link.set_connected(true);
                    bridge.link.force_republish();
                }
                Ok(Ok(Event::Incoming(Incoming::Publish(publish)))) => {
                    let topic = match std::str::from_utf8(&publish.topic) {
                        Ok(topic) => topic,
                        Err(e) => {
                            log::warn!("Skipping publish with invalid topic: {}", e);
                            continue;
                        }
                    };
                    // Every message on the subscribed topic is "Frigate
                    // heard", whatever it says.
                    match TimeBucket::now(FRIGATE_LINK_BUCKET_SIZE_S) {
                        Ok(bucket) => bridge.link.note_message(bucket.start_epoch_s),
                        Err(e) => log::error!("clock error: {e}"),
                    }
                    match bridge.process_message(&publish.payload) {
                        Ok(Processed::Sealed) => {}
                        Ok(Processed::Skipped(reason)) => {
                            log::debug!("message on {} skipped ({})", topic, reason);
                        }
                        Err(e) => log::warn!("Failed to process message on {}: {}", topic, e),
                    }
                }
                Ok(Ok(_)) => {}
                Ok(Err(e)) => {
                    log::error!("MQTT connection error: {}. Reconnecting...", e);
                    bridge.link.set_connected(false);
                    break;
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => {
                    log::warn!("MQTT connection closed unexpectedly. Reconnecting...");
                    bridge.link.set_connected(false);
                    break;
                }
            }
        }

        log::info!(
            "Attempting reconnection with {} second backoff...",
            backoff.current()
        );
        backoff.wait_and_increment(&shutdown);
    }

    // Seal a final heartbeat (covering the trailing partial bucket's
    // activity) and the clean-shutdown record, so the next boot can
    // distinguish a deliberate stop from power loss / crash.
    log::info!("shutdown signal received; sealing clean-shutdown record");
    bridge.seal_heartbeat(true);
    if let Err(e) = bridge.append_lifecycle(LifecyclePhase::ShutdownClean) {
        log::error!("failed to seal clean-shutdown record: {e}");
    }
    log::info!("frigate_bridge stopped");
    Ok(())
}

/// Clean shutdown of a live connection: say the Frigate link is down on
/// purpose (retained, so Home Assistant does not read a stale "connected"),
/// wait — bounded — for the broker to acknowledge it, then send DISCONNECT
/// at once and let it leave the socket. `disconnect_now` rather than the
/// graceful barrier: the barrier's drain is driven by a poll this loop
/// times out and re-arms, which left the DISCONNECT unsent.
fn finish_connection(
    client: &Client,
    connection: &mut Connection,
    bridge: &mut Bridge,
    frigate_link_topic: &str,
) {
    bridge.link.set_connected(false);
    bridge.link.force_republish();
    let awaiting_ack = bridge.publish_link_if_due(client, frigate_link_topic, Instant::now());

    let deadline = Instant::now() + SHUTDOWN_FLUSH;
    // Half the window for the acknowledgment, the rest for the DISCONNECT.
    let ack_deadline = Instant::now() + SHUTDOWN_FLUSH / 2;
    let mut disconnect_requested = false;
    let request_disconnect = |requested: &mut bool| {
        if !*requested {
            *requested = true;
            if let Err(e) = client.disconnect_now() {
                log::debug!("MQTT disconnect request failed: {e}");
            }
        }
    };
    if !awaiting_ack {
        request_disconnect(&mut disconnect_requested);
    }
    while Instant::now() < deadline {
        if !disconnect_requested && Instant::now() >= ack_deadline {
            log::debug!("final frigate_link publish unacknowledged; disconnecting anyway");
            request_disconnect(&mut disconnect_requested);
        }
        match connection.recv_timeout(Duration::from_millis(100)) {
            Ok(Ok(Event::Incoming(Incoming::PubAck(_)))) => {
                request_disconnect(&mut disconnect_requested);
            }
            Ok(Ok(Event::Outgoing(rumqttc::Outgoing::Disconnect))) => {
                log::debug!("MQTT DISCONNECT sent");
                break;
            }
            Ok(Ok(event)) => log::debug!("MQTT shutdown drain: {event:?}"),
            Ok(Err(e)) => {
                log::debug!("MQTT connection ended during shutdown: {e}");
                break;
            }
            Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {}
        }
    }
}

fn connect_mqtt(
    endpoint: &MqttEndpoint,
    tls_config: &TlsConfig,
    client_id: &str,
    username: Option<&str>,
    password: Option<&str>,
) -> Result<(Client, Connection)> {
    let mut options = MqttOptions::new(client_id, (endpoint.host.as_str(), endpoint.port));
    options.set_keep_alive(60);
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default().to_string());
    }
    options.set_transport(tls_config.build_transport(endpoint)?);

    let (client, connection) = rumqttc::ClientBuilder::new(options).capacity(10).build();
    log::info!(
        "Connecting to MQTT broker (TLS: {}, backend: {}, auth: {})",
        endpoint.use_tls,
        tls_config.backend,
        username.is_some()
    );
    Ok((client, connection))
}

#[cfg(test)]
mod tests {
    use super::*;

    const SEED: &str = "devkey:frigate_bridge_test:00112233445566778899aabbccddeeff";

    fn test_bridge(db_path: String, retention: Duration, mode: FrigateMode) -> Bridge {
        let cfg = KernelConfig {
            db_path,
            ruleset_id: "ruleset:frigate_v1".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:frigate_v1"),
            kernel_version: "0.0.0-test".to_string(),
            retention,
            device_key_seed: SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg).expect("open kernel");
        let now = Instant::now();
        Bridge {
            kernel,
            cfg,
            module_desc: ModuleDescriptor {
                id: "frigate_bridge",
                allowed_event_types: &[
                    EventType::BoundaryCrossingObjectSmall,
                    EventType::BoundaryCrossingObjectLarge,
                ],
                requested_capabilities: &[],
                supported_backends: &[InferenceBackend::Stub],
            },
            rules: BridgeRules {
                mode,
                allowed_camera_slugs: None,
                allowed_labels: DEFAULT_LABELS.iter().map(|l| l.to_string()).collect(),
                min_confidence: 0.5,
                bucket_size_s: 600,
            },
            recent: HashMap::new(),
            counters: Counters::default(),
            heartbeats: Heartbeats::default(),
            schedule: Schedule::new(now, Duration::from_secs(3600), Duration::from_secs(86400)),
            link: FrigateLink::new("pwk_test0000".to_string(), "frigate".to_string(), mode),
        }
    }

    /// Payload strings of the sealed log, in append order.
    fn payloads(bridge: &Bridge) -> Vec<String> {
        bridge
            .kernel
            .sealed_log_document()
            .expect("sealed log document")
            .entries
            .into_iter()
            .map(|entry| entry.payload)
            .collect()
    }

    fn count_records(bridge: &Bridge, record_type: &str) -> usize {
        let tag = format!("{{\"record_type\":\"{record_type}\"");
        payloads(bridge)
            .iter()
            .filter(|p| p.starts_with(&tag))
            .count()
    }

    const PERSON_NEW: &str = r#"{"after":{"camera":"Front Door","label":"person","top_score":0.9,
        "sub_label":["Someone Known",0.8],"entered_zones":["porch"]},"type":"new"}"#;

    #[test]
    fn ha_device_id_matches_the_shell_derivation() {
        // run.sh / entrypoint.sh: pwk_$(printf '%s' "securacv:ha-device-id:v1:${seed}" | sha256sum | cut -c1-8)
        let expected = format!(
            "pwk_{}",
            &hex::encode(Sha256::digest(b"securacv:ha-device-id:v1:devkey:x"))[..8]
        );
        assert_eq!(ha_device_id_from_seed("devkey:x"), expected);
        assert_eq!(ha_device_id_from_seed("devkey:x").len(), 12);
    }

    #[test]
    fn schedule_runs_heartbeat_and_retention_at_boot_then_on_interval() {
        let t0 = Instant::now();
        let mut schedule = Schedule::new(t0, Duration::from_secs(3600), Duration::from_secs(86400));
        assert_eq!(
            schedule.due(t0),
            Due {
                heartbeat: true,
                retention: true,
                checkpoint: false
            }
        );
        // Nothing is due again right away.
        assert_eq!(schedule.due(t0 + Duration::from_secs(1)), Due::default());
        // The heartbeat check comes back every 300 s, retention hourly,
        // the checkpoint pass daily.
        assert_eq!(
            schedule.due(t0 + Duration::from_secs(300)),
            Due {
                heartbeat: true,
                retention: false,
                checkpoint: false
            }
        );
        assert_eq!(
            schedule.due(t0 + Duration::from_secs(3600)),
            Due {
                heartbeat: true,
                retention: true,
                checkpoint: false
            }
        );
        assert_eq!(
            schedule.due(t0 + Duration::from_secs(86400)),
            Due {
                heartbeat: true,
                retention: true,
                checkpoint: true
            }
        );
    }

    #[test]
    fn frigate_link_payload_serializes_in_contract_order() {
        let mut link = FrigateLink::new(
            "pwk_1a2b3c4d".to_string(),
            "frigate".to_string(),
            FrigateMode::Events,
        );
        link.set_connected(true);
        link.note_message(1_760_140_800);
        let json = serde_json::to_string(&link.payload()).expect("serialize");
        assert_eq!(
            json,
            r#"{"v":1,"device_id":"pwk_1a2b3c4d","connected":true,"topic_prefix":"frigate","last_message_bucket_start":1760140800,"last_message_bucket_size":600,"messages_since_start":1,"mode":"events"}"#
        );
        let quiet = FrigateLink::new(
            "pwk_1a2b3c4d".to_string(),
            "frigate".to_string(),
            FrigateMode::Reviews,
        );
        let json = serde_json::to_string(&quiet.payload()).expect("serialize");
        assert!(json.contains(r#""last_message_bucket_start":null"#));
        assert!(json.contains(r#""mode":"reviews""#));
    }

    #[test]
    fn frigate_link_publishes_on_change_and_interval_not_per_message() {
        let t0 = Instant::now();
        let mut link = FrigateLink::new(
            "pwk_x".to_string(),
            "frigate".to_string(),
            FrigateMode::Events,
        );
        // Nothing published yet: due.
        assert!(link.publish_due(t0));
        link.set_connected(true);
        link.mark_published(link.payload(), t0);
        assert!(!link.publish_due(t0 + Duration::from_secs(1)));

        // A message in a new bucket is a change…
        link.note_message(600);
        assert!(link.publish_due(t0 + Duration::from_secs(2)));
        link.mark_published(link.payload(), t0 + Duration::from_secs(2));
        // …but more messages in the same bucket are not.
        link.note_message(600);
        link.note_message(600);
        assert!(!link.publish_due(t0 + Duration::from_secs(3)));
        assert_eq!(link.payload().messages_since_start, 3);

        // The interval republishes regardless.
        assert!(link.publish_due(t0 + Duration::from_secs(2) + FRIGATE_LINK_REPUBLISH_INTERVAL));

        // A connection flip is a change; a reconnect forces one.
        link.mark_published(link.payload(), t0 + Duration::from_secs(4));
        link.set_connected(false);
        assert!(link.publish_due(t0 + Duration::from_secs(5)));
        link.set_connected(true);
        link.mark_published(link.payload(), t0 + Duration::from_secs(6));
        assert!(!link.publish_due(t0 + Duration::from_secs(7)));
        link.force_republish();
        assert!(link.publish_due(t0 + Duration::from_secs(7)));
    }

    #[test]
    fn dedup_key_is_camera_and_base_label_for_objects_and_camera_for_reviews() {
        let object = ParsedFrigateEvent {
            camera: "front_door".to_string(),
            labels: vec!["Person".to_string()],
            confidence: 0.9,
            zones: vec![],
            kind: FrigateClaimKind::TrackedObject,
        };
        assert_eq!(dedup_key(&object), "front_door:person");
        let review = ParsedFrigateEvent {
            camera: "front_door".to_string(),
            labels: vec!["person".to_string(), "dog".to_string()],
            confidence: 1.0,
            zones: vec![],
            kind: FrigateClaimKind::ReviewAlert,
        };
        assert_eq!(dedup_key(&review), "front_door");
    }

    #[test]
    fn process_message_seals_once_per_bucket_and_refuses_identity_labels() {
        let mut bridge = test_bridge(
            witness_kernel::shared_memory_uri(),
            Duration::from_secs(3600),
            FrigateMode::Events,
        );
        assert_eq!(
            bridge
                .process_message(PERSON_NEW.as_bytes())
                .expect("first"),
            Processed::Sealed
        );
        assert_eq!(
            bridge.process_message(PERSON_NEW.as_bytes()).expect("dup"),
            Processed::Skipped("duplicate")
        );
        // A different base label in the same bucket is its own claim.
        let dog = PERSON_NEW.replace("\"person\"", "\"dog\"");
        assert_eq!(
            bridge.process_message(dog.as_bytes()).expect("dog"),
            Processed::Sealed
        );
        // Identity labels never reach the kernel, allowlisted or not.
        bridge.rules.allowed_labels.push("face".to_string());
        let face = PERSON_NEW.replace("\"person\"", "\"face\"");
        assert!(bridge.process_message(face.as_bytes()).is_err());
        assert_eq!(bridge.counters.events_sealed, 2);
        assert_eq!(count_records(&bridge, "event"), 2);
        // The recognized name in sub_label is nowhere in the sealed log.
        assert!(payloads(&bridge)
            .iter()
            .all(|p| !p.contains("Someone Known")));
    }

    #[test]
    fn process_message_compares_cameras_as_slugs() {
        let mut bridge = test_bridge(
            witness_kernel::shared_memory_uri(),
            Duration::from_secs(3600),
            FrigateMode::Events,
        );
        bridge.rules.allowed_camera_slugs = Some(vec![camera_slug("front-door")]);
        // Payload camera "Front Door" slugs to front_door == slug("front-door").
        assert_eq!(
            bridge
                .process_message(PERSON_NEW.as_bytes())
                .expect("slug match"),
            Processed::Sealed
        );
        bridge.rules.allowed_camera_slugs = Some(vec![camera_slug("garage")]);
        let other = PERSON_NEW.replace("\"dog\"", "\"cat\"");
        assert_eq!(
            bridge.process_message(other.as_bytes()).expect("no match"),
            Processed::Skipped("camera")
        );
    }

    #[test]
    fn reviews_mode_seals_alerts_once_per_camera_and_bucket() {
        let mut bridge = test_bridge(
            witness_kernel::shared_memory_uri(),
            Duration::from_secs(3600),
            FrigateMode::Reviews,
        );
        let alert = r#"{"type":"new","before":null,"after":{"camera":"front_door","severity":"alert",
            "data":{"objects":["person"],"zones":["porch"],"sub_labels":["Someone Known"]}}}"#;
        assert_eq!(
            bridge.process_message(alert.as_bytes()).expect("alert"),
            Processed::Sealed
        );
        // Another alert on the same camera in the same bucket, different objects: one row.
        let second = alert.replace("[\"person\"]", "[\"car\"]");
        assert_eq!(
            bridge.process_message(second.as_bytes()).expect("second"),
            Processed::Skipped("duplicate")
        );
        let detection = alert.replace("\"severity\":\"alert\"", "\"severity\":\"detection\"");
        assert!(bridge.process_message(detection.as_bytes()).is_err());
        assert_eq!(count_records(&bridge, "event"), 1);
        assert!(payloads(&bridge)
            .iter()
            .all(|p| !p.contains("Someone Known")));
    }

    #[test]
    fn heartbeats_seal_once_per_bucket_and_always_when_forced() {
        let mut bridge = test_bridge(
            witness_kernel::shared_memory_uri(),
            Duration::from_secs(3600),
            FrigateMode::Events,
        );
        bridge.seal_heartbeat(false);
        bridge.seal_heartbeat(false);
        assert_eq!(count_records(&bridge, "heartbeat"), 1);
        bridge.seal_heartbeat(true);
        assert_eq!(count_records(&bridge, "heartbeat"), 2);
        // The heartbeat carries deltas, never cumulative counters.
        bridge.process_message(PERSON_NEW.as_bytes()).expect("seal");
        bridge.seal_heartbeat(true);
        let last = payloads(&bridge).pop().expect("last record");
        assert!(last.contains("\"events_appended_delta\":1"), "{last}");
        bridge.seal_heartbeat(true);
        let last = payloads(&bridge).pop().expect("last record");
        assert!(last.contains("\"events_appended_delta\":0"), "{last}");
    }

    #[test]
    fn retention_pass_prunes_expired_events_and_checkpoints_the_cutoff() {
        let dir = tempfile::tempdir().expect("tempdir");
        let db_path = dir.path().join("witness.db");
        let mut bridge = test_bridge(
            db_path.to_str().expect("utf8").to_string(),
            Duration::from_secs(3600),
            FrigateMode::Events,
        );
        bridge.process_message(PERSON_NEW.as_bytes()).expect("seal");
        assert_eq!(count_records(&bridge, "event"), 1);
        // A pass inside retention prunes nothing and writes no checkpoint.
        bridge.retention_pass();
        assert_eq!(count_records(&bridge, "event"), 1);
        let checkpoints = |bridge: &Bridge| -> i64 {
            bridge
                .kernel
                .conn
                .query_row("SELECT COUNT(*) FROM checkpoints", [], |r| r.get(0))
                .expect("count checkpoints")
        };
        assert_eq!(checkpoints(&bridge), 0);

        // Age the row past retention. Retention ages rows by their sealed
        // `created_at` (the coarse bucket start, a plain column outside the
        // hash), which is how the store's own tests age rows too.
        bridge
            .kernel
            .conn
            .execute(
                "UPDATE sealed_events SET created_at = created_at - 7200",
                [],
            )
            .expect("age the row");
        bridge.retention_pass();
        assert_eq!(count_records(&bridge, "event"), 0);
        assert_eq!(checkpoints(&bridge), 1);
        assert_eq!(bridge.counters.failures_recorded, 0);
    }
}
