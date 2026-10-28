//! The image a flash hands to espflash: the offset-0 shape guard for a file
//! off the user's disk, and the private staging file every write goes
//! through.

/// A firmware image can't reasonably exceed the largest flash any Canary
/// carries (32 MiB parts exist; nothing bigger does). A file past this is a
/// wrong pick — a disk image, a video — not a firmware image, so the
/// local-file path refuses it before reading further.
pub const LOCAL_IMAGE_MAX_BYTES: u64 = 32 * 1024 * 1024;

/// The shape of a merged factory image: the ESP32 partition table lives at
/// 0x8000 on every variant, and its 32-byte entries open with the magic bytes
/// 0xAA 0x50 (u16le 0x50AA) — the same constants
/// firmware/scripts/make_factory.py merges by. This is what tells a factory
/// image apart from an app-only build: an app-only build starts with 0xE9,
/// and so does an S3/C3/C6 factory image (a classic ESP32's starts with 4 KB
/// of erased flash, its bootloader at 0x1000).
pub const PARTITION_TABLE_OFFSET: usize = 0x8000;
/// See [`PARTITION_TABLE_OFFSET`].
pub const PARTITION_MAGIC_LE: [u8; 2] = [0xAA, 0x50];

/// The first byte of every ESP image, bootloader or app.
pub const IMAGE_MAGIC: u8 = 0xE9;
/// Where an ESP image header names the chip it was built for
/// (esp_image_header_t.chip_id, a u16le).
pub const IMAGE_CHIP_ID_AT: usize = 12;

/// Where each chip's mask ROM loads the second-stage bootloader from, which is
/// where a factory image (a picture of flash from address 0) must hold it, and
/// the chip id a bootloader built for that chip names at [`IMAGE_CHIP_ID_AT`].
/// esptool's chip names, as the catalog's spelling folds to them ("ESP32-S3"
/// → "esp32s3"). The same table as firmware/scripts/make_factory.py's
/// ROM_BOOTLOADER and the browser's flash-core.js ROM_BOOTLOADER;
/// desktop_parity.test.js holds the three equal.
pub const ROM_BOOTLOADER: [(&str, usize, u16); 7] = [
    ("esp32", 0x1000, 0),
    ("esp32s2", 0x1000, 2),
    ("esp32s3", 0x0, 9),
    ("esp32c3", 0x0, 5),
    ("esp32c2", 0x0, 12),
    ("esp32c6", 0x0, 13),
    ("esp32h2", 0x0, 16),
];

/// A detected chip's (ROM bootloader offset, image chip id), or `None` for a
/// chip the table does not list (the P4 and the C5 load from 0x2000).
pub fn rom_bootloader(chip: &str) -> Option<(usize, u16)> {
    let token = chip.to_lowercase().replace(['-', ' ', '_'], "");
    ROM_BOOTLOADER
        .iter()
        .find(|(name, _, _)| *name == token)
        .map(|&(_, at, chip_id)| (at, chip_id))
}

/// Stage a firmware image in an atomically-created, randomly named private
/// file. `NamedTempFile` creates mode 0600 on Unix and removes the file on
/// drop, so a provisioned image never passes through a world-readable path.
pub fn stage_firmware(bytes: &[u8], safe_id: &str) -> Result<tempfile::NamedTempFile, String> {
    use std::io::Write;

    let mut staged = tempfile::Builder::new()
        .prefix(&format!("securacv-{safe_id}-"))
        .suffix(".bin")
        .tempfile()
        .map_err(|e| format!("couldn't create private firmware staging file: {e}"))?;
    staged
        .write_all(bytes)
        .and_then(|_| staged.flush())
        .map_err(|e| format!("couldn't stage the image: {e}"))?;
    Ok(staged)
}

