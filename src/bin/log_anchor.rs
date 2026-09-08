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
//! `break_glass_receipt_head`, `policy_head`, or a bare `digest`
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
//! `list`, `verify`, `query` and `anchor-all --offline-dir` open the
//! database read-only and never create the anchors table (an observer
//! writes nothing); `request`, `import`, online `anchor-all` and `relabel`
//! open read-write.

use anyhow::{anyhow, bail, Context, Result};
use clap::{ArgAction, Parser, Subcommand};
use rusqlite::Connection;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use witness_kernel::anchor_policy::{self, AnchorPolicy, Attribution, CoverageVerdict, Verified};
use witness_kernel::tsa::{self, AnchorRecord, AnchorSubject};

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
    #[arg(long, env = "DEVICE_KEY_SEED", global = true)]
    device_key_seed: Option<String>,

    /// Explicit SQLCipher key (hex), overriding the seed derivation.
    #[arg(
        long,
        env = "SECURACV_DB_KEY",
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
        /// break_glass_receipt_head, policy_head
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
        /// break_glass_receipt_head, policy_head
        #[arg(long, value_name = "S", conflicts_with_all = ["digest", "file"])]
        subject: Option<String>,
        /// Anchor an explicit hex SHA-256 digest instead of a ledger head
        #[arg(long, value_name = "HEX", conflicts_with = "file")]
        digest: Option<String>,
        /// Anchor the SHA-256 of this file's bytes
        #[arg(long, value_name = "PATH")]
        file: Option<PathBuf>,
    },
    /// Import and store TSA responses obtained out-of-band
    Import {
        /// Path to a DER .tsr response (repeatable)
        #[arg(long, value_name = "PATH", action = ArgAction::Append, required = true)]
        response: Vec<String>,
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
        #[arg(long, requires = "policy")]
        require_current: bool,
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
    // device key seed exactly as the kernel does (same logic as log_verify).
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
        } => {
            let conn = open(&args, OpenMode::ReadOnly)?;
            query(
                &conn,
                out,
                subject.as_deref(),
                digest.as_deref(),
                file.as_deref(),
            )
        }
        Command::Import {
            response,
            url,
            policy,
            tsa,
        } => {
            let conn = open(&args, OpenMode::ReadWrite)?;
            tsa::ensure_anchor_table(&conn)?;
            import(
                &conn,
                response,
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
        } => {
            let conn = open(&args, OpenMode::ReadOnly)?;
            verify(&conn, &args.db, ca, policy.as_deref(), *require_current)
        }
        Command::Relabel { id, subject } => {
            let conn = open(&args, OpenMode::ReadWrite)?;
            tsa::ensure_anchor_table(&conn)?;
            relabel(&conn, *id, subject)
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
             break_glass_receipt_head, policy_head)"
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
        Ok(s) => match s.cert_sha256 {
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
    if failed > 0 {
        bail!("{failed} of {total} TSA request(s) failed");
    }
    Ok(())
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
) -> Result<()> {
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

    // The ledgers may have advanced since the query was written; the imprint
    // must match *some* recorded state of *some* ledger, not the current head.
    let kind = match tsa::classify_hash(conn, &hash)? {
        Some(loc) => loc.subject,
        None => {
            match AnchorSubject::sentinel_owner(&hash) {
                Some(owner) => eprintln!(
                    "note: imprint {}… is the empty-ledger sentinel for {owner}; stored as a digest anchor",
                    &hex::encode(hash)[..16]
                ),
                None => eprintln!(
                    "note: imprint {} is not in this DB's chain or receipt-ledger history; storing as a \
                     generic digest anchor (use this for export-bundle digests; a ledger head pruned by \
                     retention before import also lands here)",
                    hex::encode(hash)
                ),
            }
            AnchorSubject::Digest
        }
    };

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

fn import(
    conn: &Connection,
    responses: &[String],
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
    if failed > 0 {
        bail!("{failed} of {total} response(s) failed to import");
    }
    Ok(())
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
    if failed > 0 {
        bail!(
            "{failed} of {} TSA request(s) failed",
            planned.len() * targets.len()
        );
    }
    Ok(())
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
    println!(
        "then, back on this host, within the retention window ([retention], default 7 days — a head pruned before import can only be stored as a digest anchor):\n  log_anchor --db {db} import --policy {p} --response {d}/*.tsr\n  log_anchor --db {db} verify --policy {p}",
        p = policy_path.display(),
        d = dir.display()
    );
    Ok(())
}

// -------------------- list --------------------

fn list(conn: &Connection) -> Result<()> {
    if !tsa::anchor_table_exists(conn)? {
        println!("no anchors stored");
        return Ok(());
    }
    let anchors = tsa::list_anchors(conn)?;
    if anchors.is_empty() {
        println!("no anchors stored");
        return Ok(());
    }
    for a in anchors {
        // Identity comes from the row columns; when they are NULL (legacy
        // rows) it is re-derived from the token. Display only — `verify`
        // re-derives always.
        let signer = tsa::parse_token_signer(&a.token_der).ok();
        let cert = a
            .signer_cert_sha256
            .clone()
            .or_else(|| signer.as_ref().and_then(|s| s.cert_sha256.map(hex::encode)));
        let sid = a
            .signer_sid
            .clone()
            .or_else(|| signer.as_ref().map(|s| s.sid_hex.clone()));
        let cn = signer.as_ref().and_then(|s| s.cert_subject_cn.clone());
        let signer_text = match (cert, sid) {
            (Some(c), _) => match cn {
                Some(cn) => format!("cert sha256:{}… ({cn})", &c[..16.min(c.len())]),
                None => format!("cert sha256:{}…", &c[..16.min(c.len())]),
            },
            (None, Some(s)) => format!("sid:{}…", &s[..16.min(s.len())]),
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
    Ok(())
}

// -------------------- verify --------------------

/// The membership clause for a row: what was checked, and only that.
fn membership_text(a: &AnchorRecord) -> String {
    match a.subject_kind() {
        Some(AnchorSubject::Digest) => match AnchorSubject::sentinel_owner(&a.subject_hash) {
            Some(owner) => {
                format!("empty-ledger sentinel for {owner}, chain membership not applicable")
            }
            None => AnchorSubject::Digest.membership_label().to_string(),
        },
        Some(kind) => kind.membership_label().to_string(),
        None => format!(
            "unknown subject '{}', treated as a digest anchor; ledger membership not asserted",
            a.subject
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

fn verify(
    conn: &Connection,
    db: &str,
    cas: &[String],
    policy_path: Option<&Path>,
    require_current: bool,
) -> Result<()> {
    let policy = match policy_path {
        Some(path) => {
            if !anchor_policy::openssl_available() {
                bail!(
                    "verify --policy requires the openssl CLI to check countersignatures under \
                     each TSA's CA (not found on PATH)"
                );
            }
            let p = anchor_policy::load(path)?;
            for entry in &p.tsas {
                ca_path_checked(entry)?;
            }
            Some((p, path))
        }
        None => None,
    };
    let checks_countersignature = !cas.is_empty() || policy.is_some();

    if !tsa::anchor_table_exists(conn)? {
        println!("no anchors stored");
        if let Some((p, path)) = &policy {
            for subject in p.subject_kinds() {
                println!("policy: {subject}: NOT covered — no anchors stored");
                println!(
                    "  anchor now: log_anchor --db {db} anchor-all --policy {}",
                    path.display()
                );
            }
            print_policy_note(path);
            bail!(
                "anchor policy {}: NOT SATISFIED ({} subject(s) uncovered)",
                path.display(),
                p.subjects.len()
            );
        }
        return Ok(());
    }
    let anchors = tsa::list_anchors(conn)?;
    if anchors.is_empty() && policy.is_none() {
        println!("no anchors stored");
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

        // 2. Cached identity vs the token (the token is the authority).
        let signer = tsa::parse_token_signer(&a.token_der).ok();
        let token_cert = signer.as_ref().and_then(|s| s.cert_sha256.map(hex::encode));
        if let (Some(row_cert), Some(tok_cert)) = (&a.signer_cert_sha256, &token_cert) {
            if row_cert != tok_cert {
                problems.push(format!(
                    "row records signer cert sha256:{}… but the token embeds {}…",
                    &row_cert[..16.min(row_cert.len())],
                    &tok_cert[..16]
                ));
            }
        }
        if let (Some(row_sid), Some(s)) = (&a.signer_sid, &signer) {
            if row_sid != &s.sid_hex {
                problems.push(format!(
                    "row records signer sid:{}… but the token carries {}…",
                    &row_sid[..16.min(row_sid.len())],
                    &s.sid_hex[..16]
                ));
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
            let attribution = anchor_policy::attribute(p, &v, token_cert.as_deref());
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
                    if !entry.cert_sha256.is_empty() {
                        match &token_cert {
                            Some(c) if entry.cert_sha256.iter().any(|pin| pin == c) => {}
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
                    } else if let Some(c) = &token_cert {
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
    }

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

    if !checks_countersignature {
        println!(
            "\n{unverified} anchor(s) structurally consistent but CRYPTOGRAPHICALLY UNVERIFIED. \
             Their genTime is attacker-controllable until the countersignature is checked; do not \
             rely on them as trusted timestamps. For the full countersignature check pass\n  \
             --ca <tsa-ca.pem> or --policy <anchor-policy.json>   (runs: openssl ts -verify -digest <hash> -in <token> -CAfile …)"
        );
    }

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
                    by,
                    anchor_ids,
                    bucket_start,
                    is_current,
                    ..
                } => {
                    let ids = anchor_ids
                        .iter()
                        .map(|i| format!("#{i}"))
                        .collect::<Vec<_>>()
                        .join(", ");
                    let current = if is_current {
                        "yes".to_string()
                    } else {
                        "NO (ledger advanced since)".to_string()
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
        print_policy_note(path);
        let unsatisfied = uncovered > 0 || (require_current && not_current > 0);
        if unsatisfied {
            let current_part = if require_current {
                format!(", {not_current} not current")
            } else {
                String::new()
            };
            policy_failure = Some(format!(
                "anchor policy {}: NOT SATISFIED ({uncovered} subject(s) uncovered{current_part})",
                path.display()
            ));
        } else {
            println!("anchor policy {}: SATISFIED", path.display());
        }
    }

    if failures > 0 {
        if let Some(msg) = &policy_failure {
            println!("{msg}");
        }
        bail!("{failures} anchor(s) failed verification");
    }
    if let Some(msg) = policy_failure {
        bail!("{msg}");
    }
    Ok(())
}

fn print_policy_note(path: &Path) {
    println!(
        "note: 'qualified' and 'independent' are the operator's declarations in {}; this tool \
         checks countersignatures, count and distinctness, not legal status.",
        path.display()
    );
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
    if let Some(ledger_id) = a.ledger_id {
        // Only retention removes chain rows legitimately, and only up to the
        // signed cutoff; receipt ledgers are never pruned at all. Anything
        // else is truncation or rollback.
        let cutoff = if old == AnchorSubject::ChainHead {
            witness_kernel::verify::latest_checkpoint(conn)?.cutoff_event_id
        } else {
            None
        };
        if cutoff.map(|c| ledger_id > c).unwrap_or(true) {
            bail!(
                "anchor #{id}: the anchored head (ledger row {ledger_id}) is newer than the \
                 retention cutoff — that is truncation or rollback, not a legacy prune; refusing \
                 to relabel"
            );
        }
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
