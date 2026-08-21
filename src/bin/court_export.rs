//! court_export - assemble a court-ready disclosure kit around an export bundle
//!
//! Takes an ALREADY-AUTHORIZED export bundle (written by `export_events`,
//! either auth mode) plus the kernel database, verifies the bundle end to end,
//! and assembles the package a lawyer actually files under FRE 902(13)/(14):
//! the evidence, its digests, a custody-and-control record rendered from the
//! signed receipts, every RFC 3161 anchor token that fixes it in time,
//! pre-filled certification drafts (28 U.S.C. § 1746 form), a plain-English
//! system description (the "silent witness" foundation), and verification
//! instructions runnable by opposing counsel with `sha256sum` and `openssl`
//! alone.
//!
//! This tool authorizes nothing and touches no raw media: it packages what an
//! authorized export already disclosed. It fails closed — a bundle that does
//! not verify, or whose receipt is not in this database's export-receipt
//! chain, is refused, never packaged. (spec/quorum_unseal_v2.md §5;
//! docs/security/PROVENANCE_INTEROP.md §1.1)

use anyhow::{anyhow, Result};
use clap::Parser;
use rusqlite::{Connection, OptionalExtension};
use sha2::{Digest, Sha256};
use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{
    hash_entry, tsa, verify_export_bundle, verify_helpers, ExportAuthMode, ExportBundle,
};

#[path = "../ui.rs"]
mod ui;

#[derive(Parser, Debug)]
#[command(author, version, about)]
#[command(after_help = "\
The kit is a deliberate disclosure: hand the whole directory to the receiving \
party. The certifications are DRAFTS for the person who performed the \
verification to review, complete, and sign — this tool provides engineering \
support, not legal advice.")]
struct Args {
    /// The export bundle JSON written by `export_events`.
    #[arg(long, value_name = "FILE")]
    bundle: String,
    /// Path to the witness database (source of receipts and anchors).
    #[arg(long, default_value = "witness.db")]
    db: String,
    /// Directory to assemble the kit into (created; must be absent or empty —
    /// the manifest attests every file in it).
    #[arg(long, value_name = "DIR")]
    output_dir: PathBuf,
    /// Device key seed — used only to derive the database encryption key
    /// (like log_verify). Not needed for an unencrypted database.
    #[arg(long, env = "DEVICE_KEY_SEED")]
    device_key_seed: Option<String>,
    /// Explicit SQLCipher key (hex), overriding the seed derivation.
    #[arg(
        long,
        env = "SECURACV_DB_KEY",
        conflicts_with = "device_key_seed",
        value_name = "HEX"
    )]
    db_key: Option<String>,
    /// UI mode for stderr progress (auto|plain|pretty)
    #[arg(long, default_value = "auto", value_name = "MODE")]
    ui: String,
}

/// One packaged anchor token, with what it proves.
struct KitAnchor {
    file_name: String,
    subject: String,
    gen_time: String,
    tsa_url: String,
    covers_bundle: bool,
}

