use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::Connection;

use crate::crypto::signatures::PqPublicKey;
use crate::device_public_key_from_db;
#[cfg(feature = "pqc-signatures")]
use pqcrypto_traits::sign::PublicKey as PqPublicKeyTrait;
use rusqlite::OptionalExtension;

pub fn load_verifying_key(
    conn: &Connection,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
) -> Result<VerifyingKey> {
    if let Some(hex) = public_key_hex {
        return verifying_key_from_hex(hex);
    }
    if let Some(path) = public_key_file {
        let key_hex = std::fs::read_to_string(path)
            .map_err(|e| anyhow!("failed to read public key file {}: {}", path, e))?;
        return verifying_key_from_hex(key_hex.trim());
    }
    device_public_key_from_db(conn).map_err(|e| {
        anyhow!(
            "{} (provide --public-key or --public-key-file if the database has no key)",
            e
        )
    })
}

pub fn verifying_key_from_hex(hex_str: &str) -> Result<VerifyingKey> {
    let bytes = hex::decode(hex_str.trim()).map_err(|e| anyhow!("invalid hex: {}", e))?;
    let mut key_bytes = [0u8; 32];
    if bytes.len() != 32 {
        return Err(anyhow!(
            "invalid public key length: expected 32 bytes, got {}",
            bytes.len()
        ));
    }
    key_bytes.copy_from_slice(&bytes);
    VerifyingKey::from_bytes(&key_bytes).map_err(|e| anyhow!("invalid public key bytes: {}", e))
}

#[cfg(feature = "pqc-signatures")]
pub fn load_pq_verifying_key(
    conn: &Connection,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
) -> Result<Option<PqPublicKey>> {
    if let Some(hex) = public_key_hex {
        return pq_verifying_key_from_hex(hex).map(Some);
    }
    if let Some(path) = public_key_file {
        let key_hex = std::fs::read_to_string(path)
            .map_err(|e| anyhow!("failed to read pq public key file {}: {}", path, e))?;
        return pq_verifying_key_from_hex(key_hex.trim()).map(Some);
    }
    load_pq_key_from_db_optional(conn)
}

#[cfg(not(feature = "pqc-signatures"))]
pub fn load_pq_verifying_key(
    _conn: &Connection,
    public_key_hex: Option<&str>,
    public_key_file: Option<&str>,
) -> Result<Option<PqPublicKey>> {
    if public_key_hex.is_some() || public_key_file.is_some() {
        return Err(anyhow!(
            "pq signatures not supported (pqc-signatures feature disabled)"
        ));
    }
    Ok(None)
}

#[cfg(feature = "pqc-signatures")]
fn pq_verifying_key_from_hex(hex_str: &str) -> Result<PqPublicKey> {
    let bytes = hex::decode(hex_str.trim()).map_err(|e| anyhow!("invalid hex: {}", e))?;
    PqPublicKey::from_bytes(&bytes).map_err(|e| anyhow!("invalid pq public key bytes: {}", e))
}

#[cfg(feature = "pqc-signatures")]
fn load_pq_key_from_db_optional(conn: &Connection) -> Result<Option<PqPublicKey>> {
    let bytes: Option<Vec<u8>> = conn
        .query_row(
            "SELECT pq_public_key FROM device_metadata WHERE id = 1",
            [],
            |row| row.get(0),
        )
        .optional()?;
    let Some(bytes) = bytes else {
        return Ok(None);
    };
    PqPublicKey::from_bytes(&bytes)
        .map(Some)
        .map_err(|e| anyhow!("invalid pq public key bytes: {}", e))
}

/// The device keys that may legitimately sign receipt-ledger and policy-change
/// rows: the genesis-anchored key lineage, genesis first.
///
/// Receipts and policy-change records are signed by the device key **current at
/// write time**, so after a `rotate_device_identity` the ledgers legitimately
/// carry rows signed by different lineage keys. Verifying them all against the
/// genesis key alone raises a false tamper alarm on every post-rotation row.
/// The lineage reconstruction cryptographically validates every epoch
/// (possession attestation + predecessor authorization anchored at genesis), so
/// any key it returns is trustworthy.
///
/// A database with no `device_key_history` table (pre-rotation vintage, or a
/// bare export inspected by an external verifier) has a genesis-only lineage.
pub fn lineage_verifying_keys(
    conn: &Connection,
    genesis: &VerifyingKey,
) -> Result<Vec<VerifyingKey>> {
    let has_history: bool = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='device_key_history' LIMIT 1",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false);
    if !has_history {
        return Ok(vec![*genesis]);
    }
    let lineage = crate::reconstruct_device_key_lineage_from(conn, &genesis.to_bytes())?;
    lineage
        .iter()
        .map(|epoch| {
            VerifyingKey::from_bytes(&epoch.public_key)
                .map_err(|e| anyhow!("invalid lineage verifying key: {}", e))
        })
        .collect()
}

pub fn hex32(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

/// Display prefix (up to 16 chars) of a key string that may be short or
/// garbage. Diagnostic printers (`log_verify --lineage`/`--checkpoints`)
/// format keys reconstructed from possibly-tampered database rows — the tools
/// exist to inspect exactly that data, so truncation must never panic, and it
/// must stay safe on non-ASCII bytes that survived a lossy decode.
pub fn key_prefix(s: &str) -> &str {
    match s.char_indices().nth(16) {
        Some((idx, _)) => &s[..idx],
        None => s,
    }
}

#[cfg(test)]
mod tests {
    use super::key_prefix;

    #[test]
    fn key_prefix_never_panics_and_clamps() {
        assert_eq!(key_prefix(""), "");
        assert_eq!(key_prefix("aabb"), "aabb"); // short tampered key
        assert_eq!(key_prefix("0123456789abcdef"), "0123456789abcdef");
        assert_eq!(key_prefix("0123456789abcdef0123"), "0123456789abcdef");
        assert_eq!(key_prefix("not hex at all — garbage"), "not hex at all —");
        // Multibyte chars straddling the cut must not split a boundary.
        let s = "ééééééééééééééééé";
        assert_eq!(key_prefix(s), &s[..key_prefix(s).len()]);
        assert_eq!(key_prefix(s).chars().count(), 16);
    }
}
