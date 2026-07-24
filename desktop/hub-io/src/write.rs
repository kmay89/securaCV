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

use crate::{sha256_hex, CancelToken, Progress, Stage};
use hub_core::hub_flash::WriteAuthorization;
use sha2::Digest;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// Chunk size for both the write and the read-back. 4 MiB aligns with every
/// SD/SSD erase-block size that matters and keeps syscall overhead irrelevant.
const CHUNK: usize = 4 * 1024 * 1024;

/// Whether writing a hub card is **enabled** on this platform.
///
/// Disk *detection* works everywhere — `hub_core::hub_enumerate` enumerates and
/// classifies candidates on Linux, macOS, and Windows alike. *Writing* is
/// validated and enabled on Linux and macOS. Windows has a full `open_target`
/// implementation too (see below), but it stays **gated off here until it's
/// validated on real hardware** — the same "hardware-proven before it ships"
/// bar every destructive path in this repo holds to. Enabling Windows is a
/// one-line change to this expression once a VM/hardware pass confirms a
/// round-tripped write.
///
/// The flasher checks this **up front**, before the hundreds-of-MB download and
/// ~2.5 GB decompress, so an operator on a not-yet-enabled OS gets an instant,
/// honest answer instead of waiting out the whole preparation only to fail at
/// the write.
pub const fn write_backend_available() -> bool {
    cfg!(any(target_os = "linux", target_os = "macos"))
}

