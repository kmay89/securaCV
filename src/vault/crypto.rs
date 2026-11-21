use anyhow::{anyhow, Result};
use chacha20poly1305::{
    aead::{AeadInOut, KeyInit},
    ChaCha20Poly1305, Nonce, Tag,
};
#[cfg(feature = "pqc-vault")]
use ml_kem::{
    Decapsulate, DecapsulationKey768, Encapsulate, EncapsulationKey768, ExpandedDecapsulationKey,
    Generate, KeyExport, MlKem768, TryKeyInit,
};
use rand::rngs::SysRng;
use rand::TryRng;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use zeroize::Zeroize;

use crate::vault::format::{read_slice, read_u32, EnvelopeV1, EnvelopeV2};

pub const AEAD_ALG_CHACHA20POLY1305: &str = "chacha20poly1305";
pub const KEM_ALG_ML_KEM_768: &str = "ml-kem-768";
/// A true hybrid: the DEK is derived from the ML-KEM shared secret AND a
/// classical secret wrapped under the master key, so opening the envelope
/// needs both. Envelopes stamped the bare `ml-kem-768` with a
/// `classical_wrap` are the earlier shape, where either secret alone
/// opened the envelope (an OR, no stronger than its weaker half); they
/// still decrypt, and are never written again.
pub const KEM_ALG_HYBRID_ML_KEM_768: &str = "hybrid-ml-kem-768";
pub const KEM_ALG_NONE: &str = "none";

/// Domain tag of the hybrid DEK derivation.
const HYBRID_DEK_TAG: &[u8] = b"securacv:vault:hybrid-dek:v1";

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
    let nonce = fresh_nonce();
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

    let nonce = fresh_nonce().to_vec();
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

    // The header names the algorithms but is outside the AAD; refuse any
    // other spelling before a byte is decrypted, so a future second AEAD or
    // KEM is never selectable by an at-rest edit of the header.
    if envelope.aead_alg != AEAD_ALG_CHACHA20POLY1305 {
        return Err(anyhow!(
            "vault envelope names AEAD {:?}; only {AEAD_ALG_CHACHA20POLY1305} is supported",
            envelope.aead_alg
        ));
    }
    if !matches!(
        envelope.kem_alg.as_str(),
        KEM_ALG_NONE | KEM_ALG_ML_KEM_768 | KEM_ALG_HYBRID_ML_KEM_768
    ) {
        return Err(anyhow!(
            "vault envelope names KEM {:?}; not a supported scheme",
            envelope.kem_alg
        ));
    }

    if envelope.ciphertext.len() < 16 {
        return Err(anyhow!("vault ciphertext truncated"));
    }
    let tag_offset = envelope.ciphertext.len() - 16;
    let tag = &envelope.ciphertext[tag_offset..];

    // A hybrid envelope opens with BOTH secrets or not at all: the KEM
    // shared secret and the master-key-wrapped classical secret feed one
    // derivation, and there is no fallback to either half.
    if envelope.kem_alg == KEM_ALG_HYBRID_ML_KEM_768 {
        let kp =
            kem_keypair.ok_or_else(|| anyhow!("vault KEM keypair missing for hybrid envelope"))?;
        let wrap = envelope
            .classical_wrap
            .as_ref()
            .ok_or_else(|| anyhow!("vault hybrid envelope carries no classical wrap"))?;
        let mut shared_secret = kem_decapsulate(kp, &envelope.kem_ct)?;
        let mut classical_secret = unwrap_dek(master_key, &envelope.aad, wrap)?;
        let dek_guard = DekGuard(kdf_dek_hybrid(
            &shared_secret,
            &classical_secret,
            &envelope.kdf_info,
        ));
        shared_secret.zeroize();
        classical_secret.zeroize();
        let mut ciphertext = envelope.ciphertext[..tag_offset].to_vec();
        decrypt_payload(&dek_guard.0, &envelope.nonce, &aad, &mut ciphertext, tag)?;
        return Ok(ciphertext);
    }

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

// --------------------------------------------------------------------------
// Master-key keyguard (MKG1): protect the vault master key at rest under an
// Argon2id passphrase KEK, so possession of the vault directory alone is no
// longer sufficient to decrypt evidence. This wraps ONLY how the master key
// reaches memory — the envelope crypto above is untouched, so every sealed
// v1/v2 envelope stays byte-identical.
// --------------------------------------------------------------------------

/// One Argon2id passphrase container recipe in the MKG1 byte layout:
///
/// ```text
/// magic[4] ‖ version 0x01 ‖ kind 0x01 (Argon2id)
/// ‖ m u32le ‖ t u32le ‖ p u32le      (65536 KiB / 3 / 1, refused before the KDF otherwise)
/// ‖ salt_len u8 (16) ‖ salt ‖ kek_commit[32] = SHA-256(KEK)
/// ‖ nonce[12] ‖ ChaCha20-Poly1305(KEK, nonce, aad_domain ‖ aad_suffix, secret[32]) ‖ tag[16]
/// ```
///
/// A format is the pair (magic, AAD domain), plus the label its errors
/// carry and whether its reader is exact. MKG1 (the vault master key) and
/// TKG1 (a trustee signing seed) are the two instances; a later container
/// that wraps a different 32-byte secret adds a third, never a new layout.
pub(crate) struct KeyguardFormat {
    /// The four magic bytes that open the container.
    pub(crate) magic: &'static [u8; 4],
    /// Domain bytes that open the AEAD associated data; the caller's
    /// `aad_suffix` follows them.
    pub(crate) aad_domain: &'static [u8],
    /// The prefix of every error this format's reader returns.
    pub(crate) label: &'static str,
    /// An exact reader refuses a salt length other than 16 and any byte
    /// after the tag. MKG1 is read leniently, exactly as before this
    /// recipe was shared, so no existing keyguard changes verdict.
    pub(crate) exact: bool,
    /// The error returned when the AEAD refuses (a tampered byte, or a
    /// container bound to another AAD suffix).
    pub(crate) aead_failure: &'static str,
}

/// The vault master-key keyguard (`master.keyguard`).
pub(crate) const KEYGUARD_MKG1: KeyguardFormat = KeyguardFormat {
    magic: b"MKG1",
    aad_domain: b"securacv-vault-keyguard-v1",
    label: "vault keyguard",
    exact: false,
    aead_failure: "vault decryption failed",
};

/// A trustee's Ed25519 signing seed at rest (`.tkey`): the MKG1 layout under
/// its own magic and AAD domain. The AAD suffix is
/// `u32le(len id) ‖ id ‖ public_key[32]`, so a renamed trustee or a swapped
/// public key fails the AEAD (`break_glass::trustee_key`).
pub(crate) const KEYGUARD_TKG1: KeyguardFormat = KeyguardFormat {
    magic: b"TKG1",
    aad_domain: b"securacv-trustee-key-v1",
    label: "trustee key",
    exact: true,
    aead_failure: "trustee key: the file was altered, or its trustee id or public key was changed",
};

