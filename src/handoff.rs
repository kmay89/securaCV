//! Per-file **handoff sidecars** (`spec/quorum_unseal_v2.md` §5): every act
//! that writes a disclosed file — `break_glass unseal`, the console unseal,
//! the drill, `export_events` — also writes `<file>.securacv-handoff.json`
//! beside it, so a downstream chain-of-custody system receives the file
//! together with the device-signed rows of the disclosure that released it.
//!
//! The sidecar is an **unsigned carrier**: canonical `securacv-cjson-v1`
//! bytes, no trailing newline, always mode 0600. It embeds the signed receipt
//! rows verbatim, references RFC 3161 tokens by their SHA-256, and labels
//! everything else *declared*. This module adds no signing domain: the two
//! `DOMAIN_*` strings below are hash domains for salted commitments.
//!
//! Invariant III: the sidecar's only time value is one 600 s bucket
//! (`created_bucket`). Row readers select by hash only and never read a row
//! id or `created_at`. Nothing here opens a socket (`--ca` runs the local
//! `openssl`). See `docs/handoff_sidecars.md` (operator and verifier guide)
//! and `docs/design/handoff_sidecars.md` (the decisions).

use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::Connection;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use zeroize::Zeroizing;

use crate::break_glass::{
    check_text_field, display_safe, Approval, BreakGlassOutcome, BreakGlassReceipt,
};
use crate::crypto::signatures::{
    SignatureMode, SignatureSet, DOMAIN_BREAK_GLASS_RECEIPT, DOMAIN_EXPORT_RECEIPT,
};
use crate::verify::RedemptionLedger;
use crate::{
    hash_entry, verify_entry_signature, ExportBundle, ExportReceipt, Kernel, StoredReceiptRow,
    TimeBucket,
};

/// The format literal every v1 sidecar carries.
pub const HANDOFF_FORMAT_V1: &str = "securacv-handoff:v1";
/// Appended to the disclosed file's path.
pub const HANDOFF_SUFFIX: &str = ".securacv-handoff.json";
/// Commitment domain for a declared recipient — a hash domain, NOT a signing
/// domain (33 bytes, so its length prefix is `21 00 00 00`).
pub const DOMAIN_HANDOFF_RECIPIENT: &str = "securacv:pwk:handoff-recipient:v1";
/// Commitment domain for the unsealed plaintext — a hash domain, NOT a
/// signing domain (34 bytes, so its length prefix is `22 00 00 00`).
pub const DOMAIN_UNSEALED_PLAINTEXT: &str = "securacv:pwk:unsealed-plaintext:v1";
/// Size cap on a sidecar a verifier will read.
pub const MAX_SIDECAR_BYTES: usize = 1 << 20;
/// The one bucket size a sidecar's `created_bucket` may have.
pub const HANDOFF_BUCKET_S: u32 = 600;

const KEY_ROTATION_REFUSAL: &str = "the rows of this disclosure were signed under two device keys \
     (a key rotation inside the token's bucket); this build cannot describe them in one handoff \
     sidecar";

// ---------------------------------------------------------------------------
// Recipient
// ---------------------------------------------------------------------------

/// Who receives a disclosure, as the operator DECLARES it. Never verified by
/// the device; committed (salted) into the signed record, named only in the
/// sidecar.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffRecipient {
    /// An Ed25519 public key (64 lowercase hex) the operator claims for the
    /// recipient. Never checked against anything.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<String>,
    /// The recipient's printed name.
    pub name: String,
    /// The recipient's organization.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub organization: Option<String>,
}

impl HandoffRecipient {
    /// Validate and normalize: trims, refuses an empty name, refuses control
    /// and text-direction characters (`check_text_field`), stores the key
    /// lowercase after checking it is 32 bytes of hex.
    pub fn new(name: &str, organization: Option<&str>, key: Option<&str>) -> Result<Self> {
        let name = name.trim();
        if name.is_empty() {
            return Err(anyhow!("recipient name cannot be empty"));
        }
        check_text_field("recipient name", name)?;
        let organization = match organization.map(str::trim).filter(|s| !s.is_empty()) {
            Some(org) => {
                check_text_field("recipient organization", org)?;
                Some(org.to_string())
            }
            None => None,
        };
        let key = match key.map(str::trim) {
            Some(k) => {
                let bytes = hex::decode(k).map_err(|e| {
                    anyhow!(
                        "recipient key is not valid hex ({}); it must be 32 bytes (Ed25519 \
                         public key, 64 hex)",
                        e
                    )
                })?;
                if bytes.len() != 32 {
                    return Err(anyhow!(
                        "recipient key must be 32 bytes (Ed25519 public key, 64 hex), got {}",
                        bytes.len()
                    ));
                }
                Some(k.to_ascii_lowercase())
            }
            None => None,
        };
        Ok(Self {
            key,
            name: name.to_string(),
            organization,
        })
    }

    /// Read a recipient file (`{"name": …, "organization"?: …, "key"?: …}`).
    /// The file's contents are never echoed in an error.
    pub fn from_file(path: &Path) -> Result<Self> {
        let bytes = std::fs::read(path)
            .map_err(|e| anyhow!("recipient file {}: {}", path.display(), e.kind()))?;
        let parsed: HandoffRecipient = serde_json::from_slice(&bytes).map_err(|e| {
            anyhow!(
                "recipient file {}: {} at line {} column {}",
                path.display(),
                serde_error_class(&e),
                e.line(),
                e.column()
            )
        })?;
        Self::new(
            &parsed.name,
            parsed.organization.as_deref(),
            parsed.key.as_deref(),
        )
    }

    /// The securacv-cjson-v1 bytes the recipient commitment is taken over:
    /// absent keys omitted, keys sorted, no whitespace (equal to
    /// `jq -cjS .recipient` over a sidecar).
    pub fn canonical_bytes(&self) -> Vec<u8> {
        let value = serde_json::to_value(self).expect("a recipient always serializes");
        crate::canonical_json::to_canonical_bytes(&value).expect("strings only")
    }

    fn label(&self) -> String {
        match &self.organization {
            Some(org) => format!("{} ({})", display_safe(&self.name), display_safe(org)),
            None => display_safe(&self.name),
        }
    }
}

/// A serde error described without quoting the input (a recipient file can
/// hold a name; nothing of it reaches a terminal or a journal).
fn serde_error_class(e: &serde_json::Error) -> &'static str {
    use serde_json::error::Category;
    match e.classify() {
        Category::Io => "could not be read",
        Category::Syntax => "is not valid JSON",
        Category::Data => {
            "does not match {\"name\": …, \"organization\"?: …, \"key\"?: …} (an unknown key, or \
             a missing or mistyped field)"
        }
        Category::Eof => "ends early",
    }
}

/// The recipient flags `break_glass unseal`, `export_events` and
/// `handoff_sidecar emit` flatten.
#[derive(clap::Args, Debug, Clone, Default)]
pub struct RecipientArgs {
    /// JSON file {"name": …, "organization"?: …, "key"?: …} naming who receives this
    /// disclosure. Preferred over the flags: it stays out of shell history.
    #[arg(
        long,
        value_name = "FILE",
        conflicts_with_all = ["recipient_name", "recipient_org", "recipient_key"]
    )]
    pub recipient_file: Option<PathBuf>,
    /// Who receives this disclosure (declared; committed, salted, into the signed record)
    #[arg(long, value_name = "NAME")]
    pub recipient_name: Option<String>,
    /// The recipient's organization (declared)
    #[arg(long, value_name = "ORG", requires = "recipient_name")]
    pub recipient_org: Option<String>,
    /// The recipient's Ed25519 public key, 64 hex (declared, not verified)
    #[arg(long, value_name = "HEX", requires = "recipient_name")]
    pub recipient_key: Option<String>,
}

impl RecipientArgs {
    /// The declared recipient, validated, or `None` when no flag was given.
    /// Run it before anything is spent or recorded.
    pub fn resolve(&self) -> Result<Option<HandoffRecipient>> {
        if let Some(path) = &self.recipient_file {
            return HandoffRecipient::from_file(path).map(Some);
        }
        match &self.recipient_name {
            Some(name) => HandoffRecipient::new(
                name,
                self.recipient_org.as_deref(),
                self.recipient_key.as_deref(),
            )
            .map(Some),
            None => Ok(None),
        }
    }
}

// ---------------------------------------------------------------------------
// Commitments
// ---------------------------------------------------------------------------

/// 32 fresh bytes from the OS RNG, one per commitment.
pub fn fresh_salt() -> [u8; 32] {
    rand::random::<[u8; 32]>()
}

fn commitment_hasher(domain: &str, salt: &[u8; 32]) -> Sha256 {
    let mut hasher = Sha256::new();
    hasher.update((domain.len() as u32).to_le_bytes());
    hasher.update(domain.as_bytes());
    hasher.update(salt);
    hasher
}

/// `SHA256(le32(len(domain)) ‖ domain ‖ salt ‖ body)`.
pub fn salted_commitment(domain: &str, salt: &[u8; 32], body: &[u8]) -> [u8; 32] {
    let mut hasher = commitment_hasher(domain, salt);
    hasher.update(body);
    hasher.finalize().into()
}

/// [`salted_commitment`] over a stream (large files).
pub fn salted_commitment_reader(
    domain: &str,
    salt: &[u8; 32],
    mut body: impl std::io::Read,
) -> Result<[u8; 32]> {
    let mut hasher = commitment_hasher(domain, salt);
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = body.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hasher.finalize().into())
}

/// The salted commitment to a declared recipient.
pub fn recipient_commitment(salt: &[u8; 32], r: &HandoffRecipient) -> [u8; 32] {
    salted_commitment(DOMAIN_HANDOFF_RECIPIENT, salt, &r.canonical_bytes())
}

/// The salted commitment to an unsealed plaintext.
pub fn plaintext_commitment(salt: &[u8; 32], plaintext: &[u8]) -> [u8; 32] {
    salted_commitment(DOMAIN_UNSEALED_PLAINTEXT, salt, plaintext)
}

// ---------------------------------------------------------------------------
// Paths and writes
// ---------------------------------------------------------------------------

/// `<file>.securacv-handoff.json`.
pub fn sidecar_path_for(file: &Path) -> PathBuf {
    let mut os = file.as_os_str().to_os_string();
    os.push(HANDOFF_SUFFIX);
    PathBuf::from(os)
}

