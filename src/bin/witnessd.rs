//! witnessd - Privacy Witness Kernel daemon
//!
//! This daemon:
//! 1. Ingests frames from configured sources (local files, RTSP, USB, etc.)
//! 2. Buffers frames in a bounded ring buffer (for potential vault sealing)
//! 3. Runs detection modules on InferenceView (restricted, no raw bytes)
//! 4. Enforces contract and module allowlist
//! 5. Writes conforming events to the sealed log
//! 6. Enforces retention with checkpointed pruning

use anyhow::{anyhow, Result};
use std::io::IsTerminal;
use std::time::{Duration, Instant};

#[cfg(feature = "backend-tract")]
use witness_kernel::detect::TractBackend;
use witness_kernel::{
    api::{ApiConfig, ApiServer},
    break_glass::BreakGlassTokenFile,
    config::DetectBackendPreference,
    detect::{BackendRegistry, CpuBackend, StubBackend},
    storage_health::MonitorSettings,
    BackendSelection, BucketKeyManager, CapabilityBoundaryRuntime, DeviceCapabilities, FileConfig,
    FileSource, FrameBuffer, InferenceBackend, Kernel, KernelConfig, Module, ModuleDescriptor,
    RtspConfig, RtspSource, SharedStorageHealth, StorageHealthMonitor, TimeBucket, Vault,
    VaultConfig, ZoneCrossingModule, ZonePolicy,
};
#[cfg(feature = "ingest-esp32")]
use witness_kernel::{Esp32Config, Esp32Source};
#[cfg(feature = "ingest-v4l2")]
use witness_kernel::{V4l2Config, V4l2Source};

#[path = "../ui.rs"]
mod ui;

