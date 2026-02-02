//! Core break-glass types (no CLI).

use anyhow::{anyhow, Result};
use ed25519_dalek::{Signature, SigningKey, Verifier, VerifyingKey};
use rand::RngCore;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::crypto::signatures::{sign_ed25519_only, verify_ed25519_only, DOMAIN_BREAK_GLASS_TOKEN};
use crate::TimeBucket;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub struct TrusteeId(pub String);

impl TrusteeId {
    pub fn new(id: &str) -> Self {
        Self(id.trim().to_string())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct QuorumPolicy {
    pub n: u8,
    pub m: u8,
    pub trustees: Vec<TrusteeEntry>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TrusteeEntry {
    pub id: TrusteeId,
    pub public_key: [u8; 32],
}

impl QuorumPolicy {
    pub fn new(threshold: u8, trustees: Vec<TrusteeEntry>) -> Result<Self> {
        if threshold == 0 {
            return Err(anyhow!("quorum threshold must be > 0"));
        }
        let m = trustees.len();
        if m == 0 {
            return Err(anyhow!("quorum must include at least one trustee"));
        }
        if threshold as usize > m {
            return Err(anyhow!("quorum threshold exceeds trustee count"));
        }
        let mut uniq = std::collections::HashSet::new();
        for t in &trustees {
            if t.id.0.is_empty() {
                return Err(anyhow!("trustee id cannot be empty"));
            }
            if !uniq.insert(t.id.0.clone()) {
                return Err(anyhow!("duplicate trustee id: {}", t.id.0));
            }
            VerifyingKey::from_bytes(&t.public_key)
                .map_err(|_| anyhow!("invalid public key for trustee {}", t.id.0))?;
        }
        Ok(Self {
            n: threshold,
            m: m as u8,
            trustees,
        })
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
        let mut hasher = Sha256::new();
        hasher.update(self.vault_envelope_id.as_bytes());
        hasher.update(self.ruleset_hash);
        hasher.update(self.purpose.as_bytes());
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
    pub outcome: BreakGlassOutcome,
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
            let Ok(public_key) = VerifyingKey::from_bytes(&trustee.public_key) else {
                continue;
            };
            let Ok(signature) = Signature::from_slice(&approval.signature) else {
                continue;
            };
            if public_key.verify(&request_hash, &signature).is_err() {
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
            outcome: outcome.clone(),
        };

        match outcome {
            BreakGlassOutcome::Granted => {
                let mut nonce = [0u8; 32];
                rand::thread_rng().fill_bytes(&mut nonce);
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
    use ed25519_dalek::{Signer, SigningKey};

    #[test]
    fn core_types_round_trip() {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let request = UnlockRequest::new("vault:1", [1u8; 32], "incident", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[1u8; 32]);
        let signature = signing_key.sign(&request.request_hash());
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
        let signature = other_key.sign(&request.request_hash());
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
        let signature = signing_key.sign(&request.request_hash());
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
        let signature = signing_key.sign(&request.request_hash());
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
}
