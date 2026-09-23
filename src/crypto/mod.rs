use anyhow::{anyhow, Result};
use rand::TryRng;
use std::fmt;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use zeroize::Zeroize;

pub mod signatures;

/// Where a resolved device seed came from. Daemons log the SOURCE at startup
/// (it is operational information: "which file", "the environment"); the seed
/// value itself is never logged, printed, or formatted anywhere.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SeedSource {
    /// `DEVICE_KEY_SEED` (or the equivalent `--device-key-seed` flag) supplied it.
    Env,
    /// Read from the mode-0600 seed file beside the database.
    File(PathBuf),
    /// Freshly generated from the OS RNG and persisted to the seed file.
    Generated(PathBuf),
}

impl fmt::Display for SeedSource {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SeedSource::Env => write!(f, "DEVICE_KEY_SEED (environment or --device-key-seed)"),
            SeedSource::File(path) => write!(f, "seed file {}", path.display()),
            SeedSource::Generated(path) => {
                write!(f, "generated (written to {}, mode 0600)", path.display())
            }
        }
    }
}

/// A device seed together with where it came from. `seed` is the value a
/// `KernelConfig` takes; `source` is what a daemon may log.
#[derive(Clone, Debug)]
pub struct ResolvedSeed {
    pub seed: String,
    pub source: SeedSource,
}

/// Mint a fresh device seed from the OS RNG: `devkey:` + 64 hex chars (32 bytes
/// of entropy), the format `witnessd` has always generated on first start.
pub fn generate_device_seed() -> String {
    let mut seed_bytes = [0u8; 32];
    rand::rngs::SysRng
        .try_fill_bytes(&mut seed_bytes[..])
        .expect("OS RNG unavailable");
    let seed_hex = hex::encode(seed_bytes);
    seed_bytes.zeroize();
    format!("devkey:{}", seed_hex)
}

/// Resolve the device seed for a WRITE-SIDE process (`witnessd`, the bridges,
/// `witness_api`, `break_glass_serve`): the environment wins, else the seed
/// file beside the database (`<db>.ed25519.seed`), else a fresh seed is
/// generated and persisted there at mode 0600. An environment seed is
/// persisted to the file when no file exists yet (so every other process on
/// the host can find it), and refused when the file already holds a
/// *different* seed — two identities on one database is a configuration
/// error, caught here rather than as a `device public key mismatch` at open.
///
/// A database that can have no seed file (`:memory:`) resolves from the
/// environment only.
pub fn resolve_device_seed(db_path: &str, env_seed: Option<&str>) -> Result<ResolvedSeed> {
    let env_seed = env_seed.map(str::trim).filter(|s| !s.is_empty());
    let path = match device_key_path_for_db(db_path) {
        Ok(path) => path,
        Err(err) => {
            return match env_seed {
                Some(seed) => Ok(ResolvedSeed {
                    seed: seed.to_string(),
                    source: SeedSource::Env,
                }),
                None => Err(err),
            }
        }
    };
    load_or_create_device_seed_with_source(&path, env_seed)
}

/// Find an EXISTING device seed without ever creating one: the environment
/// (or flag value) wins and is used as given; otherwise the seed file beside
/// the database is read. `Ok(None)` when neither is present. For verifier
/// CLIs and ceremony commands, which must never mint an identity as a side
/// effect of being pointed at the wrong path.
pub fn find_device_seed(db_path: &str, env_seed: Option<&str>) -> Result<Option<ResolvedSeed>> {
    if let Some(seed) = env_seed.map(str::trim).filter(|s| !s.is_empty()) {
        return Ok(Some(ResolvedSeed {
            seed: seed.to_string(),
            source: SeedSource::Env,
        }));
    }
    let Ok(path) = device_key_path_for_db(db_path) else {
        return Ok(None);
    };
    Ok(read_seed_file(&path)?.map(|seed| ResolvedSeed {
        seed,
        source: SeedSource::File(path),
    }))
}

