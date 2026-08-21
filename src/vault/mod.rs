//! Evidence vault storage for sealed raw media envelopes.
//!
//! The vault persists encrypted envelopes to a local, fixed path and
//! enforces break-glass gating for raw media export.

use anyhow::{anyhow, Result};
use rand::rngs::SysRng;
use rand::TryRng;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use zeroize::{Zeroize, Zeroizing};

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use crate::{break_glass::BreakGlassToken, BreakGlassOutcome, RawFrame, RawMediaBoundary};
use ed25519_dalek::VerifyingKey;

pub mod crypto;

// `format` stays private in every build that ships.
//
// The parser genuinely needs to be fuzzable: `unseal()` decodes the on-disk
// container *before* any break-glass check runs, so those bytes are
// attacker-controlled the moment someone has touched the card — the exact
// adversary the vault exists for. But `#[doc(hidden)] pub` was the wrong way
// to get there, and the reason is Invariant V (`AGENTS.md`: raw media access
// requires N-of-M trustee approval). `crypto::decrypt_v1`/`decrypt_v2` are
// already public and take these envelope types directly, and the master key
// sits under a known vault root — so a public `VaultEnvelope::decode` completes
// an in-process path from container bytes to plaintext that never presents a
// `BreakGlassToken`. `#[doc(hidden)]` hides a symbol from rustdoc; it does not
// restrict access to it.
//
// A determined attacker could reimplement this format from the source, so the
// delta is defense-in-depth rather than a hard boundary — which is exactly the
// kind of change `docs/governance_and_invariants.md` says to reject when it
// buys nothing. Gating on `--cfg fuzzing` (set by cargo-fuzz, and by nothing
// else) costs nothing and keeps the shipped API honest.
#[cfg(not(fuzzing))]
mod format;
#[cfg(fuzzing)]
#[doc(hidden)]
pub mod format;

use crate::vault::crypto::{decrypt_v1, decrypt_v2, seal_v2, KemKeypair, VaultCryptoMode};
use crate::vault::format::VaultEnvelope;

/// Fixed local vault path to satisfy invariant requirements.
pub const DEFAULT_VAULT_PATH: &str = "vault/envelopes";

/// How the vault master key is protected at rest.
///
/// `Plaintext` (default) is the legacy behavior: a raw `master.key` file beside
/// the ciphertext. `Passphrase` wraps the master key under an Argon2id KEK in a
/// `master.keyguard` container, so possession of the vault directory alone is no
/// longer sufficient to decrypt evidence — the operator-held passphrase (never
/// written to disk) is also required. Defense-in-depth against vault-file theft;
/// it is not a cryptographic quorum (a single passphrase holder still decrypts).
#[derive(Clone, Default)]
pub enum VaultKeyMaterial {
    #[default]
    Plaintext,
    Passphrase(Zeroizing<String>),
}

impl std::fmt::Debug for VaultKeyMaterial {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Plaintext => write!(f, "Plaintext"),
            // Never render the passphrase.
            Self::Passphrase(_) => write!(f, "Passphrase(<redacted>)"),
        }
    }
}

impl VaultKeyMaterial {
    /// Resolve from `SECURACV_VAULT_PASSPHRASE`: a non-empty value selects the
    /// passphrase keyguard; otherwise legacy plaintext. Production vault
    /// construction sites use this so passphrase mode is a pure env opt-in.
    pub fn from_env() -> Self {
        match std::env::var("SECURACV_VAULT_PASSPHRASE") {
            Ok(s) if !s.trim().is_empty() => Self::Passphrase(Zeroizing::new(s)),
            _ => Self::Plaintext,
        }
    }
}

#[derive(Clone, Debug)]
pub struct VaultConfig {
    pub local_path: PathBuf,
    pub crypto_mode: VaultCryptoMode,
    /// Master-key protection at rest (default: legacy plaintext `master.key`).
    pub key_material: VaultKeyMaterial,
}

impl Default for VaultConfig {
    fn default() -> Self {
        Self {
            local_path: PathBuf::from(DEFAULT_VAULT_PATH),
            crypto_mode: VaultCryptoMode::Classical,
            key_material: VaultKeyMaterial::default(),
        }
    }
}

pub trait VaultStore: Send + Sync {
    fn root(&self) -> Option<&Path>;

    fn seal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        raw_bytes: &mut Vec<u8>,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata>;

