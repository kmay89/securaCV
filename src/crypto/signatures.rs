use anyhow::{anyhow, Result};
use ed25519_dalek::{Signer, SigningKey, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[cfg(feature = "pqc-signatures")]
use pqcrypto_mldsa::mldsa44::{self, DetachedSignature, PublicKey, SecretKey};
#[cfg(feature = "pqc-signatures")]
use pqcrypto_traits::sign::{
    DetachedSignature as PqDetachedSignature, PublicKey as PqPublicKeyTrait,
    SecretKey as PqSecretKeyTrait,
};

pub const ED25519_SCHEME_ID: &str = "ed25519";
pub const PQ_SCHEME_MLDSA44: &str = "mldsa44";
const PQ_SCHEME_DILITHIUM2_LEGACY: &str = "dilithium2";

// Domain separation strings use a shared prefix scheme aligned with firmware.
// Firmware uses "securacv:fw:chain:v1" for its hash chain.
// Kernel uses "securacv:pwk:" prefix for all domain strings.
// This intentional divergence is documented here to prevent silent verification
// failures if firmware witness records are ever verified from the kernel side.
//
// Firmware domain: "securacv:fw:chain:v1"   (hash chain on ESP32)
// Kernel domains:  "securacv:pwk:*:v2"      (all kernel signature contexts)
pub const DOMAIN_SEALED_LOG_ENTRY: &str = "securacv:pwk:sealed-log-entry:v2";
pub const DOMAIN_CHECKPOINT: &str = "securacv:pwk:sealed-log-checkpoint:v2";
pub const DOMAIN_BREAK_GLASS_RECEIPT: &str = "securacv:pwk:break-glass-receipt:v2";
pub const DOMAIN_EXPORT_RECEIPT: &str = "securacv:pwk:export-receipt:v2";
pub const DOMAIN_BREAK_GLASS_TOKEN: &str = "securacv:pwk:break-glass-token:v2";
pub const DOMAIN_KEY_ROTATION: &str = "securacv:pwk:device-key-rotation:v1";
pub const DOMAIN_KEY_ROTATION_AUTHZ: &str = "securacv:pwk:device-key-rotation-authz:v1";
// A trustee's quorum consent (Invariant V). Previously trustee approvals
// were signed over the BARE request hash with no domain — the only kernel
// signature context that bypassed domain separation — so any Ed25519
// signature a trustee key produced over a bare 32-byte value shared an
// undomained namespace with the legacy sealed-log verify path. This binds
// a trustee's consent to its own context so a signature minted elsewhere
// cannot stand in for an approval (and vice versa).
pub const DOMAIN_TRUSTEE_APPROVAL: &str = "securacv:pwk:trustee-approval:v2";
// A trustee's consent to a QUORUM POLICY CHANGE (roster, threshold, or vault
// crypto settings). Deliberately disjoint from DOMAIN_TRUSTEE_APPROVAL so an
// unlock-request approval can never stand in for consent to rewrite the
// trustee roster, and vice versa — the two consents authorize different
// powers (see spec/quorum_unseal_v2.md §3.1).
pub const DOMAIN_POLICY_CHANGE_APPROVAL: &str = "securacv:pwk:policy-change-approval:v1";
// The device's signature over an appended policy-change history record (the
// chained ledger row itself, not a trustee's consent).
pub const DOMAIN_POLICY_CHANGE_RECORD: &str = "securacv:pwk:policy-change-record:v1";
// The device's signature over a clock-provenance record (the never-pruned
// `clock_provenance` ledger: device clock vs TSA genTime, sealed once per
// anchor row; `docs/design/clock_provenance.md`). 32 bytes, so the
// length-prefixed signing message starts `20 00 00 00`.
pub const DOMAIN_CLOCK_PROVENANCE: &str = "securacv:pwk:clock-provenance:v1";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SignatureMode {
    /// Domain-separated Ed25519 required; the post-quantum signature is
    /// verified only when present. (Historically this also accepted
    /// pre-domain-separation "bare hash" Ed25519 signatures; that legacy
    /// fallback has been retired, so Compat and Strict now differ ONLY in
    /// whether the PQ signature is mandatory.)
    Compat,
    /// Domain-separated Ed25519 AND a valid post-quantum signature both
    /// required.
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
                return Err(anyhow!("pq signature present without scheme identifier"))
            }
            (None, Some(_)) => {
                return Err(anyhow!("pq scheme identifier present without signature"))
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
        self.pq_signature
            .as_ref()
            .map(|sig| sig.signature.as_slice())
    }

    pub fn pq_scheme_id(&self) -> Option<&str> {
        self.pq_signature.as_ref().map(|sig| sig.scheme_id.as_str())
    }
}