/// A sidecar an earlier act left beside `file`, moved aside (same
/// directory, a hidden `.<name>.stale-<rand>` name) before anything is
/// burned or exported, so the sidecar path is free and an old sidecar is
/// never left beside new bytes.
///
/// It is restored to its path when the guard is dropped — every failure
/// before the new disclosure's sidecar exists — because that sidecar holds
/// the only copy of an earlier disclosure's salts: a re-run with a spent or
/// wrong token must not crypto-shred the earlier unseal's signed
/// commitments. [`StaleSidecar::discard`] deletes it once the new bytes
/// have replaced the old ones. A restore never writes over something that
/// stands at the path by then (the moved-aside file is left where it is).
#[must_use = "dropping the guard restores the earlier sidecar; call discard() on success"]
#[derive(Debug)]
pub struct StaleSidecar {
    path: PathBuf,
    aside: Option<PathBuf>,
}

impl StaleSidecar {
    /// Whether an earlier sidecar was moved aside.
    pub fn moved(&self) -> bool {
        self.aside.is_some()
    }

    /// The new disclosure replaced the old one: delete the moved-aside
    /// sidecar instead of restoring it.
    pub fn discard(mut self) {
        if let Some(aside) = self.aside.take() {
            let _ = std::fs::remove_file(aside);
        }
    }
}

impl Drop for StaleSidecar {
    fn drop(&mut self) {
        if let Some(aside) = self.aside.take() {
            if std::fs::symlink_metadata(&self.path).is_err() {
                let _ = std::fs::rename(&aside, &self.path);
            }
        }
    }
}

/// Move a sidecar left beside `file` by an earlier act aside (see
/// [`StaleSidecar`]). A missing one is fine; a directory at the path, or a
/// move that fails, is an error the caller turns into a refusal before
/// anything is spent.
pub fn set_aside_stale_sidecar(file: &Path) -> Result<StaleSidecar> {
    let path = sidecar_path_for(file);
    let meta = match std::fs::symlink_metadata(&path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return Ok(StaleSidecar { path, aside: None })
        }
        Err(e) => return Err(anyhow!(e)),
    };
    if meta.is_dir() {
        return Err(anyhow!("a directory stands at that path"));
    }
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let aside = path.with_file_name(format!(".{}.stale-{:016x}", name, rand::random::<u64>()));
    std::fs::rename(&path, &aside)?;
    Ok(StaleSidecar {
        path,
        aside: Some(aside),
    })
}

/// Remove a sidecar left beside `file` by an earlier act. A missing one is
/// fine; any other failure is an error. The emitters use
/// [`set_aside_stale_sidecar`], which can restore it.
pub fn remove_stale_sidecar(file: &Path) -> Result<()> {
    let path = sidecar_path_for(file);
    match std::fs::remove_file(&path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(anyhow!(e)),
    }
}

/// Prove, before anything is spent, that a sidecar can be created beside
/// `file`: create the sidecar path the way [`write_sidecar_new`] does, then
/// remove it. A directory or filesystem that cannot take the sidecar is
/// refused while the token is still unspent.
pub fn probe_sidecar_path(file: &Path) -> Result<()> {
    let path = sidecar_path_for(file);
    let created = open_new_0600(&path).map_err(|e| anyhow!(e))?;
    drop(created);
    std::fs::remove_file(&path)?;
    Ok(())
}

fn open_new_0600(path: &Path) -> std::io::Result<std::fs::File> {
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    options.open(path)
}

/// Write `bytes` to `path` at mode 0600 without ever clobbering or following
/// a symlink: the final path itself is opened `O_CREAT|O_EXCL`
/// (`create_new`), which fails on any existing path, a dangling or live
/// symlink included. No hard link is needed, so this works on FAT/exFAT and
/// network mounts that refuse `link(2)`. A failed write or sync removes the
/// partial file this call created.
pub fn write_sidecar_new(path: &Path, bytes: &[u8]) -> Result<()> {
    use std::io::Write;
    let mut file = open_new_0600(path).map_err(|e| {
        if e.kind() == std::io::ErrorKind::AlreadyExists {
            anyhow!(
                "a file already exists at {}; a handoff sidecar is never written over another \
                 file",
                path.display()
            )
        } else {
            anyhow!(e)
        }
    })?;
    let written = file.write_all(bytes).and_then(|()| file.sync_all());
    drop(file);
    if let Err(e) = written {
        let _ = std::fs::remove_file(path);
        return Err(e.into());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// The sidecar object
// ---------------------------------------------------------------------------

/// Which act released the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Disclosure {
    /// A quorum unseal of a vault envelope.
    Unseal,
    /// An event export (bundle).
    Export,
}

/// What binds this file to the disclosure.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Pairing {
    /// An embedded `Unsealed` record plus `salts.plaintext` open over the file.
    PlaintextCommitment,
    /// The file is the bundle whose receipt is the embedded export record.
    ExportReceipt,
    /// Nothing signed this sidecar can open binds the file; the writer says so.
    Declared,
}

/// How the sidecar's recipient relates to the signed records.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RecipientBinding {
    /// The sidecar opens a signed recipient commitment.
    ReceiptCommitment,
    /// A signed row commits to a recipient this sidecar does not name.
    CommittedNotDisclosed,
    /// The sidecar names a recipient no signed row commits to.
    Declared,
    /// No recipient.
    None,
}

/// The role of one embedded record.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RecordRole {
    /// The `Granted` break-glass receipt (trustee consent).
    Grant,
    /// The `TokenRedeemed` row that spent its token.
    Redemption,
    /// The `Unsealed` row (the device decrypted the envelope).
    Unsealed,
    /// The export receipt.
    ExportReceipt,
}

impl RecordRole {
    fn label(self) -> &'static str {
        match self {
            RecordRole::Grant => "grant",
            RecordRole::Redemption => "redemption",
            RecordRole::Unsealed => "unsealed",
            RecordRole::ExportReceipt => "export_receipt",
        }
    }
}

/// What an anchor token covers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AnchorCovers {
    /// The file's SHA-256.
    FileBytes,
    /// The disclosure record's entry hash.
    DisclosureRecord,
}

impl AnchorCovers {
    fn label(self) -> &'static str {
        match self {
            AnchorCovers::FileBytes => "the file bytes",
            AnchorCovers::DisclosureRecord => "the disclosure record",
        }
    }
    fn target(self) -> &'static str {
        match self {
            AnchorCovers::FileBytes => "file's sha256",
            AnchorCovers::DisclosureRecord => "disclosure record's entry hash",
        }
    }
}

/// One RFC 3161 anchor reference (`kind: "rfc3161"`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rfc3161AnchorRef {
    /// What the token covers.
    pub covers: AnchorCovers,
    /// Always `"rfc3161"`.
    pub kind: String,
    /// The hash the token's imprint must equal (64 hex).
    pub subject_hash: String,
    /// SHA-256 of the token's DER (64 hex); the token itself travels apart.
    pub token_sha256: String,
}

/// The disclosed file.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffFile {
    /// Length in bytes.
    pub bytes: u64,
    /// Basename only.
    pub name: String,
    /// SHA-256 (64 hex).
    pub sha256: String,
}

/// One embedded, device-signed receipt row, verbatim.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffRecord {
    /// The stored approvals JSON (break-glass rows only).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub approvals_json: Option<String>,
    /// Chain hash (64 hex).
    pub entry_hash: String,
    /// `break_glass_receipts` or `export_receipts`; the verifier picks the
    /// signing domain from it.
    pub ledger: String,
    /// The stored payload, verbatim.
    pub payload_json: String,
    /// PQ scheme id, when stored.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pq_scheme: Option<String>,
    /// PQ signature (hex), when stored.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pq_signature: Option<String>,
    /// Previous chain hash (64 hex).
    pub prev_hash: String,
    /// The record's role in this disclosure.
    pub role: RecordRole,
    /// Ed25519 signature (128 hex).
    pub signature: String,
}

/// The opening material for the salted commitments.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffSalts {
    /// Opens `Unsealed.plaintext_commitment` (64 hex).
    pub plaintext: Option<String>,
    /// Opens the recipient commitment (64 hex).
    pub recipient: Option<String>,
}

/// A `securacv-handoff:v1` sidecar.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HandoffSidecar {
    /// Anchor references, parsed by `kind`.
    pub anchors: Vec<serde_json::Value>,
    /// The emission bucket (600 s) — the only time value the sidecar owns.
    pub created_bucket: TimeBucket,
    /// The one device key every record verifies under (64 hex).
    pub device_public_key: String,
    /// Which act released the file.
    pub disclosure: Disclosure,
    /// The disclosed file.
    pub file: HandoffFile,
    /// `securacv-handoff:v1`.
    pub format: String,
    /// What binds the file to the disclosure.
    pub pairing: Pairing,
    /// The declared recipient, if named.
    pub recipient: Option<HandoffRecipient>,
    /// How the recipient relates to the signed records.
    pub recipient_binding: RecipientBinding,
    /// The device-signed rows, in role order.
    pub records: Vec<HandoffRecord>,
    /// Opening material.
    pub salts: HandoffSalts,
    /// `CARGO_PKG_VERSION` of the writer (declared).
    pub tool_version: String,
}

impl HandoffSidecar {
    /// Canonical securacv-cjson-v1 bytes, no trailing newline.
    pub fn to_canonical_bytes(&self) -> Result<Vec<u8>> {
        crate::canonical_json::to_canonical_bytes(&serde_json::to_value(self)?)
    }

    /// Parse a sidecar: size cap, unknown keys refused, the format literal,
    /// a 600 s `created_bucket`, and canonical bytes (a pretty-printed or
    /// re-serialized copy is refused: re-emit, never edit).
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        let sidecar = parse_lenient(bytes).map_err(|e| anyhow!("{}", e))?;
        if sidecar.format != HANDOFF_FORMAT_V1 {
            return Err(anyhow!(
                "unsupported format '{}'",
                display_safe(&sidecar.format)
            ));
        }
        if !bucket_ok(sidecar.created_bucket) {
            return Err(anyhow!("created_bucket is not a 600 s bucket"));
        }
        if sidecar.to_canonical_bytes()? != bytes {
            return Err(anyhow!(NOT_CANONICAL));
        }
        Ok(sidecar)
    }
}

const NOT_CANONICAL: &str = "the handoff sidecar is not canonical securacv-cjson-v1 bytes \
     (re-emit it with handoff_sidecar emit; never edit or pretty-print it)";

