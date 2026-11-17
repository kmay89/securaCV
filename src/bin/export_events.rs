//! export_events - sequential event export bundle to a local artifact
//!
//! Two authorization modes:
//!   --self-export          owner self-export; possession of the device key seed
//!                          (which decrypts the database and signs the receipt)
//!                          is the credential. Exports only the privacy-filtered
//!                          artifact the local API already serves.
//!   --break-glass-token    trustee-quorum authorization for the same artifact.
//!
//! Either way a signed, hash-chained export receipt is appended to the sealed
//! log (Invariant IV), labeled with the authorization mode. Sealed-vault
//! evidence and unsealing remain quorum-only (`break_glass unseal`).

use anyhow::{anyhow, Result};
use clap::Parser;
use sha2::Digest;
use std::io::IsTerminal;
use std::time::Duration;
use witness_kernel::break_glass::BreakGlassTokenFile;
use witness_kernel::handoff;
use witness_kernel::{
    parse_duration_s, ExportOptions, ExportWindow, Kernel, KernelConfig, ZonePolicy,
};

#[path = "../ui.rs"]
mod ui;

const BUCKET_S: u64 = 600;

#[derive(Parser, Debug)]
#[command(author, version, about)]
#[command(after_help = "\
Exported times are coarsened to 10-minute buckets and jittered by design \
(see spec/event_contract.md); raw media never leaves the kernel. Every export \
appends a signed receipt to the tamper-evident log, so disclosures are always \
auditable.

Every export also writes <file>.securacv-handoff.json, a handoff sidecar that \
carries the signed receipt rows to whoever receives the file.")]
struct Args {
    /// Path to the witness database.
    #[arg(long, default_value = "witness.db")]
    db_path: String,
    /// Ruleset identifier to bind export interpretation.
    #[arg(long, default_value = "ruleset:v0.1")]
    ruleset_id: String,
    /// Device key seed (must match witnessd). When absent, the seed file
    /// witnessd keeps beside the database (`<db>.ed25519.seed`) is used.
    #[arg(long, env = "DEVICE_KEY_SEED", hide_env_values = true)]
    device_key_seed: Option<String>,
    /// Output file path for the export artifact.
    #[arg(long, default_value = "witness_export.json")]
    output: String,
    /// Scheduled-export mode: write `securacv-events-<bucket>.json` into this
    /// directory instead of --output (cron/systemd-timer friendly; the
    /// filename carries only the coarse bucket start, never a precise time).
    #[arg(long, conflicts_with = "output", value_name = "DIR")]
    output_dir: Option<std::path::PathBuf>,
    /// With --output-dir: keep only the newest N export files there, pruning
    /// older `securacv-events-*.json` (at least 1 is always kept).
    #[arg(long, requires = "output_dir", value_name = "N")]
    keep: Option<usize>,
    /// Path to a break-glass token file authorizing export (trustee quorum).
    #[arg(long, conflicts_with = "self_export")]
    break_glass_token: Option<String>,
    /// Owner self-export: authorize with the device key seed alone.
    /// The export receipt is labeled `self_export` so it stays distinguishable
    /// from quorum-authorized disclosures.
    #[arg(long)]
    self_export: bool,
    /// Printed name of the person exporting (self-export only; declared, signed into the receipt).
    /// A quorum export's requester is the one its grant recorded and is named by reference.
    #[arg(
        long,
        value_name = "NAME",
        conflicts_with = "break_glass_token",
        requires = "reason"
    )]
    requested_by: Option<String>,
    /// Reason code for the self-export (one of the break-glass reason codes; declared).
    #[arg(
        long,
        value_name = "CODE",
        conflicts_with = "break_glass_token",
        requires = "requested_by"
    )]
    reason: Option<String>,
    /// Case / ticket reference for the self-export (declared).
    #[arg(
        long,
        value_name = "REF",
        conflicts_with = "break_glass_token",
        requires = "requested_by"
    )]
    case_ref: Option<String>,
    /// The exporting operator's Ed25519 public key, 64 hex (claimed, not checked).
    #[arg(
        long,
        value_name = "HEX",
        conflicts_with = "break_glass_token",
        requires = "requested_by"
    )]
    requester_key: Option<String>,
    /// Who receives this export (declared; committed, salted, into the signed
    /// receipt and named only in the handoff sidecar).
    #[command(flatten)]
    recipient: witness_kernel::handoff::RecipientArgs,
    /// Export window start (Unix epoch seconds, floored to a 600s bucket boundary).
    #[arg(long, requires = "end", conflicts_with = "last")]
    start: Option<u64>,
    /// Export window end (Unix epoch seconds, ceiled to a 600s bucket boundary).
    #[arg(long, requires = "start", conflicts_with = "last")]
    end: Option<u64>,
    /// Export only the trailing duration, e.g. `--last 24h`, `--last 7d`,
    /// `--last 90m` (aligned outward to 600s bucket boundaries).
    #[arg(long)]
    last: Option<String>,
    /// Maximum events per export batch.
    #[arg(long, default_value_t = 50)]
    max_events_per_batch: usize,
    /// Jitter in seconds applied to exported time buckets.
    #[arg(long, default_value_t = 120)]
    jitter_s: u64,
    /// Jitter step in seconds (granularity of jitter).
    #[arg(long, default_value_t = 60)]
    jitter_step_s: u64,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
    /// Also sign a C2PA Content Credentials sidecar manifest
    /// (`<output>.c2pa`) over the exact bundle bytes, so the artifact is
    /// verifiable by any Content Credentials tool. Fully offline; the
    /// signing credential derives from the device key seed
    /// (docs/design/c2pa_export.md).
    #[cfg(feature = "c2pa-export")]
    #[arg(long)]
    c2pa: bool,
    /// With --c2pa: also write the device CA trust anchor (PEM) here, for
    /// handing to third-party verifiers. The anchor is deterministic in the
    /// seed, so this is a convenience copy, not state.
    #[cfg(feature = "c2pa-export")]
    #[arg(long, requires = "c2pa", value_name = "PATH")]
    c2pa_anchor_out: Option<std::path::PathBuf>,
}