const KEYGUARD_KIND_ARGON2ID: u8 = 1;
/// Argon2id memory, KiB (64 MiB, interactive strength). Pinned: a reader
/// refuses any other value before deriving.
pub(crate) const KEYGUARD_M_KIB: u32 = 65536;
/// Argon2id passes. Pinned.
pub(crate) const KEYGUARD_T: u32 = 3;
/// Argon2id lanes. Pinned.
pub(crate) const KEYGUARD_P: u32 = 1;
/// Salt length written by every encoder.
pub(crate) const KEYGUARD_SALT_LEN: usize = 16;

fn read_u8b(bytes: &[u8], cursor: &mut usize, label: &str) -> Result<u8> {
    let v = *bytes
        .get(*cursor)
        .ok_or_else(|| anyhow!("{label}: truncated"))?;
    *cursor += 1;
    Ok(v)
}

fn keyguard_aad(format: &KeyguardFormat, aad_suffix: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(format.aad_domain.len() + aad_suffix.len());
    v.extend_from_slice(format.aad_domain);
    v.extend_from_slice(aad_suffix);
    v
}

/// True when (m, t, p) are the pinned keyguard parameters. Every reader
/// checks this BEFORE [`keyguard_kek`]: a writer of the file could
/// otherwise set a huge m and exhaust memory on the next open.
pub(crate) fn keyguard_params_pinned(m: u32, t: u32, p: u32) -> bool {
    m == KEYGUARD_M_KIB && t == KEYGUARD_T && p == KEYGUARD_P
}

/// Argon2id(passphrase, salt) under (m, t, p): the 32-byte KEK of every
/// keyguard container. Callers pin the parameters first
/// ([`keyguard_params_pinned`]).
pub(crate) fn keyguard_kek(
    passphrase: &str,
    salt: &[u8],
    m: u32,
    t: u32,
    p: u32,
) -> Result<zeroize::Zeroizing<[u8; 32]>> {
    let params =
        argon2::Params::new(m, t, p, None).map_err(|e| anyhow!("keyguard argon2 params: {e}"))?;
    let kdf = argon2::Argon2::new(argon2::Algorithm::Argon2id, argon2::Version::V0x13, params);
    let mut kek = zeroize::Zeroizing::new([0u8; 32]);
    kdf.hash_password_into(passphrase.as_bytes(), salt, kek.as_mut())
        .map_err(|_| anyhow!("keyguard KEK derivation failed"))?;
    Ok(kek)
}

/// Fresh random bytes from the OS CSPRNG. Built with `array::from_fn` drawing
/// each byte straight from the RNG — there is no zero-initialized buffer, so
/// the value is unambiguously RNG-derived and never a hard-coded salt/nonce.
fn random_bytes<const N: usize>() -> [u8; N] {
    let mut rng = SysRng;
    std::array::from_fn(|_| (rng.try_next_u32().expect("OS RNG unavailable") & 0xff) as u8)
}

/// A fresh 96-bit AEAD nonce, every byte drawn from the OS RNG as the array
/// is built. There is no zero-filled buffer that is later overwritten: the
/// nonce never exists in a constant state, so a reader (or a static
/// analyzer) cannot mistake it for a hard-coded value, and a refactor that
/// drops the fill call cannot silently leave zeros behind.
fn fresh_nonce() -> [u8; 12] {
    random_bytes::<12>()
}

/// Wrap a 32-byte secret in `format`'s container under an Argon2id KEK
/// derived from `passphrase`, with a fresh salt and nonce. The AEAD's
/// associated data is `format.aad_domain ‖ aad_suffix`.
pub(crate) fn keyguard_seal(
    format: &KeyguardFormat,
    secret: &[u8; 32],
    passphrase: &str,
    aad_suffix: &[u8],
) -> Result<Vec<u8>> {
    let salt: [u8; KEYGUARD_SALT_LEN] = random_bytes();
    let nonce: [u8; 12] = random_bytes();
    keyguard_seal_with(format, secret, passphrase, aad_suffix, &salt, &nonce)
}

/// [`keyguard_seal`] with the salt and nonce supplied: deterministic, for
/// byte-identity tests and known-answer vectors. Production callers use
/// [`keyguard_seal`], which draws both from the OS RNG.
pub(crate) fn keyguard_seal_with(
    format: &KeyguardFormat,
    secret: &[u8; 32],
    passphrase: &str,
    aad_suffix: &[u8],
    salt: &[u8; KEYGUARD_SALT_LEN],
    nonce: &[u8; 12],
) -> Result<Vec<u8>> {
    let kek = keyguard_kek(passphrase, salt, KEYGUARD_M_KIB, KEYGUARD_T, KEYGUARD_P)?;
    let kek_bytes: &[u8; 32] = &kek;
    let commit: [u8; 32] = Sha256::digest(kek_bytes).into();

    let mut ct = *secret;
    let aad = keyguard_aad(format, aad_suffix);
    let tag = encrypt_payload(kek_bytes, nonce, &aad, &mut ct)?;

    let mut out = Vec::with_capacity(4 + 2 + 12 + 1 + KEYGUARD_SALT_LEN + 32 + 12 + 32 + 16);
    out.extend_from_slice(format.magic);
    out.push(1u8); // container version
    out.push(KEYGUARD_KIND_ARGON2ID);
    out.extend_from_slice(&KEYGUARD_M_KIB.to_le_bytes());
    out.extend_from_slice(&KEYGUARD_T.to_le_bytes());
    out.extend_from_slice(&KEYGUARD_P.to_le_bytes());
    out.push(KEYGUARD_SALT_LEN as u8);
    out.extend_from_slice(salt);
    out.extend_from_slice(&commit);
    out.extend_from_slice(nonce);
    out.extend_from_slice(&ct);
    out.extend_from_slice(&tag);
    ct.zeroize();
    Ok(out)
}

