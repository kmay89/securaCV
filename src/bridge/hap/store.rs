//! Where a HAP accessory's identity and pairings live between restarts.
//!
//! Three things have to survive a restart or every controller in the house
//! has to pair again: the accessory's Ed25519 **seed**, its **device id**
//! (which is what controllers key their pairing on), and the **pairing
//! table**. Losing any one of them is indistinguishable, from the Home app's
//! side, from the accessory being replaced.
//!
//! # The seed is a secret, and this file treats it like one
//!
//! The seed is the accessory's identity: anyone holding it can impersonate
//! this bridge to every paired controller. So the state file is created
//! `0600` with `create_new` (never silently overwriting), and a file that is
//! readable by group or other is **refused, not fixed** — quietly
//! tightening the mode would hide the fact that the seed has already been
//! exposed to everyone who could read it. That is the same posture the
//! device key file takes in `src/crypto`.
//!
//! On a Canary the equivalent store is NVS, and the same rule applies one
//! layer down: pairing keys get the `opera_secret` treatment and are refused
//! on a device without flash encryption.

use std::fs;
use std::io::Write;
use std::path::Path;

use anyhow::{anyhow, Context, Result};
use rand::rngs::SysRng;
use rand::TryRng;
use serde::{Deserialize, Serialize};

use super::pairing::{AccessoryIdentity, Pairing, PairingStore, MAX_PAIRINGS};

/// The on-disk format version, so a future change can migrate rather than
/// misread.
pub const FORMAT_VERSION: u32 = 1;

/// Setup codes HAP forbids, because they are the ones everybody guesses.
///
/// Note that `123-45-678` is on this list, which is why it appears only in
/// tests here and never as a default.
const FORBIDDEN_CODES: [&str; 12] = [
    "00000000", "11111111", "22222222", "33333333", "44444444", "55555555", "66666666", "77777777",
    "88888888", "99999999", "12345678", "87654321",
];

/// One pairing, as stored.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredPairing {
    /// The controller's pairing id.
    pub id: String,
    /// Its long-term public key, hex-encoded.
    pub ltpk: String,
    /// Whether it may change the pairing list.
    pub admin: bool,
}

/// Everything that must outlive the process.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PersistedState {
    /// On-disk format version.
    pub version: u32,
    /// The accessory's device id, in the MAC-shaped form HAP expects.
    pub device_id: String,
    /// The setup code, formatted `XXX-XX-XXX`.
    pub setup_code: String,
    /// The four-character setup id that rides in the QR payload.
    pub setup_id: String,
    /// The Ed25519 seed, hex-encoded. **Secret.**
    pub seed: String,
    /// The accessory database's config number.
    pub config_number: u64,
    /// Hash of the fleet shape (ids + names, in order) the config number
    /// last accounted for. When `hap.toml`'s fleet changes — a Canary
    /// added, removed, renamed, or reordered — the next start bumps `c#`
    /// so controllers re-read `/accessories` instead of trusting their
    /// cache. Empty on files written before this field existed: treated
    /// as unknown, so the first start after upgrade bumps once.
    #[serde(default)]
    pub fleet_hash: String,
    /// Paired controllers.
    pub pairings: Vec<StoredPairing>,
}

impl PersistedState {
    /// Mint a brand-new accessory identity.
    pub fn generate() -> Result<Self> {
        let mut seed = [0u8; 32];
        SysRng
            .try_fill_bytes(&mut seed)
            .map_err(|e| anyhow!("OS RNG unavailable: {e}"))?;
        Ok(PersistedState {
            version: FORMAT_VERSION,
            device_id: random_device_id()?,
            setup_code: random_setup_code()?,
            setup_id: random_setup_id()?,
            seed: hex::encode(seed),
            config_number: 1,
            fleet_hash: String::new(),
            pairings: Vec::new(),
        })
    }

    /// The accessory identity this state describes.
    pub fn identity(&self) -> Result<AccessoryIdentity> {
        let bytes = hex::decode(&self.seed).context("HAP seed is not valid hex")?;
        let seed: [u8; 32] = bytes
            .as_slice()
            .try_into()
            .map_err(|_| anyhow!("HAP seed must be 32 bytes, got {}", bytes.len()))?;
        Ok(AccessoryIdentity::from_seed(self.device_id.clone(), seed))
    }

    /// The pairing table this state describes.
    pub fn pairing_store(&self) -> PairingStore {
        let pairings = self.pairings.iter().filter_map(|p| {
            let bytes = hex::decode(&p.ltpk).ok()?;
            let ltpk: [u8; 32] = bytes.as_slice().try_into().ok()?;
            Some(Pairing {
                id: p.id.clone(),
                ltpk,
                admin: p.admin,
            })
        });
        PairingStore::from_pairings(pairings)
    }

    /// Copy a live pairing table back into this state, ready to save.
    pub fn absorb(&mut self, store: &PairingStore, config_number: u64) {
        self.pairings = store
            .list()
            .iter()
            .take(MAX_PAIRINGS)
            .map(|p| StoredPairing {
                id: p.id.clone(),
                ltpk: hex::encode(p.ltpk),
                admin: p.admin,
            })
            .collect();
        self.config_number = config_number;
    }
}

