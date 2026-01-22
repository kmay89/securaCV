//! Evidence vault storage for sealed raw media envelopes.
//!
//! The vault persists encrypted envelopes to a local, fixed path and
//! enforces break-glass gating for raw media export.

use anyhow::{anyhow, Result};
use rand::RngCore;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use zeroize::Zeroize;

use crate::{break_glass::BreakGlassToken, RawMediaBoundary};

/// Fixed local vault path to satisfy invariant requirements.
pub const DEFAULT_VAULT_PATH: &str = "vault/envelopes";

#[derive(Clone, Debug)]
pub struct VaultConfig {
    pub local_path: PathBuf,
}

impl Default for VaultConfig {
    fn default() -> Self {
        Self {
            local_path: PathBuf::from(DEFAULT_VAULT_PATH),
        }
    }
}

pub struct Vault {
    root: PathBuf,
}

impl Vault {
    pub fn new(cfg: VaultConfig) -> Result<Self> {
        fs::create_dir_all(&cfg.local_path)?;
        Ok(Self {
            root: cfg.local_path,
        })
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn seal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        raw_bytes: &mut Vec<u8>,
    ) -> Result<EnvelopeMetadata> {
        let sanitized = sanitize_envelope_id(envelope_id)?;
        let envelope_path = self.envelope_path(&sanitized);

        if envelope_path.exists() {
            return Err(anyhow!("vault envelope already exists"));
        }

        let mut clear_bytes =
            RawMediaBoundary::export_for_vault(raw_bytes, token, envelope_id, expected_ruleset_hash)?;
        let envelope = Envelope::seal(envelope_id, expected_ruleset_hash, &clear_bytes)?;
        clear_bytes.zeroize();

        let encoded = envelope.encode()?;
        write_atomic(&envelope_path, &encoded)?;

        Ok(EnvelopeMetadata {
            envelope_id: envelope_id.to_string(),
            ruleset_hash: expected_ruleset_hash,
            ciphertext_len: envelope.ciphertext.len(),
        })
    }

    pub fn unseal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
    ) -> Result<Vec<u8>> {
        let sanitized = sanitize_envelope_id(envelope_id)?;
        let envelope_path = self.envelope_path(&sanitized);
        let encoded = read_file(&envelope_path)?;
        let envelope = Envelope::decode(&encoded)?;
        envelope.validate(envelope_id, expected_ruleset_hash)?;

        let mut empty = Vec::new();
        RawMediaBoundary::export_for_vault(
            &mut empty,
            token,
            envelope_id,
            expected_ruleset_hash,
        )?;

        Ok(envelope.decrypt())
    }

    fn envelope_path(&self, envelope_id: &str) -> PathBuf {
        let mut path = self.root.clone();
        path.push(format!("{}.vault", envelope_id));
        path
    }
}

#[derive(Clone, Debug)]
pub struct EnvelopeMetadata {
    pub envelope_id: String,
    pub ruleset_hash: [u8; 32],
    pub ciphertext_len: usize,
}

#[derive(Clone, Debug)]
struct Envelope {
    envelope_id: String,
    ruleset_hash: [u8; 32],
    nonce: [u8; 32],
    ciphertext: Vec<u8>,
}

impl Envelope {
    fn seal(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
        clear: &[u8],
    ) -> Result<Self> {
        let mut nonce = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut nonce);
        let key = derive_ephemeral_key(&ruleset_hash, &nonce);
        let mut ciphertext = clear.to_vec();
        xor_cipher(&mut ciphertext, &key);

        Ok(Self {
            envelope_id: envelope_id.to_string(),
            ruleset_hash,
            nonce,
            ciphertext,
        })
    }

    fn decrypt(&self) -> Vec<u8> {
        let key = derive_ephemeral_key(&self.ruleset_hash, &self.nonce);
        let mut clear = self.ciphertext.clone();
        xor_cipher(&mut clear, &key);
        clear
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
        let nonce_bytes = read_slice(bytes, &mut cursor, 32)?;
        let mut nonce = [0u8; 32];
        nonce.copy_from_slice(nonce_bytes);
        let ct_len = read_u32(bytes, &mut cursor)? as usize;
        let ciphertext = read_slice(bytes, &mut cursor, ct_len)?.to_vec();
        Ok(Self {
            envelope_id,
            ruleset_hash,
            nonce,
            ciphertext,
        })
    }
}

