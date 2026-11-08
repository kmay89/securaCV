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
    ClockProvenance { id: i64, entry_hash: [u8; 32] },
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
    /// Number of `tsa_anchors` rows that passed the structural checks (row well-formed, token parses, imprint equals the
    /// row's subject hash, and — for head subjects — the hash is in its declared ledger's history, a checkpoint witness
    /// only if that checkpoint's device signature verifies). `None` when the database has no anchors table; `Some(n)`
    /// on a failed report counts the rows checked before the failing one. Structural only: the TSA countersignature is
    /// checked by `log_anchor verify --ca` (openssl), never here. No times, no hashes: this report is published
    /// retained over MQTT.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub anchors_checked: Option<u64>,
    /// Number of `ots_anchors` rows (OpenTimestamps proofs) that passed the
    /// same structural checks (row well-formed, proof parses, its digest
    /// equals the row's subject hash, the cached attestation columns agree
    /// with the proof, and — for head subjects — the hash is in its declared
    /// ledger's history). Same contract as `anchors_checked`: `None` when the
    /// table is absent (so every existing report is byte-identical), `Some(n)`
    /// on success, on a failed report the rows checked before the failing
    /// one. Never a Bitcoin check: there is no header input here. No heights,
    /// no times, no hashes. `anchors_checked` stays RFC 3161 only.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub ots_anchors_checked: Option<u64>,
    /// Number of `clock_provenance` rows (device clock vs TSA `genTime`, one
    /// device-signed record per anchor row; `src/clock_provenance.rs`) whose
    /// hash chain and device signatures verified. `None` when the ledger has
    /// no rows — table absent or empty, so a database that never recorded
    /// one serializes exactly as before — and `None` on a failed report.
    /// Additive; the assessment of each record (offset bands, orphans) is
    /// warnings only, printed by `log_anchor verify` in full.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub clock_provenance_verified: Option<u64>,
    pub events_verified: u64,
    pub break_glass_receipts_verified: u64,
    pub break_glass_granted: u64,
    pub break_glass_denied: u64,
    /// `TokenRedeemed` rows: tokens actually spent at the unseal gate.
    pub break_glass_redeemed: u64,
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
            anchors_checked: None,
            ots_anchors_checked: None,
            clock_provenance_verified: None,
            events_verified: 0,
            break_glass_receipts_verified: 0,
            break_glass_granted: 0,
            break_glass_denied: 0,
            break_glass_redeemed: 0,
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
            // EXTEND: the anchor audit's warnings are already in the report.
            match verify::audit_chain_timeline(conn, bucket.start_epoch_s) {
                Ok(warnings) => report.warnings.extend(warnings),
                Err(err) => report
                    .warnings
                    .push(format!("timeline audit could not run: {err:#}")),
            }
        }
        Err(err) => {
            report.chain_valid = false;
            report.error = Some(format!("{err:#}"));
            report.failure = err.downcast_ref::<verify::VerifyFailure>().cloned();
            // A count of verified records has no meaning on a failed report.
            report.clock_provenance_verified = None;
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
    let checkpoint_binding = verify::verify_checkpoint_signature(
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
    report.break_glass_redeemed = counts.redeemed;

    report.export_receipts_verified = verify::verify_export_receipts_with(
        conn,
        &lineage_keys,
        signature_mode,
        pq_verifying_key.as_ref(),
        |id, entry_hash| on_item(VerifiedItem::ExportReceipt { id, entry_hash }),
    )?;

    // The clock-provenance ledger (device clock vs TSA genTime, one
    // device-signed record per anchor row). A chain or signature failure is
    // a verdict failure like any other device-signed ledger's; what the
    // records SAY is cross-checked against the anchors table below, as
    // warnings only.
    let clock_rows = verify::verify_clock_provenance_with(
        conn,
        &lineage_keys,
        signature_mode,
        pq_verifying_key.as_ref(),
        |id, entry_hash| on_item(VerifiedItem::ClockProvenance { id, entry_hash }),
    )?;
    report.clock_provenance_verified = (clock_rows > 0).then_some(clock_rows);

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
            // The checkpoint's cutoff stands in for the live high only when its
            // signature binds it. A legacy checkpoint's cutoff is an unsigned
            // column: a no-key actor who wiped the table could replay an old
            // signed checkpoint with an inflated cutoff and pass as a prune that
            // advanced past the mark. For a legacy checkpoint the one case its
            // signed head itself proves is the prune that stopped exactly at the
            // mark; anything else cannot be told from a wipe, and the mark was
            // deployed to say so.
            None => match checkpoint_binding {
                verify::CheckpointBinding::CutoffSigned => (
                    checkpoint.cutoff_event_id.unwrap_or(0),
                    checkpoint.chain_head_hash,
                ),
                _ if checkpoint.chain_head_hash == Some(hwm.head_hash) => {
                    (hwm.seq as i64, checkpoint.chain_head_hash)
                }
                _ => {
                    return Err(anyhow::Error::new(verify::VerifyFailure {
                        ledger: verify::FailedLedger::HighWaterMark,
                        entry_id: None,
                        kind: verify::FailureKind::HighWaterRegression,
                        detail: format!(
                            "sealed-log table is empty and the checkpoint does not sign its \
                             cutoff (written before cutoffs were bound), so its cutoff {} cannot \
                             stand in for the live high against the signed high-water {}: a \
                             full-retention prune cannot be told from a wipe",
                            checkpoint.cutoff_event_id.unwrap_or(0),
                            hwm.seq
                        ),
                    }));
                }
            },
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

    // RFC 3161 anchors (ENTERPRISE_CUSTODY §2, structural). An anchor is
    // auxiliary evidence ABOUT the ledgers: a head-kind row whose hash is no
    // longer in its declared ledger's history is a rollback, a truncation, or
    // a different history than the one that was anchored, so it fails the
    // verdict. Membership is keyed on signed evidence only — a live row, or a
    // checkpoint whose device signature verifies under a lineage key; the
    // unsigned `ledger_id` column only sharpens the diagnosis text. A damaged
    // row or token is a WARNING: it cannot vouch for anything, but it is not
    // evidence of a rewritten ledger, and one corrupt auxiliary row must not
    // boot witnessd into safe mode. The TSA countersignature is never checked
    // here (`log_anchor verify --ca`, openssl). Runs after the high-water-mark
    // so an anchor failure reports on top of an otherwise-consistent chain.
    // The anchors pass and the clock-provenance cross-check below read from
    // ONE snapshot: a SAVEPOINT nests inside any transaction the caller
    // holds, and outside one it is a deferred read transaction that blocks no
    // WAL writer. witnessd may insert an anchor row and seal its record
    // between two separate reads; read apart, that record would be an orphan.
    let snapshot = ReadSnapshot::begin(conn, "verify_anchor_snapshot")?;
    let mut anchor_warnings = Vec::new();
    // The rows that decoded, and the ids that did not, for the
    // clock-provenance cross-check below.
    let mut decoded_anchors: Vec<crate::tsa::AnchorRecord> = Vec::new();
    let mut malformed_anchor_ids: Vec<i64> = Vec::new();
    if crate::tsa::anchor_table_exists(conn)? {
        report.anchors_checked = Some(0);
        for (id, row) in crate::tsa::list_anchors_lenient(conn)? {
            let record = match row {
                Ok(record) => record,
                Err(err) => {
                    let reason = strip_corrupt_prefix(&err.to_string(), id);
                    anchor_warnings.push(format!(
                        "anchor #{id}: row is malformed ({reason}); run log_anchor verify"
                    ));
                    malformed_anchor_ids.push(id);
                    continue;
                }
            };
            decoded_anchors.push(record.clone());
            let token = match crate::tsa::parse_token(&record.token_der) {
                Ok(token) => token,
                Err(_) => {
                    anchor_warnings.push(format!(
                        "anchor #{id}: stored token does not parse as an RFC 3161 token; run \
                         log_anchor verify"
                    ));
                    continue;
                }
            };
            if token.imprint.as_slice() != record.subject_hash.as_slice() {
                anchor_warnings.push(format!(
                    "anchor #{id}: token imprint does not match the subject hash the row claims; \
                     run log_anchor verify"
                ));
                continue;
            }
            let subject = crate::tsa::AnchorSubject::from_row(&record.subject);
            if subject.is_head() {
                check_anchored_head(
                    conn,
                    &AnchoredHead {
                        ledger: verify::FailedLedger::Anchors,
                        label: "anchor",
                        id: record.id,
                        subject,
                        hash: &record.subject_hash,
                        ledger_id: record.ledger_id,
                    },
                    &lineage,
                    &checkpoint,
                    signature_mode,
                    pq_verifying_key.as_ref(),
                )?;
            }
            if let Some(n) = report.anchors_checked.as_mut() {
                *n += 1;
            }
        }
    }

    // OpenTimestamps proofs (`ots_anchors`): the same rules, structural only.
    // A Bitcoin attestation is never checked here — there is no header input,
    // and this runs at witnessd boot; `log_anchor verify --bitcoin-headers`
    // does that comparison. The cached `attestation`/`bitcoin_height`
    // columns are re-derived from the proof; disagreement is a warning.
    if crate::ots::ots_table_exists(conn)? {
        report.ots_anchors_checked = Some(0);
        for (id, row) in crate::ots::list_ots_anchors_lenient(conn)? {
            let record = match row {
                Ok(record) => record,
                Err(err) => {
                    let reason = strip_prefix_for(&err.to_string(), "corrupt ots anchor", id);
                    anchor_warnings.push(format!(
                        "ots anchor #{id}: row is malformed ({reason}); run log_anchor verify"
                    ));
                    continue;
                }
            };
            let proof = match crate::ots::Proof::parse(&record.proof) {
                Ok(proof) => proof,
                Err(_) => {
                    anchor_warnings.push(format!(
                        "ots anchor #{id}: stored proof does not parse as an OpenTimestamps \
                         proof; run log_anchor verify"
                    ));
                    continue;
                }
            };
            if proof.digest != record.subject_hash {
                anchor_warnings.push(format!(
                    "ots anchor #{id}: proof digest does not match the subject hash the row \
                     claims; run log_anchor verify"
                ));
                continue;
            }
            let state = proof.state();
            let caches_agree = state.column() == Some(record.attestation.as_str())
                && state.height().map(|h| h as i64) == record.bitcoin_height;
            if !caches_agree {
                anchor_warnings.push(format!(
                    "ots anchor #{id}: attestation columns disagree with the stored proof; run \
                     log_anchor verify"
                ));
                continue;
            }
            let subject = record.subject_kind_or_digest();
            if subject.is_head() {
                check_anchored_head(
                    conn,
                    &AnchoredHead {
                        ledger: verify::FailedLedger::OtsAnchors,
                        label: "ots anchor",
                        id: record.id,
                        subject,
                        hash: &record.subject_hash,
                        ledger_id: record.ledger_id,
                    },
                    &lineage,
                    &checkpoint,
                    signature_mode,
                    pq_verifying_key.as_ref(),
                )?;
            }
            if let Some(n) = report.ots_anchors_checked.as_mut() {
                *n += 1;
            }
        }
    }
    report.warnings.extend(anchor_warnings);

    // Clock-provenance records against the anchors table as it stands now:
    // warnings only (the ledger itself already passed above). A record whose
    // anchor is gone, or whose anchor's token hashes differently, names a
    // deleted or replaced anchor; a prompt-or-negative offset beyond one
    // bucket names a device clock that disagreed with the TSA's. A missing
    // record is never a warning (anchors predate the ledger). The check runs
    // even with no anchors table (every record is then an orphan) and skips
    // records whose anchor row exists but is malformed (already warned).
    #[cfg(test)]
    tests::between_the_anchor_and_clock_reads();
    if clock_rows > 0 {
        clock_provenance_warnings(
            conn,
            &decoded_anchors,
            &malformed_anchor_ids,
            &mut report.warnings,
        );
    }
    snapshot.release()?;

    Ok(())
}

/// A read snapshot over several reads of one connection: `SAVEPOINT name`
/// on begin, `RELEASE name` on release or drop. Nested inside a caller's
/// transaction it is a plain savepoint; outside one it opens a deferred
/// transaction whose first read fixes the snapshot every later read sees.
struct ReadSnapshot<'a> {
    conn: &'a Connection,
    name: &'static str,
    open: bool,
}

