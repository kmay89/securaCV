//! A custody holder's recipient key (spec §2.5): the X25519 key (and, for
//! box 2, the ML-KEM-768 key) that shares are sealed to, bound to the
//! holder's signing credential by a signature.
//!
//! - The public file `<holder>.recipient` (at most 8 KiB, mode 0644) is one
//!   line of JSON, `securacv-custody-recipient:v1`, with the binding
//!   signature over `recipient_key_hash` under
//!   `securacv:pwk:custody-recipient-binding:v1`. Without that signature an
//!   operator could enroll their own recipient key under a holder's name
//!   and receive the holder's share.
//! - The secret file `<holder>.reckey` is one line,
//!   `securacv-custody-recipient-key:v1 <hex of TEK1>`, mode 0600: the MKG1
//!   keyguard's KDF and AEAD (Argon2id 65536/3/1, ChaCha20-Poly1305) in a
//!   layout of its own, because it seals more than 32 bytes:
//!
//! ```text
//! "TEK1" ‖ 0x01 version ‖ 0x01 kind (Argon2id)
//! ‖ m u32le (65536) ‖ t u32le (3) ‖ p u32le (1)       refused BEFORE the KDF if different
//! ‖ salt_len u8 (16) ‖ salt[16] ‖ kek_commit[32]       kek_commit = SHA-256(Argon2id output)
//! ‖ holder_len u8 (1..=64) ‖ holder ‖ role u8 (0x01 trustee | 0x02 recovery)
//! ‖ signing_public_key[32] ‖ recipient_key_hash[32]
//! ‖ nonce[12] ‖ ct_len u32le (49 | 3633) ‖ ct
//! AAD       = "securacv-custody-recipient-key-v1" ‖ every byte before the nonce
//! plaintext = x25519_secret[32] ‖ (0x00 | 0x01 ‖ mlkem768_public[1184] ‖ mlkem768_secret[2400])
//! ```
//!
//! The header carries the role and signing key so that opening the file
//! recomputes `recipient_key_hash` from the decrypted keys with no other
//! input, and refuses a file whose keys do not match it.

use std::fmt;

use ed25519_dalek::Signature;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use super::group::{random_array, CustodyRng};
use super::hpke::{derive_keypair, x25519_public, MLKEM768_PK_LEN, MLKEM768_SK_LEN};
use super::{
    frame, holder_id_ok, CustodyError, RECIPIENT_DOMAIN, RECIPIENT_FORMAT,
    RECIPIENT_KEY_TEXT_PREFIX, TEK1_AAD_DOMAIN, TEK1_MAGIC,
};
use crate::crypto::signatures::{
    verify_ed25519_only, verifying_key_strict, DOMAIN_CUSTODY_RECIPIENT_BINDING,
};
use crate::vault::crypto::{
    decrypt_payload, encrypt_payload, keyguard_kek, keyguard_params_pinned, KEYGUARD_M_KIB,
    KEYGUARD_P, KEYGUARD_SALT_LEN, KEYGUARD_T,
};

/// Cap on a recipient public file, checked before it is parsed.
pub(crate) const RECIPIENT_FILE_MAX: usize = 8_192;
/// Cap on a recipient secret file's text, checked before it is parsed.
pub(crate) const RECIPIENT_KEY_FILE_MAX: usize = 8_192;
/// A holder that serves unseals (`"trustee"`).
pub(crate) const HOLDER_ROLE_TRUSTEE: u8 = 0x01;
/// A holder of the recovery split (`"recovery"`).
pub(crate) const HOLDER_ROLE_RECOVERY: u8 = 0x02;
const TEK1_CT_CLASSICAL: u32 = 32 + 1 + 16;
const TEK1_CT_PQ: u32 = (32 + 1 + MLKEM768_PK_LEN + MLKEM768_SK_LEN + 16) as u32;

/// The name a recipient file gives a holder role.
pub(crate) fn role_name(role: u8) -> &'static str {
    if role == HOLDER_ROLE_RECOVERY {
        "recovery"
    } else {
        "trustee"
    }
}

fn role_byte(name: &str) -> Option<u8> {
    match name {
        "trustee" => Some(HOLDER_ROLE_TRUSTEE),
        "recovery" => Some(HOLDER_ROLE_RECOVERY),
        _ => None,
    }
}

