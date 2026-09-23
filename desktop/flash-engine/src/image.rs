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
