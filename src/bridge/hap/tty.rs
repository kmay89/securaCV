//! Turning terminal echo off — and getting it back on however we leave.
//!
//! Asking for a broker password means suppressing echo, and suppressing echo
//! means the terminal is now in a state only this process knows how to undo.
//! Every way out has to undo it, including the ways that are not returns.
//!
//! # The path that bites
//!
//! Ctrl-C at the password prompt. `read_line` is blocked, `SIGINT` arrives,
//! the default disposition kills the process — and the restore call sitting
//! after `read_line` never runs. The user is dropped back to a shell that no
//! longer echoes what they type, with nothing on screen to explain it and no
//! obvious fix short of `reset` or a new window. A `Drop` guard alone does
//! not save you here: destructors do not run on signal death.
//!
//! So [`EchoGuard`] does both. `Drop` covers returns, `?` and panics; a
//! handler on the four signals that would otherwise terminate us covers the
//! rest, restoring the saved settings and then re-raising with the default
//! disposition so the process still dies the way the user asked. A handler
//! that swallowed Ctrl-C would be its own bug — it would leave `read_line`
//! blocked with a half-typed password on the line.
//!
//! `tcsetattr`, `signal` and `raise` are all on POSIX's async-signal-safe
//! list, which is what makes the handler legal rather than merely lucky.
//!
//! # Why the readback
//!
//! `tcsetattr` reports success when it applied **any** of the changes asked
//! for, not all of them. A zero return is therefore not evidence that echo is
//! off, and treating it as such is how a password ends up on screen under a
//! promise that it would not be. [`hide_echo`] reads the settings back and
//! checks the bit it came to clear.
//!
//! # What this does not cover
//!
//! Ctrl-Z. Suspending at the prompt hands the terminal to a shell that
//! restores its own saved settings — with echo on — so a password typed
//! after `fg` is visible. Handling it properly means catching `SIGTSTP`,
//! restoring, re-raising, and re-suppressing on `SIGCONT`. It is a real gap,
//! named here rather than left for someone to discover, and it is narrower
//! than the one this module closes: it takes a deliberate suspend mid-prompt,
//! and it fails visibly (you can see the echo) rather than silently.

use std::io;

/// Held for as long as echo should stay off. Restores on drop.
///
/// Construct with [`hide_stdin_echo`]. Only one may exist at a time, which is
/// what makes the saved-settings slot exclusive.
pub struct EchoGuard {
    /// Never read — held for its `Drop`, which is the whole mechanism. The
    /// leading underscore says so to the dead-code lint, which is otherwise
    /// right to ask why a field nobody looks at exists.
    #[cfg(unix)]
    _inner: unix::Guard,
}

/// Turn off echo on standard input until the returned guard is dropped.
///
/// Errors when there is no terminal to speak of — piped input, a `cron` job,
/// a platform without termios. That is a condition to report to the user, not
/// to fail on: a visible password is worse than an invisible one, but both
/// are better than a setup that refuses to continue.
pub fn hide_stdin_echo() -> io::Result<EchoGuard> {
    #[cfg(unix)]
    {
        hide_echo(libc::STDIN_FILENO)
    }
    #[cfg(not(unix))]
    {
        Err(io::Error::other(
            "this platform has no termios, so echo cannot be turned off",
        ))
    }
}

/// Turn off echo on an arbitrary terminal fd.
///
/// Split out from [`hide_stdin_echo`] so the tests can drive a real pty
/// instead of asserting against a mock — the whole point of this module is
/// what a terminal actually does, which a mock cannot tell us.
#[cfg(unix)]
pub fn hide_echo(fd: std::os::unix::io::RawFd) -> io::Result<EchoGuard> {
    unix::Guard::new(fd).map(|_inner| EchoGuard { _inner })
}

/// Whether `fd` currently echoes what is typed at it.
///
/// Exposed for the tests, which have to check the terminal rather than our
/// bookkeeping — bookkeeping that agreed with itself was the original bug.
#[cfg(unix)]
pub fn echo_is_on(fd: std::os::unix::io::RawFd) -> io::Result<bool> {
    let term = unix::current(fd)?;
    Ok(term.c_lflag & libc::ECHO != 0)
}

#[cfg(unix)]
mod unix {
    use std::cell::UnsafeCell;
    use std::io;
    use std::mem::MaybeUninit;
    use std::os::unix::io::RawFd;
    use std::sync::atomic::{AtomicBool, Ordering};

    /// The signals that would otherwise kill us between "echo off" and "echo
    /// on". `SIGINT` is the one users actually hit; the rest are the same
    /// hole reached by `Ctrl-\`, `kill`, and a closed terminal.
    const FATAL: [libc::c_int; 4] = [libc::SIGINT, libc::SIGQUIT, libc::SIGTERM, libc::SIGHUP];

