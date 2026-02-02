use anyhow::{anyhow, Result};

use crate::vault::crypto::decode_aad;

const V2_MAGIC: &[u8; 4] = b"VLT2";

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
        let aead_alg = read_string(bytes, &mut cursor)?;
        let nonce = read_vec(bytes, &mut cursor)?;
        let aad = read_vec(bytes, &mut cursor)?;
        let ciphertext = read_vec(bytes, &mut cursor)?;
        let kem_alg = read_string(bytes, &mut cursor)?;
        let kem_ct = read_vec(bytes, &mut cursor)?;
        let kdf_info = read_vec(bytes, &mut cursor)?;
        let has_wrap = read_u8(bytes, &mut cursor)?;
        let classical_wrap = if has_wrap == 1 {
            Some(read_vec(bytes, &mut cursor)?)
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

fn read_vec(bytes: &[u8], cursor: &mut usize) -> Result<Vec<u8>> {
    let len = read_u32(bytes, cursor)? as usize;
    let slice = read_slice(bytes, cursor, len)?;
    Ok(slice.to_vec())
}

fn read_string(bytes: &[u8], cursor: &mut usize) -> Result<String> {
    let len = read_u32(bytes, cursor)? as usize;
    let slice = read_slice(bytes, cursor, len)?;
    let s = std::str::from_utf8(slice).map_err(|_| anyhow!("invalid envelope encoding"))?;
    Ok(s.to_string())
}

fn write_bytes(out: &mut Vec<u8>, bytes: &[u8]) {
    out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(bytes);
}
