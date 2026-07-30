//! hub — the Raspberry Pi Home Assistant hub writer, wired into the app.
//!
//! This is deliberately the THINNEST layer in the hub path
//! (docs/design/raspberry_pi_hub_flashing.md): every decision lives in
//! `hub-core` (what is a legal target, is the image trusted, may we write) and
//! every mechanism lives in `hub-io` (download+hash, xz, the read-back-verified
//! write, the Wi-Fi seed) — both PR-CI-tested crates. What remains here is
//! translation: catalog JSON → `CatalogView`, judged disks → DTOs the picker
//! can render, progress callbacks → `hub:progress` events, and the one
//! orchestrating command that runs the pipeline in order.
//!
//! The safety chain the orchestration cannot skip, because it's typed:
//! `verify_download` is the only mint of `VerifiedImage`, `authorize_write`
//! (verified + still-eligible + operator-confirmed) is the only mint of
//! `WriteAuthorization`, and `hub_io::write::write_image` takes that by value.

use hub_core::hub_disk::{judge_all, Eligibility, Judged};
use hub_core::hub_enumerate::enumerate;
use hub_core::hub_flash::authorize_write;
use hub_core::hub_image::{recommended_board, resolve, BoardImage, CatalogView};
use hub_core::hub_seed::WifiSeed;
use hub_io::account::{mint_owner_store, OwnerAccount};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_shell::process::{CommandChild, CommandEvent};
use tauri_plugin_shell::ShellExt;

// Same never-rot posture as the flasher catalog: the ONE canonical
// hub_image.json is embedded fresh on every build by build.rs.
const EMBEDDED_HUB_CATALOG: &str = include_str!(concat!(env!("OUT_DIR"), "/hub_image.json"));

// Raspberry Pi's official USB device-boot host tool, bundled as a sidecar
// exactly like espflash (release CI builds it from a pinned raspberrypi/usbboot
// checkout). It waits for a Pi in ROM boot mode (power button held while
// connecting USB-C), pushes the signed mass-storage gadget, and exits — after
// which the Pi presents its SD/NVMe to this computer as an ordinary USB disk
// and the normal target polling picks it up. No card reader required.
const RPIBOOT: &str = "rpiboot";

/// The one rpiboot process we may have waiting. One at a time on purpose: two
/// concurrent rpiboots would race for the same USB device.
#[derive(Default)]
pub struct PiUsbState(std::sync::Mutex<Option<CommandChild>>);

/// The running flash's stop signal, if one is running. The UI's Stop button
/// flips it via [`hub_flash_cancel`]; every chunk loop in hub-io checks it, so
/// a stop lands within one 4 MiB chunk — no orphaned writer, no zombie state.
#[derive(Default)]
pub struct HubFlashState(std::sync::Mutex<Option<hub_io::CancelToken>>);

impl HubFlashState {
    /// Whether a flash is in flight right now (used by the quit guard).
    pub fn is_running(&self) -> bool {
        self.0.lock().map(|g| g.is_some()).unwrap_or(false)
    }
    /// Signal the running flash to stop at its next chunk boundary. Safe at
    /// every stage — before the write nothing touched the card; during it the
    /// card was being erased anyway and just needs a fresh flash.
    pub fn cancel(&self) {
        if let Ok(g) = self.0.lock() {
            if let Some(t) = g.as_ref() {
                t.cancel();
            }
        }
    }
}

/// One disk as the picker shows it — eligible or not, always with the reason,
/// so "why isn't my drive listed" is answered on screen instead of in a forum.
#[derive(Serialize)]
pub struct HubTargetDto {
    path: String,
    model: String,
    size_bytes: u64,
    eligible: bool,
    /// Human reason when refused (`None` when eligible).
    refused_because: Option<String>,
    /// Advisory lines when eligible (erases data, below recommended size, …).
    warnings: Vec<String>,
}

/// The catalog slice the UI needs to explain the flow before anything runs.
#[derive(Serialize)]
pub struct HubPlanDto {
    os_label: String,
    board_id: String,
    board_name: String,
    image_url: String,
    pinned: bool,
    min_card_bytes: u64,
    warnings: Vec<String>,
}

