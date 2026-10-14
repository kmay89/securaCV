//! Member and admin credential verifiers: `es256` and `fido2-es256`.
//!
//! - `es256`: ECDSA P-256 with SHA-256 over the message, signature ASN.1 DER.
//!   Public keys are 65-byte SEC1 uncompressed points.
//! - `fido2-es256`: a WebAuthn/CTAP2 assertion. The authenticator signs
//!   `authenticatorData ‖ clientDataHash`; for a challenge the hub sets
//!   `clientDataHash = SHA-256(msg)`. The hub checks `rpIdHash =
//!   SHA-256(rp_id)`, the UP flag, the UV flag when the caller requires it,
//!   and the DER ES256 signature. The signature counter is parsed past and
//!   **never stored or compared**: freshness comes from the single-use hub
//!   nonce, and FIDO2 clone detection is traded away for keeping no per-key
//!   usage state (spec §8).
//!
//! p256 accepts high-S signatures, so signature bytes are never used as a
//! replay or uniqueness key anywhere in this crate.

use p256::ecdsa::signature::Verifier;
use p256::ecdsa::{Signature, VerifyingKey};
use sha2::{Digest, Sha256};

/// User Present.
pub const FLAG_UP: u8 = 0x01;
/// The UV flag: the authenticator ran its own user verification (FIDO term).
pub const FLAG_UV: u8 = 0x04;
/// Attested credential data included (never in an assertion we accept).
pub const FLAG_AT: u8 = 0x40;
/// Extension data included.
pub const FLAG_ED: u8 = 0x80;

/// The longest authenticator data accepted (bytes).
pub const MAX_AUTH_DATA: usize = 1024;
/// The longest DER signature accepted (bytes). A P-256 DER signature is at
/// most 72 bytes.
pub const MAX_DER_SIG: usize = 72;

/// Parse a 65-byte SEC1 uncompressed P-256 point.
pub fn parse_sec1_uncompressed(pk: &[u8]) -> Option<VerifyingKey> {
    if pk.len() != 65 || pk[0] != 0x04 {
        return None;
    }
    VerifyingKey::from_sec1_bytes(pk).ok()
}

#[cfg(test)]
thread_local! {
    /// How many P-256 verifications ran on this thread (tests only: the
    /// checker's equal-work property is asserted by count, not by timing).
    pub(crate) static ES256_RUNS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

/// Verify an `es256` signature: ECDSA P-256 / SHA-256 over `msg`, DER.
pub fn es256(public_key_sec1: &[u8], msg: &[u8], der_sig: &[u8]) -> bool {
    if der_sig.is_empty() || der_sig.len() > MAX_DER_SIG {
        return false;
    }
    let Some(vk) = parse_sec1_uncompressed(public_key_sec1) else {
        return false;
    };
    let Ok(sig) = Signature::from_der(der_sig) else {
        return false;
    };
    #[cfg(test)]
    ES256_RUNS.with(|c| c.set(c.get() + 1));
    vk.verify(msg, &sig).is_ok()
}

/// The fields of authenticator data the hub reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AuthData {
    /// SHA-256 of the RP id.
    pub rp_id_hash: [u8; 32],
    /// The flags byte.
    pub flags: u8,
}

/// Parse assertion authenticator data: `rpIdHash(32) ‖ flags(1) ‖
/// signCount(4) ‖ [extensions]`. Attested credential data (AT) is refused;
/// trailing bytes are allowed only when the ED flag says extensions follow
/// (their CBOR is covered by the signature and otherwise ignored). The sign
/// count is skipped, never returned.
pub fn parse_auth_data(ad: &[u8]) -> Option<AuthData> {
    if ad.len() < 37 || ad.len() > MAX_AUTH_DATA {
        return None;
    }
    let flags = ad[32];
    if flags & FLAG_AT != 0 {
        return None;
    }
    if ad.len() > 37 && flags & FLAG_ED == 0 {
        return None;
    }
    if ad.len() == 37 && flags & FLAG_ED != 0 {
        return None;
    }
    let mut rp_id_hash = [0u8; 32];
    rp_id_hash.copy_from_slice(&ad[..32]);
    Some(AuthData { rp_id_hash, flags })
}

