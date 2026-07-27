//! hub_disk — which disk may we write a Home Assistant hub image to?
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
//! before a single byte is written. The destructive write itself is a separate,
//! hardware-validated step (a later change); its one hard contract is that it
//! MUST call [`classify`] and refuse anything that is not
//! [`Eligibility::Eligible`].
//!
//! **What counts as a legal target.** A hub disk must be *external to this
//! computer*: a removable SD card / USB stick, or an external / ejectable
//! SSD or NVMe (the Pi 5 "durable default" the design steers toward — see
//! `docs/design/raspberry_pi_hub_flashing.md`). A disk that is neither
//! removable nor external is an internal fixed disk and is never a target. The
//! system/boot disk is refused outright, first, on top of that. It must also be
//! big enough for the supported full-stack image (Frigate/go2rtc + dashboards),
//! whose hardware list asks for a 32 GB+ card.
//!
//! This module has no Tauri, no serial, no I/O in its decision path: it reasons
//! over already-observed facts about a disk. Enumerating the disks — and, per
//! platform, deciding which is the system disk and which non-removable disks are
//! *external* — is the enumerator's job in a follow-up change; [`from_sysblock`]
//! is the pure half of that, kept here so it can be tested the same way.

/// 1024³ — disk sizes are quoted in binary gibibytes throughout this module.
/// (Human display in [`fmt_bytes`] uses the same base, so a "32 GB" card, which
/// reports ~31.9 × 10⁹ bytes, shows as ~29.7 GB — the usual marketing-vs-OS gap.)
const GIB: u64 = 1024 * 1024 * 1024;

/// Hard floor. The supported hub is a **32 GB+** card (the Home Assistant
/// hardware list in `canary-local/tools/gen_homeassistant.py`) and this build
/// preloads the *full-stack* image — HAOS plus Frigate/go2rtc — so an undersized
/// card yields a thrashing, unsupported hub, not a smaller one. A nominal 32 GB
/// card reports ~29.7 GiB, so the floor sits just under that at 28 GiB: it
/// accepts a genuine 32 GB card and refuses 16 GB and below outright (a warning
/// is not enough once this is the writer's hard gate).
pub const MIN_TARGET_BYTES: u64 = 28 * GIB;

/// Below this we still allow the write but advise a bigger card. Endurance is
/// the reason, not capacity: `docs/sd_card_health.md` notes a 64 GB card holding
/// a couple GB of data wear-levels across far more flash, which is exactly what a
/// 24/7 hub wants.
pub const RECOMMENDED_TARGET_BYTES: u64 = 64 * GIB;

/// Above this we still allow the write but nudge the operator to confirm. An
/// external SSD is a legitimate (encouraged) Pi 5 target, so size alone is no
/// longer a "wrong device" signal the way it was for cards — but a drive this
/// large is also what a backup disk or NAS looks like, so a large target earns a
/// "is this really the one you mean?" beat at the confirm step.
pub const LARGE_ADVISORY_BYTES: u64 = 512 * GIB;

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
    /// The OS marks this device as removable (an SD reader or USB stick).
    pub removable: bool,
    /// The device is external to this computer — an external / ejectable SSD or
    /// NVMe that is not part of the running machine. This is what lets the
    /// Pi 5 "durable default" (an SSD) be a target even though such drives are
    /// usually reported non-removable. The enumerator sets it from the OS's
    /// internal/ejectable signal and MUST be conservative: when it cannot prove
    /// a non-removable disk is external, it leaves this `false` so the disk is
    /// refused rather than risked.
    pub external: bool,
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
    /// It is a fixed disk inside this computer — neither removable nor a known
    /// external drive.
    InternalFixedDisk,
    /// The OS reported an unknown (zero) size; we will not write blind.
    UnknownSize,
    /// It is smaller than the supported hub image needs.
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
            Refusal::InternalFixedDisk => {
                "this looks like a fixed disk inside your computer, not a removable card or an external SSD/USB drive"
                    .to_string()
            }
            Refusal::UnknownSize => {
                "the system wouldn't report this device's size, so we won't write to it".to_string()
            }
            Refusal::TooSmall {
                need_bytes,
                have_bytes,
            } => format!(
                "too small for a full Home Assistant hub — needs at least {}, this is {}",
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
    /// Fits, but below the recommended (endurance-headroom) card size.
    SmallerThanRecommended {
        have_bytes: u64,
        recommend_bytes: u64,
    },
    /// Large enough to be a backup drive / NAS — worth a second look.
    LargerThanTypical {
        have_bytes: u64,
        advisory_bytes: u64,
    },
    /// The device currently has mounted filesystems that the write will erase.
    HasMountedData,
    /// A non-removable target accepted only because the enumerator marked it
    /// external — surface that judgement so the operator can sanity-check it.
    ExternalNonRemovable,
}

