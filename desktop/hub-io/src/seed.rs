//! seed — put the "it joins your Wi-Fi by itself" file onto the freshly
//! written boot partition.
//!
//! `hub_core::hub_seed::wifi_keyfile` builds the NetworkManager keyfile; this
//! module owns getting it into `CONFIG/network/` on the card's first (boot)
//! partition. The layout logic — which device node is partition one, where the
//! keyfile lands under a mount root, what the file may be called — is pure and
//! host-tested. The platform edge (asking the OS to re-read the partition
//! table, mount, unmount, eject) is thin command glue over `diskutil` /
//! `udisksctl`, and every failure is a clear sentence: seeding happens AFTER a
//! verified write, so the worst case is a hub that boots without Wi-Fi and
//! says so, never a broken card.

use crate::account::SeedFile;
use crate::{Progress, Stage};
use hub_core::hub_seed::{wifi_keyfile, WifiSeed};
use std::path::{Path, PathBuf};

/// The device node of partition 1 on a whole disk, per platform naming:
///
/// ```text
///   /dev/sdb      -> /dev/sdb1        /dev/mmcblk0 -> /dev/mmcblk0p1
///   /dev/nvme0n1  -> /dev/nvme0n1p1   /dev/disk4   -> /dev/disk4s1
/// ```
///
/// Families whose whole-disk name ends in a digit take a separator (`p` on
/// Linux, `s` on macOS); plain `sd`/`vd`/`hd` names just append the number.
pub fn boot_partition_path(device: &str) -> String {
    let name = device.strip_prefix("/dev/").unwrap_or(device);
    if name.starts_with("disk") {
        return format!("/dev/{name}s1");
    }
    if name.ends_with(|c: char| c.is_ascii_digit()) {
        return format!("/dev/{name}p1");
    }
    format!("/dev/{name}1")
}

/// Mint a random UUID (version 4, RFC 4122 variant) for the keyfile's
/// `uuid=` line. HA's docs warn that a boot-partition profile without a
/// stable UUID is re-imported with a fresh identity every boot — "the IP
/// address changes on every boot" — so the flasher mints one at flash
/// time and it rides the card forever. hub-core stays dependency-free; the
/// randomness (getrandom, same as the account minting) lives here.
pub fn uuid_v4() -> Result<String, String> {
    let mut b = [0u8; 16];
    getrandom::fill(&mut b)
        .map_err(|e| format!("couldn't gather randomness for the connection UUID: {e}"))?;
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant
    let hex: Vec<String> = b.iter().map(|x| format!("{x:02x}")).collect();
    let s = hex.concat();
    Ok(format!(
        "{}-{}-{}-{}-{}",
        &s[0..8],
        &s[8..12],
        &s[12..16],
        &s[16..20],
        &s[20..32]
    ))
}

/// The keyfile's filename stem: the connection id reduced to a safe,
/// FAT-friendly name. Never empty — falls back to "securacv-hub".
pub fn keyfile_stem(connection_id: &str) -> String {
    let stem: String = connection_id
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '-'
            }
        })
        .collect();
    let trimmed = stem.trim_matches('-').to_string();
    if trimmed.is_empty() {
        "securacv-hub".to_string()
    } else {
        trimmed
    }
}

/// Write the keyfile tree under an already-mounted boot partition root:
/// `<root>/CONFIG/network/<stem>`. Returns the path written. Pure file I/O —
/// tested against a temp dir standing in for the mount.
pub fn write_wifi_seed(mount_root: &Path, seed: &WifiSeed<'_>) -> Result<PathBuf, String> {
    let keyfile = wifi_keyfile(seed).map_err(|e| e.message())?;
    let dir = mount_root.join("CONFIG").join("network");
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("couldn't create CONFIG/network on the boot partition: {e}"))?;
    let path = dir.join(keyfile_stem(seed.connection_id));
    std::fs::write(&path, keyfile).map_err(|e| format!("couldn't write the Wi-Fi keyfile: {e}"))?;
    // Flush through to the card — the very next thing that happens to it is
    // physical removal.
    if let Ok(f) = std::fs::File::open(&path) {
        let _ = f.sync_all();
    }
    Ok(path)
}

