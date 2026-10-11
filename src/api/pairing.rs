//! Pairing — how a client that cannot read the capability-token file (the
//! Home Assistant integration talking to the Docker sidecar across the LAN)
//! earns a credential of its own.
//!
//! Two artifacts, both kept beside the capability token (0600, the one
//! directory the operator already treats as secret):
//!
//! * the **pairing-code file** ([`PairingCodeFile`]): the sha256 of a
//!   6-digit code `witness_api issue-pairing-code` printed once, with its
//!   expiry and a per-CODE attempt budget. Per-IP lockouts alone do not
//!   bound a 20-bit secret (an IPv6 host sources from a /64), so the budget
//!   is the control: [`PAIRING_CODE_ATTEMPTS`] wrong guesses burn the code
//!   and the operator must issue another — P ≤ 5 × 10⁻⁶ per issued code
//!   whatever the attacker's address supply. Single-use, consumed durably
//!   before the bearer is handed out; an expired file is deleted on first
//!   touch; the clear code is never written anywhere, never logged.
//! * the **paired-token file**: the same `{v, tokens:[{id, sha256, label,
//!   minted}]}` set as the viewer credential ([`super::ViewerTokenSet`]),
//!   sha256 at rest, constant-time compare, read per request so a
//!   revocation lands on the next poll. A paired bearer opens every route
//!   the capability token opens and, unlike that token, never rotates —
//!   which is exactly why it is revocable by id from three places
//!   (`revoke-paired-token`, `DELETE /api/pair/<id>` with the capability
//!   token, or a paired bearer revoking ONLY itself).
//!
//! The HTTP side (`POST /api/pair`) answers one constant 401 for a wrong
//! code, no code, a burned code and an expired code alike — no oracle on
//! the pairing state for an unauthenticated caller — and each of those is
//! a per-IP strike besides.

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use std::io::Read;
use std::path::Path;
use subtle::ConstantTimeEq;

/// Schema version of the pairing-code file.
pub const PAIRING_CODE_FILE_VERSION: u32 = 1;
/// File name of the pairing code when only `token_path` is configured.
pub const PAIRING_CODE_FILE_NAME: &str = "pairing_code";
/// File name of the paired-bearer set when only `token_path` is configured.
pub const PAIRED_TOKEN_FILE_NAME: &str = "paired_tokens.json";
/// A pairing code is exactly this many ASCII digits.
pub const PAIRING_CODE_LEN: usize = 6;
/// Wrong guesses a single issued code survives before it is burned.
pub const PAIRING_CODE_ATTEMPTS: u32 = 5;
/// How long an issued code is redeemable, unless the issuer said otherwise.
pub const DEFAULT_PAIRING_TTL_SECS: u64 = 600;
/// The pairing-code file is a handful of short fields; anything larger is
/// not ours and is not parsed.
pub const PAIRING_CODE_MAX_FILE_BYTES: u64 = 4096;
/// `POST /api/pair` reads at most this many body bytes.
pub const PAIR_REQUEST_MAX_BYTES: usize = 1024;
/// The label a paired bearer gets when the request names none.
pub const DEFAULT_PAIRED_LABEL: &str = "home-assistant";

/// The pairing-code file (`pairing_code`, 0600):
/// `{"v":1,"sha256":"…","issued":…,"expires":…,"label":"…","attempts_left":5}`.
/// Holds the code's sha256 only — the clear code exists on the issuing
/// command's stdout and nowhere else.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PairingCodeFile {
    pub v: u32,
    /// Hex sha256 over the code's ASCII digits.
    pub sha256: String,
    /// Epoch seconds the code was issued.
    pub issued: u64,
    /// Epoch seconds after which the code is dead (deleted on first touch).
    pub expires: u64,
    /// The issuer's note ("docker exec securacv pair"), bounded like a
    /// viewer label.
    pub label: String,
    /// Wrong guesses left before the code is burned.
    pub attempts_left: u32,
}

/// What `issue-pairing-code` hands the operator: the clear code, once.
#[derive(Clone, Debug)]
pub struct IssuedPairingCode {
    /// The 6 ASCII digits.
    pub code: String,
    pub expires: u64,
    pub label: String,
}

/// The outcome of presenting a code. Every refusal is the SAME variant on
/// purpose: the caller cannot tell a wrong guess from a burned, expired or
/// never-issued code, and must not try.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Redeem {
    /// The code matched; the file is already gone. `label` is the issuer's
    /// note, for the log line.
    Accepted {
        label: String,
    },
    Refused,
}

