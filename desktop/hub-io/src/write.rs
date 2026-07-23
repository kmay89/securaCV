//! write — the destructive act, and the proof it went right.
//!
//! The entry point [`write_image`] takes `hub_core::hub_flash::
//! WriteAuthorization` **by value**. There is exactly one way to mint one
//! (`authorize_write`: verified image + still-eligible target + operator
//! confirmation), so an unauthorized write is not a bug this crate can
//! contain — it's a compile error in the caller.
//!
//! The write itself is deliberately boring: stream the raw image to the device
//! in 4 MiB chunks, `sync_all`, then — the part that earns trust — seek back
//! to zero and re-read every written byte off the device, hash it, and demand
//! it equal the image hash the verify chain produced. A card that lies about
//! writes (the classic counterfeit-SD failure) fails here, at the desk, not
//! three weeks later in the field.
//!
//! Platform edges are thin and separated: `open_target` turns the
//! authorized device path into an exclusive read/write handle (Linux
//! `O_EXCL`; macOS raw `rdisk` + `diskutil` unmount, with Apple's `authopen`
//! for privilege), and everything after that runs through the host-tested
//! [`write_verified`] core, which knows nothing about devices — any
//! `Read+Write+Seek` will do, which is exactly how the tests drive it against
//! plain files.

use crate::{sha256_hex, Progress, Stage};
use hub_core::hub_flash::WriteAuthorization;
use sha2::Digest;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// Chunk size for both the write and the read-back. 4 MiB aligns with every
/// SD/SSD erase-block size that matters and keeps syscall overhead irrelevant.
const CHUNK: usize = 4 * 1024 * 1024;

/// What a completed, read-back-verified write proved.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WriteReceipt {
    pub board_id: String,
    pub os_label: String,
    pub target_path: String,
    pub bytes_written: u64,
    /// SHA-256 the read-back reproduced from the device — equal, by
    /// construction, to the decompressed image hash from the verify chain.
    pub sha256: String,
}

/// Write the raw image behind `authz` to its authorized target device and
/// read-back verify it. `raw_image` is the decompressed image file;
/// `raw_sha256` is its hash from [`crate::xz::decompress`] — the value the
/// device must reproduce.
pub fn write_image(
    authz: WriteAuthorization,
    raw_image: &Path,
    raw_sha256: &str,
    mut progress: impl FnMut(Progress),
) -> Result<WriteReceipt, String> {
    let mut device = open_target(authz.target_path())?;
    let image = std::fs::File::open(raw_image)
        .map_err(|e| format!("couldn't open the raw image {}: {e}", raw_image.display()))?;
    let bytes = image
        .metadata()
        .map_err(|e| format!("couldn't stat the raw image: {e}"))?
        .len();
    let sha = write_verified(&mut device, image, bytes, raw_sha256, &mut progress)?;
    Ok(WriteReceipt {
        board_id: authz.plan().board_id.clone(),
        os_label: authz.plan().os_label.clone(),
        target_path: authz.target_path().to_string(),
        bytes_written: bytes,
        sha256: sha,
    })
}