/// Resolve --start/--end/--last into a bucket-aligned window (floor start,
/// ceil end) so the window itself never encodes finer-than-bucket timing.
/// Parsing and alignment live in the library (`parse_duration_s`,
/// `ExportWindow::aligned`/`::last`) and are shared with the event API's
/// `GET /export/bundle?last=` query.
fn resolve_window(args: &Args) -> Result<Option<ExportWindow>> {
    match (&args.last, args.start, args.end) {
        (Some(last), None, None) => Ok(Some(ExportWindow::last(parse_duration_s(last)?)?)),
        (None, Some(start), Some(end)) => Ok(Some(ExportWindow::aligned(start, end)?)),
        (None, None, None) => Ok(None),
        // clap's `requires`/`conflicts_with` make the remaining combinations unreachable.
        _ => Err(anyhow!("--start/--end must be given together")),
    }
}

/// Delete the oldest `securacv-events-<bucket>.json` files beyond `keep`
/// (sorted by the bucket number in the filename; at least 1 is always kept).
/// Only files matching the scheduled-export pattern are ever touched. A
/// pruned bundle takes its `.json.c2pa` sidecar (if any) with it — a
/// sidecar is meaningless without the exact bytes it signs.
fn prune_exports(dir: &std::path::Path, keep: usize) -> Result<usize> {
    let mut exports: Vec<(u64, std::path::PathBuf)> = Vec::new();
    for entry in std::fs::read_dir(dir)? {
        let path = entry?.path();
        let name = match path.file_name().and_then(|n| n.to_str()) {
            Some(name) => name,
            None => continue,
        };
        if let Some(bucket) = name
            .strip_prefix("securacv-events-")
            .and_then(|rest| rest.strip_suffix(".json"))
            .and_then(|num| num.parse::<u64>().ok())
        {
            exports.push((bucket, path));
        }
    }
    exports.sort_by_key(|(bucket, _)| *bucket);
    let excess = exports.len().saturating_sub(keep.max(1));
    for (_, path) in exports.iter().take(excess) {
        std::fs::remove_file(path)?;
        // Its C2PA manifest and its handoff sidecar go with it. A sidecar's
        // name never parses as a bundle bucket, so it is never counted.
        for suffix in [".c2pa", witness_kernel::handoff::HANDOFF_SUFFIX] {
            let mut sidecar_name = path.file_name().unwrap_or_default().to_os_string();
            sidecar_name.push(suffix);
            match std::fs::remove_file(path.with_file_name(sidecar_name)) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
        }
    }
    Ok(excess)
}