impl<'a> ReadSnapshot<'a> {
    fn begin(conn: &'a Connection, name: &'static str) -> Result<Self> {
        conn.execute_batch(&format!("SAVEPOINT {name};"))?;
        Ok(Self {
            conn,
            name,
            open: true,
        })
    }

    fn release(mut self) -> Result<()> {
        self.open = false;
        self.conn
            .execute_batch(&format!("RELEASE {};", self.name))?;
        Ok(())
    }
}

impl Drop for ReadSnapshot<'_> {
    fn drop(&mut self) {
        if self.open {
            let _ = self.conn.execute_batch(&format!("RELEASE {};", self.name));
        }
    }
}

/// The clock-provenance cross-check warnings (see the call site). No
/// `genTime`, no full hash: a 16-hex prefix of the two token hashes is all a
/// replaced-token warning carries.
fn clock_provenance_warnings(
    conn: &Connection,
    anchors: &[crate::tsa::AnchorRecord],
    malformed_anchor_ids: &[i64],
    warnings: &mut Vec<String>,
) {
    use crate::clock_provenance::{self, AnchorLink, ONE_BUCKET_S};
    let rows = match clock_provenance::list_rows(conn) {
        Ok(rows) => rows,
        Err(err) => {
            warnings.push(format!(
                "clock provenance ledger could not be read for the anchor cross-check: {err:#}"
            ));
            return;
        }
    };
    for row in &rows {
        let record = &row.record;
        match clock_provenance::link_to_anchor(record, anchors) {
            AnchorLink::AnchorMissing => {
                if malformed_anchor_ids.contains(&record.anchor_id) {
                    continue;
                }
                warnings.push(format!(
                    "clock provenance orphan: record #{} names anchor #{}, which is no longer \
                     in the anchors table (deleted, or the table was rebuilt); run log_anchor \
                     verify",
                    row.id, record.anchor_id
                ));
            }
            AnchorLink::TokenReplaced {
                stored_token_sha256,
            } => {
                warnings.push(format!(
                    "clock provenance orphan: record #{} binds anchor #{} to token sha256 {}… \
                     but the row now holds {}… (the token was replaced); run log_anchor verify",
                    row.id,
                    record.anchor_id,
                    record.token_sha256.chars().take(16).collect::<String>(),
                    &stored_token_sha256[..16]
                ));
            }
            AnchorLink::Matches => {
                let Some(anchor) = anchors.iter().find(|a| a.id == record.anchor_id) else {
                    continue;
                };
                let Some(assessment) = clock_provenance::assess(record, anchor) else {
                    continue;
                };
                if clock_provenance::beyond(record, &assessment, ONE_BUCKET_S) {
                    let offset = record.observed_offset_s.unwrap_or(0);
                    warnings.push(format!(
                        "clock offset: record #{} measured the device clock {} from anchor \
                         #{}'s TSA time — beyond one bucket (600 s); run log_anchor verify",
                        row.id,
                        clock_provenance::format_offset(offset),
                        record.anchor_id
                    ));
                }
            }
        }
    }
}

/// `list_anchors_lenient` wraps a row's reason as `corrupt anchor {id}:
/// {reason}`; the warning names the row itself, so keep only the reason.
fn strip_corrupt_prefix(message: &str, id: i64) -> String {
    strip_prefix_for(message, "corrupt anchor", id)
}

/// `{what} {id}: ` stripped from a lenient reader's reason
/// (`corrupt anchor` for `tsa_anchors`, `corrupt ots anchor` for
/// `ots_anchors`).
fn strip_prefix_for(message: &str, what: &str, id: i64) -> String {
    let prefix = format!("{what} {id}: ");
    message.strip_prefix(&prefix).unwrap_or(message).to_string()
}

/// The first 16 hex characters of a hash — all a retained MQTT report may
/// carry (Invariant III).
fn hex16(hash: &[u8; 32]) -> String {
    hex::encode(&hash[..8])
}

/// One head-kind anchor row, from either table, for the membership check.
struct AnchoredHead<'a> {
    /// `Anchors` (RFC 3161) or `OtsAnchors` (OpenTimestamps).
    ledger: verify::FailedLedger,
    /// `"anchor"` or `"ots anchor"`: every detail reads `{label} #{id}`.
    label: &'static str,
    id: i64,
    subject: crate::tsa::AnchorSubject,
    hash: &'a [u8; 32],
    ledger_id: Option<i64>,
}