/// The device-agnostic core: stream `image` into `device`, sync, then re-read
/// the same span off `device` and require its hash to equal `expected_sha256`
/// (case-insensitively, same as the rest of the trust chain). Returns the
/// read-back hash. Host-tested against plain files.
/// Force written bytes down to the medium before the read-back, and drop any
/// cached pages so verification reads the device rather than the kernel's
/// write/page cache. `write_verified` stays generic so the host tests can
/// drive it with cursors and plain files; real block devices get the strict
/// behavior via the `File` impl.
pub trait SyncStorage {
    fn sync_storage(&mut self) -> std::io::Result<()>;
    /// Best-effort page-cache invalidation for the span about to be re-read.
    fn drop_read_cache(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl<T> SyncStorage for std::io::Cursor<T> {
    fn sync_storage(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl SyncStorage for std::fs::File {
    fn sync_storage(&mut self) -> std::io::Result<()> {
        // fsync, not flush: flush() is a no-op for File and leaves the bytes
        // in the kernel cache, where a counterfeit card's lies are invisible.
        self.sync_all()
    }
    #[cfg(target_os = "linux")]
    fn drop_read_cache(&mut self) -> std::io::Result<()> {
        use std::os::unix::io::AsRawFd;
        // Evict this handle's clean pages so the verify pass re-reads the
        // medium instead of being satisfied from cache.
        let rc =
            unsafe { libc::posix_fadvise(self.as_raw_fd(), 0, 0, libc::POSIX_FADV_DONTNEED) };
        if rc == 0 {
            Ok(())
        } else {
            Err(std::io::Error::from_raw_os_error(rc))
        }
    }
    #[cfg(target_os = "macos")]
    fn drop_read_cache(&mut self) -> std::io::Result<()> {
        use std::os::unix::io::AsRawFd;
        // Raw rdisk I/O is already uncached; F_NOCACHE covers the /dev/disk
        // and regular-file cases.
        let rc = unsafe { libc::fcntl(self.as_raw_fd(), libc::F_NOCACHE, 1) };
        if rc == -1 {
            Err(std::io::Error::last_os_error())
        } else {
            Ok(())
        }
    }
}

pub fn write_verified<D: Read + Write + Seek + SyncStorage>(
    device: &mut D,
    mut image: impl Read,
    image_bytes: u64,
    expected_sha256: &str,
    progress: &mut impl FnMut(Progress),
) -> Result<String, String> {
    // ── write ──
    let mut buf = vec![0u8; CHUNK];
    let mut done: u64 = 0;
    loop {
        let n = image
            .read(&mut buf)
            .map_err(|e| format!("couldn't read the raw image: {e}"))?;
        if n == 0 {
            break;
        }
        device
            .write_all(&buf[..n])
            .map_err(|e| format!("write failed after {done} bytes: {e}"))?;
        done += n as u64;
        progress(Progress {
            stage: Stage::Write,
            done,
            total: Some(image_bytes),
        });
    }
    device
        .flush()
        .map_err(|e| format!("couldn't flush the device: {e}"))?;
    device
        .sync_storage()
        .map_err(|e| format!("couldn't sync the device before verification: {e}"))?;
    if done != image_bytes {
        return Err(format!(
            "the image changed size mid-write ({done} of {image_bytes} bytes) — aborting"
        ));
    }

    // ── read back ──
    device
        .drop_read_cache()
        .map_err(|e| format!("couldn't invalidate the read cache before verification: {e}"))?;
    device
        .seek(SeekFrom::Start(0))
        .map_err(|e| format!("couldn't rewind the device for verification: {e}"))?;
    let mut hasher = sha2::Sha256::new();
    let mut remaining = image_bytes;
    while remaining > 0 {
        let want = remaining.min(CHUNK as u64) as usize;
        device
            .read_exact(&mut buf[..want])
            .map_err(|e| format!("read-back failed with {remaining} bytes left: {e}"))?;
        hasher.update(&buf[..want]);
        remaining -= want as u64;
        progress(Progress {
            stage: Stage::Verify,
            done: image_bytes - remaining,
            total: Some(image_bytes),
        });
    }
    let got = sha256_hex(hasher);
    if !got.eq_ignore_ascii_case(expected_sha256) {
        return Err(format!(
            "read-back verification FAILED: the device holds {got} but the verified image is \
             {expected_sha256}. The card may be faulty or counterfeit — do not use it for a hub."
        ));
    }
    Ok(got)
}

// ── platform: turning an authorized path into an exclusive device handle ────

/// Linux: open the block device read/write with `O_EXCL`, which the kernel
/// honors for block devices as "fail if anyone (a filesystem mount, another
/// writer) holds it". Auto-mounted partitions are unmounted first, best-effort,
/// via udisks — the open itself is the real gate.
#[cfg(target_os = "linux")]
fn open_target(path: &str) -> Result<std::fs::File, String> {
    use std::os::unix::fs::OpenOptionsExt;

    // Best-effort unmount of any auto-mounted partitions (a fresh card often
    // mounts on insert). Failure here is fine — O_EXCL below still refuses a
    // busy device, with a clearer message than EBUSY alone.
    if let Some(name) = path.strip_prefix("/dev/") {
        if let Ok(entries) = std::fs::read_dir(format!("/sys/block/{name}")) {
            for entry in entries.flatten() {
                let part = entry.file_name().to_string_lossy().into_owned();
                if part.starts_with(name) && part != name {
                    let _ = std::process::Command::new("udisksctl")
                        .args(["unmount", "-b", &format!("/dev/{part}")])
                        .output();
                }
            }
        }
    }

    std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(libc::O_EXCL)
        .open(path)
        .map_err(|e| match e.kind() {
            std::io::ErrorKind::PermissionDenied => format!(
                "no permission to write {path}. Run the flasher with the privileges your distro \
                 uses for disk tools (e.g. launch via `pkexec` or `sudo`, or add your user to the \
                 `disk` group and re-login)."
            ),
            _ => format!(
                "couldn't open {path} exclusively: {e}. If the card just mounted, eject it in \
                 your file manager and try again."
            ),
        })
}

/// macOS: unmount the whole disk (required before raw writes), then open the
/// raw `rdisk` node — directly when we already have the privilege, otherwise
/// through Apple's `authopen`, which shows the system authorization prompt and
/// hands back an opened file descriptor. This is the same path Raspberry Pi
/// Imager and Etcher use; the app itself never runs as root.
#[cfg(target_os = "macos")]
fn open_target(path: &str) -> Result<std::fs::File, String> {
    let unmount = std::process::Command::new("diskutil")
        .args(["unmountDisk", path])
        .output()
        .map_err(|e| format!("couldn't run diskutil: {e}"))?;
    if !unmount.status.success() {
        return Err(format!(
            "couldn't unmount {path} before writing: {}",
            String::from_utf8_lossy(&unmount.stderr).trim()
        ));
    }
    // The raw (character) node skips the buffer cache — dramatically faster
    // and the conventional target for whole-disk writes.
    let raw = match path.strip_prefix("/dev/disk") {
        Some(rest) => format!("/dev/rdisk{rest}"),
        None => path.to_string(),
    };
    match std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&raw)
    {
        Ok(f) => Ok(f),
        Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => authopen(&raw),
        Err(e) => Err(format!("couldn't open {raw}: {e}")),
    }
}

/// Receive an opened read/write fd for `path` from `/usr/libexec/authopen
/// -stdoutpipe`, which authenticates the operator through the system prompt
/// and passes the descriptor back over a unix socket (SCM_RIGHTS). Isolated
/// here so the unsafe fd plumbing has one small, auditable home.
#[cfg(target_os = "macos")]
fn authopen(path: &str) -> Result<std::fs::File, String> {
    use std::os::fd::{FromRawFd, IntoRawFd};

    // A connected socket pair: authopen writes the fd to its stdout (ours is
    // the other end).
    let mut pair = [0 as libc::c_int; 2];
    if unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, pair.as_mut_ptr()) } != 0 {
        return Err("couldn't create the authopen socket pair".to_string());
    }
    let (ours, theirs) = (pair[0], pair[1]);

    let child = {
        use std::process::{Command, Stdio};
        let stdout = unsafe { Stdio::from_raw_fd(theirs) };
        Command::new("/usr/libexec/authopen")
            .arg("-stdoutpipe")
            .arg("-o")
            .arg(format!("{}", libc::O_RDWR))
            .arg(path)
            .stdin(Stdio::null())
            .stdout(stdout)
            .stderr(Stdio::null())
            .spawn()
    };
    let mut child = match child {
        Ok(c) => c,
        Err(e) => {
            unsafe { libc::close(ours) };
            return Err(format!("couldn't start authopen: {e}"));
        }
    };

    // Receive one message carrying SCM_RIGHTS with the opened descriptor.
    let mut data = [0u8; 16];
    let mut iov = libc::iovec {
        iov_base: data.as_mut_ptr().cast(),
        iov_len: data.len(),
    };
    // Space for one cmsghdr + one fd, per CMSG_SPACE.
    let mut cmsg = [0u8; 64];
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg.as_mut_ptr().cast();
    msg.msg_controllen = cmsg.len() as _;

    let received = unsafe { libc::recvmsg(ours, &mut msg, 0) };
    unsafe { libc::close(ours) };
    let status = child.wait();

    if received < 0 {
        return Err("authopen didn't hand back a descriptor".to_string());
    }
    let mut fd: Option<libc::c_int> = None;
    unsafe {
        let mut hdr = libc::CMSG_FIRSTHDR(&msg);
        while !hdr.is_null() {
            if (*hdr).cmsg_level == libc::SOL_SOCKET && (*hdr).cmsg_type == libc::SCM_RIGHTS {
                let data_ptr = libc::CMSG_DATA(hdr) as *const libc::c_int;
                fd = Some(*data_ptr);
                break;
            }
            hdr = libc::CMSG_NXTHDR(&msg, hdr);
        }
    }
    match fd {
        Some(fd) if fd >= 0 => {
            let file = unsafe { std::fs::File::from_raw_fd(fd) };
            // Keep the raw fd alive past the child reaping above.
            let fd_keep = file.into_raw_fd();
            let _ = status;
            Ok(unsafe { std::fs::File::from_raw_fd(fd_keep) })
        }
        _ => Err(
            "macOS didn't authorize writing the disk (the authorization prompt was cancelled or \
             authopen failed)"
                .to_string(),
        ),
    }
}

/// Platforms without a write backend yet (Windows). Honest error, same posture
/// as the enumerator.
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn open_target(_path: &str) -> Result<std::fs::File, String> {
    Err("writing a hub disk isn't implemented on this OS yet".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use hub_core::hub_disk::TargetDisk;
    use hub_core::hub_flash::authorize_write;
    use hub_core::hub_image::{verify_download, WritePlan};

    fn image_bytes() -> Vec<u8> {
        (0..3_000_000u32)
            .map(|i| (i.wrapping_mul(0x9E37_79B9) >> 16) as u8)
            .collect()
    }

    fn sha_of(bytes: &[u8]) -> String {
        let mut h = sha2::Sha256::new();
        h.update(bytes);
        crate::sha256_hex(h)
    }

    #[test]
    fn write_verified_round_trips_and_reports_both_stages() {
        let img = image_bytes();
        let sha = sha_of(&img);
        let mut device = std::io::Cursor::new(Vec::new());
        let mut stages = Vec::new();
        let got = write_verified(
            &mut device,
            std::io::Cursor::new(img.clone()),
            img.len() as u64,
            &sha,
            &mut |p: Progress| stages.push(p.stage),
        )
        .expect("write + read-back verify");
        assert_eq!(got, sha);
        assert_eq!(device.into_inner(), img);
        assert!(stages.contains(&Stage::Write));
        assert!(stages.contains(&Stage::Verify));
    }

    #[test]
    fn a_device_that_flips_a_bit_fails_read_back() {
        // A "device" whose storage silently corrupts one byte: the classic
        // counterfeit card. The read-back must catch it.
        struct LyingDevice {
            inner: std::io::Cursor<Vec<u8>>,
        }
        impl SyncStorage for LyingDevice {
            fn sync_storage(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        impl Write for LyingDevice {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                self.inner.write(buf)
            }
            fn flush(&mut self) -> std::io::Result<()> {
                // Corrupt after the writes land, before anyone reads back.
                let pos = self.inner.get_ref().len() / 2;
                self.inner.get_mut()[pos] ^= 0xff;
                Ok(())
            }
        }
        impl Read for LyingDevice {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                self.inner.read(buf)
            }
        }
        impl Seek for LyingDevice {
            fn seek(&mut self, pos: SeekFrom) -> std::io::Result<u64> {
                self.inner.seek(pos)
            }
        }

        let img = image_bytes();
        let sha = sha_of(&img);
        let mut device = LyingDevice {
            inner: std::io::Cursor::new(Vec::new()),
        };
        let err = write_verified(
            &mut device,
            std::io::Cursor::new(img.clone()),
            img.len() as u64,
            &sha,
            &mut |_| {},
        )
        .unwrap_err();
        assert!(err.contains("read-back verification FAILED"), "{err}");
    }

    #[test]
    fn a_wrong_expected_hash_fails_even_on_a_faithful_device() {
        let img = image_bytes();
        let mut device = std::io::Cursor::new(Vec::new());
        let err = write_verified(
            &mut device,
            std::io::Cursor::new(img.clone()),
            img.len() as u64,
            &"0".repeat(64),
            &mut |_| {},
        )
        .unwrap_err();
        assert!(err.contains("read-back verification FAILED"));
    }

    #[test]
    fn the_full_entry_point_only_runs_with_a_real_authorization() {
        // End-to-end against a temp file standing in for the device: the ONLY
        // way to call write_image is with a WriteAuthorization, which itself
        // only exists after verify + eligibility + confirm. This test walks
        // the whole chain the app will.
        let img = image_bytes();
        let sha = sha_of(&img);

        let dir = tempfile::tempdir().unwrap();
        let image_path = dir.path().join("raw.img");
        std::fs::write(&image_path, &img).unwrap();
        let device_path = dir.path().join("fake-device");
        std::fs::write(&device_path, b"").unwrap();

        let plan = WritePlan {
            board_id: "rpi5-64".to_string(),
            os_label: "Home Assistant OS 18.1".to_string(),
            image_url: "https://example/haos.img.xz".to_string(),
            expected_sha256: None,
            min_card_bytes: 28 * 1024 * 1024 * 1024,
            warnings: vec![],
        };
        let dl_sha = "a".repeat(64); // hash of the *compressed* download
        let verified = verify_download(&plan, &dl_sha, Some(&dl_sha)).unwrap();
        let target = TargetDisk {
            path: device_path.to_string_lossy().into_owned(),
            model: "Test Card".to_string(),
            size_bytes: 64 * 1024 * 1024 * 1024,
            removable: true,
            external: false,
            system: false,
            has_mounts: false,
        };
        let authz = authorize_write(plan, &verified, &target, true).unwrap();

        // open_target is platform glue aimed at real block devices; drive the
        // tested core with the same authorization data instead.
        let mut device = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(authz.target_path())
            .unwrap();
        let got = write_verified(
            &mut device,
            std::fs::File::open(&image_path).unwrap(),
            img.len() as u64,
            &sha,
            &mut |_| {},
        )
        .unwrap();
        assert_eq!(got, sha);
        assert_eq!(std::fs::read(&device_path).unwrap(), img);
    }
}
