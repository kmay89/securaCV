//! The signed announce — what lets a retained MQTT message say "a SecuraCV
//! kernel is here" without letting any broker client say it on the kernel's
//! behalf.
//!
//! The MQTT bridge publishes the announce (`<prefix>/<device_id>/announce`,
//! retained) but does not hold the signing key; the kernel API does. So the
//! bridge builds the payload WITHOUT `sig`, sends those exact bytes to
//! `POST /api/sign-announce` (token-gated, body-bounded), and publishes
//! them with the returned `sig` appended. The kernel signs only a body
//! that names it — its own `device_id` and `verifying_key` — so the key
//! never endorses an announce for some other host.
//!
//! Canonical message: [`ANNOUNCE_SIG_PREFIX`] followed by the compact
//! `serde_json` bytes of the payload with `sig` absent, keys in struct
//! order. The signature is plain Ed25519 over that message (no prehash, no
//! domain-separated digest): a Python verifier reproduces it with
//! `json.dumps(payload_minus_sig, separators=(",", ":"), ensure_ascii=False)`
//! and the pinned key.

use anyhow::{anyhow, Result};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};

/// Prefix of the signed message.
pub const ANNOUNCE_SIG_PREFIX: &[u8] = b"securacv-announce|v1|";
/// `POST /api/sign-announce` reads at most this many body bytes.
pub const SIGN_ANNOUNCE_MAX_BYTES: usize = 2048;

/// The bytes the kernel signs for a sig-less announce payload.
pub fn announce_message(payload_without_sig: &[u8]) -> Vec<u8> {
    [ANNOUNCE_SIG_PREFIX, payload_without_sig].concat()
}

/// Why a body was refused by [`check_announce_body`]: the `detail` the 400
/// carries.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnnounceBodyRefusal {
    NotAnObject,
    CarriesSig,
    NamesAnotherKernel,
}

impl AnnounceBodyRefusal {
    pub fn detail(self) -> &'static str {
        match self {
            Self::NotAnObject => "announce payload must be a JSON object",
            Self::CarriesSig => "announce payload must not carry a sig field; the kernel adds it",
            Self::NamesAnotherKernel => {
                "announce payload must name this kernel (device_id and verifying_key)"
            }
        }
    }
}

/// Whether `body` is an announce this kernel will put its name to: a JSON
/// object, no `sig` of its own, and `device_id`/`verifying_key` equal to
/// this kernel's. Only the shape is checked — the bytes signed are the
/// bytes received, never a re-serialization (key order and spacing are part
/// of what the verifier reproduces).
pub fn check_announce_body(
    body: &[u8],
    own_device_id: &str,
    own_verifying_key_hex: &str,
) -> Result<(), AnnounceBodyRefusal> {
    let value: serde_json::Value =
        serde_json::from_slice(body).map_err(|_| AnnounceBodyRefusal::NotAnObject)?;
    let object = value.as_object().ok_or(AnnounceBodyRefusal::NotAnObject)?;
    if object.contains_key("sig") {
        return Err(AnnounceBodyRefusal::CarriesSig);
    }
    let names_self = object.get("device_id").and_then(|v| v.as_str()) == Some(own_device_id)
        && object.get("verifying_key").and_then(|v| v.as_str()) == Some(own_verifying_key_hex);
    if !names_self {
        return Err(AnnounceBodyRefusal::NamesAnotherKernel);
    }
    Ok(())
}

/// Verify `sig_hex` over a sig-less payload under `key` — the integration's
/// check, written here so the two ends share one definition and a test can
/// hold the API's signature to it.
pub fn verify_announce(
    key: &VerifyingKey,
    payload_without_sig: &[u8],
    sig_hex: &str,
) -> Result<()> {
    let raw = hex::decode(sig_hex).map_err(|_| anyhow!("announce sig is not hex"))?;
    let sig_bytes: [u8; 64] = raw
        .try_into()
        .map_err(|_| anyhow!("announce sig is not 64 bytes"))?;
    key.verify(
        &announce_message(payload_without_sig),
        &Signature::from_bytes(&sig_bytes),
    )
    .map_err(|_| anyhow!("announce signature does not verify"))
}

#[cfg(test)]
mod tests {
    use super::*;

    const ID: &str = "pwk_1a2b3c4d";
    const KEY: &str = "ad220887aa0ba4d061b3f42712d73c9ce796dcb57be1f62dd2d9bd22ec29526c";

    #[test]
    fn body_checks_shape_and_self_naming() {
        let good =
            format!(r#"{{"v":1,"device_id":"{ID}","verifying_key":"{KEY}","pairing":"required"}}"#);
        assert_eq!(check_announce_body(good.as_bytes(), ID, KEY), Ok(()));
        assert_eq!(
            check_announce_body(b"[1,2]", ID, KEY),
            Err(AnnounceBodyRefusal::NotAnObject)
        );
        assert_eq!(
            check_announce_body(b"not json", ID, KEY),
            Err(AnnounceBodyRefusal::NotAnObject)
        );
        let signed = format!(r#"{{"device_id":"{ID}","verifying_key":"{KEY}","sig":"00"}}"#);
        assert_eq!(
            check_announce_body(signed.as_bytes(), ID, KEY),
            Err(AnnounceBodyRefusal::CarriesSig)
        );
        let other = format!(r#"{{"device_id":"pwk_ffffffff","verifying_key":"{KEY}"}}"#);
        assert_eq!(
            check_announce_body(other.as_bytes(), ID, KEY),
            Err(AnnounceBodyRefusal::NamesAnotherKernel)
        );
        let keyless = format!(r#"{{"device_id":"{ID}"}}"#);
        assert_eq!(
            check_announce_body(keyless.as_bytes(), ID, KEY),
            Err(AnnounceBodyRefusal::NamesAnotherKernel)
        );
    }

    #[test]
    fn the_message_is_prefix_then_the_exact_bytes() {
        let msg = announce_message(br#"{"v":1}"#);
        assert_eq!(msg, b"securacv-announce|v1|{\"v\":1}".to_vec());
    }

    #[test]
    fn verify_rejects_a_wrong_key_and_a_changed_payload() {
        use ed25519_dalek::{Signer, SigningKey};
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let payload = br#"{"v":1,"device_id":"pwk_1a2b3c4d"}"#;
        let sig = hex::encode(key.sign(&announce_message(payload)).to_bytes());
        assert_eq!(sig.len(), 128);
        verify_announce(&key.verifying_key(), payload, &sig).expect("verifies");
        assert!(verify_announce(&key.verifying_key(), br#"{"v":2}"#, &sig).is_err());
        let other = SigningKey::from_bytes(&[8u8; 32]);
        assert!(verify_announce(&other.verifying_key(), payload, &sig).is_err());
        assert!(verify_announce(&key.verifying_key(), payload, "zz").is_err());
    }
}
