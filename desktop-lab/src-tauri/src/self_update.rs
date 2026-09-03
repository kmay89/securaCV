// The Lab's self-update routine (desktop only — iOS/iPadOS builds update
// through the App Store, and the updater plugin doesn't exist there).
//
// Shape copied from the Flasher (desktop/src-tauri/src/lib.rs), as
// RELEASE_LESSONS 2026-07-27 prescribed: poll the rolling `lab-latest`
// prerelease pointer (NEVER `releases/latest` — that URL belongs to the
// firmware the fleet polls), verify the signed installer it names, and ask the user —
// with the release's own notes — before anything installs. Every check and
// install is appended to a local update journal, so what the app did is
// visible and recoverable, never silent.

use std::io::Write as _;
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons};
use tauri_plugin_updater::UpdaterExt;

/// First check shortly after launch (let the window paint first), then
/// routinely while the app stays open — a Lab left running for days must
/// still hear about updates.
pub const FIRST_CHECK_DELAY: Duration = Duration::from_secs(15);
pub const RECHECK_EVERY: Duration = Duration::from_secs(6 * 60 * 60);

/// A "Later" is an answer: a declined version isn't re-asked for this long
/// (persisted on disk, so a relaunch doesn't turn into a nag), and any NEWER
/// version asks straight away. Bounded on purpose — with no in-app updates
/// UI yet, an eternal decline would strand someone who changes their mind.
const DECLINE_SNOOZE: Duration = Duration::from_secs(7 * 24 * 60 * 60);

/// One update conversation at a time — held by the routine check AND by the
/// Settings panel's `install_update`, so the two can never race two
/// `install` operations over the same app bundle.
#[derive(Default)]
pub struct UpdateGate {
    busy: bool,
    /// True only while the updater is REPLACING the app bundle on disk.
    /// The quit guards in lib.rs read this: being killed inside that window
    /// is the one thing that can leave the Lab unable to open at all
    /// (RELEASE_LESSONS 2026-07-31). Deliberately NOT set during the
    /// download — a slow connection must never read as "don't quit".
    installing: bool,
}

/// Whether an update is overwriting the app bundle right now — what the
/// close/exit guards in lib.rs ask before letting a quit through.
pub fn install_in_progress(app: &AppHandle) -> bool {
    app.try_state::<Mutex<UpdateGate>>()
        .map(|gate| gate.lock().map(|g| g.installing).unwrap_or(false))
        .unwrap_or(false)
}

fn set_installing(app: &AppHandle, value: bool) {
    if let Some(gate) = app.try_state::<Mutex<UpdateGate>>() {
        if let Ok(mut g) = gate.lock() {
            g.installing = value;
        }
    }
}

/// Download, then install, journaling each act. Split on purpose (the
/// Flasher's shape): the download touches nothing but a buffer, so being
/// interrupted there costs a re-download and nothing else. Only `install`
/// moves the app bundle on disk, so only `install` is wrapped in the
/// `installing` flag the quit guards read.
async fn download_then_install(
    app: &AppHandle,
    update: tauri_plugin_updater::Update,
) -> Result<(), String> {
    let version = update.version.clone();
    journal(app, &format!("downloading v{version}…"));
    let bytes = update.download(|_, _| {}, || {}).await.map_err(|e| {
        let msg = format!("update download failed: {e}");
        journal(app, &msg);
        msg
    })?;
    journal(app, &format!("installing v{version}…"));
    set_installing(app, true);
    let outcome = update.install(bytes);
    // Cleared on failure too: an install that returned an error unwound and
    // put the bundle back, so there is nothing mid-write left to protect.
    set_installing(app, false);
    outcome.map_err(|e| {
        let msg = format!("update install failed: {e}");
        journal(app, &msg);
        msg
    })?;
    journal(app, &format!("installed v{version} — relaunching"));
    Ok(())
}

/// What an update offer looks like to the frontend seam (same DTO shape as
/// the Flasher's `check_update`).
#[derive(serde::Serialize)]
pub struct UpdateDto {
    version: String,
    current_version: String,
    notes: Option<String>,
}