/// `recipient_key_hash = SHA-256(frame("securacv:custody:recipient:v1") ‖
/// frame(holder) ‖ role ‖ signing_public_key ‖ x25519_public ‖ (0x00 | 0x01
/// ‖ SHA-256(mlkem768_public)))`.
pub(crate) fn recipient_key_hash(
    holder: &str,
    role: u8,
    signing_public_key: &[u8; 32],
    x25519: &[u8; 32],
    mlkem768_public: Option<&[u8]>,
) -> [u8; 32] {
    let mut buf = Vec::with_capacity(160);
    frame(&mut buf, RECIPIENT_DOMAIN);
    frame(&mut buf, holder.as_bytes());
    buf.push(role);
    buf.extend_from_slice(signing_public_key);
    buf.extend_from_slice(x25519);
    match mlkem768_public {
        None => buf.push(0x00),
        Some(pk) => {
            buf.push(0x01);
            buf.extend_from_slice(&Sha256::digest(pk));
        }
    }
    Sha256::digest(&buf).into()
}

/// The fingerprint read aloud at a ceremony: the first 8 bytes of
/// `recipient_key_hash` as `xxxxxxxx xxxxxxxx`.
pub(crate) fn fingerprint(hash: &[u8; 32]) -> String {
    format!("{} {}", hex::encode(&hash[..4]), hex::encode(&hash[4..8]))
}

fn hex_field<const N: usize>(s: &str, why: &'static str) -> Result<[u8; N], CustodyError> {
    if s.len() != 2 * N || !s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')) {
        return Err(CustodyError::Recipient(why));
    }
    let mut out = [0u8; N];
    hex::decode_to_slice(s, &mut out).map_err(|_| CustodyError::Recipient(why))?;
    Ok(out)
}

/// The recipient public file. Strings are checked by the accessors, so a
/// parsed file is never used before [`RecipientPublic::key_hash`] or a
/// typed accessor has validated it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct RecipientPublic {
    format: String,
    holder: String,
    role: String,
    signing_public_key: String,
    x25519: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    mlkem768: Option<String>,
    binding_signature: String,
}

impl RecipientPublic {
    /// Parse a recipient file: at most 8 KiB, unknown fields refused, then
    /// every field checked.
    pub(crate) fn from_json(bytes: &[u8]) -> Result<Self, CustodyError> {
        if bytes.len() > RECIPIENT_FILE_MAX {
            return Err(CustodyError::Recipient("longer than 8192 bytes"));
        }
        let parsed: RecipientPublic = serde_json::from_slice(bytes).map_err(|_| {
            CustodyError::Recipient("not a securacv-custody-recipient:v1 JSON object")
        })?;
        parsed.key_hash()?;
        hex_field::<64>(
            &parsed.binding_signature,
            "binding_signature must be 128 lowercase hex characters",
        )?;
        Ok(parsed)
    }

    /// The file's one line: compact JSON in field order, then LF.
    pub(crate) fn to_json_text(&self) -> String {
        format!(
            "{}\n",
            serde_json::to_string(self).expect("strings serialize")
        )
    }

    /// The holder id.
    pub(crate) fn holder(&self) -> &str {
        &self.holder
    }

    /// `0x01` trustee or `0x02` recovery.
    pub(crate) fn role(&self) -> Result<u8, CustodyError> {
        role_byte(&self.role).ok_or(CustodyError::Recipient("role must be trustee or recovery"))
    }

    /// The holder's Ed25519 signing public key.
    pub(crate) fn signing_public_key(&self) -> Result<[u8; 32], CustodyError> {
        hex_field(
            &self.signing_public_key,
            "signing_public_key must be 64 lowercase hex characters",
        )
    }

    /// The X25519 recipient public key.
    pub(crate) fn x25519_public(&self) -> Result<[u8; 32], CustodyError> {
        hex_field(&self.x25519, "x25519 must be 64 lowercase hex characters")
    }

    /// The ML-KEM-768 recipient public key, present for box 2.
    pub(crate) fn mlkem768_public(&self) -> Result<Option<Vec<u8>>, CustodyError> {
        match &self.mlkem768 {
            None => Ok(None),
            Some(h) => Ok(Some(
                hex_field::<MLKEM768_PK_LEN>(h, "mlkem768 must be 2368 lowercase hex characters")?
                    .to_vec(),
            )),
        }
    }

    /// `recipient_key_hash` of this file, after checking every field it
    /// covers.
    pub(crate) fn key_hash(&self) -> Result<[u8; 32], CustodyError> {
        if self.format != RECIPIENT_FORMAT {
            return Err(CustodyError::Recipient(
                "not a securacv-custody-recipient:v1 file",
            ));
        }
        if !holder_id_ok(&self.holder) {
            return Err(CustodyError::Recipient(
                "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
            ));
        }
        Ok(recipient_key_hash(
            &self.holder,
            self.role()?,
            &self.signing_public_key()?,
            &self.x25519_public()?,
            self.mlkem768_public()?.as_deref(),
        ))
    }

    /// The fingerprint of [`Self::key_hash`].
    pub(crate) fn fingerprint(&self) -> Result<String, CustodyError> {
        Ok(fingerprint(&self.key_hash()?))
    }