/// Whether assertion authenticator data passes every check but the
/// signature: it parses, its `rpIdHash` equals `rp_id_hash`, UP is set, and
/// UV is set when `require_uv`.
pub fn fido2_auth_data_ok(rp_id_hash: &[u8; 32], auth_data: &[u8], require_uv: bool) -> bool {
    let Some(ad) = parse_auth_data(auth_data) else {
        return false;
    };
    ad.rp_id_hash == *rp_id_hash
        && ad.flags & FLAG_UP != 0
        && (!require_uv || ad.flags & FLAG_UV != 0)
}

/// The bytes a FIDO2 assertion signs: `authenticatorData ‖ clientDataHash`.
pub fn fido2_signed_bytes(auth_data: &[u8], client_data_hash: &[u8; 32]) -> Vec<u8> {
    let mut signed = Vec::with_capacity(auth_data.len() + 32);
    signed.extend_from_slice(auth_data);
    signed.extend_from_slice(client_data_hash);
    signed
}

/// Verify a `fido2-es256` assertion.
///
/// `client_data_hash` is what the hub handed the authenticator (for a member
/// challenge, `SHA-256(msg)`). UP is always required; UV only when
/// `require_uv`.
pub fn fido2_es256(
    public_key_sec1: &[u8],
    rp_id: &str,
    client_data_hash: &[u8; 32],
    auth_data: &[u8],
    der_sig: &[u8],
    require_uv: bool,
) -> bool {
    let want: [u8; 32] = Sha256::digest(rp_id.as_bytes()).into();
    if !fido2_auth_data_ok(&want, auth_data, require_uv) {
        return false;
    }
    es256(
        public_key_sec1,
        &fido2_signed_bytes(auth_data, client_data_hash),
        der_sig,
    )
}

/// Whether an assertion's authenticator data carries the UV flag (parsed the
/// same way [`fido2_es256`] parses it). `false` for unparsable data.
pub fn auth_data_has_uv(auth_data: &[u8]) -> bool {
    parse_auth_data(auth_data)
        .map(|a| a.flags & FLAG_UV != 0)
        .unwrap_or(false)
}

/// Build assertion authenticator data (for tests and the reference client):
/// `SHA-256(rp_id) ‖ flags ‖ be32(sign_count)`.
pub fn build_auth_data(rp_id: &str, flags: u8, sign_count: u32) -> Vec<u8> {
    let mut ad = Vec::with_capacity(37);
    ad.extend_from_slice(&Sha256::digest(rp_id.as_bytes()));
    ad.push(flags);
    ad.extend_from_slice(&sign_count.to_be_bytes());
    ad
}

#[cfg(test)]
mod tests {
    use super::*;
    use p256::ecdsa::signature::Signer;
    use p256::ecdsa::SigningKey;

    fn sk() -> SigningKey {
        SigningKey::from_slice(&[0x11u8; 32]).unwrap()
    }

    fn pk(sk: &SigningKey) -> Vec<u8> {
        sk.verifying_key().to_sec1_point(false).as_bytes().to_vec()
    }

    fn der(sk: &SigningKey, m: &[u8]) -> Vec<u8> {
        let s: Signature = sk.sign(m);
        s.to_der().as_bytes().to_vec()
    }

    #[test]
    fn es256_accepts_valid_refuses_tamper() {
        let k = sk();
        let m = b"securacv-hk-v1\0 test";
        let s = der(&k, m);
        assert!(es256(&pk(&k), m, &s));
        assert!(!es256(&pk(&k), b"other", &s));
        let mut bad = s.clone();
        let last = bad.len() - 1;
        bad[last] ^= 1;
        assert!(!es256(&pk(&k), m, &bad));
        let other = SigningKey::from_slice(&[0x22u8; 32]).unwrap();
        assert!(!es256(&pk(&other), m, &s));
    }