/// What a finished hub flash proved, for the UI's receipt screen.
#[derive(Serialize)]
pub struct HubReceipt {
    board_id: String,
    os_label: String,
    target_path: String,
    bytes_written: u64,
    sha256: String,
    wifi_seeded: bool,
    /// A non-fatal note about the Wi-Fi seed (e.g. the write verified but
    /// seeding failed) — the receipt stays a receipt; this is the plan B.
    wifi_note: Option<String>,
    /// Whether the experimental account pre-seed was written to the card.
    account_seeded: bool,
    /// Non-fatal note about the account seed, if requested.
    account_note: Option<String>,
    /// Set when the card wouldn't cleanly auto-eject — an "eject it yourself"
    /// instruction shown INDEPENDENT of seed success (the settings are on the
    /// card, so pulling it while still mounted risks a half-written CONFIG).
    eject_note: Option<String>,
    /// True when a locally-cached, re-verified image was reused (no download).
    used_cache: bool,
}

/// The Wi-Fi the operator typed (the same secret the Canary flasher already
/// collects). Values never appear in logs or events.
#[derive(Deserialize)]
pub struct HubWifi {
    ssid: String,
    passphrase: String,
    hidden: bool,
}

/// The Home Assistant owner account, collected next to the Wi-Fi. Opt-in and
/// EXPERIMENTAL (the zero-touch restore mechanism is still under hardware
/// validation): when present, the flasher mints HA's `.storage` locally and
/// seeds it onto the boot partition so first visit is a login page. The
/// password is hashed on this computer and never logged or emitted.
#[derive(Deserialize)]
pub struct HubAccount {
    name: String,
    username: String,
    password: String,
}

/// What a preflight check found — surfaced before the flash so a disk-full or
/// tiny-target surprise never lands mid-write.
#[derive(Serialize)]
pub struct HubPreflight {
    /// Free bytes in the staging directory (download + decompress live here).
    staging_free_bytes: u64,
    /// Roughly what the pipeline needs there (compressed + raw + headroom).
    staging_need_bytes: u64,
    /// True when there's comfortably enough room.
    staging_ok: bool,
    /// The OS family, so the UI can tailor a hint (e.g. macOS permission prompt).
    platform: String,
}

fn parse_catalog() -> Result<(Value, CatalogView), String> {
    let json: Value = serde_json::from_str(EMBEDDED_HUB_CATALOG)
        .map_err(|e| format!("bundled hub catalog is corrupt: {e}"))?;
    let base = json
        .get("base_os")
        .ok_or("bundled hub catalog has no base_os")?;
    let boards = base
        .get("boards")
        .and_then(Value::as_array)
        .ok_or("bundled hub catalog has no boards")?
        .iter()
        .map(|b| {
            Ok(BoardImage {
                id: b
                    .get("id")
                    .and_then(Value::as_str)
                    .ok_or("board without id")?
                    .to_string(),
                name: b
                    .get("name")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
                recommended: b
                    .get("recommended")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                image_url: b
                    .get("image_url")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
                sha256: b
                    .get("sha256")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
            })
        })
        .collect::<Result<Vec<_>, &'static str>>()
        .map_err(|e| format!("bundled hub catalog is malformed: {e}"))?;
    let view = CatalogView {
        os_name: base
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or("Home Assistant OS")
            .to_string(),
        os_version: base
            .get("version")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string(),
        pinned: base.get("pinned").and_then(Value::as_bool).unwrap_or(false),
        min_card_bytes: json
            .get("card_requirements")
            .and_then(|c| c.get("min_bytes"))
            .and_then(Value::as_u64)
            .unwrap_or(0),
        boards,
    };
    Ok((json, view))
}

/// The full hub catalog JSON, verbatim, for the UI's explainer copy (what gets
/// pre-installed, why HAOS, the card guidance) — same schema the website reads.
#[tauri::command]
pub fn load_hub_catalog() -> Result<Value, String> {
    parse_catalog().map(|(json, _)| json)
}

/// The disks the OS can see right now, each with its verdict. The UI polls
/// this while the operator is on the "plug in your card" step, so inserting a
/// card is what advances the flow — recognition, not configuration.
#[tauri::command]
pub fn list_hub_targets() -> Result<Vec<HubTargetDto>, String> {
    let judged = judge_all(enumerate()?);
    Ok(judged
        .into_iter()
        .map(|Judged { disk, eligibility }| {
            let (eligible, refused_because, warnings) = match eligibility {
                Eligibility::Eligible { warnings } => {
                    (true, None, warnings.iter().map(|w| w.message()).collect())
                }
                Eligibility::Refused(r) => (false, Some(r.reason()), Vec::new()),
            };
            HubTargetDto {
                path: disk.path,
                model: disk.model,
                size_bytes: disk.size_bytes,
                eligible,
                refused_because,
                warnings,
            }
        })
        .collect())
}

