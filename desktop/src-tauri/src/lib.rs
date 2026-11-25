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

mod efuse;
mod fleet;
mod host;
mod hub;
mod launch_guard;
mod seal;
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
use flash_engine::image::{check_bootloader_offset, check_local_image, stage_firmware};
use flash_engine::ports::PortDto;
use flash_engine::provisioning::Provisioning;
use flash_engine::{health, port_hint, release, rescue};
use host::TauriHost;
use serde::Serialize;
use serde_json::{json, Map, Value};
use std::collections::HashMap;
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
            // Absolute: a helper spawned by bare name is found through PATH,
            // which the launching shell owns. Apple ships this one here.
            if let Ok(out) = std::process::Command::new("/usr/sbin/networksetup")
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
        // Bare names on purpose: iwgetid and nmcli live in /sbin, /usr/sbin
        // or /usr/bin depending on the distro, so PATH is the only portable
        // lookup. Neither carries a secret; current_ssid is a prefill.
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
        // Absolute (see current_ssid): this one hands back a password, so
        // it must be Apple's binary and not whatever PATH finds first.
        let out = std::process::Command::new("/usr/bin/security")
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
        // Bare name: nmcli's directory differs across distros (see
        // current_ssid), so PATH is the only portable lookup.
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
    for base in &bases {
        // One pinned client per base (fleet.rs pinned_client): the name is
        // resolved, every address judged local, and the connection pinned to
        // those addresses. A name that resolves off the LAN is skipped like
        // any other candidate that failed the gate.
        let Ok(client) =
            fleet::pinned_client(base, "SecuraCV-Flasher", std::time::Duration::from_secs(2)).await
        else {
            continue;
        };
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
/// SHA-256 fingerprint, and whether the file carries 0xE9 — the ESP32's own
/// "program starts here" marker — where a merged factory image keeps its
/// bootloader: at 0x0 on the S3/C3/C6, at 0x1000 on a classic ESP32 (whose
/// ROM loads it from there; the first 4 KB of its factory image are erased
/// flash). Here a missing magic only informs, since the chip may not be read
/// yet; the write (flash_local_file) refuses an image with no bootloader
/// where the connected chip's ROM loads it.
#[derive(Serialize)]
pub struct LocalFileInfo {
    size: u64,
    sha256: String,
    esp_magic: bool,
}

// ── files the webview may name: by handle, never by path ───────────────────
// inspect_local_file, flash_local_file and write_local_image used to take any
// absolute path the webview sent, and a script injected into the webview has
// the same `invoke` the app's own code has. Now the OS open panel is raised
// from HERE (pick_local_file), the path it answers is validated and kept in
// a per-session map, and the three commands take the opaque handle — so the
// webview can name a file the user just chose, and nothing else.

/// The files picked through the OS open panel this session, by handle.
#[derive(Default)]
pub struct ChosenFiles(std::sync::Mutex<HashMap<String, std::path::PathBuf>>);

/// What the picker hands the UI: the handle to pass back, the name to show.
#[derive(Serialize)]
pub struct ChosenFile {
    handle: String,
    name: String,
}

/// The checks a picked path passes before a handle is minted for it: it
/// resolves (symlinks followed, so a link can't dress a device or a
/// directory up as a file) to a regular file, and that file is not under
/// the app's own data dir — the staged images, the safety-copy backups,
/// the launch record and the maintenance key all live there, and none of
/// them is what "pick a file" means. Pure over two paths, so it is tested.
pub(crate) fn chosen_file_ok(
    path: &std::path::Path,
    app_data: Option<&std::path::Path>,
) -> Result<std::path::PathBuf, String> {
    if !path.is_absolute() {
        return Err("the chosen path must be absolute".into());
    }
    let resolved = path
        .canonicalize()
        .map_err(|e| format!("couldn't open that file: {e}"))?;
    let meta = std::fs::metadata(&resolved).map_err(|e| format!("couldn't read that file: {e}"))?;
    if !meta.is_file() {
        return Err("that isn't a regular file".into());
    }
    if let Some(data) = app_data.and_then(|d| d.canonicalize().ok()) {
        if resolved.starts_with(&data) {
            return Err(
                "that file belongs to this app's own data folder — pick one of yours".into(),
            );
        }
    }
    Ok(resolved)
}

/// Raise the OS open panel for a `.bin` and hand back a handle for it. `kind`
/// picks the panel's wording: `factory` (the Advanced local-file install) or
/// `restore` (the rescue bench's write). `None` when the user canceled.
#[tauri::command]
async fn pick_local_file(
    app: AppHandle,
    files: tauri::State<'_, ChosenFiles>,
    kind: String,
) -> Result<Option<ChosenFile>, String> {
    let (title, filter) = match kind.as_str() {
        "factory" => ("Choose a factory firmware image", "Factory firmware image"),
        "restore" => (
            "Choose a firmware image or backup",
            "Firmware image or backup",
        ),
        _ => return Err(format!("unknown file picker {kind:?}")),
    };
    let app_data = app.path().app_data_dir().ok();
    let picker = app.clone();
    let picked = tauri::async_runtime::spawn_blocking(move || {
        picker
            .dialog()
            .file()
            .set_title(title)
            .add_filter(filter, &["bin"])
            .blocking_pick_file()
    })
    .await
    .map_err(|e| format!("couldn't open the file panel: {e}"))?;
    let Some(picked) = picked else {
        return Ok(None);
    };
    let path = picked
        .into_path()
        .map_err(|e| format!("the panel answered something that isn't a file path: {e}"))?;
    let resolved = chosen_file_ok(&path, app_data.as_deref())?;
    let name = resolved
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "local file".to_string());
    let mut raw = [0u8; 16];
    getrandom::fill(&mut raw).map_err(|e| format!("no system randomness: {e}"))?;
    let handle: String = raw.iter().map(|b| format!("{b:02x}")).collect();
    files
        .0
        .lock()
        .map_err(|_| "chosen-file table poisoned")?
        .insert(handle.clone(), resolved);
    Ok(Some(ChosenFile { handle, name }))
}

