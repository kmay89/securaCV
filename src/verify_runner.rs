//! Shared full-verification runner.
//!
//! Composes the primitives in [`crate::verify`] / [`crate::verify_helpers`]
//! into the complete sealed-log check (checkpoint signature, event chain,
//! break-glass receipts, export receipts, alarm count). The `log_verify`
//! CLI and the event API's `POST /verify` both drive this function so the
//! two verification paths cannot drift.

use anyhow::Result;
use ed25519_dalek::VerifyingKey;
use rusqlite::Connection;
use serde::Serialize;

use crate::crypto::signatures::{PqPublicKey, SignatureMode};
use crate::{verify, verify_helpers, TimeBucket};

/// One verified entry, surfaced to the caller's progress callback
/// (the CLI prints these under `--verbose`; the API ignores them).
#[derive(Debug, Clone, Copy)]
pub enum VerifiedItem {
    Event { id: i64, entry_hash: [u8; 32] },
    BreakGlassReceipt { id: i64, entry_hash: [u8; 32] },
    ExportReceipt { id: i64, entry_hash: [u8; 32] },
}

/// Outcome of a successful full verification. Timestamps are bucketed —
/// this report flows out through the API and MQTT, so it must carry no
/// precise times (invariants §5 metadata minimization).
#[derive(Debug, Clone, Serialize)]
pub struct VerifyReport {
    pub chain_valid: bool,
    pub events_verified: u64,
    pub break_glass_receipts_verified: u64,
    pub break_glass_granted: u64,
    pub break_glass_denied: u64,
    pub export_receipts_verified: u64,
    pub alarm_count: u64,
    pub checkpoint_cutoff_event_id: Option<i64>,
    /// Hex chain-head hash of the latest checkpoint, when one exists.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub checkpoint_head_hash: Option<String>,
    pub verified_at_bucket_start: u64,
    pub verified_at_bucket_size: u32,
    /// Failure detail when `chain_valid` is false.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    /// Structured failure location/kind when `chain_valid` is false and the
    /// failure originated in a chain/checkpoint check (additive — the `error`
    /// string above is unchanged and remains the compatibility surface).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub failure: Option<verify::VerifyFailure>,
    /// Timeline-audit warnings (tail staleness, missing heartbeats,
    /// timestamp regressions). The chain is still hash/signature valid;
    /// these flag anomalies the hash chain alone cannot see. Coarse bucket
    /// starts only. Omitted when empty so old report consumers see no change.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub warnings: Vec<String>,
}

impl VerifyReport {
    fn at_now(bucket: TimeBucket) -> Self {
        Self {
            chain_valid: false,
            events_verified: 0,
            break_glass_receipts_verified: 0,
            break_glass_granted: 0,
            break_glass_denied: 0,
            export_receipts_verified: 0,
            alarm_count: 0,
            checkpoint_cutoff_event_id: None,
            checkpoint_head_hash: None,
            verified_at_bucket_start: bucket.start_epoch_s,
            verified_at_bucket_size: bucket.size_s,
            error: None,
            failure: None,
            warnings: Vec::new(),
        }
    }
}

/// Run the complete sealed-log verification and fold the result into a
/// serializable [`VerifyReport`]. A verification *failure* (tampered chain,
/// bad signature, untrusted checkpoint signer) is reported as
/// `chain_valid: false` with the error message — it is not an `Err`. `Err`
/// is reserved for being unable to attempt verification at all (e.g. the
/// current time bucket cannot be computed).
pub fn run_full_verify(
    conn: &Connection,
    public_key_hex: Option<&str>,
    pq_public_key_hex: Option<&str>,
    signature_mode: SignatureMode,
    mut on_item: impl FnMut(VerifiedItem),
) -> Result<VerifyReport> {
    let bucket = TimeBucket::now_10min()?;
    let mut report = VerifyReport::at_now(bucket);

    match run_inner(
        conn,
        public_key_hex,
        pq_public_key_hex,
        signature_mode,
        &mut report,
        &mut on_item,
    ) {
        Ok(()) => {
            report.chain_valid = true;
            // The timeline audit only makes sense over a hash-valid chain.
            match verify::audit_chain_timeline(conn, bucket.start_epoch_s) {
                Ok(warnings) => report.warnings = warnings,
                Err(err) => report
                    .warnings
                    .push(format!("timeline audit could not run: {err:#}")),
            }
        }
        Err(err) => {
            report.chain_valid = false;
            report.error = Some(format!("{err:#}"));
            report.failure = err.downcast_ref::<verify::VerifyFailure>().cloned();
        }
    }
    Ok(report)
}

