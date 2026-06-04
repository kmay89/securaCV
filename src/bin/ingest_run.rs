//! ingest_run — batch-process a video file into a signed witness log.
//!
//! Unlike `witnessd` (a continuous daemon) and `demo` (a fixed
//! frame-count synthetic run), this tool ingests an entire video file
//! from start to EOF, runs detection on every frame, appends witness
//! events to the kernel log, and finally verifies the resulting log.
//!
//! It is the runnable answer to the v1 acceptance criterion
//! "Can process video from file". Requires the `ingest-file-ffmpeg`
//! feature (real mp4 decode via libav).

use anyhow::{anyhow, Result};
use clap::{Parser, ValueEnum};

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::detect::{BackendRegistry, CpuBackend, StubBackend};
use witness_kernel::{
    device_public_key_from_db, signing_key_from_seed, verify, BackendSelection,
    CapabilityBoundaryRuntime, DeviceCapabilities, FileConfig, FileSource, InferenceBackend,
    Kernel, KernelConfig, Module, ZoneCrossingModule, ZonePolicy,
};

const RULESET_ID: &str = "ruleset:ingest";

#[derive(Parser, Debug)]
#[command(
    name = "ingest_run",
    about = "Process a video file into a signed witness log"
)]
struct Args {
    /// Path to the local video file (e.g. clip.mp4).
    #[arg(long)]
    video: String,
    /// Output SQLite database path for the witness log.
    #[arg(long, default_value = "ingest.db")]
    db: String,
    /// Target frame rate (source may decimate to this rate).
    #[arg(long, default_value_t = 10)]
    fps: u32,
    /// Zone id recorded on detected events.
    #[arg(long, default_value = "zone:ingest")]
    zone: String,
    /// Detector backend.
    #[arg(long, value_enum, default_value_t = Backend::Auto)]
    backend: Backend,
    /// Safety cap on frames processed (0 = unlimited, stop at EOF).
    #[arg(long, default_value_t = 0)]
    max_frames: u64,
    /// Device key seed. The output DB is SQLCipher-encrypted with a key
    /// derived from this seed. Supply a stable seed (and keep it secret) if
    /// you need to reopen/verify the DB later; otherwise a fresh random seed
    /// is used per run and the DB is single-use (verified in-process here).
    #[arg(long)]
    device_key_seed: Option<String>,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum Backend {
    Auto,
    Stub,
    Cpu,
}

struct Summary {
    frames: u64,
    candidates: u64,
    events: u64,
    verified: u64,
    /// Whether the device key seed was caller-supplied (reproducible/reopenable)
    /// or randomly generated per run (single-use DB).
    seed_supplied: bool,
    /// Hex SQLCipher key needed to reopen the DB with `log_verify --db-key`.
    db_key: String,
}

fn random_seed() -> String {
    use rand::RngCore;
    let mut bytes = [0u8; 32];
    rand::rngs::OsRng.fill_bytes(&mut bytes);
    format!("devkey:{}", hex::encode(bytes))
}

fn run(args: &Args) -> Result<Summary> {
    // Each run generates a fresh random device key, which derives the
    // SQLCipher DB key. Reopening an existing db with a new key would fail,
    // so start from a clean database (this is a batch tool, not a daemon).
    if std::path::Path::new(&args.db).exists() {
        let _ = std::fs::remove_file(&args.db);
        let _ = std::fs::remove_file(format!("{}-wal", args.db));
        let _ = std::fs::remove_file(format!("{}-shm", args.db));
    }

    let seed_supplied = args.device_key_seed.is_some();
    let device_key_seed = args.device_key_seed.clone().unwrap_or_else(random_seed);
    let db_key = witness_kernel::resolve_db_encryption_key(
        &signing_key_from_seed(&device_key_seed)?,
        witness_kernel::db_key_seed_from_env()
            .as_ref()
            .map(|s| s.as_str()),
    )
    .to_string();

    let cfg = KernelConfig {
        db_path: args.db.clone(),
        ruleset_id: RULESET_ID.to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: std::time::Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;

    let mut source = FileSource::new(FileConfig {
        path: args.video.clone(),
        target_fps: args.fps,
    })?;
    source.connect()?;

    let capabilities = DeviceCapabilities::cpu_only();
    let selection = match args.backend {
        Backend::Auto => BackendSelection::Auto,
        Backend::Stub => BackendSelection::Require(InferenceBackend::Stub),
        Backend::Cpu => BackendSelection::Require(InferenceBackend::Cpu),
    };
    let mut module =
        ZoneCrossingModule::with_backend_selection(&args.zone, selection, &capabilities)?
            .with_tokens(false);
    let module_desc = module.descriptor();
    let runtime = CapabilityBoundaryRuntime::new();
    runtime.validate_descriptor(&module_desc)?;

    let mut registry = BackendRegistry::new();
    registry.register(StubBackend::new());
    registry.register(CpuBackend::new());
    match module.backend() {
        InferenceBackend::Stub => registry.set_default("stub")?,
        InferenceBackend::Cpu => registry.set_default("cpu")?,
        other => return Err(anyhow!("unsupported backend: {:?}", other)),
    }

    let mut token_mgr = witness_kernel::BucketKeyManager::new();

    let mut frames = 0u64;
    let mut candidates = 0u64;
    let mut events = 0u64;

    loop {
        if args.max_frames != 0 && frames >= args.max_frames {
            break;
        }
        let frame = match source.next_frame() {
            Ok(frame) => frame,
            Err(e) => {
                // A file source signals end-of-stream by erroring once it
                // runs out of frames. Treat any error after at least one
                // decoded frame as clean EOF; a failure before the first
                // frame is a real error (e.g. unreadable file).
                if frames > 0 {
                    break;
                }
                return Err(e);
            }
        };
        frames += 1;

        let bucket = frame.timestamp_bucket;
        token_mgr.rotate_if_needed(bucket);
        let view = frame.inference_view();
        let cands = runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry)?;
        candidates += cands.len() as u64;
        for cand in cands {
            kernel.append_event_checked(
                &module_desc,
                cand,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )?;
            events += 1;
        }
    }

    if frames == 0 {
        return Err(anyhow!("no frames decoded from {}", args.video));
    }

    // Verify the resulting log with the production verifier.
    let verifying_key = device_public_key_from_db(&kernel.conn)?;
    let checkpoint = verify::latest_checkpoint(&kernel.conn)?;
    let verified = verify::verify_events_with(
        &kernel.conn,
        &verifying_key,
        checkpoint.chain_head_hash,
        SignatureMode::Compat,
        None,
        |_, _| {},
    )?;

    Ok(Summary {
        frames,
        candidates,
        events,
        verified,
        seed_supplied,
        db_key,
    })
}

fn main() -> Result<()> {
    let args = Args::parse();
    println!("ingest_run: processing {}", args.video);
    let s = run(&args)?;
    println!();
    println!("summary:");
    println!("  frames processed : {}", s.frames);
    println!("  candidates       : {}", s.candidates);
    println!("  events written   : {}", s.events);
    println!("  events verified  : {}", s.verified);
    println!("  log db           : {}", args.db);
    println!();
    // The DB is SQLCipher-encrypted with a key derived from the device key
    // seed. log_verify must be given that key via --db-key, otherwise it
    // cannot open the database. The in-process verification above already
    // confirmed all events before the key left memory.
    println!("verify independently with:");
    println!(
        "  cargo run --bin log_verify -- --db {} --db-key {}",
        args.db, s.db_key
    );
    if !s.seed_supplied {
        println!();
        println!(
            "note: a random device key seed was used, so this DB is single-use. \
             Pass --device-key-seed <seed> for a reproducible, reopenable log."
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> String {
        format!("{}/tests/fixtures/testclip.mp4", env!("CARGO_MANIFEST_DIR"))
    }

    struct TempDb {
        path: String,
    }
    impl TempDb {
        fn new() -> Self {
            let mut p = std::env::temp_dir();
            let suffix: u64 = rand::random();
            p.push(format!("ingest_run_test_{}.db", suffix));
            Self {
                path: p.to_string_lossy().to_string(),
            }
        }
    }
    impl Drop for TempDb {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
            let _ = std::fs::remove_file(format!("{}-wal", self.path));
            let _ = std::fs::remove_file(format!("{}-shm", self.path));
        }
    }

    #[test]
    fn processes_real_mp4_to_verified_log() {
        let db = TempDb::new();
        let args = Args {
            video: fixture(),
            db: db.path.clone(),
            fps: 10,
            zone: "zone:test".to_string(),
            backend: Backend::Cpu,
            max_frames: 0,
            device_key_seed: None,
        };

        let s = run(&args).expect("ingest should succeed on the fixture mp4");

        // The fixture is a multi-frame animated clip, so the motion detector
        // must fire and produce signed events — not a trivial zero-event pass.
        assert!(
            s.frames > 1,
            "expected multiple decoded frames, got {}",
            s.frames
        );
        assert!(s.events > 0, "expected detected motion to produce events");
        assert_eq!(
            s.events, s.verified,
            "every written event must verify ({} written, {} verified)",
            s.events, s.verified
        );
    }
}