/// The path behind a handle the picker minted this session.
fn chosen_path(files: &ChosenFiles, handle: &str) -> Result<std::path::PathBuf, String> {
    files
        .0
        .lock()
        .map_err(|_| "chosen-file table poisoned")?
        .get(handle)
        .cloned()
        .ok_or_else(|| {
            "pick the file again — this app only writes a file chosen in its own panel".to_string()
        })
}

#[tauri::command]
async fn inspect_local_file(
    files: tauri::State<'_, ChosenFiles>,
    handle: String,
) -> Result<LocalFileInfo, String> {
    let path = chosen_path(&files, &handle)?;
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read that file: {e}"))?;
    check_local_image(&bytes)?;
    Ok(LocalFileInfo {
        size: bytes.len() as u64,
        sha256: release::sha256_hex(&bytes),
        esp_magic: bytes.first() == Some(&0xE9) || bytes.get(0x1000) == Some(&0xE9),
    })
}

/// Flash a firmware file straight off this computer's disk (Advanced). No
/// catalog product, no manifest, no signature — a personal file has no origin
/// we can verify, so nothing here claims "verified": the file is fingerprinted
/// (SHA-256 + size) so the receipt names exactly what was written, and that is
/// all the receipt claims. `expected_size`/`expected_sha256` are the values
/// inspect_local_file showed and the user confirmed — the file is re-read and
/// re-hashed here, so a file that changed on disk between the two calls is
/// refused rather than silently written. `handle` names a file the OS panel
/// answered (pick_local_file) — never a path. `detected_chip` is the chip the
/// frontend read: the image must carry that chip's bootloader where its ROM
/// loads it (check_bootloader_offset, the catalog path's gate too). The write
/// mechanics are identical to flash(): private staged temp file,
/// `write-bin 0x0` via the bundled espflash, `flash:log` streaming.
// The argument list is the frontend's invoke contract; the file handle made
// it eight.
#[allow(clippy::too_many_arguments)]
#[tauri::command]
async fn flash_local_file(
    app: AppHandle,
    files: tauri::State<'_, ChosenFiles>,
    port: String,
    baud: u32,
    handle: String,
    expected_size: u64,
    expected_sha256: String,
    detected_chip: String,
) -> Result<FlashReceipt, String> {
    let emit = |app: &AppHandle, line: String| {
        let _ = app.emit("flash:log", line);
    };

    let path = chosen_path(&files, &handle)?;
    let file_name = path
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "local file".to_string());
    emit(&app, format!("→ reading {file_name} from this computer…"));
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read that file: {e}"))?;
    check_local_image(&bytes)?;
    check_bootloader_offset(&bytes, &detected_chip)?;
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
    // as the release path — marked as a board write for the quit guard.
    let _writing = flash_engine::flash::BOARD_WRITES.begin();
    let (code, tail) =
        flash_engine::host::run_streaming_with_tail(&TauriHost(app.clone()), args, "flash:log")
            .await?;
    // (`staged` removes the private image on scope exit.)

    if code == 0 {
        emit(
            &app,
            "✓ chip write confirmed — your file is on the board.".into(),
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

/// The most a Vision model manifest may declare, and the most this app will
/// hold of one. The manifest's `size` used to go straight into
/// `Vec::with_capacity` and the download loop read whatever the server sent,
/// both before the size and SHA-256 were checked — so a manifest naming a
/// huge size (or an asset far larger than it said) could take the app down
/// or fill its memory. A cap, not a measurement of the module's flash.
const VISION_MODEL_MAX_BYTES: u64 = 16 * 1024 * 1024;

/// The Vision model's declared size, refused when it is past
/// [`VISION_MODEL_MAX_BYTES`]. Pure, so the bound is tested.
fn vision_model_size_ok(size: u64) -> Result<u64, String> {
    if size > VISION_MODEL_MAX_BYTES {
        return Err(format!(
            "Vision model manifest declares an implausible size ({size} bytes; the limit is {VISION_MODEL_MAX_BYTES})"
        ));
    }
    Ok(size)
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
    let expected_size = vision_model_size_ok(expected_size)?;
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
        // Never hold more than the manifest declared (itself capped above):
        // a body that runs past its own size is not the model, and reading
        // on would only fill memory before the size check refused it.
        if bytes.len() as u64 > expected_size {
            return Err(format!(
                "Vision model download exceeded the declared size ({expected_size} bytes)"
            ));
        }
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
    // "Verified" only when the Ed25519 signature was checked (AGENTS.md
    // rule 4); checksum-only means the SHA-256 matched the manifest, no more.
    emit_log(if model_verification == "ed25519+sha256" {
        format!(
            "✓ model verified: {} bytes · SHA-256 {}… ({model_verification})",
            bytes.len(),
            &sha[..16]
        )
    } else {
        format!(
            "✓ model checked: {} bytes · SHA-256 {}… ({model_verification} — no release key is pinned, so no signature was checked)",
            bytes.len(),
            &sha[..16]
        )
    });

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
/// created on demand, named by the local date and a counter — never by
/// anything of the board's (the MAC's tail used to name it, and the base MAC
/// is the station address in every Wi-Fi frame). The frontend can't know the
/// app-data dir, and the browser flasher's equivalent (an unasked download
/// before every write) is the parity bar this path exists to meet — the
/// reflash-with-no-undo gap was desktop-only. `day` is the frontend's local
/// `YYYY-MM-DD` (Rust std has no local zone); the name is the engine's
/// `rescue::next_backup_filename` — the browser's `canary-…-backup.bin`
/// scheme, with the next free counter because every copy shares this folder.
///
/// The folder is owner-only (`owner_only_dir`): every copy in it is a board's
/// identity key and Wi-Fi in the clear.
///
/// Not a command: this used to answer the webview, which handed the string
/// straight back to `backup_flash` as its write target — so the "automatic"
/// destination was whatever a script in the webview said it was.
/// `backup_flash` now calls this itself (`dest: "auto"`), and `day` only ever
/// reaches a file NAME, which the engine's helper refuses unless it is a date.
fn auto_backup_path(app: &AppHandle, day: &str) -> Result<std::path::PathBuf, String> {
    let dir = backups_dir(app)?;
    owner_only_dir(&dir)
        .map_err(|e| format!("couldn't make the backups folder private to this account: {e}"))?;
    let taken: Vec<String> = std::fs::read_dir(&dir)
        .map_err(|e| format!("couldn't read the backups dir: {e}"))?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    let name = rescue::next_backup_filename(day, &taken)
        .ok_or_else(|| format!("not a date the backup can be named by: {day:?}"))?;
    Ok(dir.join(name))
}

/// The folder every automatic safety copy goes in.
fn backups_dir(app: &AppHandle) -> Result<std::path::PathBuf, String> {
    Ok(app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data dir: {e}"))?
        .join("backups"))
}

/// At every launch, close a `backups/` folder an older build left open (0755,
/// with 0644 copies in it): the folder to 0700 and each copy in it to 0600,
/// whether or not this run ever takes a safety copy. `auto_backup_path`
/// tightens the folder too, but only an install that takes a safety copy
/// calls it, so someone who turned the copy off, or saves backups only from
/// the Rescue bench, would otherwise keep the open folder. Only a folder that
/// is already there is touched, and only the regular files in it (a link is
/// not followed). Best-effort and quiet, like the launch's other sweeps.
/// Elsewhere it does nothing.
fn close_backups_folder(backups: &std::path::Path) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if !backups.is_dir() {
            return;
        }
        let _ = std::fs::set_permissions(backups, std::fs::Permissions::from_mode(0o700));
        if let Ok(entries) = std::fs::read_dir(backups) {
            for e in entries.flatten() {
                if e.file_type().is_ok_and(|t| t.is_file()) {
                    let _ =
                        std::fs::set_permissions(e.path(), std::fs::Permissions::from_mode(0o600));
                }
            }
        }
    }
    #[cfg(not(unix))]
    {
        let _ = backups;
    }
}

/// Make `dir`, and any parent it is missing, a folder only this account can
/// open: 0700 on Unix (a parent this creates gets 0700 too, as XDG asks of a
/// data folder). The mode is set again on every call, so a `backups/` an
/// older build made 0755 is tightened as well, which also puts the copies
/// already in it out of other accounts' reach. Elsewhere it is a plain
/// create (Windows is neither built nor tested).
fn owner_only_dir(dir: &std::path::Path) -> std::io::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
        std::fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(dir)?;
        std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700))
    }
    #[cfg(not(unix))]
    {
        std::fs::create_dir_all(dir)
    }
}

