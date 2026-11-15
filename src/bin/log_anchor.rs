//! log_anchor - Anchor the witness ledgers to an external RFC 3161 TSA.
//!
//! Each anchor is a Time Stamping Authority's countersignature over a
//! ledger head hash: independent, third-party proof that the ledger existed
//! in that state at a point in time. A verifier with an anchor no longer
//! needs to trust the device clock or device key alone — history recorded
//! before an anchor cannot be silently rewritten afterwards, even by someone
//! holding the device key.
//!
//! Anchoring is always operator-initiated (SecuraCV never makes outbound
//! network calls on its own) and the request carries only a 32-byte hash
//! plus a random nonce. Three flows:
//!
//!   online     log_anchor request --url https://...        (needs `--features tsa`)
//!   scheduled  log_anchor anchor-all --policy anchor-policy.json
//!   offline    log_anchor query --out chain.tsq   → submit out-of-band →
//!              log_anchor import --response chain.tsr
//!              (or anchor-all --policy … --offline-dir out/ for every subject)
//!
//! Subjects: `chain_head` (default), `export_receipt_head`,
//! `break_glass_receipt_head`, `policy_head`, `clock_provenance_head`, or a
//! bare `digest`
//! (`--digest HEX` / `--file PATH`).
//!
//! `log_anchor verify` re-checks every stored anchor structurally (imprint
//! belongs to the declared ledger's recorded history) and, given the TSA's
//! CA certificate (`--ca`, repeatable) or an anchor policy (`--policy`),
//! delegates the CMS countersignature check to an independent
//! implementation: `openssl ts -verify`. Under `--policy` it also reports
//! whether each ledger head is covered by the declared number of distinct
//! TSAs in the declared roles — roles are the operator's declarations, never
//! a legal finding.
//!
//! `verify` also reads the clock-provenance ledger `witnessd` seals (one
//! device-signed record per anchor row: the device clock measured against
//! the token's genTime, bound by the token's hash) and prints, under every
//! anchor, what that record says — with the ClockSkew correlation and the
//! structural seal-before-genTime check. It holds no key and writes nothing;
//! `docs/design/clock_provenance.md`.
//!
//! `list`, `verify`, `query` and `anchor-all --offline-dir` open the
//! database read-only and never create the anchors table (an observer
//! writes nothing); `request`, `import`, online `anchor-all` and `relabel`
//! open read-write.

use anyhow::{anyhow, bail, Context, Result};
use clap::{ArgAction, Parser, Subcommand};
use rusqlite::{Connection, OptionalExtension};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use witness_kernel::anchor_policy::{self, AnchorPolicy, Attribution, CoverageVerdict, Verified};
use witness_kernel::clock_provenance::{self, ClockProvenanceRow};
use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::ots::{self, OtsLegEvent, OtsLegReport, PlannedOts};
use witness_kernel::tsa::{self, AnchorRecord, AnchorSubject};
use witness_kernel::TimeBucket;
use witness_kernel::{verify, verify_helpers};

#[derive(Parser, Debug)]
#[command(
    name = "log_anchor",
    about = "Anchor the sealed witness chain to an RFC 3161 timestamping authority"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db", global = true)]
    db: String,

    /// Device key seed — used only to derive the database encryption key
    /// (like log_verify). Not needed for an unencrypted database.
    #[arg(long, env = "DEVICE_KEY_SEED", hide_env_values = true, global = true)]
    device_key_seed: Option<String>,

    /// Explicit SQLCipher key (hex), overriding the seed derivation.
    #[arg(
        long,
        env = "SECURACV_DB_KEY",
        hide_env_values = true,
        conflicts_with = "device_key_seed",
        global = true,
        value_name = "HEX"
    )]
    db_key: Option<String>,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Request a timestamp for a ledger head (or a digest) from one or more
    /// TSAs over HTTPS
    Request {
        /// TSA endpoint, e.g. <https://freetsa.org/tsr> (repeatable)
        #[arg(long, action = ArgAction::Append, required_unless_present = "policy", conflicts_with = "policy")]
        url: Vec<String>,
        /// Permit a plaintext http:// TSA endpoint (token integrity does not
        /// depend on transport, but https is required by default)
        #[arg(long)]
        allow_http: bool,
        /// Anchor policy file (securacv-anchor-policy:v1): request from every
        /// TSA it declares, checking each token under its declared CA
        #[arg(long, value_name = "FILE")]
        policy: Option<PathBuf>,
        /// Ledger head to anchor: chain_head (default), export_receipt_head,
        /// break_glass_receipt_head, policy_head, clock_provenance_head
        #[arg(long, value_name = "S", conflicts_with_all = ["digest", "file"])]
        subject: Option<String>,
        /// Anchor an explicit hex SHA-256 digest (e.g. an export bundle
        /// digest) instead of a ledger head
        #[arg(long, value_name = "HEX", conflicts_with = "file")]
        digest: Option<String>,
        /// Anchor the SHA-256 of this file's bytes (e.g. an export bundle)
        #[arg(long, value_name = "PATH")]
        file: Option<PathBuf>,
    },
    /// Write a DER timestamp query for a ledger head to a file (offline flow)
    Query {
        /// Output path for the .tsq request
        #[arg(long, value_name = "PATH")]
        out: String,
        /// Ledger head to anchor: chain_head (default), export_receipt_head,
        /// break_glass_receipt_head, policy_head, clock_provenance_head
        #[arg(long, value_name = "S", conflicts_with_all = ["digest", "file"])]
        subject: Option<String>,
        /// Anchor an explicit hex SHA-256 digest instead of a ledger head
        #[arg(long, value_name = "HEX", conflicts_with = "file")]
        digest: Option<String>,
        /// Anchor the SHA-256 of this file's bytes
        #[arg(long, value_name = "PATH")]
        file: Option<PathBuf>,
        /// Anchor policy file: when it has an `ots` block, also write one
        /// OpenTimestamps request (`{stem}.{calendar}.ots-request.json` and
        /// `.ots-commitment`) per declared calendar beside --out
        #[arg(long, value_name = "FILE")]
        policy: Option<PathBuf>,
    },
    /// Import and store TSA responses (and, with --ots-dir, OpenTimestamps
    /// calendar responses) obtained out-of-band
    Import {
        /// Path to a DER .tsr response (repeatable; one `--response` also
        /// takes several paths, so a shell glob like `out/*.tsr` works)
        #[arg(long, value_name = "PATH", action = ArgAction::Append, num_args = 1.., required_unless_present = "ots_dir")]
        response: Vec<String>,
        /// Directory written by `anchor-all --offline-dir` (or `query
        /// --policy`): import every `*.ots-request.json` with its `.otsr`
        /// calendar response and every `ots-*.ots-upgrade.json` with its
        /// `ots-{id}.otsr`
        #[arg(long, value_name = "DIR")]
        ots_dir: Option<PathBuf>,
        /// TSA URL to record alongside the anchor (provenance only; default
        /// "(offline)", or the policy entry's url under --policy)
        #[arg(long, value_name = "TEXT")]
        url: Option<String>,
        /// Anchor policy file: record the TSA's declared name and check the
        /// token under its declared CA before storing
        #[arg(long, value_name = "FILE")]
        policy: Option<PathBuf>,
        /// The policy entry that issued the response(s); inferred from a
        /// `{subject}.{name}.tsr` file name when omitted
        #[arg(long, value_name = "NAME", requires = "policy")]
        tsa: Option<String>,
    },
    /// Anchor every ledger head the policy names at every TSA it declares
    /// (a constant number of requests per run), or write the query files
    /// for the offline flow
    AnchorAll {
        /// Anchor policy file (securacv-anchor-policy:v1)
        #[arg(long, value_name = "FILE")]
        policy: PathBuf,
        /// Permit plaintext http:// TSA endpoints
        #[arg(long)]
        allow_http: bool,
        /// Write one .tsq per subject into this directory instead of
        /// submitting online (read-only; the database is not modified)
        #[arg(long, value_name = "DIR")]
        offline_dir: Option<PathBuf>,
    },
    /// List stored anchors
    List,
    /// Verify stored anchors against ledger history and, with --ca or
    /// --policy, run the full openssl countersignature check
    Verify {
        /// TSA CA certificate (PEM); enables `openssl ts -verify` (repeatable)
        #[arg(long, value_name = "PATH", action = ArgAction::Append, conflicts_with = "policy")]
        ca: Vec<String>,
        /// Anchor policy file: check every row under each TSA's declared CA
        /// and report per-subject coverage
        #[arg(long, value_name = "FILE")]
        policy: Option<PathBuf>,
        /// Fail unless every covered head is the ledger's current head
        /// (the RFC 3161 leg only; Bitcoin confirmation lags by design)
        #[arg(long, requires = "policy")]
        require_current: bool,
        /// Bitcoin block headers, one per line: `<height> <160 hex of the
        /// 80-byte header>`, from a node you run. The only Bitcoin source
        /// verify reads; it never contacts the network
        #[arg(long, value_name = "FILE")]
        bitcoin_headers: Option<PathBuf>,
    },
    /// Downgrade a ledger-head anchor whose head was pruned before this
    /// release to a digest anchor (the token is untouched)
    Relabel {
        /// The anchor row id
        #[arg(long, value_name = "N")]
        id: i64,
        /// The new subject; only `digest` is accepted
        #[arg(long, value_name = "S")]
        subject: String,
        /// Relabel an OpenTimestamps row (`ots_anchors`) instead of an RFC
        /// 3161 row
        #[arg(long)]
        ots: bool,
    },
}

/// How a verb opens the database. Observers (`list`, `verify`, `query`,
/// `anchor-all --offline-dir`) open READ-ONLY at the SQLite level and never
/// create the anchors table; only verbs that store rows open read-write.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum OpenMode {
    ReadOnly,
    ReadWrite,
}