    /// The settings to put back, reachable from a signal handler.
    ///
    /// A `Mutex` would be wrong here — locking is not async-signal-safe, and
    /// a handler that blocked on a lock its own thread already held would
    /// hang the process instead of restoring it. Exclusivity comes from
    /// `ACTIVE` instead: at most one guard exists, it writes this slot before
    /// installing any handler, and the handler only reads it while `SAVED`
    /// says there is something there.
    struct Slot(UnsafeCell<MaybeUninit<libc::termios>>);

    // SAFETY: see `Slot`'s doc comment — access is serialized by `ACTIVE`,
    // and ordered against the handler by `SAVED`.
    unsafe impl Sync for Slot {}

    static SAVED_TERMIOS: Slot = Slot(UnsafeCell::new(MaybeUninit::uninit()));
    static SAVED: AtomicBool = AtomicBool::new(false);
    static ACTIVE: AtomicBool = AtomicBool::new(false);

    /// Put the terminal back, if we still owe it. Returns whether we did.
    ///
    /// `swap` rather than load-then-store so the guard and a signal arriving
    /// mid-drop cannot both restore — the loser sees `false` and does
    /// nothing, which is exactly right, since the winner already finished.
    fn restore(fd: RawFd) -> bool {
        if !SAVED.swap(false, Ordering::SeqCst) {
            return false;
        }
        // SAFETY: `SAVED` was true, so `Guard::new` wrote the slot before
        // installing the handler that could bring us here. `tcsetattr` is
        // async-signal-safe (POSIX.1-2008), so this is legal from a handler.
        unsafe {
            let saved = (*SAVED_TERMIOS.0.get()).assume_init_ref();
            libc::tcsetattr(fd, libc::TCSANOW, saved);
        }
        true
    }

    /// Restore the terminal, then die the way the user asked.
    ///
    /// Always on stdin: a handler cannot be told which fd the guard holds,
    /// and stdin is the only fd `hide_stdin_echo` ever suppresses. A guard on
    /// some other fd (the tests' pty) is restored by `Drop`, which is the
    /// only path those ever take.
    extern "C" fn restore_and_die(sig: libc::c_int) {
        restore(libc::STDIN_FILENO);
        // Re-raise with the default disposition rather than calling `exit`:
        // the parent shell reads the difference between "killed by SIGINT"
        // and "exited 130", and job control depends on it.
        unsafe {
            libc::signal(sig, libc::SIG_DFL);
            libc::raise(sig);
        }
    }

    /// Read a terminal's current settings.
    pub(super) fn current(fd: RawFd) -> io::Result<libc::termios> {
        let mut term = MaybeUninit::<libc::termios>::uninit();
        // SAFETY: `tcgetattr` either fills `term` or returns nonzero, and we
        // only assume it initialized on the zero return.
        if unsafe { libc::tcgetattr(fd, term.as_mut_ptr()) } != 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: `tcgetattr` returned zero, so it wrote the whole struct.
        Ok(unsafe { term.assume_init() })
    }

    pub(super) struct Guard {
        fd: RawFd,
        /// What each signal's disposition was before we took it, so dropping
        /// the guard leaves the process exactly as it found it.
        previous: [libc::sighandler_t; FATAL.len()],
    }

    impl Guard {
        pub(super) fn new(fd: RawFd) -> io::Result<Self> {
            // One at a time. Two guards would race for the one saved-settings
            // slot, and the second to drop would restore the first's
            // already-suppressed state — putting echo off, permanently.
            if ACTIVE.swap(true, Ordering::SeqCst) {
                return Err(io::Error::other("terminal echo is already suppressed"));
            }
            match Self::install(fd) {
                Ok(guard) => Ok(guard),
                Err(e) => {
                    ACTIVE.store(false, Ordering::SeqCst);
                    Err(e)
                }
            }
        }

