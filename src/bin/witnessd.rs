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
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};

#[cfg(feature = "backend-tract")]
use witness_kernel::detect::TractBackend;
use witness_kernel::{
    api::{ApiConfig, ApiServer},
    break_glass::BreakGlassTokenFile,
    config::DetectBackendPreference,
    detect::{BackendRegistry, CpuBackend, StubBackend},
    storage_health::MonitorSettings,
    BackendSelection, BucketKeyManager, CapabilityBoundaryRuntime, DeviceCapabilities, FailureType,
    FileConfig, FileSource, FrameBuffer, InferenceBackend, Kernel, KernelConfig, LifecyclePhase,
    Module, ModuleDescriptor, RtspConfig, RtspSource, SharedStorageHealth, StorageHealthMonitor,
    TimeBucket, Vault, VaultConfig, ZoneCrossingModule, ZonePolicy,
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

    // Boot-time chain-tail verification (K4 / FR-8). Startup used to append onto
    // whatever tail existed with no crypto check, so a corrupted or tampered tail
    // was only caught at the next manual `/verify`. Verify the sealed log — the
    // latest checkpoint and the chain through the tail — BEFORE extending it. If
    // it does not verify, enter report-but-not-append safe mode: keep the read /
    // verify API up so an operator can inspect the damage, but seal nothing —
    // appending onto an untrusted tail would launder the tampering into the chain.
    let safe_mode_reason: Option<String> = {
        let _stage = ui.stage("Verify sealed-log tail");
        match kernel.verify_sealed_log() {
            Ok(report) if report.chain_valid => None,
            Ok(report) => Some(
                report
                    .error
                    .unwrap_or_else(|| "sealed-log chain did not verify".to_string()),
            ),
            Err(e) => Some(format!("sealed log could not be verified at boot: {e:#}")),
        }
    };
    if let Some(reason) = &safe_mode_reason {
        log::error!(
            "SAFE MODE: sealed-log tail failed boot verification — {reason}. \
             witnessd will serve the read/verify API but will NOT append to the chain. \
             Investigate and restore a trusted log, then restart."
        );
    }

    // Witness the daemon lifecycle. If the previous run's last lifecycle record
    // is `start` (no clean shutdown), it died uncleanly — seal a PowerLoss
    // failure record (proxy for power loss, crash, or kill; see
    // docs/failure_semantics.md). Then seal this run's start record. Sealing
    // the start record is fail-closed: if we cannot witness our own startup we
    // must not run. Skipped in safe mode — an untrusted tail must not be extended.
    if safe_mode_reason.is_none() {
        let _stage = ui.stage("Seal lifecycle start");
        match kernel.last_lifecycle_phase() {
            Ok(Some(LifecyclePhase::Start)) => {
                log::warn!(
                    "previous run did not shut down cleanly; sealing PowerLoss failure record"
                );
                if let Err(e) = kernel.append_failure_event(
                    FailureType::PowerLoss,
                    TimeBucket::now_10min()?,
                    Some("unclean_shutdown".to_string()),
                    kernel_version,
                    &cfg.ruleset_id,
                    ruleset_hash,
                ) {
                    log::error!("failed to seal PowerLoss record: {e}");
                }
            }
            Ok(_) => {}
            Err(e) => log::error!("could not determine previous lifecycle phase: {e}"),
        }
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            kernel_version,
            &cfg.ruleset_id,
            ruleset_hash,
        )?;
    }

    // Graceful shutdown: SIGINT/SIGTERM set the flag; the main loop seals a
    // clean-shutdown lifecycle record before exiting.
    let shutdown = Arc::new(AtomicBool::new(false));
    {
        let shutdown = Arc::clone(&shutdown);
        ctrlc::set_handler(move || shutdown.store(true, Ordering::SeqCst))
            .map_err(|e| anyhow!("failed to install shutdown signal handler: {e}"))?;
    }

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
        rate_limit_per_minute: config.api_rate_limit_per_minute,
        // Explicit opt-in required to expose the plaintext API off-loopback.
        allow_insecure: std::env::var("WITNESS_API_ALLOW_INSECURE")
            .map(|v| {
                let v = v.trim();
                v == "1" || v.eq_ignore_ascii_case("true")
            })
            .unwrap_or(false),
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

    // Safe mode: the sealed-log tail did not verify at boot. Serve the read /
    // verify API (already listening above) so an operator can inspect the log,
    // but do not stand up the ingest pipeline or append anything. Idle until a
    // shutdown signal, re-logging the diagnosis periodically.
    if let Some(reason) = &safe_mode_reason {
        run_safe_mode(&shutdown, reason);
        api_handle.stop()?;
        log::info!("witnessd stopped (safe mode)");
        return Ok(());
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
    // break-glass token, AND that token is only honored within the 10-minute bucket
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
                     — a break-glass seal token is honored only within the 10-minute bucket it was \
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
        // A camera that is down at boot is an outage to witness, not a reason
        // to refuse to start: the supervisor retries and seals a gap record.
        if let Err(e) = source.connect() {
            log::warn!(
                "ingest source connect failed at startup (backend={}): {e}; will retry",
                source.backend_name()
            );
        }
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
    let mut last_storage_sample: Option<Instant> = None;
    let mut last_storage_status = witness_kernel::StorageHealthStatus::Good;
    let mut event_count = 0u64;
    let mut pipeline = PipelineCounters::default();
    let mut supervisor = IngestSupervisor::new(
        config.ingest.failure_threshold,
        config.ingest.reconnect_backoff_max,
    );
    let mut clock_monitor = ClockMonitor::new(config.clock.skew_tolerance);
    let mut heartbeat = HeartbeatScheduler::new(config.health.heartbeat);
    let mut disk_monitor =
        DiskMonitor::new(config.storage.min_free_bytes, config.storage.check_interval);
    let mut health = HealthReporter::new(config.health.log_interval);

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

    // Liveness watchdog (K4 / FR-9): the main loop bumps `progress` every
    // iteration; an independent thread aborts the process if it stops advancing
    // for WATCHDOG_STALL_TIMEOUT, turning a wedged daemon into a supervisor
    // restart (panic=abort in release) instead of a silent stall. The watchdog
    // exits cleanly on shutdown so a slow, orderly drain is never mistaken for a
    // hang.
    let progress = Arc::new(AtomicU64::new(0));
    spawn_watchdog(Arc::clone(&progress), Arc::clone(&shutdown));

    while !shutdown.load(Ordering::SeqCst) {
        progress.fetch_add(1, Ordering::Relaxed);
        // Coarse time bucket (10 minutes). A broken clock must be witnessed,
        // not crash the daemon: seal a ClockSkew failure and retry.
        let bucket = match TimeBucket::now_10min() {
            Ok(bucket) => bucket,
            Err(e) => {
                log::error!("cannot derive time bucket: {e}");
                kernel.report_clock_skew(
                    &format!("time bucket unavailable: {e}"),
                    kernel_version,
                    &cfg.ruleset_id,
                    ruleset_hash,
                );
                pipeline.failures_recorded += 1;
                std::thread::sleep(Duration::from_secs(1));
                continue;
            }
        };
        clock_monitor.observe(
            bucket,
            &mut kernel,
            &mut pipeline,
            kernel_version,
            &cfg.ruleset_id,
            ruleset_hash,
        );
        heartbeat.tick(
            bucket,
            &mut kernel,
            &source,
            &pipeline,
            kernel_version,
            &cfg.ruleset_id,
            ruleset_hash,
        );
        token_mgr.rotate_if_needed(bucket);

        // Ingest frame from source. Capture errors are an outage to supervise
        // (warn, seal one gap record per outage, reconnect with backoff) — the
        // daemon never exits on a dead camera.
        let frame = match source.next_frame() {
            Ok(frame) => {
                supervisor.on_success();
                Some(frame)
            }
            Err(e) => {
                supervisor.on_error(
                    &e,
                    &mut source,
                    &config,
                    bucket,
                    &mut kernel,
                    &mut pipeline,
                    kernel_version,
                    &cfg.ruleset_id,
                    ruleset_hash,
                );
                None
            }
        };

        // Inference only runs on a freshly captured frame; during an outage the
        // buffer may still hold (TTL-bounded) pre-roll frames for vault sealing,
        // but re-running inference on stale frames would be meaningless.
        if let Some(frame) = frame {
            // Push to bounded buffer (for potential vault sealing)
            // The buffer enforces TTL and capacity limits automatically
            frame_buffer.push(frame);
            pipeline.frames_buffered += 1;

            // Modules receive InferenceView, NOT RawFrame
            // This is the isolation boundary: modules cannot access raw bytes
            let candidates = frame_buffer.latest().map(|frame_ref| {
                let view = frame_ref.inference_view();
                pipeline.inference_attempts += 1;
                match runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry) {
                    Ok(candidates) => candidates,
                    Err(err) => {
                        log::error!("module inference failed: {}", err);
                        pipeline.inference_errors += 1;
                        Vec::new()
                    }
                }
            });
            let candidates = candidates.unwrap_or_default();
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
                        // append_event_checked seals a GapMissingData record for
                        // each rejection; count it for the heartbeat delta.
                        pipeline.failures_recorded += 1;
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
        }

        health.tick(&source, &pipeline);

        // Periodic retention enforcement with checkpoint. Each pass that
        // prunes writes a signed checkpoint transaction, so the cadence is
        // configurable ([retention] check_interval_seconds, default 5 min)
        // as an SD-card endurance measure; events persist at most
        // retention + check_interval. A failure here is a storage fault to
        // witness (sealed record + health counter), not a reason to exit.
        if last_prune.elapsed() > config.retention_check_interval {
            if let Err(e) = kernel.enforce_retention_with_checkpoint(cfg.retention) {
                if let Some(counter) = &storage_write_errors {
                    counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }
                log::error!("retention enforcement failed: {e}");
                kernel.report_storage_failure(
                    &format!("retention enforcement failed: {e}"),
                    kernel_version,
                    &cfg.ruleset_id,
                    ruleset_hash,
                );
                pipeline.failures_recorded += 1;
            }
            last_prune = Instant::now();

            // Log buffer stats
            log::debug!(
                "frame buffer: {} frames, ~{} KB",
                frame_buffer.len(),
                frame_buffer.memory_bytes() / 1024
            );
        }

        // Free-space preflight: seals a StorageFull record per transition
        // (sealed-chain evidence, distinct from the endurance monitor below).
        disk_monitor.tick(
            &cfg.db_path,
            &mut kernel,
            &mut pipeline,
            kernel_version,
            &cfg.ruleset_id,
            ruleset_hash,
        );

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

    // Seal a final heartbeat (covering the trailing partial bucket's activity)
    // and the clean-shutdown record, so the next boot can distinguish a
    // deliberate stop from power loss / crash.
    log::info!("shutdown signal received; sealing clean-shutdown record");
    heartbeat.seal_final(
        &mut kernel,
        &source,
        &pipeline,
        kernel_version,
        &cfg.ruleset_id,
        ruleset_hash,
    );
    if let Err(e) = kernel.append_lifecycle(
        LifecyclePhase::ShutdownClean,
        kernel_version,
        &cfg.ruleset_id,
        ruleset_hash,
    ) {
        log::error!("failed to seal clean-shutdown record: {e}");
    }
    api_handle.stop()?;
    log::info!("witnessd stopped");
    Ok(())
}

