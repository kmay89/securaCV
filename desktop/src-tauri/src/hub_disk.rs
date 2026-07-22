//! hub_disk — which removable disk may we write a Home Assistant hub image to?
//!
//! Writing an OS image is the one place this app can do real harm. Flashing a
//! Canary is safe by construction — the ESP32's first-stage bootloader is mask
//! ROM, so you cannot pick the wrong image and you cannot brick the board. A
//! Raspberry Pi hub is the opposite: it is a whole-OS image written to a raw
//! block device, and writing it to the *wrong* device destroys whatever was
//! there — including, if we are careless, the disk the operator's own computer
//! boots from.
//!
//! So we borrow the firmware's discipline. Just as the boot policy lands as a
//! pure, host-tested decision layer *before* the risky boot-path wiring
//! (`firmware/common/health/boot_policy.h`), the decision of *what is a legal
//! write target* lives here — pure, exhaustively unit-tested, and settled
//! before a single byte is ever written. The destructive write itself is a
//! separate, hardware-validated step (a later change); its one hard contract is
//! that it MUST call [`classify`] and refuse anything that is not
//! [`Eligibility::Eligible`].
//!
//! This module has no Tauri, no serial, no I/O in its decision path: it reasons
//! over already-observed facts about a disk. Enumerating the disks (and, on each
//! platform, deciding which one backs the running OS) is the enumerator's job in
//! a follow-up change; [`from_sysblock`] is the pure half of that, kept here so
//! it can be tested the same way.

/// 1024³ — disk sizes are quoted in binary gibibytes throughout this module.
const GIB: u64 = 1024 * 1024 * 1024;

/// Hard floor: below this, a device is too small to hold a Home Assistant OS
/// image and is almost certainly not the SD card / USB stick the operator
/// means (a phantom reader slot, a tiny EFI helper partition surfaced as a
/// "disk", etc.). HAOS itself asks for 8 GiB+; we refuse well below that rather
/// than gamble.
pub const MIN_TARGET_BYTES: u64 = 3 * GIB;

/// Below this we still allow the write but warn: it fits, but it is smaller
/// than Home Assistant's recommended card and will have little headroom.
pub const RECOMMENDED_TARGET_BYTES: u64 = 8 * GIB;

/// Above this we still allow the write but warn loudly: a drive this large is
/// far more likely to be an external SSD or a backup disk than the SD/USB the
/// operator intends — exactly the "am I about to nuke my photo archive?" case.
pub const TYPICAL_CEILING_BYTES: u64 = 256 * GIB;

/// A block device as the enumerator observed it. Every field is a plain
/// observation; turning observations into a safe/unsafe verdict is [`classify`]'s
/// job, never the caller's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TargetDisk {
    /// OS device path, e.g. `/dev/sdb` (Linux), `/dev/disk4` (macOS),
    /// `\\.\PhysicalDrive2` (Windows).
    pub path: String,
    /// Friendly label for the confirmation UI, e.g. `SanDisk Extreme 64GB`.
    pub model: String,
    /// Total capacity in bytes. `0` means the OS would not tell us — which we
    /// treat as disqualifying, never as "small".
    pub size_bytes: u64,
    /// The OS marks this device as removable (an SD reader or USB stick). A
    /// fixed internal disk is `false`.
    pub removable: bool,
    /// This device backs the running operating system (its root / boot lives
    /// here). Computed by the platform enumerator; writing it would destroy the
    /// very machine the operator is using.
    pub system: bool,
    /// The device currently has one or more mounted filesystems. Not fatal (a
    /// fresh card is often auto-mounted), but surfaced so the UI can say "this
    /// has data on it that will be erased".
    pub has_mounts: bool,
}

/// Why a disk may not be offered as a write target. Each carries enough to write
/// a plain-language line for the UI — a refusal the operator can understand, not
/// a silent omission.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// It backs the running OS. The strongest refusal; checked first.
    SystemDisk,
    /// It is a fixed / internal disk, not a removable card or stick.
    NotRemovable,
    /// The OS reported an unknown (zero) size; we will not write blind.
    UnknownSize,
    /// It is smaller than a hub image needs.
    TooSmall { need_bytes: u64, have_bytes: u64 },
}

impl Refusal {
    /// A one-line, human explanation suitable for the picker's "hidden because…"
    /// affordance.
    pub fn reason(&self) -> String {
        match self {
            Refusal::SystemDisk => {
                "this is the disk your computer runs from — never a write target".to_string()
            }
            Refusal::NotRemovable => {
                "this looks like a fixed internal disk, not a removable card or USB stick"
                    .to_string()
            }
            Refusal::UnknownSize => {
                "the system wouldn't report this device's size, so we won't write to it".to_string()
            }
            Refusal::TooSmall {
                need_bytes,
                have_bytes,
            } => format!(
                "too small for a Home Assistant image — needs at least {}, this is {}",
                fmt_bytes(*need_bytes),
                fmt_bytes(*have_bytes)
            ),
        }
    }
}