    fn seal_frame(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        frame: RawFrame,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata>;

    fn unseal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<Vec<u8>>;
}

pub struct Vault<S: VaultStore = FilesystemVaultStore> {
    store: S,
}

impl Vault<FilesystemVaultStore> {
    pub fn new(cfg: VaultConfig) -> Result<Self> {
        Ok(Self {
            store: FilesystemVaultStore::new(cfg)?,
        })
    }
}

impl<S: VaultStore> Vault<S> {
    pub fn with_store(store: S) -> Self {
        Self { store }
    }

    pub fn root(&self) -> Option<&Path> {
        self.store.root()
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
        self.store.seal(
            envelope_id,
            token,
            expected_ruleset_hash,
            raw_bytes,
            verifying_key,
            receipt_lookup,
        )
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
        self.store.seal_frame(
            envelope_id,
            token,
            expected_ruleset_hash,
            frame,
            verifying_key,
            receipt_lookup,
        )
    }

    pub fn unseal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<Vec<u8>> {
        self.store.unseal(
            envelope_id,
            token,
            expected_ruleset_hash,
            verifying_key,
            receipt_lookup,
        )
    }
}

pub struct FilesystemVaultStore {
    root: PathBuf,
    master_key: [u8; 32],
    crypto_mode: VaultCryptoMode,
    kem_keypair: Option<KemKeypair>,
}

impl FilesystemVaultStore {
    pub fn new(cfg: VaultConfig) -> Result<Self> {
        fs::create_dir_all(&cfg.local_path)?;
        let master_key = load_or_create_master_key(&cfg.local_path, &cfg.key_material)?;
        let kem_keypair = load_or_create_kem_keypair(&cfg.local_path, cfg.crypto_mode)?;
        Ok(Self {
            root: cfg.local_path,
            master_key,
            crypto_mode: cfg.crypto_mode,
            kem_keypair,
        })
    }

