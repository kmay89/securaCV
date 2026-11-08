//! Reader hardening (PR M0): a receipt records only the approvals over its
//! own request hash, and the verifier skips an approval that does not count
//! instead of failing the row (D9); a stored policy that cannot be loaded
//! opens the kernel, refuses every break-glass action, and fails
//! verification, so witnessd lands in safe mode instead of exiting (D7).
//!
//! Offline: every bucket is injected, keys are fixed bytes.

use anyhow::{anyhow, Result};
use ed25519_dalek::SigningKey;

use crate::break_glass::{
    approvals_commitment, verify_approval, Approval, BreakGlass, BreakGlassOutcome,
    BreakGlassReceipt, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
use crate::verify::{FailedLedger, FailureKind};
use crate::verify_runner::{run_full_verify, VerifyReport};
use crate::{Kernel, KernelConfig, SignatureMode, TimeBucket, ZonePolicy};

/// The injected bucket every request, approval and authorize shares.
const BUCKET: TimeBucket = TimeBucket {
    start_epoch_s: 1_794_312_000,
    size_s: 600,
};

/// `break_glass::core::MAX_APPROVALS` (not re-exported); the count-exceeded
/// denial below pins its message, so a drift fails here.
const MAX_APPROVALS: usize = 64;

const NEXT_BUCKET: TimeBucket = TimeBucket {
    start_epoch_s: 1_794_312_600,
    size_s: 600,
};

fn alice() -> SigningKey {
    SigningKey::from_bytes(&[0x11; 32])
}
fn bob() -> SigningKey {
    SigningKey::from_bytes(&[0x22; 32])
}
fn carol() -> SigningKey {
    SigningKey::from_bytes(&[0x33; 32])
}

fn config(db_path: &str) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:m0reader0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

/// 2-of-3: alice, bob, carol. Stored through the gated path, so the
/// policy-change history exists and every receipt's era resolves (the
/// verifier re-derives the quorum rather than taking the legacy shortcut).
fn two_of_three() -> QuorumPolicy {
    QuorumPolicy::new(
        2,
        vec![
            TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice().verifying_key().to_bytes(),
            },
            TrusteeEntry {
                id: TrusteeId::new("bob"),
                public_key: bob().verifying_key().to_bytes(),
            },
            TrusteeEntry {
                id: TrusteeId::new("carol"),
                public_key: carol().verifying_key().to_bytes(),
            },
        ],
    )
    .expect("2-of-3 policy")
}

fn kernel_with_policy() -> Result<(Kernel, QuorumPolicy)> {
    let mut kernel = Kernel::open(&config(":memory:"))?;
    let policy = two_of_three();
    let outcome = kernel.set_break_glass_policy_gated(&policy, &[], BUCKET)?;
    assert_eq!(outcome, crate::PolicyChangeOutcome::Bootstrapped);
    Ok((kernel, policy))
}

fn request(envelope: &str) -> UnlockRequest {
    UnlockRequest::new(
        envelope,
        KernelConfig::ruleset_hash_from_id("ruleset:test"),
        "incident",
        BUCKET,
    )
    .expect("request")
}

fn approve(id: &str, key: &SigningKey, request: &UnlockRequest) -> Approval {
    Approval::signed(TrusteeId::new(id), request.request_hash(), key)
}

/// An approval a trustee gave for ANOTHER request: a stale file handed in
/// beside the right ones.
fn stale(id: &str, key: &SigningKey) -> Approval {
    approve(id, key, &request("vault:some-other-envelope"))
}

/// A known trustee's approval over the right hash with a corrupted signature.
fn bad_signature(id: &str, key: &SigningKey, request: &UnlockRequest) -> Approval {
    let mut approval = approve(id, key, request);
    approval.signature[0] ^= 0x01;
    approval
}

/// The verdict with the device key supplied out of band, so a passing chain
/// reads `valid` (not the self-anchored label).
fn full_verify(kernel: &Kernel) -> VerifyReport {
    let key = hex::encode(kernel.device_verifying_key().to_bytes());
    run_full_verify(
        &kernel.conn,
        Some(&key),
        None,
        SignatureMode::Compat,
        |_| {},
    )
    .expect("the runner runs")
}

fn assert_valid(kernel: &Kernel) {
    let report = full_verify(kernel);
    assert!(
        report.chain_valid,
        "{:?} {:?}",
        report.error, report.failure
    );
    assert_eq!(report.verdict, "valid");
}

