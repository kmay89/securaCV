//! The sealed bearer: the client half of canary-wap's `api_seal.h`.
//!
//! # Why
//!
//! A device's bearer token used to ride a plain `Authorization: Bearer`
//! header over plain HTTP, and mDNS is unauthenticated: any host on the LAN
//! can announce a Canary's device_id at its own address. A fresh signed
//! challenge (whoami.rs) does not close that, because a peer that can reach
//! the genuine board relays the challenge and hands the signature back.
//! Serving https would need a certificate to pin, and no stock build stores
//! one (the firmware's sweep F15). So the credential is made useless to
//! anyone but the device instead: a per-session key is agreed over X25519,
//! the device signs the whole transcript with the Ed25519 identity key the
//! fleet book pinned at flash time, and the token is only ever sent sealed
//! under that key. A relay can forward one sealed request; it cannot read
//! the token, replay the request, or point it at another route.
//!
//! # The protocol (v1), as the firmware states it
//!
//! 1. `GET /api/device/seal?nonce=<32 hex>&client_pub=<64 hex>` — the
//!    client's fresh X25519 public key and its freshness.
//! 2. The device answers the enroll card plus `seal: {sid, device_pub_hex,
//!    sig_hex, ttl_s}`, where `sig_hex` signs
//!    `securacv-canary-sig|v1|seal|<device_id>|<nonce>|<client_pub_hex>|<device_pub_hex>`.
//! 3. Both ends derive `key = HKDF-SHA256(salt = SHA-256(canonical),
//!    ikm = X25519(client_priv, device_pub), info = "securacv:api-seal:v1")`
//!    and `sid = SHA-256(canonical)[0..8]`.
//! 4. Every authenticated request then carries
//!    `Authorization: Sealed <sid>.<seq>.<hex(ChaCha20-Poly1305 ciphertext || tag)>`
//!    with `iv = 0x00*8 || seq (4 bytes, big-endian)`, `aad = "<METHOD> <path>"`
//!    and a strictly increasing `seq`.
//!
//! The device's host test (`tests_host/test_api_seal.cpp`) and the tests
//! below pin the same reference vectors, computed by a third implementation
//! (python-cryptography), so the two ends agree byte for byte or a test says
//! which one moved.
//!
//! # What it does not do
//!
//! The request body and the answer are not sealed: a relay can read a
//! status answer, or change identify's blink duration. The token and the
//! action are what it protects. And it is the client's choice: this app
//! never falls back to a plain bearer, so a device whose firmware predates
//! the handshake is told to update once over USB rather than handed the
//! token in the clear (a fallback would be the downgrade a relay asks for).

use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{ChaCha20Poly1305, Nonce};
use sha2::{Digest, Sha256};
use x25519_dalek::{PublicKey, StaticSecret};

use crate::whoami::{decode_hex, nonce_ok, Proof};

/// The firmware's route (`seal_api.h`, registered in canary_wap.ino).
pub const ROUTE: &str = "/api/device/seal";
/// The Authorization scheme, with its space (`api_seal::SCHEME`).
pub const SCHEME: &str = "Sealed ";
/// The HKDF info string (`api_seal::INFO`).
pub const INFO: &[u8] = b"securacv:api-seal:v1";
/// The longest plaintext the device unseals (`api_seal::TOKEN_CAP`).
pub const TOKEN_CAP: usize = 64;

/// The canonical the device signs. Must match `build_seal_canonical` in the
/// firmware's device_signature.cpp exactly: device_id, nonce, the client's
/// key, then the device's.
pub fn seal_canonical(
    device_id: &str,
    nonce: &str,
    client_pub_hex: &str,
    device_pub_hex: &str,
) -> String {
    format!("securacv-canary-sig|v1|seal|{device_id}|{nonce}|{client_pub_hex}|{device_pub_hex}")
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&Sha256::digest(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let ipad: Vec<u8> = k.iter().map(|b| b ^ 0x36).collect();
    let opad: Vec<u8> = k.iter().map(|b| b ^ 0x5c).collect();
    let inner = Sha256::new()
        .chain_update(&ipad)
        .chain_update(data)
        .finalize();
    Sha256::new()
        .chain_update(&opad)
        .chain_update(inner)
        .finalize()
        .into()
}

/// RFC 5869 HKDF-SHA256, one 32-byte block (what both ends derive).
pub fn hkdf_sha256(salt: &[u8], ikm: &[u8], info: &[u8]) -> [u8; 32] {
    let prk = hmac_sha256(salt, ikm);
    let mut t = Vec::with_capacity(info.len() + 1);
    t.extend_from_slice(info);
    t.push(1);
    hmac_sha256(&prk, &t)
}

/// The seal fields the device answers (`api_seal::write_answer`), as
/// fleet.rs reads them off the JSON. Every field is untrusted text.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Answer {
    /// The enroll card's `pubkey_hex`: the key the signature is checked with.
    pub pubkey_hex: String,
    /// `seal.sid`
    pub sid: String,
    /// `seal.device_pub_hex`
    pub device_pub_hex: String,
    /// `seal.sig_hex`
    pub sig_hex: String,
}

