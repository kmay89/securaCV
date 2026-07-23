//! xz — stream the `.img.xz` into the raw image, hashing what comes out.
//!
//! The decompressed hash is the load-bearing output: it is what the post-write
//! read-back must reproduce from the disk itself, so "what we verified" and
//! "what we wrote" and "what the card actually holds" are one value checked
//! twice. Streaming (1 MiB in, 4 MiB out buffers) because a HAOS image is a
//! few hundred MB compressed and ~1–2 GB raw — never all in memory.

use crate::{sha256_hex, Progress, Stage};
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
    mut progress: impl FnMut(Progress),
) -> Result<RawImage, String> {
    let compressed_total = std::fs::metadata(src).ok().map(|m| m.len());
    let file = std::fs::File::open(src)
        .map_err(|e| format!("couldn't open the downloaded image {}: {e}", src.display()))?;
    let counted = CountingReader {
        inner: file,
        read: 0,
    };
    let mut decoder = xz2::read::XzDecoder::new(counted);
    let mut out = std::fs::File::create(dest)
        .map_err(|e| format!("couldn't create the raw image {}: {e}", dest.display()))?;

    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 4 * 1024 * 1024];
    let mut raw_bytes: u64 = 0;
    loop {
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
        let out = decompress(&src, &dest, |p| {
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
    fn a_truncated_xz_is_a_loud_error() {
        let raw = vec![42u8; 100_000];
        let mut xz = xz_bytes(&raw);
        xz.truncate(xz.len() / 2);
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("t.img.xz");
        std::fs::write(&src, xz).unwrap();
        let err = decompress(&src, &dir.path().join("t.img"), |_| {}).unwrap_err();
        assert!(err.contains("corrupt or truncated"), "{err}");
    }

    #[test]
    fn garbage_input_is_a_loud_error() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("g.img.xz");
        std::fs::write(&src, b"this is not xz at all").unwrap();
        assert!(decompress(&src, &dir.path().join("g.img"), |_| {}).is_err());
    }
}
