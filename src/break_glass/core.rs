//! Core break-glass types (no CLI).

use anyhow::{anyhow, Result};
use ed25519_dalek::{SigningKey, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::crypto::signatures::{
    sign_ed25519_only, verify_ed25519_only, DOMAIN_BREAK_GLASS_TOKEN,
    DOMAIN_POLICY_CHANGE_APPROVAL, DOMAIN_TRUSTEE_APPROVAL,
};
use crate::vault::crypto::VaultCryptoMode;
use crate::TimeBucket;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub struct TrusteeId(pub String);

impl TrusteeId {
    pub fn new(id: &str) -> Self {
        Self(id.trim().to_string())
    }
}

/// Maximum number of trustees allowed in a quorum policy.
/// This prevents DoS via excessive policy size.
pub const MAX_TRUSTEES: usize = 32;

/// Maximum number of approvals that can be submitted per authorization request.
/// This prevents DoS via excessive approval processing.
pub const MAX_APPROVALS: usize = 64;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct QuorumPolicy {
    pub n: u8,
    pub m: u8,
    pub trustees: Vec<TrusteeEntry>,
    #[serde(default)]
    pub vault: VaultPolicy,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TrusteeEntry {
    pub id: TrusteeId,
    pub public_key: [u8; 32],
}

impl QuorumPolicy {
    pub fn new(threshold: u8, trustees: Vec<TrusteeEntry>) -> Result<Self> {
        let policy = Self {
            n: threshold,
            m: trustees.len() as u8,
            trustees,
            vault: VaultPolicy::default(),
        };
        policy.validate()?;
        Ok(policy)
    }

    /// Validates a QuorumPolicy, typically after deserialization.
    /// Returns Ok(()) if valid, or an error describing the validation failure.
    pub fn validate(&self) -> Result<()> {
        if self.n == 0 {
            return Err(anyhow!("quorum threshold must be > 0"));
        }
        if self.trustees.is_empty() {
            return Err(anyhow!("quorum must include at least one trustee"));
        }
        if self.trustees.len() > MAX_TRUSTEES {
            return Err(anyhow!(
                "quorum trustee count {} exceeds maximum {}",
                self.trustees.len(),
                MAX_TRUSTEES
            ));
        }
        if self.n as usize > self.trustees.len() {
            return Err(anyhow!("quorum threshold exceeds trustee count"));
        }
        let mut uniq = std::collections::HashSet::new();
        let mut uniq_keys = std::collections::HashSet::new();
        for t in &self.trustees {
            if t.id.0.is_empty() {
                return Err(anyhow!("trustee id cannot be empty"));
            }
            if !uniq.insert(t.id.0.clone()) {
                return Err(anyhow!("duplicate trustee id: {}", t.id.0));
            }
            VerifyingKey::from_bytes(&t.public_key)
                .map_err(|_| anyhow!("invalid public key for trustee {}", t.id.0))?;
            // Reject a key reused across trustee slots: without this, one
            // key-holder counts as several distinct trustees and can satisfy
            // a k-of-n quorum alone (Invariant V). Quorum independence is a
            // property of the KEYS, not just the ids.
            if !uniq_keys.insert(t.public_key) {
                return Err(anyhow!(
                    "duplicate trustee public key for trustee {}",
                    t.id.0
                ));
            }
        }
        Ok(())
    }

    /// A stable, order-independent commitment to this policy's quorum identity:
    /// the threshold `n`, the member count `m`, and the set of trustee
    /// `(id, public_key)` pairs (sorted, length-prefixed). Two policies with
    /// the same members and threshold commit identically regardless of trustee
    /// ordering; any change to the threshold or the trustee set changes it.
    /// A receipt records this so an audit can tell whether the current policy
    /// is the one the receipt was decided under.
    pub fn commitment(&self) -> [u8; 32] {
        let mut entries: Vec<(&str, &[u8; 32])> = self
            .trustees
            .iter()
            .map(|t| (t.id.0.as_str(), &t.public_key))
            .collect();
        entries.sort();
        let mut hasher = Sha256::new();
        hasher.update((self.n as u32).to_le_bytes());
        hasher.update((self.m as u32).to_le_bytes());
        hasher.update((entries.len() as u32).to_le_bytes());
        for (id, pk) in entries {
            let id_bytes = id.as_bytes();
            hasher.update((id_bytes.len() as u32).to_le_bytes());
            hasher.update(id_bytes);
            hasher.update(pk);
        }
        hasher.finalize().into()
    }

    /// A commitment to the WHOLE policy: the quorum identity (`commitment`)
    /// plus the vault crypto settings. `commitment()` deliberately covers only
    /// the quorum (it is the receipt-era identifier), so a policy change that
    /// alters ONLY `vault.crypto_mode` would be invisible to it — this variant
    /// exists so policy-change consent binds every field a mutation can move.
    pub fn full_commitment(&self) -> [u8; 32] {
        let mut hasher = Sha256::new();
        hasher.update(self.commitment());
        let mode = self.vault.crypto_mode.to_string();
        let mode_bytes = mode.as_bytes();
        hasher.update((mode_bytes.len() as u32).to_le_bytes());
        hasher.update(mode_bytes);
        hasher.finalize().into()
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct VaultPolicy {
    #[serde(default)]
    pub crypto_mode: VaultCryptoMode,
}

impl Default for VaultPolicy {
    fn default() -> Self {
        Self {
            crypto_mode: VaultCryptoMode::Classical,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct UnlockRequest {
    pub vault_envelope_id: String,
    pub ruleset_hash: [u8; 32],
    pub purpose: String,
    pub time_bucket: TimeBucket,
}

impl UnlockRequest {
    pub fn new(
        vault_envelope_id: &str,
        ruleset_hash: [u8; 32],
        purpose: &str,
        time_bucket: TimeBucket,
    ) -> Result<Self> {
        if vault_envelope_id.trim().is_empty() {
            return Err(anyhow!("vault envelope id cannot be empty"));
        }
        if purpose.trim().is_empty() {
            return Err(anyhow!("purpose cannot be empty"));
        }
        Ok(Self {
            vault_envelope_id: vault_envelope_id.trim().to_string(),
            ruleset_hash,
            purpose: purpose.trim().to_string(),
            time_bucket,
        })
    }

    pub fn request_hash(&self) -> [u8; 32] {
        // Length-prefix the variable-length fields so no two distinct
        // (envelope_id, purpose) pairs can collide by shifting the boundary
        // between them (mirrors `token_signing_hash`'s framing). The fixed
        // 32-byte ruleset hash and the two u64 bucket fields are
        // self-delimiting and need no prefix.
        let mut hasher = Sha256::new();
        let envelope_bytes = self.vault_envelope_id.as_bytes();
        hasher.update((envelope_bytes.len() as u32).to_le_bytes());
        hasher.update(envelope_bytes);
        hasher.update(self.ruleset_hash);
        let purpose_bytes = self.purpose.as_bytes();
        hasher.update((purpose_bytes.len() as u32).to_le_bytes());
        hasher.update(purpose_bytes);
        hasher.update(self.time_bucket.start_epoch_s.to_le_bytes());
        hasher.update(self.time_bucket.size_s.to_le_bytes());
        hasher.finalize().into()
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Approval {
    pub trustee: TrusteeId,
    pub request_hash: [u8; 32],
    pub signature: Vec<u8>,
}

impl Approval {
    pub fn new(trustee: TrusteeId, request_hash: [u8; 32], signature: Vec<u8>) -> Self {
        Self {
            trustee,
            request_hash,
            signature,
        }
    }

    /// Produce a domain-separated trustee approval: the signature is
    /// Ed25519 over `domain_separated_hash(DOMAIN_TRUSTEE_APPROVAL,
    /// request_hash)`, so it is bound to the trustee-consent context and
    /// cannot be replayed from (or into) any other signature domain.
    pub fn signed(trustee: TrusteeId, request_hash: [u8; 32], signing_key: &SigningKey) -> Self {
        let signature = sign_approval(signing_key, &request_hash).to_vec();
        Self::new(trustee, request_hash, signature)
    }
}

/// Sign a trustee approval over the request hash, domain-separated to
/// `DOMAIN_TRUSTEE_APPROVAL`. Mirrors the break-glass token signer's
/// use of `sign_ed25519_only`.
pub fn sign_approval(signing_key: &SigningKey, request_hash: &[u8; 32]) -> [u8; 64] {
    sign_ed25519_only(DOMAIN_TRUSTEE_APPROVAL, signing_key, request_hash)
}

/// Verify a trustee approval signature against a trustee public key.
/// Returns false on a malformed key, a wrong-length signature, or a
/// signature that verifies under any domain other than
/// `DOMAIN_TRUSTEE_APPROVAL` (cross-context signatures are rejected).
pub fn verify_approval(public_key: &[u8; 32], request_hash: &[u8; 32], signature: &[u8]) -> bool {
    let Ok(verifying_key) = VerifyingKey::from_bytes(public_key) else {
        return false;
    };
    let Ok(sig_array) = <[u8; 64]>::try_from(signature) else {
        return false;
    };
    verify_ed25519_only(
        DOMAIN_TRUSTEE_APPROVAL,
        &verifying_key,
        request_hash,
        &sig_array,
    )
    .is_ok()
}

/// Re-derive the quorum: count the DISTINCT trustee public keys that carry a
/// valid, domain-separated approval over `request_hash` under `policy`. This
/// never trusts a recorded `BreakGlassOutcome` — it is the ground-truth
/// recomputation used at receipt audit and at the unseal gate so that a
/// device-key holder cannot forge `Granted` with too few (or zero) genuine
/// trustee approvals. Deduping on the public KEY (not the id) means a reused
/// key can never inflate the count even if a malformed policy slipped past
/// `QuorumPolicy::validate`.
pub fn count_valid_distinct_approvals(
    policy: &QuorumPolicy,
    request_hash: &[u8; 32],
    approvals: &[Approval],
) -> usize {
    let mut distinct = std::collections::HashSet::new();
    for approval in approvals {
        if &approval.request_hash != request_hash {
            continue;
        }
        let Some(trustee) = policy
            .trustees
            .iter()
            .find(|t| t.id.0 == approval.trustee.0)
        else {
            continue;
        };
        if !verify_approval(&trustee.public_key, request_hash, &approval.signature) {
            continue;
        }
        distinct.insert(trustee.public_key);
    }
    distinct.len()
}

/// A proposed change to the stored quorum policy. Consent to it is a distinct
/// power from consent to an unlock request, carried in its own signature
/// domain (`DOMAIN_POLICY_CHANGE_APPROVAL`) so neither can stand in for the
/// other. The hash binds the FULL previous and proposed policies (via
/// `QuorumPolicy::full_commitment`, which covers roster, threshold, and vault
/// crypto settings) and a freshness bucket — any change to any of them
/// invalidates every collected approval.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PolicyChangeProposal {
    /// `full_commitment()` of the policy being replaced; all-zero when
    /// bootstrapping an empty database (there is no quorum yet to consult).
    pub prev_policy_commitment: [u8; 32],
    pub new_policy: QuorumPolicy,
    pub time_bucket: TimeBucket,
}

impl PolicyChangeProposal {
    pub fn change_hash(&self) -> [u8; 32] {
        // Every field is fixed-size (two 32-byte commitments, two u64 bucket
        // fields), so the concatenation is self-delimiting and needs no
        // length prefixes (mirrors `request_hash`'s reasoning).
        let mut hasher = Sha256::new();
        hasher.update(self.prev_policy_commitment);
        hasher.update(self.new_policy.full_commitment());
        hasher.update(self.time_bucket.start_epoch_s.to_le_bytes());
        hasher.update(self.time_bucket.size_s.to_le_bytes());
        hasher.finalize().into()
    }
}

/// Sign a trustee's consent to a policy change, domain-separated to
/// `DOMAIN_POLICY_CHANGE_APPROVAL`.
pub fn sign_policy_change_approval(signing_key: &SigningKey, change_hash: &[u8; 32]) -> [u8; 64] {
    sign_ed25519_only(DOMAIN_POLICY_CHANGE_APPROVAL, signing_key, change_hash)
}

/// Verify a policy-change approval signature. Rejects malformed keys,
/// wrong-length signatures, and — critically — signatures minted under any
/// other domain (an unlock-request approval never counts as roster consent).
pub fn verify_policy_change_approval(
    public_key: &[u8; 32],
    change_hash: &[u8; 32],
    signature: &[u8],
) -> bool {
    let Ok(verifying_key) = VerifyingKey::from_bytes(public_key) else {
        return false;
    };
    let Ok(sig_array) = <[u8; 64]>::try_from(signature) else {
        return false;
    };
    verify_ed25519_only(
        DOMAIN_POLICY_CHANGE_APPROVAL,
        &verifying_key,
        change_hash,
        &sig_array,
    )
    .is_ok()
}

/// Count the DISTINCT current-trustee public keys carrying a valid,
/// domain-separated policy-change approval over `change_hash`. The policy
/// consulted is the CURRENT (stored) policy — the quorum being replaced is
/// the one that must consent — and, as with unlock quorums, deduping is on
/// the KEY so a reused key can never inflate the count.
pub fn count_valid_distinct_policy_change_approvals(
    current_policy: &QuorumPolicy,
    change_hash: &[u8; 32],
    approvals: &[Approval],
) -> usize {
    let mut distinct = std::collections::HashSet::new();
    for approval in approvals {
        if &approval.request_hash != change_hash {
            continue;
        }
        let Some(trustee) = current_policy
            .trustees
            .iter()
            .find(|t| t.id.0 == approval.trustee.0)
        else {
            continue;
        };
        if !verify_policy_change_approval(&trustee.public_key, change_hash, &approval.signature) {
            continue;
        }
        distinct.insert(trustee.public_key);
    }
    distinct.len()
}

fn canonical_approval_bytes(approval: &Approval) -> Vec<u8> {
    let mut out =
        Vec::with_capacity(4 + approval.trustee.0.len() + 32 + 4 + approval.signature.len());
    let trustee_bytes = approval.trustee.0.as_bytes();
    out.extend_from_slice(&(trustee_bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(trustee_bytes);
    out.extend_from_slice(&approval.request_hash);
    out.extend_from_slice(&(approval.signature.len() as u32).to_le_bytes());
    out.extend_from_slice(&approval.signature);
    out
}

pub fn approvals_commitment(approvals: &[Approval]) -> [u8; 32] {
    let mut entries: Vec<Vec<u8>> = approvals.iter().map(canonical_approval_bytes).collect();
    entries.sort();
    let mut hasher = Sha256::new();
    for entry in entries {
        hasher.update((entry.len() as u32).to_le_bytes());
        hasher.update(entry);
    }
    hasher.finalize().into()
}

fn empty_approvals_commitment() -> [u8; 32] {
    approvals_commitment(&[])
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum BreakGlassOutcome {
    Granted,
    Denied { reason: String },
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BreakGlassReceipt {
    pub vault_envelope_id: String,
    pub request_hash: [u8; 32],
    pub ruleset_hash: [u8; 32],
    pub time_bucket: TimeBucket,
    pub trustees_used: Vec<TrusteeId>,
    #[serde(default = "empty_approvals_commitment")]
    pub approvals_commitment: [u8; 32],
    /// Commitment to the quorum policy that was in force when this receipt was
    /// authorized (`QuorumPolicy::commitment`). Lets an audit re-derive a
    /// historical `Granted` receipt against ITS policy era instead of the
    /// mutable current policy row: a later, legitimate policy rotation (raised
    /// threshold, changed trustee set) must not turn an already-valid receipt
    /// into a false tamper alarm. Defaults to all-zero for receipts written
    /// before this field existed (treated as "current-policy era").
    #[serde(default = "zero_policy_commitment")]
    pub policy_commitment: [u8; 32],
    pub outcome: BreakGlassOutcome,
}

fn zero_policy_commitment() -> [u8; 32] {
    [0u8; 32]
}

#[derive(Clone, Debug)]
pub struct BreakGlassToken {
    token_nonce: [u8; 32],
    expires_bucket: TimeBucket,
    vault_envelope_id: String,
    ruleset_hash: [u8; 32],
    receipt_entry_hash: [u8; 32],
    device_signature: [u8; 64],
    consumed: bool,
}

impl BreakGlassToken {
    #[deprecated(note = "use BreakGlass::authorize with quorum policy and approvals")]
    pub fn authorize_mvp(_purpose: &str) -> Result<Self> {
        Err(anyhow!(
            "break-glass requires quorum approvals; use BreakGlass::authorize"
        ))
    }

    pub fn token_nonce(&self) -> [u8; 32] {
        self.token_nonce
    }

    pub fn expires_bucket(&self) -> TimeBucket {
        self.expires_bucket
    }

    pub fn vault_envelope_id(&self) -> &str {
        &self.vault_envelope_id
    }

    pub fn ruleset_hash(&self) -> [u8; 32] {
        self.ruleset_hash
    }

    pub fn receipt_entry_hash(&self) -> [u8; 32] {
        self.receipt_entry_hash
    }

    pub fn device_signature(&self) -> [u8; 64] {
        self.device_signature
    }

    pub fn attach_receipt_signature(
        &mut self,
        receipt_entry_hash: [u8; 32],
        signing_key: &SigningKey,
    ) -> Result<()> {
        if receipt_entry_hash == [0u8; 32] {
            return Err(anyhow!("break-glass receipt hash cannot be zero"));
        }
        self.receipt_entry_hash = receipt_entry_hash;
        let signing_hash = token_signing_hash(
            self.token_nonce,
            &self.vault_envelope_id,
            self.ruleset_hash,
            self.expires_bucket,
            self.receipt_entry_hash,
        );
        self.device_signature =
            sign_ed25519_only(DOMAIN_BREAK_GLASS_TOKEN, signing_key, &signing_hash);
        Ok(())
    }

    pub fn verify_signature(&self, verifying_key: &VerifyingKey) -> Result<()> {
        if self.receipt_entry_hash == [0u8; 32] {
            return Err(anyhow!("break-glass receipt hash missing"));
        }
        if self.device_signature == [0u8; 64] {
            return Err(anyhow!("break-glass token signature missing"));
        }
        let signing_hash = token_signing_hash(
            self.token_nonce,
            &self.vault_envelope_id,
            self.ruleset_hash,
            self.expires_bucket,
            self.receipt_entry_hash,
        );
        verify_ed25519_only(
            DOMAIN_BREAK_GLASS_TOKEN,
            verifying_key,
            &signing_hash,
            &self.device_signature,
        )
    }

    #[cfg(test)]
    pub fn test_token() -> Self {
        Self {
            token_nonce: [0u8; 32],
            expires_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            vault_envelope_id: "test-envelope".to_string(),
            ruleset_hash: [0u8; 32],
            receipt_entry_hash: [0u8; 32],
            device_signature: [0u8; 64],
            consumed: false,
        }
    }

    #[cfg(test)]
    pub fn test_token_with(
        token_nonce: [u8; 32],
        expires_bucket: TimeBucket,
        vault_envelope_id: impl Into<String>,
        ruleset_hash: [u8; 32],
    ) -> Self {
        Self {
            token_nonce,
            expires_bucket,
            vault_envelope_id: vault_envelope_id.into(),
            ruleset_hash,
            receipt_entry_hash: [0u8; 32],
            device_signature: [0u8; 64],
            consumed: false,
        }
    }

    pub fn consume(&mut self) -> Result<()> {
        if self.consumed {
            return Err(anyhow!("break-glass token already consumed"));
        }
        self.consumed = true;
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BreakGlassTokenFile {
    pub token_nonce: String,
    pub expires_bucket: TimeBucket,
    pub vault_envelope_id: String,
    pub ruleset_hash: String,
    pub receipt_entry_hash: String,
    pub device_signature: String,
}

impl BreakGlassTokenFile {
    pub fn from_token(token: &BreakGlassToken) -> Result<Self> {
        if token.receipt_entry_hash() == [0u8; 32] {
            return Err(anyhow!("break-glass token missing receipt hash"));
        }
        if token.device_signature() == [0u8; 64] {
            return Err(anyhow!("break-glass token missing device signature"));
        }
        Ok(Self {
            token_nonce: hex::encode(token.token_nonce()),
            expires_bucket: token.expires_bucket(),
            vault_envelope_id: token.vault_envelope_id().to_string(),
            ruleset_hash: hex::encode(token.ruleset_hash()),
            receipt_entry_hash: hex::encode(token.receipt_entry_hash()),
            device_signature: hex::encode(token.device_signature()),
        })
    }

    pub fn into_token(self) -> Result<BreakGlassToken> {
        if self.vault_envelope_id.trim().is_empty() {
            return Err(anyhow!("token file missing vault envelope id"));
        }
        let token_nonce = parse_hex32(&self.token_nonce, "token_nonce")?;
        if token_nonce == [0u8; 32] {
            return Err(anyhow!("token nonce cannot be zero"));
        }
        let ruleset_hash = parse_hex32(&self.ruleset_hash, "ruleset_hash")?;
        let receipt_entry_hash = parse_hex32(&self.receipt_entry_hash, "receipt_entry_hash")?;
        if receipt_entry_hash == [0u8; 32] {
            return Err(anyhow!("receipt_entry_hash cannot be zero"));
        }
        let device_signature = parse_hex64(&self.device_signature, "device_signature")?;
        if device_signature == [0u8; 64] {
            return Err(anyhow!("device_signature cannot be zero"));
        }
        Ok(BreakGlassToken {
            token_nonce,
            expires_bucket: self.expires_bucket,
            vault_envelope_id: self.vault_envelope_id,
            ruleset_hash,
            receipt_entry_hash,
            device_signature,
            consumed: false,
        })
    }
}

fn parse_hex32(value: &str, label: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(value.trim()).map_err(|e| anyhow!("invalid {}: {}", label, e))?;
    if bytes.len() != 32 {
        return Err(anyhow!(
            "invalid {} length: expected 32 bytes, got {}",
            label,
            bytes.len()
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn parse_hex64(value: &str, label: &str) -> Result<[u8; 64]> {
    let bytes = hex::decode(value.trim()).map_err(|e| anyhow!("invalid {}: {}", label, e))?;
    if bytes.len() != 64 {
        return Err(anyhow!(
            "invalid {} length: expected 64 bytes, got {}",
            label,
            bytes.len()
        ));
    }
    let mut out = [0u8; 64];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn token_signing_hash(
    token_nonce: [u8; 32],
    vault_envelope_id: &str,
    ruleset_hash: [u8; 32],
    expires_bucket: TimeBucket,
    receipt_entry_hash: [u8; 32],
) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(token_nonce);
    hasher.update(expires_bucket.start_epoch_s.to_le_bytes());
    hasher.update(expires_bucket.size_s.to_le_bytes());
    let envelope_bytes = vault_envelope_id.as_bytes();
    hasher.update((envelope_bytes.len() as u32).to_le_bytes());
    hasher.update(envelope_bytes);
    hasher.update(ruleset_hash);
    hasher.update(receipt_entry_hash);
    hasher.finalize().into()
}

pub struct BreakGlass;

impl BreakGlass {
    pub fn authorize(
        policy: &QuorumPolicy,
        request: &UnlockRequest,
        approvals: &[Approval],
        now_bucket: TimeBucket,
    ) -> (Result<BreakGlassToken>, BreakGlassReceipt) {
        // Validate approval count to prevent DoS via excessive approval processing
        if approvals.len() > MAX_APPROVALS {
            let receipt = BreakGlassReceipt {
                vault_envelope_id: request.vault_envelope_id.clone(),
                request_hash: request.request_hash(),
                ruleset_hash: request.ruleset_hash,
                time_bucket: now_bucket,
                trustees_used: vec![],
                approvals_commitment: approvals_commitment(approvals),
                policy_commitment: policy.commitment(),
                outcome: BreakGlassOutcome::Denied {
                    reason: format!(
                        "approval count {} exceeds maximum {}",
                        approvals.len(),
                        MAX_APPROVALS
                    ),
                },
            };
            return (
                Err(anyhow!(
                    "approval count {} exceeds maximum {}",
                    approvals.len(),
                    MAX_APPROVALS
                )),
                receipt,
            );
        }

        let mut trustees_used = Vec::new();
        let mut unknown_trustees = std::collections::BTreeSet::new();
        let request_hash = request.request_hash();
        let mut approved = std::collections::HashSet::new();

        for approval in approvals {
            if approval.request_hash != request_hash {
                continue;
            }
            let Some(trustee) = policy
                .trustees
                .iter()
                .find(|t| t.id.0 == approval.trustee.0)
            else {
                unknown_trustees.insert(approval.trustee.0.clone());
                continue;
            };
            // Domain-separated: rejects a signature that only verifies over
            // the bare request hash or under any other context's domain.
            if !verify_approval(&trustee.public_key, &request_hash, &approval.signature) {
                continue;
            }
            if approved.insert(approval.trustee.0.clone()) {
                trustees_used.push(approval.trustee.clone());
            }
        }

        let outcome = if !unknown_trustees.is_empty() {
            BreakGlassOutcome::Denied {
                reason: format!(
                    "unrecognized trustee approvals: {}",
                    unknown_trustees
                        .iter()
                        .cloned()
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
            }
        } else if trustees_used.len() >= policy.n as usize {
            BreakGlassOutcome::Granted
        } else {
            BreakGlassOutcome::Denied {
                reason: format!(
                    "insufficient approvals: {}/{}",
                    trustees_used.len(),
                    policy.n
                ),
            }
        };

        let approvals_commitment = approvals_commitment(approvals);
        let receipt = BreakGlassReceipt {
            vault_envelope_id: request.vault_envelope_id.clone(),
            request_hash,
            ruleset_hash: request.ruleset_hash,
            time_bucket: now_bucket,
            trustees_used: trustees_used.clone(),
            approvals_commitment,
            policy_commitment: policy.commitment(),
            outcome: outcome.clone(),
        };

        match outcome {
            BreakGlassOutcome::Granted => {
                let mut nonce = [0u8; 32];
                rand::fill(&mut nonce[..]);
                let token = BreakGlassToken {
                    token_nonce: nonce,
                    expires_bucket: now_bucket,
                    vault_envelope_id: request.vault_envelope_id.clone(),
                    ruleset_hash: request.ruleset_hash,
                    receipt_entry_hash: [0u8; 32],
                    device_signature: [0u8; 64],
                    consumed: false,
                };
                (Ok(token), receipt)
            }
            BreakGlassOutcome::Denied { reason } => (Err(anyhow!(reason)), receipt),
        }
    }

    pub fn assert_token_valid(
        token: &BreakGlassToken,
        envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        now_bucket: TimeBucket,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<()> {
        if token.token_nonce() == [0u8; 32] {
            return Err(anyhow!("break-glass token nonce invalid"));
        }
        if token.consumed {
            return Err(anyhow!("break-glass token already consumed"));
        }
        if token.vault_envelope_id() != envelope_id {
            return Err(anyhow!("break-glass token envelope mismatch"));
        }
        if token.ruleset_hash() != expected_ruleset_hash {
            return Err(anyhow!("break-glass token ruleset mismatch"));
        }
        let expires_bucket = token.expires_bucket();
        if expires_bucket.start_epoch_s != now_bucket.start_epoch_s
            || expires_bucket.size_s != now_bucket.size_s
        {
            return Err(anyhow!("break-glass token expired"));
        }
        token.verify_signature(verifying_key)?;
        match receipt_lookup(&token.receipt_entry_hash())? {
            BreakGlassOutcome::Granted => Ok(()),
            BreakGlassOutcome::Denied { reason } => {
                Err(anyhow!("break-glass receipt not granted: {}", reason))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::SigningKey;

    #[test]
    fn core_types_round_trip() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:1", [1u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[1u8; 32]);
        let signature = sign_approval(&signing_key, &request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        assert!(result.is_ok());

        let json = serde_json::to_string(&receipt).unwrap();
        let round_trip: BreakGlassReceipt = serde_json::from_str(&json).unwrap();
        assert_eq!(round_trip.vault_envelope_id, "vault:1");
    }

    #[test]
    fn receipt_serialization_minimal() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let receipt = BreakGlassReceipt {
            vault_envelope_id: "vault:min".to_string(),
            request_hash: [0u8; 32],
            ruleset_hash: [1u8; 32],
            time_bucket: bucket,
            trustees_used: vec![],
            approvals_commitment: approvals_commitment(&[]),
            policy_commitment: [0u8; 32],
            outcome: BreakGlassOutcome::Denied {
                reason: "test".to_string(),
            },
        };

        let json = serde_json::to_string(&receipt).unwrap();
        let round_trip: BreakGlassReceipt = serde_json::from_str(&json).unwrap();
        assert_eq!(round_trip.vault_envelope_id, "vault:min");
    }

    #[test]
    fn invalid_signature_does_not_count_toward_quorum() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:2", [2u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[3u8; 32]);
        let other_key = SigningKey::from_bytes(&[4u8; 32]);
        let signature = sign_approval(&other_key, &request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        assert!(result.is_err());
        assert!(matches!(receipt.outcome, BreakGlassOutcome::Denied { .. }));
        assert!(receipt.trustees_used.is_empty());
    }

    #[test]
    fn valid_signature_counts_toward_quorum() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:3", [3u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[5u8; 32]);
        let signature = sign_approval(&signing_key, &request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        assert!(result.is_ok());
        assert_eq!(receipt.trustees_used.len(), 1);
    }

    #[test]
    fn forged_token_file_rejected_by_signature_check() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:4", [4u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[6u8; 32]);
        let signature = sign_approval(&signing_key, &request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        let mut token = result.expect("token");

        let device_signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let receipt_hash = [8u8; 32];
        token
            .attach_receipt_signature(receipt_hash, &device_signing_key)
            .unwrap();

        let mut token_file = BreakGlassTokenFile::from_token(&token).unwrap();
        token_file.device_signature = hex::encode([1u8; 64]);
        let forged = token_file.into_token().unwrap();

        let verifying_key = device_signing_key.verifying_key();
        let result = BreakGlass::assert_token_valid(
            &forged,
            "vault:4",
            [4u8; 32],
            bucket,
            &verifying_key,
            |_| Ok(BreakGlassOutcome::Granted),
        );
        assert!(result.is_err());
    }

    // ==================== Security Edge Case Tests ====================

    #[test]
    fn quorum_policy_rejects_excessive_trustees() {
        let trustees: Vec<TrusteeEntry> = (0..MAX_TRUSTEES + 1)
            .map(|i| {
                let seed = [i as u8; 32];
                let signing_key = SigningKey::from_bytes(&seed);
                TrusteeEntry {
                    id: TrusteeId::new(&format!("trustee-{}", i)),
                    public_key: signing_key.verifying_key().to_bytes(),
                }
            })
            .collect();

        let result = QuorumPolicy::new(1, trustees);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds maximum"));
    }

    #[test]
    fn quorum_policy_accepts_max_trustees() {
        let trustees: Vec<TrusteeEntry> = (0..MAX_TRUSTEES)
            .map(|i| {
                let seed = [i as u8; 32];
                let signing_key = SigningKey::from_bytes(&seed);
                TrusteeEntry {
                    id: TrusteeId::new(&format!("trustee-{}", i)),
                    public_key: signing_key.verifying_key().to_bytes(),
                }
            })
            .collect();

        let result = QuorumPolicy::new(1, trustees);
        assert!(result.is_ok());
    }

    #[test]
    fn authorize_rejects_excessive_approvals() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:excess", [99u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[1u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();

        // Create more approvals than the maximum allowed
        let fake_approvals: Vec<Approval> = (0..MAX_APPROVALS + 1)
            .map(|_| Approval::new(TrusteeId::new("fake"), [0u8; 32], vec![0u8; 64]))
            .collect();

        let (result, receipt) = BreakGlass::authorize(&policy, &request, &fake_approvals, bucket);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds maximum"));
        assert!(matches!(receipt.outcome, BreakGlassOutcome::Denied { .. }));
    }

    #[test]
    fn duplicate_trustee_approvals_only_count_once() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:dup", [5u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let signature = sign_approval(&signing_key, &request.request_hash());

        // Same trustee signs twice
        let approval1 = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let approval2 = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );

        // Require 2 trustees but only 1 trustee in policy
        let policy = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: signing_key.verifying_key().to_bytes(),
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: SigningKey::from_bytes(&[8u8; 32])
                        .verifying_key()
                        .to_bytes(),
                },
            ],
        )
        .unwrap();

        let (result, receipt) =
            BreakGlass::authorize(&policy, &request, &[approval1, approval2], bucket);
        // Should fail because alice can only count once
        assert!(result.is_err());
        assert!(matches!(receipt.outcome, BreakGlassOutcome::Denied { .. }));
        // Alice should only appear once in trustees_used
        assert_eq!(
            receipt
                .trustees_used
                .iter()
                .filter(|t| t.0 == "alice")
                .count(),
            1
        );
    }

    #[test]
    fn token_consumed_flag_prevents_reuse() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:consume", [10u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[11u8; 32]);
        let signature = sign_approval(&signing_key, &request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        let mut token = result.expect("token");

        let device_signing_key = SigningKey::from_bytes(&[12u8; 32]);
        let receipt_hash = [13u8; 32];
        token
            .attach_receipt_signature(receipt_hash, &device_signing_key)
            .expect("attach receipt signature");

        // First consume should succeed
        assert!(token.consume().is_ok());
        // Second consume should fail (token already consumed)
        assert!(token.consume().is_err());
    }

    // ─── domain separation for trustee approvals ─────────────────────────

    fn single_trustee_policy(id: &str, key: &SigningKey) -> (QuorumPolicy, UnlockRequest) {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:dom", [7u8; 32], "incident", bucket).unwrap();
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new(id),
                public_key: key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        (policy, request)
    }

    #[test]
    fn domain_separated_approval_verifies() {
        let key = SigningKey::from_bytes(&[1u8; 32]);
        let (policy, request) = single_trustee_policy("alice", &key);
        let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &key);
        assert!(verify_approval(
            &key.verifying_key().to_bytes(),
            &request.request_hash(),
            &approval.signature
        ));
        let bucket = request.time_bucket;
        let (result, _r) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        assert!(
            result.is_ok(),
            "domain-separated approval must reach quorum"
        );
    }

    #[test]
    fn bare_hash_signature_rejected_as_approval() {
        // A signature over the BARE request hash (the pre-fix construction,
        // and the shape a legacy sealed-log/verify signature would have) must
        // NOT count as a trustee approval now that approvals are domained.
        use ed25519_dalek::Signer;
        let key = SigningKey::from_bytes(&[2u8; 32]);
        let (policy, request) = single_trustee_policy("bob", &key);
        let bare = key.sign(&request.request_hash()).to_bytes().to_vec();
        assert!(!verify_approval(
            &key.verifying_key().to_bytes(),
            &request.request_hash(),
            &bare
        ));
        let approval = Approval::new(TrusteeId::new("bob"), request.request_hash(), bare);
        let bucket = request.time_bucket;
        let (result, _r) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        assert!(
            result.is_err(),
            "a bare-hash signature must not satisfy quorum"
        );
    }

    #[test]
    fn cross_domain_signature_rejected_as_approval() {
        // A signature minted for a DIFFERENT context (the break-glass token
        // domain) over the same request hash must not verify as a trustee
        // approval, and an approval signature must not verify under the token
        // domain — the two domains are disjoint.
        let key = SigningKey::from_bytes(&[3u8; 32]);
        let request_hash = [9u8; 32];
        let token_domain_sig =
            sign_ed25519_only(DOMAIN_BREAK_GLASS_TOKEN, &key, &request_hash).to_vec();
        assert!(!verify_approval(
            &key.verifying_key().to_bytes(),
            &request_hash,
            &token_domain_sig
        ));
        let approval_sig = sign_approval(&key, &request_hash);
        assert!(verify_ed25519_only(
            DOMAIN_TRUSTEE_APPROVAL,
            &key.verifying_key(),
            &request_hash,
            &approval_sig
        )
        .is_ok());
        assert!(verify_ed25519_only(
            DOMAIN_BREAK_GLASS_TOKEN,
            &key.verifying_key(),
            &request_hash,
            &approval_sig
        )
        .is_err());
    }

    // ─── M1: quorum independence is a property of the KEYS ───────────────

    #[test]
    fn validate_rejects_duplicate_public_key() {
        // Two DISTINCT trustee ids sharing ONE key: without this rejection a
        // single key-holder fills two quorum slots and satisfies a 2-of-2
        // quorum alone (Invariant V).
        let shared = SigningKey::from_bytes(&[42u8; 32])
            .verifying_key()
            .to_bytes();
        let result = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: shared,
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: shared,
                },
            ],
        );
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("duplicate trustee public key"));
    }

    // ─── L1: request_hash disambiguates variable-length field boundaries ──

    #[test]
    fn request_hash_length_prefix_disambiguates_boundary() {
        // ("ab", "c") and ("a", "bc") differ only in where the envelope id
        // ends and the purpose begins. Without length prefixes these hash the
        // same concatenation; with them the hashes must differ.
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let a = UnlockRequest::new("ab", [0u8; 32], "c", bucket)
            .unwrap()
            .request_hash();
        let b = UnlockRequest::new("a", [0u8; 32], "bc", bucket)
            .unwrap()
            .request_hash();
        assert_ne!(a, b, "boundary shift must change the request hash");
    }

    // ─── policy-change consent is its own domain and binds every field ───

    fn two_trustee_policy(alice: &SigningKey, bob: &SigningKey, n: u8) -> QuorumPolicy {
        QuorumPolicy::new(
            n,
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
        .unwrap()
    }

    #[test]
    fn full_commitment_binds_vault_crypto_mode() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let classical = two_trustee_policy(&alice, &bob, 2);
        let mut pq = classical.clone();
        pq.vault.crypto_mode = crate::vault::crypto::VaultCryptoMode::Pq;
        // The quorum-era commitment ignores vault settings by design…
        assert_eq!(classical.commitment(), pq.commitment());
        // …so policy-change consent must use the full commitment, which
        // must not.
        assert_ne!(classical.full_commitment(), pq.full_commitment());
    }

    #[test]
    fn policy_change_hash_binds_prev_new_and_bucket() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let carol = SigningKey::from_bytes(&[3u8; 32]);
        let bucket = TimeBucket {
            start_epoch_s: 600,
            size_s: 600,
        };
        let old = two_trustee_policy(&alice, &bob, 2);
        let new = two_trustee_policy(&alice, &carol, 2);
        let base = PolicyChangeProposal {
            prev_policy_commitment: old.full_commitment(),
            new_policy: new.clone(),
            time_bucket: bucket,
        };
        let h = base.change_hash();

        let mut other_prev = base.clone();
        other_prev.prev_policy_commitment = [9u8; 32];
        assert_ne!(h, other_prev.change_hash(), "prev commitment must bind");

        let mut other_new = base.clone();
        other_new.new_policy = two_trustee_policy(&alice, &bob, 1);
        assert_ne!(h, other_new.change_hash(), "new policy must bind");

        let mut other_bucket = base;
        other_bucket.time_bucket = TimeBucket {
            start_epoch_s: 1200,
            size_s: 600,
        };
        assert_ne!(h, other_bucket.change_hash(), "time bucket must bind");
    }

    #[test]
    fn policy_change_approval_domain_is_disjoint_from_unlock_approval() {
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let hash = [5u8; 32];
        let pk = key.verifying_key().to_bytes();

        // An unlock-request approval signature must not count as consent to a
        // policy change…
        let unlock_sig = sign_approval(&key, &hash);
        assert!(!verify_policy_change_approval(&pk, &hash, &unlock_sig));
        // …and policy-change consent must not count as an unlock approval.
        let change_sig = sign_policy_change_approval(&key, &hash);
        assert!(!verify_approval(&pk, &hash, &change_sig));
        // Each verifies only in its own domain.
        assert!(verify_policy_change_approval(&pk, &hash, &change_sig));
        assert!(verify_approval(&pk, &hash, &unlock_sig));
    }

    #[test]
    fn policy_change_count_uses_current_quorum_and_dedupes_by_key() {
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let mallory = SigningKey::from_bytes(&[6u8; 32]);
        let current = two_trustee_policy(&alice, &bob, 2);
        let proposed = two_trustee_policy(&alice, &mallory, 1);
        let proposal = PolicyChangeProposal {
            prev_policy_commitment: current.full_commitment(),
            new_policy: proposed,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
        };
        let h = proposal.change_hash();

        let a = Approval::new(
            TrusteeId::new("alice"),
            h,
            sign_policy_change_approval(&alice, &h).to_vec(),
        );
        let a_dup = a.clone();
        let b = Approval::new(
            TrusteeId::new("bob"),
            h,
            sign_policy_change_approval(&bob, &h).to_vec(),
        );
        // Mallory is in the PROPOSED roster, not the current one — her
        // consent must not count toward replacing the current quorum.
        let m = Approval::new(
            TrusteeId::new("mallory"),
            h,
            sign_policy_change_approval(&mallory, &h).to_vec(),
        );

        assert_eq!(
            count_valid_distinct_policy_change_approvals(&current, &h, &[a.clone(), a_dup]),
            1,
            "duplicate signer counts once"
        );
        assert_eq!(
            count_valid_distinct_policy_change_approvals(&current, &h, &[a.clone(), m]),
            1,
            "a proposed-roster key is not a current trustee"
        );
        assert_eq!(
            count_valid_distinct_policy_change_approvals(&current, &h, &[a, b]),
            2
        );
    }

    // ─── H1: quorum re-derivation counts distinct valid trustees only ────

    #[test]
    fn count_valid_distinct_approvals_ground_truth() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:count", [1u8; 32], "incident", bucket).unwrap();
        let rh = request.request_hash();
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let policy = QuorumPolicy::new(
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

        // Empty set → zero (the forged-Granted case).
        assert_eq!(count_valid_distinct_approvals(&policy, &rh, &[]), 0);

        // One valid + one bogus signature → one distinct valid trustee.
        let good = Approval::signed(TrusteeId::new("alice"), rh, &alice);
        let bogus = Approval::new(TrusteeId::new("bob"), rh, vec![0u8; 64]);
        assert_eq!(
            count_valid_distinct_approvals(&policy, &rh, &[good.clone(), bogus]),
            1
        );

        // Alice signing twice still counts once (dedup by key).
        let good2 = Approval::signed(TrusteeId::new("alice"), rh, &alice);
        assert_eq!(
            count_valid_distinct_approvals(&policy, &rh, &[good.clone(), good2]),
            1
        );

        // Two distinct valid trustees → two.
        let bob_ok = Approval::signed(TrusteeId::new("bob"), rh, &bob);
        assert_eq!(
            count_valid_distinct_approvals(&policy, &rh, &[good, bob_ok]),
            2
        );
    }
}
