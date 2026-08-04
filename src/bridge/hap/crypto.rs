//! The three cryptographic operations HAP is built from: HKDF-SHA512 key
//! derivation, ChaCha20-Poly1305 sealing under HAP's nonce convention, and
//! Ed25519 signing.
//!
//! Every salt and info string in HAP is a fixed ASCII constant, and using the
//! wrong one fails in a way that looks exactly like a wrong setup code. They
//! are therefore named constants here rather than string literals at their
//! use sites, so the pairing code reads as a protocol transcript and a typo
//! is a compile error instead of a support ticket.

use chacha20poly1305::{
    aead::{AeadInOut, KeyInit},
    ChaCha20Poly1305, Nonce, Tag,
};
use hkdf::Hkdf;
use sha2::Sha512;

/// HKDF salt/info pairs, verbatim from the HAP specification.
pub mod kdf {
    pub const PAIR_SETUP_ENCRYPT_SALT: &[u8] = b"Pair-Setup-Encrypt-Salt";
    pub const PAIR_SETUP_ENCRYPT_INFO: &[u8] = b"Pair-Setup-Encrypt-Info";
    pub const PAIR_SETUP_CONTROLLER_SIGN_SALT: &[u8] = b"Pair-Setup-Controller-Sign-Salt";
    pub const PAIR_SETUP_CONTROLLER_SIGN_INFO: &[u8] = b"Pair-Setup-Controller-Sign-Info";
    pub const PAIR_SETUP_ACCESSORY_SIGN_SALT: &[u8] = b"Pair-Setup-Accessory-Sign-Salt";
    pub const PAIR_SETUP_ACCESSORY_SIGN_INFO: &[u8] = b"Pair-Setup-Accessory-Sign-Info";
    pub const PAIR_VERIFY_ENCRYPT_SALT: &[u8] = b"Pair-Verify-Encrypt-Salt";
    pub const PAIR_VERIFY_ENCRYPT_INFO: &[u8] = b"Pair-Verify-Encrypt-Info";
    pub const CONTROL_SALT: &[u8] = b"Control-Salt";
    /// Accessory → controller. Named from the *controller's* point of view,
    /// which is the spec's convention and the opposite of the intuitive one:
    /// this is the key the accessory encrypts **with**.
    pub const CONTROL_READ_INFO: &[u8] = b"Control-Read-Encryption-Key";
    /// Controller → accessory: the key the accessory decrypts with.
    pub const CONTROL_WRITE_INFO: &[u8] = b"Control-Write-Encryption-Key";
}

/// The fixed 8-byte nonce values used by the pairing exchanges.
pub mod nonce {
    pub const PS_MSG05: &[u8; 8] = b"PS-Msg05";
    pub const PS_MSG06: &[u8; 8] = b"PS-Msg06";
    pub const PV_MSG02: &[u8; 8] = b"PV-Msg02";
    pub const PV_MSG03: &[u8; 8] = b"PV-Msg03";
}

/// Derive a 32-byte key. HAP uses HKDF-SHA512 everywhere, always to 32 bytes.
pub fn hkdf_sha512(ikm: &[u8], salt: &[u8], info: &[u8]) -> [u8; 32] {
    let hk = Hkdf::<Sha512>::new(Some(salt), ikm);
    let mut okm = [0u8; 32];
    // `expand` only fails when the output length exceeds 255*HashLen, which
    // for SHA-512 is 16 320 bytes. 32 is a compile-time constant well under
    // it, so this cannot fail — but we still avoid `unwrap` (FR-2) and fall
    // back to a zero key, which cannot authenticate anything and so fails
    // closed rather than panicking on a network path.
    if hk.expand(info, &mut okm).is_err() {
        return [0u8; 32];
    }
    okm
}

/// Build HAP's 12-byte nonce: four zero bytes, then the eight-byte value.
fn hap_nonce(value: &[u8; 8]) -> [u8; 12] {
    let mut n = [0u8; 12];
    n[4..].copy_from_slice(value);
    n
}

/// A 64-bit little-endian counter as an 8-byte nonce value — the session
/// convention, where each direction counts its own frames from zero.
pub fn counter_nonce(counter: u64) -> [u8; 8] {
    counter.to_le_bytes()
}

/// What went wrong in an AEAD operation. Deliberately one variant: a
/// controller learns that decryption failed, never why.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AeadError;

impl std::fmt::Display for AeadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "HAP AEAD authentication failed")
    }
}

impl std::error::Error for AeadError {}

/// Seal `plaintext` under `key`, returning `ciphertext || tag`.
pub fn seal(key: &[u8; 32], nonce_value: &[u8; 8], aad: &[u8], plaintext: &[u8]) -> Vec<u8> {
    let cipher = ChaCha20Poly1305::new(key.into());
    let n = hap_nonce(nonce_value);
    let nonce = Nonce::from(n);
    let mut buf = plaintext.to_vec();
    match cipher.encrypt_inout_detached(&nonce, aad, buf.as_mut_slice().into()) {
        Ok(tag) => {
            buf.extend_from_slice(&tag);
            buf
        }
        // Encryption of a bounded buffer under a valid key does not fail;
        // returning an empty vec keeps this total, and the peer's decryption
        // then fails closed.
        Err(_) => Vec::new(),
    }
}