fn stored_approvals(kernel: &Kernel, entry_hash: &[u8; 32]) -> Vec<Approval> {
    let json: String = kernel
        .conn
        .query_row(
            "SELECT approvals_json FROM break_glass_receipts WHERE entry_hash = ?1",
            rusqlite::params![entry_hash.to_vec()],
            |r| r.get(0),
        )
        .expect("row");
    serde_json::from_str(&json).expect("approvals_json")
}

fn trustee_ids(approvals: &[Approval]) -> Vec<&str> {
    approvals.iter().map(|a| a.trustee.0.as_str()).collect()
}

/// The row an earlier writer stored for this authorize call: every presented
/// approval, committed and stored verbatim (the pre-M0 `authorize` committed
/// `approvals_commitment(approvals)` and `append_break_glass_receipt` stored
/// `approvals` as given). Device-signed and chained like any row.
fn append_as_an_earlier_writer_did(
    kernel: &mut Kernel,
    receipt: &BreakGlassReceipt,
    presented: &[Approval],
) -> Result<[u8; 32]> {
    let receipt = BreakGlassReceipt {
        approvals_commitment: approvals_commitment(presented),
        ..receipt.clone()
    };
    kernel.append_break_glass_receipt_row(&receipt, presented)
}

/// A frozen copy of the pre-M0 reader loop (`verify_receipt_quorum`, the body
/// at `src/verify.rs:1781-1806` before this change), kept verbatim so the
/// tests prove the behavior changed rather than restate the new code. It
/// failed the row on any non-counting approval, whatever the outcome.
fn pre_m0_reader_loop(
    policy: &QuorumPolicy,
    receipt: &BreakGlassReceipt,
    approvals: &[Approval],
) -> Result<usize> {
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
        crate::crypto::signatures::verifying_key_strict(&trustee.public_key)
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
    Ok(distinct_keys.len())
}

// ---------------------------------------------------------------------------
// D9: recorded approvals (tests 1–5)
// ---------------------------------------------------------------------------

/// Test 1. One stale approval file handed to `authorize` beside a valid one
/// no longer poisons the Denied row it produces.
#[test]
fn stale_approval_does_not_poison_a_denied_row() -> Result<()> {
    let (mut kernel, policy) = kernel_with_policy()?;
    let req = request("vault:porch");
    let presented = vec![approve("alice", &alice(), &req), stale("bob", &bob())];

    let (result, receipt) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    assert_eq!(
        result.unwrap_err().to_string(),
        "insufficient approvals: 1/2"
    );
    assert!(matches!(receipt.outcome, BreakGlassOutcome::Denied { .. }));

    // The row an earlier writer stored (both approvals): today's frozen
    // reader FAILS it — the poison this PR removes ...
    let earlier = append_as_an_earlier_writer_did(&mut kernel, &receipt, &presented)?;
    assert_eq!(
        trustee_ids(&stored_approvals(&kernel, &earlier)),
        ["alice", "bob"]
    );
    let earlier_receipt = BreakGlassReceipt {
        approvals_commitment: approvals_commitment(&presented),
        ..receipt.clone()
    };
    assert_eq!(
        pre_m0_reader_loop(&policy, &earlier_receipt, &presented)
            .unwrap_err()
            .to_string(),
        "approval request_hash mismatch for trustee bob"
    );
    // ... and the new reader accepts the very same row.
    crate::verify::verify_receipt_quorum(&policy, &[], &earlier_receipt, &presented)?;

    // The write path this build runs: the stale approval is neither
    // committed nor stored.
    let written = kernel.log_break_glass_receipt(&receipt, &presented)?;
    let stored = stored_approvals(&kernel, &written);
    assert_eq!(trustee_ids(&stored), ["alice"]);
    assert_eq!(receipt.approvals_commitment, approvals_commitment(&stored));

    // Both rows — the earlier writer's and this build's — verify `valid`.
    assert_valid(&kernel);
    let report = full_verify(&kernel);
    assert_eq!(report.break_glass_receipts_verified, 2);
    assert_eq!(report.break_glass_denied, 2);
    Ok(())
}