impl PairingCodeFile {
    /// Mint a fresh 6-digit code, write its hash (overwriting any
    /// outstanding code — there is only ever one) and return the clear
    /// code. `ttl_secs` of 0 is refused: a code that is dead on arrival is
    /// a misconfiguration, not a pairing.
    pub fn issue(path: &Path, ttl_secs: u64, label: &str, now: u64) -> Result<IssuedPairingCode> {
        if ttl_secs == 0 {
            return Err(anyhow!("a pairing code needs a positive ttl"));
        }
        let label = super::clean_viewer_label(label);
        let code = random_code();
        let file = Self {
            v: PAIRING_CODE_FILE_VERSION,
            sha256: hex::encode(code_digest(&code)),
            issued: now,
            expires: now.saturating_add(ttl_secs),
            label: label.clone(),
            attempts_left: PAIRING_CODE_ATTEMPTS,
        };
        file.save(path)?;
        Ok(IssuedPairingCode {
            code,
            expires: file.expires,
            label,
        })
    }

    /// Read the file at `path`; `None` when there is none. A file past the
    /// size bound, malformed, or of another schema version is an error —
    /// the HTTP caller folds that into the constant refusal.
    pub fn load(path: &Path) -> Result<Option<Self>> {
        let file = match std::fs::File::open(path) {
            Ok(file) => file,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(err) => {
                return Err(anyhow!(
                    "failed to open pairing code file {}: {err}",
                    path.display()
                ))
            }
        };
        if file.metadata()?.len() > PAIRING_CODE_MAX_FILE_BYTES {
            return Err(anyhow!(
                "pairing code file {} is past the {PAIRING_CODE_MAX_FILE_BYTES}-byte bound",
                path.display()
            ));
        }
        let mut bytes = Vec::new();
        file.take(PAIRING_CODE_MAX_FILE_BYTES + 1)
            .read_to_end(&mut bytes)?;
        let parsed: Self = serde_json::from_slice(&bytes)
            .map_err(|err| anyhow!("pairing code file {} is malformed: {err}", path.display()))?;
        if parsed.v != PAIRING_CODE_FILE_VERSION {
            return Err(anyhow!(
                "pairing code file {} is schema v{}, this kernel reads v{PAIRING_CODE_FILE_VERSION}",
                path.display(),
                parsed.v
            ));
        }
        Ok(Some(parsed))
    }

    /// Write as a fresh 0600 regular file (atomic replace, never through a
    /// symlink — `write_restricted`'s contract).
    pub fn save(&self, path: &Path) -> Result<()> {
        let bytes = serde_json::to_vec(self)?;
        crate::break_glass::backend::write_restricted(path, &[bytes.as_slice(), b"\n"].concat())
    }

    pub fn is_expired(&self, now: u64) -> bool {
        self.expires <= now
    }

    /// The code outstanding at `path`, if one is live: an expired file is
    /// deleted on this first touch and reads as none. What `doctor` and
    /// `/api/identity` consult — neither learns the code, only that one
    /// exists and until when.
    pub fn outstanding(path: &Path, now: u64) -> Result<Option<Self>> {
        match Self::load(path)? {
            Some(file) if file.is_expired(now) => {
                remove_quietly(path);
                Ok(None)
            }
            other => Ok(other),
        }
    }

