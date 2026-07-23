use std::{env, fs, path::Path};

fn main() {
    // Never-rot catalog: embed the ONE canonical flasher catalog
    // (canary-local/devices/flash.json — the same file the website ships and
    // that tools/gen_flash.py keeps honest) fresh on every build, by copying
    // it into OUT_DIR for lib.rs to include_str!. There is no committed copy to
    // drift: if the firmware adds a board, the next desktop build embeds it
    // automatically. Building the crate outside the repo fails loudly here
    // rather than shipping a stale product list.
    let manifest = env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let out = env::var("OUT_DIR").expect("OUT_DIR");
    let src = Path::new(&manifest).join("../../canary-local/devices/flash.json");
    let dst = Path::new(&out).join("flash.json");
    let data = fs::read(&src).unwrap_or_else(|e| {
        panic!(
            "cannot read the canonical flasher catalog at {} ({e}). \
             The desktop app must be built inside the securaCV repo so it can \
             embed the current catalog.",
            src.display()
        )
    });
    fs::write(&dst, data).expect("write catalog to OUT_DIR");
    // Re-run (and re-embed) whenever the canonical catalog changes.
    println!("cargo:rerun-if-changed={}", src.display());

    // Same contract for the hub-image catalog (the Raspberry Pi writer's
    // single source of truth, kept honest by gen_hub_image.py's drift gate).
    let hub_src = Path::new(&manifest).join("../../canary-local/devices/hub_image.json");
    let hub_dst = Path::new(&out).join("hub_image.json");
    let hub_data = fs::read(&hub_src).unwrap_or_else(|e| {
        panic!(
            "cannot read the canonical hub-image catalog at {} ({e}). \
             The desktop app must be built inside the securaCV repo so it can \
             embed the current catalog.",
            hub_src.display()
        )
    });
    fs::write(&hub_dst, hub_data).expect("write hub catalog to OUT_DIR");
    println!("cargo:rerun-if-changed={}", hub_src.display());

    // Same never-rot contract for the Hatchery naming spec: the ONE shared
    // canary-local/devices/hatch.json the website also ships, so the flasher's
    // birth certificate names a Canary exactly the way the web Lab does, and a
    // change to that file re-embeds here automatically.
    let hatch_src = Path::new(&manifest).join("../../canary-local/devices/hatch.json");
    let hatch_dst = Path::new(&out).join("hatch.json");
    let hatch_data = fs::read(&hatch_src).unwrap_or_else(|e| {
        panic!(
            "cannot read the canonical Hatchery spec at {} ({e}). \
             The desktop app must be built inside the securaCV repo so it can \
             embed the current naming source of truth.",
            hatch_src.display()
        )
    });
    fs::write(&hatch_dst, hatch_data).expect("write hatch spec to OUT_DIR");
    println!("cargo:rerun-if-changed={}", hatch_src.display());

    // Build stamp: a real build number (short git rev) and a build timestamp,
    // baked in at compile time so the About panel can show exactly which build
    // is running and when it was cut — never a guess, never stale. Both fall
    // back gracefully when built outside a git checkout.
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