/// Read an existing seed file — mode-checked exactly like every other read —
/// without ever creating one. `Ok(None)` when the file does not exist. For a
/// ceremony pointed at a seed file kept somewhere other than beside the
/// database (`break_glass rotate-identity --seed-file`).
pub fn read_device_seed_file(path: &Path) -> Result<Option<String>> {
    read_seed_file(path)
}

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
    load_or_create_device_seed_with_source(path.as_ref(), provided_seed).map(|r| r.seed)
}

fn load_or_create_device_seed_with_source(
    path: &Path,
    provided_seed: Option<&str>,
) -> Result<ResolvedSeed> {
    if let Some(seed) = read_seed_file(path)? {
        if let Some(provided) = provided_seed {
            if seed != provided.trim() {
                return Err(anyhow!(
                    "device key seed mismatch: provided seed does not match stored seed"
                ));
            }
            return Ok(ResolvedSeed {
                seed,
                source: SeedSource::Env,
            });
        }
        return Ok(ResolvedSeed {
            seed,
            source: SeedSource::File(path.to_path_buf()),
        });
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
        return Ok(ResolvedSeed {
            seed: trimmed.to_string(),
            source: SeedSource::Env,
        });
    }

    let seed = generate_device_seed();
    let written = write_seed_file(path, &seed)?;
    if !written {
        if let Some(existing) = read_seed_file(path)? {
            return Ok(ResolvedSeed {
                seed: existing,
                source: SeedSource::File(path.to_path_buf()),
            });
        }
    }
    Ok(ResolvedSeed {
        seed,
        source: SeedSource::Generated(path.to_path_buf()),
    })
}

/// Read the seed file if it exists. On Unix a file that any other user can
/// read or write (`mode & 0o077 != 0`) is REFUSED, not silently accepted: the
/// seed is the signing identity, and a `chmod 644` (or a copy made with a lax
/// umask) is exactly the mistake this check exists to catch. Mirrors the
/// database's own 0600 conformance rule.
fn read_seed_file(path: &Path) -> Result<Option<String>> {
    if !path.exists() {
        return Ok(None);
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(path)
            .map_err(|e| anyhow!("failed to stat device key seed {}: {}", path.display(), e))?
            .permissions()
            .mode()
            & 0o777;
        if mode & 0o077 != 0 {
            return Err(anyhow!(
                "device key seed file {} is readable by other users (mode {:04o}); refusing to \
                 use it — run: chmod 600 {}",
                path.display(),
                mode,
                path.display()
            ));
        }
    }
    let contents = fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read device key seed {}: {}", path.display(), e))?;
    let trimmed = contents.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("device key seed file {} is empty", path.display()));
    }
    Ok(Some(trimmed.to_string()))
}

/// The path a replacement seed is staged at before it is renamed over `path`.
fn staged_seed_path(path: &Path) -> PathBuf {
    let mut name = path
        .file_name()
        .map(|n| n.to_os_string())
        .unwrap_or_default();
    name.push(".new");
    path.with_file_name(name)
}

/// Stage a replacement seed at `<path>.new` — a fresh mode-0600 file, fsynced —
/// without touching `path`. Returns the staged path for [`commit_seed_file`].
/// A rotation stages the successor BEFORE the kernel commits the rotation, so
/// the new seed is durable on disk before the old one stops opening the log.
/// Refuses when a staged file already exists: it may hold a successor from a
/// ceremony that never committed, and discarding it silently could discard
/// the only copy of a key the log already expects.
pub fn stage_seed_file(path: &Path, seed: &str) -> Result<PathBuf> {
    let staged = staged_seed_path(path);
    if !write_seed_file(&staged, seed)? {
        return Err(anyhow!(
            "a staged seed file already exists at {}: an earlier rotation did not complete. If \
             the kernel still opens with the current seed, that staged seed was never activated \
             and the file can be removed; if it does not, rename the staged file over {} instead",
            staged.display(),
            path.display()
        ));
    }
    Ok(staged)
}

