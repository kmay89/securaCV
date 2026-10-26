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

/// The full-flash backup's filename: `canary-<day>-<n>-backup.bin`, the
/// local date and a counter — the browser's own scheme (flash-core.js
/// `fileStamp` + `backupFileName`), so a file saved by either flasher is the
/// `canary-…-backup.bin` both restore panels tell the user to look for.
///
/// Nothing of the board's goes in the name. It used to carry the MAC's last
/// six hex digits, but the base MAC is the station address in every Wi-Fi
/// frame the board sends, so any slice of it names the board to anything in
/// radio range, and a file name travels with the file. `None` when `day` is
/// not a `YYYY-MM-DD` date (the frontend computes it in the local zone), so a
/// path separator or a surprise can never reach the name.
pub fn backup_filename(day: &str, n: u32) -> Option<String> {
    if !is_local_day(day) || n == 0 {
        return None;
    }
    Some(format!("canary-{day}-{n}-backup.bin"))
}

/// `YYYY-MM-DD`, digits and dashes in exactly those places.
pub fn is_local_day(day: &str) -> bool {
    let b = day.as_bytes();
    b.len() == 10
        && b.iter().enumerate().all(|(i, c)| {
            if i == 4 || i == 7 {
                *c == b'-'
            } else {
                c.is_ascii_digit()
            }
        })
}

/// The first `canary-<day>-<n>-backup.bin` (n counting from 1) that `taken`
/// does not already hold — the automatic safety copies share one folder, so
/// the counter is what keeps two copies on one day apart.
pub fn next_backup_filename(day: &str, taken: &[String]) -> Option<String> {
    (1..=u32::MAX)
        .filter_map(|n| backup_filename(day, n))
        .find(|name| !taken.iter().any(|t| t == name))
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

/// The error a failed region read ends in: the exit code, the offset, and the
/// whole captured espflash log, with the board info's "MAC address: …" masked
/// ([`crate::flash::mask_hardware_address`]) — read-flash prints it right
/// after connecting, and this error is drawn on screen and kept in the app's
/// event log.
pub fn read_failure(code: i32, offset: u32, log: &str) -> String {
    format!(
        "espflash exited with code {code} reading 0x{offset:x}: {}",
        crate::flash::mask_hardware_address(log.trim())
    )
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
/// from the same board-info call that already names the chip, for the intake
/// MAC check and to know the same board again. The frontends never draw it
/// unless asked, and no saved file, report or file name carries it. None if
/// no MAC-shaped token.
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

/// The value of a `Label:  value` line of `espflash board-info` output, by its
/// label, case-insensitive (espflash 3.3.0 cli/mod.rs print_board_info).
fn board_info_value<'a>(board_info: &'a str, label: &str) -> Option<&'a str> {
    board_info.lines().find_map(|line| {
        let (k, v) = line.trim().split_once(':')?;
        k.trim().eq_ignore_ascii_case(label).then(|| v.trim())
    })
}

/// espflash's own chip line, e.g. "esp32s3 (revision v0.2)" — kept verbatim
/// for the technical view; `catalog::canonical_chip` stays the guard.
pub fn parse_chip_desc(board_info: &str) -> Option<String> {
    board_info_value(board_info, "chip type")
        .filter(|v| !v.is_empty())
        .map(str::to_string)
}

/// The silicon revision off the chip line ("v0.2"), or None when espflash
/// printed no revision.
pub fn parse_revision(board_info: &str) -> Option<String> {
    let line = board_info_value(board_info, "chip type")?;
    let at = line.find("revision v")? + "revision ".len();
    let rev: String = line[at..]
        .chars()
        .take_while(|c| *c == 'v' || *c == '.' || c.is_ascii_digit())
        .collect();
    let (major, minor) = rev.strip_prefix('v')?.split_once('.')?;
    (!major.is_empty() && !minor.is_empty()).then_some(rev)
}

/// "Crystal frequency: 40 MHz" → 40, the leading number only. For the S3, C3
/// and C6 espflash prints a fixed value rather than measuring
/// (`targets/<chip>.rs` `crystal_freq`), which is why the frontends draw the
/// crystal from the datasheet, not from this.
pub fn parse_crystal_mhz(board_info: &str) -> Option<u32> {
    let v = board_info_value(board_info, "crystal frequency")?;
    let digits: String = v.chars().take_while(char::is_ascii_digit).collect();
    digits.parse().ok()
}

