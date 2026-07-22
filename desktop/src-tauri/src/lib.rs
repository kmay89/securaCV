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
//! The front-end (plain HTML/JS in `../src`) drives the complete two-port flow:
//!   * `load_catalog`   — the bundled product list + chip guard (offline-safe)
//!   * `list_ports`     — USB serial ports the OS can see right now
//!   * `detect_chip`    — ask the board which ESP32 it is (the "wrong image" guard)
//!   * `flash`          — verify, provision, write, and return an ESP32 receipt
//!   * `flash_vision_module` — verify + XMODEM-burn the WE2 model and run AT proof
//!   * serial monitor commands — live logs + machine-readable boot receipt
//!
//! Everything the user watches scroll by during a flash is `espflash`'s own
//! output, relayed verbatim over the `flash:log` event.

mod provisioning;
mod release;
mod serial_monitor;
mod we2;

use provisioning::Provisioning;
use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter};
use tauri_plugin_shell::process::CommandEvent;
use tauri_plugin_shell::ShellExt;
use tauri_plugin_updater::UpdaterExt;

// The Raspberry Pi Home Assistant hub path (design: docs/design/
// raspberry_pi_hub_flashing.md). Writing a whole-OS image to a raw disk is the
// one thing this app can do that ISN'T can't-brick-safe like an ESP32 flash — a
// wrong-disk write destroys data. The footgun-critical logic (what is a legal
// write target, and how to enumerate disks and tell the system disk from an
// external one) lives in the dependency-free `hub-core` crate (../hub-core) so
// it is unit-tested on every PR without the desktop stack. The enumerator's
// tauri command, the confirm UI, and the guarded write build on top of it in
// follow-up changes — each must run a candidate through `hub_core::hub_disk::
// classify` and refuse anything not `Eligible`. (Wired in via a path dependency
// when the first command that uses it lands.)

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

/// Stage a firmware image in an atomically-created, randomly named private
/// file. `NamedTempFile` creates mode 0600 on Unix and removes the file on
/// drop, so a provisioned image never passes through a world-readable path.
fn stage_firmware(bytes: &[u8], safe_id: &str) -> Result<tempfile::NamedTempFile, String> {
    use std::io::Write;

    let mut staged = tempfile::Builder::new()
        .prefix(&format!("securacv-{safe_id}-"))
        .suffix(".bin")
        .tempfile()
        .map_err(|e| format!("couldn't create private firmware staging file: {e}"))?;
    staged
        .write_all(bytes)
        .and_then(|_| staged.flush())
        .map_err(|e| format!("couldn't stage the image: {e}"))?;
    Ok(staged)
}

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

#[derive(Serialize)]
pub struct FlashReceipt {
    target: &'static str,
    product_id: String,
    version: String,
    release_sha256: String,
    installed_sha256: String,
    bytes_written: usize,
    release_verification: &'static str,
    chip_write_verified: bool,
    provisioned: bool,
}

/// The embedded flasher catalog, handed to the UI verbatim. The front-end
/// already knows this schema (it is the website's), so we don't re-type it
/// here — we just guarantee it parses.
#[tauri::command]
fn load_catalog() -> Result<Value, String> {
    serde_json::from_str(EMBEDDED_CATALOG).map_err(|e| format!("bundled catalog is corrupt: {e}"))
}

