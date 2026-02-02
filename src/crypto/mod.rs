use anyhow::{anyhow, Result};
use rand::RngCore;
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
    rand::rngs::OsRng.fill_bytes(&mut seed_bytes);
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
