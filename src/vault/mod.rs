//! Evidence vault storage for sealed raw media envelopes.
//!
//! The vault persists encrypted envelopes to a local, fixed path and
//! enforces break-glass gating for raw media export.

use anyhow::{anyhow, Result};
use chacha20poly1305::{
    aead::{AeadInPlace, KeyInit},
    ChaCha20Poly1305, Key, Nonce, Tag,
};
use rand::RngCore;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use zeroize::Zeroize;

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use crate::{break_glass::BreakGlassToken, BreakGlassOutcome, RawFrame, RawMediaBoundary};
use ed25519_dalek::VerifyingKey;

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
    master_key: [u8; 32],
}

impl Vault {
    pub fn new(cfg: VaultConfig) -> Result<Self> {
        fs::create_dir_all(&cfg.local_path)?;
        let master_key = load_or_create_master_key(&cfg.local_path)?;
        Ok(Self {
            root: cfg.local_path,
            master_key,
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
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata> {
        let mut clear_bytes = RawMediaBoundary::export_for_vault(
            raw_bytes,
            token,
            envelope_id,
            expected_ruleset_hash,
            verifying_key,
            receipt_lookup,
        )?;
        self.seal_bytes(envelope_id, expected_ruleset_hash, &mut clear_bytes)
    }

    pub fn seal_frame(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        frame: RawFrame,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata> {
        let mut clear_bytes = frame.export_for_vault(
            token,
            envelope_id,
            expected_ruleset_hash,
            verifying_key,
            receipt_lookup,
        )?;
        self.seal_bytes(envelope_id, expected_ruleset_hash, &mut clear_bytes)
    }

    pub fn unseal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
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
            verifying_key,
            receipt_lookup,
        )?;

        envelope.decrypt(&self.master_key)
    }

    fn envelope_path(&self, envelope_id: &str) -> PathBuf {
        let mut path = self.root.clone();
        path.push(format!("{}.vault", envelope_id));
        path
    }

    fn seal_bytes(
        &self,
        envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        clear_bytes: &mut Vec<u8>,
    ) -> Result<EnvelopeMetadata> {
        let sanitized = sanitize_envelope_id(envelope_id)?;
        let envelope_path = self.envelope_path(&sanitized);

        if envelope_path.exists() {
            return Err(anyhow!("vault envelope already exists"));
        }

        let envelope = Envelope::seal(
            envelope_id,
            expected_ruleset_hash,
            clear_bytes,
            &self.master_key,
        )?;
        clear_bytes.zeroize();

        let encoded = envelope.encode()?;
        write_atomic(&envelope_path, &encoded)?;

        Ok(EnvelopeMetadata {
            envelope_id: envelope_id.to_string(),
            ruleset_hash: expected_ruleset_hash,
            ciphertext_len: envelope.ciphertext.len(),
        })
    }
}

impl Drop for Vault {
    fn drop(&mut self) {
        self.master_key.zeroize();
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
    nonce: [u8; 12],
    tag: [u8; 16],
    ciphertext: Vec<u8>,
}

impl Envelope {
    fn seal(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
        clear: &[u8],
        master_key: &[u8; 32],
    ) -> Result<Self> {
        let mut nonce = [0u8; 12];
        rand::thread_rng().fill_bytes(&mut nonce);
        let mut ciphertext = clear.to_vec();
        let tag = encrypt_in_place(
            master_key,
            envelope_id,
            &ruleset_hash,
            &nonce,
            &mut ciphertext,
        )?;

        Ok(Self {
            envelope_id: envelope_id.to_string(),
            ruleset_hash,
            nonce,
            tag,
            ciphertext,
        })
    }

    fn decrypt(&self, master_key: &[u8; 32]) -> Result<Vec<u8>> {
        let mut clear = self.ciphertext.clone();
        decrypt_in_place(
            master_key,
            &self.envelope_id,
            &self.ruleset_hash,
            &self.nonce,
            &self.tag,
            &mut clear,
        )?;
        Ok(clear)
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

pub(crate) fn sanitize_envelope_id(envelope_id: &str) -> Result<String> {
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

fn envelope_aad(envelope_id: &str, ruleset_hash: &[u8; 32]) -> Vec<u8> {
    let mut aad = Vec::with_capacity(envelope_id.len() + ruleset_hash.len());
    aad.extend_from_slice(envelope_id.as_bytes());
    aad.extend_from_slice(ruleset_hash);
    aad
}

fn encrypt_in_place(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    nonce: &[u8; 12],
    buffer: &mut [u8],
) -> Result<[u8; 16]> {
    let cipher = ChaCha20Poly1305::new(Key::from_slice(master_key));
    let aad = envelope_aad(envelope_id, ruleset_hash);
    let tag = cipher
        .encrypt_in_place_detached(Nonce::from_slice(nonce), &aad, buffer)
        .map_err(|_| anyhow!("vault encryption failed"))?;
    Ok(tag.into())
}

fn decrypt_in_place(
    master_key: &[u8; 32],
    envelope_id: &str,
    ruleset_hash: &[u8; 32],
    nonce: &[u8; 12],
    tag: &[u8; 16],
    buffer: &mut [u8],
) -> Result<()> {
    let cipher = ChaCha20Poly1305::new(Key::from_slice(master_key));
    let aad = envelope_aad(envelope_id, ruleset_hash);
    let tag = Tag::from_slice(tag);
    cipher
        .decrypt_in_place_detached(Nonce::from_slice(nonce), &aad, buffer, tag)
        .map_err(|_| anyhow!("vault decryption failed"))?;
    Ok(())
}

fn load_or_create_master_key(root: &Path) -> Result<[u8; 32]> {
    let key_path = root.join("master.key");
    if key_path.exists() {
        let bytes = read_file(&key_path)?;
        if bytes.len() != 32 {
            return Err(anyhow!("vault master key length mismatch"));
        }
        #[cfg(unix)]
        {
            let mode = fs::metadata(&key_path)?.permissions().mode() & 0o777;
            if mode != 0o600 {
                fs::set_permissions(&key_path, fs::Permissions::from_mode(0o600))?;
            }
        }
        let mut key = [0u8; 32];
        key.copy_from_slice(&bytes);
        Ok(key)
    } else {
        let mut key = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut key);
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            options.mode(0o600);
        }
        let mut file = options.open(&key_path)?;
        file.write_all(&key)?;
        file.sync_all()?;
        Ok(key)
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
    use crate::break_glass::{
        Approval, BreakGlass, BreakGlassOutcome, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry,
        TrusteeId, UnlockRequest,
    };
    use crate::TimeBucket;
    use ed25519_dalek::{Signer, SigningKey, VerifyingKey};
    use sha2::{Digest, Sha256};
    use std::fs;

    fn make_break_glass_token(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
    ) -> (BreakGlassToken, VerifyingKey, [u8; 32]) {
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
        let mut token = result.expect("break-glass token");
        let device_signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let receipt_hash = [8u8; 32];
        token
            .attach_receipt_signature(receipt_hash, &device_signing_key)
            .expect("attach receipt signature");
        (token, device_signing_key.verifying_key(), receipt_hash)
    }

    fn make_raw_frame(data: &[u8]) -> RawFrame {
        let bucket = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let features: [u8; 32] = Sha256::digest(data).into();
        RawFrame::new(data.to_vec(), 640, 480, bucket, features)
    }

    #[test]
    fn vault_seal_requires_valid_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-1";
        let ruleset_hash = [1u8; 32];
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        let meta = vault.seal(
            envelope_id,
            &mut token,
            ruleset_hash,
            &mut raw,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;
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
            .seal(
                envelope_id,
                &mut token,
                ruleset_hash,
                &mut raw,
                &SigningKey::from_bytes(&[4u8; 32]).verifying_key(),
                |_| Ok(BreakGlassOutcome::Denied {
                    reason: "invalid".to_string(),
                }),
            )
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
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(
            envelope_id,
            &mut token,
            ruleset_hash,
            &mut raw,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;

        let mut invalid = BreakGlassToken::test_token_with(
            [0u8; 32],
            crate::TimeBucket::now(600)?,
            envelope_id,
            ruleset_hash,
        );
        assert!(vault
            .unseal(
                envelope_id,
                &mut invalid,
                ruleset_hash,
                &verifying_key,
                |_| Ok(BreakGlassOutcome::Granted),
            )
            .is_err());

        let (mut valid, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let clear = vault.unseal(
            envelope_id,
            &mut valid,
            ruleset_hash,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;
        assert_eq!(clear, b"raw bytes");
        Ok(())
    }

    #[test]
    fn vault_unseal_rejects_forged_token_file() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-forged";
        let ruleset_hash = [10u8; 32];
        let (mut seal_token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(
            envelope_id,
            &mut seal_token,
            ruleset_hash,
            &mut raw,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;

        let (token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut token_file = BreakGlassTokenFile::from_token(&token)?;
        token_file.device_signature = hex::encode([1u8; 64]);
        let mut forged = token_file.into_token()?;

        let result = vault.unseal(
            envelope_id,
            &mut forged,
            ruleset_hash,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        );
        assert!(result.is_err());
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
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(
            envelope_id,
            &mut token,
            ruleset_hash,
            &mut raw,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;

        let mut entries = fs::read_dir(vault_root)?.collect::<Result<Vec<_>, _>>()?;
        entries.sort_by_key(|entry| entry.path());
        assert_eq!(entries.len(), 2);
        let paths: Vec<_> = entries.iter().map(|entry| entry.path()).collect();
        assert!(paths.iter().any(|path| path.ends_with("incident-4.vault")));
        assert!(paths.iter().any(|path| path.ends_with("master.key")));
        Ok(())
    }

    #[test]
    fn vault_unseal_rejects_expired_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-5";
        let ruleset_hash = [5u8; 32];
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut raw = b"raw bytes".to_vec();
        vault.seal(
            envelope_id,
            &mut token,
            ruleset_hash,
            &mut raw,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;

        let mut expired = BreakGlassToken::test_token_with(
            [9u8; 32],
            TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            envelope_id,
            ruleset_hash,
        );
        assert!(vault
            .unseal(
                envelope_id,
                &mut expired,
                ruleset_hash,
                &verifying_key,
                |_| Ok(BreakGlassOutcome::Granted),
            )
            .is_err());
        Ok(())
    }

    #[test]
    fn vault_seal_frame_uses_break_glass_token() -> Result<()> {
        let temp_dir = tempfile::tempdir()?;
        let vault = Vault::new(VaultConfig {
            local_path: temp_dir.path().join("vault"),
        })?;
        let envelope_id = "incident-6";
        let ruleset_hash = [6u8; 32];
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let frame = make_raw_frame(b"raw bytes");
        let meta = vault.seal_frame(
            envelope_id,
            &mut token,
            ruleset_hash,
            frame,
            &verifying_key,
            |hash| {
                if hash != &receipt_hash {
                    return Err(anyhow!("unexpected receipt hash"));
                }
                Ok(BreakGlassOutcome::Granted)
            },
        )?;
        assert_eq!(meta.ciphertext_len, b"raw bytes".len());
        Ok(())
    }

    #[test]
    fn envelope_rejects_wrong_key_or_tag() -> Result<()> {
        let master_key = [9u8; 32];
        let envelope = Envelope::seal("incident-7", [7u8; 32], b"raw bytes", &master_key)?;
        let wrong_key = [10u8; 32];
        assert!(envelope.decrypt(&wrong_key).is_err());

        let mut tampered = envelope.clone();
        tampered.tag[0] ^= 0b1010_1010;
        assert!(tampered.decrypt(&master_key).is_err());
        Ok(())
    }

    #[test]
    fn envelope_ciphertext_is_not_plaintext() -> Result<()> {
        let master_key = [11u8; 32];
        let clear = b"raw bytes";
        let envelope = Envelope::seal("incident-8", [8u8; 32], clear, &master_key)?;
        assert_ne!(envelope.ciphertext, clear);
        Ok(())
    }
}
