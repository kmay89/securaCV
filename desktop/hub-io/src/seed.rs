//! seed — put the "it joins your Wi-Fi by itself" file into the *image*, before
//! the card is written.
//!
//! `hub_core::hub_seed::wifi_keyfile` builds the NetworkManager keyfile and
//! `hub_core::hub_fat` knows how to add a file to the FAT boot filesystem inside
//! a disk image. This module is the thin layer between them and the decompressed
//! `.img` on disk: open it for random access, find the boot partition, put the
//! settings in, and prove they read back.
//!
//! **This used to work the other way round** — write the card, then ask the
//! operating system to re-mount its boot partition and drop the keyfile onto it.
//! That step failed on a real operator's Mac every time, including on retry and
//! after physically reseating the card:
//!
//! ```text
//! couldn't mount the boot partition /dev/disk4s1: Volume on disk4s1 failed to mount
//! (tried for ~20 seconds; macOS wouldn't re-mount the freshly written card)
//! ```
//!
//! and the result was a hub that boots to `wlan0: (No address)` and sits on the
//! Home Assistant landing page forever, because Core downloads itself on first
//! boot and there was no network to download over. For a headless hub the seed
//! is not a convenience, it is the product.
//!
//! Moving it before the write buys four things, all of which the mount path
//! could not have:
//!
//!   * **No mount, on any OS.** No `diskutil`, no `udisksctl`, no drive letters,
//!     no DiskArbitration timing. The single most platform-specific code in the
//!     crate is gone rather than made more patient.
//!   * **The seed inherits the write's verification.** `write_verified` already
//!     reads the whole card back and re-hashes it; once the keyfile is *in* the
//!     image, proving the image proves the keyfile. No separate read-back that
//!     could be answered from the page cache.
//!   * **Failures are free.** A seed that can't be built or placed is caught in
//!     milliseconds, before a single byte is written, instead of after a
//!     multi-GB write and a 20-second mount timeout.
//!   * **It is testable without hardware.** See `hub-core`'s `hub_fat` tests and
//!     `tests/fat_against_dosfstools.rs`, where `fsck.fat` and `mtools` audit the
//!     result. The mount path could only ever be tested by a human with a card
//!     reader, which is why it shipped broken.
//!
//! What remains platform-specific here is [`eject_card`] — unmounting and
//! powering down the card so it is safe to pull. That is a courtesy after a
//! successful flash, not a step anything depends on: the settings were part of
//! the image, so nothing is pending on the card by the time it is offered.

use crate::account::SeedFile;
use crate::{sha256_hex, CancelToken, Progress, Stage};
use hub_core::hub_fat::{self, BlockIo};
use hub_core::hub_seed::{wifi_keyfile, WifiSeed};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// The device node of partition 1 on a whole disk, per platform naming:
///
/// ```text
///   /dev/sdb      -> /dev/sdb1        /dev/mmcblk0 -> /dev/mmcblk0p1
///   /dev/nvme0n1  -> /dev/nvme0n1p1   /dev/disk4   -> /dev/disk4s1
/// ```
///
/// Families whose whole-disk name ends in a digit take a separator (`p` on
/// Linux, `s` on macOS); plain `sd`/`vd`/`hd` names just append the number.
/// Only [`eject_card`] needs this now.
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

/// The keyfile's filename stem: the connection id reduced to a safe name.
/// Never empty — falls back to "securacv-hub".
///
/// The FAT writer can store any name a long-name entry can carry, so this is no
/// longer about what the filesystem will accept; it is about the file NetworkManager
/// is asked to parse, and keeping it boring keeps it debuggable over a serial
/// console.
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

/// Random access to the decompressed image on disk.
///
/// Seek-based on purpose: the image is ~2.5 GB and the pipeline streams it to a
/// temp file precisely so it never has to fit in memory. Injection touches a few
/// kilobytes scattered through it, so it reads and writes those and nothing
/// else.
struct ImageFile(std::fs::File);

