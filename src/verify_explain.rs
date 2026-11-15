//! Plain-language explanations for verification failures and timeline
//! warnings.
//!
//! `log_verify` and `envelope_verify` both print diagnoses from this table so
//! the two CLIs cannot drift. The offline viewer mirrors it in JS — that copy
//! is presentation-only (wording may differ; the structured
//! `{ledger, entry_id, kind}` triple is what stays in cross-language parity).

use crate::verify::{FailedLedger, FailureKind, VerifyFailure, WarningKind};

pub fn ledger_name(ledger: FailedLedger) -> &'static str {
    match ledger {
        FailedLedger::SealedEvents => "sealed event log",
        FailedLedger::BreakGlassReceipts => "break-glass receipt chain",
        FailedLedger::ExportReceipts => "export receipt chain",
        FailedLedger::Checkpoint => "retention checkpoint",
        FailedLedger::KeyLineage => "device key lineage",
        FailedLedger::HighWaterMark => "signed external high-water-mark",
        FailedLedger::Anchors => "RFC 3161 anchor table",
    }
}

pub struct FailureExplanation {
    pub what: &'static str,
    pub likely_causes: &'static str,
    pub next_steps: &'static str,
}

pub fn explain_failure(kind: FailureKind) -> FailureExplanation {
    match kind {
        FailureKind::PrevHashMismatch => FailureExplanation {
            what: "this entry no longer links to the previous entry's hash — the chain is \
                   broken at this point; everything before it still verified.",
            likely_causes: "rows were deleted or reordered; the database was partially \
                   restored from an older backup; the database file is older than its \
                   checkpoint (stale backup).",
            next_steps: "do NOT edit the database to \"fix\" it — that destroys evidence. \
                   Run with --verbose to see the last good entry; compare against backups. \
                   If you restored from a backup, the original (pre-restore) database is \
                   the authoritative record.",
        },
        FailureKind::EntryHashMismatch => FailureExplanation {
            what: "the stored payload at this entry no longer matches the hash that was \
                   sealed into the chain when it was written.",
            likely_causes: "the row was edited in place; disk corruption; a partial restore \
                   mixed rows from different database generations.",
            next_steps: "do NOT edit the database to \"fix\" it. Check disk health \
                   (dmesg, SMART), compare against backups, and run with --verbose to see \
                   how far the chain verifies.",
        },
        FailureKind::SignatureMismatch => FailureExplanation {
            what: "the entry's hash is consistent, but its signature does not verify under \
                   the expected device key.",
            likely_causes: "wrong verifying key — e.g. --public-key from a different \
                   device, or a database whose device key rotated; less commonly, a forged \
                   entry signed by another key.",
            next_steps: "retry with --device-key-seed (derives the correct key from the \
                   seed) or without --public-key so the key stored in the database is \
                   used. If the key legitimately rotated, the chain carries the rotation \
                   record — verify from genesis.",
        },
        FailureKind::KeyRotationInvalid => FailureExplanation {
            what: "a device-key rotation record failed validation, so entries after it \
                   cannot be tied back to the device's original identity.",
            likely_causes: "the rotation record was tampered with; the rotation was \
                   recorded by software that didn't hold the previous key.",
            next_steps: "treat entries after this id as unattributed. Verify earlier \
                   history with the genesis key, and check when/why the device key was \
                   rotated (docs/db_key_rotation.md).",
        },
        FailureKind::CheckpointInvalid => FailureExplanation {
            what: "the retention checkpoint (the signed head the pruned history was \
                   collapsed into) failed signature or shape validation.",
            likely_causes: "the checkpoint row was edited or forged; verification ran with \
                   the wrong key.",
            next_steps: "retry with --device-key-seed. If the signature still fails, the \
                   checkpoint cannot vouch for pruned history — treat events before the \
                   checkpoint cutoff as unverifiable and the database as suspect.",
        },
        FailureKind::ApprovalsCommitmentMismatch => FailureExplanation {
            what: "the trustee approvals stored with this break-glass receipt do not match \
                   the commitment sealed into the signed receipt.",
            likely_causes: "the approvals were altered after the unseal decision was \
                   recorded.",
            next_steps: "treat the unseal audit trail as tampered. Ask the trustees for \
                   their own copies of what they signed and compare request hashes.",
        },
        FailureKind::PolicyViolation => FailureExplanation {
            what: "a break-glass receipt does not satisfy the configured quorum policy \
                   (or no policy is configured while receipts exist).",
            likely_causes: "the quorum policy was changed or removed after receipts were \
                   written; a receipt was recorded without the required approvals.",
            next_steps: "restore the original quorum policy configuration and re-run; \
                   compare the trustee keys in the policy against the approvals.",
        },
        FailureKind::UntrustedSigner => FailureExplanation {
            what: "the checkpoint names a signer key that is not part of the \
                   genesis-anchored device key lineage.",
            likely_causes: "a forged checkpoint signed by a key the device never \
                   authorized; a database assembled from mismatched sources.",
            next_steps: "do not trust the checkpoint or the pruned history behind it. \
                   Verify the key lineage from genesis and check where this database \
                   file came from.",
        },
        FailureKind::HighWaterRegression => FailureExplanation {
            what: "the live sealed log is behind the signed external high-water-mark: \
                   the newest event id (or the head at that id) is lower than the \
                   furthest point the device recorded reaching. The interior chain \
                   still verified — this is loss of the tail, not corruption of the \
                   middle.",
            likely_causes: "the newest events were truncated; an older whole-database \
                   snapshot was restored; the log was wiped. Because the mark is \
                   device-signed and kept outside the database, a rollback that did \
                   not also hold the current signing key cannot move it forward to \
                   match.",
            next_steps: "treat the database as rolled back or truncated — the \
                   authoritative record is whatever holds events up to the mark's \
                   sequence. Recover the newer database (or the append-only/external \
                   copy of the mark), and check where this database file came from. \
                   Do NOT re-sign a shorter chain to clear the error.",
        },
        FailureKind::AnchorRegression => FailureExplanation {
            what: "an RFC 3161 anchor row covers a ledger head that is no longer in that \
                   ledger's history: the hash is in no live row and in no retention \
                   checkpoint whose device signature verifies. The anchor is auxiliary \
                   evidence about the ledger — a state the ledger once had, countersigned \
                   by a time-stamping authority — and this database no longer contains \
                   that state. The interior chain itself verified.",
            likely_causes: "an older whole-database snapshot was restored (rollback); the \
                   tail was truncated past the anchored head; the ledger was regenerated \
                   as a different history than the one that was anchored. One benign \
                   cause: a chain head anchored into a database that an OLDER witnessd \
                   then pruned — before this release, retention did not keep anchored \
                   heads as checkpoints — which the diagnosis text flags when the anchored \
                   row id is at or below the retention cutoff.",
            next_steps: "run `log_anchor verify` for the per-anchor picture, and check \
                   where this database file came from. Only for a head at or below the \
                   retention cutoff of a database pruned by an older build is `log_anchor \
                   relabel --id N --subject digest` the remedy: it records that membership \
                   is no longer asserted and leaves the token untouched. A head NEWER than \
                   the cutoff, or a receipt-ledger or policy head (those ledgers are never \
                   pruned), is a rollback or truncation — do NOT relabel it, and never \
                   delete the anchor row: that destroys evidence, it never clears a \
                   finding.",
        },
        FailureKind::TokenRedemption => FailureExplanation {
            what: "the break-glass chain's record of spent tokens is inconsistent: a nonce \
                   redeemed twice, a redemption naming no earlier Granted receipt, or a nonce \
                   burned in the anti-replay table with no signed redemption row behind it.",
            likely_causes: "a single-use token was replayed (its burn row deleted and the \
                   token presented again); a redemption row was forged or removed from the \
                   chain tail; the token was spent by a build that wrote no redemption rows.",
            next_steps: "treat the unseal as possibly repeated. Compare the chain head \
                   against its last anchor, ask who held the token file, and check the \
                   vault for an unsealed artifact that no redemption row accounts for.",
        },
    }
}