// KEEP IN LOCKSTEP with `desktop/src-tauri/src/launch_guard.rs`
// (`epoch_secs` / `utc_stamp`): the Flasher's launch guard carries a
// byte-for-byte copy of these two functions (two crates, no shared crate yet,
// and a helper crate for two tiny functions isn't worth its ceremony). A fix
// to either copy belongs in both.
fn epoch_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// `YYYY-MM-DD HH:MM:SSZ` from the system clock, no chrono dependency.
/// Civil-from-days per Howard Hinnant's algorithm — plenty for a journal.
fn utc_stamp() -> String {
    let secs = epoch_secs();
    let (days, rem) = (secs / 86_400, secs % 86_400);
    let (h, m, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if mo <= 2 { y + 1 } else { y };
    format!("{y:04}-{mo:02}-{d:02} {h:02}:{m:02}:{s:02}Z")
}

/// Append one line to the local update journal (`update-journal.log` in the
/// app's data dir), and mirror it over the `update:log` event for any
/// frontend that wants to show it.
fn journal(app: &AppHandle, line: &str) {
    let stamped = format!("{} {}", utc_stamp(), line);
    let _ = app.emit("update:log", stamped.clone());
    if let Ok(dir) = app.path().app_data_dir() {
        let _ = std::fs::create_dir_all(&dir);
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(dir.join("update-journal.log"))
        {
            let _ = writeln!(f, "{stamped}");
        }
    }
}

/// Read the update journal back off disk, newest line first, for the Settings
/// panel's "log of updates". Capped so a long-lived install can't hand the
/// frontend an unbounded string; the full file stays on disk either way, and
/// the panel names its path so it can always be opened directly.
///
/// A missing journal is not an error — it means nothing has happened yet.
#[tauri::command]
pub fn read_update_journal(app: AppHandle) -> Result<Vec<String>, String> {
    const MAX_LINES: usize = 200;
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data dir: {e}"))?;
    let path = dir.join("update-journal.log");
    let text = match std::fs::read_to_string(&path) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(format!("could not read the update journal: {e}")),
    };
    let mut lines: Vec<String> = text.lines().filter(|l| !l.trim().is_empty()).map(str::to_string).collect();
    lines.reverse();
    lines.truncate(MAX_LINES);
    Ok(lines)
}

/// Where that journal lives, so the panel can show the path (and so a support
/// question is answerable without guessing per-platform data dirs).
#[tauri::command]
pub fn update_journal_path(app: AppHandle) -> Result<String, String> {
    app.path()
        .app_data_dir()
        .map(|d| d.join("update-journal.log").display().to_string())
        .map_err(|e| format!("no app data dir: {e}"))
}

/// The declined-version marker: `<version> <epoch-secs>`, in the app data
/// dir next to the journal, so a "Later" survives a relaunch.
fn declined_path(app: &AppHandle) -> Option<std::path::PathBuf> {
    app.path().app_data_dir().ok().map(|d| d.join("update-declined"))
}

fn read_declined(app: &AppHandle) -> Option<(String, u64)> {
    let text = std::fs::read_to_string(declined_path(app)?).ok()?;
    let mut parts = text.split_whitespace();
    let version = parts.next()?.to_string();
    let at = parts.next()?.parse().ok()?;
    Some((version, at))
}

fn write_declined(app: &AppHandle, version: &str) {
    if let Some(path) = declined_path(app) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let _ = std::fs::write(path, format!("{version} {}\n", epoch_secs()));
    }
}

/// Ask the release channel whether a newer signed build exists (frontend
/// seam — the routine below uses the same underlying check). `None` = current.
///
/// Journals every outcome, exactly as the routine pass does. The Settings
/// panel promises "every check and install this app has done", so a
/// user-initiated check that left no trace would make that copy a lie — and
/// an offline failure is the single most useful line the journal can carry.
#[tauri::command]
pub async fn check_update(app: AppHandle) -> Result<Option<UpdateDto>, String> {
    let updater = app.updater().map_err(|e| {
        let msg = format!("update check unavailable: {e}");
        journal(&app, &msg);
        msg
    })?;
    match updater.check().await {
        Ok(Some(update)) => {
            journal(
                &app,
                &format!(
                    "checked — v{} is ready (running v{})",
                    update.version, update.current_version
                ),
            );
            Ok(Some(UpdateDto {
                version: update.version.clone(),
                current_version: update.current_version.clone(),
                notes: update.body.clone(),
            }))
        }
        Ok(None) => {
            journal(
                &app,
                &format!("checked — v{} is current", env!("CARGO_PKG_VERSION")),
            );
            Ok(None)
        }
        Err(e) => {
            let msg = format!("update check failed: {e}");
            journal(&app, &msg);
            Err(msg)
        }
    }
}

