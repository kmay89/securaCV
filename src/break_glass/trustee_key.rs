//! Trustee signing keys at rest, and the signer seam.
//!
//! A trustee's Ed25519 seed is kept in a one-line text file,
//! `securacv-trustee-key:v1 <lowercase hex of TKG1>\n`, mode 0600. TKG1 is
//! the MKG1 keyguard layout (`crate::vault::crypto`, [`KEYGUARD_TKG1`])
//! followed by a public trailer that names whose key it is:
//!
//! ```text
//! "TKG1" ‖ 0x01 version ‖ 0x01 kind (Argon2id)
//! ‖ m u32le (65536) ‖ t u32le (3) ‖ p u32le (1)        refused before the KDF if different
//! ‖ 0x10 ‖ salt[16] ‖ kek_commit[32] = SHA-256(KEK)
//! ‖ nonce[12] ‖ ct[32] ‖ tag[16]                         ChaCha20-Poly1305 of the 32-byte seed
//! ‖ u32le(len id) ‖ id (1..=64 bytes) ‖ public_key[32]   the trailer
//! AAD = "securacv-trustee-key-v1" ‖ u32le(len id) ‖ id ‖ public_key[32]
//! ```
//!
//! The trailer is exactly the AAD suffix. It is stored so the file says
//! whose key it is without the passphrase, and it is authenticated: a
//! renamed trustee or a swapped public key fails the AEAD. After decryption
//! the seed must yield the recorded public key.
//!
//! The hub never sees this file and the quorum policy records nothing about
//! it: the hub cannot check that a trustee's key is wrapped, and a field it
//! cannot check would invite a false claim.

// The container lands one commit before the commands that use it.
#![allow(dead_code)]

use anyhow::{anyhow, Result};
use ed25519_dalek::SigningKey;
use zeroize::Zeroizing;

use crate::vault::crypto::{
    keyguard_open, keyguard_params_pinned, keyguard_seal, KEYGUARD_SALT_LEN, KEYGUARD_TKG1,
};

/// The prefix of a wrapped trustee key file's one line.
pub(crate) const TRUSTEE_KEY_FILE_PREFIX: &str = "securacv-trustee-key:v1 ";

/// The refusal when a wrapped trustee key must be opened and there is no
/// terminal to ask on and no `--passphrase-fd`. A passphrase is never taken
/// from argv or the environment.
pub(crate) const NO_PASSPHRASE_SOURCE: &str =
    "wrapped trustee key needs a passphrase: run on a terminal or pass --passphrase-fd";

/// The longest trustee id a TKG1 trailer carries, in bytes.
pub(crate) const MAX_TRUSTEE_KEY_ID_LEN: usize = 64;

/// Length of the MKG1-layout part of a TKG1 container (a 16-byte salt).
const TKG1_CONTAINER_LEN: usize = 4 + 1 + 1 + 12 + 1 + KEYGUARD_SALT_LEN + 32 + 12 + 32 + 16;

/// Refuse a trustee id a TKG1 trailer cannot carry: empty, longer than
/// [`MAX_TRUSTEE_KEY_ID_LEN`] bytes, or holding a control or
/// text-direction character.
pub(crate) fn check_trustee_key_id(id: &str) -> Result<()> {
    if id.is_empty() || id.len() > MAX_TRUSTEE_KEY_ID_LEN {
        return Err(anyhow!(
            "trustee id must be 1 to {MAX_TRUSTEE_KEY_ID_LEN} bytes"
        ));
    }
    crate::break_glass::check_text_field("trustee id", id)
}

/// The TKG1 AAD suffix, which is also its stored trailer:
/// `u32le(len id) ‖ id ‖ public_key[32]`.
pub(crate) fn tkg1_aad_suffix(id: &str, public_key: &[u8; 32]) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 + id.len() + 32);
    v.extend_from_slice(&(id.len() as u32).to_le_bytes());
    v.extend_from_slice(id.as_bytes());
    v.extend_from_slice(public_key);
    v
}

