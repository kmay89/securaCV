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
/// image apart from an app-only build, because BOTH start with 0xE9.
pub const PARTITION_TABLE_OFFSET: usize = 0x8000;
/// See [`PARTITION_TABLE_OFFSET`].
pub const PARTITION_MAGIC_LE: [u8; 2] = [0xAA, 0x50];

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

#[cfg(test)]
mod local_image_tests {
    use super::{check_local_image, PARTITION_TABLE_OFFSET};

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