/// The physical-drive index in a Windows raw device path, e.g.
/// `\\.\PhysicalDrive2` → `2` (case-insensitive, a leading `\\.\` optional).
/// Pure so it's host-tested on any runner; the Windows `open_target` turns the
/// number back into a handle. Returns `None` for anything that isn't a
/// `PhysicalDrive<n>` path — the inverse of
/// `hub_core::hub_enumerate_windows::physical_drive_path`.
pub fn physical_drive_number(path: &str) -> Option<u32> {
    let p = path.trim();
    let p = p.strip_prefix(r"\\.\").unwrap_or(p);
    const PREFIX: &str = "PhysicalDrive";
    if p.len() < PREFIX.len() || !p[..PREFIX.len()].eq_ignore_ascii_case(PREFIX) {
        return None;
    }
    let digits = &p[PREFIX.len()..];
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    digits.parse::<u32>().ok()
}

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
    cancel: &CancelToken,
    mut progress: impl FnMut(Progress),
) -> Result<WriteReceipt, String> {
    cancel.checkpoint()?;
    let mut device = open_target(authz.target_path())?;
    let image = std::fs::File::open(raw_image)
        .map_err(|e| format!("couldn't open the raw image {}: {e}", raw_image.display()))?;
    let bytes = image
        .metadata()
        .map_err(|e| format!("couldn't stat the raw image: {e}"))?
        .len();
    let sha = write_verified(&mut device, image, bytes, raw_sha256, cancel, &mut progress)?;
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
        let rc = unsafe { libc::posix_fadvise(self.as_raw_fd(), 0, 0, libc::POSIX_FADV_DONTNEED) };
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
    cancel: &CancelToken,
    progress: &mut impl FnMut(Progress),
) -> Result<String, String> {
    // ── write ──
    let mut buf = vec![0u8; CHUNK];
    let mut done: u64 = 0;
    loop {
        cancel.checkpoint()?;
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
        cancel.checkpoint()?;
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

/// Windows: get an exclusive handle to `\\.\PhysicalDriveN`, with every volume
/// that lives on that disk locked and dismounted first (Windows refuses raw
/// writes to sectors a mounted filesystem owns). The returned `File` then
/// streams through the very same host-tested [`write_verified`] core as every
/// other platform — HAOS images are whole-sector, so the 4 MiB chunks and the
/// aligned tail satisfy raw-disk I/O alignment; only *getting the handle* is
/// Windows-specific.
///
/// STAGED — NOT YET ENABLED. [`write_backend_available`] returns `false` on
/// Windows, so the flasher fails fast (before any download) and never reaches
/// this. It exists so a VM/hardware pass can compile and exercise it against a
/// spare USB stick; once a round-tripped write is confirmed, enabling Windows is
/// the one-line flip in `write_backend_available`. Opening a physical drive for
/// write needs Administrator, so the shipped app will carry an elevation
/// manifest; a non-elevated run surfaces the clear permission error below.
#[cfg(target_os = "windows")]
fn open_target(path: &str) -> Result<std::fs::File, String> {
    let n = physical_drive_number(path)
        .ok_or_else(|| format!("{path} isn't a \\\\.\\PhysicalDrive<n> path"))?;
    win_io::lock_and_dismount_disk_volumes(n)?;
    win_io::open_physical_drive(n)
}

/// The Windows raw-disk primitives, hand-declared in the crate's own style (the
/// macOS path hand-rolls its `libc` FFI too) so hub-io keeps its small, audited
/// dependency set — no `windows-sys` pulled in for a handful of calls.
#[cfg(target_os = "windows")]
mod win_io {
    use std::os::windows::io::{FromRawHandle, OwnedHandle, RawHandle};
    use std::sync::Mutex;

    const INVALID_HANDLE_VALUE: RawHandle = -1isize as RawHandle;
    const GENERIC_READ: u32 = 0x8000_0000;
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const FILE_SHARE_READ: u32 = 0x0000_0001;
    const FILE_SHARE_WRITE: u32 = 0x0000_0002;
    const OPEN_EXISTING: u32 = 3;
    const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;
    const ERROR_ACCESS_DENIED: u32 = 5;

    // FSCTL / IOCTL codes from winioctl.h.
    const FSCTL_LOCK_VOLUME: u32 = 0x0009_0018;
    const FSCTL_DISMOUNT_VOLUME: u32 = 0x0009_0020;
    const FSCTL_ALLOW_EXTENDED_DASD_IO: u32 = 0x0009_0083;
    const IOCTL_STORAGE_GET_DEVICE_NUMBER: u32 = 0x002D_1080;

    // STORAGE_DEVICE_NUMBER. Only `device_number` is read; the other fields are
    // present so the struct matches the Win32 layout the kernel fills in.
    #[repr(C)]
    #[allow(dead_code)]
    struct StorageDeviceNumber {
        device_type: u32,
        device_number: u32,
        partition_number: u32,
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            security: *mut core::ffi::c_void,
            disposition: u32,
            flags: u32,
            template: RawHandle,
        ) -> RawHandle;
        fn DeviceIoControl(
            device: RawHandle,
            code: u32,
            in_buf: *mut core::ffi::c_void,
            in_size: u32,
            out_buf: *mut core::ffi::c_void,
            out_size: u32,
            returned: *mut u32,
            overlapped: *mut core::ffi::c_void,
        ) -> i32;
        fn GetLastError() -> u32;
    }

    /// Volume-lock handles held open for the duration of the current write.
    /// Keeping the LOCK/DISMOUNT handle open is what stops Windows re-mounting a
    /// volume mid-write. Replaced (prior locks released) at the start of every
    /// flash and dropped on process exit; one flash runs at a time (the flasher
    /// serialises them), so this never accumulates.
    static VOLUME_LOCKS: Mutex<Vec<OwnedHandle>> = Mutex::new(Vec::new());

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    /// Lock + dismount every mounted volume that sits on physical disk `n`,
    /// keeping each lock handle open (in `VOLUME_LOCKS`) so the volume can't
    /// remount while we write. Volumes on other disks are left untouched.
    pub fn lock_and_dismount_disk_volumes(n: u32) -> Result<(), String> {
        let mut held = VOLUME_LOCKS
            .lock()
            .map_err(|_| "volume-lock state was poisoned".to_string())?;
        held.clear(); // release any locks a previous flash left behind

        for letter in b'A'..=b'Z' {
            let vol = format!(r"\\.\{}:", letter as char);
            let handle = unsafe {
                CreateFileW(
                    wide(&vol).as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    std::ptr::null_mut(),
                    OPEN_EXISTING,
                    0,
                    std::ptr::null_mut(),
                )
            };
            if handle == INVALID_HANDLE_VALUE {
                continue; // no such drive letter
            }
            // Own it now so every early exit closes it.
            let owned = unsafe { OwnedHandle::from_raw_handle(handle) };

            let mut sdn = StorageDeviceNumber {
                device_type: 0,
                device_number: u32::MAX,
                partition_number: 0,
            };
            let mut returned = 0u32;
            let ok = unsafe {
                DeviceIoControl(
                    handle,
                    IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::addr_of_mut!(sdn).cast(),
                    std::mem::size_of::<StorageDeviceNumber>() as u32,
                    &mut returned,
                    std::ptr::null_mut(),
                )
            };
            if ok == 0 || sdn.device_number != n {
                continue; // couldn't tell, or not our disk → drop (closes) it
            }

            // It's on the target disk. Lock (best effort — open handles can
            // block it), then force the dismount, and KEEP the handle so the
            // volume stays down for the write.
            let mut r = 0u32;
            unsafe {
                DeviceIoControl(
                    handle,
                    FSCTL_LOCK_VOLUME,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null_mut(),
                    0,
                    &mut r,
                    std::ptr::null_mut(),
                );
            }
            let dismounted = unsafe {
                DeviceIoControl(
                    handle,
                    FSCTL_DISMOUNT_VOLUME,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null_mut(),
                    0,
                    &mut r,
                    std::ptr::null_mut(),
                )
            };
            if dismounted == 0 {
                let e = unsafe { GetLastError() };
                return Err(format!(
                    "couldn't dismount {vol} on PhysicalDrive{n} before writing (Windows error \
                     {e}) — close any Explorer window or program using that card and try again"
                ));
            }
            held.push(owned);
        }
        Ok(())
    }

    /// Open `\\.\PhysicalDriveN` read/write and allow whole-disk I/O. Needs
    /// Administrator; a denied open returns a clear "run as administrator" line.
    pub fn open_physical_drive(n: u32) -> Result<std::fs::File, String> {
        let path = format!(r"\\.\PhysicalDrive{n}");
        let handle = unsafe {
            CreateFileW(
                wide(&path).as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                std::ptr::null_mut(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                std::ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            let e = unsafe { GetLastError() };
            return Err(if e == ERROR_ACCESS_DENIED {
                format!(
                    "no permission to open {path} — flashing a disk needs Administrator. \
                     Right-click the flasher and choose \"Run as administrator\", then try again."
                )
            } else {
                format!("couldn't open {path} for writing (Windows error {e})")
            });
        }
        // Let writes run past the last partition to the end of the medium.
        let mut returned = 0u32;
        unsafe {
            DeviceIoControl(
                handle,
                FSCTL_ALLOW_EXTENDED_DASD_IO,
                std::ptr::null_mut(),
                0,
                std::ptr::null_mut(),
                0,
                &mut returned,
                std::ptr::null_mut(),
            );
        }
        // Safety: `handle` is a valid, owned OS handle we just opened.
        Ok(unsafe { std::fs::File::from_raw_handle(handle) })
    }
}

/// Other platforms (e.g. the BSDs) have no write backend yet. Honest error,
/// same posture as the enumerator.
#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
fn open_target(_path: &str) -> Result<std::fs::File, String> {
    Err("writing a hub disk isn't implemented on this OS yet".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use hub_core::hub_disk::TargetDisk;
    use hub_core::hub_flash::authorize_write;
    use hub_core::hub_image::{verify_download, WritePlan};

    #[test]
    fn write_backend_available_is_true_only_where_writing_is_enabled() {
        // Writing is enabled (validated) on Linux/macOS. Windows has a full
        // open_target now, but stays gated off here until a VM/hardware pass
        // proves it — so the predicate is false on Windows on purpose, and the
        // flasher fails fast there before any download.
        #[cfg(any(target_os = "linux", target_os = "macos"))]
        assert!(write_backend_available());
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        assert!(!write_backend_available());
    }

    #[test]
    fn physical_drive_number_parses_windows_raw_paths() {
        assert_eq!(physical_drive_number(r"\\.\PhysicalDrive0"), Some(0));
        assert_eq!(physical_drive_number(r"\\.\PhysicalDrive2"), Some(2));
        assert_eq!(physical_drive_number(r"\\.\PhysicalDrive10"), Some(10));
        // Case-insensitive, and the \\.\ prefix is optional.
        assert_eq!(physical_drive_number(r"\\.\physicaldrive7"), Some(7));
        assert_eq!(physical_drive_number("PhysicalDrive3"), Some(3));
        // The inverse of hub_core's physical_drive_path round-trips.
        assert_eq!(
            physical_drive_number(&hub_core::hub_enumerate_windows::physical_drive_path(5)),
            Some(5)
        );
        // Not a physical-drive path → None (never a guessed device).
        assert_eq!(physical_drive_number(r"\\.\C:"), None);
        assert_eq!(physical_drive_number("/dev/sdb"), None);
        assert_eq!(physical_drive_number(r"\\.\PhysicalDrive"), None);
        assert_eq!(physical_drive_number(r"\\.\PhysicalDriveX"), None);
        assert_eq!(physical_drive_number(""), None);
    }

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
            &CancelToken::default(),
            &mut |p: Progress| stages.push(p.stage),
        )
        .expect("write + read-back verify");
        assert_eq!(got, sha);
        assert_eq!(device.into_inner(), img);
        assert!(stages.contains(&Stage::Write));
        assert!(stages.contains(&Stage::Verify));
    }

    #[test]
    fn cancelling_mid_write_stops_between_chunks_with_the_neutral_marker() {
        // The Stop button's contract: flip the token, and the loop exits at
        // the next chunk boundary with the CANCELLED marker — never a panic,
        // never a hang, and the UI can tell "stopped" from "failed".
        let img = image_bytes();
        let sha = sha_of(&img);
        let token = crate::CancelToken::default();
        let cancel_after_first_tick = token.clone();
        let mut device = std::io::Cursor::new(Vec::new());
        let err = write_verified(
            &mut device,
            std::io::Cursor::new(img.clone()),
            img.len() as u64,
            &sha,
            &token,
            &mut |_: Progress| cancel_after_first_tick.cancel(),
        )
        .unwrap_err();
        assert!(err.starts_with(crate::CANCELLED), "{err}");
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
            &CancelToken::default(),
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
            &CancelToken::default(),
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
            &CancelToken::default(),
            &mut |_| {},
        )
        .unwrap();
        assert_eq!(got, sha);
        assert_eq!(std::fs::read(&device_path).unwrap(), img);
    }
}
