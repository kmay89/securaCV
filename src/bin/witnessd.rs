//! witnessd - Privacy Witness Kernel daemon
//!
//! This daemon:
//! 1. Ingests frames from configured sources (RTSP, USB, etc.)
//! 2. Buffers frames in a bounded ring buffer (for potential vault sealing)
//! 3. Runs detection modules on InferenceView (restricted, no raw bytes)
//! 4. Enforces contract and module allowlist
//! 5. Writes conforming events to the sealed log
//! 6. Enforces retention with checkpointed pruning

use anyhow::{anyhow, Result};
use std::path::PathBuf;
use std::time::{Duration, Instant};

use witness_kernel::{
    api::{ApiConfig, ApiServer},
    break_glass::BreakGlassTokenFile,
    BucketKeyManager, CapabilityBoundaryRuntime, FrameBuffer, Kernel, KernelConfig, Module,
    ModuleDescriptor, RtspConfig, RtspSource, TimeBucket, Vault, VaultConfig, ZoneCrossingModule,
    ZonePolicy,
};

fn main() -> Result<()> {
    // Initialize logging (simple stderr for MVP)
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let kernel_version = env!("CARGO_PKG_VERSION");
    let ruleset_id = "ruleset:v0.1";
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);
    let device_key_seed =
        std::env::var("DEVICE_KEY_SEED").map_err(|_| anyhow!("DEVICE_KEY_SEED must be set"))?;

    let cfg = KernelConfig {
        db_path: "witness.db".to_string(),
        ruleset_id: ruleset_id.to_string(),
        ruleset_hash,
        kernel_version: kernel_version.to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7), // 7 days
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let mut kernel = Kernel::open(&cfg)?;

    let api_addr = std::env::var("WITNESS_API_ADDR").unwrap_or_else(|_| "127.0.0.1:8799".into());
    let api_token_path = std::env::var("WITNESS_API_TOKEN_PATH")
        .ok()
        .map(PathBuf::from);
    let api_config = ApiConfig {
        addr: api_addr,
        token_path: api_token_path,
        ..ApiConfig::default()
    };
    let api_handle = ApiServer::new(api_config, cfg.clone()).spawn()?;
    log::info!("event api listening on {}", api_handle.addr);
    if let Some(path) = &api_handle.token_path {
        log::info!("event api capability token written to {}", path.display());
    } else {
        log::warn!(
            "event api capability token (handle securely): {}",
            api_handle.token
        );
    }

    let mut vault = Vault::new(VaultConfig::default())?;
    // Optional break-glass seal path (requires BREAK_GLASS_SEAL_TOKEN with a token JSON).
    let mut seal_token = load_seal_token()?;

    // Configure RTSP source
    let rtsp_url =
        std::env::var("WITNESS_RTSP_URL").unwrap_or_else(|_| "stub://front_camera".into());
    let rtsp_config = RtspConfig {
        url: rtsp_url,
        target_fps: 10,
        width: 640,
        height: 480,
    };
    let mut source = RtspSource::new(rtsp_config)?;
    source.connect()?;

    // Frame buffer for pre-roll (vault sealing, not accessible without break-glass)
    let mut frame_buffer = FrameBuffer::new();

    // Detection module
    let mut module = ZoneCrossingModule::new("zone:front_boundary").with_tokens(true);
    let module_desc: ModuleDescriptor = module.descriptor();
    let runtime = CapabilityBoundaryRuntime::new();
    runtime.validate_descriptor(&module_desc)?;

    // Bucket key manager (rotates per time bucket)
    let mut token_mgr = BucketKeyManager::new();

    let mut last_prune = Instant::now();
    let mut last_health_log = Instant::now();
    let mut event_count = 0u64;

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

        // Get the latest frame for processing
        let Some(frame_ref) = frame_buffer.latest() else {
            continue;
        };

        // Modules receive InferenceView, NOT RawFrame
        // This is the isolation boundary: modules cannot access raw bytes
        let view = frame_ref.inference_view();

        // Run module inference
        let candidates = runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr)?;

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
                    log::warn!("event rejected: {}", e);
                    continue;
                }
            };

            event_count += 1;
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
                "rtsp health={} frames={} url={}",
                source.is_healthy(),
                stats.frames_captured,
                stats.url
            );
            last_health_log = Instant::now();
        }

        // Periodic retention enforcement with checkpoint
        if last_prune.elapsed() > Duration::from_secs(10) {
            kernel.enforce_retention_with_checkpoint(cfg.retention)?;
            last_prune = Instant::now();

            // Log buffer stats
            log::debug!(
                "frame buffer: {} frames, ~{} KB",
                frame_buffer.len(),
                frame_buffer.memory_bytes() / 1024
            );
        }

        // Target ~10 fps (100ms between frames)
        std::thread::sleep(Duration::from_millis(100));
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
