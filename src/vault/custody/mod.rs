//! Threshold custody of the vault KEK: the core (spec `quorum_unseal_v2.md`
//! §2; design record `docs/design/threshold_custody.md`).
//!
//! **Nothing in the kernel calls this module.** It is the custody core that
//! later changes wire in: the mathematics, the HPKE boxes and the byte
//! formats, frozen as v1 and pinned by cross-implementation known-answer
//! tests (`spec/fixtures/threshold_custody_v1.json`, written by a
//! standard-library-only Python generator). The only callers are this
//! module's own tests and the holder-side `break_glass custody` / `share`
//! subcommands, so no vault can be put under custody by this code and no
//! envelope, policy or receipt byte changes.
//!
//! What it is:
//!
//! - **The KEK** is a fresh scalar pair `(k, r)` over ristretto255. It wraps
//!   the unchanged vault master key (and the ML-KEM-768 decapsulation key, if
//!   any) inside the `MKC1` custody container ([`container`]).
//! - **Pedersen verifiable secret sharing** splits `(k, r)` n-of-m
//!   ([`vss`]). The commitment `C_0 = k·G + r·H` is the same for every split
//!   of one KEK and is perfectly hiding: no public value has discrete log k,
//!   so a quantum adversary holding every commitment learns nothing about k.
//! - **A share** travels as a self-describing `SHR1` file ([`share`]) that a
//!   holder checks alone against the split's public commitments, sealed to
//!   the holder's recipient key with HPKE ([`hpke`], [`recipient`]) or
//!   wrapped at rest under their passphrase (Argon2id).
//!
//! What it promises, and what it does not:
//!
//! - Fewer than n shares, together with every commitment, reveal nothing
//!   about k (Pedersen hiding). Binding is computational: a forged share that
//!   passes its check is an integrity failure, caught at reconstruction by
//!   the `C_0` check and the `MKC1` AEAD; it never discloses anything.
//! - Every secret value (scalars, shares, recipient secrets, the master key
//!   in a container's plaintext) lives in `Zeroizing` storage, has no
//!   `Clone`, `Serialize` or `PartialEq`, and prints `<redacted>` under
//!   `Debug`. No error string carries a secret byte.
//! - Arithmetic on secrets is `curve25519-dalek`'s constant-time `Scalar`
//!   and multiscalar code; variable-time operations touch public data only
//!   (commitments and the public share points). That rests on the crate's
//!   documentation, not on measurement.
//! - **R-KEEP**, the residual this design keeps by choice: at every
//!   share-based unseal (a later change) the device rebuilds k in memory and
//!   holds the master key that opens every envelope, so a modified binary
//!   could keep it. It is detected only when the kept key is used through
//!   the device (a later unseal without share releases), never when it is
//!   used offline. A future threshold-decryption profile for new envelopes
//!   is the named seam that would remove it.
//! - The box-2 hybrid (HPKE PSK mode keyed by ML-KEM-768) is expected to stay
//!   secret unless both the X25519 key and the ML-KEM secret are broken; the
//!   HPKE draft states that this hybrid property is not proven.

pub(crate) mod container;
mod error;
pub(crate) mod group;
pub(crate) mod hpke;
#[cfg(test)]
mod kat;
pub(crate) mod recipient;
pub(crate) mod share;
pub(crate) mod split_record;
pub(crate) mod vss;

pub(crate) use error::{CustodyError, ShareReason};

