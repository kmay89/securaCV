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
//! the time of the request — see docs/timestamping.md.
//!
//! Scope: this module builds `TimeStampReq` and parses `TimeStampResp`
//! (RFC 3161 §2.4) with a minimal DER reader — enough to check the granted
//! status, match the message imprint and nonce, and extract `genTime` /
//! serial / policy. It does **not** validate the CMS countersignature
//! chain; that final cryptographic step is delegated to an independent
//! toolchain (`openssl ts -verify`), which `log_anchor verify` can invoke.
//! Relying on a second implementation for the trust-critical step is
//! deliberate (same stance as the dual JS/Rust envelope verifiers).

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

// -------------------- Anchor persistence --------------------

use crate::TimeBucket;
use rusqlite::Connection;

/// A stored anchor: an RFC 3161 token over a hash from the witness chain.
#[derive(Debug, Clone)]
pub struct AnchorRecord {
    pub id: i64,
    pub created_bucket: TimeBucket,
    /// What was anchored: `chain_head` or `digest` (operator-supplied).
    pub subject: String,
    pub subject_hash: [u8; 32],
    pub tsa_url: String,
    pub gen_time: String,
    pub token_der: Vec<u8>,
}

/// Create the anchors table if missing. Additive and independent of the
/// sealed-log schema: anchors reference chain hashes but never alter them.
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
    Ok(())
}

pub fn insert_anchor(
    conn: &Connection,
    subject: &str,
    subject_hash: &[u8; 32],
    tsa_url: &str,
    token: &TimestampToken,
) -> Result<i64> {
    let bucket = TimeBucket::now_10min()?;
    conn.execute(
        "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
            subject_hash, tsa_url, gen_time, token_der) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        rusqlite::params![
            bucket.start_epoch_s as i64,
            bucket.size_s as i64,
            subject,
            subject_hash.as_slice(),
            tsa_url,
            token.gen_time,
            token.token_der,
        ],
    )?;
    Ok(conn.last_insert_rowid())
}

pub fn list_anchors(conn: &Connection) -> Result<Vec<AnchorRecord>> {
    let mut stmt = conn.prepare(
        "SELECT id, created_bucket_start, created_bucket_size, subject, subject_hash,
                tsa_url, gen_time, token_der FROM tsa_anchors ORDER BY id",
    )?;
    let rows = stmt.query_map([], |row| {
        Ok((
            row.get::<_, i64>(0)?,
            row.get::<_, i64>(1)?,
            row.get::<_, i64>(2)?,
            row.get::<_, String>(3)?,
            row.get::<_, Vec<u8>>(4)?,
            row.get::<_, String>(5)?,
            row.get::<_, String>(6)?,
            row.get::<_, Vec<u8>>(7)?,
        ))
    })?;
    let mut out = Vec::new();
    for row in rows {
        let (id, start, size, subject, hash, tsa_url, gen_time, token_der) = row?;
        let subject_hash: [u8; 32] = hash
            .try_into()
            .map_err(|_| anyhow!("corrupt anchor {id}: subject_hash size"))?;
        out.push(AnchorRecord {
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
        });
    }
    Ok(out)
}

/// Current chain head: the newest sealed event's entry hash, else the last
/// retention checkpoint's head. Errors on an empty log — there is nothing
/// meaningful to anchor.
pub fn chain_head(conn: &Connection) -> Result<[u8; 32]> {
    let read32 = |sql: &str| -> Result<Option<[u8; 32]>> {
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
    };
    if let Some(head) = read32("SELECT entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1")? {
        return Ok(head);
    }
    if let Some(head) = read32("SELECT chain_head_hash FROM checkpoints ORDER BY id DESC LIMIT 1")?
    {
        return Ok(head);
    }
    bail!("sealed log is empty: nothing to anchor")
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