/// A non-fatal advisory shown next to an eligible target. Eligible plus a warning
/// still means "we will write here" — the warning is context for the confirm
/// step, not a veto.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Warning {
    /// Fits, but below the recommended card size.
    SmallerThanRecommended { have_bytes: u64, recommend_bytes: u64 },
    /// Far larger than a typical card — likely a real drive; double-check.
    LargerThanTypical { have_bytes: u64, ceiling_bytes: u64 },
    /// The device currently has mounted filesystems that the write will erase.
    HasMountedData,
}

impl Warning {
    /// A one-line, human explanation for the confirm step.
    pub fn message(&self) -> String {
        match self {
            Warning::SmallerThanRecommended {
                have_bytes,
                recommend_bytes,
            } => format!(
                "smaller than recommended ({}, vs {} suggested) — it will work but with little headroom",
                fmt_bytes(*have_bytes),
                fmt_bytes(*recommend_bytes)
            ),
            Warning::LargerThanTypical {
                have_bytes,
                ceiling_bytes,
            } => format!(
                "this is {} — larger than a typical SD/USB ({}+). Make sure it isn't a drive you want to keep",
                fmt_bytes(*have_bytes),
                fmt_bytes(*ceiling_bytes)
            ),
            Warning::HasMountedData => {
                "this device has data on it right now — everything on it will be erased".to_string()
            }
        }
    }
}

/// The verdict for one disk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Eligibility {
    /// Safe to offer. `warnings` may still hold advisories for the confirm step.
    Eligible { warnings: Vec<Warning> },
    /// Must never be offered. `0` is the reason.
    Refused(Refusal),
}

impl Eligibility {
    pub fn is_eligible(&self) -> bool {
        matches!(self, Eligibility::Eligible { .. })
    }
}

/// The single safety gate. Given the observed facts about a disk, decide whether
/// it may be written. Order matters: the most dangerous condition wins the
/// explanation, so a device that is both the system disk *and* (somehow) marked
/// removable is still refused as [`Refusal::SystemDisk`].
///
/// The refusals are deliberately belt-and-suspenders: `system` is refused
/// outright, but even if a platform's system-disk detection ever failed, a
/// normal internal disk is also `!removable` and would still be refused. A
/// target has to be removable, not the system disk, of known size, and big
/// enough — all four — to be written.
pub fn classify(disk: &TargetDisk) -> Eligibility {
    if disk.system {
        return Eligibility::Refused(Refusal::SystemDisk);
    }
    if !disk.removable {
        return Eligibility::Refused(Refusal::NotRemovable);
    }
    if disk.size_bytes == 0 {
        return Eligibility::Refused(Refusal::UnknownSize);
    }
    if disk.size_bytes < MIN_TARGET_BYTES {
        return Eligibility::Refused(Refusal::TooSmall {
            need_bytes: MIN_TARGET_BYTES,
            have_bytes: disk.size_bytes,
        });
    }

    let mut warnings = Vec::new();
    if disk.size_bytes < RECOMMENDED_TARGET_BYTES {
        warnings.push(Warning::SmallerThanRecommended {
            have_bytes: disk.size_bytes,
            recommend_bytes: RECOMMENDED_TARGET_BYTES,
        });
    }
    if disk.size_bytes > TYPICAL_CEILING_BYTES {
        warnings.push(Warning::LargerThanTypical {
            have_bytes: disk.size_bytes,
            ceiling_bytes: TYPICAL_CEILING_BYTES,
        });
    }
    if disk.has_mounts {
        warnings.push(Warning::HasMountedData);
    }
    Eligibility::Eligible { warnings }
}

/// A disk paired with its verdict, ready for the UI: eligible ones become the
/// picker rows, refused ones become the "N devices hidden — here's why" line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Judged {
    pub disk: TargetDisk,
    pub eligibility: Eligibility,
}

/// Classify a whole enumeration in one pass, preserving input order.
pub fn judge_all(disks: Vec<TargetDisk>) -> Vec<Judged> {
    disks
        .into_iter()
        .map(|disk| {
            let eligibility = classify(&disk);
            Judged { disk, eligibility }
        })
        .collect()
}

