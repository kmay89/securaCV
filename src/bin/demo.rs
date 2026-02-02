//! demo - end-to-end synthetic run for the Privacy Witness Kernel

use anyhow::{anyhow, Context, Result};
use clap::{Parser, ValueEnum};
use ed25519_dalek::{Signer, SigningKey};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use witness_kernel::break_glass::{
    Approval, BreakGlass, BreakGlassToken, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
use witness_kernel::crypto::signatures::{SignatureMode, DOMAIN_CHECKPOINT};
#[cfg(feature = "backend-tract")]
use witness_kernel::detect::TractBackend;
use witness_kernel::detect::{BackendRegistry, CpuBackend, StubBackend};
use witness_kernel::vault::DEFAULT_VAULT_PATH;
use witness_kernel::verify;
use witness_kernel::{
    break_glass_receipt_outcome_for_verifier, device_public_key_from_db, verify_entry_signature,
    verify_export_bundle, BackendSelection, CandidateEvent, CapabilityBoundaryRuntime,
    DeviceCapabilities, EventType, ExportArtifact, ExportOptions, ExportReceipt, FileConfig,
    FileSource, InferenceBackend, Kernel, KernelConfig, Module, RtspConfig, RtspSource, TimeBucket,
    Vault, VaultConfig, ZoneCrossingModule, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

const DEFAULT_DB_PATH: &str = "demo_witness.db";
const DEFAULT_RULESET_ID: &str = "ruleset:demo";
const DEFAULT_ZONE_ID: &str = "zone:demo";
const DEFAULT_VAULT_ENVELOPE_ID: &str = "demo-vault";
const DEMO_FRAME_WIDTH: u32 = 320;
const DEMO_FRAME_HEIGHT: u32 = 320;

struct BreakGlassContext<'a> {
    policy: &'a QuorumPolicy,
    trustee_key: &'a SigningKey,
    trustee_id: &'a TrusteeId,
}

struct BreakGlassRequest<'a> {
    envelope_id: &'a str,
    ruleset_hash: [u8; 32],
    reason: &'a str,
    bucket: TimeBucket,
}

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    /// Duration in seconds for synthetic frames/events.
    #[arg(long, default_value_t = 5)]
    seconds: u64,
    /// Frames per second for synthetic source.
    #[arg(long, default_value_t = 10)]
    fps: u32,
    /// Vault path (defaults to kernel DEFAULT_VAULT_PATH).
    #[arg(long)]
    vault: Option<String>,
    /// Output directory for export bundle.
    #[arg(long, default_value = "demo_out")]
    out: String,
    /// Local file path for ingest (requires ingest-file-ffmpeg feature).
    #[arg(long, conflicts_with = "rtsp")]
    file: Option<String>,
    /// RTSP URL for ingest (requires rtsp-gstreamer or rtsp-ffmpeg feature).
    #[arg(long, conflicts_with = "file")]
    rtsp: Option<String>,
    /// Optional deterministic seed for demo artifacts.
    #[arg(long)]
    seed: Option<u64>,
    /// Detector backend selection ("auto", "stub", "cpu", or "tract").
    #[arg(long, value_enum, default_value_t = DetectorBackendChoice::Auto)]
    detector_backend: DetectorBackendChoice,
    /// Path to ONNX model for the tract backend.
    #[arg(long)]
    tract_model: Option<PathBuf>,
}

#[derive(Copy, Clone, Debug, ValueEnum, PartialEq, Eq)]
enum DetectorBackendChoice {
    Auto,
    Stub,
    Cpu,
    Tract,
}

enum DemoSource {
    File(FileSource),
    Rtsp(RtspSource),
}

impl DemoSource {
    fn connect(&mut self) -> Result<()> {
        match self {
            DemoSource::File(source) => source.connect(),
            DemoSource::Rtsp(source) => source.connect(),
        }
    }

    fn next_frame(&mut self) -> Result<witness_kernel::RawFrame> {
        match self {
            DemoSource::File(source) => source.next_frame(),
            DemoSource::Rtsp(source) => source.next_frame(),
        }
    }
}