/// How `owner_only_file` left a backup's destination.
struct BackupFile {
    /// No file was there, so it made one: a read that fails may remove it.
    created: bool,
    /// Another account can still read it after the tightening: the drive's
    /// file system keeps no per-file modes (a FAT or exFAT stick, whose mode
    /// comes from the mount) or another account owns the file.
    readable_by_others: bool,
}

/// Get `path` ready for a full-chip dump before espflash opens it, so the
/// dump is never readable by another account, even for a moment: espflash
/// 3.3.0's `read-flash` opens its output create+truncate with no mode of its
/// own, which keeps the mode of a file that is already there (and would make
/// a new one `0666 & !umask`). On Unix a new file is created exclusively at
/// 0600; an existing one (a Rescue-bench save over an earlier copy) is
/// tightened to 0600 but not truncated, so a read that fails before espflash
/// gets to it still leaves the earlier copy whole. Elsewhere nothing is done
/// and nothing is judged.
fn owner_only_file(path: &std::path::Path) -> std::io::Result<BackupFile> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
        let (file, created) = match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(path)
        {
            Ok(file) => (file, true),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                (std::fs::OpenOptions::new().write(true).open(path)?, false)
            }
            Err(e) => return Err(e),
        };
        // A refusal here (no per-file modes, or not our file) is judged by
        // the mode the file is left with, just below.
        let _ = file.set_permissions(std::fs::Permissions::from_mode(0o600));
        let mode = file.metadata()?.permissions().mode();
        Ok(BackupFile {
            created,
            readable_by_others: mode & 0o077 != 0,
        })
    }
    #[cfg(not(unix))]
    {
        let _ = path;
        Ok(BackupFile {
            created: false,
            readable_by_others: false,
        })
    }
}

/// After a read that failed (espflash exited non-zero, or never started),
/// remove the file `owner_only_file` made for it: espflash writes the dump in
/// one go at the end of a good read, so that file is empty or a fragment, not
/// a copy of anything. A file that was already there is left as espflash left
/// it (untouched if the read failed before espflash opened it, emptied if it
/// failed after), the same as before the file was made ahead of the run.
fn discard_failed_copy(prepared: &BackupFile, ran: &Result<i32, String>, out: &std::path::Path) {
    if prepared.created && !matches!(ran, Ok(0)) {
        let _ = std::fs::remove_file(out);
    }
}