fn main() -> Result<()> {
    // Initialize logging (simple stderr for MVP)
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let ui_flag = parse_ui_flag();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(ui_flag.as_deref(), is_tty, !stdout_is_tty);

    let kernel_version = env!("CARGO_PKG_VERSION");
    let config = {
        let _stage = ui.stage("Load configuration");
        witness_kernel::config::WitnessdConfig::load()?
    };
    // Apply the SQLite synchronous preference before any connection is
    // opened — it is process-wide and must be identical on every connection.
    witness_kernel::set_sqlite_synchronous(config.storage_health.sqlite_synchronous);
    let device_key_seed = {
        let provided_seed = std::env::var("DEVICE_KEY_SEED").ok();
        let key_path = witness_kernel::crypto::device_key_path_for_db(&config.db_path)?;
        witness_kernel::crypto::load_or_create_device_seed(&key_path, provided_seed.as_deref())?
    };
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&config.ruleset_id);

    let cfg = KernelConfig {
        db_path: config.db_path.clone(),
        ruleset_id: config.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: kernel_version.to_string(),
        retention: config.retention,
        device_key_seed,
        zone_policy: ZonePolicy::new(config.zones.sensitive_zones.clone())?,
    };

    let mut kernel = {
        let _stage = ui.stage("Open kernel");
        Kernel::open(&cfg)?
    };

    // Storage endurance & health monitoring (proactive SD-card reliability:
    // wear estimation, free space, write errors, thermal awareness).
    let mut storage_monitor = config.storage_health.enabled.then(|| {
        let _stage = ui.stage("Initialize storage health monitor");
        StorageHealthMonitor::new(
            MonitorSettings {
                endurance_tbw: config.storage_health.endurance_tbw,
                block_device: config.storage_health.block_device.clone(),
                state_path: config.storage_health.state_path.clone(),
                thresholds: config.storage_health.thresholds.clone(),
            },
            &config.db_path,
        )
    });
    let storage_health: SharedStorageHealth = std::sync::Arc::new(std::sync::RwLock::new(None));
    let storage_write_errors = storage_monitor.as_ref().map(|m| m.error_counter());

    let api_config = ApiConfig {
        addr: config.api_addr.clone(),
        token_path: config.api_token_path.clone(),
        ..ApiConfig::default()
    };
    let api_handle = {
        let _stage = ui.stage("Start event API");
        ApiServer::new(api_config, cfg.clone())
            .with_storage_health(storage_health.clone())
            .spawn()?
    };
    log::info!("event api listening on {}", api_handle.addr);
    if let Some(path) = &api_handle.token_path {
        log::info!("event api capability token written to {}", path.display());
    } else {
        log::warn!(
            "event api capability token not written to file; use --api-token-path to persist it safely"
        );
    }

    let crypto_mode = kernel
        .break_glass_policy()
        .map(|policy| policy.vault.crypto_mode)
        .unwrap_or_default();
    let mut vault = {
        let _stage = ui.stage("Initialize vault");
        Vault::new(VaultConfig {
            crypto_mode,
            ..VaultConfig::default()
        })?
    };
    // Optional break-glass seal path (requires BREAK_GLASS_SEAL_TOKEN with a token JSON).
    let mut seal_token = load_seal_token()?;

    // Be explicit about whether frame sealing into the vault is actually active.
    // Sealing is opt-in: it only runs when BREAK_GLASS_SEAL_TOKEN points at a valid
    // break-glass token, AND that token is only honoured within the 10-minute bucket
    // it was issued for (the seal path rejects an out-of-window token as "expired").
    // With no usable token, boundary events are still signed and logged, but NO frame
    // is sealed into the break-glass vault — surface this so an operator never assumes
    // "sealed evidence" they are not actually capturing.
    // See docs/review/01-flag-report.md F-05.
    match &seal_token {
        Some(token) => {
            let now_bucket = TimeBucket::now_10min()?;
            let expires = token.expires_bucket();
            if expires.start_epoch_s == now_bucket.start_epoch_s
                && expires.size_s == now_bucket.size_s
            {
                log::info!(
                    "vault frame sealing: ENABLED (crypto_mode={}) — boundary events seal a \
                     pre-roll frame into the break-glass vault",
                    crypto_mode
                );
            } else {
                log::warn!(
                    "vault frame sealing: token present but EXPIRED / outside its validity window \
                     — a break-glass seal token is honoured only within the 10-minute bucket it was \
                     issued for, so NO frame will be sealed until a fresh BREAK_GLASS_SEAL_TOKEN is \
                     provided. See docs/review/01-flag-report.md F-05."
                );
            }
        }
        None => log::warn!(
            "vault frame sealing: DISABLED — boundary events are signed and logged, but NO frame \
             is sealed into the break-glass vault. To enable, generate a break-glass token (see \
             the `break_glass` CLI and spec/break_glass.md) and point BREAK_GLASS_SEAL_TOKEN at \
             the token JSON. See docs/review/01-flag-report.md F-05."
        ),
    }

    // Configure ingestion source
    let mut source = {
        let _stage = ui.stage("Configure ingest source");
        IngestSource::new(&config)?
    };
    {
        let _stage = ui.stage("Connect ingest source");
        source.connect()?;
    }

    // Frame buffer for pre-roll (vault sealing, not accessible without break-glass)
    let mut frame_buffer = FrameBuffer::new();

    // Detection module
    let (mut module, module_desc, runtime, registry) = {
        let _stage = ui.stage("Initialize detection module");
        let capabilities = DeviceCapabilities::cpu_only();
        let backend_selection = match config.detect.backend {
            DetectBackendPreference::Auto => BackendSelection::Auto,
            DetectBackendPreference::Stub => BackendSelection::Require(InferenceBackend::Stub),
            DetectBackendPreference::Cpu => BackendSelection::Require(InferenceBackend::Cpu),
            DetectBackendPreference::Tract => BackendSelection::Require(InferenceBackend::Tract),
        };
        let module = ZoneCrossingModule::with_backend_selection(
            &config.zones.module_zone_id,
            backend_selection,
            &capabilities,
        )?
        .with_tokens(true);
        let module_desc: ModuleDescriptor = module.descriptor();
        let runtime = CapabilityBoundaryRuntime::new();
        runtime.validate_descriptor(&module_desc)?;
        let mut registry = BackendRegistry::new();
        registry.register(StubBackend::new());
        registry.register(CpuBackend::new());
        if config.detect.backend == DetectBackendPreference::Tract {
            register_tract_backend(&mut registry, &config)?;
            registry.set_default("tract")?;
        } else {
            match module.backend() {
                InferenceBackend::Stub => registry.set_default("stub")?,
                InferenceBackend::Cpu => registry.set_default("cpu")?,
                InferenceBackend::Tract => registry.set_default("tract")?,
                InferenceBackend::Accelerator => {
                    return Err(anyhow!("accelerator backend requested but not available"));
                }
            }
        }
        (module, module_desc, runtime, registry)
    };

    // Be explicit about what the active detector actually does. The default build
    // resolves to a frame-difference *motion* backend (stub/cpu), NOT object
    // detection — real CV (TractBackend/ONNX) is feature-gated behind
    // `--features backend-tract` and requires a model. Surface this so an operator
    // reading "detection" never assumes classified objects when they are getting
    // motion presence only. See docs/review/01-flag-report.md F-01.
    match module.backend() {
        InferenceBackend::Stub | InferenceBackend::Cpu => {
            log::warn!(
                "detection backend '{:?}' is MOTION-ONLY (frame-difference): events report \
                 motion presence, not classified objects. To enable real object detection: \
                 build with `--features backend-tract`, run `scripts/fetch_detection_model.sh` \
                 (downloads + verifies the model), and set detect.backend=tract. \
                 detect.tract_model is optional — it defaults to the fetched model path. \
                 See docs/review/01-flag-report.md F-01.",
                module.backend()
            );
        }
        InferenceBackend::Tract => {
            log::info!("detection backend: tract (ONNX object detection) active");
        }
        InferenceBackend::Accelerator => {}
    }

    // Bucket key manager (rotates per time bucket)
    let mut token_mgr = BucketKeyManager::new();

    let mut last_prune = Instant::now();
    let mut last_health_log = Instant::now();
    let mut last_storage_sample: Option<Instant> = None;
    let mut last_storage_status = witness_kernel::StorageHealthStatus::Good;
    let mut event_count = 0u64;
    let mut pipeline = PipelineCounters::default();

    log::info!("witnessd running. writing to {}", cfg.db_path);
    log::info!(
        "ruleset_id={}, kernel_version={}",
        cfg.ruleset_id,
        cfg.kernel_version
    );
    log::info!(
        "frame buffer capacity: {} frames, {} seconds pre-roll",
        witness_kernel::MAX_BUFFER_FRAMES,
        witness_kernel::MAX_PREROLL_SECS
    );

    loop {
        // Coarse time bucket (10 minutes)
        let bucket = TimeBucket::now_10min()?;
        token_mgr.rotate_if_needed(bucket);

        // Ingest frame from source
        let frame = source.next_frame()?;

        // Push to bounded buffer (for potential vault sealing)
        // The buffer enforces TTL and capacity limits automatically
        frame_buffer.push(frame);
        pipeline.frames_buffered += 1;

        // Get the latest frame for processing
        let Some(frame_ref) = frame_buffer.latest() else {
            continue;
        };

        // Modules receive InferenceView, NOT RawFrame
        // This is the isolation boundary: modules cannot access raw bytes
        let view = frame_ref.inference_view();

        // Run module inference
        pipeline.inference_attempts += 1;
        let candidates =
            match runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry) {
                Ok(candidates) => candidates,
                Err(err) => {
                    log::error!("module inference failed: {}", err);
                    pipeline.inference_errors += 1;
                    continue;
                }
            };
        pipeline.candidate_events += candidates.len() as u64;

        for cand in candidates {
            let ev = match kernel.append_event_checked(
                &module_desc,
                cand,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            ) {
                Ok(ev) => ev,
                Err(e) => {
                    pipeline.events_rejected += 1;
                    // Count genuine sealed-log write faults for the storage
                    // health monitor; contract/allowlist rejections are
                    // normal privacy enforcement, not a disk symptom.
                    if witness_kernel::storage_health::is_storage_error(&e) {
                        if let Some(counter) = &storage_write_errors {
                            counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        }
                    }
                    log::warn!("event rejected: {}", e);
                    continue;
                }
            };

            event_count += 1;
            pipeline.events_appended += 1;
            log::info!(
                "event #{}: {:?} zone={} bucket_start={} conf={:.2} token={}",
                event_count,
                ev.event_type,
                ev.zone_id,
                ev.time_bucket.start_epoch_s,
                ev.confidence,
                ev.correlation_token.is_some()
            );

            if ev.event_type == witness_kernel::EventType::BoundaryCrossingObjectLarge {
                if let Some(token) = seal_token.as_mut() {
                    match seal_latest_frame(
                        &mut vault,
                        &mut frame_buffer,
                        token,
                        cfg.ruleset_hash,
                        &kernel,
                    ) {
                        Ok(Some(envelope_id)) => {
                            log::warn!(
                                "vault sealed for envelope {} (break-glass token consumed)",
                                envelope_id
                            );
                            seal_token = None;
                        }
                        Ok(None) => {
                            log::warn!("vault seal skipped: no buffered frame available");
                        }
                        Err(e) => {
                            log::error!("vault seal failed: {}", e);
                        }
                    }
                }
            }
        }

        if last_health_log.elapsed() >= Duration::from_secs(5) {
            let stats = source.stats();
            log::info!(
                "ingest health={} frames={} source={}",
                source.is_healthy(),
                stats.frames_captured,
                stats.source
            );
            log::info!(
                "pipeline captured={} buffered={} inference_attempts={} inference_errors={} candidates={} appended={} rejected={}",
                stats.frames_captured,
                pipeline.frames_buffered,
                pipeline.inference_attempts,
                pipeline.inference_errors,
                pipeline.candidate_events,
                pipeline.events_appended,
                pipeline.events_rejected
            );
            last_health_log = Instant::now();
        }

        // Periodic retention enforcement with checkpoint. Each pass that
        // prunes writes a signed checkpoint transaction, so the cadence is
        // configurable ([retention] check_interval_seconds, default 5 min)
        // as an SD-card endurance measure; events persist at most
        // retention + check_interval.
        if last_prune.elapsed() > config.retention_check_interval {
            if let Err(e) = kernel.enforce_retention_with_checkpoint(cfg.retention) {
                // A retention failure is a genuine storage-side fault — count
                // it for the health monitor before propagating (fatal, as
                // before: retention enforcement must not silently stop).
                if let Some(counter) = &storage_write_errors {
                    counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }
                return Err(e);
            }
            last_prune = Instant::now();

            // Log buffer stats
            log::debug!(
                "frame buffer: {} frames, ~{} KB",
                frame_buffer.len(),
                frame_buffer.memory_bytes() / 1024
            );
        }

        // Periodic storage endurance & health sample, shared with /status.
        if let Some(monitor) = storage_monitor.as_mut() {
            let due = last_storage_sample
                .is_none_or(|t| t.elapsed() >= config.storage_health.check_interval);
            if due {
                match monitor.sample() {
                    Ok(report) => {
                        let status = report.storage.status;
                        if status != last_storage_status {
                            if status > last_storage_status {
                                log::warn!(
                                    "storage health: {} -> {} (free={}%, wear={}%, write_errors_total={}) \
                                     — see docs/sd_card_health.md",
                                    last_storage_status.as_str(),
                                    status.as_str(),
                                    report
                                        .storage
                                        .free_pct
                                        .map_or("n/a".to_string(), |v| format!("{v:.1}")),
                                    report
                                        .storage
                                        .wear_pct
                                        .map_or("n/a".to_string(), |v| format!("{v:.1}")),
                                    report.storage.write_errors,
                                );
                            } else {
                                log::info!(
                                    "storage health: recovered {} -> {}",
                                    last_storage_status.as_str(),
                                    status.as_str()
                                );
                            }
                            last_storage_status = status;
                        }
                        if let Ok(mut guard) = storage_health.write() {
                            *guard = Some(report);
                        }
                    }
                    Err(e) => log::warn!("storage health sample failed: {}", e),
                }
                last_storage_sample = Some(Instant::now());
            }
        }

        // Target ~10 fps (100ms between frames)
        std::thread::sleep(Duration::from_millis(100));
    }
}

