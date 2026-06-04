//! End-to-end RTSP ingestion test.
//!
//! Stands up a real RTSP server (MediaMTX), publishes the committed mp4 fixture
//! to it on a loop (ffmpeg), and drives the *real* `RtspSource` (ffmpeg backend)
//! through the full pipeline — decode -> detection -> signed events ->
//! verification — proving the documented RTSP feature works end to end. This is
//! the runnable answer to the v1 acceptance item "RTSP ingestion works
//! end-to-end" (previously implemented but exercised only by file ingestion).
//!
//! The whole file compiles only with the `rtsp-ffmpeg` feature and is exercised
//! by the `ingest-rtsp` CI job. Locally it self-skips if `ffmpeg` or the MediaMTX
//! server binary are unavailable; set `SECURACV_RTSP_E2E=1` to turn a missing
//! dependency into a hard failure so CI can never pass this gate vacuously.
//!
//! The MediaMTX binary is located via `MEDIAMTX_BIN` (preferred) or `mediamtx`
//! on `PATH`.
#![cfg(feature = "rtsp-ffmpeg")]

use std::io::Write;
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use witness_kernel::config::RtspBackendPreference;
use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::detect::{BackendRegistry, CpuBackend, StubBackend};
use witness_kernel::{
    device_public_key_from_db, verify, BackendSelection, BucketKeyManager,
    CapabilityBoundaryRuntime, DeviceCapabilities, InferenceBackend, Kernel, KernelConfig, Module,
    RtspConfig, RtspSource, ZoneCrossingModule, ZonePolicy,
};

const FIXTURE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fixtures/testclip.mp4");
const RULESET_ID: &str = "ruleset:rtsp-e2e";
// >= MIN_SEED_LENGTH (32). Fixed so the test is deterministic; the DB is
// single-use and verified in-process, so a static seed is fine here.
const DEVICE_KEY_SEED: &str = "devkey:rtsp-e2e-fixed-test-seed-00000000000000000000";
const STREAM_PATH: &str = "securacv";