/// Why a handshake answer was not turned into a session. Each carries the
/// sentence the fleet book shows under the row.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// The device answered without a seal (firmware before the handshake,
    /// or a build whose gate cannot open one). The token stays here.
    Unsupported(String),
    /// A valid signature from a key that is not the one the book recorded.
    WrongKey {
        seen_fp: String,
        expected_fp: String,
    },
    /// The signature does not verify against the key presented.
    BadSignature,
    /// A field the client cannot use (shape, a low-order key, a sid the two
    /// ends do not agree on).
    Malformed(String),
}

impl Refusal {
    pub fn detail(&self) -> String {
        match self {
            Refusal::Unsupported(why) => format!(
                "This firmware doesn't offer the sealed key exchange yet ({why}), so the stored key \
                 stays here. Update the board once over USB; later updates work from here."
            ),
            Refusal::WrongKey {
                seen_fp,
                expected_fp,
            } => format!(
                "A different key answered for this device id (saw {seen_fp}, expected {expected_fp}). \
                 The stored key wasn't sent."
            ),
            Refusal::BadSignature => {
                "The device's handshake didn't verify against its key. The stored key wasn't sent."
                    .to_string()
            }
            Refusal::Malformed(why) => format!(
                "The device's handshake answer couldn't be used ({why}). The stored key wasn't sent."
            ),
        }
    }
}

/// One handshake in flight: the client's ephemeral secret and nonce.
pub struct Handshake {
    secret: StaticSecret,
    pub nonce: String,
}

impl Handshake {
    /// A fresh secret and a 32-hex nonce from OS randomness.
    pub fn new() -> Result<Self, String> {
        let mut secret = [0u8; 32];
        getrandom::fill(&mut secret).map_err(|e| format!("no system randomness: {e}"))?;
        let mut raw = [0u8; 16];
        getrandom::fill(&mut raw).map_err(|e| format!("no system randomness: {e}"))?;
        Ok(Self::from_parts(secret, &hex(&raw)))
    }

    /// Tests and vectors: a known scalar and nonce. `StaticSecret` clamps the
    /// scalar when it is used, as RFC 7748 asks.
    pub fn from_parts(secret: [u8; 32], nonce: &str) -> Self {
        Self {
            secret: StaticSecret::from(secret),
            nonce: nonce.to_string(),
        }
    }

    pub fn client_pub_hex(&self) -> String {
        hex(PublicKey::from(&self.secret).as_bytes())
    }

    /// The query string of step 1.
    pub fn query(&self) -> String {
        format!("nonce={}&client_pub={}", self.nonce, self.client_pub_hex())
    }

