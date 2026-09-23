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

mod fleet;
mod host;
mod hub;
mod launch_guard;
mod secret_store;
mod serial_monitor;
mod sscma;
mod we2;
mod we2_bench;
mod whoami;

// The ESP32 flash engine (desktop/flash-engine) — shared with the Lab and
// PR-CI-tested on its own. Imported at the crate root under the names these
// modules had when they lived here, so `crate::port_hint` et al. keep
// resolving for we2.rs / we2_bench.rs.
use flash_engine::catalog::Catalog;
use flash_engine::flash::{ChipInfo, FlashReceipt, FlashRequest};
use flash_engine::image::{check_local_image, stage_firmware};
use flash_engine::ports::PortDto;
use flash_engine::provisioning::Provisioning;
use flash_engine::{health, port_hint, release, rescue};
use host::TauriHost;
use serde::Serialize;
use serde_json::{json, Map, Value};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons};
use tauri_plugin_opener::OpenerExt;
use tauri_plugin_updater::UpdaterExt;

// The Raspberry Pi Home Assistant hub path (design: docs/design/
// raspberry_pi_hub_flashing.md) lives in src/hub.rs. Writing a whole-OS image
// to a raw disk is the one thing this app can do that ISN'T can't-brick-safe
// like an ESP32 flash — so every decision (legal target, image trust, write
// authorization) stays in the PR-CI-tested `hub-core` crate and every
// mechanism (download/hash, xz, read-back-verified write, Wi-Fi seed) in
// `hub-io`; hub.rs only translates and orchestrates.

// The flasher catalog is baked in at compile time so the app can list every
// Canary and enforce the chip guard with zero network. build.rs copies the ONE
// canonical `canary-local/devices/flash.json` into OUT_DIR on every build, so
// this embed can never drift from the website/firmware source of truth.
pub(crate) const EMBEDDED_CATALOG: &str = include_str!(concat!(env!("OUT_DIR"), "/flash.json"));

/// The embedded catalog and everything the flash engine derives from it —
/// the chip table behind canonical_chip(), the release origin, the manifest
/// allow-list (flash_engine::catalog) — computed once per launch.
fn bundled_catalog() -> &'static Catalog {
    static CATALOG: std::sync::OnceLock<Catalog> = std::sync::OnceLock::new();
    CATALOG.get_or_init(|| Catalog::new(EMBEDDED_CATALOG))
}

/// The one origin this app downloads release assets from, derived from the
/// catalog's own pinned manifest_url (flash_engine::catalog). None (fail
/// closed: nothing downloads) if the catalog is corrupt or its manifest_url is
/// not a releases/download URL.
fn release_origin() -> Option<&'static str> {
    bundled_catalog().release_origin()
}

// The Hatchery naming spec — the same canary-local/devices/hatch.json the
// website ships — embedded so the flasher's birth certificate names a Canary
// identically to the web Lab, offline, and can never drift from it.
const EMBEDDED_HATCH: &str = include_str!(concat!(env!("OUT_DIR"), "/hatch.json"));

/// The embedded flasher catalog, handed to the UI verbatim. The front-end
/// already knows this schema (it is the website's), so we don't re-type it
/// here — we just guarantee it parses.
#[tauri::command]
fn load_catalog() -> Result<Value, String> {
    serde_json::from_str(EMBEDDED_CATALOG).map_err(|e| format!("bundled catalog is corrupt: {e}"))
}

/// The Hatchery naming spec (name parts, mottoes, certificate copy), shared
/// verbatim with the website. The UI mints the same whimsical names + birth
/// certificate the web Lab does — offline, from this one embedded source.
#[tauri::command]
fn load_hatch() -> Result<Value, String> {
    serde_json::from_str(EMBEDDED_HATCH).map_err(|e| format!("bundled hatch spec is corrupt: {e}"))
}

/// What build am I? The version, the exact git rev, the moment it was compiled,
/// and the firmware train it embeds — everything the About/Health panel needs
/// to say "this is the build you're running" with no guessing. Build stamps are
/// baked in by build.rs; the firmware train comes from the embedded catalog.
#[derive(Serialize)]
struct AppInfo {
    version: String,
    build_rev: String,
    build_epoch: u64,
    fw_train: Option<String>,
}

