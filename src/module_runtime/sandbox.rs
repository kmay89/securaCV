//! Sandboxed module execution using a hardened syscall denylist.

use anyhow::{anyhow, Context, Result};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use std::os::raw::{c_int, c_long, c_uchar, c_uint, c_ulong, c_ushort, c_void};

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
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    {
        linux::run_in_sandbox(f)
    }

    #[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
    {
        let _ = f;
        Err(anyhow!("conformance: sandbox unavailable on this platform"))
    }
}

#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
mod linux {
    use super::*;

    const EPERM: c_uint = 1;
    const PR_SET_NO_NEW_PRIVS: c_int = 38;
    const SECCOMP_SET_MODE_FILTER: c_uint = 1;
    const SECCOMP_RET_ALLOW: c_uint = 0x7fff_0000;
    const SECCOMP_RET_ERRNO: c_uint = 0x0005_0000;
    const SECCOMP_RET_KILL: c_uint = 0x0000_0000;
    const AUDIT_ARCH_X86_64: c_uint = 0xc000_003e;
    const SYS_SECCOMP: c_long = 317;

    const BPF_LD: c_ushort = 0x00;
    const BPF_W: c_ushort = 0x00;
    const BPF_ABS: c_ushort = 0x20;
    const BPF_JMP: c_ushort = 0x05;
    const BPF_JEQ: c_ushort = 0x10;
    const BPF_K: c_ushort = 0x00;
    const BPF_RET: c_ushort = 0x06;

    const SYSCALL_NR_OFFSET: c_uint = 0;
    const ARCH_OFFSET: c_uint = 4;

    const SYS_OPEN: c_uint = 2;
    const SYS_CREAT: c_uint = 85;
    const SYS_UNLINK: c_uint = 87;
    const SYS_RENAME: c_uint = 82;
    const SYS_MKDIR: c_uint = 83;
    const SYS_RMDIR: c_uint = 84;
    const SYS_LINK: c_uint = 86;
    const SYS_SYMLINK: c_uint = 88;
    const SYS_READLINK: c_uint = 89;
    const SYS_CHDIR: c_uint = 80;
    const SYS_FCHDIR: c_uint = 81;
    const SYS_CHMOD: c_uint = 90;
    const SYS_FCHMOD: c_uint = 91;
    const SYS_CHOWN: c_uint = 92;
    const SYS_FCHOWN: c_uint = 93;
    const SYS_LCHOWN: c_uint = 94;
    const SYS_TRUNCATE: c_uint = 76;
    const SYS_FTRUNCATE: c_uint = 77;
    const SYS_STAT: c_uint = 4;
    const SYS_LSTAT: c_uint = 6;
    const SYS_FSTAT: c_uint = 5;
    const SYS_ACCESS: c_uint = 21;
    const SYS_GETDENTS: c_uint = 78;
    const SYS_GETDENTS64: c_uint = 217;
    const SYS_OPENAT: c_uint = 257;
    const SYS_OPENAT2: c_uint = 437;
    const SYS_UNLINKAT: c_uint = 263;
    const SYS_RENAMEAT: c_uint = 264;
    const SYS_RENAMEAT2: c_uint = 316;
    const SYS_MKDIRAT: c_uint = 258;
    const SYS_LINKAT: c_uint = 265;
    const SYS_SYMLINKAT: c_uint = 266;
    const SYS_READLINKAT: c_uint = 267;
    const SYS_FCHMODAT: c_uint = 268;
    const SYS_FCHOWNAT: c_uint = 260;
    const SYS_NEWFSTATAT: c_uint = 262;
    const SYS_FACCESSAT: c_uint = 269;
    const SYS_FACCESSAT2: c_uint = 439;

    const SYS_SOCKET: c_uint = 41;
    const SYS_CONNECT: c_uint = 42;
    const SYS_ACCEPT: c_uint = 43;
    const SYS_ACCEPT4: c_uint = 288;
    const SYS_BIND: c_uint = 49;
    const SYS_LISTEN: c_uint = 50;
    const SYS_SENDTO: c_uint = 44;
    const SYS_RECVFROM: c_uint = 45;
    const SYS_SENDMSG: c_uint = 46;
    const SYS_RECVMSG: c_uint = 47;
    const SYS_SHUTDOWN: c_uint = 48;
    const SYS_GETSOCKNAME: c_uint = 51;
    const SYS_GETPEERNAME: c_uint = 52;
    const SYS_SOCKETPAIR: c_uint = 53;
    const SYS_SETSOCKOPT: c_uint = 54;
    const SYS_GETSOCKOPT: c_uint = 55;

    #[repr(C)]
    struct SockFilter {
        code: c_ushort,
        jt: c_uchar,
        jf: c_uchar,
        k: c_uint,
    }

    #[repr(C)]
    struct SockFprog {
        len: c_ushort,
        filter: *const SockFilter,
    }

