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
use crate::{verify, verify_helpers, HighWaterMark, TimeBucket};

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
    /// Honest verdict label (B3 / `ENTERPRISE_CUSTODY.md` §2): a chain is only
    /// as strong as the key it is checked against. `"valid"` when an
    /// out-of-band verifying key was supplied; `"self-consistent; identity
    /// unverified"` when the anchor was taken from the database under audit
    /// (a self-anchored verdict proves internal consistency, not identity);
    /// `"verification failed"` when the chain did not verify — which does not by
    /// itself prove manipulation (it can be a wrong key or a stale DB); the
    /// precise cause is in `failure`/`error`. Additive — `chain_valid` is
    /// unchanged and remains the machine-readable pass/fail.
    pub verdict: String,
    /// Whether the identity anchor came from *outside* the audited database
    /// (an operator-supplied / escrowed key). `false` ⇒ self-anchored, and the
    /// verdict is labeled accordingly.
    pub identity_verified: bool,
    /// Whether a signed external high-water-mark was supplied and checked
    /// (`ENTERPRISE_CUSTODY.md` §2 anti-truncation/rollback/wipe). When `true`
    /// and `chain_valid` is `true`, the live log is at or ahead of the mark.
    pub high_water_mark_checked: bool,
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
            verdict: String::new(),
            identity_verified: false,
            high_water_mark_checked: false,
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
    on_item: impl FnMut(VerifiedItem),
) -> Result<VerifyReport> {
    run_full_verify_impl(
        conn,
        public_key_hex,
        pq_public_key_hex,
        signature_mode,
        None,
        on_item,
    )
}

/// Like [`run_full_verify`], but also checks a signed external high-water-mark
/// (`docs/security/ENTERPRISE_CUSTODY.md` §2). When `high_water_mark` is
/// `Some`, verification additionally fails closed if the mark's signature is
/// bad or off-lineage, or if the live log is *behind* the mark — the
/// tail-truncation, whole-file rollback, and wiped-log cases the interior hash
/// chain cannot see on its own.
pub fn run_full_verify_with_high_water_mark(
    conn: &Connection,
    public_key_hex: Option<&str>,
    pq_public_key_hex: Option<&str>,
    signature_mode: SignatureMode,
    high_water_mark: Option<&HighWaterMark>,
    on_item: impl FnMut(VerifiedItem),
) -> Result<VerifyReport> {
    run_full_verify_impl(
        conn,
        public_key_hex,
        pq_public_key_hex,
        signature_mode,
        high_water_mark,
        on_item,
    )
}