fn open(args: &Args, mode: OpenMode) -> Result<Connection> {
    // SQLCipher key: explicit --db-key wins; otherwise derive it from the
    // device key seed exactly as the kernel does (same logic as log_verify);
    // with neither flag, the seed file beside the database is tried
    // (database key only).
    let seed_for_db_key: Option<String> = match (&args.db_key, &args.device_key_seed) {
        (Some(_), _) => None,
        (None, Some(seed)) => Some(seed.clone()),
        (None, None) => witness_kernel::crypto::find_device_seed(&args.db, None)?.map(|found| {
            eprintln!("log_anchor: database key from the {}", found.source);
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
    let conn = match mode {
        OpenMode::ReadOnly => Connection::open_with_flags(
            &args.db,
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY
                | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX
                | rusqlite::OpenFlags::SQLITE_OPEN_URI,
        )
        .map_err(|e| anyhow!("could not open {} read-only: {}", args.db, e))?,
        OpenMode::ReadWrite => Connection::open(&args.db)
            .with_context(|| format!("opening witness DB '{}'", args.db))?,
    };
    if let Some(ref key) = db_key {
        conn.pragma_update(None, "key", format!("x'{}'", key))?;
    }
    conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
        .map_err(|_| {
            anyhow!(
                "could not read the witness DB — if it is encrypted, pass \
                 --device-key-seed (or --db-key)"
            )
        })?;
    Ok(conn)
}

fn main() -> Result<()> {
    let args = Args::parse();
    match &args.command {
        Command::Request {
            url,
            allow_http,
            policy,
            subject,
            digest,
            file,
        } => {
            let conn = open(&args, OpenMode::ReadWrite)?;
            tsa::ensure_anchor_table(&conn)?;
            request(
                &conn,
                url,
                *allow_http,
                policy.as_deref(),
                subject.as_deref(),
                digest.as_deref(),
                file.as_deref(),
            )
        }
        Command::Query {
            out,
            subject,
            digest,
            file,
            policy,
        } => {
            let conn = open(&args, OpenMode::ReadOnly)?;
            query(
                &conn,
                out,
                subject.as_deref(),
                digest.as_deref(),
                file.as_deref(),
                policy.as_deref(),
            )
        }
        Command::Import {
            response,
            ots_dir,
            url,
            policy,
            tsa,
        } => {
            let conn = open(&args, OpenMode::ReadWrite)?;
            if !response.is_empty() {
                tsa::ensure_anchor_table(&conn)?;
            }
            import(
                &conn,
                response,
                ots_dir.as_deref(),
                url.as_deref(),
                policy.as_deref(),
                tsa.as_deref(),
            )
        }
        Command::AnchorAll {
            policy,
            allow_http,
            offline_dir,
        } => match offline_dir {
            Some(dir) => {
                let conn = open(&args, OpenMode::ReadOnly)?;
                anchor_all_offline(&conn, &args.db, policy, dir)
            }
            None => {
                let conn = open(&args, OpenMode::ReadWrite)?;
                tsa::ensure_anchor_table(&conn)?;
                anchor_all_online(&conn, policy, *allow_http)
            }
        },
        Command::List => {
            let conn = open(&args, OpenMode::ReadOnly)?;
            list(&conn)
        }
        Command::Verify {
            ca,
            policy,
            require_current,
            bitcoin_headers,
        } => {
            let conn = open(&args, OpenMode::ReadOnly)?;
            verify(
                &conn,
                &args.db,
                ca,
                policy.as_deref(),
                *require_current,
                bitcoin_headers.as_deref(),
            )
        }
        Command::Relabel { id, subject, ots } => {
            let conn = open(&args, OpenMode::ReadWrite)?;
            if *ots {
                relabel_ots(&conn, *id, subject)
            } else {
                tsa::ensure_anchor_table(&conn)?;
                relabel(&conn, *id, subject)
            }
        }
    }
}

// -------------------- subject resolution --------------------

fn parse_head_subject(s: &str) -> Result<AnchorSubject> {
    match AnchorSubject::parse(s) {
        Some(AnchorSubject::Digest) => bail!("a digest subject needs --digest or --file"),
        Some(kind) => Ok(kind),
        None => bail!(
            "unknown subject '{s}' (allowed: chain_head, export_receipt_head, \
             break_glass_receipt_head, policy_head, clock_provenance_head)"
        ),
    }
}

fn parse_hex_digest(hex_digest: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(hex_digest).context("--digest is not valid hex")?;
    bytes
        .try_into()
        .map_err(|_| anyhow!("--digest must be a 32-byte SHA-256 hex digest"))
}

/// The head of `subject`'s ledger, or the documented "empty" error.
fn head_or_empty(conn: &Connection, subject: AnchorSubject) -> Result<[u8; 32]> {
    match tsa::ledger_head(conn, subject)? {
        Some(hash) => Ok(hash),
        None if subject == AnchorSubject::ChainHead => {
            bail!("sealed log is empty: nothing to anchor")
        }
        None => bail!("{} is empty: nothing to anchor", subject.ledger_short()),
    }
}

/// What to anchor: a ledger head (`--subject`, default `chain_head`), an
/// operator-supplied digest (`--digest`), or the SHA-256 of a file's bytes
/// (`--file`). The manual verbs refuse an empty ledger — a hand-run request
/// is not a schedule; only `anchor-all` substitutes the sentinel.
fn resolve_subject(
    conn: &Connection,
    subject: Option<&str>,
    digest: Option<&str>,
    file: Option<&Path>,
) -> Result<(AnchorSubject, [u8; 32])> {
    if let Some(hex_digest) = digest {
        return Ok((AnchorSubject::Digest, parse_hex_digest(hex_digest)?));
    }
    if let Some(path) = file {
        let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let hash: [u8; 32] = Sha256::digest(&bytes).into();
        eprintln!("digest of {}: {}", path.display(), hex::encode(hash));
        return Ok((AnchorSubject::Digest, hash));
    }
    let kind = match subject {
        Some(s) => parse_head_subject(s)?,
        None => AnchorSubject::ChainHead,
    };
    Ok((kind, head_or_empty(conn, kind)?))
}

/// The third line of a "stored" block / the trailer of a verify line: what
/// the token itself says about its signer.
fn signer_summary(token_der: &[u8]) -> String {
    match tsa::parse_token_signer(token_der) {
        Ok(s) => match s.signer_fingerprint {
            Some(cert) => format!("signer cert sha256:{}…", &hex::encode(cert)[..16]),
            None => format!(
                "signer sid:{}… (no certificate embedded)",
                &s.sid_hex[..16.min(s.sid_hex.len())]
            ),
        },
        Err(_) => "signer: not readable from the token".to_string(),
    }
}

fn print_stored(id: i64, kind: AnchorSubject, hash: &[u8; 32], token: &tsa::TimestampToken) {
    println!(
        "anchor #{id} stored: {} {}\n  genTime {}  serial 0x{}  policy {}\n  {}",
        kind,
        hex::encode(hash),
        token.gen_time,
        token.serial_hex,
        token.policy_oid,
        signer_summary(&token.token_der)
    );
}

fn ca_path_checked(entry: &anchor_policy::TsaEntry) -> Result<String> {
    if !entry.ca.exists() {
        bail!(
            "TSA '{}': ca file {} not found",
            entry.name,
            entry.ca.display()
        );
    }
    Ok(entry.ca.to_string_lossy().into_owned())
}

fn openssl_missing_warning(policy_path: &Path) {
    eprintln!(
        "warning: openssl not found; stored without a countersignature check — run \
         log_anchor verify --policy {}",
        policy_path.display()
    );
}

// -------------------- the OpenTimestamps leg (shared) --------------------

/// Refused before the first request when a policy carries `ots` but this
/// build has no calendar client: a run does every leg or nothing.
#[cfg_attr(not(feature = "tsa"), allow(dead_code))]
const NO_OTS_CLIENT: &str =
    "this build has no OpenTimestamps calendar client (rebuild with `--features tsa,ots`), \
     or use the offline flow: `log_anchor anchor-all --policy <file> --offline-dir <dir>` then \
     `log_anchor import --policy <file> --ots-dir <dir>`";

#[cfg_attr(not(feature = "tsa"), allow(dead_code))]
fn ots_client_check(policy: &AnchorPolicy) -> Result<()> {
    if policy.ots.is_some() && !cfg!(feature = "ots") {
        bail!("{NO_OTS_CLIENT}");
    }
    Ok(())
}

/// `  (empty-ledger sentinel for {owner})` for a digest row over a sentinel.
fn sentinel_suffix(stored_as: AnchorSubject, hash: &[u8; 32]) -> String {
    match AnchorSubject::sentinel_owner(hash) {
        Some(owner) if stored_as == AnchorSubject::Digest => {
            format!("  (empty-ledger sentinel for {owner})")
        }
        _ => String::new(),
    }
}

/// The two lines of a stored OpenTimestamps row.
fn print_ots_stored(
    id: i64,
    stored_as: AnchorSubject,
    hash: &[u8; 32],
    calendar: &str,
    state: ots::ProofState,
    pending_uri: Option<&str>,
) {
    println!(
        "ots anchor #{id} stored: {stored_as} {}{}",
        hex::encode(hash),
        sentinel_suffix(stored_as, hash)
    );
    match (state, pending_uri) {
        (ots::ProofState::Bitcoin { height }, _) => {
            println!("  attests Bitcoin block {height} via {calendar}")
        }
        (_, Some(uri)) => println!("  pending at {uri} via {calendar}"),
        (_, None) => println!("  via {calendar}"),
    }
}

/// Print what the leg did, in order. Every line says "attests", never
/// "confirmed": no header is checked here.
#[cfg_attr(not(feature = "tsa"), allow(dead_code))]
fn print_ots_events(report: &OtsLegReport, planned: &[PlannedOts]) {
    for event in &report.events {
        match event {
            OtsLegEvent::Upgrading { id, uri } => {
                eprintln!("ots: upgrading ots anchor #{id} at {uri}")
            }
            OtsLegEvent::Upgraded {
                id,
                subject,
                height,
            } => println!("ots anchor #{id} upgraded: {subject} attests Bitcoin block {height}"),
            OtsLegEvent::StillPending { id, uri } => {
                eprintln!("ots anchor #{id}: still pending at {uri}")
            }
            OtsLegEvent::AnsweredWithoutBitcoin { id, uri } => eprintln!(
                "ots anchor #{id}: {uri} answered without a Bitcoin attestation; left pending"
            ),
            OtsLegEvent::UpgradeFailed { id, uri, error } => {
                eprintln!("ots anchor #{id}: upgrade from {uri} FAILED: {error}")
            }
            OtsLegEvent::BeyondHorizon { .. } => {}
            OtsLegEvent::NotPolledUndeclared { id, uri } => eprintln!(
                "ots anchor #{id}: pending at {uri}, which this policy does not declare; not polled"
            ),
            OtsLegEvent::Stamping {
                planned: i,
                calendar,
                url,
            } => {
                let p = &planned[*i];
                let subject = if p.sentinel {
                    AnchorSubject::sentinel_owner(&p.hash).unwrap_or(p.stored_as)
                } else {
                    p.stored_as
                };
                eprintln!(
                    "ots: stamping {subject} {} via {calendar} ({url})",
                    hex::encode(p.hash)
                );
            }
            OtsLegEvent::Stored(i) => {
                let s = &report.stored[*i];
                print_ots_stored(
                    s.id,
                    s.stored_as,
                    &s.hash,
                    &s.calendar,
                    s.state,
                    s.first_pending_uri.as_deref(),
                );
            }
            OtsLegEvent::SubmitFailed(i) => {
                let f = &report.submit_failures[*i];
                eprintln!("ots via {} ({}) FAILED: {}", f.calendar, f.url, f.error);
            }
        }
    }
}

/// The online leg with the real transport (`--features tsa,ots`).
#[cfg(all(feature = "tsa", feature = "ots"))]
fn ots_leg_online(
    conn: &Connection,
    policy: &anchor_policy::OtsPolicy,
    planned: &[PlannedOts],
    upgrade: bool,
) -> Result<OtsLegReport> {
    ots::run_ots_leg(
        conn,
        policy,
        planned,
        &ots::UreqCalendar,
        TimeBucket::now_10min()?,
        upgrade,
        &mut ots::random_nonce,
    )
}

/// Unreachable after `ots_client_check`; kept so the build without `ots`
/// gives the same refusal.
#[cfg(all(feature = "tsa", not(feature = "ots")))]
fn ots_leg_online(
    _: &Connection,
    _: &anchor_policy::OtsPolicy,
    _: &[PlannedOts],
    _: bool,
) -> Result<OtsLegReport> {
    bail!("{NO_OTS_CLIENT}")
}

/// `{TSA text}; {OTS text}`, either alone, or nothing.
fn combined_failure(tsa_text: Option<String>, ots_text: Option<String>) -> Result<()> {
    match (tsa_text, ots_text) {
        (None, None) => Ok(()),
        (Some(t), None) => bail!("{t}"),
        (None, Some(o)) => bail!("{o}"),
        (Some(t), Some(o)) => bail!("{t}; {o}"),
    }
}

/// The curl line that submits one offline request file to its calendar.
fn ots_submit_curl(dir: &Path, stem: &str, url: &str) -> String {
    format!(
        "  curl -sf -H 'Accept: {}' --data-binary @{d}/{stem}.ots-commitment {url}/digest > {d}/{stem}.otsr",
        ots::OTS_ACCEPT,
        d = dir.display()
    )
}

/// Write `{stem}.ots-request.json` and `{stem}.ots-commitment` into `dir`
/// for one (hash, calendar) with a fresh nonce.
fn write_ots_request(
    dir: &Path,
    stem: &str,
    hash: &[u8; 32],
    calendar: &anchor_policy::CalendarEntry,
) -> Result<()> {
    let nonce = ots::random_nonce();
    let request = ots::OtsRequestFile {
        format: ots::OTS_REQUEST_FORMAT.to_string(),
        subject_hash: hex::encode(hash),
        nonce: hex::encode(nonce),
        calendar: calendar.name.clone(),
        calendar_url: calendar.url.clone(),
    };
    let json_path = dir.join(format!("{stem}.ots-request.json"));
    std::fs::write(&json_path, serde_json::to_string(&request)? + "\n")
        .with_context(|| format!("writing {}", json_path.display()))?;
    let c_path = dir.join(format!("{stem}.ots-commitment"));
    std::fs::write(&c_path, ots::commitment(hash, &nonce))
        .with_context(|| format!("writing {}", c_path.display()))?;
    Ok(())
}

/// The pending rows the upgrade pass would poll now: within the horizon,
/// with a pending leaf at a declared calendar. `(id, uri, message)`.
fn pollable_rows(
    conn: &Connection,
    policy: &anchor_policy::OtsPolicy,
    now: TimeBucket,
) -> Result<Vec<(i64, String, Vec<u8>)>> {
    let mut out = Vec::new();
    for row in ots::pending_ots_anchors(conn)? {
        if !ots::within_upgrade_horizon(now, row.created_bucket) {
            continue;
        }
        let Ok(proof) = ots::Proof::parse(&row.proof) else {
            continue;
        };
        if policy.calendar_for_url(&row.calendar_url).is_none() {
            continue;
        }
        if let Some((uri, msg)) = ots::pollable_leaf(&proof, &row.calendar_url) {
            out.push((row.id, uri, msg));
        }
    }
    Ok(out)
}

// -------------------- request (online) --------------------

/// One TSA to request from: a bare `--url`, or a policy entry.
#[cfg(feature = "tsa")]
struct Target<'a> {
    name: Option<&'a str>,
    url: &'a str,
    allow_http: bool,
    ca: Option<&'a anchor_policy::TsaEntry>,
}

#[cfg(feature = "tsa")]
fn targets<'a>(
    urls: &'a [String],
    allow_http: bool,
    policy: Option<&'a AnchorPolicy>,
) -> Vec<Target<'a>> {
    match policy {
        Some(p) => p
            .tsas
            .iter()
            .map(|e| Target {
                name: Some(e.name.as_str()),
                url: e.url.as_str(),
                allow_http: e.allow_http || allow_http,
                ca: Some(e),
            })
            .collect(),
        None => urls
            .iter()
            .map(|u| Target {
                name: None,
                url: u.as_str(),
                allow_http,
                ca: None,
            })
            .collect(),
    }
}

/// Request, check and store one token. Under a policy the token is checked
/// under the entry's declared CA BEFORE it is stored (when openssl is
/// available); a token that fails is not stored.
#[cfg(feature = "tsa")]
fn request_one(
    conn: &Connection,
    target: &Target<'_>,
    policy_path: Option<&Path>,
    kind: AnchorSubject,
    hash: &[u8; 32],
) -> Result<i64> {
    let nonce = tsa::random_nonce();
    let query = tsa::build_request(hash, Some(&nonce), true);
    let response = tsa::fetch_timestamp(target.url, &query, target.allow_http)?;
    let token = tsa::parse_response(&response)?;
    tsa::verify_match(&token, hash, Some(&nonce))?;
    if let (Some(entry), Some(name)) = (target.ca, target.name) {
        if anchor_policy::openssl_available() {
            let ca = ca_path_checked(entry)?;
            if let Err(e) = anchor_policy::openssl_ts_verify(&token.token_der, hash, &ca) {
                bail!(
                    "token from {name} does not verify under its declared CA {ca}: not stored ({e})"
                );
            }
        } else if let Some(path) = policy_path {
            openssl_missing_warning(path);
        }
    }
    let id = tsa::insert_anchor_declared(conn, kind, hash, target.url, target.name, &token)?;
    print_stored(id, kind, hash, &token);
    Ok(id)
}