fn main() -> Result<()> {
    let args = Args::parse();
    if args.fps == 0 {
        return Err(anyhow!("fps must be >= 1"));
    }

    let out_dir = PathBuf::from(&args.out);
    fs::create_dir_all(&out_dir)?;

    let vault_path = args
        .vault
        .as_deref()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_VAULT_PATH));

    let device_key_seed = match args.seed {
        Some(seed) => format!("devkey:demo:{}", seed),
        None => "devkey:demo".to_string(),
    };

    stage("open kernel + vault");
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(DEFAULT_RULESET_ID);
    let cfg = KernelConfig {
        db_path: DEFAULT_DB_PATH.to_string(),
        ruleset_id: DEFAULT_RULESET_ID.to_string(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let mut kernel = Kernel::open(&cfg)?;
    let vault = Vault::new(VaultConfig {
        local_path: vault_path.clone(),
    })?;

    let trustee_key = derive_trustee_key(args.seed);
    let trustee_id = TrusteeId::new("demo-trustee");
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: trustee_id.clone(),
            public_key: trustee_key.verifying_key().to_bytes(),
        }],
    )?;
    kernel.set_break_glass_policy(&policy)?;

    let export_path = out_dir.join("export_bundle.json");
    let total_frames = args.seconds.saturating_mul(args.fps as u64);

    let mut event_count = 0u64;
    let mut candidates_total: u64 = 0;
    let mut vault_sealed = false;
    let mut export_bundle_bytes: Option<Vec<u8>> = None;
    let mut input_summary = "verify-only (no new frames)".to_string();

    if total_frames > 0 {
        let (mut source, source_summary) = build_demo_source(&args)?;
        input_summary = source_summary.clone();
        stage(&format!(
            "ingest frames + events (input: {})",
            source_summary
        ));
        source.connect()?;

        let capabilities = DeviceCapabilities::cpu_only();
        let backend_selection = match args.detector_backend {
            DetectorBackendChoice::Auto => BackendSelection::Auto,
            DetectorBackendChoice::Stub => BackendSelection::Require(InferenceBackend::Stub),
            DetectorBackendChoice::Cpu => BackendSelection::Require(InferenceBackend::Cpu),
            DetectorBackendChoice::Tract => BackendSelection::Require(InferenceBackend::Tract),
        };
        let mut module = ZoneCrossingModule::with_backend_selection(
            DEFAULT_ZONE_ID,
            backend_selection,
            &capabilities,
        )?
        .with_tokens(false);
        let module_desc = module.descriptor();
        let runtime = CapabilityBoundaryRuntime::new();
        runtime.validate_descriptor(&module_desc)?;
        let mut registry = BackendRegistry::new();
        registry.register(StubBackend::new());
        registry.register(CpuBackend::new());
        if args.detector_backend == DetectorBackendChoice::Tract {
            register_tract_backend(&mut registry, &args)?;
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
        let mut token_mgr = witness_kernel::BucketKeyManager::new();

        let now_bucket = TimeBucket::now(600)?;
        let ctx = BreakGlassContext {
            policy: &policy,
            trustee_key: &trustee_key,
            trustee_id: &trustee_id,
        };
        let mut vault_token = issue_break_glass_token(
            &mut kernel,
            &ctx,
            BreakGlassRequest {
                envelope_id: DEFAULT_VAULT_ENVELOPE_ID,
                ruleset_hash,
                reason: "demo-vault-seal",
                bucket: now_bucket,
            },
        )?;
        let mut export_token = issue_break_glass_token(
            &mut kernel,
            &ctx,
            BreakGlassRequest {
                envelope_id: EXPORT_EVENTS_ENVELOPE_ID,
                ruleset_hash,
                reason: "demo-export",
                bucket: now_bucket,
            },
        )?;
        let verifying_key = kernel.device_verifying_key();

        for _ in 0..total_frames {
            let frame = source.next_frame()?;
            let bucket = frame.timestamp_bucket;
            token_mgr.rotate_if_needed(bucket);
            let view = frame.inference_view();
            let candidates =
                runtime.execute_sandboxed(&mut module, &view, bucket, &token_mgr, &registry)?;
            candidates_total += candidates.len() as u64;
            for cand in candidates {
                let _ev = kernel.append_event_checked(
                    &module_desc,
                    cand,
                    &cfg.kernel_version,
                    &cfg.ruleset_id,
                    cfg.ruleset_hash,
                )?;
                event_count += 1;
            }

            if !vault_sealed {
                let conn = &kernel.conn;
                let outcome = |hash: &[u8; 32]| {
                    break_glass_receipt_outcome_for_verifier(conn, &verifying_key, hash)
                };
                let _meta = vault.seal_frame(
                    DEFAULT_VAULT_ENVELOPE_ID,
                    &mut vault_token,
                    ruleset_hash,
                    frame,
                    &verifying_key,
                    outcome,
                )?;
                vault_sealed = true;
            }
        }

        if candidates_total == 0 {
            let fallback = CandidateEvent {
                event_type: EventType::BoundaryCrossingObjectLarge,
                time_bucket: now_bucket,
                zone_id: DEFAULT_ZONE_ID.to_string(),
                confidence: 0.5,
                correlation_token: None,
            };
            let _ev = kernel.append_event_checked(
                &module_desc,
                fallback,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )?;
            event_count += 1;
            eprintln!(
                "demo: module produced 0 candidates; wrote 1 fallback event to validate pipeline"
            );
        }

        stage("export events");
        let options = ExportOptions::default();
        let bundle =
            kernel.export_events_bundle_authorized(ruleset_hash, options, &mut export_token)?;
        let bundle_bytes = serde_json::to_vec(&bundle)?;
        let artifact_bytes = serde_json::to_vec(&bundle.artifact)?;
        export_bundle_bytes = Some(bundle_bytes);
        fs::write(&export_path, &artifact_bytes)
            .with_context(|| format!("writing export bundle to {}", export_path.display()))?;
    }

    if export_bundle_bytes.is_none() {
        export_bundle_bytes = Some(read_export_bundle(&export_path)?);
    }

    stage("verify log + export bundle");
    let verify_result = verify_demo(&cfg.db_path, export_bundle_bytes.as_ref().unwrap());

    println!("demo summary:");
    println!("  input: {}", input_summary);
    println!("  frames processed: {}", total_frames);
    println!("  candidates produced: {}", candidates_total);
    println!("  events written: {}", event_count);
    println!("  log db: {}", cfg.db_path);
    println!("  vault path: {}", vault_path.display());
    println!("  export bundle: {}", export_path.display());
    println!(
        "  verify: {}",
        if verify_result.is_ok() { "OK" } else { "FAIL" }
    );
    println!("next steps:");
    println!("  cargo run --bin log_verify -- --db {}", cfg.db_path);
    println!("  ls -la {}", out_dir.display());

    verify_result
}