/// One-line actionable hint per timeline-warning class. `None` for warnings
/// we can't classify — the raw warning text still prints either way.
pub fn warning_hint(kind: WarningKind) -> Option<&'static str> {
    match kind {
        WarningKind::ClockSkewNoted => Some(
            "the device itself recorded this clock jump (ClockSkew failure record), so it \
             is documented behavior — usually an NTP correction or a flat RTC battery.",
        ),
        WarningKind::CreatedAtRegression => Some(
            "timestamps went backwards with no ClockSkew record covering the jump. Check \
             whether the system clock was changed around this time; if it wasn't, treat \
             this as possible row manipulation.",
        ),
        WarningKind::CheckpointAnomaly => Some(
            "a checkpoint timestamp is out of order or in the future. Check the system \
             clock history — a back-dated checkpoint can be used to hide pruned history.",
        ),
        WarningKind::MissingHeartbeats => Some(
            "the daemon was running but some 10-minute heartbeats are missing. This \
             usually means the device lost power or crashed — look for PowerLoss or \
             StorageFull failure records near these buckets. If there are none, rows may \
             have been deleted.",
        ),
        WarningKind::StaleTail => Some(
            "the log stops while the daemon claimed to be running. Most often a crash or \
             an unplugged device; if the device demonstrably kept running afterwards, the \
             newest records may have been truncated.",
        ),
        WarningKind::AuditUnavailable => Some(
            "the timeline audit itself could not run — the hash chain is still valid, but \
             tail-truncation and clock checks were skipped.",
        ),
        WarningKind::AnchorAnomaly => Some(
            "this anchor row cannot vouch for anything — its token does not parse, does not \
             cover the hash the row claims, or the row itself is damaged. The ledgers still \
             verified and no anchored head is missing. `log_anchor verify` reports a token \
             problem as FAIL on that row; a malformed row stops it with `corrupt anchor N: …` \
             and must be restored from a backup (export tokens alongside your DB backups).",
        ),
        WarningKind::Other => None,
    }
}

/// Multi-line human diagnosis block for a structured failure. The caller has
/// already printed the raw error string; this adds location and guidance.
pub fn format_failure_diagnosis(failure: &VerifyFailure) -> String {
    let explanation = explain_failure(failure.kind);
    let location = match failure.entry_id {
        Some(id) => format!("{} at entry id {}", ledger_name(failure.ledger), id),
        None => ledger_name(failure.ledger).to_string(),
    };
    format!(
        "Where:           {}\n\
         What this means: {}\n\
         Likely causes:   {}\n\
         Next steps:      {}",
        location, explanation.what, explanation.likely_causes, explanation.next_steps
    )
}
