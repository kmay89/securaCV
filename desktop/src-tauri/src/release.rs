//! Release-byte verification shared by the ESP32 and WE2 flashing paths.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};

pub fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut out = String::with_capacity(64);
    for byte in digest {
        use std::fmt::Write as _;
        let _ = write!(out, "{byte:02x}");
    }
    out
}

fn decode_hex<const N: usize>(value: &str, label: &str) -> Result<[u8; N], String> {
    if value.len() != N * 2 || !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("{label} must be exactly {} hex characters", N * 2));
    }
    let mut out = [0u8; N];
    for (index, slot) in out.iter_mut().enumerate() {
        *slot = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16)
            .map_err(|_| format!("{label} contains invalid hex"))?;
    }
    Ok(out)
}

pub fn verify_size_and_sha(
    bytes: &[u8],
    expected_size: u64,
    expected_sha: &str,
) -> Result<String, String> {
    if bytes.len() as u64 != expected_size {
        return Err(format!(
            "downloaded {} bytes, but the release promised {expected_size}; refusing to write",
            bytes.len()
        ));
    }
    let got = sha256_hex(bytes);
    if !got.eq_ignore_ascii_case(expected_sha) {
        return Err(format!(
            "SHA-256 mismatch: release {}…, download {}…; refusing to write",
            &expected_sha[..expected_sha.len().min(16)],
            &got[..16]
        ));
    }
    Ok(got)
}

/// Verify the release signature over `uint32_le(size) || sha256(image)`.
///
/// An all-zero public key is the repository's explicit pre-ceremony state. In
/// that state SHA-256 is still mandatory and this returns `checksum-only`.
/// Once a real key is pinned, a missing or invalid signature fails closed.
pub fn verify_signature(
    bytes_len: usize,
    sha_hex: &str,
    signature_hex: Option<&str>,
    pubkey_hex: &str,
) -> Result<&'static str, String> {
    let pubkey = decode_hex::<32>(pubkey_hex, "release public key")?;
    if pubkey.iter().all(|b| *b == 0) {
        return Ok("checksum-only");
    }
    let signature_hex = signature_hex.ok_or_else(|| {
        "the official release is missing its required Ed25519 signature; refusing to write"
            .to_string()
    })?;
    let signature = Signature::from_bytes(&decode_hex::<64>(signature_hex, "release signature")?);
    let verifying_key = VerifyingKey::from_bytes(&pubkey)
        .map_err(|_| "the pinned release public key is invalid".to_string())?;
    let digest = decode_hex::<32>(sha_hex, "SHA-256")?;
    let size = u32::try_from(bytes_len)
        .map_err(|_| "image is too large for the signed release format".to_string())?;
    let mut message = [0u8; 36];
    message[..4].copy_from_slice(&size.to_le_bytes());
    message[4..].copy_from_slice(&digest);
    verifying_key
        .verify(&message, &signature)
        .map_err(|_| "the image failed Ed25519 release-signature verification".to_string())?;
    Ok("ed25519+sha256")
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    #[test]
    fn sha256_matches_known_vector() {
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn size_or_hash_mismatch_fails_closed() {
        assert!(verify_size_and_sha(b"abc", 4, &sha256_hex(b"abc")).is_err());
        assert!(verify_size_and_sha(b"abc", 3, &"00".repeat(32)).is_err());
    }

    #[test]
    fn release_signature_scheme_matches_firmware() {
        let signing = SigningKey::from_bytes(&[7u8; 32]);
        let bytes = b"factory";
        let sha = sha256_hex(bytes);
        let mut message = Vec::from((bytes.len() as u32).to_le_bytes());
        message.extend(decode_hex::<32>(&sha, "sha").unwrap());
        let signature = signing.sign(&message);
        assert_eq!(
            verify_signature(
                bytes.len(),
                &sha,
                Some(&hex(signature.to_bytes())),
                &hex(signing.verifying_key().to_bytes())
            )
            .unwrap(),
            "ed25519+sha256"
        );
    }

    fn hex<const N: usize>(bytes: [u8; N]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
}
