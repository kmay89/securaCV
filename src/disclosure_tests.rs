//! Tests for the disclosure records the handoff-sidecar work adds: the
//! device-signed `Unsealed` row, `ExportReceipt.context` /
//! `authorizing_receipt`, the U1–U5 and R0–R6 rules in every walk, and the
//! pre-PR-D reader break the release coupling documents
//! (`docs/design/handoff_sidecars.md` D10).

use std::time::Duration;

use anyhow::Result;
use ed25519_dalek::SigningKey;
use sha2::{Digest, Sha256};

use crate::break_glass::{
    Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt, BreakGlassToken, OperatorContext,
    QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
use crate::verify::{FailedLedger, FailureKind, VerifyFailure};
use crate::{
    ExportAuthMode, ExportContext, ExportOptions, ExportReceipt, ExportWindow, Kernel,
    KernelConfig, TimeBucket, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

/// A kernel with a 1-of-1 policy (trustee `alice`).
pub(crate) struct Fixture {
    pub kernel: Kernel,
    pub cfg: KernelConfig,
    pub policy: QuorumPolicy,
    pub alice: SigningKey,
}

pub(crate) fn fixture() -> Result<Fixture> {
    let cfg = KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    let alice = SigningKey::from_bytes(&[41u8; 32]);
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: alice.verifying_key().to_bytes(),
        }],
    )?;
    kernel.set_break_glass_policy(&policy)?;
    Ok(Fixture {
        kernel,
        cfg,
        policy,
        alice,
    })
}

impl Fixture {
    /// A Granted receipt for `envelope` (with a recorded operator context)
    /// and its device-signed token.
    pub fn grant(&mut self, envelope: &str) -> Result<(BreakGlassToken, [u8; 32])> {
        let bucket = TimeBucket::now(600)?;
        let context = OperatorContext::new("Riley Ops", "incident-review", Some("CASE-7"), None)?;
        let request = UnlockRequest::new(envelope, self.cfg.ruleset_hash, "incident", bucket)?
            .with_context(context)?;
        let approval =
            Approval::signed(TrusteeId::new("alice"), request.request_hash(), &self.alice);
        let (result, receipt) = BreakGlass::authorize(
            &self.policy,
            &request,
            std::slice::from_ref(&approval),
            bucket,
        );
        let mut token = result?;
        let granted = self.kernel.log_break_glass_receipt(&receipt, &[approval])?;
        self.kernel.sign_break_glass_token(&mut token, granted)?;
        Ok((token, granted))
    }

    /// Grant + redemption for `envelope`; returns (grant, redemption).
    pub fn redeemed(&mut self, envelope: &str) -> Result<([u8; 32], [u8; 32])> {
        let (token, granted) = self.grant(envelope)?;
        let redemption = self.kernel.redeem_token_durably(&token)?;
        Ok((granted, redemption))
    }

    pub fn row(&self, h: &[u8; 32]) -> Result<BreakGlassReceipt> {
        let row = crate::break_glass_row_by_entry_hash(&self.kernel.conn, h)?.expect("row");
        Ok(serde_json::from_str(&row.payload_json)?)
    }

    pub fn verify_failure(&mut self) -> VerifyFailure {
        let report = self.kernel.verify_sealed_log().expect("report");
        assert!(!report.chain_valid, "expected a failed verdict");
        report.failure.expect("typed failure")
    }
}

fn bytes_lit(b: u8) -> String {
    format!(
        "[{}]",
        std::iter::repeat_n(b.to_string(), 32)
            .collect::<Vec<_>>()
            .join(",")
    )
}

// ---------------------------------------------------------------------------
// core.rs: the Unsealed variant (tests 1–4)
// ---------------------------------------------------------------------------

#[test]
fn unsealed_row_restates_its_redemption_and_refuses_the_wrong_one() -> Result<()> {
    let mut f = fixture()?;
    let (granted, redemption) = f.redeemed("vault:1")?;
    let red = f.row(&redemption)?;
    let row = BreakGlassReceipt::unsealed_of(&red, redemption, [5; 32], None)?;
    assert_eq!(row.vault_envelope_id, red.vault_envelope_id);
    assert_eq!(row.request_hash, red.request_hash);
    assert_eq!(row.ruleset_hash, red.ruleset_hash);
    assert_eq!(row.time_bucket, red.time_bucket);
    assert_eq!(row.policy_commitment, red.policy_commitment);
    assert!(row.trustees_used.is_empty());
    assert!(row.purpose.is_none() && row.context.is_none() && row.request_bucket.is_none());
    assert_eq!(row.unsealed_record(), Some((redemption, [5; 32], None)));
    // Refuses Granted, Denied and Unsealed inputs.
    let grant = f.row(&granted)?;
    let err = BreakGlassReceipt::unsealed_of(&grant, granted, [5; 32], None).unwrap_err();
    assert_eq!(
        err.to_string(),
        "cannot record an unseal of a receipt that is not a token redemption"
    );
    let mut denied = grant.clone();
    denied.outcome = BreakGlassOutcome::Denied {
        reason: "no".into(),
    };
    assert!(BreakGlassReceipt::unsealed_of(&denied, granted, [5; 32], None).is_err());
    assert!(BreakGlassReceipt::unsealed_of(&row, granted, [5; 32], None).is_err());
    // And an export:events redemption.
    let (_, export_red) = f.redeemed(EXPORT_EVENTS_ENVELOPE_ID)?;
    let err = BreakGlassReceipt::unsealed_of(&f.row(&export_red)?, export_red, [5; 32], None)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        "an export:events redemption is not an unseal — exports are receipted in the export chain"
    );
    // Serde round trip; no recipient key when absent.
    let json = serde_json::to_string(&row)?;
    assert!(!json.contains("recipient_commitment"), "{json}");
    let back: BreakGlassReceipt = serde_json::from_str(&json)?;
    assert_eq!(serde_json::to_string(&back)?, json);
    Ok(())
}