/// Read the safety copy a flash draws its change map (and its counterfeit-
/// capacity check) from — but only a path with the shape a real safety copy
/// has. `flash` is reachable straight from the webview, and its
/// `backup_path` used to go to `std::fs::read` unchecked: any path, read
/// whole into memory (`/dev/zero` never ends), its bytes reflected back
/// through the intake refusal and the change-map rows. So the path gets the
/// Flasher's own save-path rules (`validated_backup_path`: absolute, `.bin`,
/// canonicalized so `..` and symlinks resolve before anything is judged)
/// plus what a read needs: the resolved file is still a `.bin`, a regular
/// file (never a device or a directory), and no larger than any Canary's
/// flash — the read itself is capped too, so a file growing under us can't
/// get past it. Anything else is `None`: the same honest "no map" an
/// unreadable copy gets, and the install never depends on it.
pub fn read_safety_copy(path: &str) -> Option<Vec<u8>> {
    use std::io::Read;

    let is_bin = |p: &std::path::Path| {
        p.extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e.eq_ignore_ascii_case("bin"))
    };
    let asked = std::path::Path::new(path);
    if !asked.is_absolute() || !is_bin(asked) {
        return None;
    }
    let real = asked.canonicalize().ok()?;
    if !is_bin(&real) {
        return None; // a `.bin` name pointing at something that isn't one
    }
    let meta = std::fs::metadata(&real).ok()?;
    if !meta.is_file() || meta.len() > LOCAL_IMAGE_MAX_BYTES {
        return None;
    }
    let mut bytes = Vec::with_capacity(meta.len() as usize);
    std::fs::File::open(&real)
        .ok()?
        .take(LOCAL_IMAGE_MAX_BYTES + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    (bytes.len() as u64 <= LOCAL_IMAGE_MAX_BYTES).then_some(bytes)
}

/// The cheap refusals for a user-picked firmware file: an empty file or one
/// larger than any Canary's flash is a wrong pick, and a file without a
/// partition table at 0x8000 is an app-only build — this path writes whole
/// factory images at offset 0, so an app-only .bin would land on the
/// bootloader and the board wouldn't boot (recoverable over USB download
/// mode, but a guaranteed bad hour). The 0xE9 image magic can't make that
/// call — an app-only build starts with 0xE9 too. Anything subtler (a real
/// factory image for the wrong board) is on the user — a personal file has
/// no catalog entry to check it against.
pub fn check_local_image(bytes: &[u8]) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("that file is empty — there's nothing to write".into());
    }
    if bytes.len() as u64 > LOCAL_IMAGE_MAX_BYTES {
        return Err(format!(
            "that file is {} bytes — no Canary carries more than 32 MiB of flash, so this can't be a firmware image",
            bytes.len()
        ));
    }
    let factory_shape = bytes.len() > PARTITION_TABLE_OFFSET + 32
        && bytes[PARTITION_TABLE_OFFSET..PARTITION_TABLE_OFFSET + 2] == PARTITION_MAGIC_LE;
    if !factory_shape {
        return Err(
            "this looks like an app-only build, not a merged factory image — there's no \
             partition table at 0x8000. The flasher writes whole factory images at offset 0, \
             so an app-only .bin would overwrite the bootloader and the board wouldn't boot. \
             Merge one with firmware/scripts/make_factory.py or use `dev_flash.sh <env> -f`."
                .into(),
        );
    }
    Ok(())
}

/// The chip-aware half of the factory-shape gate, for every write at 0x0 a
/// flash makes (the catalog image and a local file alike): the image must
/// carry a bootloader built for the connected chip (0xE9, and the chip's id in
/// the header) where that chip's ROM loads it from. fw-v2.4.17's ESP32-CAM and
/// WROOM images carried theirs at 0x0, where a classic ESP32 never looks, and
/// the partition-table check passed them; the chip id keeps a stray 0xE9 byte
/// at the ROM offset, or another chip's image with the same offset, from
/// passing. A chip the table does not list is not judged, so this can only
/// refuse an image the ROM would not start. The browser's twin is
/// flash-core.js bootloaderPlacement, in the same words.
pub fn check_bootloader_offset(bytes: &[u8], chip: &str) -> Result<(), String> {
    let Some((at, chip_id)) = rom_bootloader(chip) else {
        return Ok(());
    };
    // The chip id named by an image header at `o`, or `None` when none starts there.
    let id_at = |o: usize| match bytes.get(o..o + IMAGE_CHIP_ID_AT + 2) {
        Some(header) if header[0] == IMAGE_MAGIC => Some(u16::from_le_bytes([
            header[IMAGE_CHIP_ID_AT],
            header[IMAGE_CHIP_ID_AT + 1],
        ])),
        _ => None,
    };
    if id_at(at) == Some(chip_id) {
        return Ok(());
    }
    let other = [0x0, 0x1000]
        .into_iter()
        .find(|&o| o != at && id_at(o) == Some(chip_id));
    let elsewhere = match other {
        Some(o) => format!(" (it has one at {o:#x} instead)"),
        None if id_at(at).is_some() => " (the one there was built for another chip)".into(),
        None => String::new(),
    };
    Err(format!(
        "This image would not start on the {chip}: that chip loads its bootloader from {at:#x}, \
         and the image has no bootloader for it there{elsewhere}. Nothing was written. Use an \
         image laid out for the {chip}: SecuraCV factory images from fw-v2.4.18 on put the \
         bootloader where each chip loads it."
    ))
}