fn build_demo_source(args: &Args) -> Result<(DemoSource, String)> {
    if let Some(path) = args.file.as_ref() {
        let source = FileSource::new(FileConfig {
            path: path.clone(),
            target_fps: args.fps,
        })?;
        return Ok((DemoSource::File(source), format!("file: {}", path)));
    }

    if let Some(url) = args.rtsp.as_ref() {
        let source = RtspSource::new(RtspConfig {
            url: url.clone(),
            target_fps: args.fps,
            width: DEMO_FRAME_WIDTH,
            height: DEMO_FRAME_HEIGHT,
            backend: witness_kernel::config::RtspBackendPreference::Auto,
        })?;
        return Ok((DemoSource::Rtsp(source), format!("rtsp: {}", url)));
    }

    let source = RtspSource::new(RtspConfig {
        url: "stub://demo".to_string(),
        target_fps: args.fps,
        width: DEMO_FRAME_WIDTH,
        height: DEMO_FRAME_HEIGHT,
        backend: witness_kernel::config::RtspBackendPreference::Auto,
    })?;
    Ok((
        DemoSource::Rtsp(source),
        "synthetic (stub://demo)".to_string(),
    ))
}

fn register_tract_backend(registry: &mut BackendRegistry, args: &Args) -> Result<()> {
    #[cfg(feature = "backend-tract")]
    {
        let model_path = args
            .tract_model
            .as_ref()
            .ok_or_else(|| anyhow!("--tract-model is required for tract backend"))?;
        let backend = TractBackend::new(model_path, DEMO_FRAME_WIDTH, DEMO_FRAME_HEIGHT)?;
        registry.register(backend);
        return Ok(());
    }
    #[cfg(not(feature = "backend-tract"))]
    {
        let _ = registry;
        let _ = args;
        Err(anyhow!("tract backend requires the backend-tract feature"))
    }
}