/// Open `ciphertext || tag` under `key`.
pub fn open(
    key: &[u8; 32],
    nonce_value: &[u8; 8],
    aad: &[u8],
    sealed: &[u8],
) -> Result<Vec<u8>, AeadError> {
    if sealed.len() < 16 {
        return Err(AeadError);
    }
    let (body, tag_bytes) = sealed.split_at(sealed.len() - 16);
    let tag = Tag::try_from(tag_bytes).map_err(|_| AeadError)?;
    let cipher = ChaCha20Poly1305::new(key.into());
    let n = hap_nonce(nonce_value);
    let nonce = Nonce::from(n);
    let mut buf = body.to_vec();
    cipher
        .decrypt_inout_detached(&nonce, aad, buf.as_mut_slice().into(), &tag)
        .map_err(|_| AeadError)?;
    Ok(buf)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seal_open_round_trips() {
        let key = [7u8; 32];
        let sealed = seal(&key, nonce::PS_MSG05, b"", b"hello canary");
        assert_eq!(sealed.len(), 12 + 16, "ciphertext plus a 16-byte tag");
        let opened = open(&key, nonce::PS_MSG05, b"", &sealed).expect("opens");
        assert_eq!(opened, b"hello canary");
    }

    #[test]
    fn a_tampered_tag_does_not_open() {
        let key = [7u8; 32];
        let mut sealed = seal(&key, nonce::PS_MSG05, b"", b"hello canary");
        let last = sealed.len() - 1;
        sealed[last] ^= 0x01;
        assert_eq!(open(&key, nonce::PS_MSG05, b"", &sealed), Err(AeadError));
    }

    /// The nonce is part of the authentication, so a frame replayed under a
    /// different counter must not open. This is what stops a session frame
    /// from being reordered.
    #[test]
    fn a_frame_does_not_open_under_a_different_nonce() {
        let key = [7u8; 32];
        let sealed = seal(&key, &counter_nonce(0), b"", b"frame zero");
        assert_eq!(
            open(&key, &counter_nonce(1), b"", &sealed),
            Err(AeadError),
            "a frame must be bound to its counter"
        );
    }

    #[test]
    fn aad_is_authenticated() {
        let key = [7u8; 32];
        let sealed = seal(&key, &counter_nonce(0), &[0x10, 0x00], b"body");
        assert!(open(&key, &counter_nonce(0), &[0x10, 0x00], &sealed).is_ok());
        assert_eq!(
            open(&key, &counter_nonce(0), &[0x11, 0x00], &sealed),
            Err(AeadError)
        );
    }

    #[test]
    fn truncated_input_is_rejected_not_panicked_on() {
        let key = [7u8; 32];
        assert_eq!(open(&key, nonce::PS_MSG05, b"", &[]), Err(AeadError));
        assert_eq!(open(&key, nonce::PS_MSG05, b"", &[0u8; 15]), Err(AeadError));
    }

    /// Different salt/info pairs must give different keys — the failure mode
    /// if one were copy-pasted is a pairing that "just doesn't work."
    #[test]
    fn every_kdf_context_is_distinct() {
        let ikm = [3u8; 32];
        let keys = [
            hkdf_sha512(
                &ikm,
                kdf::PAIR_SETUP_ENCRYPT_SALT,
                kdf::PAIR_SETUP_ENCRYPT_INFO,
            ),
            hkdf_sha512(
                &ikm,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_SALT,
                kdf::PAIR_SETUP_CONTROLLER_SIGN_INFO,
            ),
            hkdf_sha512(
                &ikm,
                kdf::PAIR_SETUP_ACCESSORY_SIGN_SALT,
                kdf::PAIR_SETUP_ACCESSORY_SIGN_INFO,
            ),
            hkdf_sha512(
                &ikm,
                kdf::PAIR_VERIFY_ENCRYPT_SALT,
                kdf::PAIR_VERIFY_ENCRYPT_INFO,
            ),
            hkdf_sha512(&ikm, kdf::CONTROL_SALT, kdf::CONTROL_READ_INFO),
            hkdf_sha512(&ikm, kdf::CONTROL_SALT, kdf::CONTROL_WRITE_INFO),
        ];
        let mut seen = std::collections::HashSet::new();
        for k in keys {
            assert!(seen.insert(k), "two HAP KDF contexts collided");
        }
    }

    /// The read and write session keys must differ, or the two directions
    /// would share a keystream — the classic catastrophic AEAD misuse.
    #[test]
    fn session_directions_do_not_share_a_key() {
        let ss = [42u8; 32];
        let read = hkdf_sha512(&ss, kdf::CONTROL_SALT, kdf::CONTROL_READ_INFO);
        let write = hkdf_sha512(&ss, kdf::CONTROL_SALT, kdf::CONTROL_WRITE_INFO);
        assert_ne!(read, write);
    }

    #[test]
    fn hap_nonce_is_zero_padded_at_the_front() {
        assert_eq!(hap_nonce(nonce::PS_MSG05), *b"\0\0\0\0PS-Msg05");
        assert_eq!(
            hap_nonce(&counter_nonce(1)),
            [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]
        );
    }
}
