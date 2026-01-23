use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::Connection;

use witness_kernel::device_public_key_from_db;

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

pub fn hex32(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