    fn seal_impl(
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

    fn seal_frame_impl(
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

    fn unseal_impl(
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
        let envelope = VaultEnvelope::decode(&encoded)?;
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

        match envelope {
            VaultEnvelope::V1(envelope) => decrypt_v1(&envelope, &self.master_key),
            VaultEnvelope::V2(envelope) => {
                decrypt_v2(&envelope, &self.master_key, self.kem_keypair.as_ref())
            }
        }
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
        // The raw-media plaintext must be scrubbed on EVERY exit — including
        // the "already exists" early return and a seal_v2 failure — not only
        // on success, so an error path never leaves cleartext lingering in the
        // caller's buffer.
        let result = self.seal_bytes_inner(envelope_id, expected_ruleset_hash, clear_bytes);
        clear_bytes.zeroize();
        result
    }

    fn seal_bytes_inner(
        &self,
        envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        clear_bytes: &[u8],
    ) -> Result<EnvelopeMetadata> {
        let sanitized = sanitize_envelope_id(envelope_id)?;
        let envelope_path = self.envelope_path(&sanitized);

        if envelope_path.exists() {
            return Err(anyhow!("vault envelope already exists"));
        }

        let envelope = VaultEnvelope::V2(seal_v2(
            envelope_id,
            expected_ruleset_hash,
            clear_bytes,
            self.crypto_mode,
            &self.master_key,
            self.kem_keypair.as_ref(),
        )?);

        let encoded = envelope.encode()?;
        write_atomic(&envelope_path, &encoded)?;

        Ok(EnvelopeMetadata {
            envelope_id: envelope_id.to_string(),
            ruleset_hash: expected_ruleset_hash,
            ciphertext_len: envelope.ciphertext_len(),
        })
    }
}

impl VaultStore for FilesystemVaultStore {
    fn root(&self) -> Option<&Path> {
        Some(&self.root)
    }

    fn seal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        raw_bytes: &mut Vec<u8>,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata> {
        self.seal_impl(
            envelope_id,
            token,
            expected_ruleset_hash,
            raw_bytes,
            verifying_key,
            receipt_lookup,
        )
    }

    fn seal_frame(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        frame: RawFrame,
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<EnvelopeMetadata> {
        self.seal_frame_impl(
            envelope_id,
            token,
            expected_ruleset_hash,
            frame,
            verifying_key,
            receipt_lookup,
        )
    }

    fn unseal(
        &self,
        envelope_id: &str,
        token: &mut BreakGlassToken,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<BreakGlassOutcome>,
    ) -> Result<Vec<u8>> {
        self.unseal_impl(
            envelope_id,
            token,
            expected_ruleset_hash,
            verifying_key,
            receipt_lookup,
        )
    }
}

impl Drop for FilesystemVaultStore {
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

/// Maximum allowed envelope ID length to prevent DoS via memory exhaustion.
/// 128 bytes is sufficient for any reasonable identifier scheme.
pub const MAX_ENVELOPE_ID_LEN: usize = 128;

pub(crate) fn sanitize_envelope_id(envelope_id: &str) -> Result<String> {
    let trimmed = envelope_id.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("vault envelope id cannot be empty"));
    }
    if trimmed.len() > MAX_ENVELOPE_ID_LEN {
        return Err(anyhow!(
            "vault envelope id exceeds maximum length of {} bytes",
            MAX_ENVELOPE_ID_LEN
        ));
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

fn load_or_create_master_key(root: &Path, key_material: &VaultKeyMaterial) -> Result<[u8; 32]> {
    match key_material {
        VaultKeyMaterial::Plaintext => load_or_create_plaintext_master_key(root),
        VaultKeyMaterial::Passphrase(pw) => load_or_create_keyguard_master_key(root, pw),
    }
}

/// Passphrase keyguard: the master key lives only inside `master.keyguard`,
/// wrapped under an Argon2id KEK. No plaintext `master.key` is ever written in
/// this mode, and one already present is refused (the operator must migrate it
/// deliberately rather than silently keep a plaintext copy alongside).
fn load_or_create_keyguard_master_key(root: &Path, passphrase: &str) -> Result<[u8; 32]> {
    let guard_path = root.join("master.keyguard");
    // Bind the container to its canonical vault directory so a keyguard file
    // casually copied to a different path won't open with the same passphrase.
    // Canonicalize so the same directory reached by a different spelling
    // (relative vs absolute, trailing slash, symlink) still opens — the vault
    // dir already exists here (create_dir_all ran in the store constructor).
    // This is a deterrent against a careless copy, NOT a cryptographic
    // anti-exfiltration control (an attacker who knows the path and passphrase
    // can still open a copy placed there).
    let path_aad = fs::canonicalize(root)
        .unwrap_or_else(|_| root.to_path_buf())
        .as_os_str()
        .as_encoded_bytes()
        .to_vec();
    if guard_path.exists() {
        #[cfg(unix)]
        {
            let mode = fs::metadata(&guard_path)?.permissions().mode() & 0o777;
            if mode != 0o600 {
                fs::set_permissions(&guard_path, fs::Permissions::from_mode(0o600))?;
            }
        }
        let bytes = read_file(&guard_path)?;
        crate::vault::crypto::keyguard_decode(&bytes, passphrase, &path_aad)
    } else {
        if root.join("master.key").exists() {
            return Err(anyhow!(
                "vault: a plaintext master.key exists but a passphrase was supplied. \
                 Migrate it deliberately (re-seal under the keyguard) before enabling \
                 passphrase mode, rather than keeping a plaintext key alongside."
            ));
        }
        let mut key = [0u8; 32];
        SysRng
            .try_fill_bytes(&mut key[..])
            .expect("OS RNG unavailable");
        let container = crate::vault::crypto::keyguard_encode(&key, passphrase, &path_aad)?;
        // Atomic write (temp + fsync + rename, 0600): a torn write must never
        // leave a partial master.keyguard that then fails to decode and bricks
        // vault init with no recovery path.
        write_atomic(&guard_path, &container)?;
        Ok(key)
    }
}

fn load_or_create_plaintext_master_key(root: &Path) -> Result<[u8; 32]> {
    // Fail closed if this is a keyguard-protected vault opened WITHOUT a
    // passphrase: otherwise we'd mint a second, plaintext master.key and seal
    // new evidence under it, splitting the vault across two keys (restoring the
    // passphrase later silently reselects the first key). The operator must set
    // SECURACV_VAULT_PASSPHRASE to open a keyguard vault.
    if root.join("master.keyguard").exists() {
        return Err(anyhow!(
            "vault: master.keyguard is present but SECURACV_VAULT_PASSPHRASE is not set. \
             Set the vault passphrase to open this keyguard-protected vault — refusing to \
             create a second plaintext master.key that would split the vault."
        ));
    }
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
        SysRng
            .try_fill_bytes(&mut key[..])
            .expect("OS RNG unavailable");
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

fn load_or_create_kem_keypair(
    root: &Path,
    crypto_mode: VaultCryptoMode,
) -> Result<Option<KemKeypair>> {
    let key_path = root.join("kem-mlkem768.key");
    if key_path.exists() {
        #[cfg(not(feature = "pqc-vault"))]
        {
            return Err(anyhow!(
                "vault KEM keypair found but pqc-vault feature is disabled"
            ));
        }
        #[cfg(feature = "pqc-vault")]
        {
            let bytes = read_file(&key_path)?;
            let mut cursor = 0usize;
            let public_len = read_u32(&bytes, &mut cursor)? as usize;
            let public_bytes = read_slice(&bytes, &mut cursor, public_len)?;
            let secret_len = read_u32(&bytes, &mut cursor)? as usize;
            let secret_bytes = read_slice(&bytes, &mut cursor, secret_len)?;
            let keypair = KemKeypair::from_bytes(public_bytes, secret_bytes)?;
            return Ok(Some(keypair));
        }
    }

    if matches!(crypto_mode, VaultCryptoMode::Pq | VaultCryptoMode::Hybrid) {
        #[cfg(not(feature = "pqc-vault"))]
        {
            return Err(anyhow!(
                "vault crypto mode {} requires pqc-vault feature",
                crypto_mode
            ));
        }
        #[cfg(feature = "pqc-vault")]
        {
            let keypair = KemKeypair::generate();
            let mut encoded = Vec::new();
            let public = keypair.public_bytes();
            let secret = keypair.secret_bytes();
            encoded.extend_from_slice(&(public.len() as u32).to_le_bytes());
            encoded.extend_from_slice(&public);
            encoded.extend_from_slice(&(secret.len() as u32).to_le_bytes());
            encoded.extend_from_slice(&secret);

            let mut options = OpenOptions::new();
            options.write(true).create_new(true);
            #[cfg(unix)]
            {
                options.mode(0o600);
            }
            let mut file = options.open(&key_path)?;
            file.write_all(&encoded)?;
            file.sync_all()?;
            return Ok(Some(keypair));
        }
    }

    Ok(None)
}

#[cfg(feature = "pqc-vault")]
fn read_u32(bytes: &[u8], cursor: &mut usize) -> Result<u32> {
    let slice = read_slice(bytes, cursor, 4)?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

#[cfg(feature = "pqc-vault")]
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
        #[cfg(unix)]
        let mut file = {
            let mut options = OpenOptions::new();
            options.write(true).create(true).truncate(true).mode(0o600);
            options.open(&tmp_path)?
        };
        #[cfg(not(unix))]
        let mut file = File::create(&tmp_path)?;

        file.write_all(data)?;
        file.sync_all()?;
    }
    fs::rename(&tmp_path, path)?;
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
    use crate::vault::crypto::VaultCryptoMode;
    use crate::TimeBucket;
    use ed25519_dalek::{SigningKey, VerifyingKey};
    use sha2::{Digest, Sha256};
    use std::fs;

    fn passphrase(s: &str) -> VaultKeyMaterial {
        VaultKeyMaterial::Passphrase(Zeroizing::new(s.to_string()))
    }

    #[test]
    fn keyguard_creates_container_not_plaintext_and_reopens() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        let km = passphrase("open sesame please");
        let k1 = load_or_create_master_key(root, &km).unwrap();
        assert!(root.join("master.keyguard").exists());
        assert!(
            !root.join("master.key").exists(),
            "keyguard mode must never write a plaintext master.key"
        );
        let k2 = load_or_create_master_key(root, &km).unwrap();
        assert_eq!(k1, k2, "same passphrase recovers the same master key");
    }

    #[test]
    fn keyguard_wrong_passphrase_fails_to_open() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        load_or_create_master_key(root, &passphrase("the-right-one")).unwrap();
        assert!(load_or_create_master_key(root, &passphrase("the-wrong-one")).is_err());
    }

    #[test]
    fn keyguard_refuses_alongside_plaintext_key() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        load_or_create_master_key(root, &VaultKeyMaterial::Plaintext).unwrap();
        assert!(root.join("master.key").exists());
        assert!(
            load_or_create_master_key(root, &passphrase("pw")).is_err(),
            "must refuse passphrase mode while a plaintext master.key is present"
        );
    }

    #[test]
    fn plaintext_refuses_when_keyguard_present() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        load_or_create_master_key(root, &passphrase("the passphrase")).unwrap();
        assert!(root.join("master.keyguard").exists());
        // Opening without the passphrase must refuse — never mint a second key.
        assert!(load_or_create_master_key(root, &VaultKeyMaterial::Plaintext).is_err());
        assert!(
            !root.join("master.key").exists(),
            "must not create a second plaintext key alongside a keyguard"
        );
    }

