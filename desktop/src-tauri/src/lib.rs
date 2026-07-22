//! SecuraCV Flasher — native backend.
//!
//! This is the whole reason the Lab becomes a native app: flashing here does
//! **not** touch Web Serial or Chromium. Serial ports come straight from the
//! OS (`serialport`), and the flash itself is driven by the bundled
//! `espflash` engine (shipped as a Tauri sidecar), the Rust-native sibling of
//! esptool maintained by the esp-rs team. No browser, no PlatformIO, no
//! terminal — and, like the web flasher, you cannot brick the board: the
//! ESP32's first-stage bootloader is mask ROM.
//!
//! The front-end (plain HTML/JS in `../src`) drives four commands:
//!   * `load_catalog`   — the bundled product list + chip guard (offline-safe)
//!   * `list_ports`     — USB serial ports the OS can see right now
//!   * `detect_chip`    — ask the board which ESP32 it is (the "wrong image" guard)
//!   * `flash`          — resolve the signed release, download it, write it, stream progress
//!
//! Everything the user watches scroll by during a flash is `espflash`'s own
//! output, relayed verbatim over the `flash:log` event.

use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter};
use tauri_plugin_shell::process::CommandEvent;
use tauri_plugin_shell::ShellExt;
use tauri_plugin_updater::UpdaterExt;

// The Raspberry Pi Home Assistant hub path (design: docs/design/
// raspberry_pi_hub_flashing.md). Writing a whole-OS image to a raw disk is the
// one thing this app can do that ISN'T can't-brick-safe like an ESP32 flash — a
// wrong-disk write destroys data. So the *decision* of what is a legal write
// target lands first, pure and host-tested, before any byte-writing command
// exists — mirroring how the firmware lands its boot policy as a tested pure
// layer ahead of the risky wiring. The enumerator, the confirm UI, and the
// guarded write build on top of this in follow-up changes; each must run a
// candidate through `hub_disk::classify` and refuse anything not `Eligible`.
pub mod hub_disk;

// The flasher catalog is baked in at compile time so the app can list every
// Canary and enforce the chip guard with zero network. build.rs copies the ONE
// canonical `canary-local/devices/flash.json` into OUT_DIR on every build, so
// this embed can never drift from the website/firmware source of truth.
const EMBEDDED_CATALOG: &str = include_str!(concat!(env!("OUT_DIR"), "/flash.json"));

// The one sidecar we ship. This is the RUNTIME name: the bundler flattens the
// `externalBin` "binaries/espflash-<triple>" to plain `espflash` next to the
// app binary (Contents/MacOS/espflash), and Tauri resolves the sidecar by that
// basename. It must match the scope `name` in capabilities/default.json.
// (Using "binaries/espflash" here makes Tauri look for MacOS/binaries/espflash,
// which doesn't exist → "No such file or directory".)
const ESPFLASH: &str = "espflash";

/// A USB serial port as the OS sees it — enough for the UI to show a friendly
/// picker without pretending to know more than it does.
#[derive(Serialize)]
pub struct PortDto {
    /// OS port path, e.g. `/dev/tty.usbmodem1101` or `/dev/ttyACM0`.
    name: String,
    /// "usb" | "bluetooth" | "pci" | "unknown" — USB is what a Canary is.
    kind: String,
    vid: Option<u16>,
    pid: Option<u16>,
    product: Option<String>,
    manufacturer: Option<String>,
}

/// The embedded flasher catalog, handed to the UI verbatim. The front-end
/// already knows this schema (it is the website's), so we don't re-type it
/// here — we just guarantee it parses.
#[tauri::command]
fn load_catalog() -> Result<Value, String> {
    serde_json::from_str(EMBEDDED_CATALOG)
        .map_err(|e| format!("bundled catalog is corrupt: {e}"))
}

/// Serial ports the OS can see this instant. No Web Serial permission prompt,
/// no Chromium — just the platform enumerating its own devices.
#[tauri::command]
fn list_ports() -> Result<Vec<PortDto>, String> {
    let ports = serialport::available_ports()
        .map_err(|e| format!("could not list serial ports: {e}"))?;
    let mut out = Vec::new();
    for p in ports {
        use serialport::SerialPortType::*;
        let (kind, vid, pid, product, manufacturer) = match &p.port_type {
            UsbPort(info) => (
                "usb",
                Some(info.vid),
                Some(info.pid),
                info.product.clone(),
                info.manufacturer.clone(),
            ),
            BluetoothPort => ("bluetooth", None, None, None, None),
            PciPort => ("pci", None, None, None, None),
            Unknown => ("unknown", None, None, None, None),
        };
        out.push(PortDto {
            name: p.port_name,
            kind: kind.to_string(),
            vid,
            pid,
            product,
            manufacturer,
        });
    }
    Ok(out)
}

