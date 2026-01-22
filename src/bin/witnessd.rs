//! witnessd - Privacy Witness Kernel daemon
//!
//! This daemon:
//! 1. Ingests frames from configured sources (RTSP, USB, etc.)
//! 2. Buffers frames in a bounded ring buffer (for potential vault sealing)
//! 3. Runs detection modules on InferenceView (restricted, no raw bytes)
//! 4. Enforces contract and module allowlist
//! 5. Writes conforming events to the sealed log
//! 6. Enforces retention with checkpointed pruning

use anyhow::Result;
use std::time::{Duration, Instant};

use witness_kernel::{
    BucketKeyManager, FrameBuffer, Kernel, KernelConfig, Module, ModuleDescriptor, RtspConfig,
    RtspSource, TimeBucket, ZoneCrossingModule,
};

fn main() -> Result<()> {
    // Initialize logging (simple stderr for MVP)
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let kernel_version = env!("CARGO_PKG_VERSION");
    let ruleset_id = "ruleset:v0.1";
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(ruleset_id);

    let cfg = KernelConfig {
        db_path: "witness.db".to_string(),
        ruleset_id: ruleset_id.to_string(),
        ruleset_hash,
        kernel_version: kernel_version.to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7), // 7 days
        device_key_seed: "devkey:mvp".to_string(),
    };

    let mut kernel = Kernel::open(&cfg)?;

    // Configure RTSP source (synthetic for MVP)
    let rtsp_config = RtspConfig {
        url: "rtsp://synthetic/front_camera".to_string(),
        target_fps: 10,
        width: 640,
        height: 480,
    };
    let mut source = RtspSource::new(rtsp_config);
    source.connect()?;

    // Frame buffer for pre-roll (vault sealing, not accessible without break-glass)
    let mut frame_buffer = FrameBuffer::new();

    // Detection module
    let mut module = ZoneCrossingModule::new("zone:front_boundary").with_tokens(true);
    let module_desc: ModuleDescriptor = module.descriptor();

    // Bucket key manager (rotates per time bucket)
    let mut token_mgr = BucketKeyManager::new();

    let mut last_prune = Instant::now();
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
        let candidates = module.process(&view, bucket, &token_mgr)?;

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