/// Magic of the binary share envelope.
pub const SHARE_MAGIC: &[u8; 4] = b"SHR1";
/// The one share-envelope version this build reads and writes.
pub const SHARE_VERSION: u8 = 1;
/// Cap on a binary share envelope, checked before any length is read.
pub const SHARE_MAX_BYTES: usize = 16_384;
/// Cap on the text form: the prefix, a space, 32 768 hex characters and LF.
pub const SHARE_TEXT_MAX_BYTES: usize = 32_787;
/// The text form's prefix.
pub const SHARE_TEXT_PREFIX: &str = "securacv-share:v1";
/// ristretto255 Pedersen shares, H as in [`GENERATOR_H_LABEL`], the HPKE
/// suite of [`hpke`].
pub const SUITE_V1: u8 = 0x01;
/// A transport share sealed with box 1 (HPKE X25519).
pub const PROTECTION_TRANSPORT_BOX1: u8 = 0x01;
/// A transport share sealed with box 2 (HPKE X25519, PSK = ML-KEM-768).
pub const PROTECTION_TRANSPORT_BOX2: u8 = 0x02;
/// A share wrapped at rest under the holder's passphrase (Argon2id).
pub const PROTECTION_AT_REST: u8 = 0x03;
/// Box 1: HPKE base mode, DHKEM(X25519, HKDF-SHA256), HKDF-SHA256,
/// ChaCha20-Poly1305.
pub const BOX_HPKE_X25519: u8 = 0x01;
/// Box 2: the same suite in PSK mode, the PSK being an ML-KEM-768 shared
/// secret.
pub const BOX_HPKE_X25519_PSK_MLKEM768: u8 = 0x02;
/// A share of the split that serves unseals.
pub const ROLE_OPERATIONAL: u8 = 0x01;
/// A share of the split that can only rebuild a lost trustee quorum.
pub const ROLE_RECOVERY: u8 = 0x02;
/// A share of a throwaway key that protects nothing.
pub const ROLE_REHEARSAL: u8 = 0x03;
/// The smallest threshold: a 1-of-m split has one custodian.
pub const MIN_THRESHOLD: u8 = 2;
/// The most holders one split has (`MAX_TRUSTEES`).
pub const MAX_HOLDERS: u8 = 32;
/// The longest holder id, in bytes.
pub const HOLDER_ID_MAX: usize = 64;
/// Cap on the container copy a share carries, and on an `MKC1` file.
pub const CONTAINER_COPY_MAX: usize = 8_192;
/// SHA-512 input of the Pedersen generator H.
pub const GENERATOR_H_LABEL: &[u8] = b"securacv:custody:pedersen-generator-H:v1";
/// Domain of the commitment-set hash.
pub const COMMITMENTS_DOMAIN: &[u8] = b"securacv:custody:commitments:v1";
/// Domain of the recipient-key hash.
pub const RECIPIENT_DOMAIN: &[u8] = b"securacv:custody:recipient:v1";
/// Domain of a share file's trailing check.
pub const SHARE_FILE_CHECK_DOMAIN: &[u8] = b"securacv-share-file:v1";
/// AAD prefix of a share wrapped at rest.
pub const SHARE_AT_REST_AAD_PREFIX: &[u8] = b"securacv-share-at-rest-v1";
/// HPKE `info` of a share sealed to its holder.
pub const HPKE_INFO_SHARE: &[u8] = b"securacv-custody-share:v1";
/// Prefix of box 2's HPKE `psk_id`; `SHA-256(ct_M)` follows it.
pub const MLKEM_PSK_ID_PREFIX: &[u8] = b"securacv:custody:mlkem768-psk-id:v1";
/// Magic of the custody container.
pub const MKC1_MAGIC: &[u8; 4] = b"MKC1";
/// Cap on an `MKC1` container.
pub const MKC1_MAX_BYTES: usize = 8_192;
/// AAD domain of the `MKC1` AEAD.
pub const MKC1_AAD_DOMAIN: &[u8] = b"securacv-vault-custody-v1";
/// HKDF-SHA256 info of the KEK wrap key.
pub const KEK_WRAP_INFO: &[u8] = b"securacv:vault:kek-wrap:v1";
/// HKDF-SHA256 salt of the vault X25519 key derived from the master key.
pub const VAULT_X25519_SALT: &[u8] = b"securacv:vault:x25519-ikm:v1";
/// Magic of a recipient secret key wrapped at rest.
pub const TEK1_MAGIC: &[u8; 4] = b"TEK1";
/// AAD domain of a TEK1 container.
pub const TEK1_AAD_DOMAIN: &[u8] = b"securacv-custody-recipient-key-v1";
/// `format` of a recipient public file.
pub const RECIPIENT_FORMAT: &str = "securacv-custody-recipient:v1";
/// Prefix of a recipient secret file's one line.
pub const RECIPIENT_KEY_TEXT_PREFIX: &str = "securacv-custody-recipient-key:v1";
/// `format` of a split record.
pub const SPLIT_RECORD_FORMAT: &str = "securacv-custody-split-record:v1";
/// Argon2id memory, KiB: the MKG1 value.
pub const ARGON2_M_KIB: u32 = crate::vault::crypto::KEYGUARD_M_KIB;
/// Argon2id passes: the MKG1 value.
pub const ARGON2_T: u32 = crate::vault::crypto::KEYGUARD_T;
/// Argon2id lanes: the MKG1 value.
pub const ARGON2_P: u32 = crate::vault::crypto::KEYGUARD_P;

/// `u32le(len x) ‖ x`, the framing every custody hash uses.
pub(crate) fn frame(out: &mut Vec<u8>, x: &[u8]) {
    out.extend_from_slice(&(x.len() as u32).to_le_bytes());
    out.extend_from_slice(x);
}

/// A holder id: 1 to 64 bytes of `[A-Za-z0-9._-]`.
pub(crate) fn holder_id_ok(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= HOLDER_ID_MAX
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// The first eight hex characters of `bytes`, as share and split ids print.
pub(crate) fn hex8(bytes: &[u8]) -> String {
    hex::encode(&bytes[..4.min(bytes.len())])
}

/// The first sixteen hex characters of `bytes`, as hashes and keys print.
pub(crate) fn hex16(bytes: &[u8]) -> String {
    hex::encode(&bytes[..8.min(bytes.len())])
}