/// Resolve the write plan for a board — what image, from where, pinned or not
/// — so the confirm screen states exactly what will happen before it does.
#[tauri::command]
pub fn hub_plan(board_id: Option<String>) -> Result<HubPlanDto, String> {
    let (_, view) = parse_catalog()?;
    let board_id = board_id
        .or_else(|| recommended_board(&view).map(str::to_string))
        .ok_or("the hub catalog names no recommended board")?;
    let plan = resolve(&view, &board_id).map_err(|e| e.message())?;
    let board_name = view
        .boards
        .iter()
        .find(|b| b.id == board_id)
        .map(|b| b.name.clone())
        .unwrap_or_else(|| board_id.clone());
    Ok(HubPlanDto {
        os_label: plan.os_label.clone(),
        board_id,
        board_name,
        image_url: plan.image_url.clone(),
        pinned: plan.expected_sha256.is_some(),
        min_card_bytes: plan.min_card_bytes,
        warnings: plan.warnings.iter().map(|w| w.message()).collect(),
    })
}

/// Start waiting for a Raspberry Pi in USB device-boot mode and turn it into
/// a disk. Spawns the bundled `rpiboot` with the mass-storage-gadget payload;
/// rpiboot waits for the Pi, serves the gadget, and exits. Its output streams
/// over `hub:pi-usb` and the exit code over `hub:pi-usb-done`; the Pi-as-disk
/// then appears through the ordinary `list_hub_targets` polling (as an
/// `RPi-MSD…` USB disk), so the rest of the flow is unchanged.
#[tauri::command]
pub async fn hub_pi_boot_start(
    app: AppHandle,
    state: tauri::State<'_, PiUsbState>,
) -> Result<(), String> {
    {
        let guard = state.0.lock().map_err(|_| "Pi USB state poisoned")?;
        if guard.is_some() {
            return Err("already waiting for a Pi over USB".to_string());
        }
    }
    // The gadget payload ships as a bundled resource next to the app (release
    // CI vendors it from the same pinned usbboot checkout as the sidecar). A
    // dev build without it fails here with the reason, not downstream.
    let gadget = app
        .path()
        .resolve(
            "mass-storage-gadget64",
            tauri::path::BaseDirectory::Resource,
        )
        .ok()
        .filter(|p| p.exists())
        .ok_or_else(|| {
            "this build doesn't bundle the Pi USB-boot payload (mass-storage-gadget64) — \
             use a released build, or flash the card in a reader instead"
                .to_string()
        })?;

    let cmd = app
        .shell()
        .sidecar(RPIBOOT)
        .map_err(|e| format!("bundled rpiboot missing: {e}"))?
        .args(["-d".to_string(), gadget.to_string_lossy().into_owned()]);
    let (mut rx, child) = cmd
        .spawn()
        .map_err(|e| format!("could not start rpiboot: {e}"))?;
    *state.0.lock().map_err(|_| "Pi USB state poisoned")? = Some(child);

    let app2 = app.clone();
    tauri::async_runtime::spawn(async move {
        let mut code = -1;
        let mut hinted = false;
        while let Some(event) = rx.recv().await {
            match event {
                CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                    let text = String::from_utf8_lossy(&bytes);
                    for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                        let _ = app2.emit("hub:pi-usb", line.to_string());
                        // The udev fix is Linux-only, so only look for (and
                        // surface) it there — on macOS a device-open failure means
                        // something else, and this hint would mislead. Surface it
                        // once instead of leaving a cryptic error.
                        if !hinted && cfg!(target_os = "linux") {
                            if let Some(hint) = hub_core::hub_usbboot::access_denied_hint(line) {
                                let _ = app2.emit("hub:pi-usb-hint", hint.to_string());
                                hinted = true;
                            }
                        }
                    }
                }
                CommandEvent::Terminated(payload) => {
                    code = payload.code.unwrap_or(-1);
                }
                _ => {}
            }
        }
        if let Some(s) = app2.try_state::<PiUsbState>() {
            if let Ok(mut guard) = s.0.lock() {
                *guard = None;
            }
        }
        let _ = app2.emit("hub:pi-usb-done", code);
    });
    Ok(())
}

/// Stop waiting for a Pi (the operator closed the panel or changed their
/// mind). Killing a waiting rpiboot is harmless — nothing has been served yet.
#[tauri::command]
pub fn hub_pi_boot_stop(state: tauri::State<'_, PiUsbState>) -> Result<(), String> {
    if let Some(child) = state.0.lock().map_err(|_| "Pi USB state poisoned")?.take() {
        child
            .kill()
            .map_err(|e| format!("couldn't stop rpiboot: {e}"))?;
    }
    Ok(())
}

