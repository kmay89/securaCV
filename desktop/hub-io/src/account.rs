//! account — mint the Home Assistant owner account at flash time, so first
//! contact with the hub is a login page, not a setup wizard.
//!
//! The "nothing left to type" half of the account pre-seed (design doc §7
//! step 5): the operator's name/username/password, collected by the flasher
//! next to the Wi-Fi, become HA's own `.storage` auth records — built HERE,
//! on the operator's computer. The password is bcrypt-hashed locally and only
//! the hash ever leaves this function; the cleartext gets the same custody as
//! the Wi-Fi secret (used, never logged, never sent, dropped).
//!
//! What this module emits is a set of `(relative path, contents)` files ready
//! to be placed under HA's config directory by whichever zero-touch restore
//! mechanism the hardware-validation session picks (data-partition injection,
//! CONFIG import, or a curated-backup restore) — the minting is
//! mechanism-independent, which is why it can land now.
//!
//! SCHEMA HONESTY: the JSON shapes below mirror Home Assistant's storage
//! format as documented and observed (auth storage version 1, provider
//! `homeassistant`, bcrypt `$2b$12`). They are deliberately minimal — every
//! field HA itself fills in on load (refresh tokens, ip bans, person entries)
//! is left for HA to create. The validation runbook
//! (docs/hub_validation_runbook.md) includes capturing the ground-truth
//! `.storage` from a real onboarded instance and diffing it against this
//! output BEFORE the pre-seed ships in a release.

use sha2::Digest;

/// The operator's account, as the flasher collects it.
#[derive(Debug, Clone)]
pub struct OwnerAccount<'a> {
    /// Display name, e.g. "Alex Smith". 1–128 chars.
    pub name: &'a str,
    /// Login username. HA treats usernames case-insensitively and strips
    /// surrounding whitespace; we normalize to that same canonical form.
    pub username: &'a str,
    /// Login password. 8+ chars (matching HA's own minimum).
    pub password: &'a str,
}

/// Why an account can't be minted. Each is a reason login would later fail or
/// surprise — better a clear error in the flasher than a hub you can't enter.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AccountError {
    EmptyName,
    NameTooLong {
        chars: usize,
    },
    EmptyUsername,
    /// HA lowercases usernames on check; spaces inside invite typo lockouts.
    UsernameHasWhitespace,
    PasswordTooShort {
        len: usize,
    },
}

impl AccountError {
    pub fn message(&self) -> String {
        match self {
            AccountError::EmptyName => "your name is empty".to_string(),
            AccountError::NameTooLong { chars } => {
                format!("the name is {chars} characters — keep it under 128")
            }
            AccountError::EmptyUsername => "the username is empty".to_string(),
            AccountError::UsernameHasWhitespace => "the username can't contain spaces".to_string(),
            AccountError::PasswordTooShort { len } => {
                format!("the password is {len} characters — Home Assistant needs at least 8")
            }
        }
    }
}

/// One file of the minted store: a path relative to HA's config directory and
/// its contents.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SeedFile {
    pub relative_path: String,
    pub contents: String,
}

/// bcrypt cost. 12 is HA's own default for `auth_provider.homeassistant`; a
/// login check on a Pi 5 at cost 12 is tens of milliseconds — right for an
/// interactive password.
const BCRYPT_COST: u32 = 12;