#[cfg(feature = "tsa")]
fn request(
    conn: &Connection,
    urls: &[String],
    allow_http: bool,
    policy_path: Option<&Path>,
    subject: Option<&str>,
    digest: Option<&str>,
    file: Option<&Path>,
) -> Result<()> {
    let policy = policy_path.map(anchor_policy::load).transpose()?;
    if let Some(p) = &policy {
        ots_client_check(p)?;
    }
    let (kind, hash) = resolve_subject(conn, subject, digest, file)?;
    let targets = targets(urls, allow_http, policy.as_ref());
    let total = targets.len();
    let mut failed = 0usize;
    for target in &targets {
        eprintln!(
            "anchoring {} {} via {}",
            kind,
            hex::encode(hash),
            target.url
        );
        if let Err(e) = request_one(conn, target, policy_path, kind, &hash) {
            failed += 1;
            eprintln!("anchor via {} FAILED: {e:#}", target.url);
        }
    }
    let tsa_text = (failed > 0).then(|| format!("{failed} of {total} TSA request(s) failed"));
    // The policy's OpenTimestamps leg: the only online path by which a
    // court-kit bundle digest gets a proof. No upgrade pass (anchor-all's).
    let mut ots_text = None;
    if let Some(ots_policy) = policy.as_ref().and_then(|p| p.ots.as_ref()) {
        let planned = [PlannedOts {
            stored_as: kind,
            hash,
            sentinel: false,
        }];
        let report = ots_leg_online(conn, ots_policy, &planned, false)?;
        print_ots_events(&report, &planned);
        let f = report.submit_failures.len();
        if f > 0 {
            ots_text = Some(format!(
                "{f} of {} calendar submission(s) failed",
                report.submissions
            ));
        }
    }
    combined_failure(tsa_text, ots_text)
}

#[cfg(not(feature = "tsa"))]
const NO_TSA_CLIENT: &str = "this build has no online TSA client (rebuild with `--features tsa`), \
     or use the offline flow: `log_anchor query` then `log_anchor import`, or \
     `log_anchor anchor-all --policy <file> --offline-dir <dir>`";

#[cfg(not(feature = "tsa"))]
fn request(
    _: &Connection,
    _: &[String],
    _: bool,
    _: Option<&Path>,
    _: Option<&str>,
    _: Option<&str>,
    _: Option<&Path>,
) -> Result<()> {
    bail!("{NO_TSA_CLIENT}")
}

// -------------------- query (offline) --------------------

fn query(
    conn: &Connection,
    out: &str,
    subject: Option<&str>,
    digest: Option<&str>,
    file: Option<&Path>,
    policy_path: Option<&Path>,
) -> Result<()> {
    let policy = policy_path.map(anchor_policy::load).transpose()?;
    let (kind, hash) = resolve_subject(conn, subject, digest, file)?;
    // No nonce in the offline flow: the query may be submitted much later,
    // and import correlates by message imprint against ledger history instead.
    let der = tsa::build_request(&hash, None, true);
    std::fs::write(out, &der).with_context(|| format!("writing {out}"))?;
    println!(
        "query for {} {} written to {}\nsubmit it, e.g.:\n  curl -s -H 'Content-Type: application/timestamp-query' \\\n       --data-binary @{} https://freetsa.org/tsr > {}.tsr\nthen: log_anchor import --response {}.tsr --url <TSA>  (within the retention window: after the head is pruned the token can only be stored as a digest anchor)",
        kind,
        hex::encode(hash),
        out,
        out,
        out.trim_end_matches(".tsq"),
        out.trim_end_matches(".tsq"),
    );
    // The offline twin of `request --policy`: one OpenTimestamps request
    // per declared calendar beside --out (e.g. for a bundle digest).
    if let (Some(p), Some(path)) = (&policy, policy_path) {
        if let Some(ots_policy) = &p.ots {
            let out_path = Path::new(out);
            let dir = match out_path.parent() {
                Some(d) if !d.as_os_str().is_empty() => d.to_path_buf(),
                _ => PathBuf::from("."),
            };
            let stem = out_path
                .file_stem()
                .and_then(|s| s.to_str())
                .ok_or_else(|| anyhow!("--out {out} has no file name"))?;
            println!("submit every OpenTimestamps request to its calendar, e.g.:");
            for cal in &ots_policy.calendars {
                let s = format!("{stem}.{}", cal.name);
                write_ots_request(&dir, &s, &hash, cal)?;
                println!("{}", ots_submit_curl(&dir, &s, &cal.url));
            }
            println!(
                "then: log_anchor import --policy {} --ots-dir {}",
                path.display(),
                dir.display()
            );
        }
    }
    Ok(())
}

// -------------------- import (offline) --------------------

/// The policy entry a `{subject}.{name}.tsr` file name points at.
fn infer_tsa_name<'a>(policy: &'a AnchorPolicy, file: &str) -> Option<&'a str> {
    let stem = Path::new(file)
        .file_name()?
        .to_str()?
        .strip_suffix(".tsr")?;
    let (_subject, name) = stem.rsplit_once('.')?;
    policy.entry(name).map(|e| e.name.as_str())
}

/// Store one response. The subject is derived from the ledgers, never from
/// the file: a hit in any ledger is that ledger's head kind; a miss is a
/// digest anchor (a sentinel is named as such).
fn import_one(
    conn: &Connection,
    file: &str,
    url: Option<&str>,
    policy: Option<(&AnchorPolicy, &Path)>,
    tsa_name: Option<&str>,
) -> Result<()> {
    let der = std::fs::read(file).with_context(|| format!("reading {file}"))?;
    let token = tsa::parse_response(&der)?;
    let hash: [u8; 32] = token
        .imprint
        .clone()
        .try_into()
        .map_err(|_| anyhow!("token imprint is not a 32-byte SHA-256 digest"))?;

    // Which TSA, and its declared CA, under a policy.
    let (name, entry) = match policy {
        Some((p, _)) => {
            let name = match tsa_name {
                Some(n) => p
                    .entry(n)
                    .map(|e| e.name.as_str())
                    .ok_or_else(|| anyhow!("TSA '{n}' is not declared in the anchor policy"))?,
                None => infer_tsa_name(p, file)
                    .ok_or_else(|| anyhow!("cannot infer the TSA for {file}: pass --tsa <name>"))?,
            };
            (Some(name), p.entry(name))
        }
        None => (None, None),
    };
    let recorded_url = match (url, entry) {
        (Some(u), _) => u.to_string(),
        (None, Some(e)) => e.url.clone(),
        (None, None) => "(offline)".to_string(),
    };

    let kind = classify_imported(conn, &hash, "imprint")?;

    if let (Some(e), Some(n), Some((_, policy_path))) = (entry, name, policy) {
        if anchor_policy::openssl_available() {
            let ca = ca_path_checked(e)?;
            if let Err(err) = anchor_policy::openssl_ts_verify(&token.token_der, &hash, &ca) {
                bail!("response {file} from {n} does not verify under its declared CA {ca}: not stored ({err})");
            }
        } else {
            openssl_missing_warning(policy_path);
        }
    }

    let id = tsa::insert_anchor_declared(conn, kind, &hash, &recorded_url, name, &token)?;
    print_stored(id, kind, &hash, &token);
    Ok(())
}

/// The subject an imported hash is stored under, derived from the ledgers,
/// never from a file: the ledgers may have advanced since the request was
/// written, so the hash must match *some* recorded state of *some* ledger,
/// not the current head. A miss is a digest anchor (a sentinel is named).
fn classify_imported(conn: &Connection, hash: &[u8; 32], noun: &str) -> Result<AnchorSubject> {
    Ok(match tsa::classify_hash(conn, hash)? {
        Some(loc) => loc.subject,
        None => {
            match AnchorSubject::sentinel_owner(hash) {
                Some(owner) => eprintln!(
                    "note: {noun} {}… is the empty-ledger sentinel for {owner}; stored as a digest anchor",
                    &hex::encode(hash)[..16]
                ),
                None => eprintln!(
                    "note: {noun} {} is not in this DB's chain or receipt-ledger history; storing as a \
                     generic digest anchor (use this for export-bundle digests; a ledger head pruned by \
                     retention before import also lands here)",
                    hex::encode(hash)
                ),
            }
            AnchorSubject::Digest
        }
    })
}

fn import(
    conn: &Connection,
    responses: &[String],
    ots_dir: Option<&Path>,
    url: Option<&str>,
    policy_path: Option<&Path>,
    tsa_name: Option<&str>,
) -> Result<()> {
    let policy = policy_path.map(anchor_policy::load).transpose()?;
    let policy_ref = policy.as_ref().zip(policy_path);
    let total = responses.len();
    let mut failed = 0usize;
    for file in responses {
        if let Err(e) = import_one(conn, file, url, policy_ref, tsa_name) {
            failed += 1;
            eprintln!("import {file}: {e:#}");
        }
    }
    let tsa_text =
        (failed > 0).then(|| format!("{failed} of {total} response(s) failed to import"));
    let ots_text = match ots_dir {
        Some(dir) => import_ots_dir(conn, dir, policy_ref)?,
        None => None,
    };
    combined_failure(tsa_text, ots_text)
}

/// What one OpenTimestamps upgrade file did.
enum OtsUpgradeOutcome {
    Upgraded,
    StillPending,
    Ignored,
}

/// `import --ots-dir`: every `*.ots-request.json` (a stamp) and every
/// `ots-*.ots-upgrade.json` (an upgrade), in name order. Returns the exit
/// text when any response failed.
fn import_ots_dir(
    conn: &Connection,
    dir: &Path,
    policy: Option<(&AnchorPolicy, &Path)>,
) -> Result<Option<String>> {
    let now = TimeBucket::now_10min()?;
    let mut names: Vec<String> = std::fs::read_dir(dir)
        .with_context(|| format!("reading {}", dir.display()))?
        .filter_map(|e| e.ok())
        .filter_map(|e| e.file_name().to_str().map(str::to_string))
        .collect();
    names.sort();
    let (mut stored, mut upgraded, mut pending, mut failed) = (0usize, 0usize, 0usize, 0usize);
    for name in &names {
        let Some(stem) = name.strip_suffix(".ots-request.json") else {
            continue;
        };
        match import_ots_request(conn, dir, stem, policy, now) {
            Ok(()) => stored += 1,
            Err(e) => {
                failed += 1;
                eprintln!("{e:#}");
            }
        }
    }
    for name in &names {
        if !(name.starts_with("ots-") && name.ends_with(".ots-upgrade.json")) {
            continue;
        }
        match import_ots_upgrade(conn, dir, name, now) {
            Ok(OtsUpgradeOutcome::Upgraded) => upgraded += 1,
            Ok(OtsUpgradeOutcome::StillPending) => pending += 1,
            Ok(OtsUpgradeOutcome::Ignored) => {}
            Err(e) => {
                failed += 1;
                eprintln!("{e:#}");
            }
        }
    }
    println!(
        "import (ots): {stored} stored, {upgraded} upgraded, {pending} still pending, {failed} failed"
    );
    Ok((failed > 0).then(|| format!("{failed} OpenTimestamps response(s) failed to import")))
}

/// One stamp: `{stem}.ots-request.json` + `{stem}.otsr`.
fn import_ots_request(
    conn: &Connection,
    dir: &Path,
    stem: &str,
    policy: Option<(&AnchorPolicy, &Path)>,
    now: TimeBucket,
) -> Result<()> {
    let json_path = dir.join(format!("{stem}.ots-request.json"));
    let file = json_path.display().to_string();
    let response_name = format!("{stem}.otsr");
    let text = std::fs::read_to_string(&json_path).with_context(|| format!("reading {file}"))?;
    let request: ots::OtsRequestFile =
        serde_json::from_str(&text).map_err(|e| anyhow!("{file}: {e}"))?;
    let (hash, nonce) = request.decode().map_err(|e| anyhow!("{file}: {e}"))?;
    let commitment = ots::commitment(&hash, &nonce);
    let c_path = dir.join(format!("{stem}.ots-commitment"));
    if let Ok(bytes) = std::fs::read(&c_path) {
        if bytes != commitment {
            bail!(
                "{}: is not SHA256(subject_hash ‖ nonce) of {file}; not stored",
                c_path.display()
            );
        }
    }
    if let Some((p, path)) = policy {
        let declared = p
            .ots
            .as_ref()
            .and_then(|o| o.calendar(&request.calendar))
            .is_some_and(|c| c.url == request.calendar_url);
        if !declared {
            bail!(
                "{file}: calendar '{}' is not declared in {} with url {}; not stored",
                request.calendar,
                path.display(),
                request.calendar_url
            );
        }
    }
    let body = std::fs::read(dir.join(&response_name)).unwrap_or_default();
    if body.is_empty() {
        bail!("import {response_name}: no response (the calendar request failed); not stored");
    }
    let node = ots::parse_timestamp(&body, &commitment).map_err(|e| {
        anyhow!("import {response_name}: not an OpenTimestamps calendar response ({e})")
    })?;
    ots::accept_response(&node, &request.calendar, &request.calendar_url)
        .map_err(|e| anyhow!("import {response_name}: {e}"))?;
    let proof = ots::Proof::compose(&hash, &nonce, node)
        .map_err(|e| anyhow!("import {response_name}: {e}"))?;
    let kind = classify_imported(conn, &hash, "digest")?;
    let id = ots::insert_ots_anchor(conn, now, kind, &hash, &request.calendar_url, &proof)
        .map_err(|e| anyhow!("import {response_name}: {e:#}"))?;
    let pend = proof.pending_leaves();
    let uri = pend
        .iter()
        .find(|(u, _)| *u == request.calendar_url)
        .or(pend.first())
        .map(|(u, _)| u.as_str());
    print_ots_stored(id, kind, &hash, &request.calendar, proof.state(), uri);
    Ok(())
}