struct IngestStats {
    frames_captured: u64,
    source: String,
}

#[derive(Default)]
struct PipelineCounters {
    frames_buffered: u64,
    inference_attempts: u64,
    inference_errors: u64,
    candidate_events: u64,
    events_appended: u64,
    events_rejected: u64,
}

enum IngestSource {
    File(FileSource),
    Rtsp(RtspSource),
    #[cfg(feature = "ingest-esp32")]
    Esp32(Esp32Source),
    #[cfg(feature = "ingest-v4l2")]
    V4l2(V4l2Source),
}

impl IngestSource {
    fn new(config: &witness_kernel::config::WitnessdConfig) -> Result<Self> {
        match config.ingest.backend {
            witness_kernel::config::IngestBackend::File => {
                let file_config = FileConfig {
                    path: config.file.path.clone(),
                    target_fps: config.file.target_fps,
                };
                Ok(Self::File(FileSource::new(file_config)?))
            }
            witness_kernel::config::IngestBackend::Rtsp => {
                let rtsp_config = RtspConfig {
                    url: config.rtsp.url.clone(),
                    target_fps: config.rtsp.target_fps,
                    width: config.rtsp.width,
                    height: config.rtsp.height,
                    backend: config.rtsp.backend,
                    transport: config.rtsp.transport.clone(),
                };
                Ok(Self::Rtsp(RtspSource::new(rtsp_config)?))
            }
            witness_kernel::config::IngestBackend::Esp32 => build_esp32_source(config),
            witness_kernel::config::IngestBackend::V4l2 => build_v4l2_source(config),
        }
    }