/// Download and install the pending update, journaling the act, then
/// relaunch into the new version.
///
/// Holds the same [`UpdateGate`] as `routine_check`: without it, a
/// Settings-panel "Update & relaunch" could race a routine-check dialog's
/// install — two `install` operations over the same app bundle. Whoever got
/// there first owns the conversation; the loser gets told, not queued.
#[tauri::command]
pub async fn install_update(app: AppHandle) -> Result<(), String> {
    {
        let gate = app.state::<Mutex<UpdateGate>>();
        let mut g = gate.lock().unwrap();
        if g.busy {
            return Err("an update conversation is already in progress".to_string());
        }
        g.busy = true;
    }
    let done = |app: &AppHandle| {
        let gate = app.state::<Mutex<UpdateGate>>();
        gate.lock().unwrap().busy = false;
    };

    let updater = match app.updater() {
        Ok(u) => u,
        Err(e) => {
            let msg = format!("update check unavailable: {e}");
            journal(&app, &msg);
            done(&app);
            return Err(msg);
        }
    };
    let update = match updater.check().await {
        Ok(Some(u)) => u,
        Ok(None) => {
            done(&app);
            return Err("already up to date".to_string());
        }
        Err(e) => {
            let msg = format!("update check failed: {e}");
            journal(&app, &msg);
            done(&app);
            return Err(msg);
        }
    };
    match download_then_install(&app, update).await {
        // `restart()` diverges (`!`), so the gate needs no release on success.
        Ok(()) => app.restart(),
        Err(msg) => {
            done(&app);
            Err(msg)
        }
    }
}

/// Markdown release notes → the plain text a native dialog can show:
/// keep the `- ` bullets, drop the `**bold**` markers, cap the length.
fn notes_for_dialog(notes: &str) -> String {
    let text = notes.replace("**", "");
    let trimmed = text.trim();
    let mut out: String = trimmed.chars().take(1200).collect();
    if out.len() < trimmed.len() {
        out.push('…');
    }
    out
}

/// One routine pass: check, and if something newer is signed and ready, ask
/// the user (with the release's notes) and install on yes. Quiet on every
/// other outcome — offline or "already current" never interrupts anyone.
pub async fn routine_check(app: AppHandle) {
    {
        let gate = app.state::<Mutex<UpdateGate>>();
        let mut g = gate.lock().unwrap();
        if g.busy {
            return;
        }
        g.busy = true;
    }
    let done = |app: &AppHandle| {
        let gate = app.state::<Mutex<UpdateGate>>();
        gate.lock().unwrap().busy = false;
    };

    let updater = match app.updater() {
        Ok(u) => u,
        Err(e) => {
            journal(&app, &format!("update check unavailable: {e}"));
            return done(&app);
        }
    };
    let update = match updater.check().await {
        Ok(Some(u)) => u,
        Ok(None) => {
            journal(
                &app,
                &format!("checked — v{} is current", env!("CARGO_PKG_VERSION")),
            );
            return done(&app);
        }
        Err(e) => {
            // Offline, or no release published yet — routine checks stay quiet.
            journal(&app, &format!("update check failed (will retry): {e}"));
            return done(&app);
        }
    };

    let version = update.version.clone();
    journal(
        &app,
        &format!("v{version} is ready (running v{})", update.current_version),
    );
    if let Some((declined, at)) = read_declined(&app) {
        // The user already said "later" to exactly this version, recently —
        // honor that across launches instead of nagging. A newer version, or
        // the snooze running out, asks again.
        if declined == version
            && epoch_secs().saturating_sub(at) < DECLINE_SNOOZE.as_secs()
        {
            return done(&app);
        }
    }

    let mut body = format!(
        "SecuraCV Lab {version} is ready (you have {}).",
        update.current_version
    );
    if let Some(notes) = update.body.as_deref().filter(|n| !n.trim().is_empty()) {
        body.push_str("\n\nWhat's changing:\n");
        body.push_str(&notes_for_dialog(notes));
    }
    body.push_str("\n\nThe update is signed and verified before it installs.");

    let app_for_dialog = app.clone();
    let install = tauri::async_runtime::spawn_blocking(move || {
        app_for_dialog
            .dialog()
            .message(body)
            .title("A Lab update is ready")
            .buttons(MessageDialogButtons::OkCancelCustom(
                "Update & relaunch".into(),
                "Later".into(),
            ))
            .blocking_show()
    })
    .await
    .unwrap_or(false);

    if !install {
        journal(&app, &format!("v{version} offered — user chose later"));
        write_declined(&app, &version);
        return done(&app);
    }

    match download_then_install(&app, update).await {
        Ok(()) => app.restart(),
        // download_then_install already journaled the failure.
        Err(_) => done(&app),
    }
}
