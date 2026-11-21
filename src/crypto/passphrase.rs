//! Reading a passphrase without it touching argv or the environment.
//!
//! Two sources exist, and only two:
//!
//! - the controlling terminal, `/dev/tty`, with echo turned off (libc
//!   `tcgetattr` / `tcsetattr`) for the length of one line;
//! - a file descriptor the caller names (`--passphrase-fd N`), read one line
//!   per passphrase, so a script can pipe it in without it ever being a
//!   command-line argument. An fd that is itself a terminal is read like the
//!   controlling terminal: echo off, and the prompt shown on it when it is
//!   open for writing.
//!
//! While echo is off ([`EchoGuard`]) the saved terminal settings come back
//! on every return path, error returns included, and on the four signals
//! that would otherwise kill the process mid-prompt (SIGINT, SIGQUIT,
//! SIGTERM, SIGHUP): a handler restores the settings and re-raises, so the
//! process still dies of the signal. Job-control stop (SIGTSTP, Ctrl-Z) is
//! ignored for that window, so the process is never stopped with echo off
//! and resumed with the shell's echo back on. Two paths are not covered: a
//! release build aborts on panic (`panic = "abort"`) without running Drop,
//! and SIGKILL cannot be caught; after either, `stty sane` restores the
//! terminal. A line longer than [`MAX_PASSPHRASE_LEN`] typed at a terminal is
//! refused and the rest of it discarded from the terminal's input queue, so
//! no piece of it is left for the shell to read, show, and save in history.
//!
//! There is deliberately no argv flag and no environment variable: argv is
//! world-readable through `/proc/<pid>/cmdline` and lands in shell history,
//! and the environment is inherited by every child and readable through
//! `/proc/<pid>/environ`. A caller with neither a terminal nor an fd gets
//! [`PassphraseError::NoSource`] and refuses in its own words.
//!
//! The reader is generic: it knows nothing about what the passphrase opens.
//! Every value it returns is `Zeroizing`, and it never formats one.

use anyhow::anyhow;
use zeroize::Zeroizing;

/// The longest passphrase line accepted, in bytes.
pub(crate) const MAX_PASSPHRASE_LEN: usize = 1024;

/// Why no passphrase was read.
#[derive(Debug)]
pub(crate) enum PassphraseError {
    /// No `--passphrase-fd` was given and there is no controlling terminal.
    /// The caller turns this into its own refusal.
    NoSource,
    /// A source exists but reading from it failed. The message names no
    /// path and carries no passphrase byte.
    Failed(anyhow::Error),
}