fn open_db(args: &Args) -> Result<Connection> {
    // Mirrors log_verify: explicit --db-key wins; otherwise derive from the
    // device key seed exactly as the kernel does; otherwise open unkeyed.
    let db_key: Option<String> = match (&args.db_key, &args.device_key_seed) {
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
    // Read-only: this tool packages evidence; it must never mutate its
    // source, must work from a forensic copy on read-only media, and a
    // typo'd --db path must fail cleanly instead of creating an empty file.
    let conn = Connection::open_with_flags(
        &args.db,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .map_err(|e| anyhow!("could not open database {}: {}", args.db, e))?;
    if let Some(ref key) = db_key {
        conn.pragma_update(None, "key", format!("x'{}'", key))?;
    }
    conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
        .map_err(|_| {
            anyhow!(
                "could not read the kernel database — if it is encrypted, pass \
                 --device-key-seed (or --db-key)"
            )
        })?;
    Ok(conn)
}

/// Confirm the bundle's receipt entry is a genuine row of THIS database's
/// export-receipt chain, produced by THIS database's device identity, and that
/// the WHOLE chain verifies — every row's prev-hash link, entry hash, and
/// device signature, from genesis to head. A single-row re-derivation is not
/// enough: the custody record says "receipt N of M in the signed export
/// chain", and packaging that sentence over a ledger whose other rows are
/// broken would describe an intact chain that does not exist. The entry hash
/// alone is not enough either: the hashed payload carries no device identity,
/// so two devices exporting identical content in the same bucket produce
/// colliding entry hashes — the identity and signature checks below are what
/// tie the row to the bundle's signer. Returns the row's chain position
/// (1-based id) and the chain length.
fn locate_receipt_in_chain(conn: &Connection, bundle: &ExportBundle) -> Result<(i64, i64)> {
    // The database's pinned device identity must BE the bundle's signer.
    let db_device_key = witness_kernel::device_public_key_from_db(conn)
        .map_err(|e| anyhow!("could not read the database's device identity: {}", e))?;
    // The bundle stamps the device key CURRENT at export time; the database
    // pins the genesis anchor. After a key rotation those legitimately differ,
    // so custody is "the bundle's signer belongs to this database's validated
    // key lineage", not bytewise equality with genesis.
    let lineage_keys = verify_helpers::lineage_verifying_keys(conn, &db_device_key)?;
    if !lineage_keys
        .iter()
        .any(|key| key.to_bytes() == bundle.device_public_key)
    {
        return Err(anyhow!(
            "custody break: the bundle's signing key is not in this database's device key \
             lineage — the bundle was produced by a different device. Refusing to package."
        ));
    }

    // Full-chain verification under the database's pinned key. The PQ key is
    // taken from the database when stored (Compat mode: the PQ signature is
    // verified when a key is available, mandatory in neither).
    let pq_public_key = verify_helpers::load_pq_verifying_key(conn, None, None)?;
    let mut position: Option<i64> = None;
    let total = witness_kernel::verify::verify_export_receipts_with(
        conn,
        &lineage_keys,
        SignatureMode::Compat,
        pq_public_key.as_ref(),
        |id, entry_hash| {
            if entry_hash == bundle.receipt_entry.entry_hash {
                position = Some(id);
            }
        },
    )
    .map_err(|e| {
        anyhow!(
            "custody break: this database's export-receipt chain does not verify ({}). The \
             custody record must describe an intact chain — refusing to package.",
            e
        )
    })? as i64;
    let Some(id) = position else {
        return Err(anyhow!(
            "custody break: the bundle's export receipt is not present in this database's \
             export-receipt chain — this bundle did not come from this database, or the \
             chain has been altered. Refusing to package."
        ));
    };

    // Bind the stored row to the bundle's copy of the receipt entry.
    let row: Option<(String, Vec<u8>, Vec<u8>)> = conn
        .query_row(
            "SELECT payload_json, prev_hash, signature FROM export_receipts \
             WHERE id = ?1 LIMIT 1",
            rusqlite::params![id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .optional()?;
    let Some((payload_json, prev_hash, signature)) = row else {
        return Err(anyhow!(
            "custody break: the receipt row disappeared between verification and read"
        ));
    };
    if prev_hash.len() != 32 || prev_hash != bundle.receipt_entry.prev_hash.to_vec() {
        return Err(anyhow!(
            "custody break: the stored receipt's chain link does not match the bundle's"
        ));
    }
    let mut prev = [0u8; 32];
    prev.copy_from_slice(&prev_hash);
    if hash_entry(&prev, payload_json.as_bytes()) != bundle.receipt_entry.entry_hash {
        return Err(anyhow!(
            "custody break: the stored receipt bytes do not re-derive the bundle's entry hash"
        ));
    }
    // Ed25519 signing is deterministic (RFC 8032): the stored row's signature
    // over this entry hash must be byte-identical to the bundle's, proving the
    // same key signed both.
    if signature != bundle.receipt_entry.signatures.ed25519_signature {
        return Err(anyhow!(
            "custody break: the stored receipt's device signature differs from the bundle's"
        ));
    }
    Ok((id, total))
}

/// Copy every stored RFC 3161 anchor token into `anchors/`, marking which
/// tokens cover the exact bundle bytes (subject_hash == SHA-256(bundle file)).
/// Chain-head anchors are included too: they fix the chain that contains the
/// export receipt, which is the second, indirect leg of the timing proof.
fn package_anchors(
    conn: &Connection,
    kit_dir: &Path,
    bundle_sha256: &[u8; 32],
) -> Result<Vec<KitAnchor>> {
    // The database is opened read-only, so the anchors table cannot be
    // created here; a database that never anchored simply has no table.
    let table_exists: bool = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='tsa_anchors' LIMIT 1",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false);
    if !table_exists {
        return Ok(Vec::new());
    }
    let anchors = tsa::list_anchors(conn)?;
    // Include ONLY what proves something about THIS disclosure: tokens over
    // the bundle's exact bytes, and chain-head tokens (they fix the chain
    // that contains the export receipt). Digest anchors for OTHER exports
    // prove nothing here and would over-disclose that those exports exist,
    // their hashes, and when they were anchored — so they stay out.
    //
    // And a token only proves what IT embeds: the anchor row's subject_hash
    // is a database column, not the token. Before any token is packaged (or
    // counted as covering the bundle), its DER message imprint must parse and
    // equal the row's claimed subject hash — otherwise "anchored" would be
    // asserted on the strength of an unexamined blob. A chain-head anchor
    // must additionally reference a hash actually recorded in this database's
    // chain history, or it fixes nothing about THIS ledger.
    let mut relevant = Vec::new();
    for a in anchors {
        if !(&a.subject_hash == bundle_sha256 || a.subject == "chain_head") {
            continue;
        }
        match tsa::parse_token_imprint(&a.token_der) {
            Ok(imprint) if imprint == a.subject_hash => {}
            Ok(imprint) => {
                eprintln!(
                    "WARNING: anchor #{} excluded from the kit: its token's message imprint \
                     ({}) is not the digest the anchor row claims ({}). Run `log_anchor \
                     verify` on this database.",
                    a.id,
                    hex::encode(imprint),
                    hex::encode(a.subject_hash)
                );
                continue;
            }
            Err(e) => {
                eprintln!(
                    "WARNING: anchor #{} excluded from the kit: its stored token does not \
                     parse as an RFC 3161 timestamp token ({}). Run `log_anchor verify` on \
                     this database.",
                    a.id, e
                );
                continue;
            }
        }
        if a.subject == "chain_head"
            && &a.subject_hash != bundle_sha256
            && !tsa::hash_in_history(conn, &a.subject_hash)?
        {
            eprintln!(
                "WARNING: anchor #{} excluded from the kit: it claims to anchor a chain \
                 head, but the anchored hash is not in this database's chain history. Run \
                 `log_anchor verify` on this database.",
                a.id
            );
            continue;
        }
        relevant.push(a);
    }
    if relevant.is_empty() {
        return Ok(Vec::new());
    }
    let dir = kit_dir.join("anchors");
    std::fs::create_dir_all(&dir)?;
    let mut out = Vec::new();
    for anchor in relevant {
        let covers_bundle = &anchor.subject_hash == bundle_sha256;
        // The subject is database-supplied TEXT: sanitize before it becomes a
        // path component, so a tampered row cannot steer the write outside
        // `anchors/`.
        let safe_subject: String = anchor
            .subject
            .chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                    c
                } else {
                    '_'
                }
            })
            .take(32)
            .collect();
        let file_name = format!("anchor-{:04}-{}.tsr", anchor.id, safe_subject);
        std::fs::write(dir.join(&file_name), &anchor.token_der)?;
        out.push(KitAnchor {
            file_name,
            subject: anchor.subject,
            gen_time: anchor.gen_time,
            tsa_url: anchor.tsa_url,
            covers_bundle,
        });
    }
    Ok(out)
}

fn auth_mode_label(mode: Option<ExportAuthMode>) -> &'static str {
    match mode {
        Some(ExportAuthMode::BreakGlass) => {
            "break-glass (n-of-m trustee quorum authorized this disclosure)"
        }
        Some(ExportAuthMode::SelfExport) => {
            "self-export (device owner, authenticated by possession of the device key seed)"
        }
        Some(ExportAuthMode::Api) => "local capability-token API",
        None => "legacy receipt (predates the auth_mode field)",
    }
}