/// Wrap a trustee's 32-byte Ed25519 seed under `passphrase` as TKG1 bytes,
/// with a fresh salt and nonce.
pub(crate) fn tkg1_seal(seed: &[u8; 32], id: &str, passphrase: &str) -> Result<Vec<u8>> {
    check_trustee_key_id(id)?;
    let public_key = SigningKey::from_bytes(seed).verifying_key().to_bytes();
    let trailer = tkg1_aad_suffix(id, &public_key);
    let mut out = keyguard_seal(&KEYGUARD_TKG1, seed, passphrase, &trailer)?;
    out.extend_from_slice(&trailer);
    Ok(out)
}

/// A TKG1 container whose header and trailer were checked, not yet opened.
/// Holds no secret: the seed is still encrypted.
pub(crate) struct WrappedTrusteeKey {
    /// The trustee id the trailer names.
    pub(crate) id: String,
    /// The Ed25519 public key the trailer names.
    pub(crate) public_key: [u8; 32],
    container: Vec<u8>,
}

impl WrappedTrusteeKey {
    /// Check a TKG1 container's magic, version, kind, pinned Argon2id
    /// parameters, salt length and trailer. Nothing here derives a key, so
    /// a hostile file is refused before any passphrase is asked for.
    pub(crate) fn parse(bytes: &[u8]) -> Result<Self> {
        let label = KEYGUARD_TKG1.label;
        if bytes.len() < 6 {
            return Err(anyhow!("{label}: truncated"));
        }
        if &bytes[..4] != KEYGUARD_TKG1.magic {
            return Err(anyhow!("{label}: bad magic"));
        }
        if bytes[4] != 1 {
            return Err(anyhow!("{label}: unsupported version {}", bytes[4]));
        }
        if bytes[5] != 1 {
            return Err(anyhow!("{label}: unsupported kind {}", bytes[5]));
        }
        if bytes.len() < TKG1_CONTAINER_LEN + 4 {
            return Err(anyhow!("{label}: truncated"));
        }
        let u32_at = |at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().expect("4 bytes"));
        if !keyguard_params_pinned(u32_at(6), u32_at(10), u32_at(14)) {
            return Err(anyhow!("{label}: unexpected TKG1 v1 parameters"));
        }
        if bytes[18] as usize != KEYGUARD_SALT_LEN {
            return Err(anyhow!("{label}: unexpected salt length"));
        }
        let id_len = u32_at(TKG1_CONTAINER_LEN) as usize;
        if id_len == 0 || id_len > MAX_TRUSTEE_KEY_ID_LEN {
            return Err(anyhow!(
                "{label}: trustee id must be 1 to {MAX_TRUSTEE_KEY_ID_LEN} bytes"
            ));
        }
        let id_at = TKG1_CONTAINER_LEN + 4;
        let pk_at = id_at + id_len;
        if bytes.len() < pk_at + 32 {
            return Err(anyhow!("{label}: truncated"));
        }
        if bytes.len() > pk_at + 32 {
            return Err(anyhow!("{label}: trailing bytes after the public key"));
        }
        let id = std::str::from_utf8(&bytes[id_at..pk_at])
            .map_err(|_| anyhow!("{label}: trustee id is not UTF-8"))?
            .to_string();
        check_trustee_key_id(&id).map_err(|e| anyhow!("{label}: {e}"))?;
        let public_key: [u8; 32] = bytes[pk_at..].try_into().expect("32 bytes");
        Ok(Self {
            id,
            public_key,
            container: bytes.to_vec(),
        })
    }

    /// Decrypt the seed under `passphrase` and check that it yields the
    /// recorded public key.
    pub(crate) fn open(&self, passphrase: &str) -> Result<Zeroizing<[u8; 32]>> {
        let trailer = tkg1_aad_suffix(&self.id, &self.public_key);
        let seed = keyguard_open(
            &KEYGUARD_TKG1,
            &self.container[..TKG1_CONTAINER_LEN],
            passphrase,
            &trailer,
        )?;
        if SigningKey::from_bytes(&seed).verifying_key().to_bytes() != self.public_key {
            return Err(anyhow!(
                "trustee key: the decrypted seed does not yield the recorded public key"
            ));
        }
        Ok(seed)
    }
}