impl std::fmt::Display for PassphraseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PassphraseError::NoSource => f.write_str("no terminal and no --passphrase-fd"),
            PassphraseError::Failed(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for PassphraseError {}

fn failed(msg: impl std::fmt::Display) -> PassphraseError {
    PassphraseError::Failed(anyhow!("{msg}"))
}

/// Where passphrases come from for one command: the fd named by
/// `--passphrase-fd`, or else the controlling terminal. One value per
/// command; each read takes the next line from the fd, or prompts again on
/// the terminal.
pub(crate) struct PassphraseInput {
    fd: Option<i32>,
    terminal: bool,
}

impl PassphraseInput {
    /// `passphrase_fd` is the value of `--passphrase-fd`, if given. The fd is
    /// checked to be open now; nothing is read until [`Self::read`].
    pub(crate) fn new(passphrase_fd: Option<i32>) -> Result<Self, PassphraseError> {
        if let Some(fd) = passphrase_fd {
            check_fd_open(fd)?;
        }
        Ok(Self {
            fd: passphrase_fd,
            terminal: true,
        })
    }

    /// An input with no fd that never opens the terminal: every read is
    /// [`PassphraseError::NoSource`]. For tests, which must not prompt on a
    /// developer's terminal.
    #[cfg(test)]
    pub(crate) fn without_terminal() -> Self {
        Self {
            fd: None,
            terminal: false,
        }
    }

    /// Read one passphrase: the next line of the fd, or one line typed at the
    /// terminal after `prompt` with echo off. An fd that is a terminal is
    /// read with echo off too, after `prompt` when the fd is open for
    /// writing. An empty passphrase is refused.
    pub(crate) fn read(&mut self, prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
        match self.fd {
            Some(fd) => read_from_fd(fd, prompt),
            None if self.terminal => read_from_tty(prompt),
            None => Err(PassphraseError::NoSource),
        }
    }

    /// Read a NEW passphrase. From a plain fd (a pipe, a file) it is one
    /// line. Where a prompt is shown (the terminal, or an fd that is a
    /// terminal open for writing) it is typed twice (`prompt`, then
    /// `repeat_prompt`) and must match, so a typo typed blind never becomes
    /// the passphrase.
    pub(crate) fn read_new(
        &mut self,
        prompt: &str,
        repeat_prompt: &str,
    ) -> Result<Zeroizing<String>, PassphraseError> {
        let first = self.read(prompt)?;
        let confirm = match self.fd {
            Some(fd) => fd_prompts(fd),
            None => true,
        };
        if !confirm {
            return Ok(first);
        }
        let second = self.read(repeat_prompt)?;
        if first.as_bytes() != second.as_bytes() {
            return Err(failed("the two passphrases differ"));
        }
        Ok(first)
    }
}

#[cfg(unix)]
fn check_fd_open(fd: i32) -> Result<(), PassphraseError> {
    // SAFETY: F_GETFD only queries the descriptor table; any int is safe.
    if fd < 0 || unsafe { libc::fcntl(fd, libc::F_GETFD) } == -1 {
        return Err(failed(format!(
            "--passphrase-fd {fd} is not an open file descriptor"
        )));
    }
    Ok(())
}

#[cfg(not(unix))]
fn check_fd_open(_fd: i32) -> Result<(), PassphraseError> {
    Err(failed("--passphrase-fd needs a Unix host"))
}

/// Whether `fd` is a terminal.
#[cfg(unix)]
fn fd_is_tty(fd: i32) -> bool {
    // SAFETY: isatty only queries the fd.
    unsafe { libc::isatty(fd) == 1 }
}

/// Whether a read from `fd` shows its prompt: `fd` is a terminal open for
/// writing. A terminal opened read-only (`3</dev/tty`) is still read with
/// echo off, but nothing can be written to it.
#[cfg(unix)]
fn fd_prompts(fd: i32) -> bool {
    // SAFETY: F_GETFL only queries the descriptor's status flags.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    fd_is_tty(fd) && flags >= 0 && flags & libc::O_ACCMODE != libc::O_RDONLY
}

#[cfg(not(unix))]
fn fd_prompts(_fd: i32) -> bool {
    false
}

/// One passphrase from the fd `--passphrase-fd` names: echo off when it is
/// a terminal, else a plain line read.
#[cfg(unix)]
fn read_from_fd(fd: i32, prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
    if fd_is_tty(fd) {
        return read_line_echo_off(fd, fd_prompts(fd).then_some(prompt));
    }
    read_passphrase_line(fd)
}

#[cfg(not(unix))]
fn read_from_fd(fd: i32, _prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
    read_passphrase_line(fd)
}

/// Turn the raw bytes of one line into a passphrase: drop one trailing
/// `\r`, then require UTF-8 and at least one byte.
fn finish_line(mut line: Zeroizing<Vec<u8>>) -> Result<Zeroizing<String>, PassphraseError> {
    if line.last() == Some(&b'\r') {
        line.pop();
    }
    if line.is_empty() {
        return Err(failed("the passphrase is empty"));
    }
    let text = std::str::from_utf8(&line).map_err(|_| failed("the passphrase is not UTF-8"))?;
    Ok(Zeroizing::new(text.to_string()))
}

/// Read bytes from `fd` up to and excluding the next `\n` (or end of
/// input), one byte at a time so nothing past the line is consumed: a
/// second passphrase on the same fd stays for the next read.
#[cfg(unix)]
fn read_raw_line(fd: i32) -> Result<(Zeroizing<Vec<u8>>, bool), PassphraseError> {
    use zeroize::Zeroize;
    // Allocated once, at the cap: the length check below runs before every
    // push, so the buffer never grows. A Vec that grew would free its
    // outgrown buffers with passphrase bytes still in them, and Zeroizing
    // wipes only the buffer it holds at drop.
    let mut line = Zeroizing::new(Vec::with_capacity(MAX_PASSPHRASE_LEN));
    let mut saw_any = false;
    loop {
        let mut byte = 0u8;
        // SAFETY: reads at most one byte into a valid one-byte buffer.
        let n = unsafe { libc::read(fd, (&mut byte as *mut u8).cast(), 1) };
        if n < 0 {
            let err = std::io::Error::last_os_error();
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(failed(format!("reading the passphrase failed: {err}")));
        }
        if n == 0 {
            return Ok((line, saw_any));
        }
        saw_any = true;
        if byte == b'\n' {
            return Ok((line, true));
        }
        if line.len() == MAX_PASSPHRASE_LEN {
            byte.zeroize();
            return Err(failed(format!(
                "the passphrase is longer than {MAX_PASSPHRASE_LEN} bytes"
            )));
        }
        line.push(byte);
        byte.zeroize();
    }
}

/// One passphrase line from a `--passphrase-fd` that is not a terminal.
#[cfg(unix)]
fn read_passphrase_line(fd: i32) -> Result<Zeroizing<String>, PassphraseError> {
    let (line, saw_any) = read_raw_line(fd)?;
    if !saw_any {
        return Err(failed(
            "--passphrase-fd reached its end before a passphrase line",
        ));
    }
    finish_line(line)
}

#[cfg(not(unix))]
fn read_passphrase_line(_fd: i32) -> Result<Zeroizing<String>, PassphraseError> {
    Err(failed("--passphrase-fd needs a Unix host"))
}

#[cfg(unix)]
pub(crate) use echo::EchoGuard;

/// Terminal echo off, and back on however the window ends.
#[cfg(unix)]
mod echo {
    use super::{failed, PassphraseError};
    use std::cell::UnsafeCell;
    use std::mem::MaybeUninit;
    use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};

    /// The signals that would otherwise kill the process between "echo off"
    /// and "echo on": Ctrl-C, Ctrl-\, `kill`, and a closed terminal.
    const FATAL: [libc::c_int; 4] = [libc::SIGINT, libc::SIGQUIT, libc::SIGTERM, libc::SIGHUP];

    /// The settings to put back, reachable from a signal handler. A lock is
    /// not async-signal-safe, so exclusivity comes from `ACTIVE` (at most
    /// one guard exists) and the handler reads the slot only while `SAVED`
    /// says it holds something.
    struct Slot(UnsafeCell<MaybeUninit<libc::termios>>);

    // SAFETY: written only by the one guard `ACTIVE` admits, before `SAVED`
    // is set; read only after `SAVED` is swapped from true.
    unsafe impl Sync for Slot {}

    static SAVED_TERMIOS: Slot = Slot(UnsafeCell::new(MaybeUninit::uninit()));
    static SAVED: AtomicBool = AtomicBool::new(false);
    static ACTIVE: AtomicBool = AtomicBool::new(false);
    static FD: AtomicI32 = AtomicI32::new(-1);

    /// Put the terminal back, if it is still owed. `swap` so the guard's
    /// Drop and a signal arriving mid-drop never both restore.
    fn restore() {
        if !SAVED.swap(false, Ordering::SeqCst) {
            return;
        }
        // SAFETY: `SAVED` was true, so the slot was written from this fd by
        // tcgetattr; tcsetattr is async-signal-safe (POSIX.1-2008).
        unsafe {
            let saved = (*SAVED_TERMIOS.0.get()).assume_init_ref();
            libc::tcsetattr(FD.load(Ordering::SeqCst), libc::TCSANOW, saved);
        }
    }

    /// Restore the terminal, then die of the signal the user sent: the
    /// default disposition, re-raised, so the parent sees "killed by
    /// SIGINT", not an exit code. The signal is blocked while this runs, so
    /// the re-raise is delivered when it returns.
    pub(super) extern "C" fn restore_and_die(sig: libc::c_int) {
        // A signal sent from outside (SIGTERM, a `kill -INT`) does not flush
        // the terminal the way Ctrl-C does: discard what was typed before
        // echo comes back, or a half-typed passphrase waits in the queue
        // for the next reader. Only while the guard still owes the restore.
        if SAVED.load(Ordering::SeqCst) {
            // SAFETY: tcflush is async-signal-safe and only discards this
            // terminal's pending input.
            unsafe { libc::tcflush(FD.load(Ordering::SeqCst), libc::TCIFLUSH) };
        }
        restore();
        // SAFETY: signal and raise are async-signal-safe.
        unsafe {
            libc::signal(sig, libc::SIG_DFL);
            libc::raise(sig);
        }
    }

    /// What one signal's disposition was before the guard took it.
    pub(super) fn current_action(sig: libc::c_int) -> libc::sigaction {
        // SAFETY: sigaction is plain data; a null new action only queries.
        unsafe {
            let mut old: libc::sigaction = std::mem::zeroed();
            libc::sigaction(sig, std::ptr::null(), &mut old);
            old
        }
    }

    fn set_action(sig: libc::c_int, action: &libc::sigaction) {
        // SAFETY: `action` is a valid disposition, from sigaction or built
        // below with an `extern "C"` handler of the right shape.
        unsafe {
            libc::sigaction(sig, action, std::ptr::null_mut());
        }
    }

    /// Echo off on one terminal until dropped (the newline still echoes, so
    /// the next output starts on a fresh line). While held, SIGINT, SIGQUIT,
    /// SIGTERM and SIGHUP restore the terminal and re-raise (a signal the
    /// process already ignored stays ignored), and SIGTSTP is ignored. Drop
    /// restores the terminal first, then every disposition. One guard at a
    /// time per process.
    pub(crate) struct EchoGuard {
        fatal: [Option<libc::sigaction>; FATAL.len()],
        tstp: libc::sigaction,
    }

    impl EchoGuard {
        /// Turn echo off on `fd`. Not a terminal is
        /// [`PassphraseError::NoSource`]. Allocates nothing on success.
        pub(crate) fn engage(fd: i32) -> Result<Self, PassphraseError> {
            // SAFETY: isatty only queries the fd.
            if unsafe { libc::isatty(fd) } != 1 {
                return Err(PassphraseError::NoSource);
            }
            if ACTIVE.swap(true, Ordering::SeqCst) {
                return Err(failed(
                    "another passphrase prompt already holds the terminal",
                ));
            }
            let mut saved = MaybeUninit::<libc::termios>::uninit();
            // SAFETY: tcgetattr fills `saved` or returns nonzero.
            if unsafe { libc::tcgetattr(fd, saved.as_mut_ptr()) } != 0 {
                ACTIVE.store(false, Ordering::SeqCst);
                return Err(PassphraseError::NoSource);
            }
            // SAFETY: tcgetattr returned zero, so it wrote the whole struct.
            let saved = unsafe { saved.assume_init() };
            // Save before anything changes, so a handler never finds the
            // terminal quiet with nothing to restore.
            // SAFETY: `ACTIVE` is ours and no handler is installed yet.
            unsafe { (*SAVED_TERMIOS.0.get()).write(saved) };
            FD.store(fd, Ordering::SeqCst);
            SAVED.store(true, Ordering::SeqCst);

            // Handlers BEFORE echo goes off, so no signal finds the terminal
            // quiet and the default disposition in place.
            let mut fatal = [None; FATAL.len()];
            for (slot, sig) in fatal.iter_mut().zip(FATAL) {
                let previous = current_action(sig);
                if previous.sa_sigaction == libc::SIG_IGN {
                    continue; // ignored (nohup, a background job): keep it so
                }
                // SAFETY: sigaction is plain data; sigfillset fills the mask.
                let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
                action.sa_sigaction =
                    restore_and_die as extern "C" fn(libc::c_int) as libc::sighandler_t;
                // SAFETY: a valid mask to fill.
                unsafe { libc::sigfillset(&mut action.sa_mask) };
                set_action(sig, &action);
                *slot = Some(previous);
            }
            let tstp = current_action(libc::SIGTSTP);
            // SAFETY: sigaction is plain data; SIG_IGN is a valid disposition.
            let mut ignore: libc::sigaction = unsafe { std::mem::zeroed() };
            ignore.sa_sigaction = libc::SIG_IGN;
            set_action(libc::SIGTSTP, &ignore);
            // From here Drop undoes everything above.
            let guard = EchoGuard { fatal, tstp };

            let mut quiet = saved;
            quiet.c_lflag &= !libc::ECHO;
            quiet.c_lflag |= libc::ECHONL;
            // SAFETY: attributes derived from the ones just read.
            if unsafe { libc::tcsetattr(fd, libc::TCSAFLUSH, &quiet) } != 0 {
                return Err(failed(format!(
                    "could not turn terminal echo off: {}",
                    std::io::Error::last_os_error()
                )));
            }
            // tcsetattr succeeds when it applied ANY of the changes, so read
            // back the one bit this guard exists for.
            let mut now = MaybeUninit::<libc::termios>::uninit();
            // SAFETY: as above; `now` is read only on a zero return.
            let echo_off = unsafe {
                libc::tcgetattr(fd, now.as_mut_ptr()) == 0
                    && now.assume_init().c_lflag & libc::ECHO == 0
            };
            if !echo_off {
                return Err(failed("the terminal kept echo on"));
            }
            Ok(guard)
        }
    }

    impl Drop for EchoGuard {
        fn drop(&mut self) {
            // Terminal first, dispositions second: the other order leaves a
            // window with the default disposition back and echo still off.
            restore();
            for (slot, sig) in self.fatal.iter().zip(FATAL) {
                if let Some(previous) = slot {
                    set_action(sig, previous);
                }
            }
            set_action(libc::SIGTSTP, &self.tstp);
            FD.store(-1, Ordering::SeqCst);
            ACTIVE.store(false, Ordering::SeqCst);
        }
    }
}

