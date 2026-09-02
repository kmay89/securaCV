//! The chain math the Witness Wall renders from.
//!
//! Apple TV shows a *verified record*, not a video wall — so the only thing
//! this crate does is answer "does the sealed log still verify, and through
//! when?" It holds no state, touches no media, and never writes: the TV is a
//! read-only witness (spec/invariants.md, Invariant I).
//!
//! # Why a second implementation is safe here
//!
//! `tvos/README.md` says "reuse, don't rebuild — never a second implementation
//! that could disagree with the kernel." The kernel's verifier cannot compile
//! for Apple TV (it is welded to rusqlite/sqlcipher), so this follows the
//! precedent already set by the offline JavaScript verifier
//! (`viewer/verify_core.js`): re-implement the *pinned bytes*, then prove
//! byte-identity in CI against the same fixtures the kernel proves itself
//! against — `tests/fixtures/envelope/domain_separation_vectors.json`. See
//! `tests/vectors.rs`. Divergence fails the build; it cannot ship.
//!
//! The two hashes that define the chain, quoted from
//! `src/crypto/signatures.rs` and `src/log/mod.rs`:
//!
//! * entry hash:   `SHA256( prev_hash ‖ payload )`
//! * signing hash: `SHA256( le32(len(domain)) ‖ domain_utf8 ‖ entry_hash )`

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub mod ffi;
pub mod fleet;
pub mod host;

/// Domain separator for sealed-log entries. Must equal
/// `DOMAIN_SEALED_LOG_ENTRY` in `src/crypto/signatures.rs`; `tests/vectors.rs`
/// pins it against the shared fixtures.
pub const DOMAIN_SEALED_LOG_ENTRY: &str = "securacv:pwk:sealed-log-entry:v2";

/// Domain separator for checkpoints (`DOMAIN_CHECKPOINT` in the kernel).
pub const DOMAIN_CHECKPOINT: &str = "securacv:pwk:sealed-log-checkpoint:v2";

/// `SHA256(prev_hash ‖ payload)` — the kernel's `hash_entry`.
pub fn hash_entry(prev_hash: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_hash);
    hasher.update(payload);
    hasher.finalize().into()
}

/// `SHA256(le32(len(domain)) ‖ domain ‖ entry_hash)` — the exact preimage that
/// is Ed25519-signed. The length prefix is what stops a signature made for one
/// domain from being replayed into another.
pub fn domain_separated_hash(domain: &str, entry_hash: &[u8; 32]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update((domain.len() as u32).to_le_bytes());
    hasher.update(domain.as_bytes());
    hasher.update(entry_hash);
    hasher.finalize().into()
}

/// One link of the sealed log, as the kernel serves it to a read-only client.
///
/// Post-quantum signatures are intentionally **not** verified here: the TV is
/// a display, and `SignatureMode::Compat` (what a read-only viewer uses) treats
/// the ML-DSA signature as optional. If it ever needs to be strict, that is a
/// deliberate change with its own fixtures — not a silent default.
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct SealedEntry {
    pub id: i64,
    /// The exact payload bytes that were hashed — a string, not a re-serialized
    /// object. Re-encoding JSON would change the bytes and break the chain.
    pub payload: String,
    pub prev_hash: String,
    pub entry_hash: String,
    pub signature: String,
}

/// Why a chain failed, in the same vocabulary as the kernel's `FailureKind`
/// so a TV screenshot and a `witnessctl verify` transcript say the same word.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum FailureKind {
    PrevHashMismatch,
    EntryHashMismatch,
    SignatureMismatch,
    Malformed,
}

impl FailureKind {
    /// The calm, human sentence the Wall shows. Drift is stated plainly and
    /// never rendered as fine (docs/tvos/AUTOPIPELINE.md, "self-heal").
    pub fn plain_english(self) -> &'static str {
        match self {
            FailureKind::PrevHashMismatch => {
                "The record's links don't line up — an entry is missing or out of order."
            }
            FailureKind::EntryHashMismatch => "An entry's contents changed after it was sealed.",
            FailureKind::SignatureMismatch => {
                "An entry isn't signed by this device's key — it may have been rewritten."
            }
            FailureKind::Malformed => "The record couldn't be read in the expected format.",
        }
    }
}