/// The one line a wrapped trustee key file holds.
pub(crate) fn trustee_key_file_text(tkg1: &[u8]) -> String {
    format!("{TRUSTEE_KEY_FILE_PREFIX}{}\n", hex::encode(tkg1))
}

/// True when `text` starts like a wrapped trustee key file.
pub(crate) fn is_wrapped_trustee_key_text(text: &str) -> bool {
    text.starts_with(TRUSTEE_KEY_FILE_PREFIX)
}

/// Parse a wrapped trustee key file's line into its checked container.
pub(crate) fn parse_trustee_key_file_text(text: &str) -> Result<WrappedTrusteeKey> {
    let body = text.strip_prefix(TRUSTEE_KEY_FILE_PREFIX).ok_or_else(|| {
        anyhow!("trustee key file: it does not start with securacv-trustee-key:v1")
    })?;
    let body = body.strip_suffix('\n').unwrap_or(body);
    let body = body.strip_suffix('\r').unwrap_or(body);
    if body.is_empty() || !body.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(anyhow!(
            "trustee key file: expected one line of hex after securacv-trustee-key:v1"
        ));
    }
    let bytes = hex::decode(body)
        .map_err(|_| anyhow!("trustee key file: the hex after the prefix does not decode"))?;
    WrappedTrusteeKey::parse(&bytes)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::vault::crypto::keyguard_seal_with;

    /// alice's test seed and the passphrase the TKG1 tests use.
    pub(crate) const ALICE_SEED: [u8; 32] = [0x11; 32];
    pub(crate) const PASS: &str = "correct horse battery staple";

    fn alice_pk() -> [u8; 32] {
        SigningKey::from_bytes(&ALICE_SEED)
            .verifying_key()
            .to_bytes()
    }

    /// TKG1 bytes under a fixed salt and nonce, built the way `tkg1_seal`
    /// builds them.
    fn tkg1_fixed(seed: &[u8; 32], id: &str, pk: &[u8; 32]) -> Vec<u8> {
        let trailer = tkg1_aad_suffix(id, pk);
        let mut out = keyguard_seal_with(
            &KEYGUARD_TKG1,
            seed,
            PASS,
            &trailer,
            &[0x5a; 16],
            &[0xa5; 12],
        )
        .unwrap();
        out.extend_from_slice(&trailer);
        out
    }

    #[test]
    fn tkg1_round_trip_and_layout() {
        let bytes = tkg1_seal(&ALICE_SEED, "alice", PASS).unwrap();
        assert_eq!(bytes.len(), 127 + 4 + 5 + 32);
        assert_eq!(&bytes[..4], b"TKG1");
        assert_eq!(&bytes[4..6], &[1, 1]);
        assert_eq!(&bytes[6..18], &[0, 0, 1, 0, 3, 0, 0, 0, 1, 0, 0, 0]);
        assert_eq!(bytes[18], 16);
        assert_eq!(&bytes[127..131], &5u32.to_le_bytes());
        assert_eq!(&bytes[131..136], b"alice");
        assert_eq!(&bytes[136..], &alice_pk());

        let text = trustee_key_file_text(&bytes);
        assert!(text.starts_with("securacv-trustee-key:v1 544b4731"));
        assert!(text.ends_with('\n'));
        assert_eq!(text.matches('\n').count(), 1);
        let parsed = parse_trustee_key_file_text(&text).unwrap();
        assert_eq!(parsed.id, "alice");
        assert_eq!(parsed.public_key, alice_pk());
        assert_eq!(*parsed.open(PASS).unwrap(), ALICE_SEED);
    }

    #[test]
    fn tkg1_fixed_inputs_are_the_mkg1_layout_under_tkg1_magic() {
        // The deterministic builder and the production writer agree on every
        // byte except the random salt and nonce.
        let fixed = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        let live = tkg1_seal(&ALICE_SEED, "alice", PASS).unwrap();
        assert_eq!(fixed.len(), live.len());
        assert_eq!(fixed[..19], live[..19]);
        assert_eq!(fixed[127..], live[127..]);
        assert_eq!(&fixed[19..35], &[0x5a; 16]);
        assert_eq!(&fixed[67..79], &[0xa5; 12]);
        let parsed = WrappedTrusteeKey::parse(&fixed).unwrap();
        assert_eq!(*parsed.open(PASS).unwrap(), ALICE_SEED);
    }

    #[test]
    fn tkg1_wrong_passphrase() {
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        let err = WrappedTrusteeKey::parse(&bytes)
            .unwrap()
            .open("not the passphrase")
            .unwrap_err()
            .to_string();
        assert_eq!(err, "trustee key: wrong passphrase");
    }

    #[test]
    fn tkg1_tampered_byte_fails_closed() {
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        // A flipped ciphertext byte and a flipped tag byte both fail the AEAD.
        for at in [79usize, 126] {
            let mut t = bytes.clone();
            t[at] ^= 0x01;
            let err = WrappedTrusteeKey::parse(&t)
                .unwrap()
                .open(PASS)
                .unwrap_err()
                .to_string();
            assert_eq!(
                err,
                "trustee key: the file was altered, or its trustee id or public key was changed",
                "byte {at}"
            );
        }
        // A flipped KEK commitment byte reads as a wrong passphrase.
        let mut t = bytes.clone();
        t[40] ^= 0x01;
        let err = WrappedTrusteeKey::parse(&t)
            .unwrap()
            .open(PASS)
            .unwrap_err()
            .to_string();
        assert_eq!(err, "trustee key: wrong passphrase");
    }

    #[test]
    fn tkg1_aad_binds_the_id_and_the_public_key() {
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        // Renamed: "alice" -> "alicf", same length, so only the AEAD can tell.
        let mut renamed = bytes.clone();
        renamed[135] = b'f';
        let w = WrappedTrusteeKey::parse(&renamed).unwrap();
        assert_eq!(w.id, "alicf");
        let err = w.open(PASS).unwrap_err().to_string();
        assert_eq!(
            err,
            "trustee key: the file was altered, or its trustee id or public key was changed"
        );
        // Renamed to a different length: re-encode the trailer.
        let mut longer = bytes[..127].to_vec();
        longer.extend_from_slice(&tkg1_aad_suffix("alice2", &alice_pk()));
        assert!(WrappedTrusteeKey::parse(&longer)
            .unwrap()
            .open(PASS)
            .is_err());
        // Swapped public key: bob's key in alice's file.
        let bob_pk = SigningKey::from_bytes(&[0x22; 32])
            .verifying_key()
            .to_bytes();
        let mut swapped = bytes[..127].to_vec();
        swapped.extend_from_slice(&tkg1_aad_suffix("alice", &bob_pk));
        let err = WrappedTrusteeKey::parse(&swapped)
            .unwrap()
            .open(PASS)
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "trustee key: the file was altered, or its trustee id or public key was changed"
        );
        // A writer that bound a seed to a public key it does not yield (it
        // knows the passphrase) is refused after decryption.
        let lying = tkg1_fixed(&ALICE_SEED, "alice", &bob_pk);
        let err = WrappedTrusteeKey::parse(&lying)
            .unwrap()
            .open(PASS)
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "trustee key: the decrypted seed does not yield the recorded public key"
        );
    }

    #[test]
    fn tkg1_parameters_refused_before_the_kdf() {
        // parse() never derives: a huge m is refused with no passphrase at all.
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        for (at, value) in [(6usize, 9_999_999u32), (10, 1), (14, 4)] {
            let mut t = bytes.clone();
            t[at..at + 4].copy_from_slice(&value.to_le_bytes());
            let err = WrappedTrusteeKey::parse(&t).err().unwrap().to_string();
            assert_eq!(err, "trustee key: unexpected TKG1 v1 parameters");
        }
        // The shared reader refuses them too, before deriving, when handed
        // the container directly.
        let mut t = bytes[..127].to_vec();
        t[6..10].copy_from_slice(&9_999_999u32.to_le_bytes());
        let err = keyguard_open(&KEYGUARD_TKG1, &t, PASS, &bytes[127..])
            .unwrap_err()
            .to_string();
        assert_eq!(err, "trustee key: unexpected TKG1 v1 parameters");
    }

    #[test]
    fn tkg1_header_and_trailer_bounds() {
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        let err = |b: &[u8]| WrappedTrusteeKey::parse(b).err().unwrap().to_string();
        let mut t = bytes.clone();
        t[..4].copy_from_slice(b"MKG1");
        assert_eq!(err(&t), "trustee key: bad magic");
        let mut t = bytes.clone();
        t[4] = 2;
        assert_eq!(err(&t), "trustee key: unsupported version 2");
        let mut t = bytes.clone();
        t[5] = 2;
        assert_eq!(err(&t), "trustee key: unsupported kind 2");
        let mut t = bytes.clone();
        t[18] = 8;
        assert_eq!(err(&t), "trustee key: unexpected salt length");
        assert_eq!(err(&bytes[..100]), "trustee key: truncated");
        assert_eq!(err(&bytes[..bytes.len() - 1]), "trustee key: truncated");
        let mut t = bytes.clone();
        t.push(0);
        assert_eq!(err(&t), "trustee key: trailing bytes after the public key");
        let mut t = bytes[..127].to_vec();
        t.extend_from_slice(&0u32.to_le_bytes());
        t.extend_from_slice(&alice_pk());
        assert_eq!(err(&t), "trustee key: trustee id must be 1 to 64 bytes");
        let mut t = bytes[..127].to_vec();
        t.extend_from_slice(&tkg1_aad_suffix(&"a".repeat(65), &alice_pk()));
        assert_eq!(err(&t), "trustee key: trustee id must be 1 to 64 bytes");
        let mut t = bytes[..127].to_vec();
        t.extend_from_slice(&tkg1_aad_suffix("al\nice", &alice_pk()));
        assert!(err(&t).starts_with("trustee key: trustee id contains a control"));
        // The writer refuses the same ids.
        assert!(tkg1_seal(&ALICE_SEED, "", PASS).is_err());
        assert!(tkg1_seal(&ALICE_SEED, &"a".repeat(65), PASS).is_err());
        assert!(tkg1_seal(&ALICE_SEED, &"a".repeat(64), PASS).is_ok());
    }

    #[test]
    fn key_file_text_is_strict() {
        let bytes = tkg1_fixed(&ALICE_SEED, "alice", &alice_pk());
        let line = trustee_key_file_text(&bytes);
        assert!(parse_trustee_key_file_text(&line).is_ok());
        assert!(parse_trustee_key_file_text(line.trim_end()).is_ok());
        assert!(parse_trustee_key_file_text(&line.replace('\n', "\r\n")).is_ok());
        let e = |t: &str| parse_trustee_key_file_text(t).err().unwrap().to_string();
        assert_eq!(
            e(&hex::encode(&bytes)),
            "trustee key file: it does not start with securacv-trustee-key:v1"
        );
        assert_eq!(
            e(&format!("{line}extra\n")),
            "trustee key file: expected one line of hex after securacv-trustee-key:v1"
        );
        assert_eq!(
            e(&format!(
                "{}{}0\n",
                TRUSTEE_KEY_FILE_PREFIX,
                hex::encode(&bytes)
            )),
            "trustee key file: the hex after the prefix does not decode"
        );
        // No error names a path or carries key material.
        for msg in [e(&hex::encode(&bytes)), e(&format!("{line}extra\n"))] {
            assert!(!msg.contains(&hex::encode(alice_pk())));
        }
    }
}
