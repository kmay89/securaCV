use anyhow::{anyhow, Result};
use ed25519_dalek::VerifyingKey;
use rusqlite::{Connection, OptionalExtension, Row};
use serde::Serialize;

use crate::crypto::signatures::{
    verify_rotation_attestation, verify_rotation_authorization, PqPublicKey, SignatureMode,
    SignatureSet, DOMAIN_BREAK_GLASS_RECEIPT, DOMAIN_CHECKPOINT, DOMAIN_EXPORT_RECEIPT,
    DOMAIN_SEALED_LOG_ENTRY,
};
use crate::{
    approvals_commitment, hash_entry, verify_approval, verify_entry_signature, Approval,
    BreakGlassOutcome, BreakGlassReceipt, QuorumPolicy, SealedLogRecord,
};

/// Which tamper-evident structure a verification failure occurred in.
#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum FailedLedger {
    SealedEvents,
    BreakGlassReceipts,
    ExportReceipts,
    Checkpoint,
    KeyLineage,
}

/// What kind of check failed. Drives the plain-language diagnosis the CLIs
/// and the offline viewer print alongside the raw error.
#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum FailureKind {
    PrevHashMismatch,
    EntryHashMismatch,
    SignatureMismatch,
    KeyRotationInvalid,
    CheckpointInvalid,
    ApprovalsCommitmentMismatch,
    PolicyViolation,
    UntrustedSigner,
}

/// Structured verification failure. `Display` reproduces the exact legacy
/// error string (`detail`), so `VerifyReport.error` and existing consumers
/// see unchanged text while new consumers can read the typed location/kind.
#[derive(Debug, Clone, Serialize)]
pub struct VerifyFailure {
    pub ledger: FailedLedger,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub entry_id: Option<i64>,
    pub kind: FailureKind,
    pub detail: String,
}

impl std::fmt::Display for VerifyFailure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.detail)
    }
}

impl std::error::Error for VerifyFailure {}

fn fail(
    ledger: FailedLedger,
    entry_id: Option<i64>,
    kind: FailureKind,
    detail: String,
) -> anyhow::Error {
    anyhow::Error::new(VerifyFailure {
        ledger,
        entry_id,
        kind,
        detail,
    })
}

#[derive(Debug, Clone)]
pub struct CheckpointInfo {
    pub chain_head_hash: Option<[u8; 32]>,
    pub signatures: Option<SignatureSet>,
    pub cutoff_event_id: Option<i64>,
    /// The device key that signed this checkpoint (NULL on pre-rotation databases).
    pub signer_public_key: Option<[u8; 32]>,
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
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
) -> Result<()> {
    match (
        checkpoint.chain_head_hash,
        checkpoint.signatures.as_ref(),
        checkpoint.cutoff_event_id,
    ) {
        (None, None, None) => Ok(()),
        (Some(head), Some(sig), Some(_)) => verify_entry_signature(
            verifying_key,
            &head,
            sig,
            mode,
            pq_public_key,
            DOMAIN_CHECKPOINT,
        )
        .map_err(|e| {
            fail(
                FailedLedger::Checkpoint,
                None,
                FailureKind::CheckpointInvalid,
                format!("checkpoint signature verification failed: {}", e),
            )
        }),
        _ => Err(fail(
            FailedLedger::Checkpoint,
            None,
            FailureKind::CheckpointInvalid,
            "checkpoint is partially populated; integrity verification cannot proceed".to_string(),
        )),
    }
}