/// Atomically move a staged seed file over `path` (rename), then fsync the
/// directory so the rename itself is durable.
pub fn commit_seed_file(staged: &Path, path: &Path) -> Result<()> {
    fs::rename(staged, path).map_err(|e| {
        anyhow!(
            "failed to move staged seed {} over {}: {}",
            staged.display(),
            path.display(),
            e
        )
    })?;
    #[cfg(unix)]
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            if let Ok(dir) = fs::File::open(parent) {
                let _ = dir.sync_all();
            }
        }
    }
    Ok(())
}

/// Replace the seed file atomically: stage `<path>.new` (mode 0600, fsynced)
/// and rename it over `path`. Either the old seed or the new one is on disk at
/// every instant; a crash never leaves a truncated file, and no `.new` file is
/// left behind on success.
pub fn replace_seed_file(path: &Path, seed: &str) -> Result<()> {
    let staged = stage_seed_file(path, seed)?;
    commit_seed_file(&staged, path)
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

    /// A seed file another user can read is refused with the fix named, and
    /// accepted again once it is `chmod 600` — fail closed, not silent.
    #[test]
    #[cfg(unix)]
    fn loose_mode_seed_file_is_refused_until_chmod_600() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("loose.seed");
        fs::write(&path, "devkey:loose_mode_seed_value_with_entropy\n").unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();

        let err = load_or_create_device_seed(&path, None)
            .unwrap_err()
            .to_string();
        assert!(err.contains("chmod 600"), "error must name the fix: {err}");
        assert!(
            err.contains("0644"),
            "error must name the offending mode: {err}"
        );
        assert!(
            !err.contains("loose_mode_seed_value"),
            "error must never carry the seed value: {err}"
        );

        // Group-readable is just as loose as world-readable.
        fs::set_permissions(&path, fs::Permissions::from_mode(0o640)).unwrap();
        assert!(load_or_create_device_seed(&path, None).is_err());

        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(
            load_or_create_device_seed(&path, None).unwrap(),
            "devkey:loose_mode_seed_value_with_entropy"
        );
    }

    /// Resolution order for a write-side daemon: environment > seed file >
    /// generate. The source label follows the same order, and a mismatching
    /// pair is refused rather than silently preferring one.
    #[test]
    fn resolve_device_seed_prefers_env_then_file_then_generates() {
        let dir = tempfile::tempdir().unwrap();
        let db = dir.path().join("witness.db");
        let db = db.to_str().unwrap();
        let seed_path = device_key_path_for_db(db).unwrap();

        // Nothing anywhere: generate and persist.
        let first = resolve_device_seed(db, None).unwrap();
        assert_eq!(first.source, SeedSource::Generated(seed_path.clone()));
        assert!(first.seed.starts_with("devkey:"));
        assert!(seed_path.is_file());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(&seed_path).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o600);
        }

        // File present, no environment: the file.
        let second = resolve_device_seed(db, None).unwrap();
        assert_eq!(second.seed, first.seed);
        assert_eq!(second.source, SeedSource::File(seed_path.clone()));

        // Environment agrees with the file: the environment is the source.
        let third = resolve_device_seed(db, Some(&format!("  {}\n", first.seed))).unwrap();
        assert_eq!(third.seed, first.seed);
        assert_eq!(third.source, SeedSource::Env);

        // Environment disagrees with the file: refused (two identities, one log).
        let err = resolve_device_seed(db, Some("devkey:some_other_seed_with_enough_entropy"))
            .unwrap_err()
            .to_string();
        assert!(err.contains("mismatch"), "{err}");

        // An empty environment value counts as absent.
        let fourth = resolve_device_seed(db, Some("   ")).unwrap();
        assert_eq!(fourth.source, SeedSource::File(seed_path));
    }

    /// An environment seed is persisted to the seed file when none exists, so
    /// the file becomes the fallback for every other process on the host.
    #[test]
    fn resolve_device_seed_persists_env_seed_when_no_file_exists() {
        let dir = tempfile::tempdir().unwrap();
        let db = dir.path().join("witness.db");
        let db = db.to_str().unwrap();
        let env_seed = "devkey:from_the_environment_with_entropy";

        let resolved = resolve_device_seed(db, Some(env_seed)).unwrap();
        assert_eq!(resolved.seed, env_seed);
        assert_eq!(resolved.source, SeedSource::Env);

        let found = find_device_seed(db, None).unwrap().expect("file persisted");
        assert_eq!(found.seed, env_seed);
        assert!(matches!(found.source, SeedSource::File(_)));
    }

    /// `:memory:` has no seed file: the environment is the only source.
    #[test]
    fn resolve_device_seed_memory_db_is_env_only() {
        let resolved =
            resolve_device_seed(":memory:", Some("devkey:memory_db_seed_with_entropy_x")).unwrap();
        assert_eq!(resolved.source, SeedSource::Env);
        assert!(resolve_device_seed(":memory:", None).is_err());
        assert!(find_device_seed(":memory:", None).unwrap().is_none());
    }

    /// The verifier-side lookup never mints an identity: absent everywhere is
    /// `None` and leaves no file behind; a flag value is used as given.
    #[test]
    fn find_device_seed_never_creates() {
        let dir = tempfile::tempdir().unwrap();
        let db = dir.path().join("witness.db");
        let db = db.to_str().unwrap();
        let seed_path = device_key_path_for_db(db).unwrap();

        assert!(find_device_seed(db, None).unwrap().is_none());
        assert!(!seed_path.exists(), "find must not create a seed file");

        let flagged = find_device_seed(db, Some("devkey:flag_value_with_enough_entropy_"))
            .unwrap()
            .expect("flag value is a seed");
        assert_eq!(flagged.source, SeedSource::Env);
        assert!(
            !seed_path.exists(),
            "a flag value is never persisted by find"
        );

        let created = resolve_device_seed(db, None).unwrap();
        let found = find_device_seed(db, None)
            .unwrap()
            .expect("file now present");
        assert_eq!(found.seed, created.seed);
        assert_eq!(found.source, SeedSource::File(seed_path));
    }

    /// Replacement is atomic: the file holds either the old or the new seed,
    /// no `.new` file survives a successful replace, the mode stays 0600, and
    /// a stale staged file from an interrupted ceremony blocks the next one.
    #[test]
    fn replace_seed_file_is_atomic_and_leaves_no_staging_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("rotate.seed");
        load_or_create_device_seed(&path, Some("devkey:first_identity_seed_with_entropy")).unwrap();

        replace_seed_file(&path, "devkey:second_identity_seed_with_entropy").unwrap();
        assert_eq!(
            fs::read_to_string(&path).unwrap().trim(),
            "devkey:second_identity_seed_with_entropy"
        );
        let staged = staged_seed_path(&path);
        assert!(!staged.exists(), "no .new file may be left behind");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o600, "replaced seed file must stay 0600");
        }

        // A staged successor that never committed is not silently discarded.
        let staged = stage_seed_file(&path, "devkey:third_identity_seed_with_entropy_").unwrap();
        assert!(staged.exists());
        let err = stage_seed_file(&path, "devkey:fourth_identity_seed_with_entropy")
            .unwrap_err()
            .to_string();
        assert!(err.contains("did not complete"), "{err}");
        // The live file is untouched by a refused stage.
        assert_eq!(
            fs::read_to_string(&path).unwrap().trim(),
            "devkey:second_identity_seed_with_entropy"
        );
        commit_seed_file(&staged, &path).unwrap();
        assert_eq!(
            fs::read_to_string(&path).unwrap().trim(),
            "devkey:third_identity_seed_with_entropy_"
        );
        assert!(!staged.exists());
    }
}
