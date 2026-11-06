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
    use libseccomp::{ScmpAction, ScmpArgCompare, ScmpCompareOp, ScmpFilterContext, ScmpSyscall};

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
        // Rust's std issues statx FIRST for fs::metadata / Path::exists and only
        // falls back to fstatat on ENOSYS — EPERM is not ENOSYS, so with statx
        // absent from this list the call simply succeeded inside the sandbox.
        "statx",
        "mknod",
        "mknodat",
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
        // Process isolation / memory safety. The child is forked from a parent that holds
        // signing-key material in memory; without these denials a malicious or buggy backend
        // could read the parent's live memory (process_vm_readv / ptrace) and exfiltrate it out
        // the result pipe, or shell out (execve). None are needed by detection/module compute.
        // (clone/mmap/futex are deliberately left allowed so tract's threaded inference works.)
        "ptrace",
        "process_vm_readv",
        "process_vm_writev",
        "execve",
        "execveat",
        "kill",
        "tkill",
        "tgkill",
        // The three signal syscalls above were denied, but a signal reaches
        // the daemon just as well through the queueing variants: with the
        // same uid, `rt_sigqueueinfo(getppid(), SIGKILL, ..)` from the child
        // stops the witness outright. Same uid also means any SysV segment
        // or queue the daemon could reach, the child could too.
        "rt_sigqueueinfo",
        "rt_tgsigqueueinfo",
        "shmget",
        "shmat",
        "shmdt",
        "msgget",
        "msgsnd",
        "msgrcv",
        "semget",
        // Path-existence oracles that need no open(): a watch on a path
        // answers whether it exists.
        "inotify_init1",
        "inotify_add_watch",
        "fanotify_init",
        "fanotify_mark",
        // Namespace and mount surgery, none of it detection work.
        "unshare",
        "setns",
        "mount",
        "umount2",
        "pivot_root",
        "chroot",
        // The batched socket calls bypass the single-message denials above.
        "sendmmsg",
        "recvmmsg",
    ];

    /// Newer filesystem/network escape hatches that bypass the classic
    /// `open`/`socket` denials: io_uring can submit OPENAT/CONNECT/SENDMSG
    /// without ever issuing those syscalls; memfd/handle/mount openers and
    /// pidfd_getfd (which can pull a live fd from another process) are further
    /// file/descriptor vectors. These are resolved BEST-EFFORT: a name an older
    /// libseccomp cannot map is skipped rather than failing the whole
    /// (fail-closed) sandbox and breaking legitimate module execution.
    const DENY_SYSCALLS_BEST_EFFORT: &[&str] = &[
        "io_uring_setup",
        "io_uring_enter",
        "io_uring_register",
        "memfd_create",
        "open_by_handle_at",
        "name_to_handle_at",
        "pidfd_open",
        "pidfd_getfd",
        "open_tree",
        "fsopen",
        "fsconfig",
        "fsmount",
        "move_mount",
        // A grandchild could hold the result pipe open forever (a hung
        // ingest) or outlive the reaper. `clone` is filtered by flags below
        // (threads stay allowed for tract); these have no thread form.
        "fork",
        "vfork",
        "pidfd_send_signal",
        "inotify_init",
        "uselib",
    ];

    /// Longest a child may burn CPU before the kernel ends it (`RLIMIT_CPU`,
    /// SIGXCPU then SIGKILL). Detection on a Pi is seconds per frame at worst;
    /// a module spinning for longer is wedged or hostile.
    const CHILD_CPU_SECONDS: u64 = 30;

    /// Longest the parent waits for the child's reply before killing it. A
    /// `detect()` that never returns used to block the single ingest thread
    /// in a blocking `read(2)` forever: the witness stopped witnessing and
    /// sealed no gap record. Wall-clock, so a child blocked (not spinning)
    /// is bounded too.
    const CHILD_REPLY_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(45);

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
        let mut ctx = ScmpFilterContext::new(ScmpAction::Allow).context("sandbox: seccomp init")?;
        for name in DENY_SYSCALLS {
            let syscall = ScmpSyscall::from_name(name)
                .with_context(|| format!("sandbox: unknown syscall name {name}"))?;
            ctx.add_rule(ScmpAction::Errno(libc::EPERM), syscall)
                .with_context(|| format!("sandbox: failed to deny {name}"))?;
        }
        // Best-effort layer: deny the newer escape-hatch syscalls where this
        // libseccomp knows them. A name it cannot map is skipped (never fatal)
        // so the sandbox keeps working on older toolchains.
        for name in DENY_SYSCALLS_BEST_EFFORT {
            if let Ok(syscall) = ScmpSyscall::from_name(name) {
                let _ = ctx.add_rule(ScmpAction::Errno(libc::EPERM), syscall);
            }
        }
        // Processes, not threads: `clone` without CLONE_THREAD is a fork by
        // another name, so it is refused by flag; a thread (what tract's
        // inference pool creates) keeps working. `clone3` takes its flags
        // through a struct the filter cannot inspect, so it answers ENOSYS —
        // glibc then falls back to `clone`, which the flag rule sees. (EPERM
        // would not trigger that fallback and would break thread creation.)
        let clone = ScmpSyscall::from_name("clone").context("sandbox: unknown syscall clone")?;
        ctx.add_rule_conditional(
            ScmpAction::Errno(libc::EPERM),
            clone,
            &[ScmpArgCompare::new(
                0,
                ScmpCompareOp::MaskedEqual(libc::CLONE_THREAD as u64),
                0,
            )],
        )
        .context("sandbox: failed to filter clone")?;
        if let Ok(clone3) = ScmpSyscall::from_name("clone3") {
            let _ = ctx.add_rule(ScmpAction::Errno(libc::ENOSYS), clone3);
        }
        // The prior filter killed on arch mismatch; libseccomp binds the filter
        // to the native architecture, preserving intent without hardcoding AUDIT_ARCH_*.
        ctx.load().context("sandbox: seccomp load failed")?;

        Ok(())
    }

    /// `read(2)` the whole buffer, giving up at `deadline`: `poll(2)` bounds
    /// each wait, so a child that never replies (or replies one byte at a
    /// time) cannot hold the ingest thread past the budget.
    fn read_exact(fd: c_int, buf: &mut [u8], deadline: std::time::Instant) -> Result<()> {
        let mut offset = 0;
        while offset < buf.len() {
            let remaining = deadline.saturating_duration_since(std::time::Instant::now());
            if remaining.is_zero() {
                return Err(anyhow!(
                    "sandbox: child did not reply within {:?}; killed",
                    CHILD_REPLY_TIMEOUT
                ));
            }
            let mut pfd = libc::pollfd {
                fd,
                events: libc::POLLIN,
                revents: 0,
            };
            let wait_ms = remaining.as_millis().min(i32::MAX as u128) as c_int;
            let ready = unsafe { libc::poll(&mut pfd, 1, wait_ms) };
            if ready < 0 {
                let err = std::io::Error::last_os_error();
                if err.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(anyhow!(err)).context("sandbox: poll on result pipe failed");
            }
            if ready == 0 {
                continue; // deadline check at the top of the loop
            }
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

    /// Parent-side ownership of a forked child and the read end of its result
    /// pipe. `finish()` is the orderly path (child already exited, or about
    /// to); `Drop` is the early-return path — it closes the fd and reaps,
    /// killing a child that has not exited yet so a hung backend cannot pin
    /// the parent in waitpid.
    struct ChildGuard {
        pid: libc::pid_t,
        read_fd: c_int,
    }

    impl ChildGuard {
        fn finish(&mut self) {
            self.close_fd();
            if self.pid > 0 {
                let mut status = 0;
                let _ = unsafe { waitpid(self.pid, &mut status as *mut c_int, 0) };
                self.pid = 0;
            }
        }

        fn close_fd(&mut self) {
            if self.read_fd >= 0 {
                unsafe { close(self.read_fd) };
                self.read_fd = -1;
            }
        }
    }

    impl Drop for ChildGuard {
        fn drop(&mut self) {
            self.close_fd();
            if self.pid > 0 {
                let mut status = 0;
                // Reap if it already exited; otherwise kill and reap. WNOHANG
                // first so a child that is done is not signaled needlessly.
                let rc = unsafe { waitpid(self.pid, &mut status as *mut c_int, libc::WNOHANG) };
                if rc == 0 {
                    unsafe {
                        libc::kill(self.pid, libc::SIGKILL);
                        waitpid(self.pid, &mut status as *mut c_int, 0);
                    }
                }
                self.pid = 0;
            }
        }
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
            unsafe {
                close(fds[0]);
                // Prevent core dumps that could leak inherited key material.
                libc::prctl(libc::PR_SET_DUMPABLE, 0, 0, 0, 0);
                // Die with the thread that forked us: a child must never
                // outlive the reaper that bounds it.
                libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL, 0, 0, 0);
                // The std streams are the daemon's log sink (journald, a
                // container collector): `write(2)` stays allowed for the
                // result pipe, so stdout/stderr were a second channel out
                // of the sandbox. Point all three at /dev/null before the
                // filter (open is denied after it).
                let devnull = libc::open(c"/dev/null".as_ptr(), libc::O_RDWR | libc::O_CLOEXEC);
                if devnull >= 0 {
                    for fd in 0..3 {
                        libc::dup2(devnull, fd);
                    }
                    if devnull > 2 {
                        libc::close(devnull);
                    }
                }
                // A module that spins forever is ended by the kernel, not
                // waited on by the daemon.
                let cpu = libc::rlimit {
                    rlim_cur: CHILD_CPU_SECONDS,
                    rlim_max: CHILD_CPU_SECONDS + 5,
                };
                libc::setrlimit(libc::RLIMIT_CPU, &cpu);
                // Drop every inherited descriptor except std streams and the
                // result-pipe write end. The parent holds the SQLCipher DB
                // handle, MQTT/broker sockets, and config fds; the seccomp
                // filter cannot deny write()/read() (the pipe needs them), so
                // an inherited live fd would be a ready-made exfiltration or
                // log-corruption channel that the syscall denylist can't close.
                // The pipe write end is preserved so the response can still be
                // sent.
                let keep = fds[1] as libc::c_uint;
                let mut closed = true;
                if keep > 3 {
                    closed &=
                        libc::syscall(libc::SYS_close_range, 3 as libc::c_uint, keep - 1, 0) == 0;
                }
                closed &= libc::syscall(libc::SYS_close_range, keep + 1, libc::c_uint::MAX, 0) == 0;
                if !closed {
                    // close_range(2) is Linux >= 5.9. On an older kernel (a
                    // Buster-era Pi, an 18.04 host) it is ENOSYS, and silently
                    // skipping it would leave every parent descriptor open
                    // inside the filtered child — so sweep by hand. This must
                    // run before install_filter(): the filter may deny what
                    // the sweep needs.
                    let mut lim = libc::rlimit {
                        rlim_cur: 0,
                        rlim_max: 0,
                    };
                    let max_fd = if libc::getrlimit(libc::RLIMIT_NOFILE, &mut lim) == 0 {
                        lim.rlim_cur.min(65_536) as libc::c_int
                    } else {
                        1024
                    };
                    for fd in 3..max_fd {
                        if fd as libc::c_uint != keep {
                            libc::close(fd);
                        }
                    }
                }
            }
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

        // Every exit from here on — the happy path AND every `?` — must close
        // the read end and reap the child. Before this guard a child that
        // died before replying (SIGKILL, OOM, a crash inside f()) made
        // read_exact return early, skipping both: one leaked fd and one
        // zombie per failed frame, at ingest rate, until the daemon hit
        // EMFILE. The guard kills a child that is still running so a hung
        // backend cannot hold the reaper either.
        let mut child = ChildGuard {
            pid,
            read_fd: fds[0],
        };

        // SECURITY: Cap IPC payload to prevent a malicious/corrupted child
        // from sending a huge length value and causing OOM in the parent.
        // 16 MiB is generous for any serialized sandbox response.
        const MAX_SANDBOX_PAYLOAD: u64 = 16 * 1024 * 1024;

        let deadline = std::time::Instant::now() + CHILD_REPLY_TIMEOUT;
        let mut len_bytes = [0u8; 8];
        read_exact(child.read_fd, &mut len_bytes, deadline)?;
        let len = u64::from_le_bytes(len_bytes);
        if len > MAX_SANDBOX_PAYLOAD {
            return Err(anyhow!(
                "sandbox: response too large ({} bytes, max {})",
                len,
                MAX_SANDBOX_PAYLOAD
            ));
        }
        let len = len as usize;
        let mut payload = vec![0u8; len];
        read_exact(child.read_fd, &mut payload, deadline)?;
        child.finish();

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
    fn sandbox_blocks_metadata_via_statx() {
        // std::fs::metadata goes through statx first; before statx joined the
        // denylist this call SUCCEEDED inside the sandbox.
        let result = run_in_sandbox(|| {
            let err = std::fs::metadata("/etc/hosts").unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");

        assert_eq!(result, Some(1));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_child_dying_before_reply_leaks_neither_fd_nor_zombie() {
        fn open_fds() -> usize {
            std::fs::read_dir("/proc/self/fd")
                .map(|d| d.count())
                .unwrap_or(0)
        }
        let before = open_fds();
        for _ in 0..8 {
            let result: Result<u8> = run_in_sandbox(|| {
                // Die without writing a response — the short-read path.
                unsafe { libc::_exit(3) }
            });
            assert!(
                result.is_err(),
                "a child that never replies must surface an error"
            );
        }
        // Eight failed children, zero leaked read-ends (read_dir itself
        // opens one fd transiently, so allow that much slack). The count is
        // process-wide and the other tests in this binary run beside this
        // one, opening pipes and sockets of their own for a moment, so a
        // single sample can read high on a slow runner; a leaked read end
        // never goes away, a neighbor's descriptor does, so sample until
        // the count settles or a bounded wait runs out.
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        let mut after = open_fds();
        while after > before + 1 && std::time::Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(20));
            after = open_fds();
        }
        assert!(
            after <= before + 1,
            "fd leak: {before} before, {after} after"
        );
        // No zombies: every child pid has been reaped, so a reap of "any
        // child" finds nothing left.
        let rc = unsafe { libc::waitpid(-1, std::ptr::null_mut(), libc::WNOHANG) };
        assert!(rc <= 0, "an unreaped sandbox child remained");
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

    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_blocks_process_exec() {
        // A backend that shells out (execve) must be denied with EPERM *specifically* — asserting
        // the errno rather than just "errored" avoids a false pass if the binary were missing.
        // clone/fork stay allowed so tract's threaded inference is unaffected.
        let exec_errno = run_in_sandbox(|| {
            let err = std::process::Command::new("/bin/true")
                .output()
                .unwrap_err();
            Ok(err.raw_os_error())
        })
        .expect("sandbox run should succeed");
        assert_eq!(
            exec_errno,
            Some(libc::EPERM),
            "execve should be denied with EPERM inside the sandbox"
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn sandbox_blocks_malicious_detector_from_reading_files() {
        use crate::detect::{DetectionCapability, DetectionResult, DetectorBackend, SizeClass};

        // A backend that tries to exfiltrate by reading a file during detect(). It records the
        // read's errno so the test can assert the read was *denied* (EPERM), not merely that it
        // failed — a missing file would also fail and could mask a regression.
        struct MaliciousBackend {
            read_errno: Option<i32>,
        }
        impl DetectorBackend for MaliciousBackend {
            fn name(&self) -> &'static str {
                "malicious"
            }
            fn supports(&self, _c: DetectionCapability) -> bool {
                true
            }
            fn detect(&mut self, _px: &[u8], _w: u32, _h: u32) -> anyhow::Result<DetectionResult> {
                // /proc/self/cmdline always exists, so a non-EPERM failure can't hide a regression.
                let err = std::fs::read("/proc/self/cmdline").err();
                self.read_errno = err.as_ref().and_then(|e| e.raw_os_error());
                Ok(DetectionResult {
                    motion_detected: err.is_none(), // doubles as "did the exfil read succeed?"
                    detections: Vec::new(),
                    confidence: 0.0,
                    size_class: SizeClass::Unknown,
                })
            }
        }

        let mut backend = MaliciousBackend { read_errno: None };
        // Run the detector exactly as the kernel does: inside the forked, seccomp child.
        let (exfiltrated, read_errno) = run_in_sandbox(|| {
            let r = backend.detect(&[0u8; 12], 2, 2)?;
            Ok((r.motion_detected, backend.read_errno))
        })
        .expect("sandbox run should succeed");
        assert!(
            !exfiltrated,
            "a detection backend must not be able to read files inside the sandbox"
        );
        assert_eq!(
            read_errno,
            Some(libc::EPERM),
            "the file read must be denied with EPERM, not merely fail"
        );
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