fn parse_lenient(bytes: &[u8]) -> std::result::Result<HandoffSidecar, String> {
    if bytes.len() > MAX_SIDECAR_BYTES {
        return Err(format!(
            "unknown key or malformed: the sidecar is larger than the {} byte cap",
            MAX_SIDECAR_BYTES
        ));
    }
    serde_json::from_slice(bytes).map_err(|e| format!("unknown key or malformed: {}", e))
}

fn bucket_ok(bucket: TimeBucket) -> bool {
    bucket.size_s == HANDOFF_BUCKET_S
        && bucket
            .start_epoch_s
            .is_multiple_of(u64::from(HANDOFF_BUCKET_S))
}

/// The 600 s bucket containing `now_epoch_s`.
pub fn handoff_bucket(now_epoch_s: u64) -> TimeBucket {
    TimeBucket {
        start_epoch_s: now_epoch_s / u64::from(HANDOFF_BUCKET_S) * u64::from(HANDOFF_BUCKET_S),
        size_s: HANDOFF_BUCKET_S,
    }
}

/// The current 600 s bucket.
pub fn handoff_bucket_now() -> Result<TimeBucket> {
    TimeBucket::now(HANDOFF_BUCKET_S)
}

/// Remove the recipient from a sidecar (to forward it without the name):
/// `recipient` and `salts.recipient` become null and the binding is
/// recomputed (`receipt_commitment` → `committed_not_disclosed`,
/// `declared` → `none`).
pub fn redact(sidecar: &HandoffSidecar) -> HandoffSidecar {
    let mut out = sidecar.clone();
    out.recipient = None;
    out.salts.recipient = None;
    out.recipient_binding = match sidecar.recipient_binding {
        RecipientBinding::ReceiptCommitment | RecipientBinding::CommittedNotDisclosed => {
            RecipientBinding::CommittedNotDisclosed
        }
        RecipientBinding::Declared | RecipientBinding::None => RecipientBinding::None,
    };
    out
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

/// Which unseal disclosure `emit` describes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnsealSelector {
    /// By the `Unsealed` row's entry hash.
    UnsealRecord([u8; 32]),
    /// By the `Granted` receipt's entry hash.
    Grant([u8; 32]),
}

fn record_from_row(row: &StoredReceiptRow, role: RecordRole) -> HandoffRecord {
    HandoffRecord {
        approvals_json: row.approvals_json.clone(),
        entry_hash: hex::encode(row.entry_hash),
        ledger: row.ledger.to_string(),
        payload_json: row.payload_json.clone(),
        pq_scheme: row.pq_scheme.clone(),
        pq_signature: row.pq_signature.as_ref().map(hex::encode),
        prev_hash: hex::encode(row.prev_hash),
        role,
        signature: hex::encode(&row.signature),
    }
}

fn bg_row(
    conn: &Connection,
    h: &[u8; 32],
) -> Result<Option<(StoredReceiptRow, BreakGlassReceipt)>> {
    match crate::break_glass_row_by_entry_hash(conn, h)? {
        Some(row) => {
            let receipt: BreakGlassReceipt = serde_json::from_str(&row.payload_json)?;
            Ok(Some((row, receipt)))
        }
        None => Ok(None),
    }
}

/// Whether every record's device signature checks under `key`.
fn records_verify_under(records: &[HandoffRecord], key: &VerifyingKey) -> bool {
    records.iter().all(|r| check_record(r, key).is_ok())
}

/// Choose the one device key every record verifies under: `preferred`
/// first, then the database's lineage. Rows straddling a rotation are
/// refused.
fn pick_device_key(
    conn: &Connection,
    records: &[HandoffRecord],
    preferred: Option<VerifyingKey>,
) -> Result<VerifyingKey> {
    let mut candidates: Vec<VerifyingKey> = preferred.into_iter().collect();
    if let Ok(genesis) = crate::device_public_key_from_db(conn) {
        if let Ok(lineage) = crate::verify_helpers::lineage_verifying_keys(conn, &genesis) {
            candidates.extend(lineage.into_iter().rev());
        }
    }
    candidates
        .into_iter()
        .find(|key| records_verify_under(records, key))
        .ok_or_else(|| anyhow!(KEY_ROTATION_REFUSAL))
}

fn check_file_name(name: &str) -> Result<()> {
    if check_text_field("file name", name).is_err() {
        return Err(anyhow!(
            "the file name contains a control or text-direction character; rename the file \
             before emitting its handoff sidecar"
        ));
    }
    Ok(())
}

/// The recipient part of a sidecar, from the disclosure record's commitment
/// and what the caller declares. `recipient` is `(declared, salt)` where the
/// salt is known only to the process that made the commitment.
fn recipient_part(
    commitment: Option<[u8; 32]>,
    recipient: Option<(&HandoffRecipient, Option<[u8; 32]>)>,
    record_label: &str,
) -> Result<(Option<HandoffRecipient>, Option<String>, RecipientBinding)> {
    match (commitment, recipient) {
        (Some(c), Some((r, Some(salt)))) => {
            if recipient_commitment(&salt, r) != c {
                return Err(anyhow!(
                    "internal: the recipient salt does not open the signed {} commitment",
                    record_label
                ));
            }
            Ok((
                Some(r.clone()),
                Some(hex::encode(salt)),
                RecipientBinding::ReceiptCommitment,
            ))
        }
        (Some(_), Some((_, None))) => Err(anyhow!(
            "the signed {} commits to a recipient; its salt existed only in the original handoff \
             sidecar, so this tool cannot name a recipient beside it — omit the recipient flags \
             (the sidecar records the recipient as committed but not disclosed)",
            record_label
        )),
        (Some(_), None) => Ok((None, None, RecipientBinding::CommittedNotDisclosed)),
        (None, Some((r, _))) => Ok((Some(r.clone()), None, RecipientBinding::Declared)),
        (None, None) => Ok((None, None, RecipientBinding::None)),
    }
}

/// Describe an unseal: its grant, redemption and (when one exists) unseal
/// record, read by hash only. `plaintext_salt` is known only to the
/// unsealing process; without it (or without an unseal record) the pairing
/// is `declared`.
#[allow(clippy::too_many_arguments)]
pub fn build_unseal_sidecar(
    conn: &Connection,
    device_key: &VerifyingKey,
    unsealed_or_grant: UnsealSelector,
    file: &HandoffFile,
    plaintext_salt: Option<[u8; 32]>,
    recipient: Option<(&HandoffRecipient, Option<[u8; 32]>)>,
    created_bucket: TimeBucket,
    tool_version: &str,
) -> Result<HandoffSidecar> {
    build_unseal_sidecar_inner(
        conn,
        Some(*device_key),
        false,
        unsealed_or_grant,
        file,
        plaintext_salt,
        recipient,
        created_bucket,
        tool_version,
    )
}

#[allow(clippy::too_many_arguments)]
fn build_unseal_sidecar_inner(
    conn: &Connection,
    device_key: Option<VerifyingKey>,
    search_lineage: bool,
    selector: UnsealSelector,
    file: &HandoffFile,
    plaintext_salt: Option<[u8; 32]>,
    recipient: Option<(&HandoffRecipient, Option<[u8; 32]>)>,
    created_bucket: TimeBucket,
    tool_version: &str,
) -> Result<HandoffSidecar> {
    check_file_name(&file.name)?;
    let (grant_hash, redemption_hash, unsealed) = match selector {
        UnsealSelector::UnsealRecord(h) => {
            let Some((row, receipt)) = bg_row(conn, &h)? else {
                return Err(anyhow!(
                    "receipt {} is not an unseal record in this database",
                    hex::encode(h)
                ));
            };
            let Some((redemption, _, _)) = receipt.unsealed_record() else {
                return Err(anyhow!(
                    "receipt {} is not an unseal record in this database",
                    hex::encode(h)
                ));
            };
            let Some((_, red)) = bg_row(conn, &redemption)? else {
                return Err(anyhow!(
                    "receipt {} is not an unseal record in this database",
                    hex::encode(h)
                ));
            };
            let Some((_, grant)) = red.redeemed_token() else {
                return Err(anyhow!(
                    "receipt {} is not an unseal record in this database",
                    hex::encode(h)
                ));
            };
            (grant, redemption, Some((row, receipt)))
        }
        UnsealSelector::Grant(g) => {
            let granted =
                bg_row(conn, &g)?.filter(|(_, r)| matches!(r.outcome, BreakGlassOutcome::Granted));
            if granted.is_none() {
                return Err(anyhow!(
                    "no Granted receipt in this database has entry hash {}",
                    hex::encode(g)
                ));
            }
            let Some(redemption) = crate::break_glass_redemption_for_grant(conn, &g)? else {
                return Err(anyhow!(
                    "grant {} has no redemption row in this database (its token was never spent)",
                    hex::encode(g)
                ));
            };
            let unsealed = match crate::break_glass_unsealed_for_redemption(conn, &redemption)? {
                Some(u) => bg_row(conn, &u)?,
                None => None,
            };
            (g, redemption, unsealed)
        }
    };

    let Some((grant_row, grant)) = bg_row(conn, &grant_hash)? else {
        return Err(anyhow!(
            "no Granted receipt in this database has entry hash {}",
            hex::encode(grant_hash)
        ));
    };
    if !matches!(grant.outcome, BreakGlassOutcome::Granted) {
        return Err(anyhow!(
            "no Granted receipt in this database has entry hash {}",
            hex::encode(grant_hash)
        ));
    }
    let (redemption_row, redemption_receipt) =
        bg_row(conn, &redemption_hash)?.ok_or_else(|| {
            anyhow!(
                "grant {} has no redemption row in this database (its token was never spent)",
                hex::encode(grant_hash)
            )
        })?;
    if grant.vault_envelope_id == crate::EXPORT_EVENTS_ENVELOPE_ID
        || redemption_receipt.vault_envelope_id == crate::EXPORT_EVENTS_ENVELOPE_ID
    {
        return Err(anyhow!(
            "an export:events redemption is not an unseal — exports are receipted in the export \
             chain (emit the export's handoff sidecar with --file <bundle>)"
        ));
    }
    let mut records = vec![
        record_from_row(&grant_row, RecordRole::Grant),
        record_from_row(&redemption_row, RecordRole::Redemption),
    ];
    let mut commitment = None;
    let mut disclosure_hash = redemption_row.entry_hash;
    let mut pairing = Pairing::Declared;
    let mut salt_hex = None;
    if let Some((row, receipt)) = &unsealed {
        records.push(record_from_row(row, RecordRole::Unsealed));
        let (_, _, recip) = receipt.unsealed_record().expect("an unseal record");
        commitment = recip;
        disclosure_hash = row.entry_hash;
        if let Some(salt) = plaintext_salt {
            pairing = Pairing::PlaintextCommitment;
            salt_hex = Some(hex::encode(salt));
        }
    }
    let key = if search_lineage {
        pick_device_key(conn, &records, device_key)?
    } else {
        let key = device_key.ok_or_else(|| anyhow!("internal: no device key"))?;
        if !records_verify_under(&records, &key) {
            return Err(anyhow!(KEY_ROTATION_REFUSAL));
        }
        key
    };
    let (recipient, recipient_salt, binding) =
        recipient_part(commitment, recipient, "unseal record")?;
    let file_sha = hex32(&file.sha256)?;
    let anchors = select_anchors(conn, &file_sha, &disclosure_hash)?
        .into_iter()
        .map(|(v, _)| v)
        .collect();
    Ok(HandoffSidecar {
        anchors,
        created_bucket,
        device_public_key: hex::encode(key.to_bytes()),
        disclosure: Disclosure::Unseal,
        file: file.clone(),
        format: HANDOFF_FORMAT_V1.to_string(),
        pairing,
        recipient,
        recipient_binding: binding,
        records,
        salts: HandoffSalts {
            plaintext: salt_hex,
            recipient: recipient_salt,
        },
        tool_version: tool_version.to_string(),
    })
}

