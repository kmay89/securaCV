use anyhow::{anyhow, Result};
use rand::TryRng;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use zeroize::Zeroize;

pub mod signatures;

/// Resolve the device seed file path based on a SQLite database path.
pub fn device_key_path_for_db(db_path: &str) -> Result<PathBuf> {
    if db_path == ":memory:" {
        return Err(anyhow!(
            "device key file requires a persistent db_path (got :memory:)"
        ));
    }

    let path_str = db_path
        .strip_prefix("file:")
        .map(|s| s.split('?').next().unwrap_or(s))
        .unwrap_or(db_path);
    if path_str.is_empty() {
        return Err(anyhow!("device key file path is empty"));
    }

    let path = Path::new(path_str);
    Ok(path.with_extension("ed25519.seed"))
}

/// Load a device seed from disk or create one (optionally seeding from a provided value).
///
/// The seed is stored locally and reused across restarts.
pub fn load_or_create_device_seed(
    path: impl AsRef<Path>,
    provided_seed: Option<&str>,
) -> Result<String> {
    let path = path.as_ref();
    if let Some(seed) = read_seed_file(path)? {
        if let Some(provided) = provided_seed {
            if seed != provided.trim() {
                return Err(anyhow!(
                    "device key seed mismatch: provided seed does not match stored seed"
                ));
            }
        }
        return Ok(seed);
    }

    if let Some(seed) = provided_seed {
        let trimmed = seed.trim();
        if trimmed.is_empty() {
            return Err(anyhow!("device key seed is empty"));
        }
        let written = write_seed_file(path, trimmed)?;
        if !written {
            if let Some(existing) = read_seed_file(path)? {
                if existing != trimmed {
                    return Err(anyhow!(
                        "device key seed mismatch: provided seed does not match stored seed"
                    ));
                }
            }
        }
        return Ok(trimmed.to_string());
    }

    let mut seed_bytes = [0u8; 32];
    rand::rngs::SysRng
        .try_fill_bytes(&mut seed_bytes[..])
        .expect("OS RNG unavailable");
    let seed_hex = hex::encode(seed_bytes);
    seed_bytes.zeroize();
    let seed = format!("devkey:{}", seed_hex);
    let written = write_seed_file(path, &seed)?;
    if !written {
        if let Some(existing) = read_seed_file(path)? {
            return Ok(existing);
        }
    }
    Ok(seed)
}

fn read_seed_file(path: &Path) -> Result<Option<String>> {
    if !path.exists() {
        return Ok(None);
    }
    let contents = fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read device key seed {}: {}", path.display(), e))?;
    let trimmed = contents.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("device key seed file {} is empty", path.display()));
    }
    Ok(Some(trimmed.to_string()))
}

fn write_seed_file(path: &Path, seed: &str) -> Result<bool> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                anyhow!(
                    "failed to create device key directory {}: {}",
                    parent.display(),
                    e
                )
            })?;
        }
    }

    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }

    let mut file = match options.open(path) {
        Ok(file) => file,
        Err(err) if err.kind() == std::io::ErrorKind::AlreadyExists => {
            return Ok(false);
        }
        Err(err) => {
            return Err(anyhow!(
                "failed to create device key seed {}: {}",
                path.display(),
                err
            ))
        }
    };

    file.write_all(seed.as_bytes())
        .and_then(|_| file.write_all(b"\n"))
        .and_then(|_| file.sync_all())
        .map_err(|e| anyhow!("failed to write device key seed {}: {}", path.display(), e))?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_key_path_rejects_memory_db() {
        let result = device_key_path_for_db(":memory:");
        assert!(result.is_err());
        assert!(
            format!("{}", result.unwrap_err()).contains(":memory:"),
            "error should mention :memory:"
        );
    }

    #[test]
    fn device_key_path_strips_file_prefix_and_query() {
        let path = device_key_path_for_db("file:/data/witness.db?mode=rwc").unwrap();
        assert_eq!(path, PathBuf::from("/data/witness.ed25519.seed"));

        let path = device_key_path_for_db("file:///data/witness.db?mode=rwc").unwrap();
        assert_eq!(path, PathBuf::from("/data/witness.ed25519.seed"));
    }

    #[test]
    fn device_key_path_plain_db() {
        let path = device_key_path_for_db("witness.db").unwrap();
        assert_eq!(path, PathBuf::from("witness.ed25519.seed"));
    }

    #[test]
    fn device_key_path_rejects_empty() {
        assert!(device_key_path_for_db("").is_err());
        assert!(device_key_path_for_db("file:").is_err());
    }

    #[test]
    fn load_or_create_generates_seed() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test.seed");

        let seed = load_or_create_device_seed(&path, None).unwrap();
        assert!(seed.starts_with("devkey:"));
        assert_eq!(seed.len(), "devkey:".len() + 64);

        let reload = load_or_create_device_seed(&path, None).unwrap();
        assert_eq!(seed, reload, "reloaded seed should match");
    }

    #[test]
    fn load_or_create_uses_provided_seed() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("provided.seed");
        let test_seed = "devkey:a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6";

        let seed = load_or_create_device_seed(&path, Some(test_seed)).unwrap();
        assert_eq!(seed, test_seed);

        let reload = load_or_create_device_seed(&path, Some(test_seed)).unwrap();
        assert_eq!(reload, test_seed);
    }

    #[test]
    fn load_or_create_rejects_mismatched_seed() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("mismatch.seed");
        let original = "devkey:original_seed_value_with_enough_entropy";
        let different = "devkey:different_seed_value_with_enough_entropy";

        load_or_create_device_seed(&path, Some(original)).unwrap();

        let result = load_or_create_device_seed(&path, Some(different));
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("mismatch"));
    }

    #[test]
    fn load_or_create_rejects_empty_seed() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("empty.seed");
        assert!(load_or_create_device_seed(&path, Some("")).is_err());
        assert!(load_or_create_device_seed(&path, Some("   ")).is_err());
    }

    #[test]
    #[cfg(unix)]
    fn seed_file_created_with_restricted_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("perms.seed");

        load_or_create_device_seed(&path, Some("devkey:perms_test_seed_with_entropy")).unwrap();

        let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "seed file should be mode 0600");
    }

    #[test]
    fn seed_file_not_overwritten_on_race() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("race.seed");

        fs::write(&path, "devkey:first\n").unwrap();

        let result = write_seed_file(&path, "devkey:second").unwrap();
        assert!(!result, "write should return false when file exists");

        let contents = fs::read_to_string(&path).unwrap();
        assert_eq!(contents.trim(), "devkey:first");
    }
}
