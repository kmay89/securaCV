//! handoff_sidecar — emit, verify and redact per-file handoff sidecars
//! (`docs/handoff_sidecars.md`).
//!
//! A handoff sidecar (`<file>.securacv-handoff.json`) is the unsigned carrier
//! that travels with a disclosed file: it embeds the device-signed receipt
//! rows of the disclosure verbatim, references RFC 3161 tokens by hash, and
//! labels everything else declared. `break_glass unseal`, the console, the
//! drill and `export_events` write one at disclosure time; this tool writes
//! one after the fact (`emit`, read-only, never overwrites), checks one
//! (`verify`), and forwards one without the recipient's name (`redact`).
//!
//! Exit codes: 0 no check failed; 1 a check failed; 2 usage error, or the
//! tool could not run (unreadable file, unopenable database).

use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};
use clap::{ArgAction, Parser, Subcommand};
use sha2::{Digest, Sha256};
use witness_kernel::break_glass::display_safe;
use witness_kernel::handoff::{self, HandoffFile, RecipientArgs, UnsealSelector, VerifyOptions};
use witness_kernel::verify_helpers;

#[derive(Parser, Debug)]
#[command(author, version, about)]
#[command(after_help = "\
A handoff sidecar is unsigned: it carries device-signed records, and \
everything else in it is declared by the tool that wrote it. It may name the \
recipient; forward it without the name with `handoff_sidecar redact`.")]
struct Args {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Write the handoff sidecar for a disclosed file after the fact
    /// (read-only; consumes nothing; never overwrites).
    Emit {
        /// The disclosed file (an export bundle, or an unsealed file).
        #[arg(long, value_name = "PATH")]
        file: PathBuf,
        /// Path to the witness database.
        #[arg(long, default_value = "witness.db")]
        db: String,
        /// Device key seed — used only to derive the database encryption key.
        #[arg(long, env = "DEVICE_KEY_SEED", hide_env_values = true)]
        device_key_seed: Option<String>,
        /// Explicit SQLCipher key (hex), overriding the seed derivation.
        #[arg(
            long,
            env = "SECURACV_DB_KEY",
            hide_env_values = true,
            conflicts_with = "device_key_seed",
            value_name = "HEX"
        )]
        db_key: Option<String>,
        /// For an unsealed file: the Granted receipt's entry hash (from
        /// `break_glass receipts`).
        #[arg(long, value_name = "HEX", conflicts_with = "unseal_record")]
        grant: Option<String>,
        /// For an unsealed file: the unseal record's entry hash (printed by
        /// `break_glass unseal`).
        #[arg(long, value_name = "HEX")]
        unseal_record: Option<String>,
        /// Where to write the sidecar (default `<file>.securacv-handoff.json`).
        #[arg(long, value_name = "PATH")]
        out: Option<PathBuf>,
        /// Also write each referenced token as `<DIR>/<token_sha256>.der`.
        #[arg(long, value_name = "DIR")]
        anchors_out: Option<PathBuf>,
        #[command(flatten)]
        recipient: RecipientArgs,
    },
    /// Check a handoff sidecar against the file it describes.
    Verify {
        /// The disclosed file.
        #[arg(long, value_name = "PATH")]
        file: PathBuf,
        /// The sidecar (default `<file>.securacv-handoff.json`).
        #[arg(long, value_name = "PATH")]
        sidecar: Option<PathBuf>,
        /// The device public key (64 hex), obtained out of band.
        #[arg(long, value_name = "HEX", conflicts_with = "public_key_file")]
        public_key: Option<String>,
        /// Same as `--public-key`, read from a file.
        #[arg(long, value_name = "FILE")]
        public_key_file: Option<String>,
        /// The database the disclosure came from (read-only deep checks).
        #[arg(long, value_name = "PATH")]
        db: Option<String>,
        /// With --db: device key seed for the database encryption key.
        #[arg(long, env = "DEVICE_KEY_SEED", hide_env_values = true)]
        device_key_seed: Option<String>,
        /// With --db: explicit SQLCipher key (hex).
        #[arg(
            long,
            env = "SECURACV_DB_KEY",
            hide_env_values = true,
            conflicts_with = "device_key_seed",
            value_name = "HEX"
        )]
        db_key: Option<String>,
        /// A directory holding the referenced token files (a court kit's
        /// `anchors/` works as is).
        #[arg(long, value_name = "DIR")]
        anchors_dir: Option<PathBuf>,
        /// TSA CA certificate (PEM) for `openssl ts -verify`; repeatable.
        #[arg(long, value_name = "PEM", action = ArgAction::Append)]
        ca: Vec<String>,
    },
    /// Write a copy of a sidecar without the recipient (pure: no database).
    Redact {
        /// The sidecar to redact.
        #[arg(long, value_name = "PATH")]
        sidecar: PathBuf,
        /// Where to write the redacted copy (never overwritten).
        #[arg(long, value_name = "PATH")]
        out: PathBuf,
    },
}

