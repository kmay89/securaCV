//! xz — stream the `.img.xz` into the raw image, hashing what comes out.
//!
//! The decompressed hash is the load-bearing output: it is what the post-write
//! read-back must reproduce from the disk itself, so "what we verified" and
//! "what we wrote" and "what the card actually holds" are one value checked
//! twice. Streaming (1 MiB in, 4 MiB out buffers) because a HAOS image is a
//! few hundred MB compressed and ~1–2 GB raw — never all in memory.

use crate::{sha256_hex, CancelToken, Progress, Stage};
use sha2::Digest;
use std::io::{Read, Write};
use std::path::Path;

/// What a completed decompression proved about the raw image.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawImage {
    /// Size of the decompressed image in bytes.
    pub bytes: u64,
    /// SHA-256 (lowercase hex) of the decompressed bytes — the value the
    /// post-write read-back must match.
    pub sha256: String,
}

/// Decompress `src` (an `.img.xz`) into `dest`, reporting
/// [`Stage::Decompress`] progress in *compressed* bytes consumed (that's the
/// known total; the raw size isn't knowable up front without trusting the xz
/// index). Returns the decompressed size + hash.
pub fn decompress(
    src: &Path,
    dest: &Path,
    cancel: &CancelToken,
    progress: impl FnMut(Progress),
) -> Result<RawImage, String> {
    let input = open_src(src)?;
    // The raw image carries whatever the caller seeds into it next (a Wi-Fi
    // passphrase, the owner account's hash, the maintenance key): it is born
    // owner-only (0600 on Unix) rather than created world-readable and
    // tightened later, and `create_new` refuses a file already at that
    // name — a symlink planted at the predictable path fails the open
    // instead of redirecting gigabytes somewhere else.
    let mut open = std::fs::OpenOptions::new();
    open.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        open.mode(0o600);
    }
    let out = open
        .open(dest)
        .map_err(|e| format!("couldn't create the raw image {}: {e}", dest.display()))?;
    stream_into(input, &out, cancel, progress)
}

/// Decompress `src` into `dest`, a file the caller has already created for
/// the raw image, and otherwise as [`decompress`] does. The Flasher's hub
/// writer creates its staging file itself (`tempfile`: exclusively, `O_EXCL`,
/// under a random name, 0600) so that it holds that file as its own for the
/// whole run, which [`decompress`]'s `create_new` would refuse. The open here
/// never creates and, on Linux and macOS, never follows a link at `dest`; a
/// name that is not a regular file is refused.
pub fn decompress_into(
    src: &Path,
    dest: &Path,
    cancel: &CancelToken,
    progress: impl FnMut(Progress),
) -> Result<RawImage, String> {
    let input = open_src(src)?;
    let out = open_owner_only(dest).map_err(|e| {
        format!(
            "couldn't open the raw image {} as a file only this account can read: {e}",
            dest.display()
        )
    })?;
    stream_into(input, &out, cancel, progress)
}

