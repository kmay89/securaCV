//! The anchor policy: the operator's declaration of which RFC 3161 TSAs
//! anchor which ledger heads, in which declared role, and how `log_anchor
//! verify --policy` decides whether a head is covered.
//!
//! Format-tagged JSON (`securacv-anchor-policy:v1`), strict: like every
//! custody artifact carried between machines (unlock requests, court kits,
//! setup drafts) it fails with a "wrong file" message rather than being
//! half-read. The daemon's `witness.toml` never learns TSA URLs.
//!
//! Roles are declarations. `qualified` records the operator's statement
//! that a TSA is eIDAS-qualified (ETSI EN 319 421/422) — who checked the
//! trusted list and when belongs in `declaration`; the tools never evaluate
//! that status. What the tools do check is cryptographic: a row is
//! attributed to a policy entry only when `openssl ts -verify` succeeds
//! under that entry's CA, and the two-TSA control counts distinct
//! attributed entries. `cert_sha256` pins are a contradiction check always,
//! and an attribution tie-break only when more than one entry's CA validates
//! the same row (shared public roots) — a pin never attributes a row that
//! failed under its entry's CA.

use anyhow::{anyhow, bail, Context, Result};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};

use crate::tsa::{AnchorRecord, AnchorSubject};

pub const ANCHOR_POLICY_FORMAT: &str = "securacv-anchor-policy:v1";

/// The operator's declared role for a TSA. Never evaluated by the tools.
#[derive(Deserialize, Clone, Copy, PartialEq, Eq, Debug, Hash, PartialOrd, Ord)]
#[serde(rename_all = "lowercase")]
pub enum TsaRole {
    Qualified,
    Independent,
}

impl TsaRole {
    pub const fn as_str(self) -> &'static str {
        match self {
            TsaRole::Qualified => "qualified",
            TsaRole::Independent => "independent",
        }
    }
}

impl std::fmt::Display for TsaRole {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct TsaEntry {
    /// `^[A-Za-z0-9_-]{1,32}$`, unique across the policy.
    pub name: String,
    pub role: TsaRole,
    /// `https://` unless `allow_http`.
    pub url: String,
    /// The TSA's CA bundle (PEM). Resolved relative to the policy file's
    /// directory at load; existence is checked only when a CA is used.
    pub ca: PathBuf,
    #[serde(default)]
    pub allow_http: bool,
    /// SHA-256 of TSA signing certificates (DER). Two uses, in this order:
    /// (1) contradiction — a row whose countersignature verifies under this
    /// entry's CA but whose embedded signer cert is not in this list is FAIL;
    /// (2) attribution — ONLY when more than one policy CA verifies the same
    /// row, the unique entry whose pins hold the (now chain-validated) signer
    /// cert wins. A pin never attributes a row that failed under this
    /// entry's CA.
    #[serde(default)]
    #[serde(rename = "cert_sha256")]
    pub fingerprint_pins: Vec<String>,
    /// Recorded text; never evaluated.
    #[serde(default)]
    pub declaration: Option<String>,
}

#[derive(Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct AnchorPolicy {
    pub format: String,
    /// At least one.
    pub tsas: Vec<TsaEntry>,
    /// Each must be a ledger head kind (`AnchorSubject::HEADS`); `digest` is
    /// refused.
    #[serde(default = "all_heads")]
    pub subjects: Vec<String>,
    #[serde(default = "both_roles")]
    pub require_roles: Vec<TsaRole>,
    /// `>= 1`, `<= tsas.len()`.
    #[serde(default = "two")]
    pub min_distinct_tsas: u32,
    /// The largest device-clock-vs-`genTime` offset (whole seconds, either
    /// way) a clock-provenance record may carry before `log_anchor verify
    /// --policy` counts it against the policy; `>= 1`. Default: one kernel
    /// bucket (600 s), the structural threshold `verify` applies without a
    /// policy. Add this key only once every policy reader is upgraded — an
    /// older `log_anchor` refuses a policy carrying an unknown key.
    #[serde(default = "one_bucket")]
    pub max_clock_offset_s: i64,
    /// The OpenTimestamps leg (optional, additive): the calendars every
    /// planned subject is also submitted to, as a nonced commitment, and
    /// whether `verify --policy` requires a header-confirmed proof. Absent ⇒
    /// `None` ⇒ the TSA-only behavior, byte for byte. Add this key only once
    /// every policy reader is upgraded — an older `log_anchor` refuses a
    /// policy carrying an unknown key (`docs/design/opentimestamps_leg.md`).
    #[serde(default)]
    pub ots: Option<OtsPolicy>,
}

/// The most calendars an `ots` block may declare.
pub const OTS_MAX_CALENDARS: usize = 8;

/// One OpenTimestamps calendar the operator declares.
#[derive(Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct CalendarEntry {
    /// `^[A-Za-z0-9_-]{1,32}$`, unique across BOTH rosters (TSA names and
    /// calendar names).
    pub name: String,
    /// The calendar itself (not an aggregator pool): https://, no trailing
    /// '/', no query or fragment, charset `[A-Za-z0-9._/:-]` — the charset a
    /// pending attestation may carry, so a stored pending URI matches back to
    /// this entry by exact string equality.
    pub url: String,
    #[serde(default)]
    pub allow_http: bool,
}

/// The policy's optional `ots` object.
#[derive(Deserialize, Debug, Clone)]
#[serde(deny_unknown_fields)]
pub struct OtsPolicy {
    /// 1..=8 entries.
    pub calendars: Vec<CalendarEntry>,
    /// `true`: `verify --policy` needs, for every non-empty subject, a proof
    /// confirmed against a supplied header, and requires `--bitcoin-headers`.
    #[serde(default)]
    pub required: bool,
}

impl OtsPolicy {
    /// The calendar declared under `name`.
    pub fn calendar(&self, name: &str) -> Option<&CalendarEntry> {
        self.calendars.iter().find(|c| c.name == name)
    }

    /// The calendar whose url is exactly `url` (no normalization).
    pub fn calendar_for_url(&self, url: &str) -> Option<&CalendarEntry> {
        self.calendars.iter().find(|c| c.url == url)
    }
}

fn one_bucket() -> i64 {
    crate::clock_provenance::ONE_BUCKET_S
}

fn all_heads() -> Vec<String> {
    AnchorSubject::HEADS
        .iter()
        .map(|k| k.as_str().to_string())
        .collect()
}

fn both_roles() -> Vec<TsaRole> {
    vec![TsaRole::Qualified, TsaRole::Independent]
}

fn two() -> u32 {
    2
}

impl AnchorPolicy {
    /// The policy's subjects as typed head kinds (validated at load).
    pub fn subject_kinds(&self) -> Vec<AnchorSubject> {
        self.subjects
            .iter()
            .filter_map(|s| AnchorSubject::parse(s))
            .collect()
    }

    pub fn entry(&self, name: &str) -> Option<&TsaEntry> {
        self.tsas.iter().find(|t| t.name == name)
    }
}

fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 32
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn is_64_lower_hex(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// Load and validate a policy file. Every error is prefixed
/// `anchor policy {path}: `. CA paths are resolved against the policy file's
/// directory; their existence is checked only when a CA is used.
pub fn load(path: &Path) -> Result<AnchorPolicy> {
    let prefix = format!("anchor policy {}: ", path.display());
    let text = std::fs::read_to_string(path).with_context(|| format!("{prefix}cannot read"))?;
    let mut policy: AnchorPolicy =
        serde_json::from_str(&text).map_err(|e| anyhow!("{prefix}{e}"))?;
    validate(&mut policy, &prefix)?;
    let dir = path.parent().unwrap_or_else(|| Path::new("."));
    for entry in &mut policy.tsas {
        if entry.ca.is_relative() {
            entry.ca = dir.join(&entry.ca);
        }
    }
    Ok(policy)
}

fn validate(policy: &mut AnchorPolicy, prefix: &str) -> Result<()> {
    if policy.format != ANCHOR_POLICY_FORMAT {
        bail!(
            "{prefix}format must be \"{ANCHOR_POLICY_FORMAT}\" (got \"{}\")",
            policy.format
        );
    }
    if policy.tsas.is_empty() {
        bail!("{prefix}at least one TSA is required");
    }
    let mut seen: HashSet<&str> = HashSet::new();
    let mut pin_owner: HashMap<&str, &str> = HashMap::new();
    for entry in &policy.tsas {
        if !valid_name(&entry.name) {
            bail!(
                "{prefix}TSA name '{}' is invalid (1-32 characters of A-Z a-z 0-9 _ -)",
                entry.name
            );
        }
        if !seen.insert(entry.name.as_str()) {
            bail!("{prefix}duplicate TSA name '{}'", entry.name);
        }
        if !entry.url.starts_with("https://") && !entry.allow_http {
            bail!(
                "{prefix}TSA '{}': url must start with https:// (set \"allow_http\": true to override)",
                entry.name
            );
        }
        for pin in &entry.fingerprint_pins {
            if !is_64_lower_hex(pin) {
                bail!(
                    "{prefix}TSA '{}': cert_sha256 entry \"{pin}\" is not 64 hex characters",
                    entry.name
                );
            }
            if let Some(other) = pin_owner.insert(pin.as_str(), entry.name.as_str()) {
                if other != entry.name {
                    bail!(
                        "{prefix}cert_sha256 \"{pin}\" is pinned by both '{other}' and '{}'; a pin must belong to one TSA",
                        entry.name
                    );
                }
            }
        }
    }
    for role in &policy.require_roles {
        if !policy.tsas.iter().any(|t| t.role == *role) {
            bail!("{prefix}role '{role}' is required by require_roles but no TSA declares it");
        }
    }
    if policy.min_distinct_tsas < 1 {
        bail!("{prefix}min_distinct_tsas must be at least 1");
    }
    if policy.min_distinct_tsas as usize > policy.tsas.len() {
        bail!(
            "{prefix}min_distinct_tsas {} exceeds the {} declared TSA(s)",
            policy.min_distinct_tsas,
            policy.tsas.len()
        );
    }
    if policy.max_clock_offset_s < 1 {
        bail!(
            "{prefix}max_clock_offset_s must be at least 1 (got {})",
            policy.max_clock_offset_s
        );
    }
    // An explicit empty list would make `anchor-all` request nothing and
    // `verify --policy` vacuously SATISFIED — the two-TSA control turned into
    // a silent no-op by a typo. Refuse it, and refuse a repeated subject (it
    // would double the request count for that ledger).
    if policy.subjects.is_empty() {
        bail!("{prefix}at least one subject is required");
    }
    let mut seen_subjects = std::collections::BTreeSet::new();
    for s in &policy.subjects {
        match AnchorSubject::parse(s) {
            Some(k) if k.is_head() => {}
            _ => bail!(
                "{prefix}subject '{s}' is not a ledger head (allowed: chain_head, export_receipt_head, break_glass_receipt_head, policy_head, clock_provenance_head)"
            ),
        }
        if !seen_subjects.insert(s.as_str()) {
            bail!("{prefix}duplicate subject '{s}'");
        }
    }
    if let Some(ots) = &policy.ots {
        validate_ots(ots, &policy.tsas, prefix)?;
    }
    Ok(())
}

/// Whether a calendar url uses only the characters a pending attestation
/// may carry (`[A-Za-z0-9._/:-]`).
fn ots_url_charset_ok(url: &str) -> bool {
    url.bytes()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'/' | b':' | b'-'))
}

fn validate_ots(ots: &OtsPolicy, tsas: &[TsaEntry], prefix: &str) -> Result<()> {
    if ots.calendars.is_empty() {
        bail!("{prefix}ots: at least one calendar is required");
    }
    if ots.calendars.len() > OTS_MAX_CALENDARS {
        bail!("{prefix}ots: at most {OTS_MAX_CALENDARS} calendars");
    }
    let mut names: HashSet<&str> = HashSet::new();
    let mut urls: HashSet<&str> = HashSet::new();
    for c in &ots.calendars {
        if !valid_name(&c.name) {
            bail!(
                "{prefix}calendar name '{}' is invalid (1-32 characters of A-Z a-z 0-9 _ -)",
                c.name
            );
        }
        if !names.insert(c.name.as_str()) {
            bail!("{prefix}duplicate calendar name '{}'", c.name);
        }
        if tsas.iter().any(|t| t.name == c.name) {
            bail!("{prefix}name '{}' is used by both a TSA and a calendar", c.name);
        }
        let scheme_ok =
            c.url.starts_with("https://") || (c.allow_http && c.url.starts_with("http://"));
        if !scheme_ok {
            bail!(
                "{prefix}calendar '{}': url must start with https:// (set \"allow_http\": true to override)",
                c.name
            );
        }
        if c.url.ends_with('/') || c.url.contains('?') || c.url.contains('#') {
            bail!(
                "{prefix}calendar '{}': url must not end with '/' or carry a query or fragment",
                c.name
            );
        }
        if !ots_url_charset_ok(&c.url) || c.url.len() > crate::ots::OTS_MAX_URI_LEN {
            bail!(
                "{prefix}calendar '{}': url contains a character outside A-Z a-z 0-9 . _ / : - (a pending attestation could not name it)",
                c.name
            );
        }
        if !urls.insert(c.url.as_str()) {
            bail!("{prefix}duplicate calendar url '{}'", c.url);
        }
    }
    Ok(())
}

// -------------------- openssl delegation --------------------