/// Serial ports the OS can see this instant. No Web Serial permission prompt,
/// no Chromium — just the platform enumerating its own devices.
#[tauri::command]
fn list_ports() -> Result<Vec<PortDto>, String> {
    let ports =
        serialport::available_ports().map_err(|e| format!("could not list serial ports: {e}"))?;
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
    let (code, out) =
        run_sidecar_capture(&app, vec!["board-info".into(), "--port".into(), port]).await?;
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

/// Fetch the live release manifest so the UI can show what version is
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
    detected_chip: String,
    provisioning: Option<Provisioning>,
) -> Result<FlashReceipt, String> {
    let emit = |app: &AppHandle, line: String| {
        let _ = app.emit("flash:log", line);
    };

    // 1) Resolve the official factory image for this exact product.
    emit(
        &app,
        format!("→ resolving verified release for {product_id}…"),
    );
    let catalog: Value = serde_json::from_str(EMBEDDED_CATALOG)
        .map_err(|e| format!("bundled catalog is corrupt: {e}"))?;
    if catalog.get("manifest_url").and_then(Value::as_str) != Some(manifest_url.as_str()) {
        return Err("refusing an unbundled firmware manifest URL".into());
    }
    let product = catalog
        .get("products")
        .and_then(Value::as_array)
        .and_then(|products| {
            products
                .iter()
                .find(|product| product.get("id").and_then(Value::as_str) == Some(&product_id))
        })
        .ok_or_else(|| format!("{product_id} is not in the bundled product catalog"))?;
    let catalog_chip = product
        .get("chip")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no chip guard in the catalog"))?;
    if canonical_chip(catalog_chip) != canonical_chip(&detected_chip) {
        return Err(format!(
            "the catalog requires {catalog_chip}, but {detected_chip} was detected; refusing to write"
        ));
    }
    let needs_provisioning =
        product.get("provisioning").and_then(Value::as_str) == Some("usb-secrets");
    // The generated catalog derives this from the product's real serial-command
    // implementation. Missing fields fail closed for older/unknown catalogs.
    let expects_serial_receipt = product
        .get("serial_receipt")
        .and_then(Value::as_bool)
        .unwrap_or(true);
    if needs_provisioning && provisioning.is_none() {
        return Err(
            "this firmware uses generic release placeholders; Wi-Fi and MQTT provisioning is required before flash"
                .into(),
        );
    }

    let manifest = fetch_manifest(manifest_url).await?;
    if manifest.get("schema").and_then(Value::as_str) != Some("securacv-flash-1") {
        return Err("release manifest has an unexpected schema".into());
    }
    let entry = manifest
        .get("products")
        .and_then(|p| p.get(&product_id))
        .ok_or_else(|| format!("the release doesn't offer {product_id} yet"))?;
    let manifest_chip = entry
        .get("chipFamily")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no chip family in the release"))?;
    if canonical_chip(manifest_chip) != canonical_chip(&detected_chip) {
        return Err(format!(
            "the release offers {manifest_chip}, but {detected_chip} is connected; refusing to write"
        ));
    }
    let factory_url = entry
        .get("factory")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no factory image in the release"))?
        .to_string();
    if !factory_url.starts_with("https://github.com/kmay89/securaCV/releases/download/") {
        return Err(
            "release image URL is outside the bundled SecuraCV GitHub release origin".into(),
        );
    }
    let version = entry
        .get("version")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no version in the release"))?
        .to_string();
    let expected_size = entry
        .get("size")
        .and_then(Value::as_u64)
        .filter(|size| *size > 0)
        .ok_or_else(|| format!("{product_id} has an invalid release size"))?;
    let expected_sha = entry
        .get("sha256")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{product_id} has no SHA-256 in the release"))?;
    emit(&app, format!("→ version {version}"));

    // 2) Download it to a temp file. reqwest verifies TLS; GitHub serves the
    //    asset from the release the CI published.
    emit(&app, format!("→ downloading {factory_url}"));
    // The image is a few MB; guard the connect so a dead network fails fast,
    // but give the transfer itself generous headroom on a slow link.
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .connect_timeout(std::time::Duration::from_secs(15))
        .timeout(std::time::Duration::from_secs(300))
        .build()
        .map_err(|e| e.to_string())?;
    let downloaded = client
        .get(&factory_url)
        .send()
        .await
        .map_err(|e| format!("download failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("download failed: {e}"))?
        .bytes()
        .await
        .map_err(|e| format!("download failed: {e}"))?;

    // TLS identifies GitHub; these checks identify the actual release bytes.
    let release_sha = release::verify_size_and_sha(&downloaded, expected_size, expected_sha)?;
    let release_pubkey = catalog
        .get("release_pubkey")
        .and_then(Value::as_str)
        .ok_or_else(|| "bundled catalog has no release public key".to_string())?;
    let release_verification = release::verify_signature(
        downloaded.len(),
        &release_sha,
        entry.get("signature").and_then(Value::as_str),
        release_pubkey,
    )?;
    emit(
        &app,
        format!(
            "✓ release verified: SHA-256 {}… ({release_verification})",
            &release_sha[..16]
        ),
    );

    // Provision only after verifying the untouched release. The installed hash
    // records the exact per-device image we actually hand to espflash.
    let mut bytes = downloaded.to_vec();
    let provisioned = if let Some(config) = provisioning.as_ref() {
        provisioning::patch_factory_image(&mut bytes, config)?;
        emit(
            &app,
            "✓ Wi-Fi + MQTT settings sealed into the image's NVS partition (values not logged)"
                .into(),
        );
        true
    } else {
        false
    };
    let installed_sha = release::sha256_hex(&bytes);

    let safe_id: String = product_id
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    // Keep the private file handle alive until espflash exits. Its RAII guard
    // removes the path on success and on every ordinary error return.
    let staged = stage_firmware(&bytes, &safe_id)?;
    let path = staged.path();
    emit(
        &app,
        format!(
            "→ {} bytes staged (installed SHA-256 {}…), writing to the board…",
            bytes.len(),
            &installed_sha[..16]
        ),
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
    // (`staged` removes the private image on scope exit.)

    if code == 0 {
        let message = if expects_serial_receipt {
            "✓ chip write verified — reopening serial for the live boot receipt."
        } else {
            "✓ chip write verified — this firmware does not require a live receipt."
        };
        emit(&app, message.into());
        Ok(FlashReceipt {
            target: "esp32-host",
            product_id,
            version,
            release_sha256: release_sha,
            installed_sha256: installed_sha,
            bytes_written: bytes.len(),
            release_verification,
            chip_write_verified: true,
            provisioned,
        })
    } else {
        Err(format!(
            "espflash exited with code {code}. The board can't be bricked — put it back in download mode and try again."
        ))
    }
}

/// Download the pinned WE2 model, verify the manifest hash, burn it to the
/// module's custom-model slot, then require SSCMA AT + one-inference proof.
#[tauri::command]
async fn flash_vision_module(
    app: AppHandle,
    port: String,
    manifest_url: String,
) -> Result<we2::ModuleReceipt, String> {
    let port_info = serialport::available_ports()
        .map_err(|e| format!("could not list serial ports: {e}"))?
        .into_iter()
        .find(|candidate| candidate.port_name == port)
        .ok_or_else(|| "the selected Vision-module port is no longer connected".to_string())?;
    let (vid, pid) = match port_info.port_type {
        serialport::SerialPortType::UsbPort(info) => (Some(info.vid), Some(info.pid)),
        _ => (None, None),
    };
    if !we2::is_module_usb(vid, pid) {
        return Err("the selected port is not the Grove Vision AI V2 CH343 (USB 1a86:55d3)".into());
    }
    let catalog: Value = serde_json::from_str(EMBEDDED_CATALOG)
        .map_err(|e| format!("bundled catalog is corrupt: {e}"))?;
    let expected_manifest = catalog
        .get("we2_module")
        .and_then(|module| module.get("manifest_url"))
        .and_then(Value::as_str)
        .ok_or_else(|| "bundled catalog has no Vision-module manifest".to_string())?;
    if manifest_url != expected_manifest {
        return Err("refusing an unbundled Vision-module manifest URL".into());
    }
    let emit_log = |message: String| {
        let _ = app.emit("vision:log", message);
    };
    emit_log("→ resolving the pinned Grove Vision AI V2 model…".into());
    let manifest = fetch_manifest(manifest_url).await?;
    let version = manifest
        .get("version")
        .and_then(Value::as_str)
        .ok_or_else(|| "Vision model manifest has no version".to_string())?
        .to_string();
    let model = manifest
        .get("model")
        .ok_or_else(|| "Vision model manifest has no model entry".to_string())?;
    let model_url = model
        .get("url")
        .and_then(Value::as_str)
        .ok_or_else(|| "Vision model manifest has no download URL".to_string())?;
    if !model_url.starts_with("https://github.com/kmay89/securaCV/releases/download/") {
        return Err(
            "Vision model URL is outside the bundled SecuraCV GitHub release origin".into(),
        );
    }
    let expected_size = model
        .get("size")
        .and_then(Value::as_u64)
        .filter(|size| *size > 0)
        .ok_or_else(|| "Vision model manifest has an invalid size".to_string())?;
    let expected_sha = model
        .get("sha256")
        .and_then(Value::as_str)
        .ok_or_else(|| "Vision model manifest has no SHA-256".to_string())?;
    let flash_address = model
        .get("flash_addr")
        .and_then(Value::as_str)
        .unwrap_or("");
    if !flash_address.eq_ignore_ascii_case("0x400000") {
        return Err(format!(
            "Vision manifest targets {flash_address}, but this app only permits the model slot 0x400000"
        ));
    }
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .connect_timeout(std::time::Duration::from_secs(15))
        .timeout(std::time::Duration::from_secs(300))
        .build()
        .map_err(|e| e.to_string())?;
    let bytes = client
        .get(model_url)
        .send()
        .await
        .map_err(|e| format!("Vision model download failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("Vision model download failed: {e}"))?
        .bytes()
        .await
        .map_err(|e| format!("Vision model download failed: {e}"))?;
    let sha = release::verify_size_and_sha(&bytes, expected_size, expected_sha)?;
    emit_log(format!(
        "✓ model verified: {} bytes · SHA-256 {}…",
        bytes.len(),
        &sha[..16]
    ));

    let app_for_log = app.clone();
    let app_for_progress = app.clone();
    let model_bytes = bytes.to_vec();
    tauri::async_runtime::spawn_blocking(move || {
        we2::flash_and_prove(
            &port,
            &model_bytes,
            version,
            sha,
            move |line| {
                let _ = app_for_log.emit("vision:log", line);
            },
            move |fraction| {
                let _ = app_for_progress.emit("vision:progress", fraction);
            },
        )
    })
    .await
    .map_err(|e| format!("Vision flasher worker failed: {e}"))?
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
        .manage(serial_monitor::SerialMonitorState::default())
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
            flash_vision_module,
            serial_monitor::start_serial_monitor,
            serial_monitor::serial_monitor_send,
            serial_monitor::stop_serial_monitor,
            check_update,
            install_update
        ])
        .run(tauri::generate_context!())
        .expect("error while running SecuraCV Flasher");
}

#[cfg(test)]
mod staging_tests {
    use super::stage_firmware;

    #[test]
    fn staged_firmware_is_private_and_removed_on_drop() {
        let staged = stage_firmware(b"provisioned-secret-image", "canary-vision")
            .expect("private staging file");
        let path = staged.path().to_path_buf();
        assert_eq!(
            std::fs::read(&path).expect("staged bytes"),
            b"provisioned-secret-image"
        );

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&path)
                .expect("staging metadata")
                .permissions()
                .mode();
            assert_eq!(
                mode & 0o077,
                0,
                "staging file must not be group/world accessible"
            );
        }

        drop(staged);
        assert!(!path.exists(), "staging path must be removed on drop");
    }
}
