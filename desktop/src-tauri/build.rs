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

    tauri_build::build()
}