/// Whether the `openssl` CLI is on PATH.
pub fn openssl_available() -> bool {
    std::process::Command::new("openssl")
        .arg("version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

/// Run `openssl ts -verify` on a stored token — an independent second
/// implementation for the trust-critical signature check.
pub fn openssl_ts_verify(token_der: &[u8], digest: &[u8; 32], ca_path: &str) -> Result<()> {
    // NamedTempFile: unpredictable name, 0600, O_EXCL creation (no symlink
    // following), removed on drop even when openssl fails.
    let mut token_file = tempfile::Builder::new()
        .prefix("securacv_anchor_")
        .suffix(".der")
        .tempfile()?;
    std::io::Write::write_all(&mut token_file, token_der)?;
    let output = std::process::Command::new("openssl")
        .args([
            "ts",
            "-verify",
            "-digest",
            &hex::encode(digest),
            "-in",
            token_file
                .path()
                .to_str()
                .ok_or_else(|| anyhow!("temp path not UTF-8"))?,
            "-token_in",
            "-CAfile",
            ca_path,
        ])
        .output()
        .context("running openssl (is it installed?)")?;
    if !output.status.success() {
        bail!("{}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(())
}

// -------------------- attribution --------------------

/// Which policy entries a row's countersignature verified under (openssl
/// already run by the caller, once per entry CA).
pub struct Verified<'a> {
    pub anchor: &'a AnchorRecord,
    pub under: Vec<String>,
}

/// The outcome of attributing one row to a policy entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Attribution {
    /// Verified under no policy CA.
    None,
    /// Exactly one policy CA verified the row.
    One(String),
    /// Several CAs verified it and exactly one of those entries pins the
    /// (chain-validated) signer certificate.
    AmbiguousResolvedByPin(String),
    /// Several CAs verified it and no unique pin singles one out.
    Ambiguous(Vec<String>),
}

/// A.1.4: exactly one CA ⇒ `One`; several CAs ⇒ the unique entry among
/// `under` whose `cert_sha256` pins contain the row's signer cert (re-derived
/// from the token by the caller) ⇒ `AmbiguousResolvedByPin`; else
/// `Ambiguous`. A pin on an entry NOT in `under` never attributes.
pub fn attribute(
    policy: &AnchorPolicy,
    v: &Verified<'_>,
    signer_fingerprint: Option<&str>,
) -> Attribution {
    match v.under.len() {
        0 => Attribution::None,
        1 => Attribution::One(v.under[0].clone()),
        _ => {
            let Some(fp) = signer_fingerprint else {
                return Attribution::Ambiguous(v.under.clone());
            };
            let pinned: Vec<&String> = v
                .under
                .iter()
                .filter(|name| {
                    policy
                        .entry(name)
                        .map(|e| e.fingerprint_pins.iter().any(|p| p == fp))
                        .unwrap_or(false)
                })
                .collect();
            match pinned.as_slice() {
                [one] => Attribution::AmbiguousResolvedByPin((*one).clone()),
                _ => Attribution::Ambiguous(v.under.clone()),
            }
        }
    }
}

// -------------------- coverage --------------------

/// A row attributed to exactly one policy entry.
pub struct Attributed<'a> {
    pub anchor: &'a AnchorRecord,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Coverage {
    pub subject: AnchorSubject,
    pub head: Option<[u8; 32]>,
    pub verdict: CoverageVerdict,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CoverageVerdict {
    /// The ledger is empty: nothing to cover (sentinel rows do not count).
    Empty,
    Covered {
        hash: [u8; 32],
        by: Vec<(String, TsaRole)>,
        anchor_ids: Vec<i64>,
        bucket_start: u64,
        is_current: bool,
    },
    NotCovered {
        anchored_by: Vec<(String, TsaRole)>,
        missing_roles: Vec<TsaRole>,
        distinct: usize,
    },
}

/// Per policy subject (order kept): the newest hash whose attributed `OK`
/// rows cover every required role via distinct entries and number at least
/// `min_distinct_tsas`; `is_current` says whether that hash is the ledger's
/// current head. Sentinel rows are `digest` rows and never candidates.
pub fn evaluate(
    policy: &AnchorPolicy,
    heads: &[(AnchorSubject, Option<[u8; 32]>)],
    rows: &[Attributed<'_>],
) -> Vec<Coverage> {
    let role_of = |name: &str| policy.entry(name).map(|e| e.role);
    let mut out = Vec::new();
    for subject in policy.subject_kinds() {
        let head = heads
            .iter()
            .find(|(s, _)| *s == subject)
            .and_then(|(_, h)| *h);
        let Some(head) = head else {
            out.push(Coverage {
                subject,
                head: None,
                verdict: CoverageVerdict::Empty,
            });
            continue;
        };
        // Group this subject's rows by hash: distinct names, ids, newest bucket.
        struct Group {
            names: BTreeMap<String, TsaRole>,
            ids: Vec<i64>,
            newest_bucket: u64,
            newest_id: i64,
        }
        let mut groups: HashMap<[u8; 32], Group> = HashMap::new();
        for row in rows {
            if row.anchor.subject_kind() != Some(subject) {
                continue;
            }
            let Some(role) = role_of(&row.name) else {
                continue;
            };
            let g = groups
                .entry(row.anchor.subject_hash)
                .or_insert_with(|| Group {
                    names: BTreeMap::new(),
                    ids: Vec::new(),
                    newest_bucket: 0,
                    newest_id: 0,
                });
            g.names.insert(row.name.clone(), role);
            g.ids.push(row.anchor.id);
            let b = row.anchor.created_bucket.start_epoch_s;
            if b > g.newest_bucket || (b == g.newest_bucket && row.anchor.id > g.newest_id) {
                g.newest_bucket = b;
                g.newest_id = row.anchor.id;
            }
        }
        let covered = |g: &Group| -> bool {
            g.names.len() >= policy.min_distinct_tsas as usize
                && policy
                    .require_roles
                    .iter()
                    .all(|r| g.names.values().any(|have| have == r))
        };
        let pick = |pred: &dyn Fn(&Group) -> bool| -> Option<([u8; 32], &Group)> {
            groups
                .iter()
                .filter(|(_, g)| pred(g))
                .max_by_key(|(_, g)| (g.newest_bucket, g.newest_id))
                .map(|(h, g)| (*h, g))
        };
        // The ledger's CURRENT head wins whenever it is fully covered, even if
        // an older hash carries a newer anchor row (an offline response
        // imported late, a re-import): "current head: yes" must never be
        // lost to the import order. Only when the head itself is not covered
        // does the newest covered historical hash describe the state.
        let current = groups.get(&head).filter(|g| covered(g)).map(|g| (head, g));
        if let Some((hash, g)) = current.or_else(|| pick(&covered)) {
            let mut ids = g.ids.clone();
            ids.sort_unstable();
            out.push(Coverage {
                subject,
                head: Some(head),
                verdict: CoverageVerdict::Covered {
                    hash,
                    by: g.names.iter().map(|(n, r)| (n.clone(), *r)).collect(),
                    anchor_ids: ids,
                    bucket_start: g.newest_bucket,
                    is_current: hash == head,
                },
            });
            continue;
        }
        // Not covered: describe the newest anchored hash (if any).
        let (anchored_by, distinct) = match pick(&|_| true) {
            Some((_, g)) => (
                g.names
                    .iter()
                    .map(|(n, r)| (n.clone(), *r))
                    .collect::<Vec<_>>(),
                g.names.len(),
            ),
            None => (Vec::new(), 0),
        };
        let held: BTreeSet<TsaRole> = anchored_by.iter().map(|(_, r)| *r).collect();
        let missing_roles: Vec<TsaRole> = policy
            .require_roles
            .iter()
            .filter(|r| !held.contains(r))
            .copied()
            .collect();
        out.push(Coverage {
            subject,
            head: Some(head),
            verdict: CoverageVerdict::NotCovered {
                anchored_by,
                missing_roles,
                distinct,
            },
        });
    }
    out
}

// -------------------- OpenTimestamps coverage --------------------

/// What `log_anchor verify` established about one OpenTimestamps row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OtsRowState {
    /// A Bitcoin leaf matched the merkle root of a header the verifier
    /// supplied; `height` is the lowest matching block.
    Confirmed { height: u64 },
    /// The proof attests a Bitcoin block whose header was not supplied.
    Unchecked { height: u64 },
    /// Not yet in a Bitcoin block.
    Pending,
}

/// A row that passed `log_anchor verify`'s structural checks; `Confirmed`
/// only when the CALLER matched its Bitcoin leaf against a supplied header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OtsPassed {
    pub id: i64,
    pub subject: AnchorSubject,
    pub hash: [u8; 32],
    pub bucket_start: u64,
    pub state: OtsRowState,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OtsCoverageVerdict {
    /// The ledger is empty.
    Empty,
    /// The lowest confirmed height of the winning hash, every confirmed row
    /// over it, and whether it is the ledger's current head (information
    /// only: `--require-current` does not apply to this leg).
    Confirmed {
        hash: [u8; 32],
        anchor_ids: Vec<i64>,
        height: u64,
        is_current: bool,
    },
    /// No confirmed row: how many of this subject's rows are pending and how
    /// many attest a block nobody checked against a header.
    NotConfirmed { pending: usize, unchecked: usize },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OtsCoverage {
    pub subject: AnchorSubject,
    pub head: Option<[u8; 32]>,
    pub verdict: OtsCoverageVerdict,
}

/// Per policy subject (order kept): the current head when at least one of
/// its rows is `Confirmed`, else the newest confirmed hash by
/// `(bucket_start, id)` with `is_current: false`, else `NotConfirmed`.
/// Pending never covers and Unchecked never covers; rows labeled with
/// another kind (sentinel and `digest` rows included) never count.
pub fn evaluate_ots(
    policy: &AnchorPolicy,
    heads: &[(AnchorSubject, Option<[u8; 32]>)],
    rows: &[OtsPassed],
) -> Vec<OtsCoverage> {
    let mut out = Vec::new();
    for subject in policy.subject_kinds() {
        let head = heads
            .iter()
            .find(|(s, _)| *s == subject)
            .and_then(|(_, h)| *h);
        let Some(head) = head else {
            out.push(OtsCoverage {
                subject,
                head: None,
                verdict: OtsCoverageVerdict::Empty,
            });
            continue;
        };
        let mine: Vec<&OtsPassed> = rows.iter().filter(|r| r.subject == subject).collect();
        // hash -> (ids, lowest height, newest (bucket, id))
        let mut confirmed: HashMap<[u8; 32], (Vec<i64>, u64, (u64, i64))> = HashMap::new();
        for r in &mine {
            if let OtsRowState::Confirmed { height } = r.state {
                let e = confirmed
                    .entry(r.hash)
                    .or_insert_with(|| (Vec::new(), height, (r.bucket_start, r.id)));
                e.0.push(r.id);
                e.1 = e.1.min(height);
                e.2 = e.2.max((r.bucket_start, r.id));
            }
        }
        let winner = confirmed.get(&head).map(|g| (head, g)).or_else(|| {
            confirmed
                .iter()
                .max_by_key(|(_, g)| g.2)
                .map(|(h, g)| (*h, g))
        });
        if let Some((hash, (ids, height, _))) = winner {
            let mut ids = ids.clone();
            ids.sort_unstable();
            out.push(OtsCoverage {
                subject,
                head: Some(head),
                verdict: OtsCoverageVerdict::Confirmed {
                    hash,
                    anchor_ids: ids,
                    height: *height,
                    is_current: hash == head,
                },
            });
            continue;
        }
        let pending = mine
            .iter()
            .filter(|r| r.state == OtsRowState::Pending)
            .count();
        let unchecked = mine
            .iter()
            .filter(|r| matches!(r.state, OtsRowState::Unchecked { .. }))
            .count();
        out.push(OtsCoverage {
            subject,
            head: Some(head),
            verdict: OtsCoverageVerdict::NotConfirmed { pending, unchecked },
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::TimeBucket;

    const EXAMPLE: &str = r#"{
  "format": "securacv-anchor-policy:v1",
  "tsas": [
    { "name": "dfn", "role": "qualified", "url": "https://zeitstempel.dfn.de", "ca": "ca/dfn-tsa-ca.pem",
      "declaration": "Operator's statement of why this TSA is treated as eIDAS-qualified, who checked the trusted list, and when. Recorded, never evaluated." },
    { "name": "freetsa", "role": "independent", "url": "https://freetsa.org/tsr", "ca": "ca/freetsa-ca.pem",
      "allow_http": false,
      "cert_sha256": ["fbf1c838f80923a01badb6030b9d708c5ef1a65b7d23f1f53a6aa274d1b99542"] }
  ],
  "subjects": ["chain_head", "export_receipt_head", "break_glass_receipt_head", "policy_head", "clock_provenance_head"],
  "require_roles": ["qualified", "independent"],
  "min_distinct_tsas": 2
}"#;

    fn write_policy(dir: &Path, text: &str) -> PathBuf {
        let p = dir.join("anchor-policy.json");
        std::fs::write(&p, text).unwrap();
        p
    }

    fn load_text(text: &str) -> Result<AnchorPolicy> {
        let dir = tempfile::tempdir().unwrap();
        load(&write_policy(dir.path(), text))
    }

    fn err_of(text: &str) -> String {
        load_text(text).unwrap_err().to_string()
    }

    fn two_tsas(extra: &str) -> String {
        format!(
            r#"{{"format":"securacv-anchor-policy:v1","tsas":[
              {{"name":"alpha","role":"qualified","url":"https://a.example/tsr","ca":"a.pem"}},
              {{"name":"beta","role":"independent","url":"https://b.example/tsr","ca":"b.pem"}}
            ]{extra}}}"#
        )
    }

    #[test]
    fn loads_example_policy() {
        let p = load_text(EXAMPLE).unwrap();
        assert_eq!(p.tsas.len(), 2);
        assert_eq!(p.tsas[0].role, TsaRole::Qualified);
        assert_eq!(p.tsas[1].role, TsaRole::Independent);
        assert_eq!(p.subject_kinds(), AnchorSubject::HEADS.to_vec());
        assert_eq!(p.min_distinct_tsas, 2);
        assert_eq!(p.require_roles, both_roles());
        assert!(p.tsas[0]
            .declaration
            .as_deref()
            .unwrap()
            .contains("never evaluated"));
        // Defaults apply when omitted.
        let d = load_text(&two_tsas("")).unwrap();
        assert_eq!(d.subjects, all_heads());
        assert_eq!(d.require_roles, both_roles());
        assert_eq!(d.max_clock_offset_s, 600);
        assert_eq!(p.max_clock_offset_s, 600);
        assert_eq!(d.min_distinct_tsas, 2);
    }

    #[test]
    fn error_prefix_names_the_file() {
        let dir = tempfile::tempdir().unwrap();
        let p = write_policy(dir.path(), r#"{"format":"nope","tsas":[]}"#);
        let err = load(&p).unwrap_err().to_string();
        assert!(
            err.starts_with(&format!("anchor policy {}: ", p.display())),
            "{err}"
        );
    }

    #[test]
    fn wrong_format_is_refused() {
        let err = err_of(r#"{"format":"securacv-court-kit:v1","tsas":[]}"#);
        assert!(
            err.ends_with(
                "format must be \"securacv-anchor-policy:v1\" (got \"securacv-court-kit:v1\")"
            ),
            "{err}"
        );
    }

    #[test]
    fn at_least_one_tsa() {
        let err = err_of(r#"{"format":"securacv-anchor-policy:v1","tsas":[]}"#);
        assert!(err.ends_with("at least one TSA is required"), "{err}");
    }

    #[test]
    fn at_least_one_subject() {
        // An explicit empty list is not "the default": it would make the
        // policy a silent no-op (no requests, vacuously SATISFIED).
        let err = err_of(&two_tsas(r#","subjects":[]"#));
        assert!(err.ends_with("at least one subject is required"), "{err}");
    }

    #[test]
    fn duplicate_subject_is_refused() {
        let err = err_of(&two_tsas(r#","subjects":["chain_head","chain_head"]"#));
        assert!(err.ends_with("duplicate subject 'chain_head'"), "{err}");
    }

    #[test]
    fn invalid_name() {
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a b","role":"qualified","url":"https://x","ca":"x"}]}"#,
        );
        assert!(
            err.ends_with("TSA name 'a b' is invalid (1-32 characters of A-Z a-z 0-9 _ -)"),
            "{err}"
        );
        let long = "x".repeat(33);
        let err = err_of(&format!(
            r#"{{"format":"securacv-anchor-policy:v1","tsas":[{{"name":"{long}","role":"qualified","url":"https://x","ca":"x"}}]}}"#
        ));
        assert!(err.contains("is invalid"), "{err}");
    }

    #[test]
    fn duplicate_name() {
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[
              {"name":"a","role":"qualified","url":"https://x","ca":"x"},
              {"name":"a","role":"independent","url":"https://y","ca":"y"}]}"#,
        );
        assert!(err.ends_with("duplicate TSA name 'a'"), "{err}");
    }

    #[test]
    fn https_required_unless_allow_http() {
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified","url":"http://x","ca":"x"}],"require_roles":["qualified"],"min_distinct_tsas":1}"#,
        );
        assert!(
            err.ends_with(
                "TSA 'a': url must start with https:// (set \"allow_http\": true to override)"
            ),
            "{err}"
        );
        load_text(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified","url":"http://x","ca":"x","allow_http":true}],"require_roles":["qualified"],"min_distinct_tsas":1}"#,
        )
        .unwrap();
    }

    #[test]
    fn pin_must_be_64_hex() {
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified","url":"https://x","ca":"x","cert_sha256":["ABCD"]}],"require_roles":["qualified"],"min_distinct_tsas":1}"#,
        );
        assert!(
            err.ends_with("TSA 'a': cert_sha256 entry \"ABCD\" is not 64 hex characters"),
            "{err}"
        );
        // Uppercase hex is not the canonical form and is refused too.
        let upper = "F".repeat(64);
        let err = err_of(&format!(
            r#"{{"format":"securacv-anchor-policy:v1","tsas":[{{"name":"a","role":"qualified","url":"https://x","ca":"x","cert_sha256":["{upper}"]}}],"require_roles":["qualified"],"min_distinct_tsas":1}}"#
        ));
        assert!(err.contains("is not 64 hex characters"), "{err}");
    }

    #[test]
    fn pin_shared_by_two_entries_is_refused() {
        let pin = "a".repeat(64);
        let err = err_of(&format!(
            r#"{{"format":"securacv-anchor-policy:v1","tsas":[
              {{"name":"alpha","role":"qualified","url":"https://x","ca":"x","cert_sha256":["{pin}"]}},
              {{"name":"beta","role":"independent","url":"https://y","ca":"y","cert_sha256":["{pin}"]}}]}}"#
        ));
        assert!(
            err.ends_with(&format!(
                "cert_sha256 \"{pin}\" is pinned by both 'alpha' and 'beta'; a pin must belong to one TSA"
            )),
            "{err}"
        );
    }

    #[test]
    fn required_role_must_be_declared() {
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified","url":"https://x","ca":"x"}],"min_distinct_tsas":1}"#,
        );
        assert!(
            err.ends_with("role 'independent' is required by require_roles but no TSA declares it"),
            "{err}"
        );
    }

    #[test]
    fn min_distinct_bounds() {
        let err = err_of(&two_tsas(r#","min_distinct_tsas":0"#));
        assert!(
            err.ends_with("min_distinct_tsas must be at least 1"),
            "{err}"
        );
        let err = err_of(&two_tsas(r#","min_distinct_tsas":3"#));
        assert!(
            err.ends_with("min_distinct_tsas 3 exceeds the 2 declared TSA(s)"),
            "{err}"
        );
    }

    #[test]
    fn subject_must_be_a_head() {
        let err = err_of(&two_tsas(r#","subjects":["chain_head","digest"]"#));
        assert!(
            err.ends_with(
                "subject 'digest' is not a ledger head (allowed: chain_head, export_receipt_head, break_glass_receipt_head, policy_head, clock_provenance_head)"
            ),
            "{err}"
        );
        let err = err_of(&two_tsas(r#","subjects":["Chain_Head"]"#));
        assert!(
            err.contains("subject 'Chain_Head' is not a ledger head"),
            "{err}"
        );
    }

    #[test]
    fn unknown_fields_are_refused() {
        let err = err_of(&two_tsas(r#","tsa_urls":[]"#));
        assert!(err.contains("unknown field"), "{err}");
        let err = err_of(
            r#"{"format":"securacv-anchor-policy:v1","tsas":[{"name":"a","role":"qualified","url":"https://x","ca":"x","trusted":true}]}"#,
        );
        assert!(err.contains("unknown field"), "{err}");
    }

    #[test]
    fn relative_ca_resolves_against_policy_dir() {
        let dir = tempfile::tempdir().unwrap();
        let sub = dir.path().join("ops");
        std::fs::create_dir_all(&sub).unwrap();
        let p = write_policy(&sub, &two_tsas(""));
        let policy = load(&p).unwrap();
        assert_eq!(policy.tsas[0].ca, sub.join("a.pem"));
        let abs = two_tsas("").replace("\"a.pem\"", "\"/etc/ssl/a.pem\"");
        let policy = load(&write_policy(&sub, &abs)).unwrap();
        assert_eq!(policy.tsas[0].ca, PathBuf::from("/etc/ssl/a.pem"));
    }

    // ---- attribution ----

    fn record(id: i64, subject: &str, hash: [u8; 32], bucket: u64) -> AnchorRecord {
        AnchorRecord {
            id,
            created_bucket: TimeBucket {
                start_epoch_s: bucket,
                size_s: 600,
            },
            subject: subject.to_string(),
            subject_hash: hash,
            tsa_url: "(offline)".to_string(),
            gen_time: "20260610123324Z".to_string(),
            token_der: Vec::new(),
            tsa_name: None,
            signer_fingerprint: None,
            signer_sid: None,
            ledger_id: None,
        }
    }

    fn pinned_policy(alpha_pins: &[&str], beta_pins: &[&str]) -> AnchorPolicy {
        let pins = |p: &[&str]| {
            p.iter()
                .map(|s| format!("\"{s}\""))
                .collect::<Vec<_>>()
                .join(",")
        };
        load_text(&format!(
            r#"{{"format":"securacv-anchor-policy:v1","tsas":[
              {{"name":"alpha","role":"qualified","url":"https://a","ca":"a.pem","cert_sha256":[{}]}},
              {{"name":"beta","role":"independent","url":"https://b","ca":"b.pem","cert_sha256":[{}]}},
              {{"name":"gamma","role":"independent","url":"https://c","ca":"c.pem"}}
            ]}}"#,
            pins(alpha_pins),
            pins(beta_pins)
        ))
        .unwrap()
    }

    const CERT_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const CERT_B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    #[test]
    fn attribute_one_ca_is_one() {
        let policy = pinned_policy(&[], &[]);
        let a = record(1, "chain_head", [1u8; 32], 0);
        let v = Verified {
            anchor: &a,
            under: vec!["beta".into()],
        };
        assert_eq!(
            attribute(&policy, &v, None),
            Attribution::One("beta".into())
        );
        assert_eq!(
            attribute(&policy, &v, Some(CERT_A)),
            Attribution::One("beta".into())
        );
        let none = Verified {
            anchor: &a,
            under: vec![],
        };
        assert_eq!(attribute(&policy, &none, Some(CERT_A)), Attribution::None);
    }

    #[test]
    fn attribute_two_cas_one_pin_resolves() {
        let policy = pinned_policy(&[CERT_A], &[]);
        let a = record(1, "chain_head", [1u8; 32], 0);
        let v = Verified {
            anchor: &a,
            under: vec!["alpha".into(), "beta".into()],
        };
        assert_eq!(
            attribute(&policy, &v, Some(CERT_A)),
            Attribution::AmbiguousResolvedByPin("alpha".into())
        );
    }

    #[test]
    fn attribute_two_cas_no_pins_is_ambiguous() {
        let policy = pinned_policy(&[], &[]);
        let a = record(1, "chain_head", [1u8; 32], 0);
        let v = Verified {
            anchor: &a,
            under: vec!["alpha".into(), "beta".into()],
        };
        assert_eq!(
            attribute(&policy, &v, Some(CERT_A)),
            Attribution::Ambiguous(vec!["alpha".into(), "beta".into()])
        );
        assert_eq!(
            attribute(&policy, &v, None),
            Attribution::Ambiguous(vec!["alpha".into(), "beta".into()])
        );
    }

    #[test]
    fn attribute_pins_on_both_entries_is_ambiguous() {
        // Distinct pins on both entries, but the token's cert matches neither
        // uniquely — here both entries pin CERT_A via different hex? Not
        // allowed at load (duplicate pin), so the ambiguous case is: both
        // entries have pins and the token's cert is pinned by neither.
        let policy = pinned_policy(&[CERT_A], &[CERT_B]);
        let a = record(1, "chain_head", [1u8; 32], 0);
        let v = Verified {
            anchor: &a,
            under: vec!["alpha".into(), "beta".into()],
        };
        let other = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
        assert_eq!(
            attribute(&policy, &v, Some(other)),
            Attribution::Ambiguous(vec!["alpha".into(), "beta".into()])
        );
        // And with the cert matching exactly one, that one wins.
        assert_eq!(
            attribute(&policy, &v, Some(CERT_B)),
            Attribution::AmbiguousResolvedByPin("beta".into())
        );
    }

    #[test]
    fn attribute_pin_outside_under_never_attributes() {
        // beta pins the cert, but only alpha and gamma verified the row.
        let policy = pinned_policy(&[], &[CERT_A]);
        let a = record(1, "chain_head", [1u8; 32], 0);
        let v = Verified {
            anchor: &a,
            under: vec!["alpha".into(), "gamma".into()],
        };
        assert_eq!(
            attribute(&policy, &v, Some(CERT_A)),
            Attribution::Ambiguous(vec!["alpha".into(), "gamma".into()])
        );
        // A single verifying CA is attributed regardless of pins elsewhere.
        let one = Verified {
            anchor: &a,
            under: vec!["alpha".into()],
        };
        assert_eq!(
            attribute(&policy, &one, Some(CERT_A)),
            Attribution::One("alpha".into())
        );
    }

    // ---- evaluate ----

    fn eval_policy() -> AnchorPolicy {
        load_text(&two_tsas(r#","subjects":["chain_head"]"#)).unwrap()
    }

    #[test]
    fn evaluate_covered_current() {
        let policy = eval_policy();
        let h = [7u8; 32];
        let r1 = record(1, "chain_head", h, 600);
        let r2 = record(2, "chain_head", h, 600);
        let rows = vec![
            Attributed {
                anchor: &r1,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &r2,
                name: "beta".into(),
            },
        ];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(h))], &rows);
        assert_eq!(out.len(), 1);
        assert_eq!(
            out[0].verdict,
            CoverageVerdict::Covered {
                hash: h,
                by: vec![
                    ("alpha".into(), TsaRole::Qualified),
                    ("beta".into(), TsaRole::Independent)
                ],
                anchor_ids: vec![1, 2],
                bucket_start: 600,
                is_current: true,
            }
        );
    }

    #[test]
    fn evaluate_prefers_a_covered_current_head_over_a_later_imported_older_hash() {
        // The head was covered in bucket 600; an OLDER hash's responses were
        // imported afterwards (bucket 1200, higher ids). Import order must
        // not turn "current head: yes" into "NO".
        let policy = eval_policy();
        let old = [1u8; 32];
        let head = [3u8; 32];
        let a_head = record(1, "chain_head", head, 600);
        let b_head = record(2, "chain_head", head, 600);
        let a_old = record(3, "chain_head", old, 1200);
        let b_old = record(4, "chain_head", old, 1200);
        let rows = vec![
            Attributed {
                anchor: &a_head,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &b_head,
                name: "beta".into(),
            },
            Attributed {
                anchor: &a_old,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &b_old,
                name: "beta".into(),
            },
        ];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(head))], &rows);
        match &out[0].verdict {
            CoverageVerdict::Covered {
                hash,
                is_current,
                anchor_ids,
                ..
            } => {
                assert_eq!(*hash, head);
                assert!(is_current);
                assert_eq!(anchor_ids, &[1, 2]);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn evaluate_covered_stale_prefers_newest_covered_hash() {
        let policy = eval_policy();
        let old = [1u8; 32];
        let newer = [2u8; 32];
        let head = [3u8; 32];
        let a1 = record(1, "chain_head", old, 600);
        let b1 = record(2, "chain_head", old, 600);
        let a2 = record(3, "chain_head", newer, 1200);
        let b2 = record(4, "chain_head", newer, 1200);
        let a3 = record(5, "chain_head", head, 1800); // alpha only
        let rows = vec![
            Attributed {
                anchor: &a1,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &b1,
                name: "beta".into(),
            },
            Attributed {
                anchor: &a2,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &b2,
                name: "beta".into(),
            },
            Attributed {
                anchor: &a3,
                name: "alpha".into(),
            },
        ];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(head))], &rows);
        match &out[0].verdict {
            CoverageVerdict::Covered {
                hash,
                is_current,
                anchor_ids,
                ..
            } => {
                assert_eq!(*hash, newer);
                assert!(!is_current);
                assert_eq!(anchor_ids, &[3, 4]);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn evaluate_roles_held_by_one_name_is_not_covered() {
        // Two rows, both attributed to alpha (qualified): a duplicate row
        // over an unchanged head is expected from anchor-all and must not
        // count as two TSAs.
        let policy = eval_policy();
        let h = [7u8; 32];
        let r1 = record(1, "chain_head", h, 600);
        let r2 = record(2, "chain_head", h, 1200);
        let rows = vec![
            Attributed {
                anchor: &r1,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &r2,
                name: "alpha".into(),
            },
        ];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(h))], &rows);
        assert_eq!(
            out[0].verdict,
            CoverageVerdict::NotCovered {
                anchored_by: vec![("alpha".into(), TsaRole::Qualified)],
                missing_roles: vec![TsaRole::Independent],
                distinct: 1,
            }
        );
    }

    #[test]
    fn evaluate_fewer_than_min_distinct() {
        // Roles satisfied by name count is impossible below min; here
        // require_roles is just [independent] and min is 2.
        let policy = load_text(&two_tsas(
            r#","subjects":["chain_head"],"require_roles":["independent"],"min_distinct_tsas":2"#,
        ))
        .unwrap();
        let h = [7u8; 32];
        let r1 = record(1, "chain_head", h, 600);
        let rows = vec![Attributed {
            anchor: &r1,
            name: "beta".into(),
        }];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(h))], &rows);
        assert_eq!(
            out[0].verdict,
            CoverageVerdict::NotCovered {
                anchored_by: vec![("beta".into(), TsaRole::Independent)],
                missing_roles: vec![],
                distinct: 1,
            }
        );
    }

    #[test]
    fn evaluate_empty_ledger() {
        let policy = eval_policy();
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, None)], &[]);
        assert_eq!(out[0].verdict, CoverageVerdict::Empty);
        assert_eq!(out[0].head, None);
        // A subject with no head entry at all is also Empty.
        let out = evaluate(&policy, &[], &[]);
        assert_eq!(out[0].verdict, CoverageVerdict::Empty);
    }

    #[test]
    fn evaluate_ignores_sentinel_rows_and_other_subjects() {
        let policy = eval_policy();
        let head = [9u8; 32];
        let sentinel = AnchorSubject::ChainHead.empty_ledger_sentinel().unwrap();
        let s1 = record(1, "digest", sentinel, 600);
        let s2 = record(2, "digest", sentinel, 600);
        let e1 = record(3, "export_receipt_head", head, 600);
        let e2 = record(4, "export_receipt_head", head, 600);
        let rows = vec![
            Attributed {
                anchor: &s1,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &s2,
                name: "beta".into(),
            },
            Attributed {
                anchor: &e1,
                name: "alpha".into(),
            },
            Attributed {
                anchor: &e2,
                name: "beta".into(),
            },
        ];
        let out = evaluate(&policy, &[(AnchorSubject::ChainHead, Some(head))], &rows);
        assert_eq!(
            out[0].verdict,
            CoverageVerdict::NotCovered {
                anchored_by: vec![],
                missing_roles: vec![TsaRole::Qualified, TsaRole::Independent],
                distinct: 0,
            }
        );
    }

    #[test]
    fn max_clock_offset_s_defaults_to_one_bucket_and_must_be_positive() {
        let p = load_text(&two_tsas(r#","max_clock_offset_s":30"#)).unwrap();
        assert_eq!(p.max_clock_offset_s, 30);
        let err = err_of(&two_tsas(r#","max_clock_offset_s":0"#));
        assert!(
            err.contains("max_clock_offset_s must be at least 1 (got 0)"),
            "{err}"
        );
        let err = err_of(&two_tsas(r#","max_clock_offset_s":-5"#));
        assert!(
            err.contains("max_clock_offset_s must be at least 1 (got -5)"),
            "{err}"
        );
    }

    #[test]
    fn subject_error_lists_all_five_heads() {
        let err = err_of(&two_tsas(r#","subjects":["clock_provenance_head","nope"]"#));
        assert!(
            err.ends_with(
                "subject 'nope' is not a ledger head (allowed: chain_head, export_receipt_head, \
                 break_glass_receipt_head, policy_head, clock_provenance_head)"
            ),
            "{err}"
        );
        let p = load_text(&two_tsas(r#","subjects":["clock_provenance_head"]"#)).unwrap();
        assert_eq!(p.subject_kinds(), vec![AnchorSubject::ClockProvenanceHead]);
    }

    // -------------------- the OpenTimestamps block --------------------

    const ALICE: &str = "https://alice.btc.calendar.opentimestamps.org";
    const BOB: &str = "https://bob.btc.calendar.opentimestamps.org";

    fn with_ots(ots: &str) -> String {
        two_tsas(&format!(r#","ots":{ots}"#))
    }

    #[test]
    fn ots_absent_is_none_example_unchanged() {
        let p = load_text(EXAMPLE).unwrap();
        assert!(p.ots.is_none());
        let p = load_text(&two_tsas("")).unwrap();
        assert!(p.ots.is_none());
    }

    #[test]
    fn ots_block_parses_defaults_required_false() {
        let p = load_text(&with_ots(&format!(
            r#"{{"calendars":[{{"name":"alice","url":"{ALICE}"}},{{"name":"bob","url":"{BOB}"}}]}}"#
        )))
        .unwrap();
        let ots = p.ots.unwrap();
        assert!(!ots.required);
        assert_eq!(ots.calendars.len(), 2);
        assert_eq!(ots.calendar("bob").unwrap().url, BOB);
        assert_eq!(ots.calendar_for_url(ALICE).unwrap().name, "alice");
        assert!(ots.calendar_for_url(&format!("{ALICE}/")).is_none());
        assert!(!ots.calendars[0].allow_http);
        let p = load_text(&with_ots(&format!(
            r#"{{"calendars":[{{"name":"alice","url":"{ALICE}"}}],"required":true}}"#
        )))
        .unwrap();
        assert!(p.ots.unwrap().required);
    }

    #[test]
    fn ots_validation_literals() {
        let cal = |name: &str, url: &str| format!(r#"{{"name":"{name}","url":"{url}"}}"#);
        let cases: Vec<(String, String)> = vec![
            (
                r#"{"calendars":[]}"#.to_string(),
                "ots: at least one calendar is required".to_string(),
            ),
            (
                format!(
                    r#"{{"calendars":[{}]}}"#,
                    (0..9)
                        .map(|i| cal(&format!("c{i}"), &format!("https://c{i}.example")))
                        .collect::<Vec<_>>()
                        .join(",")
                ),
                "ots: at most 8 calendars".to_string(),
            ),
            (
                format!(r#"{{"calendars":[{}]}}"#, cal("bad name", ALICE)),
                "calendar name 'bad name' is invalid (1-32 characters of A-Z a-z 0-9 _ -)"
                    .to_string(),
            ),
            (
                format!(
                    r#"{{"calendars":[{},{}]}}"#,
                    cal("alice", ALICE),
                    cal("alice", BOB)
                ),
                "duplicate calendar name 'alice'".to_string(),
            ),
            (
                format!(r#"{{"calendars":[{}]}}"#, cal("alpha", ALICE)),
                "name 'alpha' is used by both a TSA and a calendar".to_string(),
            ),
            (
                format!(
                    r#"{{"calendars":[{}]}}"#,
                    cal("alice", "http://alice.example")
                ),
                "calendar 'alice': url must start with https:// (set \"allow_http\": true to override)"
                    .to_string(),
            ),
            (
                format!(r#"{{"calendars":[{}]}}"#, cal("alice", &format!("{ALICE}/"))),
                "calendar 'alice': url must not end with '/' or carry a query or fragment"
                    .to_string(),
            ),
            (
                format!(r#"{{"calendars":[{}]}}"#, cal("alice", &format!("{ALICE}?x=1"))),
                "calendar 'alice': url must not end with '/' or carry a query or fragment"
                    .to_string(),
            ),
            (
                format!(r#"{{"calendars":[{}]}}"#, cal("alice", "https://al ice.example")),
                "calendar 'alice': url contains a character outside A-Z a-z 0-9 . _ / : - (a pending attestation could not name it)"
                    .to_string(),
            ),
            (
                format!(
                    r#"{{"calendars":[{},{}]}}"#,
                    cal("alice", ALICE),
                    cal("bob", ALICE)
                ),
                format!("duplicate calendar url '{ALICE}'"),
            ),
        ];
        for (ots, want) in cases {
            let err = err_of(&with_ots(&ots));
            assert!(err.ends_with(&want), "want …{want}\n got {err}");
            assert!(err.starts_with("anchor policy "), "{err}");
        }
        // http:// is accepted with allow_http.
        let p = load_text(&with_ots(
            r#"{"calendars":[{"name":"local","url":"http://127.0.0.1:14788","allow_http":true}]}"#,
        ))
        .unwrap();
        assert!(p.ots.unwrap().calendars[0].allow_http);
    }

    #[test]
    fn ots_unknown_field_refused() {
        let err = err_of(&with_ots(&format!(
            r#"{{"calendars":[{{"name":"alice","url":"{ALICE}"}}],"min_calendars":2}}"#
        )));
        assert!(err.contains("unknown field `min_calendars`"), "{err}");
        let err = err_of(&with_ots(&format!(
            r#"{{"calendars":[{{"name":"alice","url":"{ALICE}","role":"qualified"}}]}}"#
        )));
        assert!(err.contains("unknown field `role`"), "{err}");
    }

    fn ots_policy() -> AnchorPolicy {
        load_text(&two_tsas(&format!(
            r#","subjects":["chain_head","policy_head"],"ots":{{"calendars":[{{"name":"alice","url":"{ALICE}"}}],"required":true}}"#
        )))
        .unwrap()
    }

    fn passed(id: i64, subject: AnchorSubject, hash: u8, bucket: u64, state: OtsRowState) -> OtsPassed {
        OtsPassed {
            id,
            subject,
            hash: [hash; 32],
            bucket_start: bucket,
            state,
        }
    }

    #[test]
    fn evaluate_ots_current_head_wins_newest_confirmed_otherwise() {
        let p = ots_policy();
        let heads = [
            (AnchorSubject::ChainHead, Some([1u8; 32])),
            (AnchorSubject::PolicyHead, Some([9u8; 32])),
        ];
        let rows = vec![
            // The current chain head, confirmed at two heights: lowest wins.
            passed(1, AnchorSubject::ChainHead, 1, 600, OtsRowState::Confirmed { height: 12 }),
            passed(2, AnchorSubject::ChainHead, 1, 600, OtsRowState::Confirmed { height: 10 }),
            // An older chain head confirmed in a NEWER bucket: the current head still wins.
            passed(3, AnchorSubject::ChainHead, 2, 1200, OtsRowState::Confirmed { height: 11 }),
            // Policy head: two historical hashes confirmed, the head itself pending.
            passed(4, AnchorSubject::PolicyHead, 7, 600, OtsRowState::Confirmed { height: 5 }),
            passed(5, AnchorSubject::PolicyHead, 8, 1200, OtsRowState::Confirmed { height: 6 }),
            passed(6, AnchorSubject::PolicyHead, 9, 1800, OtsRowState::Pending),
        ];
        let cov = evaluate_ots(&p, &heads, &rows);
        assert_eq!(cov.len(), 2);
        assert_eq!(
            cov[0].verdict,
            OtsCoverageVerdict::Confirmed {
                hash: [1; 32],
                anchor_ids: vec![1, 2],
                height: 10,
                is_current: true
            }
        );
        assert_eq!(
            cov[1].verdict,
            OtsCoverageVerdict::Confirmed {
                hash: [8; 32],
                anchor_ids: vec![5],
                height: 6,
                is_current: false
            }
        );
    }

    #[test]
    fn evaluate_ots_pending_and_unchecked_never_cover_sentinel_and_digest_rows_ignored() {
        let p = ots_policy();
        let sentinel = AnchorSubject::PolicyHead.empty_ledger_sentinel().unwrap();
        let heads = [
            (AnchorSubject::ChainHead, Some([1u8; 32])),
            (AnchorSubject::PolicyHead, None),
        ];
        let mut rows = vec![
            passed(1, AnchorSubject::ChainHead, 1, 600, OtsRowState::Pending),
            passed(2, AnchorSubject::ChainHead, 1, 600, OtsRowState::Unchecked { height: 3 }),
            passed(3, AnchorSubject::ChainHead, 1, 600, OtsRowState::Pending),
            // A confirmed DIGEST row over the chain head's hash never counts.
            passed(4, AnchorSubject::Digest, 1, 600, OtsRowState::Confirmed { height: 3 }),
        ];
        rows.push(OtsPassed {
            id: 5,
            subject: AnchorSubject::Digest,
            hash: sentinel,
            bucket_start: 600,
            state: OtsRowState::Confirmed { height: 3 },
        });
        let cov = evaluate_ots(&p, &heads, &rows);
        assert_eq!(
            cov[0].verdict,
            OtsCoverageVerdict::NotConfirmed {
                pending: 2,
                unchecked: 1
            }
        );
        assert_eq!(cov[1].verdict, OtsCoverageVerdict::Empty);
    }
}