fn run_inner(
    conn: &Connection,
    public_key_hex: Option<&str>,
    pq_public_key_hex: Option<&str>,
    signature_mode: SignatureMode,
    report: &mut VerifyReport,
    on_item: &mut impl FnMut(VerifiedItem),
) -> Result<()> {
    let verifying_key = verify_helpers::load_verifying_key(conn, public_key_hex, None)?;
    let pq_verifying_key: Option<PqPublicKey> =
        verify_helpers::load_pq_verifying_key(conn, pq_public_key_hex, None)?;

    let checkpoint = verify::latest_checkpoint(conn)?;

    // Reconstruct the device key lineage anchored at the genesis key. Each
    // rotation epoch is cryptographically validated, so a tampered history
    // table cannot forge a lineage without the genesis private key. Key
    // selection comes from this trusted lineage, never from the checkpoint row.
    let genesis = verifying_key.to_bytes();
    let lineage = crate::reconstruct_device_key_lineage_from(conn, &genesis)?;

    // Verify the checkpoint signature with its recorded signer — but only
    // after confirming that signer is itself a genesis-anchored lineage key.
    let (chain_key_bytes, checkpoint_key_bytes) = match checkpoint.cutoff_event_id {
        Some(cutoff) => {
            let active = crate::device_key_active_at_in(&lineage, cutoff)?;
            let signer = match checkpoint.signer_public_key {
                Some(sig) => {
                    if !lineage.iter().any(|e| e.public_key == sig) {
                        return Err(anyhow::Error::new(verify::VerifyFailure {
                            ledger: verify::FailedLedger::KeyLineage,
                            entry_id: checkpoint.cutoff_event_id,
                            kind: verify::FailureKind::UntrustedSigner,
                            detail: "checkpoint signer key is not part of the \
                                     genesis-anchored device key lineage; refusing to \
                                     trust it"
                                .to_string(),
                        }));
                    }
                    sig
                }
                None => active,
            };
            (active, signer)
        }
        None => (genesis, genesis),
    };
    let chain_key = verifying_key_from_bytes(&chain_key_bytes)?;
    let checkpoint_key = verifying_key_from_bytes(&checkpoint_key_bytes)?;
    verify::verify_checkpoint_signature(
        &checkpoint_key,
        &checkpoint,
        signature_mode,
        pq_verifying_key.as_ref(),
    )?;
    report.checkpoint_cutoff_event_id = checkpoint.cutoff_event_id;
    report.checkpoint_head_hash = checkpoint
        .chain_head_hash
        .map(|h| verify_helpers::hex32(&h));

    report.alarm_count = verify::count_alarms(conn)?;

    report.events_verified = verify::verify_events_with(
        conn,
        &chain_key,
        checkpoint.chain_head_hash,
        signature_mode,
        pq_verifying_key.as_ref(),
        |id, entry_hash| on_item(VerifiedItem::Event { id, entry_hash }),
    )?;

    // Receipt and policy-change rows are signed by the device key current at
    // write time, so the ledger walks must accept every validated lineage key
    // — verifying with genesis alone raises a false tamper alarm on the first
    // post-rotation row (and witnessd then boots into safe mode on it).
    let lineage_keys = lineage
        .iter()
        .map(|epoch| verifying_key_from_bytes(&epoch.public_key))
        .collect::<Result<Vec<_>>>()?;

    let policy = verify::load_break_glass_policy(conn)?;
    let counts = verify::verify_break_glass_receipts_with(
        conn,
        &lineage_keys,
        policy.as_ref(),
        signature_mode,
        pq_verifying_key.as_ref(),
        |id, entry_hash| on_item(VerifiedItem::BreakGlassReceipt { id, entry_hash }),
    )?;
    report.break_glass_receipts_verified = counts.total;
    report.break_glass_granted = counts.granted;
    report.break_glass_denied = counts.denied;

    report.export_receipts_verified = verify::verify_export_receipts_with(
        conn,
        &lineage_keys,
        signature_mode,
        pq_verifying_key.as_ref(),
        |id, entry_hash| on_item(VerifiedItem::ExportReceipt { id, entry_hash }),
    )?;

    Ok(())
}

