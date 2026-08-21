use anyhow::{anyhow, Result};
use chacha20poly1305::{
    aead::{AeadInOut, KeyInit},
    ChaCha20Poly1305, Nonce, Tag,
};
use rand::rngs::SysRng;
use rand::TryRng;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use zeroize::Zeroize;

use crate::vault::format::{read_slice, read_u32, EnvelopeV1, EnvelopeV2};

pub const AEAD_ALG_CHACHA20POLY1305: &str = "chacha20poly1305";
pub const KEM_ALG_ML_KEM_768: &str = "ml-kem-768";
pub const KEM_ALG_NONE: &str = "none";

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum VaultCryptoMode {
    #[default]
    Classical,
    Pq,
    Hybrid,
}

impl std::fmt::Display for VaultCryptoMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            VaultCryptoMode::Classical => "classical",
            VaultCryptoMode::Pq => "pq",
            VaultCryptoMode::Hybrid => "hybrid",
        };
        write!(f, "{}", s)
    }
}

impl std::str::FromStr for VaultCryptoMode {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        match s.trim().to_ascii_lowercase().as_str() {
            "classical" => Ok(VaultCryptoMode::Classical),
            "pq" => Ok(VaultCryptoMode::Pq),
            "hybrid" => Ok(VaultCryptoMode::Hybrid),
            other => Err(anyhow!("unknown vault crypto mode: {}", other)),
        }
    }
}

#[derive(Clone, Debug)]
pub struct VaultAad {
    pub envelope_id: String,
    pub ruleset_hash: [u8; 32],
}