fn main() {
    let args = match Args::try_parse() {
        Ok(a) => a,
        Err(e) => {
            let code = if e.use_stderr() { 2 } else { 0 };
            let _ = e.print();
            std::process::exit(code);
        }
    };
    let code = match run(args) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("error: {:#}", e);
            2
        }
    };
    std::process::exit(code);
}

fn run(args: Args) -> Result<i32> {
    match args.command {
        Command::Emit {
            file,
            db,
            device_key_seed,
            db_key,
            grant,
            unseal_record,
            out,
            anchors_out,
            recipient,
        } => emit(EmitArgs {
            file,
            db,
            device_key_seed,
            db_key,
            grant,
            unseal_record,
            out,
            anchors_out,
            recipient,
        }),
        Command::Verify {
            file,
            sidecar,
            public_key,
            public_key_file,
            db,
            device_key_seed,
            db_key,
            anchors_dir,
            ca,
        } => verify(
            &file,
            sidecar,
            public_key,
            public_key_file,
            db,
            device_key_seed,
            db_key,
            anchors_dir,
            ca,
        ),
        Command::Redact { sidecar, out } => redact(&sidecar, &out),
    }
}

struct EmitArgs {
    file: PathBuf,
    db: String,
    device_key_seed: Option<String>,
    db_key: Option<String>,
    grant: Option<String>,
    unseal_record: Option<String>,
    out: Option<PathBuf>,
    anchors_out: Option<PathBuf>,
    recipient: RecipientArgs,
}

fn parse_hash(label: &str, s: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(s.trim()).map_err(|e| anyhow!("{} is not hex: {}", label, e))?;
    bytes
        .try_into()
        .map_err(|_| anyhow!("{} must be 32 bytes (64 hex)", label))
}

fn emit(a: EmitArgs) -> Result<i32> {
    let recipient = a.recipient.resolve()?;
    let out = a
        .out
        .clone()
        .unwrap_or_else(|| handoff::sidecar_path_for(&a.file));
    if std::fs::symlink_metadata(&out).is_ok() {
        return Err(overwrite_refusal(&out));
    }
    let bytes =
        std::fs::read(&a.file).map_err(|e| anyhow!("cannot read {}: {}", a.file.display(), e))?;
    if !Path::new(&a.db).exists() {
        return Err(anyhow!("database {} does not exist", a.db));
    }
    let conn = verify_helpers::open_db_read_only(
        &a.db,
        a.device_key_seed.as_deref(),
        a.db_key.as_deref(),
        "handoff_sidecar",
    )?;
    let name = a
        .file
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let bucket = handoff::handoff_bucket_now()?;
    let version = env!("CARGO_PKG_VERSION");
    let sidecar = match serde_json::from_slice::<witness_kernel::ExportBundle>(&bytes) {
        Ok(_) => {
            if a.grant.is_some() || a.unseal_record.is_some() {
                return Err(anyhow!(
                    "{} is an export bundle; --grant and --unseal-record name an unseal",
                    display_safe(&a.file.display().to_string())
                ));
            }
            handoff::build_export_sidecar(
                &conn,
                &bytes,
                &name,
                recipient.as_ref().map(|r| (r, None)),
                bucket,
                version,
            )?
        }
        Err(_) => {
            let selector = match (&a.grant, &a.unseal_record) {
                (Some(g), None) => UnsealSelector::Grant(parse_hash("--grant", g)?),
                (None, Some(u)) => UnsealSelector::UnsealRecord(parse_hash("--unseal-record", u)?),
                _ => {
                    return Err(anyhow!(
                        "{} is not an export bundle; for an unsealed file name its disclosure \
                         with --grant <HEX> (the Granted receipt's entry hash, from break_glass \
                         receipts) or --unseal-record <HEX> (printed by break_glass unseal)",
                        display_safe(&a.file.display().to_string())
                    ))
                }
            };
            let file = HandoffFile {
                bytes: bytes.len() as u64,
                name,
                sha256: hex::encode(Sha256::digest(&bytes)),
            };
            handoff::build_unseal_sidecar_from_db(
                &conn,
                selector,
                &file,
                recipient.as_ref(),
                bucket,
                version,
            )?
        }
    };
    let canonical = sidecar.to_canonical_bytes()?;
    // Self-check against the database before anything is written.
    let report =
        handoff::verify_sidecar_with_db(&conn, &canonical, &a.file, &VerifyOptions::default())?;
    if report.failed() > 0 {
        let first = report.first_failure().expect("failed > 0");
        return Err(anyhow!(
            "internal: the handoff sidecar did not verify ({}: {}); nothing was written",
            first.subject,
            first.detail
        ));
    }
    let anchors = handoff::select_anchors(
        &conn,
        &hex32(&sidecar.file.sha256)?,
        &hex32(
            &sidecar
                .records
                .last()
                .map(|r| r.entry_hash.clone())
                .unwrap_or_default(),
        )?,
    )?;
    write_out(&out, &canonical)?;
    if let Some(dir) = &a.anchors_out {
        std::fs::create_dir_all(dir)?;
        for (reference, der) in &anchors {
            let sha = reference
                .get("token_sha256")
                .and_then(|v| v.as_str())
                .unwrap_or_default();
            let path = dir.join(format!("{}.der", sha));
            match handoff::write_sidecar_new(&path, der) {
                Ok(()) => {}
                // The same token bytes already there are the same token.
                Err(_) if std::fs::read(&path).is_ok_and(|b| &b == der) => {}
                Err(e) => return Err(e),
            }
        }
    }
    let over_file = sidecar
        .anchors
        .iter()
        .filter(|v| v.get("covers").and_then(|c| c.as_str()) == Some("file_bytes"))
        .count();
    println!("handoff sidecar written to {}", out.display());
    println!(
        "  describes: {} of {} (sha256 {}…)",
        match sidecar.disclosure {
            handoff::Disclosure::Unseal => "unseal",
            handoff::Disclosure::Export => "export",
        },
        display_safe(&sidecar.file.name),
        &sidecar.file.sha256[..16]
    );
    println!("  pairing:   {}", handoff::pairing_name(sidecar.pairing));
    println!(
        "  recipient: {}",
        handoff::binding_str(sidecar.recipient_binding)
    );
    println!(
        "  anchors:   {} token(s) referenced ({} over the file bytes, {} over the disclosure \
         record)",
        sidecar.anchors.len(),
        over_file,
        sidecar.anchors.len() - over_file
    );
    Ok(0)
}