/// Test 2. n valid approvals plus one stale approval: Granted, `valid`, and
/// the stored row carries exactly the n approvals.
#[test]
fn stale_approval_does_not_poison_a_granted_row() -> Result<()> {
    let (mut kernel, policy) = kernel_with_policy()?;
    let req = request("vault:porch");
    let presented = vec![
        approve("alice", &alice(), &req),
        stale("carol", &carol()),
        approve("bob", &bob(), &req),
    ];

    let (result, receipt) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    result.expect("2 of 2 valid approvals grant");
    assert!(matches!(receipt.outcome, BreakGlassOutcome::Granted));
    assert_eq!(trustee_ids_of(&receipt), ["alice", "bob"]);

    let written = kernel.log_break_glass_receipt(&receipt, &presented)?;
    let stored = stored_approvals(&kernel, &written);
    assert_eq!(stored.len(), policy.n as usize);
    assert_eq!(trustee_ids(&stored), ["alice", "bob"]);
    assert_eq!(receipt.approvals_commitment, approvals_commitment(&stored));

    // The earlier writer's Granted row (stale approval stored) fails the
    // frozen reader and verifies under the new one.
    let earlier = append_as_an_earlier_writer_did(&mut kernel, &receipt, &presented)?;
    assert_eq!(stored_approvals(&kernel, &earlier).len(), 3);
    let earlier_receipt = BreakGlassReceipt {
        approvals_commitment: approvals_commitment(&presented),
        ..receipt.clone()
    };
    assert_eq!(
        pre_m0_reader_loop(&policy, &earlier_receipt, &presented)
            .unwrap_err()
            .to_string(),
        "approval request_hash mismatch for trustee carol"
    );

    assert_valid(&kernel);
    let report = full_verify(&kernel);
    assert_eq!(report.break_glass_granted, 2);
    Ok(())
}

fn trustee_ids_of(receipt: &BreakGlassReceipt) -> Vec<&str> {
    receipt.trustees_used.iter().map(|t| t.0.as_str()).collect()
}

/// Test 3. A known trustee's approval over the right hash with a bad
/// signature is recorded (it is over this request) but never counted: with
/// n valid beside it the row is Granted, with n − 1 it is Denied, and both
/// verify `valid`.
#[test]
fn corrupted_signature_is_ignored_not_counted() -> Result<()> {
    let (mut kernel, policy) = kernel_with_policy()?;

    let req = request("vault:porch");
    let presented = vec![
        approve("alice", &alice(), &req),
        approve("bob", &bob(), &req),
        bad_signature("carol", &carol(), &req),
    ];
    let (result, granted) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    result.expect("2 valid approvals grant");
    assert!(matches!(granted.outcome, BreakGlassOutcome::Granted));
    assert_eq!(trustee_ids_of(&granted), ["alice", "bob"]);
    let written = kernel.log_break_glass_receipt(&granted, &presented)?;
    assert_eq!(
        trustee_ids(&stored_approvals(&kernel, &written)),
        ["alice", "bob", "carol"]
    );
    assert_eq!(
        pre_m0_reader_loop(&policy, &granted, &presented)
            .unwrap_err()
            .to_string(),
        "invalid signature for trustee carol"
    );

    let req = request("vault:garage");
    let presented = vec![
        approve("alice", &alice(), &req),
        bad_signature("carol", &carol(), &req),
    ];
    let (result, denied) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    assert_eq!(
        result.unwrap_err().to_string(),
        "insufficient approvals: 1/2"
    );
    let written = kernel.log_break_glass_receipt(&denied, &presented)?;
    assert_eq!(
        trustee_ids(&stored_approvals(&kernel, &written)),
        ["alice", "carol"]
    );
    assert_eq!(
        pre_m0_reader_loop(&policy, &denied, &presented)
            .unwrap_err()
            .to_string(),
        "invalid signature for trustee carol"
    );

    assert_valid(&kernel);
    let report = full_verify(&kernel);
    assert_eq!(report.break_glass_granted, 1);
    assert_eq!(report.break_glass_denied, 1);
    Ok(())
}

