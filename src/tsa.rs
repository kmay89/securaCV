//! RFC 3161 trusted timestamping for chain heads and export digests.
//!
//! A Time Stamping Authority (TSA) countersigns a SHA-256 digest with its
//! own key and clock, producing a token that proves the digest existed at a
//! point in time *independently of this device*. Anchoring the witness
//! chain head means a verifier no longer has to trust the device clock or
//! the device key alone: even if the device key were compromised later, an
//! attacker cannot back-date forged history past an existing anchor.
//!
//! Privacy posture (Invariants I/III/IV): a timestamp request carries only
//! a 32-byte digest and a random nonce — no events, no metadata, no
//! identifiers. It is still an *outbound network call*, which SecuraCV
//! never makes on its own: anchoring is operator-initiated (`log_anchor`)
//! or fully offline (write the query to a file, submit it out-of-band,
//! import the response). The TSA does learn the requester's IP address and
//! the time of the request — see docs/timestamping.md. Scheduled anchoring
//! (`log_anchor anchor-all`) sends a constant number of requests per run —
//! every configured subject to every configured TSA, an empty ledger anchored
//! over a fixed sentinel — so the request *count* never tracks whether an
//! export, a break-glass unseal, or a policy change happened (Invariant III).
//! The imprints themselves are visible to the TSA; see docs/timestamping.md
//! for the residual.
//!
//! Scope: this module builds `TimeStampReq` and parses `TimeStampResp`
//! (RFC 3161 §2.4) with a minimal DER reader — enough to check the granted
//! status, match the message imprint and nonce, and extract `genTime` /
//! serial / policy. It does **not** validate the CMS countersignature
//! chain; that final cryptographic step is delegated to an independent
//! toolchain (`openssl ts -verify`), which `log_anchor verify` can invoke.
//! Relying on a second implementation for the trust-critical step is
//! deliberate (same stance as the dual JS/Rust envelope verifiers).
//! A separate best-effort reader (`parse_token_signer`) extracts the TSA's
//! embedded signing certificate and `SignerInfo` identity so an
//! offline-imported token still names its issuer; it does not validate that
//! certificate — `openssl ts -verify` does.

use anyhow::{anyhow, bail, Result};

/// DER object identifier for SHA-256 (2.16.840.1.101.3.4.2.1), tag+len+body.
const OID_SHA256: &[u8] = &[
    0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
];
/// id-signedData (1.2.840.113549.1.7.2), body only.
const OID_SIGNED_DATA: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02];
/// id-ct-TSTInfo (1.2.840.113549.1.9.16.1.4), body only.
const OID_TST_INFO: &[u8] = &[
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x01, 0x04,
];

// -------------------- DER writing --------------------

fn der_len(len: usize) -> Vec<u8> {
    if len < 0x80 {
        return vec![len as u8];
    }
    let bytes = len.to_be_bytes();
    let first = bytes
        .iter()
        .position(|&b| b != 0)
        .unwrap_or(bytes.len() - 1);
    let mut out = vec![0x80 | (bytes.len() - first) as u8];
    out.extend_from_slice(&bytes[first..]);
    out
}

fn der_tlv(tag: u8, content: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    out.extend(der_len(content.len()));
    out.extend_from_slice(content);
    out
}

/// Encode an unsigned big-endian value as a DER INTEGER (positive).
fn der_uint(value: &[u8]) -> Vec<u8> {
    let first = value.iter().position(|&b| b != 0).unwrap_or(value.len());
    let trimmed = &value[first.min(value.len().saturating_sub(1))..];
    let trimmed = if trimmed.is_empty() {
        &[0u8][..]
    } else {
        trimmed
    };
    let mut body = Vec::with_capacity(trimmed.len() + 1);
    if trimmed[0] & 0x80 != 0 {
        body.push(0x00);
    }
    body.extend_from_slice(trimmed);
    der_tlv(0x02, &body)
}

/// Build a DER `TimeStampReq` (RFC 3161 §2.4.1) over a SHA-256 digest.
///
/// `cert_req = true` asks the TSA to embed its signing certificate so the
/// token remains verifiable after the TSA rotates keys.
pub fn build_request(digest: &[u8; 32], nonce: Option<&[u8]>, cert_req: bool) -> Vec<u8> {
    let mut body = Vec::new();
    body.extend(der_tlv(0x02, &[0x01])); // version 1
    let mut alg = Vec::new();
    alg.extend_from_slice(OID_SHA256);
    alg.extend(der_tlv(0x05, &[])); // parameters NULL
    let mut imprint = der_tlv(0x30, &alg);
    imprint.extend(der_tlv(0x04, digest));
    body.extend(der_tlv(0x30, &imprint));
    if let Some(nonce) = nonce {
        body.extend(der_uint(nonce));
    }
    if cert_req {
        body.extend(der_tlv(0x01, &[0xff]));
    }
    der_tlv(0x30, &body)
}

/// Random 8-byte nonce for request/response correlation (replay defense).
pub fn random_nonce() -> [u8; 8] {
    let mut nonce = [0u8; 8];
    rand::fill(&mut nonce[..]);
    nonce
}

// -------------------- DER reading --------------------

struct Der<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Der<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    fn has_more(&self) -> bool {
        self.pos < self.data.len()
    }

    fn peek_tag(&self) -> Option<u8> {
        self.data.get(self.pos).copied()
    }

    /// Read one TLV; returns (tag, content, raw including header).
    fn tlv(&mut self) -> Result<(u8, &'a [u8], &'a [u8])> {
        let start = self.pos;
        let tag = *self
            .data
            .get(self.pos)
            .ok_or_else(|| anyhow!("DER: truncated at tag"))?;
        self.pos += 1;
        let first = *self
            .data
            .get(self.pos)
            .ok_or_else(|| anyhow!("DER: truncated at length"))?;
        self.pos += 1;
        let len = if first < 0x80 {
            first as usize
        } else {
            let n = (first & 0x7f) as usize;
            if n == 0 || n > 4 {
                bail!("DER: unsupported length encoding");
            }
            let mut len = 0usize;
            for _ in 0..n {
                let b = *self
                    .data
                    .get(self.pos)
                    .ok_or_else(|| anyhow!("DER: truncated length bytes"))?;
                self.pos += 1;
                len = (len << 8) | b as usize;
            }
            len
        };
        let end = self
            .pos
            .checked_add(len)
            .filter(|&e| e <= self.data.len())
            .ok_or_else(|| anyhow!("DER: content overruns buffer"))?;
        let content = &self.data[self.pos..end];
        self.pos = end;
        Ok((tag, content, &self.data[start..end]))
    }

    fn expect(&mut self, want: u8, what: &str) -> Result<&'a [u8]> {
        let (tag, content, _) = self.tlv()?;
        if tag != want {
            bail!(
                "DER: expected {} (tag 0x{:02x}), got 0x{:02x}",
                what,
                want,
                tag
            );
        }
        Ok(content)
    }
}

fn oid_to_dotted(body: &[u8]) -> String {
    let mut parts = Vec::new();
    if let Some(&first) = body.first() {
        parts.push((first / 40).to_string());
        parts.push((first % 40).to_string());
    }
    let mut acc: u64 = 0;
    for &b in &body[1.min(body.len())..] {
        acc = (acc << 7) | u64::from(b & 0x7f);
        if b & 0x80 == 0 {
            parts.push(acc.to_string());
            acc = 0;
        }
    }
    parts.join(".")
}

/// Strip leading zero octets so DER INTEGER paddings compare equal.
fn uint_normalize(v: &[u8]) -> &[u8] {
    let first = v.iter().position(|&b| b != 0).unwrap_or(v.len());
    &v[first..]
}

// -------------------- Parsed token --------------------