fn main() -> Result<()> {
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);
    let ruleset_hash = KernelConfig::ruleset_hash_from_id(&args.ruleset_id);

    if !args.self_export && args.break_glass_token.is_none() {
        return Err(anyhow!(
            "choose an authorization mode: --self-export (owner, device key seed) \
             or --break-glass-token <FILE> (trustee quorum)"
        ));
    }

    // Declared context and recipient are validated before anything opens, so
    // a bad flag spends nothing and records nothing.
    let operator = match &args.requested_by {
        Some(name) => Some(witness_kernel::break_glass::OperatorContext::new(
            name,
            args.reason.as_deref().unwrap_or_default(),
            args.case_ref.as_deref(),
            args.requester_key.as_deref(),
        )?),
        None => None,
    };
    let recipient = args.recipient.resolve()?;
    let recipient_salt = recipient
        .as_ref()
        .map(|_| zeroize::Zeroizing::new(handoff::fresh_salt()));
    let recipient_commitment = match (&recipient, &recipient_salt) {
        (Some(r), Some(salt)) => Some(handoff::recipient_commitment(salt, r)),
        _ => None,
    };

    // An export signs a receipt under the CURRENT device identity, so the seed
    // must be the one witnessd runs with: the flag / DEVICE_KEY_SEED, else the
    // seed file beside the database. Never generated here — an export must not
    // mint an identity because it was pointed at the wrong path.
    let device_key_seed =
        witness_kernel::crypto::find_device_seed(&args.db_path, args.device_key_seed.as_deref())?
            .map(|found| found.seed.to_string())
            .ok_or_else(|| {
                anyhow!(
            "DEVICE_KEY_SEED must be set (or the seed file witnessd keeps beside {} must exist)",
            args.db_path
        )
            })?;

    let cfg = KernelConfig {
        db_path: args.db_path.clone(),
        ruleset_id: args.ruleset_id.clone(),
        ruleset_hash,
        kernel_version: env!("CARGO_PKG_VERSION").to_string(),
        retention: Duration::from_secs(60 * 60 * 24 * 7),
        device_key_seed,
        zone_policy: ZonePolicy::default(),
    };

    let window = resolve_window(&args)?;
    let output_path: std::path::PathBuf = match &args.output_dir {
        Some(dir) => {
            std::fs::create_dir_all(dir)?;
            dir.join(format!(
                "securacv-events-{}.json",
                witness_kernel::TimeBucket::now_10min()?.start_epoch_s
            ))
        }
        None => std::path::PathBuf::from(&args.output),
    };
    // The stale-sidecar rule: before anything is exported, an old handoff
    // sidecar at this path is moved aside — restored if the export fails
    // before its bundle replaces the old one (a spent or invalid token must
    // not destroy the earlier bundle's recipient opening), deleted once it
    // has — and the sidecar path is proven writable.
    let sidecar_at = handoff::sidecar_path_for(&output_path);
    let stale = handoff::set_aside_stale_sidecar(&output_path).map_err(|e| {
        anyhow!(
            "refusing to export: a stale handoff sidecar at {} could not be moved aside ({}); \
             nothing was exported",
            sidecar_at.display(),
            e
        )
    })?;
    handoff::probe_sidecar_path(&output_path).map_err(|e| {
        anyhow!(
            "refusing to export: a handoff sidecar cannot be created at {} ({}); nothing was \
             exported",
            sidecar_at.display(),
            e
        )
    })?;
    let mut kernel = {
        let _stage = ui.stage("Open kernel");
        Kernel::open(&cfg)?
    };
    let options = ExportOptions {
        max_events_per_batch: args.max_events_per_batch,
        jitter_s: args.jitter_s,
        jitter_step_s: args.jitter_step_s,
        window,
    };
    let (bundle, mode) = if args.self_export {
        let _stage = ui.stage("Export events (self)");
        let context = witness_kernel::ExportContext::new(operator.clone(), recipient_commitment)?;
        (
            kernel.export_events_bundle_self_with(cfg.ruleset_hash, options, context)?,
            "self-export (device key seed)",
        )
    } else {
        let token_path = args.break_glass_token.as_deref().expect("checked above");
        let mut token = {
            let _stage = ui.stage("Load break-glass token");
            let token_json = std::fs::read_to_string(token_path)
                .map_err(|e| anyhow!("failed to read token file {}: {}", token_path, e))?;
            let token_file: BreakGlassTokenFile = serde_json::from_str(&token_json)
                .map_err(|e| anyhow!("invalid token file: {}", e))?;
            token_file.into_token()?
        };
        let _stage = ui.stage("Export events");
        (
            kernel.export_events_bundle_authorized_with(
                cfg.ruleset_hash,
                options,
                &mut token,
                recipient_commitment,
            )?,
            "break-glass (trustee quorum)",
        )
    };
    let json = serde_json::to_vec(&bundle)?;
    {
        let _stage = ui.stage("Write export bundle");
        std::fs::write(&output_path, &json)?;
    }
    // The new bundle replaced the old one: its sidecar describes bytes that
    // are gone from this path, so it is never restored beside these.
    stale.discard();
    // The handoff sidecar, right after the bundle and BEFORE the optional
    // C2PA sidecar, so a C2PA failure can never skip it (or its fail-closed
    // ERROR and recovery text). The bundle carries its own signed receipt,
    // so a missing sidecar loses no pairing. On failure the bundle is kept,
    // the error names a secret-free recovery command, and the tool exits 1.
    let sidecar_result = {
        let _stage = ui.stage("Write handoff sidecar");
        write_export_sidecar(
            &kernel,
            &json,
            &output_path,
            recipient
                .as_ref()
                .map(|r| (r, recipient_salt.as_ref().map(|s| **s))),
        )
    };
    let sidecar_path = match sidecar_result {
        Ok(path) => path,
        Err(e) => {
            eprintln!(
                "ERROR: export bundle written to {} (sha256 {}) but its handoff sidecar could not \
                 be written ({}). The bundle and its signed export receipt are kept. Recover the \
                 sidecar with:\n  handoff_sidecar emit --file {} --db {}",
                output_path.display(),
                hex::encode(sha2::Sha256::digest(&json)),
                e,
                output_path.display(),
                args.db_path
            );
            if recipient.is_some() {
                eprintln!(
                    "The receipt commits to the recipient you named; that commitment's salt \
                     existed only in the sidecar that was not written, so a recovered sidecar \
                     records the recipient as committed but not disclosed."
                );
            }
            std::process::exit(1);
        }
    };
    #[cfg(feature = "c2pa-export")]
    let c2pa_sidecar: Option<(std::path::PathBuf, String)> = if args.c2pa {
        use witness_kernel::c2pa_export;
        let _stage = ui.stage("Sign C2PA sidecar");
        let credentials = c2pa_export::credentials_from_seed(&cfg.device_key_seed)?;
        let binding = c2pa_export::WitnessBinding {
            receipt_entry_hash: hex::encode(bundle.receipt_entry.entry_hash),
            artifact_hash: hex::encode(bundle.receipt_entry.receipt.artifact_hash),
            device_public_key: hex::encode(
                witness_kernel::verifying_key_from_seed(&cfg.device_key_seed)?.to_bytes(),
            ),
            auth_mode: if args.self_export {
                "self_export".to_string()
            } else {
                "break_glass".to_string()
            },
            ruleset_id: cfg.ruleset_id.clone(),
            kernel_version: cfg.kernel_version.clone(),
        };
        let manifest = c2pa_export::sign_export_sidecar(
            &json,
            &credentials,
            &binding,
            "SecuraCV witness export",
        )?;
        let sidecar_path = {
            let mut name = output_path.file_name().unwrap_or_default().to_os_string();
            name.push(".c2pa");
            output_path.with_file_name(name)
        };
        std::fs::write(&sidecar_path, manifest)?;
        if let Some(anchor_path) = &args.c2pa_anchor_out {
            std::fs::write(anchor_path, credentials.ca_cert_pem.as_bytes())?;
        }
        Some((sidecar_path, credentials.ca_fingerprint()?))
    } else {
        None
    };
    let pruned = match (&args.output_dir, args.keep) {
        (Some(dir), Some(keep)) => prune_exports(dir, keep)?,
        _ => 0,
    };
    let bucket_count: usize = bundle
        .artifact
        .batches
        .iter()
        .map(|b| b.buckets.len())
        .sum();
    println!("export bundle written to {}", output_path.display());
    println!("  authorization: {}", mode);
    if pruned > 0 {
        println!("  rotation: pruned {} older export file(s)", pruned);
    }
    match window {
        Some(w) => println!(
            "  window (bucket-aligned): {}..{} ({} bucket(s) in {} batch(es))",
            w.start_epoch_s,
            w.end_epoch_s,
            bucket_count,
            bundle.artifact.batches.len()
        ),
        None => println!(
            "  window: full retained history ({} bucket(s) in {} batch(es))",
            bucket_count,
            bundle.artifact.batches.len()
        ),
    }
    println!(
        "  receipt: signed export receipt {}… appended to the tamper-evident log",
        &hex::encode(bundle.receipt_entry.entry_hash)[..16]
    );
    if let Some(op) = &operator {
        println!(
            "  context:         {}, {}{} — declared by the exporting operator, recorded in the \
             signed receipt",
            witness_kernel::break_glass::display_safe(&op.requester_name),
            op.reason_code,
            op.case_ref
                .as_deref()
                .map(|c| format!(", case {}", witness_kernel::break_glass::display_safe(c)))
                .unwrap_or_default()
        );
    }
    if let Some(grant) = bundle.receipt_entry.receipt.authorizing_receipt {
        println!(
            "  consent:         recorded on the authorizing grant {}… (trustee-consented; named \
             by reference)",
            &hex::encode(grant)[..16]
        );
    }
    println!(
        "  recipient:       {}",
        if recipient.is_some() {
            "committed in the signed receipt; named only in the handoff sidecar"
        } else {
            "none named"
        }
    );
    println!("  handoff sidecar: {} (mode 0600)", sidecar_path.display());
    #[cfg(feature = "c2pa-export")]
    if let Some((sidecar_path, ca_fingerprint)) = c2pa_sidecar {
        println!(
            "  c2pa: Content Credentials sidecar written to {}",
            sidecar_path.display()
        );
        println!(
            "  c2pa: device CA anchor fingerprint sha256:{}…",
            &ca_fingerprint[..16]
        );
        if let Some(anchor_path) = &args.c2pa_anchor_out {
            println!(
                "  c2pa: trust anchor PEM written to {}",
                anchor_path.display()
            );
        }
    }
    if args.jitter_s > 0 {
        println!(
            "  note: exported times are coarsened to {}-minute buckets and jittered ±{}s \
             (step {}s) by design — see spec/event_contract.md",
            BUCKET_S / 60,
            args.jitter_s,
            args.jitter_step_s
        );
    } else {
        println!(
            "  note: exported times are coarsened to {}-minute buckets by design — \
             see spec/event_contract.md",
            BUCKET_S / 60
        );
    }
    Ok(())
}