/// One upgrade: `ots-{id}.ots-upgrade.json` + `ots-{id}.otsr`, bound to
/// the stored proof by the pending leaf's message — a misnamed file is
/// refused, never merged.
fn import_ots_upgrade(
    conn: &Connection,
    dir: &Path,
    name: &str,
    now: TimeBucket,
) -> Result<OtsUpgradeOutcome> {
    let json_path = dir.join(name);
    let file = json_path.display().to_string();
    let text = std::fs::read_to_string(&json_path).with_context(|| format!("reading {file}"))?;
    let upgrade: ots::OtsUpgradeFile =
        serde_json::from_str(&text).map_err(|e| anyhow!("{file}: {e}"))?;
    let msg = upgrade.decode().map_err(|e| anyhow!("{file}: {e}"))?;
    let id = upgrade.anchor_id;
    let row = ots::list_ots_anchors_lenient(conn)?
        .into_iter()
        .find(|(rid, _)| *rid == id)
        .and_then(|(_, r)| r.ok())
        .filter(|r| r.attestation == ots::OTS_ATTESTATION_PENDING);
    let Some(row) = row else {
        eprintln!("ots anchor #{id} is not pending; {file} ignored");
        return Ok(OtsUpgradeOutcome::Ignored);
    };
    let proof = ots::Proof::parse(&row.proof)
        .map_err(|e| anyhow!("ots anchor #{id}: stored proof does not parse ({e})"))?;
    if !proof
        .pending_leaves()
        .iter()
        .any(|(u, m)| *u == upgrade.uri && *m == msg)
    {
        eprintln!("{file}: message is not a pending leaf of ots anchor #{id}; ignored");
        return Ok(OtsUpgradeOutcome::Ignored);
    }
    let response_path = dir.join(format!("ots-{id}.otsr"));
    let body = std::fs::read(&response_path).unwrap_or_default();
    if body.is_empty() {
        eprintln!(
            "ots anchor #{id}: still pending (empty response in {})",
            response_path.display()
        );
        return Ok(OtsUpgradeOutcome::StillPending);
    }
    match ots::apply_upgrade(conn, &row, now, &upgrade.uri, &msg, &body)
        .map_err(|e| anyhow!("import {}: {e:#}", response_path.display()))?
    {
        Some(height) => {
            println!(
                "ots anchor #{id} upgraded: {} attests Bitcoin block {height}",
                row.subject
            );
            Ok(OtsUpgradeOutcome::Upgraded)
        }
        None => {
            eprintln!(
                "ots anchor #{id}: {} answered without a Bitcoin attestation; left pending",
                upgrade.uri
            );
            Ok(OtsUpgradeOutcome::StillPending)
        }
    }
}

// -------------------- anchor-all --------------------

/// What `anchor-all` anchors for one policy subject: the ledger head, or —
/// when the ledger is empty — the per-subject sentinel, stored as a digest.
struct Planned {
    subject: AnchorSubject,
    /// The subject the row is stored under (the head kind, or `digest` for
    /// a sentinel). Only the online path stores rows.
    #[cfg_attr(not(feature = "tsa"), allow(dead_code))]
    stored_as: AnchorSubject,
    hash: [u8; 32],
    sentinel: bool,
}

fn plan_subjects(conn: &Connection, policy: &AnchorPolicy) -> Result<Vec<Planned>> {
    let mut out = Vec::new();
    for subject in policy.subject_kinds() {
        match tsa::ledger_head(conn, subject)? {
            Some(hash) => out.push(Planned {
                subject,
                stored_as: subject,
                hash,
                sentinel: false,
            }),
            None => {
                let hash = subject
                    .empty_ledger_sentinel()
                    .ok_or_else(|| anyhow!("subject {subject} has no sentinel"))?;
                eprintln!(
                    "anchor-all: {subject}: ledger is empty — anchoring the empty-ledger sentinel {}… \
                     so the request count stays constant",
                    &hex::encode(hash)[..16]
                );
                if subject == AnchorSubject::ClockProvenanceHead {
                    eprintln!(
                        "anchor-all: {subject}: witnessd seals one record per anchor row this run \
                         stores, so the ledger is no longer empty afterwards and `verify --policy` \
                         reads this subject NOT covered until the next anchor-all; run anchor-all \
                         again once witnessd has polled (about 5 s)"
                    );
                }
                out.push(Planned {
                    subject,
                    stored_as: AnchorSubject::Digest,
                    hash,
                    sentinel: true,
                });
            }
        }
    }
    Ok(out)
}

#[cfg(feature = "tsa")]
fn anchor_all_online(conn: &Connection, policy_path: &Path, allow_http: bool) -> Result<()> {
    let policy = anchor_policy::load(policy_path)?;
    ots_client_check(&policy)?;
    let planned = plan_subjects(conn, &policy)?;
    let targets = targets(&[], allow_http, Some(&policy));
    let mut stored = 0usize;
    let mut failed = 0usize;
    let sentinels = planned.iter().filter(|p| p.sentinel).count();
    // Invariant III (spec/invariants.md §5): the request COUNT per run must
    // not track what happened since the last run. Every configured
    // (subject × TSA) pair is requested on every run — no already-anchored
    // lookup, no skip for an unchanged head (a duplicate row over an
    // unchanged head is harmless; coverage dedupes by hash), and an empty
    // ledger is requested over its fixed sentinel — so a TSA watching this
    // schedule sees |subjects| × |TSAs| requests whether or not an export, a
    // break-glass unseal or a policy change occurred.
    for p in &planned {
        for target in &targets {
            let name = target.name.unwrap_or("?");
            eprintln!(
                "anchoring {} {} via {name} ({})",
                p.subject,
                hex::encode(p.hash),
                target.url
            );
            match request_one(conn, target, Some(policy_path), p.stored_as, &p.hash) {
                Ok(_) => stored += 1,
                Err(e) => {
                    failed += 1;
                    eprintln!("anchor via {} FAILED: {e:#}", target.url);
                }
            }
        }
    }
    println!(
        "anchor-all: {stored} stored, {failed} failed ({}×{} requested; {sentinels} over empty-ledger sentinels)",
        planned.len(),
        targets.len()
    );
    let tsa_text = (failed > 0).then(|| {
        format!(
            "{failed} of {} TSA request(s) failed",
            planned.len() * targets.len()
        )
    });
    // The OpenTimestamps leg runs whether or not TSA requests failed, so a
    // TSA failure never changes whether calendars are asked (Invariant III):
    // upgrades of earlier pending rows first, then every planned subject at
    // every declared calendar, a fresh nonce each.
    let mut ots_text = None;
    if let Some(ots_policy) = &policy.ots {
        let planned_ots: Vec<PlannedOts> = planned
            .iter()
            .map(|p| PlannedOts {
                stored_as: p.stored_as,
                hash: p.hash,
                sentinel: p.sentinel,
            })
            .collect();
        let report = ots_leg_online(conn, ots_policy, &planned_ots, true)?;
        print_ots_events(&report, &planned_ots);
        let f = report.submit_failures.len();
        println!(
            "anchor-all (ots): {} stored, {f} failed ({}×{} submitted; {sentinels} over empty-ledger \
             sentinels); upgrades: {} now attest a Bitcoin block, {} still pending, {} error(s), {} \
             beyond the 7-day horizon (not polled)",
            report.stored.len(),
            planned_ots.len(),
            ots_policy.calendars.len(),
            report.upgraded.len(),
            report.still_pending.len(),
            report.upgrade_errors.len(),
            report.beyond_horizon.len()
        );
        if f > 0 {
            ots_text = Some(format!(
                "{f} of {} calendar submission(s) failed",
                report.submissions
            ));
        }
    }
    combined_failure(tsa_text, ots_text)
}

#[cfg(not(feature = "tsa"))]
fn anchor_all_online(_: &Connection, _: &Path, _: bool) -> Result<()> {
    bail!("{NO_TSA_CLIENT}")
}

fn anchor_all_offline(conn: &Connection, db: &str, policy_path: &Path, dir: &Path) -> Result<()> {
    let policy = anchor_policy::load(policy_path)?;
    let planned = plan_subjects(conn, &policy)?;
    std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    // Same Invariant III rule as the online path: one .tsq per configured
    // subject, every run, sentinel when the ledger is empty — the submission
    // pattern never reflects what happened since the last run.
    let mut lines = Vec::new();
    for p in &planned {
        let path = dir.join(format!("{}.tsq", p.subject));
        std::fs::write(&path, tsa::build_request(&p.hash, None, true))
            .with_context(|| format!("writing {}", path.display()))?;
        let what = if p.sentinel {
            format!(
                "{} (ledger empty) sentinel {}",
                p.subject,
                hex::encode(p.hash)
            )
        } else {
            format!("{} {}", p.subject, hex::encode(p.hash))
        };
        lines.push((path.display().to_string(), what));
    }
    let width = lines.iter().map(|(p, _)| p.len()).max().unwrap_or(0);
    println!(
        "anchor-all (offline): {} query file(s) written to {}  ({}×{} submissions per run, always)",
        planned.len(),
        dir.display(),
        planned.len(),
        policy.tsas.len()
    );
    for (path, what) in &lines {
        println!("  {path:<width$}   {what}");
    }
    // The OpenTimestamps leg, offline: |subjects| × |calendars| request
    // files every run (a fresh nonce each), plus one upgrade file per
    // pending row the online pass would poll now — the same counts.
    let mut ots_stems: Vec<(String, String)> = Vec::new();
    let mut ots_upgrades: Vec<(i64, String, Vec<u8>)> = Vec::new();
    if let Some(ots_policy) = &policy.ots {
        for p in &planned {
            for cal in &ots_policy.calendars {
                let stem = format!("{}.{}", p.subject, cal.name);
                write_ots_request(dir, &stem, &p.hash, cal)?;
                ots_stems.push((stem, cal.url.clone()));
            }
        }
        ots_upgrades = pollable_rows(conn, ots_policy, TimeBucket::now_10min()?)?;
        for (id, uri, msg) in &ots_upgrades {
            let upgrade = ots::OtsUpgradeFile {
                format: ots::OTS_UPGRADE_FORMAT.to_string(),
                anchor_id: *id,
                uri: uri.clone(),
                message: hex::encode(msg),
            };
            let path = dir.join(format!("ots-{id}.ots-upgrade.json"));
            std::fs::write(&path, serde_json::to_string(&upgrade)? + "\n")
                .with_context(|| format!("writing {}", path.display()))?;
        }
        println!(
            "anchor-all (offline): {n}×{c} OpenTimestamps request file(s) written to {}  ({n}×{c} \
             calendar submissions per run, always; {} upgrade fetch(es) for pending proofs)",
            dir.display(),
            ots_upgrades.len(),
            n = planned.len(),
            c = ots_policy.calendars.len(),
        );
    }
    println!("submit every .tsq to every TSA, e.g.:");
    for p in &planned {
        for entry in &policy.tsas {
            println!(
                "  curl -s -H 'Content-Type: application/timestamp-query' --data-binary @{}/{s}.tsq {} > {}/{s}.{}.tsr",
                dir.display(),
                entry.url,
                dir.display(),
                entry.name,
                s = p.subject
            );
        }
    }
    if policy.ots.is_some() {
        println!("submit every OpenTimestamps request to its calendar, e.g.:");
        for (stem, url) in &ots_stems {
            println!("{}", ots_submit_curl(dir, stem, url));
        }
        for (id, uri, msg) in &ots_upgrades {
            println!(
                "  curl -sf -H 'Accept: {}' {uri}/timestamp/{} > {}/ots-{id}.otsr",
                ots::OTS_ACCEPT,
                hex::encode(msg),
                dir.display()
            );
        }
    }
    println!(
        "then, back on this host, within the retention window ([retention], default 7 days — a head pruned before import can only be stored as a digest anchor):\n  log_anchor --db {db} import --policy {p} --response {d}/*.tsr\n  log_anchor --db {db} verify --policy {p}",
        p = policy_path.display(),
        d = dir.display()
    );
    if policy.ots.is_some() {
        println!(
            "and, for the OpenTimestamps leg:\n  log_anchor --db {db} import --policy {} --ots-dir {}",
            policy_path.display(),
            dir.display()
        );
    }
    Ok(())
}

/// The first 16 characters of a string read back from the database, for the
/// abbreviated `…` lines. A cache cell is operator-reachable text, so it is
/// cut on a character boundary, not a byte offset — a byte slice panics on a
/// multibyte character straddling the cut and takes the verdict with it.
fn prefix16(s: &str) -> String {
    s.chars().take(16).collect()
}

/// What a cached identity column must look like: the lowercase hex SHA-256
/// `insert_anchor` writes. Anything else was not written by this tool.
fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

// -------------------- list --------------------