/// The fields of a granted timestamp, extracted from `TimeStampResp`.
#[derive(Debug, Clone)]
pub struct TimestampToken {
    /// PKIStatus: 0 = granted, 1 = grantedWithMods.
    pub status: u32,
    /// `TSTInfo.genTime` as the TSA wrote it, e.g. `"20260610123324Z"`.
    pub gen_time: String,
    /// TSA policy OID, dotted form.
    pub policy_oid: String,
    /// Token serial number, hex (no 0x prefix).
    pub serial_hex: String,
    /// The 32-byte SHA-256 message imprint the TSA signed over.
    pub imprint: Vec<u8>,
    /// Nonce echoed by the TSA, normalized (no leading zeros), if present.
    pub nonce: Option<Vec<u8>>,
    /// The complete DER `TimeStampToken` (CMS ContentInfo) for storage and
    /// independent verification (`openssl ts -verify`).
    pub token_der: Vec<u8>,
}

/// Parse a DER `TimeStampResp`. Fails if the TSA did not grant the request.
pub fn parse_response(der: &[u8]) -> Result<TimestampToken> {
    let mut top = Der::new(der);
    let resp = top.expect(0x30, "TimeStampResp")?;
    let mut resp = Der::new(resp);

    // PKIStatusInfo ::= SEQUENCE { status INTEGER, statusString SEQ OF
    // UTF8String OPTIONAL, failInfo BIT STRING OPTIONAL }
    let status_info = resp.expect(0x30, "PKIStatusInfo")?;
    let mut si = Der::new(status_info);
    let status_bytes = si.expect(0x02, "status")?;
    let status = uint_normalize(status_bytes)
        .iter()
        .fold(0u32, |acc, &b| (acc << 8) | u32::from(b));
    if status > 1 {
        let mut detail = String::new();
        if si.peek_tag() == Some(0x30) {
            let texts = si.expect(0x30, "statusString")?;
            let mut t = Der::new(texts);
            while t.has_more() {
                let (_, s, _) = t.tlv()?;
                if !detail.is_empty() {
                    detail.push_str("; ");
                }
                detail.push_str(&String::from_utf8_lossy(s));
            }
        }
        bail!(
            "TSA rejected the request (status {}){}{}",
            status,
            if detail.is_empty() { "" } else { ": " },
            detail
        );
    }

    if !resp.has_more() {
        bail!("TSA response granted but carries no timeStampToken");
    }
    let (tag, _, token_der) = resp.tlv()?;
    if tag != 0x30 {
        bail!("DER: expected TimeStampToken ContentInfo, got tag 0x{tag:02x}");
    }
    let mut token = parse_token(token_der)?;
    token.status = status;
    Ok(token)
}

/// Parse a bare DER `TimeStampToken` (CMS ContentInfo wrapping TSTInfo), as
/// stored in `tsa_anchors.token_der`.
pub fn parse_token(token_der: &[u8]) -> Result<TimestampToken> {
    // ContentInfo → SignedData through encapContentInfo is shared with
    // `parse_token_signer` (`signed_data_reader`) so the two cannot diverge.
    let (_sd, encap) = signed_data_reader(token_der)?;
    let mut ec = Der::new(encap);
    let e_type = ec.expect(0x06, "eContentType")?;
    if e_type != OID_TST_INFO {
        bail!("SignedData payload is not TSTInfo");
    }
    let e_content = ec.expect(0xa0, "eContent [0]")?;
    let tst_der = Der::new(e_content).expect(0x04, "eContent OCTET STRING")?;

    // TSTInfo ::= SEQUENCE { version, policy OID, messageImprint, serial
    // INTEGER, genTime GeneralizedTime, accuracy?, ordering?, nonce?, … }
    let tst = Der::new(tst_der).expect(0x30, "TSTInfo")?;
    let mut t = Der::new(tst);
    t.expect(0x02, "TSTInfo.version")?;
    let policy = t.expect(0x06, "policy")?;
    let imprint_seq = t.expect(0x30, "messageImprint")?;
    let mut mi = Der::new(imprint_seq);
    let alg_seq = mi.expect(0x30, "hashAlgorithm")?;
    let alg_oid = Der::new(alg_seq).expect(0x06, "hash OID")?;
    if der_tlv(0x06, alg_oid) != OID_SHA256 {
        bail!(
            "TSA used digest algorithm {} (only SHA-256 is supported)",
            oid_to_dotted(alg_oid)
        );
    }
    let imprint = mi.expect(0x04, "hashedMessage")?;
    let serial = t.expect(0x02, "serialNumber")?;
    let gen_time = t.expect(0x18, "genTime")?;
    let gen_time = std::str::from_utf8(gen_time)
        .map_err(|_| anyhow!("genTime is not ASCII"))?
        .to_string();

    // Optional fields, in schema order: accuracy SEQUENCE, ordering BOOLEAN,
    // nonce INTEGER. (tsa [0] / extensions [1] follow; not needed here.)
    if t.peek_tag() == Some(0x30) {
        t.tlv()?;
    }
    if t.peek_tag() == Some(0x01) {
        t.tlv()?;
    }
    let nonce = if t.peek_tag() == Some(0x02) {
        let (_, n, _) = t.tlv()?;
        Some(uint_normalize(n).to_vec())
    } else {
        None
    };

    Ok(TimestampToken {
        status: 0,
        gen_time,
        policy_oid: oid_to_dotted(policy),
        serial_hex: hex::encode(uint_normalize(serial)),
        imprint: imprint.to_vec(),
        nonce,
        token_der: token_der.to_vec(),
    })
}

/// The 32-byte message imprint a stored token covers.
pub fn parse_token_imprint(token_der: &[u8]) -> Result<[u8; 32]> {
    let token = parse_token(token_der)?;
    token
        .imprint
        .try_into()
        .map_err(|_| anyhow!("token imprint is not a 32-byte SHA-256 digest"))
}

/// Check that a granted token covers exactly the digest (and nonce) we sent.
/// This is the structural half of verification; the CMS countersignature is
/// checked independently (`openssl ts -verify`).
pub fn verify_match(
    token: &TimestampToken,
    expected_digest: &[u8; 32],
    expected_nonce: Option<&[u8]>,
) -> Result<()> {
    if token.imprint != expected_digest {
        bail!(
            "message imprint mismatch: token covers {}, expected {}",
            hex::encode(&token.imprint),
            hex::encode(expected_digest)
        );
    }
    match (expected_nonce, &token.nonce) {
        (Some(sent), Some(got)) if uint_normalize(sent) != got.as_slice() => {
            bail!("nonce mismatch: possible replayed response");
        }
        (Some(_), None) => bail!("TSA dropped the nonce: possible replayed response"),
        _ => Ok(()),
    }
}

/// Parse `genTime` ("YYYYMMDDHHMMSS[.f...]Z") to a Unix timestamp.
pub fn gen_time_unix(gen_time: &str) -> Option<i64> {
    let digits = gen_time.strip_suffix('Z')?;
    let digits = digits.split('.').next()?;
    if digits.len() != 14 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let num = |range: std::ops::Range<usize>| digits[range].parse::<i64>().ok();
    let (y, m, d) = (num(0..4)?, num(4..6)?, num(6..8)?);
    let (hh, mm, ss) = (num(8..10)?, num(10..12)?, num(12..14)?);
    if !(1..=12).contains(&m) || !(1..=31).contains(&d) || hh > 23 || mm > 59 || ss > 60 {
        return None;
    }
    // Days-from-civil (Howard Hinnant's algorithm), valid for all Gregorian dates.
    let y_adj = if m <= 2 { y - 1 } else { y };
    let era = y_adj.div_euclid(400);
    let yoe = y_adj - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    Some(days * 86_400 + hh * 3_600 + mm * 60 + ss)
}

// -------------------- Anchor subjects and TSA identity --------------------

/// What an anchor row says it covers. The stored literal (`as_str`) is also
/// the CLI `--subject` value, the anchor-policy `subjects` entry, the
/// `list`/`verify` output and the court-kit file-name component — one
/// vocabulary, no mapping layer. A label is what a row *claims*; every
/// consumer re-derives what it can from the token and the ledgers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AnchorSubject {
    /// The sealed-event chain head (a sealed event's entry hash or a
    /// retention checkpoint head).
    ChainHead,
    /// An operator-supplied artifact digest (export bundle bytes, or an
    /// empty-ledger sentinel); no ledger membership is asserted.
    Digest,
    /// Head of the export-receipt chain.
    ExportReceiptHead,
    /// Head of the break-glass receipt chain.
    BreakGlassReceiptHead,
    /// Head of the policy-change history.
    PolicyHead,
}