/// Load the state file, or create one if it does not exist yet.
pub fn load_or_create(path: &Path) -> Result<PersistedState> {
    match load(path) {
        Ok(Some(state)) => Ok(state),
        Ok(None) => {
            let state = PersistedState::generate()?;
            save(path, &state)?;
            Ok(state)
        }
        Err(e) => Err(e),
    }
}

/// Read the state file if it exists.
pub fn load(path: &Path) -> Result<Option<PersistedState>> {
    if !path.exists() {
        return Ok(None);
    }
    check_permissions(path)?;
    let text = fs::read_to_string(path)
        .with_context(|| format!("failed to read HAP state {}", path.display()))?;
    let state: PersistedState = serde_json::from_str(&text)
        .with_context(|| format!("HAP state {} is not valid JSON", path.display()))?;
    if state.version != FORMAT_VERSION {
        return Err(anyhow!(
            "HAP state {} is format version {}, this build understands {}",
            path.display(),
            state.version,
            FORMAT_VERSION
        ));
    }
    Ok(Some(state))
}

/// Write the state file, replacing any previous contents.
///
/// Writes to a temporary file in the same directory and renames, so a crash
/// mid-write cannot leave a truncated identity behind — losing the seed is
/// the one failure that forces every controller in the house to re-pair.
pub fn save(path: &Path, state: &PersistedState) -> Result<()> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).with_context(|| {
                format!("failed to create HAP state directory {}", parent.display())
            })?;
        }
    }
    let text = serde_json::to_string_pretty(state)?;
    let tmp = path.with_extension("json.tmp");

    let mut options = fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    {
        let mut file = options
            .open(&tmp)
            .with_context(|| format!("failed to open {}", tmp.display()))?;
        file.write_all(text.as_bytes())?;
        file.sync_all()?;
    }
    fs::rename(&tmp, path)
        .with_context(|| format!("failed to install HAP state at {}", path.display()))?;
    Ok(())
}

/// Refuse a file holding a secret that anyone but its owner can read.
///
/// Shared with [`config`](super::config), whose file can carry the broker
/// password. Both refuse rather than silently tightening the mode: quietly
/// re-hiding a secret that has already been exposed to every local user
/// hides the fact that it *was* exposed, which is the part the operator
/// needs to act on.
#[cfg(unix)]
pub fn refuse_if_group_or_world_readable(path: &Path, holds: &str) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let mode = fs::metadata(path)
        .with_context(|| format!("failed to stat {}", path.display()))?
        .permissions()
        .mode();
    if mode & 0o077 != 0 {
        return Err(anyhow!(
            "{} is mode {:o}: it holds {holds} and must not be readable by group or other. \
             Fix with `chmod 600 {}` — and treat the secret as exposed, because it was.",
            path.display(),
            mode & 0o777,
            path.display()
        ));
    }
    Ok(())
}

#[cfg(not(unix))]
pub fn refuse_if_group_or_world_readable(_path: &Path, _holds: &str) -> Result<()> {
    Ok(())
}

fn check_permissions(path: &Path) -> Result<()> {
    refuse_if_group_or_world_readable(path, "the accessory's private key")
}

/// A random `XXX-XX-XXX` setup code that is not one HAP forbids.
pub fn random_setup_code() -> Result<String> {
    loop {
        let mut bytes = [0u8; 8];
        SysRng
            .try_fill_bytes(&mut bytes)
            .map_err(|e| anyhow!("OS RNG unavailable: {e}"))?;
        // Rejection-free mapping is not needed here: the tiny modulo bias
        // across 0..=255 into 0..=9 is irrelevant against a code whose whole
        // job is to be typed by a human and rate-limited by MAX_SETUP_ATTEMPTS.
        let digits: String = bytes.iter().map(|b| char::from(b'0' + (b % 10))).collect();
        if FORBIDDEN_CODES.contains(&digits.as_str()) {
            continue;
        }
        return Ok(format!(
            "{}-{}-{}",
            &digits[..3],
            &digits[3..5],
            &digits[5..]
        ));
    }
}

/// A random four-character setup id.
pub fn random_setup_id() -> Result<String> {
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    let mut bytes = [0u8; 4];
    SysRng
        .try_fill_bytes(&mut bytes)
        .map_err(|e| anyhow!("OS RNG unavailable: {e}"))?;
    Ok(bytes
        .iter()
        .map(|b| char::from(ALPHABET[usize::from(*b) % ALPHABET.len()]))
        .collect())
}