/// [`build_unseal_sidecar`] for `emit`: the device key is the one (from the
/// database's lineage) every record verifies under.
pub fn build_unseal_sidecar_from_db(
    conn: &Connection,
    selector: UnsealSelector,
    file: &HandoffFile,
    recipient: Option<&HandoffRecipient>,
    created_bucket: TimeBucket,
    tool_version: &str,
) -> Result<HandoffSidecar> {
    build_unseal_sidecar_inner(
        conn,
        None,
        true,
        selector,
        file,
        None,
        recipient.map(|r| (r, None)),
        created_bucket,
        tool_version,
    )
}

/// Describe an export: the bundle's receipt (located in this database's
/// export chain by entry hash) and, for a quorum export, its grant and
/// redemption.
pub fn build_export_sidecar(
    conn: &Connection,
    bundle_bytes: &[u8],
    file_name: &str,
    recipient: Option<(&HandoffRecipient, Option<[u8; 32]>)>,
    created_bucket: TimeBucket,
    tool_version: &str,
) -> Result<HandoffSidecar> {
    check_file_name(file_name)?;
    let bundle: ExportBundle = serde_json::from_slice(bundle_bytes)
        .map_err(|e| anyhow!("the file is not an export bundle: {}", e))?;
    let entry_hash = bundle.receipt_entry.entry_hash;
    let Some(export_row) = crate::export_row_by_entry_hash(conn, &entry_hash)? else {
        return Err(anyhow!(
            "the bundle's export receipt is not present in this database's export-receipt chain"
        ));
    };
    let receipt: ExportReceipt = serde_json::from_str(&export_row.payload_json)?;
    let mut records = Vec::new();
    if let Some(grant_hash) = receipt.authorizing_receipt {
        let Some((grant_row, grant)) = bg_row(conn, &grant_hash)? else {
            return Err(anyhow!(
                "no Granted receipt in this database has entry hash {}",
                hex::encode(grant_hash)
            ));
        };
        if !matches!(grant.outcome, BreakGlassOutcome::Granted) {
            return Err(anyhow!(
                "no Granted receipt in this database has entry hash {}",
                hex::encode(grant_hash)
            ));
        }
        let Some(redemption) = crate::break_glass_redemption_for_grant(conn, &grant_hash)? else {
            return Err(anyhow!(
                "grant {} has no redemption row in this database (its token was never spent)",
                hex::encode(grant_hash)
            ));
        };
        let (redemption_row, _) = bg_row(conn, &redemption)?
            .ok_or_else(|| anyhow!("internal: redemption row vanished"))?;
        records.push(record_from_row(&grant_row, RecordRole::Grant));
        records.push(record_from_row(&redemption_row, RecordRole::Redemption));
    }
    records.push(record_from_row(&export_row, RecordRole::ExportReceipt));
    let bundle_key = crate::crypto::signatures::verifying_key_strict(&bundle.device_public_key)
        .map_err(|e| anyhow!("invalid bundle device key: {}", e))?;
    if !records_verify_under(&records, &bundle_key) {
        return Err(anyhow!(KEY_ROTATION_REFUSAL));
    }
    let commitment = receipt
        .context
        .as_ref()
        .and_then(|c| c.recipient_commitment);
    let (recipient, recipient_salt, binding) =
        recipient_part(commitment, recipient, "export receipt")?;
    let file_sha: [u8; 32] = Sha256::digest(bundle_bytes).into();
    let anchors = select_anchors(conn, &file_sha, &entry_hash)?
        .into_iter()
        .map(|(v, _)| v)
        .collect();
    Ok(HandoffSidecar {
        anchors,
        created_bucket,
        device_public_key: hex::encode(bundle.device_public_key),
        disclosure: Disclosure::Export,
        file: HandoffFile {
            bytes: bundle_bytes.len() as u64,
            name: file_name.to_string(),
            sha256: hex::encode(file_sha),
        },
        format: HANDOFF_FORMAT_V1.to_string(),
        pairing: Pairing::ExportReceipt,
        recipient,
        recipient_binding: binding,
        records,
        salts: HandoffSalts {
            plaintext: None,
            recipient: recipient_salt,
        },
        tool_version: tool_version.to_string(),
    })
}

fn tsa_table_exists(conn: &Connection) -> Result<bool> {
    Ok(conn.query_row(
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'tsa_anchors'",
        [],
        |row| row.get::<_, i64>(0).map(|n| n > 0),
    )?)
}

/// The `tsa_anchors` rows whose subject is the file's SHA-256 or the
/// disclosure record's entry hash — selected by hash only, chain-head rows
/// included — each referenced only when the token's own imprint equals that
/// hash. Returns `(reference, token DER)`, sorted by `(covers, token_sha256)`
/// and de-duplicated. No row id, generation time or TSA URL is read.
pub fn select_anchors(
    conn: &Connection,
    file_sha256: &[u8; 32],
    disclosure_entry_hash: &[u8; 32],
) -> Result<Vec<(serde_json::Value, Vec<u8>)>> {
    if !tsa_table_exists(conn)? {
        return Ok(Vec::new());
    }
    let mut stmt = conn.prepare(
        "SELECT subject_hash, token_der FROM tsa_anchors WHERE subject_hash = ?1 OR subject_hash = ?2",
    )?;
    let rows: Vec<(Vec<u8>, Vec<u8>)> = stmt
        .query_map(
            rusqlite::params![file_sha256.to_vec(), disclosure_entry_hash.to_vec()],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )?
        .collect::<rusqlite::Result<_>>()?;
    let mut out: Vec<(AnchorCovers, String, serde_json::Value, Vec<u8>)> = Vec::new();
    for (subject, der) in rows {
        let Ok(imprint) = crate::tsa::parse_token_imprint(&der) else {
            continue;
        };
        if imprint.as_slice() != subject.as_slice() {
            continue;
        }
        let covers = if &imprint == file_sha256 {
            AnchorCovers::FileBytes
        } else {
            AnchorCovers::DisclosureRecord
        };
        let token_sha = hex::encode(Sha256::digest(&der));
        if out
            .iter()
            .any(|(c, t, _, _)| *c == covers && *t == token_sha)
        {
            continue;
        }
        let value = serde_json::to_value(Rfc3161AnchorRef {
            covers,
            kind: "rfc3161".to_string(),
            subject_hash: hex::encode(imprint),
            token_sha256: token_sha.clone(),
        })?;
        out.push((covers, token_sha, value, der));
    }
    out.sort_by(|a, b| {
        (a.0 == AnchorCovers::DisclosureRecord, &a.1)
            .cmp(&(b.0 == AnchorCovers::DisclosureRecord, &b.1))
    });
    Ok(out.into_iter().map(|(_, _, v, d)| (v, d)).collect())
}

// ---------------------------------------------------------------------------
// The shared unseal tail
// ---------------------------------------------------------------------------

/// What the shared unseal tail needs besides the cleartext.
pub struct UnsealRelease<'a> {
    /// Where the cleartext goes (`<dir>/<envelope>.raw`).
    pub raw_path: &'a Path,
    /// The `TokenRedeemed` row that spent the token.
    pub redemption_entry_hash: [u8; 32],
    /// The `Granted` receipt the token was minted from.
    pub granted_entry_hash: [u8; 32],
    /// The declared recipient, if any.
    pub recipient: Option<&'a HandoffRecipient>,
    /// The sidecar's 600 s bucket.
    pub created_bucket: TimeBucket,
    /// `CARGO_PKG_VERSION`.
    pub tool_version: &'a str,
}

/// The two writes of a release, behind a seam so the fail-closed order can
/// be tested.
pub trait ReleaseIo {
    /// Write the sidecar, create-new, 0600.
    fn write_sidecar(&mut self, path: &Path, bytes: &[u8]) -> Result<()>;
    /// Write the cleartext, 0600.
    fn write_raw(&mut self, path: &Path, bytes: &[u8]) -> Result<()>;
    /// Best-effort removal.
    fn remove(&mut self, path: &Path);
}

/// The filesystem [`ReleaseIo`].
pub struct FsReleaseIo;

impl ReleaseIo for FsReleaseIo {
    fn write_sidecar(&mut self, path: &Path, bytes: &[u8]) -> Result<()> {
        write_sidecar_new(path, bytes)
    }
    fn write_raw(&mut self, path: &Path, bytes: &[u8]) -> Result<()> {
        crate::break_glass::backend::write_restricted(path, bytes)
    }
    fn remove(&mut self, path: &Path) {
        let _ = std::fs::remove_file(path);
    }
}

/// What a release wrote.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UnsealOutput {
    /// The cleartext file.
    pub raw_path: PathBuf,
    /// Its handoff sidecar.
    pub sidecar_path: PathBuf,
    /// SHA-256 of the cleartext.
    pub file_sha256: [u8; 32],
    /// Its length.
    pub bytes: u64,
    /// The `Unsealed` row's entry hash.
    pub unsealed_entry_hash: [u8; 32],
    /// Whether a recipient was named (and committed).
    pub recipient_named: bool,
}