/// Domain prefix of the per-subject empty-ledger sentinel digest.
const EMPTY_LEDGER_SENTINEL_PREFIX: &[u8] = b"securacv:anchor:empty-ledger:v1:";

impl AnchorSubject {
    /// Every head kind, in the order `anchor-all` and `classify_hash` use.
    pub const HEADS: [AnchorSubject; 4] = [
        AnchorSubject::ChainHead,
        AnchorSubject::ExportReceiptHead,
        AnchorSubject::BreakGlassReceiptHead,
        AnchorSubject::PolicyHead,
    ];

    /// The stored literal.
    pub const fn as_str(self) -> &'static str {
        match self {
            AnchorSubject::ChainHead => "chain_head",
            AnchorSubject::Digest => "digest",
            AnchorSubject::ExportReceiptHead => "export_receipt_head",
            AnchorSubject::BreakGlassReceiptHead => "break_glass_receipt_head",
            AnchorSubject::PolicyHead => "policy_head",
        }
    }

    /// Exact literal match only (`"Chain_Head"`, `"chain-head"`, `""` are `None`).
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "chain_head" => Some(AnchorSubject::ChainHead),
            "digest" => Some(AnchorSubject::Digest),
            "export_receipt_head" => Some(AnchorSubject::ExportReceiptHead),
            "break_glass_receipt_head" => Some(AnchorSubject::BreakGlassReceiptHead),
            "policy_head" => Some(AnchorSubject::PolicyHead),
            _ => None,
        }
    }

    /// The historical fallthrough, now named: unknown row text is treated as
    /// a digest anchor (no ledger membership asserted).
    pub fn from_row(s: &str) -> Self {
        Self::parse(s).unwrap_or(AnchorSubject::Digest)
    }

    /// All kinds but `Digest`.
    pub const fn is_head(self) -> bool {
        !matches!(self, AnchorSubject::Digest)
    }

    /// The ledger table a head kind lives in. `ChainHead` is special-cased by
    /// its readers (sealed events plus checkpoints) and returns `None`, as
    /// does `Digest`.
    pub const fn table(self) -> Option<&'static str> {
        match self {
            AnchorSubject::ChainHead | AnchorSubject::Digest => None,
            AnchorSubject::ExportReceiptHead => Some("export_receipts"),
            AnchorSubject::BreakGlassReceiptHead => Some("break_glass_receipts"),
            AnchorSubject::PolicyHead => Some("policy_change_history"),
        }
    }

    /// Short human name of the ledger, for `… is empty: nothing to anchor`
    /// and `relabel` messages.
    pub const fn ledger_short(self) -> &'static str {
        match self {
            AnchorSubject::ChainHead => "chain",
            AnchorSubject::Digest => "artifact digest",
            AnchorSubject::ExportReceiptHead => "export-receipt chain",
            AnchorSubject::BreakGlassReceiptHead => "break-glass receipt chain",
            AnchorSubject::PolicyHead => "policy-change history",
        }
    }

    /// The membership clause of an `OK`/`UNVERIFIED` verify line.
    pub const fn membership_label(self) -> &'static str {
        match self {
            AnchorSubject::ChainHead => "in chain history",
            AnchorSubject::Digest => "digest anchor, chain membership not applicable",
            AnchorSubject::ExportReceiptHead => "in export-receipt chain history",
            AnchorSubject::BreakGlassReceiptHead => "in break-glass receipt chain history",
            AnchorSubject::PolicyHead => "in policy-change history",
        }
    }

    /// The `FAIL` problem line when a head kind's hash is missing from its
    /// declared ledger. Never used for `Digest`.
    pub const fn not_in_history_label(self) -> &'static str {
        match self {
            AnchorSubject::ChainHead => "anchored hash is not in chain history",
            AnchorSubject::Digest => "digest anchor, chain membership not applicable",
            AnchorSubject::ExportReceiptHead => {
                "anchored hash is not in export-receipt chain history"
            }
            AnchorSubject::BreakGlassReceiptHead => {
                "anchored hash is not in break-glass receipt chain history"
            }
            AnchorSubject::PolicyHead => "anchored hash is not in policy-change history",
        }
    }

    /// SHA-256("securacv:anchor:empty-ledger:v1:" ‖ as_str()); head kinds only
    /// (`None` for `Digest`). What `anchor-all` requests when this ledger is
    /// empty, so the per-run request count is constant (Invariant III).
    pub fn empty_ledger_sentinel(self) -> Option<[u8; 32]> {
        use sha2::{Digest, Sha256};
        if !self.is_head() {
            return None;
        }
        let mut h = Sha256::new();
        h.update(EMPTY_LEDGER_SENTINEL_PREFIX);
        h.update(self.as_str().as_bytes());
        Some(h.finalize().into())
    }

    /// The head kind whose sentinel this hash is, if any — for `list`,
    /// `verify` and `import` annotations only.
    pub fn sentinel_owner(hash: &[u8; 32]) -> Option<AnchorSubject> {
        Self::HEADS
            .into_iter()
            .find(|k| k.empty_ledger_sentinel().as_ref() == Some(hash))
    }
}

impl std::fmt::Display for AnchorSubject {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// How a hash was found in a ledger's history.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HistoryWitness {
    /// A live ledger row carries the hash.
    LiveRow { id: i64 },
    /// The hash survives only as a retention checkpoint's chain head.
    CheckpointHead { checkpoint_id: i64 },
}

/// Which ledger a hash belongs to, and how it was found there.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HashLocation {
    pub subject: AnchorSubject,
    pub witness: HistoryWitness,
}

/// The TSA identity embedded in a token, read best-effort from the CMS
/// `SignedData` (certificates and the single `SignerInfo.sid`). Nothing here
/// is validated — `openssl ts -verify` does that; these are the facts a
/// verifier compares against, and what `list` prints for an offline import.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TsaSigner {
    /// sha256(issuer Name TLV ‖ serial INTEGER contents) for
    /// `issuerAndSerialNumber`; sha256(SKI OCTET STRING contents) for the
    /// `[0]` form.
    pub sid_hex: String,
    /// SHA-256 of the issuer `Name` TLV; `None` for the SKI form.
    pub issuer_sha256: Option<[u8; 32]>,
    /// The SIGNER CERTIFICATE serial, hex (`TimestampToken.serial_hex` stays
    /// the TSTInfo token serial).
    pub serial_hex: Option<String>,
    /// SHA-256 over the matching embedded certificate's DER, exactly as
    /// embedded; `None` when no embedded certificate matches the sid.
    pub cert_sha256: Option<[u8; 32]>,
    /// First `2.5.4.3` (commonName) of the matching certificate's subject;
    /// printable ASCII only, control characters become `?`, at most 64
    /// characters. Display only.
    pub cert_subject_cn: Option<String>,
}

/// id-at-commonName (2.5.4.3), body only.
const OID_COMMON_NAME: &[u8] = &[0x55, 0x04, 0x03];

/// The `SignedData` walk shared by `parse_token` and `parse_token_signer`:
/// returns the reader positioned after `encapContentInfo`, plus that
/// element's content. Factored so the two readers cannot diverge.
fn signed_data_reader(token_der: &[u8]) -> Result<(Der<'_>, &[u8])> {
    let content_info = Der::new(token_der).expect(0x30, "ContentInfo")?;

    // ContentInfo ::= SEQUENCE { contentType OID, [0] EXPLICIT content }
    let mut ci = Der::new(content_info);
    let content_type = ci.expect(0x06, "contentType")?;
    if content_type != OID_SIGNED_DATA {
        bail!("TimeStampToken is not CMS SignedData");
    }
    let signed_data_wrap = ci.expect(0xa0, "content [0]")?;
    let signed_data = Der::new(signed_data_wrap).expect(0x30, "SignedData")?;

    // SignedData ::= SEQUENCE { version, digestAlgorithms SET,
    // encapContentInfo SEQUENCE { eContentType OID, [0] { OCTET STRING } }, … }
    let mut sd = Der::new(signed_data);
    sd.expect(0x02, "SignedData.version")?;
    sd.expect(0x31, "digestAlgorithms")?;
    let encap = sd.expect(0x30, "encapContentInfo")?;
    Ok((sd, encap))
}

