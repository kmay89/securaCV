//! Pure decision logic for the native **rescue bench** — the triage tools that
//! turn the flasher from a one-shot writer into something that can save a board,
//! put a copy back, wipe it clean, and flash a local file. It mirrors the
//! browser Lab's rescue behavior (canary-local: `validateBackupFile`, the
//! backup naming, the image-magic hints) so the two surfaces agree.
//!
//! Kept dependency-free (std only), and part of the tauri-free flash engine, so
//! it unit-tests WITHOUT the desktop stack (`cargo test` in
//! desktop/flash-engine), exactly like the `hub-core` crate. The Flasher's
//! Tauri commands (desktop/src-tauri/src/lib.rs) wrap these builders with the
//! `espflash` sidecar and the file dialogs — those are what a Mac /
//! release-tag build validates end-to-end.

/// The full-flash backup's filename, in the browser Lab's own scheme
/// (flash.js `takeBackup`: `canary-${macStamp()}-backup.bin`) so a file saved
/// by either flasher is the `canary-…-backup.bin` both restore panels tell the
/// user to look for: `canary-<mac6>[-<stamp>]-backup.bin`.
///
/// `mac6` is the last six hex digits of the MAC, lowercased — the browser's
/// `macStamp()` exactly, `canary` when no MAC was read. `stamp` is optional
/// (the browser leaves it to the downloads folder to de-duplicate; the
/// Flasher's automatic safety copies share one folder, so they carry the
/// moment): reduced to filename-safe characters, so it can't produce a path
/// separator or a surprise, and dropped when it comes out empty.
pub fn backup_filename(mac: &str, stamp: &str) -> String {
    let hex: String = mac
        .chars()
        .filter(char::is_ascii_hexdigit)
        .collect::<String>()
        .to_ascii_lowercase();
    let mac6 = if hex.is_empty() {
        "canary"
    } else {
        &hex[hex.len().saturating_sub(6)..]
    };
    let safe: String = stamp
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '.' || c == '-' {
                c
            } else {
                '-'
            }
        })
        .collect();
    // collapse runs of '-' and trim them, so "AA::BB" -> "AA-BB", not "AA--BB"
    let mut stamp = String::with_capacity(safe.len());
    for c in safe.chars() {
        if !(c == '-' && stamp.ends_with('-')) {
            stamp.push(c);
        }
    }
    let stamp = stamp.trim_matches(|c| c == '-' || c == '.');
    if stamp.is_empty() {
        format!("canary-{mac6}-backup.bin")
    } else {
        format!("canary-{mac6}-{stamp}-backup.bin")
    }
}

/// Validate a candidate restore/local image against the detected chip's flash
/// size, mirroring the browser's `validateBackupFile`:
///   * empty                     -> `Err` (nothing to write)
///   * larger than the chip flash-> `Err` (can't have come from this board)
///   * smaller than the chip     -> `Ok(Some(warning))` (written from 0x0, tail left)
///   * exactly the flash size,
///     or size unknown           -> `Ok(None)`
pub fn validate_restore_image(
    byte_len: u64,
    flash_bytes: Option<u64>,
) -> Result<Option<String>, String> {
    if byte_len == 0 {
        return Err("that file is empty — there's nothing to write".into());
    }
    if let Some(flash) = flash_bytes {
        if byte_len > flash {
            return Err(format!(
                "that file ({}) is bigger than this chip's flash ({}) — it can't be an image of this board",
                human_bytes(byte_len),
                human_bytes(flash)
            ));
        }
        if byte_len < flash {
            return Ok(Some(format!(
                "heads up: the file ({}) is smaller than the chip ({}) — it's written from the start of flash and the rest is left untouched",
                human_bytes(byte_len),
                human_bytes(flash)
            )));
        }
    }
    Ok(None)
}

/// One honest sentence about a file's first bytes, by the same magic the chip
/// itself uses — so a restore/local flash can say what it's about to write.
pub fn image_first_bytes_hint(bytes: &[u8]) -> Option<&'static str> {
    if bytes.first() == Some(&0xE9) {
        return Some(
            "an ESP32 firmware image — 0xE9 is the chip's own \"program starts here\" marker",
        );
    }
    if bytes.len() >= 2 && u16le(bytes, 0) == 0xAA50 {
        return Some("partition-table entries (magic 0xAA50) — the board's map of itself");
    }
    if bytes.len() >= 4 && u32le(bytes, 0) == 0xABCD_5432 {
        return Some("a firmware description block (magic 0xABCD5432)");
    }
    None
}

