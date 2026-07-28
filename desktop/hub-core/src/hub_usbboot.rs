//! hub_usbboot — read rpiboot's output and recognise the one Linux failure that
//! looks like nothing: it can't OPEN the Pi's USB device.
//!
//! On Linux the bundled `rpiboot` runs as the user, and opening the Pi's
//! boot-ROM USB device needs a udev rule (see `desktop/src-tauri/packaging/
//! rpiboot.rules` and INSTALL.md). Without it, libusb can SEE the Pi but not
//! claim it, so rpiboot prints an "access"/"failed to open" line and never
//! serves the mass-storage gadget — the Pi never appears as a disk, and to the
//! operator it just looks like nothing happened. This turns that line into a
//! one-sentence fix. Pure string logic, host-tested; the app calls it on each
//! rpiboot line (only on Linux — the fix is Linux-specific) and surfaces the
//! hint once.

/// The Linux-specific fix to show for a USB device-access failure.
pub const LINUX_UDEV_HINT: &str =
    "Linux couldn't open the Pi's USB device — this is almost always the missing udev rule. \
     The .deb installs it (replug the Pi after installing); AppImage or other users add it \
     once by hand. See the flasher's INSTALL.md → \"Flash a Pi over USB-C (Linux)\".";

/// The hint to show when a line of rpiboot output looks like it failed to open
/// the Pi's **USB device** (the missing-udev-rule symptom); `None` otherwise.
///
/// It deliberately requires USB/device context: a bare "failed to open
/// bootfiles.bin" is a payload problem, not a permissions one, and must not be
/// dressed up as a udev issue. Callers still gate this to Linux — the hint text
/// is a Linux fix.
pub fn access_denied_hint(line: &str) -> Option<&'static str> {
    let l = line.to_ascii_lowercase();
    let device_context = l.contains("device") || l.contains("usb") || l.contains("libusb");
    let access_or_open_failure = l.contains("libusb_error_access")
        || l.contains("access denied")
        || l.contains("permission denied")
        || (l.contains("open") && l.contains("failed"));
    (device_context && access_or_open_failure).then_some(LINUX_UDEV_HINT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_usb_device_open_failures_rpiboot_prints() {
        for line in [
            "Failed to open the requested device",
            "libusb: error [op_open] libusb_open() failed",
            "LIBUSB_ERROR_ACCESS",
            "Cannot open the USB device: Access denied",
            "USB device /dev/bus/usb/001/007: permission denied",
        ] {
            assert!(access_denied_hint(line).is_some(), "missed: {line}");
        }
    }

    #[test]
    fn ignores_open_failures_without_usb_context() {
        // The key false-positive to avoid: a payload/file open error is NOT a
        // udev problem, so it must not produce the udev hint.
        for line in ["Failed to open bootfiles.bin", "failed to open config.txt"] {
            assert!(access_denied_hint(line).is_none(), "false positive: {line}");
        }
    }

    #[test]
    fn leaves_normal_progress_lines_alone() {
        for line in [
            "Waiting for BCM2712...",
            "Loading embedded: bootcode.bin",
            "Second stage boot server",
            "Device located successfully",
            "Sending mass-storage-gadget64",
        ] {
            assert!(access_denied_hint(line).is_none(), "false positive: {line}");
        }
    }

    #[test]
    fn is_case_insensitive() {
        assert!(access_denied_hint("FAILED TO OPEN the USB device").is_some());
    }
}