/// The fields of one embedded X.509 certificate that identity matching needs.
struct EmbeddedCert<'a> {
    raw: &'a [u8],
    serial: &'a [u8],
    issuer_raw: &'a [u8],
    subject_raw: &'a [u8],
}

fn parse_embedded_cert(raw: &[u8]) -> Result<EmbeddedCert<'_>> {
    // Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
    let cert = Der::new(raw).expect(0x30, "Certificate")?;
    let tbs = Der::new(cert).expect(0x30, "TBSCertificate")?;
    // TBSCertificate ::= SEQUENCE { [0] version OPTIONAL, serialNumber,
    // signature AlgorithmIdentifier, issuer Name, validity, subject Name, … }
    let mut t = Der::new(tbs);
    if t.peek_tag() == Some(0xa0) {
        t.tlv()?;
    }
    let serial = t.expect(0x02, "serialNumber")?;
    t.expect(0x30, "signature AlgorithmIdentifier")?;
    let (tag, _, issuer_raw) = t.tlv()?;
    if tag != 0x30 {
        bail!("DER: expected issuer Name (tag 0x30), got 0x{tag:02x}");
    }
    t.expect(0x30, "validity")?;
    let (tag, _, subject_raw) = t.tlv()?;
    if tag != 0x30 {
        bail!("DER: expected subject Name (tag 0x30), got 0x{tag:02x}");
    }
    Ok(EmbeddedCert {
        raw,
        serial,
        issuer_raw,
        subject_raw,
    })
}

/// First commonName of a DER `Name`, sanitized for display.
fn name_common_name(name_raw: &[u8]) -> Result<Option<String>> {
    // Name ::= SEQUENCE OF RelativeDistinguishedName (SET OF
    // AttributeTypeAndValue ::= SEQUENCE { type OID, value ANY })
    let rdns = Der::new(name_raw).expect(0x30, "Name")?;
    let mut r = Der::new(rdns);
    while r.has_more() {
        let set = r.expect(0x31, "RelativeDistinguishedName")?;
        let mut s = Der::new(set);
        while s.has_more() {
            let atv = s.expect(0x30, "AttributeTypeAndValue")?;
            let mut a = Der::new(atv);
            let oid = a.expect(0x06, "attribute type")?;
            let (vtag, value, _) = a.tlv()?;
            if oid != OID_COMMON_NAME {
                continue;
            }
            // UTF8String / PrintableString / IA5String; anything else is
            // not rendered rather than guessed at.
            if !matches!(vtag, 0x0c | 0x13 | 0x16) {
                return Ok(None);
            }
            let text: String = String::from_utf8_lossy(value)
                .chars()
                .map(|c| {
                    if c.is_ascii_graphic() || c == ' ' {
                        c
                    } else {
                        '?'
                    }
                })
                .take(64)
                .collect();
            return Ok(Some(text));
        }
    }
    Ok(None)
}

/// Read the TSA's identity from a bare DER `TimeStampToken`: the single
/// `SignerInfo.sid` and, when the token embeds it, the matching signing
/// certificate. Best-effort and separate from `parse_token` on purpose — a
/// real-TSA token with an odd CMS layout must never become un-importable, so
/// a failure here yields `Err` for the caller to record as "identity not
/// readable" while the token itself still parses and stores.
pub fn parse_token_signer(token_der: &[u8]) -> Result<TsaSigner> {
    use sha2::{Digest, Sha256};

    let (mut sd, _encap) = signed_data_reader(token_der)?;

    // certificates [0] IMPLICIT CertificateSet OPTIONAL — collect every
    // element that is a Certificate SEQUENCE (other choices are skipped).
    let mut certs: Vec<&[u8]> = Vec::new();
    if sd.peek_tag() == Some(0xa0) {
        let (_, set, _) = sd.tlv()?;
        let mut cs = Der::new(set);
        while cs.has_more() {
            let (tag, _, raw) = cs.tlv()?;
            if tag == 0x30 {
                certs.push(raw);
            }
        }
    }
    // crls [1] IMPLICIT RevocationInfoChoices OPTIONAL — skipped.
    if sd.peek_tag() == Some(0xa1) {
        sd.tlv()?;
    }
    let signer_infos = sd.expect(0x31, "signerInfos")?;
    let mut sis = Der::new(signer_infos);
    let mut infos: Vec<&[u8]> = Vec::new();
    while sis.has_more() {
        let (tag, content, _) = sis.tlv()?;
        if tag != 0x30 {
            bail!("DER: expected SignerInfo (tag 0x30), got 0x{tag:02x}");
        }
        infos.push(content);
    }
    let info = match infos.len() {
        0 => bail!("TimeStampToken carries no SignerInfo"),
        1 => infos[0],
        _ => bail!(
            "TimeStampToken carries more than one SignerInfo (RFC 3161 §2.4.2 allows exactly one)"
        ),
    };

    // SignerInfo ::= SEQUENCE { version, sid SignerIdentifier, … }
    // SignerIdentifier ::= CHOICE { issuerAndSerialNumber SEQUENCE,
    //                               subjectKeyIdentifier [0] }
    let mut si = Der::new(info);
    si.expect(0x02, "SignerInfo.version")?;
    let (sid_tag, sid_content, _) = si.tlv()?;
    enum Sid<'a> {
        IssuerSerial {
            issuer_raw: &'a [u8],
            serial: &'a [u8],
        },
        Ski(&'a [u8]),
    }
    let sid = match sid_tag {
        0x30 => {
            let mut r = Der::new(sid_content);
            let (tag, _, issuer_raw) = r.tlv()?;
            if tag != 0x30 {
                bail!("DER: expected issuer Name (tag 0x30), got 0x{tag:02x}");
            }
            let serial = r.expect(0x02, "serialNumber")?;
            Sid::IssuerSerial { issuer_raw, serial }
        }
        0x80 => Sid::Ski(sid_content),
        _ => bail!("SignerInfo.sid is neither issuerAndSerialNumber nor subjectKeyIdentifier"),
    };

    let parsed_certs = certs
        .iter()
        .map(|raw| parse_embedded_cert(raw))
        .collect::<Result<Vec<_>>>()?;

    let (sid_hex, issuer_sha256, serial_hex, matched) = match sid {
        Sid::IssuerSerial { issuer_raw, serial } => {
            let mut h = Sha256::new();
            h.update(issuer_raw);
            h.update(serial);
            let sid_hex = hex::encode(h.finalize());
            let issuer_sha256: [u8; 32] = Sha256::digest(issuer_raw).into();
            let matched = parsed_certs.iter().find(|c| {
                c.issuer_raw == issuer_raw && uint_normalize(c.serial) == uint_normalize(serial)
            });
            (
                sid_hex,
                Some(issuer_sha256),
                Some(hex::encode(uint_normalize(serial))),
                matched,
            )
        }
        Sid::Ski(ski) => {
            let sid_hex = hex::encode(Sha256::digest(ski));
            // Documented assumption: the SKI form is matched only when the
            // token embeds exactly one certificate.
            let matched = if parsed_certs.len() == 1 {
                parsed_certs.first()
            } else {
                None
            };
            let serial_hex = matched.map(|c| hex::encode(uint_normalize(c.serial)));
            (sid_hex, None, serial_hex, matched)
        }
    };

    let (cert_sha256, cert_subject_cn) = match matched {
        Some(c) => (
            Some(Sha256::digest(c.raw).into()),
            name_common_name(c.subject_raw)?,
        ),
        None => (None, None),
    };

    Ok(TsaSigner {
        sid_hex,
        issuer_sha256,
        serial_hex,
        cert_sha256,
        cert_subject_cn,
    })
}

// -------------------- Anchor persistence --------------------