/// A head-kind anchor's membership check: the anchored hash must be in its
/// declared ledger's history, witnessed by a live row or by a checkpoint
/// whose signature verifies under a genesis-anchored lineage key.
fn check_anchored_head(
    conn: &Connection,
    row: &AnchoredHead<'_>,
    lineage: &[crate::DeviceKeyEpoch],
    cutoff_checkpoint: &verify::CheckpointInfo,
    signature_mode: SignatureMode,
    pq_verifying_key: Option<&PqPublicKey>,
) -> Result<()> {
    use crate::tsa::HistoryWitness;
    let id = row.id;
    let hash = row.hash;
    let subject = row.subject;
    let label = row.label;
    let regression = |detail: String| {
        anyhow::Error::new(verify::VerifyFailure {
            ledger: row.ledger,
            entry_id: Some(id),
            kind: verify::FailureKind::AnchorRegression,
            detail,
        })
    };
    match crate::tsa::hash_in_ledger(conn, hash, subject)? {
        Some(HistoryWitness::LiveRow { .. }) => Ok(()),
        Some(HistoryWitness::CheckpointHead { checkpoint_id }) => {
            // Reading the witness row is a database operation: a plain `Err`.
            let witness = verify::checkpoint_by_id(conn, checkpoint_id)?;
            // Signer: the recorded key, which must be in the lineage; a NULL
            // signer means the key active at the checkpoint's cutoff —
            // mirroring the cutoff checkpoint's own handling above.
            let signer = match witness.signer_public_key {
                Some(key) => {
                    if !lineage.iter().any(|e| e.public_key == key) {
                        return Err(regression(format!(
                            "anchor regression: {label} #{id} covers chain head {}… which survives \
                             only as checkpoint #{checkpoint_id} signed by a key outside the \
                             genesis-anchored device key lineage",
                            hex16(hash)
                        )));
                    }
                    key
                }
                None => {
                    crate::device_key_active_at_in(lineage, witness.cutoff_event_id.unwrap_or(0))?
                }
            };
            let signer = verifying_key_from_bytes(&signer)?;
            verify::verify_checkpoint_signature(&signer, &witness, signature_mode, pq_verifying_key)
                .map_err(|e| {
                    regression(format!(
                        "anchor regression: {label} #{id} covers chain head {}… which survives \
                         only as checkpoint #{checkpoint_id} whose signature does not verify: {e:#}",
                        hex16(hash)
                    ))
                })?;
            Ok(())
        }
        None => {
            // Qualifier: text only, chain-head rows only. The unsigned
            // `ledger_id` sharpens the diagnosis; it never decides the verdict.
            let qualifier = match (subject, cutoff_checkpoint.cutoff_event_id) {
                (crate::tsa::AnchorSubject::ChainHead, Some(cutoff)) => match row.ledger_id {
                    Some(ledger_id) if ledger_id <= cutoff => format!(
                        " (the anchored ledger row {ledger_id} is at or below the retention \
                         cutoff {cutoff})"
                    ),
                    Some(ledger_id) => format!(
                        " (the anchored ledger row {ledger_id} is newer than the retention \
                         cutoff {cutoff})"
                    ),
                    None => " (the anchor row records no ledger position — imported before this \
                             release, or after the head was already pruned)"
                        .to_string(),
                },
                _ => String::new(),
            };
            Err(regression(format!(
                "anchor regression: {label} #{id} covers {subject} {}… which is not in this \
                 database's {} history — rollback, truncation, or a different history than the \
                 one that was anchored{qualifier}",
                hex16(hash),
                subject.ledger_short()
            )))
        }
    }
}