/// The verdict the Wall renders.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VerifyReport {
    pub ok: bool,
    /// How many entries verified before the walk stopped.
    pub verified: u64,
    /// Chain head after the last verified entry, hex — what a checkpoint pins.
    pub head: String,
    /// `id` of the entry that failed, when one did.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub failed_at: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub kind: Option<FailureKind>,
    /// Operator-facing detail (hashes, positions) — never secrets.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
    /// The sentence the TV puts on screen.
    pub message: String,
}

impl VerifyReport {
    fn success(verified: u64, head: [u8; 32]) -> Self {
        Self {
            ok: true,
            verified,
            head: hex::encode(head),
            failed_at: None,
            kind: None,
            detail: None,
            message: if verified == 0 {
                "Nothing sealed yet.".to_string()
            } else {
                format!("Verified {verified} sealed entries.")
            },
        }
    }

    fn failure(
        verified: u64,
        head: [u8; 32],
        failed_at: Option<i64>,
        kind: FailureKind,
        detail: String,
    ) -> Self {
        Self {
            ok: false,
            verified,
            head: hex::encode(head),
            failed_at,
            kind: Some(kind),
            detail: Some(detail),
            message: kind.plain_english().to_string(),
        }
    }
}

fn hex32(label: &str, value: &str) -> Result<[u8; 32], VerifyReport> {
    let bytes = hex::decode(value).map_err(|e| {
        VerifyReport::failure(
            0,
            [0u8; 32],
            None,
            FailureKind::Malformed,
            format!("{label} is not hex: {e}"),
        )
    })?;
    bytes.try_into().map_err(|_| {
        VerifyReport::failure(
            0,
            [0u8; 32],
            None,
            FailureKind::Malformed,
            format!("{label} is not 32 bytes"),
        )
    })
}

/// Walk the chain from `checkpoint_head` (or genesis) and verify every link.
///
/// Mirrors the kernel's `verify_events_with`: for each entry, `prev_hash` must
/// equal the running head, `entry_hash` must equal `SHA256(prev ‖ payload)`,
/// and the signature must verify over the domain-separated signing hash.
///
/// Key rotation is deliberately **not** followed here. A TV that walked a
/// rotation would have to validate the possession attestation to stay safe,
/// and a display that gets that subtly wrong is worse than one that says so:
/// a rotation record ends the walk with an honest "verified through here".
pub fn verify_chain(
    entries: &[SealedEntry],
    verifying_key: &VerifyingKey,
    checkpoint_head: Option<[u8; 32]>,
) -> VerifyReport {
    let mut expected_prev = checkpoint_head.unwrap_or([0u8; 32]);
    let mut verified = 0u64;

    for entry in entries {
        let prev_hash = match hex32("prev_hash", &entry.prev_hash) {
            Ok(v) => v,
            Err(mut r) => {
                r.verified = verified;
                r.head = hex::encode(expected_prev);
                r.failed_at = Some(entry.id);
                return r;
            }
        };
        let entry_hash = match hex32("entry_hash", &entry.entry_hash) {
            Ok(v) => v,
            Err(mut r) => {
                r.verified = verified;
                r.head = hex::encode(expected_prev);
                r.failed_at = Some(entry.id);
                return r;
            }
        };

        if prev_hash != expected_prev {
            return VerifyReport::failure(
                verified,
                expected_prev,
                Some(entry.id),
                FailureKind::PrevHashMismatch,
                format!(
                    "id {}: prev_hash={}, expected_prev={}",
                    entry.id,
                    hex::encode(prev_hash),
                    hex::encode(expected_prev)
                ),
            );
        }

        let computed = hash_entry(&expected_prev, entry.payload.as_bytes());
        if computed != entry_hash {
            return VerifyReport::failure(
                verified,
                expected_prev,
                Some(entry.id),
                FailureKind::EntryHashMismatch,
                format!(
                    "id {}: computed_hash={}, stored_hash={}",
                    entry.id,
                    hex::encode(computed),
                    hex::encode(entry_hash)
                ),
            );
        }

        let signature_bytes = match hex::decode(&entry.signature) {
            Ok(b) if b.len() == 64 => b,
            _ => {
                return VerifyReport::failure(
                    verified,
                    expected_prev,
                    Some(entry.id),
                    FailureKind::Malformed,
                    format!("id {}: signature is not 64 hex-encoded bytes", entry.id),
                )
            }
        };
        let mut sig = [0u8; 64];
        sig.copy_from_slice(&signature_bytes);

        let signing_hash = domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, &entry_hash);
        if verifying_key
            .verify(&signing_hash, &Signature::from_bytes(&sig))
            .is_err()
        {
            return VerifyReport::failure(
                verified,
                expected_prev,
                Some(entry.id),
                FailureKind::SignatureMismatch,
                format!("id {}: signature mismatch", entry.id),
            );
        }

        expected_prev = entry_hash;
        verified += 1;
    }

    VerifyReport::success(verified, expected_prev)
}