use crate::TimeBucket;
use rusqlite::{Connection, OptionalExtension};

/// A stored anchor: an RFC 3161 token over a hash from the witness chain.
#[derive(Debug, Clone)]
pub struct AnchorRecord {
    pub id: i64,
    pub created_bucket: TimeBucket,
    /// What was anchored: an `AnchorSubject` literal (`chain_head`, `digest`,
    /// `export_receipt_head`, `break_glass_receipt_head`, `policy_head`), or
    /// unknown text from an older writer (treated as a digest anchor).
    pub subject: String,
    pub subject_hash: [u8; 32],
    pub tsa_url: String,
    pub gen_time: String,
    pub token_der: Vec<u8>,
    /// The anchor-policy entry name declared at insert time; `None` when the
    /// row was anchored without a policy.
    pub tsa_name: Option<String>,
    /// Cached SHA-256 (64 lowercase hex) of the token's embedded signer
    /// certificate; verifiers re-derive it from the token and a mismatch is
    /// `FAIL`.
    pub signer_cert_sha256: Option<String>,
    /// Cached `TsaSigner::sid_hex`; re-derived by verifiers likewise.
    pub signer_sid: Option<String>,
    /// Ledger row id of the anchored head AT INSERT TIME; `None` for digest
    /// rows, legacy rows, and any head already pruned when the row was
    /// written. Unsigned: it only sharpens diagnosis text and guards
    /// `relabel`, never decides pass/fail.
    pub ledger_id: Option<i64>,
}

impl AnchorRecord {
    /// The row's subject as a known kind, or `None` for unknown text.
    pub fn subject_kind(&self) -> Option<AnchorSubject> {
        AnchorSubject::parse(&self.subject)
    }

    /// The row's subject with unknown text falling through to `Digest`.
    pub fn subject_kind_or_digest(&self) -> AnchorSubject {
        AnchorSubject::from_row(&self.subject)
    }
}

/// The nullable columns added after the frozen `CREATE TABLE` (A.2.1).
const ANCHOR_EXTRA_COLUMNS: [(&str, &str); 4] = [
    ("tsa_name", "TEXT"),
    ("signer_cert_sha256", "TEXT"),
    ("signer_sid", "TEXT"),
    ("ledger_id", "INTEGER"),
];

/// Create the anchors table if missing. Additive and independent of the
/// sealed-log schema: anchors reference chain hashes but never alter them.
/// The `CREATE TABLE` text is frozen; newer columns are added with
/// `ALTER TABLE … ADD COLUMN` so a table an older build wrote migrates in
/// place. Only writers call this — read-only verbs never migrate.
pub fn ensure_anchor_table(conn: &Connection) -> Result<()> {
    conn.execute_batch(
        r#"
        CREATE TABLE IF NOT EXISTS tsa_anchors (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          created_bucket_start INTEGER NOT NULL,
          created_bucket_size INTEGER NOT NULL,
          subject TEXT NOT NULL,
          subject_hash BLOB NOT NULL,
          tsa_url TEXT NOT NULL,
          gen_time TEXT NOT NULL,
          token_der BLOB NOT NULL
        );
        "#,
    )?;
    crate::storage::ensure_columns(conn, "tsa_anchors", &ANCHOR_EXTRA_COLUMNS)?;
    Ok(())
}

pub fn insert_anchor(
    conn: &Connection,
    subject: &str,
    subject_hash: &[u8; 32],
    tsa_url: &str,
    token: &TimestampToken,
) -> Result<i64> {
    insert_anchor_row(conn, subject, subject_hash, tsa_url, None, token)
}

/// `insert_anchor` with a typed subject and the anchor-policy entry name the
/// operator declared for this TSA (recorded, never trusted).
pub fn insert_anchor_declared(
    conn: &Connection,
    subject: AnchorSubject,
    subject_hash: &[u8; 32],
    tsa_url: &str,
    tsa_name: Option<&str>,
    token: &TimestampToken,
) -> Result<i64> {
    insert_anchor_row(
        conn,
        subject.as_str(),
        subject_hash,
        tsa_url,
        tsa_name,
        token,
    )
}

fn insert_anchor_row(
    conn: &Connection,
    subject: &str,
    subject_hash: &[u8; 32],
    tsa_url: &str,
    tsa_name: Option<&str>,
    token: &TimestampToken,
) -> Result<i64> {
    let bucket = TimeBucket::now_10min()?;
    // Identity is read from the token, never from `tsa_url`; a signer that
    // cannot be parsed leaves the cache columns NULL (the caller may warn).
    let signer = parse_token_signer(&token.token_der).ok();
    let signer_cert_sha256 = signer.as_ref().and_then(|s| s.cert_sha256.map(hex::encode));
    let signer_sid = signer.as_ref().map(|s| s.sid_hex.clone());
    let ledger_id = ledger_position(conn, AnchorSubject::from_row(subject), subject_hash)?;
    conn.execute(
        "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
            subject_hash, tsa_url, gen_time, token_der, tsa_name, signer_cert_sha256,
            signer_sid, ledger_id) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
        rusqlite::params![
            bucket.start_epoch_s as i64,
            bucket.size_s as i64,
            subject,
            subject_hash.as_slice(),
            tsa_url,
            token.gen_time,
            token.token_der,
            tsa_name,
            signer_cert_sha256,
            signer_sid,
            ledger_id,
        ],
    )?;
    Ok(conn.last_insert_rowid())
}

/// The anchor SELECT, with `NULL AS <col>` for every newer column the table
/// does not have yet — so a READ_ONLY opener (`court_export`, an observer's
/// `list`) reads a table an older build wrote without migrating it.
fn anchor_select_sql(conn: &Connection) -> Result<String> {
    let mut stmt = conn.prepare("PRAGMA table_info(tsa_anchors)")?;
    let present: std::collections::HashSet<String> = stmt
        .query_map([], |row| row.get::<_, String>(1))?
        .collect::<std::result::Result<_, _>>()?;
    let mut cols = vec![
        "id",
        "created_bucket_start",
        "created_bucket_size",
        "subject",
        "subject_hash",
        "tsa_url",
        "gen_time",
        "token_der",
    ]
    .into_iter()
    .map(str::to_string)
    .collect::<Vec<_>>();
    for (name, _) in ANCHOR_EXTRA_COLUMNS {
        if present.contains(name) {
            cols.push(name.to_string());
        } else {
            cols.push(format!("NULL AS {name}"));
        }
    }
    Ok(format!(
        "SELECT {} FROM tsa_anchors ORDER BY id",
        cols.join(", ")
    ))
}

type RawAnchorRow = (
    i64,
    Option<i64>,
    Option<i64>,
    Option<String>,
    Option<Vec<u8>>,
    Option<String>,
    Option<String>,
    Option<Vec<u8>>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<i64>,
);

fn decode_anchor_row(raw: RawAnchorRow) -> std::result::Result<AnchorRecord, String> {
    let (
        id,
        start,
        size,
        subject,
        hash,
        tsa_url,
        gen_time,
        token_der,
        tsa_name,
        signer_cert_sha256,
        signer_sid,
        ledger_id,
    ) = raw;
    let start = start.ok_or("created_bucket_start is NULL")?;
    let size = size.ok_or("created_bucket_size is NULL")?;
    let subject = subject.ok_or("subject is NULL")?;
    let hash = hash.ok_or("subject_hash is NULL")?;
    let tsa_url = tsa_url.ok_or("tsa_url is NULL")?;
    let gen_time = gen_time.ok_or("gen_time is NULL")?;
    let token_der = token_der.ok_or("token_der is NULL")?;
    let subject_hash: [u8; 32] = hash.try_into().map_err(|_| "subject_hash size")?;
    Ok(AnchorRecord {
        id,
        created_bucket: TimeBucket {
            start_epoch_s: start as u64,
            size_s: size as u32,
        },
        subject,
        subject_hash,
        tsa_url,
        gen_time,
        token_der,
        tsa_name,
        signer_cert_sha256,
        signer_sid,
        ledger_id,
    })
}