// ── espflash arg builders (pure, so the exact invocation is host-tested) ─────
// The sidecar is the esp-rs `espflash` CLI; these mirror the shapes the existing
// `flash`/`board-info` commands already use.

/// `read-flash 0x0 <size> <out> --port <port> --baud <baud>` — a full-chip backup.
pub fn read_flash_args(port: &str, size: u64, out: &str, baud: u32) -> Vec<String> {
    vec![
        "read-flash".into(),
        "0x0".into(),
        size.to_string(),
        out.into(),
        "--port".into(),
        port.into(),
        "--baud".into(),
        baud.to_string(),
    ]
}

/// `read-flash <offset> <size> <out> --port <port> --baud <baud>` — read ONE
/// region (the health check reads the partition table, app descriptors, otadata,
/// coredump, and NVS this way, each to a temp file it reads back).
pub fn read_region_args(port: &str, offset: u32, size: u32, out: &str, baud: u32) -> Vec<String> {
    vec![
        "read-flash".into(),
        format!("0x{offset:x}"),
        size.to_string(),
        out.into(),
        "--port".into(),
        port.into(),
        "--baud".into(),
        baud.to_string(),
    ]
}

/// `write-bin 0x0 <path> --port <port> --baud <baud>` — restore or local flash.
/// Identical shape to the `flash` command's write step, so a restored image lands
/// exactly where a factory image would.
pub fn write_bin_args(port: &str, path: &str, baud: u32) -> Vec<String> {
    vec![
        "write-bin".into(),
        "0x0".into(),
        path.into(),
        "--port".into(),
        port.into(),
        "--baud".into(),
        baud.to_string(),
    ]
}

/// `erase-flash --port <port>` — wipe the whole chip for a truly clean install.
pub fn erase_flash_args(port: &str) -> Vec<String> {
    vec!["erase-flash".into(), "--port".into(), port.into()]
}

/// Pull the flash size (in bytes) out of `espflash board-info` output — the
/// "Flash size: 8MB" line — so a full-chip backup knows how much to read. The
/// same board-info call already names the chip; this reads the size from it, no
/// extra round-trip. Returns None when the line is absent or unparseable (an
/// older espflash, or a board that didn't report it), which the caller turns
/// into an honest "reconnect and try again" rather than a wrong-sized read.
pub fn parse_flash_size(board_info: &str) -> Option<u64> {
    for line in board_info.lines() {
        let lower = line.trim().to_ascii_lowercase();
        let Some(rest) = lower.strip_prefix("flash size:") else {
            continue;
        };
        let rest = rest.trim();
        let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
        if digits.is_empty() {
            continue;
        }
        let n: u64 = digits.parse().ok()?;
        let unit = rest[digits.len()..].trim_start();
        let mult: u64 = if unit.starts_with("mb") {
            1 << 20
        } else if unit.starts_with("kb") {
            1 << 10
        } else if unit.starts_with("gb") {
            1 << 30
        } else if unit.starts_with('b') {
            1
        } else {
            continue;
        };
        return n.checked_mul(mult);
    }
    None
}

/// Pull the board's MAC address out of `espflash board-info` output — the
/// "MAC address: aa:bb:cc:dd:ee:ff" line — lowercased. `detect_chip` reads it
/// from the same board-info call that already names the chip, so the health
/// report and backup names can show a stable ID. None if no MAC-shaped token.
pub fn parse_mac(board_info: &str) -> Option<String> {
    for line in board_info.lines() {
        if !line.to_ascii_lowercase().contains("mac") {
            continue;
        }
        for tok in line.split_whitespace() {
            let parts: Vec<&str> = tok.split(':').collect();
            if parts.len() == 6
                && parts
                    .iter()
                    .all(|p| p.len() == 2 && p.chars().all(|c| c.is_ascii_hexdigit()))
            {
                return Some(tok.to_ascii_lowercase());
            }
        }
    }
    None
}