/// The whole pipeline, in order, in a blocking worker: download → verify →
/// decompress → authorize → write → read-back → Wi-Fi seed → eject. Progress
/// streams over `hub:progress` ({stage, done, total}) and human lines over
/// `hub:log`; the Wi-Fi values themselves are never logged.
#[tauri::command]
#[allow(clippy::too_many_arguments)]
pub async fn hub_flash(
    app: AppHandle,
    state: tauri::State<'_, HubFlashState>,
    board_id: String,
    disk_path: String,
    confirmed: bool,
    wifi: Option<HubWifi>,
    account: Option<HubAccount>,
) -> Result<HubReceipt, String> {
    let cancel = hub_io::CancelToken::default();
    {
        let mut guard = state.0.lock().map_err(|_| "hub flash state poisoned")?;
        if guard.is_some() {
            return Err("a hub flash is already running".to_string());
        }
        *guard = Some(cancel.clone());
    }
    let result = tauri::async_runtime::spawn_blocking(move || {
        hub_flash_blocking(&app, board_id, disk_path, confirmed, wifi, account, &cancel)
    })
    .await
    .map_err(|e| format!("hub writer worker failed: {e}"));
    if let Ok(mut guard) = state.0.lock() {
        *guard = None;
    }
    result?
}

/// The staging directory the pipeline downloads and decompresses into. Uses
/// the OS temp dir; the cached `.xz` lives in the app cache dir instead so it
/// survives between runs.
fn staging_dir() -> std::path::PathBuf {
    std::env::temp_dir()
}

/// Sweep files a crash or pulled plug could have orphaned: the ~2.5 GB raw
/// images we stage in the temp dir (whose RaiiPath cleanup can't run if the
/// process is killed) and any half-finished `.partial` downloads in the image
/// cache. Best-effort and quiet — a file we can't remove is simply skipped.
/// Run once at startup, before this process flashes anything of its own, so it
/// never races its own work.
pub fn cleanup_orphans(app: &AppHandle) {
    // Raw staging images: securacv-hub-<pid>.img in the OS temp dir.
    if let Ok(entries) = std::fs::read_dir(staging_dir()) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with("securacv-hub-") && name.ends_with(".img") {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    // Half-finished downloads: <image>.partial in the image cache dir.
    if let Ok(dir) = app.path().app_cache_dir() {
        if let Ok(entries) = std::fs::read_dir(dir.join("hub-images")) {
            for e in entries.flatten() {
                if e.file_name()
                    .to_string_lossy()
                    .ends_with(hub_io::fetch::PARTIAL_SUFFIX)
                {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
    }
}

/// A filesystem-safe cache filename for an image URL — the last path segment,
/// which for HAOS assets is already the unique `haos_<board>-<ver>.img.xz`.
fn cache_name(url: &str) -> String {
    url.rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .map(|s| {
            s.chars()
                .map(|c| {
                    if c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == '_' {
                        c
                    } else {
                        '-'
                    }
                })
                .collect()
        })
        .unwrap_or_else(|| "hub-image.img.xz".to_string())
}

/// Preflight the flash: is there room to stage the download + decompressed
/// image, and what platform are we on (so the UI can pre-warn about, e.g., the
/// macOS disk-permission prompt). Cheap and read-only.
#[tauri::command]
pub fn hub_preflight() -> Result<HubPreflight, String> {
    // The full-stack raw image is ~2.5 GB and the compressed download ~0.5 GB;
    // ask for 6 GB of headroom so decompression + the OS both breathe.
    const NEED: u64 = 6 * 1024 * 1024 * 1024;
    let free = free_space_bytes(&staging_dir()).unwrap_or(u64::MAX);
    Ok(HubPreflight {
        staging_free_bytes: free,
        staging_need_bytes: NEED,
        staging_ok: free >= NEED,
        platform: std::env::consts::OS.to_string(),
    })
}

#[cfg(unix)]
fn free_space_bytes(path: &std::path::Path) -> Option<u64> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    let c = CString::new(path.as_os_str().as_bytes()).ok()?;
    // SAFETY: statvfs writes into a zeroed struct; we read scalar fields only.
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut s) } != 0 {
        return None;
    }
    // f_bavail/f_frsize are u64 on Linux and u32 on macOS — go through u128 so
    // the multiply can't overflow and the cast is right on both.
    Some((u128::from(s.f_bavail) * u128::from(s.f_frsize)).min(u128::from(u64::MAX)) as u64)
}

#[cfg(not(unix))]
fn free_space_bytes(_path: &std::path::Path) -> Option<u64> {
    None
}

/// Poll whether a just-flashed hub is answering yet, so the UI can close the
/// loop across the long silent first boot instead of abandoning the user.
/// Returns true once the host responds with any HTTP status (HA serving a
/// login page, a redirect, anything) — reachability is the signal, not 200.
#[tauri::command]
pub async fn hub_probe_hub(host: String) -> Result<bool, String> {
    // Guard the host: only a plain hostname[:port], no scheme/path, so this
    // can't be steered into probing an arbitrary URL.
    let host = host.trim();
    if host.is_empty()
        || host.contains('/')
        || host.contains(' ')
        || !host
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == ':')
    {
        return Err("that doesn't look like a hostname".to_string());
    }
    let url = format!("http://{host}/");
    let client = reqwest::Client::builder()
        .connect_timeout(std::time::Duration::from_secs(3))
        .timeout(std::time::Duration::from_secs(5))
        .build()
        .map_err(|e| e.to_string())?;
    // Any HTTP response at all — 200, a redirect, even a 401 — means HA is up
    // and serving. Only a transport failure (refused/timeout) means "not yet".
    Ok(client.get(&url).send().await.is_ok())
}