/// Recover the 32-byte secret from `format`'s container. Checks, in order:
/// magic, version, kind, the pinned Argon2id parameters (before any
/// derivation), then the KEK commitment (a clean "wrong passphrase"), then
/// the AEAD over `format.aad_domain ‖ aad_suffix` (a tampered byte, or a
/// container bound to another suffix).
pub(crate) fn keyguard_open(
    format: &KeyguardFormat,
    container: &[u8],
    passphrase: &str,
    aad_suffix: &[u8],
) -> Result<zeroize::Zeroizing<[u8; 32]>> {
    let label = format.label;
    let mut cur = 0usize;
    if read_slice(container, &mut cur, 4)? != format.magic {
        return Err(anyhow!("{label}: bad magic"));
    }
    let version = read_u8b(container, &mut cur, label)?;
    if version != 1 {
        return Err(anyhow!("{label}: unsupported version {version}"));
    }
    let kind = read_u8b(container, &mut cur, label)?;
    if kind != KEYGUARD_KIND_ARGON2ID {
        return Err(anyhow!("{label}: unsupported kind {kind}"));
    }
    let m = read_u32(container, &mut cur)?;
    let t = read_u32(container, &mut cur)?;
    let p = read_u32(container, &mut cur)?;
    // Encode always uses the fixed v1 parameters, so a container claiming any
    // other m/t/p is corrupt or hostile. Reject BEFORE deriving the KEK: an
    // attacker who can write the container could otherwise set a huge m and
    // OOM the process on the next open. (Parameter agility is a future kind.)
    if !keyguard_params_pinned(m, t, p) {
        let magic = String::from_utf8_lossy(format.magic);
        return Err(anyhow!("{label}: unexpected {magic} v1 parameters"));
    }
    let salt_len = read_u8b(container, &mut cur, label)? as usize;
    if format.exact && salt_len != KEYGUARD_SALT_LEN {
        return Err(anyhow!("{label}: unexpected salt length"));
    }
    let salt = read_slice(container, &mut cur, salt_len)?.to_vec();
    let commit = read_slice(container, &mut cur, 32)?.to_vec();
    let nonce = read_slice(container, &mut cur, 12)?.to_vec();
    let ct = read_slice(container, &mut cur, 32)?.to_vec();
    let tag = read_slice(container, &mut cur, 16)?.to_vec();
    if format.exact && cur != container.len() {
        return Err(anyhow!("{label}: trailing bytes after the tag"));
    }

    let kek = keyguard_kek(passphrase, &salt, m, t, p)?;
    let kek_bytes: &[u8; 32] = &kek;
    let got: [u8; 32] = Sha256::digest(kek_bytes).into();
    if got != commit.as_slice() {
        return Err(anyhow!("{label}: wrong passphrase"));
    }
    let aad = keyguard_aad(format, aad_suffix);
    let mut buf = zeroize::Zeroizing::new([0u8; 32]);
    buf.copy_from_slice(&ct);
    decrypt_payload(kek_bytes, &nonce, &aad, buf.as_mut(), &tag)
        .map_err(|_| anyhow!("{}", format.aead_failure))?;
    Ok(buf)
}

/// Wrap a 32-byte master key under an Argon2id KEK derived from `passphrase`,
/// producing a self-describing MKG1 container. `path_aad` binds the container
/// to its vault directory (via AAD) so a keyguard casually copied to a
/// different path won't open with the same passphrase.
pub(crate) fn keyguard_encode(
    master_key: &[u8; 32],
    passphrase: &str,
    path_aad: &[u8],
) -> Result<Vec<u8>> {
    keyguard_seal(&KEYGUARD_MKG1, master_key, passphrase, path_aad)
}

/// Recover the master key from an MKG1 container. Fails with a clean
/// "wrong passphrase" (via the KEK commitment) before the AEAD, and fails the
/// AEAD if the container was tampered with or copied to a different vault path.
pub(crate) fn keyguard_decode(
    container: &[u8],
    passphrase: &str,
    path_aad: &[u8],
) -> Result<[u8; 32]> {
    keyguard_open(&KEYGUARD_MKG1, container, passphrase, path_aad).map(|key| *key)
}

#[cfg(test)]
mod keyguard_tests {
    use super::*;

    #[test]
    fn keyguard_roundtrip() {
        let master = [7u8; 32];
        let c = keyguard_encode(&master, "hunter2 correct horse", b"/vault/a").unwrap();
        let got = keyguard_decode(&c, "hunter2 correct horse", b"/vault/a").unwrap();
        assert_eq!(got, master);
    }

    #[test]
    fn keyguard_wrong_passphrase_fails() {
        let c = keyguard_encode(&[9u8; 32], "right", b"/v").unwrap();
        let err = keyguard_decode(&c, "wrong", b"/v").unwrap_err().to_string();
        assert!(err.contains("wrong passphrase"), "got: {err}");
    }

    #[test]
    fn keyguard_path_binding_fails_when_moved() {
        // Same passphrase, different vault path: commitment passes, AEAD fails.
        let c = keyguard_encode(&[3u8; 32], "pw", b"/vault/a").unwrap();
        assert!(keyguard_decode(&c, "pw", b"/vault/b").is_err());
    }

    #[test]
    fn keyguard_rejects_out_of_spec_params() {
        // A tampered m (attacker trying to force a huge Argon2 allocation) is
        // rejected before the KEK is derived. m is a u32-LE at offset 6
        // (magic[4] + version[1] + kind[1]).
        let mut c = keyguard_encode(&[2u8; 32], "pw", b"/v").unwrap();
        c[6..10].copy_from_slice(&9_999_999u32.to_le_bytes());
        let err = keyguard_decode(&c, "pw", b"/v").unwrap_err().to_string();
        assert!(err.contains("v1 parameters"), "got: {err}");
    }

    #[test]
    fn keyguard_tamper_fails() {
        let mut c = keyguard_encode(&[1u8; 32], "pw", b"/v").unwrap();
        let last = c.len() - 1;
        c[last] ^= 0xff; // flip a tag byte
        assert!(keyguard_decode(&c, "pw", b"/v").is_err());
    }

    #[test]
    fn keyguard_truncated_fails() {
        let c = keyguard_encode(&[1u8; 32], "pw", b"/v").unwrap();
        assert!(keyguard_decode(&c[..c.len() - 10], "pw", b"/v").is_err());
    }

    /// An MKG1 container produced by the keyguard code as it stood BEFORE the
    /// encoder was shared over (magic, AAD domain): master key `[0x5a; 32]`,
    /// passphrase "mkg1 byte identity", path AAD "/vault/identity". Captured
    /// once from that code and frozen here; MKG1 is a frozen format.
    const MKG1_PRE_REFACTOR_HEX: &str = "4d4b4731010100000100030000000100000010474045a42a873ce25ef20c557064eecf4891775d3b0097d3399c16062abb6be70e3e52d7267fb02d7412d4e390200a7b763858fb9d5dfa4fa7f447612c16d88126887cd2f6b13ce1b87f126aa3a0898494abcf8f31adc4c5e06fa7c74559e07fdce960735174389deb0ca8f8";

    #[test]
    fn keyguard_mkg1_bytes_unchanged_by_the_shared_recipe() {
        let frozen = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        assert_eq!(frozen.len(), 127, "MKG1 v1 is 127 bytes");
        // The refactored reader opens the pre-refactor bytes.
        let master = keyguard_decode(&frozen, "mkg1 byte identity", b"/vault/identity").unwrap();
        assert_eq!(master, [0x5a; 32]);
        // The refactored writer, given the same salt and nonce, reproduces
        // them byte for byte: salt at [19..35), nonce at [67..79).
        let salt: [u8; 16] = frozen[19..35].try_into().unwrap();
        let nonce: [u8; 12] = frozen[67..79].try_into().unwrap();
        let again = keyguard_seal_with(
            &KEYGUARD_MKG1,
            &[0x5a; 32],
            "mkg1 byte identity",
            b"/vault/identity",
            &salt,
            &nonce,
        )
        .unwrap();
        assert_eq!(hex::encode(&again), MKG1_PRE_REFACTOR_HEX);
    }