fn bucket_utc(start_epoch_s: u64) -> String {
    // Coarse, bucket-level rendering only: the kit is an external disclosure,
    // and event timing stays at the 10-minute granularity the receipts carry
    // (Invariant III). Minute precision — never seconds.
    let (y, m, d, hh, mm, _ss) = witness_kernel::epoch_civil_utc(start_epoch_s);
    format!("{:04}-{:02}-{:02} {:02}:{:02} UTC", y, m, d, hh, mm)
}

fn main() -> Result<()> {
    let args = Args::parse();
    let is_tty = std::io::stderr().is_terminal();
    let stdout_is_tty = std::io::stdout().is_terminal();
    let ui = ui::Ui::from_args(Some(&args.ui), is_tty, !stdout_is_tty);

    // ── 1. Read and VERIFY the bundle before anything is packaged ────────
    let bundle_bytes = {
        let _stage = ui.stage("Read export bundle");
        std::fs::read(&args.bundle)
            .map_err(|e| anyhow!("failed to read bundle {}: {}", args.bundle, e))?
    };
    let bundle_sha256: [u8; 32] = Sha256::digest(&bundle_bytes).into();
    let bundle: ExportBundle = serde_json::from_slice(&bundle_bytes)
        .map_err(|e| anyhow!("not an export bundle ({}): {}", args.bundle, e))?;
    {
        let _stage = ui.stage("Verify bundle");
        verify_export_bundle(&bundle).map_err(|e| {
            anyhow!(
                "the bundle does not verify ({}). A court kit must only ever package \
                 verified evidence — refusing.",
                e
            )
        })?;
    }

    // ── 2. Custody: the receipt must be a real row of this chain ─────────
    let conn = {
        let _stage = ui.stage("Open database");
        open_db(&args)?
    };
    let (chain_position, chain_length) = {
        let _stage = ui.stage("Locate receipt in chain");
        locate_receipt_in_chain(&conn, &bundle)?
    };

    // ── 3. Assemble the kit ──────────────────────────────────────────────
    // The output directory must be fresh: MANIFEST.json attests every file in
    // the kit, so a pre-existing file (a stale anchor token, a leftover
    // evidence file, anything) would sit inside the directory the manifest
    // claims to describe without being listed in it. Empty-or-absent only.
    let kit = &args.output_dir;
    if kit.exists() {
        if !kit.is_dir() {
            return Err(anyhow!(
                "{} exists and is not a directory — pick a fresh directory",
                kit.display()
            ));
        }
        if std::fs::read_dir(kit)?.next().is_some() {
            return Err(anyhow!(
                "{} already exists and is not empty — the kit manifest attests every file \
                 in the directory, so the kit must be assembled into a fresh (or empty) one",
                kit.display()
            ));
        }
    }
    std::fs::create_dir_all(kit.join("evidence"))?;

    let bundle_file_name = Path::new(&args.bundle)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("export_bundle.json")
        .to_string();
    std::fs::write(kit.join("evidence").join(&bundle_file_name), &bundle_bytes)?;

    // A C2PA sidecar beside the input bundle travels with it: it signs these
    // exact bytes and is meaningless apart from them.
    let sidecar_src = PathBuf::from(format!("{}.c2pa", args.bundle));
    let sidecar_name = if sidecar_src.exists() {
        let name = format!("{}.c2pa", bundle_file_name);
        std::fs::copy(&sidecar_src, kit.join("evidence").join(&name))?;
        Some(name)
    } else {
        None
    };

    let anchors = {
        let _stage = ui.stage("Package anchor tokens");
        package_anchors(&conn, kit, &bundle_sha256)?
    };
    let digest_anchors: Vec<&KitAnchor> = anchors.iter().filter(|a| a.covers_bundle).collect();
    let anchored = !digest_anchors.is_empty();

    let receipt = &bundle.receipt_entry.receipt;
    let bundle_hex = hex::encode(bundle_sha256);
    let artifact_hash_hex = hex::encode(receipt.artifact_hash);
    let entry_hash_hex = hex::encode(bundle.receipt_entry.entry_hash);
    let device_key_hex = hex::encode(bundle.device_public_key);
    let ruleset_hash_hex = hex::encode(receipt.ruleset_hash);
    let created = bucket_utc(receipt.time_bucket.start_epoch_s);
    let window_text = match &receipt.window {
        Some(w) => format!(
            "{} .. {} (bucket-aligned)",
            bucket_utc(w.start_epoch_s),
            bucket_utc(w.end_epoch_s)
        ),
        None => "full retained history at export time".to_string(),
    };
    let event_count: usize = bundle
        .artifact
        .batches
        .iter()
        .flat_map(|b| b.buckets.iter())
        .map(|bucket| bucket.events.len())
        .sum();

    {
        let _stage = ui.stage("Write kit documents");
        write_readme(kit, &bundle_file_name, anchored)?;
        write_system_description(kit)?;
        write_custody_record(
            kit,
            &bundle_file_name,
            &bundle_hex,
            &artifact_hash_hex,
            &entry_hash_hex,
            &device_key_hex,
            &ruleset_hash_hex,
            &created,
            &window_text,
            event_count,
            auth_mode_label(receipt.auth_mode),
            chain_position,
            chain_length,
            &anchors,
        )?;
        write_verification(kit, &bundle_file_name, &bundle_hex, &anchors)?;
        write_certifications(kit, &bundle_file_name, &bundle_hex, &artifact_hash_hex)?;
        write_manifest(
            kit,
            &bundle_file_name,
            sidecar_name.as_deref(),
            &bundle_hex,
            &artifact_hash_hex,
            &entry_hash_hex,
            &device_key_hex,
            receipt.auth_mode,
            anchored,
            &anchors,
        )?;
    }

    println!("Court disclosure kit assembled: {}", kit.display());
    println!("  evidence:  evidence/{}", bundle_file_name);
    println!("  digest:    sha256 {}", bundle_hex);
    println!(
        "  custody:   receipt {} of {} in the signed export chain",
        chain_position, chain_length
    );
    println!(
        "  anchors:   {} token(s), {} covering the bundle bytes",
        anchors.len(),
        digest_anchors.len()
    );
    if !anchored {
        println!();
        println!(
            "WARNING: no RFC 3161 anchor covers this bundle's bytes — its timing rests on \
             the device clock and key alone. Anchor it now, then re-assemble:"
        );
        println!(
            "  log_anchor request --db {} --url https://freetsa.org/tsr --digest {}",
            args.db, bundle_hex
        );
        if args.db_key.is_some() || args.device_key_seed.is_some() {
            println!(
                "  (encrypted database: log_anchor accepts the same --device-key-seed / \
                 --db-key flags and environment variables used here)"
            );
        }
    }
    Ok(())
}