    fn connect(&mut self) -> Result<()> {
        match self {
            IngestSource::File(source) => source.connect(),
            IngestSource::Rtsp(source) => source.connect(),
            #[cfg(feature = "ingest-esp32")]
            IngestSource::Esp32(source) => source.connect(),
            #[cfg(feature = "ingest-v4l2")]
            IngestSource::V4l2(source) => source.connect(),
        }
    }

    fn next_frame(&mut self) -> Result<witness_kernel::RawFrame> {
        match self {
            IngestSource::File(source) => source.next_frame(),
            IngestSource::Rtsp(source) => source.next_frame(),
            #[cfg(feature = "ingest-esp32")]
            IngestSource::Esp32(source) => source.next_frame(),
            #[cfg(feature = "ingest-v4l2")]
            IngestSource::V4l2(source) => source.next_frame(),
        }
    }

    fn is_healthy(&self) -> bool {
        match self {
            IngestSource::File(source) => source.is_healthy(),
            IngestSource::Rtsp(source) => source.is_healthy(),
            #[cfg(feature = "ingest-esp32")]
            IngestSource::Esp32(source) => source.is_healthy(),
            #[cfg(feature = "ingest-v4l2")]
            IngestSource::V4l2(source) => source.is_healthy(),
        }
    }

    fn stats(&self) -> IngestStats {
        match self {
            IngestSource::File(source) => {
                let stats = source.stats();
                IngestStats {
                    frames_captured: stats.frames_captured,
                    source: stats.path,
                }
            }
            IngestSource::Rtsp(source) => {
                let stats = source.stats();
                IngestStats {
                    frames_captured: stats.frames_captured,
                    source: stats.url,
                }
            }
            #[cfg(feature = "ingest-esp32")]
            IngestSource::Esp32(source) => {
                let stats = source.stats();
                IngestStats {
                    frames_captured: stats.frames_captured,
                    source: stats.source,
                }
            }
            #[cfg(feature = "ingest-v4l2")]
            IngestSource::V4l2(source) => {
                let stats = source.stats();
                IngestStats {
                    frames_captured: stats.frames_captured,
                    source: stats.device,
                }
            }
        }
    }
}

