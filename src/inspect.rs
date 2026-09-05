//! Diagnostic inspectors for the device key lineage and retention checkpoints.
//!
//! Unlike the verifiers (which fail closed at the first problem), these walk
//! *everything* and report per-item status so an operator can see where a
//! lineage or checkpoint went wrong and what remains trustworthy. Surfaced by
//! `log_verify --lineage` / `log_verify --checkpoints`.

use anyhow::Result;
use rusqlite::Connection;
use serde::Serialize;

use crate::crypto::signatures::{
    verify_rotation_attestation, verify_rotation_authorization, PqPublicKey, SignatureMode,
    SignatureSet, DOMAIN_CHECKPOINT,
};
use crate::{
    key32, recover_legacy_rotation_authorization, verify_entry_signature, verifying_key_from_bytes,
    DeviceKeyEpoch,
};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case", tag = "status")]
pub enum EpochStatus {
    Valid,
    Invalid {
        reason: String,
    },
    /// Cannot be judged because an earlier epoch already failed — the chain of
    /// custody for the key is broken before this point.
    Unverifiable {
        depends_on_epoch: i64,
    },
}

#[derive(Debug, Clone, Serialize)]
pub struct EpochReport {
    pub epoch: i64,
    /// Hex Ed25519 verifying key this epoch rotated TO.
    pub public_key: String,
    /// Sealed-event id at/after which this key signs entries (0 for genesis).
    pub activated_at_event_id: i64,
    #[serde(flatten)]
    pub status: EpochStatus,
}

#[derive(Debug, Clone, Serialize)]
pub struct LineageReport {
    pub genesis_public_key: String,
    pub epochs: Vec<EpochReport>,
    pub lineage_valid: bool,
    /// The newest key that is still genesis-anchored (the last valid epoch).
    pub trusted_public_key: String,
}

