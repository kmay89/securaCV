use std::{env, fs, path::Path};

fn main() {
    // Never-rot catalog, the Flasher's contract (desktop/src-tauri/build.rs):
    // embed the ONE canonical flasher catalog (canary-local/devices/flash.json)
    // fresh on every build by copying it into OUT_DIR for src/flash.rs to
    // include_str!. The native flash path's chip guard and manifest allow-list
    // read this copy, never anything the webview hands over. No committed copy
    // to drift; building outside the repo fails loudly here.
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let out = env::var("OUT_DIR").expect("OUT_DIR");
    let src = Path::new(&manifest_dir).join("../../canary-local/devices/flash.json");
    let data = fs::read(&src).unwrap_or_else(|e| {
        panic!(
            "cannot read the canonical flasher catalog at {} ({e}). \
             The Lab must be built inside the securaCV repo so it can \
             embed the current catalog.",
            src.display()
        )
    });
    fs::write(Path::new(&out).join("flash.json"), data).expect("write catalog to OUT_DIR");
    // Re-run (and re-embed) whenever the canonical catalog changes.
    println!("cargo:rerun-if-changed={}", src.display());

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