/// Normalize whatever `espflash board-info` calls the chip into the catalog's
/// canonical spelling ("ESP32-S3", "ESP32-C3", …). Order matters: the variant
/// chips must be matched before bare "esp32".
fn canonical_chip(raw: &str) -> Option<&'static str> {
    let s = raw.to_lowercase().replace(['-', ' ', '_'], "");
    for (needle, canon) in [
        ("esp32s3", "ESP32-S3"),
        ("esp32c3", "ESP32-C3"),
        ("esp32c6", "ESP32-C6"),
        ("esp32c2", "ESP32-C2"),
        ("esp32s2", "ESP32-S2"),
        ("esp32h2", "ESP32-H2"),
        ("esp32", "ESP32"),
    ] {
        if s.contains(needle) {
            return Some(canon);
        }
    }
    None
}

/// Run the sidecar to completion, collecting stdout+stderr. Used for the short
/// `board-info` probe where we want the whole answer, not a live stream.
async fn run_sidecar_capture(app: &AppHandle, args: Vec<String>) -> Result<(i32, String), String> {
    let cmd = app
        .shell()
        .sidecar(ESPFLASH)
        .map_err(|e| format!("bundled espflash missing: {e}"))?
        .args(args);
    let (mut rx, _child) = cmd
        .spawn()
        .map_err(|e| format!("could not start espflash: {e}"))?;

    let mut buf = String::new();
    let mut code = -1;
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                buf.push_str(&String::from_utf8_lossy(&bytes));
            }
            CommandEvent::Terminated(payload) => {
                code = payload.code.unwrap_or(-1);
            }
            _ => {}
        }
    }
    Ok((code, buf))
}

/// Ask the connected board which ESP32 it is. This is the "you can't pick the
/// wrong image" guard: the UI only offers products whose `chip` matches.
#[tauri::command]
async fn detect_chip(app: AppHandle, port: String) -> Result<String, String> {
    let (code, out) = run_sidecar_capture(
        &app,
        vec!["board-info".into(), "--port".into(), port],
    )
    .await?;
    // Check the exit code before parsing: a *failed* board-info can still print
    // a chip name in its error text, which would otherwise read as a false
    // positive detection.
    if code != 0 {
        return Err(format!(
            "couldn't read the chip (espflash exit {code}). Put the board in download mode (hold BOOT, tap RESET, release BOOT) and try again.\n\nespflash said:\n{}",
            out.trim()
        ));
    }
    match canonical_chip(&out) {
        Some(chip) => Ok(chip.to_string()),
        None => Err(format!(
            "couldn't recognize the chip from espflash's output:\n{}",
            out.trim()
        )),
    }
}

/// Fetch the live signed release manifest so the UI can show what version is
/// currently published for each product (mirrors the website's manifest state).
#[tauri::command]
async fn fetch_manifest(manifest_url: String) -> Result<Value, String> {
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .timeout(std::time::Duration::from_secs(30))
        .build()
        .map_err(|e| e.to_string())?;
    let resp = client
        .get(&manifest_url)
        .send()
        .await
        .map_err(|e| format!("couldn't reach the release manifest: {e}"))?;
    if !resp.status().is_success() {
        return Err(format!(
            "no published release yet (manifest returned HTTP {}). You can still flash a local .bin.",
            resp.status().as_u16()
        ));
    }
    resp.json::<Value>()
        .await
        .map_err(|e| format!("release manifest is malformed: {e}"))
}