/// The shared unseal tail (CLI, console, drill), after the burn and a
/// successful decrypt: record the `Unsealed` row, build and self-verify the
/// sidecar, write the sidecar, then the cleartext. Fail-closed: nothing is
/// written unless the record and the sidecar both exist first. `clear` is
/// zeroized on every path (callers hand it over already wrapped, straight
/// from the decrypt, so no early return before this call drops it
/// unzeroized); no salt and no recipient name ever enters an
/// error.
pub fn release_unsealed(
    kernel: &mut Kernel,
    clear: impl Into<Zeroizing<Vec<u8>>>,
    req: &UnsealRelease<'_>,
    io: &mut dyn ReleaseIo,
) -> Result<UnsealOutput> {
    let clear: Zeroizing<Vec<u8>> = clear.into();
    let file_sha256: [u8; 32] = Sha256::digest(&clear[..]).into();
    let bytes = clear.len() as u64;
    let plaintext_salt = Zeroizing::new(fresh_salt());
    let recipient_salt = req.recipient.map(|_| Zeroizing::new(fresh_salt()));
    let pc = plaintext_commitment(&plaintext_salt, &clear);
    let rc = match (req.recipient, recipient_salt.as_ref()) {
        (Some(r), Some(salt)) => Some(recipient_commitment(salt, r)),
        _ => None,
    };
    let unsealed_entry_hash = kernel
        .record_unsealed(req.redemption_entry_hash, pc, rc)
        .map_err(|e| {
            anyhow!(
                "the unseal could not be recorded in the receipt chain ({}); the cleartext was \
                 discarded and nothing was written. The token is spent — its redemption is \
                 recorded — so the trustees must approve a new request to unseal again.",
                e
            )
        })?;
    let sidecar_path = sidecar_path_for(req.raw_path);
    let file = HandoffFile {
        bytes,
        name: req
            .raw_path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
        sha256: hex::encode(file_sha256),
    };
    let device_key = kernel.device_verifying_key();
    let sidecar = build_unseal_sidecar(
        &kernel.conn,
        &device_key,
        UnsealSelector::UnsealRecord(unsealed_entry_hash),
        &file,
        Some(*plaintext_salt),
        req.recipient
            .map(|r| (r, recipient_salt.as_ref().map(|s| **s))),
        req.created_bucket,
        req.tool_version,
    )
    .map_err(|e| {
        anyhow!(
            "internal: the handoff sidecar could not be built ({}); nothing was written",
            e
        )
    })?;
    if sidecar
        .records
        .iter()
        .all(|r| r.role != RecordRole::Unsealed)
        || sidecar.records[0].entry_hash != hex::encode(req.granted_entry_hash)
    {
        return Err(anyhow!(
            "internal: the handoff sidecar did not verify (the records do not name this \
             redemption's grant); nothing was written"
        ));
    }
    let sidecar_bytes = Zeroizing::new(sidecar.to_canonical_bytes()?);
    let report = verify_core(
        &sidecar_bytes,
        FileInput::Bytes(&clear),
        &VerifyOptions {
            pinned: Some(device_key),
            anchors_dir: None,
            tsa_cas: &[],
        },
        None,
    );
    if report.verdict() != "valid" {
        let first = report
            .first_failure()
            .map(|c| format!("{}: {}", c.subject, c.detail))
            .unwrap_or_else(|| "identity not pinned".to_string());
        return Err(anyhow!(
            "internal: the handoff sidecar did not verify ({}); nothing was written",
            first
        ));
    }
    io.write_sidecar(&sidecar_path, &sidecar_bytes)
        .map_err(|e| {
            anyhow!(
                "the unseal is recorded (unseal record {}…) but its handoff sidecar could not be \
             written to {} ({}); the cleartext was discarded and nothing was written, because the \
             salt that opens the signed record exists only in that sidecar. The token is spent — \
             the trustees must approve a new request to unseal again.",
                &hex::encode(unsealed_entry_hash)[..16],
                sidecar_path.display(),
                e
            )
        })?;
    if let Err(e) = io.write_raw(req.raw_path, &clear) {
        io.remove(&sidecar_path);
        return Err(anyhow!(
            "the cleartext could not be written to {} ({}); nothing was released and its handoff \
             sidecar was removed. The unseal record stays in the receipt chain (the device did \
             decrypt the envelope). The token is spent — the trustees must approve a new request \
             to unseal again.",
            req.raw_path.display(),
            e
        ));
    }
    Ok(UnsealOutput {
        raw_path: req.raw_path.to_path_buf(),
        sidecar_path,
        file_sha256,
        bytes,
        unsealed_entry_hash,
        recipient_named: req.recipient.is_some(),
    })
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

/// One check's outcome. `Declared` and `Unverified` never fail a verdict.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CheckStatus {
    /// Checked and holds.
    Ok,
    /// Checked and does not hold.
    Fail,
    /// Not checked here (the line says how to check it).
    Unverified,
    /// Declared by the writer; nothing signed backs it.
    Declared,
}

impl CheckStatus {
    /// The printed label.
    pub fn label(self) -> &'static str {
        match self {
            CheckStatus::Ok => "OK",
            CheckStatus::Fail => "FAIL",
            CheckStatus::Unverified => "UNVERIFIED",
            CheckStatus::Declared => "DECLARED",
        }
    }
}

/// One printed check line.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Check {
    /// Outcome.
    pub status: CheckStatus,
    /// What was checked (`format`, `file`, `record grant`, …).
    pub subject: String,
    /// The detail, display-safe.
    pub detail: String,
}

impl Check {
    /// `format!("{:<10} {}: {}", STATUS, subject, detail)`.
    pub fn line(&self) -> String {
        format!(
            "{:<10} {}: {}",
            self.status.label(),
            self.subject,
            self.detail
        )
    }
}

/// The result of checking one sidecar.
#[derive(Clone, Debug, Default)]
pub struct SidecarReport {
    /// Every check, in order.
    pub checks: Vec<Check>,
    /// Whether the device key was anchored out of band (a pinned key).
    pub identity_pinned: bool,
    /// Whether the deep database checks ran.
    pub db_checked: bool,
}

impl SidecarReport {
    /// FAIL count.
    pub fn failed(&self) -> usize {
        self.checks
            .iter()
            .filter(|c| c.status == CheckStatus::Fail)
            .count()
    }
    /// DECLARED count.
    pub fn declared(&self) -> usize {
        self.checks
            .iter()
            .filter(|c| c.status == CheckStatus::Declared)
            .count()
    }
    /// `VerifyReport.verdict`'s three literals.
    pub fn verdict(&self) -> &'static str {
        if self.failed() > 0 {
            "verification failed"
        } else if self.identity_pinned {
            "valid"
        } else {
            "self-consistent; identity unverified"
        }
    }
    /// The closing line `handoff_sidecar verify` prints.
    pub fn verdict_line(&self) -> String {
        match self.verdict() {
            "valid" => format!(
                "OK: handoff sidecar checked — VALID (identity anchored out-of-band). {} item(s) \
                 are declared, not attested by the device.",
                self.declared()
            ),
            "verification failed" => format!(
                "FAIL: handoff sidecar verification failed — {} check(s) failed.",
                self.failed()
            ),
            _ => format!(
                "OK: handoff sidecar checked — SELF-CONSISTENT, IDENTITY UNVERIFIED. The device \
                 key came from the sidecar itself; pass --public-key-file <device.pub> obtained \
                 out of band for an evidentiary verdict. {} item(s) are declared, not attested by \
                 the device.",
                self.declared()
            ),
        }
    }
    /// The first FAIL, if any.
    pub fn first_failure(&self) -> Option<&Check> {
        self.checks.iter().find(|c| c.status == CheckStatus::Fail)
    }
    fn push(&mut self, status: CheckStatus, subject: impl Into<String>, detail: impl Into<String>) {
        self.checks.push(Check {
            status,
            subject: subject.into(),
            detail: detail.into(),
        });
    }
}

/// What a verifier was given besides the sidecar and the file.
#[derive(Clone, Copy, Debug, Default)]
pub struct VerifyOptions<'a> {
    /// The device key obtained out of band.
    pub pinned: Option<VerifyingKey>,
    /// A directory of token files (`sha256(file) == token_sha256` selects).
    pub anchors_dir: Option<&'a Path>,
    /// TSA CA certificates for `openssl ts -verify`.
    pub tsa_cas: &'a [String],
}

/// The file a sidecar is checked against.
pub(crate) enum FileInput<'a> {
    Path(&'a Path),
    Bytes(&'a [u8]),
}

impl FileInput<'_> {
    fn digest(&self) -> Result<([u8; 32], u64)> {
        match self {
            FileInput::Bytes(b) => Ok((Sha256::digest(b).into(), b.len() as u64)),
            FileInput::Path(p) => {
                let mut f = std::fs::File::open(p)?;
                let mut hasher = Sha256::new();
                let mut buf = vec![0u8; 64 * 1024];
                let mut n_total = 0u64;
                loop {
                    let n = f.read(&mut buf)?;
                    if n == 0 {
                        break;
                    }
                    n_total += n as u64;
                    hasher.update(&buf[..n]);
                }
                Ok((hasher.finalize().into(), n_total))
            }
        }
    }
    fn commitment(&self, domain: &str, salt: &[u8; 32]) -> Result<[u8; 32]> {
        match self {
            FileInput::Bytes(b) => Ok(salted_commitment(domain, salt, b)),
            FileInput::Path(p) => salted_commitment_reader(domain, salt, std::fs::File::open(p)?),
        }
    }
    fn read_all(&self) -> Result<Vec<u8>> {
        match self {
            FileInput::Bytes(b) => Ok(b.to_vec()),
            FileInput::Path(p) => Ok(std::fs::read(p)?),
        }
    }
}

/// Check a sidecar offline against the file it describes. Never fails on a
/// verification finding (that is a FAIL check); `Err` only when the file
/// cannot be read.
pub fn verify_sidecar(
    sidecar: &[u8],
    file: &Path,
    opts: &VerifyOptions<'_>,
) -> Result<SidecarReport> {
    std::fs::metadata(file).map_err(|e| anyhow!("cannot read {}: {}", file.display(), e))?;
    Ok(verify_core(sidecar, FileInput::Path(file), opts, None))
}