/// Read one line from the terminal `fd` with echo off ([`EchoGuard`]),
/// after writing `prompt` to it when given. Echo is turned off BEFORE the
/// prompt is written, so anything typed after the prompt appears is never
/// shown; the newline still echoes (ECHONL) so the next output starts on a
/// fresh line. If the read fails (a line longer than
/// [`MAX_PASSPHRASE_LEN`]), the terminal's pending input is discarded before
/// echo comes back, so the rest of the line never reaches the shell. `fd`
/// must be a terminal; anything else is [`PassphraseError::NoSource`].
#[cfg(unix)]
pub(crate) fn read_line_echo_off(
    fd: i32,
    prompt: Option<&str>,
) -> Result<Zeroizing<String>, PassphraseError> {
    let guard = EchoGuard::engage(fd)?;
    if let Some(prompt) = prompt {
        write_all_fd(fd, prompt.as_bytes())?;
    }
    let line = match read_raw_line(fd) {
        Ok((line, _)) => line,
        Err(e) => {
            // SAFETY: tcflush only discards this terminal's pending input.
            unsafe { libc::tcflush(fd, libc::TCIFLUSH) };
            return Err(e);
        }
    };
    drop(guard);
    finish_line(line)
}

#[cfg(unix)]
fn write_all_fd(fd: i32, mut bytes: &[u8]) -> Result<(), PassphraseError> {
    while !bytes.is_empty() {
        // SAFETY: writes from a valid slice.
        let n = unsafe { libc::write(fd, bytes.as_ptr().cast(), bytes.len()) };
        if n < 0 {
            let err = std::io::Error::last_os_error();
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(failed(format!("writing the prompt failed: {err}")));
        }
        bytes = &bytes[n as usize..];
    }
    Ok(())
}