/// Every anchor row in id order, with a malformed row surfaced as
/// `(id, Err(reason))` instead of aborting the read. A verifier folding
/// anchors into a verdict uses this so one corrupt auxiliary row cannot turn
/// into an undiagnosed failure.
pub fn list_anchors_lenient(conn: &Connection) -> Result<Vec<(i64, Result<AnchorRecord>)>> {
    let sql = anchor_select_sql(conn)?;
    let mut stmt = conn.prepare(&sql)?;
    let rows = stmt.query_map([], |row| {
        Ok((
            row.get::<_, i64>(0)?,
            row.get::<_, Option<i64>>(1)?,
            row.get::<_, Option<i64>>(2)?,
            row.get::<_, Option<String>>(3)?,
            row.get::<_, Option<Vec<u8>>>(4)?,
            row.get::<_, Option<String>>(5)?,
            row.get::<_, Option<String>>(6)?,
            row.get::<_, Option<Vec<u8>>>(7)?,
            row.get::<_, Option<String>>(8)?,
            row.get::<_, Option<String>>(9)?,
            row.get::<_, Option<String>>(10)?,
            row.get::<_, Option<i64>>(11)?,
        ))
    })?;
    let mut out = Vec::new();
    for row in rows {
        let raw = row?;
        let id = raw.0;
        out.push((
            id,
            decode_anchor_row(raw).map_err(|reason| anyhow!("corrupt anchor {id}: {reason}")),
        ));
    }
    Ok(out)
}

/// Every anchor row in id order. A malformed row (a `subject_hash` that is
/// not 32 bytes) is a hard error — `log_anchor` and `court_export` want to
/// stop there, not paper over it.
pub fn list_anchors(conn: &Connection) -> Result<Vec<AnchorRecord>> {
    list_anchors_lenient(conn)?
        .into_iter()
        .map(|(_, row)| row)
        .collect()
}

fn table_exists(conn: &Connection, name: &str) -> Result<bool> {
    Ok(conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1",
            [name],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false))
}

/// Whether the database has a `tsa_anchors` table at all. A read-only opener
/// treats its absence as "no anchors stored"; only writers create it.
pub fn anchor_table_exists(conn: &Connection) -> Result<bool> {
    table_exists(conn, "tsa_anchors")
}

fn read32(conn: &Connection, sql: &str) -> Result<Option<[u8; 32]>> {
    let mut stmt = conn.prepare(sql)?;
    let mut rows = stmt.query([])?;
    match rows.next()? {
        Some(row) => {
            let bytes: Vec<u8> = row.get(0)?;
            Ok(Some(bytes.try_into().map_err(|_| {
                anyhow!("corrupt chain: hash column is not 32 bytes")
            })?))
        }
        None => Ok(None),
    }
}

fn chain_head_opt(conn: &Connection) -> Result<Option<[u8; 32]>> {
    if let Some(head) = read32(
        conn,
        "SELECT entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1",
    )? {
        return Ok(Some(head));
    }
    read32(
        conn,
        "SELECT chain_head_hash FROM checkpoints ORDER BY id DESC LIMIT 1",
    )
}

/// Current chain head: the newest sealed event's entry hash, else the last
/// retention checkpoint's head. Errors on an empty log — there is nothing
/// meaningful to anchor.
pub fn chain_head(conn: &Connection) -> Result<[u8; 32]> {
    match chain_head_opt(conn)? {
        Some(head) => Ok(head),
        None => bail!("sealed log is empty: nothing to anchor"),
    }
}

/// Whether a hash is part of recorded chain history (a sealed event's entry
/// hash or a checkpoint head). Used when verifying anchors after the chain
/// has moved on.
pub fn hash_in_history(conn: &Connection, hash: &[u8; 32]) -> Result<bool> {
    let mut stmt = conn.prepare(
        "SELECT EXISTS(SELECT 1 FROM sealed_events WHERE entry_hash = ?1)
             OR EXISTS(SELECT 1 FROM checkpoints WHERE chain_head_hash = ?1)",
    )?;
    Ok(stmt.query_row([hash.as_slice()], |row| row.get::<_, bool>(0))?)
}

fn row_id_by_hash(
    conn: &Connection,
    table: &str,
    column: &str,
    hash: &[u8; 32],
) -> Result<Option<i64>> {
    if !table_exists(conn, table)? {
        return Ok(None);
    }
    Ok(conn
        .query_row(
            &format!("SELECT id FROM {table} WHERE {column} = ?1 ORDER BY id ASC LIMIT 1"),
            [hash.as_slice()],
            |row| row.get::<_, i64>(0),
        )
        .optional()?)
}

/// Whether `hash` is in the history of the ledger `subject` DECLARES — a
/// live row, or (chain head only) a retention checkpoint's head. Membership
/// is checked in the declared ledger only; `Digest` is always `None`. A
/// missing ledger table reads as "not present".
pub fn hash_in_ledger(
    conn: &Connection,
    hash: &[u8; 32],
    subject: AnchorSubject,
) -> Result<Option<HistoryWitness>> {
    match subject {
        AnchorSubject::Digest => Ok(None),
        AnchorSubject::ChainHead => {
            if let Some(id) = row_id_by_hash(conn, "sealed_events", "entry_hash", hash)? {
                return Ok(Some(HistoryWitness::LiveRow { id }));
            }
            Ok(
                row_id_by_hash(conn, "checkpoints", "chain_head_hash", hash)?
                    .map(|checkpoint_id| HistoryWitness::CheckpointHead { checkpoint_id }),
            )
        }
        other => {
            let table = other
                .table()
                .ok_or_else(|| anyhow!("subject {other} has no ledger table"))?;
            Ok(row_id_by_hash(conn, table, "entry_hash", hash)?
                .map(|id| HistoryWitness::LiveRow { id }))
        }
    }
}

/// Which ledger a hash belongs to (chain head first, then the export-receipt,
/// break-glass receipt and policy-change ledgers), tolerating missing
/// tables. Sentinel hashes are never classified as heads: they are in no
/// ledger by construction.
pub fn classify_hash(conn: &Connection, hash: &[u8; 32]) -> Result<Option<HashLocation>> {
    for subject in AnchorSubject::HEADS {
        if let Some(witness) = hash_in_ledger(conn, hash, subject)? {
            return Ok(Some(HashLocation { subject, witness }));
        }
    }
    Ok(None)
}

/// The current head of a ledger, or `None` when it is empty (or its table
/// does not exist). `Digest` has no ledger and is an error.
pub fn ledger_head(conn: &Connection, subject: AnchorSubject) -> Result<Option<[u8; 32]>> {
    match subject {
        AnchorSubject::Digest => bail!("a digest subject needs --digest or --file"),
        AnchorSubject::ChainHead => chain_head_opt(conn),
        other => {
            let table = other
                .table()
                .ok_or_else(|| anyhow!("subject {other} has no ledger table"))?;
            if !table_exists(conn, table)? {
                return Ok(None);
            }
            read32(
                conn,
                &format!("SELECT entry_hash FROM {table} ORDER BY id DESC LIMIT 1"),
            )
        }
    }
}

/// The ledger row id carrying `hash` (chain head: `sealed_events.id`), or
/// `None` when the head survives only as a checkpoint, is gone, or the
/// subject is a digest.
pub fn ledger_position(
    conn: &Connection,
    subject: AnchorSubject,
    hash: &[u8; 32],
) -> Result<Option<i64>> {
    match hash_in_ledger(conn, hash, subject)? {
        Some(HistoryWitness::LiveRow { id }) => Ok(Some(id)),
        _ => Ok(None),
    }
}

// -------------------- HTTP submission (feature-gated) --------------------