#[cfg(test)]
mod local_image_tests {
    use super::{
        check_bootloader_offset, check_local_image, rom_bootloader, IMAGE_CHIP_ID_AT, IMAGE_MAGIC,
        PARTITION_MAGIC_LE, PARTITION_TABLE_OFFSET,
    };

    #[test]
    fn app_only_builds_are_refused_and_factory_shapes_pass() {
        assert!(check_local_image(&[]).is_err());
        // An app-only PlatformIO build starts with 0xE9 too — the refusal
        // must come from the missing partition table, not the image magic.
        let app_only = vec![0xE9; PARTITION_TABLE_OFFSET / 2];
        assert!(check_local_image(&app_only).is_err());
        // Right length, no 0xAA 0x50 at 0x8000 → still not a factory image.
        let unmerged = vec![0xE9; PARTITION_TABLE_OFFSET + 64];
        assert!(check_local_image(&unmerged).is_err());
        let mut factory = vec![0xFF; PARTITION_TABLE_OFFSET + 64];
        factory[0] = 0xE9;
        factory[PARTITION_TABLE_OFFSET] = 0xAA;
        factory[PARTITION_TABLE_OFFSET + 1] = 0x50;
        assert!(check_local_image(&factory).is_ok());
    }

    /// A factory image with a bootloader for chip id `chip_id` at `at` and the
    /// partition table at 0x8000, everything else erased flash.
    fn factory_with_bootloader_at(at: usize, chip_id: u16) -> Vec<u8> {
        let mut image = vec![0xFF; PARTITION_TABLE_OFFSET + 64];
        image[at] = IMAGE_MAGIC;
        image[at + IMAGE_CHIP_ID_AT..at + IMAGE_CHIP_ID_AT + 2]
            .copy_from_slice(&chip_id.to_le_bytes());
        image[PARTITION_TABLE_OFFSET..PARTITION_TABLE_OFFSET + 2]
            .copy_from_slice(&PARTITION_MAGIC_LE);
        image
    }

    #[test]
    fn the_bootloader_must_sit_where_the_chips_rom_loads_it() {
        // Correct images pass, in the spellings both the catalog and
        // espflash use.
        for (chip, at, id) in [
            ("ESP32", 0x1000, 0),
            ("esp32", 0x1000, 0),
            ("ESP32-S2", 0x1000, 2),
            ("ESP32-S3", 0x0, 9),
            ("ESP32-C3", 0x0, 5),
            ("ESP32-C6", 0x0, 13),
            ("esp32c2", 0x0, 12),
            ("ESP32-H2", 0x0, 16),
        ] {
            let image = factory_with_bootloader_at(at, id);
            assert!(check_bootloader_offset(&image, chip).is_ok(), "{chip}");
        }
        // fw-v2.4.17's classic-ESP32 shape: the bootloader at 0x0, nothing
        // at 0x1000. Refused, and the words say where it is and what to use.
        let misplaced = factory_with_bootloader_at(0x0, 0);
        let err = check_bootloader_offset(&misplaced, "ESP32").unwrap_err();
        assert!(err.contains("loads its bootloader from 0x1000"), "{err}");
        assert!(err.contains("(it has one at 0x0 instead)"), "{err}");
        assert!(err.contains("Nothing was written"), "{err}");
        assert!(err.contains("fw-v2.4.18"), "{err}");
        // …and when its bootloader's body happens to hold 0xE9 at 0x1000: a
        // stray byte there is not a header naming the ESP32.
        let mut stray = misplaced.clone();
        stray[0x1000] = IMAGE_MAGIC;
        stray[0x1000 + IMAGE_CHIP_ID_AT] = 0x3a;
        let err = check_bootloader_offset(&stray, "ESP32").unwrap_err();
        assert!(err.contains("(it has one at 0x0 instead)"), "{err}");
        // A classic image on an S3: its bootloader at 0x1000 is an ESP32's.
        let classic = factory_with_bootloader_at(0x1000, 0);
        let err = check_bootloader_offset(&classic, "ESP32-S3").unwrap_err();
        assert!(err.contains("loads its bootloader from 0x0,"), "{err}");
        assert!(!err.contains("instead"), "{err}");
        // An S3 image on a C3: the right place, another chip's bootloader.
        let s3 = factory_with_bootloader_at(0x0, 9);
        let err = check_bootloader_offset(&s3, "ESP32-C3").unwrap_err();
        assert!(
            err.contains("(the one there was built for another chip)"),
            "{err}"
        );
        // No magic anywhere: refused, with nothing to add.
        let blank = vec![0xFF; PARTITION_TABLE_OFFSET + 64];
        let err = check_bootloader_offset(&blank, "ESP32-C6").unwrap_err();
        assert!(
            err.contains("has no bootloader for it there. Nothing"),
            "{err}"
        );
        // A chip the table doesn't list is never judged.
        assert!(check_bootloader_offset(&blank, "ESP32-P4").is_ok());
        assert_eq!(rom_bootloader("ESP32-C5"), None);
    }
}