#[test]
fn unsealed_payload_bytes_pinned() -> Result<()> {
    let red = BreakGlassReceipt {
        vault_envelope_id: "vault:1".into(),
        request_hash: [6; 32],
        ruleset_hash: [7; 32],
        time_bucket: TimeBucket {
            start_epoch_s: 1_794_177_000,
            size_s: 600,
        },
        trustees_used: vec![],
        approvals_commitment: crate::approvals_commitment(&[]),
        policy_commitment: [8; 32],
        outcome: BreakGlassOutcome::TokenRedeemed {
            nonce: [9; 32],
            receipt_entry_hash: [10; 32],
        },
        purpose: None,
        context: None,
        request_bucket: None,
    };
    let approvals = bytes_lit_from(&crate::approvals_commitment(&[]));
    let prefix = format!(
        r#"{{"vault_envelope_id":"vault:1","request_hash":{},"ruleset_hash":{},"time_bucket":{{"start_epoch_s":1794177000,"size_s":600}},"trustees_used":[],"approvals_commitment":{},"policy_commitment":{},"outcome":{{"Unsealed":{{"redemption_entry_hash":{},"plaintext_commitment":{}"#,
        bytes_lit(6),
        bytes_lit(7),
        approvals,
        bytes_lit(8),
        bytes_lit(11),
        bytes_lit(12)
    );
    let tail = r#"}},"purpose":null,"context":null,"request_bucket":null}"#;
    let with = BreakGlassReceipt::unsealed_of(&red, [11; 32], [12; 32], Some([13; 32]))?;
    assert_eq!(
        serde_json::to_string(&with)?,
        format!("{prefix},\"recipient_commitment\":{}{tail}", bytes_lit(13))
    );
    let without = BreakGlassReceipt::unsealed_of(&red, [11; 32], [12; 32], None)?;
    assert_eq!(serde_json::to_string(&without)?, format!("{prefix}{tail}"));
    Ok(())
}

fn bytes_lit_from(b: &[u8; 32]) -> String {
    format!(
        "[{}]",
        b.iter()
            .map(|x| x.to_string())
            .collect::<Vec<_>>()
            .join(",")
    )
}

#[test]
fn render_human_labels_unseal_record_declared_recipient() -> Result<()> {
    let mut f = fixture()?;
    let (_, redemption) = f.redeemed("vault:1")?;
    let red = f.row(&redemption)?;
    let row = BreakGlassReceipt::unsealed_of(&red, redemption, [5; 32], Some([6; 32]))?;
    let text = row.render_human();
    assert!(text.contains("UNSEALED"), "{text}");
    assert!(text.contains(&format!(
        "redemption {}; plaintext committed (salted; opened only by the handoff sidecar); \
         recipient committed — declared by the operator, not verified",
        hex::encode(redemption)
    )));
    assert!(!text.contains("trustee-consented"), "{text}");
    let row = BreakGlassReceipt::unsealed_of(&red, redemption, [5; 32], None)?;
    assert!(!row.render_human().contains("recipient committed"));
    Ok(())
}

