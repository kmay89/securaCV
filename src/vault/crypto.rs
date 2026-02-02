use anyhow::{anyhow, Result};
use chacha20poly1305::{
    aead::{AeadInPlace, KeyInit},
    ChaCha20Poly1305, Key, Nonce, Tag,
};
use rand::RngCore;
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
    rand::thread_rng().fill_bytes(&mut nonce);
    let mut ciphertext = clear.to_vec();
    let tag = encrypt_in_place(master_key, envelope_id, &ruleset_hash, &nonce, &mut ciphertext)?;

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

pub fn seal_v2(
    envelope_id: &str,
    ruleset_hash: [u8; 32],
    clear: &[u8],
    mode: VaultCryptoMode,
    master_key: &[u8; 32],
    kem_keypair: Option<&KemKeypair>,
) -> Result<EnvelopeV2> {
    let aad = encode_aad(envelope_id, &ruleset_hash);
    let derived = derive_dek(
        envelope_id,
        &ruleset_hash,
        mode,
        master_key,
        kem_keypair,
    )?;

    let mut nonce = vec![0u8; 12];
    rand::thread_rng().fill_bytes(&mut nonce);
    let mut ciphertext = clear.to_vec();
    let tag = encrypt_payload(&derived.dek, &nonce, &aad, &mut ciphertext)?;

    let mut final_ciphertext = ciphertext;
    final_ciphertext.extend_from_slice(&tag);

    let mut dek = derived.dek;
    dek.zeroize();

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
    let dek = recover_dek(envelope, master_key, kem_keypair)?;
    if envelope.ciphertext.len() < 16 {
        return Err(anyhow!("vault ciphertext truncated"));
    }
    let tag_offset = envelope.ciphertext.len() - 16;
    let mut ciphertext = envelope.ciphertext[..tag_offset].to_vec();
    let tag = &envelope.ciphertext[tag_offset..];

    decrypt_payload(&dek, &envelope.nonce, &aad, &mut ciphertext, tag)?;

    let mut dek = dek;
    dek.zeroize();

    Ok(ciphertext)
}

fn encrypt_payload(
    dek: &[u8; 32],
    nonce: &[u8],
    aad: &[u8],
    buffer: &mut [u8],
) -> Result<[u8; 16]> {
    if nonce.len() != 12 {
        return Err(anyhow!("vault nonce length mismatch"));
    }
    let cipher = ChaCha20Poly1305::new(Key::from_slice(dek));
    let tag = cipher
        .encrypt_in_place_detached(Nonce::from_slice(nonce), aad, buffer)
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
    if nonce.len() != 12 {
        return Err(anyhow!("vault nonce length mismatch"));
    }
    if tag.len() != 16 {
        return Err(anyhow!("vault tag length mismatch"));
    }
    let cipher = ChaCha20Poly1305::new(Key::from_slice(dek));
    let tag = Tag::from_slice(tag);
    cipher
        .decrypt_in_place_detached(Nonce::from_slice(nonce), aad, buffer, tag)
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
            let kem_keypair = kem_keypair
                .ok_or_else(|| anyhow!("vault KEM keypair missing for PQ mode"))?;
            let (kem_ct, shared_secret) = kem_encapsulate(kem_keypair)?;
            let mut kdf_info = vec![0u8; 32];
            rand::thread_rng().fill_bytes(&mut kdf_info);
            let dek = kdf_dek(&shared_secret, &kdf_info);
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

fn recover_dek(
    envelope: &EnvelopeV2,
    master_key: &[u8; 32],
    kem_keypair: Option<&KemKeypair>,
) -> Result<[u8; 32]> {
    if envelope.kem_alg == KEM_ALG_ML_KEM_768 {
        let kem_keypair = kem_keypair
            .ok_or_else(|| anyhow!("vault KEM keypair missing for PQ envelope"))?;
        let shared_secret = kem_decapsulate(kem_keypair, &envelope.kem_ct)?;
        return Ok(kdf_dek(&shared_secret, &envelope.kdf_info));
    }

    if let Some(wrap) = &envelope.classical_wrap {
        return unwrap_dek(master_key, &envelope.aad, wrap);
    }

    Err(anyhow!("vault envelope lacks usable key wrap"))
}

fn random_dek() -> [u8; 32] {
    let mut dek = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut dek);
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
    rand::thread_rng().fill_bytes(&mut nonce);
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
    if wrap.len() < 12 + 16 + 32 {
        return Err(anyhow!("vault classical wrap truncated"));
    }
    let nonce = &wrap[..12];
    let tag = &wrap[12..28];
    let mut ciphertext = wrap[28..].to_vec();
    let wrap_aad = wrap_aad_from_aad(aad);
    decrypt_payload(master_key, nonce, &wrap_aad, &mut ciphertext, tag)?;
    if ciphertext.len() != 32 {
        return Err(anyhow!("vault DEK length mismatch"));
    }
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
    let cipher = ChaCha20Poly1305::new(Key::from_slice(master_key));
    let aad = encode_aad(envelope_id, ruleset_hash);
    let tag = cipher
        .encrypt_in_place_detached(Nonce::from_slice(nonce), &aad, buffer)
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
    let cipher = ChaCha20Poly1305::new(Key::from_slice(master_key));
    let aad = encode_aad(envelope_id, ruleset_hash);
    let tag = Tag::from_slice(tag);
    cipher
        .decrypt_in_place_detached(Nonce::from_slice(nonce), &aad, buffer, tag)
        .map_err(|_| anyhow!("vault decryption failed"))?;
    Ok(())
}

#[derive(Clone, Debug)]
pub struct KemKeypair {
    #[cfg(feature = "pqc-vault")]
    pub public: pqcrypto_kyber::kyber768::PublicKey,
    #[cfg(feature = "pqc-vault")]
    pub secret: pqcrypto_kyber::kyber768::SecretKey,
}

#[cfg(feature = "pqc-vault")]
impl KemKeypair {
    pub fn generate() -> Self {
        let (public, secret) = pqcrypto_kyber::kyber768::keypair();
        Self { public, secret }
    }

    pub fn public_bytes(&self) -> Vec<u8> {
        self.public.as_bytes().to_vec()
    }

    pub fn secret_bytes(&self) -> Vec<u8> {
        self.secret.as_bytes().to_vec()
    }

    pub fn from_bytes(public: &[u8], secret: &[u8]) -> Result<Self> {
        let public = pqcrypto_kyber::kyber768::PublicKey::from_bytes(public)
            .map_err(|_| anyhow!("invalid KEM public key"))?;
        let secret = pqcrypto_kyber::kyber768::SecretKey::from_bytes(secret)
            .map_err(|_| anyhow!("invalid KEM secret key"))?;
        Ok(Self { public, secret })
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
        use pqcrypto_traits::kem::{Ciphertext, PublicKey, SharedSecret};
        let (shared, ct) = pqcrypto_kyber::kyber768::encapsulate(&kem.public);
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
        let ct = pqcrypto_kyber::kyber768::Ciphertext::from_bytes(kem_ct)
            .map_err(|_| anyhow!("invalid KEM ciphertext"))?;
        let shared = pqcrypto_kyber::kyber768::decapsulate(&ct, &kem.secret);
        return Ok(shared.as_bytes().to_vec());
    }
    #[cfg(not(feature = "pqc-vault"))]
    {
        let _ = (kem, kem_ct);
        Err(anyhow!("pqc-vault feature not enabled"))
    }
}