pub fn latest_checkpoint(conn: &Connection) -> Result<CheckpointInfo> {
    let mut stmt = conn.prepare(
        "SELECT chain_head_hash, signature, pq_signature, pq_scheme, cutoff_event_id, signer_public_key FROM checkpoints ORDER BY id DESC LIMIT 1",
    )?;
    let mut rows = stmt.query([])?;
    if let Some(row) = rows.next()? {
        let head = blob32(row, 0)?;
        let signature: Vec<u8> = row.get(1)?;
        let pq_signature: Option<Vec<u8>> = row.get(2)?;
        let pq_scheme: Option<String> = row.get(3)?;
        let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        let cutoff_event_id: i64 = row.get(4)?;
        let signer_public_key: Option<Vec<u8>> = row.get(5)?;
        let signer_public_key = match signer_public_key {
            Some(bytes) if bytes.len() == 32 => {
                let mut out = [0u8; 32];
                out.copy_from_slice(&bytes);
                Some(out)
            }
            Some(_) => return Err(anyhow!("corrupt checkpoint signer_public_key length")),
            None => None,
        };
        Ok(CheckpointInfo {
            chain_head_hash: Some(head),
            signatures: Some(signature_set),
            cutoff_event_id: Some(cutoff_event_id),
            signer_public_key,
        })
    } else {
        Ok(CheckpointInfo {
            chain_head_hash: None,
            signatures: None,
            cutoff_event_id: None,
            signer_public_key: None,
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
        let policy: QuorumPolicy = serde_json::from_str(&policy_json)?;
        policy.validate()?;
        Ok(Some(policy))
    } else {
        Ok(None)
    }
}

/// Every quorum policy that the signed, hash-chained policy-change history
/// records as having been in force — each record's `prev_policy` and
/// `new_policy`. This is the ground truth for "was this receipt's declared
/// policy era ever real?": a receipt whose `policy_commitment` matches none of
/// these (and is not the current policy) cannot have been authorized under any
/// policy this device actually adopted. On a legacy database with no history
/// table or no rows this returns an empty vec, and callers fall back to the
/// pre-history lenient behavior for backward compatibility.
pub fn load_policies_ever_in_force(conn: &Connection) -> Result<Vec<QuorumPolicy>> {
    // The table may not exist on a pre-feature database.
    let exists: bool = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='policy_change_history' LIMIT 1",
            [],
            |_| Ok(true),
        )
        .optional()?
        .unwrap_or(false);
    if !exists {
        return Ok(Vec::new());
    }
    let mut stmt =
        conn.prepare("SELECT payload_json FROM policy_change_history ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    let mut out: Vec<QuorumPolicy> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    while let Some(row) = rows.next()? {
        let payload: String = row.get(0)?;
        let record: crate::PolicyChangeRecord = serde_json::from_str(&payload)?;
        for policy in [record.prev_policy, Some(record.new_policy)].into_iter().flatten() {
            if policy.validate().is_ok() && seen.insert(policy.commitment()) {
                out.push(policy);
            }
        }
    }
    Ok(out)
}

/// Resolve the quorum policy a receipt should be re-derived against, using the
/// receipt's recorded `policy_commitment`:
/// - zero (a pre-field receipt) or the current policy's commitment → the
///   current policy;
/// - otherwise a historical era: the policy from `history` whose commitment
///   matches. When `history` is non-empty and none matches, the receipt claims
///   an era that never existed — `Err` (a forged/unverifiable receipt).
/// - `Ok(None)` means "unresolved on a legacy database (no history recorded)":
///   the caller preserves the pre-history behavior of not re-deriving, since it
///   cannot prove the era either way.
fn resolve_receipt_policy<'a>(
    current_policy: &'a QuorumPolicy,
    history: &'a [QuorumPolicy],
    receipt: &BreakGlassReceipt,
) -> Result<Option<&'a QuorumPolicy>> {
    if receipt.policy_commitment == [0u8; 32]
        || receipt.policy_commitment == current_policy.commitment()
    {
        return Ok(Some(current_policy));
    }
    if let Some(p) = history
        .iter()
        .find(|p| p.commitment() == receipt.policy_commitment)
    {
        return Ok(Some(p));
    }
    if history.is_empty() {
        // Legacy database: no policy-change history exists, so an era mismatch
        // cannot be distinguished from a legitimate pre-feature rotation.
        // Preserve the historical lenient behavior (do not re-derive) rather
        // than raise a false tamper alarm on an old deployment.
        return Ok(None);
    }
    Err(anyhow!(
        "break-glass receipt policy_commitment {} matches neither the current policy nor any \
         recorded policy-change history entry — the policy era it claims cannot be verified",
        hex::encode(receipt.policy_commitment)
    ))
}