/// Raise the OS save panel from HERE and answer the path it gave, validated
/// by `validate` (`validated_backup_path` / `validated_save_path`) — or
/// `None` when the user canceled. The save twin of `pick_local_file`: the two
/// commands that write a file for the user (`backup_flash`,
/// `save_text_file`) used to take the destination from the webview, and a
/// script injected there has the same `invoke` the app's own code has. Now
/// neither takes a path at all; the only destination is one a person just
/// chose in a panel this process raised.
async fn ask_save_path(
    app: &AppHandle,
    title: &'static str,
    file_name: String,
    filter: &'static str,
    ext: &'static str,
    validate: fn(&std::path::Path) -> Result<std::path::PathBuf, String>,
) -> Result<Option<std::path::PathBuf>, String> {
    let picker = app.clone();
    let picked = tauri::async_runtime::spawn_blocking(move || {
        picker
            .dialog()
            .file()
            .set_title(title)
            .set_file_name(file_name)
            .add_filter(filter, &[ext])
            .blocking_save_file()
    })
    .await
    .map_err(|e| format!("couldn't open the save panel: {e}"))?;
    let Some(picked) = picked else {
        return Ok(None);
    };
    let path = picked
        .into_path()
        .map_err(|e| format!("the panel answered something that isn't a file path: {e}"))?;
    validate(&path).map(Some)
}