fn build_esp32_source(config: &witness_kernel::config::WitnessdConfig) -> Result<IngestSource> {
    #[cfg(feature = "ingest-esp32")]
    {
        let esp32_config = Esp32Config {
            url: config.esp32.url.clone(),
            target_fps: config.esp32.target_fps,
        };
        Ok(IngestSource::Esp32(Esp32Source::new(esp32_config)?))
    }
    #[cfg(not(feature = "ingest-esp32"))]
    {
        let _ = config;
        Err(anyhow!("esp32 ingestion requires the ingest-esp32 feature"))
    }
}

fn build_v4l2_source(config: &witness_kernel::config::WitnessdConfig) -> Result<IngestSource> {
    #[cfg(feature = "ingest-v4l2")]
    {
        let v4l2_config = V4l2Config {
            device: config.v4l2.device.clone(),
            target_fps: config.v4l2.target_fps,
            width: config.v4l2.width,
            height: config.v4l2.height,
        };
        Ok(IngestSource::V4l2(V4l2Source::new(v4l2_config)?))
    }
    #[cfg(not(feature = "ingest-v4l2"))]
    {
        let _ = config;
        Err(anyhow!("v4l2 ingestion requires the ingest-v4l2 feature"))
    }
}

fn load_seal_token() -> Result<Option<witness_kernel::BreakGlassToken>> {
    let token_path = match std::env::var("BREAK_GLASS_SEAL_TOKEN") {
        Ok(path) => path,
        Err(_) => return Ok(None),
    };
    let json = std::fs::read_to_string(&token_path).map_err(|e| {
        anyhow!(
            "failed to read BREAK_GLASS_SEAL_TOKEN {}: {}",
            token_path,
            e
        )
    })?;
    let token_file: BreakGlassTokenFile =
        serde_json::from_str(&json).map_err(|e| anyhow!("invalid token file: {}", e))?;
    let token = token_file.into_token()?;
    Ok(Some(token))
}

