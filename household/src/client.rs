//! A reference software credential for the LAN API.
//!
//! It holds a P-256 key in a file and answers challenges the way a phone key
//! would (`es256`), or, with an RP id, the way a FIDO2 authenticator would
//! (`fido2-es256`, with or without the UV flag). It exists so the API can be
//! exercised end to end without a phone. It is **not** a secure credential:
//! the key is a plain file, and nothing asks a person anything before it
//! signs, so enroll it as `possession` unless you are testing.
//!
//! Before signing, it parses the challenge and refuses one whose purpose is
//! not the one asked for, or whose door id is not the LAN API's.

use crate::{hex, verify, wire, Error, Result};
use base64::engine::general_purpose::STANDARD as B64;
use base64::Engine;
use p256::ecdsa::signature::Signer;
use p256::ecdsa::{Signature, SigningKey};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::Path;
use std::time::Duration;
use zeroize::Zeroizing;

/// How the credential answers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Binding {
    /// ECDSA P-256 over the message.
    Es256,
    /// A FIDO2-style assertion.
    Fido2 {
        /// RP id the roster entry names.
        rp_id: String,
        /// Whether to set the UV flag.
        uv: bool,
    },
}

/// A software credential.
pub struct SoftCredential {
    key: SigningKey,
    /// The member handle the roster assigned.
    pub handle: [u8; 16],
    /// The binding.
    pub binding: Binding,
}

impl std::fmt::Debug for SoftCredential {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("SoftCredential { .. }")
    }
}

/// Generate a key and write it (hex scalar, mode 0600) to `path`. Returns
/// the SEC1 uncompressed public key to enroll.
pub fn keygen(path: &Path) -> Result<Vec<u8>> {
    let key = loop {
        let b = Zeroizing::new(crate::rng::array::<32>()?);
        if let Ok(k) = SigningKey::from_slice(&b[..]) {
            break k;
        }
    };
    let secret = Zeroizing::new(hex::encode(&key.to_bytes()));
    let body = Zeroizing::new(format!(
        "{{\"alg\":\"es256\",\"secret_key\":\"{}\"}}\n",
        secret.as_str()
    ));
    if path.exists() {
        return Err(Error::Invalid("refusing to overwrite an existing key file"));
    }
    crate::store::write_atomic(path, body.as_bytes())?;
    Ok(public_key(&key))
}

fn public_key(k: &SigningKey) -> Vec<u8> {
    k.verifying_key().to_sec1_point(false).as_bytes().to_vec()
}

impl SoftCredential {
    /// A credential from a key.
    pub fn new(key: SigningKey, handle: [u8; 16], binding: Binding) -> Self {
        SoftCredential {
            key,
            handle,
            binding,
        }
    }

    /// Load the key file written by [`keygen`].
    pub fn load(path: &Path, handle: [u8; 16], binding: Binding) -> Result<Self> {
        let raw = Zeroizing::new(crate::store::read_bounded(path, 4096)?);
        let v: Value =
            serde_json::from_slice(&raw).map_err(|_| Error::Invalid("key file is not JSON"))?;
        if v["alg"] != "es256" {
            return Err(Error::Invalid("key file is not an es256 key"));
        }
        let s = Zeroizing::new(hex::decode_array::<32>(
            v["secret_key"]
                .as_str()
                .ok_or(Error::Invalid("key file has no secret_key"))?,
        )?);
        let key = SigningKey::from_slice(&s[..])
            .map_err(|_| Error::Invalid("key file holds an invalid key"))?;
        Ok(Self::new(key, handle, binding))
    }

    /// The SEC1 uncompressed public key.
    pub fn public_key(&self) -> Vec<u8> {
        public_key(&self.key)
    }

    /// Answer a challenge message, refusing one that does not ask for
    /// `expected` at the LAN API. Returns `(signature, authenticator_data)`.
    pub fn answer(
        &self,
        msg: &[u8],
        expected: wire::Purpose,
    ) -> Result<(Vec<u8>, Option<Vec<u8>>)> {
        let p = wire::parse_msg(msg)?;
        if p.purpose != expected {
            return Err(Error::Rejected(
                "the hub sent a challenge for a different purpose",
            ));
        }
        if p.door_id != wire::LAN_DOOR_ID {
            return Err(Error::Rejected("the challenge is not for the LAN API"));
        }
        match &self.binding {
            Binding::Es256 => {
                let s: Signature = self.key.sign(msg);
                Ok((s.to_der().as_bytes().to_vec(), None))
            }
            Binding::Fido2 { rp_id, uv } => {
                let cdh: [u8; 32] = Sha256::digest(msg).into();
                let flags = verify::FLAG_UP | if *uv { verify::FLAG_UV } else { 0 };
                let (ad, sig) = crate::admin::soft_fido2_assert(&self.key, rp_id, flags, &cdh);
                Ok((sig, Some(ad)))
            }
        }
    }
}

