//! The clock-provenance ledger, read side.
//!
//! `witnessd` — the process whose wall clock stamps every `time_bucket` —
//! polls the local `tsa_anchors` table and seals one device-signed,
//! hash-chained [`ClockProvenanceRecord`] per anchor row into the never-pruned
//! `clock_provenance` table (`Kernel::observe_anchors_for_clock_provenance`).
//! The record binds the token by hash and, when the row appeared between two
//! polls of a running daemon, carries the measured device-clock-vs-`genTime`
//! offset in whole clamped seconds and the poll gap that bounds how old the
//! row could have been. Its head is the fifth anchor subject
//! (`clock_provenance_head`), so every record is fixed in external time by
//! the next scheduled `anchor-all`.
//!
//! This module holds what every reader shares — `log_anchor verify`,
//! `run_full_verify` and `court_export`: row decoding, the record↔anchor
//! link, the arithmetic a third party can repeat with `sha256sum` and
//! `openssl`, the display bands, and the ClockSkew correlation helpers. The
//! writer lives in `lib.rs`; the chain walk lives in `verify.rs`. Design and
//! the verifier-first table of checks: `docs/design/clock_provenance.md`.

use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::{Connection, OptionalExtension};
use sha2::{Digest, Sha256};

use crate::crypto::signatures::{PqPublicKey, SignatureMode, SignatureSet};
use crate::tsa::AnchorRecord;
use crate::{ClockProvenanceRecord, CLOCK_PROVENANCE_CLAMP_S};

/// One kernel time bucket, the structural threshold and the display band.
pub const ONE_BUCKET_S: i64 = 600;

/// A record is *prompt* when the anchor row's bucket starts no more than this
/// after the token's `genTime` — the row was stored within two buckets of
/// issuance (online anchoring; an offline round trip completed quickly). A
/// positive offset on a delayed import is not a clock reading and is never
/// evaluated; a negative one is (behind is behind, however long the token sat
/// in a mailbox).
pub const PROMPT_LAG_MAX_S: i64 = 1200;

/// One stored ledger row: the decoded record beside the literal bytes that
/// were hashed. `payload_json` is what `entry_hash` covers; never re-serialize
/// `record` to check it.
#[derive(Clone, Debug)]
pub struct ClockProvenanceRow {
    pub id: i64,
    /// `time_bucket.start_epoch_s` at seal time.
    pub created_at: i64,
    pub payload_json: String,
    pub prev_hash: [u8; 32],
    pub entry_hash: [u8; 32],
    pub signatures: SignatureSet,
    pub record: ClockProvenanceRecord,
}

impl ClockProvenanceRow {
    /// Binding-only: the row was already present at the first poll of a
    /// `witnessd` process, so its age was unbounded and nothing was measured.
    pub fn is_binding_only(&self) -> bool {
        self.record.observed_offset_s.is_none()
    }
}

/// Whether the database has a `clock_provenance` table. Read-only tools treat
/// absence as "no records"; only `Kernel::ensure_schema` creates it.
pub fn table_exists(conn: &Connection) -> Result<bool> {
    Ok(conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='clock_provenance' LIMIT 1",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false))
}

/// Row count, 0 when the table is absent.
pub fn row_count(conn: &Connection) -> Result<u64> {
    if !table_exists(conn)? {
        return Ok(0);
    }
    Ok(
        conn.query_row("SELECT COUNT(*) FROM clock_provenance", [], |row| {
            row.get::<_, i64>(0)
        })? as u64,
    )
}

/// Every row in id order (none when the table is absent). A payload that
/// does not parse as a [`ClockProvenanceRecord`] is a hard error: the callers
/// want to stop there, not reason about a record they cannot read.
pub fn list_rows(conn: &Connection) -> Result<Vec<ClockProvenanceRow>> {
    if !table_exists(conn)? {
        return Ok(Vec::new());
    }
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, prev_hash, entry_hash, signature, pq_signature, \
         pq_scheme FROM clock_provenance ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let payload_json: String = row.get(2)?;
        let prev_hash = blob32(row.get::<_, Vec<u8>>(3)?, id, "prev_hash")?;
        let entry_hash = blob32(row.get::<_, Vec<u8>>(4)?, id, "entry_hash")?;
        let signature: Vec<u8> = row.get(5)?;
        let pq_signature: Option<Vec<u8>> = row.get(6)?;
        let pq_scheme: Option<String> = row.get(7)?;
        let signatures = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        let record: ClockProvenanceRecord = serde_json::from_str(&payload_json)
            .map_err(|e| anyhow!("clock-provenance record {id}: payload does not parse: {e}"))?;
        out.push(ClockProvenanceRow {
            id,
            created_at,
            payload_json,
            prev_hash,
            entry_hash,
            signatures,
            record,
        });
    }
    Ok(out)
}