/// Build, self-verify and write the export's handoff sidecar (create-new,
/// 0600). Returns its path.
fn write_export_sidecar(
    kernel: &Kernel,
    bundle_bytes: &[u8],
    output_path: &std::path::Path,
    recipient: Option<(&handoff::HandoffRecipient, Option<[u8; 32]>)>,
) -> Result<std::path::PathBuf> {
    let name = output_path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let sidecar = handoff::build_export_sidecar(
        &kernel.conn,
        bundle_bytes,
        &name,
        recipient,
        handoff::handoff_bucket_now()?,
        env!("CARGO_PKG_VERSION"),
    )?;
    let bytes = zeroize::Zeroizing::new(sidecar.to_canonical_bytes()?);
    let report = handoff::verify_sidecar_bytes(
        &bytes,
        bundle_bytes,
        &handoff::VerifyOptions {
            pinned: Some(kernel.device_verifying_key()),
            ..Default::default()
        },
    );
    if report.verdict() != "valid" {
        return Err(anyhow!(
            "internal: the handoff sidecar did not verify ({})",
            report
                .first_failure()
                .map(|c| format!("{}: {}", c.subject, c.detail))
                .unwrap_or_else(|| report.verdict().to_string())
        ));
    }
    let path = handoff::sidecar_path_for(output_path);
    // Test seam (debug builds only): plant an obstacle at the sidecar path
    // after the stale check, so the failure path can be exercised.
    #[cfg(debug_assertions)]
    if std::env::var_os("SECURACV_TEST_HANDOFF_OBSTACLE").is_some() {
        std::fs::create_dir_all(path.join("obstacle"))?;
    }
    handoff::write_sidecar_new(&path, &bytes)?;
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::{resolve_window, Args, BUCKET_S};
    use clap::Parser;
    use witness_kernel::parse_duration_s;

    #[test]
    fn windows_align_outward_and_reject_overflow() {
        let args = |start: &str, end: &str| {
            Args::parse_from([
                "export_events",
                "--device-key-seed",
                "devkey:test:seed",
                "--self-export",
                "--start",
                start,
                "--end",
                end,
            ])
        };
        let w = resolve_window(&args("601", "1799")).unwrap().unwrap();
        assert_eq!(w.start_epoch_s, 600);
        assert_eq!(w.end_epoch_s, 1800);
        // End near u64::MAX must error instead of overflowing on bucket ceil.
        let max = u64::MAX.to_string();
        assert!(resolve_window(&args("600", &max)).is_err());
        // Exact multiple of the bucket stays put.
        let w = resolve_window(&args("1200", "2400")).unwrap().unwrap();
        assert_eq!((w.start_epoch_s, w.end_epoch_s), (1200, 2400));
        assert_eq!(w.end_epoch_s % BUCKET_S, 0);
    }

    #[test]
    fn prune_keeps_newest_and_ignores_other_files() {
        let dir = tempfile::tempdir().expect("tempdir");
        for bucket in [600u64, 1200, 1800, 2400] {
            std::fs::write(
                dir.path().join(format!("securacv-events-{bucket}.json")),
                b"{}",
            )
            .expect("write");
        }
        std::fs::write(dir.path().join("unrelated.json"), b"{}").expect("write");
        // C2PA sidecars: present for some bundles (600, 1800), absent for
        // others — pruning a bundle must take its sidecar with it, never
        // count sidecars as bundles, and not fail when a sidecar is absent.
        std::fs::write(dir.path().join("securacv-events-600.json.c2pa"), b"x").expect("write");
        std::fs::write(dir.path().join("securacv-events-1800.json.c2pa"), b"x").expect("write");
        // Handoff sidecars travel and prune the same way, and never count
        // as a bundle (their name never parses as a bucket).
        std::fs::write(
            dir.path()
                .join("securacv-events-600.json.securacv-handoff.json"),
            b"{}",
        )
        .expect("write");
        std::fs::write(
            dir.path()
                .join("securacv-events-2400.json.securacv-handoff.json"),
            b"{}",
        )
        .expect("write");

        let pruned = super::prune_exports(dir.path(), 2).expect("prune");
        assert_eq!(pruned, 2);
        assert!(!dir.path().join("securacv-events-600.json").exists());
        assert!(!dir.path().join("securacv-events-600.json.c2pa").exists());
        assert!(!dir
            .path()
            .join("securacv-events-600.json.securacv-handoff.json")
            .exists());
        assert!(dir
            .path()
            .join("securacv-events-2400.json.securacv-handoff.json")
            .exists());
        assert!(!dir.path().join("securacv-events-1200.json").exists());
        assert!(dir.path().join("securacv-events-1800.json").exists());
        assert!(dir.path().join("securacv-events-1800.json.c2pa").exists());
        assert!(dir.path().join("securacv-events-2400.json").exists());
        assert!(dir.path().join("unrelated.json").exists());

        // keep=0 still keeps the newest file — a rotation misconfiguration
        // must never delete the export that was just written.
        let pruned = super::prune_exports(dir.path(), 0).expect("prune");
        assert_eq!(pruned, 1);
        assert!(dir.path().join("securacv-events-2400.json").exists());
        assert!(!dir.path().join("securacv-events-1800.json.c2pa").exists());
    }

    #[test]
    fn durations_parse() {
        assert_eq!(parse_duration_s("24h").unwrap(), 86_400);
        assert_eq!(parse_duration_s("7d").unwrap(), 604_800);
        assert_eq!(parse_duration_s("90m").unwrap(), 5_400);
        assert_eq!(parse_duration_s("3600s").unwrap(), 3_600);
        assert_eq!(parse_duration_s("3600").unwrap(), 3_600);
        assert!(parse_duration_s("yesterday").is_err());
        assert!(parse_duration_s("h").is_err());
    }
}