    /// Step 3: check the device's answer against the fingerprint the book
    /// recorded, and derive the session. Consumes the secret.
    ///
    /// `expected_fp` must be non-empty: with nothing to check the signature
    /// against there is no one to seal the token to, and the handshake is
    /// refused rather than trusted on first use (the SHOWN identity probe
    /// is where first sight is recorded; only then can a token follow).
    pub fn accept(
        self,
        device_id: &str,
        expected_fp: &str,
        answer: &Answer,
    ) -> Result<Session, Refusal> {
        if expected_fp.is_empty() {
            return Err(Refusal::Malformed(
                "no identity fingerprint is recorded for this board yet".into(),
            ));
        }
        if !nonce_ok(&self.nonce) {
            return Err(Refusal::Malformed("we minted a malformed nonce".into()));
        }
        if answer.sid.is_empty() && answer.device_pub_hex.is_empty() && answer.sig_hex.is_empty() {
            return Err(Refusal::Unsupported("the answer carried no seal".into()));
        }
        let Some(device_pub) = decode_hex(&answer.device_pub_hex).filter(|k| k.len() == 32) else {
            return Err(Refusal::Malformed(
                "device_pub_hex is not 64 hex chars".into(),
            ));
        };
        if answer.device_pub_hex != answer.device_pub_hex.to_ascii_lowercase() {
            return Err(Refusal::Malformed("device_pub_hex is not lowercase".into()));
        }
        if decode_hex(&answer.sid).filter(|s| s.len() == 8).is_none() {
            return Err(Refusal::Malformed("sid is not 16 hex chars".into()));
        }
        let client_pub_hex = self.client_pub_hex();
        let canonical = seal_canonical(
            device_id,
            &self.nonce,
            &client_pub_hex,
            &answer.device_pub_hex,
        );
        match crate::whoami::check_signature(
            canonical.as_bytes(),
            &answer.pubkey_hex,
            &answer.sig_hex,
            expected_fp,
        ) {
            Proof::Answered => {}
            Proof::WrongKey {
                seen_fp,
                expected_fp,
            } => {
                return Err(Refusal::WrongKey {
                    seen_fp,
                    expected_fp,
                })
            }
            Proof::BadSignature => return Err(Refusal::BadSignature),
            Proof::Unavailable(why) => return Err(Refusal::Malformed(why)),
        }
        let device_pub_bytes: [u8; 32] = device_pub[..].try_into().expect("checked length");
        let shared = self
            .secret
            .diffie_hellman(&PublicKey::from(device_pub_bytes));
        // A low-order device key gives an all-zero secret anyone can compute:
        // refused here as the firmware refuses it on its side.
        if !shared.was_contributory() {
            return Err(Refusal::Malformed(
                "the device's ephemeral key is a low-order point".into(),
            ));
        }
        let transcript: [u8; 32] = Sha256::digest(canonical.as_bytes()).into();
        let sid = hex(&transcript[..8]);
        if !sid.eq_ignore_ascii_case(&answer.sid) {
            // The two ends derive the id from the same transcript; a device
            // that names another is not running this protocol.
            return Err(Refusal::Malformed(
                "the session id does not match the transcript".into(),
            ));
        }
        let key = hkdf_sha256(&transcript, shared.as_bytes(), INFO);
        Ok(Session { sid, key, seq: 0 })
    }
}

/// An agreed session: the device's table holds its twin for `ttl_s`.
#[derive(Clone)]
pub struct Session {
    pub sid: String,
    key: [u8; 32],
    seq: u32,
}

/// Hand-written so a log line never carries the key: the sid and the
/// counter name the session well enough.
impl std::fmt::Debug for Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Session")
            .field("sid", &self.sid)
            .field("seq", &self.seq)
            .field("key", &"<sealed>")
            .finish()
    }
}

impl Session {
    /// Tests and vectors.
    pub fn from_parts(sid: &str, key: [u8; 32], seq: u32) -> Self {
        Self {
            sid: sid.to_string(),
            key,
            seq,
        }
    }

    /// Step 4: the `Authorization` header value for one request. Each call
    /// takes the next counter value; the device refuses any value at or
    /// below the last it accepted, so a request this app never saw answered
    /// still spends its number.
    pub fn seal(&mut self, method: &str, path: &str, token: &str) -> Result<String, String> {
        if token.is_empty() || token.len() > TOKEN_CAP {
            return Err("the credential is not a size the device unseals".into());
        }
        if path.contains('?') || path.contains(' ') || method.contains(' ') {
            return Err("a sealed request binds a bare method and path".into());
        }
        let seq = self
            .seq
            .checked_add(1)
            .ok_or_else(|| "this session's counter is spent".to_string())?;
        self.seq = seq;
        let mut iv = [0u8; 12];
        iv[8..].copy_from_slice(&seq.to_be_bytes());
        let aad = format!("{method} {path}");
        let cipher = ChaCha20Poly1305::new((&self.key).into());
        let sealed = cipher
            .encrypt(
                Nonce::from_slice(&iv),
                Payload {
                    msg: token.as_bytes(),
                    aad: aad.as_bytes(),
                },
            )
            .map_err(|_| "sealing the credential failed".to_string())?;
        Ok(format!("{SCHEME}{}.{seq}.{}", self.sid, hex(&sealed)))
    }
}

