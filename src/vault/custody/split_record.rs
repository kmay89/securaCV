//! The split record `<split8>.split.json` (spec §2.5): the public record of
//! one dealing that every holder compares their share against. One line of
//! compact JSON, at most 64 KiB, unknown fields refused.

use serde::{Deserialize, Serialize};

use super::group::{decode_commitment, Commitments};
use super::share::{RosterEntry, ShareHeader};
use super::{
    holder_id_ok, CustodyError, MAX_HOLDERS, MIN_THRESHOLD, ROLE_OPERATIONAL, ROLE_RECOVERY,
    ROLE_REHEARSAL, SPLIT_RECORD_FORMAT, SUITE_V1,
};

/// Cap on a split record, checked before it is parsed.
pub(crate) const SPLIT_RECORD_MAX: usize = 65_536;

/// One holder in a split record.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct SplitHolder {
    index: u8,
    id: String,
    signing_public_key: String,
    recipient_key_hash: String,
}

/// The public record of one split.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct SplitRecord {
    format: String,
    role: String,
    split_id: String,
    n: u8,
    m: u8,
    kek_commitment: String,
    commitments: Vec<String>,
    commitments_sha256: String,
    container_sha256: String,
    device_public_key: String,
    issued_bucket: u64,
    holders: Vec<SplitHolder>,
}

/// The name a split record gives a share role.
pub(crate) fn role_name(role: u8) -> &'static str {
    match role {
        ROLE_OPERATIONAL => "operational",
        ROLE_RECOVERY => "recovery",
        _ => "rehearsal",
    }
}

fn role_byte(name: &str) -> Option<u8> {
    match name {
        "operational" => Some(ROLE_OPERATIONAL),
        "recovery" => Some(ROLE_RECOVERY),
        "rehearsal" => Some(ROLE_REHEARSAL),
        _ => None,
    }
}

fn hex_n<const N: usize>(s: &str) -> Result<[u8; N], CustodyError> {
    if s.len() != 2 * N || !s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')) {
        return Err(CustodyError::SplitRecord(
            "a hex field has the wrong length or case",
        ));
    }
    let mut out = [0u8; N];
    hex::decode_to_slice(s, &mut out)
        .map_err(|_| CustodyError::SplitRecord("a hex field has the wrong length or case"))?;
    Ok(out)
}

impl SplitRecord {
    /// The record of the split a share header describes.
    pub(crate) fn from_header(h: &ShareHeader) -> Self {
        let commitments: Vec<String> = h.commitments.encode().iter().map(hex::encode).collect();
        SplitRecord {
            format: SPLIT_RECORD_FORMAT.to_string(),
            role: role_name(h.role).to_string(),
            split_id: hex::encode(h.split_id),
            n: h.n,
            m: h.m,
            kek_commitment: commitments[0].clone(),
            commitments,
            commitments_sha256: hex::encode(h.commitments_sha256()),
            container_sha256: hex::encode(h.container_sha256),
            device_public_key: hex::encode(h.device_public_key),
            issued_bucket: h.issued_bucket_start,
            holders: h
                .roster
                .iter()
                .map(|e| SplitHolder {
                    index: e.index,
                    id: e.id.clone(),
                    signing_public_key: hex::encode(e.signing_public_key),
                    recipient_key_hash: hex::encode(e.recipient_key_hash),
                })
                .collect(),
        }
    }

    /// The file's one line: compact JSON in field order, then LF.
    pub(crate) fn to_json_text(&self) -> String {
        format!(
            "{}\n",
            serde_json::to_string(self).expect("plain fields serialize")
        )
    }