pub fn encode_aad(envelope_id: &str, ruleset_hash: &[u8; 32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(envelope_id.len() + ruleset_hash.len() + 4);
    out.extend_from_slice(&(envelope_id.len() as u32).to_le_bytes());
    out.extend_from_slice(envelope_id.as_bytes());
    out.extend_from_slice(ruleset_hash);
    out
}

pub fn decode_aad(aad: &[u8]) -> Result<VaultAad> {
    let mut cursor = 0usize;
    let id_len = read_u32(aad, &mut cursor)? as usize;
    let id_bytes = read_slice(aad, &mut cursor, id_len)?;
    let envelope_id = std::str::from_utf8(id_bytes)
        .map_err(|_| anyhow!("invalid vault aad encoding"))?
        .to_string();
    let ruleset_bytes = read_slice(aad, &mut cursor, 32)?;
    let mut ruleset_hash = [0u8; 32];
    ruleset_hash.copy_from_slice(ruleset_bytes);
    Ok(VaultAad {
        envelope_id,
        ruleset_hash,
    })
}

pub fn seal_v1(
    envelope_id: &str,
    ruleset_hash: [u8; 32],
    clear: &[u8],
    master_key: &[u8; 32],
) -> Result<EnvelopeV1> {
    let mut nonce = [0u8; 12];
    SysRng
        .try_fill_bytes(&mut nonce[..])
        .expect("OS RNG unavailable");
    let mut ciphertext = clear.to_vec();
    let tag = encrypt_in_place(
        master_key,
        envelope_id,
        &ruleset_hash,
        &nonce,
        &mut ciphertext,
    )?;

    Ok(EnvelopeV1::new(
        envelope_id.to_string(),
        ruleset_hash,
        nonce,
        tag,
        ciphertext,
    ))
}

pub fn decrypt_v1(envelope: &EnvelopeV1, master_key: &[u8; 32]) -> Result<Vec<u8>> {
    let mut clear = envelope.ciphertext.clone();
    decrypt_in_place(
        master_key,
        &envelope.envelope_id,
        &envelope.ruleset_hash,
        &envelope.nonce,
        &envelope.tag,
        &mut clear,
    )?;
    Ok(clear)
}

/// RAII guard to ensure DEK is zeroized on drop, including error paths.
struct DekGuard([u8; 32]);

impl Drop for DekGuard {
    fn drop(&mut self) {
        self.0.zeroize();
    }
}

pub fn seal_v2(
    envelope_id: &str,
    ruleset_hash: [u8; 32],
    clear: &[u8],
    mode: VaultCryptoMode,
    master_key: &[u8; 32],
    kem_keypair: Option<&KemKeypair>,
) -> Result<EnvelopeV2> {
    let aad = encode_aad(envelope_id, &ruleset_hash);
    let mut derived = derive_dek(envelope_id, &ruleset_hash, mode, master_key, kem_keypair)?;

    // Wrap DEK in guard to ensure zeroization on all paths including errors.
    let dek_guard = DekGuard(derived.dek);
    // The guard now owns the working copy; scrub the source copy that still
    // lives in `derived.dek` (a plain [u8;32] has no Drop, so it would
    // otherwise linger on the stack un-zeroized).
    derived.dek.zeroize();

    let mut nonce = vec![0u8; 12];
    SysRng
        .try_fill_bytes(&mut nonce[..])
        .expect("OS RNG unavailable");
    let mut ciphertext = clear.to_vec();
    let tag = encrypt_payload(&dek_guard.0, &nonce, &aad, &mut ciphertext)?;

    let mut final_ciphertext = ciphertext;
    final_ciphertext.extend_from_slice(&tag);

    // dek_guard will be dropped and zeroized here

    Ok(EnvelopeV2 {
        version: 2,
        aead_alg: AEAD_ALG_CHACHA20POLY1305.to_string(),
        nonce,
        aad,
        ciphertext: final_ciphertext,
        kem_alg: derived.kem_alg,
        kem_ct: derived.kem_ct,
        kdf_info: derived.kdf_info,
        classical_wrap: derived.classical_wrap,
    })
}

pub fn decrypt_v2(
    envelope: &EnvelopeV2,
    master_key: &[u8; 32],
    kem_keypair: Option<&KemKeypair>,
) -> Result<Vec<u8>> {
    let aad = envelope.aad.clone();

    if envelope.ciphertext.len() < 16 {
        return Err(anyhow!("vault ciphertext truncated"));
    }
    let tag_offset = envelope.ciphertext.len() - 16;
    let tag = &envelope.ciphertext[tag_offset..];

    // Try KEM-based decryption first. ML-KEM-768 uses implicit rejection
    // (FO transform), so decapsulating a legacy Kyber768 ciphertext returns
    // a wrong shared secret instead of an error. We detect this at the AEAD
    // level: if the derived DEK produces an auth tag mismatch, fall back to
    // classical_wrap.
    if envelope.kem_alg == KEM_ALG_ML_KEM_768 {
        if let Some(kp) = kem_keypair {
            if let Ok(mut shared_secret) = kem_decapsulate(kp, &envelope.kem_ct) {
                let dek_guard = DekGuard(kdf_dek(&shared_secret, &envelope.kdf_info));
                // Spent — scrub the decapsulated secret before it drops.
                shared_secret.zeroize();
                let mut ciphertext = envelope.ciphertext[..tag_offset].to_vec();
                if decrypt_payload(&dek_guard.0, &envelope.nonce, &aad, &mut ciphertext, tag)
                    .is_ok()
                {
                    return Ok(ciphertext);
                }
            }
        }
    }

    // Fallback: classical key wrap (handles legacy Kyber768 hybrid envelopes
    // and classical-only envelopes).
    if let Some(wrap) = &envelope.classical_wrap {
        let dek_guard = DekGuard(unwrap_dek(master_key, &envelope.aad, wrap)?);
        let mut ciphertext = envelope.ciphertext[..tag_offset].to_vec();
        decrypt_payload(&dek_guard.0, &envelope.nonce, &aad, &mut ciphertext, tag)?;
        return Ok(ciphertext);
    }

    Err(anyhow!("vault envelope decryption failed"))
}

fn encrypt_payload(
    dek: &[u8; 32],
    nonce: &[u8],
    aad: &[u8],
    buffer: &mut [u8],
) -> Result<[u8; 16]> {
    let nonce = Nonce::try_from(nonce).map_err(|_| anyhow!("vault nonce length mismatch"))?;
    let cipher = ChaCha20Poly1305::new(dek.into());
    let tag = cipher
        .encrypt_inout_detached(&nonce, aad, buffer.into())
        .map_err(|_| anyhow!("vault encryption failed"))?;
    Ok(tag.into())
}

fn decrypt_payload(
    dek: &[u8; 32],
    nonce: &[u8],
    aad: &[u8],
    buffer: &mut [u8],
    tag: &[u8],
) -> Result<()> {
    let nonce = Nonce::try_from(nonce).map_err(|_| anyhow!("vault nonce length mismatch"))?;
    let tag = Tag::try_from(tag).map_err(|_| anyhow!("vault tag length mismatch"))?;
    let cipher = ChaCha20Poly1305::new(dek.into());
    cipher
        .decrypt_inout_detached(&nonce, aad, buffer.into(), &tag)
        .map_err(|_| anyhow!("vault decryption failed"))?;
    Ok(())
}

struct DerivedDek {
    dek: [u8; 32],
    kem_alg: String,
    kem_ct: Vec<u8>,
    kdf_info: Vec<u8>,
    classical_wrap: Option<Vec<u8>>,
}

fn derive_dek(
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    mode: VaultCryptoMode,
    master_key: &[u8; 32],
    kem_keypair: Option<&KemKeypair>,
) -> Result<DerivedDek> {
    match mode {
        VaultCryptoMode::Classical => {
            let dek = random_dek();
            let wrap = wrap_dek(master_key, envelope_id, ruleset_hash, &dek)?;
            Ok(DerivedDek {
                dek,
                kem_alg: KEM_ALG_NONE.to_string(),
                kem_ct: Vec::new(),
                kdf_info: Vec::new(),
                classical_wrap: Some(wrap),
            })
        }
        VaultCryptoMode::Pq | VaultCryptoMode::Hybrid => {
            let kem_keypair =
                kem_keypair.ok_or_else(|| anyhow!("vault KEM keypair missing for PQ mode"))?;
            let (kem_ct, mut shared_secret) = kem_encapsulate(kem_keypair)?;
            let mut kdf_info = vec![0u8; 32];
            SysRng
                .try_fill_bytes(&mut kdf_info[..])
                .expect("OS RNG unavailable");
            let dek = kdf_dek(&shared_secret, &kdf_info);
            // The KEM shared secret is spent; scrub it so the raw secret does
            // not linger in the heap allocation after the DEK is derived.
            shared_secret.zeroize();
            let classical_wrap = if matches!(mode, VaultCryptoMode::Hybrid) {
                Some(wrap_dek(master_key, envelope_id, ruleset_hash, &dek)?)
            } else {
                None
            };
            Ok(DerivedDek {
                dek,
                kem_alg: KEM_ALG_ML_KEM_768.to_string(),
                kem_ct,
                kdf_info,
                classical_wrap,
            })
        }
    }
}

fn random_dek() -> [u8; 32] {
    let mut dek = [0u8; 32];
    SysRng
        .try_fill_bytes(&mut dek[..])
        .expect("OS RNG unavailable");
    dek
}

fn kdf_dek(shared_secret: &[u8], kdf_info: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(shared_secret);
    hasher.update(kdf_info);
    hasher.finalize().into()
}

fn wrap_dek(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    dek: &[u8; 32],
) -> Result<Vec<u8>> {
    let mut nonce = [0u8; 12];
    SysRng
        .try_fill_bytes(&mut nonce[..])
        .expect("OS RNG unavailable");
    let mut ciphertext = dek.to_vec();
    let aad = wrap_aad(envelope_id, ruleset_hash);
    let tag = encrypt_payload(master_key, &nonce, &aad, &mut ciphertext)?;
    let mut out = Vec::with_capacity(12 + 16 + ciphertext.len());
    out.extend_from_slice(&nonce);
    out.extend_from_slice(&tag);
    out.extend_from_slice(&ciphertext);
    Ok(out)
}

fn unwrap_dek(master_key: &[u8; 32], aad: &[u8], wrap: &[u8]) -> Result<[u8; 32]> {
    // Wrap format: 12-byte nonce + 16-byte tag + 32-byte encrypted DEK = 60 bytes
    const WRAP_LEN: usize = 12 + 16 + 32;
    if wrap.len() != WRAP_LEN {
        return Err(anyhow!(
            "vault classical wrap invalid length: expected {} bytes, got {}",
            WRAP_LEN,
            wrap.len()
        ));
    }
    let nonce = &wrap[..12];
    let tag = &wrap[12..28];
    let mut ciphertext = wrap[28..].to_vec();
    let wrap_aad = wrap_aad_from_aad(aad);
    decrypt_payload(master_key, nonce, &wrap_aad, &mut ciphertext, tag)?;
    let mut dek = [0u8; 32];
    dek.copy_from_slice(&ciphertext);
    Ok(dek)
}

fn wrap_aad(envelope_id: &str, ruleset_hash: &[u8; 32]) -> Vec<u8> {
    let mut aad = Vec::with_capacity(envelope_id.len() + ruleset_hash.len() + 12);
    aad.extend_from_slice(b"vault-dek");
    aad.extend_from_slice(&encode_aad(envelope_id, ruleset_hash));
    aad
}

fn wrap_aad_from_aad(aad: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(aad.len() + 8);
    out.extend_from_slice(b"vault-dek");
    out.extend_from_slice(aad);
    out
}

fn encrypt_in_place(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    nonce: &[u8; 12],
    buffer: &mut [u8],
) -> Result<[u8; 16]> {
    let cipher = ChaCha20Poly1305::new(master_key.into());
    // V1 AAD format: envelope_id || ruleset_hash (no length prefix for backward compatibility)
    let mut aad = Vec::with_capacity(envelope_id.len() + ruleset_hash.len());
    aad.extend_from_slice(envelope_id.as_bytes());
    aad.extend_from_slice(ruleset_hash);
    let tag = cipher
        .encrypt_inout_detached(nonce.into(), &aad, buffer.into())
        .map_err(|_| anyhow!("vault encryption failed"))?;
    Ok(tag.into())
}

fn decrypt_in_place(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    nonce: &[u8; 12],
    tag: &[u8; 16],
    buffer: &mut [u8],
) -> Result<()> {
    let cipher = ChaCha20Poly1305::new(master_key.into());
    // V1 AAD format: envelope_id || ruleset_hash (no length prefix for backward compatibility)
    let mut aad = Vec::with_capacity(envelope_id.len() + ruleset_hash.len());
    aad.extend_from_slice(envelope_id.as_bytes());
    aad.extend_from_slice(ruleset_hash);
    cipher
        .decrypt_inout_detached(nonce.into(), &aad, buffer.into(), tag.into())
        .map_err(|_| anyhow!("vault decryption failed"))?;
    Ok(())
}

#[derive(Clone, Debug)]
pub struct KemKeypair {
    #[cfg(feature = "pqc-vault")]
    pub public: pqcrypto_mlkem::mlkem768::PublicKey,
    #[cfg(feature = "pqc-vault")]
    pub secret: pqcrypto_mlkem::mlkem768::SecretKey,
    #[cfg(feature = "pqc-vault")]
    secret_bytes: zeroize::Zeroizing<Vec<u8>>,
}

#[cfg(feature = "pqc-vault")]
impl KemKeypair {
    pub fn generate() -> Self {
        use pqcrypto_traits::kem::SecretKey as _;
        let (public, secret) = pqcrypto_mlkem::mlkem768::keypair();
        let secret_bytes = zeroize::Zeroizing::new(secret.as_bytes().to_vec());
        Self {
            public,
            secret,
            secret_bytes,
        }
    }

    pub fn public_bytes(&self) -> Vec<u8> {
        use pqcrypto_traits::kem::PublicKey;
        self.public.as_bytes().to_vec()
    }

    pub fn secret_bytes(&self) -> Vec<u8> {
        self.secret_bytes.to_vec()
    }

    pub fn from_bytes(public: &[u8], secret: &[u8]) -> Result<Self> {
        use pqcrypto_traits::kem::{PublicKey as _, SecretKey as _};
        let public = pqcrypto_mlkem::mlkem768::PublicKey::from_bytes(public)
            .map_err(|_| anyhow!("invalid KEM public key"))?;
        let secret = pqcrypto_mlkem::mlkem768::SecretKey::from_bytes(secret)
            .map_err(|_| anyhow!("invalid KEM secret key"))?;
        let secret_bytes = zeroize::Zeroizing::new(secret.as_bytes().to_vec());
        Ok(Self {
            public,
            secret,
            secret_bytes,
        })
    }
}

#[cfg(not(feature = "pqc-vault"))]
impl KemKeypair {
    pub fn generate() -> Self {
        Self {}
    }

    pub fn public_bytes(&self) -> Vec<u8> {
        Vec::new()
    }

    pub fn secret_bytes(&self) -> Vec<u8> {
        Vec::new()
    }

    pub fn from_bytes(_public: &[u8], _secret: &[u8]) -> Result<Self> {
        Err(anyhow!("pqc-vault feature not enabled"))
    }
}

fn kem_encapsulate(kem: &KemKeypair) -> Result<(Vec<u8>, Vec<u8>)> {
    #[cfg(feature = "pqc-vault")]
    {
        use pqcrypto_traits::kem::{Ciphertext, SharedSecret};
        let (shared, ct) = pqcrypto_mlkem::mlkem768::encapsulate(&kem.public);
        return Ok((ct.as_bytes().to_vec(), shared.as_bytes().to_vec()));
    }
    #[cfg(not(feature = "pqc-vault"))]
    {
        let _ = kem;
        Err(anyhow!("pqc-vault feature not enabled"))
    }
}

fn kem_decapsulate(kem: &KemKeypair, kem_ct: &[u8]) -> Result<Vec<u8>> {
    #[cfg(feature = "pqc-vault")]
    {
        use pqcrypto_traits::kem::{Ciphertext, SharedSecret};
        let ct = pqcrypto_mlkem::mlkem768::Ciphertext::from_bytes(kem_ct)
            .map_err(|_| anyhow!("invalid KEM ciphertext"))?;
        let shared = pqcrypto_mlkem::mlkem768::decapsulate(&ct, &kem.secret);
        return Ok(shared.as_bytes().to_vec());
    }
    #[cfg(not(feature = "pqc-vault"))]
    {
        let _ = (kem, kem_ct);
        Err(anyhow!("pqc-vault feature not enabled"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Known-answer test pinning the exact on-disk AEAD bytes to RFC 8439.
    /// Goldens were generated with an INDEPENDENT implementation (Python
    /// `cryptography`'s ChaCha20Poly1305), so this locks the wire format
    /// across chacha20poly1305 crate upgrades: an already-sealed envelope
    /// must keep decrypting byte-for-byte identically after any bump. If
    /// this test ever fails after a dependency change, sealed evidence on
    /// disk would no longer open — do not "fix" the goldens.
    #[test]
    fn aead_known_answer_rfc8439() {
        let key: [u8; 32] = std::array::from_fn(|i| i as u8);
        let nonce: [u8; 12] = std::array::from_fn(|i| i as u8);
        let clear = b"securacv vault known-answer test";
        let ruleset = [0xAAu8; 32];

        const GOLDEN_CT: &str = "fa9e6b755b76c63697f55e86f4692e08a71fc5897c15c3ca91f25de501a5d548";
        const GOLDEN_TAG_V2: &str = "32c1c9adf7f92ccf7e8a00f47288b92c";
        const GOLDEN_TAG_V1: &str = "df41bbadcff6d7e7c1e9283ca8f1923f";

        // V2 path (length-prefixed AAD via encode_aad).
        let aad = encode_aad("kat-envelope", &ruleset);
        let mut buf = clear.to_vec();
        let tag = encrypt_payload(&key, &nonce, &aad, &mut buf).unwrap();
        assert_eq!(hex::encode(&buf), GOLDEN_CT);
        assert_eq!(hex::encode(tag), GOLDEN_TAG_V2);
        // ...and the pinned bytes decrypt back (a 0.10.1-sealed envelope
        // must open on any future crate version).
        let mut ct = hex::decode(GOLDEN_CT).unwrap();
        let tag_bytes = hex::decode(GOLDEN_TAG_V2).unwrap();
        decrypt_payload(&key, &nonce, &aad, &mut ct, &tag_bytes).unwrap();
        assert_eq!(ct, clear);

        // V1 path (raw-concatenation AAD, encrypt_in_place/decrypt_in_place).
        let mut buf = clear.to_vec();
        let tag = encrypt_in_place(&key, "kat-envelope", &ruleset, &nonce, &mut buf).unwrap();
        assert_eq!(hex::encode(&buf), GOLDEN_CT);
        assert_eq!(hex::encode(tag), GOLDEN_TAG_V1);
        let mut ct = hex::decode(GOLDEN_CT).unwrap();
        let mut tag_arr = [0u8; 16];
        tag_arr.copy_from_slice(&hex::decode(GOLDEN_TAG_V1).unwrap());
        decrypt_in_place(&key, "kat-envelope", &ruleset, &nonce, &tag_arr, &mut ct).unwrap();
        assert_eq!(ct, clear);
    }

    #[test]
    fn aad_roundtrip() {
        let envelope_id = "incident-42";
        let ruleset_hash = [7u8; 32];
        let encoded = encode_aad(envelope_id, &ruleset_hash);
        let decoded = decode_aad(&encoded).unwrap();
        assert_eq!(decoded.envelope_id, envelope_id);
        assert_eq!(decoded.ruleset_hash, ruleset_hash);
    }

    #[test]
    fn aad_rejects_truncated_input() {
        assert!(decode_aad(&[0, 0, 0]).is_err());
        assert!(decode_aad(&[]).is_err());
    }

    #[test]
    fn seal_v1_roundtrip() {
        let key = [42u8; 32];
        let clear = b"privacy-preserving payload";
        let envelope = seal_v1("test-env", [1u8; 32], clear, &key).unwrap();
        let decrypted = decrypt_v1(&envelope, &key).unwrap();
        assert_eq!(decrypted, clear);
    }

    #[test]
    fn seal_v1_wrong_key_fails() {
        let key = [42u8; 32];
        let envelope = seal_v1("test-env", [1u8; 32], b"secret", &key).unwrap();
        let wrong_key = [99u8; 32];
        assert!(decrypt_v1(&envelope, &wrong_key).is_err());
    }

    #[test]
    fn seal_v2_classical_roundtrip() {
        let key = [42u8; 32];
        let clear = b"v2 payload";
        let envelope = seal_v2(
            "test-v2",
            [2u8; 32],
            clear,
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        let decrypted = decrypt_v2(&envelope, &key, None).unwrap();
        assert_eq!(decrypted, clear);
    }

    #[test]
    fn seal_v2_wrong_key_fails() {
        let key = [42u8; 32];
        let envelope = seal_v2(
            "test-v2",
            [2u8; 32],
            b"secret",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        assert!(decrypt_v2(&envelope, &[99u8; 32], None).is_err());
    }

    #[test]
    fn seal_v2_tampered_ciphertext_fails() {
        let key = [42u8; 32];
        let mut envelope = seal_v2(
            "test-v2",
            [2u8; 32],
            b"secret",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        envelope.ciphertext[0] ^= 0xFF;
        assert!(decrypt_v2(&envelope, &key, None).is_err());
    }

    #[test]
    fn seal_v2_different_envelopes_produce_different_nonces() {
        let key = [42u8; 32];
        let e1 = seal_v2(
            "env-1",
            [1u8; 32],
            b"data",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        let e2 = seal_v2(
            "env-2",
            [2u8; 32],
            b"data",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        assert_ne!(e1.nonce, e2.nonce);
    }

    #[test]
    fn wrap_unwrap_dek_roundtrip() {
        let master_key = [11u8; 32];
        let dek = [22u8; 32];
        let aad = encode_aad("wrap-test", &[3u8; 32]);
        let wrapped = wrap_dek(&master_key, "wrap-test", &[3u8; 32], &dek).unwrap();
        let unwrapped = unwrap_dek(&master_key, &aad, &wrapped).unwrap();
        assert_eq!(unwrapped, dek);
    }

    #[test]
    fn wrap_dek_wrong_key_fails() {
        let master_key = [11u8; 32];
        let dek = [22u8; 32];
        let aad = encode_aad("wrap-test", &[3u8; 32]);
        let wrapped = wrap_dek(&master_key, "wrap-test", &[3u8; 32], &dek).unwrap();
        assert!(unwrap_dek(&[99u8; 32], &aad, &wrapped).is_err());
    }

    #[test]
    fn kdf_dek_deterministic() {
        let secret = [5u8; 32];
        let info = b"test-context";
        let d1 = kdf_dek(&secret, info);
        let d2 = kdf_dek(&secret, info);
        assert_eq!(d1, d2);

        let d3 = kdf_dek(&secret, b"different-context");
        assert_ne!(d1, d3);
    }

    #[test]
    fn decrypt_v2_rejects_truncated_ciphertext() {
        let key = [42u8; 32];
        let mut envelope = seal_v2(
            "trunc",
            [1u8; 32],
            b"data",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        envelope.ciphertext = vec![0u8; 10]; // less than 16 (tag size)
        assert!(decrypt_v2(&envelope, &key, None).is_err());
    }

    #[test]
    #[cfg(feature = "pqc-vault")]
    fn seal_v2_pq_and_hybrid_roundtrip() {
        let key = [42u8; 32];
        let clear = b"pq payload";
        let kp = KemKeypair::generate();

        let envelope_pq = seal_v2(
            "test-pq",
            [3u8; 32],
            clear,
            VaultCryptoMode::Pq,
            &key,
            Some(&kp),
        )
        .unwrap();
        let decrypted_pq = decrypt_v2(&envelope_pq, &key, Some(&kp)).unwrap();
        assert_eq!(decrypted_pq, clear);

        let envelope_hybrid = seal_v2(
            "test-hybrid",
            [4u8; 32],
            clear,
            VaultCryptoMode::Hybrid,
            &key,
            Some(&kp),
        )
        .unwrap();
        let decrypted_hybrid = decrypt_v2(&envelope_hybrid, &key, Some(&kp)).unwrap();
        assert_eq!(decrypted_hybrid, clear);
    }
}
