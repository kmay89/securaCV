//! Real [`BreakGlassOps`] backend over a live [`Kernel`] + [`Vault`].
//!
//! This wires the transport-agnostic break-glass HTTP handler (see
//! [`super::http`]) to the kernel and vault, reusing exactly the authorize →
//! log-receipt → sign-token → unseal sequence the `break_glass authorize` /
//! `break_glass unseal` CLI performs. A granted unseal writes the recovered
//! envelope to a **server-configured** directory at mode `0600` and returns only
//! the path — no cleartext crosses the trait boundary, matching the CLI.
//!
//! The whole sequence runs through a single `Kernel`: the receipt lookup the
//! vault needs for token validation is served by [`Kernel::break_glass_receipt_outcome`]
//! and the device verifying key by [`Kernel::device_verifying_key`], so there is
//! no second database connection.

use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};
use zeroize::Zeroize;

use super::http::BreakGlassOps;
use crate::break_glass::{Approval, BreakGlass, QuorumPolicy, UnlockRequest};
use crate::{Kernel, KernelConfig, TimeBucket, Vault, VaultConfig};

/// Owns an open kernel and the vault location, and implements [`BreakGlassOps`].
pub struct KernelVaultOps {
    kernel: Kernel,
    ruleset_hash: [u8; 32],
    vault_path: String,
}

impl KernelVaultOps {
    /// Open the kernel described by `kernel_cfg` and bind it to the vault at
    /// `vault_path`.
    pub fn open(kernel_cfg: KernelConfig, vault_path: impl Into<String>) -> Result<Self> {
        let ruleset_hash = kernel_cfg.ruleset_hash;
        let kernel = Kernel::open(&kernel_cfg)?;
        Ok(Self {
            kernel,
            ruleset_hash,
            vault_path: vault_path.into(),
        })
    }

    /// Store the quorum policy through the quorum-gated path: a fresh
    /// database bootstraps freely; replacing a live policy requires
    /// current-quorum approvals, which this provisioning convenience does not
    /// carry — use the CLI `policy propose`/`approve`/`set --approvals` flow
    /// for changes (Invariant V).
    pub fn set_policy(&mut self, policy: &QuorumPolicy) -> Result<()> {
        self.kernel
            .set_break_glass_policy_gated(policy, &[], crate::TimeBucket::now_10min()?)
            .map(|_| ())
    }
}

impl BreakGlassOps for KernelVaultOps {
    fn policy(&self) -> Result<Option<QuorumPolicy>> {
        Ok(self.kernel.break_glass_policy().cloned())
    }

    fn ruleset_hash(&self) -> [u8; 32] {
        self.ruleset_hash
    }

    fn authorize_unseal(
        &mut self,
        request: &UnlockRequest,
        approvals: &[Approval],
        now_bucket: TimeBucket,
        output_dir: &str,
    ) -> Result<PathBuf> {
        // Clone the policy so the kernel is free to be borrowed mutably below.
        let policy = self
            .kernel
            .break_glass_policy()
            .cloned()
            .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?;

        // Authorize, then log a tamper-evident receipt regardless of outcome —
        // exactly as `break_glass authorize` does.
        let (result, receipt) = BreakGlass::authorize(&policy, request, approvals, now_bucket);
        let receipt_entry_hash = self.kernel.log_break_glass_receipt(&receipt, approvals)?;

        // A denial leaves the receipt in the chain and surfaces the reason.
        let mut token = result?;
        self.kernel
            .sign_break_glass_token(&mut token, receipt_entry_hash)?;

        // Burn-first (durable anti-replay): record the nonce in the
        // receipts DB before any cleartext exists, so this token — even if
        // its file form leaks — can never authorize a second unseal.
        self.kernel.consume_token_durably(&token)?;

        // Unseal through the same kernel: device key + receipt lookup both come
        // from it, so no second connection is opened.
        let vault = Vault::new(VaultConfig {
            local_path: self.vault_path.clone().into(),
            crypto_mode: policy.vault.crypto_mode,
            key_material: crate::VaultKeyMaterial::from_env(),
        })?;
        let verifying_key = self.kernel.device_verifying_key();
        let mut clear = vault.unseal(
            &request.vault_envelope_id,
            &mut token,
            self.ruleset_hash,
            &verifying_key,
            |hash| {
                self.kernel.break_glass_receipt_outcome(
                    &request.vault_envelope_id,
                    self.ruleset_hash,
                    hash,
                )
            },
        )?;

        // Write the recovered envelope to the server-configured directory (0600).
        let sanitized = crate::vault::sanitize_envelope_id(&request.vault_envelope_id)?;
        let dir = Path::new(output_dir);
        std::fs::create_dir_all(dir)?;
        let path = dir.join(format!("{}.raw", sanitized));
        let write_result = write_restricted(&path, &clear);
        // Wipe the recovered cleartext from RAM regardless of whether the write
        // succeeded — it must not linger in a heap buffer for a core dump to leak.
        clear.zeroize();
        write_result?;
        Ok(path)
    }
}