impl Warning {
    /// A one-line, human explanation for the confirm step.
    pub fn message(&self) -> String {
        match self {
            Warning::SmallerThanRecommended {
                have_bytes,
                recommend_bytes,
            } => format!(
                "smaller than recommended ({}, vs {} suggested) — it will work but with little endurance headroom",
                fmt_bytes(*have_bytes),
                fmt_bytes(*recommend_bytes)
            ),
            Warning::LargerThanTypical {
                have_bytes,
                advisory_bytes,
            } => format!(
                "this is {} — larger than a typical card ({}+). Make sure it isn't a drive you want to keep",
                fmt_bytes(*have_bytes),
                fmt_bytes(*advisory_bytes)
            ),
            Warning::HasMountedData => {
                "this device has data on it right now — everything on it will be erased".to_string()
            }
            Warning::ExternalNonRemovable => {
                "treated as an external drive (e.g. an SSD/NVMe), not a card — double-check it's the right one".to_string()
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
/// removable/external is still refused as [`Refusal::SystemDisk`].
///
/// The refusals are deliberately belt-and-suspenders. A legal target must be
/// external to this machine (removable OR enumerator-confirmed external), not the
/// system disk, of known size, and big enough — all four. Because `external`
/// defaults conservative in the enumerator, the failure mode of a
/// misclassification is a *refused* disk (the operator falls back to a plain
/// card), never a wrongly-offered internal one.
pub fn classify(disk: &TargetDisk) -> Eligibility {
    if disk.system {
        return Eligibility::Refused(Refusal::SystemDisk);
    }
    if !disk.removable && !disk.external {
        return Eligibility::Refused(Refusal::InternalFixedDisk);
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
    if disk.size_bytes > LARGE_ADVISORY_BYTES {
        warnings.push(Warning::LargerThanTypical {
            have_bytes: disk.size_bytes,
            advisory_bytes: LARGE_ADVISORY_BYTES,
        });
    }
    // A non-removable disk that got here did so because it's external; say so.
    if !disk.removable && disk.external {
        warnings.push(Warning::ExternalNonRemovable);
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
/// does the file I/O and the system-disk / external resolution, then hands the
/// strings here, so the parsing is testable without real hardware.
///
/// - `removable_raw`  is `/sys/block/<name>/removable` (`"1"` = removable).
/// - `size_512_blocks` is `/sys/block/<name>/size` (in 512-byte sectors, as the
///   Linux block layer always reports regardless of the device's real sector
///   size).
/// - `model_raw` is `/sys/block/<name>/device/model` (may be empty/whitespace).
/// - `external`, `system`, and `has_mounts` are decided by the enumerator: from
///   the device's transport/subsystem (USB/UAS → external; the internal
///   NVMe/eMMC → not), the disk backing `/`, and the mount table respectively.
pub fn from_sysblock(
    name: &str,
    removable_raw: &str,
    size_512_blocks: &str,
    model_raw: &str,
    external: bool,
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
        external,
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

    /// A nominal 32 GB card, as `docs/sd_card_health.md`'s own example reports
    /// one (~29.7 GiB usable) — the supported-minimum reference size.
    const CARD_32GB_BYTES: u64 = 31_914_983_424;

    /// A normal, safe target: a 64 GB removable USB stick with nothing mounted.
    fn good_card() -> TargetDisk {
        TargetDisk {
            path: "/dev/sdb".to_string(),
            model: "Generic USB 64GB".to_string(),
            size_bytes: 64 * GIB,
            removable: true,
            external: false, // removable already qualifies it; external is for non-removable
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
    fn system_disk_wins_the_reason_even_if_marked_removable_and_external() {
        // Defence in depth: a device that is somehow the system disk must still
        // be refused, and refused *as* the system disk, whatever else it claims.
        let d = TargetDisk {
            system: true,
            removable: true,
            external: true,
            ..good_card()
        };
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::SystemDisk));
    }

    #[test]
    fn a_fixed_internal_disk_is_refused() {
        // Not removable and not external → an internal fixed disk.
        let d = TargetDisk {
            removable: false,
            external: false,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Refused(Refusal::InternalFixedDisk)
        );
    }

    #[test]
    fn an_external_ssd_is_eligible_even_though_it_is_not_removable() {
        // The Pi 5 durable default: a 256 GB external SSD/NVMe. Non-removable,
        // but the enumerator proved it external, so it is a legal target — with
        // an advisory that it was treated as an external drive.
        let d = TargetDisk {
            path: "/dev/sda".to_string(),
            model: "Samsung T7 256GB".to_string(),
            size_bytes: 256 * GIB,
            removable: false,
            external: true,
            system: false,
            has_mounts: false,
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::ExternalNonRemovable]
            }
        );
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
    fn a_16gb_card_is_refused_as_too_small_for_the_full_stack_hub() {
        let d = TargetDisk {
            size_bytes: 16 * GIB,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Refused(Refusal::TooSmall {
                need_bytes: MIN_TARGET_BYTES,
                have_bytes: 16 * GIB,
            })
        );
    }

    #[test]
    fn a_nominal_32gb_card_is_eligible_and_warns_it_is_below_recommended() {
        let d = TargetDisk {
            size_bytes: CARD_32GB_BYTES,
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::SmallerThanRecommended {
                    have_bytes: CARD_32GB_BYTES,
                    recommend_bytes: RECOMMENDED_TARGET_BYTES,
                }]
            }
        );
    }

    #[test]
    fn a_disk_just_below_the_floor_is_refused() {
        let d = TargetDisk {
            size_bytes: MIN_TARGET_BYTES - 1,
            ..good_card()
        };
        assert!(matches!(
            classify(&d),
            Eligibility::Refused(Refusal::TooSmall { .. })
        ));
    }

    #[test]
    fn a_disk_exactly_at_the_floor_is_eligible() {
        let d = TargetDisk {
            size_bytes: MIN_TARGET_BYTES,
            ..good_card()
        };
        // At the floor it fits but is below the recommended size → one warning.
        match classify(&d) {
            Eligibility::Eligible { warnings } => {
                assert_eq!(
                    warnings,
                    vec![Warning::SmallerThanRecommended {
                        have_bytes: MIN_TARGET_BYTES,
                        recommend_bytes: RECOMMENDED_TARGET_BYTES,
                    }]
                );
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn a_very_large_drive_is_allowed_but_warned() {
        let d = TargetDisk {
            size_bytes: 1024 * GIB, // 1 TiB — could be a backup disk
            ..good_card()
        };
        assert_eq!(
            classify(&d),
            Eligibility::Eligible {
                warnings: vec![Warning::LargerThanTypical {
                    have_bytes: 1024 * GIB,
                    advisory_bytes: LARGE_ADVISORY_BYTES,
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
        // A nominal 32 GB, mounted card: below recommended AND has data.
        let d = TargetDisk {
            size_bytes: CARD_32GB_BYTES,
            has_mounts: true,
            ..good_card()
        };
        match classify(&d) {
            Eligibility::Eligible { warnings } => {
                assert!(warnings.contains(&Warning::SmallerThanRecommended {
                    have_bytes: CARD_32GB_BYTES,
                    recommend_bytes: RECOMMENDED_TARGET_BYTES,
                }));
                assert!(warnings.contains(&Warning::HasMountedData));
                assert_eq!(warnings.len(), 2);
            }
            other => panic!("expected eligible, got {other:?}"),
        }
    }

    #[test]
    fn eligible_targets_keeps_safe_disks_incl_external_ssd_and_preserves_order() {
        let disks = vec![
            good_card(), // eligible (removable)
            TargetDisk {
                path: "/dev/sda".to_string(),
                system: true,
                ..good_card()
            }, // refused: system
            TargetDisk {
                path: "/dev/nvme0n1".to_string(),
                removable: false,
                external: false,
                ..good_card()
            }, // refused: internal fixed
            TargetDisk {
                path: "/dev/nvme1n1".to_string(),
                removable: false,
                external: true,
                ..good_card()
            }, // eligible: external SSD/NVMe
        ];
        let kept = eligible_targets(disks);
        let paths: Vec<_> = kept.iter().map(|j| j.disk.path.as_str()).collect();
        assert_eq!(paths, vec!["/dev/sdb", "/dev/nvme1n1"]);
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
            have_bytes: 16 * GIB,
        };
        assert!(r.reason().contains("28.0 GB"));
        assert!(r.reason().contains("16.0 GB"));
        assert!(!Refusal::SystemDisk.reason().is_empty());
        assert!(!Refusal::InternalFixedDisk.reason().is_empty());
        for w in [
            Warning::HasMountedData,
            Warning::ExternalNonRemovable,
            Warning::LargerThanTypical {
                have_bytes: 1024 * GIB,
                advisory_bytes: LARGE_ADVISORY_BYTES,
            },
        ] {
            assert!(!w.message().is_empty());
        }
    }

    #[test]
    fn from_sysblock_converts_512_byte_sectors_to_bytes() {
        // 125_000_000 sectors × 512 B = 64_000_000_000 B (a ~64 GB card).
        let d = from_sysblock(
            "sdb",
            "1",
            "125000000",
            "SanDisk Extreme",
            false,
            false,
            false,
        );
        assert_eq!(d.path, "/dev/sdb");
        assert_eq!(d.model, "SanDisk Extreme");
        assert_eq!(d.size_bytes, 64_000_000_000);
        assert!(d.removable);
        assert!(!d.external);
        // A real 64 GB card is comfortably eligible.
        assert!(classify(&d).is_eligible());
    }

    #[test]
    fn from_sysblock_marks_an_internal_fixed_disk_refused() {
        let d = from_sysblock(
            "nvme0n1",
            "0",
            "1000215216",
            "WD Blue SSD",
            false, // not external — the internal boot NVMe
            false,
            true,
        );
        assert!(!d.removable);
        assert!(!d.external);
        assert_eq!(
            classify(&d),
            Eligibility::Refused(Refusal::InternalFixedDisk)
        );
    }

    #[test]
    fn from_sysblock_carries_external_through_for_an_ssd() {
        // A big external NVMe over USB: not removable, but external → eligible.
        let d = from_sysblock(
            "sda",
            "0",
            "500118192",
            "Samsung T7",
            true, // external
            false,
            false,
        );
        assert!(!d.removable);
        assert!(d.external);
        assert!(classify(&d).is_eligible());
    }

    #[test]
    fn from_sysblock_falls_back_to_the_device_name_when_model_is_blank() {
        let d = from_sysblock("sdb", "1", "125000000", "   ", false, false, false);
        assert_eq!(d.model, "sdb");
    }

    #[test]
    fn from_sysblock_reads_garbage_size_as_zero_and_refuses_it() {
        let d = from_sysblock(
            "sdb",
            "1",
            "not-a-number",
            "Weird Reader",
            false,
            false,
            false,
        );
        assert_eq!(d.size_bytes, 0);
        assert_eq!(classify(&d), Eligibility::Refused(Refusal::UnknownSize));
    }

    #[test]
    fn fmt_bytes_is_human_readable() {
        assert_eq!(fmt_bytes(0), "unknown size");
        assert_eq!(fmt_bytes(512), "512 B");
        assert_eq!(fmt_bytes(64 * GIB), "64.0 GB");
        assert_eq!(fmt_bytes(2 * 1024 * GIB), "2.0 TB");
    }
}