fn verifying_key_from_bytes(bytes: &[u8; 32]) -> Result<VerifyingKey> {
    VerifyingKey::from_bytes(bytes).map_err(|e| anyhow::anyhow!("invalid verifying key: {}", e))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        derive_db_encryption_key, signing_key_from_seed, CandidateEvent, EventType,
        InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, ZonePolicy,
    };
    use std::path::{Path, PathBuf};

    const TEST_SEED: &str = "devkey:test:runner1234567890abcdef1234567890";

    struct TempDb {
        path: PathBuf,
    }

    impl TempDb {
        fn new() -> Self {
            let mut path = std::env::temp_dir();
            let suffix: u64 = rand::random();
            path.push(format!("verify_runner_test_{}.db", suffix));
            Self { path }
        }

        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempDb {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }

    fn open_kernel(path: &Path) -> Kernel {
        Kernel::open(&KernelConfig {
            db_path: path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        })
        .expect("open kernel")
    }

    fn write_test_event(kernel: &mut Kernel) {
        let module = ModuleDescriptor {
            id: "test-module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket::now(600).expect("bucket"),
            zone_id: "zone:test".to_string(),
            confidence: 0.9,
            correlation_token: None,
            attestation: None,
        };
        kernel
            .append_event_checked(
                &module,
                cand,
                env!("CARGO_PKG_VERSION"),
                "ruleset:test",
                KernelConfig::ruleset_hash_from_id("ruleset:test"),
            )
            .expect("append event");
    }

    fn open_encrypted(path: &Path) -> Connection {
        let signing_key = signing_key_from_seed(TEST_SEED).unwrap();
        let db_key = derive_db_encryption_key(&signing_key);
        let conn = Connection::open(path).unwrap();
        conn.pragma_update(None, "key", format!("x'{}'", *db_key))
            .unwrap();
        conn
    }

    #[test]
    fn full_verify_reports_valid_chain_and_counts() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let mut seen_events = 0u64;
        let report = run_full_verify(&conn, None, None, SignatureMode::Compat, |item| {
            if matches!(item, VerifiedItem::Event { .. }) {
                seen_events += 1;
            }
        })
        .expect("runner runs");

        assert!(
            report.chain_valid,
            "expected valid chain: {:?}",
            report.error
        );
        assert!(report.events_verified >= 1);
        assert_eq!(seen_events, report.events_verified);
        assert!(report.error.is_none());
        // Privacy: report timestamps are bucketed, never precise.
        assert_eq!(report.verified_at_bucket_size, 600);
        assert_eq!(report.verified_at_bucket_start % 600, 0);
    }

    #[test]
    fn full_verify_flags_tampered_payload_without_err() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        kernel
            .conn
            .execute(
                "UPDATE sealed_events SET payload_json = '{\"tampered\":true}' WHERE id = 1",
                [],
            )
            .expect("tamper");
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report =
            run_full_verify(&conn, None, None, SignatureMode::Compat, |_| {}).expect("runner runs");

        assert!(!report.chain_valid);
        assert!(report.error.is_some());
        // The structured failure serializes additively next to the unchanged
        // error string...
        let json = serde_json::to_string(&report).expect("serialize");
        assert!(json.contains("\"failure\""));
        assert!(json.contains("\"kind\":\"entry_hash_mismatch\""));
        // ...and pinpoints the break for the diagnosis output.
        let failure = report.failure.expect("chain failure carries location/kind");
        assert_eq!(failure.ledger, verify::FailedLedger::SealedEvents);
        assert_eq!(failure.entry_id, Some(1));
        assert_eq!(failure.kind, verify::FailureKind::EntryHashMismatch);
    }

    #[test]
    fn full_verify_flags_wrong_public_key() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);

        let wrong_key = ed25519_dalek::SigningKey::from_bytes(&[42u8; 32]);
        let wrong_hex = hex::encode(wrong_key.verifying_key().to_bytes());

        let conn = open_encrypted(db.path());
        let report = run_full_verify(&conn, Some(&wrong_hex), None, SignatureMode::Compat, |_| {})
            .expect("runner runs");

        assert!(!report.chain_valid);
    }

    #[test]
    fn full_verify_surfaces_timeline_warnings() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        let now_bucket = TimeBucket::now_10min().expect("bucket").start_epoch_s;
        let hash = KernelConfig::ruleset_hash_from_id("ruleset:test");
        kernel
            .append_lifecycle(
                crate::LifecyclePhase::Start,
                env!("CARGO_PKG_VERSION"),
                "ruleset:test",
                hash,
            )
            .expect("lifecycle");
        // Heartbeat for the start bucket and start+1200 with a hole at
        // start+600 — the shape mid-chain deletion leaves behind.
        for bucket_start in [now_bucket, now_bucket + 1200] {
            kernel
                .append_heartbeat(
                    TimeBucket {
                        start_epoch_s: bucket_start,
                        size_s: 600,
                    },
                    true,
                    1,
                    0,
                    0,
                    env!("CARGO_PKG_VERSION"),
                    "ruleset:test",
                    hash,
                )
                .expect("heartbeat");
        }
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report =
            run_full_verify(&conn, None, None, SignatureMode::Compat, |_| {}).expect("runner runs");

        // Hash/signature chain is intact — warnings flag the timeline anomaly.
        assert!(report.chain_valid, "chain: {:?}", report.error);
        assert!(
            report.warnings.iter().any(|w| w.contains("no heartbeat")),
            "expected missing-heartbeat warning, got: {:?}",
            report.warnings
        );
        let json = serde_json::to_string(&report).expect("serialize");
        assert!(json.contains("warnings"));
    }

    #[test]
    fn report_serializes_to_json() {
        let report = VerifyReport {
            chain_valid: true,
            events_verified: 3,
            break_glass_receipts_verified: 0,
            break_glass_granted: 0,
            break_glass_denied: 0,
            export_receipts_verified: 1,
            alarm_count: 0,
            checkpoint_cutoff_event_id: None,
            checkpoint_head_hash: None,
            verified_at_bucket_start: 1_700_000_400,
            verified_at_bucket_size: 600,
            error: None,
            failure: None,
            warnings: Vec::new(),
        };
        let json = serde_json::to_string(&report).expect("serialize");
        assert!(json.contains("\"chain_valid\":true"));
        assert!(json.contains("\"events_verified\":3"));
        // Optional fields are omitted, not null.
        assert!(!json.contains("checkpoint_head_hash"));
        assert!(!json.contains("error"));
        // Empty warnings are omitted entirely (additive-field compat).
        assert!(!json.contains("warnings"));
    }
}