fn seal_latest_frame(
    vault: &mut Vault,
    frame_buffer: &mut FrameBuffer,
    token: &mut witness_kernel::BreakGlassToken,
    ruleset_hash: [u8; 32],
    kernel: &Kernel,
) -> Result<Option<String>> {
    if token.ruleset_hash() != ruleset_hash {
        return Err(anyhow!(
            "break-glass token ruleset hash mismatch (token={}, expected={})",
            hex::encode(token.ruleset_hash()),
            hex::encode(ruleset_hash)
        ));
    }
    let Some(frame) = frame_buffer.drain_for_vault(token).next() else {
        return Ok(None);
    };
    let envelope_id = token.vault_envelope_id().to_string();
    let verifying_key = kernel.device_verifying_key();
    vault.seal_frame(
        &envelope_id,
        token,
        ruleset_hash,
        frame,
        &verifying_key,
        |hash| kernel.break_glass_receipt_outcome(hash),
    )?;
    Ok(Some(envelope_id))
}

fn parse_ui_flag() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if let Some(value) = arg.strip_prefix("--ui=") {
            return Some(value.to_string());
        }
        if arg == "--ui" {
            if let Some(value) = args.next() {
                return Some(value);
            }
        }
    }
    None
}

fn register_tract_backend(
    registry: &mut BackendRegistry,
    config: &witness_kernel::config::WitnessdConfig,
) -> Result<()> {
    #[cfg(feature = "backend-tract")]
    {
        use witness_kernel::config::TractFormat;
        // Defaults to vendor/models/tinyyolov2-8.onnx (see fetch_detection_model.sh) when
        // detect.tract_model is unset; a missing file yields a clear load error below.
        let model_path = config.detect.tract_model_path();
        let backend = match config.detect.tract_format {
            // tiny-YOLOv2 has a fixed 416×416 input and resizes frames internally, so it does
            // not need the ingest dimensions and works with any ingest backend.
            TractFormat::Yolov2 => TractBackend::tiny_yolov2(&model_path)?,
            TractFormat::PostNms => {
                let (width, height) = tract_input_dimensions(config)?;
                TractBackend::new(&model_path, width, height)?
            }
        };
        registry.register(backend.with_threshold(config.detect.confidence_threshold));
        Ok(())
    }
    #[cfg(not(feature = "backend-tract"))]
    {
        let _ = registry;
        let _ = config;
        Err(anyhow!(
            "detect.backend=tract requires the backend-tract feature"
        ))
    }
}

#[cfg(feature = "backend-tract")]
fn tract_input_dimensions(config: &witness_kernel::config::WitnessdConfig) -> Result<(u32, u32)> {
    match config.ingest.backend {
        witness_kernel::config::IngestBackend::Rtsp => {
            Ok((config.rtsp.width, config.rtsp.height))
        }
        witness_kernel::config::IngestBackend::V4l2 => {
            Ok((config.v4l2.width, config.v4l2.height))
        }
        witness_kernel::config::IngestBackend::File
        | witness_kernel::config::IngestBackend::Esp32 => Err(anyhow!(
            "tract backend requires ingest width/height; use rtsp/v4l2 or add a backend with fixed dimensions"
        )),
    }
}