/// How often the watchdog samples the main loop's progress counter.
const WATCHDOG_CHECK_INTERVAL: Duration = Duration::from_secs(5);
/// How long the main loop may make no progress before the watchdog aborts.
/// Generous relative to the ~100 ms loop cadence: only a genuine wedge — a
/// blocked syscall, a deadlock — stalls this long.
const WATCHDOG_STALL_TIMEOUT: Duration = Duration::from_secs(60);

/// Tracks whether the monitored progress counter is advancing. Extracted from
/// the watchdog thread so the stall decision is unit-testable without spawning
/// threads or aborting the process.
struct WatchdogState {
    last_value: u64,
    last_change: Instant,
    stall_timeout: Duration,
}

impl WatchdogState {
    fn new(initial: u64, now: Instant, stall_timeout: Duration) -> Self {
        Self {
            last_value: initial,
            last_change: now,
            stall_timeout,
        }
    }

    /// Record the latest counter value at time `now`. Returns `true` when the
    /// counter has not advanced for at least `stall_timeout` — i.e. the loop is
    /// wedged and the process should abort.
    fn observe(&mut self, value: u64, now: Instant) -> bool {
        if value != self.last_value {
            self.last_value = value;
            self.last_change = now;
            false
        } else {
            now.saturating_duration_since(self.last_change) >= self.stall_timeout
        }
    }
}

