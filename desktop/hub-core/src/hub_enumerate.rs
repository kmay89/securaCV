//! hub_enumerate — turn the OS's raw view of its disks into classified hub
//! targets, safely.
//!
//! Step 2 of the native Pi hub writer (docs/design/raspberry_pi_hub_flashing.md).
//! The safety-critical judgement here is *which physical disk backs the running
//! OS* — so `hub_disk::classify` can refuse it — and *which non-removable disks
//! are external* — so an SSD/NVMe (the Pi 5 durable default) is still offerable.
//! Get the first wrong and we could offer the operator's own boot disk; get the
//! second wrong and the durable path disappears.
//!
//! So, exactly like `hub_disk`, all the *reasoning* is pure functions over
//! already-read system data (`/proc/mounts` and the `/sys/block` attributes),
//! host-tested without touching a disk. Only the thin `enumerate()` wrapper does
//! file I/O, and it hands every string it reads to these tested functions. It
//! never decides eligibility itself — it builds [`crate::hub_disk::TargetDisk`]s
//! and the caller runs them through `hub_disk::classify`.
//!
//! Conservative by construction: anything this layer cannot prove safe, it leans
//! toward *refused*. An external drive it can't confirm is external stays
//! internal (→ refused); a root filesystem on LVM/dm it can't map to a physical
//! disk simply isn't matched (and such a disk is non-removable anyway, so the
//! `hub_disk` gate still refuses it).

#[cfg(target_os = "linux")]
use crate::hub_disk::from_sysblock;
#[cfg(target_os = "linux")]
use crate::hub_disk::TargetDisk;

/// Mount points whose backing disk is "the machine you're using" — writing any
/// of them would be catastrophic, so a disk carrying one is flagged `system`.
const CRITICAL_MOUNTS: &[&str] = &["/", "/boot", "/boot/efi", "/boot/firmware"];

/// Map any block-device node name to its parent WHOLE disk.
///
/// ```text
///   sda1        -> sda        nvme0n1p2 -> nvme0n1
///   sda         -> sda        nvme0n1   -> nvme0n1
///   sdb15       -> sdb        mmcblk0p1 -> mmcblk0
/// ```
///
/// The partition separator differs by family: `nvme`/`mmcblk`/`loop` use a
/// `p<N>` suffix (and their whole-disk names legitimately end in digits, so we
/// must NOT strip trailing digits from them); `sd`/`vd`/`hd`/`xvd` use trailing
/// digits. A leading `/dev/` is tolerated.
pub fn parent_disk(dev: &str) -> String {
    let d = dev.strip_prefix("/dev/").unwrap_or(dev);
    if d.starts_with("nvme") || d.starts_with("mmcblk") || d.starts_with("loop") {
        // Partition form is "<disk>p<digits>". Only strip when a 'p' is followed
        // by all-digits to the end AND preceded by a digit (the disk's own
        // trailing digit) — so "nvme0n1" (no 'p') is left whole.
        if let Some(idx) = d.rfind('p') {
            let (head, tail) = (&d[..idx], &d[idx + 1..]);
            if !tail.is_empty()
                && tail.bytes().all(|b| b.is_ascii_digit())
                && head.bytes().last().is_some_and(|b| b.is_ascii_digit())
            {
                return head.to_string();
            }
        }
        return d.to_string();
    }
    // sd/vd/hd/xvd… — trailing digits are the partition number.
    let trimmed = d.trim_end_matches(|c: char| c.is_ascii_digit());
    if trimmed.is_empty() {
        d.to_string()
    } else {
        trimmed.to_string()
    }
}