/// "Features: WiFi, BLE" → ["WiFi", "BLE"]. Fixed strings per chip in
/// espflash 3.3.0, kept for the technical view only.
pub fn parse_features(board_info: &str) -> Vec<String> {
    board_info_value(board_info, "features")
        .map(|v| {
            v.split(',')
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
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
    fn a_failed_region_read_never_carries_the_address_espflash_printed() {
        // read-flash prints the board info, "MAC address: …" included, right
        // after connecting (espflash 3.3.0); the health check's error keeps
        // the whole captured log.
        let log = "Chip type:         esp32s3 (revision v0.2)\nMAC address:       dc:54:75:c1:22:30\nError: espflash::timeout\n";
        let err = read_failure(1, 0x8000, log);
        assert!(!err.contains("c1:22:30"), "{err}");
        assert_eq!(
            err,
            "espflash exited with code 1 reading 0x8000: Chip type:         esp32s3 (revision v0.2)\nMAC address:       [hardware address hidden]\nError: espflash::timeout"
        );
    }

    #[test]
    fn backup_filename_is_the_browsers_scheme_and_names_no_board() {
        // flash-core.js: backupFileName(fileStamp(day, n)) — the local date
        // and a counter. desktop_parity.test.js runs the browser's builder on
        // the same day and counter and pins the same name.
        assert_eq!(
            backup_filename("2026-10-22", 1).as_deref(),
            Some("canary-2026-10-22-1-backup.bin")
        );
        assert_eq!(
            backup_filename("2026-10-22", 12).as_deref(),
            Some("canary-2026-10-22-12-backup.bin")
        );
        // Anything that is not a plain date never reaches a file name: no
        // MAC, no path separator, no climbing out of the backups folder.
        for bad in [
            "dc:54:75:c1:22:30",
            "2026/10/22",
            "../../etc",
            "2026-10-2x",
            "",
            "2026-10-222",
        ] {
            assert_eq!(backup_filename(bad, 1), None, "{bad}");
        }
        assert_eq!(backup_filename("2026-10-22", 0), None);
        // Still the pattern both restore panels describe.
        let name = backup_filename("2026-01-05", 3).unwrap();
        assert!(name.starts_with("canary-") && name.ends_with("-backup.bin"));
    }

    #[test]
    fn the_next_backup_name_counts_past_the_ones_already_there() {
        let taken = vec![
            "canary-2026-10-22-1-backup.bin".to_string(),
            "canary-2026-10-22-2-backup.bin".to_string(),
            "canary-2026-10-21-3-backup.bin".to_string(),
        ];
        assert_eq!(
            next_backup_filename("2026-10-22", &taken).as_deref(),
            Some("canary-2026-10-22-3-backup.bin")
        );
        assert_eq!(
            next_backup_filename("2026-10-21", &taken).as_deref(),
            Some("canary-2026-10-21-1-backup.bin")
        );
        assert_eq!(next_backup_filename("not-a-day", &taken), None);
    }

    #[test]
    fn board_info_chip_line_crystal_and_features_parse() {
        // espflash 3.3.0's print_board_info, verbatim layout.
        let info = "[INFO] Serial port: '/dev/ttyACM0'\n[INFO] Connecting...\n\
                    Chip type:         esp32s3 (revision v0.2)\n\
                    Crystal frequency: 40 MHz\n\
                    Flash size:        8MB\n\
                    Features:          WiFi, BLE\n\
                    MAC address:       dc:54:75:c1:22:30\n";
        assert_eq!(
            parse_chip_desc(info).as_deref(),
            Some("esp32s3 (revision v0.2)")
        );
        assert_eq!(parse_revision(info).as_deref(), Some("v0.2"));
        assert_eq!(parse_crystal_mhz(info), Some(40));
        assert_eq!(parse_features(info), vec!["WiFi", "BLE"]);
        // A chip line with no revision, and absent lines, say nothing.
        let bare = "Chip type: esp32c6\nFeatures: WiFi 6, BT 5\n";
        assert_eq!(parse_revision(bare), None);
        assert_eq!(parse_crystal_mhz(bare), None);
        assert_eq!(parse_features(bare), vec!["WiFi 6", "BT 5"]);
        assert_eq!(parse_chip_desc("no chip here"), None);
        assert!(parse_features("").is_empty());
        assert_eq!(parse_revision("Chip type: esp32s3 (revision v)"), None);
        // A revision missing either half is no revision.
        assert_eq!(parse_revision("Chip type: esp32s3 (revision v.2)"), None);
        assert_eq!(parse_revision("Chip type: esp32s3 (revision v1.)"), None);
        // The crystal is the leading number; a later one is not appended.
        assert_eq!(
            parse_crystal_mhz("Crystal frequency: 26 MHz (not 40)"),
            Some(26)
        );
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
