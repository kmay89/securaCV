use anyhow::{anyhow, Result};

use crate::vault::crypto::decode_aad;

const V2_MAGIC: &[u8; 4] = b"VLT2";

/// Maximum allowed envelope ID length (same as vault/mod.rs).
const MAX_ENVELOPE_ID_LEN: usize = 128;

/// Maximum allowed ciphertext size (1 GiB) to prevent memory exhaustion.
/// This is generous for evidence storage but prevents DoS attacks.
const MAX_CIPHERTEXT_LEN: usize = 1024 * 1024 * 1024;

/// Maximum allowed AAD size (1 KiB) - AAD contains envelope_id + ruleset_hash.
const MAX_AAD_LEN: usize = 1024;

/// Maximum allowed KEM ciphertext size (8 KiB) - sufficient for ML-KEM-768.
const MAX_KEM_CT_LEN: usize = 8 * 1024;

/// Maximum allowed algorithm identifier length.
const MAX_ALG_LEN: usize = 64;

#[derive(Clone, Debug)]
pub enum VaultEnvelope {
    V1(EnvelopeV1),
    V2(EnvelopeV2),
}

impl VaultEnvelope {
    pub fn encode(&self) -> Result<Vec<u8>> {
        match self {
            VaultEnvelope::V1(envelope) => envelope.encode(),
            VaultEnvelope::V2(envelope) => envelope.encode(),
        }
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.starts_with(V2_MAGIC) {
            Ok(VaultEnvelope::V2(EnvelopeV2::decode(bytes)?))
        } else {
            Ok(VaultEnvelope::V1(EnvelopeV1::decode(bytes)?))
        }
    }

    /// Returns the plaintext length (ciphertext without the authentication tag).
    pub fn ciphertext_len(&self) -> usize {
        match self {
            VaultEnvelope::V1(envelope) => envelope.ciphertext.len(),
            // V2 stores ciphertext + 16-byte tag together, so subtract tag size
            VaultEnvelope::V2(envelope) => envelope.ciphertext.len().saturating_sub(16),
        }
    }

    pub fn validate(&self, envelope_id: &str, ruleset_hash: [u8; 32]) -> Result<()> {
        match self {
            VaultEnvelope::V1(envelope) => envelope.validate(envelope_id, ruleset_hash),
            VaultEnvelope::V2(envelope) => envelope.validate(envelope_id, ruleset_hash),
        }
    }
}

#[derive(Clone, Debug)]
pub struct EnvelopeV1 {
    pub envelope_id: String,
    pub ruleset_hash: [u8; 32],
    pub nonce: [u8; 12],
    pub tag: [u8; 16],
    pub ciphertext: Vec<u8>,
}

impl EnvelopeV1 {
    pub fn new(
        envelope_id: String,
        ruleset_hash: [u8; 32],
        nonce: [u8; 12],
        tag: [u8; 16],
        ciphertext: Vec<u8>,
    ) -> Self {
        Self {
            envelope_id,
            ruleset_hash,
            nonce,
            tag,
            ciphertext,
        }
    }

    fn validate(&self, envelope_id: &str, ruleset_hash: [u8; 32]) -> Result<()> {
        if self.envelope_id != envelope_id {
            return Err(anyhow!("vault envelope id mismatch"));
        }
        if self.ruleset_hash != ruleset_hash {
            return Err(anyhow!("vault envelope ruleset mismatch"));
        }
        Ok(())
    }