    /// Present `code` (already checked to be [`PAIRING_CODE_LEN`] ASCII
    /// digits) against the file at `path`. On a match the file is deleted
    /// BEFORE `Accepted` is returned — the code is single-use even if the
    /// caller crashes between this return and handing out the bearer. On a
    /// mismatch `attempts_left` is decremented and written before the
    /// refusal is returned (burned at zero: deleted). Missing, unreadable,
    /// malformed and expired files all refuse the same way.
    pub fn redeem(path: &Path, code: &str, now: u64) -> Result<Redeem> {
        let file = match Self::load(path) {
            Ok(Some(file)) => file,
            Ok(None) => return Ok(Redeem::Refused),
            Err(err) => {
                log::warn!("pairing code file ignored: {err:#}");
                return Ok(Redeem::Refused);
            }
        };
        if file.is_expired(now) {
            remove_quietly(path);
            return Ok(Redeem::Refused);
        }
        let stored = match super::parse_hex32(&file.sha256) {
            Ok(stored) => stored,
            Err(_) => {
                log::warn!(
                    "pairing code file {} holds an unreadable hash; discarding it",
                    path.display()
                );
                remove_quietly(path);
                return Ok(Redeem::Refused);
            }
        };
        // SECURITY: constant-time compare of the two digests, so response
        // timing says nothing about how many leading bytes agreed.
        if code_digest(code).ct_eq(&stored).unwrap_u8() == 1 {
            // Consumed durably first: a code that could be redeemed twice
            // would mint two bearers for one ceremony.
            std::fs::remove_file(path).map_err(|err| {
                anyhow!(
                    "failed to consume pairing code file {}: {err}",
                    path.display()
                )
            })?;
            return Ok(Redeem::Accepted { label: file.label });
        }
        let attempts_left = file.attempts_left.saturating_sub(1);
        if attempts_left == 0 {
            // Burned: the budget is the brute-force control, so the file
            // goes before the refusal is written.
            remove_quietly(path);
            log::warn!(
                "pairing code burned after {PAIRING_CODE_ATTEMPTS} wrong guesses; \
                 issue a new one with `witness_api issue-pairing-code`"
            );
        } else {
            let decremented = Self {
                attempts_left,
                ..file
            };
            // Written BEFORE the refusal goes out: a guess that the
            // kernel forgot to count is a guess that did not cost anything.
            decremented.save(path)?;
        }
        Ok(Redeem::Refused)
    }
}

/// `true` when `code` has the shape a pairing code can have: exactly
/// [`PAIRING_CODE_LEN`] ASCII digits. Anything else is malformed input
/// (a 400), not a guess.
pub fn is_well_formed_code(code: &str) -> bool {
    code.len() == PAIRING_CODE_LEN && code.bytes().all(|b| b.is_ascii_digit())
}

/// Six uniformly random decimal digits (leading zeros kept).
fn random_code() -> String {
    // 1 000 000 is not a power of two, so a raw `u32 % 1_000_000` would
    // bias the low codes by ~0.03 %; `random_range` rejects the tail.
    let n: u32 = rand::random_range(0..1_000_000);
    format!("{n:06}")
}

/// sha256 over the code's ASCII digits — the only form kept at rest.
fn code_digest(code: &str) -> [u8; 32] {
    <sha2::Sha256 as sha2::Digest>::digest(code.as_bytes()).into()
}

/// Delete the file, treating "already gone" as done. Used where the file's
/// absence is the point (expiry, burn) and a second remover losing the race
/// is not an error worth surfacing.
fn remove_quietly(path: &Path) {
    if let Err(err) = std::fs::remove_file(path) {
        if err.kind() != std::io::ErrorKind::NotFound {
            log::warn!(
                "could not remove pairing code file {}: {err}",
                path.display()
            );
        }
    }
}

/// The first 8 hex of `sha256(key)` over the 32 RAW key bytes — the short
/// form a person compares across two screens (the `pair` printout beside
/// the code, the Home Assistant pairing step). The same digest rule the
/// Canary trust store uses for its `fingerprint_hex`, cut to 8.
pub fn key_fingerprint(verifying_key_hex: &str) -> Result<String> {
    let raw = super::parse_hex32(verifying_key_hex)
        .map_err(|_| anyhow!("verifying key must be 64 hex characters"))?;
    let digest = <sha2::Sha256 as sha2::Digest>::digest(raw);
    Ok(hex::encode(&digest[..4]))
}

/// The body `POST /api/pair` accepts: `{"code":"483921","label":"home-assistant"}`.
/// Unknown keys are ignored (a newer integration may send more); a `label`
/// that is not a string, or a missing `code`, is a 400.
#[derive(Debug, Deserialize)]
pub struct PairRequest {
    pub code: String,
    #[serde(default)]
    pub label: Option<String>,
}

/// The one-time answer to a redeemed code. `token` is the bearer in the
/// clear — the only copy that will ever exist, the file keeps its sha256.
#[derive(Debug, Serialize)]
pub struct PairResponse {
    pub token: String,
    pub token_id: String,
    pub device_id: String,
    pub verifying_key: String,
    pub kernel_version: String,
}