    /// Check the binding signature against the file's own signing key.
    /// (From the custody epoch on, a trustee's binding is checked through
    /// the policy's credential instead.)
    pub(crate) fn check_binding_ed25519(&self) -> Result<(), CustodyError> {
        let refused = CustodyError::Recipient(
            "its binding signature does not check against its own signing key",
        );
        let hash = self.key_hash()?;
        let sig: [u8; 64] = hex_field(
            &self.binding_signature,
            "binding_signature must be 128 lowercase hex characters",
        )?;
        let vk = verifying_key_strict(&self.signing_public_key()?).map_err(|_| refused.clone())?;
        verify_ed25519_only(DOMAIN_CUSTODY_RECIPIENT_BINDING, &vk, &hash, &sig).map_err(|_| refused)
    }

    /// This file with `binding_signature` set.
    pub(crate) fn with_binding(mut self, signature: &[u8]) -> Self {
        self.binding_signature = hex::encode(signature);
        self
    }

    /// Whether a signature is a well-formed 64 bytes (the parsed check).
    #[cfg(test)]
    pub(crate) fn binding_signature_bytes(&self) -> Option<[u8; 64]> {
        Signature::from_slice(&hex::decode(&self.binding_signature).ok()?)
            .ok()
            .map(|s| s.to_bytes())
    }
}

/// A recipient secret: the X25519 secret and, for box 2, the ML-KEM-768
/// key pair. No `Clone`, no `Serialize`, no `PartialEq`; `Debug` prints
/// `<redacted>`.
pub(crate) struct RecipientSecret {
    x25519: Zeroizing<[u8; 32]>,
    mlkem: Option<(Vec<u8>, Zeroizing<Vec<u8>>)>,
    holder: String,
    role: u8,
}

impl fmt::Debug for RecipientSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "RecipientSecret('{}', <redacted>)", self.holder)
    }
}

/// The checked public header of a TEK1 container.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Tek1Header {
    /// The holder id the file names.
    pub(crate) holder: String,
    /// `0x01` trustee or `0x02` recovery.
    pub(crate) role: u8,
    /// The holder's Ed25519 signing public key.
    pub(crate) signing_public_key: [u8; 32],
    /// The recipient key hash the decrypted keys must reproduce.
    pub(crate) recipient_key_hash: [u8; 32],
    salt: [u8; 16],
    kek_commit: [u8; 32],
    nonce: [u8; 12],
    aad_len: usize,
}

/// Parse a TEK1 container's header, refusing a hostile file before any
/// key derivation: magic, version, kind, the pinned Argon2id parameters,
/// the salt length, the holder id, the role, the ciphertext length and the
/// exact total length.
pub(crate) fn parse_tek1(bytes: &[u8]) -> Result<Tek1Header, CustodyError> {
    let short = CustodyError::RecipientKey("truncated");
    let get = |at: usize, n: usize| bytes.get(at..at + n).ok_or(short.clone());
    if get(0, 4)? != TEK1_MAGIC {
        return Err(CustodyError::RecipientKey("bad magic"));
    }
    let version = get(4, 1)?[0];
    if version != 1 {
        return Err(CustodyError::RecipientKeyVersion(version));
    }
    if get(5, 1)?[0] != 1 {
        return Err(CustodyError::RecipientKey("unsupported kind"));
    }
    let u32_at = |at: usize| -> Result<u32, CustodyError> {
        Ok(u32::from_le_bytes(get(at, 4)?.try_into().expect("4 bytes")))
    };
    if !keyguard_params_pinned(u32_at(6)?, u32_at(10)?, u32_at(14)?) {
        return Err(CustodyError::RecipientKey("unexpected TEK1 v1 parameters"));
    }
    if usize::from(get(18, 1)?[0]) != KEYGUARD_SALT_LEN {
        return Err(CustodyError::RecipientKey("unexpected salt length"));
    }
    let salt: [u8; 16] = get(19, 16)?.try_into().expect("16 bytes");
    let kek_commit: [u8; 32] = get(35, 32)?.try_into().expect("32 bytes");
    let holder_len = usize::from(get(67, 1)?[0]);
    let holder = std::str::from_utf8(get(68, holder_len)?)
        .ok()
        .filter(|h| holder_id_ok(h))
        .ok_or(CustodyError::RecipientKey(
            "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
        ))?
        .to_string();
    let at = 68 + holder_len;
    let role = get(at, 1)?[0];
    if role != HOLDER_ROLE_TRUSTEE && role != HOLDER_ROLE_RECOVERY {
        return Err(CustodyError::RecipientKey(
            "role must be trustee or recovery",
        ));
    }
    let signing_public_key: [u8; 32] = get(at + 1, 32)?.try_into().expect("32 bytes");
    let recipient_key_hash: [u8; 32] = get(at + 33, 32)?.try_into().expect("32 bytes");
    let aad_len = at + 65;
    let nonce: [u8; 12] = get(aad_len, 12)?.try_into().expect("12 bytes");
    let ct_len = u32_at(aad_len + 12)?;
    if ct_len != TEK1_CT_CLASSICAL && ct_len != TEK1_CT_PQ {
        return Err(CustodyError::RecipientKey("unexpected ciphertext length"));
    }
    let end = aad_len + 16 + ct_len as usize;
    if bytes.len() < end {
        return Err(short);
    }
    if bytes.len() > end {
        return Err(CustodyError::RecipientKey("trailing bytes"));
    }
    Ok(Tek1Header {
        holder,
        role,
        signing_public_key,
        recipient_key_hash,
        salt,
        kek_commit,
        nonce,
        aad_len,
    })
}