#[cfg(feature = "pqc-signatures")]
#[derive(Clone, Debug)]
pub struct PqKeypair {
    pub public_key: PublicKey,
    secret_key_bytes: zeroize::Zeroizing<Vec<u8>>,
    pub secret_key: SecretKey,
}

#[cfg(feature = "pqc-signatures")]
impl PqKeypair {
    pub fn generate() -> Self {
        let (public_key, secret_key) = mldsa44::keypair();
        let secret_key_bytes = zeroize::Zeroizing::new(secret_key.as_bytes().to_vec());
        Self {
            public_key,
            secret_key_bytes,
            secret_key,
        }
    }

    pub fn from_bytes(public_key: &[u8], secret_key: &[u8]) -> Result<Self> {
        let public_key = PublicKey::from_bytes(public_key)
            .map_err(|e| anyhow!("invalid pq public key bytes: {}", e))?;
        let sk = SecretKey::from_bytes(secret_key)
            .map_err(|e| anyhow!("invalid pq secret key bytes: {}", e))?;
        let secret_key_bytes = zeroize::Zeroizing::new(sk.as_bytes().to_vec());
        Ok(Self {
            public_key,
            secret_key_bytes,
            secret_key: sk,
        })
    }

    pub fn public_key_bytes(&self) -> Vec<u8> {
        self.public_key.as_bytes().to_vec()
    }