/// Mint the `.storage` files for a hub whose owner is `account`. Returns the
/// three files HA needs to boot straight to a login page:
///
///   * `.storage/auth_provider.homeassistant` — the username + bcrypt hash;
///   * `.storage/auth` — the owner user, the standard groups, and the
///     credential linking the two;
///   * `.storage/onboarding` — the wizard marked done, so first visit is
///     login, not setup.
///
/// Pure aside from randomness (ids, bcrypt salt). The password enters only
/// the bcrypt call; it appears in no output but the hash.
pub fn mint_owner_store(account: &OwnerAccount<'_>) -> Result<Vec<SeedFile>, String> {
    validate(account).map_err(|e| e.message())?;
    let username = canonical_username(account.username);

    let password_hash = bcrypt::hash(account.password, BCRYPT_COST)
        .map_err(|e| format!("couldn't hash the password: {e}"))?;
    let user_id = random_id()?;
    let credential_id = random_id()?;

    // Serialized via serde_json values so escaping of arbitrary names/usernames
    // is correct by construction, never by string surgery.
    let provider = serde_store(
        "auth_provider.homeassistant",
        serde_json::json!({
            "users": [{ "username": username, "password": password_hash }]
        }),
    )?;
    let auth = serde_store(
        "auth",
        serde_json::json!({
            "users": [{
                "id": user_id,
                "group_ids": ["system-admin"],
                "is_owner": true,
                "is_active": true,
                "name": account.name,
                "system_generated": false,
                "local_only": false,
            }],
            "groups": [
                { "id": "system-admin", "name": "Administrators" },
                { "id": "system-users", "name": "Users" },
                { "id": "system-read-only", "name": "Read Only" },
            ],
            "credentials": [{
                "id": credential_id,
                "user_id": user_id,
                "auth_provider_type": "homeassistant",
                "auth_provider_id": null,
                "data": { "username": username },
            }],
            "refresh_tokens": [],
        }),
    )?;
    let onboarding = serde_store(
        "onboarding",
        serde_json::json!({
            "done": ["user", "core_config", "analytics", "integration"]
        }),
    )?;

    Ok(vec![
        SeedFile {
            relative_path: ".storage/auth_provider.homeassistant".to_string(),
            contents: provider,
        },
        SeedFile {
            relative_path: ".storage/auth".to_string(),
            contents: auth,
        },
        SeedFile {
            relative_path: ".storage/onboarding".to_string(),
            contents: onboarding,
        },
    ])
}

/// HA's canonical username form: trimmed, lowercased.
pub fn canonical_username(raw: &str) -> String {
    raw.trim().to_lowercase()
}

fn validate(account: &OwnerAccount<'_>) -> Result<(), AccountError> {
    let name_chars = account.name.trim().chars().count();
    if name_chars == 0 {
        return Err(AccountError::EmptyName);
    }
    if name_chars > 128 {
        return Err(AccountError::NameTooLong { chars: name_chars });
    }
    let username = canonical_username(account.username);
    if username.is_empty() {
        return Err(AccountError::EmptyUsername);
    }
    if username.chars().any(char::is_whitespace) {
        return Err(AccountError::UsernameHasWhitespace);
    }
    let pw_len = account.password.chars().count();
    if pw_len < 8 {
        return Err(AccountError::PasswordTooShort { len: pw_len });
    }
    Ok(())
}

/// Wrap `data` in HA's storage envelope: `{version, minor_version, key, data}`,
/// pretty-printed the way HA writes its own stores.
fn serde_store(key: &str, data: serde_json::Value) -> Result<String, String> {
    let envelope = serde_json::json!({
        "version": 1,
        "minor_version": 1,
        "key": key,
        "data": data,
    });
    serde_json::to_string_pretty(&envelope).map_err(|e| format!("couldn't serialize {key}: {e}"))
}