// ── tiny helpers (std only) ──────────────────────────────────────────────────
fn u16le(b: &[u8], o: usize) -> u16 {
    (b[o] as u16) | ((b[o + 1] as u16) << 8)
}
fn u32le(b: &[u8], o: usize) -> u32 {
    (b[o] as u32) | ((b[o + 1] as u32) << 8) | ((b[o + 2] as u32) << 16) | ((b[o + 3] as u32) << 24)
}
pub fn human_bytes(n: u64) -> String {
    if n >= 1 << 20 {
        format!("{:.2} MB", n as f64 / (1u64 << 20) as f64)
    } else if n >= 1 << 10 {
        format!("{} KB", n / (1 << 10))
    } else {
        format!("{} B", n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backup_filename_is_the_browsers_scheme_and_safe() {
        // flash.js takeBackup: `canary-${macStamp()}-backup.bin`, macStamp =
        // the MAC's last six hex digits, lowercased, or "canary".
        assert_eq!(
            backup_filename("AA:BB:CC:DD:EE:FF", ""),
            "canary-ddeeff-backup.bin"
        );
        assert_eq!(backup_filename("", ""), "canary-canary-backup.bin");
        assert_eq!(
            backup_filename("aa-bb-cc-dd-ee-ff", "1789000000"),
            "canary-ddeeff-1789000000-backup.bin"
        );
        // A MAC shorter than six digits keeps what it has.
        assert_eq!(backup_filename("0:1", ""), "canary-01-backup.bin");
        // Path-separator / space attempts collapse to '-', never escape the name.
        assert_eq!(
            backup_filename("", "es/p 32"),
            "canary-canary-es-p-32-backup.bin"
        );
        assert!(!backup_filename("../etc", "../../x").contains('/'));
        assert!(!backup_filename("", "../..").contains(".."));
    }

    #[test]
    fn validate_restore_matches_the_browser_contract() {
        assert!(validate_restore_image(0, Some(0x400000)).is_err()); // empty
        assert!(validate_restore_image(0x500000, Some(0x400000)).is_err()); // too big
        assert_eq!(
            validate_restore_image(0x400000, Some(0x400000)).unwrap(),
            None
        ); // exact
        assert!(validate_restore_image(0x100000, Some(0x400000))
            .unwrap()
            .is_some()); // smaller -> warn
        assert_eq!(validate_restore_image(0x1234, None).unwrap(), None); // size unknown -> allowed
    }

    #[test]
    fn image_hint_reads_the_chip_magic() {
        assert!(image_first_bytes_hint(&[0xE9, 0, 0])
            .unwrap()
            .contains("firmware"));
        assert!(image_first_bytes_hint(&[0x50, 0xAA])
            .unwrap()
            .contains("partition"));
        assert!(image_first_bytes_hint(&[0x32, 0x54, 0xCD, 0xAB])
            .unwrap()
            .contains("description"));
        assert_eq!(image_first_bytes_hint(&[0x00, 0x01]), None);
        assert_eq!(image_first_bytes_hint(&[]), None);
    }

    #[test]
    fn flash_size_parses_from_board_info() {
        let info = "Chip type:         esp32s3 (revision v0.2)\nCrystal frequency: 40 MHz\nFlash size:        8MB\nMAC address:       aa:bb:cc:dd:ee:ff";
        assert_eq!(parse_flash_size(info), Some(8 << 20));
        assert_eq!(parse_flash_size("flash size: 4 MB"), Some(4 << 20));
        assert_eq!(parse_flash_size("Flash size:16MB"), Some(16 << 20));
        assert_eq!(parse_flash_size("Flash size: unknown"), None);
        assert_eq!(parse_flash_size("Chip type: esp32c3\nno size here"), None);
    }

    #[test]
    fn espflash_args_mirror_the_flash_command() {
        assert_eq!(
            read_flash_args("/dev/tty.usb", 0x400000, "/tmp/b.bin", 921600),
            vec![
                "read-flash",
                "0x0",
                "4194304",
                "/tmp/b.bin",
                "--port",
                "/dev/tty.usb",
                "--baud",
                "921600"
            ]
        );
        assert_eq!(
            write_bin_args("/dev/tty.usb", "/tmp/b.bin", 921600),
            vec![
                "write-bin",
                "0x0",
                "/tmp/b.bin",
                "--port",
                "/dev/tty.usb",
                "--baud",
                "921600"
            ]
        );
        assert_eq!(
            erase_flash_args("/dev/tty.usb"),
            vec!["erase-flash", "--port", "/dev/tty.usb"]
        );
        assert_eq!(
            read_region_args("/dev/tty.usb", 0x8000, 0xc00, "/tmp/pt.bin", 115200),
            vec![
                "read-flash",
                "0x8000",
                "3072",
                "/tmp/pt.bin",
                "--port",
                "/dev/tty.usb",
                "--baud",
                "115200"
            ]
        );
    }

    #[test]
    fn mac_parses_from_board_info() {
        let info = "Chip type:   esp32s3\nMAC address: AA:BB:CC:11:22:33\nFlash size:  8MB";
        assert_eq!(parse_mac(info), Some("aa:bb:cc:11:22:33".to_string()));
        assert_eq!(parse_mac("no mac here"), None);
        assert_eq!(parse_mac("Chip type: esp32c3"), None);
    }
}