fn blob32(bytes: Vec<u8>, id: i64, column: &str) -> Result<[u8; 32]> {
    bytes
        .try_into()
        .map_err(|_| anyhow!("clock-provenance record {id}: {column} is not 32 bytes"))
}

/// The record naming `anchor_id`, if any. Records are 1:1 with anchor rows by
/// construction (the cursor); when a database nonetheless holds two, the
/// first (oldest) one is the one the daemon sealed first.
pub fn record_for_anchor(
    rows: &[ClockProvenanceRow],
    anchor_id: i64,
) -> Option<&ClockProvenanceRow> {
    rows.iter().find(|r| r.record.anchor_id == anchor_id)
}

/// SHA-256 over a stored token's DER bytes, 64 lowercase hex — the value
/// `sha256sum anchors/<token>.der` prints.
pub fn token_sha256_hex(token_der: &[u8]) -> String {
    hex::encode(Sha256::digest(token_der))
}

/// How a record relates to the anchors table as it stands now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AnchorLink {
    /// The row exists and its token hashes to the record's `token_sha256`.
    Matches,
    /// No row with the record's `anchor_id`: the anchor was deleted (or the
    /// table rebuilt).
    AnchorMissing,
    /// The row exists but its token hashes differently: the token was
    /// replaced after the device bound the original.
    TokenReplaced { stored_token_sha256: String },
}

/// Cross-check one record against the anchor rows that decoded.
pub fn link_to_anchor(record: &ClockProvenanceRecord, anchors: &[AnchorRecord]) -> AnchorLink {
    match anchors.iter().find(|a| a.id == record.anchor_id) {
        None => AnchorLink::AnchorMissing,
        Some(a) => {
            let stored = token_sha256_hex(&a.token_der);
            if stored == record.token_sha256 {
                AnchorLink::Matches
            } else {
                AnchorLink::TokenReplaced {
                    stored_token_sha256: stored,
                }
            }
        }
    }
}

/// The arithmetic over one measured record and its anchor row — every term
/// re-derivable by a third party from the token, the row and the record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Assessment {
    /// The TSA's time, from the token (fractional seconds truncated).
    pub gen_time_unix: i64,
    /// `anchor.created_bucket.start − genTime`: how long after issuance the
    /// anchoring host stored the row (its own clock, bucketed).
    pub lag_s: i64,
    /// `lag_s <= PROMPT_LAG_MAX_S`.
    pub prompt: bool,
    /// `genTime + observed_offset_s`: what the device clock read when it saw
    /// the row.
    pub device_wall_s: i64,
    /// `device_wall_s − anchor.created_bucket.start`; expected in
    /// `0..PROMPT_LAG_MAX_S` when the anchoring host and the device agree.
    pub host_gap_s: i64,
    /// `host_gap_s` outside that range.
    pub host_clock_disagrees: bool,
    /// The stored offset sits at the clamp bound.
    pub clamped: bool,
}

/// Assess a measured record against its anchor row. `None` for a
/// binding-only record, or when the row's token (or its `genTime`) no longer
/// parses — the caller distinguishes the two by `observed_offset_s`.
pub fn assess(record: &ClockProvenanceRecord, anchor: &AnchorRecord) -> Option<Assessment> {
    let offset = i64::from(record.observed_offset_s?);
    let token = crate::tsa::parse_token(&anchor.token_der).ok()?;
    let gen_time_unix = crate::tsa::gen_time_unix(&token.gen_time)?;
    let bucket_start = anchor.created_bucket.start_epoch_s as i64;
    let lag_s = bucket_start - gen_time_unix;
    let device_wall_s = gen_time_unix + offset;
    let host_gap_s = device_wall_s - bucket_start;
    Some(Assessment {
        gen_time_unix,
        lag_s,
        prompt: lag_s <= PROMPT_LAG_MAX_S,
        device_wall_s,
        host_gap_s,
        host_clock_disagrees: !(0..PROMPT_LAG_MAX_S).contains(&host_gap_s),
        clamped: offset.abs() >= CLOCK_PROVENANCE_CLAMP_S,
    })
}