/// What the kernel serves a read-only viewer: the key, an optional checkpoint,
/// and the entries to walk.
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct SealedLogDocument {
    /// Ed25519 verifying key, 32 bytes hex.
    pub verifying_key: String,
    #[serde(default)]
    pub checkpoint_head: Option<String>,
    #[serde(default)]
    pub entries: Vec<SealedEntry>,
}

/// Verify a whole document. This is what the FFI (and therefore Swift) calls.
pub fn verify_document(doc: &SealedLogDocument) -> VerifyReport {
    let key_bytes = match hex32("verifying_key", &doc.verifying_key) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let key = match VerifyingKey::from_bytes(&key_bytes) {
        Ok(k) => k,
        Err(e) => {
            return VerifyReport::failure(
                0,
                [0u8; 32],
                None,
                FailureKind::Malformed,
                format!("verifying_key is not a valid Ed25519 key: {e}"),
            )
        }
    };
    let checkpoint = match doc.checkpoint_head.as_deref() {
        None => None,
        Some(h) => match hex32("checkpoint_head", h) {
            Ok(v) => Some(v),
            Err(r) => return r,
        },
    };
    verify_chain(&doc.entries, &key, checkpoint)
}

/// Parse-and-verify in one step, so a malformed document is a *report*, not a
/// thrown error. The Wall must always have something calm to render.
pub fn verify_json(json: &str) -> VerifyReport {
    match serde_json::from_str::<SealedLogDocument>(json) {
        Ok(doc) => verify_document(&doc),
        Err(e) => VerifyReport::failure(
            0,
            [0u8; 32],
            None,
            FailureKind::Malformed,
            format!("could not parse the sealed log: {e}"),
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    fn sign_entry(key: &SigningKey, id: i64, payload: &str, prev: [u8; 32]) -> SealedEntry {
        let entry_hash = hash_entry(&prev, payload.as_bytes());
        let signing_hash = domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, &entry_hash);
        SealedEntry {
            id,
            payload: payload.to_string(),
            prev_hash: hex::encode(prev),
            entry_hash: hex::encode(entry_hash),
            signature: hex::encode(key.sign(&signing_hash).to_bytes()),
        }
    }

    fn chain(key: &SigningKey, payloads: &[&str]) -> Vec<SealedEntry> {
        let mut prev = [0u8; 32];
        let mut out = Vec::new();
        for (i, p) in payloads.iter().enumerate() {
            let e = sign_entry(key, i as i64 + 1, p, prev);
            prev = hex32("entry_hash", &e.entry_hash).unwrap();
            out.push(e);
        }
        out
    }

    fn key() -> SigningKey {
        SigningKey::from_bytes(&[7u8; 32])
    }

    #[test]
    fn a_well_formed_chain_verifies() {
        let k = key();
        let entries = chain(&k, &["{\"a\":1}", "{\"b\":2}", "{\"c\":3}"]);
        let report = verify_chain(&entries, &k.verifying_key(), None);
        assert!(report.ok, "{report:?}");
        assert_eq!(report.verified, 3);
    }

    #[test]
    fn an_empty_chain_is_honest_not_broken() {
        let report = verify_chain(&[], &key().verifying_key(), None);
        assert!(report.ok);
        assert_eq!(report.verified, 0);
        assert_eq!(report.message, "Nothing sealed yet.");
    }

    #[test]
    fn edited_payload_is_caught_as_an_entry_hash_mismatch() {
        let k = key();
        let mut entries = chain(&k, &["{\"a\":1}", "{\"b\":2}"]);
        entries[1].payload = "{\"b\":99}".to_string();
        let report = verify_chain(&entries, &k.verifying_key(), None);
        assert!(!report.ok);
        assert_eq!(report.kind, Some(FailureKind::EntryHashMismatch));
        assert_eq!(report.failed_at, Some(2));
        // The first entry still verified — "verified through" stays truthful.
        assert_eq!(report.verified, 1);
    }

    #[test]
    fn a_removed_entry_breaks_the_links() {
        let k = key();
        let mut entries = chain(&k, &["{\"a\":1}", "{\"b\":2}", "{\"c\":3}"]);
        entries.remove(1);
        let report = verify_chain(&entries, &k.verifying_key(), None);
        assert!(!report.ok);
        assert_eq!(report.kind, Some(FailureKind::PrevHashMismatch));
    }

    #[test]
    fn a_chain_signed_by_another_device_is_rejected() {
        let k = key();
        let entries = chain(&k, &["{\"a\":1}"]);
        let stranger = SigningKey::from_bytes(&[9u8; 32]);
        let report = verify_chain(&entries, &stranger.verifying_key(), None);
        assert!(!report.ok);
        assert_eq!(report.kind, Some(FailureKind::SignatureMismatch));
    }

    #[test]
    fn a_signature_from_another_domain_does_not_transfer() {
        // The whole point of the length-prefixed domain: a checkpoint signature
        // must not verify as a log entry.
        let k = key();
        let mut entries = chain(&k, &["{\"a\":1}"]);
        let entry_hash = hex32("entry_hash", &entries[0].entry_hash).unwrap();
        let wrong_domain = domain_separated_hash(DOMAIN_CHECKPOINT, &entry_hash);
        entries[0].signature = hex::encode(k.sign(&wrong_domain).to_bytes());
        let report = verify_chain(&entries, &k.verifying_key(), None);
        assert!(!report.ok);
        assert_eq!(report.kind, Some(FailureKind::SignatureMismatch));
    }

    #[test]
    fn a_checkpoint_head_anchors_a_partial_walk() {
        let k = key();
        let all = chain(&k, &["{\"a\":1}", "{\"b\":2}", "{\"c\":3}"]);
        let head = hex32("entry_hash", &all[0].entry_hash).unwrap();
        let report = verify_chain(&all[1..], &k.verifying_key(), Some(head));
        assert!(report.ok, "{report:?}");
        assert_eq!(report.verified, 2);
    }

    #[test]
    fn garbage_json_reports_instead_of_panicking() {
        let report = verify_json("not json at all");
        assert!(!report.ok);
        assert_eq!(report.kind, Some(FailureKind::Malformed));
        assert!(!report.message.is_empty());
    }

    #[test]
    fn a_document_round_trips_through_json() {
        let k = key();
        let doc = SealedLogDocument {
            verifying_key: hex::encode(k.verifying_key().to_bytes()),
            checkpoint_head: None,
            entries: chain(&k, &["{\"a\":1}", "{\"b\":2}"]),
        };
        let json = serde_json::to_string(&doc).unwrap();
        let report = verify_json(&json);
        assert!(report.ok, "{report:?}");
        assert_eq!(report.verified, 2);
    }
}
