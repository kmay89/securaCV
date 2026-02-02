//! frigate_bridge - Subscribe to Frigate MQTT events and convert to privacy-preserving PWK events.
//!
//! This bridge enables PWK to work with Frigate NVR:
//! 1. Subscribes to Frigate's MQTT event topics (frigate/events, frigate/reviews)
//! 2. Strips identity data (object IDs, precise coordinates, thumbnails)
//! 3. Coarsens timestamps to PWK time buckets
//! 4. Maps Frigate object types to PWK event types
//! 5. Writes sanitized events to the PWK sealed log
//!
//! This allows users to leverage Frigate's excellent ML detection while
//! maintaining PWK's privacy guarantees for long-term event storage.

use anyhow::{anyhow, Context, Result};
use clap::Parser;
use rumqttc::v5::{mqttbytes::QoS, Client, Connection, Event, Incoming, MqttOptions};
use serde::Deserialize;
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Duration;

use witness_kernel::transport::{
    parse_mqtt_endpoint, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy,
};

const BRIDGE_NAME: &str = "frigate_bridge";

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

    /// Frigate MQTT topic to subscribe to.
    #[arg(long, env = "FRIGATE_MQTT_TOPIC", default_value = "frigate/events")]
    frigate_topic: String,

    /// MQTT client identifier.
    #[arg(long, env = "MQTT_CLIENT_ID", default_value = BRIDGE_NAME)]
    mqtt_client_id: String,

    /// Path to witness database.
    #[arg(long, env = "WITNESS_DB_PATH", default_value = "witness.db")]
    db_path: String,

    /// Ruleset identifier for logged events.
    #[arg(long, env = "WITNESS_RULESET_ID", default_value = "ruleset:frigate_v1")]
    ruleset_id: String,

    /// Time bucket size in seconds (default: 10 minutes).
    #[arg(long, env = "WITNESS_BUCKET_SIZE", default_value_t = 600)]
    bucket_size_secs: u64,

    /// Minimum confidence threshold (0.0-1.0). Events below this are ignored.
    #[arg(long, env = "FRIGATE_MIN_CONFIDENCE", default_value_t = 0.5)]
    min_confidence: f64,

    /// Comma-separated list of Frigate camera names to process.
    /// If empty, processes all cameras.
    #[arg(long, env = "FRIGATE_CAMERAS")]
    cameras: Option<String>,

    /// Comma-separated list of object labels to process (e.g., "person,car,dog").
    /// If empty, processes: person, car, dog, cat, bird, bicycle, motorcycle.
    #[arg(long, env = "FRIGATE_LABELS")]
    labels: Option<String>,
}

/// Frigate event from MQTT - handles the nested before/after format.
/// Frigate publishes: { "before": {...}, "after": {...}, "type": "new"|"update"|"end" }
#[derive(Debug, Deserialize)]
struct FrigateEventWrapper {
    /// The "after" state contains current detection info
    after: Option<FrigateEventData>,

    /// Event type: "new", "update", or "end"
    #[serde(rename = "type")]
    event_type: Option<String>,
}

/// Inner event data from Frigate (the "after" section).
#[derive(Debug, Deserialize)]
struct FrigateEventData {
    /// Event ID (will be stripped - not logged)
    #[allow(dead_code)]
    id: Option<String>,

    /// Camera name (mapped to zone_id)
    camera: String,

    /// Object label (person, car, dog, etc.)
    label: String,

    /// Sub-label for more specific classification (e.g., "amazon" for package)
    #[serde(default)]
    sub_label: Option<String>,

    /// Detection confidence (0.0-1.0)
    #[serde(default)]
    score: f64,

    /// Top score seen for this object (use this if available)
    top_score: Option<f64>,

    /// Current zones the object is in (use first if available)
    #[serde(default)]
    current_zones: Vec<String>,

    /// All zones the object has entered during tracking
    #[serde(default)]
    entered_zones: Vec<String>,

    /// Whether this is a false positive (skip if true)
    #[serde(default)]
    false_positive: bool,

    /// Whether a clip is available for this event
    #[serde(default)]
    has_clip: bool,