/// Whether a measured record's offset is evaluated AND beyond `threshold_s`:
/// a prompt record is evaluated either way; a delayed one only when the
/// device read behind (a late import cannot make a clock look slow).
pub fn beyond(record: &ClockProvenanceRecord, a: &Assessment, threshold_s: i64) -> bool {
    let Some(offset) = record.observed_offset_s else {
        return false;
    };
    let offset = i64::from(offset);
    (a.prompt || offset < 0) && offset.abs() > threshold_s
}

/// Structural check on a `chain_head` anchor whose head is a live sealed
/// row: the row's bucket started `n = created_at − genTime` seconds after
/// the TSA's time. The device sealed at a wall clock at or past its bucket
/// start, and the token was issued at or after the seal, so `n > 1` means
/// the device read at least `n` seconds ahead at seal time. `Some(n)` only
/// then (a one-second margin absorbs the token's own second truncation).
pub fn seal_precedes_gen_time(sealed_row_bucket_start: i64, gen_time_unix: i64) -> Option<i64> {
    let n = sealed_row_bucket_start - gen_time_unix;
    (n > 1).then_some(n)
}

/// The `created_at` buckets of every retained `ClockSkew` failure record in
/// the sealed log (tolerant parse: unreadable payloads are skipped; a
/// database without a sealed log has none). Retention prunes failure
/// records, so an empty answer means "none retained", never "none happened".
pub fn clock_skew_buckets(conn: &Connection) -> Result<Vec<i64>> {
    let has_table: bool = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sealed_events' LIMIT 1",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false);
    if !has_table {
        return Ok(Vec::new());
    }
    let mut stmt =
        conn.prepare("SELECT created_at, payload_json FROM sealed_events ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let created_at: i64 = row.get(0)?;
        let payload: String = row.get(1)?;
        if let Ok(crate::SealedLogRecord::Failure(f)) =
            crate::SealedLogRecord::deserialize_compat(&payload)
        {
            if f.failure_type == crate::FailureType::ClockSkew {
                out.push(created_at);
            }
        }
    }
    Ok(out)
}

/// The first retained ClockSkew bucket within `radius` seconds of `center`.
pub fn skew_within(buckets: &[i64], center: i64, radius: i64) -> Option<i64> {
    buckets
        .iter()
        .copied()
        .find(|b| (b - center).abs() <= radius)
}

/// The first retained ClockSkew bucket in `lo..=hi` (either order).
pub fn skew_between(buckets: &[i64], lo: i64, hi: i64) -> Option<i64> {
    let (lo, hi) = if lo <= hi { (lo, hi) } else { (hi, lo) };
    buckets.iter().copied().find(|b| (lo..=hi).contains(b))
}

/// The offset as the tools print it: signed whole seconds, the clamp bound
/// named as a bound.
pub fn format_offset(offset_s: i32) -> String {
    let clamp = CLOCK_PROVENANCE_CLAMP_S as i32;
    if offset_s >= clamp {
        format!(">= +{clamp} s (clamped)")
    } else if offset_s <= -clamp {
        format!("<= -{clamp} s (clamped)")
    } else {
        format!("{offset_s:+} s")
    }
}

/// A lag in coarse human units: `2d 3h`, `1h 5m`, `45m`, `0m`; a negative
/// lag (the row's bucket starts before the token's time) keeps its sign.
pub fn human_lag(lag_s: i64) -> String {
    let sign = if lag_s < 0 { "-" } else { "" };
    let s = lag_s.unsigned_abs();
    let days = s / 86_400;
    let hours = (s % 86_400) / 3_600;
    let minutes = (s % 3_600) / 60;
    let body = if days > 0 {
        format!("{days}d {hours}h")
    } else if hours > 0 {
        format!("{hours}h {minutes}m")
    } else {
        format!("{minutes}m")
    };
    format!("{sign}{body}")
}