/// The bytes of a recipient secret file's line (`securacv-custody-recipient-key:v1 <hex>`).
pub(crate) fn tek1_from_text(text: &[u8]) -> Result<Vec<u8>, CustodyError> {
    if text.len() > RECIPIENT_KEY_FILE_MAX {
        return Err(CustodyError::RecipientKey("longer than 8192 bytes"));
    }
    let not_one = CustodyError::RecipientKey("not a securacv-custody-recipient-key:v1 file");
    let text = std::str::from_utf8(text).map_err(|_| not_one.clone())?;
    let body = text
        .strip_prefix(RECIPIENT_KEY_TEXT_PREFIX)
        .and_then(|b| b.strip_prefix(' '))
        .and_then(|b| b.strip_suffix('\n'))
        .ok_or(not_one)?;
    if body.is_empty()
        || body.len() % 2 != 0
        || !body.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
    {
        return Err(CustodyError::RecipientKey("the hex form is malformed"));
    }
    hex::decode(body).map_err(|_| CustodyError::RecipientKey("the hex form is malformed"))
}

impl RecipientSecret {
    /// A fresh recipient key for `holder`: the X25519 secret is HPKE
    /// `DeriveKeyPair` of 32 random bytes; `pq` adds an ML-KEM-768 key pair
    /// (a `pqc-vault` build only).
    pub(crate) fn generate(
        holder: &str,
        role: u8,
        pq: bool,
        rng: &mut dyn CustodyRng,
    ) -> Result<Self, CustodyError> {
        if !holder_id_ok(holder) {
            return Err(CustodyError::Recipient(
                "holder id must be 1 to 64 bytes of [A-Za-z0-9._-]",
            ));
        }
        let ikm = Zeroizing::new(random_array::<32>(rng));
        let (x25519, _) = derive_keypair(ikm.as_ref());
        let mlkem = if pq { Some(generate_mlkem()?) } else { None };
        Ok(RecipientSecret {
            x25519,
            mlkem,
            holder: holder.to_string(),
            role,
        })
    }

    /// A recipient secret from known parts (the known-answer tests).
    #[cfg(test)]
    pub(crate) fn from_parts(
        holder: &str,
        role: u8,
        x25519: [u8; 32],
        mlkem: Option<(Vec<u8>, Vec<u8>)>,
    ) -> Self {
        RecipientSecret {
            x25519: Zeroizing::new(x25519),
            mlkem: mlkem.map(|(pk, sk)| (pk, Zeroizing::new(sk))),
            holder: holder.to_string(),
            role,
        }
    }

    /// The holder id.
    pub(crate) fn holder(&self) -> &str {
        &self.holder
    }

    /// `0x01` trustee or `0x02` recovery.
    pub(crate) fn role(&self) -> u8 {
        self.role
    }

    /// The X25519 secret.
    pub(crate) fn x25519_secret(&self) -> &[u8; 32] {
        &self.x25519
    }

    /// The X25519 public key.
    pub(crate) fn x25519_public(&self) -> [u8; 32] {
        x25519_public(&self.x25519)
    }

    /// The ML-KEM-768 key pair, if any.
    pub(crate) fn mlkem(&self) -> Option<(&[u8], &[u8])> {
        self.mlkem
            .as_ref()
            .map(|(pk, sk)| (pk.as_slice(), sk.as_slice()))
    }

    /// `recipient_key_hash` of this key under `signing_public_key`.
    pub(crate) fn key_hash(&self, signing_public_key: &[u8; 32]) -> [u8; 32] {
        recipient_key_hash(
            &self.holder,
            self.role,
            signing_public_key,
            &self.x25519_public(),
            self.mlkem.as_ref().map(|(pk, _)| pk.as_slice()),
        )
    }

