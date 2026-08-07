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

mod changemap;
mod fleet;
mod health;
mod hub;
mod intake;
mod launch_guard;
mod port_hint;
mod provisioning;
mod release;
mod rescue;
mod secret_store;
mod serial_monitor;
mod sscma;
mod we2;
mod we2_bench;

use provisioning::Provisioning;
use serde::Serialize;
use serde_json::{json, Map, Value};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons};
use tauri_plugin_opener::OpenerExt;
use tauri_plugin_shell::process::CommandEvent;
use tauri_plugin_shell::ShellExt;
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
const EMBEDDED_CATALOG: &str = include_str!(concat!(env!("OUT_DIR"), "/flash.json"));

// The Hatchery naming spec — the same canary-local/devices/hatch.json the
// website ships — embedded so the flasher's birth certificate names a Canary
// identically to the web Lab, offline, and can never drift from it.
const EMBEDDED_HATCH: &str = include_str!(concat!(env!("OUT_DIR"), "/hatch.json"));

// The dev channel's one stable address: the rolling fw-dev-latest prerelease
// that CI re-points on every fw-v*-dev.*/-rc.* tag. This is a fixed
// first-party constant, deliberately NOT a general manifest-URL override —
// the dev toggle can only ever mean this URL. It is the ONE alternative
// flash() accepts to the catalog's pinned manifest_url; everything downstream
// (chip guard, release origin, size/SHA, signature policy) is identical.
// Mirrors canary-local/assets/flash-core.js DEV_FLASH_MANIFEST_URL — the
// desktop-parity test fails if the two drift.
const DEV_FLASH_MANIFEST_URL: &str =
    "https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-flash.json";

// A firmware image can't reasonably exceed the largest flash any Canary
// carries (32 MiB parts exist; nothing bigger does). A file past this is a
// wrong pick — a disk image, a video — not a firmware image, so the local-file
// path refuses it before reading further.
const LOCAL_IMAGE_MAX_BYTES: u64 = 32 * 1024 * 1024;