#[test]
fn gate_refuses_a_token_whose_receipt_is_an_unseal_row() -> Result<()> {
    let mut f = fixture()?;
    let (token, _) = f.grant("vault:1")?;
    let key = f.kernel.device_verifying_key();
    let err = BreakGlass::assert_token_valid(
        &token,
        "vault:1",
        f.cfg.ruleset_hash,
        token.expires_bucket(),
        &key,
        |_| {
            Ok(BreakGlassOutcome::Unsealed {
                redemption_entry_hash: [1; 32],
                plaintext_commitment: [2; 32],
                recipient_commitment: None,
            })
        },
    )
    .unwrap_err();
    assert_eq!(
        err.to_string(),
        "break-glass token points at an unseal record, not a grant"
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// lib.rs: ExportReceipt tail, record_unsealed, lookups (tests 5–15)
// ---------------------------------------------------------------------------

#[test]
fn export_receipt_generations_serialize_byte_identically() -> Result<()> {
    let head = format!(
        r#"{{"time_bucket":{{"start_epoch_s":600,"size_s":600}},"ruleset_hash":{},"batch_size":50,"artifact_hash":{}"#,
        bytes_lit(1),
        bytes_lit(2)
    );
    let base = ExportReceipt {
        time_bucket: TimeBucket {
            start_epoch_s: 600,
            size_s: 600,
        },
        ruleset_hash: [1; 32],
        batch_size: 50,
        artifact_hash: [2; 32],
        auth_mode: None,
        window: None,
        token_id: None,
        context: None,
        authorizing_receipt: None,
    };
    let win = Some(ExportWindow {
        start_epoch_s: 600,
        end_epoch_s: 1200,
    });
    let operator = OperatorContext {
        requester_name: "Dana Owner".into(),
        reason_code: "owner-recovery".into(),
        case_ref: Some("HOA-114".into()),
        requester_key: None,
    };
    let cases: Vec<(ExportReceipt, String)> = vec![
        (base.clone(), format!("{head}}}")),
        (
            ExportReceipt {
                auth_mode: Some(ExportAuthMode::Paired),
                window: win,
                token_id: Some("deadbeef".into()),
                ..base.clone()
            },
            format!(
                r#"{head},"auth_mode":"paired","window":{{"start_epoch_s":600,"end_epoch_s":1200}},"token_id":"deadbeef"}}"#
            ),
        ),
        (
            ExportReceipt {
                auth_mode: Some(ExportAuthMode::SelfExport),
                context: Some(ExportContext {
                    operator: Some(operator),
                    recipient_commitment: Some([3; 32]),
                }),
                ..base.clone()
            },
            format!(
                r#"{head},"auth_mode":"self_export","context":{{"operator":{{"requester_name":"Dana Owner","reason_code":"owner-recovery","case_ref":"HOA-114","requester_key":null}},"recipient_commitment":{}}}}}"#,
                bytes_lit(3)
            ),
        ),
        (
            ExportReceipt {
                auth_mode: Some(ExportAuthMode::BreakGlass),
                window: win,
                context: Some(ExportContext {
                    operator: None,
                    recipient_commitment: Some([3; 32]),
                }),
                authorizing_receipt: Some([4; 32]),
                ..base.clone()
            },
            format!(
                r#"{head},"auth_mode":"break_glass","window":{{"start_epoch_s":600,"end_epoch_s":1200}},"context":{{"recipient_commitment":{}}},"authorizing_receipt":{}}}"#,
                bytes_lit(3),
                bytes_lit(4)
            ),
        ),
        (
            ExportReceipt {
                auth_mode: Some(ExportAuthMode::BreakGlass),
                authorizing_receipt: Some([4; 32]),
                ..base.clone()
            },
            format!(
                r#"{head},"auth_mode":"break_glass","authorizing_receipt":{}}}"#,
                bytes_lit(4)
            ),
        ),
    ];
    for (receipt, want) in cases {
        let got = serde_json::to_string(&receipt)?;
        assert_eq!(got, want);
        let back: ExportReceipt = serde_json::from_str(&got)?;
        assert_eq!(serde_json::to_string(&back)?, got, "byte-stable round trip");
    }
    Ok(())
}

#[test]
fn export_context_new_is_none_when_empty_and_validates_operator() -> Result<()> {
    assert_eq!(ExportContext::new(None, None)?, None);
    assert!(ExportContext::new(None, Some([1; 32]))?.is_some());
    let bad = OperatorContext {
        requester_name: "x\u{202e}y".into(),
        reason_code: "incident-review".into(),
        case_ref: None,
        requester_key: None,
    };
    assert!(ExportContext::new(Some(bad), None).is_err());
    let bad_reason = OperatorContext {
        requester_name: "Dana".into(),
        reason_code: "not-a-code".into(),
        case_ref: None,
        requester_key: None,
    };
    assert!(ExportContext::new(Some(bad_reason), None).is_err());
    Ok(())
}

fn options() -> ExportOptions {
    ExportOptions {
        jitter_s: 0,
        ..ExportOptions::default()
    }
}

#[test]
fn self_export_with_context_signs_it_into_the_receipt() -> Result<()> {
    let mut f = fixture()?;
    let op = OperatorContext::new("Dana Owner", "owner-recovery", Some("HOA-114"), None)?;
    let ctx = ExportContext::new(Some(op.clone()), Some([3; 32]))?;
    let bundle = f
        .kernel
        .export_events_bundle_self_with(f.cfg.ruleset_hash, options(), ctx)?;
    crate::verify_export_bundle(&bundle)?;
    let receipt = &bundle.receipt_entry.receipt;
    assert_eq!(
        receipt.context.as_ref().unwrap().operator.as_ref(),
        Some(&op)
    );
    assert_eq!(receipt.authorizing_receipt, None);
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.export_receipts_authorized, None);
    Ok(())
}

#[test]
fn authorized_export_names_its_grant_and_carries_no_operator() -> Result<()> {
    for recipient in [None, Some([3u8; 32])] {
        let mut f = fixture()?;
        let (mut token, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
        let bundle = f.kernel.export_events_bundle_authorized_with(
            f.cfg.ruleset_hash,
            options(),
            &mut token,
            recipient,
        )?;
        let receipt = &bundle.receipt_entry.receipt;
        assert_eq!(receipt.authorizing_receipt, Some(granted));
        assert_eq!(receipt.auth_mode, Some(ExportAuthMode::BreakGlass));
        match recipient {
            Some(c) => {
                let ctx = receipt.context.as_ref().expect("context");
                assert!(ctx.operator.is_none());
                assert_eq!(ctx.recipient_commitment, Some(c));
            }
            None => assert!(receipt.context.is_none()),
        }
        let report = f.kernel.verify_sealed_log()?;
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.export_receipts_authorized, Some(1));
    }
    // The delegating entry point carries authorizing_receipt too.
    let mut f = fixture()?;
    let (mut token, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    let bundle =
        f.kernel
            .export_events_bundle_authorized(f.cfg.ruleset_hash, options(), &mut token)?;
    assert_eq!(
        bundle.receipt_entry.receipt.authorizing_receipt,
        Some(granted)
    );
    Ok(())
}

#[test]
fn plain_exports_carry_neither_new_key() -> Result<()> {
    let mut f = fixture()?;
    f.kernel.export_events_self(f.cfg.ruleset_hash, options())?;
    f.kernel
        .export_events_for_api(f.cfg.ruleset_hash, options())?;
    f.kernel.export_events_for_api_with(
        f.cfg.ruleset_hash,
        options(),
        crate::ApiCredential::Paired {
            token_id: "0badf00d".into(),
        },
    )?;
    let mut stmt = f
        .kernel
        .conn
        .prepare("SELECT payload_json FROM export_receipts")?;
    let payloads: Vec<String> = stmt
        .query_map([], |r| r.get(0))?
        .collect::<rusqlite::Result<_>>()?;
    assert_eq!(payloads.len(), 3);
    for p in payloads {
        assert!(
            !p.contains("\"context\"") && !p.contains("\"authorizing_receipt\""),
            "{p}"
        );
    }
    Ok(())
}

/// `ExportContext`'s fields are public, so a library caller can hand the
/// kernel an empty one. The kernel normalizes it to `None`: it never signs
/// `"context":{}`, and the receipt is the plain self-export's shape.
#[test]
fn an_empty_export_context_signs_no_context_key() -> Result<()> {
    let mut f = fixture()?;
    let empty = Some(ExportContext {
        operator: None,
        recipient_commitment: None,
    });
    let bundle = f
        .kernel
        .export_events_bundle_self_with(f.cfg.ruleset_hash, options(), empty)?;
    assert!(bundle.receipt_entry.receipt.context.is_none());
    f.kernel
        .export_events_bundle_self(f.cfg.ruleset_hash, options())?;
    let payloads: Vec<String> = {
        let mut stmt = f
            .kernel
            .conn
            .prepare("SELECT payload_json FROM export_receipts ORDER BY id")?;
        let rows = stmt
            .query_map([], |r| r.get(0))?
            .collect::<rusqlite::Result<_>>()?;
        rows
    };
    assert_eq!(payloads.len(), 2);
    assert!(!payloads[0].contains("\"context\""), "{}", payloads[0]);
    // Byte-identical in shape to the plain self-export (the same keys in
    // the same order; only the receipt's own values may differ).
    let keys = |p: &str| -> Vec<String> {
        serde_json::from_str::<serde_json::Map<String, serde_json::Value>>(p)
            .unwrap()
            .keys()
            .cloned()
            .collect()
    };
    assert_eq!(keys(&payloads[0]), keys(&payloads[1]));
    assert_eq!(
        serde_json::to_string(&bundle.receipt_entry.receipt)?,
        payloads[0]
    );
    // An operator that fails validation is still refused at this boundary.
    let bad = Some(ExportContext {
        operator: Some(OperatorContext {
            requester_name: " ".into(),
            reason_code: "owner-recovery".into(),
            case_ref: None,
            requester_key: None,
        }),
        recipient_commitment: None,
    });
    assert!(f
        .kernel
        .export_events_bundle_self_with(f.cfg.ruleset_hash, options(), bad)
        .is_err());
    Ok(())
}

#[test]
fn record_unsealed_appends_after_the_redemption_and_walks_green() -> Result<()> {
    let mut f = fixture()?;
    let (_, redemption) = f.redeemed("vault:1")?;
    let unsealed = f
        .kernel
        .record_unsealed(redemption, [5; 32], Some([6; 32]))?;
    let row = crate::break_glass_row_by_entry_hash(&f.kernel.conn, &unsealed)?.expect("row");
    assert_eq!(
        row.prev_hash, redemption,
        "chained right after the redemption"
    );
    assert_eq!(row.approvals_json.as_deref(), Some("[]"));
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.break_glass_unsealed, Some(1));
    assert_eq!(report.break_glass_redeemed, 2 - 1);
    Ok(())
}

fn bg_rows(k: &Kernel) -> i64 {
    k.conn
        .query_row("SELECT COUNT(*) FROM break_glass_receipts", [], |r| {
            r.get(0)
        })
        .unwrap()
}

#[test]
fn record_unsealed_refuses_missing_non_redemption_duplicate_and_export_redemption_and_rolls_back(
) -> Result<()> {
    let mut f = fixture()?;
    let (granted, redemption) = f.redeemed("vault:1")?;
    let before = bg_rows(&f.kernel);
    let err = f
        .kernel
        .record_unsealed([9; 32], [5; 32], None)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "unseal record refused: receipt {} is not in the receipt chain",
            "09".repeat(32)
        )
    );
    let err = f
        .kernel
        .record_unsealed(granted, [5; 32], None)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "unseal record refused: receipt {} is not a token redemption row",
            hex::encode(granted)
        )
    );
    assert_eq!(bg_rows(&f.kernel), before, "a refusal appends nothing");
    f.kernel.record_unsealed(redemption, [5; 32], None)?;
    let err = f
        .kernel
        .record_unsealed(redemption, [5; 32], None)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "unseal record refused: redemption {} already has an unseal record",
            hex::encode(redemption)
        )
    );
    let (_, export_red) = f.redeemed(EXPORT_EVENTS_ENVELOPE_ID)?;
    let before = bg_rows(&f.kernel);
    let err = f
        .kernel
        .record_unsealed(export_red, [5; 32], None)
        .unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "unseal record refused: redemption {} spent an export:events token — exports are \
             receipted in the export chain",
            hex::encode(export_red)
        )
    );
    assert_eq!(bg_rows(&f.kernel), before);
    assert!(f.kernel.conn.is_autocommit(), "the transaction was closed");
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    Ok(())
}