fn stage(msg: &str) {
    eprintln!("demo: {}", msg);
}

fn derive_trustee_key(seed: Option<u64>) -> SigningKey {
    let mut hasher = Sha256::new();
    hasher.update(b"demo-trustee");
    if let Some(seed) = seed {
        hasher.update(seed.to_le_bytes());
    }
    let digest: [u8; 32] = hasher.finalize().into();
    SigningKey::from_bytes(&digest)
}

fn issue_break_glass_token(
    kernel: &mut Kernel,
    ctx: &BreakGlassContext<'_>,
    request: BreakGlassRequest<'_>,
) -> Result<BreakGlassToken> {
    let request = UnlockRequest::new(
        request.envelope_id,
        request.ruleset_hash,
        request.reason,
        request.bucket,
    )?;
    let signature = ctx.trustee_key.sign(&request.request_hash());
    let approval = Approval::new(
        ctx.trustee_id.clone(),
        request.request_hash(),
        signature.to_vec(),
    );
    let approvals = std::slice::from_ref(&approval);
    let (result, receipt) =
        BreakGlass::authorize(ctx.policy, &request, approvals, request.time_bucket);
    let mut token = result?;
    let receipt_hash = kernel.log_break_glass_receipt(&receipt, approvals)?;
    kernel.sign_break_glass_token(&mut token, receipt_hash)?;
    Ok(token)
}

fn read_export_bundle(path: &Path) -> Result<Vec<u8>> {
    fs::read(path).with_context(|| format!("reading export bundle from {}", path.display()))
}

fn verify_demo(db_path: &str, export_bundle_bytes: &[u8]) -> Result<()> {
    let conn = rusqlite::Connection::open(db_path)?;
    let verifying_key = device_public_key_from_db(&conn)?;
    let checkpoint = verify::latest_checkpoint(&conn)?;
    if let (Some(head), Some(sig)) = (checkpoint.chain_head_hash, checkpoint.signatures.as_ref()) {
        verify_entry_signature(
            &verifying_key,
            &head,
            sig,
            SignatureMode::Compat,
            None,
            DOMAIN_CHECKPOINT,
        )
        .context("checkpoint signature mismatch")?;
    }

    verify::verify_events_with(
        &conn,
        &verifying_key,
        checkpoint.chain_head_hash,
        SignatureMode::Compat,
        None,
        |_, _| {},
    )?;
    let policy = verify::load_break_glass_policy(&conn)?;
    verify::verify_break_glass_receipts_with(
        &conn,
        &verifying_key,
        policy.as_ref(),
        SignatureMode::Compat,
        None,
        |_, _| {},
    )?;
    verify::verify_export_receipts_with(
        &conn,
        &verifying_key,
        SignatureMode::Compat,
        None,
        |_, _| {},
    )?;

    if let Ok(export_bundle) =
        serde_json::from_slice::<witness_kernel::ExportBundle>(export_bundle_bytes)
    {
        verify_export_bundle(&export_bundle)?;
        return Ok(());
    }

    let artifact: ExportArtifact = serde_json::from_slice(export_bundle_bytes)?;
    let artifact_bytes = serde_json::to_vec(&artifact)?;
    let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
    let mut stmt = conn.prepare("SELECT payload_json FROM export_receipts ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    while let Some(row) = rows.next()? {
        let payload: String = row.get(0)?;
        let receipt: ExportReceipt = serde_json::from_str(&payload)?;
        if receipt.artifact_hash == artifact_hash {
            return Ok(());
        }
    }
    Err(anyhow!("export artifact hash not found in export receipts"))
}