// The shape of a merged factory image: the ESP32 partition table lives at
// 0x8000 on every variant, and its 32-byte entries open with the magic bytes
// 0xAA 0x50 (u16le 0x50AA) — the same constants
// firmware/scripts/make_factory.py merges by. This is what tells a factory
// image apart from an app-only build, because BOTH start with 0xE9.
const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_MAGIC_LE: [u8; 2] = [0xAA, 0x50];

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
    /// "stable" | "dev" for manifest flashes, "local" for a file off this
    /// computer's disk — so the receipt names which train the bytes came from.
    channel: &'static str,
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
// or NetworkManager via `nmcli -s` (polkit may prompt). Runs only on an
// explicit "Use saved" click in the frontend; the value goes straight into
// the field and is never logged or persisted by the app.
#[tauri::command]
fn saved_wifi_password(ssid: String) -> Result<String, String> {
    let ssid = ssid.trim().to_string();
    if ssid.is_empty() {
        return Err("type the Wi-Fi name first".to_string());
    }
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
    // Tracked, not bare: espflash's PID is on disk for as long as it runs, so
    // a force quit can't strand it holding the board's serial port with
    // nothing left to clean it up. `_ticket` un-records it on the way out.
    let (mut rx, _child, _ticket) = launch_guard::spawn_tracked(app, cmd, ESPFLASH)?;

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

/// What `detect_chip` reports: the canonical chip name — the "you can't pick the
/// wrong image" guard, since the UI only offers products whose `chip` matches —
/// plus the flash size in bytes when board-info named it, which the rescue
/// bench's full-chip backup needs. Both come from the one board-info call.
#[derive(Serialize)]
pub struct ChipInfo {
    chip: String,
    flash_bytes: Option<u64>,
    mac: Option<String>,
}

/// Ask the connected board which ESP32 it is (and how much flash it carries).
#[tauri::command]
async fn detect_chip(app: AppHandle, port: String) -> Result<ChipInfo, String> {
    let (code, out) =
        run_sidecar_capture(&app, vec!["board-info".into(), "--port".into(), port]).await?;
    // Check the exit code before parsing: a *failed* board-info can still print
    // a chip name in its error text, which would otherwise read as a false
    // positive detection.
    if code != 0 {
        // On Linux, a failed board-info is more often the OS refusing or
        // holding the port than a board out of download mode — when the
        // output names that cause, lead with its real fix instead of the
        // BOOT/RESET ritual (which can't help and reads as the board's
        // fault). Linux-only: the hint text is a Linux fix.
        if cfg!(target_os = "linux") {
            if let Some(hint) = port_hint::linux_open_hint(&out) {
                return Err(format!("{hint}\n\nespflash said:\n{}", out.trim()));
            }
        }
        return Err(format!(
            "couldn't read the chip (espflash exit {code}). Put the board in download mode (hold BOOT, tap RESET, release BOOT) and try again.\n\nespflash said:\n{}",
            out.trim()
        ));
    }
    match canonical_chip(&out) {
        Some(chip) => Ok(ChipInfo {
            chip: chip.to_string(),
            flash_bytes: rescue::parse_flash_size(&out),
            mac: rescue::parse_mac(&out),
        }),
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
    // The `HTTP <code>` token is a CONTRACT, not just prose: app.js matches it to
    // tell "the release we're pinned to has no images" (a real answer — someone
    // must cut that release) apart from a transport failure above (offline, DNS,
    // TLS), which proves nothing about whether the release exists. Reword freely,
    // but keep `HTTP <code>` in it or the UI silently falls back to the cautious
    // wording for every failure.
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
    let client = reqwest::Client::builder()
        .user_agent("SecuraCV-Flasher")
        .timeout(std::time::Duration::from_secs(2))
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
    erase_first: Option<bool>,
    // The safety copy taken moments ago, if there is one. Present = draw the
    // change map from it; absent = the user skipped the copy or the board
    // wouldn't read, and no map is the truthful outcome.
    backup_path: Option<String>,
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
    // Exactly two manifests are ever resolved: the catalog's pinned stable
    // release, and the fixed fw-dev-latest constant. Anything else is an
    // unbundled URL and is refused before a byte moves.
    let channel =
        if catalog.get("manifest_url").and_then(Value::as_str) == Some(manifest_url.as_str()) {
            "stable"
        } else if manifest_url == DEV_FLASH_MANIFEST_URL {
            "dev"
        } else {
            return Err("refusing an unbundled firmware manifest URL".into());
        };
    if channel == "dev" {
        emit(
            &app,
            "→ DEV CHANNEL: resolving the rolling fw-dev-latest prerelease, not the pinned stable release".into(),
        );
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

    // 2a) The change map, computed HERE because this is the only moment both
    // sides exist: the safety copy has every byte that is on the board, and
    // `bytes` is the verified image about to replace them. A separate command
    // would have to download and verify the image a second time.
    //
    // Entirely best-effort and never fatal: a missing or unreadable backup
    // (the copy was skipped, or the board wouldn't read) means no map, which
    // is the honest answer. Failing the install because we couldn't draw a
    // picture of it would be absurd.
    if let Some(bp) = backup_path.as_deref().filter(|p| !p.is_empty()) {
        if let Ok(old) = std::fs::read(bp) {
            // Both facts the verdict needs are known right here: whether this
            // install erases the whole chip first (so regions the image never
            // reaches do NOT survive), and whether we just wrote the user's
            // own network into the replacement NVS (so a differing settings
            // region means "replaced with what you asked for", not "cleared").
            let erase_all = erase_first.unwrap_or(false);
            let baked_wifi = provisioning
                .as_ref()
                .map(|p| !p.wifi_ssid.is_empty())
                .unwrap_or(false);

            // Free intake check while we hold the whole chip: does the flash
            // really hold what it claims? A relabeled part (a 4 MB die sold as
            // 16 MB) ACCEPTS writes past its real end and discards them, so
            // the install "succeeds" and the board can't boot, with no error
            // at any layer. This costs no serial time — the bytes are already
            // here — and it is the last moment the write can still be stopped.
            // The safety copy was read with the chip's DECLARED size, so the
            // dump's own length is that claim — no extra argument needed.
            let declared = old.len() as u64;
            if declared >= 0x2000 {
                let f = intake::flash_alias_verdict(&old, declared);
                if f.level == "stop" {
                    emit(&app, format!("✗ {}", f.label));
                    if let Some(d) = &f.detail {
                        emit(&app, format!("  {d}"));
                    }
                    return Err(format!(
                        "{} {} Nothing was written.",
                        f.label,
                        f.detail.unwrap_or_default()
                    ));
                }
                emit(&app, format!("✓ {}", f.label));
            }
            if let Some(map) = changemap::diff_install(&old, &bytes, erase_all) {
                let had_wifi = old.windows(9).any(|w| w == b"wifi_ssid");
                let verdict = changemap::settings_verdict(&map, had_wifi, baked_wifi);
                let _ = app.emit(
                    "flash:changemap",
                    json!({
                        "layoutChanged": map.layout_changed,
                        "settings": verdict.map(|(kept, text)| json!({ "kept": kept, "text": text })),
                        "rows": map.rows.iter().map(|r| json!({
                            "label": r.label,
                            "kind": r.kind,
                            "offset": r.offset,
                            "size": r.size,
                            "verdict": r.verdict.as_str(),
                            "changedPct": r.changed_pct,
                            "before": r.before,
                            "after": r.after,
                        })).collect::<Vec<_>>(),
                    }),
                );
            }
        }
    }

    // 2b) First contact: wipe the WHOLE chip before writing. `write-bin` only
    //     touches the regions the image covers, so a board that arrived
    //     carrying somebody else's firmware would keep whatever sat in the
    //     partitions we don't write — on a board the user now believes is
    //     theirs. Erasing first is the only way that leftover goes away.
    //
    //     This mirrors the browser flasher, which forces the same erase on a
    //     board it has never written (canary-local/assets/intake.js:
    //     isFirstContact). The browser decides it by reading the board;
    //     espflash reports nothing about resident firmware, so here it comes
    //     from the step-1 checkbox.
    if erase_first.unwrap_or(false) {
        emit(
            &app,
            "→ first contact with this board — erasing the whole chip before writing".into(),
        );
        let code =
            run_sidecar_streaming(&app, rescue::erase_flash_args(&port), "flash:log").await?;
        if code != 0 {
            return Err(format!(
                "the full erase failed (espflash exit {code}). Nothing was written. The board can't be bricked — put it back in download mode and try again."
            ));
        }
        emit(
            &app,
            "✓ chip erased — nothing of the old firmware is left".into(),
        );
    }

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
    // Tracked, not bare: espflash's PID is on disk for as long as it runs, so
    // a force quit can't strand it holding the board's serial port with
    // nothing left to clean it up. `_ticket` un-records it on the way out.
    let (mut rx, _child, _ticket) = launch_guard::spawn_tracked(&app, cmd, ESPFLASH)?;

    let mut code = -1;
    // Keep espflash's last words. They stream to the console for the user to
    // read, but the FAILURE needs them too: the frontend classifies errors by
    // their text ("permission denied", "resource busy", "no serial data"), and
    // an error saying only "exited with code 1" classifies as `unknown` —
    // indistinguishable from a bad cable. That makes a busy port look like a
    // transport fault and get retried down the whole baud ladder, re-erasing
    // and re-downloading each time. A few lines of tail is the difference
    // between a diagnosis and a shrug. (read_region already did this.)
    let mut tail: Vec<String> = Vec::new();
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                let text = String::from_utf8_lossy(&bytes);
                for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                    emit(&app, line.to_string());
                    tail.push(line.trim().to_string());
                    if tail.len() > 8 {
                        tail.remove(0);
                    }
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
            channel,
            chip_write_verified: true,
            provisioned,
        })
    } else {
        Err(format!(
            "espflash exited with code {code}. The board can't be bricked — put it back in download mode and try again.\n{}",
            tail.join("\n")
        ))
    }
}