    /// Whether a snapshot is available for this event
    #[serde(default)]
    has_snapshot: bool,
    // Fields we intentionally ignore for privacy:
    // - thumbnail: raw image data
    // - snapshot: raw image data
    // - box: precise bounding box coordinates
    // - region: detection region
    // - stationary: tracking state
    // - motionless_count: tracking state
    // - position_changes: movement tracking
    // - start_time: precise timestamp (we coarsen to buckets)
    // - end_time: precise timestamp
}

/// Frigate review event (alternative topic for confirmations).
#[derive(Debug, Deserialize)]
struct FrigateReview {
    /// Type: "new", "update", or "end"
    #[serde(rename = "type")]
    review_type: Option<String>,

    /// Review ID
    #[allow(dead_code)]
    id: Option<String>,

    /// Camera name
    camera: String,

    /// Detection data
    data: Option<FrigateReviewData>,
}

#[derive(Debug, Deserialize)]
struct FrigateReviewData {
    /// Object detections in this review
    #[serde(default)]
    objects: Vec<String>,

    /// Detection scores by object type
    #[serde(default)]
    score: Option<f64>,

    /// Zones involved
    #[serde(default)]
    zones: Vec<String>,
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let mqtt_endpoint = parse_mqtt_endpoint(&args.mqtt_broker_addr, args.mqtt_use_tls)?;
    let tls_backend = TlsBackend::from_str(&args.mqtt_tls_backend)?;
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

    // Parse allowed cameras and labels
    let allowed_cameras: Option<Vec<String>> = args.cameras.as_ref().map(|s| {
        s.split(',')
            .map(|c| c.trim().to_lowercase())
            .filter(|c| !c.is_empty())
            .collect()
    });

    let allowed_labels: Vec<String> = args
        .labels
        .as_ref()
        .map(|s| {
            s.split(',')
                .map(|l| l.trim().to_lowercase())
                .filter(|l| !l.is_empty())
                .collect()
        })
        .unwrap_or_else(|| {
            vec![
                "person".to_string(),
                "car".to_string(),
                "dog".to_string(),
                "cat".to_string(),
                "bird".to_string(),
                "bicycle".to_string(),
                "motorcycle".to_string(),
            ]
        });

    log::info!("Frigate bridge starting");
    log::info!(
        "  MQTT broker: {}:{} (TLS: {})",
        mqtt_endpoint.host,
        mqtt_endpoint.port,
        mqtt_endpoint.use_tls
    );
    log::info!("  Frigate topic: {}", args.frigate_topic);
    log::info!("  Database: {}", args.db_path);
    log::info!("  Min confidence: {}", args.min_confidence);
    log::info!("  Bucket size: {}s", args.bucket_size_secs);
    log::info!(
        "  Cameras: {}",
        allowed_cameras
            .as_ref()
            .map(|c| c.join(", "))
            .unwrap_or_else(|| "all".to_string())
    );
    log::info!("  Labels: {}", allowed_labels.join(", "));

    // Open kernel
    let device_key_seed =
        std::env::var("DEVICE_KEY_SEED").map_err(|_| anyhow!("DEVICE_KEY_SEED must be set"))?;
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);
    let cfg = KernelConfig {
        db_path: args.db_path.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7), // 7 days
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let mut kernel = Kernel::open(&cfg)?;
    log::info!("Kernel opened: {}", args.db_path);

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

    // Connect to MQTT and subscribe
    // Event deduplication (by camera+label within same bucket)
    let mut recent_events: HashMap<String, u64> = HashMap::new();

    // Main loop - process MQTT messages
    loop {
        let (client, mut connection) = connect_mqtt(
            &mqtt_endpoint,
            &tls_config,
            &args.mqtt_client_id,
            args.mqtt_username.as_deref(),
            args.mqtt_password.as_deref(),
        )?;
        client.subscribe(&args.frigate_topic, QoS::AtMostOnce)?;
        log::info!("Subscribed to {}", args.frigate_topic);

        let mut should_reconnect = false;
        for event in connection.iter() {
            match event {
                Ok(Event::Incoming(Incoming::Publish(publish))) => {
                    let topic = match std::str::from_utf8(&publish.topic) {
                        Ok(topic) => topic.to_string(),
                        Err(e) => {
                            log::warn!("Skipping publish with invalid topic: {}", e);
                            continue;
                        }
                    };
                    let payload = publish.payload.to_vec();
                    if let Err(e) = process_message(
                        &topic,
                        &payload,
                        &mut kernel,
                        &module_desc,
                        &cfg,
                        &allowed_cameras,
                        &allowed_labels,
                        args.min_confidence,
                        args.bucket_size_secs,
                        &mut recent_events,
                    ) {
                        log::warn!("Failed to process message: {}", e);
                    }
                }
                Ok(_) => {}
                Err(e) => {
                    log::error!("MQTT connection error: {}. Reconnecting...", e);
                    should_reconnect = true;
                    break;
                }
            }
        }

        if should_reconnect {
            std::thread::sleep(Duration::from_secs(5));
            continue;
        }

        log::warn!("MQTT connection closed. Reconnecting...");
        std::thread::sleep(Duration::from_secs(5));
    }
}