/// Spawn the liveness watchdog (K4 / FR-9). It samples `progress` every
/// [`WATCHDOG_CHECK_INTERVAL`]; if the counter has not advanced for
/// [`WATCHDOG_STALL_TIMEOUT`] it logs the cause and aborts the process, so a
/// wedged daemon becomes a supervisor restart instead of a silent stall. It
/// returns as soon as `shutdown` is set, so an orderly drain is never mistaken
/// for a hang.
fn spawn_watchdog(progress: Arc<AtomicU64>, shutdown: Arc<AtomicBool>) {
    std::thread::spawn(move || {
        let mut state = WatchdogState::new(
            progress.load(Ordering::Relaxed),
            Instant::now(),
            WATCHDOG_STALL_TIMEOUT,
        );
        loop {
            std::thread::sleep(WATCHDOG_CHECK_INTERVAL);
            if shutdown.load(Ordering::SeqCst) {
                return;
            }
            let value = progress.load(Ordering::Relaxed);
            if state.observe(value, Instant::now()) {
                log::error!(
                    "watchdog: witnessd main loop made no progress for {}s (stuck at tick {}); \
                     aborting so the supervisor can restart it",
                    WATCHDOG_STALL_TIMEOUT.as_secs(),
                    value
                );
                std::process::abort();
            }
        }
    });
}

/// Report-but-not-append safe mode: idle until shutdown, re-logging the boot
/// verification failure every minute. The read/verify API keeps serving from
/// the (read-only, untrusted) log so an operator can inspect it; nothing is
/// appended, because extending an unverified tail would launder tampering into
/// the chain.
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

#[cfg(test)]
mod watchdog_tests {
    use super::*;