/// Re-sign a forged export receipt over a real artifact (the device key
/// holder's trust level), so only the R rules decide.
fn forged_bundle(
    f: &mut Fixture,
    mutate: impl FnOnce(&mut ExportReceipt),
) -> Result<crate::ExportBundle> {
    let mut bundle = f
        .kernel
        .export_events_bundle_self(f.cfg.ruleset_hash, options())?;
    let mut receipt = bundle.receipt_entry.receipt.clone();
    mutate(&mut receipt);
    f.kernel.append_export_receipt(&receipt)?;
    bundle.receipt_entry = f.kernel.latest_export_receipt_entry()?;
    Ok(bundle)
}

#[test]
fn verify_export_bundle_applies_r0_r1_r6() -> Result<()> {
    let mut f = fixture()?;
    let bundle = forged_bundle(&mut f, |r| {
        r.authorizing_receipt = Some([4; 32]);
    })?;
    let err = crate::verify_export_bundle(&bundle).unwrap_err();
    assert_eq!(
        err.to_string(),
        "export receipt names an authorizing receipt but its auth_mode is self_export, not break_glass"
    );
    let bundle = forged_bundle(&mut f, |r| {
        r.auth_mode = None;
        r.authorizing_receipt = Some([4; 32]);
    })?;
    assert_eq!(
        crate::verify_export_bundle(&bundle)
            .unwrap_err()
            .to_string(),
        "export receipt names an authorizing receipt but its auth_mode is absent, not break_glass"
    );
    let bundle = forged_bundle(&mut f, |r| {
        r.auth_mode = Some(ExportAuthMode::BreakGlass);
        r.authorizing_receipt = Some([4; 32]);
        r.context = Some(ExportContext {
            operator: Some(OperatorContext::new("Dana", "incident-review", None, None).unwrap()),
            recipient_commitment: None,
        });
    })?;
    assert_eq!(
        crate::verify_export_bundle(&bundle)
            .unwrap_err()
            .to_string(),
        "export receipt carries an operator context beside its authorizing receipt (a quorum \
         export's operator is its grant's)"
    );
    // R0 is structural over the JSON (a typed receipt cannot hold it).
    let v = serde_json::json!({"auth_mode": "break_glass", "authorizing_receipt": [1, 2]});
    assert_eq!(
        crate::verify::export_authorization_shape(&v).unwrap_err(),
        "export receipt's authorizing_receipt is not a 32-byte array"
    );
    Ok(())
}

