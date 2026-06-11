//! Canonical evidence envelope — the single, versioned, self-verifying interchange
//! format for the Privacy Witness Kernel's chains.
//!
//! See `spec/evidence_envelope.md` for the normative definition. The envelope wraps
//! (rather than replaces) the existing [`ExportBundle`]: it carries the coarse,
//! human-readable [`ExportArtifact`] plus the four hash-chained ledgers verbatim, a
//! self-describing manifest, provenance, explicit gaps, and a disclosure manifest.
//!
//! Two verifiers MUST agree on this format byte-for-byte: the Rust verifier here
//! ([`verify_envelope`]) and the offline JavaScript verifier (`viewer/verify_core.js`).

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::canonical_json::{self, CANONICALIZATION_ID};
use crate::crypto::signatures::{
    PqPublicKey, SignatureMode, SignatureSet, DOMAIN_BREAK_GLASS_RECEIPT, DOMAIN_CHECKPOINT,
    DOMAIN_EXPORT_RECEIPT, DOMAIN_SEALED_LOG_ENTRY, ED25519_SCHEME_ID, PQ_SCHEME_MLDSA44,
};
use crate::verify;
use crate::{
    approvals_commitment, hash_entry, verify_entry_signature, Approval, BreakGlassReceipt,
    ExportArtifact, ExportReceiptEntry,
};

/// Fixed format identifier. Never reused for an incompatible layout.
pub const ENVELOPE_FORMAT: &str = "securacv-evidence-envelope";
/// Monotonic envelope version. Bump for any incompatible change.
pub const ENVELOPE_VERSION: u32 = 1;

const MIN_BUCKET_S: u32 = 300;
const DEFAULT_BUCKET_S: u32 = 600;

