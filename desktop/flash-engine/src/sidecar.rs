//! The bundled `espflash` sidecar: its runtime name, the one argv the engine
//! builds that `rescue` doesn't, and the spawn-failure wording every app
//! shares.

/// The one sidecar the flash path ships. This is the RUNTIME name: the bundler
/// flattens the `externalBin` `binaries/espflash-<triple>` to plain `espflash`
/// next to the app binary (Contents/MacOS/espflash), and Tauri resolves the
/// sidecar by that basename. (Using "binaries/espflash" here makes Tauri look
/// for MacOS/binaries/espflash, which doesn't exist → "No such file or
/// directory".) No capability scope is involved: shell scopes gate only the
/// webview's own plugin:shell IPC (which neither app grants — see each app's
/// capabilities/default.json); Rust-side `shell().sidecar()` resolves by
/// externalBin alone.
pub const ESPFLASH: &str = "espflash";

/// `board-info --port <port>` — ask the connected board which ESP32 it is.
pub fn board_info_args(port: String) -> Vec<String> {
    vec!["board-info".into(), "--port".into(), port]
}

/// Whether the file at `path` can plausibly be exec'd as a sidecar: a regular
/// file (symlinks followed), not empty, and — on Unix — carrying an execute
/// bit. An app asks this before it ADVERTISES a flash path, because a
/// missing or stub sidecar only fails at spawn ("Exec format error", "No such
/// file or directory"), and a frontend that reads that failure as a board
/// problem coaches download mode for a board that was never the issue. A
/// dev build's empty compile-only stub, a repackaged binary and a deleted
/// file all answer false here. It cannot prove the binary runs on this CPU —
/// `spawn_error` names that one at spawn time.
pub fn looks_runnable(path: &std::path::Path) -> bool {
    let Ok(meta) = std::fs::metadata(path) else {
        return false;
    };
    if !meta.is_file() || meta.len() == 0 {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        meta.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        true
    }
}

/// A sidecar that won't exec at all reports in the OS's vocabulary ("Bad CPU
/// type in executable (os error 86)"), which tells the operator neither what
/// is wrong nor what to do. Translate that one class — every sidecar spawn in
/// both apps goes through here, so espflash gets the same treatment as
/// rpiboot. The raw error is kept so a bug report still carries the real
/// cause.
pub fn spawn_error(name: &str, raw: &str) -> String {
    match hub_core::hub_sidecar::arch_mismatch_hint(raw) {
        Some(hint) => format!("could not start {name}: {hint} ({raw})"),
        None => format!("could not start {name}: {raw}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn board_info_is_the_shape_detect_chip_always_sent() {
        assert_eq!(
            board_info_args("/dev/ttyACM0".into()),
            vec!["board-info", "--port", "/dev/ttyACM0"]
        );
    }

    #[test]
    fn only_a_nonempty_executable_file_looks_runnable() {
        use std::io::Write;
        let dir = tempfile::tempdir().unwrap();
        // Missing: the repackaged app, the deleted file.
        assert!(!looks_runnable(&dir.path().join(ESPFLASH)));
        // A directory where the binary should be.
        assert!(!looks_runnable(dir.path()));
        // The empty compile-only stub a dev build or desktop-lab-check.yml
        // puts there: exec'ing it fails with "Exec format error".
        let stub = dir.path().join("stub");
        std::fs::File::create(&stub).unwrap();
        set_exec(&stub);
        assert!(
            !looks_runnable(&stub),
            "an empty stub must not advertise flashing"
        );
        // A real file with content and an execute bit.
        let real = dir.path().join("real");
        let mut f = std::fs::File::create(&real).unwrap();
        f.write_all(b"#!/bin/sh\nexit 0\n").unwrap();
        drop(f);
        set_exec(&real);
        assert!(looks_runnable(&real));
        #[cfg(unix)]
        {
            // The same bytes without the execute bit won't spawn either.
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&real, std::fs::Permissions::from_mode(0o644)).unwrap();
            assert!(!looks_runnable(&real));
        }
    }

    fn set_exec(path: &std::path::Path) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        #[cfg(not(unix))]
        let _ = path;
    }

    #[test]
    fn an_architecture_mismatch_is_named_and_the_raw_error_kept() {
        let raw = "Bad CPU type in executable (os error 86)";
        let msg = spawn_error(ESPFLASH, raw);
        assert!(msg.starts_with("could not start espflash: "));
        assert!(msg.ends_with(&format!("({raw})")));
        assert_ne!(msg, format!("could not start espflash: {raw}"));
    }

    #[test]
    fn other_spawn_failures_keep_their_own_words() {
        let raw = "No such file or directory (os error 2)";
        assert_eq!(
            spawn_error(ESPFLASH, raw),
            format!("could not start espflash: {raw}")
        );
    }
}