/// Stop the running flash. Safe at every stage: before the write nothing has
/// touched the card; during the write the card was being erased anyway and
/// just needs a fresh flash. The pipeline stops within one chunk.
#[tauri::command]
pub fn hub_flash_cancel(state: tauri::State<'_, HubFlashState>) -> Result<(), String> {
    if let Some(token) = state
        .0
        .lock()
        .map_err(|_| "hub flash state poisoned")?
        .as_ref()
    {
        token.cancel();
    }
    Ok(())
}

fn hub_flash_blocking(
    app: &AppHandle,
    board_id: String,
    disk_path: String,
    confirmed: bool,
    wifi: Option<HubWifi>,
    account: Option<HubAccount>,
    cancel: &hub_io::CancelToken,
) -> Result<HubReceipt, String> {
    let log = |line: String| {
        let _ = app.emit("hub:log", line);
    };
    let progress_app = app.clone();
    let mut progress = move |p: hub_io::Progress| {
        let _ = progress_app.emit(
            "hub:progress",
            serde_json::json!({
                "stage": p.stage.name(),
                "done": p.done,
                "total": p.total,
            }),
        );
    };

    // 0) Platform gate — fail before ANY work. Disk detection works everywhere
    //    (hub-core enumerates + classifies on Linux/macOS/Windows), but the raw
    //    disk *writer* only exists on Linux and macOS today. Stop here, before
    //    the hundreds-of-MB download and ~2.5 GB decompress, rather than deep in
    //    the write step — an operator on an unsupported OS gets an instant,
    //    honest answer instead of a wasted wait. (`open_target` enforces the
    //    same limit at the write itself, as belt-and-suspenders.)
    if !hub_io::write::write_backend_available() {
        return Err(
            "Writing a Home Assistant hub card isn't supported on this operating system yet — \
             detecting your disk works, but the card writer is currently Linux and macOS only. \
             Please run the flasher on a Mac or Linux computer to flash the hub."
                .to_string(),
        );
    }

    // 1) Resolve the plan from the embedded catalog.
    let (_, view) = parse_catalog()?;
    let plan = resolve(&view, &board_id).map_err(|e| e.message())?;
    log(format!(
        "→ plan: {} for {board_id}, image {}",
        plan.os_label, plan.image_url
    ));

    // 2) Re-enumerate and find the chosen disk NOW — never trust a stale
    //    picker row; the disk must still exist and still classify eligible
    //    (authorize_write re-checks classify as well).
    let target = enumerate()?
        .into_iter()
        .find(|d| d.path == disk_path)
        .ok_or_else(|| format!("{disk_path} is no longer connected — was it unplugged?"))?;

    // Mint the connection UUID once, up front — the keyfile needs a stable
    // uuid= or HAOS re-imports it with a fresh identity (and possibly a fresh
    // IP) every boot. If randomness somehow fails we still flash: NetworkManager
    // assigns a UUID on load, we just lose the cross-boot stability.
    let wifi_uuid = hub_io::seed::uuid_v4().ok();

    // Validate the Wi-Fi seed BEFORE any download or write: a typo'd
    // passphrase should fail in one second, not after two GB.
    if let Some(w) = wifi.as_ref() {
        let seed = WifiSeed {
            ssid: &w.ssid,
            passphrase: &w.passphrase,
            connection_id: "securacv-hub",
            uuid: wifi_uuid.as_deref(),
            hidden: w.hidden,
        };
        hub_core::hub_seed::wifi_keyfile(&seed).map_err(|e| e.message())?;
    }
    // Mint the account store up front too (opt-in) so a bad password fails
    // instantly, before any bytes move. The hash is computed here, on this
    // computer; the cleartext is dropped when `account` goes out of scope.
    let account_files = match account.as_ref() {
        Some(a) => mint_owner_store(&OwnerAccount {
            name: &a.name,
            username: &a.username,
            password: &a.password,
        })?,
        None => Vec::new(),
    };

    // 3) Get the image — reuse a locally cached, RE-VERIFIED copy when we have
    //    one (skips the network, never the trust check), else download it.
    //    One automatic retry on a network hiccup; a cancel is never retried.
    let published = if plan.expected_sha256.is_none() {
        hub_io::fetch::fetch_published_sha256(&plan.image_url)?
    } else {
        None
    };

    let cache_dir = app
        .path()
        .app_cache_dir()
        .map(|d| d.join("hub-images"))
        .ok();
    let cached_path = cache_dir
        .as_ref()
        .map(|d| d.join(cache_name(&plan.image_url)));

    // Try the cache first: hash the local file and see if it still verifies
    // against the plan.
    let mut used_cache = false;
    let (xz_path, xz_is_temp, verified) = 'image: {
        if let Some(cp) = cached_path.as_ref().filter(|p| p.exists()) {
            log("→ found a local copy — re-verifying it instead of downloading…".to_string());
            match hub_io::fetch::sha256_file(cp, cancel) {
                // A cancel during the hash is a cancel, not a bad cache: stop
                // cleanly and KEEP the file.
                Err(e) if e.starts_with(hub_io::CANCELLED) => return Err(e),
                Err(_) => {
                    // Couldn't even read it — treat as absent and re-download.
                    log("→ couldn't read the local copy; downloading a fresh one".to_string());
                    let _ = std::fs::remove_file(cp);
                }
                Ok(sha) => {
                    match hub_core::hub_image::verify_download(&plan, &sha, published.as_deref()) {
                        Ok(v) => {
                            log("✓ local copy verified — skipping the download".to_string());
                            used_cache = true;
                            break 'image (cp.clone(), false, v);
                        }
                        // No checksum to verify against (e.g. offline + unpinned) is
                        // TRANSIENT — don't destroy a possibly-good cache over it.
                        Err(hub_core::hub_image::VerifyError::NoChecksumAvailable) => {
                            return Err(
                            "you're offline and this image isn't pinned yet, so the local copy \
                             can't be verified — connect to the internet once and try again (your \
                             download is kept)"
                                .to_string(),
                        );
                        }
                        // A genuine content mismatch: the cache is bad, replace it.
                        Err(e) => {
                            log(format!(
                                "→ the local copy didn't match ({}); downloading a fresh one",
                                e.message()
                            ));
                            let _ = std::fs::remove_file(cp);
                        }
                    }
                }
            }
        }

        // Download to the cache location when we have one (so it survives for
        // next time), else to a temp file we must clean up ourselves.
        let is_temp = cache_dir.is_none();
        let dest = match cache_dir.as_ref() {
            Some(d) => {
                let _ = std::fs::create_dir_all(d);
                d.join(cache_name(&plan.image_url))
            }
            None => staging_dir().join(cache_name(&plan.image_url)),
        };
        log(format!("→ downloading {}", plan.image_url));
        let dl = match hub_io::fetch::download(&plan.image_url, &dest, cancel, &mut progress) {
            Ok(dl) => dl,
            Err(e) if !e.starts_with(hub_io::CANCELLED) => {
                log(format!(
                    "→ the download tripped over its shoelaces ({e}) — dusting off and trying once more…"
                ));
                hub_io::fetch::download(&plan.image_url, &dest, cancel, &mut progress)?
            }
            Err(e) => return Err(e),
        };
        log(format!(
            "✓ downloaded {} bytes · SHA-256 {}…",
            dl.bytes,
            &dl.sha256[..16]
        ));
        let v = hub_core::hub_image::verify_download(&plan, &dl.sha256, published.as_deref())
            .map_err(|e| {
                // A bad download shouldn't poison the cache for next time.
                let _ = std::fs::remove_file(&dest);
                e.message()
            })?;
        break 'image (dest, is_temp, v);
    };
    // A .xz downloaded to the OS temp dir (no app cache dir available) is ours
    // to clean up; a cached one is kept for reuse. The raw image is always
    // temporary. Both delete on drop — success path and every `?` below.
    let _xz_cleanup = CleanupPath {
        path: xz_path.clone(),
        active: xz_is_temp,
    };
    log(format!(
        "✓ image verified against {}",
        if plan.expected_sha256.is_some() {
            "the repo-pinned checksum"
        } else {
            "Home Assistant's published checksum"
        }
    ));

    // 5) Decompress to a raw image in a private temp file (it's ~2.5 GB — the
    //    cached .xz is the artifact worth keeping, not the expanded image).
    let raw_path = CleanupPath {
        path: staging_dir().join(format!("securacv-hub-{}.img", std::process::id())),
        active: true,
    };
    let raw = hub_io::xz::decompress(&xz_path, &raw_path.path, cancel, &mut progress)?;
    log(format!("✓ raw image ready: {} bytes", raw.bytes));
    if raw.bytes > target.size_bytes {
        return Err(format!(
            "the raw image ({} bytes) is larger than {} ({} bytes)",
            raw.bytes, target.path, target.size_bytes
        ));
    }

    // 6) The gate. The download/decompress window is long and /dev paths get
    //    reused, so re-resolve the disk NOW: it must still be present, still
    //    classify eligible (authorize_write re-checks), and still be the SAME
    //    device the operator confirmed — not another stick that inherited the
    //    path mid-download.
    let target = {
        let fresh = enumerate()?
            .into_iter()
            .find(|d| d.path == disk_path)
            .ok_or_else(|| {
                format!("{disk_path} disappeared while the image was being prepared — was it unplugged?")
            })?;
        if fresh.model != target.model
            || fresh.size_bytes != target.size_bytes
            || fresh.removable != target.removable
            || fresh.external != target.external
            || fresh.system != target.system
        {
            return Err(format!(
                "{disk_path} now reports a different device than the one confirmed \
                 ({} → {}) — replug the intended card and start again",
                target.model, fresh.model
            ));
        }
        fresh
    };
    let authz = authorize_write(plan, &verified, &target, confirmed).map_err(|e| e.message())?;

    // 7) The destructive write + full read-back.
    log(format!(
        "→ writing to {} — do not remove the card…",
        target.path
    ));
    // On macOS the write opens the raw disk through Apple's `authopen`, which
    // pops a Touch ID / password prompt the instant the write starts. Announce
    // it a beat BEFORE it appears — and name the unfamiliar "authopen" — so it
    // reads as expected, not sketchy. Emitted before the blocking write_image
    // call, so the UI shows the cue while authopen waits on the operator.
    #[cfg(target_os = "macos")]
    {
        let _ = app.emit("hub:mac-auth", true);
        log(
            "→ macOS is about to ask for Touch ID or your password. That's the 'authopen' \
             prompt — Apple's built-in helper we use to write the card without running the app \
             as administrator (the same way Raspberry Pi Imager does). Approve it to continue."
                .to_string(),
        );
    }
    let receipt =
        hub_io::write::write_image(authz, &raw_path.path, &raw.sha256, cancel, &mut progress)?;
    log("✓ written and read back — the card verifiably holds the image".to_string());

    // 8) Seed the card in ONE mount — Wi-Fi and (opt-in) the account store —
    //    then eject. From here the card is verifiably GOOD, so a seeding
    //    stumble is a note with a plan B, never a failure.
    let wifi_seed = wifi.as_ref().map(|w| WifiSeed {
        ssid: &w.ssid,
        passphrase: &w.passphrase,
        connection_id: "securacv-hub",
        uuid: wifi_uuid.as_deref(),
        hidden: w.hidden,
    });
    let want_wifi = wifi_seed.is_some();
    let want_account = !account_files.is_empty();

    // eject_note is tracked SEPARATELY from seed success and surfaced on its
    // own always-shown receipt field: a card whose settings wrote fine but
    // wouldn't cleanly unmount still needs a "eject it yourself before pulling
    // it" instruction, or the operator may yank a still-mounted card before
    // the CONFIG writes flush.
    let (wifi_seeded, wifi_note, account_seeded, mut account_note, mut eject_note) =
        if want_wifi || want_account {
            match hub_io::seed::seed_card(
                &receipt.target_path,
                wifi_seed.as_ref(),
                &account_files,
                &mut progress,
            ) {
                Ok(o) => {
                    if o.wifi_written {
                        log(
                            "✓ Wi-Fi seeded — the hub will join your network on first boot"
                                .to_string(),
                        );
                    }
                    if o.account_written {
                        log(
                            "✓ account seeded (experimental) — first visit should be a login page"
                                .to_string(),
                        );
                    }
                    if let Some(ej) = o.eject_note.as_ref() {
                        log(format!("→ settings written, but: {ej}"));
                    }
                    (
                        o.wifi_written,
                        o.wifi_note,
                        o.account_written,
                        o.account_note,
                        o.eject_note,
                    )
                }
                Err(e) => {
                    // Couldn't even mount — the card is still good; say so plainly.
                    log(format!(
                        "→ the image is on the card and verified, but the card couldn't be \
                         re-mounted to add settings: {e}"
                    ));
                    (
                        false,
                        if want_wifi { Some(e.clone()) } else { None },
                        false,
                        if want_account { Some(e) } else { None },
                        None,
                    )
                }
            }
        } else {
            log("→ no Wi-Fi or account to seed (wired ethernet assumed)".to_string());
            true_eject(&receipt.target_path, &log);
            (false, None, false, None, None)
        };

    // A headless hub that never joins a network is not a successful flash.
    // This step used to be best-effort — "a seeding stumble is a note, NEVER a
    // failure" — which is right for a convenience and wrong for the ONE thing
    // that makes a keyboard-less, screen-less Pi reachable. Seeded Wi-Fi IS the
    // product here: without it the card boots to `wlan0: (No address)` and sits
    // on the Home Assistant landing page forever, while the app reports
    // "Done — written, read back, and verified." Fail now, while the card is
    // still in the reader and the person is still standing there.
    if want_wifi && !wifi_seeded {
        return Err(format!(
            // Deliberately avoids the words the UI's error classifier greps for
            // (network / timed out / connection): this is a POST-write failure,
            // and being mistaken for a mid-download stumble would tell the
            // operator "your card is untouched" when the card is in fact
            // complete and verified.
            "The image is written and verified, but your Wi-Fi could not be saved onto the card, \
             so the hub would start up with nothing to join and never answer at \
             homeassistant.local. Nothing is broken — the card is good. Flash again (the second \
             pass almost always mounts fine), or use ethernet for the first boot and set Wi-Fi \
             inside Home Assistant afterwards. Reason: {}",
            wifi_note.as_deref().unwrap_or("unknown")
        ));
    }

    // Turn a raw eject stumble into a plain, alarming-enough instruction.
    if let Some(ej) = eject_note.take() {
        eject_note = Some(format!(
            "Your settings are on the card, but it wouldn't auto-eject — please eject it in your \
             file manager before removing it, so nothing is left half-written. ({ej})"
        ));
    }

    // Turn the raw seed notes into calm, actionable copy. A failed WI-FI seed
    // no longer reaches here at all — it returns Err above, because a hub that
    // can't join a network is a failed flash, not a flash with a footnote. The
    // ACCOUNT seed is genuinely optional (Home Assistant's own setup wizard is
    // the fallback), so it keeps the note-and-carry-on treatment.
    if want_account && !account_seeded {
        account_note = Some({
            format!(
                "The card is perfect and (if set) your Wi-Fi is on it — only the experimental \
                 account pre-seed didn't apply, so first boot will show Home Assistant's normal \
                 setup wizard. ({})",
                account_note.as_deref().unwrap_or("unknown reason")
            )
        });
    } else if want_account && account_seeded {
        account_note = Some(
            "Experimental: we wrote your account onto the card. On first boot, tell us whether \
             homeassistant.local:8123 showed a login page (success) or the setup wizard."
                .to_string(),
        );
    }

    Ok(HubReceipt {
        board_id: receipt.board_id,
        os_label: receipt.os_label,
        target_path: receipt.target_path,
        bytes_written: receipt.bytes_written,
        sha256: receipt.sha256,
        wifi_seeded,
        wifi_note,
        account_seeded,
        account_note,
        eject_note,
        used_cache,
    })
}