#[allow(clippy::too_many_arguments)]
fn process_message(
    topic: &str,
    payload: &[u8],
    kernel: &mut Kernel,
    module_desc: &ModuleDescriptor,
    cfg: &KernelConfig,
    allowed_cameras: &Option<Vec<String>>,
    allowed_labels: &[String],
    min_confidence: f64,
    bucket_size_secs: u64,
    recent_events: &mut HashMap<String, u64>,
) -> Result<()> {
    // Try to parse as event or review
    let (camera, label, confidence, zones) = if topic.contains("/reviews") {
        parse_review_event(payload)?
    } else {
        parse_frigate_event(payload)?
    };

    // Filter by camera
    if let Some(allowed) = allowed_cameras {
        if !allowed.contains(&camera.to_lowercase()) {
            log::debug!("Skipping camera: {}", camera);
            return Ok(());
        }
    }

    // Filter by label
    if !allowed_labels.contains(&label.to_lowercase()) {
        log::debug!("Skipping label: {}", label);
        return Ok(());
    }

    // Filter by confidence
    if confidence < min_confidence {
        log::debug!(
            "Skipping low confidence: {} < {}",
            confidence,
            min_confidence
        );
        return Ok(());
    }

    // Coarsen timestamp to bucket
    let bucket = TimeBucket::now(bucket_size_secs as u32)?;

    // Deduplicate within same bucket
    let dedup_key = format!("{}:{}:{}", camera, label, bucket.start_epoch_s);
    if let Some(&last_bucket) = recent_events.get(&dedup_key) {
        if last_bucket == bucket.start_epoch_s {
            log::debug!("Deduplicating event in same bucket: {}", dedup_key);
            return Ok(());
        }
    }
    recent_events.insert(dedup_key, bucket.start_epoch_s);

    // Clean old dedup entries
    recent_events.retain(|_, &mut v| {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        now.saturating_sub(v) < bucket_size_secs * 2
    });

    // Map Frigate label to PWK event type
    let event_type = map_label_to_event_type(&label);

    // Determine zone_id (use Frigate zone or camera name)
    let zone_id = if let Some(zone) = zones.first() {
        format!("zone:{}", sanitize_zone_name(zone))
    } else {
        format!("zone:{}", sanitize_zone_name(&camera))
    };

    // Create candidate event (privacy-sanitized)
    let candidate = CandidateEvent {
        event_type,
        time_bucket: bucket,
        zone_id: zone_id.clone(),
        confidence: confidence as f32,
        correlation_token: None, // Never include Frigate's object IDs
    };

    // Write to sealed log
    match kernel.append_event_checked(
        module_desc,
        candidate,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    ) {
        Ok(ev) => {
            log::info!(
                "Event logged: {:?} zone={} conf={:.2}",
                ev.event_type,
                zone_id,
                confidence
            );
        }
        Err(e) => {
            log::warn!("Event rejected: {}", e);
        }
    }

    Ok(())
}