#[test]
fn lookups_select_by_hash_only() -> Result<()> {
    let mut f = fixture()?;
    let (granted, redemption) = f.redeemed("vault:1")?;
    let unsealed = f.kernel.record_unsealed(redemption, [5; 32], None)?;
    assert_eq!(
        crate::break_glass_redemption_for_grant(&f.kernel.conn, &granted)?,
        Some(redemption)
    );
    assert_eq!(
        crate::break_glass_unsealed_for_redemption(&f.kernel.conn, &redemption)?,
        Some(unsealed)
    );
    assert_eq!(
        crate::break_glass_redemption_for_grant(&f.kernel.conn, &[9; 32])?,
        None
    );
    let row = crate::break_glass_row_by_entry_hash(&f.kernel.conn, &granted)?.expect("row");
    // StoredReceiptRow has no id and no created_at field: its Debug names
    // every field it carries.
    let debug = format!("{row:?}");
    assert!(
        !debug.contains("created_at") && !debug.contains(" id:"),
        "{debug}"
    );
    let source = include_str!("lib.rs");
    let start = source
        .find("pub(crate) fn break_glass_row_by_entry_hash")
        .unwrap();
    let end = source[start..].find("pub fn verify_export_bundle").unwrap() + start;
    let block = &source[start..end];
    assert!(
        !block.contains("created_at") && !block.contains("SELECT id"),
        "{block}"
    );
    Ok(())
}

/// D10: a #1936-era reader (strict serde over this enum WITHOUT `Unsealed`)
/// fails on a post-PR-D row. This pins the documented release coupling:
/// PR D must merge before any release containing #1936, and every reader
/// is upgraded before the first post-PR-D unseal.
#[test]
fn pre_d_reader_rejects_unsealed_rows() -> Result<()> {
    #[derive(serde::Deserialize, Debug)]
    #[allow(dead_code)]
    enum PreDOutcome {
        Granted,
        Denied {
            reason: String,
        },
        TokenRedeemed {
            nonce: [u8; 32],
            receipt_entry_hash: [u8; 32],
        },
    }
    #[derive(serde::Deserialize, Debug)]
    #[allow(dead_code)]
    struct PreDReceipt {
        outcome: PreDOutcome,
    }
    let mut f = fixture()?;
    let (_, redemption) = f.redeemed("vault:1")?;
    let unsealed = f.kernel.record_unsealed(redemption, [5; 32], None)?;
    let row = crate::break_glass_row_by_entry_hash(&f.kernel.conn, &unsealed)?.unwrap();
    let err = serde_json::from_str::<PreDReceipt>(&row.payload_json).unwrap_err();
    assert!(
        err.to_string().contains("unknown variant `Unsealed`"),
        "{err}"
    );
    // The redemption row itself still parses under the old reader.
    let red = crate::break_glass_row_by_entry_hash(&f.kernel.conn, &redemption)?.unwrap();
    serde_json::from_str::<PreDReceipt>(&red.payload_json)?;
    Ok(())
}

