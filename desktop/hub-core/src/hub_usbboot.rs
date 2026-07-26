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
//! rpiboot line and surfaces the hint once.

/// The hint to show when a line of rpiboot output looks like a device-open
/// permission failure (the missing-udev-rule symptom); `None` for normal output.
pub fn access_denied_hint(line: &str) -> Option<&'static str> {
    let l = line.to_ascii_lowercase();
    // "failed to open" and libusb's "libusb_open() failed" both reduce to
    // open+failed; normal rpiboot progress lines never contain "failed".
    let denied = (l.contains("open") && l.contains("failed"))
        || l.contains("access denied")
        || l.contains("libusb_error_access")
        || l.contains("permission denied")
        || (l.contains("cannot open") && l.contains("device"));
    denied.then_some(
        "Linux couldn't open the Pi's USB device — this is almost always the missing udev rule. \
         The .deb installs it (replug the Pi after installing); AppImage or other users add it \
         once by hand. See the flasher's INSTALL.md → \"Flash a Pi over USB-C (Linux)\".",
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_the_access_failures_rpiboot_prints() {
        for line in [
            "Failed to open the requested device",
            "libusb: error [op_open] libusb_open() failed",
            "LIBUSB_ERROR_ACCESS",
            "Cannot open the USB device: Access denied",
            "error: permission denied",
        ] {
            assert!(access_denied_hint(line).is_some(), "missed: {line}");
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
        assert!(access_denied_hint("FAILED TO OPEN device").is_some());
    }
}
