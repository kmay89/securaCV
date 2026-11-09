//! Reading a passphrase without it touching argv or the environment.
//!
//! Two sources exist, and only two:
//!
//! - the controlling terminal, `/dev/tty`, with echo turned off (libc
//!   `tcgetattr` / `tcsetattr`) for the length of one line and restored
//!   afterwards, even on error;
//! - a file descriptor the caller names (`--passphrase-fd N`), read one line
//!   per passphrase, so a script can pipe it in without it ever being a
//!   command-line argument.
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
    /// terminal after `prompt` with echo off. An empty passphrase is refused.
    pub(crate) fn read(&mut self, prompt: &str) -> Result<Zeroizing<String>, PassphraseError> {
        match self.fd {
            Some(fd) => read_passphrase_line(fd),
            None if self.terminal => read_from_tty(prompt),
            None => Err(PassphraseError::NoSource),
        }
    }

    /// Read a NEW passphrase. From the fd it is one line. On the terminal it
    /// is typed twice (`prompt`, then `repeat_prompt`) and must match.
    pub(crate) fn read_new(
        &mut self,
        prompt: &str,
        repeat_prompt: &str,
    ) -> Result<Zeroizing<String>, PassphraseError> {
        let first = self.read(prompt)?;
        if self.fd.is_some() {
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
    let mut line = Zeroizing::new(Vec::with_capacity(64));
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
            return Err(failed(format!(
                "the passphrase is longer than {MAX_PASSPHRASE_LEN} bytes"
            )));
        }
        line.push(byte);
    }
}

/// One passphrase line from `--passphrase-fd`.
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

/// Restores a terminal's saved attributes when dropped, so an error or a
/// panic between "echo off" and "echo on" never leaves the terminal mute.
#[cfg(unix)]
struct EchoGuard {
    fd: i32,
    saved: libc::termios,
}

#[cfg(unix)]
impl Drop for EchoGuard {
    fn drop(&mut self) {
        // SAFETY: restores attributes read from this same fd by tcgetattr.
        unsafe {
            libc::tcsetattr(self.fd, libc::TCSANOW, &self.saved);
        }
    }
}

/// Write `prompt` to the terminal `fd` and read one line with echo off.
/// Echo is turned off BEFORE the prompt is written, so anything typed after
/// the prompt appears is never shown; the newline still echoes (ECHONL) so
/// the next output starts on a fresh line. Attributes are restored on every
/// path. `fd` must be a terminal.
#[cfg(unix)]
pub(crate) fn read_line_echo_off(
    fd: i32,
    prompt: &str,
) -> Result<Zeroizing<String>, PassphraseError> {
    // SAFETY: isatty only queries the fd.
    if unsafe { libc::isatty(fd) } != 1 {
        return Err(PassphraseError::NoSource);
    }
    // SAFETY: termios is plain data; tcgetattr fills it or fails.
    let mut saved: libc::termios = unsafe { std::mem::zeroed() };
    if unsafe { libc::tcgetattr(fd, &mut saved) } != 0 {
        return Err(PassphraseError::NoSource);
    }
    let mut quiet = saved;
    quiet.c_lflag &= !libc::ECHO;
    quiet.c_lflag |= libc::ECHONL;
    // SAFETY: sets attributes derived from the ones just read.
    if unsafe { libc::tcsetattr(fd, libc::TCSAFLUSH, &quiet) } != 0 {
        return Err(failed(format!(
            "could not turn terminal echo off: {}",
            std::io::Error::last_os_error()
        )));
    }
    let guard = EchoGuard { fd, saved };
    write_all_fd(fd, prompt.as_bytes())?;
    let (line, _) = read_raw_line(fd)?;
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
    read_line_echo_off(tty.as_raw_fd(), prompt)
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
    fn a_non_terminal_fd_is_no_source() {
        let fd = pipe_with(b"secret\n");
        assert!(matches!(
            read_line_echo_off(fd, "pw: "),
            Err(PassphraseError::NoSource)
        ));
        unsafe { libc::close(fd) };
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

    #[cfg(target_os = "linux")]
    #[test]
    fn terminal_echo_is_off_while_typing_and_restored_after() {
        let (controller, terminal) = open_pty();
        let mut before: libc::termios = unsafe { std::mem::zeroed() };
        assert_eq!(unsafe { libc::tcgetattr(terminal, &mut before) }, 0);
        assert_ne!(before.c_lflag & libc::ECHO, 0, "a fresh pty echoes");

        let (tx, rx) = std::sync::mpsc::channel();
        let reader = std::thread::spawn(move || {
            let got = read_line_echo_off(terminal, "trustee key passphrase: ")
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

        let mut after: libc::termios = unsafe { std::mem::zeroed() };
        assert_eq!(unsafe { libc::tcgetattr(terminal, &mut after) }, 0);
        assert_eq!(
            after.c_lflag, before.c_lflag,
            "terminal attributes restored"
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