impl BlockIo for ImageFile {
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> std::io::Result<()> {
        self.0.seek(SeekFrom::Start(offset))?;
        self.0.read_exact(buf)
    }
    fn write_at(&mut self, offset: u64, buf: &[u8]) -> std::io::Result<()> {
        self.0.seek(SeekFrom::Start(offset))?;
        self.0.write_all(buf)
    }
}

/// What was put into the image, for the log and the receipt.
#[derive(Debug, Default, Clone)]
pub struct SeedReport {
    pub wifi_written: bool,
    pub account_written: bool,
    /// Why the (experimental, opt-in) account seed didn't go in, if it didn't.
    /// Non-fatal by design — see [`inject_into_image`].
    pub account_note: Option<String>,
    /// Where the settings went, e.g. `GPT partition 1 "hassos-boot" (FAT16)` —
    /// worth logging, because "which partition" is the first question when a
    /// future HAOS layout change breaks this.
    pub volume: String,
}

/// Put the requested settings into the boot filesystem inside `image`.
///
/// **Wi-Fi failing is fatal.** Nothing has been written to any card yet, so
/// there is no "the card is fine, the settings aren't" middle ground worth
/// preserving — that ambiguity is exactly what the old mount path produced. A
/// hub whose Wi-Fi didn't land is not a hub, and saying so now costs the
/// operator seconds instead of a four-minute write and a card they can't use.
///
/// **The account seed failing is not.** It is opt-in and experimental, and Home
/// Assistant's own setup wizard is a perfectly good fallback, so it comes back
/// as [`SeedReport::account_note`] rather than sinking a flash that would
/// otherwise work. On the first failure the rest of the batch is skipped: half a
/// provisioning bundle is worse than none.
pub fn inject_into_image(
    image: &Path,
    wifi: Option<&WifiSeed<'_>>,
    account_files: &[SeedFile],
    mut progress: impl FnMut(Progress),
) -> Result<SeedReport, String> {
    progress(Progress {
        stage: Stage::Seed,
        done: 0,
        total: Some(1),
    });

    // Build the keyfile FIRST, so a bad SSID or a too-short passphrase is
    // reported without having opened anything.
    let keyfile = match wifi {
        Some(seed) => Some((
            keyfile_stem(seed.connection_id),
            wifi_keyfile(seed).map_err(|e| e.message())?,
        )),
        None => None,
    };

    let file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(image)
        .map_err(|e| format!("couldn't open the prepared image to add your settings: {e}"))?;
    let len = file
        .metadata()
        .map_err(|e| format!("couldn't measure the prepared image: {e}"))?
        .len();
    let mut io = ImageFile(file);

    let (part, vol) = hub_fat::find_fat_partition(&mut io, len).map_err(|e| e.message())?;
    let volume = format!(
        "{} partition {}{} ({:?}, {} clusters of {} bytes)",
        part.scheme,
        part.number,
        part.name
            .as_deref()
            .map(|n| format!(" {n:?}"))
            .unwrap_or_default(),
        vol.kind,
        vol.cluster_count,
        vol.cluster_bytes()
    );

    let mut report = SeedReport {
        volume,
        ..Default::default()
    };

    if let Some((stem, text)) = &keyfile {
        let path = ["CONFIG", "network", stem.as_str()];
        hub_fat::insert_file(&mut io, &vol, &path, text.as_bytes()).map_err(|e| e.message())?;
        // Read it straight back out of the image. This is not a durability
        // claim — the bytes are still in the page cache and the card hasn't been
        // touched. What it proves is that the directory entries and cluster
        // chain we just wrote actually resolve to this file's bytes, which is
        // the failure a hand-rolled FAT writer would plausibly have. Durability
        // is the write's own read-back, which now covers the keyfile because the
        // keyfile is part of the image.
        let back = hub_fat::read_file(&mut io, &vol, &path).map_err(|e| e.message())?;
        if back != text.as_bytes() {
            return Err(format!(
                "the Wi-Fi settings went into the image but read back as {} bytes instead of {} \
                 — the prepared image looks damaged, so nothing was written to your card",
                back.len(),
                text.len()
            ));
        }
        report.wifi_written = true;
    }

    for f in account_files {
        // The relative paths are ours, but treat them as untrusted on principle:
        // a `..` here would place a file outside CONFIG/ on the boot volume.
        if f.relative_path.contains("..") || f.relative_path.starts_with('/') {
            report.account_note =
                Some(format!("refusing an unsafe seed path: {}", f.relative_path));
            break;
        }
        let mut path = vec!["CONFIG"];
        path.extend(f.relative_path.split('/').filter(|s| !s.is_empty()));
        if let Err(e) = hub_fat::insert_file(&mut io, &vol, &path, f.contents.as_bytes()) {
            report.account_note = Some(format!("{}: {}", f.relative_path, e.message()));
            break;
        }
        report.account_written = true;
    }
    if report.account_note.is_some() {
        // All-or-nothing: a partly-written bundle would have the runner find
        // its plan and not its executor, which is a worse failure to diagnose
        // than simply not being there.
        report.account_written = false;
    }

    io.0.sync_all().map_err(|e| {
        format!("couldn't flush the prepared image after adding your settings: {e}")
    })?;

    progress(Progress {
        stage: Stage::Seed,
        done: 1,
        total: Some(1),
    });
    Ok(report)
}