/// POST a timestamp query to a TSA (RFC 3161 §3.4). HTTPS-only unless the
/// caller explicitly opts into plaintext (some public TSAs are http-only;
/// the token's own signature makes transport integrity non-critical, but
/// the default stays strict).
#[cfg(feature = "tsa")]
pub fn fetch_timestamp(url: &str, query_der: &[u8], allow_http: bool) -> Result<Vec<u8>> {
    let scheme_ok = url.starts_with("https://") || (allow_http && url.starts_with("http://"));
    if !scheme_ok {
        bail!("TSA URL must be https:// (pass --allow-http to override)");
    }
    // Bounded timeout so a hung TSA fails the run instead of accumulating
    // stuck processes under cron.
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(std::time::Duration::from_secs(30)))
        .build()
        .into();
    let mut response = agent
        .post(url)
        .header("Content-Type", "application/timestamp-query")
        .send(query_der)
        .map_err(|e| anyhow!("TSA request to {url} failed: {e}"))?;
    let body = response
        .body_mut()
        .with_config()
        .limit(256 * 1024)
        .read_to_vec()
        .map_err(|e| anyhow!("reading TSA response failed: {e}"))?;
    Ok(body)
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};

    fn fixture_digest() -> [u8; 32] {
        Sha256::digest(b"securacv-fixture").into()
    }

    #[test]
    fn request_matches_openssl_golden() {
        // openssl ts -query -digest <sha256("securacv-fixture")> -sha256 -cert -no_nonce
        let golden = "30390201013031300d060960864801650304020105000420\
                      89e4120edf60957bde82c27f35ef282ba6608dfdc29b454534ae136da1cd9dd2\
                      0101ff";
        let built = build_request(&fixture_digest(), None, true);
        assert_eq!(hex::encode(built), golden.replace(char::is_whitespace, ""));
    }

    #[test]
    fn request_nonce_encoding_matches_openssl() {
        // openssl encodes the high-bit-set 8-byte nonce with a 0x00 pad.
        let built = build_request(
            &fixture_digest(),
            Some(&hex::decode("ac06d335ca6b0758").unwrap()),
            true,
        );
        let hex = hex::encode(built);
        assert!(hex.contains("020900ac06d335ca6b0758"), "got {hex}");
    }

    #[test]
    fn der_uint_edge_cases() {
        assert_eq!(der_uint(&[0x00, 0x00]), vec![0x02, 0x01, 0x00]); // zero stays one octet
        assert_eq!(der_uint(&[0x7f]), vec![0x02, 0x01, 0x7f]); // no pad below 0x80
        assert_eq!(der_uint(&[0x80]), vec![0x02, 0x02, 0x00, 0x80]); // pad at high bit
        assert_eq!(der_uint(&[0x00, 0x01]), vec![0x02, 0x01, 0x01]); // strip leading zero
    }

    #[test]
    fn oid_decoding() {
        assert_eq!(oid_to_dotted(&OID_SHA256[2..]), "2.16.840.1.101.3.4.2.1");
        assert_eq!(oid_to_dotted(OID_TST_INFO), "1.2.840.113549.1.9.16.1.4");
    }

    #[test]
    fn gen_time_conversion() {
        // Cross-checked with `date -u -d "2026-06-10 12:33:24" +%s`.
        assert_eq!(gen_time_unix("20260610123324Z"), Some(1781094804));
        assert_eq!(gen_time_unix("19700101000000Z"), Some(0));
        assert_eq!(gen_time_unix("20000301000000Z"), Some(951868800)); // leap-century day
                                                                       // Fractional seconds are truncated, not rejected (RFC 3161 allows them).
        assert_eq!(gen_time_unix("20260610123324.5Z"), Some(1781094804));
        assert_eq!(gen_time_unix("garbage"), None);
        assert_eq!(gen_time_unix("20261310123324Z"), None); // month 13
    }

    fn fixture_token_der() -> Vec<u8> {
        let path = format!(
            "{}/tests/fixtures/tsa/reply.tsr",
            env!("CARGO_MANIFEST_DIR")
        );
        parse_response(&std::fs::read(&path).expect("reading TSA fixture"))
            .expect("fixture token parses")
            .token_der
    }

    /// Re-assemble the fixture token with its `signerInfos` SET replaced:
    /// `Some(list)` writes those SignerInfo TLVs, `None` ends `SignedData`
    /// right after `encapContentInfo` (no certificates, no signerInfos).
    /// Lengths are recomputed by `der_tlv`, so the result is well-formed DER.
    fn rebuild_token(token_der: &[u8], signer_infos: Option<Vec<Vec<u8>>>) -> Vec<u8> {
        let content_info = Der::new(token_der).expect(0x30, "ContentInfo").unwrap();
        let mut ci = Der::new(content_info);
        let (_, oid, _) = ci.tlv().unwrap();
        let wrap = ci.expect(0xa0, "content [0]").unwrap();
        let signed_data = Der::new(wrap).expect(0x30, "SignedData").unwrap();
        let mut sd = Der::new(signed_data);
        let (_, _, version) = sd.tlv().unwrap();
        let (_, _, digest_algs) = sd.tlv().unwrap();
        let (_, _, encap) = sd.tlv().unwrap();
        let mut body = Vec::new();
        body.extend_from_slice(version);
        body.extend_from_slice(digest_algs);
        body.extend_from_slice(encap);
        if let Some(infos) = signer_infos {
            // Keep certificates/crls as they are; swap the SET.
            while let Some(tag) = sd.peek_tag() {
                let (_, _, raw) = sd.tlv().unwrap();
                if tag == 0x31 {
                    break;
                }
                body.extend_from_slice(raw);
            }
            let set: Vec<u8> = infos.concat();
            body.extend(der_tlv(0x31, &set));
        }
        let sd_tlv = der_tlv(0x30, &body);
        let mut ci_body = der_tlv(0x06, oid);
        ci_body.extend(der_tlv(0xa0, &sd_tlv));
        der_tlv(0x30, &ci_body)
    }

    fn fixture_signer_info() -> Vec<u8> {
        let der = fixture_token_der();
        let (mut sd, _) = signed_data_reader(&der).unwrap();
        loop {
            let (tag, content, _) = sd.tlv().unwrap();
            if tag == 0x31 {
                let (_, _, raw) = Der::new(content).tlv().unwrap();
                return raw.to_vec();
            }
        }
    }

    #[test]
    fn anchor_subject_literals_round_trip() {
        for kind in [
            AnchorSubject::ChainHead,
            AnchorSubject::Digest,
            AnchorSubject::ExportReceiptHead,
            AnchorSubject::BreakGlassReceiptHead,
            AnchorSubject::PolicyHead,
        ] {
            assert_eq!(AnchorSubject::parse(kind.as_str()), Some(kind));
            assert_eq!(kind.to_string(), kind.as_str());
            assert_eq!(AnchorSubject::from_row(kind.as_str()), kind);
        }
        assert_eq!(AnchorSubject::ChainHead.as_str(), "chain_head");
        assert_eq!(AnchorSubject::Digest.as_str(), "digest");
        assert_eq!(
            AnchorSubject::ExportReceiptHead.as_str(),
            "export_receipt_head"
        );
        assert_eq!(
            AnchorSubject::BreakGlassReceiptHead.as_str(),
            "break_glass_receipt_head"
        );
        assert_eq!(AnchorSubject::PolicyHead.as_str(), "policy_head");
        for bad in ["Chain_Head", "chain-head", "", "digest "] {
            assert_eq!(AnchorSubject::parse(bad), None, "{bad:?}");
            assert_eq!(AnchorSubject::from_row(bad), AnchorSubject::Digest);
        }
        assert!(AnchorSubject::HEADS.iter().all(|k| k.is_head()));
        assert!(!AnchorSubject::Digest.is_head());
        assert_eq!(AnchorSubject::ChainHead.table(), None);
        assert_eq!(
            AnchorSubject::ExportReceiptHead.table(),
            Some("export_receipts")
        );
        assert_eq!(
            AnchorSubject::BreakGlassReceiptHead.table(),
            Some("break_glass_receipts")
        );
        assert_eq!(
            AnchorSubject::PolicyHead.table(),
            Some("policy_change_history")
        );
        // The runbook quotes these two; they must stay byte-identical.
        assert_eq!(
            AnchorSubject::ChainHead.membership_label(),
            "in chain history"
        );
        assert_eq!(
            AnchorSubject::ChainHead.not_in_history_label(),
            "anchored hash is not in chain history"
        );
        assert_eq!(
            AnchorSubject::Digest.membership_label(),
            "digest anchor, chain membership not applicable"
        );
        assert_eq!(
            AnchorSubject::ExportReceiptHead.not_in_history_label(),
            "anchored hash is not in export-receipt chain history"
        );
    }

    #[test]
    fn empty_ledger_sentinels_are_distinct_and_recognized() {
        let sentinels: Vec<[u8; 32]> = AnchorSubject::HEADS
            .iter()
            .map(|k| {
                k.empty_ledger_sentinel()
                    .expect("head kinds have a sentinel")
            })
            .collect();
        for (i, a) in sentinels.iter().enumerate() {
            for (j, b) in sentinels.iter().enumerate() {
                assert_eq!(a == b, i == j, "sentinels must be distinct per subject");
            }
        }
        for kind in AnchorSubject::HEADS {
            let h = kind.empty_ledger_sentinel().unwrap();
            assert_eq!(AnchorSubject::sentinel_owner(&h), Some(kind));
            // Pinned derivation: SHA-256 of the documented prefix ‖ literal.
            let expect: [u8; 32] = Sha256::digest(
                [
                    b"securacv:anchor:empty-ledger:v1:".as_slice(),
                    kind.as_str().as_bytes(),
                ]
                .concat(),
            )
            .into();
            assert_eq!(h, expect);
        }
        assert_eq!(AnchorSubject::Digest.empty_ledger_sentinel(), None);
        assert_eq!(AnchorSubject::sentinel_owner(&fixture_digest()), None);
        assert_eq!(AnchorSubject::sentinel_owner(&[0u8; 32]), None);
    }

    #[test]
    fn fixture_token_signer_identity() {
        let der = fixture_token_der();
        assert_eq!(der.len(), 947);
        let signer = parse_token_signer(&der).unwrap();
        assert_eq!(
            signer.sid_hex,
            "d806acd919f81e613c74e1b025e3aab8d47502181d30a00bf761bf67aa7ce3ef"
        );
        assert_eq!(
            hex::encode(signer.issuer_sha256.unwrap()),
            "a1e69de782f4ad7844c668f732a481820710a9a2e66dec6423230f922384e644"
        );
        assert_eq!(
            signer.serial_hex.as_deref(),
            Some("45c7728db49cb1c57be1b8f3fe5970522be1f00c")
        );
        assert_eq!(
            hex::encode(signer.cert_sha256.unwrap()),
            "fbf1c838f80923a01badb6030b9d708c5ef1a65b7d23f1f53a6aa274d1b99542"
        );
        assert_eq!(signer.cert_subject_cn.as_deref(), Some("SecuraCV Test TSA"));
        // The token's own serial is untouched: TSTInfo serial, not the cert's.
        assert_eq!(parse_token(&der).unwrap().serial_hex, "02");
        // The embedded certificate is byte-equal to the committed tsa.crt.
        let pem = std::fs::read_to_string(format!(
            "{}/tests/fixtures/tsa/tsa.crt",
            env!("CARGO_MANIFEST_DIR")
        ))
        .unwrap();
        let b64: String = pem.lines().filter(|l| !l.starts_with("-----")).collect();
        let cert_der = base64_decode(&b64);
        assert_eq!(cert_der.len(), 448);
        let cert_sha: [u8; 32] = Sha256::digest(&cert_der).into();
        assert_eq!(Some(cert_sha), signer.cert_sha256);
    }

    /// Minimal standard-alphabet base64 decoder for the PEM fixture (no
    /// base64 crate in the dependency set).
    fn base64_decode(s: &str) -> Vec<u8> {
        const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut out = Vec::new();
        let mut acc = 0u32;
        let mut bits = 0;
        for c in s.bytes() {
            if c == b'=' {
                break;
            }
            let v = ALPHABET.iter().position(|&a| a == c).expect("base64 char") as u32;
            acc = (acc << 6) | v;
            bits += 6;
            if bits >= 8 {
                bits -= 8;
                out.push((acc >> bits) as u8);
                acc &= (1 << bits) - 1;
            }
        }
        out
    }

    #[test]
    fn token_with_two_signer_infos_is_rejected() {
        let info = fixture_signer_info();
        let two = rebuild_token(&fixture_token_der(), Some(vec![info.clone(), info]));
        let err = parse_token_signer(&two).unwrap_err().to_string();
        assert!(err.contains("more than one SignerInfo"), "{err}");
    }

    #[test]
    fn token_without_signer_infos_is_rejected() {
        let none = rebuild_token(&fixture_token_der(), None);
        assert!(parse_token_signer(&none).is_err());
        // An explicitly empty SET is the named case.
        let empty = rebuild_token(&fixture_token_der(), Some(Vec::new()));
        let err = parse_token_signer(&empty).unwrap_err().to_string();
        assert!(err.contains("carries no SignerInfo"), "{err}");
    }

    #[test]
    fn parse_token_unchanged_on_signer_failure() {
        let original = parse_token(&fixture_token_der()).unwrap();
        let info = fixture_signer_info();
        for surgery in [
            rebuild_token(&fixture_token_der(), Some(vec![info.clone(), info])),
            rebuild_token(&fixture_token_der(), None),
            rebuild_token(&fixture_token_der(), Some(Vec::new())),
        ] {
            // A rebuilt token that keeps the original layout is byte-equal,
            // proving the rebuild itself is faithful.
            let token = parse_token(&surgery).unwrap();
            assert_eq!(token.imprint, original.imprint);
            assert_eq!(token.gen_time, original.gen_time);
            assert_eq!(token.serial_hex, original.serial_hex);
            assert_eq!(token.policy_oid, original.policy_oid);
            assert_eq!(parse_token_imprint(&surgery).unwrap(), fixture_digest());
        }
        let faithful = rebuild_token(&fixture_token_der(), Some(vec![fixture_signer_info()]));
        assert_eq!(faithful, fixture_token_der());
    }

    #[test]
    fn list_anchors_lenient_isolates_a_malformed_row() {
        let conn = Connection::open_in_memory().unwrap();
        ensure_anchor_table(&conn).unwrap();
        let token = parse_token(&fixture_token_der()).unwrap();
        let good1 = insert_anchor(&conn, "digest", &fixture_digest(), "(offline)", &token).unwrap();
        conn.execute(
            "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
                subject_hash, tsa_url, gen_time, token_der)
             VALUES (0, 600, 'digest', ?1, '(offline)', '20260610123324Z', x'00')",
            [vec![0u8; 31]],
        )
        .unwrap();
        let bad = conn.last_insert_rowid();
        let good2 = insert_anchor(&conn, "digest", &fixture_digest(), "(offline)", &token).unwrap();

        let err = list_anchors(&conn).unwrap_err().to_string();
        assert!(err.contains(&format!("corrupt anchor {bad}")), "{err}");

        let rows = list_anchors_lenient(&conn).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].0, good1);
        assert!(rows[0].1.is_ok());
        assert_eq!(rows[1].0, bad);
        let reason = rows[1].1.as_ref().unwrap_err().to_string();
        assert!(reason.contains("subject_hash size"), "{reason}");
        assert_eq!(rows[2].0, good2);
        let last = rows[2].1.as_ref().unwrap();
        assert_eq!(
            last.signer_cert_sha256.as_deref(),
            Some("fbf1c838f80923a01badb6030b9d708c5ef1a65b7d23f1f53a6aa274d1b99542")
        );
        assert_eq!(last.ledger_id, None);
        assert_eq!(last.tsa_name, None);
    }

    #[test]
    fn rejection_response_fails_loudly() {
        // TimeStampResp { PKIStatusInfo { status 2 (rejection),
        // statusString ["nope"] } } — hand-built DER.
        let der = [
            0x30, 0x0f, 0x30, 0x0d, 0x02, 0x01, 0x02, 0x30, 0x08, 0x0c, 0x06, b'd', b'e', b'n',
            b'i', b'e', b'd',
        ];
        let err = parse_response(&der).unwrap_err().to_string();
        assert!(err.contains("status 2") && err.contains("denied"), "{err}");
    }
}
