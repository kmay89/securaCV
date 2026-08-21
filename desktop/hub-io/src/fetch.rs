//! fetch — stream the image download to disk, hashing as it arrives.
//!
//! Two jobs, both feeding `hub_core::hub_image::verify_download` (the only
//! authority on trust):
//!
//!   * [`download`] streams a URL to a destination file while computing the
//!     SHA-256 of every byte written — no "download, then re-read to hash"
//!     window where the file could differ from what was hashed.
//!   * [`fetch_published_sha256`] retrieves the small `.sha256` file HA
//!     publishes next to each image, for the unpinned fallback. A failure here
//!     is `Ok(None)`, not an error — *whether* a missing checksum is fatal is
//!     `verify_download`'s call (pinned: irrelevant; unpinned: refuse), not
//!     this layer's.
//!
//! No redirect surprises: ureq follows redirects internally but only over
//! https for https origins; the image URL itself is already validated by the
//! caller against the bundled catalog before it ever reaches here.

use crate::{sha256_hex, CancelToken, Progress, Stage};
use sha2::Digest;
use std::io::{Read, Write};
use std::path::Path;

/// What a completed download proved about itself.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DownloadReceipt {
    /// Bytes written to the destination file.
    pub bytes: u64,
    /// SHA-256 (lowercase hex) of exactly those bytes.
    pub sha256: String,
}

fn agent() -> ureq::Agent {
    ureq::Agent::config_builder()
        .user_agent("SecuraCV-Flasher")
        .timeout_connect(Some(std::time::Duration::from_secs(15)))
        // No overall/body timeout: a HAOS image is hundreds of MB and a slow
        // link is not an error. The response headers must still arrive
        // promptly; a dead socket mid-body is left to TCP to detect (ureq 3
        // has no per-read timeout, only whole-body deadlines we can't use).
        .timeout_recv_response(Some(std::time::Duration::from_secs(60)))
        .build()
        .into()
}

/// The suffix a download-in-progress wears until it's complete. A crash or a
/// pulled plug leaves one of these, never a file at `dest` that only looks
/// finished — the atomic rename below is the whole point.
pub const PARTIAL_SUFFIX: &str = ".partial";

/// Sibling path `dest` + [`PARTIAL_SUFFIX`], where a download streams before
/// it earns the real name.
pub fn partial_path(dest: &Path) -> std::path::PathBuf {
    let mut s = dest.as_os_str().to_os_string();
    s.push(PARTIAL_SUFFIX);
    std::path::PathBuf::from(s)
}

