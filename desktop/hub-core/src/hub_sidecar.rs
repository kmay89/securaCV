//! hub_sidecar — turn the OS's refusal to *start* a bundled helper into a
//! sentence an operator can act on.
//!
//! The flasher does its real work through bundled sidecars (`espflash`,
//! `rpiboot`). When one of them won't even exec, the operating system reports
//! it in its own vocabulary and the app used to pass that straight through:
//!
//! ```text
//! could not start rpiboot: Bad CPU type in executable (os error 86)
//! ```
//!
//! That is macOS saying "this binary has no slice for this Mac's processor" —
//! i.e. the *download* is wrong for the machine, which is a packaging bug, not
//! anything the operator did. Nothing in that sentence says so, and nothing in
//! it says what to do instead. Linux has the same failure under a different
//! name ("Exec format error", os error 8).
//!
//! This module recognizes that one class and supplies the missing sentence.
//! Pure string logic with no dependencies, so it is host-tested on every PR
//! (the app itself only builds on release tags — see the crate docs).

/// What to tell an operator when a sidecar can't run on this machine's CPU.
///
/// Deliberately does not name a specific architecture: the same message is
/// correct for an Intel Mac given an arm64-only build and for the mirror-image
/// mistake, and it stays true as the supported set changes. It points at the
/// fix that is actually in the operator's hands (update, or use a card reader)
/// rather than explaining Mach-O slices.
pub const ARCH_MISMATCH_HINT: &str =
    "this download doesn't include a build of that helper for your computer's processor. \
     Update to the newest SecuraCV Flasher release — the macOS build is universal, one \
     download for both Apple Silicon and Intel. If you're already on the newest release, \
     please report it; meanwhile you can flash the card in a USB card reader instead.";

/// The hint to show when a sidecar spawn failed because the binary has no code
/// for this machine's CPU; `None` for every other spawn failure.
///
/// Matches the OS's words *and* its numbered errno, because Rust's
/// `io::Error` renders the message on some platforms and the bare code on
/// others. It stays narrow on purpose: a missing file or a permission problem
/// is a different fix, and must keep its own error.
pub fn arch_mismatch_hint(err: &str) -> Option<&'static str> {
    let e = err.to_ascii_lowercase();
    let named = e.contains("bad cpu type")
        || e.contains("exec format error")
        || e.contains("wrong architecture")
        || e.contains("cputype");
    // macOS EBADARCH is 86, Linux ENOEXEC is 8. Match the bracketed form
    // `io::Error` actually renders, so "os error 860" can't trip it.
    let numbered = e.contains("(os error 86)") || e.contains("(os error 8)");
    (named || numbered).then_some(ARCH_MISMATCH_HINT)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flags_the_macos_intel_failure_verbatim() {
        // The exact string a 2017 Intel MacBook Pro showed when the shipped
        // "universal" rpiboot sidecar carried only an arm64 slice.
        assert!(arch_mismatch_hint("Bad CPU type in executable (os error 86)").is_some());
    }

    #[test]
    fn flags_the_linux_equivalent() {
        assert!(arch_mismatch_hint("Exec format error (os error 8)").is_some());
    }

    #[test]
    fn is_case_insensitive() {
        assert!(arch_mismatch_hint("BAD CPU TYPE IN EXECUTABLE").is_some());
    }

    #[test]
    fn leaves_other_spawn_failures_their_own_error() {
        // Each of these has a different fix, so none may be dressed up as an
        // architecture problem.
        for err in [
            "No such file or directory (os error 2)",
            "Permission denied (os error 13)",
            "Resource temporarily unavailable (os error 11)",
            "program not found",
            "Operation not permitted (os error 1)",
        ] {
            assert!(arch_mismatch_hint(err).is_none(), "false positive: {err}");
        }
    }

    #[test]
    fn a_digit_8_elsewhere_is_not_an_exec_error() {
        for err in [
            "No such file or directory (os error 2): /opt/app-v8/bin/rpiboot",
            "failed to open /dev/disk8 (os error 2)",
        ] {
            assert!(arch_mismatch_hint(err).is_none(), "false positive: {err}");
        }
    }

    #[test]
    fn hint_says_what_to_do_without_naming_an_architecture() {
        let hint = arch_mismatch_hint("Bad CPU type in executable (os error 86)").unwrap();
        assert!(hint.contains("card reader"), "must offer the way around it");
        assert!(hint.contains("Update"), "must offer the actual fix");
    }
}
