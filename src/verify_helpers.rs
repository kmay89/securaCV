use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::Connection;

use crate::crypto::signatures::PqPublicKey;
use crate::device_public_key_from_db;
#[cfg(feature = "pqc-signatures")]
use pqcrypto_traits::sign::PublicKey as PqPublicKeyTrait;
#[cfg(feature = "pqc-signatures")]
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

pub fn hex32(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