    fn encode(&self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        let id_bytes = self.envelope_id.as_bytes();
        out.extend_from_slice(&(id_bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(id_bytes);
        out.extend_from_slice(&self.ruleset_hash);
        out.extend_from_slice(&self.nonce);
        out.extend_from_slice(&self.tag);
        out.extend_from_slice(&(self.ciphertext.len() as u32).to_le_bytes());
        out.extend_from_slice(&self.ciphertext);
        Ok(out)
    }

    fn decode(bytes: &[u8]) -> Result<Self> {
        let mut cursor = 0usize;
        let id_len = read_u32(bytes, &mut cursor)? as usize;
        if id_len > MAX_ENVELOPE_ID_LEN {
            return Err(anyhow!(
                "envelope id length {} exceeds maximum {}",
                id_len,
                MAX_ENVELOPE_ID_LEN
            ));
        }
        let id_bytes = read_slice(bytes, &mut cursor, id_len)?;
        let envelope_id = std::str::from_utf8(id_bytes)
            .map_err(|_| anyhow!("invalid envelope id encoding"))?
            .to_string();
        let ruleset_hash_bytes = read_slice(bytes, &mut cursor, 32)?;
        let mut ruleset_hash = [0u8; 32];
        ruleset_hash.copy_from_slice(ruleset_hash_bytes);
        let nonce_bytes = read_slice(bytes, &mut cursor, 12)?;
        let mut nonce = [0u8; 12];
        nonce.copy_from_slice(nonce_bytes);
        let tag_bytes = read_slice(bytes, &mut cursor, 16)?;
        let mut tag = [0u8; 16];
        tag.copy_from_slice(tag_bytes);
        let ct_len = read_u32(bytes, &mut cursor)? as usize;
        if ct_len > MAX_CIPHERTEXT_LEN {
            return Err(anyhow!(
                "ciphertext length {} exceeds maximum {}",
                ct_len,
                MAX_CIPHERTEXT_LEN
            ));
        }
        let ciphertext = read_slice(bytes, &mut cursor, ct_len)?.to_vec();
        Ok(Self {
            envelope_id,
            ruleset_hash,
            nonce,
            tag,
            ciphertext,
        })
    }
}

#[derive(Clone, Debug)]
pub struct EnvelopeV2 {
    pub version: u8,
    pub aead_alg: String,
    pub nonce: Vec<u8>,
    pub aad: Vec<u8>,
    pub ciphertext: Vec<u8>,
    pub kem_alg: String,
    pub kem_ct: Vec<u8>,
    pub kdf_info: Vec<u8>,
    pub classical_wrap: Option<Vec<u8>>,
}

impl EnvelopeV2 {
    fn validate(&self, envelope_id: &str, ruleset_hash: [u8; 32]) -> Result<()> {
        let aad = decode_aad(&self.aad)?;
        if aad.envelope_id != envelope_id {
            return Err(anyhow!("vault envelope id mismatch"));
        }
        if aad.ruleset_hash != ruleset_hash {
            return Err(anyhow!("vault envelope ruleset mismatch"));
        }
        Ok(())
    }

    fn encode(&self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        out.extend_from_slice(V2_MAGIC);
        out.push(self.version);
        write_bytes(&mut out, self.aead_alg.as_bytes());
        write_bytes(&mut out, &self.nonce);
        write_bytes(&mut out, &self.aad);
        write_bytes(&mut out, &self.ciphertext);
        write_bytes(&mut out, self.kem_alg.as_bytes());
        write_bytes(&mut out, &self.kem_ct);
        write_bytes(&mut out, &self.kdf_info);
        match &self.classical_wrap {
            Some(wrap) => {
                out.push(1);
                write_bytes(&mut out, wrap);
            }
            None => {
                out.push(0);
            }
        }
        Ok(out)
    }

    fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < V2_MAGIC.len() + 1 {
            return Err(anyhow!("invalid envelope encoding"));
        }
        if &bytes[..4] != V2_MAGIC {
            return Err(anyhow!("invalid envelope encoding"));
        }
        let mut cursor = V2_MAGIC.len();
        let version = read_u8(bytes, &mut cursor)?;
        if version != 2 {
            return Err(anyhow!("unsupported vault envelope version"));
        }
        let aead_alg = read_string_bounded(bytes, &mut cursor, MAX_ALG_LEN)?;
        let nonce = read_vec_bounded(bytes, &mut cursor, 32)?; // Max 32 bytes for any nonce
        let aad = read_vec_bounded(bytes, &mut cursor, MAX_AAD_LEN)?;
        let ciphertext = read_vec_bounded(bytes, &mut cursor, MAX_CIPHERTEXT_LEN)?;
        let kem_alg = read_string_bounded(bytes, &mut cursor, MAX_ALG_LEN)?;
        let kem_ct = read_vec_bounded(bytes, &mut cursor, MAX_KEM_CT_LEN)?;
        let kdf_info = read_vec_bounded(bytes, &mut cursor, 64)?; // Max 64 bytes for KDF info
        let has_wrap = read_u8(bytes, &mut cursor)?;
        let classical_wrap = if has_wrap == 1 {
            Some(read_vec_bounded(bytes, &mut cursor, 128)?) // Max 128 bytes for wrap
        } else {
            None
        };
        Ok(Self {
            version,
            aead_alg,
            nonce,
            aad,
            ciphertext,
            kem_alg,
            kem_ct,
            kdf_info,
            classical_wrap,
        })
    }
}

fn read_u8(bytes: &[u8], cursor: &mut usize) -> Result<u8> {
    if *cursor + 1 > bytes.len() {
        return Err(anyhow!("invalid envelope encoding"));
    }
    let out = bytes[*cursor];
    *cursor += 1;
    Ok(out)
}

pub(crate) fn read_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32> {
    let slice = read_slice(bytes, cursor, 4)?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

pub(crate) fn read_slice<'a>(bytes: &'a [u8], cursor: &mut usize, len: usize) -> Result<&'a [u8]> {
    if *cursor + len > bytes.len() {
        return Err(anyhow!("invalid envelope encoding"));
    }
    let out = &bytes[*cursor..*cursor + len];
    *cursor += len;
    Ok(out)
}

fn read_vec_bounded(bytes: &[u8], cursor: &mut usize, max_len: usize) -> Result<Vec<u8>> {
    let len = read_u32(bytes, cursor)? as usize;
    if len > max_len {
        return Err(anyhow!(
            "field length {} exceeds maximum allowed {}",
            len,
            max_len
        ));
    }
    let slice = read_slice(bytes, cursor, len)?;
    Ok(slice.to_vec())
}

