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

use anyhow::{anyhow, Result};
use clap::Parser;
use rumqttc::v5::{mqttbytes::QoS, Client, Connection, Event, Incoming, MqttOptions};
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Duration;

use witness_kernel::transport::{
    map_label_to_event_type, parse_frigate_event, parse_mqtt_endpoint, parse_review_event,
    sanitize_zone_name, validate_loopback_addr, MqttEndpoint, TlsBackend, TlsConfig, TlsMaterials,
};
use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy, MIN_BUCKET_SIZE_S,
};

const BRIDGE_NAME: &str = "frigate_bridge";

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

    fn wait_and_increment(&mut self) {
        std::thread::sleep(Duration::from_secs(self.current_secs));
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

    /// Time bucket size in seconds (default: 10 minutes, minimum: 5 minutes per spec).
    /// Per spec/event_contract.md §3: bucket size is conformance-critical and MUST be >= 300.
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

    // Exponential backoff for reconnection
    let mut backoff = ExponentialBackoff::new();

    // Main loop - process MQTT messages with automatic reconnection
    loop {
        let connect_result = connect_mqtt(
            &mqtt_endpoint,
            &tls_config,
            &args.mqtt_client_id,
            args.mqtt_username.as_deref(),
            args.mqtt_password.as_deref(),
        );

        let (client, mut connection) = match connect_result {
            Ok((c, conn)) => {
                backoff.reset();
                (c, conn)
            }
            Err(e) => {
                log::error!(
                    "Failed to connect to MQTT broker: {}. Retrying in {} seconds...",
                    e,
                    backoff.current()
                );
                backoff.wait_and_increment();
                continue;
            }
        };

        // Subscribe with QoS 1 (AtLeastOnce) for reliable message delivery
        if let Err(e) = client.subscribe(&args.frigate_topic, QoS::AtLeastOnce) {
            log::error!(
                "Failed to subscribe to {}: {}. Retrying in {} seconds...",
                args.frigate_topic,
                e,
                backoff.current()
            );
            backoff.wait_and_increment();
            continue;
        }
        log::info!("Subscribed to {} (QoS 1)", args.frigate_topic);

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
            log::info!(
                "Attempting reconnection with {} second backoff...",
                backoff.current()
            );
            backoff.wait_and_increment();
            continue;
        }

        log::warn!("MQTT connection closed unexpectedly. Reconnecting...");
        backoff.wait_and_increment();
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
    let parsed = if topic.contains("/reviews") {
        parse_review_event(payload)?
    } else {
        parse_frigate_event(payload)?
    };
    let camera = parsed.camera;
    let label = parsed.label;
    let confidence = parsed.confidence;
    let zones = parsed.zones;

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