/// Just the disks it is safe to write to. This is what the picker offers.
pub fn eligible_targets(disks: Vec<TargetDisk>) -> Vec<Judged> {
    judge_all(disks)
        .into_iter()
        .filter(|j| j.eligibility.is_eligible())
        .collect()
}

/// Parse the raw reads of one Linux `/sys/block/<name>` entry into a
/// [`TargetDisk`]. Pure on purpose: the platform enumerator (a later change)
/// does the file I/O and root-disk resolution, then hands the strings here, so
/// the parsing is testable without real hardware.
///
/// - `removable_raw`  is `/sys/block/<name>/removable` (`"1"` = removable).
/// - `size_512_blocks` is `/sys/block/<name>/size` (in 512-byte sectors, as the
///   Linux block layer always reports regardless of the device's real sector
///   size).
/// - `model_raw` is `/sys/block/<name>/device/model` (may be empty/whitespace).
/// - `system` and `has_mounts` are decided by the enumerator from the mount
///   table and the device backing `/`.
pub fn from_sysblock(
    name: &str,
    removable_raw: &str,
    size_512_blocks: &str,
    model_raw: &str,
    system: bool,
    has_mounts: bool,
) -> TargetDisk {
    let removable = removable_raw.trim() == "1";
    // 512-byte sectors → bytes. A garbage/empty value reads as 0, which
    // `classify` refuses as UnknownSize — the safe direction.
    let size_bytes = size_512_blocks
        .trim()
        .parse::<u64>()
        .unwrap_or(0)
        .saturating_mul(512);
    let model = {
        let m = model_raw.trim();
        if m.is_empty() {
            name.to_string()
        } else {
            m.to_string()
        }
    };
    TargetDisk {
        path: format!("/dev/{name}"),
        model,
        size_bytes,
        removable,
        system,
        has_mounts,
    }
}

