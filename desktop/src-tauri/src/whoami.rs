//! Ask a device to answer a fresh challenge, and check its signature.
//!
//! # What this establishes, and what it deliberately does not
//!
//! A device holding the identity key behind `device_id` was **reachable**
//! and signed a nonce it could not have precomputed. That rules out an
//! impersonator who cannot reach the genuine device — a stale announcement
//! for a Canary that has left the network, or a peer on a segment that
//! cannot talk to it.
//!
//! It is **not channel binding**, and nothing here may be used as
//! authorization. A hostile peer on the same LAN can spoof the mDNS record,
//! relay our nonce to the genuine device's (deliberately unauthenticated)
//! endpoint, and hand the signature back as its own; the verification below
//! would succeed against that peer's socket. Since such a peer is already on
//! the LAN — the precondition that let it spoof at all — relaying costs it
//! nothing.
//!
//! So the fleet book shows this result and **gates nothing on it**. Making a
//! valid proof the condition for attaching a bearer token would authenticate
//! the key while saying nothing about the socket receiving the credential,
//! which is worse than today's honest "unverified": false confidence beats
//! no confidence only in the wrong direction. Closing that gap requires the
//! token exchange itself to be authenticated to the device key, which is an
//! open design decision — see docs/flasher_profiles_fleet_book.md.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};

/// The device's own fingerprint derivation, byte for byte:
/// `SHA256(domain || 0x00 || pubkey)[0..8]`, lowercase hex.
///
/// The 0x00 separator is load-bearing and easy to drop when working from a
/// prose description — without it every comparison here would fail against
/// real hardware while looking perfectly reasonable. Source of truth is
/// `wc_sha256_domain` in firmware/common/witness/witness_chain.h.
const DOMAIN_FINGERPRINT: &str = "securacv:pubkey:fingerprint";

pub fn pubkey_fingerprint(pubkey: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(DOMAIN_FINGERPRINT.as_bytes());
    h.update([0x00u8]);
    h.update(pubkey);
    let digest = h.finalize();
    let mut out = String::with_capacity(16);
    for byte in digest.iter().take(8) {
        use std::fmt::Write as _;
        let _ = write!(out, "{byte:02x}");
    }
    out
}

/// The canonical the device signs. Must match `build_whoami_canonical` in
/// canary-wap's device_signature.cpp exactly — a mismatch here verifies
/// nothing while appearing to work on well-formed input.
pub fn whoami_canonical(device_id: &str, nonce: &str) -> String {
    format!("securacv-canary-sig|v1|whoami|{device_id}|{nonce}")
}

/// The same gate the firmware applies before it will sign: 16-64 lowercase
/// hex. Enforced on our side too so we never ask a device to sign something
/// it will refuse, and so a nonce we minted is always well-formed.
pub fn nonce_ok(nonce: &str) -> bool {
    let n = nonce.len();
    (16..=64).contains(&n)
        && nonce
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn decode_hex(value: &str) -> Option<Vec<u8>> {
    if value.len() % 2 != 0 || !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    (0..value.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&value[i..i + 2], 16).ok())
        .collect()
}

#[derive(Debug, PartialEq, Eq)]
pub enum Proof {
    /// The signature checks out AND the key is the one the book recorded.
    Answered,
    /// A valid signature from a key that is NOT the one we expected. The
    /// loudest possible outcome: something is answering for this device_id
    /// with a different identity.
    WrongKey { seen_fp: String, expected_fp: String },
    /// Reached it, but the answer doesn't verify.
    BadSignature,
    /// The device doesn't offer the endpoint (older firmware) or didn't
    /// answer. Absence of proof, never disproof.
    Unavailable(String),
}

impl Proof {
    pub fn as_str(&self) -> &'static str {
        match self {
            Proof::Answered => "answered",
            Proof::WrongKey { .. } => "wrong-key",
            Proof::BadSignature => "bad-signature",
            Proof::Unavailable(_) => "unavailable",
        }
    }
}

