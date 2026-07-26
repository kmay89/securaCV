//! Pure decision logic for the native **rescue bench** — the triage tools that
//! turn the flasher from a one-shot writer into something that can save a board,
//! put a copy back, wipe it clean, and flash a local file. It mirrors the
//! browser Lab's rescue behaviour (canary-local: `validateBackupFile`, the
//! backup naming, the image-magic hints) so the two surfaces agree.
//!
//! Kept dependency-free (std only) so it unit-tests WITHOUT the desktop stack
//! (`rustc --test src/rescue.rs`), exactly like the `hub-core` crate. The Tauri
//! commands in `lib.rs` wrap these builders with the `espflash` sidecar and the
//! file dialogs — those are what a Mac / release-tag build validates end-to-end.

/// A safe, descriptive backup filename: `securacv-backup-<chip>-<mac>-<stamp>.bin`.
/// Every component is reduced to filename-safe characters so it can't produce a
/// path separator or a surprise. Empty components are dropped from the middle.
pub fn backup_filename(chip: &str, mac: &str, stamp: &str) -> String {
    let safe = |s: &str| -> String {
        let out: String = s
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '-' })
            .collect();
        // collapse runs of '-' and trim them, so "AA::BB" -> "AA-BB", not "AA--BB"
        let mut collapsed = String::with_capacity(out.len());
        let mut prev_dash = false;
        for c in out.chars() {
            if c == '-' {
                if !prev_dash { collapsed.push(c); }
                prev_dash = true;
            } else {
                collapsed.push(c);
                prev_dash = false;
            }
        }
        collapsed.trim_matches('-').to_string()
    };
    let parts: Vec<String> = ["securacv-backup", chip, mac, stamp]
        .iter()
        .map(|p| safe(p))
        .filter(|p| !p.is_empty())
        .collect();
    format!("{}.bin", parts.join("-"))
}

/// Validate a candidate restore/local image against the detected chip's flash
/// size, mirroring the browser's `validateBackupFile`:
///   * empty                     -> `Err` (nothing to write)
///   * larger than the chip flash-> `Err` (can't have come from this board)
///   * smaller than the chip     -> `Ok(Some(warning))` (written from 0x0, tail left)
///   * exactly the flash size,
///     or size unknown           -> `Ok(None)`
pub fn validate_restore_image(byte_len: u64, flash_bytes: Option<u64>) -> Result<Option<String>, String> {
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
        return Some("an ESP32 firmware image — 0xE9 is the chip's own \"program starts here\" marker");
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
        "read-flash".into(), "0x0".into(), size.to_string(), out.into(),
        "--port".into(), port.into(), "--baud".into(), baud.to_string(),
    ]
}

/// `write-bin 0x0 <path> --port <port> --baud <baud>` — restore or local flash.
/// Identical shape to the `flash` command's write step, so a restored image lands
/// exactly where a factory image would.
pub fn write_bin_args(port: &str, path: &str, baud: u32) -> Vec<String> {
    vec![
        "write-bin".into(), "0x0".into(), path.into(),
        "--port".into(), port.into(), "--baud".into(), baud.to_string(),
    ]
}

/// `erase-flash --port <port>` — wipe the whole chip for a truly clean install.
pub fn erase_flash_args(port: &str) -> Vec<String> {
    vec!["erase-flash".into(), "--port".into(), port.into()]
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
    fn backup_filename_is_safe_and_descriptive() {
        assert_eq!(
            backup_filename("ESP32-S3", "AA:BB:CC:DD:EE:FF", "20260726-1200"),
            "securacv-backup-ESP32-S3-AA-BB-CC-DD-EE-FF-20260726-1200.bin"
        );
        // Path-separator / space attempts collapse to '-', never escape the name.
        assert_eq!(
            backup_filename("es/p 32", "", "x"),
            "securacv-backup-es-p-32-x.bin"
        );
        assert!(!backup_filename("../etc", "m", "s").contains('/'));
    }

    #[test]
    fn validate_restore_matches_the_browser_contract() {
        assert!(validate_restore_image(0, Some(0x400000)).is_err()); // empty
        assert!(validate_restore_image(0x500000, Some(0x400000)).is_err()); // too big
        assert_eq!(validate_restore_image(0x400000, Some(0x400000)).unwrap(), None); // exact
        assert!(validate_restore_image(0x100000, Some(0x400000)).unwrap().is_some()); // smaller -> warn
        assert_eq!(validate_restore_image(0x1234, None).unwrap(), None); // size unknown -> allowed
    }

    #[test]
    fn image_hint_reads_the_chip_magic() {
        assert!(image_first_bytes_hint(&[0xE9, 0, 0]).unwrap().contains("firmware"));
        assert!(image_first_bytes_hint(&[0x50, 0xAA]).unwrap().contains("partition"));
        assert!(image_first_bytes_hint(&[0x32, 0x54, 0xCD, 0xAB]).unwrap().contains("description"));
        assert_eq!(image_first_bytes_hint(&[0x00, 0x01]), None);
        assert_eq!(image_first_bytes_hint(&[]), None);
    }

    #[test]
    fn espflash_args_mirror_the_flash_command() {
        assert_eq!(
            read_flash_args("/dev/tty.usb", 0x400000, "/tmp/b.bin", 921600),
            vec!["read-flash", "0x0", "4194304", "/tmp/b.bin", "--port", "/dev/tty.usb", "--baud", "921600"]
        );
        assert_eq!(
            write_bin_args("/dev/tty.usb", "/tmp/b.bin", 921600),
            vec!["write-bin", "0x0", "/tmp/b.bin", "--port", "/dev/tty.usb", "--baud", "921600"]
        );
        assert_eq!(erase_flash_args("/dev/tty.usb"), vec!["erase-flash", "--port", "/dev/tty.usb"]);
    }
}