fn read_string_bounded(bytes: &[u8], cursor: &mut usize, max_len: usize) -> Result<String> {
    let len = read_u32(bytes, cursor)? as usize;
    if len > max_len {
        return Err(anyhow!(
            "string length {} exceeds maximum allowed {}",
            len,
            max_len
        ));
    }
    let slice = read_slice(bytes, cursor, len)?;
    let s = std::str::from_utf8(slice).map_err(|_| anyhow!("invalid envelope encoding"))?;
    Ok(s.to_string())
}

fn write_bytes(out: &mut Vec<u8>, bytes: &[u8]) {
    out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(bytes);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn envelope_v1_rejects_oversized_envelope_id() {
        // Craft a malicious V1 envelope with an oversized ID length
        let mut malicious = Vec::new();
        // Write an envelope ID length that exceeds the maximum
        let oversized_len = (MAX_ENVELOPE_ID_LEN + 100) as u32;
        malicious.extend_from_slice(&oversized_len.to_le_bytes());
        // Add dummy data for the ID
        malicious.extend_from_slice(&vec![b'a'; MAX_ENVELOPE_ID_LEN + 100]);
        // Add ruleset hash (32 bytes)
        malicious.extend_from_slice(&[0u8; 32]);
        // Add nonce (12 bytes)
        malicious.extend_from_slice(&[0u8; 12]);
        // Add tag (16 bytes)
        malicious.extend_from_slice(&[0u8; 16]);
        // Add ciphertext length and data
        malicious.extend_from_slice(&10u32.to_le_bytes());
        malicious.extend_from_slice(&[0u8; 10]);

        let result = EnvelopeV1::decode(&malicious);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds maximum"));
    }

    #[test]
    fn envelope_v1_rejects_oversized_ciphertext() {
        // Craft a malicious V1 envelope with an oversized ciphertext length
        let mut malicious = Vec::new();
        // Write valid envelope ID
        let id = b"valid-id";
        malicious.extend_from_slice(&(id.len() as u32).to_le_bytes());
        malicious.extend_from_slice(id);
        // Add ruleset hash (32 bytes)
        malicious.extend_from_slice(&[0u8; 32]);
        // Add nonce (12 bytes)
        malicious.extend_from_slice(&[0u8; 12]);
        // Add tag (16 bytes)
        malicious.extend_from_slice(&[0u8; 16]);
        // Write a ciphertext length that exceeds the maximum
        let oversized_len = (MAX_CIPHERTEXT_LEN + 1) as u32;
        malicious.extend_from_slice(&oversized_len.to_le_bytes());
        // We don't need actual data - the length check should fail first

        let result = EnvelopeV1::decode(&malicious);
        assert!(result.is_err());
        // Could fail on length check or on bounds check - both are acceptable
    }

    #[test]
    fn envelope_v2_rejects_oversized_fields() {
        // Craft a malicious V2 envelope with oversized algorithm string
        let mut malicious = Vec::new();
        malicious.extend_from_slice(V2_MAGIC);
        malicious.push(2); // version
                           // Write an algorithm length that exceeds the maximum
        let oversized_len = (MAX_ALG_LEN + 100) as u32;
        malicious.extend_from_slice(&oversized_len.to_le_bytes());
        malicious.extend_from_slice(&vec![b'a'; MAX_ALG_LEN + 100]);

        let result = EnvelopeV2::decode(&malicious);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds maximum"));
    }

    #[test]
    fn envelope_v2_rejects_oversized_aad() {
        // Craft a malicious V2 envelope with oversized AAD
        let mut malicious = Vec::new();
        malicious.extend_from_slice(V2_MAGIC);
        malicious.push(2); // version
                           // Valid algorithm
        write_bytes(&mut malicious, b"ChaCha20-Poly1305");
        // Valid nonce
        write_bytes(&mut malicious, &[0u8; 12]);
        // Oversized AAD
        let oversized_len = (MAX_AAD_LEN + 100) as u32;
        malicious.extend_from_slice(&oversized_len.to_le_bytes());
        malicious.extend_from_slice(&vec![0u8; MAX_AAD_LEN + 100]);

        let result = EnvelopeV2::decode(&malicious);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("exceeds maximum"));
    }

    #[test]
    fn envelope_v2_rejects_invalid_version() {
        let mut malicious = Vec::new();
        malicious.extend_from_slice(V2_MAGIC);
        malicious.push(99); // invalid version

        let result = EnvelopeV2::decode(&malicious);
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("unsupported vault envelope version"));
    }

    #[test]
    fn envelope_decode_rejects_truncated_input() {
        // Test various truncated inputs
        let too_short = vec![0u8; 3];
        assert!(VaultEnvelope::decode(&too_short).is_err());

        let v2_header_only = V2_MAGIC.to_vec();
        assert!(VaultEnvelope::decode(&v2_header_only).is_err());
    }

    #[test]
    fn envelope_round_trip_works() {
        let v1 = EnvelopeV1::new(
            "test-envelope".to_string(),
            [1u8; 32],
            [2u8; 12],
            [3u8; 16],
            vec![4u8; 100],
        );
        let encoded = v1.encode().unwrap();
        let decoded = EnvelopeV1::decode(&encoded).unwrap();
        assert_eq!(decoded.envelope_id, "test-envelope");
        assert_eq!(decoded.ciphertext.len(), 100);
    }
}
