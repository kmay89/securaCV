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
}

/// The Wi-Fi the operator typed (the same secret the Canary flasher already
/// collects). Values never appear in logs or events.
#[derive(Deserialize)]
pub struct HubWifi {
    ssid: String,
    passphrase: String,
    hidden: bool,
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
        while let Some(event) = rx.recv().await {
            match event {
                CommandEvent::Stdout(bytes) | CommandEvent::Stderr(bytes) => {
                    let text = String::from_utf8_lossy(&bytes);
                    for line in text.split(['\r', '\n']).filter(|l| !l.trim().is_empty()) {
                        let _ = app2.emit("hub:pi-usb", line.to_string());
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
pub async fn hub_flash(
    app: AppHandle,
    state: tauri::State<'_, HubFlashState>,
    board_id: String,
    disk_path: String,
    confirmed: bool,
    wifi: Option<HubWifi>,
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
        hub_flash_blocking(&app, board_id, disk_path, confirmed, wifi, &cancel)
    })
    .await
    .map_err(|e| format!("hub writer worker failed: {e}"));
    if let Ok(mut guard) = state.0.lock() {
        *guard = None;
    }
    result?
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

    // Validate the Wi-Fi seed BEFORE any download or write: a typo'd
    // passphrase should fail in one second, not after two GB.
    if let Some(w) = wifi.as_ref() {
        let seed = WifiSeed {
            ssid: &w.ssid,
            passphrase: &w.passphrase,
            connection_id: "securacv-hub",
            uuid: None,
            hidden: w.hidden,
        };
        hub_core::hub_seed::wifi_keyfile(&seed).map_err(|e| e.message())?;
    }

    // 3) Download the image, hashing as it streams. One automatic retry on a
    //    network hiccup — the internet drops packets, users shouldn't have to
    //    care — but a cancel is a cancel, never retried.
    let staging = tempfile::tempdir().map_err(|e| format!("couldn't create a staging dir: {e}"))?;
    let xz_path = staging.path().join("haos.img.xz");
    log(format!("→ downloading {}", plan.image_url));
    let dl = match hub_io::fetch::download(&plan.image_url, &xz_path, cancel, &mut progress) {
        Ok(dl) => dl,
        Err(e) if !e.starts_with(hub_io::CANCELLED) => {
            log(format!(
                "→ the download tripped over its shoelaces ({e}) — dusting off and trying once more…"
            ));
            hub_io::fetch::download(&plan.image_url, &xz_path, cancel, &mut progress)?
        }
        Err(e) => return Err(e),
    };
    log(format!(
        "✓ downloaded {} bytes · SHA-256 {}…",
        dl.bytes,
        &dl.sha256[..16]
    ));

    // 4) The trust call. Pinned catalog: repo-vouched hash is authoritative.
    //    Unpinned: HA's published .sha256 must match. Neither: refuse.
    let published = if plan.expected_sha256.is_none() {
        hub_io::fetch::fetch_published_sha256(&plan.image_url)?
    } else {
        None
    };
    let verified = hub_core::hub_image::verify_download(&plan, &dl.sha256, published.as_deref())
        .map_err(|e| e.message())?;
    log(format!(
        "✓ image verified against {}",
        if plan.expected_sha256.is_some() {
            "the repo-pinned checksum"
        } else {
            "Home Assistant's published checksum"
        }
    ));

    // 5) Decompress to the raw image; its hash is what the card must hold.
    let raw_path = staging.path().join("haos.img");
    let raw = hub_io::xz::decompress(&xz_path, &raw_path, cancel, &mut progress)?;
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
    let receipt = hub_io::write::write_image(authz, &raw_path, &raw.sha256, cancel, &mut progress)?;
    log("✓ written and read back — the card verifiably holds the image".to_string());

    // 8) Seed Wi-Fi onto the boot partition, then eject. From here on the
    //    card is verifiably GOOD — a seeding stumble must never demote the
    //    receipt to a failure. It becomes a note with the plan B instead.
    let (wifi_seeded, wifi_note) = if let Some(w) = wifi {
        let seed = WifiSeed {
            ssid: &w.ssid,
            passphrase: &w.passphrase,
            connection_id: "securacv-hub",
            uuid: None,
            hidden: w.hidden,
        };
        match hub_io::seed::seed_wifi(&receipt.target_path, &seed, &mut progress) {
            Ok(_) => {
                log("✓ Wi-Fi seeded — the hub will join your network on first boot".to_string());
                (true, None)
            }
            Err(e) => {
                log(format!(
                    "→ the image is on the card and verified, but the Wi-Fi seed didn't stick: {e}"
                ));
                (
                    false,
                    Some(format!(
                        "The card itself is perfect — only the Wi-Fi note didn't make it on. \
                         Easiest fixes: plug in ethernet for the first boot, or replug the card \
                         and flash again. ({e})"
                    )),
                )
            }
        }
    } else {
        log("→ no Wi-Fi seeded (wired ethernet assumed)".to_string());
        true_eject(&receipt.target_path, &log);
        (false, None)
    };

    Ok(HubReceipt {
        board_id: receipt.board_id,
        os_label: receipt.os_label,
        target_path: receipt.target_path,
        bytes_written: receipt.bytes_written,
        sha256: receipt.sha256,
        wifi_seeded,
        wifi_note,
    })
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