fn parse_frigate_event(payload: &[u8]) -> Result<(String, String, f64, Vec<String>)> {
    // Frigate publishes events in nested format: { "before": {...}, "after": {...} }
    let wrapper: FrigateEventWrapper =
        serde_json::from_slice(payload).context("parse Frigate event JSON")?;

    // Only process "new" events (not "update" or "end")
    // This prevents duplicate logging for the same detection
    match wrapper.event_type.as_deref() {
        Some("new") => {}
        Some("end") => return Err(anyhow!("event ended, already processed")),
        Some("update") => return Err(anyhow!("update event, already processed")),
        _ => {} // Accept if no type specified (backward compat)
    }

    let event = wrapper
        .after
        .ok_or_else(|| anyhow!("missing 'after' section in event"))?;

    if event.false_positive {
        return Err(anyhow!("false positive event"));
    }

    let confidence = event.top_score.unwrap_or(event.score);

    // Combine sub_label with label if present (e.g., "package:amazon")
    let label = match &event.sub_label {
        Some(sub) if !sub.is_empty() => format!("{}:{}", event.label, sub),
        _ => event.label.clone(),
    };

    // Prefer entered_zones over current_zones for more complete zone coverage
    let zones = if !event.entered_zones.is_empty() {
        event.entered_zones.clone()
    } else {
        event.current_zones.clone()
    };

    log::debug!(
        "Frigate event: camera={}, label={}, conf={:.2}, zones={:?}, has_clip={}, has_snapshot={}",
        event.camera,
        label,
        confidence,
        zones,
        event.has_clip,
        event.has_snapshot
    );

    Ok((event.camera, label, confidence, zones))
}

fn parse_review_event(payload: &[u8]) -> Result<(String, String, f64, Vec<String>)> {
    let review: FrigateReview =
        serde_json::from_slice(payload).context("parse Frigate review JSON")?;

    // Only process "new" reviews
    if review.review_type.as_deref() != Some("new") {
        return Err(anyhow!("not a new review"));
    }

    let data = review.data.ok_or_else(|| anyhow!("no review data"))?;
    let label = data
        .objects
        .first()
        .cloned()
        .unwrap_or_else(|| "unknown".to_string());
    let confidence = data.score.unwrap_or(0.5);

    Ok((review.camera, label, confidence, data.zones))
}

fn map_label_to_event_type(label: &str) -> EventType {
    // Handle sub_label format (e.g., "package:amazon" -> use "package")
    let base_label = label.split(':').next().unwrap_or(label);

    match base_label.to_lowercase().as_str() {
        "person" | "face" => EventType::BoundaryCrossingObjectLarge,
        "car" | "truck" | "bus" | "motorcycle" => EventType::BoundaryCrossingObjectLarge,
        "dog" | "cat" | "bird" | "animal" => EventType::BoundaryCrossingObjectSmall,
        "bicycle" | "skateboard" => EventType::BoundaryCrossingObjectSmall,
        "package" | "box" => EventType::BoundaryCrossingObjectSmall,
        _ => EventType::BoundaryCrossingObjectSmall,
    }
}

fn sanitize_zone_name(name: &str) -> String {
    name.to_lowercase()
        .chars()
        .map(|c| {
            if c.is_alphanumeric() || c == '_' || c == '-' {
                c
            } else {
                '_'
            }
        })
        .take(64)
        .collect()
}

fn connect_mqtt(
    endpoint: &MqttEndpoint,
    tls_config: &TlsConfig,
    client_id: &str,
    username: Option<&str>,
    password: Option<&str>,
) -> Result<(Client, Connection)> {
    let mut options = MqttOptions::new(client_id, &endpoint.host, endpoint.port);
    options.set_keep_alive(Duration::from_secs(60));
    options.set_clean_start(true);
    if let Some(user) = username {
        options.set_credentials(user, password.unwrap_or_default());
    }
    options.set_transport(tls_config.build_transport(endpoint)?);

    let (client, connection) = Client::new(options, 10);
    log::info!(
        "Connected to MQTT broker (TLS: {}, backend: {}, auth: {})",
        endpoint.use_tls,
        tls_config.backend,
        username.is_some()
    );
    Ok((client, connection))
}