/// A random MAC-shaped device id. Not a real MAC address — HAP only needs a
/// stable identifier in that shape, and using the host's actual MAC would
/// put a hardware serial number on every mDNS packet.
pub fn random_device_id() -> Result<String> {
    let mut bytes = [0u8; 6];
    SysRng
        .try_fill_bytes(&mut bytes)
        .map_err(|e| anyhow!("OS RNG unavailable: {e}"))?;
    // Set the locally-administered bit and clear the multicast bit, so the
    // identifier cannot collide with a real vendor-assigned address.
    bytes[0] = (bytes[0] | 0b0000_0010) & 0b1111_1110;
    Ok(bytes
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect::<Vec<_>>()
        .join(":"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir() -> tempfile::TempDir {
        tempfile::tempdir().expect("tempdir")
    }

    #[test]
    fn a_generated_state_round_trips_through_a_file() {
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        let created = load_or_create(&path).expect("creates");
        let loaded = load(&path).expect("loads").expect("present");
        assert_eq!(created.seed, loaded.seed);
        assert_eq!(created.device_id, loaded.device_id);
        assert_eq!(created.setup_code, loaded.setup_code);
    }

    /// The identity must be reproducible from the file, or every controller
    /// re-pairs on restart.
    #[test]
    fn identity_survives_a_restart() {
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        let first = load_or_create(&path).expect("creates");
        let second = load_or_create(&path).expect("loads existing");
        assert_eq!(
            first.identity().expect("id").ltpk(),
            second.identity().expect("id").ltpk(),
            "a restart must not change the accessory's identity"
        );
    }

    #[test]
    fn pairings_round_trip() {
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        let mut state = load_or_create(&path).expect("creates");

        let mut store = state.pairing_store();
        store
            .add(Pairing {
                id: "controller-1".into(),
                ltpk: [7u8; 32],
                admin: true,
            })
            .expect("add");
        state.absorb(&store, 4);
        save(&path, &state).expect("saves");

        let reloaded = load(&path).expect("loads").expect("present");
        assert_eq!(reloaded.config_number, 4);
        let restored = reloaded.pairing_store();
        assert_eq!(restored.len(), 1);
        assert_eq!(
            restored.get("controller-1").map(|p| p.ltpk),
            Some([7u8; 32])
        );
        assert_eq!(restored.get("controller-1").map(|p| p.admin), Some(true));
    }

    #[cfg(unix)]
    #[test]
    fn the_state_file_is_created_private() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        load_or_create(&path).expect("creates");
        let mode = fs::metadata(&path).expect("stat").permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "the seed must not be world-readable");
    }

    /// A leaked key must be reported, not silently re-hidden.
    #[cfg(unix)]
    #[test]
    fn a_world_readable_state_file_is_refused() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        load_or_create(&path).expect("creates");
        fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).expect("chmod");

        let err = load(&path).expect_err("must refuse");
        let msg = err.to_string();
        assert!(msg.contains("644"), "got: {msg}");
        assert!(msg.contains("chmod 600"), "must say how to fix it: {msg}");
    }

    #[test]
    fn a_future_format_version_is_refused_not_misread() {
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        let mut state = PersistedState::generate().expect("generate");
        state.version = FORMAT_VERSION + 1;
        save(&path, &state).expect("saves");
        assert!(load(&path).is_err());
    }

    #[test]
    fn setup_codes_avoid_the_forbidden_list() {
        for _ in 0..200 {
            let code = random_setup_code().expect("code");
            assert_eq!(code.len(), 10, "XXX-XX-XXX");
            let digits: String = code.chars().filter(|c| c.is_ascii_digit()).collect();
            assert_eq!(digits.len(), 8);
            assert!(
                !FORBIDDEN_CODES.contains(&digits.as_str()),
                "generated a forbidden code: {code}"
            );
        }
    }

    #[test]
    fn setup_ids_are_four_uppercase_alphanumerics() {
        for _ in 0..50 {
            let id = random_setup_id().expect("id");
            assert_eq!(id.len(), 4);
            assert!(id
                .chars()
                .all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()));
        }
    }

    /// The device id must be locally administered, so it cannot collide with
    /// a real vendor MAC — and must not *be* the host's MAC.
    #[test]
    fn device_ids_are_locally_administered() {
        for _ in 0..50 {
            let id = random_device_id().expect("id");
            let first = u8::from_str_radix(&id[..2], 16).expect("hex");
            assert_eq!(first & 0b0000_0010, 0b0000_0010, "locally administered bit");
            assert_eq!(first & 0b0000_0001, 0, "not multicast");
            assert_eq!(id.len(), 17, "AA:BB:CC:DD:EE:FF");
        }
    }

    #[test]
    fn two_generated_accessories_are_different() {
        let a = PersistedState::generate().expect("a");
        let b = PersistedState::generate().expect("b");
        assert_ne!(a.seed, b.seed);
        assert_ne!(a.device_id, b.device_id);
    }

    #[test]
    fn a_corrupt_state_file_is_an_error_not_a_panic() {
        let dir = tmpdir();
        let path = dir.path().join("hap.json");
        fs::write(&path, "{ not json").expect("write");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).expect("chmod");
        }
        assert!(load(&path).is_err());
    }

    /// A seed of the wrong length must be reported rather than padded into
    /// a different accessory.
    #[test]
    fn a_short_seed_is_refused() {
        let mut state = PersistedState::generate().expect("generate");
        state.seed = hex::encode([1u8; 16]);
        let err = state.identity().expect_err("must refuse");
        assert!(err.to_string().contains("32 bytes"), "got: {err}");
    }
}