// The Wi-Fi network this computer is on right now — the network a new Canary
// almost always wants — so the SSID field starts filled and joining is one
// password away. Best-effort: any failure just means no prefill.
#[tauri::command]
fn current_ssid() -> Option<String> {
    #[cfg(target_os = "macos")]
    {
        for iface in ["en0", "en1"] {
            if let Ok(out) = std::process::Command::new("networksetup")
                .args(["-getairportnetwork", iface])
                .output()
            {
                // "Current Wi-Fi Network: <name>" — anything else (off,
                // not a Wi-Fi interface) has no colon-name to take.
                let text = String::from_utf8_lossy(&out.stdout);
                if let Some((_, name)) = text.split_once(": ") {
                    let name = name.trim();
                    if !name.is_empty() {
                        return Some(name.to_string());
                    }
                }
            }
        }
        None
    }
    #[cfg(target_os = "linux")]
    {
        if let Ok(out) = std::process::Command::new("iwgetid").arg("-r").output() {
            let name = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !name.is_empty() {
                return Some(name);
            }
        }
        if let Ok(out) = std::process::Command::new("nmcli")
            .args(["-t", "-f", "active,ssid", "dev", "wifi"])
            .output()
        {
            for line in String::from_utf8_lossy(&out.stdout).lines() {
                if let Some(ssid) = line.strip_prefix("yes:") {
                    if !ssid.is_empty() {
                        return Some(ssid.to_string());
                    }
                }
            }
        }
        None
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    {
        None
    }
}

// The saved password for a Wi-Fi network, read from the OS's own store — the
// macOS Keychain via `security` (the system shows its consent prompt first)
// or NetworkManager via `nmcli -s` (polkit may prompt). The value goes
// straight into the field and is never logged or persisted by the app.
//
// The "Use saved" click that reaches here is the frontend's word — and a
// script injected into the webview has the same word, for any SSID it likes.
// So a native dialog naming the network asks first, and the OS store is read
// only after a real hand said yes (macOS then adds its Keychain prompt as a
// second layer; nmcli on Linux would otherwise answer with no prompt at all).
#[tauri::command]
async fn saved_wifi_password(app: AppHandle, ssid: String) -> Result<String, String> {
    let ssid = ssid.trim().to_string();
    if ssid.is_empty() {
        return Err("type the Wi-Fi name first".to_string());
    }
    let prompt_app = app.clone();
    let prompt_ssid = ssid.clone();
    let approved = tauri::async_runtime::spawn_blocking(move || {
        prompt_app
            .dialog()
            .message(format!(
                "Fill in the saved password for \u{201c}{prompt_ssid}\u{201d}?\n\n\
                 It is read from this computer's own password store straight into \
                 the field — never logged or kept by the app."
            ))
            .title("Use the saved Wi-Fi password?")
            .buttons(MessageDialogButtons::OkCancelCustom(
                "Use saved password".into(),
                "Not now".into(),
            ))
            .blocking_show()
    })
    .await
    .map_err(|e| format!("couldn't ask: {e}"))?;
    if !approved {
        return Err("not filled in — the saved password stays where it is".to_string());
    }
    read_saved_wifi_password(ssid)
}

fn read_saved_wifi_password(ssid: String) -> Result<String, String> {
    #[cfg(target_os = "macos")]
    {
        let out = std::process::Command::new("security")
            .args([
                "find-generic-password",
                "-D",
                "AirPort network password",
                "-a",
                &ssid,
                "-w",
            ])
            .output()
            .map_err(|e| format!("couldn't ask the keychain: {e}"))?;
        let pw = String::from_utf8_lossy(&out.stdout)
            .trim_end_matches(['\r', '\n'])
            .to_string();
        if out.status.success() && !pw.is_empty() {
            Ok(pw)
        } else {
            Err(format!(
                "no saved password for \u{201c}{ssid}\u{201d} — or the keychain prompt was declined. Typing it works too."
            ))
        }
    }
    #[cfg(target_os = "linux")]
    {
        let out = std::process::Command::new("nmcli")
            .args([
                "-s",
                "-g",
                "802-11-wireless-security.psk",
                "connection",
                "show",
                &ssid,
            ])
            .output()
            .map_err(|e| format!("couldn't ask NetworkManager: {e}"))?;
        // Strip only the command's trailing newline — a PSK may legitimately
        // begin or end with a space (hub-core's seed tests cover exactly
        // that), and trimming it would provision a different credential.
        let pw = String::from_utf8_lossy(&out.stdout)
            .trim_end_matches(['\r', '\n'])
            .to_string();
        if out.status.success() && !pw.is_empty() {
            Ok(pw)
        } else {
            Err(format!(
                "no saved password for \u{201c}{ssid}\u{201d} here (open network, different connection name, or permission declined). Typing it works too."
            ))
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    {
        let _ = ssid;
        Err("reading saved Wi-Fi passwords isn't supported on this platform".to_string())
    }
}

#[tauri::command]
fn app_info() -> AppInfo {
    let fw_train = serde_json::from_str::<Value>(EMBEDDED_CATALOG)
        .ok()
        .and_then(|c| {
            c.get("fw_train")
                .and_then(|v| v.as_str())
                .map(str::to_string)
        });
    AppInfo {
        version: env!("CARGO_PKG_VERSION").to_string(),
        build_rev: env!("SECURACV_BUILD_REV").to_string(),
        build_epoch: env!("SECURACV_BUILD_EPOCH").parse::<u64>().unwrap_or(0),
        fw_train,
    }
}

/// Serial ports the OS can see this instant. No Web Serial permission prompt,
/// no Chromium — just the platform enumerating its own devices.
#[tauri::command]
fn list_ports() -> Result<Vec<PortDto>, String> {
    flash_engine::ports::list_ports()
}

/// Run the sidecar to completion, collecting stdout+stderr. Used for the short
/// region reads where we want the whole answer, not a live stream. The spawn
/// is this app's (host::TauriHost → launch_guard::spawn_tracked).
async fn run_sidecar_capture(app: &AppHandle, args: Vec<String>) -> Result<(i32, String), String> {
    flash_engine::host::run_capture(&TauriHost(app.clone()), args).await
}

/// Ask the connected board which ESP32 it is (and how much flash it carries).
#[tauri::command]
async fn detect_chip(app: AppHandle, port: String) -> Result<ChipInfo, String> {
    flash_engine::flash::detect_chip(&TauriHost(app), bundled_catalog(), port).await
}

/// Fetch the live release manifest so the UI can show what version is
/// currently published for each product — only ever one of the bundled
/// manifest URLs (the gate is the engine's, before any socket).
#[tauri::command]
async fn fetch_manifest(app: AppHandle, manifest_url: String) -> Result<Value, String> {
    flash_engine::flash::fetch_manifest(&TauriHost(app), bundled_catalog(), manifest_url).await
}

/// Find a freshly-flashed Canary on the LAN and return its kernel's fleet.
///
/// After a flash the device joins the Wi-Fi the Flasher just provisioned, so —
/// unlike a sandboxed browser page (the embedded Witness Wall runs under a
/// tight `connect-src 'self'` CSP) — the native app can reach it directly.
/// `.local` hostnames resolve through the OS resolver (Bonjour / avahi), so no
/// mDNS crate is needed. This does ONE pass over the candidate bases and
/// returns the first `/api/fleet` that answers; the frontend polls it while the
/// board boots and joins the network. Coarse presence/health only — the same
/// endpoint the emulator's "connect" uses (see `tvos/discovery/DISCOVERY.md`).
#[tauri::command]
async fn witness_discover(bases: Vec<String>) -> Result<Value, String> {
    // Same transport policy as every fleet-book call (fleet.rs base_ok): a
    // discovery probe must never be steerable at an internet host. Candidates
    // that fail the local-host gate are skipped, not fatal — the list is
    // best-effort by design and the `.local` defaults always qualify.
    let bases: Vec<&String> = bases.iter().filter(|b| fleet::base_ok(b)).collect();
    if bases.is_empty() {
        return Err(
            "no local device address to try — device addresses must be local/private hosts"
                .to_string(),
        );
    }
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .timeout(std::time::Duration::from_secs(2))
        // Same local-first transport policy as fleet.rs's device calls: the
        // URL is gated, the connection needs .no_proxy() too.
        .no_proxy()
        .build()
        .map_err(|e| e.to_string())?;
    for base in &bases {
        let url = format!("{}/api/fleet", base.trim_end_matches('/'));
        if let Ok(resp) = client.get(&url).send().await {
            if resp.status().is_success() {
                if let Ok(v) = resp.json::<Value>().await {
                    return Ok(v);
                }
            }
        }
    }
    Err("no kernel answered on the LAN yet".to_string())
}

/// Resolve → download → flash. Streams every line espflash prints over the
/// `flash:log` event so the UI is a live console, then returns Ok on a clean
/// exit or a human error otherwise. The pipeline is the flash engine's
/// (flash_engine::flash::flash) — the Lab runs the same one.
// The argument list IS the frontend's invoke contract (and the Lab's twin
// takes the same nine), so it stays flat rather than becoming a struct.
#[allow(clippy::too_many_arguments)]
#[tauri::command]
async fn flash(
    app: AppHandle,
    port: String,
    product_id: String,
    manifest_url: String,
    baud: u32,
    detected_chip: String,
    provisioning: Option<Provisioning>,
    erase_first: Option<bool>,
    // The safety copy taken moments ago, if there is one. Present = draw the
    // change map from it; absent = the user skipped the copy or the board
    // wouldn't read, and no map is the truthful outcome.
    backup_path: Option<String>,
) -> Result<FlashReceipt, String> {
    let request = FlashRequest {
        port,
        product_id,
        manifest_url,
        baud,
        detected_chip,
        provisioning,
        erase_first,
        backup_path,
    };
    flash_engine::flash::flash(&TauriHost(app), bundled_catalog(), request).await
}

/// What the Advanced local-file panel shows BEFORE anything is written: size,
/// SHA-256 fingerprint, and whether the file starts with 0xE9 — the ESP32's
/// own "program starts here" marker, which a merged factory image also opens
/// with (it begins with the bootloader image). A missing magic only informs;
/// the write path never blocks on it.
#[derive(Serialize)]
pub struct LocalFileInfo {
    size: u64,
    sha256: String,
    esp_magic: bool,
}

#[tauri::command]
async fn inspect_local_file(path: String) -> Result<LocalFileInfo, String> {
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read that file: {e}"))?;
    check_local_image(&bytes)?;
    Ok(LocalFileInfo {
        size: bytes.len() as u64,
        sha256: release::sha256_hex(&bytes),
        esp_magic: bytes.first() == Some(&0xE9),
    })
}

/// Flash a firmware file straight off this computer's disk (Advanced). No
/// catalog product, no manifest, no signature — a personal file has no origin
/// we can verify, so nothing here claims "verified": the file is fingerprinted
/// (SHA-256 + size) so the receipt names exactly what was written, and that is
/// all the receipt claims. `expected_size`/`expected_sha256` are the values
/// inspect_local_file showed and the user confirmed — the file is re-read and
/// re-hashed here, so a file that changed on disk between the two calls is
/// refused rather than silently written. The write mechanics are identical to
/// flash(): private staged temp file, `write-bin 0x0` via the bundled
/// espflash, `flash:log` streaming.
#[tauri::command]
async fn flash_local_file(
    app: AppHandle,
    port: String,
    baud: u32,
    path: String,
    expected_size: u64,
    expected_sha256: String,
) -> Result<FlashReceipt, String> {
    let emit = |app: &AppHandle, line: String| {
        let _ = app.emit("flash:log", line);
    };

    let file_name = std::path::Path::new(&path)
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "local file".to_string());
    emit(&app, format!("→ reading {file_name} from this computer…"));
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read that file: {e}"))?;
    check_local_image(&bytes)?;
    let sha = release::sha256_hex(&bytes);
    // The confirm was over the inspected fingerprint, not over a path — a
    // path can point at different bytes a moment later.
    if bytes.len() as u64 != expected_size || !sha.eq_ignore_ascii_case(&expected_sha256) {
        return Err("the file changed since it was inspected — pick it again".into());
    }
    emit(
        &app,
        format!(
            "→ fingerprint only — we can't vouch for a personal file's origin. SHA-256 {}…",
            &sha[..16]
        ),
    );
    if bytes[0] != 0xE9 {
        // Warn, never block: the partition table at 0x8000 already vouched
        // for the factory shape, and on the catalog's chips (C3/C6/S3) the
        // bootloader sits at 0x0, so a factory image normally opens with the
        // 0xE9 image magic — but the board can't be bricked either way.
        emit(
            &app,
            format!(
                "⚠ first byte is 0x{:02X}, not the ESP32 image magic 0xE9 — unusual for a C3/C6/S3 factory image. Writing anyway; the board can't be bricked.",
                bytes[0]
            ),
        );
    }

    // Same staging discipline as the release path: a private, RAII-removed
    // temp file, alive until espflash exits.
    let staged = stage_firmware(&bytes, "local-file")?;
    let staged_path = staged.path();
    emit(
        &app,
        format!("→ {} bytes staged, writing to the board…", bytes.len()),
    );

    let args = rescue::write_bin_args(&port, &staged_path.to_string_lossy(), baud);
    // The engine's write: the same tracked spawn, `flash:log` streaming and
    // kept tail (the frontend classifies a failure by espflash's last words)
    // as the release path.
    let (code, tail) =
        flash_engine::host::run_streaming_with_tail(&TauriHost(app.clone()), args, "flash:log")
            .await?;
    // (`staged` removes the private image on scope exit.)

    if code == 0 {
        emit(
            &app,
            "✓ chip write verified — your file is on the board.".into(),
        );
        Ok(FlashReceipt {
            target: "esp32-host",
            product_id: file_name,
            version: "local".to_string(),
            release_sha256: sha.clone(),
            installed_sha256: sha,
            bytes_written: bytes.len(),
            release_verification: "local-file (fingerprint only)",
            channel: "local",
            chip_write_verified: true,
            provisioned: false,
            broker_tls: None,
        })
    } else {
        Err(flash_engine::flash::write_failure(code, &tail))
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
    let manifest = flash_engine::flash::fetch_manifest(
        &TauriHost(app.clone()),
        bundled_catalog(),
        manifest_url,
    )
    .await?;
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
    if !release_origin().is_some_and(|origin| model_url.starts_with(origin)) {
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
    // Chunked so the console keeps moving on a slow link — the wait between
    // "resolving…" and "verified" used to be a single silent bytes() call.
    emit_log(format!(
        "→ downloading the model ({} KB)…",
        expected_size / 1024
    ));
    let mut resp = client
        .get(model_url)
        .send()
        .await
        .map_err(|e| format!("Vision model download failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("Vision model download failed: {e}"))?;
    let mut bytes: Vec<u8> = Vec::with_capacity(expected_size as usize);
    let mut last_tick = std::time::Instant::now();
    while let Some(chunk) = resp
        .chunk()
        .await
        .map_err(|e| format!("Vision model download failed: {e}"))?
    {
        bytes.extend_from_slice(&chunk);
        if last_tick.elapsed().as_millis() >= 500 {
            last_tick = std::time::Instant::now();
            emit_log(format!(
                "  … {} of {} KB",
                bytes.len() / 1024,
                expected_size / 1024
            ));
        }
    }
    let sha = release::verify_size_and_sha(&bytes, expected_size, expected_sha)?;
    // Same fail-closed trust model as the firmware channel (and the browser
    // flasher's verifyPinnedModelAsset): once a real release key is pinned, the
    // model manifest MUST carry a valid Ed25519 signature over
    // uint32_le(size)||sha256 — a checksum alone is repointable by whoever can
    // swap release assets. checksum-only survives only the pre-key ceremony
    // (all-zero pinned key). "Two flashers, two frontends": this must match the
    // browser channel, and desktop_parity asserts it.
    let release_pubkey = catalog
        .get("release_pubkey")
        .and_then(Value::as_str)
        .ok_or_else(|| "bundled catalog has no release public key".to_string())?;
    let model_verification = release::verify_signature(
        bytes.len(),
        &sha,
        model.get("signature").and_then(Value::as_str),
        release_pubkey,
    )?;
    emit_log(format!(
        "✓ model verified: {} bytes · SHA-256 {}… ({model_verification})",
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

    // Deliberately NOT `download_and_install`: that would put the download
    // inside the danger window too. Downloading touches nothing but a buffer —
    // being killed there costs the user a re-download and nothing else, so
    // marking it would tell them to reinstall a copy that is perfectly fine.
    // Only `install` moves the app on disk, so only `install` is marked.
    let app2 = app.clone();
    let bytes = update
        .download(
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
        .map_err(|e| format!("update download failed: {e}"))?;

    // From here the app bundle itself moves. On macOS `install` renames the
    // running `.app` out to a temp backup and renames the new one in — two
    // atomic steps, but with a window between them where nothing is at the
    // original path, and a privileged install (`rm -rf` via AppleScript) that
    // is not atomic at all. A process killed in here leaves the user with an
    // app that is missing or incomplete, which no code inside that app can
    // repair afterwards. The marker goes down first so the next launch can at
    // least *name* what happened. See `launch_guard`.
    let guard = app.state::<Arc<launch_guard::LaunchGuard>>();
    guard.begin_install(&update.version);
    let outcome = update.install(bytes);
    // Cleared on failure too: an install that returned an error unwound and
    // put the bundle back, so the next launch has nothing to warn about.
    guard.end_install();
    outcome.map_err(|e| format!("update install failed: {e}"))?;

    // Relaunch into the freshly-installed version. `restart()` diverges (`!`),
    // so it stands in for the `Result` return as the tail expression.
    app.restart()
}

// ── the rescue bench: back up / restore / erase / flash a local image ────────
// The espflash I/O around the pure `rescue` module (host-tested). Each streams
// the sidecar's output over `rescue:log` so the UI is a live console, and none
// can brick the board — the ESP32's first-stage bootloader is mask ROM.

/// Spawn the espflash sidecar with `args`, streaming each non-empty line over
/// `event`, and return its exit code (flash_engine::host::run_streaming over
/// this app's tracked spawn).
async fn run_sidecar_streaming(
    app: &AppHandle,
    args: Vec<String>,
    event: &'static str,
) -> Result<i32, String> {
    flash_engine::host::run_streaming(&TauriHost(app.clone()), args, event).await
}

/// Where the automatic pre-flash safety copy lands: a per-app backups folder,
/// named by the board's MAC + moment, created on demand. The frontend can't
/// know the app-data dir, and the browser flasher's equivalent (an unasked
/// download before every write) is the parity bar this path exists to meet —
/// the reflash-with-no-undo gap was desktop-only.
#[tauri::command]
fn auto_backup_path(app: AppHandle, mac: String) -> Result<String, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data dir: {e}"))?
        .join("backups");
    std::fs::create_dir_all(&dir).map_err(|e| format!("couldn't create backups dir: {e}"))?;
    let safe_mac: String = mac
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    Ok(dir
        .join(format!(
            "canary-{}-{stamp}.bin",
            if safe_mac.is_empty() {
                "unknown".into()
            } else {
                safe_mac
            }
        ))
        .to_string_lossy()
        .to_string())
}

/// Back up the whole chip to `out_path` — a full-flash read the operator keeps
/// and can restore later. The safety copy the one-shot flow never had.
#[tauri::command]
async fn backup_flash(
    app: AppHandle,
    port: String,
    out_path: String,
    flash_size: u64,
    baud: u32,
) -> Result<(), String> {
    if flash_size == 0 {
        return Err("couldn't read this chip's flash size — reconnect and try again".into());
    }
    let out_path = validated_backup_path(&out_path)?
        .to_string_lossy()
        .into_owned();
    let _ = app.emit(
        "rescue:log",
        format!(
            "→ reading {} of flash → {out_path}…",
            rescue::human_bytes(flash_size)
        ),
    );
    let code = run_sidecar_streaming(
        &app,
        rescue::read_flash_args(&port, flash_size, &out_path, baud),
        "rescue:log",
    )
    .await?;
    if code == 0 {
        let _ = app.emit(
            "rescue:log",
            "✓ backup saved — keep it safe; restore it any time.".to_string(),
        );
        Ok(())
    } else {
        Err(format!(
            "espflash exited with code {code} while reading the chip. Nothing on the board changed — try again."
        ))
    }
}

/// Write a local image to the chip at 0x0 — a restored backup, or any `.bin`.
///
/// Guards the offset-0 write with the SAME check the Advanced local-file path
/// uses (`check_local_image`): an app-only build has no partition table at
/// 0x8000 and, written from 0x0, would land on the bootloader and stop the
/// board booting. Both surfaces agree here — the browser's local-file picker
/// gates on `core.localImageShape`, native on `check_local_image`, same refusal
/// in the same words. A genuine full-flash backup carries that table, so it
/// passes; only an app-only file is turned away. The flash-size fit is then
/// checked against THIS chip, mirroring the browser's restore validation.
#[tauri::command]
async fn write_local_image(
    app: AppHandle,
    port: String,
    path: String,
    flash_size: Option<u64>,
    baud: u32,
) -> Result<(), String> {
    // A firmware image is at most one chip's flash (≤ 32 MiB), so read it once —
    // the same bounded read `flash_local_file` does — and run the offset-0 guard
    // before anything is written. espflash re-reads the path when it writes.
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read {path}: {e}"))?;
    // Single source of truth for "safe to write at 0x0": empty, larger than any
    // Canary's flash, or an app-only build with no partition table at 0x8000 are
    // all refused here, before espflash runs.
    check_local_image(&bytes)?;
    // Fit against the detected chip (the shape gate doesn't look at flash size):
    // bigger than this board can't be its image; smaller writes from 0x0 and
    // leaves the tail. Mirrors the browser's validateBackupFile.
    match rescue::validate_restore_image(bytes.len() as u64, flash_size) {
        Err(reason) => return Err(reason),
        Ok(Some(warn)) => {
            let _ = app.emit("rescue:log", warn);
        }
        Ok(None) => {}
    }
    if let Some(hint) = rescue::image_first_bytes_hint(&bytes) {
        let _ = app.emit("rescue:log", format!("→ this looks like {hint}"));
    }
    // Write the bytes we just validated — not the path. A sync tool or another
    // process could swap the file between the check above and espflash's own
    // read, slipping unvalidated bytes past the shape/size guards. Staging the
    // validated bytes to a private, RAII-removed temp file (alive until espflash
    // exits) closes that window, exactly as flash_local_file does.
    let staged = stage_firmware(&bytes, "rescue-restore")?;
    let staged_path = staged.path().to_string_lossy().to_string();
    let _ = app.emit("rescue:log", format!("→ writing {path} to the board…"));
    let code = run_sidecar_streaming(
        &app,
        rescue::write_bin_args(&port, &staged_path, baud),
        "rescue:log",
    )
    .await?;
    if code == 0 {
        let _ = app.emit(
            "rescue:log",
            "✓ written — the board is rebooting into it.".to_string(),
        );
        Ok(())
    } else {
        Err(format!(
            "espflash exited with code {code}. The board can't be bricked — put it in download mode and try again."
        ))
    }
}

/// Erase the whole chip — a truly clean slate before a fresh install.
#[tauri::command]
async fn erase_chip(app: AppHandle, port: String) -> Result<(), String> {
    let _ = app.emit("rescue:log", "→ erasing the whole chip…".to_string());
    let code = run_sidecar_streaming(&app, rescue::erase_flash_args(&port), "rescue:log").await?;
    if code == 0 {
        let _ = app.emit(
            "rescue:log",
            "✓ chip erased — factory-fresh. Flash any image next.".to_string(),
        );
        Ok(())
    } else {
        Err(format!(
            "espflash exited with code {code} while erasing. Nothing is bricked — try again."
        ))
    }
}

// ── the health check: read the board's story, change nothing ─────────────────
// Reads a handful of flash regions with the espflash sidecar and feeds their
// bytes to the pure `health` parsers (host-tested), then assembles the same
// report the browser Lab produces. Every read is to a private temp file we read
// back — `espflash read-flash` writes a file, not stdout.

/// Read one flash region into memory: a quiet sidecar run to a private 0600 temp
/// file (auto-removed), read back. Quiet on purpose — a health check does many
/// small reads, and streaming each one's progress bar would bury the console.
async fn read_region(
    app: &AppHandle,
    port: &str,
    offset: u32,
    size: u32,
    baud: u32,
) -> Result<Vec<u8>, String> {
    let tmp = tempfile::Builder::new()
        .prefix("securacv-health-")
        .suffix(".bin")
        .tempfile()
        .map_err(|e| format!("couldn't create a temp file for the read: {e}"))?;
    let out = tmp.path().to_string_lossy().to_string();
    let (code, log) = run_sidecar_capture(
        app,
        rescue::read_region_args(port, offset, size, &out, baud),
    )
    .await?;
    if code != 0 {
        return Err(format!(
            "espflash exited with code {code} reading 0x{offset:x}: {}",
            log.trim()
        ));
    }
    let data = std::fs::read(tmp.path())
        .map_err(|e| format!("couldn't read back region 0x{offset:x}: {e}"))?;
    // A region read can carry secrets — the NVS/settings partition holds the
    // Ed25519 private key and saved Wi-Fi. We parse them in memory with an
    // allow-list and never surface the values, but espflash had to write the raw
    // bytes to this temp file first. Overwrite it with zeros before it's removed
    // so nothing recoverable is left on the host disk.
    let _ = std::fs::write(tmp.path(), vec![0u8; data.len()]);
    Ok(data)
}

/// The checks a save path must pass before this app writes to it. The comment
/// on `save_text_file` used to be the only guard ("the save dialog already
/// blessed it") — but a comment binds nobody, and this command is reachable
/// straight from the webview, so an injected script could hand it any path at
/// all. Enforce the shape a save-dialog answer actually has: an absolute
/// path, into a directory that already exists (canonicalized, so `..` can't
/// dress one directory up as another), ending in the one extension the
/// export feature offers (`.json` — the health report). Returns the resolved
/// path to write.
/// A path the webview says came from the OS save panel: absolute, in a folder
/// that exists (canonicalized, so `..` never survives), with a file name and
/// the one extension this export writes. Validated, not trusted — a script
/// injected into the webview can hand a command any path at all.
fn validated_chosen_path(
    path: &str,
    ext: &str,
    wrong_ext: &str,
) -> Result<std::path::PathBuf, String> {
    let p = std::path::Path::new(path);
    if !p.is_absolute() {
        return Err("save path must be absolute — pick it in the save dialog".into());
    }
    let ext_ok = p
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case(ext));
    if !ext_ok {
        return Err(wrong_ext.into());
    }
    let name = p
        .file_name()
        .ok_or_else(|| "save path has no file name".to_string())?;
    let dir = p
        .parent()
        .ok_or_else(|| "save path has no parent directory".to_string())?
        .canonicalize()
        .map_err(|e| format!("save folder doesn't exist: {e}"))?;
    Ok(dir.join(name))
}

fn validated_save_path(path: &str) -> Result<std::path::PathBuf, String> {
    validated_chosen_path(path, "json", "this app only saves .json exports")
}

/// The full-flash backup is the identity key and the Wi-Fi secrets in
/// cleartext, written wherever the webview says — so it gets the same rule as
/// the JSON export, `.bin` only.
fn validated_backup_path(path: &str) -> Result<std::path::PathBuf, String> {
    validated_chosen_path(path, "bin", "a flash backup is saved as a .bin file")
}

/// Write UTF-8 text to a path the user just chose in the OS save panel — the
/// health report's JSON export. No FS plugin, no ambient access — and the
/// path is validated (`validated_save_path`), not merely trusted to have come
/// from the dialog.
#[tauri::command]
async fn save_text_file(path: String, contents: String) -> Result<(), String> {
    let out = validated_save_path(&path)?;
    std::fs::write(&out, contents).map_err(|e| format!("couldn't save {path}: {e}"))
}

fn verdict_json(v: &health::Verdict) -> Value {
    json!({
        "level": v.level,
        "headline": v.headline,
        "findings": v.findings.iter().map(|f| json!({
            "severity": f.severity, "title": f.title, "fix": f.fix,
        })).collect::<Vec<_>>(),
    })
}

/// Read the connected board's health: partition map, the firmware in each slot,
/// update history, crash dump, and the witness chain — read-only. Returns the
/// same report shape the browser renders, plus a self-heal verdict.
#[tauri::command]
async fn health_check(
    app: AppHandle,
    port: String,
    chip: String,
    mac: Option<String>,
    flash_bytes: Option<u64>,
    baud: u32,
) -> Result<Value, String> {
    let emit = |m: &str| {
        let _ = app.emit("health:log", m.to_string());
    };

    emit("→ reading the partition map…");
    let pt = read_region(&app, &port, 0x8000, 0xc00, baud).await?;
    let entries = health::parse_partition_table(&pt);
    if entries.is_empty() {
        let verdict = health::report_verdict(&health::VerdictInput {
            blank: true,
            ..Default::default()
        });
        emit("✓ read complete — the chip looks blank.");
        return Ok(json!({
            "chip": chip, "mac": mac, "flashBytes": flash_bytes,
            "blank": true, "partitions": [], "verdict": verdict_json(&verdict),
        }));
    }

    let partitions: Vec<Value> = entries
        .iter()
        .map(|e| {
            json!({
                "label": e.label,
                "kind": health::partition_kind(e.ptype, e.subtype),
                "offset": e.offset,
                "size": e.size,
            })
        })
        .collect();

    // App slots + their descriptors.
    let apps = health::app_partitions(&entries);
    let ota_slots: Vec<&health::Partition> = apps
        .iter()
        .filter(|a| (0x10..0x20).contains(&a.subtype))
        .collect();
    emit("→ reading the firmware in each slot…");
    let mut slots: Vec<Map<String, Value>> = Vec::new();
    for app_p in &apps {
        let desc = read_region(
            &app,
            &port,
            app_p.offset + health::APP_DESC_OFFSET,
            256,
            baud,
        )
        .await
        .ok()
        .and_then(|b| health::parse_app_descriptor(&b));
        let mut m = Map::new();
        let label = if app_p.label.is_empty() {
            health::partition_kind(app_p.ptype, app_p.subtype)
        } else {
            app_p.label.clone()
        };
        m.insert("label".into(), json!(label));
        m.insert("subtype".into(), json!(app_p.subtype));
        m.insert("empty".into(), json!(desc.is_none()));
        if let Some(d) = &desc {
            let built = format!("{} {}", d.date, d.time).trim().to_string();
            m.insert("project".into(), json!(d.project_name));
            m.insert("version".into(), json!(d.version));
            m.insert(
                "built".into(),
                if built.is_empty() {
                    Value::Null
                } else {
                    json!(built)
                },
            );
            m.insert("idf".into(), json!(d.idf_ver));
        }
        slots.push(m);
    }

    // otadata → which slot is booting, and how many updates it has seen.
    let mut ota_json = Value::Null;
    let mut active_label: Option<String> = None;
    if let Some(otap) = entries.iter().find(|e| health::is_ota_data(e)) {
        emit("→ reading update history…");
        let ob = read_region(&app, &port, otap.offset, otap.size.min(0x2000), baud).await?;
        let ota = health::parse_ota_data(&ob, ota_slots.len() as u32);
        if !ota.fresh {
            active_label = ota_slots
                .get(ota.active_ota as usize)
                .map(|s| s.label.clone());
        } else {
            let first = apps
                .iter()
                .find(|a| a.subtype == 0x00)
                .or_else(|| ota_slots.first().copied());
            active_label = first.map(|a| a.label.clone());
        }
        ota_json = json!({
            "fresh": ota.fresh, "activeOta": ota.active_ota, "updatesSeen": ota.updates_seen,
            "stateText": ota.state_text, "pendingVerify": ota.pending_verify,
        });
    }
    // No otadata partition at all → a factory-only layout; the ESP32 boots the
    // factory app, so mark it running (otherwise the verdict falsely warns that
    // nothing is bootable).
    if active_label.is_none() {
        active_label = apps
            .iter()
            .find(|a| a.subtype == 0x00)
            .map(|a| a.label.clone());
    }

    // Mark the booted slot.
    let mut has_running = false;
    if let Some(al) = &active_label {
        for m in slots.iter_mut() {
            if m.get("label").and_then(|v| v.as_str()) == Some(al.as_str()) {
                m.insert("active".into(), json!(true));
                has_running = true;
            }
        }
    }

    // Crash dump.
    let mut coredump_present = false;
    let coredump_json = if let Some(cd) = entries.iter().find(|e| health::is_coredump(e)) {
        let cb = read_region(&app, &port, cd.offset, 16, baud).await?;
        let c = health::parse_coredump_header(&cb, cd.size);
        coredump_present = c.present;
        json!({ "present": c.present, "size": c.size })
    } else {
        Value::Null
    };

    // Witness chain (NVS), presence-only for secrets.
    let mut witness_json = Value::Null;
    let mut tamper: Option<u64> = None;
    let mut provisioned = false;
    if let Some(nv) = entries.iter().find(|e| health::is_nvs(e)) {
        emit("→ reading the witness chain…");
        let nb = read_region(&app, &port, nv.offset, nv.size, baud).await?;
        let items = health::parse_nvs(&nb, &[health::WITNESS_CHAIN_BLOB_KEY]);
        if let Some(w) = health::witness_summary(&items) {
            tamper = w.tamper;
            provisioned = w.provisioned;
            witness_json = json!({
                "seq": w.seq, "boots": w.boots, "tamper": w.tamper, "logSeq": w.log_seq,
                "chainHeadFp": w.chain_head_fp, "provisioned": w.provisioned,
                "wifiConfigured": w.wifi_configured,
            });
        }
    }
    let witness_log_json = entries
        .iter()
        .find(|e| health::is_witness_log(e))
        .map(|e| json!({ "label": e.label, "size": e.size }))
        .unwrap_or(Value::Null);

    let rolled_back = ota_json
        .get("stateText")
        .and_then(|v| v.as_str())
        .map(|s| s.contains("rolled back"))
        .unwrap_or(false);
    let verdict = health::report_verdict(&health::VerdictInput {
        blank: false,
        coredump_present,
        ota_pending_verify: ota_json
            .get("pendingVerify")
            .and_then(|v| v.as_bool())
            .unwrap_or(false),
        ota_rolled_back: rolled_back,
        tamper,
        has_running_slot: has_running,
        provisioned,
    });

    emit("✓ health check complete.");
    Ok(json!({
        "chip": chip, "mac": mac, "flashBytes": flash_bytes,
        "partitions": partitions,
        "slots": slots.into_iter().map(Value::Object).collect::<Vec<_>>(),
        "ota": ota_json, "coredump": coredump_json,
        "witness": witness_json, "witnessLog": witness_log_json,
        "verdict": verdict_json(&verdict),
    }))
}

/// The board's passport: who it is and what it has lived through, read at
/// CONNECT time so the install verdict and the fleet book can both speak
/// before a byte is written.
///
/// Deliberately not `health_check`. That command reads a descriptor for
/// EVERY app slot plus the full partition story — the right depth for the
/// Advanced health report a user asks for, and far too slow to sit in the
/// path between plugging a board in and seeing what it is. This reads five
/// regions: the partition table, otadata, the booted slot's descriptor, the
/// coredump header, and NVS. Same parsers, same answers, a fraction of the
/// serial traffic.
///
/// Every probe past the partition table is best-effort: an old board, a
/// layout with no coredump region, or a flaky cable degrades a field to null
/// rather than failing the connect. A passport that cannot be read must never
/// be reported as a blank board — missing evidence is its own answer.
#[tauri::command]
async fn board_passport(app: AppHandle, port: String, baud: u32) -> Result<Value, String> {
    // Each region is its own espflash spawn (bootloader re-sync included), so
    // the whole read runs 8–25 s — long enough that a silent label reads as a
    // hang. `passport:log` narrates each step; the frontend shows the line in
    // the flash-target label so the wait is visibly moving, never a freeze.
    let narrate = |line: &str| {
        let _ = app.emit("passport:log", line.to_string());
    };
    narrate("reading the partition table…");
    let pt = read_region(&app, &port, 0x8000, 0xc00, baud).await?;
    let entries = health::parse_partition_table(&pt);
    if entries.is_empty() {
        // Read fine, found no table: a genuinely blank (or fully erased) chip.
        return Ok(json!({ "blank": true, "resident": Value::Null }));
    }

    let apps = health::app_partitions(&entries);
    let slots = health::ota_slots(&apps);

    // otadata first — it decides which slot's descriptor is the truth.
    let mut ota_json = Value::Null;
    let mut otadata: Option<health::OtaInfo> = None;
    if let Some(otap) = entries.iter().find(|e| health::is_ota_data(e)) {
        if !slots.is_empty() {
            narrate("reading its update history…");
            if let Ok(ob) = read_region(&app, &port, otap.offset, otap.size.min(0x2000), baud).await
            {
                let o = health::parse_ota_data(&ob, slots.len() as u32);
                ota_json = json!({
                    "fresh": o.fresh, "activeOta": o.active_ota, "updatesSeen": o.updates_seen,
                    "stateText": o.state_text, "pendingVerify": o.pending_verify,
                });
                otadata = Some(o);
            }
        }
    }

    // The firmware actually running, off the booted slot.
    let mut resident = Value::Null;
    if let Some(booted) = health::pick_booted_app_partition(&apps, otadata.as_ref()) {
        narrate("reading what it's running…");
        if let Ok(db) = read_region(
            &app,
            &port,
            booted.offset + health::APP_DESC_OFFSET,
            256,
            baud,
        )
        .await
        {
            if let Some(d) = health::parse_app_descriptor(&db) {
                let built = format!("{} {}", d.date, d.time).trim().to_string();
                resident = json!({
                    "version": d.version,
                    "projectName": d.project_name,
                    "built": if built.is_empty() { Value::Null } else { json!(built) },
                    "idf": d.idf_ver,
                    "slot": booted.label,
                });
            }
        }
    }

    // Crash record + witness counters — the passport's lived-history rows.
    let coredump_json = match entries.iter().find(|e| health::is_coredump(e)) {
        Some(cd) => match read_region(&app, &port, cd.offset, 16, baud).await {
            Ok(cb) => {
                let c = health::parse_coredump_header(&cb, cd.size);
                json!({ "present": c.present, "size": c.size })
            }
            Err(_) => Value::Null,
        },
        None => Value::Null,
    };

    let mut witness_json = Value::Null;
    if let Some(nv) = entries.iter().find(|e| health::is_nvs(e)) {
        // The NVS read is the passport's longest single pull.
        narrate("reading its witness counters…");
        if let Ok(nb) = read_region(&app, &port, nv.offset, nv.size, baud).await {
            let items = health::parse_nvs(&nb, &[health::WITNESS_CHAIN_BLOB_KEY]);
            if let Some(w) = health::witness_summary(&items) {
                witness_json = json!({
                    "seq": w.seq, "boots": w.boots, "tamper": w.tamper,
                    "provisioned": w.provisioned, "wifiConfigured": w.wifi_configured,
                });
            }
        }
    }

    Ok(json!({
        "blank": false,
        "resident": resident,
        "ota": ota_json,
        "coredump": coredump_json,
        "witness": witness_json,
    }))
}

/// Where a user gets a fresh copy of the app. The versioned `flasher-v*`
/// releases, not the rolling `flasher-latest` pointer — that one exists for
/// the updater and carries no installer (see `docs/RELEASE_BUTTONS.md`).
pub(crate) fn open_releases_page(app: &AppHandle) {
    let _ = app.opener().open_url(
        "https://github.com/kmay89/securaCV/releases?q=flasher-v&expanded=true",
        None::<&str>,
    );
}

/// The frontend finished `boot()` and the window is usable. This is the signal
/// the launch guard waits for: without it, a launch counts as one that never
/// arrived, and the next one repairs rather than repeating.
#[tauri::command]
fn ui_ready(guard: tauri::State<'_, Arc<launch_guard::LaunchGuard>>) {
    guard.note(launch_guard::Stage::UiReady);
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // Before Tauri builds anything. A launch that dies before its window
    // appears can't report itself from inside the app, so the record of the
    // *last* launch is read — and acted on — while there is still no webview
    // to be holding a wedged store open. See `launch_guard`.
    let guard = launch_guard::begin();
    let on_exit = Arc::clone(&guard);

    tauri::Builder::default()
        .manage(Arc::clone(&guard))
        .manage(serial_monitor::SerialMonitorState::default())
        .manage(we2_bench::We2BenchState::default())
        .manage(hub::PiUsbState::default())
        .manage(hub::HubFlashState::default())
        .manage(hub::HeadlessState::default())
        // On launch, reclaim anything a crash or pulled plug orphaned (a
        // ~2.5 GB raw staging image, a half-finished .partial download). Off
        // the main thread so it never delays the window. Orphaned *processes*
        // are already gone by now — the launch guard reaps those first, before
        // the UI enumerates ports and finds the board still held.
        .setup(move |app| {
            let handle = app.handle().clone();
            std::thread::spawn(move || hub::cleanup_orphans(&handle));
            guard.note(launch_guard::Stage::WindowUp);
            launch_guard::report(app.handle(), &guard);
            launch_guard::watch(app.handle(), &guard);
            Ok(())
        })
        // If the operator tries to quit while a hub flash is running, don't
        // just die mid-write. Hold the close, ask, and — if they mean it —
        // cancel the writer cleanly (the card is always re-flashable) before
        // exiting. A flash-free window closes instantly as usual.
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                // An update is mid-write over this very bundle. Closing now is
                // how you get an app that never opens again — nothing in the
                // bundle is guaranteed complete until the install returns. This
                // one is not negotiable, so there's no "quit anyway".
                let installing = window
                    .try_state::<Arc<launch_guard::LaunchGuard>>()
                    .map(|g| g.is_installing())
                    .unwrap_or(false);
                if installing {
                    api.prevent_close();
                    window
                        .dialog()
                        .message(
                            "An update is being written over the app right now. \
                             Quitting mid-write is the one thing that can leave the \
                             Flasher unable to open at all, so this has to finish — \
                             it takes a few seconds, then the app relaunches itself.",
                        )
                        .title("Finishing the update")
                        .buttons(MessageDialogButtons::OkCustom("OK".into()))
                        .show(|_| {});
                    return;
                }
                let running = window
                    .try_state::<hub::HubFlashState>()
                    .map(|s| s.is_running())
                    .unwrap_or(false);
                if running {
                    api.prevent_close();
                    let w = window.clone();
                    w.dialog()
                        .message(
                            "A hub flash is still running. Quitting now leaves the card \
                             unfinished — that's completely safe, you'd just flash it again. \
                             Quit anyway?",
                        )
                        .title("Quit while flashing?")
                        .buttons(MessageDialogButtons::OkCancelCustom(
                            "Quit".into(),
                            "Keep flashing".into(),
                        ))
                        .show(move |quit| {
                            if quit {
                                if let Some(s) = w.try_state::<hub::HubFlashState>() {
                                    s.cancel();
                                }
                                w.app_handle().exit(0);
                            }
                        });
                }
            }
        })
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_process::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .invoke_handler(tauri::generate_handler![
            load_catalog,
            load_hatch,
            app_info,
            current_ssid,
            saved_wifi_password,
            list_ports,
            detect_chip,
            fetch_manifest,
            witness_discover,
            flash,
            inspect_local_file,
            flash_local_file,
            flash_vision_module,
            hub::load_hub_catalog,
            hub::list_hub_targets,
            hub::hub_plan,
            hub::hub_flash,
            hub::hub_flash_cancel,
            hub::hub_preflight,
            hub::hub_probe_hub,
            hub::hub_onboard,
            hub::hub_headless_setup,
            hub::hub_headless_available,
            hub::hub_pi_boot_start,
            hub::hub_pi_boot_stop,
            serial_monitor::start_serial_monitor,
            serial_monitor::serial_monitor_send,
            serial_monitor::stop_serial_monitor,
            we2_bench::we2_bench_start,
            we2_bench::we2_bench_cmd,
            we2_bench::we2_bench_stop,
            fleet::fleet_scan,
            fleet::fleet_device_call,
            fleet::device_whoami,
            secret_store::secret_backend,
            secret_store::secret_set,
            secret_store::secret_get,
            secret_store::secret_delete,
            auto_backup_path,
            backup_flash,
            write_local_image,
            erase_chip,
            health_check,
            board_passport,
            save_text_file,
            check_update,
            install_update,
            ui_ready
        ])
        .build(tauri::generate_context!())
        .expect("error while building SecuraCV Flasher")
        .run(move |app, event| match event {
            // Cmd-Q and the app menu's Quit do NOT go through a window's
            // CloseRequested — they request an application exit directly, so
            // the close guard above never sees them. This is the only place
            // that can stop them, and `Exit` is already too late to try.
            //
            // Refusing outright, with no "quit anyway": every other guard in
            // this app is advisory because the thing at risk is re-doable (a
            // half-flashed board is re-flashed, a half-written card rewritten).
            // A half-moved app bundle is the one exception — it can leave the
            // Flasher unable to open at all, and nothing inside it can undo
            // that. The install takes seconds and relaunches itself.
            tauri::RunEvent::ExitRequested { api, .. } if on_exit.is_installing() => {
                api.prevent_exit();
                on_exit.log_quit_blocked();
                app.dialog()
                    .message(
                        "An update is being written over the app right now. \
                         Quitting mid-write is the one thing that can leave the \
                         Flasher unable to open at all, so this has to finish — \
                         it takes a few seconds, then the app relaunches itself.",
                    )
                    .title("Finishing the update")
                    .buttons(MessageDialogButtons::OkCustom("OK".into()))
                    .show(|_| {});
            }
            // A clean exit is what makes the next launch silent. Everything
            // else — force quit, crash, power loss — leaves the record showing
            // where this run got to, which is exactly what the next one reads.
            tauri::RunEvent::Exit => on_exit.mark_clean(),
            _ => {}
        });
}

#[cfg(test)]
mod catalog_derivation_tests {
    use super::{bundled_catalog, release_origin, EMBEDDED_CATALOG};
    use serde_json::Value;

    // The derivations themselves (chip table, release origin, the manifest
    // allow-list) are the flash engine's and are pinned by its own tests
    // against this same catalog file (desktop/flash-engine/src/catalog.rs).
    // What this app owns is the wiring: bundled_catalog() must wrap the catalog
    // build.rs embedded, not some other text.

    #[test]
    fn the_engine_catalog_wraps_the_embedded_catalog() {
        assert_eq!(bundled_catalog().raw(), EMBEDDED_CATALOG);
        let embedded: Value = serde_json::from_str(EMBEDDED_CATALOG).unwrap();
        let chips = embedded["chips"].as_object().expect("catalog has chips");
        assert_eq!(bundled_catalog().chip_table().len(), chips.len());
        let manifest_url = embedded["manifest_url"].as_str().unwrap();
        let origin = release_origin().expect("catalog carries a releases/download manifest_url");
        assert!(manifest_url.starts_with(origin));
        assert!(bundled_catalog().manifest_url_allowed(manifest_url));
        assert!(
            bundled_catalog().manifest_url_allowed(flash_engine::catalog::DEV_FLASH_MANIFEST_URL)
        );
    }

    #[test]
    fn we2_usb_identity_derives_from_the_catalog() {
        let catalog: Value = serde_json::from_str(EMBEDDED_CATALOG).unwrap();
        let module = &catalog["we2_module"];
        let vid = u16::from_str_radix(
            module["usb_vid"].as_str().unwrap().trim_start_matches("0x"),
            16,
        )
        .unwrap();
        let pid = u16::from_str_radix(
            module["usb_pid"].as_str().unwrap().trim_start_matches("0x"),
            16,
        )
        .unwrap();
        assert!(crate::we2::is_module_usb(Some(vid), Some(pid)));
        assert!(!crate::we2::is_module_usb(Some(vid), Some(pid ^ 1)));
        assert!(!crate::we2::is_module_usb(None, None));
    }
}

#[cfg(test)]
mod webview_boundary_tests {
    use super::{validated_backup_path, validated_save_path};

    #[test]
    fn backup_paths_are_validated_not_trusted() {
        // The backup is the identity key and Wi-Fi secrets in cleartext, so a
        // webview-supplied destination gets exactly the JSON export's rules.
        let dir = tempfile::tempdir().expect("tempdir");
        let good = dir.path().join("canary-backup.bin");
        let resolved = validated_backup_path(good.to_str().unwrap()).expect("a dialog-shaped path");
        assert_eq!(resolved.file_name().unwrap(), "canary-backup.bin");

        assert!(validated_backup_path("backup.bin").is_err(), "relative");
        assert!(
            validated_backup_path(dir.path().join("backup.json").to_str().unwrap()).is_err(),
            "only .bin"
        );
        assert!(
            validated_backup_path(dir.path().join("no-such-dir/backup.bin").to_str().unwrap())
                .is_err(),
            "missing folder"
        );
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let sneaky = dir.path().join("sub/../canary-backup.bin");
        let resolved = validated_backup_path(sneaky.to_str().unwrap()).unwrap();
        assert!(!resolved.to_string_lossy().contains(".."));
        assert_eq!(
            resolved.parent().unwrap(),
            dir.path().canonicalize().unwrap()
        );
    }

    #[test]
    fn save_paths_are_validated_not_trusted() {
        let dir = tempfile::tempdir().expect("tempdir");
        let good = dir.path().join("canary-report.json");
        let resolved = validated_save_path(good.to_str().unwrap()).expect("a dialog-shaped path");
        assert_eq!(resolved.file_name().unwrap(), "canary-report.json");

        // Relative paths, non-.json targets, and missing folders are refused.
        assert!(validated_save_path("report.json").is_err());
        assert!(validated_save_path(dir.path().join("evil.sh").to_str().unwrap()).is_err());
        assert!(
            validated_save_path(dir.path().join("no-such-dir/report.json").to_str().unwrap())
                .is_err()
        );

        // `..` segments are resolved away, never written through blindly.
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let sneaky = dir.path().join("sub/../canary-report.json");
        let resolved = validated_save_path(sneaky.to_str().unwrap()).expect("canonicalized");
        assert_eq!(
            resolved,
            dir.path()
                .canonicalize()
                .unwrap()
                .join("canary-report.json")
        );
    }
}
