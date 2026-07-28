//! port_hint — recognise the two Linux-specific reasons a Canary's serial
//! port won't open, and say the real fix instead of retrying in silence.
//!
//! On macOS a USB-CDC port the OS lists is a port the app can open, so the
//! flows there never see these. On Linux two things get in the way:
//!
//!  * **Permission denied** — the user isn't in `dialout` and no udev rule
//!    grants access. This never heals by itself, so a silent retry loop (or
//!    the generic "put the board in download mode" advice) leaves the user
//!    stuck on the one machine where the fix is a one-liner.
//!  * **Busy** — something else holds the port. Right after plug-in or after
//!    the post-flash reboot re-enumerates the board, that's usually
//!    ModemManager probing the "modem" — it can hold the port for ~30 s and
//!    its control-line toggling can reset an ESP32-S3 mid-boot, which is
//!    exactly a "flash verified but the boot receipt never arrived" outcome.
//!
//! Same shape as hub-core's `hub_usbboot::access_denied_hint`: a pure,
//! host-tested string classifier; callers gate it to Linux because the hint
//! text is a Linux fix.
//!
//! Wording constraint: the serial monitor shows these over `serial:status`,
//! and app.js treats a status containing "could not" or "stopped" as the
//! monitor being dead — so neither phrase may appear in a hint.

/// The port exists but Linux refused to open it for this user.
pub const PERMISSION_HINT: &str =
    "Linux blocked opening the board's serial port (permission denied). Install the .deb \
     (it ships a udev rule that grants access), or add yourself to the dialout group: \
     sudo usermod -aG dialout $USER — then log out and back in, and replug the board. \
     See the flasher's INSTALL.md → \"Linux\".";

/// The port exists but another process holds it right now.
pub const BUSY_HINT: &str =
    "Another program is holding the board's serial port. On Linux that's usually \
     ModemManager probing a just-plugged board — it can hold the port for ~30 s and even \
     reset the board mid-boot. The app keeps retrying; the .deb's udev rule tells \
     ModemManager to leave Canaries alone (AppImage users: see INSTALL.md → \"Linux\").";

/// Classify a serial-open failure (or espflash's output around one) into the
/// Linux fix it needs; `None` for anything unrecognised, so ordinary failures
/// keep their ordinary messages. Permission wins over busy: an EACCES text
/// that also mentions the device path must not read as "wait it out".
pub fn linux_open_hint(message: &str) -> Option<&'static str> {
    let m = message.to_lowercase();
    if ["permission denied", "access denied", "eacces"]
        .iter()
        .any(|needle| m.contains(needle))
    {
        return Some(PERMISSION_HINT);
    }
    if [
        "resource busy",
        "port is busy",
        "ebusy",
        "in use by another",
    ]
    .iter()
    .any(|needle| m.contains(needle))
    {
        return Some(BUSY_HINT);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn permission_denied_gets_the_dialout_udev_fix() {
        for msg in [
            "Permission denied (os error 13)",
            "could not open the Vision module port: Permission denied",
            "Error: IO error while using serial port: EACCES",
        ] {
            assert_eq!(linux_open_hint(msg), Some(PERMISSION_HINT), "{msg}");
        }
    }

    #[test]
    fn busy_gets_the_modemmanager_explanation() {
        for msg in [
            "Device or resource busy",
            "Error: the serial port is in use by another program",
        ] {
            assert_eq!(linux_open_hint(msg), Some(BUSY_HINT), "{msg}");
        }
    }

    #[test]
    fn permission_wins_when_both_words_appear() {
        assert_eq!(
            linux_open_hint("open /dev/ttyACM0: Permission denied (port busy?)"),
            Some(PERMISSION_HINT)
        );
    }

    #[test]
    fn ordinary_failures_stay_unhinted() {
        for msg in [
            "Serial port not found",
            "Failed to connect to the device: wrong boot mode",
            "timed out waiting for the bootloader",
            "",
        ] {
            assert_eq!(linux_open_hint(msg), None, "{msg}");
        }
    }

    // app.js reads "could not" / "stopped" in a serial:status line as the
    // monitor being dead and disables its buttons — a hint must never trip it.
    #[test]
    fn hints_avoid_the_monitor_dead_phrases() {
        for hint in [PERMISSION_HINT, BUSY_HINT] {
            let lower = hint.to_lowercase();
            assert!(!lower.contains("could not"));
            assert!(!lower.contains("stopped"));
        }
    }
}