/// Open `dest`, which the caller made, for the raw image: written from the
/// start (emptied) and, on Unix, readable and writable by this account only
/// (0600). The image stops being public bytes the moment
/// [`crate::seed::inject_into_image`] writes the Wi-Fi passphrase and the
/// owner's bcrypt hash into it, and it sits in the staging dir (the OS temp
/// dir, on Linux the shared 1777 `/tmp`, when the app has no cache dir) for
/// the whole multi-GB write and read-back, so its mode is set before the
/// first byte lands. The file is tightened to 0600 through its own
/// descriptor and only then emptied, so one this account cannot chmod
/// (another account owns it) is refused untouched. What this cannot undo is
/// a reader that opened the file before it was tightened, so a caller
/// staging a secret hands in a file it just created owner-only and
/// exclusively, as the Flasher's hub writer does (`tempfile`, random name,
/// `O_EXCL`). Elsewhere the mode is the platform's default.
fn open_owner_only(dest: &Path) -> std::io::Result<std::fs::File> {
    let mut options = std::fs::OpenOptions::new();
    // Not truncated by the open: emptied below, once the mode is this
    // account's.
    options.write(true).truncate(false);
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    {
        use std::os::unix::fs::OpenOptionsExt;
        // No link followed, and no wait on a FIFO for a reader.
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    let file = options.open(dest)?;
    if !file.metadata()?.is_file() {
        return Err(std::io::Error::other("not a regular file"));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    }
    file.set_len(0)?;
    Ok(file)
}

/// The downloaded `.img.xz`, opened, and its size (the progress total).
fn open_src(src: &Path) -> Result<(std::fs::File, Option<u64>), String> {
    let compressed_total = std::fs::metadata(src).ok().map(|m| m.len());
    let file = std::fs::File::open(src)
        .map_err(|e| format!("couldn't open the downloaded image {}: {e}", src.display()))?;
    Ok((file, compressed_total))
}

/// Stream the opened `.img.xz` through the decoder into `out`, hashing what
/// comes out, then flush and sync it.
fn stream_into(
    (file, compressed_total): (std::fs::File, Option<u64>),
    out: &std::fs::File,
    cancel: &CancelToken,
    mut progress: impl FnMut(Progress),
) -> Result<RawImage, String> {
    let counted = CountingReader {
        inner: file,
        read: 0,
    };
    let mut decoder = xz2::read::XzDecoder::new(counted);
    let mut out = out;

    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 4 * 1024 * 1024];
    let mut raw_bytes: u64 = 0;
    loop {
        cancel.checkpoint()?;
        let n = decoder
            .read(&mut buf)
            .map_err(|e| format!("the .xz image is corrupt or truncated: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        out.write_all(&buf[..n])
            .map_err(|e| format!("couldn't write the raw image: {e}"))?;
        raw_bytes += n as u64;
        progress(Progress {
            stage: Stage::Decompress,
            done: decoder.get_ref().read,
            total: compressed_total,
        });
    }
    out.flush()
        .and_then(|_| out.sync_all())
        .map_err(|e| format!("couldn't finish writing the raw image: {e}"))?;
    if raw_bytes == 0 {
        return Err("the .xz image decompressed to nothing".to_string());
    }
    Ok(RawImage {
        bytes: raw_bytes,
        sha256: sha256_hex(hasher),
    })
}

/// A reader that counts what it hands out, so decompress progress can report
/// compressed-bytes-consumed (the total we actually know).
struct CountingReader<R> {
    inner: R,
    read: u64,
}

impl<R: Read> Read for CountingReader<R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.read += n as u64;
        Ok(n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn xz_bytes(raw: &[u8]) -> Vec<u8> {
        let mut enc = xz2::write::XzEncoder::new(Vec::new(), 6);
        enc.write_all(raw).unwrap();
        enc.finish().unwrap()
    }

    #[test]
    fn decompress_reproduces_the_raw_bytes_and_their_hash() {
        // Deterministic pseudo-random content, larger than one buffer round.
        let raw: Vec<u8> = (0..2_000_000u32)
            .map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8)
            .collect();
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("image.img.xz");
        let dest = dir.path().join("image.img");
        std::fs::write(&src, xz_bytes(&raw)).unwrap();

        let mut ticks = 0;
        let out = decompress(&src, &dest, &CancelToken::default(), |p| {
            assert_eq!(p.stage, Stage::Decompress);
            ticks += 1;
        })
        .expect("decompress succeeds");

        assert_eq!(out.bytes, raw.len() as u64);
        assert_eq!(std::fs::read(&dest).unwrap(), raw);
        let mut h = sha2::Sha256::new();
        h.update(&raw);
        assert_eq!(out.sha256, crate::sha256_hex(h));
        assert!(ticks > 0);
    }

    #[test]
    fn the_raw_image_is_private_and_never_written_over_an_existing_file() {
        let raw = vec![7u8; 10_000];
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("p.img.xz");
        std::fs::write(&src, xz_bytes(&raw)).unwrap();
        let dest = dir.path().join("p.img");
        decompress(&src, &dest, &CancelToken::default(), |_| {}).expect("fresh dest");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&dest).unwrap().permissions().mode() & 0o777;
            assert_eq!(
                mode, 0o600,
                "the raw image must be owner-only, got {mode:o}"
            );
        }
        // A file (or a planted link) already at the name is refused, not
        // overwritten or followed.
        let err = decompress(&src, &dest, &CancelToken::default(), |_| {}).unwrap_err();
        assert!(err.contains("couldn't create the raw image"), "{err}");
        assert_eq!(
            std::fs::read(&dest).unwrap(),
            raw,
            "the existing file is untouched"
        );
    }

    #[test]
    fn a_truncated_xz_is_a_loud_error() {
        let raw = vec![42u8; 100_000];
        let mut xz = xz_bytes(&raw);
        xz.truncate(xz.len() / 2);
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("t.img.xz");
        std::fs::write(&src, xz).unwrap();
        let err = decompress(
            &src,
            &dir.path().join("t.img"),
            &CancelToken::default(),
            |_| {},
        )
        .unwrap_err();
        assert!(err.contains("corrupt or truncated"), "{err}");
    }

    /// A dest that already exists with group and other bits (a caller that
    /// made it with the umask's default) is tightened to owner-only before
    /// the image goes in: `File::create` kept whatever mode the file had.
    #[cfg(unix)]
    #[test]
    fn an_existing_dest_is_tightened_to_owner_only() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("image.img.xz");
        let dest = dir.path().join("image.img");
        std::fs::write(&src, xz_bytes(&[7u8; 4096])).unwrap();
        std::fs::write(&dest, b"stale").unwrap();
        std::fs::set_permissions(&dest, std::fs::Permissions::from_mode(0o644)).unwrap();

        decompress_into(&src, &dest, &CancelToken::default(), |_| {}).expect("decompress succeeds");

        let mode = std::fs::metadata(&dest).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "raw image mode {mode:o}");
        assert_eq!(std::fs::read(&dest).unwrap(), vec![7u8; 4096]);
    }

    /// The open no longer truncates (the file is emptied only after its mode
    /// is set), so a stale file longer than the image must still end up
    /// exactly the image, with no stale tail for the write to carry onto a
    /// card.
    #[test]
    fn a_longer_stale_dest_ends_up_exactly_the_image() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("image.img.xz");
        let dest = dir.path().join("image.img");
        std::fs::write(&src, xz_bytes(&[7u8; 4096])).unwrap();
        std::fs::write(&dest, vec![0xEEu8; 3 * 4096]).unwrap();

        let out =
            decompress_into(&src, &dest, &CancelToken::default(), |_| {}).expect("decompress");

        assert_eq!(out.bytes, 4096);
        assert_eq!(std::fs::read(&dest).unwrap(), vec![7u8; 4096]);
    }

    /// `decompress_into` writes only into the file the caller made: it never
    /// creates one at a free name, and never follows a link planted at the
    /// name into the file it points at.
    #[test]
    fn decompress_into_never_creates_or_follows_a_link() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("image.img.xz");
        std::fs::write(&src, xz_bytes(&[7u8; 4096])).unwrap();
        let missing = dir.path().join("missing.img");
        let err = decompress_into(&src, &missing, &CancelToken::default(), |_| {}).unwrap_err();
        assert!(err.contains("couldn't open the raw image"), "{err}");
        assert!(!missing.exists(), "nothing is created");
        #[cfg(any(target_os = "linux", target_os = "macos"))]
        {
            let victim = dir.path().join("victim.txt");
            std::fs::write(&victim, b"victim").unwrap();
            let link = dir.path().join("link.img");
            std::os::unix::fs::symlink(&victim, &link).unwrap();
            let err = decompress_into(&src, &link, &CancelToken::default(), |_| {}).unwrap_err();
            assert!(err.contains("couldn't open the raw image"), "{err}");
            assert_eq!(std::fs::read(&victim).unwrap(), b"victim");
        }
    }

    #[test]
    fn garbage_input_is_a_loud_error() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("g.img.xz");
        std::fs::write(&src, b"this is not xz at all").unwrap();
        assert!(decompress(
            &src,
            &dir.path().join("g.img"),
            &CancelToken::default(),
            |_| {}
        )
        .is_err());
    }
}