    /// Parse and check a split record: the cap, unknown fields, the
    /// format, the bounds, the commitments (canonical, the first equal to
    /// the KEK commitment, the hash recomputed) and the holder list
    /// (indices exactly `1..=m`, ids unique).
    pub(crate) fn from_json(bytes: &[u8]) -> Result<Self, CustodyError> {
        if bytes.len() > SPLIT_RECORD_MAX {
            return Err(CustodyError::SplitRecord("longer than 65536 bytes"));
        }
        let r: SplitRecord = serde_json::from_slice(bytes).map_err(|_| {
            CustodyError::SplitRecord("not a securacv-custody-split-record:v1 JSON object")
        })?;
        if r.format != SPLIT_RECORD_FORMAT {
            return Err(CustodyError::SplitRecord(
                "not a securacv-custody-split-record:v1 file",
            ));
        }
        let role = role_byte(&r.role).ok_or(CustodyError::SplitRecord(
            "role must be operational, recovery or rehearsal",
        ))?;
        if r.n < MIN_THRESHOLD || r.m > MAX_HOLDERS || r.n > r.m {
            return Err(CustodyError::SplitRecord(
                "threshold must be between 2 and 32 and at most the holder count",
            ));
        }
        let commitments = r.commitments()?;
        if hex_n::<32>(&r.kek_commitment)? != commitments.kek_commitment() {
            return Err(CustodyError::SplitRecord(
                "its first commitment is not the KEK commitment",
            ));
        }
        let split_id = hex_n::<16>(&r.split_id)?;
        if hex_n::<32>(&r.commitments_sha256)? != commitments.sha256(SUITE_V1, role, &split_id, r.m)
        {
            return Err(CustodyError::SplitRecord(
                "commitments_sha256 does not follow from its commitments",
            ));
        }
        hex_n::<32>(&r.container_sha256)?;
        hex_n::<32>(&r.device_public_key)?;
        let roster = r.roster()?;
        if roster.len() != usize::from(r.m)
            || roster
                .iter()
                .enumerate()
                .any(|(i, e)| usize::from(e.index) != i + 1)
        {
            return Err(CustodyError::SplitRecord(
                "holder indices must be 1..m in order",
            ));
        }
        for (i, a) in roster.iter().enumerate() {
            if roster[i + 1..].iter().any(|b| b.id == a.id) {
                return Err(CustodyError::SplitRecord("a holder id appears twice"));
            }
        }
        Ok(r)
    }

    fn commitments(&self) -> Result<Commitments, CustodyError> {
        if self.commitments.len() != usize::from(self.n) {
            return Err(CustodyError::SplitRecord(
                "it must list exactly n commitments",
            ));
        }
        let mut points = Vec::with_capacity(self.commitments.len());
        for (j, c) in self.commitments.iter().enumerate() {
            points.push(decode_commitment(&hex_n::<32>(c)?, j).map_err(|_| {
                CustodyError::SplitRecord("a commitment is not a canonical, non-identity point")
            })?);
        }
        Ok(Commitments::from_points(points))
    }

    fn roster(&self) -> Result<Vec<RosterEntry>, CustodyError> {
        self.holders
            .iter()
            .map(|h| {
                if !holder_id_ok(&h.id) {
                    return Err(CustodyError::SplitRecord(
                        "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
                    ));
                }
                Ok(RosterEntry {
                    index: h.index,
                    id: h.id.clone(),
                    signing_public_key: hex_n(&h.signing_public_key)?,
                    recipient_key_hash: hex_n(&h.recipient_key_hash)?,
                })
            })
            .collect()
    }

    /// The split id.
    pub(crate) fn split_id(&self) -> Result<[u8; 16], CustodyError> {
        hex_n(&self.split_id)
    }

    /// True when the share header describes exactly this split: role, id,
    /// n and m, every commitment, the container and device pins, the
    /// bucket and every holder.
    pub(crate) fn matches(&self, h: &ShareHeader) -> bool {
        SplitRecord::from_header(h) == *self
    }