    #[test]
    fn watchdog_trips_only_after_stall_timeout_and_resets_on_progress() {
        let base = Instant::now();
        let timeout = Duration::from_secs(60);
        let mut wd = WatchdogState::new(0, base, timeout);

        // Same counter value, but only 5s elapsed — not a stall yet.
        assert!(!wd.observe(0, base + Duration::from_secs(5)));
        // Still stuck at the same value past the timeout — abort.
        assert!(wd.observe(0, base + Duration::from_secs(61)));
        // The loop advanced — the watchdog resets and does not abort.
        assert!(!wd.observe(1, base + Duration::from_secs(62)));
        // Shortly after the reset, no stall.
        assert!(!wd.observe(1, base + Duration::from_secs(63)));
        // Stuck again long enough after the reset — abort once more.
        assert!(wd.observe(1, base + Duration::from_secs(200)));
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
    /// Failure records sealed via this process (outages, clock skew, storage),
    /// including the per-rejection records the kernel seals. Feeds the
    /// heartbeat's per-bucket failure delta.
    failures_recorded: u64,
}

/// Supervises the ingest source: rate-limits warnings, seals exactly one
/// GapMissingData failure record per outage (after `failure_threshold`), and
/// drives reconnect attempts with exponential backoff.
struct IngestSupervisor {
    failure_threshold: Duration,
    backoff_max: Duration,
    consecutive_errors: u64,
    unhealthy_since: Option<Instant>,
    outage_recorded: bool,
    backoff: Duration,
    next_reconnect_at: Option<Instant>,
    last_warn: Option<Instant>,
}

impl IngestSupervisor {
    const INITIAL_BACKOFF: Duration = Duration::from_secs(1);
    const WARN_INTERVAL: Duration = Duration::from_secs(30);