/// [`verify_sidecar`] over a file already in memory (an emitter's
/// self-check before it writes).
pub fn verify_sidecar_bytes(
    sidecar: &[u8],
    file: &[u8],
    opts: &VerifyOptions<'_>,
) -> SidecarReport {
    verify_core(sidecar, FileInput::Bytes(file), opts, None)
}

/// [`verify_sidecar`] plus the deep checks against the database the sidecar
/// came from (read-only): the key lineage, every embedded record
/// byte-identical to its row, both receipt chains end to end (with the
/// grant's quorum re-derived under its policy era), the export
/// authorizations, and tokens from `tsa_anchors`.
pub fn verify_sidecar_with_db(
    conn: &Connection,
    sidecar: &[u8],
    file: &Path,
    opts: &VerifyOptions<'_>,
) -> Result<SidecarReport> {
    std::fs::metadata(file).map_err(|e| anyhow!("cannot read {}: {}", file.display(), e))?;
    Ok(verify_core(
        sidecar,
        FileInput::Path(file),
        opts,
        Some(conn),
    ))
}

fn hex32(s: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(s).map_err(|e| anyhow!("not hex: {}", e))?;
    bytes.try_into().map_err(|_| anyhow!("not 32 bytes of hex"))
}

fn fp16(key: &VerifyingKey) -> String {
    hex::encode(key.to_bytes())[..16].to_string()
}

fn h16(s: &str) -> String {
    display_safe(s.get(..16).unwrap_or(s))
}

/// Re-derive one record's entry hash and check its Ed25519 signature under
/// `key`, in the domain the LEDGER implies (never read from the sidecar).
fn check_record(
    record: &HandoffRecord,
    key: &VerifyingKey,
) -> std::result::Result<(), RecordFault> {
    let domain = match record.ledger.as_str() {
        "break_glass_receipts" => DOMAIN_BREAK_GLASS_RECEIPT,
        "export_receipts" => DOMAIN_EXPORT_RECEIPT,
        _ => return Err(RecordFault::Ledger),
    };
    let prev = hex32(&record.prev_hash).map_err(|_| RecordFault::Hash)?;
    let entry = hex32(&record.entry_hash).map_err(|_| RecordFault::Hash)?;
    if hash_entry(&prev, record.payload_json.as_bytes()) != entry {
        return Err(RecordFault::Hash);
    }
    let sig = hex::decode(&record.signature).map_err(|_| RecordFault::Signature)?;
    let set = SignatureSet::from_storage(&sig, None, None).map_err(|_| RecordFault::Signature)?;
    verify_entry_signature(key, &entry, &set, SignatureMode::Compat, None, domain)
        .map_err(|_| RecordFault::Signature)
}

enum RecordFault {
    Ledger,
    Hash,
    Signature,
}

fn role_label(disclosure: Disclosure) -> &'static str {
    match disclosure {
        Disclosure::Unseal => "unseal record",
        Disclosure::Export => "export receipt",
    }
}

fn pairing_str(p: Pairing) -> &'static str {
    match p {
        Pairing::PlaintextCommitment => "plaintext_commitment",
        Pairing::ExportReceipt => "export_receipt",
        Pairing::Declared => "declared",
    }
}

/// The printed name of a recipient binding.
pub fn binding_str(b: RecipientBinding) -> &'static str {
    match b {
        RecipientBinding::ReceiptCommitment => "receipt_commitment",
        RecipientBinding::CommittedNotDisclosed => "committed_not_disclosed",
        RecipientBinding::Declared => "declared",
        RecipientBinding::None => "none",
    }
}

/// The printed name of a pairing.
pub fn pairing_name(p: Pairing) -> &'static str {
    pairing_str(p)
}