/// Open the controlling terminal and read one line from it with echo off.
/// No controlling terminal is [`PassphraseError::NoSource`].
#[cfg(unix)]
fn read_from_tty(prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
    use std::os::fd::AsRawFd;
    use std::os::unix::fs::OpenOptionsExt;
    let tty = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(libc::O_NOCTTY)
        .open("/dev/tty")
        .map_err(|_| PassphraseError::NoSource)?;
    read_line_echo_off(tty.as_raw_fd(), Some(prompt))
}

#[cfg(not(unix))]
fn read_from_tty(_prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
    Err(PassphraseError::NoSource)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    fn pipe_with(contents: &[u8]) -> i32 {
        let mut fds = [0i32; 2];
        assert_eq!(unsafe { libc::pipe(fds.as_mut_ptr()) }, 0);
        write_all_fd(fds[1], contents).unwrap();
        unsafe { libc::close(fds[1]) };
        fds[0]
    }

    fn err_text(r: Result<Zeroizing<String>, PassphraseError>) -> String {
        match r {
            Ok(_) => panic!("expected an error"),
            Err(e) => e.to_string(),
        }
    }

    #[test]
    fn fd_reads_one_line_per_passphrase() {
        let fd = pipe_with(b"first one\nsecond\r\nthird-no-newline");
        let mut input = PassphraseInput::new(Some(fd)).unwrap();
        assert_eq!(input.read("unused").unwrap().as_str(), "first one");
        assert_eq!(
            input.read_new("unused", "unused").unwrap().as_str(),
            "second"
        );
        assert_eq!(input.read("unused").unwrap().as_str(), "third-no-newline");
        assert_eq!(
            err_text(input.read("unused")),
            "--passphrase-fd reached its end before a passphrase line"
        );
        unsafe { libc::close(fd) };
    }

    #[test]
    fn fd_refusals() {
        let fd = pipe_with(b"\n");
        let mut input = PassphraseInput::new(Some(fd)).unwrap();
        assert_eq!(err_text(input.read("x")), "the passphrase is empty");
        unsafe { libc::close(fd) };

        let fd = pipe_with(b"\xff\xfe\n");
        let mut input = PassphraseInput::new(Some(fd)).unwrap();
        assert_eq!(err_text(input.read("x")), "the passphrase is not UTF-8");
        unsafe { libc::close(fd) };

        let long = vec![b'a'; MAX_PASSPHRASE_LEN + 1];
        let fd = pipe_with(&long);
        let mut input = PassphraseInput::new(Some(fd)).unwrap();
        assert_eq!(
            err_text(input.read("x")),
            "the passphrase is longer than 1024 bytes"
        );
        unsafe { libc::close(fd) };

        // A closed descriptor is refused before anything is read.
        let fd = pipe_with(b"x\n");
        unsafe { libc::close(fd) };
        match PassphraseInput::new(Some(fd)) {
            Err(e) => assert_eq!(
                e.to_string(),
                format!("--passphrase-fd {fd} is not an open file descriptor")
            ),
            Ok(_) => panic!("a closed fd must be refused"),
        }
        match PassphraseInput::new(Some(-1)) {
            Err(e) => assert_eq!(
                e.to_string(),
                "--passphrase-fd -1 is not an open file descriptor"
            ),
            Ok(_) => panic!("a negative fd must be refused"),
        }
    }

    #[test]
    fn the_line_buffer_is_allocated_once_and_never_grows() {
        // A buffer that grew would free its outgrown copies of the
        // passphrase unwiped. A line at the cap keeps the first allocation.
        let fd = pipe_with(&[b'a'; MAX_PASSPHRASE_LEN]);
        let (line, saw_any) = read_raw_line(fd).unwrap();
        assert!(saw_any);
        assert_eq!(line.len(), MAX_PASSPHRASE_LEN);
        assert_eq!(
            line.capacity(),
            Vec::<u8>::with_capacity(MAX_PASSPHRASE_LEN).capacity()
        );
        unsafe { libc::close(fd) };
    }

    #[test]
    fn a_non_terminal_fd_is_no_source() {
        let fd = pipe_with(b"secret\n");
        assert!(matches!(
            read_line_echo_off(fd, Some("pw: ")),
            Err(PassphraseError::NoSource)
        ));
        assert!(matches!(
            EchoGuard::engage(fd),
            Err(PassphraseError::NoSource)
        ));
        unsafe { libc::close(fd) };
    }

    /// Every test that turns echo off holds this: the guard is one per
    /// process and installs process-wide signal dispositions.
    #[cfg(target_os = "linux")]
    static TERMINAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[cfg(target_os = "linux")]
    fn serialize() -> std::sync::MutexGuard<'static, ()> {
        TERMINAL.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Open a pseudo-terminal pair: (controller, terminal).
    #[cfg(target_os = "linux")]
    fn open_pty() -> (i32, i32) {
        unsafe {
            let controller = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
            assert!(controller >= 0, "posix_openpt");
            assert_eq!(libc::grantpt(controller), 0);
            assert_eq!(libc::unlockpt(controller), 0);
            let mut name = [0 as libc::c_char; 128];
            assert_eq!(
                libc::ptsname_r(controller, name.as_mut_ptr(), name.len()),
                0
            );
            let terminal = libc::open(name.as_ptr(), libc::O_RDWR | libc::O_NOCTTY);
            assert!(terminal >= 0, "open the terminal side");
            (controller, terminal)
        }
    }

    /// Read whatever the controller side has, waiting up to `ms` for the
    /// first byte and briefly for more.
    #[cfg(target_os = "linux")]
    fn drain(controller: i32, ms: i32) -> Vec<u8> {
        let mut out = Vec::new();
        let mut wait = ms;
        loop {
            let mut pfd = libc::pollfd {
                fd: controller,
                events: libc::POLLIN,
                revents: 0,
            };
            if unsafe { libc::poll(&mut pfd, 1, wait) } <= 0 {
                return out;
            }
            let mut buf = [0u8; 256];
            let n = unsafe { libc::read(controller, buf.as_mut_ptr().cast(), buf.len()) };
            if n <= 0 {
                return out;
            }
            out.extend_from_slice(&buf[..n as usize]);
            wait = 100;
        }
    }

    /// Read from the controller until exactly `expected` has been shown (or
    /// five seconds pass), and check it.
    #[cfg(target_os = "linux")]
    fn expect_shown(controller: i32, expected: &[u8]) {
        let mut out = Vec::new();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while out.len() < expected.len() && std::time::Instant::now() < deadline {
            out.extend_from_slice(&drain(controller, 200));
        }
        assert_eq!(
            String::from_utf8_lossy(&out),
            String::from_utf8_lossy(expected)
        );
    }

    #[cfg(target_os = "linux")]
    fn lflag(fd: i32) -> libc::tcflag_t {
        let mut t: libc::termios = unsafe { std::mem::zeroed() };
        assert_eq!(unsafe { libc::tcgetattr(fd, &mut t) }, 0);
        t.c_lflag
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn terminal_echo_is_off_while_typing_and_restored_after() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let before = lflag(terminal);
        assert_ne!(before & libc::ECHO, 0, "a fresh pty echoes");

        let (tx, rx) = std::sync::mpsc::channel();
        let reader = std::thread::spawn(move || {
            let got = read_line_echo_off(terminal, Some("trustee key passphrase: "))
                .map(|s| s.to_string())
                .map_err(|e| e.to_string());
            tx.send(got).unwrap();
        });
        // The prompt is written only after echo is off, so once it is seen
        // the typed line cannot echo.
        let shown = drain(controller, 5000);
        assert_eq!(shown, b"trustee key passphrase: ");
        write_all_fd(controller, b"s3cret-typed\n").unwrap();
        let got = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("the reader returns");
        reader.join().unwrap();
        assert_eq!(got.unwrap(), "s3cret-typed");
        let echoed = drain(controller, 200);
        assert!(
            !echoed.windows(6).any(|w| w == b"s3cret"),
            "the passphrase was echoed: {echoed:?}"
        );
        assert_eq!(lflag(terminal), before, "terminal attributes restored");
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }

    /// An over-long line typed at a terminal is refused, and the rest of
    /// it does not stay queued on the terminal for the shell to read, show,
    /// run and save in its history.
    #[cfg(target_os = "linux")]
    #[test]
    fn an_over_long_terminal_line_leaves_nothing_queued() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let before = lflag(terminal);
        let (tx, rx) = std::sync::mpsc::channel();
        let reader = std::thread::spawn(move || {
            let got = read_line_echo_off(terminal, Some("pw: "))
                .map(|s| s.to_string())
                .map_err(|e| e.to_string());
            tx.send(got).unwrap();
        });
        expect_shown(controller, b"pw: ");
        let mut typed = vec![b'a'; MAX_PASSPHRASE_LEN + 76];
        typed.push(b'\n');
        write_all_fd(controller, &typed).unwrap();
        let got = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("the reader returns");
        reader.join().unwrap();
        let err = got.expect_err("an over-long line is refused");
        assert_eq!(err, "the passphrase is longer than 1024 bytes");
        // Nothing of the line is left on the terminal's input queue (the
        // terminal side, where the shell would read it).
        let mut pending: libc::c_int = -1;
        assert_eq!(
            unsafe { libc::ioctl(terminal, libc::FIONREAD, &mut pending) },
            0
        );
        assert_eq!(pending, 0, "bytes of the passphrase left queued");
        assert_eq!(lflag(terminal), before, "terminal attributes restored");
        let shown = drain(controller, 200);
        assert!(!shown.contains(&b'a'), "the passphrase was echoed");
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }

    /// `--passphrase-fd` naming a terminal: echo off, the prompt shown on
    /// it, and a new passphrase typed twice.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_terminal_fd_reads_with_echo_off_and_confirms_a_new_passphrase() {
        let _serial = serialize();
        for (first, second, expected) in [
            (
                &b"same-one\n"[..],
                &b"same-one\n"[..],
                Ok("same-one".to_string()),
            ),
            (
                &b"typed-one\n"[..],
                &b"typed-two\n"[..],
                Err("the two passphrases differ".to_string()),
            ),
        ] {
            let (controller, terminal) = open_pty();
            let before = lflag(terminal);
            let (tx, rx) = std::sync::mpsc::channel();
            let reader = std::thread::spawn(move || {
                let mut input = PassphraseInput::new(Some(terminal)).unwrap();
                let got = input
                    .read_new("new passphrase: ", "repeat it: ")
                    .map(|s| s.to_string())
                    .map_err(|e| e.to_string());
                tx.send(got).unwrap();
            });
            expect_shown(controller, b"new passphrase: ");
            write_all_fd(controller, first).unwrap();
            // The newline echoes (ECHONL, as \r\n on a pty), then the second
            // prompt, written only once echo is off again.
            expect_shown(controller, b"\r\nrepeat it: ");
            write_all_fd(controller, second).unwrap();
            let got = rx
                .recv_timeout(std::time::Duration::from_secs(5))
                .expect("the reader returns");
            reader.join().unwrap();
            assert_eq!(got, expected);
            expect_shown(controller, b"\r\n");
            assert_eq!(lflag(terminal), before, "terminal attributes restored");
            unsafe {
                libc::close(terminal);
                libc::close(controller);
            }
        }
    }

    /// A terminal fd opened read-only cannot show a prompt: it is still read
    /// with echo off, once.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_read_only_terminal_fd_reads_once_with_echo_off_and_no_prompt() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let mut name = [0 as libc::c_char; 128];
        assert_eq!(
            unsafe { libc::ptsname_r(controller, name.as_mut_ptr(), name.len()) },
            0
        );
        let read_only = unsafe { libc::open(name.as_ptr(), libc::O_RDONLY | libc::O_NOCTTY) };
        assert!(read_only >= 0);
        let (tx, rx) = std::sync::mpsc::channel();
        let reader = std::thread::spawn(move || {
            let mut input = PassphraseInput::new(Some(read_only)).unwrap();
            let got = input
                .read_new("never shown: ", "never shown: ")
                .map(|s| s.to_string())
                .map_err(|e| e.to_string());
            tx.send(got).unwrap();
        });
        // Nothing to wait for on screen: wait until echo is off instead.
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while lflag(terminal) & libc::ECHO != 0 && std::time::Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(lflag(terminal) & libc::ECHO, 0, "echo went off");
        write_all_fd(controller, b"quiet-one\n").unwrap();
        let got = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("the reader returns");
        reader.join().unwrap();
        assert_eq!(got.unwrap(), "quiet-one");
        assert_eq!(drain(controller, 200), b"\r\n");
        unsafe {
            libc::close(read_only);
            libc::close(terminal);
            libc::close(controller);
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_second_guard_is_refused_while_one_is_held() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let guard = EchoGuard::engage(terminal).unwrap_or_else(|e| panic!("{e}"));
        match EchoGuard::engage(terminal) {
            Err(e) => assert_eq!(
                e.to_string(),
                "another passphrase prompt already holds the terminal"
            ),
            Ok(_) => panic!("a second guard must be refused"),
        }
        drop(guard);
        drop(EchoGuard::engage(terminal).unwrap_or_else(|e| panic!("{e}")));
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }

    /// Fork a child that runs `body` and `_exit`s with what it returns; the
    /// parent waits and returns the raw wait status. The child does only
    /// what the guard does on its own path: no allocation.
    #[cfg(target_os = "linux")]
    fn in_child(body: impl FnOnce() -> i32) -> i32 {
        // SAFETY: the child runs `body` (async-signal-safe calls only) and
        // leaves through _exit or a signal; it never returns to the harness.
        let child = unsafe { libc::fork() };
        assert!(child >= 0, "fork");
        if child == 0 {
            let code = body();
            unsafe { libc::_exit(code) };
        }
        let mut status = 0;
        // SAFETY: reaping the child just forked.
        assert_eq!(unsafe { libc::waitpid(child, &mut status, 0) }, child);
        status
    }

    /// While the guard is held: SIGTSTP is ignored (Ctrl-Z cannot stop the
    /// process with echo off), the fatal signals restore the terminal, and a
    /// signal the process already ignored stays ignored. Drop puts every
    /// disposition back. Checked in a forked child, so the dispositions of
    /// the test process itself are never touched.
    #[cfg(target_os = "linux")]
    #[test]
    fn the_guard_ignores_sigtstp_and_takes_the_fatal_signals_only_while_held() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let status = in_child(|| {
            use echo::{current_action, restore_and_die};
            let handler = restore_and_die as extern "C" fn(libc::c_int) as libc::sighandler_t;
            let tstp = current_action(libc::SIGTSTP).sa_sigaction;
            let int = current_action(libc::SIGINT).sa_sigaction;
            // SAFETY: plain disposition change in the single-threaded child.
            unsafe { libc::signal(libc::SIGHUP, libc::SIG_IGN) };
            let Ok(guard) = EchoGuard::engage(terminal) else {
                return 2;
            };
            if current_action(libc::SIGTSTP).sa_sigaction != libc::SIG_IGN {
                return 3;
            }
            for sig in [libc::SIGINT, libc::SIGQUIT, libc::SIGTERM] {
                if current_action(sig).sa_sigaction != handler {
                    return 4;
                }
            }
            if current_action(libc::SIGHUP).sa_sigaction != libc::SIG_IGN {
                return 5;
            }
            drop(guard);
            if current_action(libc::SIGTSTP).sa_sigaction != tstp
                || current_action(libc::SIGINT).sa_sigaction != int
                || current_action(libc::SIGHUP).sa_sigaction != libc::SIG_IGN
            {
                return 6;
            }
            0
        });
        assert!(
            libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0,
            "child status {status:#x} (2 = no guard, 3 = SIGTSTP not ignored, \
             4 = a fatal signal not taken, 5 = an ignored signal un-ignored, \
             6 = a disposition not restored)"
        );
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }

    /// Ctrl-C (or SIGTERM) at the prompt: Drop never runs, so the handler
    /// must put echo back, and the process must still die of the signal.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_fatal_signal_at_the_prompt_restores_echo_and_still_kills() {
        let _serial = serialize();
        for sig in [libc::SIGINT, libc::SIGTERM] {
            let (controller, terminal) = open_pty();
            assert_ne!(lflag(terminal) & libc::ECHO, 0);
            let status = in_child(|| {
                let guard = EchoGuard::engage(terminal);
                if guard.is_err() {
                    return 2;
                }
                let mut t: libc::termios = unsafe { std::mem::zeroed() };
                if unsafe { libc::tcgetattr(terminal, &mut t) } != 0 || t.c_lflag & libc::ECHO != 0
                {
                    return 4; // echo never went off: the test would prove nothing
                }
                // SAFETY: what the terminal delivers on Ctrl-C.
                unsafe { libc::raise(sig) };
                3 // the handler swallowed the signal
            });
            assert!(
                libc::WIFSIGNALED(status) && libc::WTERMSIG(status) == sig,
                "the child must die of signal {sig} while holding the guard \
                 (status {status:#x}; 2 = no guard, 3 = swallowed, 4 = echo never off)"
            );
            assert_ne!(
                lflag(terminal) & libc::ECHO,
                0,
                "signal {sig} at the prompt left the terminal with echo off"
            );
            unsafe {
                libc::close(terminal);
                libc::close(controller);
            }
        }
    }

    /// SIGTERM after a half-typed passphrase: the handler must discard the
    /// terminal's pending input before it restores echo and dies, or the
    /// typed bytes wait in the queue for the next reader (the shell). Ctrl-C
    /// flushes on its own; a signal sent from outside does not.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_fatal_signal_mid_passphrase_leaves_no_typed_bytes_queued() {
        let _serial = serialize();
        let (controller, terminal) = open_pty();
        let half: &[u8] = b"half-typed";
        let pause = libc::timespec {
            tv_sec: 0,
            tv_nsec: 100_000_000,
        };
        let status = in_child(|| {
            let Ok(_guard) = EchoGuard::engage(terminal) else {
                return 2;
            };
            // The trustee types, but has not pressed Enter.
            // SAFETY: write and nanosleep are async-signal-safe.
            unsafe {
                if libc::write(controller, half.as_ptr().cast(), half.len()) != half.len() as isize
                {
                    return 4;
                }
                libc::nanosleep(&pause, std::ptr::null_mut());
                libc::raise(libc::SIGTERM);
            }
            3
        });
        assert!(
            libc::WIFSIGNALED(status) && libc::WTERMSIG(status) == libc::SIGTERM,
            "child status {status:#x} (2 = no guard, 3 = swallowed, 4 = write failed)"
        );
        // The next reader of the terminal: end the line and see what it gets.
        let mut got = [0u8; 64];
        // SAFETY: plain reads and writes on the pair this test opened.
        let n = unsafe {
            libc::write(controller, b"\n".as_ptr().cast(), 1);
            libc::nanosleep(&pause, std::ptr::null_mut());
            let flags = libc::fcntl(terminal, libc::F_GETFL);
            libc::fcntl(terminal, libc::F_SETFL, flags | libc::O_NONBLOCK);
            libc::read(terminal, got.as_mut_ptr().cast(), got.len())
        };
        assert_eq!(
            &got[..n.max(0) as usize],
            b"\n",
            "the half-typed passphrase survived the signal in the input queue"
        );
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }

    #[test]
    fn the_reader_has_no_argv_or_environment_source() {
        // This module is the only passphrase source for trustee and custody
        // keys. Its code (tests excluded) must read neither argv nor the
        // environment. The needles are built at run time so this test does
        // not match itself.
        let src = include_str!("passphrase.rs");
        let code = &src[..src.find("#[cfg(all(test, unix))]").unwrap()];
        for needle in [
            ["env", "::var"].concat(),
            ["env", "::args"].concat(),
            ["var", "_os("].concat(),
            ["args", "_os("].concat(),
            ["get", "env("].concat(),
        ] {
            assert!(!code.contains(&needle), "passphrase.rs reads {needle}");
        }
    }
}