/// Is this `/sys/block` entry a whole disk we should even consider as a target?
/// Accept the real storage families (`sd`, `vd`, `hd`, `xvd`, `nvme…n…`,
/// `mmcblk`); reject partitions and the pseudo/virtual devices (`loop`, `ram`,
/// `zram`, `dm-`, `md`, `sr`, `fd`, `nbd`).
pub fn is_candidate_disk(name: &str) -> bool {
    // A partition is never a whole disk.
    if parent_disk(name) != name {
        return false;
    }
    const REJECT: &[&str] = &["loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "dm"];
    if REJECT.iter().any(|p| name.starts_with(p)) {
        return false;
    }
    const ACCEPT: &[&str] = &["sd", "vd", "hd", "xvd", "nvme", "mmcblk"];
    ACCEPT.iter().any(|p| name.starts_with(p))
}

/// The set of whole disks that back a critical mount point, from `/proc/mounts`.
/// A root (or /boot) on LVM/dm or an overlay resolves to nothing here — safe,
/// because such a target is non-removable and the `hub_disk` gate refuses it
/// regardless. Deduplicated and sorted for a stable result.
pub fn system_disks(mounts: &str) -> Vec<String> {
    let mut set = Vec::new();
    for line in mounts.lines() {
        let mut f = line.split_whitespace();
        let (src, mnt) = match (f.next(), f.next()) {
            (Some(s), Some(m)) => (s, m),
            _ => continue,
        };
        if !CRITICAL_MOUNTS.contains(&mnt) {
            continue;
        }
        let dev = match src.strip_prefix("/dev/") {
            Some(d) => d,
            None => continue, // root on overlay/tmpfs/network — nothing physical to name
        };
        // /dev/mapper/* and /dev/dm-* can't be mapped to a physical disk from
        // the mount table alone; leave unmatched (still refused as non-removable).
        if dev.starts_with("mapper/") || dev.starts_with("dm-") {
            continue;
        }
        let disk = parent_disk(dev);
        if !set.contains(&disk) {
            set.push(disk);
        }
    }
    set.sort();
    set
}

/// Does any partition (or the whole device) of `disk` currently appear as a
/// mount source in `/proc/mounts`? Surfaced as an advisory ("this has data on
/// it"), not a refusal.
pub fn disk_has_mounts(mounts: &str, disk: &str) -> bool {
    for line in mounts.lines() {
        let Some(src) = line.split_whitespace().next() else {
            continue;
        };
        if let Some(dev) = src.strip_prefix("/dev/") {
            if parent_disk(dev) == disk {
                return true;
            }
        }
    }
    false
}

/// Conservative external check from a `/sys/block/<name>` symlink target (which
/// threads through the bus: `…/usb1/…/block/sdb` for a USB drive). Only USB /
/// USB-Attached-SCSI counts as external here; a Thunderbolt/PCIe external
/// enclosure looks internal and is therefore refused — the safe direction (the
/// operator can still use a card). Anything unproven is NOT external.
pub fn is_external(sys_path_hint: &str) -> bool {
    let h = sys_path_hint.to_ascii_lowercase();
    h.contains("/usb") || h.contains("usb/") || h.contains("/uas")
}

/// Enumerate the machine's block devices as hub-target candidates. Read-only:
/// it reads `/proc/mounts` and `/sys/block/*`, runs every value through the pure
/// functions above, and returns [`TargetDisk`]s for the caller to `classify`.
/// It makes no eligibility decision of its own.
#[cfg(target_os = "linux")]
pub fn enumerate() -> Result<Vec<TargetDisk>, String> {
    use std::fs;
    use std::path::Path;

    let mounts = fs::read_to_string("/proc/mounts")
        .map_err(|e| format!("couldn't read /proc/mounts: {e}"))?;
    let sys_disks = system_disks(&mounts);

    let block = Path::new("/sys/block");
    let entries = fs::read_dir(block).map_err(|e| format!("couldn't read /sys/block: {e}"))?;

    let mut out = Vec::new();
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !is_candidate_disk(&name) {
            continue;
        }
        let base = block.join(&name);
        // Missing attrs read as empty; from_sysblock turns that into a refused
        // (unknown-size / not-removable) disk rather than a silent guess.
        let removable = fs::read_to_string(base.join("removable")).unwrap_or_default();
        let size = fs::read_to_string(base.join("size")).unwrap_or_default();
        let model = fs::read_to_string(base.join("device/model")).unwrap_or_default();
        // The /sys/block/<name> entry is a symlink whose target names the bus.
        let hint = fs::read_link(&base)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();

        let external = is_external(&hint);
        let system = sys_disks.iter().any(|d| d == &name);
        let has_mounts = disk_has_mounts(&mounts, &name);

        out.push(from_sysblock(
            &name, &removable, &size, &model, external, system, has_mounts,
        ));
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

/// Enumeration for platforms whose backend isn't written yet. Honest rather than
/// empty: an empty list would read as "no disks found", inviting the UI to say
/// the wrong thing. macOS (`diskutil list -plist` + Internal/Ejectable) and
/// Windows are follow-ups.
#[cfg(not(target_os = "linux"))]
pub fn enumerate() -> Result<Vec<crate::hub_disk::TargetDisk>, String> {
    Err("hub-disk enumeration isn't implemented on this OS yet".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hub_disk::{classify, Eligibility, Refusal};

    #[test]
    fn parent_disk_strips_sd_partition_numbers() {
        assert_eq!(parent_disk("sda1"), "sda");
        assert_eq!(parent_disk("sda"), "sda");
        assert_eq!(parent_disk("sdb15"), "sdb");
        assert_eq!(parent_disk("/dev/sdc2"), "sdc");
        assert_eq!(parent_disk("vda3"), "vda");
    }

    #[test]
    fn parent_disk_handles_nvme_and_mmcblk_without_eating_their_digits() {
        assert_eq!(parent_disk("nvme0n1"), "nvme0n1"); // whole disk ends in a digit
        assert_eq!(parent_disk("nvme0n1p2"), "nvme0n1");
        assert_eq!(parent_disk("/dev/nvme1n1p13"), "nvme1n1");
        assert_eq!(parent_disk("mmcblk0"), "mmcblk0");
        assert_eq!(parent_disk("mmcblk0p1"), "mmcblk0");
    }

    #[test]
    fn is_candidate_disk_accepts_whole_disks_only() {
        for ok in ["sda", "sdb", "vda", "nvme0n1", "mmcblk0", "hda"] {
            assert!(is_candidate_disk(ok), "{ok} should be a candidate");
        }
        for no in [
            "sda1",
            "nvme0n1p1",
            "mmcblk0p2", // partitions
            "loop0",
            "ram0",
            "zram0",
            "dm-0",
            "md0",
            "sr0",
            "fd0",
            "nbd0", // pseudo/virtual
        ] {
            assert!(!is_candidate_disk(no), "{no} should NOT be a candidate");
        }
    }

    #[test]
    fn system_disks_finds_the_root_backing_disk() {
        let mounts = "\
sysfs /sys sysfs rw 0 0
/dev/nvme0n1p2 / ext4 rw,relatime 0 0
/dev/nvme0n1p1 /boot/efi vfat rw 0 0
/dev/sdb1 /mnt/usb ext4 rw 0 0
tmpfs /run tmpfs rw 0 0
";
        // Root and /boot/efi are both on nvme0n1 → the one system disk.
        assert_eq!(system_disks(mounts), vec!["nvme0n1".to_string()]);
    }

    #[test]
    fn system_disks_covers_a_separate_boot_disk() {
        let mounts = "\
/dev/sda2 / ext4 rw 0 0
/dev/mmcblk0p1 /boot firmware rw 0 0
";
        assert_eq!(
            system_disks(mounts),
            vec!["mmcblk0".to_string(), "sda".to_string()]
        );
    }

    #[test]
    fn system_disks_is_empty_for_lvm_or_overlay_root() {
        // Can't map dm/mapper/overlay root to a physical disk — return nothing
        // (safe: such a disk is non-removable and refused anyway).
        assert!(system_disks("/dev/mapper/vg-root / ext4 rw 0 0\n").is_empty());
        assert!(system_disks("/dev/dm-0 / ext4 rw 0 0\n").is_empty());
        assert!(system_disks("overlay / overlay rw 0 0\n").is_empty());
    }

    #[test]
    fn disk_has_mounts_detects_a_mounted_partition() {
        let mounts = "/dev/sdb1 /media/pi/CARD vfat rw 0 0\n";
        assert!(disk_has_mounts(mounts, "sdb"));
        assert!(!disk_has_mounts(mounts, "sdc"));
    }

    #[test]
    fn is_external_recognises_usb_only() {
        assert!(is_external(
            "../devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host4/target4:0:0/4:0:0:0/block/sdb"
        ));
        assert!(is_external("../devices/platform/usb/…/block/sdc"));
        // internal SATA / NVMe are not external
        assert!(!is_external(
            "../devices/pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0/block/sda"
        ));
        assert!(!is_external(
            "../devices/pci0000:00/0000:00:1c.4/0000:04:00.0/nvme/nvme0/nvme0n1"
        ));
        assert!(!is_external(""));
    }

    // The payoff: the pure enumeration facts feed hub_disk::classify to the
    // right verdicts — the two modules compose exactly as the writer will use them.

    #[test]
    fn a_usb_card_becomes_an_eligible_target() {
        let mounts = "/dev/nvme0n1p2 / ext4 rw 0 0\n";
        let name = "sdb";
        let system = system_disks(mounts).iter().any(|d| d == name);
        let external = is_external(".../usb1/.../block/sdb");
        // 64 GB removable USB stick, nothing mounted.
        let d = from_sysblock(name, "1", "125000000", "USB DISK", external, system, false);
        assert!(classify(&d).is_eligible());
    }

    #[test]
    fn the_root_nvme_is_refused_as_the_system_disk() {
        let mounts = "/dev/nvme0n1p2 / ext4 rw 0 0\n";
        let name = "nvme0n1";
        let system = system_disks(mounts).iter().any(|d| d == name);
        assert!(system, "root's nvme must be flagged system");
        let d = from_sysblock(name, "0", "1000215216", "INTERNAL SSD", false, system, true);
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn an_external_nvme_that_is_not_root_is_eligible() {
        let mounts = "/dev/nvme0n1p2 / ext4 rw 0 0\n"; // internal boot disk is nvme0n1
        let name = "nvme1n1"; // a SECOND nvme, over USB
        let system = system_disks(mounts).iter().any(|d| d == name);
        assert!(!system);
        let external = is_external(".../usb4/.../nvme/nvme1/nvme1n1");
        let d = from_sysblock(name, "0", "500000000", "USB NVMe", external, system, false);
        assert!(classify(&d).is_eligible());
    }
}