/// Kills every spawned child process (server + publisher) on drop.
struct ProcGuard(Vec<Child>);
impl Drop for ProcGuard {
    fn drop(&mut self) {
        for child in &mut self.0 {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn binary_runs(bin: &str, version_flag: &str) -> bool {
    Command::new(bin)
        .arg(version_flag)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn mediamtx_bin() -> Option<String> {
    let candidate = std::env::var("MEDIAMTX_BIN").unwrap_or_else(|_| "mediamtx".to_string());
    binary_runs(&candidate, "--version").then_some(candidate)
}

/// Grab an ephemeral free TCP port by binding to :0 and releasing it. A short
/// TOCTOU window remains before the server rebinds it; connect/readiness polls
/// cover it.
fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .expect("bind ephemeral port")
        .local_addr()
        .expect("local addr")
        .port()
}

fn port_open(port: u16) -> bool {
    TcpStream::connect_timeout(
        &format!("127.0.0.1:{port}").parse().unwrap(),
        Duration::from_millis(200),
    )
    .is_ok()
}

fn wait_for_port(port: u16, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if port_open(port) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    false
}

/// Start MediaMTX with a minimal config: RTSP only, on `port`, accepting any
/// publish/read path. Returns the child and the temp config file (kept alive).
fn start_server(bin: &str, port: u16) -> (Child, tempfile::NamedTempFile) {
    let mut cfg = tempfile::Builder::new()
        .prefix("mediamtx_")
        .suffix(".yml")
        .tempfile()
        .expect("temp config");
    write!(
        cfg,
        "logLevel: error\n\
         rtmp: no\n\
         hls: no\n\
         webrtc: no\n\
         srt: no\n\
         rtspAddress: :{port}\n\
         paths:\n  all_others:\n"
    )
    .expect("write mediamtx config");
    cfg.flush().expect("flush config");

    let child = Command::new(bin)
        .arg(cfg.path())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn mediamtx");
    (child, cfg)
}

/// Publish the fixture to the server on a loop. Re-encoding to H.264 baseline
/// yuv420p guarantees an RTP-friendly stream regardless of the fixture codec.
fn start_publisher(port: u16) -> Child {
    let url = format!("rtsp://127.0.0.1:{port}/{STREAM_PATH}");
    Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            FIXTURE,
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rtsp",
            "-rtsp_transport",
            "tcp",
            &url,
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn ffmpeg publisher")
}

/// Connect to the stream, retrying while the publisher comes up and announces.
fn connect_with_retry(port: u16, timeout: Duration) -> RtspSource {
    let cfg = RtspConfig {
        url: format!("rtsp://127.0.0.1:{port}/{STREAM_PATH}"),
        target_fps: 10,
        width: 160,
        height: 120,
        // Force the ffmpeg backend explicitly: if both rtsp features are built,
        // Auto would prefer gstreamer, but this gate targets the ffmpeg path.
        backend: RtspBackendPreference::Ffmpeg,
        // Force interleaved TCP so loopback transport is deterministic.
        transport: Some("tcp".to_string()),
    };
    let deadline = Instant::now() + timeout;
    let mut last_err = String::from("(no attempt completed)");
    while Instant::now() < deadline {
        match RtspSource::new(cfg.clone()).and_then(|mut s| s.connect().map(|()| s)) {
            Ok(source) => return source,
            Err(e) => last_err = format!("{e:#}"),
        }
        std::thread::sleep(Duration::from_millis(300));
    }
    panic!("could not connect to RTSP stream within {timeout:?}: {last_err}");
}

#[test]
fn rtsp_ingest_produces_verified_log() {
    let hard_required = std::env::var("SECURACV_RTSP_E2E").as_deref() == Ok("1");

    let ffmpeg_ok = binary_runs("ffmpeg", "-version");
    let server = mediamtx_bin();
    if !ffmpeg_ok || server.is_none() {
        assert!(
            !hard_required,
            "SECURACV_RTSP_E2E=1 requires ffmpeg and MediaMTX, but ffmpeg_ok={ffmpeg_ok}, \
             mediamtx={:?} (set MEDIAMTX_BIN or put `mediamtx` on PATH)",
            server
        );
        eprintln!(
            "skipping rtsp_e2e: missing dependency (ffmpeg_ok={ffmpeg_ok}, mediamtx={server:?}); \
             set SECURACV_RTSP_E2E=1 to require it"
        );
        return;
    }
    let server_bin = server.unwrap();

    let port = free_port();
    let (server_child, _cfg) = start_server(&server_bin, port);
    let mut guard = ProcGuard(vec![server_child]);
    assert!(
        wait_for_port(port, Duration::from_secs(15)),
        "MediaMTX did not open RTSP port {port} in time"
    );

    guard.0.push(start_publisher(port));

    let mut source = connect_with_retry(port, Duration::from_secs(45));

    // Single-use DB encrypted from a fixed seed; verified in-process below.
    let tmp = tempfile::Builder::new()
        .prefix("rtsp_e2e_")
        .suffix(".db")
        .tempfile()
        .expect("temp db");
    let db_path = tmp.path().to_string_lossy().to_string();

    let cfg = KernelConfig {
        db_path,
        ruleset_id: RULESET_ID.to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed: DEVICE_KEY_SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg).expect("open kernel");

    let capabilities = DeviceCapabilities::cpu_only();
    let mut module = ZoneCrossingModule::with_backend_selection(
        "zone:rtsp",
        BackendSelection::Require(InferenceBackend::Cpu),
        &capabilities,
    )
    .expect("build module")
    .with_tokens(false);
    let module_desc = module.descriptor();
    let runtime = CapabilityBoundaryRuntime::new();
    runtime
        .validate_descriptor(&module_desc)
        .expect("validate descriptor");

    let mut registry = BackendRegistry::new();
    registry.register(StubBackend::new());
    registry.register(CpuBackend::new());
    registry.set_default("cpu").expect("set default backend");

    let mut token_mgr = BucketKeyManager::new();

    // Read a bounded number of frames; stop early once motion has produced
    // events so the test stays fast. A wall-clock ceiling guards against a
    // stalled stream (next_frame blocks on packet reads).
    let mut frames = 0u64;
    let mut events = 0u64;
    let frame_ceiling = 60u64;
    let deadline = Instant::now() + Duration::from_secs(45);

    while frames < frame_ceiling && Instant::now() < deadline {
        let frame = match source.next_frame() {
            Ok(f) => f,
            Err(e) => {
                if frames > 0 {
                    break; // tolerate end/stall after we've decoded real frames
                }
                panic!("RTSP decode failed before any frame: {e:#}");
            }
        };
        frames += 1;

        let bucket = frame.timestamp_bucket;
        token_mgr.rotate_if_needed(bucket);
        let view = frame.inference_view();
        let cands = runtime
            .execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry)
            .expect("run detection");
        for cand in cands {
            kernel
                .append_event_checked(
                    &module_desc,
                    cand,
                    &cfg.kernel_version,
                    &cfg.ruleset_id,
                    cfg.ruleset_hash,
                )
                .expect("append event");
            events += 1;
        }

        if events > 0 && frames >= 8 {
            break;
        }
    }

    assert!(
        frames > 1,
        "expected multiple frames decoded over RTSP, got {frames}"
    );
    assert!(
        events > 0,
        "expected RTSP-ingested motion to produce signed events (frames={frames})"
    );

    // Verify the resulting log with the production verifier.
    let verifying_key = device_public_key_from_db(&kernel.conn).expect("device key");
    let checkpoint = verify::latest_checkpoint(&kernel.conn).expect("checkpoint");
    let verified = verify::verify_events_with(
        &kernel.conn,
        &verifying_key,
        checkpoint.chain_head_hash,
        SignatureMode::Compat,
        None,
        |_, _| {},
    )
    .expect("verify events");

    assert_eq!(
        events, verified,
        "every RTSP-ingested event must verify ({events} written, {verified} verified)"
    );

    eprintln!("rtsp_e2e: frames={frames} events={events} verified={verified}");
    drop(guard);
}