/// The cheap refusals for a user-picked firmware file: an empty file or one
/// larger than any Canary's flash is a wrong pick, and a file without a
/// partition table at 0x8000 is an app-only build — this path writes whole
/// factory images at offset 0, so an app-only .bin would land on the
/// bootloader and the board wouldn't boot (recoverable over USB download
/// mode, but a guaranteed bad hour). The 0xE9 image magic can't make that
/// call — an app-only build starts with 0xE9 too. Anything subtler (a real
/// factory image for the wrong board) is on the user — a personal file has
/// no catalog entry to check it against.
fn check_local_image(bytes: &[u8]) -> Result<(), String> {
    if bytes.is_empty() {
        return Err("that file is empty — there's nothing to write".into());
    }
    if bytes.len() as u64 > LOCAL_IMAGE_MAX_BYTES {
        return Err(format!(
            "that file is {} bytes — no Canary carries more than 32 MiB of flash, so this can't be a firmware image",
            bytes.len()
        ));
    }
    let factory_shape = bytes.len() > PARTITION_TABLE_OFFSET + 32
        && bytes[PARTITION_TABLE_OFFSET..PARTITION_TABLE_OFFSET + 2] == PARTITION_MAGIC_LE;
    if !factory_shape {
        return Err(
            "this looks like an app-only build, not a merged factory image — there's no \
             partition table at 0x8000. The flasher writes whole factory images at offset 0, \
             so an app-only .bin would overwrite the bootloader and the board wouldn't boot. \
             Merge one with firmware/scripts/make_factory.py or use `dev_flash.sh <env> -f`."
                .into(),
        );
    }
    Ok(())
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

    let args = vec![
        "write-bin".into(),
        "0x0".into(),
        staged_path.to_string_lossy().to_string(),
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
    // Tracked, not bare: espflash's PID is on disk for as long as it runs, so
    // a force quit can't strand it holding the board's serial port with
    // nothing left to clean it up. `_ticket` un-records it on the way out.
    let (mut rx, _child, _ticket) = launch_guard::spawn_tracked(&app, cmd, ESPFLASH)?;

    let mut code = -1;
    // Keep espflash's last words. They stream to the console for the user to
    // read, but the FAILURE needs them too: the frontend classifies errors by
    // their text ("permission denied", "resource busy", "no serial data"), and
    // an error saying only "exited with code 1" classifies as `unknown` —
    // indistinguishable from a bad cable. That makes a busy port look like a
    // transport fault and get retried down the whole baud ladder, re-erasing
    // and re-downloading each time. A few lines of tail is the difference
    // between a diagnosis and a shrug. (read_region already did this.)
    let mut tail: Vec<String> = Vec::new();
    while let Some(event) = rx.recv().await {
        match event {
            CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                let text = String::from_utf8_lossy(&bytes);
                for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                    emit(&app, line.to_string());
                    tail.push(line.trim().to_string());
                    if tail.len() > 8 {
                        tail.remove(0);
                    }
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
        })
    } else {
        Err(format!(
            "espflash exited with code {code}. The board can't be bricked — put it back in download mode and try again.\n{}",
            tail.join("\n")
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
/// `event`, and return its exit code.
async fn run_sidecar_streaming(
    app: &AppHandle,
    args: Vec<String>,
    event: &'static str,
) -> Result<i32, String> {
    let cmd = app
        .shell()
        .sidecar(ESPFLASH)
        .map_err(|e| format!("bundled espflash missing: {e}"))?
        .args(args);
    // Tracked, not bare: espflash's PID is on disk for as long as it runs, so
    // a force quit can't strand it holding the board's serial port with
    // nothing left to clean it up. `_ticket` un-records it on the way out.
    let (mut rx, _child, _ticket) = launch_guard::spawn_tracked(app, cmd, ESPFLASH)?;
    let mut code = -1;
    while let Some(ev) = rx.recv().await {
        match ev {
            CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                let text = String::from_utf8_lossy(&bytes);
                for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                    let _ = app.emit(event, line.to_string());
                }
            }
            CommandEvent::Terminated(payload) => code = payload.code.unwrap_or(-1),
            _ => {}
        }
    }
    Ok(code)
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
            if safe_mac.is_empty() { "unknown".into() } else { safe_mac }
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

/// Write UTF-8 text to a path the user just chose in the OS save panel — the
/// health report's JSON export. No FS plugin, no ambient access: the frontend
/// hands over a path the save dialog already blessed.
#[tauri::command]
async fn save_text_file(path: String, contents: String) -> Result<(), String> {
    std::fs::write(&path, contents).map_err(|e| format!("couldn't save {path}: {e}"))
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
async fn board_passport(
    app: AppHandle,
    port: String,
    baud: u32,
) -> Result<Value, String> {
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
            if let Ok(ob) =
                read_region(&app, &port, otap.offset, otap.size.min(0x2000), baud).await
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
mod local_image_tests {
    use super::{check_local_image, PARTITION_TABLE_OFFSET};

    #[test]
    fn app_only_builds_are_refused_and_factory_shapes_pass() {
        assert!(check_local_image(&[]).is_err());
        // An app-only PlatformIO build starts with 0xE9 too — the refusal
        // must come from the missing partition table, not the image magic.
        let app_only = vec![0xE9; PARTITION_TABLE_OFFSET / 2];
        assert!(check_local_image(&app_only).is_err());
        // Right length, no 0xAA 0x50 at 0x8000 → still not a factory image.
        let unmerged = vec![0xE9; PARTITION_TABLE_OFFSET + 64];
        assert!(check_local_image(&unmerged).is_err());
        let mut factory = vec![0xFF; PARTITION_TABLE_OFFSET + 64];
        factory[0] = 0xE9;
        factory[PARTITION_TABLE_OFFSET] = 0xAA;
        factory[PARTITION_TABLE_OFFSET + 1] = 0x50;
        assert!(check_local_image(&factory).is_ok());
    }
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