/// Write `bytes` to `path` at mode `0600`. On Unix the cleartext is written to
/// a fresh temp file created `O_EXCL` (`create_new`) at `0600` and then
/// atomically renamed over the target. `OpenOptions::mode()` only applies to
/// newly created files, so overwriting a pre-existing `<envelope>.raw` in place
/// could leave it at a laxer mode (e.g. `0644`) or, worse, follow a planted
/// symlink and write the evidence through it. The temp+rename approach gives a
/// fresh `0600` regular file every time and never writes cleartext through an
/// existing path. `pub(crate)` because the rotating bearer-token files
/// (api/mod.rs, break_glass/server.rs) are written under the same rule.
pub(crate) fn write_restricted(path: &Path, bytes: &[u8]) -> Result<()> {
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let dir = path.parent().unwrap_or_else(|| Path::new("."));
        let file_name = path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("output");
        let tmp = dir.join(format!(".{file_name}.{:016x}.tmp", rand::random::<u64>()));
        // `create_new` (O_EXCL) refuses to follow or clobber an existing path,
        // so cleartext can never be written through a planted file or symlink.
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&tmp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        // Atomic replace: the target becomes a fresh 0600 regular file even if it
        // previously existed (or was a symlink).
        if let Err(e) = std::fs::rename(&tmp, path) {
            let _ = std::fs::remove_file(&tmp);
            return Err(e.into());
        }
    }
    #[cfg(not(unix))]
    {
        std::fs::write(path, bytes)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{TrusteeEntry, TrusteeId};
    use crate::ZonePolicy;
    use ed25519_dalek::SigningKey;
    use std::time::Duration;

    fn test_config(dir: &Path) -> KernelConfig {
        let ruleset_id = "ruleset:test";
        KernelConfig {
            db_path: dir.join("witness.db").to_string_lossy().to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id(ruleset_id),
            kernel_version: "test".to_string(),
            retention: Duration::from_secs(3600),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        }
    }

    fn approval_for(key: &SigningKey, id: &str, request: &UnlockRequest) -> Approval {
        Approval::signed(TrusteeId::new(id), request.request_hash(), key)
    }

    /// Sleep until safely past a 10-minute bucket boundary if we're within the
    /// last few seconds of the current bucket, so a short token-scoped flow does
    /// not straddle two buckets.
    fn wait_for_bucket_headroom() {
        use std::time::{SystemTime, UNIX_EPOCH};
        let secs = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();
        let into_bucket = secs % 600;
        if into_bucket > 594 {
            std::thread::sleep(Duration::from_secs(600 - into_bucket + 1));
        }
    }

    #[test]
    #[cfg(unix)]
    fn write_restricted_tightens_preexisting_lax_file() -> Result<()> {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("secura_bg_perms_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        let path = dir.join("evidence.raw");
        // Pre-create the target world-readable, as a stale unseal could have.
        std::fs::write(&path, b"old")?;
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644))?;

        write_restricted(&path, b"new-cleartext")?;

        let mode = std::fs::metadata(&path)?.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "cleartext file must be restricted to 0600");
        assert_eq!(std::fs::read(&path)?, b"new-cleartext");
        Ok(())
    }

    #[test]
    #[cfg(unix)]
    fn write_restricted_does_not_follow_symlink() -> Result<()> {
        use std::os::unix::fs::{symlink, PermissionsExt};
        let dir = std::env::temp_dir().join(format!("secura_bg_symlink_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        // An attacker plants <envelope>.raw as a symlink to a file elsewhere.
        let outside = dir.join("attacker-target");
        std::fs::write(&outside, b"original")?;
        let path = dir.join("evidence.raw");
        symlink(&outside, &path)?;

        write_restricted(&path, b"secret-cleartext")?;

        // The symlink target must be untouched, and the path is now a fresh
        // 0600 regular file holding the cleartext (not written through the link).
        assert_eq!(std::fs::read(&outside)?, b"original");
        assert!(!std::fs::symlink_metadata(&path)?.file_type().is_symlink());
        assert_eq!(std::fs::read(&path)?, b"secret-cleartext");
        assert_eq!(
            std::fs::metadata(&path)?.permissions().mode() & 0o777,
            0o600
        );
        Ok(())
    }

    #[test]
    fn policy_roundtrip_and_denial_logs_receipt() -> Result<()> {
        let dir = std::env::temp_dir().join(format!("secura_bg_backend_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        let cfg = test_config(&dir);
        let mut ops = KernelVaultOps::open(cfg.clone(), dir.join("vault").to_string_lossy())?;

        // No policy yet.
        assert!(ops.policy()?.is_none());

        // 2-of-2 policy.
        let alice = SigningKey::from_bytes(&[1u8; 32]);
        let bob = SigningKey::from_bytes(&[2u8; 32]);
        let policy = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: alice.verifying_key().to_bytes(),
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: bob.verifying_key().to_bytes(),
                },
            ],
        )?;
        ops.set_policy(&policy)?;
        assert_eq!(ops.policy()?.unwrap().n, 2);

        // Only one valid approval — denied, but the receipt is still logged.
        let bucket = TimeBucket::now_10min()?;
        let request = UnlockRequest::new("vault-x", cfg.ruleset_hash, "audit", bucket)?;
        let out = dir.join("out");
        let err = ops
            .authorize_unseal(
                &request,
                &[approval_for(&alice, "alice", &request)],
                bucket,
                &out.to_string_lossy(),
            )
            .unwrap_err();
        // Returning the quorum-denial reason proves the path ran end to end:
        // the receipt is logged *before* the denial is surfaced, so a logging
        // failure would show up as a different error here.
        assert!(err.to_string().contains("insufficient"), "got: {err}");
        // Nothing was written on denial.
        assert!(!out.join("vault-x.raw").exists());
        Ok(())
    }

    #[test]
    fn granted_seal_then_unseal_roundtrip() -> Result<()> {
        let dir =
            std::env::temp_dir().join(format!("secura_bg_backend_grant_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        let cfg = test_config(&dir);
        let vault_path = dir.join("vault").to_string_lossy().to_string();
        let envelope = "incident-42";
        let secret = b"raw-evidence-bytes".to_vec();

        let alice = SigningKey::from_bytes(&[5u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
        // Break-glass tokens are scoped to a 10-minute bucket, and the vault
        // re-derives "now" when it validates them. If the wall clock rolls over a
        // bucket boundary between authorizing and sealing/unsealing, the token
        // looks expired. Wait out the boundary if we're near it so the whole
        // seal→unseal flow runs inside a single bucket.
        wait_for_bucket_headroom();
        let bucket = TimeBucket::now_10min()?;

        // --- Provision policy + seal an envelope using a real granted token. ---
        // Scoped so this kernel is dropped before the ops kernel opens.
        {
            let mut kernel = Kernel::open(&cfg)?;
            kernel.set_break_glass_policy(&policy)?;

            let seal_req = UnlockRequest::new(envelope, cfg.ruleset_hash, "seal", bucket)?;
            let (res, receipt) = BreakGlass::authorize(
                &policy,
                &seal_req,
                &[approval_for(&alice, "alice", &seal_req)],
                bucket,
            );
            let mut token = res?;
            let reh = kernel
                .log_break_glass_receipt(&receipt, &[approval_for(&alice, "alice", &seal_req)])?;
            kernel.sign_break_glass_token(&mut token, reh)?;

            let vault = Vault::new(VaultConfig {
                local_path: vault_path.clone().into(),
                crypto_mode: policy.vault.crypto_mode,
                key_material: crate::VaultKeyMaterial::default(),
            })?;
            let vk = kernel.device_verifying_key();
            let mut raw = secret.clone();
            vault.seal(envelope, &mut token, cfg.ruleset_hash, &mut raw, &vk, |h| {
                kernel.break_glass_receipt_outcome(envelope, cfg.ruleset_hash, h)
            })?;
        }

        // --- Unseal through the backend with a fresh granted authorization. ---
        let mut ops = KernelVaultOps::open(cfg.clone(), vault_path)?;
        let unseal_req = UnlockRequest::new(envelope, cfg.ruleset_hash, "unseal", bucket)?;
        let out = dir.join("out");
        let path = ops.authorize_unseal(
            &unseal_req,
            &[approval_for(&alice, "alice", &unseal_req)],
            bucket,
            &out.to_string_lossy(),
        )?;

        // A correct recovery exercises the whole chain — authorize, receipt
        // logging, token signing, and the vault's receipt-verified unseal.
        let recovered = std::fs::read(&path)?;
        assert_eq!(
            recovered, secret,
            "unsealed bytes must match what was sealed"
        );
        Ok(())
    }
}
