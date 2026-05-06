//! Meta-test: ensure `firmware/scripts/regression_check.sh` itself stays green.
//!
//! That script is a static guard against historical regressions
//! (mbedTLS `_ret` calls, hardcoded AP password, browser storage in dashboards,
//! TLS bypass, etc.). It runs in `firmware.yml`, but only when `firmware/**`
//! changes. A bad refactor of the script — or a violation introduced by code
//! that doesn't trigger the path filter — should still fail `cargo test`.
//!
//! Skipped on non-Unix platforms and when `bash` is unavailable.

#[cfg(unix)]
#[test]
fn firmware_regression_check_script_passes() {
    use std::path::PathBuf;
    use std::process::Command;

    let script = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("firmware")
        .join("scripts")
        .join("regression_check.sh");

    if !script.is_file() {
        eprintln!(
            "skipping: regression_check.sh not found at {}",
            script.display()
        );
        return;
    }

    let output = match Command::new("bash").arg(&script).output() {
        Ok(out) => out,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            eprintln!("skipping: bash not found in PATH");
            return;
        }
        Err(e) => panic!("failed to spawn bash for regression_check.sh: {e}"),
    };

    if !output.status.success() {
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        panic!(
            "firmware/scripts/regression_check.sh exited with {:?}\n\
             --- stdout ---\n{}\n--- stderr ---\n{}",
            output.status.code(),
            stdout,
            stderr
        );
    }
}