    /// The public file, without its binding signature: the caller signs
    /// [`RecipientPublic::key_hash`] under
    /// `DOMAIN_CUSTODY_RECIPIENT_BINDING` and sets it with
    /// [`RecipientPublic::with_binding`].
    pub(crate) fn public_unsigned(&self, signing_public_key: &[u8; 32]) -> RecipientPublic {
        RecipientPublic {
            format: RECIPIENT_FORMAT.to_string(),
            holder: self.holder.clone(),
            role: role_name(self.role).to_string(),
            signing_public_key: hex::encode(signing_public_key),
            x25519: hex::encode(self.x25519_public()),
            mlkem768: self.mlkem.as_ref().map(|(pk, _)| hex::encode(pk)),
            binding_signature: String::new(),
        }
    }

    fn plaintext(&self) -> Zeroizing<Vec<u8>> {
        let mut pt = Zeroizing::new(Vec::with_capacity(TEK1_CT_PQ as usize));
        pt.extend_from_slice(self.x25519.as_ref());
        match &self.mlkem {
            None => pt.push(0x00),
            Some((pk, sk)) => {
                pt.push(0x01);
                pt.extend_from_slice(pk);
                pt.extend_from_slice(sk);
            }
        }
        pt
    }

    /// TEK1 bytes under a given Argon2id output, salt and nonce (the KAT
    /// seam: the AEAD layer without the KDF).
    pub(crate) fn wrap_with_kek(
        &self,
        signing_public_key: &[u8; 32],
        argon_out: &[u8; 32],
        salt: &[u8; 16],
        nonce: &[u8; 12],
    ) -> Result<Vec<u8>, CustodyError> {
        let recorded = self.key_hash(signing_public_key);
        self.encode_tek1(signing_public_key, &recorded, argon_out, salt, nonce)
    }

    /// TEK1 bytes recording `recorded_hash` (tests pass a wrong one).
    fn encode_tek1(
        &self,
        signing_public_key: &[u8; 32],
        recorded_hash: &[u8; 32],
        argon_out: &[u8; 32],
        salt: &[u8; 16],
        nonce: &[u8; 12],
    ) -> Result<Vec<u8>, CustodyError> {
        let mut out = Vec::with_capacity(160 + TEK1_CT_PQ as usize);
        out.extend_from_slice(TEK1_MAGIC);
        out.push(1);
        out.push(1);
        out.extend_from_slice(&KEYGUARD_M_KIB.to_le_bytes());
        out.extend_from_slice(&KEYGUARD_T.to_le_bytes());
        out.extend_from_slice(&KEYGUARD_P.to_le_bytes());
        out.push(KEYGUARD_SALT_LEN as u8);
        out.extend_from_slice(salt);
        out.extend_from_slice(&Sha256::digest(argon_out));
        out.push(self.holder.len() as u8);
        out.extend_from_slice(self.holder.as_bytes());
        out.push(self.role);
        out.extend_from_slice(signing_public_key);
        out.extend_from_slice(recorded_hash);
        let mut aad = TEK1_AAD_DOMAIN.to_vec();
        aad.extend_from_slice(&out);
        let mut ct = self.plaintext();
        let tag = encrypt_payload(argon_out, nonce, &aad, &mut ct)
            .map_err(|_| CustodyError::RecipientKey("encryption failed"))?;
        out.extend_from_slice(nonce);
        out.extend_from_slice(&((ct.len() + 16) as u32).to_le_bytes());
        out.extend_from_slice(&ct);
        out.extend_from_slice(&tag);
        Ok(out)
    }

    /// Wrap under `passphrase` (Argon2id 65536/3/1, fresh salt and nonce)
    /// and return the file's one line.
    pub(crate) fn wrap(
        &self,
        signing_public_key: &[u8; 32],
        passphrase: &str,
        rng: &mut dyn CustodyRng,
    ) -> Result<String, CustodyError> {
        let salt: [u8; 16] = random_array(rng);
        let nonce: [u8; 12] = random_array(rng);
        let kek = keyguard_kek(passphrase, &salt, KEYGUARD_M_KIB, KEYGUARD_T, KEYGUARD_P)
            .map_err(|_| CustodyError::RecipientKey("key derivation failed"))?;
        let tek1 = self.wrap_with_kek(signing_public_key, &kek, &salt, &nonce)?;
        Ok(format!(
            "{RECIPIENT_KEY_TEXT_PREFIX} {}\n",
            hex::encode(tek1)
        ))
    }