// --------------------------------------------------------------------------
// Schema
// --------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EvidenceEnvelope {
    pub envelope_format: String,
    pub envelope_version: u32,
    pub manifest: EvidenceManifest,
    pub provenance: Provenance,
    pub ledgers: Ledgers,
    pub artifact: ExportArtifact,
    pub gaps: Gaps,
    pub disclosure: DisclosureManifest,
    pub export_receipt_entry: ExportReceiptEntry,
    /// SHA-256 (hex) over the canonical digest input (see `spec` §9).
    pub whole_envelope_digest: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct EvidenceManifest {
    pub permitted_fields: Vec<String>,
    pub forbidden_fields: Vec<String>,
    pub time_granularity: TimeGranularity,
    pub hash_rule: String,
    pub canonicalization: String,
    pub signature_domains: SignatureDomains,
    pub signature_schemes: SignatureSchemes,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimeGranularity {
    pub min_bucket_s: u32,
    pub default_bucket_s: u32,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SignatureDomains {
    pub sealed_log_entry: String,
    pub checkpoint: String,
    pub break_glass: String,
    pub export_receipt: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SignatureSchemes {
    pub classical: String,
    pub pq: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Provenance {
    pub kernel_version: String,
    pub ruleset_id: String,
    #[serde(with = "hex32")]
    pub ruleset_hash: [u8; 32],
    #[serde(with = "hex32")]
    pub device_public_key: [u8; 32],
    #[serde(with = "hex_opt_bytes")]
    pub pq_public_key: Option<Vec<u8>>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Ledgers {
    pub sealed_events: LedgerSummary<SealedEntryView>,
    pub break_glass_receipts: LedgerSummary<BreakGlassEntryView>,
    pub export_receipts: LedgerSummary<SealedEntryView>,
    pub checkpoints: CheckpointsField,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct LedgerSummary<E> {
    #[serde(with = "hex32_opt")]
    pub head_hash: Option<[u8; 32]>,
    pub count: u64,
    pub entries: Vec<E>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SealedEntryView {
    /// The exact UTF-8 bytes that were hashed at seal time.
    pub payload_json: String,
    #[serde(with = "hex32")]
    pub prev_hash: [u8; 32],
    #[serde(with = "hex32")]
    pub entry_hash: [u8; 32],
    pub signatures: SignatureSet,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BreakGlassEntryView {
    pub payload_json: String,
    pub approvals_json: String,
    #[serde(with = "hex32")]
    pub prev_hash: [u8; 32],
    #[serde(with = "hex32")]
    pub entry_hash: [u8; 32],
    pub signatures: SignatureSet,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CheckpointsField {
    pub latest: Option<CheckpointView>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CheckpointView {
    #[serde(with = "hex32")]
    pub chain_head_hash: [u8; 32],
    pub cutoff_event_id: i64,
    pub signatures: SignatureSet,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Gaps {
    pub failure_count: u64,
    pub checkpoint_cutoff_event_id: Option<i64>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DisclosureManifest {
    pub profile: String,
    pub disclosed_window: Option<DisclosedWindow>,
    pub break_glass_included: bool,
    pub redactions: Vec<Redaction>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DisclosedWindow {
    pub start_bucket_s: u64,
    pub end_bucket_s: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Redaction {
    pub ledger: String,
    pub omitted_count: u64,
    pub omitted_entry_hashes: Vec<String>,
}

impl EvidenceManifest {
    /// The v1 manifest. The rules carried here are what a reader applies to a v1 bundle.
    pub fn v1() -> Self {
        Self {
            permitted_fields: [
                "event_type",
                "time_bucket",
                "zone_id",
                "confidence",
                "kernel_version",
                "ruleset_id",
            ]
            .iter()
            .map(|s| s.to_string())
            .collect(),
            forbidden_fields: [
                "raw_media",
                "precise_timestamp",
                "absolute_location",
                "stable_identifier",
                "free_text",
            ]
            .iter()
            .map(|s| s.to_string())
            .collect(),
            time_granularity: TimeGranularity {
                min_bucket_s: MIN_BUCKET_S,
                default_bucket_s: DEFAULT_BUCKET_S,
            },
            hash_rule: "SHA256(prev_hash || payload_json_utf8_bytes)".to_string(),
            canonicalization: CANONICALIZATION_ID.to_string(),
            signature_domains: SignatureDomains {
                sealed_log_entry: DOMAIN_SEALED_LOG_ENTRY.to_string(),
                checkpoint: DOMAIN_CHECKPOINT.to_string(),
                break_glass: DOMAIN_BREAK_GLASS_RECEIPT.to_string(),
                export_receipt: DOMAIN_EXPORT_RECEIPT.to_string(),
            },
            signature_schemes: SignatureSchemes {
                classical: ED25519_SCHEME_ID.to_string(),
                pq: PQ_SCHEME_MLDSA44.to_string(),
            },
        }
    }
}

// --------------------------------------------------------------------------
// Assembly
// --------------------------------------------------------------------------

/// Inputs required to assemble an envelope, gathered by the kernel from its ledgers.
pub struct EnvelopeParts {
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
    pub device_public_key: [u8; 32],
    pub pq_public_key: Option<Vec<u8>>,
    pub artifact: ExportArtifact,
    pub export_receipt_entry: ExportReceiptEntry,
    pub sealed_events: Vec<SealedEntryView>,
    pub break_glass_receipts: Vec<BreakGlassEntryView>,
    pub export_receipts: Vec<SealedEntryView>,
    pub checkpoint: Option<CheckpointView>,
}

impl EvidenceEnvelope {
    /// Assemble a `profile: "full"` envelope and compute its digest.
    pub fn assemble(parts: EnvelopeParts) -> Result<Self> {
        let failure_count = parts
            .artifact
            .batches
            .iter()
            .flat_map(|b| b.buckets.iter())
            .map(|bucket| bucket.failures.len() as u64)
            .sum();

        let break_glass_included = !parts.break_glass_receipts.is_empty();

        let ledgers = Ledgers {
            sealed_events: LedgerSummary {
                head_hash: parts.sealed_events.last().map(|e| e.entry_hash),
                count: parts.sealed_events.len() as u64,
                entries: parts.sealed_events,
            },
            break_glass_receipts: LedgerSummary {
                head_hash: parts.break_glass_receipts.last().map(|e| e.entry_hash),
                count: parts.break_glass_receipts.len() as u64,
                entries: parts.break_glass_receipts,
            },
            export_receipts: LedgerSummary {
                head_hash: parts.export_receipts.last().map(|e| e.entry_hash),
                count: parts.export_receipts.len() as u64,
                entries: parts.export_receipts,
            },
            checkpoints: CheckpointsField {
                latest: parts.checkpoint.clone(),
            },
        };

        let mut envelope = EvidenceEnvelope {
            envelope_format: ENVELOPE_FORMAT.to_string(),
            envelope_version: ENVELOPE_VERSION,
            manifest: EvidenceManifest::v1(),
            provenance: Provenance {
                kernel_version: parts.kernel_version,
                ruleset_id: parts.ruleset_id,
                ruleset_hash: parts.ruleset_hash,
                device_public_key: parts.device_public_key,
                pq_public_key: parts.pq_public_key,
            },
            ledgers,
            artifact: parts.artifact,
            gaps: Gaps {
                failure_count,
                checkpoint_cutoff_event_id: parts.checkpoint.map(|c| c.cutoff_event_id),
            },
            disclosure: DisclosureManifest {
                profile: "full".to_string(),
                disclosed_window: None,
                break_glass_included,
                redactions: Vec::new(),
            },
            export_receipt_entry: parts.export_receipt_entry,
            whole_envelope_digest: String::new(),
        };
        envelope.whole_envelope_digest = compute_whole_envelope_digest(&envelope)?;
        Ok(envelope)
    }
}

/// Compute the `whole_envelope_digest`: SHA-256 over the canonical digest input, where the
/// artifact is bound by reference (its committed hash) so the input contains no floats.
pub fn compute_whole_envelope_digest(envelope: &EvidenceEnvelope) -> Result<String> {
    let mut value = serde_json::to_value(envelope)?;
    let obj = value
        .as_object_mut()
        .ok_or_else(|| anyhow!("envelope must serialize to a JSON object"))?;
    obj.remove("whole_envelope_digest");
    // Bind the artifact by reference (its committed SHA-256) rather than by value; this keeps
    // the digest input free of floating-point numbers (e.g. `confidence: f32`).
    let artifact_hash_hex = hex::encode(envelope.export_receipt_entry.receipt.artifact_hash);
    obj.insert("artifact".to_string(), Value::String(artifact_hash_hex));
    let bytes = canonical_json::to_canonical_bytes(&value)?;
    Ok(hex::encode(Sha256::digest(&bytes)))
}

// --------------------------------------------------------------------------
// Verification
// --------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IntegrityStatus {
    Ok,
    ValidWithWarnings,
}

#[derive(Clone, Debug)]
pub struct EnvelopeReport {
    pub status: IntegrityStatus,
    pub sealed_events: u64,
    pub break_glass_granted: u64,
    pub break_glass_denied: u64,
    pub export_receipts: u64,
    pub pq_checked: bool,
    pub warnings: Vec<String>,
}

/// Fully verify an evidence envelope from its own contents (no database, no network).
///
/// Mirrors `src/verify.rs`. Returns an error on any integrity, hash, or signature failure
/// (status `compromised`); otherwise returns a report whose status is `Ok` or
/// `ValidWithWarnings` (gaps, redactions, or PQ-not-checked).
pub fn verify_envelope(envelope: &EvidenceEnvelope, mode: SignatureMode) -> Result<EnvelopeReport> {
    if envelope.envelope_format != ENVELOPE_FORMAT {
        return Err(anyhow!(
            "unknown envelope_format: {}",
            envelope.envelope_format
        ));
    }
    if envelope.envelope_version != ENVELOPE_VERSION {
        return Err(anyhow!(
            "unsupported envelope_version {} (this verifier supports {})",
            envelope.envelope_version,
            ENVELOPE_VERSION
        ));
    }

    // The manifest carries the rules used to interpret the bundle ("rules travel with the
    // data"). For a v1 envelope it MUST equal the canonical v1 manifest; otherwise a forged
    // bundle could claim weaker domains/hash rules (with a recomputed digest) yet still pass.
    // This keeps the manifest the verifier reads identical to the constants it verifies with.
    if envelope.manifest != EvidenceManifest::v1() {
        return Err(anyhow!(
            "manifest does not match the canonical v1 manifest for envelope_version 1"
        ));
    }

    // 1. Digest integrity.
    let expected_digest = compute_whole_envelope_digest(envelope)?;
    if expected_digest != envelope.whole_envelope_digest {
        return Err(anyhow!(
            "whole_envelope_digest mismatch: computed={}, stored={}",
            expected_digest,
            envelope.whole_envelope_digest
        ));
    }

    // 2. Keys.
    let verifying_key =
        ed25519_dalek::VerifyingKey::from_bytes(&envelope.provenance.device_public_key)
            .map_err(|e| anyhow!("invalid device public key: {}", e))?;
    let pq_key = parse_pq_key(envelope.provenance.pq_public_key.as_deref());

    let mut warnings = Vec::new();
    let mut pq_checked = false;

    // 3. Sealed events chain (starts at checkpoint head if present, else genesis).
    let sealed_start = envelope
        .ledgers
        .checkpoints
        .latest
        .as_ref()
        .map(|c| c.chain_head_hash)
        .unwrap_or([0u8; 32]);
    let sealed_iter = envelope.ledgers.sealed_events.entries.iter().map(|e| {
        (
            e.payload_json.as_str(),
            &e.prev_hash,
            &e.entry_hash,
            &e.signatures,
        )
    });
    let sealed_head = verify_linear_chain(
        sealed_iter,
        sealed_start,
        DOMAIN_SEALED_LOG_ENTRY,
        &verifying_key,
        mode,
        pq_key.as_ref(),
        "sealed_events",
        &mut pq_checked,
    )?;
    validate_ledger_summary(
        "sealed_events",
        envelope.ledgers.sealed_events.entries.len(),
        envelope.ledgers.sealed_events.count,
        envelope.ledgers.sealed_events.head_hash,
        sealed_head,
    )?;

    // 4. Checkpoint signature.
    if let Some(cp) = envelope.ledgers.checkpoints.latest.as_ref() {
        verify_entry_signature(
            &verifying_key,
            &cp.chain_head_hash,
            &cp.signatures,
            mode,
            pq_key.as_ref(),
            DOMAIN_CHECKPOINT,
        )
        .map_err(|e| {
            anyhow::Error::new(verify::VerifyFailure {
                ledger: verify::FailedLedger::Checkpoint,
                entry_id: None,
                kind: verify::FailureKind::CheckpointInvalid,
                detail: format!("checkpoint signature verification failed: {}", e),
            })
        })?;
    }

    // 5. Break-glass receipts: chain + signatures + approvals commitment + tally.
    let bg_iter = envelope
        .ledgers
        .break_glass_receipts
        .entries
        .iter()
        .map(|e| {
            (
                e.payload_json.as_str(),
                &e.prev_hash,
                &e.entry_hash,
                &e.signatures,
            )
        });
    let bg_head = verify_linear_chain(
        bg_iter,
        [0u8; 32],
        DOMAIN_BREAK_GLASS_RECEIPT,
        &verifying_key,
        mode,
        pq_key.as_ref(),
        "break_glass_receipts",
        &mut pq_checked,
    )?;
    validate_ledger_summary(
        "break_glass_receipts",
        envelope.ledgers.break_glass_receipts.entries.len(),
        envelope.ledgers.break_glass_receipts.count,
        envelope.ledgers.break_glass_receipts.head_hash,
        bg_head,
    )?;
    let (mut granted, mut denied) = (0u64, 0u64);
    for (idx, entry) in envelope
        .ledgers
        .break_glass_receipts
        .entries
        .iter()
        .enumerate()
    {
        let receipt: BreakGlassReceipt = serde_json::from_str(&entry.payload_json)?;
        let approvals: Vec<Approval> = serde_json::from_str(&entry.approvals_json)?;
        let commitment = approvals_commitment(&approvals);
        if commitment != receipt.approvals_commitment {
            return Err(chain_fail(
                "break_glass_receipts",
                idx,
                verify::FailureKind::ApprovalsCommitmentMismatch,
                format!(
                    "break-glass approvals commitment mismatch: stored={}, computed={}",
                    hex::encode(receipt.approvals_commitment),
                    hex::encode(commitment)
                ),
            ));
        }
        match receipt.outcome {
            crate::BreakGlassOutcome::Granted => granted += 1,
            crate::BreakGlassOutcome::Denied { .. } => denied += 1,
        }
    }

    // 6. Export receipts chain.
    let exp_iter = envelope.ledgers.export_receipts.entries.iter().map(|e| {
        (
            e.payload_json.as_str(),
            &e.prev_hash,
            &e.entry_hash,
            &e.signatures,
        )
    });
    let exp_head = verify_linear_chain(
        exp_iter,
        [0u8; 32],
        DOMAIN_EXPORT_RECEIPT,
        &verifying_key,
        mode,
        pq_key.as_ref(),
        "export_receipts",
        &mut pq_checked,
    )?;
    validate_ledger_summary(
        "export_receipts",
        envelope.ledgers.export_receipts.entries.len(),
        envelope.ledgers.export_receipts.count,
        envelope.ledgers.export_receipts.head_hash,
        exp_head,
    )?;

    // 7. Artifact binding + export-receipt entry signature.
    let artifact_bytes = serde_json::to_vec(&envelope.artifact)?;
    let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
    if artifact_hash != envelope.export_receipt_entry.receipt.artifact_hash {
        return Err(anyhow!("export artifact hash mismatch"));
    }
    // Provenance must be consistent with the signed export receipt.
    if envelope.provenance.ruleset_hash != envelope.export_receipt_entry.receipt.ruleset_hash {
        return Err(anyhow!(
            "provenance ruleset_hash does not match the signed export receipt ruleset_hash"
        ));
    }
    let receipt_payload = serde_json::to_string(&envelope.export_receipt_entry.receipt)?;
    let computed_entry_hash = hash_entry(
        &envelope.export_receipt_entry.prev_hash,
        receipt_payload.as_bytes(),
    );
    if computed_entry_hash != envelope.export_receipt_entry.entry_hash {
        return Err(anyhow!("export receipt entry hash mismatch"));
    }
    verify_entry_signature(
        &verifying_key,
        &envelope.export_receipt_entry.entry_hash,
        &envelope.export_receipt_entry.signatures,
        mode,
        pq_key.as_ref(),
        DOMAIN_EXPORT_RECEIPT,
    )?;

    // Warnings & status.
    let has_pq_material = envelope.provenance.pq_public_key.is_some();
    if has_pq_material && !pq_checked {
        warnings.push("post-quantum signatures present but not verified".to_string());
    } else if !has_pq_material {
        warnings.push("no post-quantum public key; PQ signatures not checked".to_string());
    }
    if envelope.gaps.failure_count > 0 {
        warnings.push(format!(
            "{} failure record(s) present (explicit gaps)",
            envelope.gaps.failure_count
        ));
    }
    if !envelope.disclosure.redactions.is_empty() {
        warnings.push("disclosure contains redactions".to_string());
    }

    let status = if warnings.is_empty() {
        IntegrityStatus::Ok
    } else {
        IntegrityStatus::ValidWithWarnings
    };

    Ok(EnvelopeReport {
        status,
        // Report verified counts (validated to equal the declared summary above).
        sealed_events: envelope.ledgers.sealed_events.entries.len() as u64,
        break_glass_granted: granted,
        break_glass_denied: denied,
        export_receipts: envelope.ledgers.export_receipts.entries.len() as u64,
        pq_checked,
        warnings,
    })
}

/// Validate a ledger's declared summary (`count`, `head_hash`) against the entries that were
/// actually verified. The summary fields are not individually signed, so they must be checked
/// against the verified chain to prevent a forged bundle from misreporting its contents.
fn validate_ledger_summary(
    ledger_name: &str,
    entries_len: usize,
    declared_count: u64,
    declared_head: Option<[u8; 32]>,
    computed_head: [u8; 32],
) -> Result<()> {
    if declared_count != entries_len as u64 {
        return Err(anyhow!(
            "{} count mismatch: declared={}, actual={}",
            ledger_name,
            declared_count,
            entries_len
        ));
    }
    let expected_head = if entries_len == 0 {
        None
    } else {
        Some(computed_head)
    };
    if declared_head != expected_head {
        return Err(anyhow!(
            "{} head_hash does not match the verified chain head",
            ledger_name
        ));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
// Map a ledger's wire name onto the structured failure location. The wire
// names are fixed by the envelope format; the message strings keep using them
// verbatim so existing consumers (and the parity fixtures) see unchanged text.
fn failed_ledger_from_name(name: &str) -> verify::FailedLedger {
    match name {
        "break_glass_receipts" => verify::FailedLedger::BreakGlassReceipts,
        "export_receipts" => verify::FailedLedger::ExportReceipts,
        _ => verify::FailedLedger::SealedEvents,
    }
}

fn chain_fail(
    ledger_name: &str,
    idx: usize,
    kind: verify::FailureKind,
    detail: String,
) -> anyhow::Error {
    anyhow::Error::new(verify::VerifyFailure {
        ledger: failed_ledger_from_name(ledger_name),
        entry_id: Some(idx as i64),
        kind,
        detail,
    })
}

// The argument list mirrors the verification context (chain start, domain,
// keys, mode, ledger identity) — bundling them into a struct would obscure
// the 1:1 mapping with the Rust/JS parity algorithm for no reuse benefit.
#[allow(clippy::too_many_arguments)]
fn verify_linear_chain<'a, I>(
    entries: I,
    mut expected_prev: [u8; 32],
    domain: &str,
    verifying_key: &ed25519_dalek::VerifyingKey,
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
    ledger_name: &str,
    pq_checked: &mut bool,
) -> Result<[u8; 32]>
where
    I: IntoIterator<Item = (&'a str, &'a [u8; 32], &'a [u8; 32], &'a SignatureSet)>,
{
    for (idx, (payload, prev_hash, entry_hash, signatures)) in entries.into_iter().enumerate() {
        if *prev_hash != expected_prev {
            return Err(chain_fail(
                ledger_name,
                idx,
                verify::FailureKind::PrevHashMismatch,
                format!(
                    "{} integrity check failed at index {}: prev_hash={}, expected={}",
                    ledger_name,
                    idx,
                    hex::encode(prev_hash),
                    hex::encode(expected_prev)
                ),
            ));
        }
        let computed = hash_entry(&expected_prev, payload.as_bytes());
        if computed != *entry_hash {
            return Err(chain_fail(
                ledger_name,
                idx,
                verify::FailureKind::EntryHashMismatch,
                format!(
                    "{} integrity check failed at index {}: computed_hash={}, stored_hash={}",
                    ledger_name,
                    idx,
                    hex::encode(computed),
                    hex::encode(entry_hash)
                ),
            ));
        }
        verify_entry_signature(
            verifying_key,
            entry_hash,
            signatures,
            mode,
            pq_public_key,
            domain,
        )
        .map_err(|e| {
            chain_fail(
                ledger_name,
                idx,
                verify::FailureKind::SignatureMismatch,
                format!(
                    "{} signature verification failed at index {}: {}",
                    ledger_name, idx, e
                ),
            )
        })?;
        if pq_public_key.is_some() && signatures.pq_signature.is_some() {
            *pq_checked = true;
        }
        expected_prev = *entry_hash;
    }
    Ok(expected_prev)
}

#[cfg(feature = "pqc-signatures")]
fn parse_pq_key(bytes: Option<&[u8]>) -> Option<PqPublicKey> {
    use pqcrypto_traits::sign::PublicKey as _;
    bytes.and_then(|b| PqPublicKey::from_bytes(b).ok())
}

#[cfg(not(feature = "pqc-signatures"))]
fn parse_pq_key(_bytes: Option<&[u8]>) -> Option<PqPublicKey> {
    None
}

// --------------------------------------------------------------------------
// Ledger readers (raw rows -> envelope views; no re-verification here)
// --------------------------------------------------------------------------

use rusqlite::Connection;

fn row_blob32(bytes: Vec<u8>, ctx: &str) -> Result<[u8; 32]> {
    if bytes.len() != 32 {
        return Err(anyhow!(
            "corrupt {}: expected 32 bytes, got {}",
            ctx,
            bytes.len()
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

/// Read all sealed events as envelope views, in chain order.
pub fn read_sealed_events(conn: &Connection) -> Result<Vec<SealedEntryView>> {
    let mut stmt = conn.prepare(
        "SELECT payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM sealed_events ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let payload_json: String = row.get(0)?;
        let prev_hash = row_blob32(row.get(1)?, "sealed_events.prev_hash")?;
        let entry_hash = row_blob32(row.get(2)?, "sealed_events.entry_hash")?;
        let signature: Vec<u8> = row.get(3)?;
        let pq_signature: Option<Vec<u8>> = row.get(4)?;
        let pq_scheme: Option<String> = row.get(5)?;
        let signatures = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        out.push(SealedEntryView {
            payload_json,
            prev_hash,
            entry_hash,
            signatures,
        });
    }
    Ok(out)
}

/// Read all break-glass receipts as envelope views, in chain order.
pub fn read_break_glass_receipts(conn: &Connection) -> Result<Vec<BreakGlassEntryView>> {
    let mut stmt = conn.prepare(
        "SELECT payload_json, approvals_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM break_glass_receipts ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let payload_json: String = row.get(0)?;
        let approvals_json: String = row.get(1)?;
        let prev_hash = row_blob32(row.get(2)?, "break_glass_receipts.prev_hash")?;
        let entry_hash = row_blob32(row.get(3)?, "break_glass_receipts.entry_hash")?;
        let signature: Vec<u8> = row.get(4)?;
        let pq_signature: Option<Vec<u8>> = row.get(5)?;
        let pq_scheme: Option<String> = row.get(6)?;
        let signatures = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        out.push(BreakGlassEntryView {
            payload_json,
            approvals_json,
            prev_hash,
            entry_hash,
            signatures,
        });
    }
    Ok(out)
}

/// Read all export receipts as envelope views, in chain order.
pub fn read_export_receipts(conn: &Connection) -> Result<Vec<SealedEntryView>> {
    let mut stmt = conn.prepare(
        "SELECT payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM export_receipts ORDER BY id ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let payload_json: String = row.get(0)?;
        let prev_hash = row_blob32(row.get(1)?, "export_receipts.prev_hash")?;
        let entry_hash = row_blob32(row.get(2)?, "export_receipts.entry_hash")?;
        let signature: Vec<u8> = row.get(3)?;
        let pq_signature: Option<Vec<u8>> = row.get(4)?;
        let pq_scheme: Option<String> = row.get(5)?;
        let signatures = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        out.push(SealedEntryView {
            payload_json,
            prev_hash,
            entry_hash,
            signatures,
        });
    }
    Ok(out)
}

/// Read the latest checkpoint as an envelope view, if one exists.
pub fn read_checkpoint(conn: &Connection) -> Result<Option<CheckpointView>> {
    let info = crate::verify::latest_checkpoint(conn)?;
    match (info.chain_head_hash, info.signatures, info.cutoff_event_id) {
        (Some(chain_head_hash), Some(signatures), Some(cutoff_event_id)) => {
            Ok(Some(CheckpointView {
                chain_head_hash,
                cutoff_event_id,
                signatures,
            }))
        }
        _ => Ok(None),
    }
}

// --------------------------------------------------------------------------
// serde hex helpers
// --------------------------------------------------------------------------

mod hex32 {
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(value: &[u8; 32], s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&hex::encode(value))
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<[u8; 32], D::Error> {
        let s = String::deserialize(d)?;
        let bytes = hex::decode(&s).map_err(serde::de::Error::custom)?;
        if bytes.len() != 32 {
            return Err(serde::de::Error::custom(format!(
                "expected 32-byte hex, got {} bytes",
                bytes.len()
            )));
        }
        let mut out = [0u8; 32];
        out.copy_from_slice(&bytes);
        Ok(out)
    }
}

mod hex32_opt {
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(value: &Option<[u8; 32]>, s: S) -> Result<S::Ok, S::Error> {
        match value {
            Some(v) => s.serialize_some(&hex::encode(v)),
            None => s.serialize_none(),
        }
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Option<[u8; 32]>, D::Error> {
        let opt = Option::<String>::deserialize(d)?;
        match opt {
            None => Ok(None),
            Some(s) => {
                let bytes = hex::decode(&s).map_err(serde::de::Error::custom)?;
                if bytes.len() != 32 {
                    return Err(serde::de::Error::custom(format!(
                        "expected 32-byte hex, got {} bytes",
                        bytes.len()
                    )));
                }
                let mut out = [0u8; 32];
                out.copy_from_slice(&bytes);
                Ok(Some(out))
            }
        }
    }
}

mod hex_opt_bytes {
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(value: &Option<Vec<u8>>, s: S) -> Result<S::Ok, S::Error> {
        match value {
            Some(v) => s.serialize_some(&hex::encode(v)),
            None => s.serialize_none(),
        }
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Option<Vec<u8>>, D::Error> {
        let opt = Option::<String>::deserialize(d)?;
        match opt {
            None => Ok(None),
            Some(s) => Ok(Some(hex::decode(&s).map_err(serde::de::Error::custom)?)),
        }
    }
}