/// Write the minted account `.storage` files under the boot partition's
/// `CONFIG/` tree (so `.storage/auth` becomes `CONFIG/.storage/auth`).
///
/// EXPERIMENTAL: whether HAOS imports these on first boot is exactly what the
/// hardware-validation runbook decides — this is the boot-partition (FAT,
/// writable from every OS) candidate. Files HAOS doesn't recognize are simply
/// ignored, so the worst case is a normal onboarding wizard, never a broken
/// boot. Never called unless the operator opts in.
pub fn write_account_seed(mount_root: &Path, files: &[SeedFile]) -> Result<Vec<PathBuf>, String> {
    let mut written = Vec::new();
    for f in files {
        // Guard against any path escaping CONFIG/ (the relative paths are ours,
        // but treat them as untrusted on principle).
        if f.relative_path.contains("..") || f.relative_path.starts_with('/') {
            return Err(format!("refusing an unsafe seed path: {}", f.relative_path));
        }
        let dest = mount_root.join("CONFIG").join(&f.relative_path);
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| format!("couldn't create {}: {e}", parent.display()))?;
        }
        std::fs::write(&dest, &f.contents)
            .map_err(|e| format!("couldn't write {}: {e}", dest.display()))?;
        written.push(dest);
    }
    Ok(written)
}

/// The result of seeding a freshly-written card: what made it on, and the
/// non-fatal notes for what didn't. The write already verified, so nothing
/// here is a failure — each note is context + a plan B.
#[derive(Debug, Default, Clone)]
pub struct SeedOutcome {
    pub wifi_written: bool,
    pub account_written: bool,
    /// Non-fatal note about the Wi-Fi seed, if it stumbled.
    pub wifi_note: Option<String>,
    /// Non-fatal note about the account seed, if requested and it stumbled.
    pub account_note: Option<String>,
    /// Set when the settings were written but the card wouldn't cleanly
    /// eject — the seed still succeeded, so this is "eject it yourself", not
    /// a failure. (A busy mount right after write is common on Linux.)
    pub eject_note: Option<String>,
}

/// Seed a freshly-written card in ONE mount: re-probe the partition table,
/// mount partition 1, write whatever was requested (Wi-Fi and/or the
/// experimental account store), then always unmount/eject. Mounting once
/// avoids racing the OS's auto-mounter twice.
///
/// A mount failure is the only hard error (nothing could be written); once
/// mounted, each item's failure becomes a note so a verified card is never
/// demoted to a failure over a seed hiccup.
pub fn seed_card(
    device: &str,
    wifi: Option<&WifiSeed<'_>>,
    account_files: &[SeedFile],
    mut progress: impl FnMut(Progress),
) -> Result<SeedOutcome, String> {
    progress(Progress {
        stage: Stage::Seed,
        done: 0,
        total: None,
    });
    let partition = boot_partition_path(device);
    let mount_root = mount_partition(&partition)?;

    let mut outcome = SeedOutcome::default();
    if let Some(seed) = wifi {
        match write_wifi_seed(&mount_root, seed) {
            Ok(_) => outcome.wifi_written = true,
            Err(e) => outcome.wifi_note = Some(e),
        }
    }
    if !account_files.is_empty() {
        match write_account_seed(&mount_root, account_files) {
            Ok(_) => outcome.account_written = true,
            Err(e) => outcome.account_note = Some(e),
        }
    }

    // Always try to unmount/eject — but the settings are already written, so an
    // eject stumble is a note, NOT a failure that discards the outcome (a mount
    // is often momentarily busy right after write+sync). Never demote a card
    // whose settings physically made it on.
    if let Err(e) = eject(device, &partition) {
        outcome.eject_note = Some(e);
    }
    progress(Progress {
        stage: Stage::Seed,
        done: 1,
        total: Some(1),
    });
    Ok(outcome)
}

// ── platform edges ──────────────────────────────────────────────────────────

#[cfg(target_os = "linux")]
fn mount_partition(partition: &str) -> Result<PathBuf, String> {
    // Give the kernel a beat to re-read the new partition table after the raw
    // write, then let udisks do a user-session mount (no root needed). The
    // first mount attempt right after a multi-GB write can still fail while
    // udev/udisks catch up, so retry a few times before giving up.
    wait_for_node(partition)?;
    let mut last = String::new();
    for attempt in 0..4u32 {
        if attempt > 0 {
            std::thread::sleep(std::time::Duration::from_millis(2000));
        }
        let out = std::process::Command::new("udisksctl")
            .args(["mount", "-b", partition])
            .output()
            .map_err(|e| format!("couldn't run udisksctl: {e}"))?;
        if out.status.success() {
            // "Mounted /dev/sdb1 at /media/user/hassos-boot" (a trailing
            // period on older udisks). Parse the mount point from udisks'
            // own answer.
            let text = String::from_utf8_lossy(&out.stdout);
            let at = text
                .split(" at ")
                .nth(1)
                .map(|s| s.trim().trim_end_matches('.').to_string())
                .ok_or_else(|| format!("couldn't read the mount point from: {}", text.trim()))?;
            return Ok(PathBuf::from(at));
        }
        let stderr = String::from_utf8_lossy(&out.stderr);
        // Already mounted (e.g. the desktop auto-mounter won the race) is a
        // success in disguise — udisks names the existing mount point.
        if let Some(at) = stderr.split(" already mounted at ").nth(1) {
            let at = at
                .trim()
                .trim_start_matches('`')
                .trim_end_matches('.')
                .trim_end_matches('\'')
                .trim_end_matches('`');
            if !at.is_empty() {
                return Ok(PathBuf::from(at));
            }
        }
        last = format!(
            "couldn't mount the boot partition {partition}: {}",
            stderr.trim()
        );
    }
    Err(last)
}