/// Test 4. Skipping is not counting: a device-signed Granted row carrying
/// n − 1 valid approvals (here beside a stale one and a bad signature, which
/// add nothing) still fails with the existing message.
#[test]
fn forged_granted_still_fails() -> Result<()> {
    let (mut kernel, policy) = kernel_with_policy()?;
    let req = request("vault:porch");
    let presented = vec![
        approve("alice", &alice(), &req),
        stale("bob", &bob()),
        bad_signature("carol", &carol(), &req),
    ];
    let (result, receipt) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    assert_eq!(
        result.unwrap_err().to_string(),
        "insufficient approvals: 1/2"
    );
    // The device-key holder relabels the denial as a grant naming alice.
    let forged = BreakGlassReceipt {
        outcome: BreakGlassOutcome::Granted,
        trustees_used: vec![TrusteeId::new("alice")],
        ..receipt
    };
    append_as_an_earlier_writer_did(&mut kernel, &forged, &presented)?;

    let report = full_verify(&kernel);
    assert!(!report.chain_valid);
    assert_eq!(report.verdict, "verification failed");
    let failure = report.failure.expect("typed failure");
    assert_eq!(failure.ledger, FailedLedger::BreakGlassReceipts);
    assert_eq!(failure.kind, FailureKind::PolicyViolation);
    assert_eq!(
        failure.detail,
        "break-glass receipt claims Granted but only 1 of the required 2 distinct trustee \
         approvals are valid"
    );
    Ok(())
}

/// Test 5. The two denials `authorize` writes before examining any approval
/// (the request's window expired; too many approvals) also commit and store
/// only the approvals over the request's hash.
#[test]
fn early_denial_records_only_matching_approvals() -> Result<()> {
    let (mut kernel, policy) = kernel_with_policy()?;

    // Window expired: the request was opened in BUCKET, authorize runs in
    // the next one.
    let req = request("vault:porch");
    let presented = vec![stale("bob", &bob()), approve("alice", &alice(), &req)];
    let (result, receipt) = BreakGlass::authorize(&policy, &req, &presented, NEXT_BUCKET);
    assert_eq!(
        result.unwrap_err().to_string(),
        "request time window expired: opened in bucket 1794312000+600s, now 1794312600+600s"
    );
    assert!(receipt.trustees_used.is_empty());
    assert_eq!(
        receipt.approvals_commitment,
        approvals_commitment(&presented[1..])
    );
    let written = kernel.log_break_glass_receipt(&receipt, &presented)?;
    assert_eq!(trustee_ids(&stored_approvals(&kernel, &written)), ["alice"]);

    // Count exceeded: MAX_APPROVALS over the right hash plus one stale
    // approval. The cap counts what was presented; the row records the
    // matching ones, in presentation order.
    let req = request("vault:garage");
    let mut presented = vec![stale("carol", &carol())];
    presented.extend((0..MAX_APPROVALS).map(|_| approve("alice", &alice(), &req)));
    let (result, receipt) = BreakGlass::authorize(&policy, &req, &presented, BUCKET);
    assert_eq!(
        result.unwrap_err().to_string(),
        format!(
            "approval count {} exceeds maximum {}",
            MAX_APPROVALS + 1,
            MAX_APPROVALS
        )
    );
    assert_eq!(
        receipt.approvals_commitment,
        approvals_commitment(&presented[1..])
    );
    let written = kernel.log_break_glass_receipt(&receipt, &presented)?;
    let stored = stored_approvals(&kernel, &written);
    assert_eq!(stored.len(), MAX_APPROVALS);
    assert!(stored.iter().all(|a| a.request_hash == req.request_hash()));

    assert_valid(&kernel);
    assert_eq!(full_verify(&kernel).break_glass_denied, 2);
    Ok(())
}

// ---------------------------------------------------------------------------
// D7: a policy that cannot be loaded (tests 6–7; test 8 is the doctor CLI,
// tests/break_glass_unreadable_policy.rs)
// ---------------------------------------------------------------------------

/// The policy row the brief names: `trustees` is a string, so it does not
/// parse. A newer release's field this build refuses lands the same way.
const UNREADABLE_POLICY: &str = r#"{"n":1,"m":1,"trustees":"x"}"#;

/// The loader's error for [`UNREADABLE_POLICY`] (serde's, verbatim).
const UNREADABLE_POLICY_ERROR: &str =
    "invalid type: string \"x\", expected a sequence at line 1 column 27";

/// An on-disk database (the kernel is reopened), removed on drop.
struct TempDb(std::path::PathBuf);

impl TempDb {
    fn new(tag: &str) -> Self {
        Self(
            std::env::temp_dir().join(format!("m0_reader_{tag}_{:016x}.db", rand::random::<u64>())),
        )
    }
    fn path(&self) -> String {
        self.0.to_string_lossy().into_owned()
    }
}

