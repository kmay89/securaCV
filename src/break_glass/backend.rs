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

    /// Store (or replace) the quorum policy. Convenience for provisioning.
    pub fn set_policy(&mut self, policy: &QuorumPolicy) -> Result<()> {
        self.kernel.set_break_glass_policy(policy)
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

        // Unseal through the same kernel: device key + receipt lookup both come
        // from it, so no second connection is opened.
        let vault = Vault::new(VaultConfig {
            local_path: self.vault_path.clone().into(),
            crypto_mode: policy.vault.crypto_mode,
        })?;
        let verifying_key = self.kernel.device_verifying_key();
        let mut clear = vault.unseal(
            &request.vault_envelope_id,
            &mut token,
            self.ruleset_hash,
            &verifying_key,
            |hash| self.kernel.break_glass_receipt_outcome(hash),
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

/// Write `bytes` to `path`, truncating, with `0600` permissions on Unix.
fn write_restricted(path: &Path, bytes: &[u8]) -> Result<()> {
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
        let mut file = std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .mode(0o600)
            .open(path)?;
        // `mode()` only takes effect when the file is newly created; if it already
        // existed with laxer permissions, tighten it explicitly so the cleartext
        // is never left world-readable.
        let mut perms = file.metadata()?.permissions();
        if perms.mode() & 0o777 != 0o600 {
            perms.set_mode(0o600);
            file.set_permissions(perms)?;
        }
        file.write_all(bytes)?;
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
    use ed25519_dalek::{Signer, SigningKey};
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
        let rh = request.request_hash();
        Approval::new(TrusteeId::new(id), rh, key.sign(&rh).to_vec())
    }

    fn count_receipts(db_path: &str) -> i64 {
        let conn = rusqlite::Connection::open(db_path).unwrap();
        conn.query_row("SELECT COUNT(*) FROM break_glass_receipts", [], |r| {
            r.get(0)
        })
        .unwrap()
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
    fn policy_roundtrip_and_denial_logs_receipt() -> Result<()> {
        let dir = std::env::temp_dir().join(format!("secura_bg_backend_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        let cfg = test_config(&dir);
        let db_path = cfg.db_path.clone();
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
        let request = UnlockRequest::new("vault:x", cfg.ruleset_hash, "audit", bucket)?;
        let out = dir.join("out");
        let err = ops
            .authorize_unseal(
                &request,
                &[approval_for(&alice, "alice", &request)],
                bucket,
                &out.to_string_lossy(),
            )
            .unwrap_err();
        assert!(err.to_string().contains("insufficient"), "got: {err}");
        assert_eq!(count_receipts(&db_path), 1, "denial should log a receipt");
        // Nothing was written on denial.
        assert!(!out.join("vault_x.raw").exists());
        Ok(())
    }

    #[test]
    fn granted_seal_then_unseal_roundtrip() -> Result<()> {
        let dir =
            std::env::temp_dir().join(format!("secura_bg_backend_grant_{}", rand::random::<u64>()));
        std::fs::create_dir_all(&dir)?;
        let cfg = test_config(&dir);
        let vault_path = dir.join("vault").to_string_lossy().to_string();
        let envelope = "incident:42";
        let secret = b"raw-evidence-bytes".to_vec();

        let alice = SigningKey::from_bytes(&[5u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
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
            })?;
            let vk = kernel.device_verifying_key();
            let mut raw = secret.clone();
            vault.seal(envelope, &mut token, cfg.ruleset_hash, &mut raw, &vk, |h| {
                kernel.break_glass_receipt_outcome(h)
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

        let recovered = std::fs::read(&path)?;
        assert_eq!(
            recovered, secret,
            "unsealed bytes must match what was sealed"
        );
        // Two receipts now: one for the seal authorization, one for the unseal.
        assert_eq!(count_receipts(&cfg.db_path), 2);
        Ok(())
    }
}