    #[test]
    fn plaintext_master_key_mode_unchanged() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        let k1 = load_or_create_master_key(root, &VaultKeyMaterial::Plaintext).unwrap();
        assert!(root.join("master.key").exists());
        assert!(!root.join("master.keyguard").exists());
        let k2 = load_or_create_master_key(root, &VaultKeyMaterial::Plaintext).unwrap();
        assert_eq!(k1, k2);
    }

    fn make_break_glass_token(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
    ) -> (BreakGlassToken, VerifyingKey, [u8; 32]) {
        let bucket = crate::TimeBucket::now(600).expect("time bucket");
        let request = UnlockRequest::new(envelope_id, ruleset_hash, "test-export", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let approval = Approval::signed(
            TrusteeId::new("alice"),
            request.request_hash(),
            &signing_key,
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
            ..VaultConfig::default()
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
        let envelope = seal_v2(
            "incident-7",
            [7u8; 32],
            b"raw bytes",
            VaultCryptoMode::Classical,
            &master_key,
            None,
        )?;
        let wrong_key = [10u8; 32];
        assert!(decrypt_v2(&envelope, &wrong_key, None).is_err());

        let mut tampered = envelope.clone();
        if let Some(last) = tampered.ciphertext.last_mut() {
            *last ^= 0b1010_1010;
        }
        assert!(decrypt_v2(&tampered, &master_key, None).is_err());
        Ok(())
    }

    #[test]
    fn envelope_ciphertext_is_not_plaintext() -> Result<()> {
        let master_key = [11u8; 32];
        let clear = b"raw bytes";
        let envelope = seal_v2(
            "incident-8",
            [8u8; 32],
            clear,
            VaultCryptoMode::Classical,
            &master_key,
            None,
        )?;
        assert_ne!(envelope.ciphertext, clear);
        Ok(())
    }

    // ==================== Security Edge Case Tests ====================

    #[test]
    fn envelope_id_rejects_oversized_input() {
        // Create an envelope ID that exceeds the maximum length
        let oversized_id = "a".repeat(MAX_ENVELOPE_ID_LEN + 1);
        let result = sanitize_envelope_id(&oversized_id);
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("exceeds maximum length"));
    }

    #[test]
    fn envelope_id_accepts_max_length() {
        // Create an envelope ID at exactly the maximum length
        let max_len_id = "a".repeat(MAX_ENVELOPE_ID_LEN);
        let result = sanitize_envelope_id(&max_len_id);
        assert!(result.is_ok());
    }

    #[test]
    fn envelope_id_rejects_empty() {
        let result = sanitize_envelope_id("");
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("cannot be empty"));
    }