    /// Open TEK1 bytes with a given Argon2id output (the KAT seam).
    pub(crate) fn open_with_kek(
        tek1: &[u8],
        argon_out: &[u8; 32],
    ) -> Result<(Self, Tek1Header), CustodyError> {
        let header = parse_tek1(tek1)?;
        let commit: [u8; 32] = Sha256::digest(argon_out).into();
        if !bool::from(commit.ct_eq(&header.kek_commit)) {
            return Err(CustodyError::RecipientKey("wrong passphrase"));
        }
        let mut aad = TEK1_AAD_DOMAIN.to_vec();
        aad.extend_from_slice(&tek1[..header.aad_len]);
        let ct = &tek1[header.aad_len + 16..];
        let (body, tag) = ct.split_at(ct.len() - 16);
        let mut pt = Zeroizing::new(body.to_vec());
        decrypt_payload(argon_out, &header.nonce, &aad, &mut pt, tag).map_err(|_| {
            CustodyError::RecipientKey("authentication failed — the file was altered or damaged")
        })?;
        let mut x25519 = Zeroizing::new([0u8; 32]);
        x25519.copy_from_slice(&pt[..32]);
        let mlkem = match &pt[32..] {
            [0x00] => None,
            [0x01, rest @ ..] if rest.len() == MLKEM768_PK_LEN + MLKEM768_SK_LEN => Some((
                rest[..MLKEM768_PK_LEN].to_vec(),
                Zeroizing::new(rest[MLKEM768_PK_LEN..].to_vec()),
            )),
            _ => {
                return Err(CustodyError::RecipientKey(
                    "the decrypted keys are malformed",
                ))
            }
        };
        let secret = RecipientSecret {
            x25519,
            mlkem,
            holder: header.holder.clone(),
            role: header.role,
        };
        if secret.key_hash(&header.signing_public_key) != header.recipient_key_hash {
            return Err(CustodyError::RecipientKey(
                "its keys do not match its recorded recipient hash",
            ));
        }
        Ok((secret, header))
    }

    /// Open a recipient secret file's text under `passphrase`. The header
    /// is checked, and the Argon2id parameters pinned, before the KDF runs.
    pub(crate) fn unwrap(
        text: &[u8],
        passphrase: &str,
    ) -> Result<(Self, Tek1Header), CustodyError> {
        let tek1 = tek1_from_text(text)?;
        let header = parse_tek1(&tek1)?;
        let kek = keyguard_kek(
            passphrase,
            &header.salt,
            KEYGUARD_M_KIB,
            KEYGUARD_T,
            KEYGUARD_P,
        )
        .map_err(|_| CustodyError::RecipientKey("key derivation failed"))?;
        Self::open_with_kek(&tek1, &kek)
    }
}

#[cfg(feature = "pqc-vault")]
fn generate_mlkem() -> Result<(Vec<u8>, Zeroizing<Vec<u8>>), CustodyError> {
    use pqcrypto_traits::kem::{PublicKey as _, SecretKey as _};
    let (pk, sk) = pqcrypto_mlkem::mlkem768::keypair();
    Ok((
        pk.as_bytes().to_vec(),
        Zeroizing::new(sk.as_bytes().to_vec()),
    ))
}

