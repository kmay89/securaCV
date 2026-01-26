use anyhow::{anyhow, Result};
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};

/// Hashes a log entry payload with the previous chain hash.
pub fn hash_entry(prev_hash: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_hash);
    hasher.update(payload);
    hasher.finalize().into()
}

/// Signs a log entry hash using Ed25519.
pub fn sign_entry(signing_key: &SigningKey, entry_hash: &[u8; 32]) -> [u8; 64] {
    signing_key.sign(entry_hash).to_bytes()
}

/// Verifies a log entry signature against its entry hash.
pub fn verify_entry_signature(
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signature: &[u8; 64],
) -> Result<()> {
    let sig = ed25519_dalek::Signature::from_bytes(signature);
    verifying_key
        .verify(entry_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}