        fn install(fd: RawFd) -> io::Result<Self> {
            let original = current(fd)?;

            // Save before touching anything, so a handler can never find
            // `SAVED` true with nothing behind it — and never find the
            // terminal suppressed with `SAVED` false, which is the ordering
            // that would strand echo off.
            // SAFETY: `ACTIVE` is ours, so nothing else can be in this slot,
            // and no handler is installed yet.
            unsafe { (*SAVED_TERMIOS.0.get()).write(original) };
            SAVED.store(true, Ordering::SeqCst);

            let mut quiet = original;
            quiet.c_lflag &= !libc::ECHO;
            // SAFETY: `quiet` is a valid termios read from this same fd.
            if unsafe { libc::tcsetattr(fd, libc::TCSANOW, &quiet) } != 0 {
                let e = io::Error::last_os_error();
                SAVED.store(false, Ordering::SeqCst);
                return Err(e);
            }

            // `tcsetattr` succeeds if it applied *any* requested change, so
            // check the one bit we came for rather than trusting the return.
            if current(fd)
                .map(|t| t.c_lflag & libc::ECHO != 0)
                .unwrap_or(true)
            {
                restore(fd);
                return Err(io::Error::other(
                    "the terminal accepted the request but kept echo on",
                ));
            }

            // Last, because until now `Drop` was the only cleanup needed and
            // a handler with nothing to restore is a handler that kills the
            // process for no reason.
            let mut previous = [0 as libc::sighandler_t; FATAL.len()];
            let handler = restore_and_die as extern "C" fn(libc::c_int);
            for (slot, sig) in previous.iter_mut().zip(FATAL) {
                // SAFETY: `handler` is an `extern "C"` fn of the right shape,
                // and does only async-signal-safe work.
                *slot = unsafe { libc::signal(sig, handler as libc::sighandler_t) };
            }

            Ok(Guard { fd, previous })
        }
    }

    impl Drop for Guard {
        fn drop(&mut self) {
            // Terminal first, dispositions second. The other order leaves a
            // window where the default disposition is back but echo is still
            // off — precisely the state this module exists to prevent, and
            // one a signal in that window would make permanent.
            restore(self.fd);
            for (slot, sig) in self.previous.iter().zip(FATAL) {
                // SAFETY: putting back a disposition `signal` handed us.
                unsafe { libc::signal(sig, *slot) };
            }
            ACTIVE.store(false, Ordering::SeqCst);
        }
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::io::RawFd;
    use std::sync::{Mutex, MutexGuard};

    /// Serializes these tests against each other.
    ///
    /// The guard is deliberately a *process*-wide singleton — a process has
    /// one terminal — so two tests running concurrently contend for the real
    /// thing and fail each other. That is the design working, not a flake, so
    /// the fix belongs here rather than in the module. `cargo test` is
    /// threaded by default and CI does not pass `--test-threads=1`.
    static ONE_AT_A_TIME: Mutex<()> = Mutex::new(());

    /// Take the lock, ignoring poisoning — the panic test poisons it on
    /// purpose, and every test restores the terminal before releasing.
    fn serialize() -> MutexGuard<'static, ()> {
        ONE_AT_A_TIME.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Open a pty pair, or `None` where the sandbox has no `/dev/pts`.
    ///
    /// `posix_openpt` and friends are in libc proper, unlike `openpty`, which
    /// lives in libutil and does not link everywhere.
    fn open_pty() -> Option<(RawFd, RawFd)> {
        // SAFETY: the standard pty-allocation dance; every pointer handed out
        // is checked before use.
        unsafe {
            let controller = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
            if controller < 0 {
                return None;
            }
            if libc::grantpt(controller) != 0 || libc::unlockpt(controller) != 0 {
                libc::close(controller);
                return None;
            }
            let name = libc::ptsname(controller);
            if name.is_null() {
                libc::close(controller);
                return None;
            }
            let device = libc::open(name, libc::O_RDWR | libc::O_NOCTTY);
            if device < 0 {
                libc::close(controller);
                return None;
            }
            Some((controller, device))
        }
    }

    fn close(fds: (RawFd, RawFd)) {
        // SAFETY: both came from this test's `open_pty` and are still open.
        unsafe {
            libc::close(fds.0);
            libc::close(fds.1);
        }
    }

    /// The property the module exists for, checked against a real terminal
    /// rather than our own bookkeeping — bookkeeping that agreed with itself
    /// is what shipped the bug.
    #[test]
    fn echo_goes_off_inside_the_guard_and_comes_back_after() {
        let _serial = serialize();
        let Some(pty) = open_pty() else {
            return; // no /dev/pts here; the no-terminal test still runs
        };
        let (_, device) = pty;

        assert!(echo_is_on(device).expect("a fresh pty has settings"));
        {
            let _guard = hide_echo(device).expect("suppresses echo");
            assert!(
                !echo_is_on(device).expect("still a terminal"),
                "the whole point: a password typed here must not appear"
            );
        }
        assert!(
            echo_is_on(device).expect("still a terminal"),
            "dropping the guard must hand the terminal back as it was found"
        );
        close(pty);
    }

    /// Restoration must survive the unwinding path too — `?` on the read, or
    /// a panic deeper in. `Drop` is what covers those, so assert it does.
    #[test]
    fn echo_comes_back_when_the_guard_is_dropped_by_a_panic() {
        let _serial = serialize();
        let Some(pty) = open_pty() else {
            return;
        };
        let (_, device) = pty;

        let caught = std::panic::catch_unwind(|| {
            let _guard = hide_echo(device).expect("suppresses echo");
            panic!("something failed while the password was being typed");
        });
        assert!(caught.is_err(), "the panic must still propagate");
        assert!(
            echo_is_on(device).expect("still a terminal"),
            "a panic must not strand the terminal with echo off"
        );
        close(pty);
    }