#[cfg(not(feature = "pqc-vault"))]
fn generate_mlkem() -> Result<(Vec<u8>, Zeroizing<Vec<u8>>), CustodyError> {
    Err(CustodyError::Box2NeedsFeature)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::crypto::signatures::sign_ed25519_only;
    use crate::vault::crypto::{keyguard_open, keyguard_seal_with, KEYGUARD_MKG1, KEYGUARD_TKG1};
    use crate::vault::custody::group::OsCustodyRng;
    use ed25519_dalek::SigningKey;

    /// A signed public file for `secret` under `seed`.
    pub(crate) fn signed_public(secret: &RecipientSecret, seed: &[u8; 32]) -> RecipientPublic {
        let key = SigningKey::from_bytes(seed);
        let unsigned = secret.public_unsigned(&key.verifying_key().to_bytes());
        let hash = unsigned.key_hash().unwrap();
        let sig = sign_ed25519_only(DOMAIN_CUSTODY_RECIPIENT_BINDING, &key, &hash);
        unsigned.with_binding(&sig)
    }

    pub(crate) fn recipient_secret_debug_is_redacted() {
        let secret = RecipientSecret::from_parts("alice", HOLDER_ROLE_TRUSTEE, [0x42; 32], None);
        let shown = format!("{secret:?}");
        assert_eq!(shown, "RecipientSecret('alice', <redacted>)");
        assert!(!shown.contains("4242"));
    }

    #[test]
    fn tek1_round_trip_wrong_passphrase_and_param_refusal() {
        let mut rng = OsCustodyRng;
        let secret =
            RecipientSecret::generate("alice", HOLDER_ROLE_TRUSTEE, false, &mut rng).unwrap();
        let signing_pk = SigningKey::from_bytes(&[0x11; 32])
            .verifying_key()
            .to_bytes();
        let text = secret
            .wrap(&signing_pk, "recipient pass", &mut rng)
            .unwrap();
        assert!(text.starts_with("securacv-custody-recipient-key:v1 "));
        assert!(text.ends_with('\n'));
        let (opened, header) = RecipientSecret::unwrap(text.as_bytes(), "recipient pass").unwrap();
        assert_eq!(opened.x25519_secret(), secret.x25519_secret());
        assert_eq!(header.holder, "alice");
        assert_eq!(header.role, HOLDER_ROLE_TRUSTEE);
        assert_eq!(header.signing_public_key, signing_pk);
        assert_eq!(header.recipient_key_hash, secret.key_hash(&signing_pk));
        assert_eq!(
            RecipientSecret::unwrap(text.as_bytes(), "not it")
                .unwrap_err()
                .to_string(),
            "recipient key file: wrong passphrase"
        );
        let tek1 = tek1_from_text(text.as_bytes()).unwrap();
        // m = 9999999 is refused before Argon2id runs: a KDF at that
        // memory cost would need about 10 GB.
        let mut big_m = tek1.clone();
        big_m[6..10].copy_from_slice(&9_999_999u32.to_le_bytes());
        let line = format!(
            "securacv-custody-recipient-key:v1 {}\n",
            hex::encode(&big_m)
        );
        assert_eq!(
            RecipientSecret::unwrap(line.as_bytes(), "recipient pass")
                .unwrap_err()
                .to_string(),
            "recipient key file: unexpected TEK1 v1 parameters"
        );
        // A renamed holder fails the AEAD (the header is its AAD).
        let kek = keyguard_kek(
            "recipient pass",
            &tek1[19..35],
            KEYGUARD_M_KIB,
            KEYGUARD_T,
            KEYGUARD_P,
        )
        .unwrap();
        let mut renamed = tek1.clone();
        renamed[68] = b'b';
        assert_eq!(
            RecipientSecret::open_with_kek(&renamed, &kek)
                .unwrap_err()
                .to_string(),
            "recipient key file: authentication failed — the file was altered or damaged"
        );
        // Keys that do not reproduce the recorded hash are refused: a file
        // whose header records alice's hash but seals other keys.
        let other = RecipientSecret::from_parts("alice", HOLDER_ROLE_TRUSTEE, [9; 32], None);
        let lying = other
            .encode_tek1(
                &signing_pk,
                &secret.key_hash(&signing_pk),
                &kek,
                &[1; 16],
                &[2; 12],
            )
            .unwrap();
        assert_eq!(
            RecipientSecret::open_with_kek(&lying, &kek)
                .unwrap_err()
                .to_string(),
            "recipient key file: its keys do not match its recorded recipient hash"
        );
        // Text-form refusals.
        assert!(tek1_from_text(text.to_uppercase().as_bytes()).is_err());
        assert!(tek1_from_text(text.trim_end().as_bytes()).is_err());
        let mut trailing = tek1.clone();
        trailing.push(0);
        assert_eq!(
            parse_tek1(&trailing).unwrap_err(),
            CustodyError::RecipientKey("trailing bytes")
        );
        let mut v2 = tek1;
        v2[4] = 2;
        assert_eq!(
            parse_tek1(&v2).unwrap_err().to_string(),
            "recipient key file: unsupported version 2"
        );
    }

    #[test]
    fn binding_signature_checks_and_rejects_other_key() {
        let secret =
            RecipientSecret::generate("bob", HOLDER_ROLE_TRUSTEE, false, &mut OsCustodyRng)
                .unwrap();
        let public = signed_public(&secret, &[0x22; 32]);
        public.check_binding_ed25519().unwrap();
        assert_eq!(
            public.key_hash().unwrap(),
            secret.key_hash(&public.signing_public_key().unwrap())
        );
        assert!(public.binding_signature_bytes().is_some());
        let refusal =
            "recipient file: its binding signature does not check against its own signing key";
        // Signed by another key, but naming bob's.
        let bob_pk = SigningKey::from_bytes(&[0x22; 32])
            .verifying_key()
            .to_bytes();
        let unsigned = secret.public_unsigned(&bob_pk);
        let forged = sign_ed25519_only(
            DOMAIN_CUSTODY_RECIPIENT_BINDING,
            &SigningKey::from_bytes(&[0x33; 32]),
            &unsigned.key_hash().unwrap(),
        );
        assert_eq!(
            unsigned
                .clone()
                .with_binding(&forged)
                .check_binding_ed25519()
                .unwrap_err()
                .to_string(),
            refusal
        );
        // An operator who substitutes their own recipient key under bob's
        // name and signing key cannot produce bob's signature.
        let operator =
            RecipientSecret::generate("bob", HOLDER_ROLE_TRUSTEE, false, &mut OsCustodyRng)
                .unwrap();
        let swapped = operator
            .public_unsigned(&bob_pk)
            .with_binding(&public.binding_signature_bytes().unwrap());
        assert_eq!(
            swapped.check_binding_ed25519().unwrap_err().to_string(),
            refusal
        );
        // The right key under the wrong domain does not bind.
        let wrong_domain = sign_ed25519_only(
            "securacv:pwk:trustee-approval:v2",
            &SigningKey::from_bytes(&[0x22; 32]),
            &unsigned.key_hash().unwrap(),
        );
        assert!(unsigned
            .clone()
            .with_binding(&wrong_domain)
            .check_binding_ed25519()
            .is_err());
        // A role change is a different hash.
        let mut json: serde_json::Value = serde_json::from_str(&public.to_json_text()).unwrap();
        json["role"] = "recovery".into();
        let recovery: RecipientPublic = serde_json::from_value(json).unwrap();
        assert!(recovery.check_binding_ed25519().is_err());
        // The text form round-trips.
        assert_eq!(
            RecipientPublic::from_json(public.to_json_text().as_bytes()).unwrap(),
            public
        );
    }

    #[test]
    fn recipient_json_rejects_unknown_fields() {
        let secret =
            RecipientSecret::generate("carol", HOLDER_ROLE_RECOVERY, false, &mut OsCustodyRng)
                .unwrap();
        let public = signed_public(&secret, &[0x33; 32]);
        let text = public.to_json_text();
        assert!(RecipientPublic::from_json(text.as_bytes()).is_ok());
        let extra = text.replacen('{', "{\"comment\":\"hi\",", 1);
        assert_eq!(
            RecipientPublic::from_json(extra.as_bytes())
                .unwrap_err()
                .to_string(),
            "recipient file: not a securacv-custody-recipient:v1 JSON object"
        );
        let wrong_format = text.replace(
            "securacv-custody-recipient:v1",
            "securacv-custody-recipient:v2",
        );
        assert_eq!(
            RecipientPublic::from_json(wrong_format.as_bytes())
                .unwrap_err()
                .to_string(),
            "recipient file: not a securacv-custody-recipient:v1 file"
        );
        let upper = text.replace(
            &public.signing_public_key,
            &public.signing_public_key.to_uppercase(),
        );
        assert!(RecipientPublic::from_json(upper.as_bytes()).is_err());
        let bad_role = text.replace("\"recovery\"", "\"witness\"");
        assert_eq!(
            RecipientPublic::from_json(bad_role.as_bytes())
                .unwrap_err()
                .to_string(),
            "recipient file: role must be trustee or recovery"
        );
        let mut huge = text.into_bytes();
        huge.resize(8193, b' ');
        assert_eq!(
            RecipientPublic::from_json(&huge).unwrap_err().to_string(),
            "recipient file: longer than 8192 bytes"
        );
    }

    /// The MKG1 container captured before the keyguard recipe was shared
    /// over (magic, AAD domain), as `vault::crypto`'s own test pins it.
    const MKG1_PRE_REFACTOR_HEX: &str = "4d4b4731010100000100030000000100000010474045a42a873ce25ef20c557064eecf4891775d3b0097d3399c16062abb6be70e3e52d7267fb02d7412d4e390200a7b763858fb9d5dfa4fa7f447612c16d88126887cd2f6b13ce1b87f126aa3a0898494abcf8f31adc4c5e06fa7c74559e07fdce960735174389deb0ca8f8";

    #[test]
    fn mkg1_bytes_unchanged_by_refactor() {
        // The shared recipe still writes MKG1 byte for byte...
        let frozen = hex::decode(MKG1_PRE_REFACTOR_HEX).unwrap();
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
        // ...and TEK1, which reuses its KDF and AEAD in a layout of its
        // own, is not an MKG1 or a TKG1: each reader refuses the others'
        // magic before deriving anything.
        let secret = RecipientSecret::from_parts("alice", HOLDER_ROLE_TRUSTEE, [1; 32], None);
        let tek1 = secret
            .wrap_with_kek(&[2; 32], &[3; 32], &salt, &nonce)
            .unwrap();
        assert_eq!(
            &tek1[4..19],
            &frozen[4..19],
            "the same version, kind and Argon2id parameters"
        );
        assert_eq!(
            keyguard_open(&KEYGUARD_MKG1, &tek1, "x", b"")
                .unwrap_err()
                .to_string(),
            "vault keyguard: bad magic"
        );
        assert_eq!(
            keyguard_open(&KEYGUARD_TKG1, &tek1, "x", b"")
                .unwrap_err()
                .to_string(),
            "trustee key: bad magic"
        );
        assert_eq!(
            parse_tek1(&frozen).unwrap_err(),
            CustodyError::RecipientKey("bad magic")
        );
        assert_ne!(TEK1_AAD_DOMAIN, KEYGUARD_MKG1.aad_domain);
        assert_ne!(TEK1_AAD_DOMAIN, KEYGUARD_TKG1.aad_domain);
    }
}