/// Walk `device_key_history` from the genesis anchor, validating every epoch
/// like [`crate::reconstruct_device_key_lineage_from`] but *continuing past
/// failures*: the failing epoch carries its reason, later epochs are marked
/// unverifiable.
pub fn inspect_key_lineage(conn: &Connection, genesis: &[u8; 32]) -> Result<LineageReport> {
    let mut epochs = vec![EpochReport {
        epoch: 0,
        public_key: hex::encode(genesis),
        activated_at_event_id: 0,
        status: EpochStatus::Valid,
    }];
    let mut trusted = *genesis;

    let mut stmt = conn.prepare(
        "SELECT epoch, public_key, prev_public_key, activated_at_event_id, attestation, authorization \
         FROM device_key_history WHERE epoch >= 1 ORDER BY epoch ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut expected_epoch = 1i64;
    let mut current_bytes = *genesis;
    let mut broken_at: Option<i64> = None;

    while let Some(row) = rows.next()? {
        let epoch: i64 = row.get(0)?;
        let public_key: Vec<u8> = row.get(1)?;
        let prev_public_key: Option<Vec<u8>> = row.get(2)?;
        let activated_at_event_id: i64 = row.get(3)?;
        let attestation: Option<Vec<u8>> = row.get(4)?;
        let authorization: Option<Vec<u8>> = row.get(5)?;
        let key_hex = hex::encode(&public_key);

        if let Some(first_bad) = broken_at {
            epochs.push(EpochReport {
                epoch,
                public_key: key_hex,
                activated_at_event_id,
                status: EpochStatus::Unverifiable {
                    depends_on_epoch: first_bad,
                },
            });
            continue;
        }

        let failure = validate_epoch(
            conn,
            epoch,
            expected_epoch,
            &public_key,
            prev_public_key.as_deref().unwrap_or_default(),
            &current_bytes,
            attestation.as_deref(),
            authorization.as_deref(),
            activated_at_event_id,
        );
        match failure {
            Ok(new_bytes) => {
                epochs.push(EpochReport {
                    epoch,
                    public_key: key_hex,
                    activated_at_event_id,
                    status: EpochStatus::Valid,
                });
                current_bytes = new_bytes;
                trusted = new_bytes;
                expected_epoch = epoch + 1;
            }
            Err(reason) => {
                epochs.push(EpochReport {
                    epoch,
                    public_key: key_hex,
                    activated_at_event_id,
                    status: EpochStatus::Invalid { reason },
                });
                broken_at = Some(epoch);
            }
        }
    }

    Ok(LineageReport {
        genesis_public_key: hex::encode(genesis),
        lineage_valid: broken_at.is_none(),
        trusted_public_key: hex::encode(trusted),
        epochs,
    })
}

/// One epoch's checks, mirroring `reconstruct_device_key_lineage_from` but
/// returning the failure as a string instead of erroring out.
#[allow(clippy::too_many_arguments)]
fn validate_epoch(
    conn: &Connection,
    epoch: i64,
    expected_epoch: i64,
    public_key: &[u8],
    prev_public_key: &[u8],
    current_bytes: &[u8; 32],
    attestation: Option<&[u8]>,
    authorization: Option<&[u8]>,
    activated_at_event_id: i64,
) -> std::result::Result<[u8; 32], String> {
    if epoch != expected_epoch {
        return Err(format!(
            "non-contiguous epoch (expected {}, found {}): history rows were deleted or inserted",
            expected_epoch, epoch
        ));
    }
    let new_bytes =
        key32(public_key, "device_key_history.public_key").map_err(|e| e.to_string())?;
    let prev_bytes =
        key32(prev_public_key, "device_key_history.prev_public_key").map_err(|e| e.to_string())?;
    if prev_bytes != *current_bytes {
        return Err(format!(
            "prev_public_key does not chain from epoch {}: the rotation claims a predecessor \
             the lineage never held",
            epoch - 1
        ));
    }
    let new_key = verifying_key_from_bytes(&new_bytes).map_err(|e| e.to_string())?;
    let current = verifying_key_from_bytes(current_bytes).map_err(|e| e.to_string())?;
    let attestation = attestation.ok_or_else(|| {
        "missing attestation: the incoming key never proved possession".to_string()
    })?;
    verify_rotation_attestation(&new_key, &prev_bytes, &new_bytes, attestation)
        .map_err(|e| format!("attestation failed: {}", e))?;
    match authorization {
        Some(authz) if !authz.is_empty() => {
            verify_rotation_authorization(&current, &prev_bytes, &new_bytes, authz)
                .map_err(|e| format!("authorization failed: {}", e))?;
        }
        _ => {
            recover_legacy_rotation_authorization(
                conn,
                &current,
                &prev_bytes,
                &new_bytes,
                activated_at_event_id,
            )
            .map_err(|e| format!("legacy authorization recovery failed: {}", e))?;
        }
    }
    Ok(new_bytes)
}

#[derive(Debug, Clone, Serialize)]
pub struct CheckpointReport {
    pub id: i64,
    pub created_at: i64,
    pub cutoff_event_id: i64,
    pub chain_head_hash: String,
    /// Hex signer key recorded on the row (None on pre-rotation databases).
    pub recorded_signer: Option<String>,
    /// Lineage epoch the signer belongs to, when it is genesis-anchored.
    pub signer_epoch: Option<i64>,
    pub signature_valid: bool,
    /// Everything wrong with this checkpoint, in plain language. Empty = healthy.
    pub problems: Vec<String>,
}

/// Inspect every row of the `checkpoints` table: resolve each signer against
/// the (already-inspected) lineage, verify the signature, and flag timestamp
/// or cutoff regressions. Never bails — every checkpoint gets a report.
pub fn inspect_checkpoints(
    conn: &Connection,
    lineage: &[DeviceKeyEpoch],
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
) -> Result<Vec<CheckpointReport>> {
    let mut stmt = conn.prepare(
        "SELECT id, created_at, chain_head_hash, signature, pq_signature, pq_scheme, \
                cutoff_event_id, signer_public_key \
         FROM checkpoints ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut reports: Vec<CheckpointReport> = Vec::new();
    let mut prev_created_at: Option<i64> = None;
    let mut prev_cutoff: Option<i64> = None;

    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let created_at: i64 = row.get(1)?;
        let head: Vec<u8> = row.get(2)?;
        let signature: Vec<u8> = row.get(3)?;
        let pq_signature: Option<Vec<u8>> = row.get(4)?;
        let pq_scheme: Option<String> = row.get(5)?;
        let cutoff_event_id: i64 = row.get(6)?;
        let signer_public_key: Option<Vec<u8>> = row.get(7)?;

        let mut problems = Vec::new();
        let recorded_signer = signer_public_key.as_ref().map(hex::encode);

        // Resolve the signer: the recorded key when present, otherwise the
        // lineage key active at the cutoff (pre-rotation databases).
        let signer_bytes: Option<[u8; 32]> = match signer_public_key.as_deref() {
            Some(bytes) => key32(bytes, "checkpoints.signer_public_key")
                .map_err(|e| problems.push(e.to_string()))
                .ok(),
            None => crate::device_key_active_at_in(lineage, cutoff_event_id)
                .map_err(|e| problems.push(format!("cannot resolve active key: {}", e)))
                .ok(),
        };
        let signer_epoch = signer_bytes.and_then(|key| {
            lineage
                .iter()
                .find(|e| e.public_key == key)
                .map(|e| e.epoch)
        });
        if signer_bytes.is_some() && signer_epoch.is_none() {
            problems.push(
                "signer key is not part of the genesis-anchored device key lineage — \
                 do not trust this checkpoint or the pruned history behind it"
                    .to_string(),
            );
        }

        let mut signature_valid = false;
        match (signer_bytes, key32(&head, "checkpoints.chain_head_hash")) {
            (Some(signer), Ok(head32)) => {
                match SignatureSet::from_storage(&signature, pq_signature, pq_scheme) {
                    Ok(signature_set) => {
                        // The cutoff-bound message first (log::checkpoint_message),
                        // then the bare head for checkpoints written before the
                        // cutoff was bound to the signature.
                        let verified = verifying_key_from_bytes(&signer).and_then(|key| {
                            verify_entry_signature(
                                &key,
                                &crate::log::checkpoint_message(&head32, cutoff_event_id),
                                &signature_set,
                                mode,
                                pq_public_key,
                                DOMAIN_CHECKPOINT,
                            )
                            .or_else(|_| {
                                verify_entry_signature(
                                    &key,
                                    &head32,
                                    &signature_set,
                                    mode,
                                    pq_public_key,
                                    DOMAIN_CHECKPOINT,
                                )
                            })
                        });
                        match verified {
                            Ok(()) => signature_valid = true,
                            Err(e) => problems.push(format!(
                                "signature does not verify under the resolved signer: {}",
                                e
                            )),
                        }
                    }
                    Err(e) => problems.push(format!("corrupt signature columns: {}", e)),
                }
            }
            (_, Err(e)) => problems.push(e.to_string()),
            (None, _) => {}
        }

        if let Some(prev) = prev_created_at {
            if created_at < prev {
                problems.push(
                    "created_at regressed from the previous checkpoint (possible back-dated \
                     checkpoint)"
                        .to_string(),
                );
            }
        }
        if let Some(prev) = prev_cutoff {
            if cutoff_event_id < prev {
                problems.push(
                    "cutoff_event_id regressed from the previous checkpoint (pruning history \
                     never moves backwards)"
                        .to_string(),
                );
            }
        }
        prev_created_at = Some(created_at);
        prev_cutoff = Some(cutoff_event_id);

        reports.push(CheckpointReport {
            id,
            created_at,
            cutoff_event_id,
            chain_head_hash: hex::encode(&head),
            recorded_signer,
            signer_epoch,
            signature_valid,
            problems,
        });
    }

    Ok(reports)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
        TimeBucket, ZonePolicy,
    };
    use std::time::Duration;

    fn open_kernel() -> (Kernel, KernelConfig) {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(3600),
            device_key_seed: "devkey:inspect:000000000000000000000".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg).expect("open kernel");
        (kernel, cfg)
    }

    fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) {
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        kernel
            .append_event_checked(
                &desc,
                CandidateEvent {
                    event_type: EventType::BoundaryCrossingObjectLarge,
                    time_bucket: TimeBucket {
                        start_epoch_s: 600,
                        size_s: 600,
                    },
                    zone_id: zone.to_string(),
                    confidence: 0.5,
                    correlation_token: None,
                    attestation: None,
                },
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
            .expect("seal event");
    }

    #[test]
    fn healthy_lineage_and_checkpoint_inspect_clean() {
        let (mut kernel, cfg) = open_kernel();
        let genesis = kernel.device_key_for_verify_only();
        seal_event(&mut kernel, &cfg, "zone:a");
        kernel
            .rotate_device_identity("devkey:inspect:111111111111111111111")
            .expect("rotate");
        seal_event(&mut kernel, &cfg, "zone:b");
        kernel
            .enforce_retention_with_checkpoint(Duration::from_secs(0))
            .expect("checkpoint");

        let report = inspect_key_lineage(&kernel.conn, &genesis).expect("inspect");
        assert!(report.lineage_valid);
        assert_eq!(report.epochs.len(), 2);
        assert!(report
            .epochs
            .iter()
            .all(|e| matches!(e.status, EpochStatus::Valid)));
        assert_ne!(report.trusted_public_key, report.genesis_public_key);

        let lineage =
            crate::reconstruct_device_key_lineage_from(&kernel.conn, &genesis).expect("lineage");
        let checkpoints = inspect_checkpoints(&kernel.conn, &lineage, SignatureMode::Compat, None)
            .expect("inspect checkpoints");
        assert_eq!(checkpoints.len(), 1);
        assert!(checkpoints[0].signature_valid);
        assert!(checkpoints[0].problems.is_empty());
        assert_eq!(checkpoints[0].signer_epoch, Some(1));
    }

    #[test]
    fn tampered_rotation_reports_invalid_then_unverifiable() {
        let (mut kernel, cfg) = open_kernel();
        let genesis = kernel.device_key_for_verify_only();
        seal_event(&mut kernel, &cfg, "zone:a");
        kernel
            .rotate_device_identity("devkey:inspect:111111111111111111111")
            .expect("rotate 1");
        kernel
            .rotate_device_identity("devkey:inspect:222222222222222222222")
            .expect("rotate 2");
        // Corrupt epoch 1's attestation: epoch 1 must report Invalid and
        // epoch 2 Unverifiable — the inspector keeps walking where the
        // verifier would bail.
        kernel
            .conn
            .execute(
                "UPDATE device_key_history SET attestation = x'00' WHERE epoch = 1",
                [],
            )
            .expect("tamper");

        let report = inspect_key_lineage(&kernel.conn, &genesis).expect("inspect");
        assert!(!report.lineage_valid);
        assert_eq!(report.epochs.len(), 3);
        assert!(matches!(report.epochs[0].status, EpochStatus::Valid));
        assert!(
            matches!(&report.epochs[1].status, EpochStatus::Invalid { reason } if reason.contains("attestation"))
        );
        assert!(matches!(
            report.epochs[2].status,
            EpochStatus::Unverifiable {
                depends_on_epoch: 1
            }
        ));
        // The trusted key stops at genesis.
        assert_eq!(report.trusted_public_key, report.genesis_public_key);
    }

    #[test]
    fn forged_checkpoint_signer_is_flagged() {
        let (mut kernel, cfg) = open_kernel();
        let genesis = kernel.device_key_for_verify_only();
        seal_event(&mut kernel, &cfg, "zone:a");
        kernel
            .enforce_retention_with_checkpoint(Duration::from_secs(0))
            .expect("checkpoint");
        // Swap the recorded signer for a key the device never authorized.
        let forged = ed25519_dalek::SigningKey::from_bytes(&[42u8; 32]);
        kernel
            .conn
            .execute(
                "UPDATE checkpoints SET signer_public_key = ?1",
                [forged.verifying_key().to_bytes().to_vec()],
            )
            .expect("forge signer");

        let lineage =
            crate::reconstruct_device_key_lineage_from(&kernel.conn, &genesis).expect("lineage");
        let checkpoints = inspect_checkpoints(&kernel.conn, &lineage, SignatureMode::Compat, None)
            .expect("inspect checkpoints");
        assert_eq!(checkpoints.len(), 1);
        assert_eq!(checkpoints[0].signer_epoch, None);
        assert!(checkpoints[0]
            .problems
            .iter()
            .any(|p| p.contains("not part of the genesis-anchored")));
        // The forged key did not produce the signature either.
        assert!(!checkpoints[0].signature_valid);
    }
}