/// Whether a token may be sent at all: the public probes carry none; a
/// token needs the device id it belongs to and a recorded fingerprint to
/// seal it to. `Ok(false)` is "no credential, nothing to seal".
pub fn seal_need(token: &str, device_id: &str, expected_fp: &str) -> Result<bool, String> {
    if token.is_empty() {
        return Ok(false);
    }
    if device_id.is_empty() {
        return Err("A device credential needs the device id it belongs to.".into());
    }
    if expected_fp.is_empty() {
        return Err(
            "No identity fingerprint is recorded for this board yet, so there is nothing to seal \
             its key to. Wait for the fleet book's identity badge, then try again."
                .into(),
        );
    }
    Ok(true)
}

/// What a 401's `seal` word asks of the client: a new handshake (the device
/// rebooted, the session aged out, or a counter the device no longer
/// accepts), or nothing (a wrong token, or a build that cannot open seals).
#[derive(Debug, PartialEq, Eq)]
pub enum OnRefused {
    /// Open a new session and send once more.
    Rehandshake,
    /// The device's gate cannot open a seal: the firmware is behind.
    Unsupported,
    /// Not about the seal: the stored token is wrong for this device.
    StaleToken,
}

pub fn on_refused(status: u16, body: &serde_json::Value) -> OnRefused {
    let word = body.get("seal").and_then(|v| v.as_str()).unwrap_or("");
    match (status, word) {
        (401, "no_session") | (401, "expired") | (401, "replay") => OnRefused::Rehandshake,
        (401, "unsupported") => OnRefused::Unsupported,
        _ => OnRefused::StaleToken,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The reference vectors (python-cryptography; the firmware's
    // test_api_seal.cpp pins the same ones).
    const DEVICE_ID: &str = "canary_wap_a1";
    const NONCE: &str = "00112233445566778899aabbccddeeff";
    const CLIENT_PUB: &str = "7b4e909bbe7ffe44c465a220037d608ee35897d31ef972f07f74892cb0f73f13";
    const DEVICE_PUB: &str = "0faa684ed28867b97f4a6a2dee5df8ce974e76b7018e3f22a1c4cf2678570f20";
    const CANONICAL: &str = "securacv-canary-sig|v1|seal|canary_wap_a1|00112233445566778899aabbccddeeff|7b4e909bbe7ffe44c465a220037d608ee35897d31ef972f07f74892cb0f73f13|0faa684ed28867b97f4a6a2dee5df8ce974e76b7018e3f22a1c4cf2678570f20";
    const KEY: &str = "634cb1324c4783a5163c33dd458a926e225b1094aeab74750315524d9b6aef44";
    const SID: &str = "c47ccac8932f3cb2";
    const ED_PUB: &str = "ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c";
    const SIG: &str = "0c895bbdd8479fe919e44ec10dfaa5ce91155b0420fcc587a7f4b6d75e4dae0cd0ba64cafa5ac8346d4ecf8254afd7bfb11d7305d12816569c773d68f0461e08";
    const FP: &str = "015c29625b4dea08";
    const TOKEN: &str = "cv_a1B2c3D4e5F6g7H8i9J0k1L2m3N4o5P6";
    const SEALED_IDENTIFY_1: &str = "Sealed c47ccac8932f3cb2.1.e935397a49b1784f0a00e9b882c4510f67a9cefc069d1432865ed42aef79a0b1dc394d6257b29775cbbc4639caf91ba4bc28dc";
    const SEALED_STATUS_2: &str = "Sealed c47ccac8932f3cb2.2.4da26059880d43c037ac09fa4b96861865b91b5fe4f0699240e198651facd214fc8cd711968a92f1e8b4ada2c3e1f47475bc4b";

    fn handshake() -> Handshake {
        // The vector's client scalar before clamping: 0x11 bytes.
        Handshake::from_parts([0x11u8; 32], NONCE)
    }

    fn answer() -> Answer {
        Answer {
            pubkey_hex: ED_PUB.into(),
            sid: SID.into(),
            device_pub_hex: DEVICE_PUB.into(),
            sig_hex: SIG.into(),
        }
    }

    fn key_bytes() -> [u8; 32] {
        decode_hex(KEY).unwrap().try_into().unwrap()
    }

    #[test]
    fn hkdf_matches_rfc_5869_case_1() {
        let ikm = [0x0bu8; 22];
        let salt: Vec<u8> = (0..13u8).collect();
        let info: Vec<u8> = (0xf0..=0xf9u8).collect();
        let okm = hkdf_sha256(&salt, &ikm, &info);
        assert_eq!(
            hex(&okm),
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        );
    }

    #[test]
    fn the_client_key_and_query_match_the_vectors() {
        let h = handshake();
        assert_eq!(h.client_pub_hex(), CLIENT_PUB);
        assert_eq!(h.query(), format!("nonce={NONCE}&client_pub={CLIENT_PUB}"));
        assert_eq!(
            seal_canonical(DEVICE_ID, NONCE, CLIENT_PUB, DEVICE_PUB),
            CANONICAL
        );
    }

    #[test]
    fn a_signed_answer_from_the_pinned_key_yields_the_vectors_session() {
        let s = handshake().accept(DEVICE_ID, FP, &answer()).unwrap();
        assert_eq!(s.sid, SID);
        assert_eq!(hex(&s.key), KEY);
        let mut s = s;
        assert_eq!(
            s.seal("POST", "/api/identify", TOKEN).unwrap(),
            SEALED_IDENTIFY_1
        );
        assert_eq!(
            s.seal("GET", "/api/ota/status", TOKEN).unwrap(),
            SEALED_STATUS_2
        );
        // The firmware's device_pub_hex is lowercase; the sid may come in
        // either case from a JSON reader, the key does not care.
        let mut upper = answer();
        upper.sid = SID.to_ascii_uppercase();
        assert!(handshake().accept(DEVICE_ID, FP, &upper).is_ok());
    }

    #[test]
    fn a_session_seals_from_known_parts_and_counts_up() {
        let mut s = Session::from_parts(SID, key_bytes(), 0);
        assert_eq!(
            s.seal("POST", "/api/identify", TOKEN).unwrap(),
            SEALED_IDENTIFY_1
        );
        assert_eq!(
            s.seal("GET", "/api/ota/status", TOKEN).unwrap(),
            SEALED_STATUS_2
        );
        // Each call is a new counter value, never a repeat.
        let a = s.seal("GET", "/api/ota/status", TOKEN).unwrap();
        let b = s.seal("GET", "/api/ota/status", TOKEN).unwrap();
        assert_ne!(a, b);
        assert!(a.starts_with("Sealed c47ccac8932f3cb2.3."));
        assert!(b.starts_with("Sealed c47ccac8932f3cb2.4."));
        // Shapes the device would not unseal are refused before sealing.
        assert!(s.seal("GET", "/api/ota/status?x=1", TOKEN).is_err());
        assert!(s.seal("GET", "/api/ota/status", "").is_err());
        assert!(s.seal("GET", "/api/ota/status", &"x".repeat(65)).is_err());
        assert!(s.seal("GET", "/api/ota/status", &"x".repeat(64)).is_ok());
        let mut spent = Session::from_parts(SID, key_bytes(), u32::MAX);
        assert!(spent.seal("GET", "/api/ota/status", TOKEN).is_err());
    }

    #[test]
    fn the_sealed_header_opens_only_for_its_route_and_counter() {
        // The device side, in miniature: the same AEAD opens the vector
        // under its route, and nothing else opens it.
        let open = |header: &str, method: &str, path: &str| -> Option<String> {
            let value = header.strip_prefix(SCHEME)?;
            let mut parts = value.splitn(3, '.');
            let sid = parts.next()?;
            let seq: u32 = parts.next()?.parse().ok()?;
            let ct = decode_hex(parts.next()?)?;
            assert_eq!(sid, SID);
            let mut iv = [0u8; 12];
            iv[8..].copy_from_slice(&seq.to_be_bytes());
            let aad = format!("{method} {path}");
            ChaCha20Poly1305::new((&key_bytes()).into())
                .decrypt(
                    Nonce::from_slice(&iv),
                    Payload {
                        msg: &ct,
                        aad: aad.as_bytes(),
                    },
                )
                .ok()
                .map(|p| String::from_utf8_lossy(&p).into_owned())
        };
        assert_eq!(
            open(SEALED_IDENTIFY_1, "POST", "/api/identify").as_deref(),
            Some(TOKEN)
        );
        assert_eq!(open(SEALED_IDENTIFY_1, "GET", "/api/identify"), None);
        assert_eq!(open(SEALED_IDENTIFY_1, "POST", "/api/ota/install"), None);
        let other_counter = SEALED_IDENTIFY_1.replacen(".1.", ".2.", 1);
        assert_eq!(open(&other_counter, "POST", "/api/identify"), None);
    }

    #[test]
    fn the_wrong_key_a_bad_signature_and_a_missing_seal_are_refused() {
        // A different recorded fingerprint: the loudest outcome.
        match handshake().accept(DEVICE_ID, "fedcba9876543210", &answer()) {
            Err(Refusal::WrongKey {
                seen_fp,
                expected_fp,
            }) => {
                assert_eq!(seen_fp, FP);
                assert_eq!(expected_fp, "fedcba9876543210");
            }
            other => panic!("a foreign key must not pass: {other:?}"),
        }
        // The signature covers the device's ephemeral key: swap it and the
        // signature no longer verifies, so a key the device did not make is
        // never agreed with.
        let mut swapped = answer();
        swapped.device_pub_hex = CLIENT_PUB.into();
        assert_eq!(
            handshake().accept(DEVICE_ID, FP, &swapped).unwrap_err(),
            Refusal::BadSignature
        );
        // ...and the device id: the same answer for another device fails.
        assert_eq!(
            handshake()
                .accept("canary_wap_b2", FP, &answer())
                .unwrap_err(),
            Refusal::BadSignature
        );
        // ...and the nonce: a replayed old answer fails.
        let old = Handshake::from_parts([0x11u8; 32], "ffeeddccbbaa99887766554433221100");
        assert_eq!(
            old.accept(DEVICE_ID, FP, &answer()).unwrap_err(),
            Refusal::BadSignature
        );
        // A card without a seal is the old firmware, named as such.
        let bare = Answer {
            pubkey_hex: ED_PUB.into(),
            ..Default::default()
        };
        assert!(matches!(
            handshake().accept(DEVICE_ID, FP, &bare).unwrap_err(),
            Refusal::Unsupported(_)
        ));
        // Nothing recorded to check against: refused, never trusted on
        // first use (that is the badge's job, and it records; this sends).
        assert!(matches!(
            handshake().accept(DEVICE_ID, "", &answer()).unwrap_err(),
            Refusal::Malformed(_)
        ));
        // A sid the device made up: not this protocol.
        let mut odd = answer();
        odd.sid = "0000000000000000".into();
        assert!(matches!(
            handshake().accept(DEVICE_ID, FP, &odd).unwrap_err(),
            Refusal::Malformed(_)
        ));
        // Malformed fields are unusable, never a panic.
        for (field, value) in [("pub", "zz"), ("pub", ""), ("sid", "abc"), ("sig", "00")] {
            let mut bad = answer();
            match field {
                "pub" => bad.device_pub_hex = value.into(),
                "sid" => bad.sid = value.into(),
                _ => bad.sig_hex = value.into(),
            }
            assert!(
                handshake().accept(DEVICE_ID, FP, &bad).is_err(),
                "{field}={value:?}"
            );
        }
        // Every refusal reads as a sentence that says the key stayed here.
        for r in [
            Refusal::Unsupported("x".into()),
            Refusal::WrongKey {
                seen_fp: "a".into(),
                expected_fp: "b".into(),
            },
            Refusal::BadSignature,
            Refusal::Malformed("y".into()),
        ] {
            let d = r.detail();
            assert!(d.contains("stays here") || d.contains("wasn't sent"), "{d}");
        }
    }

    #[test]
    fn a_token_needs_a_device_id_and_a_recorded_fingerprint() {
        assert_eq!(seal_need("", "", ""), Ok(false));
        assert!(seal_need("cv_x", "", FP).is_err());
        assert!(seal_need("cv_x", DEVICE_ID, "").is_err());
        assert_eq!(seal_need("cv_x", DEVICE_ID, FP), Ok(true));
    }

    #[test]
    fn a_401s_word_says_whether_to_handshake_again() {
        let body = |w: &str| serde_json::json!({ "error": "unauthorized", "seal": w });
        for w in ["no_session", "expired", "replay"] {
            assert_eq!(on_refused(401, &body(w)), OnRefused::Rehandshake, "{w}");
        }
        assert_eq!(
            on_refused(401, &body("unsupported")),
            OnRefused::Unsupported
        );
        assert_eq!(on_refused(401, &body("bad_tag")), OnRefused::StaleToken);
        assert_eq!(
            on_refused(403, &serde_json::json!({ "error": "forbidden" })),
            OnRefused::StaleToken
        );
        assert_eq!(
            on_refused(401, &serde_json::Value::Null),
            OnRefused::StaleToken
        );
        // A 200 with the word is not a refusal at all.
        assert_eq!(on_refused(200, &body("no_session")), OnRefused::StaleToken);
    }
}