/// Resolve → download → flash. Streams every line espflash prints over the
/// `flash:log` event so the UI is a live console, then returns Ok on a clean
/// exit or a human error otherwise.
#[tauri::command]
async fn flash(
    app: AppHandle,
    port: String,
    product_id: String,
    manifest_url: String,
    baud: u32,
) -> Result<(), String> {
    let emit = |app: &AppHandle, line: String| {
        let _ = app.emit("flash:log", line);
    };

    // 1) Resolve the signed factory image for this exact product.
    emit(&app, format!("→ resolving signed release for {product_id}…"));
    let manifest = fetch_manifest(manifest_url).await?;
    let entry = manifest
        .get("products")
        .and_then(|p| p.get(&product_id))
        .ok_or_else(|| format!("the release doesn't offer {product_id} yet"))?;
    let factory_url = entry
        .get("factory")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no factory image in the release"))?
        .to_string();
    if let Some(v) = entry.get("version").and_then(Value::as_str) {
        emit(&app, format!("→ version {v}"));
    }

    // 2) Download it to a temp file. reqwest verifies TLS; GitHub serves the
    //    asset from the signed release the CI published.
    emit(&app, format!("→ downloading {factory_url}"));
    // The image is a few MB; guard the connect so a dead network fails fast,
    // but give the transfer itself generous headroom on a slow link.
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .connect_timeout(std::time::Duration::from_secs(15))
        .timeout(std::time::Duration::from_secs(300))
        .build()
        .map_err(|e| e.to_string())?;
    let bytes = client
        .get(&factory_url)
        .send()
        .await
        .map_err(|e| format!("download failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("download failed: {e}"))?
        .bytes()
        .await
        .map_err(|e| format!("download failed: {e}"))?;

    let safe_id: String = product_id
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    // Unique per-run name so concurrent runs (or a stale file owned by another
    // user) can't collide. `unwrap_or_default()` keeps it panic-free on a clock
    // set before the epoch (SystemTime::saturating_duration_since is nightly-only).
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let path = std::env::temp_dir().join(format!("securacv-{safe_id}-{stamp}.bin"));
    std::fs::write(&path, &bytes).map_err(|e| format!("couldn't stage the image: {e}"))?;

    // RAII cleanup: the staged image is removed on every exit path — including an
    // early return if the sidecar fails to start below.
    struct TempFileGuard(std::path::PathBuf);
    impl Drop for TempFileGuard {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let _guard = TempFileGuard(path.clone());
    emit(
        &app,
        format!("→ {} bytes staged, writing to the board…", bytes.len()),
    );

    // 3) Flash the merged factory image at 0x0. A factory image already carries
    //    the bootloader/partition table at their real offsets, so 0x0 is right.
    //    espflash hard-resets the board when it's done.
    let args = vec![
        "write-bin".into(),
        "0x0".into(),
        path.to_string_lossy().to_string(),
        "--port".into(),
        port,
        "--baud".into(),
        baud.to_string(),
    ];

    let cmd = app
        .shell()
        .sidecar(ESPFLASH)
        .map_err(|e| format!("bundled espflash missing: {e}"))?
        .args(args);
    let (mut rx, _child) = cmd
        .spawn()
        .map_err(|e| format!("could not start espflash: {e}"))?;

    let mut code = -1;
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                let text = String::from_utf8_lossy(&bytes);
                for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                    emit(&app, line.to_string());
                }
            }
            CommandEvent::Terminated(payload) => {
                code = payload.code.unwrap_or(-1);
            }
            _ => {}
        }
    }
    // (the staged image is removed by TempFileGuard on scope exit)

    if code == 0 {
        emit(&app, "✓ done — the Canary is rebooting into its new firmware.".into());
        Ok(())
    } else {
        Err(format!(
            "espflash exited with code {code}. The board can't be bricked — put it back in download mode and try again."
        ))
    }
}

/// What the UI shows in the "an update is ready" banner.
#[derive(Serialize)]
pub struct UpdateDto {
    version: String,
    current_version: String,
    notes: Option<String>,
}

/// Ask the release channel whether a newer signed build exists. Returns
/// `None` when we're current. This is the "heals itself" half: the app checks
/// the web on its own and never needs the App Store.
#[tauri::command]
async fn check_update(app: AppHandle) -> Result<Option<UpdateDto>, String> {
    let updater = app.updater().map_err(|e| e.to_string())?;
    match updater.check().await {
        Ok(Some(update)) => Ok(Some(UpdateDto {
            version: update.version.clone(),
            current_version: update.current_version.clone(),
            notes: update.body.clone(),
        })),
        Ok(None) => Ok(None),
        Err(e) => Err(format!("update check failed: {e}")),
    }
}

/// Download and install the pending update, streaming progress over
/// `update:log`, then relaunch into the new version.
#[tauri::command]
async fn install_update(app: AppHandle) -> Result<(), String> {
    let updater = app.updater().map_err(|e| e.to_string())?;
    let update = updater
        .check()
        .await
        .map_err(|e| format!("update check failed: {e}"))?
        .ok_or_else(|| "already up to date".to_string())?;

    let app2 = app.clone();
    update
        .download_and_install(
            move |chunk, total| {
                let msg = match total {
                    Some(t) => format!("downloading update… {chunk}/{t} bytes"),
                    None => format!("downloading update… {chunk} bytes"),
                };
                let _ = app2.emit("update:log", msg);
            },
            || {},
        )
        .await
        .map_err(|e| format!("update install failed: {e}"))?;

    // Relaunch into the freshly-installed version. `restart()` diverges (`!`),
    // so it stands in for the `Result` return as the tail expression.
    app.restart()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_process::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .invoke_handler(tauri::generate_handler![
            load_catalog,
            list_ports,
            detect_chip,
            fetch_manifest,
            flash,
            check_update,
            install_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running SecuraCV Flasher");
}