/// A path that deletes its file when dropped — used for the ~2.5 GB raw image
/// so it never lingers, on the happy path or any early return.
/// A path whose file is deleted on drop when `active` — used for the ~2.5 GB
/// raw image (always) and a temp-dir `.xz` fallback (only when there's no
/// persistent cache dir to keep it in). Cleans up on the happy path and on
/// every early return alike.
struct CleanupPath {
    path: std::path::PathBuf,
    active: bool,
}
impl Drop for CleanupPath {
    fn drop(&mut self) {
        if self.active {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

/// Best-effort eject for the wired-ethernet path (the seed path ejects as part
/// of its own flow). Failure is a log line, not an error — the write already
/// verified.
fn true_eject(device: &str, log: &impl Fn(String)) {
    #[cfg(target_os = "macos")]
    let result = std::process::Command::new("diskutil")
        .args(["eject", device])
        .output();
    #[cfg(target_os = "linux")]
    let result = std::process::Command::new("udisksctl")
        .args(["power-off", "-b", device])
        .output();
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    let result: std::io::Result<std::process::Output> = Err(std::io::Error::other("unsupported"));

    match result {
        Ok(out) if out.status.success() => log(format!("✓ {device} ejected — safe to remove")),
        _ => log(format!(
            "→ couldn't auto-eject {device}; eject it in your file manager before removing"
        )),
    }
}
