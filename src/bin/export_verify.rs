//! export_verify - Verify an export bundle against export receipts.
//!
//! This tool proves:
//! - The export receipt log is hash-chained (tamper-evident)
//! - Each receipt entry is signed by the device key (Ed25519)
//! - The bundle's artifact hashes to a receipt recorded in the DB (how much of
//!   the file that covers is below)
//!
//! "The device key" holds only when the verifying key came from out of band
//! (`--public-key`, `--public-key-file`, `--device-key-seed` or
//! `DEVICE_KEY_SEED`). With none of them the key is read from the database
//! under audit, which proves the bundle and the receipt chain agree with each
//! other, not whose device signed them, and the final line says so:
//! `SELF-CONSISTENT, IDENTITY UNVERIFIED`, the label `log_verify` prints for
//! the same case (its B3 rule). The label follows the flag, not the key's
//! source: a seed copied out of the file beside the database into
//! `DEVICE_KEY_SEED` reads as anchored without being out of band.
//!
//! What the bundle check covers depends on the file. A full `ExportBundle`
//! (what `export_events` writes) is parsed, not hashed whole: its receipt
//! entry must be one of the entries in the checked receipt chain, and its
//! artifact, re-serialized, must hash to that receipt's `artifact_hash`.
//! Whitespace, key order and any field the structs do not carry are
//! therefore outside the verdict. Only a bare-artifact file (no receipt
//! wrapper) is hashed byte for byte.

use anyhow::{anyhow, Result};
use clap::{Parser, ValueEnum};
use rusqlite::Connection;
use sha2::{Digest, Sha256};
use std::io::IsTerminal;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{verify, verify_export_bundle, verify_helpers, ExportBundle, ExportReceipt};

#[path = "../ui.rs"]
mod ui;

#[derive(Parser, Debug)]
#[command(
    name = "export_verify",
    about = "Verify an export bundle against export receipts"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db")]
    db: String,

    /// Path to export bundle file
    #[arg(long, value_name = "PATH")]
    bundle: String,

    /// Device public key (hex-encoded Ed25519 verifying key)
    #[arg(long, value_name = "HEX", conflicts_with = "public_key_file")]
    public_key: Option<String>,

    /// Path to file containing hex-encoded device public key
    #[arg(long, value_name = "PATH", conflicts_with = "public_key")]
    public_key_file: Option<String>,

    /// Verbose output
    #[arg(short, long)]
    verbose: bool,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
    /// Signature verification mode (compat = Ed25519 OR PQ, strict = Ed25519 AND PQ)
    #[arg(long, value_enum, default_value = "compat")]
    sig_mode: SignatureModeArg,
    /// PQ public key (hex-encoded)
    #[arg(long, value_name = "HEX", conflicts_with = "pq_public_key_file")]
    pq_public_key: Option<String>,
    /// Path to file containing PQ public key (hex-encoded)
    #[arg(long, value_name = "PATH", conflicts_with = "pq_public_key")]
    pq_public_key_file: Option<String>,
    /// SQLCipher database encryption key (hex-encoded, 32 bytes)
    #[arg(
        long,
        value_name = "HEX",
        env = "SECURACV_DB_KEY",
        hide_env_values = true
    )]
    db_key: Option<String>,
    /// Device key seed (as used by the kernel/bridges). Derives the SQLCipher
    /// key (when --db-key is not given) and the verifying key (when no
    /// --public-key/--public-key-file is given) — the semantics log_verify
    /// uses, so `DEVICE_KEY_SEED=... export_verify --db witness.db
    /// --bundle ...` verifies an owner self-export with the seed alone. One
    /// exception: after `break_glass rotate-identity` the current seed
    /// derives a later epoch's key, which log_verify verifies self-anchored
    /// and export_verify refuses (docs/db_key_rotation.md, "Verifier CLIs").
    /// The anchored label records which flag carried the seed, not where the
    /// seed came from: one copied out of the seed file beside the database
    /// under audit earns the label but anchors nothing.
    #[arg(
        long,
        value_name = "SEED",
        env = "DEVICE_KEY_SEED",
        hide_env_values = true
    )]
    device_key_seed: Option<String>,
    /// Also verify a C2PA Content Credentials sidecar manifest against the
    /// bundle bytes (docs/design/c2pa_export.md).
    #[arg(long, value_name = "PATH")]
    #[cfg(feature = "c2pa-export")]
    c2pa_manifest: Option<String>,
    /// Trust anchor (device CA, PEM) for the sidecar verification. When
    /// omitted, the anchor is re-derived from --device-key-seed; one of the
    /// two must be available.
    #[arg(long, value_name = "PATH", requires = "c2pa_manifest")]
    #[cfg(feature = "c2pa-export")]
    c2pa_anchor: Option<String>,
}