/// Where the two pairing files live. Both or neither: a kernel that can
/// redeem a code must have somewhere to keep the bearer it mints, and the
/// config layer refuses a lone half.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PairingPaths {
    pub code_path: std::path::PathBuf,
    pub tokens_path: std::path::PathBuf,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file_at(dir: &tempfile::TempDir) -> std::path::PathBuf {
        dir.path().join(PAIRING_CODE_FILE_NAME)
    }

    #[test]
    fn issue_writes_the_hash_only_and_redeem_consumes_it() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let path = file_at(&dir);
        let issued = PairingCodeFile::issue(&path, 600, "docker exec securacv pair", 1_000)?;
        assert!(is_well_formed_code(&issued.code));
        assert_eq!(issued.expires, 1_600);
        let on_disk = std::fs::read_to_string(&path)?;
        assert!(
            !on_disk.contains(&issued.code),
            "the clear code must never be written"
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(&path)?.permissions().mode() & 0o777,
                0o600
            );
        }
        let stored = PairingCodeFile::load(&path)?.expect("file");
        assert_eq!(stored.attempts_left, PAIRING_CODE_ATTEMPTS);
        assert_eq!(stored.label, "docker exec securacv pair");

        assert_eq!(
            PairingCodeFile::redeem(&path, &issued.code, 1_100)?,
            Redeem::Accepted {
                label: "docker exec securacv pair".to_string()
            }
        );
        assert!(!path.exists(), "a redeemed code is consumed");
        // Single-use: the same code a second time is just a missing file.
        assert_eq!(
            PairingCodeFile::redeem(&path, &issued.code, 1_100)?,
            Redeem::Refused
        );
        Ok(())
    }

    #[test]
    fn wrong_guesses_are_counted_durably_and_the_fifth_burns_the_code() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let path = file_at(&dir);
        let issued = PairingCodeFile::issue(&path, 600, "x", 1_000)?;
        let wrong = if issued.code == "000000" {
            "000001"
        } else {
            "000000"
        };
        for expected_left in (0..PAIRING_CODE_ATTEMPTS).rev() {
            assert_eq!(
                PairingCodeFile::redeem(&path, wrong, 1_001)?,
                Redeem::Refused
            );
            if expected_left == 0 {
                assert!(!path.exists(), "the budget's last guess burns the file");
            } else {
                let on_disk = PairingCodeFile::load(&path)?.expect("still issued");
                assert_eq!(on_disk.attempts_left, expected_left);
            }
        }
        // The right code after the burn is refused like any other guess.
        assert_eq!(
            PairingCodeFile::redeem(&path, &issued.code, 1_001)?,
            Redeem::Refused
        );
        Ok(())
    }

    #[test]
    fn an_expired_code_is_deleted_on_first_touch_and_refused() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let path = file_at(&dir);
        let issued = PairingCodeFile::issue(&path, 60, "x", 1_000)?;
        assert!(PairingCodeFile::outstanding(&path, 1_059)?.is_some());
        assert_eq!(
            PairingCodeFile::redeem(&path, &issued.code, 1_060)?,
            Redeem::Refused
        );
        assert!(!path.exists(), "an expired file is removed when touched");
        assert!(PairingCodeFile::outstanding(&path, 1_060)?.is_none());
        Ok(())
    }

    #[test]
    fn malformed_and_foreign_files_refuse_and_a_zero_ttl_is_an_error() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let path = file_at(&dir);
        std::fs::write(&path, "{not json")?;
        assert_eq!(
            PairingCodeFile::redeem(&path, "123456", 1)?,
            Redeem::Refused
        );
        std::fs::write(
            &path,
            r#"{"v":2,"sha256":"00","issued":0,"expires":9,"label":"","attempts_left":5}"#,
        )?;
        assert_eq!(
            PairingCodeFile::redeem(&path, "123456", 1)?,
            Redeem::Refused
        );
        assert!(PairingCodeFile::issue(&path, 0, "x", 1).is_err());
        Ok(())
    }

    #[test]
    fn code_shape_and_fingerprint_rules() {
        assert!(is_well_formed_code("000000"));
        assert!(is_well_formed_code("483921"));
        assert!(!is_well_formed_code("48392"));
        assert!(!is_well_formed_code("4839211"));
        assert!(!is_well_formed_code("48392a"));
        assert!(!is_well_formed_code("４８３９２１"));
        for _ in 0..64 {
            assert!(is_well_formed_code(&random_code()));
        }
        let key = "ad220887aa0ba4d061b3f42712d73c9ce796dcb57be1f62dd2d9bd22ec29526c";
        let fp = key_fingerprint(key).expect("fingerprint");
        assert_eq!(fp.len(), 8);
        let raw = hex::decode(key).unwrap();
        let digest = <sha2::Sha256 as sha2::Digest>::digest(&raw);
        assert_eq!(fp, hex::encode(&digest[..4]));
        assert!(key_fingerprint("zz").is_err());
    }
}