fn hex32(s: &str) -> Result<[u8; 32]> {
    parse_hash("hash", s)
}

#[allow(clippy::too_many_arguments)]
fn verify(
    file: &Path,
    sidecar: Option<PathBuf>,
    public_key: Option<String>,
    public_key_file: Option<String>,
    db: Option<String>,
    device_key_seed: Option<String>,
    db_key: Option<String>,
    anchors_dir: Option<PathBuf>,
    ca: Vec<String>,
) -> Result<i32> {
    let sidecar_path = sidecar.unwrap_or_else(|| handoff::sidecar_path_for(file));
    let sidecar_bytes = std::fs::read(&sidecar_path)
        .map_err(|e| anyhow!("cannot read {}: {}", sidecar_path.display(), e))?;
    std::fs::metadata(file).map_err(|e| anyhow!("cannot read {}: {}", file.display(), e))?;
    let pinned = match (&public_key, &public_key_file) {
        (Some(hex), _) => Some(verify_helpers::verifying_key_from_hex(hex)?),
        (None, Some(path)) => {
            let text = std::fs::read_to_string(path)
                .map_err(|e| anyhow!("failed to read public key file {}: {}", path, e))?;
            Some(verify_helpers::verifying_key_from_hex(text.trim())?)
        }
        (None, None) => None,
    };
    let opts = VerifyOptions {
        pinned,
        anchors_dir: anchors_dir.as_deref(),
        tsa_cas: &ca,
    };
    let report = match &db {
        Some(db) => {
            let conn = verify_helpers::open_db_read_only(
                db,
                device_key_seed.as_deref(),
                db_key.as_deref(),
                "handoff_sidecar",
            )?;
            handoff::verify_sidecar_with_db(&conn, &sidecar_bytes, file, &opts)?
        }
        None => handoff::verify_sidecar(&sidecar_bytes, file, &opts)?,
    };
    println!(
        "handoff sidecar {} for {}",
        display_safe(&sidecar_path.display().to_string()),
        display_safe(&file.display().to_string())
    );
    println!(
        "This sidecar is unsigned. It carries device-signed records; everything else in it is \
         declared by the tool that wrote it."
    );
    for check in &report.checks {
        println!("{}", check.line());
    }
    println!("{}", report.verdict_line());
    Ok(if report.failed() > 0 { 1 } else { 0 })
}

/// The overwrite refusal of `emit` and `redact`, the two commands that take
/// `--out`.
fn overwrite_refusal(out: &Path) -> anyhow::Error {
    anyhow!(
        "refusing to overwrite {}: a handoff sidecar is never refreshed in place — write the \
         new one with --out <PATH>",
        out.display()
    )
}

/// `write_sidecar_new` for `--out`: a path that appeared after the pre-check
/// gets the same refusal (with the `--out` hint this command has).
fn write_out(out: &Path, bytes: &[u8]) -> Result<()> {
    handoff::write_sidecar_new(out, bytes).map_err(|e| {
        if std::fs::symlink_metadata(out).is_ok() {
            overwrite_refusal(out)
        } else {
            e
        }
    })
}

fn redact(sidecar_path: &Path, out: &Path) -> Result<i32> {
    let bytes = std::fs::read(sidecar_path)
        .map_err(|e| anyhow!("cannot read {}: {}", sidecar_path.display(), e))?;
    let sidecar = handoff::HandoffSidecar::parse(&bytes)?;
    if std::fs::symlink_metadata(out).is_ok() {
        return Err(overwrite_refusal(out));
    }
    let redacted = handoff::redact(&sidecar);
    write_out(out, &redacted.to_canonical_bytes()?)?;
    println!(
        "handoff sidecar written to {} without the recipient (recipient: {})",
        out.display(),
        handoff::binding_str(redacted.recipient_binding)
    );
    Ok(0)
}