pub(crate) fn verify_core(
    sidecar_bytes: &[u8],
    file: FileInput<'_>,
    opts: &VerifyOptions<'_>,
    conn: Option<&Connection>,
) -> SidecarReport {
    let mut report = SidecarReport {
        db_checked: conn.is_some(),
        ..SidecarReport::default()
    };
    // 1. Parse.
    let sidecar = match parse_lenient(sidecar_bytes) {
        Ok(s) => s,
        Err(why) => {
            report.push(CheckStatus::Fail, "format", why);
            return report;
        }
    };
    if sidecar.format != HANDOFF_FORMAT_V1 {
        report.push(
            CheckStatus::Fail,
            "format",
            format!("unsupported format '{}'", display_safe(&sidecar.format)),
        );
        return report;
    }
    let canonical = sidecar
        .to_canonical_bytes()
        .map(|b| b == sidecar_bytes)
        .unwrap_or(false);
    if !canonical {
        report.push(
            CheckStatus::Fail,
            "format",
            "not in canonical securacv-cjson-v1 form — the sidecar was edited after it was \
             written (re-emit it; never edit or pretty-print it)",
        );
    }
    if !bucket_ok(sidecar.created_bucket) {
        report.push(
            CheckStatus::Fail,
            "format",
            "created_bucket is not a 600 s bucket",
        );
    }
    if canonical && bucket_ok(sidecar.created_bucket) {
        report.push(
            CheckStatus::Ok,
            "format",
            "securacv-handoff:v1, canonical (securacv-cjson-v1)",
        );
    }

    // 2. File.
    let file_sha = match file.digest() {
        Ok((sha, n)) => {
            let sha_hex = hex::encode(sha);
            if sha_hex != sidecar.file.sha256 {
                report.push(
                    CheckStatus::Fail,
                    "file",
                    format!(
                        "sha256 {} does not match the sidecar's {} — this sidecar does not \
                         describe this file",
                        sha_hex,
                        display_safe(&sidecar.file.sha256)
                    ),
                );
            } else if n != sidecar.file.bytes {
                report.push(
                    CheckStatus::Fail,
                    "file",
                    format!(
                        "sha256 {} matches but the length {} does not match the sidecar's {}",
                        sha_hex, n, sidecar.file.bytes
                    ),
                );
            } else if check_text_field("file name", &sidecar.file.name).is_err() {
                report.push(
                    CheckStatus::Fail,
                    "file",
                    "the sidecar's file name contains a control or text-direction character",
                );
            } else {
                report.push(
                    CheckStatus::Ok,
                    "file",
                    format!("sha256 {} and {} bytes match the sidecar", sha_hex, n),
                );
            }
            Some(sha)
        }
        Err(e) => {
            report.push(
                CheckStatus::Fail,
                "file",
                format!("could not be read ({})", e),
            );
            None
        }
    };

    // 3. Key.
    let key = match hex32(&sidecar.device_public_key)
        .ok()
        .and_then(|b| crate::crypto::signatures::verifying_key_strict(&b).ok())
    {
        Some(k) => k,
        None => {
            report.push(
                CheckStatus::Fail,
                "device key",
                "the sidecar's device_public_key is not a valid Ed25519 public key",
            );
            return report;
        }
    };
    let mut lineage: Option<Vec<VerifyingKey>> = None;
    if let Some(conn) = conn {
        let anchor = opts
            .pinned
            .or_else(|| crate::device_public_key_from_db(conn).ok());
        match anchor.map(|a| crate::verify_helpers::lineage_verifying_keys(conn, &a)) {
            Some(Ok(keys)) => {
                let db_key = crate::device_public_key_from_db(conn).ok();
                if opts.pinned.is_some()
                    && !db_key.is_some_and(|d| keys.iter().any(|k| k.to_bytes() == d.to_bytes()))
                {
                    report.push(
                        CheckStatus::Fail,
                        "device key",
                        "this database's device identity is not in the lineage of the key you \
                         supplied — this is not that device's database",
                    );
                } else if !keys.iter().any(|k| k.to_bytes() == key.to_bytes()) {
                    report.push(
                        CheckStatus::Fail,
                        "device key",
                        format!(
                            "the sidecar's records name key {}…, which is not in this database's \
                             device key lineage",
                            fp16(&key)
                        ),
                    );
                } else {
                    report.identity_pinned = opts.pinned.is_some();
                }
                lineage = Some(keys);
            }
            Some(Err(e)) => report.push(
                CheckStatus::Fail,
                "database",
                format!("the device key lineage could not be read ({})", e),
            ),
            None => report.push(
                CheckStatus::Fail,
                "database",
                "the database records no device identity",
            ),
        }
    } else if let Some(pinned) = opts.pinned {
        if pinned.to_bytes() != key.to_bytes() {
            report.push(
                CheckStatus::Fail,
                "device key",
                format!(
                    "the sidecar's records name key {}…, not the key you supplied (after a key \
                     rotation, pass --db to check the lineage)",
                    fp16(&key)
                ),
            );
        } else {
            report.identity_pinned = true;
        }
    }

    // 4. Records.
    for record in &sidecar.records {
        let subject = format!("record {}", record.role.label());
        let wants_approvals = record.ledger == "break_glass_receipts";
        if wants_approvals != record.approvals_json.is_some() {
            report.push(
                CheckStatus::Fail,
                subject,
                "approvals_json is carried only by break_glass_receipts rows",
            );
            continue;
        }
        match check_record(record, &key) {
            Ok(()) => report.push(
                CheckStatus::Ok,
                subject,
                format!(
                    "entry hash re-derives; device signature checks under {}…",
                    fp16(&key)
                ),
            ),
            Err(RecordFault::Ledger) => report.push(
                CheckStatus::Fail,
                subject,
                format!("unknown ledger '{}'", display_safe(&record.ledger)),
            ),
            Err(RecordFault::Hash) => report.push(
                CheckStatus::Fail,
                subject,
                "entry hash does not re-derive from prev_hash and payload_json",
            ),
            Err(RecordFault::Signature) => report.push(
                CheckStatus::Fail,
                subject,
                format!("device signature does not check under {}…", fp16(&key)),
            ),
        }
    }

    // 5. Structure and links.
    let roles: Vec<RecordRole> = sidecar.records.iter().map(|r| r.role).collect();
    let structure_ok = match sidecar.disclosure {
        Disclosure::Unseal => {
            roles == [RecordRole::Grant, RecordRole::Redemption]
                || roles
                    == [
                        RecordRole::Grant,
                        RecordRole::Redemption,
                        RecordRole::Unsealed,
                    ]
        }
        Disclosure::Export => {
            roles == [RecordRole::ExportReceipt]
                || roles
                    == [
                        RecordRole::Grant,
                        RecordRole::Redemption,
                        RecordRole::ExportReceipt,
                    ]
        }
    };
    let ledger_ok = sidecar.records.iter().all(|r| {
        r.ledger
            == if r.role == RecordRole::ExportReceipt {
                "export_receipts"
            } else {
                "break_glass_receipts"
            }
    });
    if !structure_ok || !ledger_ok {
        report.push(
            CheckStatus::Fail,
            "records",
            match sidecar.disclosure {
                Disclosure::Unseal => {
                    "an unseal sidecar carries grant, redemption[, unsealed] break-glass records, \
                     in that order"
                }
                Disclosure::Export => {
                    "an export sidecar carries [grant, redemption,] export_receipt, in that order"
                }
            },
        );
        return finish_db(report, &sidecar, conn, lineage.as_deref());
    }
    let by_role = |role: RecordRole| sidecar.records.iter().find(|r| r.role == role);
    let mut ledger = RedemptionLedger::default();
    let mut grant: Option<BreakGlassReceipt> = None;
    let mut redemption: Option<BreakGlassReceipt> = None;
    let mut unsealed: Option<BreakGlassReceipt> = None;
    let mut links_ok = true;
    for record in sidecar
        .records
        .iter()
        .filter(|r| r.ledger == "break_glass_receipts")
    {
        let parsed: std::result::Result<(BreakGlassReceipt, Vec<Approval>), String> =
            serde_json::from_str::<BreakGlassReceipt>(&record.payload_json)
                .map_err(|e| e.to_string())
                .and_then(|r| {
                    serde_json::from_str::<Vec<Approval>>(
                        record.approvals_json.as_deref().unwrap_or("[]"),
                    )
                    .map(|a| (r, a))
                    .map_err(|e| e.to_string())
                });
        let (receipt, approvals) = match parsed {
            Ok(v) => v,
            Err(e) => {
                links_ok = false;
                report.push(
                    CheckStatus::Fail,
                    format!("record {}", record.role.label()),
                    format!("payload does not parse ({})", display_safe(&e)),
                );
                break;
            }
        };
        let kind_ok = match record.role {
            RecordRole::Grant => matches!(receipt.outcome, BreakGlassOutcome::Granted),
            RecordRole::Redemption => receipt.redeemed_token().is_some(),
            RecordRole::Unsealed => receipt.unsealed_record().is_some(),
            RecordRole::ExportReceipt => false,
        };
        if !kind_ok {
            links_ok = false;
            report.push(
                CheckStatus::Fail,
                format!("record {}", record.role.label()),
                format!("the payload is not a {} row", record.role.label()),
            );
            break;
        }
        let entry = hex32(&record.entry_hash).unwrap_or([0u8; 32]);
        if let Err(e) = ledger.observe(&receipt, entry, &approvals) {
            links_ok = false;
            report.push(CheckStatus::Fail, "links", display_safe(&e.to_string()));
            break;
        }
        match record.role {
            RecordRole::Grant => grant = Some(receipt),
            RecordRole::Redemption => redemption = Some(receipt),
            RecordRole::Unsealed => unsealed = Some(receipt),
            _ => {}
        }
    }
    let export_receipt: Option<serde_json::Value> =
        by_role(RecordRole::ExportReceipt).and_then(|r| serde_json::from_str(&r.payload_json).ok());
    if links_ok && sidecar.disclosure == Disclosure::Export {
        let rec = by_role(RecordRole::ExportReceipt).expect("structure checked");
        let names_grant = export_receipt
            .as_ref()
            .and_then(|v| v.get("authorizing_receipt"))
            .is_some_and(|v| !v.is_null());
        match crate::verify::check_export_authorizations(
            &ledger,
            [(0i64, rec.payload_json.as_str())],
        ) {
            Err((_, why)) => {
                report.push(CheckStatus::Fail, "links", display_safe(&why));
            }
            Ok(_) if names_grant != (grant.is_some()) => {
                report.push(
                    CheckStatus::Fail,
                    "links",
                    "the export receipt names no authorizing grant, but the sidecar embeds one",
                );
            }
            Ok(_) if names_grant => report.push(
                CheckStatus::Ok,
                "links",
                "the export receipt names the grant, and the grant was redeemed once",
            ),
            Ok(_) => {}
        }
    } else if links_ok
        && [grant.as_ref(), redemption.as_ref()]
            .into_iter()
            .flatten()
            .any(|r| r.vault_envelope_id == crate::EXPORT_EVENTS_ENVELOPE_ID)
    {
        // U3 for a sidecar with no unseal record: an export:events grant
        // consented to an event export, never to an unseal, so an unseal
        // sidecar must not present a file as released under it.
        report.push(
            CheckStatus::Fail,
            "links",
            "an unseal sidecar embeds the redemption of an export:events token",
        );
    } else if links_ok {
        if unsealed.is_some() {
            report.push(
                CheckStatus::Ok,
                "links",
                "the unseal record names the redemption, and the redemption names the grant",
            );
        } else {
            report.push(CheckStatus::Ok, "links", "the redemption names the grant");
        }
    }
    if let Some(grant) = &grant {
        match grant.verify_context_binding() {
            Ok(true) => {
                let ids: Vec<String> = grant
                    .trustees_used
                    .iter()
                    .map(|t| display_safe(&t.0))
                    .collect();
                report.push(
                    CheckStatus::Ok,
                    "consent",
                    format!(
                        "the grant's recorded request context re-derives the request hash its {} \
                         trustee(s) approved ({})",
                        ids.len(),
                        ids.join(", ")
                    ),
                );
            }
            Ok(false) => report.push(
                CheckStatus::Unverified,
                "consent",
                "the grant records no request context to re-derive (a receipt written before \
                 the operator-context fields)",
            ),
            Err(e) => report.push(CheckStatus::Fail, "consent", display_safe(&e.to_string())),
        }
        if conn.is_none() {
            report.push(
                CheckStatus::Unverified,
                "consent",
                "the quorum behind the grant is re-derived against its policy era only with --db",
            );
        }
    }

    // 6. Pairing.
    let (computed_pairing, disclosure_record) = match sidecar.disclosure {
        Disclosure::Unseal => (
            if unsealed.is_some() && sidecar.salts.plaintext.is_some() {
                Pairing::PlaintextCommitment
            } else {
                Pairing::Declared
            },
            sidecar.records.last().expect("structure checked"),
        ),
        Disclosure::Export => (
            Pairing::ExportReceipt,
            by_role(RecordRole::ExportReceipt).expect("structure checked"),
        ),
    };
    if sidecar.pairing != computed_pairing {
        report.push(
            CheckStatus::Fail,
            "binding",
            format!(
                "the sidecar says pairing={}; its contents support {}",
                pairing_str(sidecar.pairing),
                pairing_str(computed_pairing)
            ),
        );
    } else {
        match computed_pairing {
            Pairing::PlaintextCommitment => {
                let (_, want, _) = unsealed
                    .as_ref()
                    .and_then(|u| u.unsealed_record())
                    .expect("unsealed present");
                let got = sidecar
                    .salts
                    .plaintext
                    .as_deref()
                    .and_then(|s| hex32(s).ok())
                    .and_then(|salt| file.commitment(DOMAIN_UNSEALED_PLAINTEXT, &salt).ok());
                if got == Some(want) {
                    report.push(
                        CheckStatus::Ok,
                        "pairing",
                        "this file opens the plaintext commitment in the device-signed unseal \
                         record",
                    );
                } else {
                    report.push(
                        CheckStatus::Fail,
                        "pairing",
                        "this file does not open the plaintext commitment in the signed unseal \
                         record — wrong file or wrong salt",
                    );
                }
            }
            Pairing::Declared => report.push(
                CheckStatus::Declared,
                "pairing",
                "no device-signed record this sidecar can open binds this file to the envelope; \
                 the tool that wrote the sidecar says it does",
            ),
            Pairing::ExportReceipt => {
                if sidecar.salts.plaintext.is_some() {
                    report.push(
                        CheckStatus::Fail,
                        "binding",
                        "the sidecar says salts.plaintext=present; its contents support null",
                    );
                }
                match export_pairing(&file, disclosure_record) {
                    Ok(()) => report.push(
                        CheckStatus::Ok,
                        "pairing",
                        "this file is the export bundle whose receipt is the embedded export \
                         record",
                    ),
                    Err(why) => report.push(
                        CheckStatus::Fail,
                        "pairing",
                        format!(
                            "this file is not the export bundle whose receipt is the embedded \
                             export record ({})",
                            display_safe(&why)
                        ),
                    ),
                }
            }
        }
    }
    if sidecar.disclosure == Disclosure::Unseal
        && computed_pairing == Pairing::Declared
        && sidecar.salts.plaintext.is_some()
    {
        report.push(
            CheckStatus::Fail,
            "binding",
            "the sidecar says salts.plaintext=present; its contents support null",
        );
    }

    // 7. Recipient.
    let commitment: Option<[u8; 32]> = match sidecar.disclosure {
        Disclosure::Unseal => unsealed
            .as_ref()
            .and_then(|u| u.unsealed_record())
            .and_then(|(_, _, r)| r),
        Disclosure::Export => export_receipt
            .as_ref()
            .and_then(|v| v.get("context"))
            .and_then(|c| c.get("recipient_commitment"))
            .and_then(|c| serde_json::from_value::<[u8; 32]>(c.clone()).ok()),
    };
    let computed_binding = match (commitment.is_some(), sidecar.recipient.is_some()) {
        (true, true) => RecipientBinding::ReceiptCommitment,
        (true, false) => RecipientBinding::CommittedNotDisclosed,
        (false, true) => RecipientBinding::Declared,
        (false, false) => RecipientBinding::None,
    };
    let label = role_label(sidecar.disclosure);
    if sidecar.recipient_binding != computed_binding {
        report.push(
            CheckStatus::Fail,
            "binding",
            format!(
                "the sidecar says recipient_binding={}; its contents support {}",
                binding_str(sidecar.recipient_binding),
                binding_str(computed_binding)
            ),
        );
    } else if (computed_binding == RecipientBinding::ReceiptCommitment)
        != sidecar.salts.recipient.is_some()
    {
        report.push(
            CheckStatus::Fail,
            "binding",
            format!(
                "the sidecar says salts.recipient={}; its contents support {}",
                if sidecar.salts.recipient.is_some() {
                    "present"
                } else {
                    "null"
                },
                if computed_binding == RecipientBinding::ReceiptCommitment {
                    "present"
                } else {
                    "null"
                }
            ),
        );
    } else {
        let fields_ok = sidecar.recipient.as_ref().is_none_or(|r| {
            HandoffRecipient::new(&r.name, r.organization.as_deref(), r.key.as_deref())
                .is_ok_and(|n| &n == r)
        });
        match computed_binding {
            _ if !fields_ok => report.push(
                CheckStatus::Fail,
                "recipient",
                "the named recipient is not in the form this build writes (a control or \
                 text-direction character, untrimmed text, or a malformed key)",
            ),
            RecipientBinding::ReceiptCommitment => {
                let r = sidecar.recipient.as_ref().expect("named");
                let opens = sidecar
                    .salts
                    .recipient
                    .as_deref()
                    .and_then(|s| hex32(s).ok())
                    .map(|salt| recipient_commitment(&salt, r));
                if opens == commitment {
                    report.push(
                        CheckStatus::Ok,
                        "recipient",
                        format!(
                            "opens the commitment in the signed {} — declared by the operator at \
                             disclosure, not verified by the device",
                            label
                        ),
                    );
                } else {
                    report.push(
                        CheckStatus::Fail,
                        "recipient",
                        format!("does not open the commitment in the signed {}", label),
                    );
                }
            }
            RecipientBinding::CommittedNotDisclosed => report.push(
                CheckStatus::Ok,
                "recipient",
                format!(
                    "the signed {} commits to a recipient this sidecar does not disclose",
                    label
                ),
            ),
            RecipientBinding::Declared => report.push(
                CheckStatus::Declared,
                "recipient",
                format!(
                    "{} — named in this sidecar only; no signed record commits to it",
                    sidecar.recipient.as_ref().expect("named").label()
                ),
            ),
            RecipientBinding::None => report.push(CheckStatus::Ok, "recipient", "none named"),
        }
    }

    // 8. Anchors.
    let disclosure_hash = hex32(&disclosure_record.entry_hash).ok();
    let db_tokens = conn.map(db_tokens).unwrap_or_default();
    for anchor in &sidecar.anchors {
        let kind = anchor
            .get("kind")
            .and_then(|k| k.as_str())
            .unwrap_or("")
            .to_string();
        if kind != "rfc3161" {
            report.push(
                CheckStatus::Unverified,
                format!("anchor ({})", display_safe(&kind)),
                "this build does not check that anchor kind",
            );
            continue;
        }
        let Ok(a) = serde_json::from_value::<Rfc3161AnchorRef>(anchor.clone()) else {
            report.push(
                CheckStatus::Fail,
                "anchor (rfc3161)",
                "the anchor reference is malformed (covers, kind, subject_hash, token_sha256)",
            );
            continue;
        };
        let subject = format!("anchor (rfc3161, covers {})", a.covers.label());
        let expected = match a.covers {
            AnchorCovers::FileBytes => file_sha,
            AnchorCovers::DisclosureRecord => disclosure_hash,
        };
        let t16 = h16(&a.token_sha256);
        if hex32(&a.subject_hash).ok() != expected || expected.is_none() {
            report.push(
                CheckStatus::Fail,
                subject,
                format!(
                    "token {}… covers {}…, not the {}",
                    t16,
                    h16(&a.subject_hash),
                    a.covers.target()
                ),
            );
            continue;
        }
        let expected = expected.expect("checked");
        let token = db_tokens.get(&a.token_sha256).cloned().or_else(|| {
            opts.anchors_dir
                .and_then(|d| token_from_dir(d, &a.token_sha256))
        });
        let Some(token) = token else {
            report.push(
                CheckStatus::Unverified,
                subject,
                format!("token {}… not supplied (pass --db or --anchors-dir)", t16),
            );
            continue;
        };
        match crate::tsa::parse_token_imprint(&token) {
            Ok(imprint) if imprint == expected => {}
            Ok(imprint) => {
                report.push(
                    CheckStatus::Fail,
                    subject,
                    format!(
                        "token {}… covers {}…, not the {}",
                        t16,
                        &hex::encode(imprint)[..16],
                        a.covers.target()
                    ),
                );
                continue;
            }
            Err(e) => {
                report.push(
                    CheckStatus::Fail,
                    subject,
                    format!("token {}… does not parse ({})", t16, e),
                );
                continue;
            }
        }
        if opts.tsa_cas.is_empty() {
            report.push(
                CheckStatus::Unverified,
                subject,
                format!(
                    "token {}… found; imprint matches; countersignature not checked (pass --ca \
                     <tsa-ca.pem>)",
                    t16
                ),
            );
            continue;
        }
        let mut last_err = String::new();
        let mut under = None;
        for ca in opts.tsa_cas {
            match crate::anchor_policy::openssl_ts_verify(&token, &expected, ca) {
                Ok(()) => {
                    under = Some(ca.clone());
                    break;
                }
                Err(e) => last_err = e.to_string(),
            }
        }
        match under {
            Some(ca) => report.push(
                CheckStatus::Ok,
                subject,
                format!(
                    "token {}… found; imprint matches; countersignature checks under {}",
                    t16,
                    display_safe(&ca)
                ),
            ),
            None => report.push(
                CheckStatus::Fail,
                subject,
                format!(
                    "countersignature does not check under any --ca given ({})",
                    display_safe(&last_err)
                ),
            ),
        }
    }
    finish_db(report, &sidecar, conn, lineage.as_deref())
}