fn run_full_verify_impl(
    conn: &Connection,
    public_key_hex: Option<&str>,
    pq_public_key_hex: Option<&str>,
    signature_mode: SignatureMode,
    high_water_mark: Option<&HighWaterMark>,
    mut on_item: impl FnMut(VerifiedItem),
) -> Result<VerifyReport> {
    let bucket = TimeBucket::now_10min()?;
    let mut report = VerifyReport::at_now(bucket);
    // B3 honesty: a verdict is only as strong as the key it is checked
    // against. An out-of-band key means we verified identity; a self-anchored
    // key (taken from the DB under audit) means we verified internal
    // consistency only. Record it before the walk so it labels a failure too.
    report.identity_verified = public_key_hex.is_some();

    match run_inner(
        conn,
        public_key_hex,
        pq_public_key_hex,
        signature_mode,
        high_water_mark,
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
    report.verdict = if report.chain_valid {
        if report.identity_verified {
            "valid".to_string()
        } else {
            "self-consistent; identity unverified".to_string()
        }
    } else {
        // A failed chain does not, by itself, prove data was manipulated: it can
        // equally be a wrong out-of-band key, a mismatched genesis anchor, or a
        // stale/partial database. So the verdict says "did not verify" without
        // asserting a cause — the precise cause lives in `failure.kind` and
        // `error` (an entry/prev-hash mismatch there does prove tampering; a
        // signature/lineage mismatch may just be a key/provenance error).
        "verification failed".to_string()
    };
    Ok(report)
}

fn run_inner(
    conn: &Connection,
    public_key_hex: Option<&str>,
    pq_public_key_hex: Option<&str>,
    signature_mode: SignatureMode,
    high_water_mark: Option<&HighWaterMark>,
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

    // Signed external high-water-mark (ENTERPRISE_CUSTODY §2): the interior
    // hash chain, now proven above, cannot bind its own length or head. If a
    // mark was supplied, it does. Runs last so a mark failure reports on top of
    // an otherwise-consistent chain.
    if let Some(hwm) = high_water_mark {
        // The mark's signer must be a genesis-anchored lineage key — the same
        // rule the checkpoint signer obeys — so a tampered mark file cannot
        // introduce an untrusted signer.
        if !lineage
            .iter()
            .any(|e| e.public_key == hwm.signer_public_key)
        {
            return Err(anyhow::Error::new(verify::VerifyFailure {
                ledger: verify::FailedLedger::HighWaterMark,
                entry_id: None,
                kind: verify::FailureKind::UntrustedSigner,
                detail: "high-water-mark signer key is not part of the genesis-anchored \
                         device key lineage; refusing to trust it"
                    .to_string(),
            }));
        }
        let signer = verifying_key_from_bytes(&hwm.signer_public_key)?;
        hwm.verify_signature(&signer, signature_mode, pq_verifying_key.as_ref())
            .map_err(|e| {
                anyhow::Error::new(verify::VerifyFailure {
                    ledger: verify::FailedLedger::HighWaterMark,
                    entry_id: None,
                    kind: verify::FailureKind::SignatureMismatch,
                    detail: format!("high-water-mark signature invalid: {e:#}"),
                })
            })?;

        // The live high is the newest REAL signed row — never the writable
        // sqlite_sequence counter, which an actor with DB access could inflate
        // to fake progress past the mark. When retention pruning has emptied
        // the live table, the authoritative head/high is the signed checkpoint
        // (already signature- and lineage-verified above): the checkpoint is
        // the pruning survivor and its head chains transitively from the mark's
        // recorded head. Reconciling here is what tells a legitimate
        // full-retention prune apart from a wipe.
        let (newest_id, newest_head) = crate::log::high_water_mark::read_live_head(conn)?;
        let (live_high, live_head): (i64, Option<[u8; 32]>) = match newest_id {
            Some(id) => (id, newest_head),
            None => (
                checkpoint.cutoff_event_id.unwrap_or(0),
                checkpoint.chain_head_hash,
            ),
        };
        let live_high = live_high.max(0) as u64;

        if live_high < hwm.seq {
            return Err(anyhow::Error::new(verify::VerifyFailure {
                ledger: verify::FailedLedger::HighWaterMark,
                entry_id: None,
                kind: verify::FailureKind::HighWaterRegression,
                detail: format!(
                    "sealed-log high-water regression: live high {live_high} is behind the signed \
                     high-water {} — tail truncation, whole-file rollback, or wipe",
                    hwm.seq
                ),
            }));
        }
        if live_high == hwm.seq && live_head != Some(hwm.head_hash) {
            return Err(anyhow::Error::new(verify::VerifyFailure {
                ledger: verify::FailedLedger::HighWaterMark,
                entry_id: None,
                kind: verify::FailureKind::HighWaterRegression,
                detail: format!(
                    "sealed-log head fork at high-water seq {}: live head does not match the \
                     signed mark",
                    hwm.seq
                ),
            }));
        }
        // live_high > hwm.seq is a legitimate forward advance: the live high now
        // comes only from real signed rows (unforgeable without the signing key)
        // and the verified checkpoint, so it cannot be inflated past the mark by
        // a no-key actor. The interior chain verify above already tied every
        // live row (and, post-prune, the checkpoint) back through the mark's
        // recorded head, so the anchor is not abandoned. A mark that legitimately
        // lagged the true high is the documented best-effort residual.
        report.high_water_mark_checked = true;
    }

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
            verdict: "valid".to_string(),
            identity_verified: true,
            high_water_mark_checked: false,
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
        // B3 labeling fields are always present.
        assert!(json.contains("\"verdict\":\"valid\""));
        assert!(json.contains("\"identity_verified\":true"));
    }

    fn device_signature_keys(
        sk: &ed25519_dalek::SigningKey,
    ) -> crate::crypto::signatures::SignatureKeys<'_> {
        crate::crypto::signatures::SignatureKeys::new(sk)
    }

    #[test]
    fn full_verify_passes_with_valid_high_water_mark() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.expect("has events") as u64,
            head.expect("has events"),
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");

        assert!(report.chain_valid, "{:?}", report.error);
        assert!(report.high_water_mark_checked);
        // Self-anchored (no out-of-band key) → honest label.
        assert!(!report.identity_verified);
        assert_eq!(report.verdict, "self-consistent; identity unverified");
    }

    #[test]
    fn full_verify_fails_closed_when_live_log_is_behind_mark() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, _head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        // A device-signed mark that records the log having reached far past
        // where it now sits — the mark that would exist before a rollback/wipe.
        let mark = crate::HighWaterMark::sign(
            newest_id.unwrap() as u64 + 5,
            [7u8; 32],
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");

        assert!(!report.chain_valid);
        let failure = report.failure.expect("structured failure");
        assert_eq!(failure.ledger, verify::FailedLedger::HighWaterMark);
        assert_eq!(failure.kind, verify::FailureKind::HighWaterRegression);
        assert_eq!(report.verdict, "verification failed");
    }

    #[test]
    fn full_verify_fails_closed_on_tail_truncation() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.unwrap() as u64,
            head.unwrap(),
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();

        // Lop off the newest event. The interior chain still verifies (shorter,
        // internally consistent) but the live high now drops below the mark's
        // recorded seq, so the regression is caught.
        conn.execute(
            "DELETE FROM sealed_events WHERE id = (SELECT MAX(id) FROM sealed_events)",
            [],
        )
        .unwrap();

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");

        assert!(!report.chain_valid);
        assert_eq!(
            report.failure.unwrap().kind,
            verify::FailureKind::HighWaterRegression
        );
    }

    #[test]
    fn full_verify_rejects_high_water_mark_signed_by_untrusted_key() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        // A key the device never authorized.
        let rogue = ed25519_dalek::SigningKey::from_bytes(&[123u8; 32]);
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.unwrap() as u64,
            head.unwrap(),
            600,
            &device_signature_keys(&rogue),
        )
        .unwrap();

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");

        assert!(!report.chain_valid);
        assert_eq!(
            report.failure.unwrap().kind,
            verify::FailureKind::UntrustedSigner
        );
    }

    #[test]
    fn full_verify_passes_after_full_retention_prune() {
        // Regression guard: a legitimate full-retention prune empties
        // sealed_events but leaves a signed checkpoint. The verifier must
        // reconcile the empty table against the checkpoint head and NOT mistake
        // the prune for a wipe.
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);

        // The mark the writer would have signed at the current tip.
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&kernel.conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.expect("has events") as u64,
            head.expect("has events"),
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();

        // Age every event into the past and prune them all — writes a signed
        // checkpoint and empties the table (the case that used to false-fail).
        kernel
            .conn
            .execute("UPDATE sealed_events SET created_at = 1", [])
            .unwrap();
        kernel
            .enforce_retention_with_checkpoint(std::time::Duration::from_secs(1))
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
        let (newest_after, _) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        assert!(
            newest_after.is_none(),
            "retention should have emptied the live table"
        );

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");
        assert!(
            report.chain_valid,
            "a full retention prune must not false-fail the mark: {:?}",
            report.error
        );
        assert!(report.high_water_mark_checked);
    }

    #[test]
    fn high_water_mark_ignores_inflated_sqlite_sequence() {
        // A no-signing-key actor truncates the newest rows and inflates the
        // writable sqlite_sequence counter to fake forward progress past the
        // mark. Because the verifier reads MAX(id) of real signed rows (not the
        // counter), the truncation is still caught.
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.unwrap() as u64,
            head.unwrap(),
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();

        // Truncate the newest rows, then inflate the counter well past the mark.
        conn.execute("DELETE FROM sealed_events WHERE id > 1", [])
            .unwrap();
        conn.execute(
            "UPDATE sqlite_sequence SET seq = 999 WHERE name = 'sealed_events'",
            [],
        )
        .unwrap();

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");
        assert!(
            !report.chain_valid,
            "an inflated sqlite_sequence must not mask a real truncation"
        );
        assert_eq!(
            report.failure.unwrap().kind,
            verify::FailureKind::HighWaterRegression
        );
    }

    #[test]
    fn store_advances_high_water_mark_on_append_then_verify_passes() {
        // End-to-end: a store opened with a mark path advances the mark on
        // every append, and the resulting mark verifies against the live log.
        let db = TempDb::new();
        let hwm_path = std::env::temp_dir().join(format!("hwm_e2e_{}.bin", suffix()));
        let _ = std::fs::remove_file(&hwm_path);

        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let db_key = derive_db_encryption_key(&sk);
        let db_path = db.path().to_string_lossy().to_string();
        let store =
            crate::storage::SqliteSealedLogStore::open_with_key(&db_path, Some(db_key.as_str()))
                .unwrap()
                .with_high_water_mark(Some(hwm_path.clone()));
        let cfg = KernelConfig {
            db_path: db_path.clone(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open_with_sealed_log(&cfg, Box::new(store)).expect("open kernel");
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        drop(kernel);

        // The mark was written and points at the newest event.
        let mark = crate::log::high_water_mark::load(&hwm_path)
            .unwrap()
            .expect("mark written by the store on append");
        let conn = open_encrypted(db.path());
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&conn).unwrap();
        assert_eq!(mark.seq, newest_id.unwrap() as u64);
        assert_eq!(Some(mark.head_hash), head);

        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");
        assert!(report.chain_valid, "{:?}", report.error);
        assert!(report.high_water_mark_checked);

        let _ = std::fs::remove_file(&hwm_path);
    }

    #[test]
    fn boot_verify_fails_closed_on_rollback_when_mark_configured() {
        // Codex P1: witnessd's boot check (Kernel::verify_sealed_log) must fold
        // in the configured mark, or a rolled-back-but-internally-consistent DB
        // passes boot and the daemon launders the rollback by extending it.
        let db = TempDb::new();
        let hwm_path = std::env::temp_dir().join(format!("hwm_boot_{}.bin", suffix()));
        let _ = std::fs::remove_file(&hwm_path);

        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let db_key = derive_db_encryption_key(&sk);
        let db_path = db.path().to_string_lossy().to_string();
        let store =
            crate::storage::SqliteSealedLogStore::open_with_key(&db_path, Some(db_key.as_str()))
                .unwrap()
                .with_high_water_mark(Some(hwm_path.clone()));
        let cfg = KernelConfig {
            db_path: db_path.clone(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open_with_sealed_log(&cfg, Box::new(store)).expect("open kernel");
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);

        // With the mark current, boot verify passes and checks the mark.
        let ok = kernel
            .verify_sealed_log_with_hwm_path(Some(&hwm_path))
            .expect("verify runs");
        assert!(ok.chain_valid, "{:?}", ok.error);
        assert!(ok.high_water_mark_checked);

        // Roll the newest event back on the kernel's own connection — the
        // interior chain stays consistent, but it is now behind the mark.
        kernel
            .conn
            .execute(
                "DELETE FROM sealed_events WHERE id = (SELECT MAX(id) FROM sealed_events)",
                [],
            )
            .unwrap();

        let rolled = kernel
            .verify_sealed_log_with_hwm_path(Some(&hwm_path))
            .expect("verify runs");
        assert!(
            !rolled.chain_valid,
            "boot verify must fail closed on a rollback when the mark is set"
        );
        assert_eq!(
            rolled.failure.unwrap().kind,
            verify::FailureKind::HighWaterRegression
        );

        let _ = std::fs::remove_file(&hwm_path);
    }

    fn suffix() -> u64 {
        use std::time::{SystemTime, UNIX_EPOCH};
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos() as u64
    }

    #[test]
    fn full_verify_labels_identity_verified_with_out_of_band_key() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);

        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let pubhex = hex::encode(sk.verifying_key().to_bytes());
        let conn = open_encrypted(db.path());
        let report = run_full_verify(&conn, Some(&pubhex), None, SignatureMode::Compat, |_| {})
            .expect("runner runs");
        assert!(report.chain_valid, "{:?}", report.error);
        assert!(report.identity_verified);
        assert_eq!(report.verdict, "valid");
    }
}