fn write_readme(kit: &Path, bundle_file_name: &str, anchored: bool) -> Result<()> {
    let anchor_note = if anchored {
        "RFC 3161 timestamp tokens in `anchors/` fix this exact file in time; \
         `VERIFICATION.md` shows how to check them with openssl."
    } else {
        "NOTE: no timestamp token in this kit covers the evidence file itself — see the \
         warning in `VERIFICATION.md`."
    };
    let text = format!(
        "# SecuraCV court disclosure kit\n\
         \n\
         This directory is a self-contained disclosure package assembled by \
         `court_export`. Reading order:\n\
         \n\
         1. `SYSTEM_DESCRIPTION.md` — what produced this evidence and why its output \
         is accurate (the FRE 901(b)(9) / \"silent witness\" foundation).\n\
         2. `CUSTODY_AND_CONTROL.md` — the item, its digests, its authorization, and \
         its position in the signed, hash-chained receipt ledger.\n\
         3. `VERIFICATION.md` — steps any party can run with `sha256sum` and \
         `openssl` alone; no SecuraCV software required for the trust-critical checks.\n\
         4. `CERTIFICATION_FRE_902_13.md` / `CERTIFICATION_FRE_902_14.md` — draft \
         self-authentication certifications for the person who performed the \
         verification to review, complete, and sign.\n\
         5. `evidence/{bundle}` — the evidence itself: the privacy-filtered event \
         artifact and its signed export receipt, exactly as exported.\n\
         6. `MANIFEST.json` — machine-readable list of every file here with its \
         SHA-256.\n\
         \n\
         {anchor_note}\n\
         \n\
         Under FRE 902(11), the proponent must give the adverse party reasonable \
         written notice of the intent to offer the record and make it available for \
         inspection — hand over this entire directory.\n\
         \n\
         The certifications are drafts prepared by software; review them with \
         counsel. Nothing in this kit is legal advice.\n",
        bundle = bundle_file_name,
        anchor_note = anchor_note,
    );
    std::fs::write(kit.join("README.md"), text)?;
    Ok(())
}