    #[test]
    fn envelope_id_rejects_invalid_characters() {
        // Test uppercase rejection
        assert!(sanitize_envelope_id("UPPER").is_err());
        // Test whitespace rejection
        assert!(sanitize_envelope_id("has space").is_err());
        // Test special character rejection
        assert!(sanitize_envelope_id("has/slash").is_err());
        // Test valid characters are accepted
        assert!(sanitize_envelope_id("valid-envelope_id-123").is_ok());
    }

    #[test]
    fn decrypt_v2_classical_fallback_on_bad_kem_ct() -> Result<()> {
        use crate::vault::crypto::{decrypt_v2, seal_v2, KemKeypair};
        let master_key = [12u8; 32];
        let clear = b"secret payload";

        let mut envelope = seal_v2(
            "incident-fallback",
            [12u8; 32],
            clear,
            VaultCryptoMode::Classical,
            &master_key,
            None,
        )?;

        // Simulate a legacy hybrid envelope: set kem_alg to ML-KEM-768 with
        // garbage KEM ciphertext. With a real keypair, decrypt_v2 will attempt
        // KEM decapsulation (implicit rejection returns wrong shared secret),
        // derive a wrong DEK, fail AEAD tag verification, then fall back to
        // classical_wrap.
        envelope.kem_alg = "ml-kem-768".to_string();
        envelope.kem_ct = vec![0xBA; 1088];
        envelope.kdf_info = vec![0x01; 16];

        let kp = KemKeypair::generate();
        let result = decrypt_v2(&envelope, &master_key, Some(&kp))?;
        assert_eq!(result, clear);
        Ok(())
    }
}