/// Re-hash the image after injection.
///
/// The post-write read-back compares the card against a hash of the image, so
/// once the image changes that hash has to change with it — otherwise every
/// seeded flash would "fail" verification. One extra streaming pass over the
/// temp file, and only on flashes that actually carry settings.
pub fn rehash_image(
    image: &Path,
    cancel: &CancelToken,
    mut progress: impl FnMut(Progress),
) -> Result<String, String> {
    use sha2::Digest;
    let mut file = std::fs::File::open(image)
        .map_err(|e| format!("couldn't reopen the prepared image to re-check it: {e}"))?;
    let total = file.metadata().ok().map(|m| m.len());
    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 4 * 1024 * 1024];
    let mut done: u64 = 0;
    loop {
        cancel.checkpoint()?;
        let n = file
            .read(&mut buf)
            .map_err(|e| format!("couldn't read the prepared image: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        done += n as u64;
        progress(Progress {
            stage: Stage::Seed,
            done,
            total,
        });
    }
    Ok(sha256_hex(hasher))
}

// ── platform edge: offering a clean unmount ─────────────────────────────────
//
// The only OS-specific code left. Nothing depends on it — the settings are on
// the card the moment the write verifies — but a card the OS has auto-mounted
// should be ejected before it is pulled, and saying so is friendlier than
// letting the operator find out from a filesystem warning.

/// Eject the card. `Err` is advice, not failure: callers surface it as a note.
pub fn eject_card(device: &str) -> Result<(), String> {
    eject(device, &boot_partition_path(device))
}

#[cfg(target_os = "linux")]
fn eject(device: &str, partition: &str) -> Result<(), String> {
    // Unmount first, then power the device down. `power-off` is the real "safe
    // to remove" — it detaches the device rather than just releasing a mount —
    // but it refuses while any partition is mounted, and a desktop
    // auto-mounter often grabs a freshly written card. The unmount is therefore
    // best-effort: "Not mounted" is the expected answer now that this code
    // never mounts anything itself.
    let _ = std::process::Command::new("udisksctl")
        .args(["unmount", "-b", partition])
        .output();
    let out = std::process::Command::new("udisksctl")
        .args(["power-off", "-b", device])
        .output()
        .map_err(|e| format!("couldn't run udisksctl: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "powering down {device} failed: {} — eject it in your file manager before removing \
             the card",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn eject(device: &str, _partition: &str) -> Result<(), String> {
    let out = std::process::Command::new("diskutil")
        .args(["eject", device])
        .output()
        .map_err(|e| format!("couldn't run diskutil: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "ejecting {device} failed: {} — eject it in Finder before removing the card",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(())
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn eject(_device: &str, _partition: &str) -> Result<(), String> {
    Ok(())
}

#[cfg(test)]
pub(crate) mod tests {
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

    /// A stand-in for the decompressed HAOS image: a GPT holding one FAT16
    /// volume with the geometry `haos_rpi5-64-18.1.img` really has.
    pub(crate) fn fake_image(path: &Path) {
        let mut vol = vec![0u8; 131_072 * 512];
        {
            let b = &mut vol[..512];
            b[0..3].copy_from_slice(&[0xeb, 0x3c, 0x90]);
            b[3..11].copy_from_slice(b"MSWIN4.1");
            b[0x0b..0x0d].copy_from_slice(&512u16.to_le_bytes());
            b[0x0d] = 4;
            b[0x0e..0x10].copy_from_slice(&4u16.to_le_bytes());
            b[0x10] = 2;
            b[0x11..0x13].copy_from_slice(&512u16.to_le_bytes());
            b[0x15] = 0xf8;
            b[0x16..0x18].copy_from_slice(&128u16.to_le_bytes());
            b[0x20..0x24].copy_from_slice(&131_072u32.to_le_bytes());
            b[0x26] = 0x29;
            b[0x2b..0x36].copy_from_slice(b"hassos-boot");
            b[0x36..0x3e].copy_from_slice(b"FAT16   ");
            b[510] = 0x55;
            b[511] = 0xaa;
        }
        for f in 0..2u32 {
            let at = (4 + f * 128) as usize * 512;
            vol[at..at + 2].copy_from_slice(&0xfff8u16.to_le_bytes());
            vol[at + 2..at + 4].copy_from_slice(&0xffffu16.to_le_bytes());
        }

        let first_lba = 2048usize;
        let mut img = vec![0u8; (first_lba + vol.len() / 512 + 34) * 512];
        img[446 + 4] = 0xee; // protective MBR
        img[510] = 0x55;
        img[511] = 0xaa;
        img[512..520].copy_from_slice(b"EFI PART");
        img[512 + 72..512 + 80].copy_from_slice(&2u64.to_le_bytes());
        img[512 + 80..512 + 84].copy_from_slice(&128u32.to_le_bytes());
        img[512 + 84..512 + 88].copy_from_slice(&128u32.to_le_bytes());
        let at = 1024;
        img[at..at + 16].copy_from_slice(&[0x0b; 16]);
        img[at + 32..at + 40].copy_from_slice(&(first_lba as u64).to_le_bytes());
        img[at + 40..at + 48]
            .copy_from_slice(&((first_lba + vol.len() / 512 - 1) as u64).to_le_bytes());
        for (i, u) in "hassos-boot".encode_utf16().enumerate() {
            img[at + 56 + i * 2..at + 58 + i * 2].copy_from_slice(&u.to_le_bytes());
        }
        img[first_lba * 512..first_lba * 512 + vol.len()].copy_from_slice(&vol);
        std::fs::write(path, &img).unwrap();
    }

    fn seed<'a>(ssid: &'a str, psk: &'a str) -> WifiSeed<'a> {
        WifiSeed {
            ssid,
            passphrase: psk,
            connection_id: "securacv-hub",
            uuid: None,
            hidden: false,
        }
    }

    #[test]
    fn the_seed_lands_in_the_image_where_haos_reads_it() {
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);

        let report = inject_into_image(
            &img,
            Some(&seed("Home Wi-Fi", "correct horse")),
            &[],
            |_| {},
        )
        .expect("injection");
        assert!(report.wifi_written);
        assert!(report.volume.contains("hassos-boot"), "{}", report.volume);
        assert!(report.volume.contains("Fat16"), "{}", report.volume);

        // Read it back through a fresh handle, the way the card's filesystem
        // driver will.
        let mut bytes = std::fs::read(&img).unwrap();
        let len = bytes.len() as u64;
        let (_p, vol) = hub_fat::find_fat_partition(&mut bytes, len).unwrap();
        let text = String::from_utf8(
            hub_fat::read_file(&mut bytes, &vol, &["CONFIG", "network", "securacv-hub"]).unwrap(),
        )
        .unwrap();
        assert!(text.contains("[802-11-wireless]"));
        assert!(text.contains("psk=correct horse"));
        assert!(text.contains("mdns=2"));
        assert!(!text.contains('\r'), "HAOS ignores CRLF keyfiles");
    }

    #[test]
    fn an_invalid_wifi_seed_is_refused_before_the_image_is_opened() {
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);
        let before = std::fs::read(&img).unwrap();

        let err = inject_into_image(&img, Some(&seed("Home", "short")), &[], |_| {})
            .expect_err("a WPA-invalid passphrase must be refused");
        assert!(err.contains("at least 8"), "{err}");
        assert_eq!(
            std::fs::read(&img).unwrap(),
            before,
            "the image was modified"
        );
    }

    #[test]
    fn the_account_seed_lands_under_config_preserving_structure() {
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);
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
        let report = inject_into_image(&img, None, &files, |_| {}).expect("injection");
        assert!(report.account_written);

        let mut bytes = std::fs::read(&img).unwrap();
        let len = bytes.len() as u64;
        let (_p, vol) = hub_fat::find_fat_partition(&mut bytes, len).unwrap();
        assert_eq!(
            hub_fat::read_file(&mut bytes, &vol, &["CONFIG", ".storage", "auth"]).unwrap(),
            b"{\"auth\":true}"
        );
        assert_eq!(
            hub_fat::read_file(&mut bytes, &vol, &["CONFIG", ".storage", "onboarding"]).unwrap(),
            b"{\"done\":[]}"
        );
    }

    #[test]
    fn the_account_seed_refuses_path_traversal() {
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);
        let files = vec![SeedFile {
            relative_path: "../escape".to_string(),
            contents: "x".to_string(),
        }];
        let report = inject_into_image(&img, None, &files, |_| {}).expect("not fatal");
        assert!(!report.account_written);
        assert!(report.account_note.unwrap().contains("unsafe seed path"));
    }

    #[test]
    fn a_failed_account_seed_is_a_note_but_a_failed_wifi_seed_is_fatal() {
        // The account pre-seed is opt-in and experimental — HA's own wizard is
        // the fallback — so it must never sink a flash that would otherwise
        // work. Wi-Fi is the opposite: without it a headless hub is unreachable,
        // so it fails the flash before the card is touched.
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);

        let files = vec![SeedFile {
            relative_path: "/absolute".to_string(),
            contents: "x".to_string(),
        }];
        let report = inject_into_image(&img, Some(&seed("Home", "supersecret")), &files, |_| {})
            .expect("a bad account seed must not fail the flash");
        assert!(report.wifi_written, "the Wi-Fi still went in");
        assert!(!report.account_written);
        assert!(report.account_note.is_some());

        // Whereas a Wi-Fi seed that can't be built stops everything.
        assert!(inject_into_image(&img, Some(&seed("", "supersecret")), &[], |_| {}).is_err());
    }

    #[test]
    fn an_image_with_no_boot_filesystem_fails_before_anything_is_written() {
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("not-an-image.img");
        std::fs::write(&img, vec![0u8; 4 << 20]).unwrap();
        let err = inject_into_image(&img, Some(&seed("Home", "supersecret")), &[], |_| {})
            .expect_err("a bogus image must be refused");
        assert!(err.contains("partition table"), "{err}");
    }

    #[test]
    fn rehashing_reflects_the_injected_bytes() {
        // The post-write read-back compares the card against this hash, so it
        // has to move when the image does — otherwise every seeded flash would
        // report a verification failure on a perfectly good card.
        let dir = tempfile::tempdir().unwrap();
        let img = dir.path().join("haos.img");
        fake_image(&img);
        let cancel = CancelToken::default();
        let before = rehash_image(&img, &cancel, |_| {}).unwrap();
        inject_into_image(&img, Some(&seed("Home", "supersecret")), &[], |_| {}).unwrap();
        let after = rehash_image(&img, &cancel, |_| {}).unwrap();
        assert_ne!(before, after, "the hash must track the injected bytes");
        assert_eq!(after.len(), 64);
    }
}