    /// True when the commitments (and their hash) are the share's: the
    /// broadcast-consistency question `share verify` answers first.
    pub(crate) fn same_commitments(&self, h: &ShareHeader) -> bool {
        let other = SplitRecord::from_header(h);
        other.commitments == self.commitments && other.commitments_sha256 == self.commitments_sha256
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vault::custody::share::tests::test_split;
    use crate::vault::custody::PROTECTION_TRANSPORT_BOX1;

    #[test]
    fn split_record_round_trip_and_refusals() {
        let split = test_split(3, 5, ROLE_OPERATIONAL);
        let header = split.header(2, PROTECTION_TRANSPORT_BOX1);
        let record = SplitRecord::from_header(&header);
        let text = record.to_json_text();
        assert!(text.starts_with(
            "{\"format\":\"securacv-custody-split-record:v1\",\"role\":\"operational\",\"split_id\":\"5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c\",\"n\":3,\"m\":5,\"kek_commitment\":\""
        ));
        assert!(text.ends_with("}]}\n"));
        let parsed = SplitRecord::from_json(text.as_bytes()).unwrap();
        assert_eq!(parsed, record);
        assert!(parsed.matches(&header));
        assert!(parsed.same_commitments(&header));
        assert_eq!(parsed.split_id().unwrap(), [0x5c; 16]);
        // Every holder's header names the same record.
        for i in 1..=5 {
            assert!(parsed.matches(&split.header(i, PROTECTION_TRANSPORT_BOX1)));
        }
        // A different dealing has different commitments.
        let other = test_split(3, 5, ROLE_OPERATIONAL);
        let other_header = other.header(2, PROTECTION_TRANSPORT_BOX1);
        assert!(!parsed.same_commitments(&other_header));
        assert!(!parsed.matches(&other_header));

        let refuse = |s: String| {
            SplitRecord::from_json(s.as_bytes())
                .unwrap_err()
                .to_string()
        };
        assert_eq!(
            refuse(text.replacen('{', "{\"x\":1,", 1)),
            "split record: not a securacv-custody-split-record:v1 JSON object"
        );
        assert_eq!(
            refuse(text.replace("split-record:v1", "split-record:v2")),
            "split record: not a securacv-custody-split-record:v1 file"
        );
        assert_eq!(
            refuse(text.replace("\"operational\"", "\"spare\"")),
            "split record: role must be operational, recovery or rehearsal"
        );
        // The role is in the commitment hash: relabeling is caught.
        assert_eq!(
            refuse(text.replace("\"operational\"", "\"recovery\"")),
            "split record: commitments_sha256 does not follow from its commitments"
        );
        assert_eq!(
            refuse(text.replace("\"n\":3", "\"n\":1")),
            "split record: threshold must be between 2 and 32 and at most the holder count"
        );
        assert_eq!(
            refuse(text.replace("\"n\":3", "\"n\":2")),
            "split record: it must list exactly n commitments"
        );
        let c1 = &record.commitments[1];
        assert_eq!(
            refuse(text.replace(c1.as_str(), &"0".repeat(64))),
            "split record: a commitment is not a canonical, non-identity point"
        );
        assert_eq!(
            refuse(text.replacen(&record.kek_commitment, &record.commitments[2], 1)),
            "split record: its first commitment is not the KEK commitment"
        );
        assert_eq!(
            refuse(text.replace(&record.split_id, &record.split_id.to_uppercase())),
            "split record: a hex field has the wrong length or case"
        );
        assert_eq!(
            refuse(text.replace("\"id\":\"erin\"", "\"id\":\"alice\"")),
            "split record: a holder id appears twice"
        );
        assert_eq!(
            refuse(text.replace("{\"index\":5,", "{\"index\":6,")),
            "split record: holder indices must be 1..m in order"
        );
        let mut huge = text.clone().into_bytes();
        huge.resize(SPLIT_RECORD_MAX + 1, b' ');
        assert_eq!(
            SplitRecord::from_json(&huge).unwrap_err().to_string(),
            "split record: longer than 65536 bytes"
        );
    }
}