#[test]
fn plaintext_commitment_never_stored_unsalted() -> Result<()> {
    let mut f = fixture()?;
    let (_, redemption) = f.redeemed("vault:1")?;
    let plaintext = b"securacv-fixture";
    let salt = crate::handoff::fresh_salt();
    let commitment = crate::handoff::plaintext_commitment(&salt, plaintext);
    f.kernel.record_unsealed(redemption, commitment, None)?;
    let digest: [u8; 32] = Sha256::digest(plaintext).into();
    let as_ints = bytes_lit_from(&digest);
    let as_hex = hex::encode(digest);
    let mut stmt = f.kernel.conn.prepare(
        "SELECT payload_json, approvals_json, prev_hash, entry_hash FROM break_glass_receipts",
    )?;
    let rows: Vec<(String, String, Vec<u8>, Vec<u8>)> = stmt
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?
        .collect::<rusqlite::Result<_>>()?;
    for (payload, approvals, prev, entry) in rows {
        for text in [&payload, &approvals] {
            assert!(
                !text.contains(&as_ints) && !text.contains(&as_hex),
                "{text}"
            );
            assert!(!text.contains(&hex::encode(salt)));
        }
        assert_ne!(prev, digest.to_vec());
        assert_ne!(entry, digest.to_vec());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// verify.rs / verify_runner.rs / envelope.rs (tests 16–27)
// ---------------------------------------------------------------------------

#[test]
fn unseal_rows_walk_green_and_are_counted() -> Result<()> {
    let mut f = fixture()?;
    for _ in 0..2 {
        let (_, red) = f.redeemed("vault:1")?;
        f.kernel.record_unsealed(red, [5; 32], None)?;
    }
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.break_glass_unsealed, Some(2));
    assert_eq!(report.break_glass_granted, 2);
    assert_eq!(report.break_glass_redeemed, 2);
    Ok(())
}

fn assert_redemption_failure(f: &mut Fixture, want: &str) {
    let failure = f.verify_failure();
    assert_eq!(failure.ledger, FailedLedger::BreakGlassReceipts);
    assert_eq!(failure.kind, FailureKind::TokenRedemption);
    assert!(
        failure
            .detail
            .starts_with("integrity check failed at receipt id ")
            && failure.detail.ends_with(&format!(": {want}")),
        "{}",
        failure.detail
    );
}

#[test]
fn u1_an_unseal_record_naming_no_redemption_fails() -> Result<()> {
    let mut f = fixture()?;
    let (granted, redemption) = f.redeemed("vault:1")?;
    let mut forged = BreakGlassReceipt::unsealed_of(&f.row(&redemption)?, [9; 32], [5; 32], None)?;
    forged.outcome = BreakGlassOutcome::Unsealed {
        redemption_entry_hash: granted,
        plaintext_commitment: [5; 32],
        recipient_commitment: None,
    };
    f.kernel.append_break_glass_receipt(&forged, &[])?;
    assert_redemption_failure(
        &mut f,
        &format!(
            "unseal record names receipt {} but no earlier redemption row has that hash",
            hex::encode(granted)
        ),
    );
    Ok(())
}

#[test]
fn u2_a_second_unseal_record_for_one_redemption_fails() -> Result<()> {
    let mut f = fixture()?;
    let (_, redemption) = f.redeemed("vault:1")?;
    let row = BreakGlassReceipt::unsealed_of(&f.row(&redemption)?, redemption, [5; 32], None)?;
    f.kernel.append_break_glass_receipt(&row, &[])?;
    f.kernel.append_break_glass_receipt(&row, &[])?;
    assert_redemption_failure(
        &mut f,
        &format!(
            "redemption {} is recorded as unsealed twice — a token releases one cleartext",
            hex::encode(redemption)
        ),
    );
    Ok(())
}

#[test]
fn u3_u4_an_export_or_misstated_unseal_record_fails() -> Result<()> {
    // U3: an unseal of an export:events redemption.
    let mut f = fixture()?;
    let (_, red) = f.redeemed(EXPORT_EVENTS_ENVELOPE_ID)?;
    assert!(BreakGlassReceipt::unsealed_of(&f.row(&red)?, red, [5; 32], None).is_err());
    let mut row = f.row(&red)?;
    row.outcome = BreakGlassOutcome::Unsealed {
        redemption_entry_hash: red,
        plaintext_commitment: [5; 32],
        recipient_commitment: None,
    };
    f.kernel.append_break_glass_receipt(&row, &[])?;
    assert_redemption_failure(
        &mut f,
        "unseal record names the redemption of an export:events token",
    );
    // U4: a wrong envelope, then a wrong bucket.
    let mut f = fixture()?;
    let (_, red) = f.redeemed("vault:1")?;
    let mut row = BreakGlassReceipt::unsealed_of(&f.row(&red)?, red, [5; 32], None)?;
    row.vault_envelope_id = "vault:2".into();
    f.kernel.append_break_glass_receipt(&row, &[])?;
    assert_redemption_failure(
        &mut f,
        "unseal record does not restate the envelope, ruleset and request hash of the redemption \
         it records",
    );
    let mut f = fixture()?;
    let (_, red) = f.redeemed("vault:1")?;
    let mut row = BreakGlassReceipt::unsealed_of(&f.row(&red)?, red, [5; 32], None)?;
    row.time_bucket.start_epoch_s += 600;
    f.kernel.append_break_glass_receipt(&row, &[])?;
    assert_redemption_failure(
        &mut f,
        "unseal record is stamped outside its redemption's bucket",
    );
    Ok(())
}

/// U5 in both walks: the database walk applies an unseal record's U-rules
/// before its per-receipt quorum and attribution check, so a row naming
/// trustees fails as `token_redemption` there too, with the same reason the
/// envelope walk gives.
#[test]
fn u5_an_unseal_record_naming_trustees_fails() -> Result<()> {
    use crate::crypto::signatures::SignatureMode;
    let mut f = fixture()?;
    let (_, red) = f.redeemed("vault:1")?;
    let mut row = BreakGlassReceipt::unsealed_of(&f.row(&red)?, red, [5; 32], None)?;
    row.trustees_used = vec![TrusteeId::new("alice")];
    f.kernel.append_break_glass_receipt(&row, &[])?;
    let failure = f.verify_failure();
    assert_eq!(failure.ledger, FailedLedger::BreakGlassReceipts);
    assert_eq!(failure.kind, FailureKind::TokenRedemption);
    let id = failure.entry_id.expect("row id");
    assert_eq!(
        failure.detail,
        format!(
            "integrity check failed at receipt id {id}: unseal record carries approvals or names \
             trustees"
        )
    );
    let bundle = f
        .kernel
        .export_events_bundle_self(f.cfg.ruleset_hash, options())?;
    let envelope = f.kernel.evidence_envelope_from_bundle(
        bundle,
        "ruleset:test",
        "0.0.0-test",
        f.cfg.ruleset_hash,
    )?;
    let err = crate::verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
    let failure = err.downcast_ref::<VerifyFailure>().expect("typed");
    assert_eq!(failure.kind, FailureKind::TokenRedemption);
    assert_eq!(
        failure.detail,
        "break-glass token redemption invalid: unseal record carries approvals or names trustees"
    );
    Ok(())
}

#[test]
fn export_authorizations_pass_for_a_redeemed_export_grant() -> Result<()> {
    let mut f = fixture()?;
    let (mut token, _) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    f.kernel
        .export_events_authorized_with(f.cfg.ruleset_hash, options(), &mut token, None)?;
    assert_eq!(
        crate::verify::verify_export_authorizations(&f.kernel.conn)?,
        1
    );
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.export_receipts_authorized, Some(1));
    Ok(())
}

