use anyhow::{anyhow, Result};
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[cfg(feature = "pqc-signatures")]
use pqcrypto_dilithium::dilithium2::{self, DetachedSignature, PublicKey, SecretKey};
#[cfg(feature = "pqc-signatures")]
use pqcrypto_traits::sign::{
    DetachedSignature as PqDetachedSignature, PublicKey as PqPublicKeyTrait,
    SecretKey as PqSecretKeyTrait,
};

pub const ED25519_SCHEME_ID: &str = "ed25519";
pub const PQ_SCHEME_DILITHIUM2: &str = "dilithium2";

pub const DOMAIN_SEALED_LOG_ENTRY: &str = "pwk:sealed-log-entry:v2";
pub const DOMAIN_CHECKPOINT: &str = "pwk:sealed-log-checkpoint:v2";
pub const DOMAIN_BREAK_GLASS_RECEIPT: &str = "pwk:break-glass-receipt:v2";
pub const DOMAIN_EXPORT_RECEIPT: &str = "pwk:export-receipt:v2";
pub const DOMAIN_BREAK_GLASS_TOKEN: &str = "pwk:break-glass-token:v2";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SignatureMode {
    Compat,
    Strict,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PqSignature {
    pub scheme_id: String,
    pub signature: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SignatureSet {
    pub ed25519_scheme: String,
    pub ed25519_signature: Vec<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pq_signature: Option<PqSignature>,
}

impl SignatureSet {
    pub fn new(ed25519_signature: [u8; 64], pq_signature: Option<PqSignature>) -> Self {
        Self {
            ed25519_scheme: ED25519_SCHEME_ID.to_string(),
            ed25519_signature: ed25519_signature.to_vec(),
            pq_signature,
        }
    }

    pub fn from_storage(
        ed25519_signature: &[u8],
        pq_signature: Option<Vec<u8>>,
        pq_scheme: Option<String>,
    ) -> Result<Self> {
        if ed25519_signature.len() != 64 {
            return Err(anyhow!(
                "invalid ed25519 signature length: expected 64 bytes, got {}",
                ed25519_signature.len()
            ));
        }
        let ed = ed25519_signature.to_vec();
        let pq = match (pq_signature, pq_scheme) {
            (Some(signature), Some(scheme_id)) => Some(PqSignature {
                scheme_id,
                signature,
            }),
            (None, None) => None,
            (Some(_), None) => {
                return Err(anyhow!(
                    "pq signature present without scheme identifier"
                ))
            }
            (None, Some(_)) => {
                return Err(anyhow!(
                    "pq scheme identifier present without signature"
                ))
            }
        };
        Ok(Self {
            ed25519_scheme: ED25519_SCHEME_ID.to_string(),
            ed25519_signature: ed,
            pq_signature: pq,
        })
    }

    pub fn ed25519_signature_array(&self) -> Result<[u8; 64]> {
        if self.ed25519_signature.len() != 64 {
            return Err(anyhow!(
                "invalid ed25519 signature length: expected 64 bytes, got {}",
                self.ed25519_signature.len()
            ));
        }
        let mut out = [0u8; 64];
        out.copy_from_slice(&self.ed25519_signature);
        Ok(out)
    }

    pub fn pq_signature_bytes(&self) -> Option<&[u8]> {
        self.pq_signature.as_ref().map(|sig| sig.signature.as_slice())
    }

    pub fn pq_scheme_id(&self) -> Option<&str> {
        self.pq_signature.as_ref().map(|sig| sig.scheme_id.as_str())
    }
}

#[cfg(feature = "pqc-signatures")]
#[derive(Clone, Debug)]
pub struct PqKeypair {
    pub public_key: PublicKey,
    pub secret_key: SecretKey,
}

#[cfg(feature = "pqc-signatures")]
impl PqKeypair {
    pub fn generate() -> Self {
        let (public_key, secret_key) = dilithium2::keypair();
        Self {
            public_key,
            secret_key,
        }
    }

    pub fn from_bytes(public_key: &[u8], secret_key: &[u8]) -> Result<Self> {
        let public_key = PublicKey::from_bytes(public_key)
            .map_err(|e| anyhow!("invalid pq public key bytes: {}", e))?;
        let secret_key = SecretKey::from_bytes(secret_key)
            .map_err(|e| anyhow!("invalid pq secret key bytes: {}", e))?;
        Ok(Self {
            public_key,
            secret_key,
        })
    }

    pub fn public_key_bytes(&self) -> Vec<u8> {
        self.public_key.as_bytes().to_vec()
    }

    pub fn secret_key_bytes(&self) -> Vec<u8> {
        self.secret_key.as_bytes().to_vec()
    }
}

#[cfg(feature = "pqc-signatures")]
pub type PqPublicKey = PublicKey;
#[cfg(feature = "pqc-signatures")]
pub type PqSecretKey = SecretKey;

#[cfg(not(feature = "pqc-signatures"))]
#[derive(Clone, Debug)]
pub struct PqPublicKey;
#[cfg(not(feature = "pqc-signatures"))]
#[derive(Clone, Debug)]
pub struct PqSecretKey;

#[derive(Clone, Copy, Debug)]
pub struct SignatureKeys<'a> {
    pub ed25519: &'a SigningKey,
    #[cfg(feature = "pqc-signatures")]
    pub pq: Option<&'a PqSecretKey>,
}

impl<'a> SignatureKeys<'a> {
    pub fn new(ed25519: &'a SigningKey) -> Self {
        Self {
            ed25519,
            #[cfg(feature = "pqc-signatures")]
            pq: None,
        }
    }

    #[cfg(feature = "pqc-signatures")]
    pub fn with_pq(ed25519: &'a SigningKey, pq: Option<&'a PqSecretKey>) -> Self {
        Self { ed25519, pq }
    }
}

pub fn sign_with_domain(
    domain: &str,
    keys: &SignatureKeys<'_>,
    entry_hash: &[u8; 32],
) -> Result<SignatureSet> {
    let signing_hash = domain_separated_hash(domain, entry_hash);
    let ed25519_signature = keys.ed25519.sign(&signing_hash).to_bytes();
    let pq_signature = sign_pq(&signing_hash, keys);
    Ok(SignatureSet::new(ed25519_signature, pq_signature?))
}

pub fn verify_with_domain(
    domain: &str,
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signatures: &SignatureSet,
    mode: SignatureMode,
    pq_public_key: Option<&PqPublicKey>,
) -> Result<()> {
    let signing_hash = domain_separated_hash(domain, entry_hash);
    let ed_result = verify_ed25519(verifying_key, &signing_hash, signatures);
    let ed_result = match (mode, ed_result) {
        (SignatureMode::Compat, Err(err)) => verify_ed25519_legacy(verifying_key, entry_hash, signatures).map_err(|_| err),
        (_, result) => result,
    };
    let pq_result = verify_pq_signature(&signing_hash, signatures, pq_public_key);

    match mode {
        SignatureMode::Compat => {
            if ed_result.is_ok() || pq_result.is_ok() {
                Ok(())
            } else {
                Err(anyhow!(
                    "signature verification failed: ed25519_error={}, pq_error={}",
                    ed_result.unwrap_err(),
                    pq_result.unwrap_err()
                ))
            }
        }
        SignatureMode::Strict => {
            ed_result?;
            pq_result?;
            Ok(())
        }
    }
}

pub fn sign_ed25519_only(
    domain: &str,
    signing_key: &SigningKey,
    entry_hash: &[u8; 32],
) -> [u8; 64] {
    let signing_hash = domain_separated_hash(domain, entry_hash);
    signing_key.sign(&signing_hash).to_bytes()
}

pub fn verify_ed25519_only(
    domain: &str,
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signature: &[u8; 64],
) -> Result<()> {
    let signing_hash = domain_separated_hash(domain, entry_hash);
    let sig = ed25519_dalek::Signature::from_bytes(signature);
    verifying_key
        .verify(&signing_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}

fn domain_separated_hash(domain: &str, entry_hash: &[u8; 32]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    let domain_bytes = domain.as_bytes();
    hasher.update((domain_bytes.len() as u32).to_le_bytes());
    hasher.update(domain_bytes);
    hasher.update(entry_hash);
    hasher.finalize().into()
}

fn verify_ed25519(
    verifying_key: &VerifyingKey,
    signing_hash: &[u8; 32],
    signatures: &SignatureSet,
) -> Result<()> {
    let signature_bytes = signatures.ed25519_signature_array()?;
    let sig = ed25519_dalek::Signature::from_bytes(&signature_bytes);
    verifying_key
        .verify(signing_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}

fn verify_ed25519_legacy(
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signatures: &SignatureSet,
) -> Result<()> {
    let signature_bytes = signatures.ed25519_signature_array()?;
    let sig = ed25519_dalek::Signature::from_bytes(&signature_bytes);
    verifying_key
        .verify(entry_hash, &sig)
        .map_err(|e| anyhow!("legacy signature verification failed: {}", e))
}

fn sign_pq(signing_hash: &[u8; 32], keys: &SignatureKeys<'_>) -> Result<Option<PqSignature>> {
    #[cfg(feature = "pqc-signatures")]
    {
        if let Some(secret_key) = keys.pq {
            let signature = dilithium2::detached_sign(signing_hash, secret_key);
            return Ok(Some(PqSignature {
                scheme_id: PQ_SCHEME_DILITHIUM2.to_string(),
                signature: signature.as_bytes().to_vec(),
            }));
        }
        Ok(None)
    }
    #[cfg(not(feature = "pqc-signatures"))]
    {
        let _ = signing_hash;
        let _ = keys;
        Ok(None)
    }
}

fn verify_pq_signature(
    signing_hash: &[u8; 32],
    signatures: &SignatureSet,
    pq_public_key: Option<&PqPublicKey>,
) -> Result<()> {
    let Some(signature) = signatures.pq_signature.as_ref() else {
        return Err(anyhow!("pq signature missing"));
    };
    if signature.scheme_id != PQ_SCHEME_DILITHIUM2 {
        return Err(anyhow!(
            "unsupported pq signature scheme: {}",
            signature.scheme_id
        ));
    }

    #[cfg(feature = "pqc-signatures")]
    {
        let public_key =
            pq_public_key.ok_or_else(|| anyhow!("pq public key is required"))?;
        let detached = DetachedSignature::from_bytes(&signature.signature)
            .map_err(|e| anyhow!("invalid pq signature bytes: {}", e))?;
        dilithium2::verify_detached_signature(&detached, signing_hash, public_key)
            .map_err(|e| anyhow!("pq signature verification failed: {}", e))
    }
    #[cfg(not(feature = "pqc-signatures"))]
    {
        let _ = signing_hash;
        let _ = pq_public_key;
        Err(anyhow!(
            "pq signature verification not available (pqc-signatures feature disabled)"
        ))
    }
}