impl Drop for TempDb {
    fn drop(&mut self) {
        for suffix in ["", "-wal", "-shm"] {
            let _ = std::fs::remove_file(format!("{}{suffix}", self.0.display()));
        }
    }
}

/// A database whose stored policy was bootstrapped (one history row) and
/// then overwritten with [`UNREADABLE_POLICY`].
fn database_with_unreadable_policy(db: &TempDb) -> Result<()> {
    let (mut kernel, _) = {
        let mut kernel = Kernel::open(&config(&db.path()))?;
        let policy = two_of_three();
        kernel.set_break_glass_policy_gated(&policy, &[], BUCKET)?;
        (kernel, policy)
    };
    assert_eq!(kernel.policy_change_history()?.len(), 1);
    kernel.conn.execute(
        "UPDATE break_glass_policy SET policy_json = ?1 WHERE id = 1",
        [UNREADABLE_POLICY],
    )?;
    kernel
        .conn
        .execute_batch("PRAGMA wal_checkpoint(TRUNCATE);")?;
    Ok(())
}

/// Test 6. The kernel opens over an unreadable policy, records the error,
/// refuses every break-glass action with it, and fails verification — the
/// report witnessd's boot check turns into safe mode.
#[test]
fn unreadable_policy_opens_and_refuses() -> Result<()> {
    let db = TempDb::new("opens");
    database_with_unreadable_policy(&db)?;

    let kernel = Kernel::open(&config(&db.path())).expect("Kernel::open succeeds");
    assert!(kernel.break_glass_policy().is_none());
    let refusal = format!(
        "break-glass policy could not be loaded: {UNREADABLE_POLICY_ERROR} — this build \
         refuses every break-glass action on this database (run log_verify; upgrade if the \
         policy was written by a newer release)"
    );
    assert_eq!(
        kernel.break_glass_policy_checked().unwrap_err().to_string(),
        refusal
    );

    // The verifier refuses the database: a failed verdict, never `valid`.
    let report = full_verify(&kernel);
    assert!(!report.chain_valid);
    assert_eq!(report.verdict, "verification failed");
    assert_eq!(report.error.as_deref(), Some(UNREADABLE_POLICY_ERROR));
    // witnessd's boot check (`verify_sealed_log`): `chain_valid: false` is
    // the safe-mode trigger (`src/bin/witnessd.rs`), as an `Err` would be.
    let boot = kernel.verify_sealed_log_with_hwm_path(None)?;
    assert!(!boot.chain_valid);
    assert_eq!(boot.error.as_deref(), Some(UNREADABLE_POLICY_ERROR));
    drop(kernel);

    // The console backend refuses with the same recorded error (its HTTP
    // handler answers 500 `policy_load_failed` on it).
    use crate::break_glass::BreakGlassOps;
    let ops = crate::break_glass::KernelVaultOps::open(config(&db.path()), "unused-vault")?;
    assert_eq!(ops.policy().unwrap_err().to_string(), refusal);
    Ok(())
}

/// Test 7. A policy set against an unreadable policy refuses — it is never
/// taken for "no policy" and bootstrapped over — and writes no history row.
#[test]
fn unreadable_policy_is_never_bootstrapped_over() -> Result<()> {
    let db = TempDb::new("bootstrap");
    database_with_unreadable_policy(&db)?;

    let mut kernel = Kernel::open(&config(&db.path()))?;
    let replacement = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("mallory"),
            public_key: SigningKey::from_bytes(&[0x44; 32])
                .verifying_key()
                .to_bytes(),
        }],
    )?;
    let err = kernel
        .set_break_glass_policy_gated(&replacement, &[], NEXT_BUCKET)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "refusing to change the break-glass policy: the stored policy could not be loaded \
             ({UNREADABLE_POLICY_ERROR}) — a quorum change needs the stored policy; it is never \
             treated as absent"
        )
    );
    assert_eq!(kernel.policy_change_history()?.len(), 1);
    let stored: String = kernel.conn.query_row(
        "SELECT policy_json FROM break_glass_policy WHERE id = 1",
        [],
        |r| r.get(0),
    )?;
    assert_eq!(stored, UNREADABLE_POLICY);
    assert!(kernel.break_glass_policy_checked().is_err());
    Ok(())
}