    extern "C" {
        fn prctl(
            option: c_int,
            arg2: c_ulong,
            arg3: c_ulong,
            arg4: c_ulong,
            arg5: c_ulong,
        ) -> c_int;
        fn syscall(num: c_long, ...) -> c_long;
        fn fork() -> c_int;
        fn waitpid(pid: c_int, status: *mut c_int, options: c_int) -> c_int;
        fn pipe(fds: *mut c_int) -> c_int;
        fn close(fd: c_int) -> c_int;
        fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
        fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
        fn _exit(status: c_int) -> !;
    }

    fn stmt(code: c_ushort, k: c_uint) -> SockFilter {
        SockFilter {
            code,
            jt: 0,
            jf: 0,
            k,
        }
    }

    fn jump(code: c_ushort, k: c_uint, jt: c_uchar, jf: c_uchar) -> SockFilter {
        SockFilter { code, jt, jf, k }
    }

    fn install_filter() -> Result<()> {
        let rc = unsafe { prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) };
        if rc != 0 {
            return Err(anyhow!(std::io::Error::last_os_error()))
                .context("sandbox: PR_SET_NO_NEW_PRIVS failed");
        }

        let deny_errno = SECCOMP_RET_ERRNO | EPERM;
        let mut filters = Vec::new();

        filters.push(stmt(BPF_LD | BPF_W | BPF_ABS, ARCH_OFFSET));
        filters.push(jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
        filters.push(stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL));

        filters.push(stmt(BPF_LD | BPF_W | BPF_ABS, SYSCALL_NR_OFFSET));

        let deny_syscalls = [
            SYS_OPEN,
            SYS_OPENAT,
            SYS_OPENAT2,
            SYS_CREAT,
            SYS_UNLINK,
            SYS_UNLINKAT,
            SYS_RENAME,
            SYS_RENAMEAT,
            SYS_RENAMEAT2,
            SYS_MKDIR,
            SYS_MKDIRAT,
            SYS_RMDIR,
            SYS_LINK,
            SYS_LINKAT,
            SYS_SYMLINK,
            SYS_SYMLINKAT,
            SYS_READLINK,
            SYS_READLINKAT,
            SYS_CHDIR,
            SYS_FCHDIR,
            SYS_CHMOD,
            SYS_FCHMOD,
            SYS_FCHMODAT,
            SYS_CHOWN,
            SYS_FCHOWN,
            SYS_FCHOWNAT,
            SYS_LCHOWN,
            SYS_TRUNCATE,
            SYS_FTRUNCATE,
            SYS_STAT,
            SYS_LSTAT,
            SYS_FSTAT,
            SYS_NEWFSTATAT,
            SYS_ACCESS,
            SYS_FACCESSAT,
            SYS_FACCESSAT2,
            SYS_GETDENTS,
            SYS_GETDENTS64,
            SYS_SOCKET,
            SYS_CONNECT,
            SYS_ACCEPT,
            SYS_ACCEPT4,
            SYS_BIND,
            SYS_LISTEN,
            SYS_SENDTO,
            SYS_RECVFROM,
            SYS_SENDMSG,
            SYS_RECVMSG,
            SYS_SHUTDOWN,
            SYS_GETSOCKNAME,
            SYS_GETPEERNAME,
            SYS_SOCKETPAIR,
            SYS_SETSOCKOPT,
            SYS_GETSOCKOPT,
        ];

        for syscall in deny_syscalls {
            filters.push(jump(BPF_JMP | BPF_JEQ | BPF_K, syscall, 0, 1));
            filters.push(stmt(BPF_RET | BPF_K, deny_errno));
        }

        filters.push(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

        let prog = SockFprog {
            len: filters.len() as c_ushort,
            filter: filters.as_ptr(),
        };

        let rc = unsafe {
            syscall(
                SYS_SECCOMP,
                SECCOMP_SET_MODE_FILTER as c_uint,
                0u32,
                &prog as *const SockFprog,
            )
        };

        if rc != 0 {
            return Err(anyhow!(std::io::Error::last_os_error()))
                .context("sandbox: seccomp syscall failed");
        }

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

    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    #[test]
    fn sandbox_blocks_filesystem_access() {
        let result = run_in_sandbox(|| {
            let err = std::fs::File::open("/etc/hosts").unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");

        assert_eq!(result, Some(1));
    }

    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    #[test]
    fn sandbox_blocks_network_access() {
        let result = run_in_sandbox(|| {
            let err = std::net::TcpStream::connect("127.0.0.1:1").unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");

        assert_eq!(result, Some(1));
    }

    #[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
    #[test]
    fn sandbox_unavailable_is_a_conformance_error() {
        let err = run_in_sandbox(|| Ok(())).expect_err("sandbox should be unavailable");
        assert!(
            err.to_string().contains("sandbox unavailable"),
            "unexpected error: {err}"
        );
    }
}