    /// Two guards would share one saved-settings slot, and the second to drop
    /// would restore the first's *suppressed* state — leaving echo off for
    /// good. Refusing the second is the fix.
    #[test]
    fn a_second_guard_is_refused_rather_than_racing_the_first() {
        let _serial = serialize();
        let Some(pty) = open_pty() else {
            return;
        };
        let (_, device) = pty;

        let first = hide_echo(device).expect("suppresses echo");
        assert!(
            hide_echo(device).is_err(),
            "a second guard must be refused, not silently allowed"
        );
        drop(first);
        assert!(echo_is_on(device).expect("still a terminal"));
        close(pty);
    }

    /// A refusal must not poison the next attempt. If the failure path left
    /// `ACTIVE` set, every later prompt would report "already suppressed" and
    /// echo the password — a fix that becomes the bug it replaced.
    #[test]
    fn a_failed_guard_does_not_poison_later_ones() {
        let _serial = serialize();
        // Not a terminal, so this cannot succeed.
        let err = hide_echo(-1);
        assert!(err.is_err(), "a bad fd cannot suppress echo");
        drop(err);

        let Some(pty) = open_pty() else {
            return;
        };
        let (_, device) = pty;
        let guard = hide_echo(device);
        assert!(
            guard.is_ok(),
            "an earlier failure must not lock out later prompts"
        );
        drop(guard);
        close(pty);
    }

    /// Piped stdin, `cron`, a CI runner: no terminal, so report it instead of
    /// pretending the password is hidden.
    #[test]
    fn a_non_terminal_is_an_error_not_a_false_promise() {
        let _serial = serialize();
        assert!(hide_echo(-1).is_err());
    }

    /// **The bug this module was written for.**
    ///
    /// Ctrl-C at the password prompt. `Drop` cannot cover it — the process is
    /// killed by the signal, and destructors do not run — so the only proof
    /// is to actually die of `SIGINT` while holding the guard and then look
    /// at the terminal that was left behind.
    ///
    /// So: fork, point the child's stdin at a pty, suppress echo, and have it
    /// take a real `SIGINT`. The parent then checks the pty. The child does
    /// nothing between `fork` and `raise` that the guard does not already do
    /// on this path in production.
    #[test]
    fn a_sigint_while_typing_leaves_the_terminal_usable() {
        let _serial = serialize();
        let Some(pty) = open_pty() else {
            return;
        };
        let (_, device) = pty;
        assert!(echo_is_on(device).expect("a fresh pty has settings"));

        // SAFETY: the child touches only the fd it was handed and exits via
        // the signal; it never returns into the test harness.
        let child = unsafe { libc::fork() };
        assert!(child >= 0, "fork failed");
        if child == 0 {
            // SAFETY: child of a fork, single-threaded by definition.
            unsafe {
                libc::dup2(device, libc::STDIN_FILENO);
                // Bound, not `hide_stdin_echo().is_err()` — a temporary would
                // drop at the end of that statement and restore echo before
                // the signal ever arrived, which is exactly how the first
                // draft of this test passed against a deliberately broken
                // handler. The guard has to still be held when SIGINT lands.
                let guard = hide_stdin_echo();
                if guard.is_err() {
                    libc::_exit(2);
                }
                // And prove the premise rather than assuming it: if echo is
                // somehow still on, dying with it on proves nothing at all.
                if !matches!(echo_is_on(libc::STDIN_FILENO), Ok(false)) {
                    libc::_exit(4);
                }
                // Exactly what a user pressing Ctrl-C delivers.
                libc::raise(libc::SIGINT);
                // Only reached if the handler swallowed it, which is itself a
                // bug — the prompt would still be blocked.
                libc::_exit(3);
            }
        }

        let mut status = 0;
        // SAFETY: reaping the child we just forked.
        unsafe { libc::waitpid(child, &mut status, 0) };
        assert!(
            libc::WIFSIGNALED(status) && libc::WTERMSIG(status) == libc::SIGINT,
            "the child must die of SIGINT while still holding the guard \
             (status {status:#x}; exit 2 = could not suppress echo, \
             4 = echo was never off so the test would be vacuous, \
             3 = the handler swallowed the signal)"
        );
        assert!(
            echo_is_on(device).expect("still a terminal"),
            "SIGINT during a password prompt must not strand the terminal with echo off"
        );
        close(pty);
    }
}