    fn new(failure_threshold: Duration, backoff_max: Duration) -> Self {
        Self {
            failure_threshold,
            backoff_max,
            consecutive_errors: 0,
            unhealthy_since: None,
            outage_recorded: false,
            backoff: Self::INITIAL_BACKOFF,
            next_reconnect_at: None,
            last_warn: None,
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn on_error(
        &mut self,
        err: &anyhow::Error,
        source: &mut IngestSource,
        config: &witness_kernel::config::WitnessdConfig,
        bucket: TimeBucket,
        kernel: &mut Kernel,
        pipeline: &mut PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        let now = Instant::now();
        self.consecutive_errors += 1;
        let unhealthy_since = *self.unhealthy_since.get_or_insert(now);

        // First error logs immediately; after that, one WARN per interval.
        if self
            .last_warn
            .is_none_or(|t| t.elapsed() >= Self::WARN_INTERVAL)
        {
            log::warn!(
                "ingest frame capture failing (backend={}, {} consecutive errors, {}s): {}",
                source.backend_name(),
                self.consecutive_errors,
                unhealthy_since.elapsed().as_secs(),
                err
            );
            self.last_warn = Some(now);
        }

        // One sealed gap record per outage, once the threshold is crossed.
        // Details carry the backend name only — never URLs or device paths
        // (sealed records must not contain network identifiers).
        if !self.outage_recorded && unhealthy_since.elapsed() >= self.failure_threshold {
            let details = format!(
                "ingest_stalled backend={} consecutive_errors={}",
                source.backend_name(),
                self.consecutive_errors
            );
            match kernel.append_failure_event(
                FailureType::GapMissingData,
                bucket,
                Some(details),
                kernel_version,
                ruleset_id,
                ruleset_hash,
            ) {
                Ok(_) => {
                    pipeline.failures_recorded += 1;
                    log::warn!("sealed GapMissingData record for ingest outage");
                }
                Err(e) => {
                    log::error!("failed to seal ingest-outage record: {e}");
                    kernel.report_storage_failure(
                        &format!("ingest-outage record append failed: {e}"),
                        kernel_version,
                        ruleset_id,
                        ruleset_hash,
                    );
                    pipeline.failures_recorded += 1;
                }
            }
            self.outage_recorded = true;
        }

        // Reconnect with backoff. If a plain reconnect fails, rebuild the
        // source from config — some backends (gstreamer pipelines) cannot be
        // revived in place once dead.
        if self.next_reconnect_at.is_none_or(|t| now >= t) {
            match source.connect() {
                Ok(()) => log::info!("ingest source reconnect attempt succeeded"),
                Err(connect_err) => {
                    log::debug!("ingest reconnect failed: {connect_err}; rebuilding source");
                    match IngestSource::new(config).and_then(|mut rebuilt| {
                        rebuilt.connect()?;
                        Ok(rebuilt)
                    }) {
                        Ok(rebuilt) => {
                            *source = rebuilt;
                            log::info!("ingest source rebuilt and reconnected");
                        }
                        Err(rebuild_err) => {
                            log::debug!("ingest source rebuild failed: {rebuild_err}");
                        }
                    }
                }
            }
            self.backoff = (self.backoff * 2).min(self.backoff_max);
            self.next_reconnect_at = Some(now + self.backoff);
        }
    }

    fn on_success(&mut self) {
        if let Some(since) = self.unhealthy_since {
            log::info!(
                "ingest recovered after {}s ({} consecutive errors)",
                since.elapsed().as_secs(),
                self.consecutive_errors
            );
        }
        self.consecutive_errors = 0;
        self.unhealthy_since = None;
        self.outage_recorded = false;
        self.backoff = Self::INITIAL_BACKOFF;
        self.next_reconnect_at = None;
        self.last_warn = None;
    }
}

/// Detects clock desynchronization: monotonic-vs-wallclock drift beyond
/// tolerance, and coarse time-bucket regression. Seals one ClockSkew failure
/// record per excursion, then re-baselines (hysteresis).
struct ClockMonitor {
    tolerance: Duration,
    baseline_wall: SystemTime,
    baseline_mono: Instant,
    last_bucket_start: Option<u64>,
}

impl ClockMonitor {
    fn new(tolerance: Duration) -> Self {
        Self {
            tolerance,
            baseline_wall: SystemTime::now(),
            baseline_mono: Instant::now(),
            last_bucket_start: None,
        }
    }

    /// Absolute difference between observed wall clock and the wall time the
    /// monotonic clock predicts from the baseline.
    fn drift(&self) -> Duration {
        let expected = self.baseline_wall + self.baseline_mono.elapsed();
        match SystemTime::now().duration_since(expected) {
            Ok(ahead) => ahead,
            Err(behind) => behind.duration(),
        }
    }

    fn observe(
        &mut self,
        bucket: TimeBucket,
        kernel: &mut Kernel,
        pipeline: &mut PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        let drift = self.drift();
        if drift > self.tolerance {
            log::warn!(
                "clock skew detected: wallclock drifted {}s against monotonic clock",
                drift.as_secs()
            );
            kernel.report_clock_skew(
                &format!("wallclock drift {}s vs monotonic", drift.as_secs()),
                kernel_version,
                ruleset_id,
                ruleset_hash,
            );
            pipeline.failures_recorded += 1;
            // Re-baseline so a single jump produces a single record.
            self.baseline_wall = SystemTime::now();
            self.baseline_mono = Instant::now();
        }

        if let Some(last) = self.last_bucket_start {
            if bucket.start_epoch_s < last {
                log::warn!(
                    "time bucket regressed: {} < {} (clock moved backwards)",
                    bucket.start_epoch_s,
                    last
                );
                kernel.report_clock_skew(
                    &format!("time bucket regressed: {} < {}", bucket.start_epoch_s, last),
                    kernel_version,
                    ruleset_id,
                    ruleset_hash,
                );
                pipeline.failures_recorded += 1;
            }
        }
        // Track the observed bucket (even after regression, so one record per jump).
        self.last_bucket_start = Some(bucket.start_epoch_s);
    }
}

/// Seals one heartbeat record per 10-minute bucket carrying per-bucket
/// counter deltas. Anchors the chain tail (see HeartbeatRecord docs).
struct HeartbeatScheduler {
    enabled: bool,
    last_bucket_start: Option<u64>,
    prev_frames: u64,
    prev_events: u64,
    prev_failures: u64,
}

impl HeartbeatScheduler {
    fn new(enabled: bool) -> Self {
        Self {
            enabled,
            last_bucket_start: None,
            prev_frames: 0,
            prev_events: 0,
            prev_failures: 0,
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn tick(
        &mut self,
        bucket: TimeBucket,
        kernel: &mut Kernel,
        source: &IngestSource,
        pipeline: &PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        if self.last_bucket_start == Some(bucket.start_epoch_s) {
            return;
        }
        self.seal(
            bucket,
            kernel,
            source,
            pipeline,
            kernel_version,
            ruleset_id,
            ruleset_hash,
        );
    }

    /// Seals a final heartbeat covering activity since the last one, so the
    /// trailing partial bucket is represented in the trace at shutdown.
    #[allow(clippy::too_many_arguments)]
    fn seal_final(
        &mut self,
        kernel: &mut Kernel,
        source: &IngestSource,
        pipeline: &PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        let Ok(bucket) = TimeBucket::now_10min() else {
            return;
        };
        self.seal(
            bucket,
            kernel,
            source,
            pipeline,
            kernel_version,
            ruleset_id,
            ruleset_hash,
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn seal(
        &mut self,
        bucket: TimeBucket,
        kernel: &mut Kernel,
        source: &IngestSource,
        pipeline: &PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        if !self.enabled {
            return;
        }
        let frames = source.stats().frames_captured;
        let events = pipeline.events_appended;
        let failures = pipeline.failures_recorded;
        if let Err(e) = kernel.append_heartbeat(
            bucket,
            source.is_healthy(),
            frames.saturating_sub(self.prev_frames),
            events.saturating_sub(self.prev_events),
            failures.saturating_sub(self.prev_failures),
            kernel_version,
            ruleset_id,
            ruleset_hash,
        ) {
            // Heartbeats anchor the chain; a failed write is a storage fault.
            log::error!("failed to seal heartbeat record: {e}");
            kernel.report_storage_failure(
                &format!("heartbeat append failed: {e}"),
                kernel_version,
                ruleset_id,
                ruleset_hash,
            );
        } else {
            log::debug!(
                "heartbeat sealed for bucket {} (healthy={})",
                bucket.start_epoch_s,
                source.is_healthy()
            );
        }
        self.last_bucket_start = Some(bucket.start_epoch_s);
        self.prev_frames = frames;
        self.prev_events = events;
        self.prev_failures = failures;
    }
}

/// Free-space preflight: seals one StorageFull failure record per
/// below-threshold transition (latched until space recovers). Distinct from
/// StorageWriteFailed, which marks actual write errors.
struct DiskMonitor {
    min_free_bytes: u64,
    check_interval: Duration,
    last_check: Option<Instant>,
    below_threshold: bool,
}

impl DiskMonitor {
    fn new(min_free_bytes: u64, check_interval: Duration) -> Self {
        Self {
            min_free_bytes,
            check_interval,
            last_check: None,
            below_threshold: false,
        }
    }

    fn tick(
        &mut self,
        db_path: &str,
        kernel: &mut Kernel,
        pipeline: &mut PipelineCounters,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        if self
            .last_check
            .is_some_and(|t| t.elapsed() < self.check_interval)
        {
            return;
        }
        self.last_check = Some(Instant::now());

        let free = match free_bytes_for_db(db_path) {
            Ok(free) => free,
            Err(e) => {
                log::debug!("disk free-space check unavailable: {e}");
                return;
            }
        };

        if free < self.min_free_bytes {
            if !self.below_threshold {
                log::warn!(
                    "storage free space low: {} MB free (floor {} MB); sealing StorageFull record",
                    free / (1024 * 1024),
                    self.min_free_bytes / (1024 * 1024)
                );
                match TimeBucket::now_10min() {
                    Ok(bucket) => {
                        if let Err(e) = kernel.append_failure_event(
                            FailureType::StorageFull,
                            bucket,
                            Some(format!("free_mb={}", free / (1024 * 1024))),
                            kernel_version,
                            ruleset_id,
                            ruleset_hash,
                        ) {
                            log::error!("failed to seal StorageFull record: {e}");
                        } else {
                            pipeline.failures_recorded += 1;
                        }
                    }
                    Err(e) => log::error!("cannot seal StorageFull record (no bucket): {e}"),
                }
                self.below_threshold = true;
            }
        } else if self.below_threshold {
            log::info!(
                "storage free space recovered: {} MB free",
                free / (1024 * 1024)
            );
            self.below_threshold = false;
        }
    }
}

/// Free bytes on the filesystem holding the sealed-log database.
/// statvfs is POSIX, so the preflight works on any Unix (Linux, macOS, BSD).
#[cfg(unix)]
fn free_bytes_for_db(db_path: &str) -> Result<u64> {
    use std::ffi::CString;
    use std::path::Path;

    let dir = Path::new(db_path)
        .parent()
        .filter(|p| !p.as_os_str().is_empty());
    let dir = dir.unwrap_or_else(|| Path::new("."));
    let c_path = CString::new(dir.as_os_str().as_encoded_bytes())
        .map_err(|_| anyhow!("db path contains NUL"))?;
    let mut stats: libc::statvfs = unsafe { std::mem::zeroed() };
    let rc = unsafe { libc::statvfs(c_path.as_ptr(), &mut stats) };
    if rc != 0 {
        return Err(anyhow!(
            "statvfs({}) failed: {}",
            dir.display(),
            std::io::Error::last_os_error()
        ));
    }
    // The casts are no-ops on 64-bit Linux but required on other targets
    // (32-bit, macOS), where statvfs fields are narrower types.
    #[allow(clippy::unnecessary_cast)]
    Ok((stats.f_bavail as u64).saturating_mul(stats.f_frsize as u64))
}

#[cfg(not(unix))]
fn free_bytes_for_db(_db_path: &str) -> Result<u64> {
    Err(anyhow!("free-space check not supported on this platform"))
}

/// Operational health logging: state transitions at WARN/INFO, a full counter
/// summary at INFO on `log_interval`, and the detailed dump at DEBUG every 5s.
struct HealthReporter {
    log_interval: Duration,
    last_summary: Instant,
    last_debug: Instant,
    last_healthy: Option<bool>,
}

impl HealthReporter {
    const DEBUG_INTERVAL: Duration = Duration::from_secs(5);

    fn new(log_interval: Duration) -> Self {
        Self {
            log_interval,
            last_summary: Instant::now(),
            last_debug: Instant::now(),
            last_healthy: None,
        }
    }

    fn tick(&mut self, source: &IngestSource, pipeline: &PipelineCounters) {
        let healthy = source.is_healthy();
        if self.last_healthy != Some(healthy) {
            if healthy {
                // Don't announce "healthy" at first observation, only on recovery.
                if self.last_healthy.is_some() {
                    log::info!("ingest source healthy (backend={})", source.backend_name());
                }
            } else {
                log::warn!(
                    "ingest source UNHEALTHY (backend={})",
                    source.backend_name()
                );
            }
            self.last_healthy = Some(healthy);
        }

        if self.last_summary.elapsed() >= self.log_interval {
            let stats = source.stats();
            log::info!(
                "pipeline health={} captured={} buffered={} inference_attempts={} inference_errors={} candidates={} appended={} rejected={} failures={}",
                healthy,
                stats.frames_captured,
                pipeline.frames_buffered,
                pipeline.inference_attempts,
                pipeline.inference_errors,
                pipeline.candidate_events,
                pipeline.events_appended,
                pipeline.events_rejected,
                pipeline.failures_recorded
            );
            self.last_summary = Instant::now();
            self.last_debug = Instant::now();
        } else if self.last_debug.elapsed() >= Self::DEBUG_INTERVAL {
            let stats = source.stats();
            log::debug!(
                "ingest health={} frames={} source={}",
                healthy,
                stats.frames_captured,
                stats.source
            );
            log::debug!(
                "pipeline captured={} buffered={} inference_attempts={} inference_errors={} candidates={} appended={} rejected={} failures={}",
                stats.frames_captured,
                pipeline.frames_buffered,
                pipeline.inference_attempts,
                pipeline.inference_errors,
                pipeline.candidate_events,
                pipeline.events_appended,
                pipeline.events_rejected,
                pipeline.failures_recorded
            );
            self.last_debug = Instant::now();
        }
    }
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

    /// Backend name for logs and sealed failure details. Names only — never
    /// URLs or device paths (sealed records must not carry identifiers).
    fn backend_name(&self) -> &'static str {
        match self {
            IngestSource::File(_) => "file",
            IngestSource::Rtsp(_) => "rtsp",
            #[cfg(feature = "ingest-esp32")]
            IngestSource::Esp32(_) => "esp32",
            #[cfg(feature = "ingest-v4l2")]
            IngestSource::V4l2(_) => "v4l2",
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
    let envelope_id = token.vault_envelope_id().to_string();
    let verifying_key = kernel.device_verifying_key();
    // Validate the token BEFORE the drain (spec/invariants.md §3.1): draining
    // destroys and zeroizes the entire pre-roll buffer, so a stale or
    // mismatched token must fail while the incident's pre-roll is still
    // intact — otherwise one expired token erases the evidence it was meant
    // to seal. The vault re-validates (and consumes) inside seal_frame.
    witness_kernel::break_glass::BreakGlass::assert_token_valid(
        token,
        &envelope_id,
        ruleset_hash,
        witness_kernel::TimeBucket::now(600)?,
        &verifying_key,
        |hash| kernel.break_glass_receipt_outcome(&envelope_id, ruleset_hash, hash),
    )?;
    // `.last()` seals the NEWEST frame — the one closest to the triggering
    // event, matching this function's name — while the drain still clears and
    // zeroizes the remaining pre-roll, as the buffer contract requires.
    let Some(frame) = frame_buffer.drain_for_vault(token).last() else {
        return Ok(None);
    };
    vault.seal_frame(
        &envelope_id,
        token,
        ruleset_hash,
        frame,
        &verifying_key,
        |hash| kernel.break_glass_receipt_outcome(&envelope_id, ruleset_hash, hash),
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

#[cfg(test)]
mod tests {
    use super::*;
    use witness_kernel::config::{
        ClockSettings, DetectSettings, Esp32Settings, FileSettings, HealthSettings, IngestBackend,
        IngestSettings, RtspBackendPreference, RtspSettings, StorageHealthSettings,
        StorageSettings, TractFormat, V4l2Settings, WitnessdConfig, ZoneSettings,
    };
    use witness_kernel::{FailureType, SealedLogRecord};

    fn test_config() -> WitnessdConfig {
        WitnessdConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            api_addr: "127.0.0.1:0".to_string(),
            api_token_path: None,
            api_rate_limit_per_minute: witness_kernel::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE,
            ingest: IngestSettings {
                backend: IngestBackend::Rtsp,
                failure_threshold: Duration::ZERO,
                reconnect_backoff_max: Duration::from_secs(1),
            },
            rtsp: RtspSettings {
                url: "stub://test".to_string(),
                target_fps: 10,
                width: 64,
                height: 64,
                backend: RtspBackendPreference::Auto,
                transport: None,
            },
            file: FileSettings {
                path: String::new(),
                target_fps: 10,
            },
            v4l2: V4l2Settings {
                device: "/dev/video0".to_string(),
                target_fps: 10,
                width: 64,
                height: 64,
            },
            esp32: Esp32Settings {
                url: "http://127.0.0.1:81/stream".to_string(),
                target_fps: 10,
            },
            detect: DetectSettings {
                backend: witness_kernel::config::DetectBackendPreference::Stub,
                tract_model: None,
                tract_format: TractFormat::Yolov2,
                confidence_threshold: 0.5,
            },
            zones: ZoneSettings {
                module_zone_id: "zone:test".to_string(),
                sensitive_zones: vec![],
            },
            retention: Duration::from_secs(60),
            health: HealthSettings {
                heartbeat: true,
                log_interval: Duration::from_secs(60),
            },
            storage: StorageSettings {
                min_free_bytes: 0,
                check_interval: Duration::from_secs(60),
            },
            clock: ClockSettings {
                skew_tolerance: Duration::from_secs(30),
            },
            retention_check_interval: Duration::from_secs(300),
            storage_health: StorageHealthSettings::default(),
        }
    }

    fn test_kernel() -> (Kernel, KernelConfig) {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg).expect("open kernel");
        (kernel, cfg)
    }

    fn count_failures(kernel: &mut Kernel, cfg: &KernelConfig, failure_type: FailureType) -> usize {
        kernel
            .read_events_ruleset_bound(cfg.ruleset_hash, 1000)
            .expect("read records")
            .into_iter()
            .filter(|record| {
                matches!(record, SealedLogRecord::Failure(f) if f.failure_type == failure_type)
            })
            .count()
    }

    #[test]
    fn supervisor_seals_one_gap_record_per_outage() {
        let config = test_config();
        let (mut kernel, cfg) = test_kernel();
        let mut source = IngestSource::new(&config).expect("source");
        let mut pipeline = PipelineCounters::default();
        // failure_threshold is ZERO so the first error already counts as an outage.
        let mut supervisor = IngestSupervisor::new(Duration::ZERO, Duration::from_secs(1));
        let bucket = TimeBucket::now_10min().expect("bucket");
        let err = anyhow!("RTSP stream stalled");

        for _ in 0..3 {
            supervisor.on_error(
                &err,
                &mut source,
                &config,
                bucket,
                &mut kernel,
                &mut pipeline,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            );
        }
        assert_eq!(
            count_failures(&mut kernel, &cfg, FailureType::GapMissingData),
            1,
            "exactly one gap record per outage, regardless of error count"
        );

        // Recovery resets the latch; a new outage seals a new record.
        supervisor.on_success();
        supervisor.on_error(
            &err,
            &mut source,
            &config,
            bucket,
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(
            count_failures(&mut kernel, &cfg, FailureType::GapMissingData),
            2
        );
        assert_eq!(pipeline.failures_recorded, 2);
    }

    #[test]
    fn gap_record_details_carry_backend_name_not_url() {
        let config = test_config();
        let (mut kernel, cfg) = test_kernel();
        let mut source = IngestSource::new(&config).expect("source");
        let mut pipeline = PipelineCounters::default();
        let mut supervisor = IngestSupervisor::new(Duration::ZERO, Duration::from_secs(1));
        let bucket = TimeBucket::now_10min().expect("bucket");

        supervisor.on_error(
            &anyhow!("boom"),
            &mut source,
            &config,
            bucket,
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );

        let records = kernel
            .read_events_ruleset_bound(cfg.ruleset_hash, 10)
            .expect("read records");
        let details = records
            .iter()
            .find_map(|record| match record {
                SealedLogRecord::Failure(f) => f.details.clone(),
                _ => None,
            })
            .expect("failure record with details");
        assert!(details.contains("ingest_stalled backend=rtsp"), "{details}");
        assert!(
            !details.contains("stub://") && !details.contains("url"),
            "sealed details must not carry source URLs: {details}"
        );
    }

    #[test]
    fn clock_monitor_seals_one_record_per_bucket_regression() {
        let (mut kernel, cfg) = test_kernel();
        let mut pipeline = PipelineCounters::default();
        let mut monitor = ClockMonitor::new(Duration::from_secs(3600));
        let bucket = |start| TimeBucket {
            start_epoch_s: start,
            size_s: 600,
        };

        monitor.observe(
            bucket(1200),
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(count_failures(&mut kernel, &cfg, FailureType::ClockSkew), 0);

        // Bucket goes backwards: one ClockSkew record.
        monitor.observe(
            bucket(600),
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(count_failures(&mut kernel, &cfg, FailureType::ClockSkew), 1);

        // Staying at the regressed bucket does not re-fire.
        monitor.observe(
            bucket(600),
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(count_failures(&mut kernel, &cfg, FailureType::ClockSkew), 1);
    }

    #[test]
    fn clock_monitor_seals_drift_excursion_and_rebaselines() {
        let (mut kernel, cfg) = test_kernel();
        let mut pipeline = PipelineCounters::default();
        let mut monitor = ClockMonitor::new(Duration::from_secs(30));
        // Simulate a 2-minute wallclock jump by back-dating the baseline.
        monitor.baseline_wall = SystemTime::now() - Duration::from_secs(120);
        let bucket = TimeBucket::now_10min().expect("bucket");

        monitor.observe(
            bucket,
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(count_failures(&mut kernel, &cfg, FailureType::ClockSkew), 1);

        // Re-baselined: the same drift is not re-reported.
        monitor.observe(
            bucket,
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        assert_eq!(count_failures(&mut kernel, &cfg, FailureType::ClockSkew), 1);
    }

    #[test]
    fn disk_monitor_latches_storage_full_per_transition() {
        let (mut kernel, cfg) = test_kernel();
        let mut pipeline = PipelineCounters::default();
        // Threshold of u64::MAX: any real filesystem is "below threshold".
        let mut monitor = DiskMonitor::new(u64::MAX, Duration::ZERO);

        monitor.tick(
            "/tmp/witness-test.db",
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        monitor.tick(
            "/tmp/witness-test.db",
            &mut kernel,
            &mut pipeline,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );
        // On Linux the check runs and latches after the first tick; elsewhere
        // the check is unavailable and seals nothing.
        let expected = if cfg!(target_os = "linux") { 1 } else { 0 };
        assert_eq!(
            count_failures(&mut kernel, &cfg, FailureType::StorageFull),
            expected
        );
    }
}
