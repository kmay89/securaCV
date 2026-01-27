use anyhow::{anyhow, Result};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CryptoMode {
    Classical,
    Pq,
    Hybrid,
}

impl CryptoMode {
    pub fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "classical" | "classic" => Ok(Self::Classical),
            "pq" | "post-quantum" | "postquantum" => Ok(Self::Pq),
            "hybrid" | "hybrid-pq" => Ok(Self::Hybrid),
            other => Err(anyhow!(
                "unsupported crypto mode '{}'; expected 'classical', 'pq', or 'hybrid'",
                other
            )),
        }
    }
}

impl Default for CryptoMode {
    fn default() -> Self {
        Self::Classical
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SignaturePolicy {
    Ed25519Only,
    DualOptional,
    DualRequired,
}

impl SignaturePolicy {
    pub fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "ed25519-only" | "ed25519" | "classic" => Ok(Self::Ed25519Only),
            "dual-optional" | "dual_optional" => Ok(Self::DualOptional),
            "dual-required" | "dual_required" => Ok(Self::DualRequired),
            other => Err(anyhow!(
                "unsupported signature policy '{}'; expected 'ed25519-only', 'dual-optional', or 'dual-required'",
                other
            )),
        }
    }
}

impl Default for SignaturePolicy {
    fn default() -> Self {
        Self::Ed25519Only
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportPolicy {
    ClassicTls,
    HybridPqTls,
}

impl TransportPolicy {
    pub fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "classic" | "classic-tls" | "classical" => Ok(Self::ClassicTls),
            "hybrid" | "hybrid-pq" | "hybrid-pq-tls" => Ok(Self::HybridPqTls),
            other => Err(anyhow!(
                "unsupported transport TLS policy '{}'; expected 'classic-tls' or 'hybrid-pq-tls'",
                other
            )),
        }
    }
}

impl Default for TransportPolicy {
    fn default() -> Self {
        Self::ClassicTls
    }
}

pub fn ensure_vault_crypto_mode(mode: CryptoMode) -> Result<()> {
    match mode {
        CryptoMode::Classical => Ok(()),
        CryptoMode::Pq => Err(anyhow!(
            "vault crypto_mode=pq is not implemented; use classical"
        )),
        CryptoMode::Hybrid => Err(anyhow!(
            "vault crypto_mode=hybrid is not implemented; use classical"
        )),
    }
}

pub fn verify_entry_signature_with_policy(
    policy: SignaturePolicy,
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signature: &[u8; 64],
    pq_signature: Option<&[u8]>,
) -> Result<()> {
    let sig = Signature::from_bytes(signature);
    verifying_key
        .verify(entry_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))?;

    match policy {
        SignaturePolicy::Ed25519Only => Ok(()),
        SignaturePolicy::DualOptional => {
            if pq_signature.is_some() {
                return Err(anyhow!("pq signature verification is not implemented"));
            }
            Ok(())
        }
        SignaturePolicy::DualRequired => Err(anyhow!(
            "signature policy requires pq verification, which is not implemented"
        )),
    }
}

pub fn enforce_transport_policy(policy: TransportPolicy, tls_enabled: bool) -> Result<()> {
    match policy {
        TransportPolicy::ClassicTls => Ok(()),
        TransportPolicy::HybridPqTls => {
            if !tls_enabled {
                return Err(anyhow!(
                    "hybrid PQ TLS policy requires TLS to be enabled"
                ));
            }
            Err(anyhow!(
                "hybrid PQ TLS transport is not implemented; use classic-tls"
            ))
        }
    }
}