#[derive(ValueEnum, Clone, Debug)]
enum SignatureModeArg {
    Compat,
    Strict,
}

impl From<SignatureModeArg> for SignatureMode {
    fn from(value: SignatureModeArg) -> Self {
        match value {
            SignatureModeArg::Compat => SignatureMode::Compat,
            SignatureModeArg::Strict => SignatureMode::Strict,
        }
    }
}

fn main() -> Result<()> {
    let args = Args::parse();
    let signature_mode: SignatureMode = args.sig_mode.into();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);

    // SQLCipher key: explicit --db-key wins; otherwise derive it from the
    // device key seed exactly as the kernel/bridges do (same logic as
    // log_verify); with neither flag, the seed file beside the database is
    // tried — for the database key only, never as an identity anchor (it is
    // not out-of-band, and after a rotation it holds the current key).
    let seed_for_db_key: Option<String> = match (&args.db_key, &args.device_key_seed) {
        (Some(_), _) => None,
        (None, Some(seed)) => Some(seed.clone()),
        (None, None) => witness_kernel::crypto::find_device_seed(&args.db, None)?.map(|found| {
            eprintln!(
                "export_verify: database key from the {} (it does not anchor identity)",
                found.source
            );
            found.seed.to_string()
        }),
    };
    let db_key: Option<String> = match (&args.db_key, seed_for_db_key.as_deref()) {
        (Some(key), _) => Some(key.clone()),
        (None, Some(seed)) => {
            let signing_key = witness_kernel::signing_key_from_seed(seed)?;
            let seed_env = witness_kernel::db_key_seed_from_env();
            Some(
                witness_kernel::resolve_db_encryption_key(
                    &signing_key,
                    seed_env.as_ref().map(|s| s.as_str()),
                )
                .to_string(),
            )
        }
        (None, None) => None,
    };

    let conn = {
        let _stage = ui.stage("Open database");
        let conn = Connection::open(&args.db)?;
        if let Some(ref key) = db_key {
            conn.pragma_update(None, "key", format!("x'{}'", key))?;
        }
        conn
    };
    let public_key_hex: Option<String> = match (&args.public_key, args.device_key_seed.as_deref()) {
        (Some(hex), _) => Some(hex.clone()),
        (None, Some(seed)) if args.public_key_file.is_none() => Some(hex::encode(
            witness_kernel::verifying_key_from_seed(seed)?.to_bytes(),
        )),
        _ => None,
    };
    // B3 honesty, the rule log_verify applies (verify_runner sets
    // identity_verified = public_key_hex.is_some()): "signed by the device
    // key" in the module doc is true only of a key that came from outside the
    // database under audit. --public-key and --public-key-file are that;
    // --device-key-seed / DEVICE_KEY_SEED derive it (public_key_hex is Some for
    // the seed only when no key file is given, and then the file anchors). The
    // seed file beside the database never sets either: it only opens the
    // database (see seed_for_db_key above). With none of them,
    // load_verifying_key falls through to device_metadata.public_key, so a
    // wholesale rewrite under another key passes, and the verdict must say it
    // is self-consistent, not identity-bound. One path differs from
    // log_verify: a seed that derives a later lineage epoch's key (after
    // rotate-identity) is downgraded to self-anchored there, while here it is
    // taken as genesis and the lineage check refuses the run before any label
    // (the open decision in docs/db_key_rotation.md, "Verifier CLIs").
    let identity_anchored = public_key_hex.is_some() || args.public_key_file.is_some();
    let verifying_key = {
        let _stage = ui.stage("Load verifying key");
        verify_helpers::load_verifying_key(
            &conn,
            public_key_hex.as_deref(),
            args.public_key_file.as_deref(),
        )?
    };
    let pq_verifying_key = {
        let _stage = ui.stage("Load PQ verifying key (optional)");
        verify_helpers::load_pq_verifying_key(
            &conn,
            args.pq_public_key.as_deref(),
            args.pq_public_key_file.as_deref(),
        )?
    };

    println!("export_verify: checking {}", args.bundle);
    println!();

    // Entry hashes verified below against the key resolved above (out of band
    // when identity_anchored, else the database's own; never the bundle's).
    // Bundle files are later required to reference one of these entries, so a
    // bundle re-signed under an attacker-chosen key can never pass.
    let mut verified_entry_hashes: Vec<[u8; 32]> = Vec::new();
    {
        let _stage = ui.stage("Verify export receipts");
        let lineage_keys = verify_helpers::lineage_verifying_keys(&conn, &verifying_key)?;
        let count = verify::verify_export_receipts_with(
            &conn,
            &lineage_keys,
            signature_mode,
            pq_verifying_key.as_ref(),
            |id, entry_hash| {
                verified_entry_hashes.push(entry_hash);
                if args.verbose {
                    println!(
                        "  receipt {}: hash={} OK",
                        id,
                        &verify_helpers::hex32(&entry_hash)[..16]
                    );
                }
            },
        )?;
        println!("verified {} export receipt entries", count);
    }
    println!();

    let bundle_bytes = {
        let _stage = ui.stage("Read bundle");
        std::fs::read(&args.bundle)
            .map_err(|e| anyhow!("failed to read bundle {}: {}", args.bundle, e))?
    };

    // The file is normally an ExportBundle as written by `export_events`
    // (artifact + signed receipt + keys). The bundle carries its own device
    // key, which an attacker could swap alongside a re-signed receipt, so
    // bundle-internal checks (verify_export_bundle: artifact binding + receipt
    // signature) prove only internal consistency. Trust comes from requiring
    // the bundled receipt entry to be one of the entries checked above under
    // the key resolved above — that entry hash commits to the full receipt
    // payload, including auth_mode and window, so none of it can be
    // rewritten. A bare-artifact file (no receipt wrapper) is matched by its
    // whole-file hash against the chain checked above, as older exports
    // produced.
    #[cfg_attr(not(feature = "c2pa-export"), allow(unused_variables))]
    let (artifact_hash, bundle_entry_hash): ([u8; 32], Option<[u8; 32]>) =
        match serde_json::from_slice::<ExportBundle>(&bundle_bytes) {
            Ok(bundle) => {
                let _stage = ui.stage("Verify bundle receipt + artifact binding");
                verify_export_bundle(&bundle)?;
                if !verified_entry_hashes.contains(&bundle.receipt_entry.entry_hash) {
                    return Err(anyhow!(
                        "TAMPER: bundle receipt entry not found in the verified export-receipt chain"
                    ));
                }
                if let Some(mode) = bundle.receipt_entry.receipt.auth_mode {
                    println!("bundle authorization: {:?}", mode);
                }
                let receipt = &bundle.receipt_entry.receipt;
                if let Some(op) = receipt.context.as_ref().and_then(|c| c.operator.as_ref()) {
                    let safe = witness_kernel::break_glass::display_safe;
                    println!(
                        "  disclosure context: {} ({}{}) — declared by the exporting operator, \
                         not trustee-consented",
                        safe(&op.requester_name),
                        safe(&op.reason_code),
                        op.case_ref
                            .as_deref()
                            .map(|c| format!(", case {}", safe(c)))
                            .unwrap_or_default()
                    );
                }
                if let Some(grant) = receipt.authorizing_receipt {
                    println!(
                        "  authorizing grant: {} (consent recorded on the grant)",
                        hex::encode(grant)
                    );
                }
                if receipt
                    .context
                    .as_ref()
                    .is_some_and(|c| c.recipient_commitment.is_some())
                {
                    println!(
                        "  recipient: committed in the signed receipt (named only in a handoff \
                         sidecar)"
                    );
                }
                (
                    bundle.receipt_entry.receipt.artifact_hash,
                    Some(bundle.receipt_entry.entry_hash),
                )
            }
            Err(_) => (Sha256::digest(&bundle_bytes).into(), None),
        };
    if args.verbose {
        println!("artifact hash: {}", verify_helpers::hex32(&artifact_hash));
    }

    let found = {
        let _stage = ui.stage("Match bundle hash");
        bundle_hash_in_export_receipts(&conn, &artifact_hash)?
    };
    if !found {
        return Err(anyhow!("TAMPER: bundle hash not found in export receipts"));
    }

    #[cfg(feature = "c2pa-export")]
    if let Some(manifest_path) = &args.c2pa_manifest {
        use witness_kernel::c2pa_export;
        let _stage = ui.stage("Verify C2PA sidecar");
        let manifest_bytes = std::fs::read(manifest_path)
            .map_err(|e| anyhow!("failed to read C2PA manifest {}: {}", manifest_path, e))?;
        let anchor_pem = match (&args.c2pa_anchor, args.device_key_seed.as_deref()) {
            (Some(path), _) => std::fs::read_to_string(path)
                .map_err(|e| anyhow!("failed to read C2PA anchor {}: {}", path, e))?,
            (None, Some(seed)) => c2pa_export::ca_anchor_from_seed(seed)?,
            (None, None) => {
                return Err(anyhow!(
                    "C2PA verification needs a trust anchor: pass --c2pa-anchor <PEM> \
                     or --device-key-seed to re-derive the device CA"
                ))
            }
        };
        let report =
            c2pa_export::verify_export_sidecar(&manifest_bytes, &bundle_bytes, &anchor_pem)?;
        println!(
            "c2pa: manifest valid, state {:?}{}",
            report.state,
            report
                .issuer
                .as_deref()
                .map(|i| format!(", issuer {i:?}"))
                .unwrap_or_default()
        );
        // A `Valid` (internally consistent, wrong root) manifest is an
        // attacker's manifest here: this tool was handed a specific anchor,
        // so anything not chaining to it is a forgery attempt, however
        // well-formed. Only `Trusted` passes.
        if report.state != c2pa_export::C2paValidationState::Trusted {
            return Err(anyhow!(
                "TAMPER: C2PA manifest signer does not chain to the supplied trust anchor \
                 (state {:?})",
                report.state
            ));
        }
        // The manifest must not merely be trusted — its witness binding has
        // to point at THIS disclosure. Sidecars only exist for full
        // ExportBundle files, and the binding must name exactly the receipt
        // entry embedded in this bundle (already proven above to be in the
        // chain checked above), with the artifact hash that receipt committed
        // to. Identity, not membership: a trusted manifest naming any *other*
        // receipt in that chain is a graft and fails.
        let expected_entry = bundle_entry_hash.ok_or_else(|| {
            anyhow!(
                "C2PA verification requires a full export bundle (this file is a bare \
                 artifact with no embedded receipt)"
            )
        })?;
        let binding = report.binding.ok_or_else(|| {
            anyhow!("TAMPER: C2PA manifest carries no org.securacv.witness assertion")
        })?;
        let bound_entry: [u8; 32] = hex::decode(&binding.receipt_entry_hash)
            .ok()
            .and_then(|v| v.try_into().ok())
            .ok_or_else(|| anyhow!("TAMPER: malformed receipt hash in witness assertion"))?;
        if bound_entry != expected_entry {
            return Err(anyhow!(
                "TAMPER: C2PA witness assertion names a different receipt entry than the \
                 one embedded in this bundle"
            ));
        }
        if binding.artifact_hash != verify_helpers::hex32(&artifact_hash) {
            return Err(anyhow!(
                "TAMPER: C2PA witness assertion artifact hash does not match the receipt"
            ));
        }
        // "Checked above", not "verified": unanchored, that chain was checked
        // against the database's own key, and the final line says so.
        println!("c2pa: witness binding matches this bundle's receipt in the chain checked above");
    }

    // Exit 0 on both paths, as log_verify: the label changes what the verdict
    // claims, not whether the checks passed.
    if identity_anchored {
        println!("OK: export bundle hash verified (identity anchored out-of-band).");
    } else {
        println!(
            "OK: export bundle hash matches the receipt chain — SELF-CONSISTENT, IDENTITY \
             UNVERIFIED. The verifying key was taken from the database under audit, so this \
             proves internal consistency, not identity. Supply --public-key / \
             --public-key-file / --device-key-seed from an out-of-band source for an \
             evidentiary verdict."
        );
    }
    Ok(())
}

fn bundle_hash_in_export_receipts(conn: &Connection, bundle_hash: &[u8; 32]) -> Result<bool> {
    let mut stmt = conn.prepare("SELECT payload_json FROM export_receipts ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    while let Some(row) = rows.next()? {
        let payload: String = row.get(0)?;
        let receipt: ExportReceipt = serde_json::from_str(&payload)?;
        if &receipt.artifact_hash == bundle_hash {
            return Ok(true);
        }
    }
    Ok(false)
}