/// A 32-hex random id, the shape HA uses for user/credential ids. Randomness
/// straight from the OS; hashed through SHA-256 so even a theoretically biased
/// source still yields uniform hex.
fn random_id() -> Result<String, String> {
    let mut seed = [0u8; 32];
    getrandom::fill(&mut seed).map_err(|e| format!("couldn't gather randomness: {e}"))?;
    let mut h = sha2::Sha256::new();
    h.update(seed);
    Ok(crate::sha256_hex(h)[..32].to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn owner<'a>() -> OwnerAccount<'a> {
        OwnerAccount {
            name: "Alex Smith",
            username: "  Alex  ",
            password: "correct horse battery staple",
        }
    }

    #[test]
    fn the_store_holds_a_verifying_hash_and_never_the_password() {
        let files = mint_owner_store(&owner()).expect("mints");
        let provider = files
            .iter()
            .find(|f| f.relative_path.ends_with("auth_provider.homeassistant"))
            .expect("provider file");
        // The cleartext appears nowhere in any emitted file.
        for f in &files {
            assert!(!f.contents.contains("correct horse battery staple"));
        }
        // The stored hash actually verifies the password (round trip through
        // the same bcrypt HA uses) and rejects a wrong one.
        let v: serde_json::Value = serde_json::from_str(&provider.contents).unwrap();
        let hash = v["data"]["users"][0]["password"].as_str().unwrap();
        assert!(hash.starts_with("$2"));
        assert!(bcrypt::verify("correct horse battery staple", hash).unwrap());
        assert!(!bcrypt::verify("wrong password", hash).unwrap());
        // Username was canonicalized.
        assert_eq!(v["data"]["users"][0]["username"], "alex");
    }

    #[test]
    fn auth_links_owner_user_and_credential_by_one_id() {
        let files = mint_owner_store(&owner()).unwrap();
        let auth = files
            .iter()
            .find(|f| f.relative_path == ".storage/auth")
            .unwrap();
        let v: serde_json::Value = serde_json::from_str(&auth.contents).unwrap();
        let user = &v["data"]["users"][0];
        let cred = &v["data"]["credentials"][0];
        assert_eq!(user["is_owner"], true);
        assert_eq!(user["group_ids"][0], "system-admin");
        assert_eq!(user["name"], "Alex Smith");
        assert_eq!(cred["user_id"], user["id"]);
        assert_eq!(cred["auth_provider_type"], "homeassistant");
        assert_eq!(cred["data"]["username"], "alex");
        // Ids are 32-hex and distinct.
        let uid = user["id"].as_str().unwrap();
        let cid = cred["id"].as_str().unwrap();
        assert_eq!(uid.len(), 32);
        assert_eq!(cid.len(), 32);
        assert!(uid.bytes().all(|b| b.is_ascii_hexdigit()));
        assert_ne!(uid, cid);
    }

    #[test]
    fn onboarding_is_marked_done_so_first_visit_is_login() {
        let files = mint_owner_store(&owner()).unwrap();
        let ob = files
            .iter()
            .find(|f| f.relative_path == ".storage/onboarding")
            .unwrap();
        let v: serde_json::Value = serde_json::from_str(&ob.contents).unwrap();
        let done: Vec<_> = v["data"]["done"]
            .as_array()
            .unwrap()
            .iter()
            .map(|s| s.as_str().unwrap())
            .collect();
        assert!(done.contains(&"user"));
        assert!(done.contains(&"core_config"));
    }

    #[test]
    fn every_store_wears_the_ha_envelope() {
        for f in mint_owner_store(&owner()).unwrap() {
            let v: serde_json::Value = serde_json::from_str(&f.contents).unwrap();
            assert_eq!(v["version"], 1);
            assert!(v["key"].as_str().unwrap().len() > 3);
            assert!(v["data"].is_object());
        }
    }

    #[test]
    fn bad_inputs_fail_with_reasons_before_any_hashing() {
        let cases = [
            (
                OwnerAccount {
                    name: "  ",
                    ..owner()
                },
                AccountError::EmptyName,
            ),
            (
                OwnerAccount {
                    username: "",
                    ..owner()
                },
                AccountError::EmptyUsername,
            ),
            (
                OwnerAccount {
                    username: "two words",
                    ..owner()
                },
                AccountError::UsernameHasWhitespace,
            ),
            (
                OwnerAccount {
                    password: "short",
                    ..owner()
                },
                AccountError::PasswordTooShort { len: 5 },
            ),
        ];
        for (acct, want) in cases {
            assert_eq!(validate(&acct).unwrap_err(), want);
            assert!(!want.message().is_empty());
        }
    }
}
