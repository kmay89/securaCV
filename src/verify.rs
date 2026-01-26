use anyhow::{anyhow, Result};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use rusqlite::{Connection, Row};

use crate::{
    approvals_commitment, hash_entry, verify_entry_signature, Approval, BreakGlassOutcome,
    BreakGlassReceipt, QuorumPolicy,
};

#[derive(Debug, Clone, Copy)]
pub struct CheckpointInfo {
    pub chain_head_hash: Option<[u8; 32]>,
    pub signature: Option<[u8; 64]>,
    pub cutoff_event_id: Option<i64>,
}

#[derive(Debug, Clone)]
pub struct AlarmEntry {
    pub created_at: i64,
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Copy)]
pub struct BreakGlassReceiptCounts {
    pub total: u64,
    pub granted: u64,
    pub denied: u64,
}

pub fn verify_checkpoint_signature(
    verifying_key: &VerifyingKey,
    checkpoint: &CheckpointInfo,
) -> Result<()> {
    match (
        checkpoint.chain_head_hash,
        checkpoint.signature,
        checkpoint.cutoff_event_id,
    ) {
        (None, None, None) => Ok(()),
        (Some(head), Some(sig), Some(_)) => verify_entry_signature(verifying_key, &head, &sig)
            .map_err(|e| anyhow!("checkpoint signature verification failed: {}", e)),
        _ => Err(anyhow!(
            "checkpoint is partially populated; integrity verification cannot proceed"
        )),
    }
}

pub fn latest_checkpoint(conn: &Connection) -> Result<CheckpointInfo> {
    let mut stmt = conn.prepare(
        "SELECT chain_head_hash, signature, cutoff_event_id FROM checkpoints ORDER BY id DESC LIMIT 1",
    )?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let head = blob32(row, 0)?;
        let sig = blob64(row, 1)?;
        let cutoff_event_id: i64 = row.get(2)?;
        Ok(CheckpointInfo {
            chain_head_hash: Some(head),
            signature: Some(sig),
            cutoff_event_id: Some(cutoff_event_id),
        })
    } else {
        Ok(CheckpointInfo {
            chain_head_hash: None,
            signature: None,
            cutoff_event_id: None,
        })
    }
}

pub fn count_alarms(conn: &Connection) -> Result<u64> {
    let mut stmt = conn.prepare("SELECT COUNT(*) FROM conformance_alarms")?;
    let count: u64 = stmt.query_row([], |row| row.get(0))?;
    Ok(count)
}

pub fn load_alarms(conn: &Connection) -> Result<Vec<AlarmEntry>> {
    let mut stmt =
        conn.prepare("SELECT created_at, code, message FROM conformance_alarms ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    let mut alarms = Vec::new();
    while let Some(row) = rows.next()? {
        alarms.push(AlarmEntry {
            created_at: row.get(0)?,
            code: row.get(1)?,
            message: row.get(2)?,
        });
    }
    Ok(alarms)
}

pub fn load_break_glass_policy(conn: &Connection) -> Result<Option<QuorumPolicy>> {
    let mut stmt = conn.prepare("SELECT policy_json FROM break_glass_policy WHERE id = 1")?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let policy_json: String = row.get(0)?;
        let stored: QuorumPolicy = serde_json::from_str(&policy_json)?;
        let policy = QuorumPolicy::new(stored.n, stored.trustees)?;
        Ok(Some(policy))
    } else {
        Ok(None)
    }
}

pub fn verify_events_with<F>(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    checkpoint_head: Option<[u8; 32]>,
    mut on_event: F,
) -> Result<u64>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, payload_json, prev_hash, entry_hash, signature FROM sealed_events ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev: [u8; 32] = checkpoint_head.unwrap_or([0u8; 32]);
    let mut count = 0u64;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let payload: String = row.get(1)?;
        let prev_hash = blob32(row, 2)?;
        let entry_hash = blob32(row, 3)?;
        let sig = blob64(row, 4)?;

        if prev_hash != expected_prev {
            return Err(anyhow!(
                "integrity check failed at id {}: prev_hash={}, expected_prev={}",
                id,
                hex::encode(prev_hash),
                hex::encode(expected_prev)
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(anyhow!(
                "integrity check failed at id {}: computed_hash={}, stored_hash={}",
                id,
                hex::encode(computed),
                hex::encode(entry_hash)
            ));
        }

        if verify_entry_signature(verifying_key, &entry_hash, &sig).is_err() {
            return Err(anyhow!(
                "integrity check failed at id {}: signature mismatch (stored={})",
                id,
                hex::encode(sig)
            ));
        }

        on_event(id, entry_hash);

        expected_prev = entry_hash;
        count += 1;
    }

    Ok(count)
}