    #[test]
    fn keyguard_error_strings_unchanged_for_mkg1() {
        // Older-build messages other briefs and the runbook quote.
        let mut c = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        c[0] = b'X';
        let err = keyguard_decode(&c, "mkg1 byte identity", b"/vault/identity")
            .unwrap_err()
            .to_string();
        assert_eq!(err, "vault keyguard: bad magic");
        let mut c = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        c[5] = 2;
        let err = keyguard_decode(&c, "x", b"/vault/identity")
            .unwrap_err()
            .to_string();
        assert_eq!(err, "vault keyguard: unsupported kind 2");
        let mut c = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        c[10..14].copy_from_slice(&4u32.to_le_bytes());
        let err = keyguard_decode(&c, "x", b"/vault/identity")
            .unwrap_err()
            .to_string();
        assert_eq!(err, "vault keyguard: unexpected MKG1 v1 parameters");
        let c = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        let err = keyguard_decode(&c, "not it", b"/vault/identity")
            .unwrap_err()
            .to_string();
        assert_eq!(err, "vault keyguard: wrong passphrase");
        // MKG1 stays a lenient reader: a trailing byte is ignored, as before.
        let mut c = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
        c.push(0);
        assert!(keyguard_decode(&c, "mkg1 byte identity", b"/vault/identity").is_ok());
    }
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
        VaultCryptoMode::Pq => {
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
            Ok(DerivedDek {
                dek,
                kem_alg: KEM_ALG_ML_KEM_768.to_string(),
                kem_ct,
                kdf_info,
                classical_wrap: None,
            })
        }
        VaultCryptoMode::Hybrid => {
            // Two independent secrets, one DEK. The master key wraps a fresh
            // classical secret (not the DEK), so the wrap alone opens
            // nothing; the KEM shared secret alone derives nothing either.
            let kem_keypair =
                kem_keypair.ok_or_else(|| anyhow!("vault KEM keypair missing for hybrid mode"))?;
            let (kem_ct, mut shared_secret) = kem_encapsulate(kem_keypair)?;
            let mut kdf_info = vec![0u8; 32];
            SysRng
                .try_fill_bytes(&mut kdf_info[..])
                .expect("OS RNG unavailable");
            let mut classical_secret = random_dek();
            let wrap = wrap_dek(master_key, envelope_id, ruleset_hash, &classical_secret)?;
            let dek = kdf_dek_hybrid(&shared_secret, &classical_secret, &kdf_info);
            shared_secret.zeroize();
            classical_secret.zeroize();
            Ok(DerivedDek {
                dek,
                kem_alg: KEM_ALG_HYBRID_ML_KEM_768.to_string(),
                kem_ct,
                kdf_info,
                classical_wrap: Some(wrap),
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

/// The hybrid DEK: a domain-tagged, length-prefixed hash over the KEM shared
/// secret, the classical secret and the per-envelope info. Either secret
/// missing or wrong yields a different key, so the AEAD tag fails.
fn kdf_dek_hybrid(shared_secret: &[u8], classical_secret: &[u8; 32], kdf_info: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(HYBRID_DEK_TAG);
    hasher.update((shared_secret.len() as u32).to_le_bytes());
    hasher.update(shared_secret);
    hasher.update(classical_secret);
    hasher.update((kdf_info.len() as u32).to_le_bytes());
    hasher.update(kdf_info);
    hasher.finalize().into()
}

fn wrap_dek(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    dek: &[u8; 32],
) -> Result<Vec<u8>> {
    let nonce = fresh_nonce();
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

/// The vault's ML-KEM-768 (FIPS 203) keypair, with the decapsulation key's
/// stored encoding.
#[derive(Clone)]
pub struct KemKeypair {
    #[cfg(feature = "pqc-vault")]
    pub public: EncapsulationKey768,
    #[cfg(feature = "pqc-vault")]
    pub secret: DecapsulationKey768,
    #[cfg(feature = "pqc-vault")]
    secret_bytes: zeroize::Zeroizing<Vec<u8>>,
}

impl std::fmt::Debug for KemKeypair {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("KemKeypair").finish_non_exhaustive()
    }
}

#[cfg(feature = "pqc-vault")]
impl KemKeypair {
    /// Generates a keypair (`ML-KEM.KeyGen`, FIPS 203 section 7.1) from the
    /// OS RNG. What is stored is the 2400-byte decapsulation key, the same
    /// encoding every earlier build stored and reads back, not the seed.
    pub fn generate() -> Self {
        let secret =
            DecapsulationKey768::try_generate_from_rng(&mut SysRng).expect("OS RNG unavailable");
        let public = secret.encapsulation_key().clone();
        // Deprecated upstream in favor of the seed, like the import in
        // `KemKeypair::from_bytes`.
        #[allow(deprecated)]
        let mut encoded = ml_kem::ExpandedKeyEncoding::to_expanded_bytes(&secret);
        let secret_bytes = zeroize::Zeroizing::new(encoded.to_vec());
        encoded.zeroize();
        Self {
            public,
            secret,
            secret_bytes,
        }
    }

    pub fn public_bytes(&self) -> Vec<u8> {
        self.public.to_bytes().to_vec()
    }

    pub fn secret_bytes(&self) -> Vec<u8> {
        self.secret_bytes.to_vec()
    }

    /// Loads a stored keypair: the FIPS 203 encapsulation key (1184 bytes)
    /// and decapsulation key (2400 bytes), the two halves of
    /// `kem-mlkem768.key`.
    ///
    /// Both FIPS 203 input checks are `ml-kem`'s, made here on import and so
    /// before any encapsulation or decapsulation:
    ///
    /// - section 7.2, the encapsulation key check, on `public`: the length,
    ///   and the modulus check (every 12-bit coefficient is below q);
    /// - section 7.3, the decapsulation key check, on `secret`: the length,
    ///   and the hash check (the `H(ek)` it stores is the hash of the
    ///   encapsulation key it stores beside it).
    ///
    /// `ml-kem` keeps the decapsulation-key import for keys that were not
    /// stored as a seed and marks it deprecated in favor of seeds; a stored
    /// key has no seed to return to, so the vault depends on it for as long
    /// as such key files exist.
    pub fn from_bytes(public: &[u8], secret: &[u8]) -> Result<Self> {
        let public = EncapsulationKey768::new_from_slice(public)
            .map_err(|_| anyhow!("invalid KEM public key"))?;
        let mut encoded = ExpandedDecapsulationKey::<MlKem768>::try_from(secret)
            .map_err(|_| anyhow!("invalid KEM secret key"))?;
        #[allow(deprecated)]
        let decoded = DecapsulationKey768::from_expanded(&encoded);
        encoded.zeroize();
        let secret_key = decoded.map_err(|_| anyhow!("invalid KEM secret key"))?;
        let secret_bytes = zeroize::Zeroizing::new(secret.to_vec());
        Ok(Self {
            public,
            secret: secret_key,
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
        // `ML-KEM.Encaps` (FIPS 203 section 7.2). Its 32 random bytes come
        // from the OS RNG, and a failure of that RNG panics here, as it does
        // for the nonce and the DEK drawn beside this call.
        let (ciphertext, mut shared) = kem
            .public
            .encapsulate_with_rng(&mut rand::rand_core::UnwrapErr(SysRng));
        let out = (ciphertext.to_vec(), shared.to_vec());
        shared.zeroize();
        Ok(out)
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
        // `ML-KEM.Decaps` (FIPS 203 section 7.3). The ciphertext type check
        // of that section is the length check; the key was checked on import.
        let mut shared = kem
            .secret
            .decapsulate_slice(kem_ct)
            .map_err(|_| anyhow!("invalid KEM ciphertext"))?;
        let out = shared.to_vec();
        shared.zeroize();
        Ok(out)
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
        assert_eq!(envelope_hybrid.kem_alg, KEM_ALG_HYBRID_ML_KEM_768);
        assert!(envelope_hybrid.classical_wrap.is_some());
    }

    #[test]
    #[cfg(feature = "pqc-vault")]
    fn hybrid_needs_both_secrets() {
        // Neither the master key alone nor the KEM keypair alone opens a
        // hybrid envelope: the wrap holds a classical secret, not the DEK,
        // and the DEK derivation takes both.
        let key = random_dek(); // a fresh master key, never a fixture that could be mistaken for one
        let kp = KemKeypair::generate();
        let envelope = seal_v2(
            "test-hybrid",
            [4u8; 32],
            b"pq payload",
            VaultCryptoMode::Hybrid,
            &key,
            Some(&kp),
        )
        .unwrap();
        assert!(
            decrypt_v2(&envelope, &key, None).is_err(),
            "master key alone"
        );
        assert!(
            decrypt_v2(&envelope, &random_dek(), Some(&kp)).is_err(),
            "KEM keypair with the wrong master key"
        );
        assert!(
            decrypt_v2(&envelope, &key, Some(&KemKeypair::generate())).is_err(),
            "master key with the wrong KEM keypair"
        );
        let mut stripped = envelope.clone();
        stripped.classical_wrap = None;
        assert!(
            decrypt_v2(&stripped, &key, Some(&kp)).is_err(),
            "wrap removed"
        );
        let mut relabeled = envelope.clone();
        relabeled.kem_alg = KEM_ALG_ML_KEM_768.to_string();
        assert!(
            decrypt_v2(&relabeled, &key, Some(&kp)).is_err(),
            "relabeled as the OR shape, the wrap is not a DEK and the KEM DEK is not the hybrid DEK"
        );
    }

    #[test]
    #[cfg(feature = "pqc-vault")]
    fn legacy_or_shaped_envelope_still_opens_with_either_secret() {
        // An envelope written before the hybrid fix: `ml-kem-768` plus a
        // wrap of the DEK itself. It must keep opening (with either secret,
        // as it always did) so an existing vault stays readable.
        let key = random_dek();
        let kp = KemKeypair::generate();
        let mut envelope = seal_v2(
            "legacy",
            [4u8; 32],
            b"old",
            VaultCryptoMode::Pq,
            &key,
            Some(&kp),
        )
        .unwrap();
        let mut shared = kem_decapsulate(&kp, &envelope.kem_ct).unwrap();
        let dek = kdf_dek(&shared, &envelope.kdf_info);
        shared.zeroize();
        envelope.classical_wrap = Some(wrap_dek(&key, "legacy", &[4u8; 32], &dek).unwrap());
        assert_eq!(decrypt_v2(&envelope, &key, None).unwrap(), b"old");
        assert_eq!(
            decrypt_v2(&envelope, &random_dek(), Some(&kp)).unwrap(),
            b"old"
        );
    }

    /// The known-answer material in `tests/fixtures/pqcrypto_kat/`: an
    /// ML-KEM-768 key file and sealed envelopes exactly as a vault directory
    /// holds them, with a ciphertext and its shared secret, all written by the
    /// kernel while it linked `pqcrypto-mlkem` 0.1.1 (the README there says
    /// how). These tests hold the implementation this build links to that
    /// material: a vault keeps its key file and every envelope sealed to it.
    /// They passed unchanged on `pqcrypto-mlkem` before `ml-kem` replaced it.
    #[cfg(feature = "pqc-vault")]
    mod pqcrypto_kat {
        use super::*;
        use crate::vault::format::VaultEnvelope;
        use crate::vault::{FilesystemVaultStore, VaultConfig, VaultKeyMaterial};

        const KAT_PATH: &str = "tests/fixtures/pqcrypto_kat/mlkem768_kat.json";
        const KEY_FILE_NAME: &str = "kem-mlkem768.key";

        pub(super) struct KatEnvelope {
            pub mode: String,
            pub envelope_id: String,
            pub plaintext: Vec<u8>,
            pub file: Vec<u8>,
        }

        pub(super) struct Kat {
            pub public_key: Vec<u8>,
            pub secret_key: Vec<u8>,
            pub ciphertext: Vec<u8>,
            pub shared_secret: Vec<u8>,
            pub rejected_ciphertext: Vec<u8>,
            pub rejected_shared_secret: Vec<u8>,
            pub key_file: Vec<u8>,
            pub master_key: [u8; 32],
            pub ruleset_hash: [u8; 32],
            pub envelopes: Vec<KatEnvelope>,
        }

        impl Kat {
            pub fn keypair(&self) -> KemKeypair {
                KemKeypair::from_bytes(&self.public_key, &self.secret_key)
                    .expect("a key the vault stored must load")
            }
        }

        fn hex_field(doc: &serde_json::Value, name: &str) -> Vec<u8> {
            let field = doc[name]
                .as_str()
                .unwrap_or_else(|| panic!("{KAT_PATH}: no string field {name}"));
            hex::decode(field).unwrap_or_else(|e| panic!("{KAT_PATH}: {name} is not hex: {e}"))
        }

        pub(super) fn load() -> Kat {
            let text = std::fs::read_to_string(KAT_PATH)
                .unwrap_or_else(|e| panic!("read {KAT_PATH}: {e}"));
            let doc: serde_json::Value =
                serde_json::from_str(&text).unwrap_or_else(|e| panic!("parse {KAT_PATH}: {e}"));
            let envelopes = doc["envelopes"]
                .as_array()
                .expect("an envelopes array")
                .iter()
                .map(|envelope| KatEnvelope {
                    mode: envelope["mode"].as_str().expect("mode").to_string(),
                    envelope_id: envelope["envelope_id"]
                        .as_str()
                        .expect("envelope_id")
                        .to_string(),
                    plaintext: hex_field(envelope, "plaintext"),
                    file: hex_field(envelope, "file"),
                })
                .collect();
            Kat {
                public_key: hex_field(&doc, "public_key"),
                secret_key: hex_field(&doc, "secret_key"),
                ciphertext: hex_field(&doc, "ciphertext"),
                shared_secret: hex_field(&doc, "shared_secret"),
                rejected_ciphertext: hex_field(&doc, "rejected_ciphertext"),
                rejected_shared_secret: hex_field(&doc, "rejected_shared_secret"),
                key_file: hex_field(&doc, "key_file"),
                master_key: hex_field(&doc, "master_key")
                    .try_into()
                    .expect("32-byte master key"),
                ruleset_hash: hex_field(&doc, "ruleset_hash")
                    .try_into()
                    .expect("32-byte ruleset hash"),
                envelopes,
            }
        }

        /// The key file a vault directory holds:
        /// `le32(len) || public || le32(len) || secret`.
        fn split_key_file(bytes: &[u8]) -> (&[u8], &[u8]) {
            let mut cursor = 0usize;
            let public_len = read_u32(bytes, &mut cursor).unwrap() as usize;
            let public = read_slice(bytes, &mut cursor, public_len).unwrap();
            let secret_len = read_u32(bytes, &mut cursor).unwrap() as usize;
            let secret = read_slice(bytes, &mut cursor, secret_len).unwrap();
            assert_eq!(cursor, bytes.len(), "nothing after the secret key");
            (public, secret)
        }

        fn open_store(root: &std::path::Path, mode: VaultCryptoMode) -> FilesystemVaultStore {
            FilesystemVaultStore::new(VaultConfig {
                local_path: root.to_path_buf(),
                crypto_mode: mode,
                key_material: VaultKeyMaterial::Plaintext,
            })
            .expect("open the vault directory")
        }

        #[test]
        fn stored_key_encodings_load_and_come_back_byte_for_byte() {
            let kat = load();
            assert_eq!(
                kat.public_key.len(),
                1184,
                "FIPS 203 ML-KEM-768 encapsulation key"
            );
            assert_eq!(
                kat.secret_key.len(),
                2400,
                "FIPS 203 ML-KEM-768 decapsulation key"
            );
            let keypair = kat.keypair();
            assert_eq!(keypair.public_bytes(), kat.public_key);
            assert_eq!(keypair.secret_bytes(), kat.secret_key);
        }

        #[test]
        fn stored_key_file_is_the_stored_keys_length_prefixed() {
            let kat = load();
            let (public, secret) = split_key_file(&kat.key_file);
            assert_eq!(public, kat.public_key);
            assert_eq!(secret, kat.secret_key);
        }

        #[test]
        fn stored_ciphertext_decapsulates_to_the_stored_shared_secret() {
            let kat = load();
            assert_eq!(kat.ciphertext.len(), 1088, "FIPS 203 ML-KEM-768 ciphertext");
            assert_eq!(kat.shared_secret.len(), 32);
            let shared = kem_decapsulate(&kat.keypair(), &kat.ciphertext).unwrap();
            assert_eq!(shared, kat.shared_secret);
        }

        #[test]
        fn stored_implicit_rejection_secret_is_reproduced() {
            // The rejection secret is J(z || c): the one output that depends
            // on the z value at the end of the stored decapsulation key.
            let kat = load();
            let shared = kem_decapsulate(&kat.keypair(), &kat.rejected_ciphertext).unwrap();
            assert_eq!(shared, kat.rejected_shared_secret);
            assert_ne!(shared, kat.shared_secret);
        }

        #[test]
        fn a_fresh_encapsulation_to_the_stored_key_decapsulates() {
            let kat = load();
            let keypair = kat.keypair();
            let (ciphertext, shared_secret) = kem_encapsulate(&keypair).unwrap();
            assert_eq!(ciphertext.len(), 1088);
            assert_eq!(shared_secret.len(), 32);
            assert_ne!(ciphertext, kat.ciphertext, "encapsulation is randomized");
            assert_eq!(
                kem_decapsulate(&keypair, &ciphertext).unwrap(),
                shared_secret
            );
        }

        #[test]
        fn stored_key_file_opens_every_stored_envelope() {
            let kat = load();
            assert_eq!(
                kat.envelopes.len(),
                3,
                "pq, hybrid and the earlier hybrid shape"
            );
            for mode in [VaultCryptoMode::Pq, VaultCryptoMode::Hybrid] {
                // A vault directory as the earlier build left it.
                let dir = tempfile::tempdir().unwrap();
                std::fs::write(dir.path().join(KEY_FILE_NAME), &kat.key_file).unwrap();
                std::fs::write(dir.path().join("master.key"), kat.master_key).unwrap();
                let store = open_store(dir.path(), mode);
                assert_eq!(
                    std::fs::read(dir.path().join(KEY_FILE_NAME)).unwrap(),
                    kat.key_file,
                    "opening a vault must not rewrite its key file"
                );
                let keypair = store.kem_keypair.as_ref().expect("the key file loads");
                assert_eq!(keypair.public_bytes(), kat.public_key);
                assert_eq!(keypair.secret_bytes(), kat.secret_key);

                for stored in &kat.envelopes {
                    let VaultEnvelope::V2(envelope) = VaultEnvelope::decode(&stored.file).unwrap()
                    else {
                        panic!("{} is not a v2 envelope", stored.envelope_id);
                    };
                    VaultEnvelope::V2(envelope.clone())
                        .validate(&stored.envelope_id, kat.ruleset_hash)
                        .unwrap();
                    let expected_label = match stored.mode.as_str() {
                        "hybrid" => KEM_ALG_HYBRID_ML_KEM_768,
                        _ => KEM_ALG_ML_KEM_768,
                    };
                    assert_eq!(envelope.kem_alg, expected_label, "{}", stored.envelope_id);
                    assert_eq!(envelope.kem_ct.len(), 1088, "{}", stored.envelope_id);
                    let clear = decrypt_v2(&envelope, &store.master_key, Some(keypair))
                        .unwrap_or_else(|e| panic!("{} does not open: {e}", stored.envelope_id));
                    assert_eq!(clear, stored.plaintext, "{}", stored.envelope_id);
                }
            }
        }

        #[test]
        fn stored_envelopes_open_through_the_kem_and_not_only_the_wrap() {
            // A wrong master key removes the classical path: the pq envelope
            // (no wrap) and the earlier hybrid shape (either secret) must
            // still open through decapsulation alone, and the true hybrid,
            // which needs both secrets, must not.
            let kat = load();
            let keypair = kat.keypair();
            // Any key but the stored one will do, so none is written down:
            // it is drawn the way the vault draws a DEK.
            let wrong_master_key = random_dek();
            assert_ne!(wrong_master_key, kat.master_key);
            for stored in &kat.envelopes {
                let VaultEnvelope::V2(envelope) = VaultEnvelope::decode(&stored.file).unwrap()
                else {
                    panic!("{} is not a v2 envelope", stored.envelope_id);
                };
                let opened = decrypt_v2(&envelope, &wrong_master_key, Some(&keypair));
                match stored.mode.as_str() {
                    "hybrid" => assert!(opened.is_err(), "hybrid opened without the master key"),
                    _ => assert_eq!(opened.unwrap(), stored.plaintext, "{}", stored.envelope_id),
                }
            }
        }

        #[test]
        fn stored_envelopes_do_not_open_with_another_keypair() {
            let kat = load();
            let stranger = KemKeypair::generate();
            for stored in &kat.envelopes {
                let VaultEnvelope::V2(envelope) = VaultEnvelope::decode(&stored.file).unwrap()
                else {
                    panic!("{} is not a v2 envelope", stored.envelope_id);
                };
                let opened = decrypt_v2(&envelope, &kat.master_key, Some(&stranger));
                match stored.mode.as_str() {
                    // The earlier hybrid shape opens with the master key alone.
                    "legacy-or" => assert_eq!(opened.unwrap(), stored.plaintext),
                    _ => assert!(opened.is_err(), "{} opened", stored.envelope_id),
                }
            }
        }

        #[test]
        fn keys_and_ciphertexts_of_the_wrong_length_are_refused() {
            let kat = load();
            assert!(KemKeypair::from_bytes(&kat.public_key[..1183], &kat.secret_key).is_err());
            assert!(KemKeypair::from_bytes(&kat.public_key, &kat.secret_key[..2399]).is_err());
            assert!(KemKeypair::from_bytes(&[], &[]).is_err());
            let mut long = kat.public_key.clone();
            long.push(0);
            assert!(KemKeypair::from_bytes(&long, &kat.secret_key).is_err());

            let keypair = kat.keypair();
            assert!(kem_decapsulate(&keypair, &kat.ciphertext[..1087]).is_err());
            let mut long = kat.ciphertext.clone();
            long.push(0);
            assert!(kem_decapsulate(&keypair, &long).is_err());
            assert!(kem_decapsulate(&keypair, &[]).is_err());
        }

        #[test]
        fn a_generated_keypair_round_trips_through_its_stored_encodings() {
            let generated = KemKeypair::generate();
            assert_eq!(generated.public_bytes().len(), 1184);
            assert_eq!(generated.secret_bytes().len(), 2400);
            let reloaded =
                KemKeypair::from_bytes(&generated.public_bytes(), &generated.secret_bytes())
                    .expect("what generate() stores must load");
            assert_eq!(reloaded.public_bytes(), generated.public_bytes());
            assert_eq!(reloaded.secret_bytes(), generated.secret_bytes());

            // Encapsulated to one copy, decapsulated by the other, both ways.
            for (sender, receiver) in [(&generated, &reloaded), (&reloaded, &generated)] {
                let (ciphertext, shared_secret) = kem_encapsulate(sender).unwrap();
                assert_eq!(
                    kem_decapsulate(receiver, &ciphertext).unwrap(),
                    shared_secret
                );
            }
        }

        #[test]
        fn a_new_vault_directory_writes_a_key_file_that_reopens() {
            let dir = tempfile::tempdir().unwrap();
            let first = open_store(dir.path(), VaultCryptoMode::Hybrid);
            let key_file = std::fs::read(dir.path().join(KEY_FILE_NAME)).unwrap();
            assert_eq!(key_file.len(), 4 + 1184 + 4 + 2400);
            let (public, secret) = split_key_file(&key_file);
            let written = first.kem_keypair.as_ref().expect("a keypair is created");
            assert_eq!(public, written.public_bytes());
            assert_eq!(secret, written.secret_bytes());

            let mut clear = b"round trip".to_vec();
            first
                .seal_bytes("round-trip", [7u8; 32], &mut clear)
                .unwrap();
            drop(first);

            let second = open_store(dir.path(), VaultCryptoMode::Hybrid);
            let file = std::fs::read(dir.path().join("round-trip.vault")).unwrap();
            let VaultEnvelope::V2(envelope) = VaultEnvelope::decode(&file).unwrap() else {
                panic!("not a v2 envelope");
            };
            assert_eq!(
                decrypt_v2(&envelope, &second.master_key, second.kem_keypair.as_ref()).unwrap(),
                b"round trip"
            );
        }
    }

    /// The two input checks FIPS 203 requires and RUSTSEC-2026-0585 found
    /// missing from the implementation this module used before `ml-kem`. The
    /// checks are `ml-kem`'s; these tests prove they are made where a key
    /// enters the vault, before anything is encapsulated to it or
    /// decapsulated with it.
    #[cfg(feature = "pqc-vault")]
    mod mlkem768_checks {
        use super::*;
        use crate::vault::{FilesystemVaultStore, VaultConfig, VaultKeyMaterial};

        /// The field modulus q of ML-KEM.
        const Q: u16 = 3329;
        /// The decapsulation key is
        /// `dk_pke (1152) || ek (1184) || H(ek) (32) || z (32)`
        /// (FIPS 203, `ML-KEM.KeyGen_internal`), and the encapsulation key is
        /// `t-hat (1152) || rho (32)`.
        const EK_IN_DK: usize = 1152;
        const HASH_IN_DK: usize = EK_IN_DK + 1184;
        const Z_IN_DK: usize = HASH_IN_DK + 32;

        /// Writes one coefficient of t-hat. `ByteEncode_12` (FIPS 203 section
        /// 4.2.1) packs two 12-bit coefficients into three bytes, least
        /// significant bits first.
        fn set_coefficient(key: &mut [u8], index: usize, value: u16) {
            assert!(value < 1 << 12);
            let start = (index / 2) * 3;
            if index.is_multiple_of(2) {
                key[start] = value as u8;
                key[start + 1] = (key[start + 1] & 0xF0) | (value >> 8) as u8;
            } else {
                key[start + 1] = (key[start + 1] & 0x0F) | ((value & 0x0F) << 4) as u8;
                key[start + 2] = (value >> 4) as u8;
            }
        }

        fn refusal(public: &[u8], secret: &[u8]) -> String {
            KemKeypair::from_bytes(public, secret)
                .expect_err("the key must be refused")
                .to_string()
        }

        #[test]
        fn an_encapsulation_key_with_a_coefficient_not_below_q_is_refused() {
            let kat = pqcrypto_kat::load();
            // FIPS 203 section 7.2, modulus check. The first coefficient set
            // to 0xFFF, then q itself and 0xFFF at the first and last
            // coefficient of each of the three polynomials (the positions of
            // Wycheproof's ModulusOverflow cases).
            let mut public = kat.public_key.clone();
            set_coefficient(&mut public, 0, 0xFFF);
            assert_eq!(&public[..2], &[0xFF, kat.public_key[1] | 0x0F]);
            assert_eq!(refusal(&public, &kat.secret_key), "invalid KEM public key");

            for index in [0, 255, 256, 511, 512, 767] {
                for value in [Q, 0xFFF] {
                    let mut public = kat.public_key.clone();
                    set_coefficient(&mut public, index, value);
                    assert_eq!(
                        refusal(&public, &kat.secret_key),
                        "invalid KEM public key",
                        "coefficient {index} = {value}"
                    );
                }
                // One below q is a coefficient. The key is then another
                // well-formed key, and it loads: nothing here ties the public
                // half of the file to the secret half, as nothing did before.
                let mut public = kat.public_key.clone();
                set_coefficient(&mut public, index, Q - 1);
                assert_ne!(public, kat.public_key);
                let keypair = KemKeypair::from_bytes(&public, &kat.secret_key)
                    .unwrap_or_else(|e| panic!("coefficient {index} = q - 1: {e}"));
                assert_eq!(keypair.public_bytes(), public);
            }
        }

        #[test]
        fn a_decapsulation_key_whose_stored_hash_is_altered_is_refused() {
            let kat = pqcrypto_kat::load();
            // FIPS 203 section 7.3, hash check: H(ek) stored in the key must
            // be the hash of the encapsulation key stored beside it.
            for offset in [0, 15, 31] {
                for mask in [0x01u8, 0x80] {
                    let mut secret = kat.secret_key.clone();
                    secret[HASH_IN_DK + offset] ^= mask;
                    assert_eq!(
                        refusal(&kat.public_key, &secret),
                        "invalid KEM secret key",
                        "hash byte {offset} xor {mask:#04x}"
                    );
                }
            }
        }

        #[test]
        fn a_decapsulation_key_whose_embedded_encapsulation_key_is_altered_is_refused() {
            let kat = pqcrypto_kat::load();
            assert_eq!(
                &kat.secret_key[EK_IN_DK..HASH_IN_DK],
                &kat.public_key[..],
                "the decapsulation key embeds the encapsulation key"
            );
            // A bit of t-hat, a bit of rho, and a coefficient past q: in each
            // the stored hash is no longer the hash of the embedded key.
            let mut altered_t_hat = kat.secret_key.clone();
            altered_t_hat[EK_IN_DK + 700] ^= 0x01;
            let mut altered_rho = kat.secret_key.clone();
            altered_rho[HASH_IN_DK - 1] ^= 0x01;
            let mut overflowed = kat.secret_key.clone();
            set_coefficient(&mut overflowed[EK_IN_DK..HASH_IN_DK], 0, 0xFFF);
            for (what, secret) in [
                ("t-hat", altered_t_hat),
                ("rho", altered_rho),
                ("coefficient", overflowed),
            ] {
                assert_eq!(
                    refusal(&kat.public_key, &secret),
                    "invalid KEM secret key",
                    "{what}"
                );
            }
        }

        #[test]
        fn a_vault_directory_whose_key_file_fails_a_check_does_not_open() {
            let kat = pqcrypto_kat::load();
            let key_file = |public: &[u8], secret: &[u8]| -> Vec<u8> {
                let mut file = Vec::new();
                file.extend_from_slice(&(public.len() as u32).to_le_bytes());
                file.extend_from_slice(public);
                file.extend_from_slice(&(secret.len() as u32).to_le_bytes());
                file.extend_from_slice(secret);
                file
            };
            assert_eq!(key_file(&kat.public_key, &kat.secret_key), kat.key_file);

            let mut overflowed_public = kat.public_key.clone();
            set_coefficient(&mut overflowed_public, 0, 0xFFF);
            let mut altered_hash = kat.secret_key.clone();
            altered_hash[HASH_IN_DK] ^= 0x01;

            for (file, expected) in [
                (
                    key_file(&overflowed_public, &kat.secret_key),
                    "invalid KEM public key",
                ),
                (
                    key_file(&kat.public_key, &altered_hash),
                    "invalid KEM secret key",
                ),
            ] {
                for mode in [
                    VaultCryptoMode::Classical,
                    VaultCryptoMode::Pq,
                    VaultCryptoMode::Hybrid,
                ] {
                    let dir = tempfile::tempdir().unwrap();
                    std::fs::write(dir.path().join("kem-mlkem768.key"), &file).unwrap();
                    std::fs::write(dir.path().join("master.key"), kat.master_key).unwrap();
                    let opened = FilesystemVaultStore::new(VaultConfig {
                        local_path: dir.path().to_path_buf(),
                        crypto_mode: mode,
                        key_material: VaultKeyMaterial::Plaintext,
                    });
                    match opened {
                        Ok(_) => panic!("a vault opened in {mode} mode with a refused key file"),
                        Err(e) => assert_eq!(e.to_string(), expected, "{mode} mode"),
                    }
                    assert_eq!(
                        std::fs::read(dir.path().join("kem-mlkem768.key")).unwrap(),
                        file,
                        "a refused key file is left as it was"
                    );
                }
            }
        }

        #[test]
        fn what_no_check_covers_still_cannot_open_an_envelope() {
            // Neither check reads the secret vector (the first 1152 bytes) or
            // z (the last 32). A key altered there loads, as it always did.
            // An altered secret vector then decapsulates the stored
            // ciphertext to another secret; an altered z changes only the
            // implicit-rejection secret.
            let kat = pqcrypto_kat::load();

            let mut altered_vector = kat.secret_key.clone();
            for byte in altered_vector[..EK_IN_DK].iter_mut().step_by(7) {
                *byte ^= 0x55;
            }
            let keypair = KemKeypair::from_bytes(&kat.public_key, &altered_vector).unwrap();
            assert_ne!(
                kem_decapsulate(&keypair, &kat.ciphertext).unwrap(),
                kat.shared_secret
            );
            let pq = kat
                .envelopes
                .iter()
                .find(|envelope| envelope.mode == "pq")
                .expect("the pq envelope");
            let crate::vault::format::VaultEnvelope::V2(envelope) =
                crate::vault::format::VaultEnvelope::decode(&pq.file).unwrap()
            else {
                panic!("not a v2 envelope");
            };
            assert!(decrypt_v2(&envelope, &kat.master_key, Some(&keypair)).is_err());

            let mut altered_z = kat.secret_key.clone();
            altered_z[Z_IN_DK] ^= 0x01;
            let keypair = KemKeypair::from_bytes(&kat.public_key, &altered_z).unwrap();
            assert_eq!(
                kem_decapsulate(&keypair, &kat.ciphertext).unwrap(),
                kat.shared_secret
            );
            assert_ne!(
                kem_decapsulate(&keypair, &kat.rejected_ciphertext).unwrap(),
                kat.rejected_shared_secret
            );
        }

        /// Deterministic filler: SHA-256 in counter mode.
        fn filler(label: &str, len: usize) -> Vec<u8> {
            let mut out = Vec::with_capacity(len + 32);
            let mut counter = 0u32;
            while out.len() < len {
                let mut hasher = Sha256::new();
                hasher.update(label.as_bytes());
                hasher.update(counter.to_le_bytes());
                out.extend_from_slice(&hasher.finalize());
                counter += 1;
            }
            out.truncate(len);
            out
        }

        #[test]
        fn arbitrary_bytes_as_a_key_file_or_ciphertext_never_panic() {
            let kat = pqcrypto_kat::load();
            let keypair = kat.keypair();
            for case in 0..32u32 {
                // 1184 arbitrary bytes hold a coefficient at or past q with
                // near certainty, and 2400 arbitrary bytes do not carry the
                // hash of the key they embed: both are refused.
                let public = filler(&format!("public {case}"), 1184);
                let secret = filler(&format!("secret {case}"), 2400);
                assert_eq!(refusal(&public, &kat.secret_key), "invalid KEM public key");
                assert_eq!(refusal(&kat.public_key, &secret), "invalid KEM secret key");
                // Any 1088 bytes decapsulate (implicit rejection), to a
                // secret that is not the stored one.
                let ciphertext = filler(&format!("ciphertext {case}"), 1088);
                let shared = kem_decapsulate(&keypair, &ciphertext).unwrap();
                assert_eq!(shared.len(), 32);
                assert_ne!(shared, kat.shared_secret);
            }
        }

        #[test]
        fn debug_output_carries_no_key_material() {
            let kat = pqcrypto_kat::load();
            assert_eq!(format!("{:?}", kat.keypair()), "KemKeypair { .. }");
        }
    }

    #[test]
    fn decrypt_v2_refuses_an_unknown_aead_or_kem_label() {
        let key = random_dek();
        let envelope = seal_v2(
            "alg",
            [1u8; 32],
            b"x",
            VaultCryptoMode::Classical,
            &key,
            None,
        )
        .unwrap();
        let mut other_aead = envelope.clone();
        other_aead.aead_alg = "aes-256-gcm".to_string();
        assert!(decrypt_v2(&other_aead, &key, None).is_err());
        let mut other_kem = envelope;
        other_kem.kem_alg = "kyber768".to_string();
        assert!(decrypt_v2(&other_kem, &key, None).is_err());
    }
}