/// Render a byte count as a short human size (`"32.0 GB"`). Uses binary units
/// but the SI-ish suffix operators recognise; sizing here is for humans reading
/// a confirm dialog, not for accounting.
fn fmt_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    if bytes == 0 {
        return "unknown size".to_string();
    }
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{bytes} {}", UNITS[unit])
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A normal, safe target: a 32 GB USB stick with nothing mounted.
    fn good_card() -> TargetDisk {
        TargetDisk {
            path: "/dev/sdb".to_string(),
            model: "Generic USB 32GB".to_string(),
            size_bytes: 32 * GIB,
            removable: true,
            system: false,
            has_mounts: false,
        }
    }

    #[test]
    fn a_normal_card_is_eligible_with_no_warnings() {
        assert_eq!(
            classify(&good_card()),
            Eligibility::Eligible { warnings: vec![] }
        );
    }

    #[test]
    fn the_system_disk_is_always_refused() {
        let mut d = good_card();
        d.system = true;
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn system_disk_wins_the_reason_even_if_marked_removable() {
        // Defence in depth: a device that is somehow both removable and the
        // system disk must still be refused, and refused *as* the system disk.
        let d = TargetDisk {
            system: true,
            removable: true,
            ..good_card()
        };
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn a_fixed_internal_disk_is_refused() {
        let d = TargetDisk {
            removable: false,
            ..good_card()
        };
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::NotRemovable));
    }

    #[test]
    fn an_unknown_size_disk_is_refused_not_treated_as_small() {
        let d = TargetDisk {
            size_bytes: 0,
            ..good_card()
        };
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::UnknownSize));
    }

    #[test]
    fn a_too_small_card_is_refused_with_the_numbers() {
        let d = TargetDisk {
            size_bytes: 2 * GIB,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Refused(Refusal::TooSmall {
                need_bytes: MIN_TARGET_BYTES,
                have_bytes: 2 * GIB,
            })
        );
    }

    #[test]
    fn a_card_at_exactly_the_floor_is_eligible() {
        let d = TargetDisk {
            size_bytes: MIN_TARGET_BYTES,
            ..good_card()
        };
        // At the floor it fits but is below the recommended size, so exactly one
        // (small) warning.
        match classify(&d) {
            Eligibility::Eligible { warnings } => {
                assert_eq!(warnings.len(), 1);
                assert!(matches!(
                    warnings[0],
                    Warning::SmallerThanRecommended { .. }
                ));
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn a_small_but_usable_card_warns_it_is_below_recommended() {
        let d = TargetDisk {
            size_bytes: 4 * GIB,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::SmallerThanRecommended {
                    have_bytes: 4 * GIB,
                    recommend_bytes: RECOMMENDED_TARGET_BYTES,
                }]
            }
        );
    }

    #[test]
    fn a_very_large_disk_is_allowed_but_warned() {
        let d = TargetDisk {
            size_bytes: 512 * GIB,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::LargerThanTypical {
                    have_bytes: 512 * GIB,
                    ceiling_bytes: TYPICAL_CEILING_BYTES,
                }]
            }
        );
    }

    #[test]
    fn a_mounted_card_is_eligible_but_warns_about_erasing_data() {
        let d = TargetDisk {
            has_mounts: true,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::HasMountedData]
            }
        );
    }

    #[test]
    fn warnings_can_stack() {
        // A tiny-but-legal, mounted card: below recommended AND has data.
        let d = TargetDisk {
            size_bytes: 4 * GIB,
            has_mounts: true,
            ..good_card()
        };
        match classify(&d) {
            Eligibility::Eligible { warnings } => {
                assert!(warnings.contains(&Warning::SmallerThanRecommended {
                    have_bytes: 4 * GIB,
                    recommend_bytes: RECOMMENDED_TARGET_BYTES,
                }));
                assert!(warnings.contains(&Warning::HasMountedData));
                assert_eq!(warnings.len(), 2);
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn eligible_targets_keeps_only_safe_disks_and_preserves_order() {
        let disks = vec![
            good_card(), // eligible
            TargetDisk {
                path: "/dev/sda".to_string(),
                system: true,
                ..good_card()
            }, // refused: system
            TargetDisk {
                path: "/dev/nvme0n1".to_string(),
                removable: false,
                ..good_card()
            }, // refused: fixed
            TargetDisk {
                path: "/dev/sdc".to_string(),
                ..good_card()
            }, // eligible
        ];
        let kept = eligible_targets(disks);
        let paths: Vec<_> = kept.iter().map(|j| j.disk.path.as_str()).collect();
        assert_eq!(paths, vec!["/dev/sdb", "/dev/sdc"]);
    }

    #[test]
    fn judge_all_reports_every_disk_with_its_verdict() {
        let disks = vec![
            good_card(),
            TargetDisk {
                system: true,
                ..good_card()
            },
        ];
        let judged = judge_all(disks);
        assert_eq!(judged.len(), 2);
        assert!(judged[0].eligibility.is_eligible());
        assert_eq!(
            judged[1].eligibility,
            Eligibility::Refused(Refusal::SystemDisk)
        );
    }

    #[test]
    fn refusals_and_warnings_render_human_lines() {
        // Not asserting exact copy — just that they produce non-empty, sized text
        // the UI can show without post-processing.
        let r = Refusal::TooSmall {
            need_bytes: MIN_TARGET_BYTES,
            have_bytes: 2 * GIB,
        };
        assert!(r.reason().contains("3.0 GB"));
        assert!(r.reason().contains("2.0 GB"));
        assert!(!Refusal::SystemDisk.reason().is_empty());

        let w = Warning::HasMountedData;
        assert!(!w.message().is_empty());
    }

    #[test]
    fn from_sysblock_converts_512_byte_sectors_to_bytes() {
        // 62_500_000 sectors × 512 B = 32_000_000_000 B (a ~32 GB card).
        let d = from_sysblock("sdb", "1", "62500000", "SanDisk Extreme", false, false);
        assert_eq!(d.path, "/dev/sdb");
        assert_eq!(d.model, "SanDisk Extreme");
        assert_eq!(d.size_bytes, 32_000_000_000);
        assert!(d.removable);
        // A real 32 GB card is comfortably eligible.
        assert!(classify(&d).is_eligible());
    }

    #[test]
    fn from_sysblock_treats_a_fixed_disk_as_non_removable() {
        // system=false here so the verdict isolates the removable check; the
        // system-first ordering is covered by its own test above.
        let d = from_sysblock("nvme0n1", "0", "1000215216", "WD Blue SSD", false, false);
        assert!(!d.removable);
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::NotRemovable));
    }

    #[test]
    fn from_sysblock_falls_back_to_the_device_name_when_model_is_blank() {
        let d = from_sysblock("sdb", "1", "62500000", "   ", false, false);
        assert_eq!(d.model, "sdb");
    }

    #[test]
    fn from_sysblock_reads_garbage_size_as_zero_and_refuses_it() {
        let d = from_sysblock("sdb", "1", "not-a-number", "Weird Reader", false, false);
        assert_eq!(d.size_bytes, 0);
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::UnknownSize));
    }

    #[test]
    fn fmt_bytes_is_human_readable() {
        assert_eq!(fmt_bytes(0), "unknown size");
        assert_eq!(fmt_bytes(512), "512 B");
        assert_eq!(fmt_bytes(32 * GIB), "32.0 GB");
        assert_eq!(fmt_bytes(2 * 1024 * GIB), "2.0 TB");
    }
}