fn sanitize_envelope_id(envelope_id: &str) -> Result<String> {
    let trimmed = envelope_id.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("vault envelope id cannot be empty"));
    }
    if !trimmed
        .chars()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_')
    {
        return Err(anyhow!(
            "vault envelope id must be lowercase [a-z0-9_-] only"
        ));
    }
    Ok(trimmed.to_string())
}

fn derive_ephemeral_key(ruleset_hash: &[u8; 32], nonce: &[u8; 32]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(ruleset_hash);
    hasher.update(nonce);
    hasher.finalize().into()
}

fn xor_cipher(data: &mut [u8], key: &[u8; 32]) {
    for (idx, byte) in data.iter_mut().enumerate() {
        *byte ^= key[idx % key.len()];
    }
}

fn read_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32> {
    let slice = read_slice(bytes, cursor, 4)?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn read_slice<'a>(bytes: &'a [u8], cursor: &mut usize, len: usize) -> Result<&'a [u8]> {
    if *cursor + len > bytes.len() {
        return Err(anyhow!("invalid envelope encoding"));
    }
    let out = &bytes[*cursor..*cursor + len];
    *cursor += len;
    Ok(out)
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<()> {
    let tmp_path = path.with_extension("tmp");
    {
        let mut file = File::create(&tmp_path)?;
        file.write_all(data)?;
        file.sync_all()?;
    }
    fs::rename(tmp_path, path)?;
    Ok(())
}

fn read_file(path: &Path) -> Result<Vec<u8>> {
    let mut file = File::open(path)?;
    let mut buf = Vec::new();
    file.read_to_end(&mut buf)?;
    Ok(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{Approval, BreakGlass, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest};
    use ed25519_dalek::{Signer, SigningKey};
    use std::fs;

    fn make_break_glass_token(envelope_id: &str, ruleset_hash: [u8; 32]) -> BreakGlassToken {
        let bucket = crate::TimeBucket::now(600).expect("time bucket");
        let request = UnlockRequest::new(envelope_id, ruleset_hash, "test-export", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let signature = signing_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        result.expect("break-glass token")
    }

    #[test]
    fn vault_seal_requires_valid_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-1";
        let ruleset_hash = [1u8; 32];
        let mut token = make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        let meta = vault.seal(envelope_id, &mut token, ruleset_hash, &mut raw)?;
        assert_eq!(meta.ciphertext_len, b"raw bytes".len());
        assert!(raw.is_empty());
        Ok(())
    }

    #[test]
    fn vault_seal_rejects_invalid_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-2";
        let ruleset_hash = [2u8; 32];
        let mut token = BreakGlassToken::test_token_with(
            [0u8; 32],
            crate::TimeBucket::now(600)?,
            "wrong-envelope",
            ruleset_hash,
        );
        let mut raw = b"raw bytes".to_vec();
        assert!(vault
            .seal(envelope_id, &mut token, ruleset_hash, &mut raw)
            .is_err());
        Ok(())
    }

    #[test]
    fn vault_unseal_requires_quorum_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-3";
        let ruleset_hash = [3u8; 32];
        let mut token = make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(envelope_id, &mut token, ruleset_hash, &mut raw)?;

        let mut invalid = BreakGlassToken::test_token_with(
            [0u8; 32],
            crate::TimeBucket::now(600)?,
            envelope_id,
            ruleset_hash,
        );
        assert!(vault
            .unseal(envelope_id, &mut invalid, ruleset_hash)
            .is_err());

        let mut valid = make_break_glass_token(envelope_id, ruleset_hash);
        let clear = vault.unseal(envelope_id, &mut valid, ruleset_hash)?;
        assert_eq!(clear, b"raw bytes");
        Ok(())
    }

    #[test]
    fn vault_writes_under_root() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault_root = temp_dir.path().join("vault");
        let vault = Vault::new(VaultConfig {
            local_path: vault_root.clone(),
        })?;
        let envelope_id = "incident-4";
        let ruleset_hash = [4u8; 32];
        let mut token = make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(envelope_id, &mut token, ruleset_hash, &mut raw)?;

        let mut entries = fs::read_dir(vault_root)?.collect::<Result<Vec<_>, _>>()?;
        entries.sort_by_key(|entry| entry.path());
        assert_eq!(entries.len(), 1);
        assert!(entries[0].path().ends_with("incident-4.vault"));
        Ok(())
    }
}