    pub fn secret_key_bytes(&self) -> Vec<u8> {
        self.secret_key_bytes.to_vec()
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
    // Ed25519 verification always requires domain separation. The
    // pre-domain-separation "bare entry-hash" fallback that Compat used to
    // accept has been retired — no v1 (pre-domain-separation) data is
    // deployed, and accepting an undomained signature was a
    // cross-context-confusion surface (the same class the trustee-approval
    // domain fix closed). Compat now differs from Strict ONLY in that the
    // post-quantum signature is optional; both require a domain-separated
    // Ed25519 signature.
    let ed_result = verify_ed25519(verifying_key, &signing_hash, signatures);
    let pq_result = verify_pq_signature(&signing_hash, signatures, pq_public_key);

    match mode {
        SignatureMode::Compat => {
            ed_result?;
            if pq_result.is_err() && signatures.pq_signature.is_some() && pq_public_key.is_some() {
                pq_result?;
            }
            Ok(())
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
        .verify_strict(&signing_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}

/// Parses an Ed25519 verifying key and refuses a **weak** one (a point of
/// small order, the identity included). Under such a key `(R = identity,
/// s = 0)` verifies for *every* message, so "signed by this key" proves no
/// act of signing. Every key read from storage, the wire, a policy or a
/// rotation record goes through here; `ed25519_dalek::VerifyingKey::from_bytes`
/// alone accepts them.
pub fn verifying_key_strict(
    bytes: &[u8; 32],
) -> Result<VerifyingKey, ed25519_dalek::SignatureError> {
    let key = VerifyingKey::from_bytes(bytes)?;
    if key.is_weak() {
        return Err(ed25519_dalek::SignatureError::new());
    }
    Ok(key)
}

/// Compute the domain-separated signing hash for a chain entry.
///
/// This is the exact preimage that is Ed25519/ML-DSA-signed:
/// `SHA256( le32(len(domain)) || domain_utf8 || entry_hash )`.
/// The offline JavaScript verifier (`viewer/verify_core.js`) MUST reproduce this byte layout
/// exactly; `tests/fixtures/envelope/domain_separation_vectors.json` pins it cross-language.
/// Hash binding a device-key rotation's old→new identity. The new key signs this
/// Domain-separated hash binding a rotation's `(prev ‖ new)` identity pair, used by both
/// the new key's *possession attestation* and the old key's *authorization* (distinct
/// domains keep the two signatures non-interchangeable).
fn rotation_binding_hash(
    domain: &str,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_public_key);
    hasher.update(new_public_key);
    let inner: [u8; 32] = hasher.finalize().into();
    domain_separated_hash(domain, &inner)
}

fn verify_rotation_binding(
    domain: &str,
    verifying_key: &VerifyingKey,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
    signature: &[u8],
    what: &str,
) -> Result<()> {
    if signature.len() != 64 {
        return Err(anyhow!(
            "rotation {}: expected 64 bytes, got {}",
            what,
            signature.len()
        ));
    }
    let mut sig_bytes = [0u8; 64];
    sig_bytes.copy_from_slice(signature);
    let sig = ed25519_dalek::Signature::from_bytes(&sig_bytes);
    verifying_key
        .verify_strict(
            &rotation_binding_hash(domain, prev_public_key, new_public_key),
            &sig,
        )
        .map_err(|e| anyhow!("rotation {} verification failed: {}", what, e))
}

/// Produce the new key's possession attestation over `(prev_public_key ‖ new_public_key)`,
/// proving the rotator controls the incoming private key.
pub fn sign_rotation_attestation(
    new_signing_key: &SigningKey,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
) -> [u8; 64] {
    new_signing_key
        .sign(&rotation_binding_hash(
            DOMAIN_KEY_ROTATION,
            prev_public_key,
            new_public_key,
        ))
        .to_bytes()
}

/// Verify the new key's possession attestation (proves the incoming key holder signed the
/// old→new binding).
pub fn verify_rotation_attestation(
    new_verifying_key: &VerifyingKey,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
    attestation: &[u8],
) -> Result<()> {
    verify_rotation_binding(
        DOMAIN_KEY_ROTATION,
        new_verifying_key,
        prev_public_key,
        new_public_key,
        attestation,
        "attestation",
    )
}

/// Produce the retiring (old) key's authorization over `(prev_public_key ‖ new_public_key)`.
///
/// This is the anchor that makes a stored key-history lineage *unforgeable*: each rotation
/// is signed by its predecessor, and the chain is rooted at the genesis key. A tamperer who
/// rewrites the history table cannot forge the first authorization without the genesis
/// private key, so the whole lineage remains genesis-anchored even after the in-chain
/// rotation records are pruned.
pub fn sign_rotation_authorization(
    old_signing_key: &SigningKey,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
) -> [u8; 64] {
    old_signing_key
        .sign(&rotation_binding_hash(
            DOMAIN_KEY_ROTATION_AUTHZ,
            prev_public_key,
            new_public_key,
        ))
        .to_bytes()
}

/// Verify the retiring key's authorization (proves the predecessor approved this successor).
pub fn verify_rotation_authorization(
    old_verifying_key: &VerifyingKey,
    prev_public_key: &[u8; 32],
    new_public_key: &[u8; 32],
    authorization: &[u8],
) -> Result<()> {
    verify_rotation_binding(
        DOMAIN_KEY_ROTATION_AUTHZ,
        old_verifying_key,
        prev_public_key,
        new_public_key,
        authorization,
        "authorization",
    )
}

pub fn domain_separated_hash(domain: &str, entry_hash: &[u8; 32]) -> [u8; 32] {
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
        .verify_strict(signing_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}

fn sign_pq(signing_hash: &[u8; 32], keys: &SignatureKeys<'_>) -> Result<Option<PqSignature>> {
    #[cfg(feature = "pqc-signatures")]
    {
        if let Some(secret_key) = keys.pq {
            let signature = mldsa44::detached_sign(signing_hash, secret_key);
            return Ok(Some(PqSignature {
                scheme_id: PQ_SCHEME_MLDSA44.to_string(),
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
    if signature.scheme_id != PQ_SCHEME_MLDSA44
        && signature.scheme_id != PQ_SCHEME_DILITHIUM2_LEGACY
    {
        return Err(anyhow!(
            "unsupported pq signature scheme: {}",
            signature.scheme_id
        ));
    }

    #[cfg(feature = "pqc-signatures")]
    {
        let public_key = pq_public_key.ok_or_else(|| anyhow!("pq public key is required"))?;
        let detached = DetachedSignature::from_bytes(&signature.signature)
            .map_err(|e| anyhow!("invalid pq signature bytes: {}", e))?;
        mldsa44::verify_detached_signature(&detached, signing_hash, public_key)
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

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::SigningKey;

    fn test_keys() -> (SigningKey, [u8; 32]) {
        let signing_key = SigningKey::from_bytes(&[42u8; 32]);
        let entry_hash = [1u8; 32];
        (signing_key, entry_hash)
    }

    #[test]
    fn rotation_attestation_round_trips_and_rejects_tampering() {
        let old = SigningKey::from_bytes(&[1u8; 32])
            .verifying_key()
            .to_bytes();
        let new_sk = SigningKey::from_bytes(&[2u8; 32]);
        let new = new_sk.verifying_key().to_bytes();

        let att = sign_rotation_attestation(&new_sk, &old, &new);
        let new_vk = new_sk.verifying_key();
        // Valid attestation verifies.
        verify_rotation_attestation(&new_vk, &old, &new, &att).expect("valid attestation");

        // A flipped byte is rejected.
        let mut bad = att;
        bad[0] ^= 0xff;
        assert!(verify_rotation_attestation(&new_vk, &old, &new, &bad).is_err());

        // Wrong key is rejected (an attacker cannot attest to a key they don't control).
        let attacker_vk = SigningKey::from_bytes(&[3u8; 32]).verifying_key();
        assert!(verify_rotation_attestation(&attacker_vk, &old, &new, &att).is_err());

        // Rebinding to a different identity pair is rejected.
        let other = SigningKey::from_bytes(&[4u8; 32])
            .verifying_key()
            .to_bytes();
        assert!(verify_rotation_attestation(&new_vk, &old, &other, &att).is_err());

        // Wrong length is rejected without panicking.
        assert!(verify_rotation_attestation(&new_vk, &old, &new, &att[..32]).is_err());
    }

    #[test]
    fn compat_mode_rejects_bare_hash_signature() {
        // A signature over the BARE entry hash (the pre-domain-separation
        // construction the retired legacy fallback used to accept) must now
        // be rejected even under Compat — Ed25519 verification always
        // requires domain separation.
        use ed25519_dalek::Signer;
        let (signing_key, entry_hash) = test_keys();
        let bare_sig = signing_key.sign(&entry_hash).to_bytes();
        let sig_set = SignatureSet::new(bare_sig, None);

        let result = verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &sig_set,
            SignatureMode::Compat,
            None,
        );
        assert!(
            result.is_err(),
            "Compat must reject a bare-hash (undomained) Ed25519 signature"
        );

        // And the correctly domain-separated signature still verifies.
        let keys = SignatureKeys::new(&signing_key);
        let good = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();
        assert!(verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &good,
            SignatureMode::Compat,
            None,
        )
        .is_ok());
    }

    #[test]
    fn compat_mode_requires_ed25519() {
        let (signing_key, entry_hash) = test_keys();
        let keys = SignatureKeys::new(&signing_key);
        let sig_set = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();

        let mut bad_sig_set = sig_set.clone();
        bad_sig_set.ed25519_signature[0] ^= 0xFF;

        let result = verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &bad_sig_set,
            SignatureMode::Compat,
            None,
        );
        assert!(
            result.is_err(),
            "Compat mode must reject bad Ed25519 signature"
        );
    }

    #[test]
    fn compat_mode_accepts_missing_pq() {
        let (signing_key, entry_hash) = test_keys();
        let keys = SignatureKeys::new(&signing_key);
        let sig_set = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();

        let result = verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &sig_set,
            SignatureMode::Compat,
            None,
        );
        assert!(
            result.is_ok(),
            "Compat mode should accept valid Ed25519 without PQ"
        );
    }

    #[test]
    fn compat_mode_rejects_bad_pq_when_present() {
        let (signing_key, entry_hash) = test_keys();
        let keys = SignatureKeys::new(&signing_key);
        let mut sig_set = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();

        sig_set.pq_signature = Some(PqSignature {
            scheme_id: PQ_SCHEME_MLDSA44.to_string(),
            signature: vec![0xDE; 64],
        });

        // Without a PQ public key, Compat mode skips PQ verification
        let result_no_key = verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &sig_set,
            SignatureMode::Compat,
            None,
        );
        assert!(
            result_no_key.is_ok(),
            "Compat mode should skip PQ verification when no PQ key is available"
        );

        // Strict mode always requires PQ to pass
        let result_strict = verify_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &signing_key.verifying_key(),
            &entry_hash,
            &sig_set,
            SignatureMode::Strict,
            None,
        );
        assert!(
            result_strict.is_err(),
            "Strict mode must reject when PQ verification fails"
        );
    }

    /// The known-answer material in `tests/fixtures/pqcrypto_kat/`: an
    /// ML-DSA-44 keypair in the encodings `device_metadata` stores, and a
    /// signature over a sealed-log entry hash, all written by the kernel while
    /// it linked `pqcrypto-mldsa` 0.1.2 (the README there says how). These
    /// tests hold whichever ML-DSA implementation this build links to that
    /// material: a device keeps its stored key and every entry it has signed.
    #[cfg(feature = "pqc-signatures")]
    mod pqcrypto_kat {
        use super::*;

        pub(super) const KAT_PATH: &str = "tests/fixtures/pqcrypto_kat/mldsa44_kat.json";

        pub(super) struct Kat {
            pub domain: String,
            pub entry_hash: [u8; 32],
            pub ed25519_public_key: VerifyingKey,
            pub ed25519_signature: Vec<u8>,
            pub scheme_id: String,
            pub public_key: Vec<u8>,
            pub secret_key: Vec<u8>,
            pub signature: Vec<u8>,
        }

        impl Kat {
            pub fn signature_set(&self, pq_signature: Vec<u8>) -> SignatureSet {
                SignatureSet::from_storage(
                    &self.ed25519_signature,
                    Some(pq_signature),
                    Some(self.scheme_id.clone()),
                )
                .expect("the stored columns form a signature set")
            }

            pub fn verify(&self, pq_signature: Vec<u8>, mode: SignatureMode) -> Result<()> {
                let keypair = PqKeypair::from_bytes(&self.public_key, &self.secret_key)?;
                verify_with_domain(
                    &self.domain,
                    &self.ed25519_public_key,
                    &self.entry_hash,
                    &self.signature_set(pq_signature),
                    mode,
                    Some(&keypair.public_key),
                )
            }
        }

        pub(super) fn load() -> Kat {
            let text = std::fs::read_to_string(KAT_PATH)
                .unwrap_or_else(|e| panic!("read {KAT_PATH}: {e}"));
            let doc: serde_json::Value =
                serde_json::from_str(&text).unwrap_or_else(|e| panic!("parse {KAT_PATH}: {e}"));
            let bytes = |name: &str| -> Vec<u8> {
                let field = doc[name]
                    .as_str()
                    .unwrap_or_else(|| panic!("{KAT_PATH}: no string field {name}"));
                hex::decode(field).unwrap_or_else(|e| panic!("{KAT_PATH}: {name} is not hex: {e}"))
            };
            let text = |name: &str| -> String {
                doc[name]
                    .as_str()
                    .unwrap_or_else(|| panic!("{KAT_PATH}: no string field {name}"))
                    .to_string()
            };
            let ed25519_public_key: [u8; 32] = bytes("ed25519_public_key")
                .try_into()
                .expect("32-byte Ed25519 public key");
            Kat {
                domain: text("domain"),
                entry_hash: bytes("entry_hash").try_into().expect("32-byte entry hash"),
                ed25519_public_key: verifying_key_strict(&ed25519_public_key)
                    .expect("fixture Ed25519 key"),
                ed25519_signature: bytes("ed25519_signature"),
                scheme_id: text("scheme_id"),
                public_key: bytes("pq_public_key"),
                secret_key: bytes("pq_secret_key"),
                signature: bytes("pq_signature"),
            }
        }

        /// Writes the fixture with the ML-DSA-44 implementation this build
        /// links. It is run once, by hand, while that implementation is
        /// `pqcrypto-mldsa`, and never again:
        ///   cargo test --features pqc-signatures --lib write_pqcrypto_mldsa44_kat -- --ignored
        #[test]
        #[ignore = "writes tests/fixtures/pqcrypto_kat/mldsa44_kat.json; see the README beside it"]
        fn write_pqcrypto_mldsa44_kat() {
            let ed25519 = SigningKey::from_bytes(&[42u8; 32]);
            let entry_hash: [u8; 32] =
                Sha256::digest(b"securacv pqcrypto kat: sealed log entry").into();
            let keypair = PqKeypair::generate();
            let keys = SignatureKeys::with_pq(&ed25519, Some(&keypair.secret_key));
            let set = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();
            let pq = set.pq_signature.as_ref().expect("a pq signature");
            let doc = serde_json::json!({
                "description": "ML-DSA-44 known-answer material: a keypair in the byte encodings \
                    device_metadata.pq_public_key and .pq_secret_key hold, and the signature set \
                    sign_with_domain() produced over entry_hash under domain. Never regenerate.",
                "written_by": "witness-kernel linking pqcrypto-mldsa 0.1.2 (PQClean ML-DSA-44)",
                "scheme_id": pq.scheme_id,
                "domain": DOMAIN_SEALED_LOG_ENTRY,
                "entry_hash": hex::encode(entry_hash),
                "signing_hash": hex::encode(domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, &entry_hash)),
                "ed25519_public_key": hex::encode(ed25519.verifying_key().to_bytes()),
                "ed25519_signature": hex::encode(&set.ed25519_signature),
                "pq_public_key": hex::encode(keypair.public_key_bytes()),
                "pq_secret_key": hex::encode(keypair.secret_key_bytes()),
                "pq_signature": hex::encode(&pq.signature),
            });
            let json = serde_json::to_string_pretty(&doc).unwrap();
            std::fs::write(KAT_PATH, json + "\n").unwrap();
        }

        #[test]
        fn stored_key_encodings_load_and_come_back_byte_for_byte() {
            let kat = load();
            assert_eq!(kat.public_key.len(), 1312, "FIPS 204 ML-DSA-44 public key");
            assert_eq!(kat.secret_key.len(), 2560, "FIPS 204 ML-DSA-44 private key");
            let keypair = PqKeypair::from_bytes(&kat.public_key, &kat.secret_key)
                .expect("a key the kernel stored must load");
            assert_eq!(keypair.public_key_bytes(), kat.public_key);
            assert_eq!(keypair.secret_key_bytes(), kat.secret_key);
        }

        #[test]
        fn stored_signature_verifies_in_strict_mode() {
            let kat = load();
            assert_eq!(kat.scheme_id, PQ_SCHEME_MLDSA44);
            assert_eq!(kat.signature.len(), 2420, "FIPS 204 ML-DSA-44 signature");
            kat.verify(kat.signature.clone(), SignatureMode::Strict)
                .expect("an entry the kernel signed must keep verifying");
            kat.verify(kat.signature.clone(), SignatureMode::Compat)
                .expect("and in compat mode, which checks a pq signature that is present");
        }

        #[test]
        fn stored_key_signs_and_its_new_signatures_verify() {
            let kat = load();
            let ed25519 = SigningKey::from_bytes(&[42u8; 32]);
            assert_eq!(ed25519.verifying_key(), kat.ed25519_public_key);
            let keypair = PqKeypair::from_bytes(&kat.public_key, &kat.secret_key).unwrap();
            let keys = SignatureKeys::with_pq(&ed25519, Some(&keypair.secret_key));
            for entry_hash in [kat.entry_hash, [0u8; 32], [0xFFu8; 32]] {
                let set = sign_with_domain(&kat.domain, &keys, &entry_hash).unwrap();
                let pq = set.pq_signature.as_ref().expect("a pq signature");
                assert_eq!(pq.scheme_id, PQ_SCHEME_MLDSA44);
                assert_eq!(pq.signature.len(), 2420);
                verify_with_domain(
                    &kat.domain,
                    &kat.ed25519_public_key,
                    &entry_hash,
                    &set,
                    SignatureMode::Strict,
                    Some(&keypair.public_key),
                )
                .expect("a fresh signature from the stored key verifies");
            }
        }

        #[test]
        fn stored_signature_fails_under_any_other_message_domain_or_key() {
            let kat = load();
            let keypair = PqKeypair::from_bytes(&kat.public_key, &kat.secret_key).unwrap();
            let set = kat.signature_set(kat.signature.clone());

            // The Ed25519 half is re-signed for each case, so the only thing
            // that can refuse is the post-quantum signature.
            let ed25519 = SigningKey::from_bytes(&[42u8; 32]);
            let resigned = |domain: &str, entry_hash: &[u8; 32]| {
                let mut other = set.clone();
                other.ed25519_signature = sign_ed25519_only(domain, &ed25519, entry_hash).to_vec();
                other
            };

            let mut other_hash = kat.entry_hash;
            other_hash[31] ^= 1;
            assert!(verify_with_domain(
                &kat.domain,
                &kat.ed25519_public_key,
                &other_hash,
                &resigned(&kat.domain, &other_hash),
                SignatureMode::Strict,
                Some(&keypair.public_key),
            )
            .is_err());
            assert!(verify_with_domain(
                DOMAIN_CHECKPOINT,
                &kat.ed25519_public_key,
                &kat.entry_hash,
                &resigned(DOMAIN_CHECKPOINT, &kat.entry_hash),
                SignatureMode::Strict,
                Some(&keypair.public_key),
            )
            .is_err());

            let stranger = PqKeypair::generate();
            assert!(verify_with_domain(
                &kat.domain,
                &kat.ed25519_public_key,
                &kat.entry_hash,
                &set,
                SignatureMode::Strict,
                Some(&stranger.public_key),
            )
            .is_err());
            // Compat checks a pq signature whenever one and its key are present.
            assert!(verify_with_domain(
                &kat.domain,
                &kat.ed25519_public_key,
                &kat.entry_hash,
                &set,
                SignatureMode::Compat,
                Some(&stranger.public_key),
            )
            .is_err());
        }

        #[test]
        fn one_flipped_byte_anywhere_in_the_stored_signature_fails() {
            let kat = load();
            kat.verify(kat.signature.clone(), SignatureMode::Strict)
                .expect("the untouched signature verifies");
            // The challenge hash is bytes 0..32, the response z is 32..2336
            // and the hint is 2336..2420: the ends of each, and a stride
            // across all three.
            let mut positions: Vec<usize> = vec![0, 31, 32, 2335, 2336, 2419];
            positions.extend((0..kat.signature.len()).step_by(41));
            for position in positions {
                for mask in [0x01u8, 0x80] {
                    let mut tampered = kat.signature.clone();
                    tampered[position] ^= mask;
                    assert!(
                        kat.verify(tampered.clone(), SignatureMode::Strict).is_err(),
                        "strict accepted the signature with byte {position} xor {mask:#04x}"
                    );
                    assert!(
                        kat.verify(tampered, SignatureMode::Compat).is_err(),
                        "compat accepted the signature with byte {position} xor {mask:#04x}"
                    );
                }
            }
        }

        #[test]
        fn a_signature_of_the_wrong_length_fails() {
            let kat = load();
            for len in [0usize, 64, 2419] {
                assert!(
                    kat.verify(kat.signature[..len].to_vec(), SignatureMode::Strict)
                        .is_err(),
                    "accepted a {len}-byte signature"
                );
            }
            let mut long = kat.signature.clone();
            long.push(0);
            assert!(kat.verify(long, SignatureMode::Strict).is_err());
        }

        #[test]
        fn keys_of_the_wrong_length_are_refused() {
            let kat = load();
            assert!(PqKeypair::from_bytes(&kat.public_key[..1311], &kat.secret_key).is_err());
            assert!(PqKeypair::from_bytes(&kat.public_key, &kat.secret_key[..2559]).is_err());
            assert!(PqKeypair::from_bytes(&[], &[]).is_err());
            let mut long = kat.secret_key.clone();
            long.push(0);
            assert!(PqKeypair::from_bytes(&kat.public_key, &long).is_err());
        }

        #[test]
        fn a_generated_keypair_round_trips_through_its_stored_encodings() {
            let ed25519 = SigningKey::from_bytes(&[7u8; 32]);
            let entry_hash = [9u8; 32];
            let generated = PqKeypair::generate();
            assert_eq!(generated.public_key_bytes().len(), 1312);
            assert_eq!(generated.secret_key_bytes().len(), 2560);
            let reloaded =
                PqKeypair::from_bytes(&generated.public_key_bytes(), &generated.secret_key_bytes())
                    .expect("what generate() stores must load");
            assert_eq!(reloaded.public_key_bytes(), generated.public_key_bytes());
            assert_eq!(reloaded.secret_key_bytes(), generated.secret_key_bytes());

            // Signed by one copy, verified with the other's public key, both ways.
            for (signer, verifier) in [(&generated, &reloaded), (&reloaded, &generated)] {
                let keys = SignatureKeys::with_pq(&ed25519, Some(&signer.secret_key));
                let set = sign_with_domain(DOMAIN_CHECKPOINT, &keys, &entry_hash).unwrap();
                verify_with_domain(
                    DOMAIN_CHECKPOINT,
                    &ed25519.verifying_key(),
                    &entry_hash,
                    &set,
                    SignatureMode::Strict,
                    Some(&verifier.public_key),
                )
                .expect("round trip");
            }
        }
    }

    #[test]
    fn legacy_dilithium2_scheme_id_not_rejected_as_unsupported() {
        let (signing_key, entry_hash) = test_keys();
        let keys = SignatureKeys::new(&signing_key);
        let mut sig_set = sign_with_domain(DOMAIN_SEALED_LOG_ENTRY, &keys, &entry_hash).unwrap();

        sig_set.pq_signature = Some(PqSignature {
            scheme_id: "dilithium2".to_string(),
            signature: vec![0xAB; 64],
        });

        let signing_hash = domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, &entry_hash);
        let result = verify_pq_signature(&signing_hash, &sig_set, None);
        let err = result.unwrap_err();
        let msg = format!("{err}");
        assert!(
            !msg.contains("unsupported pq signature scheme"),
            "dilithium2 scheme ID should be accepted, got: {msg}"
        );
    }
}