pub fn verify_break_glass_receipts_with<F>(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    policy: Option<&QuorumPolicy>,
    mut on_receipt: F,
) -> Result<BreakGlassReceiptCounts>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, approvals_json, prev_hash, entry_hash, signature FROM break_glass_receipts ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev = [0u8; 32];
    let mut count = 0u64;
    let mut granted = 0u64;
    let mut denied = 0u64;

    while let Some(row) = rows.next()? {
        let policy =
            policy.ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?;
        let id: i64 = row.get(0)?;
        let _created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let approvals_json: String = row.get(3)?;
        let prev_hash = blob32(row, 4)?;
        let entry_hash = blob32(row, 5)?;
        let sig = blob64(row, 6)?;

        if prev_hash != expected_prev {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: prev_hash={}, expected_prev={}",
                id,
                hex::encode(prev_hash),
                hex::encode(expected_prev)
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: computed_hash={}, stored_hash={}",
                id,
                hex::encode(computed),
                hex::encode(entry_hash)
            ));
        }

        if verify_entry_signature(verifying_key, &entry_hash, &sig).is_err() {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: signature mismatch (stored={})",
                id,
                hex::encode(sig)
            ));
        }

        let receipt: BreakGlassReceipt = serde_json::from_str(&payload)?;
        match receipt.outcome {
            BreakGlassOutcome::Granted => granted += 1,
            BreakGlassOutcome::Denied { .. } => denied += 1,
        }
        let approvals: Vec<Approval> = serde_json::from_str(&approvals_json)?;
        let commitment = approvals_commitment(&approvals);
        if commitment != receipt.approvals_commitment {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: stored_commitment={}, expected_commitment={}",
                id,
                hex::encode(receipt.approvals_commitment),
                hex::encode(commitment)
            ));
        }
        verify_approvals_against_policy(policy, &receipt, &approvals)?;

        on_receipt(id, entry_hash);

        expected_prev = entry_hash;
        count += 1;
    }

    Ok(BreakGlassReceiptCounts {
        total: count,
        granted,
        denied,
    })
}

pub fn verify_export_receipts_with<F>(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    mut on_receipt: F,
) -> Result<u64>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, prev_hash, entry_hash, signature FROM export_receipts ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev = [0u8; 32];
    let mut count = 0u64;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let _created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let prev_hash = blob32(row, 3)?;
        let entry_hash = blob32(row, 4)?;
        let sig = blob64(row, 5)?;

        if prev_hash != expected_prev {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: prev_hash={}, expected_prev={}",
                id,
                hex::encode(prev_hash),
                hex::encode(expected_prev)
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: computed_hash={}, stored_hash={}",
                id,
                hex::encode(computed),
                hex::encode(entry_hash)
            ));
        }

        if verify_entry_signature(verifying_key, &entry_hash, &sig).is_err() {
            return Err(anyhow!(
                "integrity check failed at receipt id {}: signature mismatch (stored={})",
                id,
                hex::encode(sig)
            ));
        }

        on_receipt(id, entry_hash);

        expected_prev = entry_hash;
        count += 1;
    }

    Ok(count)
}

fn verify_approvals_against_policy(
    policy: &QuorumPolicy,
    receipt: &BreakGlassReceipt,
    approvals: &[Approval],
) -> Result<()> {
    for approval in approvals {
        if approval.request_hash != receipt.request_hash {
            return Err(anyhow!(
                "approval request_hash mismatch for trustee {}",
                approval.trustee.0
            ));
        }
        let trustee = policy
            .trustees
            .iter()
            .find(|t| t.id.0 == approval.trustee.0)
            .ok_or_else(|| anyhow!("unknown trustee approval: {}", approval.trustee.0))?;
        let verifying_key = VerifyingKey::from_bytes(&trustee.public_key)
            .map_err(|_| anyhow!("invalid public key for trustee {}", trustee.id.0))?;
        let signature = Signature::from_slice(&approval.signature)
            .map_err(|_| anyhow!("invalid signature bytes for trustee {}", trustee.id.0))?;
        verifying_key
            .verify(&approval.request_hash, &signature)
            .map_err(|_| anyhow!("invalid signature for trustee {}", trustee.id.0))?;
    }
    Ok(())
}

fn blob32(row: &Row<'_>, idx: usize) -> Result<[u8; 32]> {
    let bytes: Vec<u8> = row.get(idx)?;
    if bytes.len() != 32 {
        return Err(anyhow!("expected 32-byte blob at col {}", idx));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn blob64(row: &Row<'_>, idx: usize) -> Result<[u8; 64]> {
    let bytes: Vec<u8> = row.get(idx)?;
    if bytes.len() != 64 {
        return Err(anyhow!("expected 64-byte blob at col {}", idx));
    }
    let mut out = [0u8; 64];
    out.copy_from_slice(&bytes);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    #[test]
    fn verify_checkpoint_signature_accepts_genesis() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let checkpoint = CheckpointInfo {
            chain_head_hash: None,
            signature: None,
            cutoff_event_id: None,
        };

        verify_checkpoint_signature(&verifying_key, &checkpoint)?;
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_accepts_valid_signature() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let head = [1u8; 32];
        let signature = signing_key.sign(&head).to_bytes();
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some(head),
            signature: Some(signature),
            cutoff_event_id: Some(42),
        };

        verify_checkpoint_signature(&verifying_key, &checkpoint)?;
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_rejects_invalid_signature() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[11u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let head = [2u8; 32];
        let mut signature = signing_key.sign(&head).to_bytes();
        signature[0] ^= 0xff;
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some(head),
            signature: Some(signature),
            cutoff_event_id: Some(7),
        };

        let result = verify_checkpoint_signature(&verifying_key, &checkpoint);
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_rejects_partial_checkpoint() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[13u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some([3u8; 32]),
            signature: None,
            cutoff_event_id: Some(1),
        };

        let result = verify_checkpoint_signature(&verifying_key, &checkpoint);
        assert!(result.is_err());
        Ok(())
    }
}
