use anyhow::Result;
use ed25519_dalek::VerifyingKey;
use sha2::{Digest, Sha256};

use crate::crypto::signatures::{
    sign_with_domain, verify_with_domain, PqPublicKey, SignatureKeys, SignatureMode, SignatureSet,
};

/// Hashes a log entry payload with the previous chain hash.
pub fn hash_entry(prev_hash: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_hash);
    hasher.update(payload);
    hasher.finalize().into()
}

/// Signs a log entry hash using the configured signature set and domain separation.
pub fn sign_entry(
    signature_keys: &SignatureKeys<'_>,
    entry_hash: &[u8; 32],
    domain: &str,
) -> Result<SignatureSet> {
    sign_with_domain(domain, signature_keys, entry_hash)
}

/// Verifies a log entry signature set against its entry hash.
pub fn verify_entry_signature(
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signatures: &SignatureSet,
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
    domain: &str,
) -> Result<()> {
    verify_with_domain(domain, verifying_key, entry_hash, signatures, mode, pq_public_key)
}