/// What a proof claims, for `list` (display only; `verify` re-derives).
fn ots_state_text(proof: &ots::Proof) -> String {
    match proof.state() {
        ots::ProofState::Bitcoin { height } => format!("attests Bitcoin block {height}"),
        ots::ProofState::Pending => format!(
            "pending at {}",
            proof
                .pending_leaves()
                .into_iter()
                .map(|(u, _)| u)
                .collect::<Vec<_>>()
                .join(", ")
        ),
        ots::ProofState::NoKnownAttestation => "no pending or Bitcoin attestation".to_string(),
    }
}

fn list(conn: &Connection) -> Result<()> {
    let anchors = if tsa::anchor_table_exists(conn)? {
        tsa::list_anchors(conn)?
    } else {
        Vec::new()
    };
    let ots_rows = ots::list_ots_anchors_lenient(conn)?;
    if anchors.is_empty() && ots_rows.is_empty() {
        println!("no anchors stored");
        return Ok(());
    }
    for a in anchors {
        // Identity comes from the row columns; when they are NULL (legacy
        // rows) it is re-derived from the token. Display only — `verify`
        // re-derives always.
        let signer = tsa::parse_token_signer(&a.token_der).ok();
        let fp = a.signer_fingerprint.clone().or_else(|| {
            signer
                .as_ref()
                .and_then(|s| s.signer_fingerprint.map(hex::encode))
        });
        let sid = a
            .signer_sid
            .clone()
            .or_else(|| signer.as_ref().map(|s| s.sid_hex.clone()));
        let cn = signer.as_ref().and_then(|s| s.signer_common_name.clone());
        let signer_text = match (fp, sid) {
            (Some(c), _) => match cn {
                Some(cn) => format!("cert sha256:{}… ({cn})", prefix16(&c)),
                None => format!("cert sha256:{}…", prefix16(&c)),
            },
            (None, Some(s)) => format!("sid:{}…", prefix16(&s)),
            (None, None) => "(not readable)".to_string(),
        };
        let sentinel = match AnchorSubject::sentinel_owner(&a.subject_hash) {
            Some(owner) if a.subject_kind_or_digest() == AnchorSubject::Digest => {
                format!("  (empty-ledger sentinel for {owner})")
            }
            _ => String::new(),
        };
        println!(
            "#{}  {}  {}{}  genTime {}  via {}  tsa {}  signer {}",
            a.id,
            a.subject,
            hex::encode(a.subject_hash),
            sentinel,
            a.gen_time,
            a.tsa_url,
            a.tsa_name.as_deref().unwrap_or("(undeclared)"),
            signer_text
        );
    }
    for (id, row) in ots_rows {
        let r = match row {
            Ok(r) => r,
            Err(e) => {
                println!("ots #{id}  (row malformed: {e})");
                continue;
            }
        };
        let state = match ots::Proof::parse(&r.proof) {
            Ok(proof) => ots_state_text(&proof),
            Err(_) => "(proof unreadable)".to_string(),
        };
        println!(
            "ots #{}  {}  {}{}  bucket {}  {state}  via {}",
            r.id,
            r.subject,
            hex::encode(r.subject_hash),
            sentinel_suffix(r.subject_kind_or_digest(), &r.subject_hash),
            bucket_utc(r.created_bucket.start_epoch_s),
            r.calendar_url
        );
    }
    Ok(())
}

// -------------------- verify --------------------

/// The membership clause for a row: what was checked, and only that.
fn membership_text(a: &AnchorRecord) -> String {
    membership_text_for(&a.subject, a.subject_kind(), &a.subject_hash)
}

/// `membership_text` for any row: its subject text, kind and hash.
fn membership_text_for(subject_text: &str, kind: Option<AnchorSubject>, hash: &[u8; 32]) -> String {
    match kind {
        Some(AnchorSubject::Digest) => match AnchorSubject::sentinel_owner(hash) {
            Some(owner) => {
                format!("empty-ledger sentinel for {owner}, chain membership not applicable")
            }
            None => AnchorSubject::Digest.membership_label().to_string(),
        },
        Some(kind) => kind.membership_label().to_string(),
        None => format!(
            "unknown subject '{subject_text}', treated as a digest anchor; ledger membership not asserted"
        ),
    }
}

fn bucket_utc(start_epoch_s: u64) -> String {
    let (y, m, d, hh, mm, _ss) = witness_kernel::epoch_civil_utc(start_epoch_s);
    format!("{y:04}-{m:02}-{d:02} {hh:02}:{mm:02} UTC")
}

fn roles_text(by: &[(String, anchor_policy::TsaRole)]) -> String {
    by.iter()
        .map(|(n, r)| format!("{n} ({r}, declared)"))
        .collect::<Vec<_>>()
        .join(", ")
}

/// What the OpenTimestamps rows of `verify` came to.
struct OtsVerifyOutcome {
    failures: usize,
    /// Rows that passed every structural check, for the policy coverage.
    passed: Vec<anchor_policy::OtsPassed>,
}

/// The display text of a proof's pending URIs.
fn pending_uris_text(proof: &ots::Proof) -> String {
    proof
        .pending_leaves()
        .into_iter()
        .map(|(u, _)| u)
        .collect::<Vec<_>>()
        .join(", ")
}

/// Every OpenTimestamps row, in id order: structural checks (any problem is
/// FAIL), then the Bitcoin tier — `OK … confirmed` only when a leaf reaches
/// the merkle root of a header the operator supplied; a height alone, or a
/// pending proof, is `UNVERIFIED`. Network-silent.
fn verify_ots_rows(
    conn: &Connection,
    rows: Vec<(i64, Result<ots::OtsAnchorRecord>)>,
    headers: Option<&ots::HeaderFile>,
    now: TimeBucket,
) -> Result<OtsVerifyOutcome> {
    let mut out = OtsVerifyOutcome {
        failures: 0,
        passed: Vec::new(),
    };
    let (mut confirmed, mut unchecked, mut pending) = (0usize, 0usize, 0usize);
    let total = rows.len();
    for (id, row) in rows {
        let r = match row {
            Ok(r) => r,
            Err(e) => {
                out.failures += 1;
                println!("ots anchor #{id}: FAIL");
                println!("    row is malformed: {e}");
                continue;
            }
        };
        let mut problems: Vec<String> = Vec::new();
        let proof = match ots::Proof::parse(&r.proof) {
            Ok(p) => Some(p),
            Err(e) => {
                problems.push(format!(
                    "proof does not parse as an OpenTimestamps proof: {e}"
                ));
                None
            }
        };
        let mut checks = Vec::new();
        if let Some(proof) = &proof {
            if proof.digest != r.subject_hash {
                problems.push(format!(
                    "proof covers {} but anchor row claims {}",
                    hex::encode(proof.digest),
                    hex::encode(r.subject_hash)
                ));
            }
            let state = proof.state();
            let holds = match state {
                ots::ProofState::Bitcoin { height } => format!("Bitcoin block {height}"),
                _ => "pending".to_string(),
            };
            match r.attestation.as_str() {
                ots::OTS_ATTESTATION_PENDING | ots::OTS_ATTESTATION_BITCOIN => {
                    if let Some(column) = state.column() {
                        if column != r.attestation {
                            problems.push(format!(
                                "row records attestation '{}' but the proof holds {holds}",
                                r.attestation
                            ));
                        } else if let ots::ProofState::Bitcoin { height } = state {
                            if r.bitcoin_height != Some(height as i64) {
                                problems.push(format!(
                                    "row records Bitcoin block {} but the proof's lowest Bitcoin \
                                     attestation is block {height}",
                                    r.bitcoin_height
                                        .map(|h| h.to_string())
                                        .unwrap_or_else(|| "(none)".to_string())
                                ));
                            }
                        } else if let Some(h) = r.bitcoin_height {
                            problems.push(format!(
                                "row records Bitcoin block {h} but the proof holds pending"
                            ));
                        }
                    }
                }
                other => problems.push(format!(
                    "row records an attestation value this tool never writes ({other:?})"
                )),
            }
            if state == ots::ProofState::NoKnownAttestation {
                problems.push("proof carries no Pending or Bitcoin attestation".to_string());
            }
            checks = ots::check_bitcoin_leaves(proof, headers);
            for c in &checks {
                if let ots::BitcoinCheck::Malformed { height, len } = c {
                    problems.push(format!(
                        "Bitcoin attestation for block {height} is over a {len}-byte message; a \
                         merkle root is 32 bytes"
                    ));
                }
            }
        }
        if let Some(kind) = r.subject_kind().filter(|k| k.is_head()) {
            if tsa::hash_in_ledger(conn, &r.subject_hash, kind)?.is_none() {
                problems.push(kind.not_in_history_label().to_string());
            }
        }
        // A branch that contradicts a header the verifier trusts is FAIL,
        // even when another branch matches.
        for c in &checks {
            if let ots::BitcoinCheck::Mismatch {
                height,
                proof_display,
                header_display,
            } = c
            {
                problems.push(format!(
                    "merkle root of the supplied header for Bitcoin block {height} \
                     ({header_display}) does not match the proof ({proof_display})"
                ));
            }
        }
        let (Some(proof), true) = (&proof, problems.is_empty()) else {
            out.failures += 1;
            println!("ots anchor #{}: FAIL", r.id);
            for p in problems {
                println!("    {p}");
            }
            continue;
        };
        let membership = membership_text_for(&r.subject, r.subject_kind(), &r.subject_hash);
        let matched = checks
            .iter()
            .filter_map(|c| match c {
                ots::BitcoinCheck::Matches {
                    height,
                    block_hash_display,
                } => Some((*height, block_hash_display.clone())),
                _ => None,
            })
            .min_by_key(|(h, _)| *h);
        let state = if let Some((height, block_hash)) = matched {
            confirmed += 1;
            println!(
                "ots anchor #{}: OK (proof digest matches, {membership}; confirmed in Bitcoin block \
                 {height} against the supplied header, block hash {block_hash}) via {}",
                r.id, r.calendar_url
            );
            anchor_policy::OtsRowState::Confirmed { height }
        } else if let ots::ProofState::Bitcoin { height } = proof.state() {
            unchecked += 1;
            let why = match headers {
                None => {
                    "merkle root NOT checked — re-run with --bitcoin-headers <file>".to_string()
                }
                Some(h) => format!("no header for block {height} in {}", h.path),
            };
            println!(
                "ots anchor #{}: UNVERIFIED (proof digest matches, {membership}; attests Bitcoin \
                 block {height}; {why}) via {}",
                r.id, r.calendar_url
            );
            anchor_policy::OtsRowState::Unchecked { height }
        } else {
            pending += 1;
            let tail = if ots::within_upgrade_horizon(now, r.created_bucket) {
                ""
            } else if r.created_bucket.start_epoch_s > now.start_epoch_s {
                ", dated more than 7 days after this host's clock — not polled; check the host \
                 clock"
            } else {
                ", beyond the 7-day upgrade horizon — no longer polled; the calendar may never \
                 have committed it"
            };
            println!(
                "ots anchor #{}: UNVERIFIED (proof digest matches, {membership}; pending (not yet \
                 in a Bitcoin block) at {}{tail}) via {}",
                r.id,
                pending_uris_text(proof),
                r.calendar_url
            );
            anchor_policy::OtsRowState::Pending
        };
        out.passed.push(anchor_policy::OtsPassed {
            id: r.id,
            subject: r.subject_kind_or_digest(),
            hash: r.subject_hash,
            bucket_start: r.created_bucket.start_epoch_s,
            state,
        });
    }
    if total > 0 {
        println!(
            "{total} OpenTimestamps anchor(s): {confirmed} confirmed against the supplied header, \
             {unchecked} attest a Bitcoin block not checked against a header, {pending} pending \
             (not yet in a Bitcoin block), {} failed",
            out.failures
        );
        if unchecked > 0 && headers.is_none() {
            println!(
                "  A block height alone is a claim anyone can write into a file; pass\n  \
                 --bitcoin-headers <file>   (one line per block: <height> <160 hex of the 80-byte \
                 header>, from a node you run: bitcoin-cli getblockheader $(bitcoin-cli \
                 getblockhash N) false)"
            );
        }
    }
    Ok(out)
}