/// Stream `url` into `dest` **atomically**: bytes go to a `<dest>.partial`
/// sibling, and only a fully-downloaded, size-checked file is `rename`d onto
/// `dest` (an atomic op on the same filesystem). So a pulled plug, a killed
/// app, or a dropped connection never leaves a truncated file wearing the real
/// name — it leaves a `.partial` orphan that the startup sweep tidies and the
/// cache logic ignores. Reports [`Stage::Download`] progress; returns the byte
/// count + SHA-256 of what landed.
pub fn download(
    url: &str,
    dest: &Path,
    cancel: &CancelToken,
    mut progress: impl FnMut(Progress),
) -> Result<DownloadReceipt, String> {
    cancel.checkpoint()?;
    let response = agent()
        .get(url)
        .call()
        .map_err(|e| format!("download failed: {e}"))?;
    let total = response
        .headers()
        .get("Content-Length")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.trim().parse::<u64>().ok());

    let tmp = partial_path(dest);
    // `into_reader` is deliberately unlimited — the convenience readers cap
    // at 10MB and an image is far bigger than that.
    let mut reader = response.into_body().into_reader();
    let mut file = std::fs::File::create(&tmp)
        .map_err(|e| format!("couldn't create {}: {e}", tmp.display()))?;

    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 1024 * 1024];
    let mut done: u64 = 0;
    loop {
        cancel.checkpoint()?;
        let n = reader
            .read(&mut buf)
            .map_err(|e| format!("download interrupted after {done} bytes: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        file.write_all(&buf[..n])
            .map_err(|e| format!("couldn't write the download to disk: {e}"))?;
        done += n as u64;
        progress(Progress {
            stage: Stage::Download,
            done,
            total,
        });
    }
    file.flush()
        .and_then(|_| file.sync_all())
        .map_err(|e| format!("couldn't finish writing the download: {e}"))?;

    // A server that promised a size and delivered another is a broken/truncated
    // transfer even if the socket closed cleanly. Leave the .partial for the
    // sweep; never promote it.
    if let Some(t) = total {
        if done != t {
            return Err(format!(
                "download ended early: got {done} bytes of a promised {t}"
            ));
        }
    }

    // Earn the real name only now that the bytes are all here and on the medium.
    std::fs::rename(&tmp, dest)
        .map_err(|e| format!("couldn't finalize the download to {}: {e}", dest.display()))?;

    Ok(DownloadReceipt {
        bytes: done,
        sha256: sha256_hex(hasher),
    })
}

/// Hash an existing file's bytes with SHA-256, checking `cancel` between
/// chunks. This is how a *cached* image earns the same trust as a fresh
/// download: the cached bytes are re-hashed and fed to `verify_download`, so
/// reusing a local copy skips the network but never the verification.
pub fn sha256_file(path: &Path, cancel: &CancelToken) -> Result<String, String> {
    let mut file = std::fs::File::open(path)
        .map_err(|e| format!("couldn't open the cached image {}: {e}", path.display()))?;
    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 4 * 1024 * 1024];
    loop {
        cancel.checkpoint()?;
        let n = file
            .read(&mut buf)
            .map_err(|e| format!("couldn't read the cached image: {e}"))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(sha256_hex(hasher))
}

/// The largest plausible `.sha256` file — a hash line plus filename. Anything
/// bigger is not a checksum and is not worth reading.
const SHA256_FILE_MAX: usize = 4096;

/// Fetch HA's published checksum that ships next to the image
/// (`<image_url>.sha256`). Returns `Ok(None)` when it can't be fetched or is
/// oversized — the verify decision downstream turns that into "refuse to
/// write" for an unpinned catalog, which is the correct fate, with the pinned
/// path unaffected.
pub fn fetch_published_sha256(image_url: &str) -> Result<Option<String>, String> {
    let url = format!("{image_url}.sha256");
    let response = match agent().get(&url).call() {
        Ok(r) => r,
        Err(_) => return Ok(None),
    };
    let mut text = String::new();
    let mut limited = response
        .into_body()
        .into_reader()
        .take(SHA256_FILE_MAX as u64 + 1);
    if limited.read_to_string(&mut text).is_err() {
        return Ok(None);
    }
    if text.len() > SHA256_FILE_MAX {
        return Ok(None);
    }
    let trimmed = text.trim().to_string();
    Ok((!trimmed.is_empty()).then_some(trimmed))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// A one-request loopback HTTP server: accept, read the request head, send
    /// the canned response. Tiny on purpose — enough to test the streaming
    /// download path against real sockets without any network.
    fn serve_once(status: &'static str, headers: Vec<String>, body: Vec<u8>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        std::thread::spawn(move || {
            let (mut sock, _) = listener.accept().expect("accept");
            // Read until the blank line ending the request head.
            let mut req = Vec::new();
            let mut byte = [0u8; 1];
            while !req.ends_with(b"\r\n\r\n") {
                use std::io::Read as _;
                if sock.read(&mut byte).unwrap_or(0) == 0 {
                    break;
                }
                req.push(byte[0]);
            }
            let mut head = format!("HTTP/1.1 {status}\r\nConnection: close\r\n");
            for h in headers {
                head.push_str(&h);
                head.push_str("\r\n");
            }
            head.push_str("\r\n");
            let _ = sock.write_all(head.as_bytes());
            let _ = sock.write_all(&body);
        });
        format!("http://{addr}")
    }

    #[test]
    fn download_streams_hashes_and_reports_progress() {
        let body: Vec<u8> = (0..100_000u32).flat_map(|i| i.to_le_bytes()).collect();
        let url = serve_once(
            "200 OK",
            vec![format!("Content-Length: {}", body.len())],
            body.clone(),
        );
        let dir = tempfile::tempdir().unwrap();
        let dest = dir.path().join("image.xz");
        let mut ticks = Vec::new();
        let receipt = download(&url, &dest, &CancelToken::default(), |p| ticks.push(p))
            .expect("download succeeds");

        assert_eq!(receipt.bytes, body.len() as u64);
        assert_eq!(std::fs::read(&dest).unwrap(), body);
        // Atomic: the finished file wears the real name; no .partial left behind.
        assert!(
            !partial_path(&dest).exists(),
            "partial must be renamed away"
        );
        // The receipt's hash is the hash of the file's actual bytes.
        let mut h = sha2::Sha256::new();
        h.update(&body);
        assert_eq!(receipt.sha256, crate::sha256_hex(h));
        // Progress reached the end and knew the total.
        let last = ticks.last().expect("at least one tick");
        assert_eq!(last.done, body.len() as u64);
        assert_eq!(last.total, Some(body.len() as u64));
        assert_eq!(last.stage, Stage::Download);
    }

    #[test]
    fn a_truncated_download_is_an_error_and_never_promotes_the_partial() {
        // Promise more than we send: the client must refuse the short file AND
        // leave no file at the real name (only the .partial, for the sweep).
        let body = vec![7u8; 1000];
        let url = serve_once("200 OK", vec!["Content-Length: 2000".to_string()], body);
        let dir = tempfile::tempdir().unwrap();
        let dest = dir.path().join("x");
        let err = download(&url, &dest, &CancelToken::default(), |_| {}).unwrap_err();
        assert!(!err.is_empty());
        // The real name must NOT exist — a truncated transfer never earns it.
        assert!(
            !dest.exists(),
            "a truncated download must not create the real file"
        );
    }

    #[test]
    fn an_http_error_status_fails_the_download() {
        let url = serve_once(
            "404 Not Found",
            vec!["Content-Length: 0".to_string()],
            vec![],
        );
        let dir = tempfile::tempdir().unwrap();
        let err =
            download(&url, &dir.path().join("x"), &CancelToken::default(), |_| {}).unwrap_err();
        assert!(err.contains("download failed"), "{err}");
    }

    #[test]
    fn published_sha256_returns_the_file_text() {
        let text =
            b"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  haos.img.xz\n";
        let url = serve_once(
            "200 OK",
            vec![format!("Content-Length: {}", text.len())],
            text.to_vec(),
        );
        // fetch_published_sha256 appends ".sha256" to the image URL; give it a
        // URL with a path so the suffix lands on the path (the tiny server
        // ignores the path entirely).
        let got = fetch_published_sha256(&format!("{url}/haos.img.xz"))
            .unwrap()
            .expect("some checksum text");
        assert!(got.starts_with("0123456789abcdef"));
        assert!(got.ends_with("haos.img.xz"));
    }

    #[test]
    fn a_missing_published_sha256_is_none_not_an_error() {
        let url = serve_once(
            "404 Not Found",
            vec!["Content-Length: 0".to_string()],
            vec![],
        );
        assert_eq!(fetch_published_sha256(&url).unwrap(), None);
    }

    #[test]
    fn sha256_file_matches_a_streamed_download_of_the_same_bytes() {
        // The cache-trust invariant: hashing a file on disk yields exactly the
        // hash a fresh download would have produced, so a cached image can be
        // re-verified byte-for-byte instead of re-downloaded.
        let body: Vec<u8> = (0..500_000u32).flat_map(|i| i.to_le_bytes()).collect();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("cached.xz");
        std::fs::write(&path, &body).unwrap();
        let mut h = sha2::Sha256::new();
        h.update(&body);
        assert_eq!(
            sha256_file(&path, &CancelToken::default()).unwrap(),
            crate::sha256_hex(h)
        );
    }
}