/// When the anchoring host stored the row, relative to the token's time, as
/// the tools print it. `lag_s` is `anchor bucket start − genTime`, so a row
/// stored in the bucket that contains `genTime` has `-600 < lag_s <= 0`:
/// `stored in the bucket of issuance`; later, `stored 2d 3h after issuance`;
/// a bucket that starts a whole bucket or more before the token's time can
/// only come from the anchoring host's clock: `stored 20m before issuance by
/// the anchoring host's clock`.
pub fn stored_text(lag_s: i64) -> String {
    if lag_s > 0 {
        format!("stored {} after issuance", human_lag(lag_s))
    } else if lag_s > -ONE_BUCKET_S {
        "stored in the bucket of issuance".to_string()
    } else {
        format!(
            "stored {} before issuance by the anchoring host's clock",
            human_lag(-lag_s)
        )
    }
}

/// The lineage key under which a row's device signature verifies, if any.
pub fn signer_of(
    row: &ClockProvenanceRow,
    keys: &[VerifyingKey],
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
) -> Option<VerifyingKey> {
    keys.iter().copied().find(|key| {
        crate::verify_entry_signature(
            key,
            &row.entry_hash,
            &row.signatures,
            mode,
            pq_public_key,
            crate::crypto::signatures::DOMAIN_CLOCK_PROVENANCE,
        )
        .is_ok()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::TimeBucket;

    /// The committed fixture token's genTime, `20260610123324Z`.
    const FIXTURE_GEN_TIME: i64 = 1_781_094_804;

    fn fixture_token_der() -> Vec<u8> {
        let path = format!(
            "{}/tests/fixtures/tsa/reply.tsr",
            env!("CARGO_MANIFEST_DIR")
        );
        crate::tsa::parse_response(&std::fs::read(&path).expect("reading TSA fixture"))
            .expect("fixture token parses")
            .token_der
    }

    fn anchor_at(bucket_start: u64) -> AnchorRecord {
        let token_der = fixture_token_der();
        AnchorRecord {
            id: 7,
            created_bucket: TimeBucket {
                start_epoch_s: bucket_start,
                size_s: 600,
            },
            subject: "chain_head".to_string(),
            subject_hash: [0u8; 32],
            tsa_url: "(offline)".to_string(),
            gen_time: "20260610123324Z".to_string(),
            token_der,
            tsa_name: None,
            signer_fingerprint: None,
            signer_sid: None,
            ledger_id: None,
        }
    }

    fn record(offset: Option<i32>, gap: Option<u32>) -> ClockProvenanceRecord {
        ClockProvenanceRecord {
            schema: crate::CLOCK_PROVENANCE_SCHEMA_V1.to_string(),
            time_bucket: TimeBucket {
                start_epoch_s: 1_781_094_600,
                size_s: 600,
            },
            anchor_id: 7,
            token_sha256: token_sha256_hex(&fixture_token_der()),
            observed_offset_s: offset,
            poll_gap_s: gap,
            kernel_version: "0.0.0-test".to_string(),
        }
    }

    #[test]
    fn format_offset_pins_the_display_strings() {
        assert_eq!(format_offset(2), "+2 s");
        assert_eq!(format_offset(-3), "-3 s");
        assert_eq!(format_offset(0), "+0 s");
        assert_eq!(format_offset(3600), ">= +3600 s (clamped)");
        assert_eq!(format_offset(-3600), "<= -3600 s (clamped)");
    }

    #[test]
    fn human_lag_pins_the_units() {
        assert_eq!(human_lag(0), "0m");
        assert_eq!(human_lag(59), "0m");
        assert_eq!(human_lag(45 * 60), "45m");
        assert_eq!(human_lag(3_600 + 5 * 60), "1h 5m");
        assert_eq!(human_lag(2 * 86_400 + 3 * 3_600 + 59 * 60), "2d 3h");
        assert_eq!(human_lag(-600), "-10m");
    }

    #[test]
    fn stored_text_pins_the_phrases() {
        assert_eq!(stored_text(-204), "stored in the bucket of issuance");
        assert_eq!(stored_text(0), "stored in the bucket of issuance");
        assert_eq!(stored_text(-599), "stored in the bucket of issuance");
        assert_eq!(
            stored_text(2 * 86_400 - 204),
            "stored 1d 23h after issuance"
        );
        assert_eq!(stored_text(45 * 60), "stored 45m after issuance");
        assert_eq!(
            stored_text(-2_004),
            "stored 33m before issuance by the anchoring host's clock"
        );
    }

    #[test]
    fn assess_prompt_record_within_one_bucket() {
        // Row stored in the bucket the token was issued in; device +2 s.
        let a = assess(&record(Some(2), Some(5)), &anchor_at(1_781_094_600)).unwrap();
        assert_eq!(a.gen_time_unix, FIXTURE_GEN_TIME);
        assert_eq!(a.lag_s, -204);
        assert!(a.prompt);
        assert_eq!(a.device_wall_s, FIXTURE_GEN_TIME + 2);
        assert_eq!(a.host_gap_s, 206);
        assert!(!a.host_clock_disagrees);
        assert!(!a.clamped);
        assert!(!beyond(&record(Some(2), Some(5)), &a, ONE_BUCKET_S));
    }

    #[test]
    fn assess_marks_delayed_imports_and_host_disagreement() {
        // Row stored two days after issuance: a delayed offline import.
        let late = anchor_at(1_781_094_600 + 2 * 86_400);
        let positive = record(Some(900), Some(5));
        let a = assess(&positive, &late).unwrap();
        assert!(!a.prompt);
        assert_eq!(a.lag_s, 2 * 86_400 - 204);
        // A delayed positive offset is never evaluated …
        assert!(!beyond(&positive, &a, ONE_BUCKET_S));
        // … a delayed negative one is (behind is behind).
        let negative = record(Some(-900), Some(5));
        let b = assess(&negative, &late).unwrap();
        assert!(beyond(&negative, &b, ONE_BUCKET_S));
        assert!(!beyond(&negative, &b, 1_000));
        // The device reading sits far before the host's row bucket.
        assert!(b.host_clock_disagrees);
    }

    #[test]
    fn assess_is_none_for_binding_only_and_unreadable_tokens() {
        assert!(assess(&record(None, None), &anchor_at(1_781_094_600)).is_none());
        let mut broken = anchor_at(1_781_094_600);
        broken.token_der = vec![0x30, 0x00];
        assert!(assess(&record(Some(1), Some(5)), &broken).is_none());
    }

    #[test]
    fn clamped_offsets_are_flagged() {
        let a = assess(&record(Some(3600), Some(5)), &anchor_at(1_781_094_600)).unwrap();
        assert!(a.clamped);
        assert!(beyond(&record(Some(3600), Some(5)), &a, ONE_BUCKET_S));
    }

    #[test]
    fn link_to_anchor_distinguishes_missing_from_replaced() {
        let anchor = anchor_at(1_781_094_600);
        assert_eq!(
            link_to_anchor(&record(Some(1), Some(5)), std::slice::from_ref(&anchor)),
            AnchorLink::Matches
        );
        assert_eq!(
            link_to_anchor(&record(Some(1), Some(5)), &[]),
            AnchorLink::AnchorMissing
        );
        let mut replaced = anchor_at(1_781_094_600);
        replaced.token_der.push(0);
        let stored = token_sha256_hex(&replaced.token_der);
        assert_eq!(
            link_to_anchor(&record(Some(1), Some(5)), &[replaced]),
            AnchorLink::TokenReplaced {
                stored_token_sha256: stored
            }
        );
    }

    #[test]
    fn seal_precedes_gen_time_needs_more_than_a_second() {
        assert_eq!(seal_precedes_gen_time(100, 100), None);
        assert_eq!(seal_precedes_gen_time(101, 100), None);
        assert_eq!(seal_precedes_gen_time(102, 100), Some(2));
        assert_eq!(seal_precedes_gen_time(50, 100), None);
    }

    #[test]
    fn skew_lookups() {
        let buckets = [600, 3_000, 9_000];
        assert_eq!(skew_within(&buckets, 1_200, 600), Some(600));
        assert_eq!(skew_within(&buckets, 1_201, 600), None);
        assert_eq!(skew_between(&buckets, 1_000, 4_000), Some(3_000));
        assert_eq!(skew_between(&buckets, 4_000, 1_000), Some(3_000));
        assert_eq!(skew_between(&buckets, 4_000, 8_000), None);
    }
}