fn verify(
    conn: &Connection,
    db: &str,
    cas: &[String],
    policy_path: Option<&Path>,
    require_current: bool,
    bitcoin_headers: Option<&Path>,
) -> Result<()> {
    // The only Bitcoin source verify reads: the operator's header file,
    // loaded (and proof-of-work sanity-checked) before any output.
    let headers = bitcoin_headers.map(ots::load_headers).transpose()?;
    let policy = match policy_path {
        Some(path) => {
            let p = anchor_policy::load(path)?;
            if p.ots.as_ref().is_some_and(|o| o.required) && headers.is_none() {
                bail!(
                    "verify --policy requires --bitcoin-headers <file> when the policy's ots leg is \
                     required (a Bitcoin attestation is only a block height until its merkle root is \
                     checked against a header)"
                );
            }
            if !anchor_policy::openssl_available() {
                bail!(
                    "verify --policy requires the openssl CLI to check countersignatures under \
                     each TSA's CA (not found on PATH)"
                );
            }
            for entry in &p.tsas {
                ca_path_checked(entry)?;
            }
            Some((p, path))
        }
        None => None,
    };
    let checks_countersignature = !cas.is_empty() || policy.is_some();

    let threshold_s = policy
        .as_ref()
        .map(|(p, _)| p.max_clock_offset_s)
        .unwrap_or(clock_provenance::ONE_BUCKET_S);
    // The anchors table, the OpenTimestamps rows and the clock-provenance
    // ledger are read inside ONE read transaction. witnessd may insert an
    // anchor row and seal its record between two separate reads, and a record
    // read without its row would be reported after the loop as naming a
    // deleted anchor. The database is in WAL mode, so the snapshot blocks no
    // writer; it ends (a no-op rollback) when `snapshot` drops at the end of
    // this block.
    let (anchors, ots_rows, mut clock) = {
        let snapshot = conn.unchecked_transaction()?;
        // A database no writer has touched has no anchors table; it reads as
        // zero rows, never as a table to create. Under --policy the coverage
        // block below still runs over the ledgers themselves, so an absent
        // table and an empty one reach the same per-subject verdicts (an empty
        // ledger has nothing to cover; a non-empty one is uncovered).
        let anchors = if tsa::anchor_table_exists(&snapshot)? {
            tsa::list_anchors(&snapshot)?
        } else {
            Vec::new()
        };
        let ots_rows = ots::list_ots_anchors_lenient(&snapshot)?;
        if anchors.is_empty() {
            if ots_rows.is_empty() {
                println!("no anchors stored");
            } else {
                println!("no RFC 3161 anchors stored");
            }
        }
        #[cfg(test)]
        tests::between_the_two_reads();
        // The clock-provenance ledger, whole, before any anchor: its chain and
        // device signatures under the key lineage this database names. The
        // per-anchor clock lines below only mean something over a ledger that
        // holds together.
        let clock = ClockContext::load(&snapshot, threshold_s)?;
        (anchors, ots_rows, clock)
    };
    if anchors.is_empty() && ots_rows.is_empty() && policy.is_none() {
        let orphans = clock.finish(&anchors);
        if !clock.ledger_ok {
            bail!("clock provenance ledger failed verification");
        }
        if orphans > 0 {
            bail!(
                "{orphans} clock-provenance record(s) name anchors that were deleted or replaced"
            );
        }
        return Ok(());
    }

    let mut failures = 0usize;
    let mut unverified = 0usize;
    // OK rows, for the distinctness lines and the policy evaluation.
    let mut ok_heads: BTreeMap<(String, [u8; 32]), BTreeSet<String>> = BTreeMap::new();
    let mut head_order: Vec<(String, [u8; 32])> = Vec::new();
    let mut attributed: Vec<(usize, String)> = Vec::new();

    for (idx, a) in anchors.iter().enumerate() {
        let mut problems: Vec<String> = Vec::new();

        // 1. Structural: stored subject hash must be what the token covers …
        match tsa::parse_token_imprint(&a.token_der) {
            Ok(imprint) if imprint == a.subject_hash => {}
            Ok(imprint) => problems.push(format!(
                "token covers {} but anchor row claims {}",
                hex::encode(imprint),
                hex::encode(a.subject_hash)
            )),
            Err(e) => problems.push(format!("token unparseable: {e}")),
        }

        // 2. Cached identity vs the token (the token is the authority). A
        //    cache the token can no longer support is a contradiction too:
        //    a row that remembers a certificate its token does not embed,
        //    or a signer its token no longer names, must not pass as if the
        //    two agreed.
        let signer_res = tsa::parse_token_signer(&a.token_der);
        let signer = signer_res.as_ref().ok();
        let token_fp = signer.and_then(|s| s.signer_fingerprint.map(hex::encode));
        // The cache columns are text this tool wrote as lowercase hex
        // SHA-256; a cell that is anything else was not written by it and is
        // its own contradiction, reported before any comparison rather than
        // fed into one (or into a byte slice that a multibyte character
        // would make panic mid-run, with no verdict at all).
        let mut row_fp = a.signer_fingerprint.as_deref();
        let mut row_sid = a.signer_sid.as_deref();
        for (what, slot) in [("cert sha256:", &mut row_fp), ("sid:", &mut row_sid)] {
            if let Some(v) = *slot {
                if !is_hex64(v) {
                    problems.push(format!(
                        "row records a malformed signer cache ({what}{v:?}); expected 64 \
                         lowercase hex characters"
                    ));
                    *slot = None;
                }
            }
        }
        match &signer_res {
            Err(e) if row_fp.is_some() || row_sid.is_some() => {
                let (what, cached) = match (row_fp, row_sid) {
                    (Some(fp), _) => ("cert sha256:", fp),
                    (None, Some(sid)) => ("sid:", sid),
                    (None, None) => unreachable!("guarded by the match arm"),
                };
                problems.push(format!(
                    "row records signer {what}{}… but the token's SignerInfo is not readable: {e}",
                    prefix16(cached)
                ));
            }
            Err(_) => {}
            Ok(s) => {
                if let Some(row_fp) = row_fp {
                    match &token_fp {
                        Some(tok_fp) if row_fp != tok_fp => problems.push(format!(
                            "row records signer cert sha256:{}… but the token embeds {}…",
                            prefix16(row_fp),
                            &tok_fp[..16]
                        )),
                        Some(_) => {}
                        None => problems.push(format!(
                            "row records signer cert sha256:{}… but the token embeds no certificate",
                            prefix16(row_fp)
                        )),
                    }
                }
                if let Some(row_sid) = row_sid {
                    if row_sid != s.sid_hex {
                        problems.push(format!(
                            "row records signer sid:{}… but the token carries {}…",
                            prefix16(row_sid),
                            &s.sid_hex[..16]
                        ));
                    }
                }
            }
        }

        // 3. Head kinds must be in the ledger they DECLARE.
        if let Some(kind) = a.subject_kind().filter(|k| k.is_head()) {
            if tsa::hash_in_ledger(conn, &a.subject_hash, kind)?.is_none() {
                problems.push(kind.not_in_history_label().to_string());
            }
        }

        // 4. Cryptographic: delegate the CMS countersignature to openssl.
        let mut under_label: Option<String> = None;
        if !cas.is_empty() {
            let mut last_err = String::new();
            for ca in cas {
                match anchor_policy::openssl_ts_verify(&a.token_der, &a.subject_hash, ca) {
                    Ok(()) => {
                        under_label = Some(ca.clone());
                        break;
                    }
                    Err(e) => last_err = e.to_string(),
                }
            }
            if under_label.is_none() {
                problems.push(format!(
                    "openssl ts -verify failed under every CA given ({} tried): {last_err}",
                    cas.len()
                ));
            }
        } else if let Some((p, _)) = &policy {
            let mut under: Vec<String> = Vec::new();
            for entry in &p.tsas {
                let ca = entry.ca.to_string_lossy();
                if anchor_policy::openssl_ts_verify(&a.token_der, &a.subject_hash, &ca).is_ok() {
                    under.push(entry.name.clone());
                }
            }
            let v = Verified { anchor: a, under };
            let attribution = anchor_policy::attribute(p, &v, token_fp.as_deref());
            let name = match attribution {
                Attribution::One(n) => {
                    under_label = Some(format!("{n}'s CA"));
                    Some(n)
                }
                Attribution::AmbiguousResolvedByPin(n) => {
                    under_label = Some(format!("{n}'s CA, attributed to {n} by cert_sha256 pin"));
                    Some(n)
                }
                Attribution::Ambiguous(names) => {
                    problems.push(format!(
                        "verifies under the CA of both {} — their CA bundles overlap and no \
                         cert_sha256 pin singles one out; add cert_sha256 pins to disambiguate, \
                         or use leaf-issuer CA files",
                        names.join(" and ")
                    ));
                    None
                }
                Attribution::None => {
                    problems.push(format!(
                        "countersignature verifies under none of the policy CAs ({})",
                        p.tsas
                            .iter()
                            .map(|e| e.name.as_str())
                            .collect::<Vec<_>>()
                            .join(", ")
                    ));
                    None
                }
            };
            if let Some(n) = &name {
                if let Some(declared) = &a.tsa_name {
                    if declared != n {
                        problems.push(format!(
                            "declared TSA '{declared}' but the countersignature attributes to '{n}'"
                        ));
                    }
                }
                if let Some(entry) = p.entry(n) {
                    if !entry.fingerprint_pins.is_empty() {
                        match &token_fp {
                            Some(c) if entry.fingerprint_pins.iter().any(|pin| pin == c) => {}
                            Some(c) => problems.push(format!(
                                "signer certificate sha256:{}… is not among the cert_sha256 pins \
                                 declared for '{n}'",
                                &c[..16]
                            )),
                            None => problems.push(format!(
                                "token embeds no signer certificate to check against the \
                                 cert_sha256 pins declared for '{n}'"
                            )),
                        }
                    }
                }
                if problems.is_empty() {
                    attributed.push((idx, n.clone()));
                }
            }
        }

        let membership = membership_text(a);
        let signer_text = signer_summary(&a.token_der);
        let line_ok = problems.is_empty() && under_label.is_some();
        if problems.is_empty() {
            if let Some(label) = under_label {
                println!(
                    "anchor #{}: OK (imprint matches, {}, countersignature OK under {}) genTime {} {}",
                    a.id, membership, label, a.gen_time, signer_text
                );
                if let Some(kind) = a.subject_kind().filter(|k| k.is_head()) {
                    let key = (kind.as_str().to_string(), a.subject_hash);
                    let entry = ok_heads.entry(key.clone()).or_default();
                    if policy.is_some() {
                        if let Some((_, n)) = attributed.last().filter(|(i, _)| *i == idx) {
                            entry.insert(n.clone());
                        }
                    } else if let Some(c) = &token_fp {
                        entry.insert(c.clone());
                    }
                    if !head_order.contains(&key) {
                        head_order.push(key);
                    }
                }
            } else {
                // Structural checks passed, but the CMS countersignature — the
                // step that makes the timestamp an *authority* rather than an
                // attacker-chosen string — was NOT checked. Never print "OK"
                // here: it reads as a verified anchor. genTime is untrusted
                // until --ca cryptographically verifies the token.
                unverified += 1;
                println!(
                    "anchor #{}: UNVERIFIED (imprint matches, {}; countersignature \
                     NOT checked — re-run with --ca) genTime {} (untrusted) {} (unverified)",
                    a.id, membership, a.gen_time, signer_text
                );
            }
        } else {
            failures += 1;
            println!("anchor #{}: FAIL", a.id);
            for p in problems {
                println!("    {p}");
            }
        }
        clock.print_for_anchor(conn, a, line_ok, policy.is_some())?;
    }
    let orphans = clock.finish(&anchors);

    // Distinctness: only OK rows, only head kinds.
    for key in &head_order {
        let set = &ok_heads[key];
        let n = set.len();
        let what = match (policy.is_some(), n) {
            (true, 1) => "policy entry",
            (true, _) => "policy entries",
            (false, 1) => "signer certificate",
            (false, _) => "signer certificates",
        };
        println!(
            "head {} {}…: countersigned by {n} distinct TSA {what}",
            key.0,
            &hex::encode(key.1)[..16]
        );
    }

    if !checks_countersignature && !anchors.is_empty() {
        println!(
            "\n{unverified} anchor(s) structurally consistent but CRYPTOGRAPHICALLY UNVERIFIED. \
             Their genTime is attacker-controllable until the countersignature is checked; do not \
             rely on them as trusted timestamps. For the full countersignature check pass\n  \
             --ca <tsa-ca.pem> or --policy <anchor-policy.json>   (runs: openssl ts -verify -digest <hash> -in <token> -CAfile …)"
        );
    }

    // OpenTimestamps rows, after the RFC 3161 rows and their lines.
    let ots_outcome = verify_ots_rows(conn, ots_rows, headers.as_ref(), TimeBucket::now_10min()?)?;
    failures += ots_outcome.failures;

    // Coverage under the policy.
    let mut policy_failure: Option<String> = None;
    if let Some((p, path)) = &policy {
        let heads: Vec<(AnchorSubject, Option<[u8; 32]>)> = p
            .subject_kinds()
            .into_iter()
            .map(|s| tsa::ledger_head(conn, s).map(|h| (s, h)))
            .collect::<Result<_>>()?;
        let rows: Vec<anchor_policy::Attributed<'_>> = attributed
            .iter()
            .map(|(i, n)| anchor_policy::Attributed {
                anchor: &anchors[*i],
                name: n.clone(),
            })
            .collect();
        let anchor_now = format!(
            "  anchor now: log_anchor --db {db} anchor-all --policy {}",
            path.display()
        );
        let mut uncovered = 0usize;
        let mut not_current = 0usize;
        for c in anchor_policy::evaluate(p, &heads, &rows) {
            match c.verdict {
                CoverageVerdict::Empty => {
                    println!("policy: {}: ledger is empty; nothing to cover", c.subject)
                }
                CoverageVerdict::Covered {
                    hash,
                    by,
                    anchor_ids,
                    bucket_start,
                    is_current,
                } => {
                    let ids = anchor_ids
                        .iter()
                        .map(|i| format!("#{i}"))
                        .collect::<Vec<_>>()
                        .join(", ");
                    // The clock-provenance ledger grows by one record per
                    // anchor row witnessd observes, this run's own rows
                    // included, so its head always moves past what a run
                    // anchored. It reads current when every record since
                    // seals a later anchor row (`docs/timestamping.md`).
                    let self_advanced = (!is_current
                        && c.subject == AnchorSubject::ClockProvenanceHead
                        && clock.ledger_ok)
                        .then(|| {
                            clock_provenance::advanced_only_by_later_anchors(&clock.rows, &hash)
                        })
                        .flatten();
                    let is_current = is_current || self_advanced.is_some();
                    let current = match self_advanced {
                        _ if !is_current => "NO (ledger advanced since)".to_string(),
                        Some(n) => format!(
                            "yes (advanced since only by {n} record(s) of later anchor rows; this \
                             ledger grows by one record per anchor row, this run's own included)"
                        ),
                        None => "yes".to_string(),
                    };
                    println!(
                        "policy: {}: covered by {} — anchors {ids}, bucket {}; current head: {current}",
                        c.subject,
                        roles_text(&by),
                        bucket_utc(bucket_start)
                    );
                    if !is_current {
                        not_current += 1;
                        if require_current {
                            println!("{anchor_now}");
                        }
                    }
                }
                CoverageVerdict::NotCovered {
                    anchored_by,
                    missing_roles,
                    distinct,
                } => {
                    uncovered += 1;
                    let by = if anchored_by.is_empty() {
                        "no attributed TSA".to_string()
                    } else {
                        format!("{} only", roles_text(&anchored_by))
                    };
                    let why = if !missing_roles.is_empty() {
                        format!(
                            "missing role(s): {}",
                            missing_roles
                                .iter()
                                .map(|r| r.as_str())
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    } else {
                        format!(
                            "fewer than {} distinct TSAs ({distinct})",
                            p.min_distinct_tsas
                        )
                    };
                    println!(
                        "policy: {}: NOT covered — anchored by {by}; {why}",
                        c.subject
                    );
                    println!("{anchor_now}");
                }
            }
        }
        // The OpenTimestamps leg: only header-confirmed rows cover;
        // `--require-current` does not apply (Bitcoin confirmation lags by
        // design), so `current head` is information only.
        let mut ots_unconfirmed = 0usize;
        let ots_required = p.ots.as_ref().is_some_and(|o| o.required);
        if p.ots.is_some() {
            let tail = if ots_required {
                ""
            } else {
                "; not required by this policy"
            };
            for c in anchor_policy::evaluate_ots(p, &heads, &ots_outcome.passed) {
                match c.verdict {
                    anchor_policy::OtsCoverageVerdict::Empty => {}
                    anchor_policy::OtsCoverageVerdict::Confirmed {
                        anchor_ids,
                        height,
                        is_current,
                        ..
                    } => {
                        let ids = anchor_ids
                            .iter()
                            .map(|i| format!("#{i}"))
                            .collect::<Vec<_>>()
                            .join(", ");
                        let current = if is_current {
                            "yes"
                        } else {
                            "NO (ledger advanced since)"
                        };
                        println!(
                            "policy: {}: ots confirmed in Bitcoin block {height} against the \
                             supplied header — ots anchors {ids}; current head: {current}{tail}",
                            c.subject
                        );
                    }
                    anchor_policy::OtsCoverageVerdict::NotConfirmed { pending, unchecked } => {
                        ots_unconfirmed += 1;
                        println!(
                            "policy: {}: ots NOT confirmed — {pending} pending, {unchecked} \
                             attested but not checked against a header{tail}",
                            c.subject
                        );
                    }
                }
            }
        }
        print_policy_note(path, p.ots.is_some());
        let breaches = clock.breaches;
        let ots_short = if ots_required { ots_unconfirmed } else { 0 };
        let unsatisfied =
            uncovered > 0 || (require_current && not_current > 0) || breaches > 0 || ots_short > 0;
        if unsatisfied {
            let current_part = if require_current {
                format!(", {not_current} not current")
            } else {
                String::new()
            };
            let clock_part = if breaches > 0 {
                format!(
                    ", {breaches} clock-offset breach(es) of max_clock_offset_s {}",
                    p.max_clock_offset_s
                )
            } else {
                String::new()
            };
            let ots_part = if ots_short > 0 {
                format!(", {ots_short} without a confirmed OpenTimestamps proof")
            } else {
                String::new()
            };
            let msg = format!(
                "anchor policy {}: NOT SATISFIED ({uncovered} subject(s) uncovered{current_part}{clock_part}{ots_part})",
                path.display()
            );
            // The verdict line goes to stdout either way, on the same stream
            // as SATISFIED and the per-subject lines; the bail below carries
            // it to stderr for the exit status.
            println!("{msg}");
            policy_failure = Some(msg);
        } else {
            println!("anchor policy {}: SATISFIED", path.display());
        }
    }

    if failures > 0 {
        bail!("{failures} anchor(s) failed verification");
    }
    if !clock.ledger_ok {
        bail!("clock provenance ledger failed verification");
    }
    if orphans > 0 {
        bail!("{orphans} clock-provenance record(s) name anchors that were deleted or replaced");
    }
    if let Some(msg) = policy_failure {
        bail!("{msg}");
    }
    Ok(())
}

/// What `verify` knows about the clock-provenance ledger while it walks the
/// anchors (`docs/design/clock_provenance.md`, "Verification"). Read-only
/// and key-free: the ledger is checked under the key lineage the database
/// itself names, so its line says self-consistent, never verified.
struct ClockContext {
    rows: Vec<ClockProvenanceRow>,
    /// `false` after the ledger line printed `FAIL`: the per-anchor clock
    /// lines are suppressed and the run bails after the loop.
    ledger_ok: bool,
    /// `max_clock_offset_s` from the policy, else one bucket (600 s).
    threshold_s: i64,
    /// Retained ClockSkew failure buckets, for the correlation notes.
    skew_buckets: Vec<i64>,
    /// Records that contradict the anchors table (anchor deleted or token
    /// replaced); each fails the run.
    orphans: usize,
    /// Under `--policy`: evaluated offsets beyond the threshold, plus
    /// seal-before-genTime findings beyond it on anchors whose own line is OK.
    breaches: usize,
}

impl ClockContext {
    fn load(conn: &Connection, threshold_s: i64) -> Result<Self> {
        let mut ctx = Self {
            rows: Vec::new(),
            ledger_ok: true,
            threshold_s,
            skew_buckets: Vec::new(),
            orphans: 0,
            breaches: 0,
        };
        let count = clock_provenance::row_count(conn)?;
        if count == 0 {
            println!("clock provenance: no records");
            return Ok(ctx);
        }
        match Self::walk_ledger(conn) {
            Ok(rows) => {
                println!(
                    "clock provenance: {} record(s), ledger self-consistent (chain and device \
                     signatures check under the key lineage this database names; identity \
                     unverified)",
                    rows.len()
                );
                ctx.rows = rows;
                ctx.skew_buckets = clock_provenance::clock_skew_buckets(conn)?;
            }
            Err(e) => {
                println!("clock provenance: FAIL ({e:#})");
                ctx.ledger_ok = false;
            }
        }
        Ok(ctx)
    }

    /// The court_export pattern: the database's pinned device key, every
    /// validated lineage key, the stored PQ key when there is one, Compat.
    fn walk_ledger(conn: &Connection) -> Result<Vec<ClockProvenanceRow>> {
        let genesis = witness_kernel::device_public_key_from_db(conn)
            .map_err(|e| anyhow!("could not read the database's device identity: {e}"))?;
        let keys = verify_helpers::lineage_verifying_keys(conn, &genesis)?;
        let pq = verify_helpers::load_pq_verifying_key(conn, None, None)?;
        let mut walked: Vec<(i64, [u8; 32])> = Vec::new();
        verify::verify_clock_provenance_with(
            conn,
            &keys,
            SignatureMode::Compat,
            pq.as_ref(),
            |id, entry_hash| walked.push((id, entry_hash)),
        )?;
        // Only the rows the walk checked: witnessd may have committed one
        // since, and an unchecked row must not ride on this verdict.
        clock_provenance::list_walked_rows(conn, &walked)
    }

    fn band(&self, is_beyond: bool) -> String {
        let (word, what) = (
            if is_beyond { "BEYOND" } else { "within" },
            if self.threshold_s == clock_provenance::ONE_BUCKET_S {
                "one bucket (600 s)".to_string()
            } else {
                format!("{} s (max_clock_offset_s)", self.threshold_s)
            },
        );
        format!("{word} {what}")
    }

    /// The clock lines under one anchor's block: the record's statement
    /// (steps 2, 5, 6, 8), the token cross-check (3), and the structural
    /// seal-before-genTime check on a chain-head anchor over a live sealed
    /// row (7), which needs no record at all.
    fn print_for_anchor(
        &mut self,
        conn: &Connection,
        a: &AnchorRecord,
        line_ok: bool,
        under_policy: bool,
    ) -> Result<()> {
        if self.ledger_ok {
            let row = clock_provenance::record_for_anchor(&self.rows, a.id).cloned();
            match row {
                None => println!(
                    "    clock: no record (anchored before clock provenance, or witnessd was not \
                     running to observe it)"
                ),
                Some(row) => {
                    let stored = clock_provenance::token_sha256_hex(&a.token_der);
                    if stored != row.record.token_sha256 {
                        self.orphans += 1;
                        println!(
                            "    clock: FAIL record #{} binds token sha256 {}… but the row now \
                             holds {}… — the token was replaced after the device bound it",
                            row.id,
                            prefix16(&row.record.token_sha256),
                            &stored[..16]
                        );
                    } else {
                        self.print_record(&row, a, under_policy);
                    }
                }
            }
        }
        // Step 7: structural, record or no record. The sealed row's bucket
        // start versus the token's time bounds how far ahead the device read
        // when it sealed the anchored row.
        if a.subject_kind() == Some(AnchorSubject::ChainHead) {
            let created_at: Option<i64> = conn
                .query_row(
                    "SELECT created_at FROM sealed_events WHERE entry_hash = ?1 ORDER BY id ASC \
                     LIMIT 1",
                    [a.subject_hash.as_slice()],
                    |row| row.get(0),
                )
                .optional()?;
            let gen_time = tsa::parse_token(&a.token_der)
                .ok()
                .and_then(|t| tsa::gen_time_unix(&t.gen_time));
            if let (Some(created_at), Some(gen_time)) = (created_at, gen_time) {
                if let Some(n) = clock_provenance::seal_precedes_gen_time(created_at, gen_time) {
                    if n > self.threshold_s {
                        println!(
                            "    WARNING: the anchored sealed row's bucket starts {n} s after the \
                             TSA genTime: the device clock read at least {n} s ahead when it sealed \
                             that row ({})",
                            self.band(true)
                        );
                        if under_policy && line_ok {
                            self.breaches += 1;
                        }
                    } else {
                        println!(
                            "    note: the anchored sealed row's bucket starts {n} s after the TSA \
                             genTime: the device clock read at least {n} s ahead when it sealed \
                             that row"
                        );
                    }
                }
            }
        }
        Ok(())
    }

    fn print_record(&mut self, row: &ClockProvenanceRow, a: &AnchorRecord, under_policy: bool) {
        let rec = &row.record;
        let Some(offset) = rec.observed_offset_s else {
            println!(
                "    clock: record #{} binds this token (sha256 {}…); binding only, no \
                 measurement (row already present at the first poll of a witnessd process, or \
                 first seen more than 3600 s after the previous completed poll)",
                row.id,
                prefix16(&rec.token_sha256)
            );
            return;
        };
        let offset_text = clock_provenance::format_offset(offset);
        let Some(asmt) = clock_provenance::assess(rec, a) else {
            println!(
                "    clock: record #{}: device clock {offset_text} recorded, but genTime not \
                 readable from the token; not evaluated",
                row.id
            );
            return;
        };
        let gap = rec.poll_gap_s.unwrap_or(0);
        let stored = clock_provenance::stored_text(asmt.lag_s);
        if !(asmt.prompt || offset < 0) {
            println!(
                "    clock: record #{}: device clock {offset_text} vs TSA genTime; row {stored} \
                 (delayed import), positive offset not evaluated",
                row.id
            );
        } else {
            let is_beyond = clock_provenance::beyond(rec, &asmt, self.threshold_s);
            let when = if asmt.prompt {
                format!("row seen within {gap} s, {stored}")
            } else {
                format!(
                    "row seen within {gap} s, {stored} (delayed import; a negative offset is \
                     evaluated regardless)"
                )
            };
            println!(
                "    clock: record #{}: device clock {offset_text} vs TSA genTime ({when}); {}",
                row.id,
                self.band(is_beyond)
            );
            if is_beyond {
                if under_policy {
                    self.breaches += 1;
                } else {
                    println!(
                        "    WARNING: device clock offset beyond the structural threshold; with \
                         --policy this counts against the anchor policy"
                    );
                }
                let center = rec.time_bucket.start_epoch_s as i64;
                match clock_provenance::skew_within(
                    &self.skew_buckets,
                    center,
                    clock_provenance::ONE_BUCKET_S,
                ) {
                    Some(b) => println!(
                        "    note: a ClockSkew failure record is retained in bucket {b} (within \
                         one bucket of this reading)"
                    ),
                    None => println!(
                        "    note: no ClockSkew failure record retained within one bucket of this \
                         reading (retention prunes failure records)"
                    ),
                }
            }
        }
        if asmt.host_clock_disagrees {
            // A clamped offset gives a bound on the reading, so the gap is
            // printed as the inequality it is.
            let gap = if !asmt.clamped {
                format!("{:+}", asmt.host_gap_s)
            } else if offset > 0 {
                format!(">= {:+}", asmt.host_gap_s)
            } else {
                format!("<= {:+}", asmt.host_gap_s)
            };
            println!(
                "    note: host clock: the device's reading at observation is {gap} s from the \
                 anchor row's bucket start (expected 0 to 1199 s); the anchoring host's clock \
                 and the device clock disagree"
            );
        }
    }

    /// After the anchor loop: records whose anchor is gone (step 3), and the
    /// step check between consecutive measured records (step 9). Returns the
    /// orphan count.
    fn finish(&mut self, anchors: &[AnchorRecord]) -> usize {
        if !self.ledger_ok {
            return self.orphans;
        }
        for row in &self.rows {
            if !anchors.iter().any(|a| a.id == row.record.anchor_id) {
                self.orphans += 1;
                println!(
                    "clock provenance: FAIL record #{} names anchor #{}, which is not in the \
                     anchors table (deleted, or the table was rebuilt)",
                    row.id, row.record.anchor_id
                );
            }
        }
        // Only evaluated readings are compared: the gate print_record and
        // beyond() use (a readable genTime, and prompt or negative). A
        // delayed import's positive offset is not a clock reading, so it
        // can neither make nor hide a step.
        let measured: Vec<&ClockProvenanceRow> = self
            .rows
            .iter()
            .filter(|r| {
                let Some(offset) = r.record.observed_offset_s else {
                    return false;
                };
                anchors
                    .iter()
                    .find(|a| a.id == r.record.anchor_id)
                    .and_then(|a| clock_provenance::assess(&r.record, a))
                    .is_some_and(|asmt| asmt.prompt || offset < 0)
            })
            .collect();
        for pair in measured.windows(2) {
            let (a, b) = (pair[0], pair[1]);
            let (oa, ob) = (
                i64::from(a.record.observed_offset_s.unwrap_or(0)),
                i64::from(b.record.observed_offset_s.unwrap_or(0)),
            );
            let step = ob - oa;
            if step.abs() > clock_provenance::ONE_BUCKET_S {
                let (lo, hi) = (
                    a.record.time_bucket.start_epoch_s as i64,
                    b.record.time_bucket.start_epoch_s as i64,
                );
                match clock_provenance::skew_between(&self.skew_buckets, lo, hi) {
                    Some(bk) => println!(
                        "clock provenance: note: device clock offset stepped {step:+} s between \
                         records #{} and #{}; a ClockSkew failure record is retained between \
                         their buckets (bucket {bk})",
                        a.id, b.id
                    ),
                    None => println!(
                        "clock provenance: note: device clock offset stepped {step:+} s between \
                         records #{} and #{}; no ClockSkew failure record is retained between \
                         their buckets (retention prunes failure records)",
                        a.id, b.id
                    ),
                }
            }
        }
        self.orphans
    }
}

fn print_policy_note(path: &Path, has_ots: bool) {
    println!(
        "note: 'qualified' and 'independent' are the operator's declarations in {}; this tool \
         checks countersignatures, count and distinctness, not legal status.",
        path.display()
    );
    if has_ots {
        println!(
            "note: OpenTimestamps calendars are relays, not authorities; \"confirmed\" means the \
             proof's path reaches the merkle root of a Bitcoin block header you supplied — \
             existence before that block, not identity."
        );
    }
}

// -------------------- relabel --------------------

/// Downgrade a ledger-head row whose head was pruned before anchored heads
/// were preserved (or by an older pruner) to a `digest` anchor. The token is
/// untouched; the row simply stops asserting ledger membership. Refuses the
/// truncation shape — a head newer than the signed retention cutoff did not
/// go missing through retention.
fn relabel(conn: &Connection, id: i64, subject: &str) -> Result<()> {
    if subject != AnchorSubject::Digest.as_str() {
        bail!("relabel only downgrades a ledger-head anchor to digest (got '{subject}')");
    }
    let anchors = tsa::list_anchors(conn)?;
    let Some(a) = anchors.iter().find(|a| a.id == id) else {
        bail!("anchor #{id} not found");
    };
    let old = match a.subject_kind() {
        Some(kind) if kind.is_head() => kind,
        Some(_) => bail!("anchor #{id} is already a digest anchor; nothing to relabel"),
        None => bail!(
            "anchor #{id} has subject '{}', not a ledger head; nothing to relabel",
            a.subject
        ),
    };
    match tsa::parse_token_imprint(&a.token_der) {
        Ok(imprint) if imprint == a.subject_hash => {}
        _ => bail!("anchor #{id}: token does not cover the row's hash; fix the row's evidence problem first"),
    }
    if tsa::hash_in_ledger(conn, &a.subject_hash, old)?.is_some() {
        bail!(
            "anchor #{id} is in {} history; nothing to relabel",
            old.ledger_short()
        );
    }
    // Only retention removes chain rows legitimately, and only up to the
    // signed cutoff; receipt ledgers are never pruned at all. Anything else
    // is truncation or rollback — whatever the row's `ledger_id` says, and
    // especially when it says nothing (a legacy row): the decision is the
    // checkpoint's, not the row's.
    if old != AnchorSubject::ChainHead {
        bail!(
            "anchor #{id}: the {} is never pruned, so its missing head is truncation or \
             rollback, not a legacy prune; refusing to relabel",
            old.ledger_short()
        );
    }
    let cutoff = witness_kernel::verify::latest_checkpoint(conn)?.cutoff_event_id;
    match (cutoff, a.ledger_id) {
        (None, _) => bail!(
            "anchor #{id}: no retention checkpoint exists, so the anchored head did not go \
             missing through retention — that is truncation or rollback, not a legacy prune; \
             refusing to relabel"
        ),
        (Some(c), Some(ledger_id)) if ledger_id > c => bail!(
            "anchor #{id}: the anchored head (ledger row {ledger_id}) is newer than the \
             retention cutoff — that is truncation or rollback, not a legacy prune; refusing \
             to relabel"
        ),
        // A checkpoint exists and the row is at or below its cutoff, or is a
        // legacy row whose position was never recorded: the shape a prune
        // by an older build leaves.
        _ => {}
    }
    conn.execute(
        "UPDATE tsa_anchors SET subject = ?1 WHERE id = ?2",
        rusqlite::params![AnchorSubject::Digest.as_str(), id],
    )?;
    println!(
        "anchor #{id} relabeled {old} -> digest (the token is unchanged; ledger membership is no longer asserted)"
    );
    Ok(())
}

/// `relabel --ots`: the RFC 3161 refusals, for an OpenTimestamps row ("the
/// proof covers the row's hash" in place of the token). Never creates the
/// table.
fn relabel_ots(conn: &Connection, id: i64, subject: &str) -> Result<()> {
    if subject != AnchorSubject::Digest.as_str() {
        bail!("relabel only downgrades a ledger-head anchor to digest (got '{subject}')");
    }
    if !ots::ots_table_exists(conn)? {
        bail!("no ots_anchors table in this database");
    }
    let rows = ots::list_ots_anchors(conn)?;
    let Some(a) = rows.iter().find(|a| a.id == id) else {
        bail!("ots anchor #{id} not found");
    };
    let old = match a.subject_kind() {
        Some(kind) if kind.is_head() => kind,
        Some(_) => bail!("ots anchor #{id} is already a digest anchor; nothing to relabel"),
        None => bail!(
            "ots anchor #{id} has subject '{}', not a ledger head; nothing to relabel",
            a.subject
        ),
    };
    match ots::Proof::parse(&a.proof) {
        Ok(p) if p.digest == a.subject_hash => {}
        _ => bail!(
            "ots anchor #{id}: proof does not cover the row's hash; fix the row's evidence problem first"
        ),
    }
    if tsa::hash_in_ledger(conn, &a.subject_hash, old)?.is_some() {
        bail!(
            "ots anchor #{id} is in {} history; nothing to relabel",
            old.ledger_short()
        );
    }
    if old != AnchorSubject::ChainHead {
        bail!(
            "ots anchor #{id}: the {} is never pruned, so its missing head is truncation or \
             rollback, not a legacy prune; refusing to relabel",
            old.ledger_short()
        );
    }
    let cutoff = witness_kernel::verify::latest_checkpoint(conn)?.cutoff_event_id;
    match (cutoff, a.ledger_id) {
        (None, _) => bail!(
            "ots anchor #{id}: no retention checkpoint exists, so the anchored head did not go \
             missing through retention — that is truncation or rollback, not a legacy prune; \
             refusing to relabel"
        ),
        (Some(c), Some(ledger_id)) if ledger_id > c => bail!(
            "ots anchor #{id}: the anchored head (ledger row {ledger_id}) is newer than the \
             retention cutoff — that is truncation or rollback, not a legacy prune; refusing \
             to relabel"
        ),
        _ => {}
    }
    ots::relabel_ots_anchor_to_digest(conn, id)?;
    println!(
        "ots anchor #{id} relabeled {old} -> digest (the proof is unchanged; ledger membership is no longer asserted)"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use witness_kernel::{ClockObservation, Kernel, KernelConfig, ZonePolicy};

    thread_local! {
        static BETWEEN_THE_TWO_READS: RefCell<Option<Box<dyn Fn()>>> = const { RefCell::new(None) };
    }

    /// Test seam: runs between `verify`'s read of the anchors table and its
    /// walk of the clock-provenance ledger, where a writer may land.
    pub(super) fn between_the_two_reads() {
        BETWEEN_THE_TWO_READS.with(|hook| {
            if let Some(f) = hook.borrow().as_ref() {
                f();
            }
        });
    }

    fn test_cfg(db: &Path) -> KernelConfig {
        KernelConfig {
            db_path: db.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: std::time::Duration::from_secs(60 * 60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        }
    }

    /// The committed OpenSSL-generated token (`tests/fixtures/tsa/reply.tsr`).
    fn fixture_token() -> tsa::TimestampToken {
        let der = std::fs::read(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/fixtures/tsa/reply.tsr"
        ))
        .expect("reading the TSA fixture");
        tsa::parse_response(&der).expect("the fixture token parses")
    }

    /// One daemon poll with injected readings (never the wall clock): the
    /// number of records it sealed.
    fn observe(cfg: &KernelConfig, wall: i64, gap: Option<u32>) -> usize {
        let mut kernel = Kernel::open(cfg).expect("open kernel");
        kernel
            .observe_anchors_for_clock_provenance(&ClockObservation {
                observed_wall_s: wall as u64,
                poll_gap_s: gap,
                kernel_version: "0.0.0-test",
            })
            .expect("observe")
            .len()
    }

    /// A digest anchor over the fixture token's own imprint, through a
    /// connection of its own.
    fn anchor_the_fixture(cfg: &KernelConfig) -> i64 {
        let kernel = Kernel::open(cfg).expect("open kernel");
        tsa::ensure_anchor_table(&kernel.conn).expect("anchors table");
        let token = fixture_token();
        let imprint: [u8; 32] = token.imprint.clone().try_into().expect("32-byte imprint");
        tsa::insert_anchor(
            &kernel.conn,
            "digest",
            &imprint,
            "https://tsa.example/tsr",
            &token,
        )
        .expect("insert anchor")
    }

    /// `verify` reads the anchors table and the clock-provenance ledger from
    /// one snapshot: a row anchored and sealed between the two reads is
    /// neither a deleted anchor nor an orphan. Without the shared read
    /// transaction the record sealed in the seam names an anchor the earlier
    /// read never saw, and the run fails with "deleted or replaced".
    #[test]
    fn verify_reads_anchors_and_clock_ledger_from_one_snapshot() {
        let dir = tempfile::tempdir().unwrap();
        let db = dir.path().join("witness.db");
        let cfg = test_cfg(&db);
        let gen_time = tsa::gen_time_unix(&fixture_token().gen_time).expect("genTime");
        // The first poll only places the cursor; the next seals anchor #1's record.
        assert_eq!(observe(&cfg, gen_time, None), 0);
        anchor_the_fixture(&cfg);
        assert_eq!(observe(&cfg, gen_time + 5, Some(5)), 1);

        let writer_cfg = cfg.clone();
        BETWEEN_THE_TWO_READS.with(|hook| {
            *hook.borrow_mut() = Some(Box::new(move || {
                anchor_the_fixture(&writer_cfg);
                assert_eq!(observe(&writer_cfg, gen_time + 10, Some(5)), 1);
            }));
        });
        let reader = Kernel::open(&cfg).expect("open kernel");
        let result = verify(&reader.conn, &cfg.db_path, &[], None, false);
        BETWEEN_THE_TWO_READS.with(|hook| *hook.borrow_mut() = None);
        result.expect("verify over one snapshot passes");
        // Both rows and both records exist once the snapshot has ended.
        assert_eq!(tsa::list_anchors(&reader.conn).unwrap().len(), 2);
        assert_eq!(clock_provenance::row_count(&reader.conn).unwrap(), 2);
    }

    /// `anchor-all` over empty ledgers: every head kind is planned over its
    /// sentinel and STORED AS `digest`, so a sentinel row is never a head the
    /// full verifier would look for in a ledger (it is in none by
    /// construction).
    #[test]
    fn empty_ledger_sentinels_are_planned_as_digest_rows() {
        let conn = Connection::open_in_memory().unwrap();
        // The chain reader needs its two tables to exist; the receipt and
        // policy readers tolerate a missing table. All five ledgers are empty.
        conn.execute_batch(
            "CREATE TABLE sealed_events (id INTEGER PRIMARY KEY, entry_hash BLOB);
             CREATE TABLE checkpoints (id INTEGER PRIMARY KEY, chain_head_hash BLOB);",
        )
        .unwrap();
        let policy: AnchorPolicy = serde_json::from_str(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified",
                "url":"https://a.example/tsr","ca":"a.pem"}],
                "subjects":["chain_head","export_receipt_head","break_glass_receipt_head","policy_head",
                "clock_provenance_head"]}"#,
        )
        .unwrap();
        let planned = plan_subjects(&conn, &policy).unwrap();
        assert_eq!(planned.len(), AnchorSubject::HEADS.len());
        assert_eq!(planned.len(), 5);
        for (p, subject) in planned.iter().zip(AnchorSubject::HEADS) {
            assert_eq!(p.subject, subject);
            assert!(p.sentinel);
            assert_eq!(p.stored_as, AnchorSubject::Digest);
            assert_eq!(p.hash, subject.empty_ledger_sentinel().unwrap());
        }
    }
}