pub fn verify_events_with<F>(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    checkpoint_head: Option<[u8; 32]>,
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
    mut on_event: F,
) -> Result<u64>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM sealed_events ORDER BY id ASC",
    )?;

    let mut rows = stmt.query([])?;
    let mut expected_prev: [u8; 32] = checkpoint_head.unwrap_or([0u8; 32]);
    // The verifying key for the *start* of the walked range. It advances as the chain
    // crosses device-key rotation records, so the whole log stays verifiable across
    // identity changes (anchored to the genesis key the caller seeds here).
    let mut active_key = *verifying_key;
    let mut count = 0u64;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let payload: String = row.get(1)?;
        let prev_hash = blob32(row, 2)?;
        let entry_hash = blob32(row, 3)?;
        let signature: Vec<u8> = row.get(4)?;
        let pq_signature: Option<Vec<u8>> = row.get(5)?;
        let pq_scheme: Option<String> = row.get(6)?;
        let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;

        if prev_hash != expected_prev {
            return Err(fail(
                FailedLedger::SealedEvents,
                Some(id),
                FailureKind::PrevHashMismatch,
                format!(
                    "integrity check failed at id {}: prev_hash={}, expected_prev={}",
                    id,
                    hex::encode(prev_hash),
                    hex::encode(expected_prev)
                ),
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(fail(
                FailedLedger::SealedEvents,
                Some(id),
                FailureKind::EntryHashMismatch,
                format!(
                    "integrity check failed at id {}: computed_hash={}, stored_hash={}",
                    id,
                    hex::encode(computed),
                    hex::encode(entry_hash)
                ),
            ));
        }

        if verify_entry_signature(
            &active_key,
            &entry_hash,
            &signature_set,
            mode,
            pq_public_key,
            DOMAIN_SEALED_LOG_ENTRY,
        )
        .is_err()
        {
            return Err(fail(
                FailedLedger::SealedEvents,
                Some(id),
                FailureKind::SignatureMismatch,
                format!(
                    "integrity check failed at id {}: signature mismatch (stored={})",
                    id,
                    hex::encode(signature_set.ed25519_signature)
                ),
            ));
        }

        // Follow device-key rotations. A KeyRotation record is signed by the *retiring*
        // key (verified just above as `active_key`); after validating the incoming key's
        // possession attestation we advance the expected verifying key for later entries.
        if let Ok(SealedLogRecord::KeyRotation(rotation)) =
            SealedLogRecord::deserialize_compat(&payload)
        {
            if rotation.prev_public_key != active_key.to_bytes() {
                return Err(fail(
                    FailedLedger::SealedEvents,
                    Some(id),
                    FailureKind::KeyRotationInvalid,
                    format!(
                        "key rotation at id {}: prev_public_key does not match the active verifying key",
                        id
                    ),
                ));
            }
            let new_key = VerifyingKey::from_bytes(&rotation.new_public_key).map_err(|e| {
                fail(
                    FailedLedger::SealedEvents,
                    Some(id),
                    FailureKind::KeyRotationInvalid,
                    format!("key rotation at id {}: invalid new public key: {}", id, e),
                )
            })?;
            // Retiring key authorized the successor; the new key proves possession. The entry
            // signature (verified above under `active_key`) already proves the predecessor
            // signed this rotation into the chain, so the explicit authorization is required
            // only when present — legacy records written before the field existed carry an
            // empty authorization and are anchored solely by that entry signature.
            if !rotation.prev_key_authorization.is_empty() {
                verify_rotation_authorization(
                    &active_key,
                    &rotation.prev_public_key,
                    &rotation.new_public_key,
                    &rotation.prev_key_authorization,
                )
                .map_err(|e| {
                    fail(
                        FailedLedger::SealedEvents,
                        Some(id),
                        FailureKind::KeyRotationInvalid,
                        format!("key rotation at id {}: {}", id, e),
                    )
                })?;
            }
            verify_rotation_attestation(
                &new_key,
                &rotation.prev_public_key,
                &rotation.new_public_key,
                &rotation.new_key_attestation,
            )
            .map_err(|e| {
                fail(
                    FailedLedger::SealedEvents,
                    Some(id),
                    FailureKind::KeyRotationInvalid,
                    format!("key rotation at id {}: {}", id, e),
                )
            })?;
            active_key = new_key;
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
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
    mut on_receipt: F,
) -> Result<BreakGlassReceiptCounts>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, approvals_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM break_glass_receipts ORDER BY id ASC",
    )?;

    // Ground truth for a receipt's declared policy era: every policy the
    // signed policy-change history records as having been in force. Loaded
    // once for the whole ledger walk.
    let history = load_policies_ever_in_force(conn)?;

    let mut rows = stmt.query([])?;
    let mut expected_prev = [0u8; 32];
    let mut count = 0u64;
    let mut granted = 0u64;
    let mut denied = 0u64;

    while let Some(row) = rows.next()? {
        let policy = policy.ok_or_else(|| {
            fail(
                FailedLedger::BreakGlassReceipts,
                None,
                FailureKind::PolicyViolation,
                "break-glass quorum policy is not configured".to_string(),
            )
        })?;
        let id: i64 = row.get(0)?;
        let _created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;
        let approvals_json: String = row.get(3)?;
        let prev_hash = blob32(row, 4)?;
        let entry_hash = blob32(row, 5)?;
        let signature: Vec<u8> = row.get(6)?;
        let pq_signature: Option<Vec<u8>> = row.get(7)?;
        let pq_scheme: Option<String> = row.get(8)?;
        let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;

        if prev_hash != expected_prev {
            return Err(fail(
                FailedLedger::BreakGlassReceipts,
                Some(id),
                FailureKind::PrevHashMismatch,
                format!(
                    "integrity check failed at receipt id {}: prev_hash={}, expected_prev={}",
                    id,
                    hex::encode(prev_hash),
                    hex::encode(expected_prev)
                ),
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(fail(
                FailedLedger::BreakGlassReceipts,
                Some(id),
                FailureKind::EntryHashMismatch,
                format!(
                    "integrity check failed at receipt id {}: computed_hash={}, stored_hash={}",
                    id,
                    hex::encode(computed),
                    hex::encode(entry_hash)
                ),
            ));
        }

        if verify_entry_signature(
            verifying_key,
            &entry_hash,
            &signature_set,
            mode,
            pq_public_key,
            DOMAIN_BREAK_GLASS_RECEIPT,
        )
        .is_err()
        {
            return Err(fail(
                FailedLedger::BreakGlassReceipts,
                Some(id),
                FailureKind::SignatureMismatch,
                format!(
                    "integrity check failed at receipt id {}: signature mismatch (stored={})",
                    id,
                    hex::encode(signature_set.ed25519_signature)
                ),
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
            return Err(fail(
                FailedLedger::BreakGlassReceipts,
                Some(id),
                FailureKind::ApprovalsCommitmentMismatch,
                format!(
                    "integrity check failed at receipt id {}: stored_commitment={}, expected_commitment={}",
                    id,
                    hex::encode(receipt.approvals_commitment),
                    hex::encode(commitment)
                ),
            ));
        }
        verify_receipt_quorum(policy, &history, &receipt, &approvals).map_err(|e| {
            fail(
                FailedLedger::BreakGlassReceipts,
                Some(id),
                FailureKind::PolicyViolation,
                format!("{}", e),
            )
        })?;

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
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
    mut on_receipt: F,
) -> Result<u64>
where
    F: FnMut(i64, [u8; 32]),
{
    let mut stmt = conn.prepare(
        "SELECT id, created_at, payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM export_receipts ORDER BY id ASC",
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
        let signature: Vec<u8> = row.get(5)?;
        let pq_signature: Option<Vec<u8>> = row.get(6)?;
        let pq_scheme: Option<String> = row.get(7)?;
        let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;

        if prev_hash != expected_prev {
            return Err(fail(
                FailedLedger::ExportReceipts,
                Some(id),
                FailureKind::PrevHashMismatch,
                format!(
                    "integrity check failed at receipt id {}: prev_hash={}, expected_prev={}",
                    id,
                    hex::encode(prev_hash),
                    hex::encode(expected_prev)
                ),
            ));
        }

        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != entry_hash {
            return Err(fail(
                FailedLedger::ExportReceipts,
                Some(id),
                FailureKind::EntryHashMismatch,
                format!(
                    "integrity check failed at receipt id {}: computed_hash={}, stored_hash={}",
                    id,
                    hex::encode(computed),
                    hex::encode(entry_hash)
                ),
            ));
        }

        if verify_entry_signature(
            verifying_key,
            &entry_hash,
            &signature_set,
            mode,
            pq_public_key,
            DOMAIN_EXPORT_RECEIPT,
        )
        .is_err()
        {
            return Err(fail(
                FailedLedger::ExportReceipts,
                Some(id),
                FailureKind::SignatureMismatch,
                format!(
                    "integrity check failed at receipt id {}: signature mismatch (stored={})",
                    id,
                    hex::encode(signature_set.ed25519_signature)
                ),
            ));
        }

        on_receipt(id, entry_hash);

        expected_prev = entry_hash;
        count += 1;
    }

    Ok(count)
}

/// Timeline/coverage audit over an already hash-valid chain.
///
/// Hash-chain verification proves interior integrity but cannot see *tail
/// truncation* (deleting the newest N rows leaves a valid chain) or clock
/// games. This pass cross-checks the record timeline and returns warnings
/// (never errors — the chain is still cryptographically valid):
///
/// 1. `created_at` regressions (softened when a ClockSkew failure record
///    exists in the same bucket — then the daemon witnessed the jump itself);
/// 2. checkpoint `created_at` regressions or far-future stamps;
/// 3. missing heartbeat buckets inside lifecycle segments where the daemon
///    was demonstrably running (skipped entirely on chains that predate
///    heartbeat records);
/// 4. a stale tail: last lifecycle record is `start` but the newest record is
///    older than two buckets — possible tail truncation, crash, or a daemon
///    stopped without a shutdown record.
///
/// Warning text carries coarse bucket starts only (safe for API/MQTT export).
pub fn audit_chain_timeline(conn: &Connection, now_bucket_start: u64) -> Result<Vec<String>> {
    const BUCKET_S: u64 = 600;
    let mut warnings = Vec::new();

    let mut stmt =
        conn.prepare("SELECT id, created_at, payload_json FROM sealed_events ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;

    let mut prev_created_at: Option<(i64, i64)> = None; // (id, created_at)
    let mut clock_skew_buckets: Vec<i64> = Vec::new();
    let mut created_at_regressions: Vec<(i64, i64, i64)> = Vec::new(); // (id, created_at, prev)
    let mut heartbeat_buckets: Vec<u64> = Vec::new();
    // Lifecycle segments: (start_bucket, last_record_bucket, closed_cleanly)
    let mut segments: Vec<(u64, u64, bool)> = Vec::new();
    let mut last_record_bucket: Option<u64> = None;
    let mut last_lifecycle_is_start = false;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let payload: String = row.get(2)?;

        if let Some((_, prev)) = prev_created_at {
            if created_at < prev {
                created_at_regressions.push((id, created_at, prev));
            }
        }
        prev_created_at = Some((id, created_at));

        let bucket = u64::try_from(created_at).unwrap_or(0);
        last_record_bucket = Some(bucket);
        if let Some((_, seg_last, closed)) = segments.last_mut() {
            if !*closed {
                *seg_last = bucket;
            }
        }

        // Unknown/corrupt payloads were already accepted by hash verification;
        // the timeline pass only inspects the records it can parse.
        match SealedLogRecord::deserialize_compat(&payload) {
            Ok(SealedLogRecord::Heartbeat(h)) => {
                heartbeat_buckets.push(h.time_bucket.start_epoch_s);
            }
            Ok(SealedLogRecord::Lifecycle(l)) => match l.phase {
                crate::LifecyclePhase::Start => {
                    segments.push((bucket, bucket, false));
                    last_lifecycle_is_start = true;
                }
                crate::LifecyclePhase::ShutdownClean => {
                    if let Some((_, seg_last, closed)) = segments.last_mut() {
                        *seg_last = bucket;
                        *closed = true;
                    }
                    last_lifecycle_is_start = false;
                }
            },
            Ok(SealedLogRecord::Failure(f)) if f.failure_type == crate::FailureType::ClockSkew => {
                clock_skew_buckets.push(created_at);
            }
            _ => {}
        }
    }

    for (id, created_at, prev) in created_at_regressions {
        if clock_skew_buckets.contains(&created_at) || clock_skew_buckets.contains(&prev) {
            warnings.push(format!(
                "note: created_at regression at id {} (bucket {} after {}); a ClockSkew \
                 failure record covers this jump",
                id, created_at, prev
            ));
        } else {
            warnings.push(format!(
                "created_at regression at id {} (bucket {} after {}) with no ClockSkew \
                 record: possible clock tampering or row manipulation",
                id, created_at, prev
            ));
        }
    }

    // Checkpoint timestamp sanity (created_at here is wall seconds, not a bucket).
    let mut stmt = conn.prepare("SELECT id, created_at FROM checkpoints ORDER BY id ASC")?;
    let mut rows = stmt.query([])?;
    let mut prev_checkpoint_at: Option<i64> = None;
    let future_limit = now_bucket_start.saturating_add(2 * BUCKET_S) as i64;
    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        if let Some(prev) = prev_checkpoint_at {
            if created_at < prev {
                warnings.push(format!(
                    "checkpoint {} created_at regressed (possible back-dated checkpoint)",
                    id
                ));
            }
        }
        if created_at > future_limit {
            warnings.push(format!(
                "checkpoint {} created_at is in the future (possible forged checkpoint)",
                id
            ));
        }
        prev_checkpoint_at = Some(created_at);
    }

    // Heartbeat cadence inside lifecycle segments. Skip on chains that predate
    // heartbeats (no false alarms on old databases).
    if !heartbeat_buckets.is_empty() {
        for (seg_start, seg_end, _closed) in &segments {
            let mut missing: Vec<u64> = Vec::new();
            let mut bucket = *seg_start;
            while bucket <= *seg_end {
                if !heartbeat_buckets.contains(&bucket) {
                    missing.push(bucket);
                }
                bucket += BUCKET_S;
            }
            if !missing.is_empty() {
                warnings.push(format!(
                    "no heartbeat for {} bucket(s) in {}..{} while the daemon was running: \
                     possible record deletion",
                    missing.len(),
                    seg_start,
                    seg_end
                ));
            }
        }
    }

    // Stale tail: daemon claims to be running but the chain stopped growing.
    if last_lifecycle_is_start {
        if let Some(last_bucket) = last_record_bucket {
            if last_bucket + 2 * BUCKET_S < now_bucket_start {
                warnings.push(format!(
                    "chain tail is stale: last record bucket {} but now {} and no clean \
                     shutdown record — possible tail truncation, crash, or daemon stopped \
                     without shutdown",
                    last_bucket, now_bucket_start
                ));
            }
        }
    }

    Ok(warnings)
}

/// Coarse classification of a timeline-audit warning string, used to attach
/// plain-language hints in the CLIs and the offline viewer. Classification is
/// substring-based against the exact `format!` literals in
/// [`audit_chain_timeline`] (unit-tested below) so the warning text itself —
/// which flows out over API/MQTT — never has to change. A miss degrades to
/// [`WarningKind::Other`] (no hint), never a wrong verdict.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WarningKind {
    /// created_at regression covered by a ClockSkew failure record.
    ClockSkewNoted,
    /// created_at regression with no covering ClockSkew record.
    CreatedAtRegression,
    /// Checkpoint created_at regressed or sits in the future.
    CheckpointAnomaly,
    /// Heartbeat buckets missing inside a running segment.
    MissingHeartbeats,
    /// Chain tail stale while the daemon claimed to be running.
    StaleTail,
    /// Timeline audit itself could not run.
    AuditUnavailable,
    Other,
}

pub fn classify_warning(warning: &str) -> WarningKind {
    if warning.starts_with("note: created_at regression") {
        WarningKind::ClockSkewNoted
    } else if warning.starts_with("created_at regression") {
        WarningKind::CreatedAtRegression
    } else if warning.starts_with("checkpoint") {
        WarningKind::CheckpointAnomaly
    } else if warning.starts_with("no heartbeat") {
        WarningKind::MissingHeartbeats
    } else if warning.starts_with("chain tail is stale") {
        WarningKind::StaleTail
    } else if warning.starts_with("timeline audit could not run") {
        WarningKind::AuditUnavailable
    } else {
        WarningKind::Other
    }
}

/// Re-derive a break-glass receipt's quorum against the policy that was
/// actually in force when it was authorized (Invariant V). Never trusts the
/// receipt's recorded `outcome`: a device-key holder can forge a `Granted`
/// receipt, but cannot manufacture the trustee signatures the quorum requires.
///
/// A receipt records the `policy_commitment` of its authorizing policy era.
/// Earlier this check simply *skipped* re-derivation whenever that commitment
/// was not the current policy's — which let a forged receipt carrying an
/// arbitrary non-current `policy_commitment` slip through the audit entirely.
/// It now RESOLVES the era against the signed, hash-chained policy-change
/// history (`load_policies_ever_in_force`) and re-derives against that
/// historical policy's own roster and threshold. A commitment that matches no
/// real era (with history present) is a forgery and fails. On a legacy
/// database with no history the pre-history lenient behavior is preserved
/// (the era cannot be proven either way), so old deployments do not
/// false-alarm.
pub(crate) fn verify_receipt_quorum(
    current_policy: &QuorumPolicy,
    history: &[QuorumPolicy],
    receipt: &BreakGlassReceipt,
    approvals: &[Approval],
) -> Result<()> {
    let Some(policy) = resolve_receipt_policy(current_policy, history, receipt)? else {
        // Legacy, unresolvable era: keep the pre-history behavior (do not
        // re-derive). The chain hash and device signature the caller already
        // verified remain the tamper evidence for the receipt's bytes.
        return Ok(());
    };

    let mut distinct_keys = std::collections::HashSet::new();
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
        // Distinct message for an unusable key; the signature check itself is
        // domain-separated (verify_approval rejects a bare-hash or
        // cross-context signature), matching how authorize now signs.
        VerifyingKey::from_bytes(&trustee.public_key)
            .map_err(|_| anyhow!("invalid public key for trustee {}", trustee.id.0))?;
        if !verify_approval(
            &trustee.public_key,
            &approval.request_hash,
            &approval.signature,
        ) {
            return Err(anyhow!("invalid signature for trustee {}", trustee.id.0));
        }
        distinct_keys.insert(trustee.public_key);
    }
    // A Granted receipt MUST carry at least `policy.n` distinct valid trustee
    // approvals. A device-key holder who forges `Granted` over an empty or
    // under-quorum approval set is rejected here — the recorded outcome is not,
    // by itself, evidence that the quorum was met.
    if matches!(receipt.outcome, BreakGlassOutcome::Granted)
        && distinct_keys.len() < policy.n as usize
    {
        return Err(anyhow!(
            "break-glass receipt claims Granted but only {} of the required {} distinct trustee approvals are valid",
            distinct_keys.len(),
            policy.n
        ));
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::signatures::SignatureKeys;
    use crate::sign_entry;
    use ed25519_dalek::SigningKey;

    #[test]
    fn classify_warning_matches_audit_literals() {
        // These mirror the exact `format!` literals in `audit_chain_timeline`
        // (and the runner's audit-unavailable wrapper). If a literal changes,
        // this test fails before the hint mapping silently goes dead.
        assert_eq!(
            classify_warning(
                "note: created_at regression at id 4 (bucket 100 after 200); a ClockSkew \
                 failure record covers this jump"
            ),
            WarningKind::ClockSkewNoted
        );
        assert_eq!(
            classify_warning(
                "created_at regression at id 4 (bucket 100 after 200) with no ClockSkew \
                 record: possible clock tampering or row manipulation"
            ),
            WarningKind::CreatedAtRegression
        );
        assert_eq!(
            classify_warning("checkpoint 2 created_at regressed (possible back-dated checkpoint)"),
            WarningKind::CheckpointAnomaly
        );
        assert_eq!(
            classify_warning(
                "checkpoint 2 created_at is in the future (possible forged checkpoint)"
            ),
            WarningKind::CheckpointAnomaly
        );
        assert_eq!(
            classify_warning(
                "no heartbeat for 3 bucket(s) in 600..2400 while the daemon was running: \
                 possible record deletion"
            ),
            WarningKind::MissingHeartbeats
        );
        assert_eq!(
            classify_warning(
                "chain tail is stale: last record bucket 600 but now 3000 and no clean \
                 shutdown record — possible tail truncation, crash, or daemon stopped \
                 without shutdown"
            ),
            WarningKind::StaleTail
        );
        assert_eq!(
            classify_warning("timeline audit could not run: boom"),
            WarningKind::AuditUnavailable
        );
        assert_eq!(classify_warning("something else"), WarningKind::Other);
    }

    #[test]
    fn verify_checkpoint_signature_accepts_genesis() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let checkpoint = CheckpointInfo {
            chain_head_hash: None,
            signatures: None,
            cutoff_event_id: None,
            signer_public_key: None,
        };

        verify_checkpoint_signature(&verifying_key, &checkpoint, SignatureMode::Compat, None)?;
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_accepts_valid_signature() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let head = [1u8; 32];
        let signature = sign_entry(&SignatureKeys::new(&signing_key), &head, DOMAIN_CHECKPOINT)?;
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some(head),
            signatures: Some(signature),
            cutoff_event_id: Some(42),
            signer_public_key: None,
        };

        verify_checkpoint_signature(&verifying_key, &checkpoint, SignatureMode::Compat, None)?;
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_rejects_invalid_signature() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[11u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let head = [2u8; 32];
        let mut signature =
            sign_entry(&SignatureKeys::new(&signing_key), &head, DOMAIN_CHECKPOINT)?;
        signature.ed25519_signature[0] ^= 0xff;
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some(head),
            signatures: Some(signature),
            cutoff_event_id: Some(7),
            signer_public_key: None,
        };

        let result =
            verify_checkpoint_signature(&verifying_key, &checkpoint, SignatureMode::Compat, None);
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn verify_checkpoint_signature_rejects_partial_checkpoint() -> Result<()> {
        let signing_key = SigningKey::from_bytes(&[13u8; 32]);
        let verifying_key = signing_key.verifying_key();
        let checkpoint = CheckpointInfo {
            chain_head_hash: Some([3u8; 32]),
            signatures: None,
            cutoff_event_id: Some(1),
            signer_public_key: None,
        };

        let result =
            verify_checkpoint_signature(&verifying_key, &checkpoint, SignatureMode::Compat, None);
        assert!(result.is_err());
        Ok(())
    }

    // ─── H1: the audit verifier re-derives the quorum, never trusting the
    // recorded outcome. Covers both `verify_break_glass_receipts_with` and the
    // CLI `cmd_receipts`, which share `verify_approvals_against_policy`. ─────

    #[test]
    fn granted_receipt_below_quorum_is_rejected() {
        use crate::break_glass::{TrusteeEntry, TrusteeId, UnlockRequest};
        use crate::TimeBucket;

        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let key = SigningKey::from_bytes(&[1u8; 32]);
        let request = UnlockRequest::new("vault:v", [1u8; 32], "incident", bucket).unwrap();
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();

        // Forged: outcome=Granted with an EMPTY approval set (the device-key
        // holder's forgery). Must be rejected — the recorded outcome is not
        // self-authenticating.
        // Forged: outcome=Granted with an EMPTY approval set (the device-key
        // holder's forgery). Commitment matches the current policy so the
        // re-derivation runs; it must be rejected — the recorded outcome is not
        // self-authenticating.
        let forged = BreakGlassReceipt {
            vault_envelope_id: "vault:v".to_string(),
            request_hash: request.request_hash(),
            ruleset_hash: [1u8; 32],
            time_bucket: bucket,
            trustees_used: vec![],
            approvals_commitment: approvals_commitment(&[]),
            policy_commitment: policy.commitment(),
            outcome: BreakGlassOutcome::Granted,
        };
        assert!(verify_receipt_quorum(&policy, &[], &forged, &[]).is_err());

        // Legit: the same Granted receipt WITH a real trustee approval passes.
        let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &key);
        assert!(
            verify_receipt_quorum(&policy, &[], &forged, std::slice::from_ref(&approval)).is_ok()
        );

        // A Denied receipt carries no quorum floor — empty approvals are fine.
        let denied = BreakGlassReceipt {
            outcome: BreakGlassOutcome::Denied {
                reason: "insufficient".to_string(),
            },
            ..forged.clone()
        };
        assert!(verify_receipt_quorum(&policy, &[], &denied, &[]).is_ok());
    }

    // ─── the era guard is not a bypass: a receipt claiming a policy era that
    // never existed is a forgery when the policy-change history is present. ──

    #[test]
    fn granted_receipt_with_fabricated_policy_era_is_rejected_when_history_present() {
        use crate::break_glass::{TrusteeEntry, TrusteeId, UnlockRequest};
        use crate::TimeBucket;

        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let key = SigningKey::from_bytes(&[1u8; 32]);
        let request = UnlockRequest::new("vault:v", [1u8; 32], "incident", bucket).unwrap();
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();

        // A device-key holder forges Granted with ZERO approvals and stamps a
        // policy_commitment that is neither the current policy nor any real
        // era — the old code skipped re-derivation on any era mismatch, so
        // this passed. With a non-empty history recording the real eras, an
        // era that appears nowhere is a forgery and must fail.
        let forged = BreakGlassReceipt {
            vault_envelope_id: "vault:v".to_string(),
            request_hash: request.request_hash(),
            ruleset_hash: [1u8; 32],
            time_bucket: bucket,
            trustees_used: vec![],
            approvals_commitment: approvals_commitment(&[]),
            policy_commitment: [0x99u8; 32],
            outcome: BreakGlassOutcome::Granted,
        };
        // history contains only the real current policy's era.
        let history = vec![policy.clone()];
        assert!(
            verify_receipt_quorum(&policy, &history, &forged, &[]).is_err(),
            "a fabricated policy era must be rejected when history is present"
        );
        // On a legacy DB (empty history) the era cannot be disproven, so the
        // pre-history lenient behavior is preserved (not flagged).
        assert!(verify_receipt_quorum(&policy, &[], &forged, &[]).is_ok());
    }

    // ─── R1: historical receipts audit against THEIR policy era, not the
    // mutable current policy — a legitimate rotation must not false-positive. ─

    #[test]
    fn granted_receipt_from_earlier_policy_era_is_not_flagged() {
        use crate::break_glass::{BreakGlass, TrusteeEntry, TrusteeId, UnlockRequest};
        use crate::TimeBucket;

        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let request = UnlockRequest::new("vault:v", [1u8; 32], "incident", bucket).unwrap();

        // Old policy: 1-of-1 (alice). A receipt authorized under it.
        let old_policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &alice);
        let (_r, receipt) = BreakGlass::authorize(
            &old_policy,
            &request,
            std::slice::from_ref(&approval),
            bucket,
        );
        assert!(matches!(receipt.outcome, BreakGlassOutcome::Granted));

        // The policy is later rotated to 2-of-2 (alice + bob). Auditing the old
        // 1-of-1 receipt against the NEW policy must NOT flag it: its commitment
        // marks a different era, so the quorum re-derivation is skipped and the
        // receipt is not treated as a forgery.
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let new_policy = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: alice.verifying_key().to_bytes(),
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: bob.verifying_key().to_bytes(),
                },
            ],
        )
        .unwrap();
        assert_ne!(new_policy.commitment(), old_policy.commitment());
        // With the old era recorded in history, auditing the old receipt
        // against the NEW current policy resolves it to old_policy and fully
        // re-derives (passing on its real approval) — no false alarm, but no
        // free pass either.
        let history = vec![old_policy.clone(), new_policy.clone()];
        assert!(verify_receipt_quorum(
            &new_policy,
            &history,
            &receipt,
            std::slice::from_ref(&approval)
        )
        .is_ok());
        // And a forged old-era Granted with NO approvals is now caught, where
        // the pre-history code would have waved it through.
        let forged_old = BreakGlassReceipt {
            outcome: BreakGlassOutcome::Granted,
            ..receipt.clone()
        };
        assert!(verify_receipt_quorum(&new_policy, &history, &forged_old, &[]).is_err());

        // Under its OWN (old) policy it still fully re-derives and passes.
        assert!(verify_receipt_quorum(
            &old_policy,
            &[],
            &receipt,
            std::slice::from_ref(&approval)
        )
        .is_ok());
    }
}
