//! Sandboxed module execution using a hardened syscall denylist.

use anyhow::{anyhow, Context, Result};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use std::os::raw::{c_int, c_void};

#[derive(Debug, Serialize, Deserialize)]
enum SandboxResponse<T> {
    Ok(T),
    Err(String),
}

pub fn run_in_sandbox<T, F>(f: F) -> Result<T>
where
    T: Serialize + DeserializeOwned,
    F: FnOnce() -> Result<T>,
{
    #[cfg(target_os = "linux")]
    {
        linux::run_in_sandbox(f)
    }

    #[cfg(not(target_os = "linux"))]
    {
        let _ = f;
        Err(anyhow!("conformance: sandbox unavailable on this platform"))
    }
}

#[cfg(target_os = "linux")]
mod linux {
    use super::*;
    use libseccomp::{ScmpAction, ScmpFilterContext, ScmpSyscall};

    const DENY_SYSCALLS: &[&str] = &[
        "open",
        "openat",
        "openat2",
        "creat",
        "unlink",
        "unlinkat",
        "rename",
        "renameat",
        "renameat2",
        "mkdir",
        "mkdirat",
        "rmdir",
        "link",
        "linkat",
        "symlink",
        "symlinkat",
        "readlink",
        "readlinkat",
        "chdir",
        "fchdir",
        "chmod",
        "fchmod",
        "fchmodat",
        "chown",
        "fchown",
        "fchownat",
        "lchown",
        "truncate",
        "ftruncate",
        "stat",
        "lstat",
        "fstat",
        "newfstatat",
        "access",
        "faccessat",
        "faccessat2",
        "getdents",
        "getdents64",
        "socket",
        "connect",
        "accept",
        "accept4",
        "bind",
        "listen",
        "sendto",
        "recvfrom",
        "sendmsg",
        "recvmsg",
        "shutdown",
        "getsockname",
        "getpeername",
        "socketpair",
        "setsockopt",
        "getsockopt",
    ];

    extern "C" {
        fn fork() -> c_int;
        fn waitpid(pid: c_int, status: *mut c_int, options: c_int) -> c_int;
        fn pipe(fds: *mut c_int) -> c_int;
        fn close(fd: c_int) -> c_int;
        fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
        fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
        fn _exit(status: c_int) -> !;
    }

    fn install_filter() -> Result<()> {
        let rc = unsafe { libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) };
        if rc != 0 {
            return Err(anyhow!(std::io::Error::last_os_error()))
                .context("sandbox: PR_SET_NO_NEW_PRIVS failed");
        }

        // libseccomp provides portable syscall-name mapping for aarch64/x86_64,
        // avoiding hardcoded syscall numbers while preserving the same denylist.
        let mut ctx =
            ScmpFilterContext::new_filter(ScmpAction::Allow).context("sandbox: seccomp init")?;
        for name in DENY_SYSCALLS {
            let syscall = ScmpSyscall::from_name(name)
                .with_context(|| format!("sandbox: unknown syscall name {name}"))?;
            ctx.add_rule(ScmpAction::Errno(libc::EPERM), syscall)
                .with_context(|| format!("sandbox: failed to deny {name}"))?;
        }
        // The prior filter killed on arch mismatch; libseccomp binds the filter
        // to the native architecture, preserving intent without hardcoding AUDIT_ARCH_*.
        ctx.load().context("sandbox: seccomp load failed")?;

        Ok(())
    }

    fn read_exact(fd: c_int, buf: &mut [u8]) -> Result<()> {
        let mut offset = 0;
        while offset < buf.len() {
            let rc = unsafe {
                read(
                    fd,
                    buf[offset..].as_mut_ptr() as *mut c_void,
                    buf.len() - offset,
                )
            };
            if rc <= 0 {
                return Err(anyhow!("sandbox: failed to read response"));
            }
            offset += rc as usize;
        }
        Ok(())
    }

    fn write_all(fd: c_int, buf: &[u8]) -> Result<()> {
        let mut offset = 0;
        while offset < buf.len() {
            let rc = unsafe {
                write(
                    fd,
                    buf[offset..].as_ptr() as *const c_void,
                    buf.len() - offset,
                )
            };
            if rc <= 0 {
                return Err(anyhow!("sandbox: failed to write response"));
            }
            offset += rc as usize;
        }
        Ok(())
    }

    pub(super) fn run_in_sandbox<T, F>(f: F) -> Result<T>
    where
        T: Serialize + DeserializeOwned,
        F: FnOnce() -> Result<T>,
    {
        let mut fds = [0; 2];
        let rc = unsafe { pipe(fds.as_mut_ptr()) };
        if rc != 0 {
            return Err(anyhow!(std::io::Error::last_os_error())).context("sandbox: pipe failed");
        }

        let pid = unsafe { fork() };
        if pid < 0 {
            return Err(anyhow!(std::io::Error::last_os_error())).context("sandbox: fork failed");
        }

        if pid == 0 {
            unsafe { close(fds[0]) };
            let response = match install_filter().and_then(|_| f()) {
                Ok(value) => SandboxResponse::Ok(value),
                Err(err) => SandboxResponse::Err(format!("{err:#}")),
            };
            let payload = serde_json::to_vec(&response).unwrap_or_else(|err| {
                serde_json::to_vec(&SandboxResponse::<()>::Err(format!(
                    "sandbox: serialization failed: {err}"
                )))
                .expect("sandbox: serialization of error failed")
            });
            let len_bytes = (payload.len() as u64).to_le_bytes();
            let _ = write_all(fds[1], &len_bytes);
            let _ = write_all(fds[1], &payload);
            unsafe {
                close(fds[1]);
                _exit(0);
            }
        }

        unsafe { close(fds[1]) };

        let mut len_bytes = [0u8; 8];
        read_exact(fds[0], &mut len_bytes)?;
        let len = u64::from_le_bytes(len_bytes) as usize;
        let mut payload = vec![0u8; len];
        read_exact(fds[0], &mut payload)?;
        unsafe { close(fds[0]) };

        let mut status = 0;
        let _ = unsafe { waitpid(pid, &mut status as *mut c_int, 0) };

        let response: SandboxResponse<T> =
            serde_json::from_slice(&payload).context("sandbox: response decode failed")?;
        match response {
            SandboxResponse::Ok(value) => Ok(value),
            SandboxResponse::Err(message) => Err(anyhow!(message)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_blocks_filesystem_access() {
        let result = run_in_sandbox(|| {
            let err = std::fs::File::open("/etc/hosts").unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");

        assert_eq!(result, Some(1));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_blocks_network_access() {
        let result = run_in_sandbox(|| {
            let err = std::net::TcpStream::connect("127.0.0.1:1").unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");

        assert_eq!(result, Some(1));
    }

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn sandbox_unavailable_is_a_conformance_error() {
        let err = run_in_sandbox(|| Ok(())).expect_err("sandbox should be unavailable");
        assert!(
            err.to_string().contains("sandbox unavailable"),
            "unexpected error: {err}"
        );
    }
}