fn assert_authorization_failure(f: &mut Fixture, want: &str) {
    let failure = f.verify_failure();
    assert_eq!(failure.ledger, FailedLedger::ExportReceipts);
    assert_eq!(failure.kind, FailureKind::ExportAuthorization);
    assert!(
        failure
            .detail
            .starts_with("integrity check failed at export receipt id ")
            && failure.detail.ends_with(&format!(": {want}")),
        "{}",
        failure.detail
    );
}

fn forge_export(f: &mut Fixture, mutate: impl FnOnce(&mut ExportReceipt)) -> Result<()> {
    let mut receipt = ExportReceipt {
        time_bucket: TimeBucket::now(600)?,
        ruleset_hash: f.cfg.ruleset_hash,
        batch_size: 50,
        artifact_hash: [2; 32],
        auth_mode: Some(ExportAuthMode::BreakGlass),
        window: None,
        token_id: None,
        context: None,
        authorizing_receipt: None,
    };
    mutate(&mut receipt);
    f.kernel.append_export_receipt(&receipt)?;
    Ok(())
}

#[test]
fn r1_r2_r6_export_authorization_failures() -> Result<()> {
    let mut f = fixture()?;
    forge_export(&mut f, |r| {
        r.auth_mode = Some(ExportAuthMode::Api);
        r.authorizing_receipt = Some([4; 32]);
    })?;
    assert_authorization_failure(
        &mut f,
        "export receipt names an authorizing receipt but its auth_mode is api, not break_glass",
    );
    let mut f = fixture()?;
    forge_export(&mut f, |r| r.authorizing_receipt = Some([4; 32]))?;
    assert_authorization_failure(
        &mut f,
        &format!(
            "export receipt names authorizing receipt {} but no Granted break-glass receipt has \
             that hash",
            "04".repeat(32)
        ),
    );
    let mut f = fixture()?;
    let (_, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    forge_export(&mut f, |r| {
        r.authorizing_receipt = Some(granted);
        r.context = Some(ExportContext {
            operator: Some(OperatorContext::new("Dana", "incident-review", None, None).unwrap()),
            recipient_commitment: None,
        });
    })?;
    assert_authorization_failure(
        &mut f,
        "export receipt carries an operator context beside its authorizing receipt (a quorum \
         export's operator is its grant's)",
    );
    Ok(())
}

/// R0 reads the signed payload's own integers (serde_json's `as_u64`):
/// a float-encoded `authorizing_receipt` (`4.0`, `4e0`, `-0`) fails R0 even
/// though a JSON parser can read it as integers. viewer/verify_core.test.js
/// pins the same payload texts to the same string.
#[test]
fn r0_refuses_a_float_encoded_authorizing_receipt() {
    let ledger = crate::verify::RedemptionLedger::default();
    let r0 = "export receipt's authorizing_receipt is not a 32-byte array";
    for element in ["4.0", "4e0", "-0"] {
        let mut items = ["4"; 32];
        items[7] = element;
        let payload = format!(
            "{{\"auth_mode\":\"break_glass\",\"authorizing_receipt\":[{}]}}",
            items.join(",")
        );
        assert_eq!(
            crate::verify::check_export_authorizations(&ledger, [(0i64, payload.as_str())]),
            Err((0, r0.to_string())),
            "{payload}"
        );
    }
    // All integers: R0 passes and R2 (no such grant) is next.
    let payload = format!(
        "{{\"auth_mode\":\"break_glass\",\"authorizing_receipt\":[{}]}}",
        ["4"; 32].join(",")
    );
    let (_, why) = crate::verify::check_export_authorizations(&ledger, [(0i64, payload.as_str())])
        .unwrap_err();
    assert!(
        why.starts_with("export receipt names authorizing receipt 0404"),
        "{why}"
    );
}

/// R1 echoes a forged `auth_mode` through display_safe (control and
/// text-direction characters become U+FFFD), so a forged mode can never
/// forge or hide a verdict line; a non-string mode is named by kind. The
/// JS mirror prints the same strings (viewer/verify_core.test.js).
#[test]
fn r1_sanitizes_a_forged_auth_mode() {
    let ledger = crate::verify::RedemptionLedger::default();
    let bytes = ["4"; 32].join(",");
    let payload = format!(
        "{{\"auth_mode\":\"x\\u001b[2J\\nOK: chain valid\\u202e\",\"authorizing_receipt\":[{bytes}]}}"
    );
    assert_eq!(
        crate::verify::check_export_authorizations(&ledger, [(0i64, payload.as_str())]),
        Err((
            0,
            "export receipt names an authorizing receipt but its auth_mode is \
             x\u{fffd}[2J\u{fffd}OK: chain valid\u{fffd}, not break_glass"
                .to_string()
        ))
    );
    let payload = format!("{{\"auth_mode\":1.0,\"authorizing_receipt\":[{bytes}]}}");
    assert_eq!(
        crate::verify::check_export_authorizations(&ledger, [(0i64, payload.as_str())]),
        Err((
            0,
            "export receipt names an authorizing receipt but its auth_mode is a non-string \
             value, not break_glass"
                .to_string()
        ))
    );
}

#[test]
fn r3_r4_export_authorization_failures() -> Result<()> {
    let mut f = fixture()?;
    let (granted, _) = f.redeemed("vault:1")?;
    forge_export(&mut f, |r| r.authorizing_receipt = Some(granted))?;
    assert_authorization_failure(
        &mut f,
        &format!(
            "export receipt's authorizing grant {} is for envelope 'vault:1', not export:events",
            hex::encode(granted)
        ),
    );
    let mut f = fixture()?;
    let (_, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    forge_export(&mut f, |r| r.authorizing_receipt = Some(granted))?;
    assert_authorization_failure(
        &mut f,
        &format!(
            "export receipt's authorizing grant {} has no redemption row — its token was never \
             spent",
            hex::encode(granted)
        ),
    );
    Ok(())
}

#[test]
fn r5_one_grant_authorizes_one_export() -> Result<()> {
    let mut f = fixture()?;
    let (mut token, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    f.kernel
        .export_events_authorized_with(f.cfg.ruleset_hash, options(), &mut token, None)?;
    forge_export(&mut f, |r| r.authorizing_receipt = Some(granted))?;
    let failure = f.verify_failure();
    assert_eq!(failure.kind, FailureKind::ExportAuthorization);
    assert!(
        failure.detail.ends_with(&format!(
            "both name authorizing grant {} — a grant authorizes one export",
            hex::encode(granted)
        )),
        "{}",
        failure.detail
    );
    Ok(())
}

#[test]
fn legacy_quorum_export_without_authorizing_receipt_passes() -> Result<()> {
    let mut f = fixture()?;
    let (_, _) = f.redeemed(EXPORT_EVENTS_ENVELOPE_ID)?;
    forge_export(&mut f, |_| {})?;
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.export_receipts_authorized, None);
    Ok(())
}

#[test]
fn verify_report_omits_new_counts_when_zero() -> Result<()> {
    let mut f = fixture()?;
    f.redeemed("vault:1")?;
    f.kernel.export_events_self(f.cfg.ruleset_hash, options())?;
    let report = f.kernel.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    let json = serde_json::to_string(&report)?;
    assert!(
        !json.contains("break_glass_unsealed") && !json.contains("export_receipts_authorized"),
        "{json}"
    );
    Ok(())
}

#[test]
fn envelope_walk_runs_u_and_r_rules() -> Result<()> {
    use crate::crypto::signatures::SignatureMode;
    let mut f = fixture()?;
    let (_, red) = f.redeemed("vault:1")?;
    f.kernel.record_unsealed(red, [5; 32], None)?;
    let (mut token, _) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    let bundle = f.kernel.export_events_bundle_authorized_with(
        f.cfg.ruleset_hash,
        options(),
        &mut token,
        Some([3; 32]),
    )?;
    let envelope = f.kernel.evidence_envelope_from_bundle(
        bundle,
        "ruleset:test",
        "0.0.0-test",
        f.cfg.ruleset_hash,
    )?;
    let report = crate::verify_envelope(&envelope, SignatureMode::Compat)?;
    assert_eq!(
        (
            report.break_glass_unsealed,
            report.export_receipts_authorized
        ),
        (1, 1)
    );

    // A re-signed export receipt whose authorizing_receipt names nothing.
    let mut f = fixture()?;
    let bundle = forged_bundle(&mut f, |r| {
        r.auth_mode = Some(ExportAuthMode::BreakGlass);
        r.authorizing_receipt = Some([4; 32]);
    })?;
    let envelope = f.kernel.evidence_envelope_from_bundle(
        bundle,
        "ruleset:test",
        "0.0.0-test",
        f.cfg.ruleset_hash,
    )?;
    let err = crate::verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
    let failure = err.downcast_ref::<VerifyFailure>().expect("typed");
    assert_eq!(failure.kind, FailureKind::ExportAuthorization);
    assert_eq!(failure.ledger, FailedLedger::ExportReceipts);
    assert_eq!(
        failure.detail,
        format!(
            "export receipt authorization invalid: export receipt names authorizing receipt {} \
             but no Granted break-glass receipt has that hash",
            "04".repeat(32)
        )
    );
    Ok(())
}

#[test]
fn record_unsealed_inside_a_callers_transaction_uses_a_savepoint() -> Result<()> {
    let mut f = fixture()?;
    let (_, red) = f.redeemed("vault:1")?;
    f.kernel.conn.execute_batch("BEGIN IMMEDIATE;")?;
    f.kernel.record_unsealed(red, [5; 32], None)?;
    assert!(!f.kernel.conn.is_autocommit());
    f.kernel.conn.execute_batch("ROLLBACK;")?;
    assert_eq!(
        crate::break_glass_unsealed_for_redemption(&f.kernel.conn, &red)?,
        None
    );
    Ok(())
}