#[cfg(test)]
mod staging_tests {
    use super::stage_firmware;

    #[test]
    fn staged_firmware_is_private_and_removed_on_drop() {
        let staged = stage_firmware(b"provisioned-secret-image", "canary-vision")
            .expect("private staging file");
        let path = staged.path().to_path_buf();
        assert_eq!(
            std::fs::read(&path).expect("staged bytes"),
            b"provisioned-secret-image"
        );

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&path)
                .expect("staging metadata")
                .permissions()
                .mode();
            assert_eq!(
                mode & 0o077,
                0,
                "staging file must not be group/world accessible"
            );
        }

        drop(staged);
        assert!(!path.exists(), "staging path must be removed on drop");
    }
}

#[cfg(test)]
mod safety_copy_tests {
    use super::{read_safety_copy, LOCAL_IMAGE_MAX_BYTES};

    #[test]
    fn only_a_real_bin_safety_copy_is_read() {
        let dir = tempfile::tempdir().unwrap();
        let s = |p: &std::path::Path| p.to_string_lossy().into_owned();

        // The shape the Flasher's safety copy has: absolute, `.bin`, a file.
        let good = dir.path().join("canary-dc-54-75-1700000000.bin");
        std::fs::write(&good, b"the whole chip").unwrap();
        assert_eq!(
            read_safety_copy(&s(&good)).as_deref(),
            Some(&b"the whole chip"[..])
        );
        // Extension case doesn't matter (validated_chosen_path agrees).
        let upper = dir.path().join("COPY.BIN");
        std::fs::write(&upper, b"x").unwrap();
        assert!(read_safety_copy(&s(&upper)).is_some());

        // Relative, not a .bin, missing, a directory: no read, no map.
        assert!(read_safety_copy("copy.bin").is_none());
        let txt = dir.path().join("notes.txt");
        std::fs::write(&txt, b"secret").unwrap();
        assert!(read_safety_copy(&s(&txt)).is_none());
        assert!(read_safety_copy(&s(&dir.path().join("gone.bin"))).is_none());
        let folder = dir.path().join("folder.bin");
        std::fs::create_dir(&folder).unwrap();
        assert!(read_safety_copy(&s(&folder)).is_none());
        // `..` resolves before the judgment, and still lands on a .bin here.
        let dotted = dir
            .path()
            .join("folder.bin/../canary-dc-54-75-1700000000.bin");
        assert!(read_safety_copy(&s(&dotted)).is_some());

        // Larger than any Canary's flash (sparse, so it costs no disk).
        let huge = dir.path().join("huge.bin");
        std::fs::File::create(&huge)
            .unwrap()
            .set_len(LOCAL_IMAGE_MAX_BYTES + 1)
            .unwrap();
        assert!(read_safety_copy(&s(&huge)).is_none());

        #[cfg(unix)]
        {
            // A `.bin` NAME is not enough: a link onto a device (which never
            // ends) or onto a file that isn't a .bin is refused once resolved.
            let zero = dir.path().join("zero.bin");
            std::os::unix::fs::symlink("/dev/zero", &zero).unwrap();
            assert!(read_safety_copy(&s(&zero)).is_none());
            let sneaky = dir.path().join("sneaky.bin");
            std::os::unix::fs::symlink(&txt, &sneaky).unwrap();
            assert!(read_safety_copy(&s(&sneaky)).is_none());
            // A link onto a real .bin is fine — it IS the copy.
            let alias = dir.path().join("alias.bin");
            std::os::unix::fs::symlink(&good, &alias).unwrap();
            assert!(read_safety_copy(&s(&alias)).is_some());
        }
    }
}