#[cfg(target_os = "linux")]
fn eject(_device: &str, partition: &str) -> Result<(), String> {
    let out = std::process::Command::new("udisksctl")
        .args(["unmount", "-b", partition])
        .output()
        .map_err(|e| format!("couldn't run udisksctl: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "the Wi-Fi seed is written, but unmounting {partition} failed: {} — eject it in your \
             file manager before removing the card",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn wait_for_node(partition: &str) -> Result<(), String> {
    for _ in 0..40 {
        if std::path::Path::new(partition).exists() {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(250));
    }
    Err(format!(
        "the boot partition {partition} never appeared after the write — unplug and replug the \
         card, then run the Wi-Fi seed step again"
    ))
}

#[cfg(target_os = "macos")]
fn mount_partition(partition: &str) -> Result<PathBuf, String> {
    // The raw write just replaced the whole partition table under macOS's
    // feet, and DiskArbitration takes a few seconds to re-probe before the
    // FAT boot volume becomes mountable. The very first `diskutil mount`
    // right after the write routinely fails with "Volume on diskXsY failed
    // to mount" — a timing hiccup, not a damaged card (this is exactly the
    // failure that used to drop the Wi-Fi seed). So: retry over ~20 s, and
    // between attempts nudge the OS with `diskutil mountDisk` on the whole
    // disk, which forces a re-probe of the fresh partition table.
    let whole_disk = whole_disk_of(partition);
    let mut last = String::new();
    for attempt in 0..8u32 {
        if attempt > 0 {
            std::thread::sleep(std::time::Duration::from_millis(2500));
            let _ = std::process::Command::new("diskutil")
                .args(["mountDisk", &whole_disk])
                .output();
        }
        match try_mount_macos(partition) {
            Ok(p) => return Ok(p),
            Err(e) => last = e,
        }
    }
    Err(format!(
        "{last} (tried for ~20 seconds; macOS wouldn't re-mount the freshly written card)"
    ))
}

/// `/dev/disk11s1` → `/dev/disk11` — the whole-disk node a partition sits on.
#[cfg(target_os = "macos")]
fn whole_disk_of(partition: &str) -> String {
    match partition.rfind('s') {
        Some(i) if i > 0 && partition[i + 1..].chars().all(|c| c.is_ascii_digit()) => {
            partition[..i].to_string()
        }
        _ => partition.to_string(),
    }
}

#[cfg(target_os = "macos")]
fn try_mount_macos(partition: &str) -> Result<PathBuf, String> {
    use hub_core::hub_enumerate_macos::{parse_plist, Plist};

    let mount = std::process::Command::new("diskutil")
        .args(["mount", partition])
        .output()
        .map_err(|e| format!("couldn't run diskutil: {e}"))?;
    // Even when `mount` reports failure, the volume may (already) be mounted —
    // e.g. the auto-mounter won the race — so trust the MountPoint diskutil
    // reports over the exit status alone.
    let info = std::process::Command::new("diskutil")
        .args(["info", "-plist", partition])
        .output()
        .map_err(|e| format!("couldn't run diskutil: {e}"))?;
    if let Ok(plist) = parse_plist(&String::from_utf8_lossy(&info.stdout)) {
        if let Some(mp) = plist
            .get("MountPoint")
            .and_then(Plist::as_str)
            .filter(|m| !m.is_empty())
        {
            return Ok(PathBuf::from(mp));
        }
    }
    if !mount.status.success() {
        return Err(format!(
            "couldn't mount the boot partition {partition}: {}",
            String::from_utf8_lossy(&mount.stderr).trim()
        ));
    }
    Err(format!(
        "{partition} mounted but macOS reports no mount point"
    ))
}

#[cfg(target_os = "macos")]
fn eject(device: &str, _partition: &str) -> Result<(), String> {
    let out = std::process::Command::new("diskutil")
        .args(["eject", device])
        .output()
        .map_err(|e| format!("couldn't run diskutil: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "the Wi-Fi seed is written, but ejecting {device} failed: {} — eject it in Finder \
             before removing the card",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(())
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn mount_partition(_partition: &str) -> Result<PathBuf, String> {
    Err("seeding the boot partition isn't implemented on this OS yet".to_string())
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn eject(_device: &str, _partition: &str) -> Result<(), String> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn boot_partition_paths_cover_every_family() {
        assert_eq!(boot_partition_path("/dev/sdb"), "/dev/sdb1");
        assert_eq!(boot_partition_path("/dev/mmcblk0"), "/dev/mmcblk0p1");
        assert_eq!(boot_partition_path("/dev/nvme0n1"), "/dev/nvme0n1p1");
        assert_eq!(boot_partition_path("/dev/disk4"), "/dev/disk4s1");
        assert_eq!(boot_partition_path("sdb"), "/dev/sdb1");
    }

    #[test]
    fn minted_uuids_are_well_formed_v4_and_unique() {
        let a = uuid_v4().expect("mints");
        let b = uuid_v4().expect("mints");
        for u in [&a, &b] {
            assert_eq!(u.len(), 36);
            let parts: Vec<&str> = u.split('-').collect();
            assert_eq!(
                parts.iter().map(|p| p.len()).collect::<Vec<_>>(),
                vec![8, 4, 4, 4, 12]
            );
            assert!(u.chars().all(|c| c == '-' || c.is_ascii_hexdigit()));
            assert!(parts[2].starts_with('4'), "version nibble in {u}");
            assert!(
                matches!(parts[3].chars().next(), Some('8' | '9' | 'a' | 'b')),
                "variant nibble in {u}"
            );
        }
        assert_ne!(a, b, "two mints must differ");
    }

    #[test]
    fn keyfile_stems_are_safe_and_never_empty() {
        assert_eq!(keyfile_stem("securacv-hub"), "securacv-hub");
        assert_eq!(keyfile_stem("My Home / Wi-Fi!"), "My-Home---Wi-Fi");
        assert_eq!(keyfile_stem("///"), "securacv-hub");
        assert_eq!(keyfile_stem(""), "securacv-hub");
    }

    #[test]
    fn the_seed_tree_lands_where_haos_reads_it() {
        let root = tempfile::tempdir().unwrap();
        let seed = WifiSeed {
            ssid: "Home Wi-Fi",
            passphrase: "correct horse battery",
            connection_id: "securacv-hub",
            uuid: None,
            hidden: false,
        };
        let path = write_wifi_seed(root.path(), &seed).expect("seed writes");
        assert_eq!(
            path,
            root.path()
                .join("CONFIG")
                .join("network")
                .join("securacv-hub")
        );
        let contents = std::fs::read_to_string(&path).unwrap();
        assert!(contents.contains("[802-11-wireless]"));
        assert!(contents.contains("psk=correct horse battery"));
    }

    #[test]
    fn an_invalid_wifi_seed_never_touches_the_tree() {
        let root = tempfile::tempdir().unwrap();
        let seed = WifiSeed {
            ssid: "Home",
            passphrase: "short", // < 8 chars: WPA-invalid
            connection_id: "securacv-hub",
            uuid: None,
            hidden: false,
        };
        assert!(write_wifi_seed(root.path(), &seed).is_err());
        assert!(!root.path().join("CONFIG").exists());
    }

    #[test]
    fn account_seed_lands_under_config_preserving_structure() {
        let root = tempfile::tempdir().unwrap();
        let files = vec![
            SeedFile {
                relative_path: ".storage/auth".to_string(),
                contents: "{\"auth\":true}".to_string(),
            },
            SeedFile {
                relative_path: ".storage/onboarding".to_string(),
                contents: "{\"done\":[]}".to_string(),
            },
        ];
        let written = write_account_seed(root.path(), &files).expect("writes");
        assert_eq!(written.len(), 2);
        let auth = root.path().join("CONFIG").join(".storage").join("auth");
        assert_eq!(std::fs::read_to_string(&auth).unwrap(), "{\"auth\":true}");
    }

    #[test]
    fn account_seed_refuses_path_traversal() {
        let root = tempfile::tempdir().unwrap();
        let files = vec![SeedFile {
            relative_path: "../escape".to_string(),
            contents: "x".to_string(),
        }];
        assert!(write_account_seed(root.path(), &files).is_err());
        // Nothing written on a rejected batch member.
        assert!(!root.path().join("CONFIG").join("escape").exists());
    }
}