/// A minimal HTTP/1.1 request. Returns `(status, body)`.
pub fn http(
    addr: SocketAddr,
    method: &str,
    path: &str,
    token: Option<&str>,
    body: Option<&str>,
) -> Result<(u16, String)> {
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(5))
        .map_err(Error::io("connect to the hub"))?;
    s.set_read_timeout(Some(Duration::from_secs(10)))
        .map_err(Error::io("set timeout"))?;
    s.set_write_timeout(Some(Duration::from_secs(10)))
        .map_err(Error::io("set timeout"))?;
    let mut req = format!("{method} {path} HTTP/1.1\r\nHost: {addr}\r\nConnection: close\r\n");
    if let Some(t) = token {
        req.push_str(&format!("Authorization: Bearer {t}\r\n"));
    }
    let body = body.unwrap_or("");
    if method == "POST" {
        req.push_str("Content-Type: application/json\r\n");
        req.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    req.push_str("\r\n");
    req.push_str(body);
    s.write_all(req.as_bytes())
        .map_err(Error::io("send the request"))?;
    let mut out = Vec::new();
    s.take(64 * 1024)
        .read_to_end(&mut out)
        .map_err(Error::io("read the response"))?;
    let text = String::from_utf8(out).map_err(|_| Error::Invalid("response is not UTF-8"))?;
    let (head, body) = text
        .split_once("\r\n\r\n")
        .ok_or(Error::Invalid("response has no head"))?;
    let status = head
        .split(' ')
        .nth(1)
        .and_then(|s| s.parse().ok())
        .ok_or(Error::Invalid("response has no status"))?;
    Ok((status, body.to_string()))
}

/// Request a challenge, answer it and send the command. Returns the final
/// `(status, body)`; a refused challenge request returns its `401`.
pub fn set_posture(
    addr: SocketAddr,
    token: &str,
    cred: &SoftCredential,
    purpose: wire::Purpose,
) -> Result<(u16, String)> {
    let (st, body) = http(
        addr,
        "POST",
        "/v1/challenge",
        Some(token),
        Some(&json!({"purpose": purpose.word()}).to_string()),
    )?;
    if st != 200 {
        return Ok((st, body));
    }
    let v: Value = serde_json::from_str(&body)
        .map_err(|_| Error::Invalid("challenge response is not JSON"))?;
    let id = v["challenge_id"]
        .as_str()
        .ok_or(Error::Invalid("no challenge_id"))?;
    let msg = B64
        .decode(v["msg_b64"].as_str().ok_or(Error::Invalid("no msg_b64"))?)
        .map_err(|_| Error::Invalid("msg_b64 is not base64"))?;
    let (sig, ad) = cred.answer(&msg, purpose)?;
    let mut cmd = json!({
        "challenge_id": id,
        "handle": hex::encode(&cred.handle),
        "signature_b64": B64.encode(&sig),
    });
    if let Some(ad) = ad {
        cmd["authenticator_data_b64"] = json!(B64.encode(&ad));
    }
    http(
        addr,
        "POST",
        "/v1/command",
        Some(token),
        Some(&cmd.to_string()),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refuses_a_swapped_purpose_or_door() {
        let c = SoftCredential::new(
            SigningKey::from_slice(&[3; 32]).unwrap(),
            [1; 16],
            Binding::Es256,
        );
        let m = wire::build_msg(wire::Purpose::Home, &wire::LAN_DOOR_ID, 1, &[0; 32]);
        assert!(c.answer(&m, wire::Purpose::Away).is_err());
        assert!(c.answer(&m, wire::Purpose::Home).is_ok());
        let m = wire::build_msg(wire::Purpose::Away, &[1; 16], 1, &[0; 32]);
        assert!(c.answer(&m, wire::Purpose::Away).is_err());
    }

    #[test]
    fn keygen_writes_a_private_file_and_loads() {
        let p = std::env::temp_dir().join(format!(
            "hk-client-{}",
            hex::encode(&crate::rng::array::<8>().unwrap())
        ));
        let pk = keygen(&p).unwrap();
        assert!(keygen(&p).is_err(), "overwrote a key file");
        let c = SoftCredential::load(&p, [1; 16], Binding::Es256).unwrap();
        assert_eq!(c.public_key(), pk);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&p).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
        std::fs::remove_file(&p).unwrap();
    }
}