/// Check a device's answer. Pure over the response fields so it is testable
/// without a device or a network.
///
/// `expected_fp` is what the fleet book recorded from the serial receipt at
/// flash time. Empty means we have nothing to compare against (trust on
/// first use), in which case a good signature is `Answered` and the caller
/// records the fingerprint it saw.
pub fn check_answer(
    device_id: &str,
    nonce: &str,
    pubkey_hex: &str,
    sig_hex: &str,
    expected_fp: &str,
) -> Proof {
    if !nonce_ok(nonce) {
        return Proof::Unavailable("we minted a malformed nonce".into());
    }
    let Some(pubkey) = decode_hex(pubkey_hex).filter(|k| k.len() == 32) else {
        return Proof::Unavailable("device sent no usable public key".into());
    };
    let Some(sig) = decode_hex(sig_hex).filter(|s| s.len() == 64) else {
        return Proof::Unavailable("device sent no usable signature".into());
    };
    let key_bytes: [u8; 32] = pubkey[..].try_into().expect("checked length");
    let Ok(verifying_key) = VerifyingKey::from_bytes(&key_bytes) else {
        return Proof::BadSignature;
    };
    let sig_bytes: [u8; 64] = sig[..].try_into().expect("checked length");
    let signature = Signature::from_bytes(&sig_bytes);
    let canonical = whoami_canonical(device_id, nonce);
    if verifying_key.verify(canonical.as_bytes(), &signature).is_err() {
        return Proof::BadSignature;
    }
    // The signature is good. Now: is it the RIGHT key? Verifying a signature
    // from an unexpected key and calling it proof would be the whole point
    // missed — anyone can generate a key and sign our nonce with it.
    let seen = pubkey_fingerprint(&pubkey);
    if expected_fp.is_empty() {
        return Proof::Answered; // trust on first use; caller records `seen`
    }
    if !seen.eq_ignore_ascii_case(expected_fp) {
        return Proof::WrongKey {
            seen_fp: seen,
            expected_fp: expected_fp.to_ascii_lowercase(),
        };
    }
    Proof::Answered
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    fn key() -> SigningKey {
        // Deterministic: these tests must not depend on an RNG.
        SigningKey::from_bytes(&[7u8; 32])
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    fn answer(id: &str, nonce: &str, k: &SigningKey) -> (String, String) {
        let sig = k.sign(whoami_canonical(id, nonce).as_bytes());
        (hex(k.verifying_key().as_bytes()), hex(&sig.to_bytes()))
    }

    const NONCE: &str = "0123456789abcdef";

    #[test]
    fn a_real_answer_from_the_expected_key_is_accepted() {
        let k = key();
        let (pk, sig) = answer("canary_wap_a1", NONCE, &k);
        let fp = pubkey_fingerprint(k.verifying_key().as_bytes());
        assert_eq!(
            check_answer("canary_wap_a1", NONCE, &pk, &sig, &fp),
            Proof::Answered
        );
    }

    #[test]
    fn a_valid_signature_from_the_wrong_key_is_not_proof() {
        // The failure that would defeat the whole check: anyone can mint a
        // key and sign our nonce with it. A good signature is necessary and
        // nowhere near sufficient — it must be THIS device's key.
        let impostor = SigningKey::from_bytes(&[9u8; 32]);
        let (pk, sig) = answer("canary_wap_a1", NONCE, &impostor);
        let expected = pubkey_fingerprint(key().verifying_key().as_bytes());
        match check_answer("canary_wap_a1", NONCE, &pk, &sig, &expected) {
            Proof::WrongKey { seen_fp, expected_fp } => {
                assert_ne!(seen_fp, expected_fp);
                assert_eq!(expected_fp, expected);
            }
            other => panic!("a foreign key must not pass: {other:?}"),
        }
    }

    #[test]
    fn a_signature_over_a_different_device_or_nonce_does_not_verify() {
        let k = key();
        let (pk, sig) = answer("canary_wap_a1", NONCE, &k);
        let fp = pubkey_fingerprint(k.verifying_key().as_bytes());
        // Same key, but the canonical names another device — this is the
        // cross-device replay the device_id field exists to stop.
        assert_eq!(
            check_answer("canary_wap_b2", NONCE, &pk, &sig, &fp),
            Proof::BadSignature
        );
        // Same key, a different nonce: the replay of an OLD answer.
        assert_eq!(
            check_answer("canary_wap_a1", "fedcba9876543210", &pk, &sig, &fp),
            Proof::BadSignature
        );
    }

    #[test]
    fn first_sight_accepts_and_leaves_recording_to_the_caller() {
        let k = key();
        let (pk, sig) = answer("canary_wap_a1", NONCE, &k);
        assert_eq!(
            check_answer("canary_wap_a1", NONCE, &pk, &sig, ""),
            Proof::Answered
        );
    }

    #[test]
    fn malformed_input_is_unavailable_never_proof_and_never_a_panic() {
        let k = key();
        let (pk, sig) = answer("canary_wap_a1", NONCE, &k);
        for (p, s) in [
            ("", sig.as_str()),
            (pk.as_str(), ""),
            ("zz", sig.as_str()),
            (pk.as_str(), "abc"), // odd length
            ("00", sig.as_str()), // right alphabet, wrong length
        ] {
            assert!(
                matches!(check_answer("canary_wap_a1", NONCE, p, s, ""), Proof::Unavailable(_)),
                "malformed input must be Unavailable, not proof"
            );
        }
        // A nonce we could never have minted is our bug, not the device's.
        assert!(matches!(
            check_answer("canary_wap_a1", "short", &pk, &sig, ""),
            Proof::Unavailable(_)
        ));
    }

    #[test]
    fn the_fingerprint_matches_the_firmwares_derivation() {
        // SHA256(domain || 0x00 || pubkey)[0..8]. The separator is the part
        // a prose description drops; recomputed here the long way so this
        // test fails if the helper ever stops including it.
        let pubkey = [0xABu8; 32];
        let mut h = Sha256::new();
        h.update(b"securacv:pubkey:fingerprint");
        h.update([0x00u8]);
        h.update(pubkey);
        let want: String = h.finalize().iter().take(8).map(|b| format!("{b:02x}")).collect();
        assert_eq!(pubkey_fingerprint(&pubkey), want);
        assert_eq!(pubkey_fingerprint(&pubkey).len(), 16);

        // And WITHOUT the separator it must differ — proving the assertion
        // above is actually testing the separator.
        let mut h2 = Sha256::new();
        h2.update(b"securacv:pubkey:fingerprint");
        h2.update(pubkey);
        let without: String = h2.finalize().iter().take(8).map(|b| format!("{b:02x}")).collect();
        assert_ne!(pubkey_fingerprint(&pubkey), without);
    }

    #[test]
    fn the_canonical_and_nonce_gate_match_the_firmware() {
        assert_eq!(
            whoami_canonical("canary_wap_a1", NONCE),
            format!("securacv-canary-sig|v1|whoami|canary_wap_a1|{NONCE}")
        );
        assert!(nonce_ok(NONCE));
        assert!(nonce_ok(&"a".repeat(64)));
        assert!(!nonce_ok(&"a".repeat(65)));
        assert!(!nonce_ok("short"));
        assert!(!nonce_ok("ABCDEF0123456789"), "uppercase is refused by the device");
        assert!(!nonce_ok("ghijklmnopqrstuv"), "non-hex is refused by the device");
    }
}