fn write_system_description(kit: &Path) -> Result<()> {
    let text = format!(
        "# System description (for the record)\n\
         \n\
         Produced by SecuraCV witness-kernel version {version}. This document \
         describes, in plain language, the process that produced the evidence in \
         this kit — the foundation FRE 901(b)(9) asks for: evidence describing a \
         process or system and showing that it produces an accurate result.\n\
         \n\
         ## What the system records\n\
         \n\
         SecuraCV is witness infrastructure: it turns camera and sensor input into \
         semantic event records (for example, \"person present in zone A during a \
         ten-minute window\"). The exported artifact contains those event records \
         only. By construction it contains no video frames, no audio, no faces, no \
         license plates, and no biometric identifiers; event times are deliberately \
         coarsened to ten-minute buckets. These are structural properties of the \
         software (the recording pipeline has no code path that exports raw media \
         without a separate, quorum-authorized process), not configuration options.\n\
         \n\
         ## Why the records are accurate and tamper-evident\n\
         \n\
         1. **Append-only signed chain.** Every event record is appended to a \
         hash-chained ledger: each entry stores the SHA-256 hash of the previous \
         entry plus its own content, and is signed with the device's Ed25519 \
         private key. Altering, reordering, or deleting any interior record breaks \
         the chain arithmetic and the signatures, which the verification tools \
         detect.\n\
         2. **Signed export receipts.** Every disclosure (including this one) \
         appends a signed receipt to a second hash-chained ledger, recording what \
         was exported, when (to the ten-minute bucket), under what authorization, \
         and the SHA-256 hash of the exported artifact. The receipt for this \
         disclosure travels inside the evidence file itself.\n\
         3. **Independent timestamping.** The operator can anchor chain state and \
         exported files at public RFC 3161 Time Stamping Authorities. A timestamp \
         token is a countersignature by a third party over a SHA-256 hash: it \
         proves the hashed bytes existed no later than the token's time, on the \
         authority of a party that is not the device, the operator, or the vendor.\n\
         4. **Independent verification.** The hash and signature checks are \
         verifiable with standard tools (`sha256sum`, `openssl`); SecuraCV also \
         ships two independent verifier implementations (Rust and JavaScript) that \
         must agree, so no single implementation is trusted.\n\
         \n\
         ## Honest limits\n\
         \n\
         - \"Verified\" in this kit means: an Ed25519 signature checked against a \
         stated public key, and SHA-256 hashes recomputed and compared. It does not \
         mean any assessment of what the events depict.\n\
         - Event records are the output of detection software operating on sensor \
         input; the ruleset version that produced them is permanently recorded in \
         the receipt (`ruleset_hash`) so its documentation can be examined.\n\
         - Timestamp tokens bound timing from above (\"existed no later than\"); \
         records after the most recent anchor are bounded only by the device clock \
         and key.\n",
        version = env!("CARGO_PKG_VERSION"),
    );
    std::fs::write(kit.join("SYSTEM_DESCRIPTION.md"), text)?;
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn write_custody_record(
    kit: &Path,
    bundle_file_name: &str,
    bundle_hex: &str,
    artifact_hash_hex: &str,
    entry_hash_hex: &str,
    device_key_hex: &str,
    ruleset_hash_hex: &str,
    created: &str,
    window_text: &str,
    event_count: usize,
    auth_label: &str,
    chain_position: i64,
    chain_length: i64,
    anchors: &[KitAnchor],
) -> Result<()> {
    let mut anchor_rows = String::new();
    if anchors.is_empty() {
        anchor_rows.push_str("(none recorded)\n");
    } else {
        for a in anchors {
            anchor_rows.push_str(&format!(
                "- `anchors/{}` — subject `{}`, issued {} by {}{}\n",
                a.file_name,
                a.subject,
                a.gen_time,
                a.tsa_url,
                if a.covers_bundle {
                    " — **covers the evidence file's exact bytes**"
                } else {
                    ""
                }
            ));
        }
    }
    let text = format!(
        "# Custody and control record\n\
         \n\
         Rendered from the signed, hash-chained ledgers of the producing device. \
         The receipt described here is embedded, signed, inside the evidence file; \
         this document is a human-readable rendering, not a separate authority.\n\
         \n\
         ## Item\n\
         \n\
         | Field | Value |\n\
         |---|---|\n\
         | Evidence file | `evidence/{bundle}` |\n\
         | SHA-256 (whole file) | `{bundle_hex}` |\n\
         | SHA-256 (event artifact, as committed in the receipt) | `{artifact}` |\n\
         | Event records disclosed | {events} |\n\
         | Disclosure window | {window} |\n\
         \n\
         ## Creation and authorization\n\
         \n\
         | Field | Value |\n\
         |---|---|\n\
         | Export created (10-minute bucket) | {created} |\n\
         | Authorization | {auth} |\n\
         | Ruleset in force (hash) | `{ruleset}` |\n\
         | Device public key (Ed25519) | `{device_key}` |\n\
         \n\
         ## Chain of custody (digital)\n\
         \n\
         | Field | Value |\n\
         |---|---|\n\
         | Export receipt entry hash | `{entry_hash}` |\n\
         | Position in signed export-receipt chain | {pos} of {len} |\n\
         \n\
         The export receipt is hash-linked to every receipt before it and signed by \
         the device key; the chain re-derivation performed at packaging time \
         confirmed the stored bytes reproduce this entry hash. Full-chain \
         verification is available to any party via `log_verify` (see \
         `VERIFICATION.md`).\n\
         \n\
         ## Independent timestamp anchors\n\
         \n\
         {anchors}\n\
         \n\
         ## Handling\n\
         \n\
         Digital custody transfers by hash, not by possession: any recipient \
         re-computes the SHA-256 of `evidence/{bundle}` and compares it to the \
         value above. A match means the evidence is bit-for-bit the item this \
         record describes, regardless of how many hands the files passed through.\n",
        bundle = bundle_file_name,
        bundle_hex = bundle_hex,
        artifact = artifact_hash_hex,
        events = event_count,
        window = window_text,
        created = created,
        auth = auth_label,
        ruleset = ruleset_hash_hex,
        device_key = device_key_hex,
        entry_hash = entry_hash_hex,
        pos = chain_position,
        len = chain_length,
        anchors = anchor_rows,
    );
    std::fs::write(kit.join("CUSTODY_AND_CONTROL.md"), text)?;
    Ok(())
}

fn write_verification(
    kit: &Path,
    bundle_file_name: &str,
    bundle_hex: &str,
    anchors: &[KitAnchor],
) -> Result<()> {
    let mut anchor_steps = String::new();
    let covering: Vec<&KitAnchor> = anchors.iter().filter(|a| a.covers_bundle).collect();
    if covering.is_empty() {
        anchor_steps.push_str(
            "**No timestamp token in this kit covers the evidence file itself.** The \
             chain-head tokens under `anchors/` (if any) fix the producing device's \
             ledger state in time, but verifying them against this evidence requires \
             the SecuraCV tooling path in step 4. Timing for the evidence file \
             otherwise rests on the device's own clock and key.\n",
        );
    } else {
        for a in &covering {
            anchor_steps.push_str(&format!(
                "```sh\n\
                 # Token issued {} by {}\n\
                 openssl ts -verify -digest {} -in anchors/{} -CAfile <tsa-ca.pem>\n\
                 ```\n\
                 A `Verification: OK` result is the TSA's countersignature that these \
                 exact bytes existed no later than the token's time. Obtain \
                 `<tsa-ca.pem>` from the TSA's published CA certificate, not from this \
                 kit.\n\n",
                a.gen_time, a.tsa_url, bundle_hex, a.file_name
            ));
        }
    }
    let text = format!(
        "# Verification instructions\n\
         \n\
         Steps 1-3 need only `sha256sum` and `openssl` — no SecuraCV software.\n\
         \n\
         ## 1. Confirm the manifest\n\
         \n\
         For every file listed in `MANIFEST.json`, recompute and compare:\n\
         \n\
         ```sh\n\
         sha256sum evidence/{bundle}\n\
         # expect: {bundle_hex}\n\
         ```\n\
         \n\
         ## 2. Confirm the evidence digest\n\
         \n\
         The SHA-256 above is the identity of the evidence. If it matches, the file \
         is bit-for-bit the item described in `CUSTODY_AND_CONTROL.md`.\n\
         \n\
         ## 3. Verify the timestamp anchors\n\
         \n\
         {anchor_steps}\
         \n\
         ## 4. Deeper verification (optional, SecuraCV tooling)\n\
         \n\
         The evidence file carries its signed export receipt and the device \
         public key, so the open-source SecuraCV tools can re-check the receipt \
         signature, the artifact hash, and every ledger end to end. These checks \
         run at the PRODUCING side — they need the producing database, which is \
         not part of this kit: `export_verify` cross-checks the bundle against \
         that database, and `log_verify` re-derives its chains. A party without \
         database access relies on steps 1-3 (which are the trust-critical \
         checks) or requests supervised verification. Two independent verifier \
         implementations (Rust and JavaScript) exist so no single \
         implementation needs to be trusted.\n",
        bundle = bundle_file_name,
        bundle_hex = bundle_hex,
        anchor_steps = anchor_steps,
    );
    std::fs::write(kit.join("VERIFICATION.md"), text)?;
    Ok(())
}

fn write_certifications(
    kit: &Path,
    bundle_file_name: &str,
    bundle_hex: &str,
    artifact_hash_hex: &str,
) -> Result<()> {
    let c13 = format!(
        "# Draft certification — FRE 902(13)\n\
         ## Certification of a record generated by an electronic process or system\n\
         \n\
         > DRAFT prepared by software for review by the certifier and counsel.\n\
         \n\
         I, [NAME], declare as follows:\n\
         \n\
         1. I am [ROLE — e.g., the owner and operator of the SecuraCV witness \
         system that produced the attached record]. I am familiar with how the \
         system records, stores, and exports event records, and with the process \
         described in the accompanying System Description.\n\
         2. The file `evidence/{bundle}` was generated by that system's electronic \
         process, which produces an accurate result: every record is signed and \
         hash-chained at creation, and every export is recorded in a signed \
         receipt embedded in the exported file itself.\n\
         3. The record was produced in the ordinary operation of the system, by a \
         process described in `SYSTEM_DESCRIPTION.md`, which I reviewed and which \
         accurately describes the system as operated.\n\
         4. I verified the record as described in `VERIFICATION.md` on [DATE], \
         and the checks succeeded.\n\
         \n\
         I declare under penalty of perjury that the foregoing is true and \
         correct. Executed on [DATE].\n\
         \n\
         [SIGNATURE]\n\
         [NAME, PRINTED]\n",
        bundle = bundle_file_name,
    );
    let c14 = format!(
        "# Draft certification — FRE 902(14)\n\
         ## Certification of data copied from an electronic device, storage medium, or file\n\
         \n\
         > DRAFT prepared by software for review by the certifier and counsel.\n\
         \n\
         I, [NAME], declare as follows:\n\
         \n\
         1. I am [ROLE]. I am familiar with the process of digital identification \
         described below and performed the verification personally.\n\
         2. The file `evidence/{bundle}` is a true and accurate copy of the export \
         produced by the SecuraCV witness system, authenticated by a process of \
         digital identification: I computed the SHA-256 hash value of the copy and \
         compared it to the hash value of the original export.\n\
         3. The SHA-256 hash value of the file is:\n\
         \n\
         `{bundle_hex}`\n\
         \n\
         and the SHA-256 hash value of the event artifact committed in the \
         export's signed receipt is:\n\
         \n\
         `{artifact}`\n\
         \n\
         The values matched. SHA-256 is an industry-standard cryptographic hash \
         function; two files with the same SHA-256 value are identical for all \
         practical purposes.\n\
         4. The verification steps and their results are recorded in \
         `VERIFICATION.md`, performed on [DATE].\n\
         \n\
         I declare under penalty of perjury that the foregoing is true and \
         correct. Executed on [DATE].\n\
         \n\
         [SIGNATURE]\n\
         [NAME, PRINTED]\n",
        bundle = bundle_file_name,
        bundle_hex = bundle_hex,
        artifact = artifact_hash_hex,
    );
    std::fs::write(kit.join("CERTIFICATION_FRE_902_13.md"), c13)?;
    std::fs::write(kit.join("CERTIFICATION_FRE_902_14.md"), c14)?;
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn write_manifest(
    kit: &Path,
    bundle_file_name: &str,
    sidecar_name: Option<&str>,
    bundle_hex: &str,
    artifact_hash_hex: &str,
    entry_hash_hex: &str,
    device_key_hex: &str,
    auth_mode: Option<ExportAuthMode>,
    anchored: bool,
    anchors: &[KitAnchor],
) -> Result<()> {
    // Enumerate every file the kit contains (the manifest itself excluded) and
    // hash the bytes actually on disk — the manifest attests the kit as built.
    let mut files = Vec::new();
    let mut paths: Vec<PathBuf> = Vec::new();
    for name in [
        "README.md",
        "SYSTEM_DESCRIPTION.md",
        "CUSTODY_AND_CONTROL.md",
        "VERIFICATION.md",
        "CERTIFICATION_FRE_902_13.md",
        "CERTIFICATION_FRE_902_14.md",
    ] {
        paths.push(kit.join(name));
    }
    paths.push(kit.join("evidence").join(bundle_file_name));
    if let Some(name) = sidecar_name {
        paths.push(kit.join("evidence").join(name));
    }
    for a in anchors {
        paths.push(kit.join("anchors").join(&a.file_name));
    }
    for path in paths {
        let bytes = std::fs::read(&path)?;
        let digest: [u8; 32] = Sha256::digest(&bytes).into();
        let rel = path
            .strip_prefix(kit)
            .unwrap_or(&path)
            .to_string_lossy()
            .replace('\\', "/");
        files.push(serde_json::json!({
            "path": rel,
            "sha256": hex::encode(digest),
            "bytes": bytes.len(),
        }));
    }
    let manifest = serde_json::json!({
        "format": "securacv-court-kit:v1",
        "created_bucket_start": witness_kernel::TimeBucket::now_10min()?.start_epoch_s,
        "tool_version": env!("CARGO_PKG_VERSION"),
        "evidence": {
            "file": format!("evidence/{}", bundle_file_name),
            "sha256": bundle_hex,
            "artifact_hash": artifact_hash_hex,
            "receipt_entry_hash": entry_hash_hex,
            "device_public_key": device_key_hex,
            "auth_mode": auth_mode,
        },
        "anchored": anchored,
        "anchor_count": anchors.len(),
        "files": files,
    });
    std::fs::write(
        kit.join("MANIFEST.json"),
        format!("{}\n", serde_json::to_string_pretty(&manifest)?),
    )?;
    Ok(())
}
