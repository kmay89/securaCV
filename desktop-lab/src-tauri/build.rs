fn main() {
    // Build stamp: a real build number (short git rev) and a build timestamp,
    // baked in at compile time so the Settings panel can show exactly which
    // build is running and when it was cut — never a guess, never stale. Same
    // contract as the Flasher's build.rs, including the graceful fallback when
    // the crate is built outside a git checkout.
    let manifest = std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let rev = std::process::Command::new("git")
        .args(["rev-parse", "--short=9", "HEAD"])
        .current_dir(&manifest)
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "source".to_string());
    println!("cargo:rustc-env=SECURACV_BUILD_REV={rev}");

    let epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    println!("cargo:rustc-env=SECURACV_BUILD_EPOCH={epoch}");

    tauri_build::build()
}