    #[test]
    fn es256_refuses_raw_rs_and_compressed_keys() {
        let k = sk();
        let m = b"msg";
        let s: Signature = k.sign(m);
        // Raw r||s (64 bytes) is not DER and is refused: one encoding only.
        assert!(!es256(&pk(&k), m, &s.to_bytes()));
        let compressed = k.verifying_key().to_sec1_point(true).as_bytes().to_vec();
        assert!(!es256(&compressed, m, &der(&k, m)));
        assert!(!es256(&[0u8; 65], m, &der(&k, m)));
        assert!(!es256(&pk(&k), m, &[]));
        assert!(!es256(&pk(&k), m, &[0x30; 80]));
    }

    fn assertion(k: &SigningKey, rp: &str, flags: u8, cdh: &[u8; 32]) -> (Vec<u8>, Vec<u8>) {
        let ad = build_auth_data(rp, flags, 0);
        let mut m = ad.clone();
        m.extend_from_slice(cdh);
        (ad, der(k, &m))
    }

    #[test]
    fn fido2_checks_rp_up_uv_and_signature() {
        let k = sk();
        let cdh: [u8; 32] = Sha256::digest(b"msg").into();
        let rp = "hk.example";
        let (ad, s) = assertion(&k, rp, FLAG_UP | FLAG_UV, &cdh);
        assert!(fido2_es256(&pk(&k), rp, &cdh, &ad, &s, true));
        assert!(fido2_es256(&pk(&k), rp, &cdh, &ad, &s, false));
        // Wrong RP id.
        assert!(!fido2_es256(&pk(&k), "hk.other", &cdh, &ad, &s, false));
        // Wrong client data hash.
        let other: [u8; 32] = Sha256::digest(b"other").into();
        assert!(!fido2_es256(&pk(&k), rp, &other, &ad, &s, false));
        // UP only: fine without UV, refused when UV is required.
        let (ad, s) = assertion(&k, rp, FLAG_UP, &cdh);
        assert!(fido2_es256(&pk(&k), rp, &cdh, &ad, &s, false));
        assert!(!fido2_es256(&pk(&k), rp, &cdh, &ad, &s, true));
        assert!(!auth_data_has_uv(&ad));
        // No UP at all: refused.
        let (ad, s) = assertion(&k, rp, FLAG_UV, &cdh);
        assert!(!fido2_es256(&pk(&k), rp, &cdh, &ad, &s, false));
        // Flipping the UV flag after signing breaks the signature.
        let (mut ad, s) = assertion(&k, rp, FLAG_UP, &cdh);
        ad[32] |= FLAG_UV;
        assert!(!fido2_es256(&pk(&k), rp, &cdh, &ad, &s, true));
    }

    #[test]
    fn sign_count_is_ignored() {
        // Two assertions with counters 0 and 0 (and a lower counter after a
        // higher one) both verify: no counter is kept.
        let k = sk();
        let cdh = [9u8; 32];
        let rp = "hk.example";
        for count in [5u32, 0, 0, 3] {
            let ad = build_auth_data(rp, FLAG_UP, count);
            let mut m = ad.clone();
            m.extend_from_slice(&cdh);
            assert!(fido2_es256(&pk(&k), rp, &cdh, &ad, &der(&k, &m), false));
        }
    }

    #[test]
    fn auth_data_shape() {
        assert!(parse_auth_data(&[0u8; 36]).is_none());
        let mut ad = build_auth_data("x", FLAG_UP | FLAG_AT, 0);
        assert!(parse_auth_data(&ad).is_none());
        ad[32] = FLAG_UP;
        assert!(parse_auth_data(&ad).is_some());
        ad.push(0xa0); // trailing bytes without ED
        assert!(parse_auth_data(&ad).is_none());
        ad[32] = FLAG_UP | FLAG_ED;
        assert!(parse_auth_data(&ad).is_some());
        let ed_no_ext = build_auth_data("x", FLAG_UP | FLAG_ED, 0);
        assert!(parse_auth_data(&ed_no_ext).is_none());
        assert!(parse_auth_data(&vec![0u8; MAX_AUTH_DATA + 1]).is_none());
    }
}