fn export_pairing(file: &FileInput<'_>, record: &HandoffRecord) -> std::result::Result<(), String> {
    let bytes = file.read_all().map_err(|e| e.to_string())?;
    let bundle: ExportBundle = serde_json::from_slice(&bytes)
        .map_err(|_| "it does not parse as an export bundle".to_string())?;
    crate::verify_export_bundle(&bundle).map_err(|e| e.to_string())?;
    if hex::encode(bundle.receipt_entry.entry_hash) != record.entry_hash {
        return Err("its receipt is a different export receipt".to_string());
    }
    let payload =
        serde_json::to_string(&bundle.receipt_entry.receipt).map_err(|e| e.to_string())?;
    if payload != record.payload_json {
        return Err("its receipt does not serialize to the embedded payload".to_string());
    }
    Ok(())
}

fn db_tokens(conn: &Connection) -> HashMap<String, Vec<u8>> {
    let mut out = HashMap::new();
    if !tsa_table_exists(conn).unwrap_or(false) {
        return out;
    }
    if let Ok(mut stmt) = conn.prepare("SELECT token_der FROM tsa_anchors") {
        if let Ok(rows) = stmt.query_map([], |row| row.get::<_, Vec<u8>>(0)) {
            for der in rows.flatten() {
                out.insert(hex::encode(Sha256::digest(&der)), der);
            }
        }
    }
    out
}

fn token_from_dir(dir: &Path, token_sha256: &str) -> Option<Vec<u8>> {
    let named = dir.join(format!("{}.der", token_sha256));
    if let Ok(bytes) = std::fs::read(&named) {
        if hex::encode(Sha256::digest(&bytes)) == token_sha256 {
            return Some(bytes);
        }
    }
    for entry in std::fs::read_dir(dir).ok()?.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        if let Ok(bytes) = std::fs::read(&path) {
            if hex::encode(Sha256::digest(&bytes)) == token_sha256 {
                return Some(bytes);
            }
        }
    }
    None
}

/// The deep database checks (read-only), appended last.
fn finish_db(
    mut report: SidecarReport,
    sidecar: &HandoffSidecar,
    conn: Option<&Connection>,
    lineage: Option<&[VerifyingKey]>,
) -> SidecarReport {
    let (Some(conn), Some(lineage)) = (conn, lineage) else {
        return report;
    };
    let mut ok = true;
    for record in &sidecar.records {
        let Ok(entry) = hex32(&record.entry_hash) else {
            continue;
        };
        let row = match record.ledger.as_str() {
            "break_glass_receipts" => crate::break_glass_row_by_entry_hash(conn, &entry),
            "export_receipts" => crate::export_row_by_entry_hash(conn, &entry),
            _ => continue,
        };
        match row {
            Ok(Some(row)) if record_from_row(&row, record.role) == *record => {}
            Ok(Some(_)) => {
                ok = false;
                report.push(
                    CheckStatus::Fail,
                    "database",
                    format!(
                        "record {} differs from this database's {} row with that entry hash",
                        record.role.label(),
                        record.ledger
                    ),
                );
            }
            Ok(None) | Err(_) => {
                ok = false;
                report.push(
                    CheckStatus::Fail,
                    "database",
                    format!(
                        "record {} is not in this database's {} (the sidecar did not come from \
                         this database, or the row was removed)",
                        record.role.label(),
                        record.ledger
                    ),
                );
            }
        }
    }
    let pq = crate::verify_helpers::load_pq_verifying_key(conn, None, None)
        .ok()
        .flatten();
    let policy = crate::verify::load_break_glass_policy(conn).ok().flatten();
    let walks: [(&str, Result<()>); 3] = [
        (
            "break_glass_receipts",
            if policy.is_none() && bg_rows(conn) == 0 {
                Ok(())
            } else {
                crate::verify::verify_break_glass_receipts_with(
                    conn,
                    lineage,
                    policy.as_ref(),
                    SignatureMode::Compat,
                    pq.as_ref(),
                    |_, _| {},
                )
                .map(|_| ())
            },
        ),
        (
            "export_receipts",
            crate::verify::verify_export_receipts_with(
                conn,
                lineage,
                SignatureMode::Compat,
                pq.as_ref(),
                |_, _| {},
            )
            .map(|_| ()),
        ),
        (
            "export_receipts",
            crate::verify::verify_export_authorizations(conn).map(|_| ()),
        ),
    ];
    for (ledger, result) in walks {
        if let Err(e) = result {
            ok = false;
            report.push(
                CheckStatus::Fail,
                "database",
                format!(
                    "this database's {} does not verify ({})",
                    ledger,
                    display_safe(&format!("{:#}", e))
                ),
            );
        }
    }
    if ok {
        report.push(
            CheckStatus::Ok,
            "database",
            "every embedded record is a row of this database, both receipt chains verify end to \
             end, the grant's quorum re-derives, and the device key is in its lineage",
        );
    }
    report
}

fn bg_rows(conn: &Connection) -> i64 {
    conn.query_row("SELECT COUNT(*) FROM break_glass_receipts", [], |r| {
        r.get(0)
    })
    .unwrap_or(0)
}

#[cfg(test)]
#[path = "handoff_tests.rs"]
mod tests;
