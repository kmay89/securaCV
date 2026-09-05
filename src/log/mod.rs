use anyhow::Result;
use ed25519_dalek::VerifyingKey;
use sha2::{Digest, Sha256};

use crate::crypto::signatures::{
    sign_with_domain, verify_with_domain, PqPublicKey, SignatureKeys, SignatureMode, SignatureSet,
};

pub mod high_water_mark;

/// Hashes a log entry payload with the previous chain hash.
pub fn hash_entry(prev_hash: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_hash);
    hasher.update(payload);
    hasher.finalize().into()
}

/// Version tag of the cutoff-bound checkpoint message (see [`checkpoint_message`]).
pub const CHECKPOINT_CUTOFF_TAG: &[u8] = b"securacv:pwk:sealed-log-checkpoint-cutoff:v3";

/// The message a checkpoint signs once its cutoff is bound to it: a hash over a
/// version tag, the chain head and `cutoff_event_id`, then signed under
/// `DOMAIN_CHECKPOINT` like any entry hash. Earlier checkpoints signed the bare
/// head, which left `cutoff_event_id` an unsigned column — and post-prune that
/// column is what the high-water-mark check reconciles against, so a no-key
/// actor could wipe the table, replay an old signed checkpoint with an inflated
/// cutoff, and pass as a full-retention prune. Verifiers accept both messages
/// and report which one held (`verify::CheckpointBinding`).
pub fn checkpoint_message(chain_head_hash: &[u8; 32], cutoff_event_id: i64) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(CHECKPOINT_CUTOFF_TAG);
    hasher.update(chain_head_hash);
    hasher.update((cutoff_event_id as u64).to_le_bytes());
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
    verify_with_domain(
        domain,
        verifying_key,
        entry_hash,
        signatures,
        mode,
        pq_public_key,
    )
}