fn verifying_key_from_bytes(bytes: &[u8; 32]) -> Result<VerifyingKey> {
    crate::crypto::signatures::verifying_key_strict(bytes)
        .map_err(|e| anyhow::anyhow!("invalid verifying key: {}", e))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        derive_db_encryption_key, signing_key_from_seed, CandidateEvent, EventType,
        InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, ZonePolicy,
    };
    use std::cell::RefCell;
    use std::path::{Path, PathBuf};

    thread_local! {
        static BETWEEN_THE_ANCHOR_AND_CLOCK_READS: RefCell<Option<Box<dyn Fn()>>> = const { RefCell::new(None) };
    }

    /// Test seam: runs between the anchors pass and the clock-provenance
    /// cross-check, where a writer may land.
    pub(super) fn between_the_anchor_and_clock_reads() {
        BETWEEN_THE_ANCHOR_AND_CLOCK_READS.with(|hook| {
            if let Some(f) = hook.borrow().as_ref() {
                f();
            }
        });
    }

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
        write_test_event_aged(kernel, 0)
    }

    /// Seals an event whose SIGNED bucket is `age_s` in the past — the only
    /// honest way to put an expired row in front of the pruner, which ages
    /// rows by the signed bucket and not by the `created_at` column.
    fn write_test_event_aged(kernel: &mut Kernel, age_s: u64) {
        let module = ModuleDescriptor {
            id: "test-module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let now = TimeBucket::now(600).expect("bucket");
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: now.start_epoch_s.saturating_sub(age_s) / 600 * 600,
                size_s: 600,
            },
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
            anchors_checked: None,
            ots_anchors_checked: None,
            clock_provenance_verified: None,
            events_verified: 3,
            break_glass_receipts_verified: 0,
            break_glass_granted: 0,
            break_glass_denied: 0,
            break_glass_redeemed: 0,
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
        // A ledger with no rows leaves the report byte-identical to before.
        assert!(!json.contains("clock_provenance_verified"));
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
        write_test_event_aged(&mut kernel, 86_400);
        write_test_event_aged(&mut kernel, 86_400);

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

        // Every event was sealed a day ago; prune them all — writes a signed
        // checkpoint and empties the table (the case that used to false-fail).
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

    /// Two sealed events, a mark signed at the tip, then a full-retention prune
    /// (which writes a cutoff-bound checkpoint and empties the table). Returns
    /// the mark, the entry hash of the FIRST event (an "older head" for the
    /// replay case) and the open kernel for the caller to tamper with.
    fn prune_everything_under_a_mark(
        kernel: &mut Kernel,
    ) -> (crate::HighWaterMark, [u8; 32], ed25519_dalek::SigningKey) {
        write_test_event_aged(kernel, 86_400);
        write_test_event_aged(kernel, 86_400);
        let sk = signing_key_from_seed(TEST_SEED).unwrap();
        let (newest_id, head) = crate::log::high_water_mark::read_live_head(&kernel.conn).unwrap();
        let mark = crate::HighWaterMark::sign(
            newest_id.expect("has events") as u64,
            head.expect("has events"),
            600,
            &device_signature_keys(&sk),
        )
        .unwrap();
        let first_hash: Vec<u8> = kernel
            .conn
            .query_row(
                "SELECT entry_hash FROM sealed_events ORDER BY id ASC LIMIT 1",
                [],
                |r| r.get(0),
            )
            .unwrap();
        let mut older_head = [0u8; 32];
        older_head.copy_from_slice(&first_hash);
        kernel
            .enforce_retention_with_checkpoint(std::time::Duration::from_secs(1))
            .unwrap();
        (mark, older_head, sk)
    }

    #[test]
    fn high_water_mark_rejects_an_inflated_cutoff_on_a_bound_checkpoint() {
        // The empty-table branch lets the checkpoint's cutoff stand in for the
        // live high. Inflating it used to pass as a prune that advanced past
        // the mark; now the cutoff is in the signed message, so it cannot move.
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        let (mark, _, _) = prune_everything_under_a_mark(&mut kernel);
        kernel
            .conn
            .execute(
                "UPDATE checkpoints SET cutoff_event_id = cutoff_event_id + 100",
                [],
            )
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = run_full_verify_with_high_water_mark(
            &conn,
            None,
            None,
            SignatureMode::Compat,
            Some(&mark),
            |_| {},
        )
        .expect("runner runs");
        assert!(!report.chain_valid, "an inflated cutoff must not verify");
        assert!(
            report
                .error
                .as_deref()
                .unwrap_or("")
                .contains("checkpoint signature"),
            "{:?}",
            report.error
        );
    }

    #[test]
    fn legacy_checkpoint_cutoff_is_not_trusted_over_an_empty_table() {
        // A checkpoint from before cutoffs were bound signs only its head. Left
        // exactly at the mark it still verifies (the head proves the prune
        // stopped there); replayed as an OLDER head with an inflated cutoff —
        // the wipe dressed as a prune — it must not.
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        let (mark, older_head, sk) = prune_everything_under_a_mark(&mut kernel);
        let keys = device_signature_keys(&sk);
        let at_mark = crate::log::sign_entry(
            &keys,
            &mark.head_hash,
            crate::crypto::signatures::DOMAIN_CHECKPOINT,
        )
        .unwrap();
        // A legacy row is Ed25519-only over the bare head. Under the
        // pqc-signatures build the kernel writes a hybrid checkpoint, so the PQ
        // columns must go too — a PQ signature over the BOUND message left
        // beside a head-only Ed25519 one is a corrupt row, not a legacy one.
        kernel
            .conn
            .execute(
                "UPDATE checkpoints SET signature = ?1, pq_signature = NULL, pq_scheme = NULL",
                rusqlite::params![at_mark.ed25519_signature],
            )
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
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
            "a legacy prune that stopped at the mark still verifies: {:?}",
            report.error
        );
        assert!(report.high_water_mark_checked);

        let replayed = crate::log::sign_entry(
            &keys,
            &older_head,
            crate::crypto::signatures::DOMAIN_CHECKPOINT,
        )
        .unwrap();
        conn.execute(
            "UPDATE checkpoints SET chain_head_hash = ?1, signature = ?2, cutoff_event_id = ?3, \
             pq_signature = NULL, pq_scheme = NULL",
            rusqlite::params![
                older_head.to_vec(),
                replayed.ed25519_signature,
                mark.seq as i64 + 100
            ],
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
            "a replayed legacy checkpoint must not pass"
        );
        assert!(
            report
                .error
                .as_deref()
                .unwrap_or("")
                .contains("cannot be told from a wipe"),
            "{:?}",
            report.error
        );
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

    // ---------------- RFC 3161 anchors folded into the verdict ----------------

    use crate::tsa::AnchorSubject;

    fn fixture_token_der() -> Vec<u8> {
        let path = format!(
            "{}/tests/fixtures/tsa/reply.tsr",
            env!("CARGO_MANIFEST_DIR")
        );
        crate::tsa::parse_response(&std::fs::read(&path).expect("reading TSA fixture"))
            .expect("fixture token parses")
            .token_der
    }

    /// The fixture token with its message imprint spliced to `h`: parses,
    /// covers `h`, verifies under no CA (a copy of
    /// `tests/common::token_with_imprint`, which `src` cannot reach).
    fn token_with_imprint(h: &[u8; 32]) -> Vec<u8> {
        use sha2::{Digest, Sha256};
        let needle: [u8; 32] = Sha256::digest(b"securacv-fixture").into();
        let mut token = fixture_token_der();
        let hits: Vec<usize> = (0..=token.len() - 32)
            .filter(|&i| token[i..i + 32] == needle)
            .collect();
        assert_eq!(hits.len(), 1, "fixture digest occurs once in the token");
        token[hits[0]..hits[0] + 32].copy_from_slice(h);
        token
    }

    fn spliced_token(h: &[u8; 32]) -> crate::tsa::TimestampToken {
        crate::tsa::parse_token(&token_with_imprint(h)).expect("spliced token parses")
    }

    /// Insert an anchor row over `hash` under `subject`, as `log_anchor`
    /// would (ledger_id recorded from the live row, when there is one).
    fn anchor(conn: &Connection, subject: AnchorSubject, hash: &[u8; 32]) -> i64 {
        crate::tsa::ensure_anchor_table(conn).unwrap();
        crate::tsa::insert_anchor_declared(
            conn,
            subject,
            hash,
            "(offline)",
            None,
            &spliced_token(hash),
        )
        .unwrap()
    }

    fn event_hash(conn: &Connection, id: i64) -> [u8; 32] {
        let bytes: Vec<u8> = conn
            .query_row(
                "SELECT entry_hash FROM sealed_events WHERE id = ?1",
                [id],
                |r| r.get(0),
            )
            .unwrap();
        bytes.try_into().unwrap()
    }

    fn verify(conn: &Connection) -> VerifyReport {
        run_full_verify(conn, None, None, SignatureMode::Compat, |_| {}).expect("runner runs")
    }

    fn has_64_hex(s: &str) -> bool {
        let mut run = 0usize;
        for b in s.bytes() {
            if b.is_ascii_hexdigit() {
                run += 1;
                if run >= 64 {
                    return true;
                }
            } else {
                run = 0;
            }
        }
        false
    }

    /// Invariant III for the retained MQTT report: no anchored hash leaves
    /// in full. `error`, `failure.detail` and every warning carry at most a
    /// 16-hex prefix, and the anchored hash's full hex is nowhere in the JSON.
    fn assert_report_hygiene(report: &VerifyReport, anchored: &[u8; 32]) {
        if let Some(err) = &report.error {
            assert!(!has_64_hex(err), "full hash in error: {err}");
        }
        if let Some(failure) = &report.failure {
            assert!(
                !has_64_hex(&failure.detail),
                "full hash in detail: {}",
                failure.detail
            );
        }
        for w in &report.warnings {
            assert!(!has_64_hex(w), "full hash in warning: {w}");
        }
        let json = serde_json::to_string(report).unwrap();
        assert!(
            !json.contains(&hex::encode(anchored)),
            "anchored hash leaked into the report: {json}"
        );
        assert!(
            json.contains(&hex::encode(&anchored[..8])) || report.failure.is_none(),
            "a failure names the anchored hash by its 16-hex prefix: {json}"
        );
    }

    fn regression(report: &VerifyReport, anchor_id: i64) -> verify::VerifyFailure {
        assert!(!report.chain_valid, "expected an anchor regression");
        assert_eq!(report.verdict, "verification failed");
        let failure = report.failure.clone().expect("structured failure");
        assert_eq!(failure.ledger, verify::FailedLedger::Anchors);
        assert_eq!(failure.kind, verify::FailureKind::AnchorRegression);
        assert_eq!(failure.entry_id, Some(anchor_id));
        assert_eq!(report.error.as_deref(), Some(failure.detail.as_str()));
        failure
    }

    // (a)
    #[test]
    fn anchors_absent_table_leaves_the_report_unchanged() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, None);
        let json = serde_json::to_string(&report).unwrap();
        assert!(
            !json.contains("anchors_checked"),
            "a database with no anchors table must serialize as before: {json}"
        );
        assert!(report.warnings.is_empty());
    }

    // (b)
    #[test]
    fn anchors_live_head_and_digest_rows_count() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 2);
        anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        anchor(&kernel.conn, AnchorSubject::Digest, &[9u8; 32]);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, Some(2));
        assert!(report.warnings.is_empty(), "{:?}", report.warnings);
        assert_eq!(report.verdict, "self-consistent; identity unverified");
        assert!(serde_json::to_string(&report)
            .unwrap()
            .contains("\"anchors_checked\":2"));
        assert_report_hygiene(&report, &head);
    }

    /// Two day-old events, the FIRST anchored as a chain head, then a
    /// full-retention prune: PR A keeps the anchored head as its own
    /// device-signed checkpoint (the cutoff checkpoint over event 2 keeps
    /// the highest id). Returns the anchor row id and the anchored hash.
    fn preserved_head_db(db: &TempDb) -> (i64, [u8; 32]) {
        let mut kernel = open_kernel(db.path());
        write_test_event_aged(&mut kernel, 86_400);
        write_test_event_aged(&mut kernel, 86_400);
        let first = event_hash(&kernel.conn, 1);
        let anchor_id = anchor(&kernel.conn, AnchorSubject::ChainHead, &first);
        kernel
            .enforce_retention_with_checkpoint(std::time::Duration::from_secs(1))
            .unwrap();
        let live: i64 = kernel
            .conn
            .query_row("SELECT count(*) FROM sealed_events", [], |r| r.get(0))
            .unwrap();
        assert_eq!(live, 0, "retention should have emptied the live table");
        let witnesses: i64 = kernel
            .conn
            .query_row(
                "SELECT count(*) FROM checkpoints WHERE chain_head_hash = ?1",
                [first.as_slice()],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(witnesses, 1, "the anchored head survives as a checkpoint");
        (anchor_id, first)
    }

    // (c)
    #[test]
    fn anchors_head_preserved_by_a_retention_checkpoint_is_valid() {
        let db = TempDb::new();
        let (_, first) = preserved_head_db(&db);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, Some(1));
        assert_report_hygiene(&report, &first);

        // A pre-rotation witness row records no signer: the key active at
        // its cutoff signs it, as for the cutoff checkpoint itself.
        conn.execute(
            "UPDATE checkpoints SET signer_public_key = NULL WHERE chain_head_hash = ?1",
            [first.as_slice()],
        )
        .unwrap();
        let report = verify(&conn);
        assert!(
            report.chain_valid,
            "NULL-signer witness: {:?}",
            report.error
        );
        assert_eq!(report.anchors_checked, Some(1));
    }

    // (d) — no checkpoint
    #[test]
    fn anchors_missing_head_fails_closed_with_no_qualifier() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 2);
        let anchor_id = anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        // Rollback: the newest event is gone; the shorter chain still verifies.
        kernel
            .conn
            .execute("DELETE FROM sealed_events WHERE id = 2", [])
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert_eq!(
            failure.detail,
            format!(
                "anchor regression: anchor #{anchor_id} covers chain_head {}… which is not in \
                 this database's chain history — rollback, truncation, or a different history \
                 than the one that was anchored",
                hex::encode(&head[..8])
            )
        );
        // The failing row is not counted; the rows before it would be.
        assert_eq!(report.anchors_checked, Some(0));
        assert_report_hygiene(&report, &head);
    }

    // (d) — ledger_id at or below the cutoff (a legacy prune's shape)
    #[test]
    fn anchors_missing_head_below_the_cutoff_names_a_legacy_prune() {
        let db = TempDb::new();
        let (anchor_id, first) = preserved_head_db(&db);
        let conn = open_encrypted(db.path());
        // An older witnessd would never have written the witness checkpoint.
        conn.execute(
            "DELETE FROM checkpoints WHERE chain_head_hash = ?1",
            [first.as_slice()],
        )
        .unwrap();
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert!(
            failure
                .detail
                .ends_with(" (the anchored ledger row 1 is at or below the retention cutoff 2)"),
            "{}",
            failure.detail
        );
        assert_report_hygiene(&report, &first);
    }

    // (d) — ledger_id newer than the cutoff (truncation past the anchor)
    #[test]
    fn anchors_missing_head_above_the_cutoff_is_truncation() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event_aged(&mut kernel, 86_400);
        write_test_event_aged(&mut kernel, 86_400);
        write_test_event_aged(&mut kernel, 0);
        let third = event_hash(&kernel.conn, 3);
        let anchor_id = anchor(&kernel.conn, AnchorSubject::ChainHead, &third);
        kernel
            .enforce_retention_with_checkpoint(std::time::Duration::from_secs(3_600))
            .unwrap();
        let cutoff: i64 = kernel
            .conn
            .query_row(
                "SELECT cutoff_event_id FROM checkpoints ORDER BY id DESC LIMIT 1",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(cutoff, 2);
        // The anchored tail is lopped off after the prune.
        kernel
            .conn
            .execute("DELETE FROM sealed_events WHERE id = 3", [])
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert!(
            failure
                .detail
                .ends_with(" (the anchored ledger row 3 is newer than the retention cutoff 2)"),
            "{}",
            failure.detail
        );
        assert_report_hygiene(&report, &third);
    }

    // (d) — ledger_id NULL
    #[test]
    fn anchors_missing_head_with_no_ledger_position_says_so() {
        let db = TempDb::new();
        let (anchor_id, first) = preserved_head_db(&db);
        let conn = open_encrypted(db.path());
        conn.execute(
            "DELETE FROM checkpoints WHERE chain_head_hash = ?1",
            [first.as_slice()],
        )
        .unwrap();
        conn.execute("UPDATE tsa_anchors SET ledger_id = NULL", [])
            .unwrap();
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert!(
            failure.detail.ends_with(
                " (the anchor row records no ledger position — imported before this release, \
                 or after the head was already pruned)"
            ),
            "{}",
            failure.detail
        );
    }

    // (e)
    #[test]
    fn anchors_damaged_rows_are_warnings_not_failures() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 1);
        let good = anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let conn = &kernel.conn;
        let raw = |hash: rusqlite::types::Value,
                   start: rusqlite::types::Value,
                   token: rusqlite::types::Value| {
            conn.execute(
                "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
                    subject_hash, tsa_url, gen_time, token_der)
                 VALUES (?1, 600, 'chain_head', ?2, '(offline)', '20260610123324Z', ?3)",
                rusqlite::params![start, hash, token],
            )
            .unwrap();
            conn.last_insert_rowid()
        };
        use rusqlite::types::Value::{Blob, Integer, Text};
        let short = raw(
            Blob(vec![0u8; 31]),
            Integer(0),
            Blob(token_with_imprint(&head)),
        );
        let unparseable = raw(Blob(head.to_vec()), Integer(0), Blob(vec![0u8]));
        let text_hash = raw(
            Text("abc".into()),
            Integer(0),
            Blob(token_with_imprint(&head)),
        );
        let text_start = raw(
            Blob(head.to_vec()),
            Text("abc".into()),
            Blob(token_with_imprint(&head)),
        );
        let text_token = raw(Blob(head.to_vec()), Integer(0), Text("abc".into()));
        // A token over a DIFFERENT hash than the row claims.
        let mismatch = raw(
            Blob(head.to_vec()),
            Integer(0),
            Blob(token_with_imprint(&[5u8; 32])),
        );
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert!(report.failure.is_none());
        assert!(report.error.is_none());
        assert_eq!(report.anchors_checked, Some(1), "only row #{good} passed");
        assert_eq!(
            report.warnings,
            vec![
                format!(
                    "anchor #{short}: row is malformed (subject_hash size); run log_anchor verify"
                ),
                format!(
                    "anchor #{unparseable}: stored token does not parse as an RFC 3161 token; run \
                     log_anchor verify"
                ),
                format!(
                    "anchor #{text_hash}: row is malformed (subject_hash is not a BLOB); run \
                     log_anchor verify"
                ),
                format!(
                    "anchor #{text_start}: row is malformed (created_bucket_start is not an \
                     INTEGER); run log_anchor verify"
                ),
                format!(
                    "anchor #{text_token}: row is malformed (token_der is not a BLOB); run \
                     log_anchor verify"
                ),
                format!(
                    "anchor #{mismatch}: token imprint does not match the subject hash the row \
                     claims; run log_anchor verify"
                ),
            ]
        );
        for w in &report.warnings {
            assert_eq!(
                verify::classify_warning(w),
                verify::WarningKind::AnchorAnomaly
            );
        }
        assert_report_hygiene(&report, &head);
    }

    /// The anchor audit's warnings must survive the timeline audit's
    /// (`run_full_verify_impl` extends, never assigns).
    #[test]
    fn anchor_warnings_survive_alongside_timeline_warnings() {
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
        crate::tsa::ensure_anchor_table(&kernel.conn).unwrap();
        kernel
            .conn
            .execute(
                "INSERT INTO tsa_anchors (created_bucket_start, created_bucket_size, subject,
                    subject_hash, tsa_url, gen_time, token_der)
                 VALUES (0, 600, 'digest', ?1, '(offline)', '20260610123324Z', x'00')",
                [vec![0u8; 31]],
            )
            .unwrap();
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, Some(0));
        assert!(
            report
                .warnings
                .iter()
                .any(|w| w.starts_with("anchor #1: row is malformed")),
            "{:?}",
            report.warnings
        );
        assert!(
            report.warnings.iter().any(|w| w.contains("no heartbeat")),
            "{:?}",
            report.warnings
        );
    }

    // (f) — bad witness signature
    #[test]
    fn anchors_witness_checkpoint_with_bad_signature_is_a_regression() {
        let db = TempDb::new();
        let (anchor_id, first) = preserved_head_db(&db);
        let conn = open_encrypted(db.path());
        conn.execute(
            "UPDATE checkpoints SET signature = ?1, pq_signature = NULL, pq_scheme = NULL \
             WHERE chain_head_hash = ?2",
            rusqlite::params![vec![0u8; 64], first.as_slice()],
        )
        .unwrap();
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert!(
            failure.detail.starts_with(&format!(
                "anchor regression: anchor #{anchor_id} covers chain head {}… which survives \
                 only as checkpoint #1 whose signature does not verify: ",
                hex::encode(&first[..8])
            )),
            "{}",
            failure.detail
        );
        assert_report_hygiene(&report, &first);
    }

    // (f) — off-lineage witness signer
    #[test]
    fn anchors_witness_checkpoint_signed_off_lineage_is_a_regression() {
        let db = TempDb::new();
        let (anchor_id, first) = preserved_head_db(&db);
        let conn = open_encrypted(db.path());
        let rogue = ed25519_dalek::SigningKey::from_bytes(&[123u8; 32]);
        conn.execute(
            "UPDATE checkpoints SET signer_public_key = ?1 WHERE chain_head_hash = ?2",
            rusqlite::params![rogue.verifying_key().to_bytes().to_vec(), first.as_slice()],
        )
        .unwrap();
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert_eq!(
            failure.detail,
            format!(
                "anchor regression: anchor #{anchor_id} covers chain head {}… which survives \
                 only as checkpoint #1 signed by a key outside the genesis-anchored device key \
                 lineage",
                hex::encode(&first[..8])
            )
        );
    }

    // (f) — unreadable witness row: a database error, not a verdict
    #[test]
    fn anchors_unreadable_witness_checkpoint_is_an_error_not_a_regression() {
        let db = TempDb::new();
        let (_, first) = preserved_head_db(&db);
        let conn = open_encrypted(db.path());
        conn.execute(
            "UPDATE checkpoints SET signer_public_key = x'0102', cutoff_event_id = 'abc' \
             WHERE chain_head_hash = ?1",
            [first.as_slice()],
        )
        .unwrap();
        let report = verify(&conn);
        assert!(!report.chain_valid);
        assert!(report.failure.is_none(), "{:?}", report.failure);
        let err = report.error.clone().expect("error");
        assert!(!err.contains("anchor regression"), "{err}");
        assert!(!has_64_hex(&err), "{err}");
    }

    #[test]
    fn checkpoint_by_id_errs_on_a_missing_row() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);
        let conn = open_encrypted(db.path());
        let err = verify::checkpoint_by_id(&conn, 7).unwrap_err().to_string();
        assert!(err.contains("checkpoint #7 does not exist"), "{err}");
    }

    // (g) — the receipt ledgers and the policy history
    fn export_receipt_head(kernel: &mut Kernel) -> [u8; 32] {
        let bundle = kernel
            .export_events_bundle_self(
                KernelConfig::ruleset_hash_from_id("ruleset:test"),
                crate::ExportOptions::default(),
            )
            .expect("export");
        bundle.receipt_entry.entry_hash
    }

    fn trustee_policy(sk: &ed25519_dalek::SigningKey) -> crate::QuorumPolicy {
        crate::QuorumPolicy::new(
            1,
            vec![crate::TrusteeEntry {
                id: crate::TrusteeId::new("alice"),
                public_key: sk.verifying_key().to_bytes(),
            }],
        )
        .unwrap()
    }

    fn policy_head(kernel: &mut Kernel) -> [u8; 32] {
        let trustee = ed25519_dalek::SigningKey::from_bytes(&[31u8; 32]);
        let outcome = kernel
            .set_break_glass_policy_gated(
                &trustee_policy(&trustee),
                &[],
                TimeBucket::now_10min().unwrap(),
            )
            .expect("bootstrap");
        assert_eq!(outcome, crate::PolicyChangeOutcome::Bootstrapped);
        let bytes: Vec<u8> = kernel
            .conn
            .query_row(
                "SELECT entry_hash FROM policy_change_history ORDER BY id DESC LIMIT 1",
                [],
                |r| r.get(0),
            )
            .unwrap();
        bytes.try_into().unwrap()
    }

    fn break_glass_receipt_head(kernel: &mut Kernel) -> [u8; 32] {
        let trustee = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let policy = trustee_policy(&trustee);
        kernel.set_break_glass_policy(&policy).unwrap();
        let bucket = TimeBucket::now_10min().unwrap();
        let ruleset_hash = KernelConfig::ruleset_hash_from_id("ruleset:test");
        let request = crate::UnlockRequest::new(
            crate::EXPORT_EVENTS_ENVELOPE_ID,
            ruleset_hash,
            "export_events",
            bucket,
        )
        .unwrap();
        let approval = crate::Approval::signed(
            crate::TrusteeId::new("alice"),
            request.request_hash(),
            &trustee,
        );
        let (result, receipt) = crate::BreakGlass::authorize(
            &policy,
            &request,
            std::slice::from_ref(&approval),
            bucket,
        );
        result.expect("token issued");
        kernel
            .log_break_glass_receipt(&receipt, &[approval])
            .unwrap()
    }

    fn head_kind_round_trip(
        subject: AnchorSubject,
        produce: fn(&mut Kernel) -> [u8; 32],
        table: &str,
    ) {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        let head = produce(&mut kernel);
        let anchor_id = anchor(&kernel.conn, subject, &head);
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{subject}: {:?}", report.error);
        assert_eq!(report.anchors_checked, Some(1));
        assert_report_hygiene(&report, &head);

        // Verified against the ledger the row DECLARES: a head that is no
        // longer there is a regression, with no cutoff qualifier (these
        // ledgers are never pruned).
        conn.execute(&format!("DELETE FROM {table}"), []).unwrap();
        let report = verify(&conn);
        let failure = regression(&report, anchor_id);
        assert_eq!(
            failure.detail,
            format!(
                "anchor regression: anchor #{anchor_id} covers {subject} {}… which is not in \
                 this database's {} history — rollback, truncation, or a different history \
                 than the one that was anchored",
                hex::encode(&head[..8]),
                subject.ledger_short()
            )
        );
        assert_report_hygiene(&report, &head);
    }

    #[test]
    fn anchors_export_receipt_head_present_then_absent() {
        head_kind_round_trip(
            AnchorSubject::ExportReceiptHead,
            export_receipt_head,
            "export_receipts",
        );
    }

    #[test]
    fn anchors_break_glass_receipt_head_present_then_absent() {
        head_kind_round_trip(
            AnchorSubject::BreakGlassReceiptHead,
            break_glass_receipt_head,
            "break_glass_receipts",
        );
    }

    #[test]
    fn anchors_policy_head_present_then_absent() {
        head_kind_round_trip(
            AnchorSubject::PolicyHead,
            policy_head,
            "policy_change_history",
        );
    }

    // (h)
    #[test]
    fn anchors_empty_ledger_sentinels_stored_as_digest_are_counted() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        for subject in AnchorSubject::HEADS {
            // `anchor-all` stores a sentinel under `digest`: in no ledger by
            // construction, so never read as a head.
            anchor(
                &kernel.conn,
                AnchorSubject::Digest,
                &subject.empty_ledger_sentinel().unwrap(),
            );
        }
        drop(kernel);

        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(
            report.anchors_checked,
            Some(AnchorSubject::HEADS.len() as u64)
        );
        assert_eq!(report.anchors_checked, Some(5));
        assert!(report.warnings.is_empty());
    }

    // ---------------- the clock-provenance ledger ----------------
    // Every clock reading is injected, keyed to the fixture token's genTime.

    const FIXTURE_GEN_TIME: u64 = 1_781_094_804;
    const FIXTURE_BUCKET: i64 = 1_781_094_600;

    fn observe(
        kernel: &mut Kernel,
        wall: u64,
        gap: Option<u32>,
    ) -> Vec<crate::SealedClockProvenance> {
        kernel
            .observe_anchors_for_clock_provenance(&crate::ClockObservation {
                observed_wall_s: wall,
                poll_gap_s: gap,
                kernel_version: "0.0.0-test",
            })
            .expect("observe")
    }

    /// One event, the cursor initialized, then a chain-head anchor stored in
    /// the fixture token's own bucket (a prompt row) and measured at
    /// `genTime + offset_s` by a running daemon (gap 5 s). Returns the
    /// anchor id and the anchored head.
    fn clock_db(db: &TempDb, offset_s: i64) -> (i64, [u8; 32]) {
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        assert!(observe(&mut kernel, FIXTURE_GEN_TIME, None).is_empty());
        let head = event_hash(&kernel.conn, 1);
        let anchor_id = anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        kernel
            .conn
            .execute(
                "UPDATE tsa_anchors SET created_bucket_start = ?1 WHERE id = ?2",
                rusqlite::params![FIXTURE_BUCKET, anchor_id],
            )
            .unwrap();
        let sealed = observe(
            &mut kernel,
            (FIXTURE_GEN_TIME as i64 + offset_s) as u64,
            Some(5),
        );
        assert_eq!(sealed.len(), 1);
        (anchor_id, head)
    }

    /// No genTime (as text or as epoch seconds), no full hash.
    fn assert_clock_hygiene(report: &VerifyReport, head: &[u8; 32]) {
        for text in report
            .warnings
            .iter()
            .chain(report.error.iter())
            .chain(report.failure.iter().map(|f| &f.detail))
        {
            assert!(!has_64_hex(text), "full hash in report text: {text}");
        }
        let json = serde_json::to_string(report).unwrap();
        assert!(
            !json.contains(&hex::encode(head)),
            "anchored hash leaked: {json}"
        );
        for needle in ["20260610123324", &FIXTURE_GEN_TIME.to_string()] {
            assert!(!json.contains(needle), "genTime leaked: {json}");
        }
    }

    #[test]
    fn clock_provenance_walk_valid_tampered_and_off_lineage() {
        let db = TempDb::new();
        let (_, _) = clock_db(&db, 2);
        let conn = open_encrypted(db.path());
        let genesis = signing_key_from_seed(TEST_SEED).unwrap().verifying_key();
        let mut seen = Vec::new();
        let n = verify::verify_clock_provenance_with(
            &conn,
            &[genesis],
            SignatureMode::Compat,
            None,
            |id, _| seen.push(id),
        )
        .unwrap();
        assert_eq!((n, seen), (1, vec![1]));

        // A key outside the lineage: the signature does not check.
        let stranger = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]).verifying_key();
        let err = verify::verify_clock_provenance_with(
            &conn,
            &[stranger],
            SignatureMode::Compat,
            None,
            |_, _| {},
        )
        .unwrap_err();
        let failure = err.downcast_ref::<verify::VerifyFailure>().unwrap();
        assert_eq!(failure.ledger, verify::FailedLedger::ClockProvenance);
        assert_eq!(failure.kind, verify::FailureKind::SignatureMismatch);
        assert_eq!(failure.entry_id, Some(1));

        // One byte of the stored payload literal changed: the entry hash no
        // longer re-derives (the literal is hashed, never a re-serialization).
        conn.execute(
            "UPDATE clock_provenance SET payload_json = replace(payload_json, \
             '\"observed_offset_s\":2', '\"observed_offset_s\":3')",
            [],
        )
        .unwrap();
        let err = verify::verify_clock_provenance_with(
            &conn,
            &[genesis],
            SignatureMode::Compat,
            None,
            |_, _| {},
        )
        .unwrap_err();
        let failure = err.downcast_ref::<verify::VerifyFailure>().unwrap();
        assert_eq!(failure.kind, verify::FailureKind::EntryHashMismatch);
        assert!(!has_64_hex(&failure.detail), "{}", failure.detail);

        // A table with no rows, and no table at all, walk as zero.
        conn.execute("DELETE FROM clock_provenance", []).unwrap();
        assert_eq!(
            verify::verify_clock_provenance_with(
                &conn,
                &[genesis],
                SignatureMode::Compat,
                None,
                |_, _| {}
            )
            .unwrap(),
            0
        );
        conn.execute_batch("DROP TABLE clock_provenance").unwrap();
        assert_eq!(
            verify::verify_clock_provenance_with(
                &conn,
                &[genesis],
                SignatureMode::Compat,
                None,
                |_, _| {}
            )
            .unwrap(),
            0
        );
    }

    #[test]
    fn clock_provenance_rows_are_counted_and_a_good_record_warns_nothing() {
        // Zero rows (the table exists on every upgraded database): omitted.
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);
        let report = verify(&open_encrypted(db.path()));
        assert_eq!(report.clock_provenance_verified, None);
        assert!(!serde_json::to_string(&report)
            .unwrap()
            .contains("clock_provenance_verified"));

        let db = TempDb::new();
        let (_, head) = clock_db(&db, 2);
        let conn = open_encrypted(db.path());
        let mut items = 0;
        let report = run_full_verify(&conn, None, None, SignatureMode::Compat, |item| {
            if matches!(item, VerifiedItem::ClockProvenance { .. }) {
                items += 1;
            }
        })
        .unwrap();
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.clock_provenance_verified, Some(1));
        assert_eq!(items, 1);
        assert!(report.warnings.is_empty(), "{:?}", report.warnings);
        assert!(serde_json::to_string(&report)
            .unwrap()
            .contains("\"clock_provenance_verified\":1"));
        assert_clock_hygiene(&report, &head);
    }

    #[test]
    fn clock_provenance_tampered_ledger_fails_the_verdict() {
        let db = TempDb::new();
        let (_, head) = clock_db(&db, 2);
        let conn = open_encrypted(db.path());
        conn.execute(
            "UPDATE clock_provenance SET payload_json = replace(payload_json, \
             '\"poll_gap_s\":5', '\"poll_gap_s\":1')",
            [],
        )
        .unwrap();
        let report = verify(&conn);
        assert!(!report.chain_valid);
        assert_eq!(report.verdict, "verification failed");
        assert_eq!(report.clock_provenance_verified, None);
        let failure = report.failure.clone().expect("structured failure");
        assert_eq!(failure.ledger, verify::FailedLedger::ClockProvenance);
        assert_eq!(failure.kind, verify::FailureKind::EntryHashMismatch);
        assert_eq!(failure.entry_id, Some(1));
        assert_eq!(
            crate::verify_explain::ledger_name(failure.ledger),
            "clock-provenance ledger"
        );
        assert_eq!(
            serde_json::to_value(failure.ledger).unwrap(),
            serde_json::json!("clock_provenance")
        );
        assert_clock_hygiene(&report, &head);
    }

    #[test]
    fn clock_provenance_orphans_are_warnings() {
        // The anchor row deleted.
        let db = TempDb::new();
        let (anchor_id, head) = clock_db(&db, 2);
        let conn = open_encrypted(db.path());
        conn.execute("DELETE FROM tsa_anchors WHERE id = ?1", [anchor_id])
            .unwrap();
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.clock_provenance_verified, Some(1));
        assert_eq!(
            report.warnings,
            vec![format!(
                "clock provenance orphan: record #1 names anchor #{anchor_id}, which is no \
                 longer in the anchors table (deleted, or the table was rebuilt); run \
                 log_anchor verify"
            )]
        );
        assert_eq!(
            verify::classify_warning(&report.warnings[0]),
            verify::WarningKind::ClockProvenance
        );
        assert_clock_hygiene(&report, &head);

        // The anchors table dropped altogether: still an orphan.
        conn.execute_batch("DROP TABLE tsa_anchors").unwrap();
        let report = verify(&conn);
        assert_eq!(report.anchors_checked, None);
        assert_eq!(report.warnings.len(), 1);
        assert!(report.warnings[0].starts_with("clock provenance orphan: record #1"));

        // The token replaced (here by one over another hash, which the
        // anchor audit flags on its own as well).
        let db = TempDb::new();
        let (anchor_id, head) = clock_db(&db, 2);
        let conn = open_encrypted(db.path());
        conn.execute(
            "UPDATE tsa_anchors SET token_der = ?1 WHERE id = ?2",
            rusqlite::params![token_with_imprint(&[5u8; 32]), anchor_id],
        )
        .unwrap();
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        let orphan: Vec<&String> = report
            .warnings
            .iter()
            .filter(|w| w.starts_with("clock provenance orphan:"))
            .collect();
        assert_eq!(orphan.len(), 1, "{:?}", report.warnings);
        assert!(
            orphan[0].starts_with(&format!(
                "clock provenance orphan: record #1 binds anchor #{anchor_id} to token sha256 "
            )),
            "{}",
            orphan[0]
        );
        assert!(orphan[0].ends_with("(the token was replaced); run log_anchor verify"));
        assert_clock_hygiene(&report, &head);
    }

    #[test]
    fn clock_provenance_offset_beyond_one_bucket_is_a_warning() {
        let db = TempDb::new();
        let (anchor_id, head) = clock_db(&db, 900);
        let report = verify(&open_encrypted(db.path()));
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(
            report.warnings,
            vec![format!(
                "clock offset: record #1 measured the device clock +900 s from anchor \
                 #{anchor_id}'s TSA time — beyond one bucket (600 s); run log_anchor verify"
            )]
        );
        assert_eq!(
            verify::classify_warning(&report.warnings[0]),
            verify::WarningKind::ClockProvenance
        );
        assert_clock_hygiene(&report, &head);

        // Within one bucket either way: nothing.
        let db = TempDb::new();
        clock_db(&db, -600);
        assert!(verify(&open_encrypted(db.path())).warnings.is_empty());

        // A delayed import (row stored two days after issuance) with a
        // positive offset is not a clock reading: not a warning. Negative is.
        for (offset, warns) in [(3600, false), (-3600, true)] {
            let db = TempDb::new();
            let (anchor_id, _) = clock_db(&db, offset);
            let conn = open_encrypted(db.path());
            conn.execute(
                "UPDATE tsa_anchors SET created_bucket_start = ?1 WHERE id = ?2",
                rusqlite::params![FIXTURE_BUCKET + 2 * 86_400, anchor_id],
            )
            .unwrap();
            let report = verify(&conn);
            assert_eq!(
                !report.warnings.is_empty(),
                warns,
                "{offset}: {:?}",
                report.warnings
            );
            if warns {
                assert!(report.warnings[0].contains("<= -3600 s (clamped)"));
            }
        }
    }

    #[test]
    fn clock_provenance_missing_record_is_not_a_warning() {
        // Anchors that predate the ledger have no record; one anchor has a
        // record, the next one none: no warning about the missing one.
        let db = TempDb::new();
        let (_, _) = clock_db(&db, 2);
        let kernel = open_kernel(db.path());
        anchor(&kernel.conn, AnchorSubject::Digest, &[9u8; 32]);
        drop(kernel);
        let report = verify(&open_encrypted(db.path()));
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, Some(2));
        assert_eq!(report.clock_provenance_verified, Some(1));
        assert!(report.warnings.is_empty(), "{:?}", report.warnings);
    }

    fn clock_provenance_head(kernel: &mut Kernel) -> [u8; 32] {
        let record = crate::ClockProvenanceRecord {
            schema: crate::CLOCK_PROVENANCE_SCHEMA_V1.to_string(),
            time_bucket: TimeBucket {
                start_epoch_s: FIXTURE_BUCKET as u64,
                size_s: 600,
            },
            anchor_id: 1,
            token_sha256: crate::clock_provenance::token_sha256_hex(&fixture_token_der()),
            observed_offset_s: None,
            poll_gap_s: None,
            kernel_version: "0.0.0-test".to_string(),
        };
        kernel.append_clock_provenance(&record).unwrap().entry_hash
    }

    #[test]
    fn anchors_clock_provenance_head_present_then_absent() {
        head_kind_round_trip(
            AnchorSubject::ClockProvenanceHead,
            clock_provenance_head,
            "clock_provenance",
        );
    }

    /// The anchors pass and the clock-provenance cross-check read from one
    /// snapshot: a row anchored and its record sealed between them is not an
    /// orphan. Without the savepoint the cross-check reads the new record
    /// against an anchors list that never saw its row and warns
    /// "clock provenance orphan".
    #[test]
    fn anchors_and_clock_records_are_read_from_one_snapshot() {
        let db = TempDb::new();
        let cfg = KernelConfig {
            db_path: db.path().to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(60),
            device_key_seed: TEST_SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let fixture = crate::tsa::parse_response(
            &std::fs::read(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/tests/fixtures/tsa/reply.tsr"
            ))
            .expect("reading the TSA fixture"),
        )
        .expect("the fixture token parses");
        let gen_time = crate::tsa::gen_time_unix(&fixture.gen_time).expect("genTime");
        let imprint: [u8; 32] = fixture.imprint.clone().try_into().expect("32-byte imprint");
        let observe = |cfg: &KernelConfig, wall: i64, gap: Option<u32>| -> usize {
            Kernel::open(cfg)
                .expect("open kernel")
                .observe_anchors_for_clock_provenance(&crate::ClockObservation {
                    observed_wall_s: wall as u64,
                    poll_gap_s: gap,
                    kernel_version: "0.0.0-test",
                })
                .expect("observe")
                .len()
        };
        let anchor_fixture = |cfg: &KernelConfig| {
            let kernel = Kernel::open(cfg).expect("open kernel");
            crate::tsa::ensure_anchor_table(&kernel.conn).expect("anchors table");
            crate::tsa::insert_anchor(
                &kernel.conn,
                "digest",
                &imprint,
                "https://tsa.example/tsr",
                &fixture,
            )
            .expect("insert anchor");
        };
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        drop(kernel);
        // The first poll only places the cursor; the next seals anchor #1's record.
        assert_eq!(observe(&cfg, gen_time, None), 0);
        anchor_fixture(&cfg);
        assert_eq!(observe(&cfg, gen_time + 5, Some(5)), 1);

        let writer_cfg = cfg.clone();
        let writer_fixture = fixture.clone();
        BETWEEN_THE_ANCHOR_AND_CLOCK_READS.with(|hook| {
            *hook.borrow_mut() = Some(Box::new(move || {
                let kernel = Kernel::open(&writer_cfg).expect("open kernel");
                crate::tsa::ensure_anchor_table(&kernel.conn).expect("anchors table");
                crate::tsa::insert_anchor(
                    &kernel.conn,
                    "digest",
                    &imprint,
                    "https://tsa.example/tsr",
                    &writer_fixture,
                )
                .expect("insert anchor");
                drop(kernel);
                assert_eq!(observe(&writer_cfg, gen_time + 10, Some(5)), 1);
            }));
        });
        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        BETWEEN_THE_ANCHOR_AND_CLOCK_READS.with(|hook| *hook.borrow_mut() = None);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.anchors_checked, Some(1));
        assert_eq!(report.clock_provenance_verified, Some(1));
        assert!(
            report.warnings.iter().all(|w| !w.contains("orphan")),
            "{:?}",
            report.warnings
        );
        // Both rows and both records exist once the snapshot has ended.
        assert_eq!(crate::tsa::list_anchors(&conn).unwrap().len(), 2);
        assert_eq!(crate::clock_provenance::row_count(&conn).unwrap(), 2);
    }

    // -------------------- OpenTimestamps rows (ots_anchors) --------------------

    fn ots_anchor(conn: &Connection, subject: AnchorSubject, hash: &[u8; 32]) -> i64 {
        let body = std::fs::read(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/tests/fixtures/ots/response-bob.otsr"
        ))
        .unwrap();
        let nonce = [0x24u8; crate::ots::OTS_NONCE_LEN];
        let c = crate::ots::commitment(hash, &nonce);
        let node = crate::ots::parse_timestamp(&body, &c).unwrap();
        let proof = crate::ots::Proof::compose(hash, &nonce, node).unwrap();
        crate::ots::insert_ots_anchor(
            conn,
            TimeBucket {
                start_epoch_s: 600,
                size_s: 600,
            },
            subject,
            hash,
            "https://bob.btc.calendar.opentimestamps.org",
            &proof,
        )
        .unwrap()
    }

    #[test]
    fn ots_table_absent_report_byte_identical() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 1);
        anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        drop(kernel);
        let conn = open_encrypted(db.path());
        assert!(!crate::ots::ots_table_exists(&conn).unwrap());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.ots_anchors_checked, None);
        let json = serde_json::to_string(&report).unwrap();
        assert!(!json.contains("ots_anchors"), "{json}");
        // Byte-identical to the pre-OTS serialization: the field is skipped,
        // so the keys are exactly the ones a pre-OTS build wrote, in order.
        let keys: Vec<&str> = json
            .trim_matches(|c| c == '{' || c == '}')
            .split(',')
            .filter_map(|kv| kv.split(':').next())
            .map(|k| k.trim_matches('"'))
            .collect();
        assert_eq!(
            keys,
            [
                "chain_valid",
                "verdict",
                "identity_verified",
                "high_water_mark_checked",
                "anchors_checked",
                "events_verified",
                "break_glass_receipts_verified",
                "break_glass_granted",
                "break_glass_denied",
                "export_receipts_verified",
                "alarm_count",
                "checkpoint_cutoff_event_id",
                "verified_at_bucket_start",
                "verified_at_bucket_size"
            ]
        );
        assert!(!crate::ots::ots_table_exists(&conn).unwrap(), "verify never creates it");
    }

    #[test]
    fn ots_rows_counted() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 2);
        ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        ots_anchor(&kernel.conn, AnchorSubject::Digest, &[9u8; 32]);
        ots_anchor(
            &kernel.conn,
            AnchorSubject::Digest,
            &AnchorSubject::PolicyHead.empty_ledger_sentinel().unwrap(),
        );
        drop(kernel);
        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.ots_anchors_checked, Some(3));
        assert_eq!(report.anchors_checked, None, "anchors_checked stays RFC 3161 only");
        assert!(report.warnings.is_empty(), "{:?}", report.warnings);
        assert!(serde_json::to_string(&report)
            .unwrap()
            .contains("\"ots_anchors_checked\":3"));
        assert_report_hygiene(&report, &head);
    }

    #[test]
    fn ots_warnings_four_literals_classified_anchor_anomaly_and_hygienic() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 1);
        let malformed = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let unparseable = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let wrong_digest = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let cache = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let good = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        let c = &kernel.conn;
        c.execute("UPDATE ots_anchors SET proof = 'text' WHERE id = ?1", [malformed])
            .unwrap();
        c.execute(
            "UPDATE ots_anchors SET proof = x'00010203' WHERE id = ?1",
            [unparseable],
        )
        .unwrap();
        c.execute(
            "UPDATE ots_anchors SET subject_hash = ?2 WHERE id = ?1",
            rusqlite::params![wrong_digest, vec![7u8; 32]],
        )
        .unwrap();
        c.execute(
            "UPDATE ots_anchors SET attestation = 'bitcoin', bitcoin_height = 840000 WHERE id = ?1",
            [cache],
        )
        .unwrap();
        drop(kernel);
        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(report.chain_valid, "{:?}", report.error);
        assert_eq!(report.ots_anchors_checked, Some(1), "only #{good} passed");
        assert_eq!(
            report.warnings,
            vec![
                format!("ots anchor #{malformed}: row is malformed (proof is not a BLOB); run log_anchor verify"),
                format!("ots anchor #{unparseable}: stored proof does not parse as an OpenTimestamps proof; run log_anchor verify"),
                format!("ots anchor #{wrong_digest}: proof digest does not match the subject hash the row claims; run log_anchor verify"),
                format!("ots anchor #{cache}: attestation columns disagree with the stored proof; run log_anchor verify"),
            ]
        );
        for w in &report.warnings {
            assert_eq!(verify::classify_warning(w), verify::WarningKind::AnchorAnomaly);
            assert!(!w.contains("http"), "no URL in a warning: {w}");
            assert!(!w.contains("840000"), "no height in a warning: {w}");
        }
        assert_report_hygiene(&report, &head);
    }

    #[test]
    fn ots_head_gone_is_anchor_regression_on_ots_ledger() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 2);
        let id = ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        kernel
            .conn
            .execute("DELETE FROM sealed_events WHERE id = 2", [])
            .unwrap();
        drop(kernel);
        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        assert!(!report.chain_valid);
        assert_eq!(report.verdict, "verification failed");
        let failure = report.failure.clone().expect("structured failure");
        assert_eq!(failure.ledger, verify::FailedLedger::OtsAnchors);
        assert_eq!(failure.kind, verify::FailureKind::AnchorRegression);
        assert_eq!(failure.entry_id, Some(id));
        assert_eq!(
            failure.detail,
            format!(
                "anchor regression: ots anchor #{id} covers chain_head {}… which is not in \
                 this database's chain history — rollback, truncation, or a different history \
                 than the one that was anchored",
                hex::encode(&head[..8])
            )
        );
        let json = serde_json::to_string(&failure).unwrap();
        assert!(json.contains("\"ledger\":\"ots_anchors\""), "{json}");
        assert_eq!(report.ots_anchors_checked, Some(0));
        assert_report_hygiene(&report, &head);
        assert_eq!(
            crate::verify_explain::ledger_name(verify::FailedLedger::OtsAnchors),
            "OpenTimestamps anchor table"
        );
    }

    #[test]
    fn tsa_block_unchanged_with_both_tables() {
        let db = TempDb::new();
        let mut kernel = open_kernel(db.path());
        write_test_event(&mut kernel);
        write_test_event(&mut kernel);
        let head = event_hash(&kernel.conn, 2);
        let anchor_id = anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        ots_anchor(&kernel.conn, AnchorSubject::ChainHead, &head);
        kernel
            .conn
            .execute("DELETE FROM sealed_events WHERE id = 2", [])
            .unwrap();
        drop(kernel);
        let conn = open_encrypted(db.path());
        let report = verify(&conn);
        // The TSA block runs first and fails exactly as it does alone.
        let failure = regression(&report, anchor_id);
        assert_eq!(
            failure.detail,
            format!(
                "anchor regression: anchor #{anchor_id} covers chain_head {}… which is not in \
                 this database's chain history — rollback, truncation, or a different history \
                 than the one that was anchored",
                hex::encode(&head[..8])
            )
        );
        assert_eq!(report.anchors_checked, Some(0));
        assert_report_hygiene(&report, &head);
    }
}