/// Back up the whole chip — a full-flash read the operator keeps and can
/// restore later. The safety copy the one-shot flow never had.
///
/// There is no path argument. `dest` names one of two destinations this
/// process works out for itself: `"ask"` raises the OS save panel (the rescue
/// bench's Back up button) and `"auto"` is the app's own backups folder (the
/// safety copy before an install; `auto_backup_path`). `day` is the page's
/// local date, which names the automatic copy; `name` is the file name the
/// panel opens on (`suggested_file_name` holds it to one plain `.bin` name).
/// Neither is a path. Answers the path written, for display and for the
/// change map, or `None` when the user canceled the panel — nothing is read
/// off the board in that case. The file is made owner-only before espflash
/// writes it (`owner_only_file`), whether it is the automatic copy or the
/// file chosen in the save panel.
#[tauri::command]
async fn backup_flash(
    app: AppHandle,
    port: String,
    flash_size: u64,
    baud: u32,
    day: String,
    name: String,
    dest: String,
) -> Result<Option<String>, String> {
    if flash_size == 0 {
        return Err("couldn't read this chip's flash size — reconnect and try again".into());
    }
    let out = match dest.as_str() {
        "auto" => validated_backup_path(&auto_backup_path(&app, &day)?)?,
        "ask" => {
            let asked = ask_save_path(
                &app,
                "Save the flash backup",
                suggested_file_name(&name, "bin")?,
                "Flash backup",
                "bin",
                validated_backup_path,
            )
            .await?;
            match asked {
                Some(path) => path,
                None => return Ok(None),
            }
        }
        _ => return Err(format!("unknown backup destination {dest:?}")),
    };
    let prepared = owner_only_file(&out)
        .map_err(|e| format!("couldn't create the backup file {}: {e}", out.display()))?;
    let out_path = out.to_string_lossy().into_owned();
    if prepared.readable_by_others {
        let _ = app.emit(
            "rescue:log",
            format!(
                "⚠ {out_path} can't be kept to your account alone: this drive keeps no \
                 per-file permissions (a FAT or exFAT stick), or the file belongs to another \
                 account. The copy holds the board's identity key and Wi-Fi, so keep the drive \
                 like the board itself."
            ),
        );
    }
    let _ = app.emit(
        "rescue:log",
        format!(
            "→ reading {} of flash → {out_path}…",
            rescue::human_bytes(flash_size)
        ),
    );
    let ran = run_sidecar_streaming(
        &app,
        rescue::read_flash_args(&port, flash_size, &out_path, baud),
        "rescue:log",
    )
    .await;
    discard_failed_copy(&prepared, &ran, &out);
    let code = ran?;
    if code == 0 {
        let _ = app.emit(
            "rescue:log",
            "✓ backup saved — keep it safe; restore it any time.".to_string(),
        );
        Ok(Some(out_path))
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
/// `handle` names a file the OS panel answered (pick_local_file), never a path.
///
/// `chip` is the chip the frontend read. Every other write at 0x0 — the
/// catalog install and the Advanced local file — refuses an image whose
/// bootloader is not where that chip's ROM loads it
/// (`check_bootloader_offset`); the restore was the one path that skipped it,
/// so a backup taken from a classic ESP32 (bootloader at 0x1000) went onto an
/// S3 as "a smaller file" and the S3 did not start. Same guard, same words,
/// and the browser's restore (flash.js onRestoreFile) runs its twin.
#[tauri::command]
async fn write_local_image(
    app: AppHandle,
    files: tauri::State<'_, ChosenFiles>,
    port: String,
    handle: String,
    flash_size: Option<u64>,
    baud: u32,
    chip: String,
) -> Result<(), String> {
    let path = chosen_path(&files, &handle)?;
    let path = path.to_string_lossy().to_string();
    // A firmware image is at most one chip's flash (≤ 32 MiB), so read it once —
    // the same bounded read `flash_local_file` does — and run the offset-0 guard
    // before anything is written. espflash re-reads the path when it writes.
    let bytes = std::fs::read(&path).map_err(|e| format!("couldn't read {path}: {e}"))?;
    // Single source of truth for "safe to write at 0x0": empty, larger than any
    // Canary's flash, or an app-only build with no partition table at 0x8000 are
    // all refused here, before espflash runs.
    check_local_image(&bytes)?;
    // …and so is an image built for another chip family, or one whose
    // bootloader is not where this chip's ROM loads it.
    check_bootloader_offset(&bytes, &chip)?;
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
    let _writing = flash_engine::flash::BOARD_WRITES.begin();
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
    let _writing = flash_engine::flash::BOARD_WRITES.begin();
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
        return Err(rescue::read_failure(code, offset, &log));
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

/// The checks a save destination passes before this app writes to it, even
/// though the path now comes from a panel this process raised (`ask_save_path`)
/// or from its own backups folder — never from the webview. It has the shape
/// a save-panel answer actually has: absolute, in a folder that exists
/// (canonicalized, so `..` never survives and a linked folder is judged by
/// where it leads), with a file name and the one extension this export
/// writes. And if something already sits at that name it must be a regular
/// file: a symlink there is refused rather than written through (the write
/// would land wherever it points), and so is a folder, a FIFO or a device.
/// `symlink_metadata` on purpose — it judges the name itself, not its target.
/// Returns the resolved path to write.
fn validated_chosen_path(
    p: &std::path::Path,
    ext: &str,
    wrong_ext: &str,
) -> Result<std::path::PathBuf, String> {
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
    let out = dir.join(name);
    match std::fs::symlink_metadata(&out) {
        Ok(meta) if !meta.file_type().is_file() => Err(
            "something that isn't a regular file already has that name (a link, a folder or \
             a device) — choose another name"
                .into(),
        ),
        _ => Ok(out),
    }
}

fn validated_save_path(path: &std::path::Path) -> Result<std::path::PathBuf, String> {
    validated_chosen_path(path, "json", "this app only saves .json exports")
}

/// The full-flash backup is the identity key and the Wi-Fi secrets in
/// cleartext — so its destination gets the same rule as the JSON export,
/// `.bin` only.
fn validated_backup_path(path: &std::path::Path) -> Result<std::path::PathBuf, String> {
    validated_chosen_path(path, "bin", "a flash backup is saved as a .bin file")
}

/// The file name a save panel opens on. The page suggests it (the browser
/// flasher's own neutral names: the local date and a counter, never the
/// board's address), and a suggestion is all it is: a person sees it in the
/// panel and answers. Held to one plain file name with the expected ending,
/// so nothing the page sends can carry a directory or a parent step into the
/// panel's starting point.
fn suggested_file_name(name: &str, ext: &str) -> Result<String, String> {
    let ending = format!(".{ext}");
    let plain = name.len() <= 80
        && !name.starts_with('.')
        && !name.contains("..")
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'));
    if plain && name.len() > ending.len() && name.ends_with(&ending) {
        Ok(name.to_string())
    } else {
        Err(format!("not a file name this app suggests: {name:?}"))
    }
}

/// Save UTF-8 text — the health report's JSON export — to a file the user
/// chooses in the OS save panel, which is raised from HERE (`ask_save_path`).
/// No path argument, no FS plugin, no ambient access: a script in the webview
/// can ask for the panel and suggest a name (`suggested_file_name`), and only
/// a person can answer it. `.json` is a
/// config format other tools execute (editor settings, package manifests), so
/// "any .json path the webview names" was a write primitive worth closing.
/// `true` when the file was written, `false` when the user canceled.
#[tauri::command]
async fn save_text_file(app: AppHandle, name: String, contents: String) -> Result<bool, String> {
    let asked = ask_save_path(
        &app,
        "Save the health report",
        suggested_file_name(&name, "json")?,
        "Health report",
        "json",
        validated_save_path,
    )
    .await?;
    let Some(out) = asked else {
        return Ok(false);
    };
    std::fs::write(&out, contents).map_err(|e| format!("couldn't save {}: {e}", out.display()))?;
    Ok(true)
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
///
/// The report carries no hardware address: the frontend shows the board's
/// behind a reveal on screen and saves none of it (app.js onSaveHealthReport).
#[tauri::command]
async fn health_check(
    app: AppHandle,
    port: String,
    chip: String,
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
            "chip": chip, "flashBytes": flash_bytes,
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
        "chip": chip, "flashBytes": flash_bytes,
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
///
/// `chip` (detect_chip's canonical name) places the bootloader's image
/// header for the About-this-board rows: 32 bytes at 0x0, or 0x1000 on the
/// classic ESP32. `bootloader` is null when no image is there and `false`
/// when the read itself failed — two answers the rows keep apart.
#[tauri::command]
async fn board_passport(
    app: AppHandle,
    port: String,
    chip: Option<String>,
    baud: u32,
) -> Result<Value, String> {
    // Each region is its own espflash spawn (bootloader re-sync included), so
    // the whole read runs 8–25 s — long enough that a silent label reads as a
    // hang. `passport:log` narrates each step; the frontend shows the line in
    // the flash-target label so the wait is visibly moving, never a freeze.
    let narrate = |line: &str| {
        let _ = app.emit("passport:log", line.to_string());
    };
    narrate("reading the bootloader's image header…");
    let chip_name = chip.unwrap_or_default();
    let bootloader = match read_region(
        &app,
        &port,
        health::bootloader_offset(&chip_name),
        0x20,
        baud,
    )
    .await
    {
        Ok(hb) => match health::parse_bootloader_header(&hb, &chip_name) {
            Some(h) => json!({
                "flashMode": h.flash_mode, "flashMhz": h.flash_mhz,
                "flashMhzEither": h.flash_mhz_either, "flashBytes": h.flash_bytes,
                "codes": { "mode": h.mode_code, "size": h.size_code, "freq": h.freq_code },
            }),
            None => Value::Null,
        },
        Err(_) => json!(false),
    };
    narrate("reading the partition table…");
    let pt = read_region(&app, &port, 0x8000, 0xc00, baud).await?;
    let entries = health::parse_partition_table(&pt);
    if entries.is_empty() {
        // Read fine, found no table: a genuinely blank (or fully erased) chip.
        return Ok(json!({ "blank": true, "resident": Value::Null, "bootloader": bootloader }));
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
            // No descriptor date: beside the product and version it reads as
            // the release's build date, and an Arduino build's is the core's.
            // The health report's slots carry it, labeled (app.js).
            if let Some(d) = health::parse_app_descriptor(&db) {
                resident = json!({
                    "version": d.version,
                    "projectName": d.project_name,
                    "idf": d.idf_ver,
                    "secureVersion": d.secure_version,
                    "elfSha256Prefix": d.elf_sha256_prefix,
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
        "bootloader": bootloader,
        "ota": ota_json,
        "coredump": coredump_json,
        "witness": witness_json,
    }))
}

/// Where a user gets a fresh copy of the app: the website's download page.
/// Not a GitHub release search — `?q=flasher-v` lists the rolling
/// `flasher-latest` pointer (an updater manifest, no installer; see
/// `docs/RELEASE_BUTTONS.md`) and a firmware release, not the Flasher's own.
pub(crate) fn open_releases_page(app: &AppHandle) {
    let _ = app
        .opener()
        .open_url("https://securacv.com/download", None::<&str>);
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
        .manage(fleet::SealState::default())
        .manage(ChosenFiles::default())
        // On launch, reclaim anything a crash or pulled plug orphaned (a
        // ~2.5 GB raw staging image, a half-finished .partial download), and
        // close a safety-copy folder an older build left open to other
        // accounts. Off the main thread so it never delays the window.
        // Orphaned *processes* are already gone by now — the launch guard
        // reaps those first, before the UI enumerates ports and finds the
        // board still held.
        .setup(move |app| {
            let handle = app.handle().clone();
            std::thread::spawn(move || {
                hub::cleanup_orphans(&handle);
                if let Ok(backups) = backups_dir(&handle) {
                    close_backups_folder(&backups);
                }
            });
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
                    return;
                }
                // The same courtesy for a board: espflash is erasing or
                // writing one right now. Two facts, both needed — a live
                // espflash ticket alone also means a read (the chip probe, a
                // backup, the health check), and the engine's write marker
                // says which it is (flash_engine::flash::BOARD_WRITES).
                // Advisory, with Quit: an interrupted write is re-done from
                // the ROM bootloader, never a brick. The Lab asks the same
                // question in the same words.
                let writing = flash_engine::flash::BOARD_WRITES.any()
                    && window
                        .try_state::<Arc<launch_guard::LaunchGuard>>()
                        .map(|g| g.any_live(flash_engine::sidecar::ESPFLASH))
                        .unwrap_or(false);
                if writing {
                    api.prevent_close();
                    let w = window.clone();
                    w.dialog()
                        .message(
                            "A flash is still writing to the board. Quitting now leaves it \
                             unfinished — safe, you would flash it again. Quit anyway?",
                        )
                        .title("Quit while flashing?")
                        .buttons(MessageDialogButtons::OkCancelCustom(
                            "Quit".into(),
                            "Keep flashing".into(),
                        ))
                        .show(move |quit| {
                            if quit {
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
            efuse::read_security_efuses,
            fetch_manifest,
            witness_discover,
            flash,
            pick_local_file,
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
            hub::hub_resolve_hub,
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
    use super::{
        chosen_file_ok, suggested_file_name, validated_backup_path, validated_save_path,
        vision_model_size_ok, VISION_MODEL_MAX_BYTES,
    };
    use std::path::Path;

    #[test]
    fn a_picked_file_is_a_regular_file_outside_the_apps_own_folder() {
        let dir = tempfile::tempdir().expect("tempdir");
        let app_data = dir.path().join("app-data");
        std::fs::create_dir_all(app_data.join("backups")).unwrap();
        let mine = dir.path().join("firmware.bin");
        std::fs::write(&mine, b"\xE9hello").unwrap();

        let ok = chosen_file_ok(&mine, Some(&app_data)).expect("the user's own file");
        assert_eq!(ok, mine.canonicalize().unwrap());
        // No app data dir known (a path lookup failed): the file check alone.
        assert!(chosen_file_ok(&mine, None).is_ok());

        // Not a file: a directory, a missing path, a relative path.
        assert!(
            chosen_file_ok(dir.path(), Some(&app_data)).is_err(),
            "a directory"
        );
        assert!(
            chosen_file_ok(&dir.path().join("nope.bin"), Some(&app_data)).is_err(),
            "missing"
        );
        assert!(
            chosen_file_ok(std::path::Path::new("firmware.bin"), Some(&app_data)).is_err(),
            "relative"
        );

        // The app's own files are not "a file of yours": a safety-copy backup
        // (the identity key and Wi-Fi in cleartext) must not be writable to
        // a board by a script that guessed its name.
        let backup = app_data.join("backups/canary-abc-backup.bin");
        std::fs::write(&backup, b"backup").unwrap();
        assert!(chosen_file_ok(&backup, Some(&app_data)).is_err());
        // ...and `..` can't walk back out of the refusal.
        let sneaky = app_data.join("backups/../backups/canary-abc-backup.bin");
        assert!(chosen_file_ok(&sneaky, Some(&app_data)).is_err());
    }

    #[cfg(unix)]
    #[test]
    fn a_symlink_is_judged_by_what_it_points_at() {
        let dir = tempfile::tempdir().expect("tempdir");
        let app_data = dir.path().join("app-data");
        std::fs::create_dir_all(&app_data).unwrap();
        let secret = app_data.join("launch.json");
        std::fs::write(&secret, b"{}").unwrap();
        let link = dir.path().join("innocent.bin");
        std::os::unix::fs::symlink(&secret, &link).unwrap();
        assert!(
            chosen_file_ok(&link, Some(&app_data)).is_err(),
            "a link into app data"
        );
        let to_dir = dir.path().join("dir.bin");
        std::os::unix::fs::symlink(&app_data, &to_dir).unwrap();
        assert!(
            chosen_file_ok(&to_dir, Some(&app_data)).is_err(),
            "a link to a directory"
        );
    }

    #[test]
    fn backup_paths_are_validated_not_trusted() {
        // The backup is the identity key and Wi-Fi secrets in cleartext. Its
        // destination is the save panel's answer (or the app's own backups
        // folder) and still gets exactly the JSON export's rules.
        let dir = tempfile::tempdir().expect("tempdir");
        let good = dir.path().join("canary-backup.bin");
        let resolved = validated_backup_path(&good).expect("a dialog-shaped path");
        assert_eq!(resolved.file_name().unwrap(), "canary-backup.bin");

        assert!(
            validated_backup_path(Path::new("backup.bin")).is_err(),
            "relative"
        );
        assert!(
            validated_backup_path(&dir.path().join("backup.json")).is_err(),
            "only .bin"
        );
        assert!(
            validated_backup_path(&dir.path().join("no-such-dir/backup.bin")).is_err(),
            "missing folder"
        );
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let sneaky = dir.path().join("sub/../canary-backup.bin");
        let resolved = validated_backup_path(&sneaky).unwrap();
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
        let resolved = validated_save_path(&good).expect("a dialog-shaped path");
        assert_eq!(resolved.file_name().unwrap(), "canary-report.json");

        // Relative paths, non-.json targets, and missing folders are refused.
        assert!(validated_save_path(Path::new("report.json")).is_err());
        assert!(validated_save_path(&dir.path().join("evil.sh")).is_err());
        assert!(validated_save_path(&dir.path().join("no-such-dir/report.json")).is_err());

        // `..` segments are resolved away, never written through blindly.
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let sneaky = dir.path().join("sub/../canary-report.json");
        let resolved = validated_save_path(&sneaky).expect("canonicalized");
        assert_eq!(
            resolved,
            dir.path()
                .canonicalize()
                .unwrap()
                .join("canary-report.json")
        );
    }

    #[test]
    fn a_save_never_writes_through_a_name_that_is_not_a_regular_file() {
        let dir = tempfile::tempdir().expect("tempdir");
        // Replacing a file of the user's own is what a save panel's "Replace"
        // means: allowed.
        let existing = dir.path().join("old-report.json");
        std::fs::write(&existing, b"{}").unwrap();
        assert!(
            validated_save_path(&existing).is_ok(),
            "an existing regular file"
        );

        // A folder wearing the extension is not a file to write.
        let folder = dir.path().join("folder.json");
        std::fs::create_dir(&folder).unwrap();
        assert!(validated_save_path(&folder).is_err(), "a directory");
        let folder_bin = dir.path().join("folder.bin");
        std::fs::create_dir(&folder_bin).unwrap();
        assert!(validated_backup_path(&folder_bin).is_err(), "a directory");

        // A link at the name is refused, never followed: the write would land
        // on whatever it points at (somebody's settings.json, a device).
        #[cfg(unix)]
        {
            let victim = dir.path().join("settings.json");
            std::fs::write(&victim, b"{\"keep\":true}").unwrap();
            let link = dir.path().join("report.json");
            std::os::unix::fs::symlink(&victim, &link).unwrap();
            assert!(validated_save_path(&link).is_err(), "a link to a file");
            let dangling = dir.path().join("dangling.bin");
            std::os::unix::fs::symlink(dir.path().join("nowhere"), &dangling).unwrap();
            assert!(validated_backup_path(&dangling).is_err(), "a dangling link");
            assert_eq!(std::fs::read(&victim).unwrap(), b"{\"keep\":true}");
        }
    }

    #[test]
    fn a_suggested_name_is_one_plain_file_name_with_the_expected_ending() {
        // The browser's own names, which the page suggests.
        assert_eq!(
            suggested_file_name("canary-2026-10-22-1-backup.bin", "bin").unwrap(),
            "canary-2026-10-22-1-backup.bin"
        );
        assert_eq!(
            suggested_file_name("canary-2026-10-22-2-report.json", "json").unwrap(),
            "canary-2026-10-22-2-report.json"
        );
        // The name is the one value the webview supplies: a directory, a
        // parent step, a hidden file, another ending or nothing is refused.
        for hostile in [
            "../../etc/passwd.json",
            "/abs/olute.json",
            "a\\b.json",
            "..json",
            "x/../y.json",
            ".bashrc.json",
            "report.json.sh",
            "report",
            ".json",
            "",
            "with space.json",
        ] {
            assert!(suggested_file_name(hostile, "json").is_err(), "{hostile}");
        }
        assert!(suggested_file_name("canary-2026-10-22-1-backup.bin", "json").is_err());
        assert!(suggested_file_name(&format!("{}.json", "a".repeat(90)), "json").is_err());
    }

    #[test]
    fn a_vision_model_size_past_the_cap_is_refused_by_name() {
        assert_eq!(vision_model_size_ok(1), Ok(1));
        assert_eq!(
            vision_model_size_ok(VISION_MODEL_MAX_BYTES),
            Ok(VISION_MODEL_MAX_BYTES)
        );
        for huge in [VISION_MODEL_MAX_BYTES + 1, 1 << 40, u64::MAX] {
            let err = vision_model_size_ok(huge).unwrap_err();
            assert!(err.contains("implausible size"), "{err}");
            assert!(err.contains(&VISION_MODEL_MAX_BYTES.to_string()), "{err}");
        }
    }
}

#[cfg(test)]
mod failed_copy_tests {
    use super::{discard_failed_copy, BackupFile};

    /// Only a file this run made is removed, and only when the run failed.
    #[test]
    fn a_failed_read_removes_only_the_file_it_made() {
        let dir = tempfile::tempdir().unwrap();
        let made = BackupFile {
            created: true,
            readable_by_others: false,
        };
        let was_there = BackupFile {
            created: false,
            readable_by_others: false,
        };
        for (prepared, ran, kept) in [
            (&made, Ok(0), true),
            (&made, Ok(1), false),
            (&made, Err("espflash didn't start".to_string()), false),
            (&was_there, Ok(1), true),
            (&was_there, Err("espflash didn't start".to_string()), true),
        ] {
            let out = dir.path().join("canary-backup.bin");
            std::fs::write(&out, b"").unwrap();
            discard_failed_copy(prepared, &ran, &out);
            assert_eq!(
                out.exists(),
                kept,
                "created={} ran={ran:?}",
                prepared.created
            );
        }
    }
}

#[cfg(all(test, unix))]
mod owner_only_tests {
    use super::{close_backups_folder, owner_only_dir, owner_only_file};
    use std::os::unix::fs::PermissionsExt;

    fn mode(p: &std::path::Path) -> u32 {
        std::fs::metadata(p).unwrap().permissions().mode() & 0o777
    }

    /// Run `f` under the usual desktop umask (022), so a plain create comes
    /// out 0755 (folder) or 0644 (file) on every runner, whatever its own
    /// umask. SAFETY: umask(2) swaps the process's file-creation mask; it
    /// cannot fail and touches no memory.
    fn under_umask_022<T>(f: impl FnOnce() -> T) -> T {
        let previous = unsafe { libc::umask(0o022) };
        let out = f();
        unsafe { libc::umask(previous) };
        out
    }

    #[test]
    fn a_fresh_backups_folder_is_owner_only() {
        let dir = tempfile::tempdir().unwrap();
        let app_data = dir.path().join("com.securacv.flasher");
        let backups = app_data.join("backups");
        under_umask_022(|| owner_only_dir(&backups)).expect("made");
        assert_eq!(mode(&backups), 0o700, "backups/");
        assert_eq!(mode(&app_data), 0o700, "a parent this call made");
    }

    /// An older build made `backups/` with the umask's default (0755) and left
    /// its copies 0644 inside. The next backup tightens the folder, which puts
    /// those copies out of every other account's reach too.
    #[test]
    fn an_older_builds_open_folder_is_tightened() {
        let dir = tempfile::tempdir().unwrap();
        let backups = dir.path().join("backups");
        std::fs::create_dir(&backups).unwrap();
        std::fs::set_permissions(&backups, std::fs::Permissions::from_mode(0o755)).unwrap();
        let old_copy = backups.join("canary-aabbcc-1700000000-backup.bin");
        std::fs::write(&old_copy, b"old dump").unwrap();
        std::fs::set_permissions(&old_copy, std::fs::Permissions::from_mode(0o644)).unwrap();

        owner_only_dir(&backups).expect("tightened");
        assert_eq!(mode(&backups), 0o700);
        assert_eq!(std::fs::read(&old_copy).unwrap(), b"old dump", "left whole");
    }

    /// Every launch closes a folder an older build left open, and tightens
    /// the copies in it, whether or not this run takes a safety copy (the
    /// Rescue bench and an install with the copy turned off never reach
    /// `owner_only_dir`). A link in the folder is not followed.
    #[test]
    fn every_launch_closes_an_older_builds_open_folder() {
        let dir = tempfile::tempdir().unwrap();
        let backups = dir.path().join("backups");
        std::fs::create_dir(&backups).unwrap();
        std::fs::set_permissions(&backups, std::fs::Permissions::from_mode(0o755)).unwrap();
        let old_copy = backups.join("canary-aabbcc-1700000000-backup.bin");
        std::fs::write(&old_copy, b"old dump").unwrap();
        std::fs::set_permissions(&old_copy, std::fs::Permissions::from_mode(0o644)).unwrap();
        let elsewhere = dir.path().join("notes.txt");
        std::fs::write(&elsewhere, b"not a copy").unwrap();
        std::fs::set_permissions(&elsewhere, std::fs::Permissions::from_mode(0o644)).unwrap();
        std::os::unix::fs::symlink(&elsewhere, backups.join("link.bin")).unwrap();

        close_backups_folder(&backups);

        assert_eq!(mode(&backups), 0o700, "backups/");
        assert_eq!(mode(&old_copy), 0o600, "a copy an older build left 0644");
        assert_eq!(std::fs::read(&old_copy).unwrap(), b"old dump", "left whole");
        assert_eq!(mode(&elsewhere), 0o644, "a link's target is not touched");
    }

    /// A launch only closes a folder that is there; it makes none.
    #[test]
    fn a_launch_makes_no_backups_folder() {
        let dir = tempfile::tempdir().unwrap();
        let backups = dir.path().join("backups");
        close_backups_folder(&backups);
        assert!(!backups.exists());
    }

    /// A new copy is created 0600 before espflash runs, and espflash's own
    /// open (3.3.0's `read_flash`: write + truncate + create, no mode) keeps
    /// it 0600 while it writes the dump.
    #[test]
    fn a_new_copy_is_owner_only_before_and_after_espflash_writes_it() {
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("canary-aabbcc-backup.bin");
        let prepared = under_umask_022(|| owner_only_file(&out)).expect("made");
        assert!(prepared.created && !prepared.readable_by_others);
        assert_eq!(mode(&out), 0o600, "pre-created");

        let mut f = under_umask_022(|| {
            std::fs::OpenOptions::new()
                .write(true)
                .truncate(true)
                .create(true)
                .open(&out)
        })
        .unwrap();
        std::io::Write::write_all(&mut f, b"FAKE-NVS wifi.pass=hunter2").unwrap();
        drop(f);
        assert_eq!(mode(&out), 0o600, "after espflash's open and write");
    }

    /// A Rescue-bench save over an earlier copy: that file is tightened to
    /// 0600 but not truncated, so a read that fails before espflash opens it
    /// still leaves the earlier copy whole.
    #[test]
    fn an_existing_copy_is_tightened_not_truncated() {
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("canary-backup.bin");
        std::fs::write(&out, b"earlier copy").unwrap();
        std::fs::set_permissions(&out, std::fs::Permissions::from_mode(0o644)).unwrap();

        let prepared = owner_only_file(&out).expect("tightened");
        assert!(!prepared.created && !prepared.readable_by_others);
        assert_eq!(mode(&out), 0o600);
        assert_eq!(std::fs::read(&out).unwrap(), b"earlier copy");
    }
}
