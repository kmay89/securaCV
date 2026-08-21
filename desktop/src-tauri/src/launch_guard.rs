//! The launch guard: **the Flasher must always be able to open.**
//!
//! A force quit is not a clean exit, and this app leaves three things behind
//! when it is killed — each of which can turn the *next* launch into a Dock
//! icon that bounces forever with no window and no explanation:
//!
//! 1. **Orphaned sidecars.** `espflash` and `rpiboot` are spawned as ordinary
//!    child processes. SIGKILL to us does not reach them; they are reparented
//!    and keep running — and `rpiboot` in particular is *designed* to wait
//!    forever for a Pi. They hold the serial/USB device the next run needs.
//!    The old startup sweep ([`crate::hub::cleanup_orphans`]) reclaimed
//!    orphaned *files* only; nothing ever reclaimed orphaned *processes*.
//! 2. **A half-written webview store.** The frontend reads `localStorage`
//!    synchronously before first paint (`src/app.js`). WebKit backs that with
//!    SQLite; killed mid-write it can leave a store whose recovery wedges the
//!    web content process, so the window never paints.
//! 3. **A half-applied self-update.** Installing an update moves the app
//!    bundle itself. On macOS (`tauri-plugin-updater` 2.10.1) the ordinary
//!    path is two `rename`s — the running `.app` out to a temp backup, the new
//!    one in — so a kill between them leaves *no app at that path* rather than
//!    a torn one; the privileged path (`rm -rf && mv` via AppleScript) is not
//!    atomic and can leave a partial bundle that macOS refuses to finish
//!    launching. Either way the user is stuck and nothing inside the app can
//!    undo it, because the thing that would run the repair is the thing that
//!    moved. All this module can do is *recognize* it afterwards and say so —
//!    which beats a silent bounce, and is why the marker lives in the app data
//!    dir and not in the bundle.
//!
//! Only the second of those is repairable from here. The first is preventable
//! and reclaimable; the third is only ever nameable.
//!
//! So this module writes a breadcrumb as each launch advances, and reads the
//! previous one on the way up. The important part is that all of it happens in
//! [`begin`] — **before** `tauri::Builder`, before any window exists — because
//! the failure we are guarding against is precisely the one where no window
//! ever appears and the app cannot report anything from inside itself.
//!
//! Nothing here is best-effort-and-silent: every decision is appended to
//! `launch.log` in the app data dir, so "it just bounces" always has an
//! answer on disk.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons};
use tauri_plugin_shell::process::{Command, CommandChild, CommandEvent};

/// The bundle identifier, duplicated from `tauri.conf.json` on purpose: this
/// module runs before Tauri exists, so it cannot ask Tauri where things live.
const IDENTIFIER: &str = "com.securacv.flasher";

/// How long the frontend gets to finish `boot()` and call `ui_ready` before we
/// stop assuming it will. Generous — a cold start on a slow disk with a big
/// catalog is a few seconds — but far short of the user's patience.
const LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

/// Where a launch got to. Written to disk as it advances, so the *next* launch
/// can see exactly where the last one died.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Stage {
    /// The process is up; Tauri has not handed us a window yet.
    #[default]
    Starting,
    /// Tauri built the window and ran our setup hook.
    WindowUp,
    /// The frontend finished booting and said so. The app is usable.
    UiReady,
    /// The process exited through the event loop, on purpose.
    Clean,
}

/// One sidecar this run spawned and has not yet seen exit.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Sidecar {
    pub pid: u32,
    /// The sidecar's executable name (`espflash`, `rpiboot`) — what we match
    /// a live process against before killing anything.
    pub name: String,
}

/// The breadcrumb one launch leaves for the next.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct LaunchRecord {
    pub pid: u32,
    pub version: String,
    pub started_at: u64,
    pub stage: Stage,
    /// Set while a self-update is overwriting this bundle. Finding this in a
    /// *previous* run's record means that overwrite was interrupted.
    pub installing: Option<String>,
    /// Sidecars spawned and not yet reaped.
    pub sidecars: Vec<Sidecar>,
    /// How many launches in a row have already cleared the webview store
    /// trying to fix a hang. Stops a reset loop from repeating forever when
    /// the store was never the problem.
    pub auto_resets: u32,
}

/// What the previous launch's breadcrumb means. Ordered by severity: an
/// interrupted update outranks everything, because it is the only one the app
/// cannot fix for itself.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// The last run exited cleanly, or this is a first run. Say nothing.
    Clean,
    /// The last run was killed while overwriting this bundle with an update.
    /// This copy of the app may be incomplete; only a reinstall fixes it.
    UpdateInterrupted { version: String },
    /// The last run was killed before its window ever became usable — the
    /// bounce-with-no-window case.
    HungAtLaunch { stage: Stage, auto_resets: u32 },
    /// The last run was killed while it was up and working. Ordinary force
    /// quit; the only fallout is whatever it had spawned.
    KilledWhileRunning,
}

/// Read a previous record and decide what it means. Pure, so the decision is
/// testable without a filesystem, a process table, or a window.
///
/// `running_version` is the version *this* binary was built as, and it is what
/// makes an interrupted-install marker trustworthy. The marker alone only says
/// an install was in flight; whether it landed is answered by what is running
/// now. If we are already the version that was being installed, it finished —
/// or the user reinstalled into it — and there is nothing to warn about. Only
/// a marker naming a version we are *not* running means the swap didn't
/// complete. Without this, the first launch after a successful-but-unrecorded
/// install would tell the user to reinstall the copy they are already running.
pub fn verdict_for(previous: Option<&LaunchRecord>, running_version: &str) -> Verdict {
    let Some(prev) = previous else {
        return Verdict::Clean;
    };
    if let Some(target) = prev.installing.as_deref().filter(|v| *v != running_version) {
        return Verdict::UpdateInterrupted {
            version: target.to_string(),
        };
    }
    match prev.stage {
        Stage::Clean => Verdict::Clean,
        Stage::UiReady => Verdict::KilledWhileRunning,
        stage @ (Stage::Starting | Stage::WindowUp) => Verdict::HungAtLaunch {
            stage,
            auto_resets: prev.auto_resets,
        },
    }
}

// ── is the previous run actually over? ───────────────────────────────────────

/// Whether the process that wrote the previous record is *still running* as
/// this same app — i.e. we are a second instance, not a relaunch.
///
/// This gates every recovery action. A second Flasher (`open -n`, a Linux
/// launcher, a stray double-click) reads the first one's record, and every
/// sidecar listed in it is alive *because the first instance is still using
/// it*. Reaping those would abort a running flash; clearing the webview store
/// would pull it out from under a live window. Neither is a mistake worth
/// making, so an ambiguous answer means do nothing.
///
/// A PID equal to our own is not another instance — it is a number the OS
/// handed back to us after the old process died, so recovery proceeds.
pub fn owner_is_alive(
    previous: &LaunchRecord,
    live_exe: Option<&str>,
    self_exe: Option<&str>,
    self_pid: u32,
) -> bool {
    if previous.pid == 0 || previous.pid == self_pid {
        return false;
    }
    match (live_exe, self_exe) {
        (Some(live), Some(mine)) => live == mine,
        _ => false,
    }
}

// ── orphaned sidecars ────────────────────────────────────────────────────────

/// Whether the live process at this PID is the sidecar we recorded.
///
/// We compare the *executable*, never the PID alone: PIDs are reused, and
/// killing a stranger's process because it inherited a number is not a trade
/// this app makes. `None` — no such process, or an OS that won't tell us —
/// always means "leave it alone".
pub fn is_our_sidecar(record: &Sidecar, live_exe: Option<&str>) -> bool {
    live_exe
        .map(Path::new)
        .and_then(Path::file_name)
        .and_then(|n| n.to_str())
        .is_some_and(|n| n == record.name)
}

/// The sidecars from a previous run that are still alive and still ours.
/// Split out from the killing so it can be tested with a fake process table.
pub fn orphans_of(
    previous: &LaunchRecord,
    exe_of: impl Fn(u32) -> Option<String>,
) -> Vec<&Sidecar> {
    previous
        .sidecars
        .iter()
        .filter(|s| is_our_sidecar(s, exe_of(s.pid).as_deref()))
        .collect()
}

/// The executable path behind a PID, when the OS will tell us. `None` means
/// "no such process" *or* "can't tell" — both of which mean don't kill it.
#[cfg(target_os = "macos")]
fn exe_of(pid: u32) -> Option<String> {
    // `proc_pidpath` lives in libproc, which is part of libSystem — no extra
    // link flag needed. It returns the absolute path of the running image.
    const MAXSIZE: usize = 4096;
    let mut buf = vec![0u8; MAXSIZE];
    let written = unsafe {
        libc::proc_pidpath(
            pid as libc::c_int,
            buf.as_mut_ptr() as *mut libc::c_void,
            buf.len() as u32,
        )
    };
    if written <= 0 {
        return None;
    }
    buf.truncate(written as usize);
    String::from_utf8(buf).ok()
}

#[cfg(target_os = "linux")]
fn exe_of(pid: u32) -> Option<String> {
    std::fs::read_link(format!("/proc/{pid}/exe"))
        .ok()
        .map(|p| p.to_string_lossy().into_owned())
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn exe_of(_pid: u32) -> Option<String> {
    None
}

#[cfg(unix)]
fn kill_pid(pid: u32) -> bool {
    // SIGKILL, not SIGTERM: espflash and rpiboot are already orphaned and have
    // no cleanup we need. Neither can damage a board by dying — the ESP32
    // bootloader is mask ROM and a half-written card is simply re-flashed.
    unsafe { libc::kill(pid as libc::pid_t, libc::SIGKILL) == 0 }
}

#[cfg(not(unix))]
fn kill_pid(_pid: u32) -> bool {
    false
}

// ── the webview store, and clearing it ───────────────────────────────────────

/// Every path a reset may remove: the webview's own storage, and nothing else.
/// Our launch journal sits beside these and is never touched — it is the
/// evidence for what went wrong.
///
/// Each path is required to contain the bundle identifier, which is what keeps
/// a bad `home` from turning this into a much bigger delete than intended.
pub fn webview_state_paths(home: &Path, identifier: &str) -> Vec<PathBuf> {
    let candidates: Vec<PathBuf> = if cfg!(target_os = "macos") {
        vec![
            home.join("Library/WebKit").join(identifier),
            home.join("Library/Caches").join(identifier),
            home.join("Library/HTTPStorages").join(identifier),
        ]
    } else {
        // webkit2gtk keeps its website data under the app's data dir, in
        // well-known subdirectories. Name them explicitly rather than removing
        // the directory wholesale, which also holds this journal.
        let data = home.join(".local/share").join(identifier);
        let mut v: Vec<PathBuf> = ["localstorage", "databases", "indexeddb", "ServiceWorkers"]
            .iter()
            .map(|sub| data.join(sub))
            .collect();
        v.push(home.join(".cache").join(identifier));
        v
    };
    candidates
        .into_iter()
        .filter(|p| p.to_string_lossy().contains(identifier))
        .collect()
}

/// Remove the webview's stored state. Returns what it actually removed, for
/// the journal — a reset that quietly did nothing is worse than no reset.
fn wipe_webview_state(home: &Path) -> Vec<PathBuf> {
    webview_state_paths(home, IDENTIFIER)
        .into_iter()
        .filter(|p| p.exists() && std::fs::remove_dir_all(p).is_ok())
        .collect()
}

// ── time, borrowed from the Lab's update journal ─────────────────────────────
// KEEP IN LOCKSTEP with `desktop-lab/src-tauri/src/self_update.rs`
// (`epoch_secs` / `utc_stamp`): byte-for-byte the same two functions (two
// crates, no shared crate yet). A fix to either copy belongs in both.

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

/// The app data dir, derived the same way Tauri derives it from the bundle
/// identifier — computed here because this runs before Tauri is built.
fn state_dir() -> Option<PathBuf> {
    let home = home_dir()?;
    Some(if cfg!(target_os = "macos") {
        home.join("Library/Application Support").join(IDENTIFIER)
    } else {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".local/share"))
            .join(IDENTIFIER)
    })
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
}

// ── the guard itself ─────────────────────────────────────────────────────────

/// The live launch record, plus everything the app needs to keep it honest.
pub struct LaunchGuard {
    record_path: Option<PathBuf>,
    log_path: Option<PathBuf>,
    record: Mutex<LaunchRecord>,
    ready: AtomicBool,
    /// What the previous launch's breadcrumb said. Decided once, on the way
    /// up, before anything else could overwrite it.
    verdict: Verdict,
    /// Webview state this launch cleared on its own, if any.
    auto_reset: Vec<PathBuf>,
}

impl LaunchGuard {
    /// Read the previous breadcrumb, act on it, and start a new one.
    ///
    /// `home` and `dir` are parameters so the whole sequence can be tested
    /// against a temp directory; [`begin`] supplies the real ones.
    fn open_at(dir: Option<PathBuf>, home: Option<PathBuf>, version: &str) -> Self {
        let record_path = dir.as_ref().map(|d| d.join("launch-state.json"));
        let log_path = dir.as_ref().map(|d| d.join("launch.log"));
        if let Some(d) = &dir {
            let _ = std::fs::create_dir_all(d);
        }

        let previous: Option<LaunchRecord> = record_path
            .as_ref()
            .and_then(|p| std::fs::read_to_string(p).ok())
            // A record from a future version we can't parse is not a crash
            // report — treat it as no record rather than failing the launch.
            .and_then(|text| serde_json::from_str(&text).ok());

        // Is that record a *previous* run's, or a *concurrent* one's? If the
        // process that wrote it is still alive as this same app, we are a
        // second instance and every recovery below would be sabotage: its
        // sidecars are live because it is using them. Stand down entirely.
        let self_exe = std::env::current_exe()
            .ok()
            .map(|p| p.to_string_lossy().into_owned());
        let sharing_with_a_live_instance = previous.as_ref().is_some_and(|prev| {
            owner_is_alive(
                prev,
                exe_of(prev.pid).as_deref(),
                self_exe.as_deref(),
                std::process::id(),
            )
        });
        let previous = if sharing_with_a_live_instance {
            None
        } else {
            previous
        };
        let verdict = verdict_for(previous.as_ref(), version);

        let guard = LaunchGuard {
            record_path,
            log_path,
            record: Mutex::new(LaunchRecord {
                pid: std::process::id(),
                version: version.to_string(),
                started_at: epoch_secs(),
                stage: Stage::Starting,
                installing: None,
                sidecars: Vec::new(),
                auto_resets: 0,
            }),
            ready: AtomicBool::new(false),
            verdict: verdict.clone(),
            auto_reset: Vec::new(),
        };

        guard.log(&format!(
            "launch: pid={} v{version} — last exit: {}",
            std::process::id(),
            if sharing_with_a_live_instance {
                "still running in another instance — standing down".to_string()
            } else {
                describe(&verdict)
            }
        ));

        // Reclaim anything the last run stranded. Processes first: a stuck
        // espflash holding the board has to be gone *before* the UI enumerates
        // ports, or the user is told their board is missing.
        if let Some(prev) = previous.as_ref() {
            for orphan in orphans_of(prev, exe_of) {
                let killed = kill_pid(orphan.pid);
                guard.log(&format!(
                    "reaped orphaned {} (pid {}) from the last run: {}",
                    orphan.name,
                    orphan.pid,
                    if killed { "killed" } else { "could not kill" }
                ));
            }
        }

        let mut guard = guard;

        // A launch that never reached a usable window is the loop we are here
        // to break: clear the webview store *now*, before Tauri creates the
        // webview that would hold it open. Prefs are a nicety by design (no
        // secrets, nothing that can't be re-entered), so this is cheap. Do it
        // once: if the previous run already tried this and still hung, the
        // store was never the problem and wiping again would just loop.
        if let (Verdict::HungAtLaunch { stage, auto_resets }, Some(home)) =
            (&guard.verdict, home.as_ref())
        {
            if *auto_resets == 0 {
                let removed = wipe_webview_state(home);
                guard.log(&format!(
                    "last launch never got past {stage:?} — cleared webview state: {}",
                    if removed.is_empty() {
                        "nothing was there".to_string()
                    } else {
                        removed
                            .iter()
                            .map(|p| p.display().to_string())
                            .collect::<Vec<_>>()
                            .join(", ")
                    }
                ));
                guard.auto_reset = removed;
                if let Ok(mut r) = guard.record.lock() {
                    r.auto_resets = auto_resets + 1;
                }
            } else {
                guard.log(&format!(
                    "last launch never got past {stage:?} even after {auto_resets} \
                     reset(s) — not clearing again; the store is not the problem"
                ));
                if let Ok(mut r) = guard.record.lock() {
                    r.auto_resets = auto_resets + 1;
                }
            }
        }

        guard.persist();
        guard
    }

    /// What the previous launch's breadcrumb said.
    pub fn verdict(&self) -> &Verdict {
        &self.verdict
    }

    /// Whether the frontend has reported itself usable yet.
    pub fn is_ready(&self) -> bool {
        self.ready.load(Ordering::SeqCst)
    }

    /// Advance the recorded stage.
    pub fn note(&self, stage: Stage) {
        if stage == Stage::UiReady {
            self.ready.store(true, Ordering::SeqCst);
        }
        if let Ok(mut r) = self.record.lock() {
            r.stage = stage;
            // Reaching a usable window is what clears the "we tried resetting"
            // counter — not a clean exit. Somebody who force quits every time
            // never produces a clean exit, and their next real problem must
            // still get its one repair attempt.
            if stage == Stage::UiReady {
                r.auto_resets = 0;
            }
        }
        self.log(&format!("stage: {stage:?}"));
        self.persist();
    }

    /// Remember a sidecar we just spawned, so the next launch can kill it if
    /// this one is killed first.
    pub fn track(&self, pid: u32, name: &str) {
        if let Ok(mut r) = self.record.lock() {
            r.sidecars.push(Sidecar {
                pid,
                name: name.to_string(),
            });
        }
        self.persist();
    }

    /// Forget a sidecar that exited on its own.
    pub fn forget(&self, pid: u32) {
        if let Ok(mut r) = self.record.lock() {
            r.sidecars.retain(|s| s.pid != pid);
        }
        self.persist();
    }

    /// Open the window during which this bundle is being overwritten. Anything
    /// that kills us in here leaves a bundle that only a reinstall can fix, so
    /// the marker has to be on disk *before* the first byte is written.
    pub fn begin_install(&self, version: &str) {
        if let Ok(mut r) = self.record.lock() {
            r.installing = Some(version.to_string());
        }
        self.log(&format!("installing v{version} — overwriting this bundle"));
        self.persist();
    }

    /// Close it again. Called on success and on failure alike: a *failed*
    /// install that returned cleanly did not leave a torn bundle.
    pub fn end_install(&self) {
        if let Ok(mut r) = self.record.lock() {
            r.installing = None;
        }
        self.log("install window closed");
        self.persist();
    }

    /// Note that a quit was refused because an install was in flight. Worth a
    /// line: "Cmd-Q did nothing" is otherwise indistinguishable from a hang.
    pub fn log_quit_blocked(&self) {
        self.log("quit refused — an install is writing the app bundle");
    }

    /// Whether a self-update is overwriting the bundle right now.
    pub fn is_installing(&self) -> bool {
        self.record
            .lock()
            .map(|r| r.installing.is_some())
            .unwrap_or(false)
    }

    /// The user asked for a reset by hand. Re-arm the automatic one so the
    /// next launch actually performs it: the once-only cap exists to stop a
    /// silent loop, and a person clicking a button is not a loop.
    pub fn request_reset(&self) {
        if let Ok(mut r) = self.record.lock() {
            r.auto_resets = 0;
        }
        self.log("reset requested by the user — the next launch will clear the store");
        self.persist();
    }

    /// Record a clean exit, so the next launch says nothing at all.
    pub fn mark_clean(&self) {
        if let Ok(mut r) = self.record.lock() {
            r.stage = Stage::Clean;
            r.sidecars.clear();
            r.auto_resets = 0;
        }
        self.log("stage: Clean — exited normally");
        self.persist();
    }

    fn persist(&self) {
        let Some(path) = &self.record_path else {
            return;
        };
        let Ok(record) = self.record.lock() else {
            return;
        };
        if let Ok(text) = serde_json::to_string_pretty(&*record) {
            let _ = std::fs::write(path, text);
        }
    }

    /// Append one line to `launch.log`. Best-effort by design: a journal we
    /// can't write must never be the reason the app won't start.
    fn log(&self, line: &str) {
        let Some(path) = &self.log_path else { return };
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
        {
            let _ = writeln!(f, "{} {line}", utc_stamp());
        }
    }
}

/// One-line English for a verdict, for the journal.
fn describe(v: &Verdict) -> String {
    match v {
        Verdict::Clean => "clean".to_string(),
        Verdict::UpdateInterrupted { version } => {
            format!("killed while installing v{version}")
        }
        Verdict::HungAtLaunch { stage, auto_resets } => {
            format!("never became usable (stopped at {stage:?}, {auto_resets} prior reset(s))")
        }
        Verdict::KilledWhileRunning => "killed while running".to_string(),
    }
}

/// Start the guard. Call this first thing in `run()`, before `tauri::Builder`:
/// the whole point is to act on a broken previous launch *before* creating the
/// webview that a broken store would wedge.
pub fn begin() -> Arc<LaunchGuard> {
    Arc::new(LaunchGuard::open_at(
        state_dir(),
        home_dir(),
        env!("CARGO_PKG_VERSION"),
    ))
}

// ── what the user sees ───────────────────────────────────────────────────────

/// Tell the user what the last launch did, when there is something worth
/// saying. Runs off the main thread (native dialogs block).
///
/// Only two verdicts speak up. An ordinary force quit says nothing — the
/// orphans are already reaped and there is nothing left to decide.
pub fn report(app: &AppHandle, guard: &Arc<LaunchGuard>) {
    let (title, body, offer_releases) = match guard.verdict() {
        Verdict::UpdateInterrupted { version } => (
            "That update didn't finish",
            format!(
                "The Flasher was being replaced with version {version} when it was \
                 force quit, and you're still running {}. Installing moves the app \
                 itself, so being stopped part-way can leave a copy that won't open \
                 at all.\n\n\
                 If it's opening now, you're fine — just update again when you're \
                 ready. If it starts refusing to open, reinstall from the latest \
                 download: that's the one thing the app can't fix for itself, \
                 because the repair would have to run from the copy that moved.\n\n\
                 Nothing you've flashed is affected. A Canary keeps the firmware it \
                 already has, and the board can't be bricked.",
                env!("CARGO_PKG_VERSION")
            ),
            true,
        ),
        Verdict::HungAtLaunch { .. } if !guard.auto_reset.is_empty() => (
            "Recovered from a bad shutdown",
            "The last time the Flasher started it never finished opening — a force \
             quit had left its saved session mid-write. That's now cleared and the \
             app is running normally.\n\n\
             You'll need to re-enter the things it used to remember (the Wi-Fi \
             network name and device names you last used). It never stored a \
             password, so nothing secret was lost."
                .to_string(),
            false,
        ),
        Verdict::HungAtLaunch { auto_resets, .. } if *auto_resets > 0 => (
            "The Flasher is having trouble starting",
            format!(
                "This is launch {} in a row that didn't finish opening, and clearing \
                 the saved session didn't help — so that isn't the cause.\n\n\
                 Reinstalling from the latest download is the next step. \
                 launch.log in the app's data folder has the details.",
                auto_resets + 1
            ),
            true,
        ),
        _ => return,
    };

    let app = app.clone();
    let guard = Arc::clone(guard);
    std::thread::spawn(move || {
        guard.log(&format!("told the user: {title}"));
        let buttons = if offer_releases {
            MessageDialogButtons::OkCancelCustom("Get the latest version".into(), "Not now".into())
        } else {
            MessageDialogButtons::OkCustom("OK".into())
        };
        let clicked = app
            .dialog()
            .message(body)
            .title(title)
            .buttons(buttons)
            .blocking_show();
        if offer_releases && clicked {
            crate::open_releases_page(&app);
        }
    });
}

/// Watch this launch actually become usable, and say something if it doesn't.
///
/// This is the second line of defense. The first — clearing a wedged store on
/// the way up — handles the case we know how to fix. This one catches the case
/// we don't: after [`LAUNCH_TIMEOUT`] with no `ui_ready`, offer the user the
/// same reset by hand rather than leaving them with a bouncing icon.
pub fn watch(app: &AppHandle, guard: &Arc<LaunchGuard>) {
    let app = app.clone();
    let guard = Arc::clone(guard);
    std::thread::spawn(move || {
        std::thread::sleep(LAUNCH_TIMEOUT);
        if guard.is_ready() {
            return;
        }
        guard.log(&format!(
            "watchdog: no ui_ready after {}s — offering a reset",
            LAUNCH_TIMEOUT.as_secs()
        ));
        let restart = app
            .dialog()
            .message(
                "The Flasher's window hasn't finished loading. Almost always this is \
                 a saved session left mid-write by a force quit.\n\n\
                 Clearing it and reopening usually fixes it. You'd re-enter the Wi-Fi \
                 network name and device names you last used — no password was ever \
                 stored.",
            )
            .title("Still opening…")
            .buttons(MessageDialogButtons::OkCancelCustom(
                "Clear and reopen".into(),
                "Keep waiting".into(),
            ))
            .blocking_show();
        if !restart {
            guard.log("watchdog: user chose to keep waiting");
            return;
        }
        // Leave the record showing a launch that never got there. The next
        // process reads exactly that and clears the store before it builds a
        // webview — the one moment when nothing is holding those files open.
        guard.request_reset();
        guard.log("watchdog: user asked for a reset — restarting");
        app.restart();
    });
}

// ── tracked sidecars ─────────────────────────────────────────────────────────

/// A spawned sidecar's entry in the launch record, removed when this drops.
///
/// Holding one of these for as long as the child runs is what makes the record
/// true: a sidecar is listed exactly while it might still be alive.
pub struct Ticket {
    guard: Option<Arc<LaunchGuard>>,
    pid: u32,
}

impl Drop for Ticket {
    fn drop(&mut self) {
        if let Some(g) = &self.guard {
            g.forget(self.pid);
        }
    }
}

/// Spawn a sidecar and record its PID for the next launch.
///
/// Every sidecar spawn goes through here. The alternative — a bare
/// `cmd.spawn()` — is what stranded `rpiboot` on the user's USB bus with
/// nothing to clean it up.
pub fn spawn_tracked(
    app: &AppHandle,
    cmd: Command,
    name: &str,
) -> Result<
    (
        tauri::async_runtime::Receiver<CommandEvent>,
        CommandChild,
        Ticket,
    ),
    String,
> {
    let (rx, child) = cmd.spawn().map_err(|e| {
        // A sidecar that won't exec at all reports in the OS's vocabulary
        // ("Bad CPU type in executable (os error 86)"), which tells the
        // operator neither what is wrong nor what to do. Translate that one
        // class here — every sidecar spawn goes through this function, so
        // espflash gets the same treatment as rpiboot. The raw error is kept
        // so a bug report still carries the real cause.
        let raw = e.to_string();
        match hub_core::hub_sidecar::arch_mismatch_hint(&raw) {
            Some(hint) => format!("could not start {name}: {hint} ({raw})"),
            None => format!("could not start {name}: {raw}"),
        }
    })?;
    let pid = child.pid();
    let guard = app.try_state::<Arc<LaunchGuard>>().map(|s| Arc::clone(&s));
    if let Some(g) = &guard {
        g.track(pid, name);
    }
    Ok((rx, child, Ticket { guard, pid }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record(stage: Stage) -> LaunchRecord {
        LaunchRecord {
            pid: 42,
            version: "0.3.7".into(),
            started_at: 1_753_900_000,
            stage,
            installing: None,
            sidecars: Vec::new(),
            auto_resets: 0,
        }
    }

    #[test]
    fn no_previous_record_is_a_clean_launch() {
        assert_eq!(verdict_for(None, "0.3.7"), Verdict::Clean);
        assert_eq!(
            verdict_for(Some(&record(Stage::Clean)), "0.3.7"),
            Verdict::Clean
        );
    }

    #[test]
    fn a_kill_before_the_ui_was_ready_is_a_hang_and_a_kill_after_is_not() {
        // The distinction the whole module turns on: dying at Starting or
        // WindowUp is the bounce-with-no-window loop and gets a reset; dying
        // at UiReady is an ordinary force quit and must not.
        assert_eq!(
            verdict_for(Some(&record(Stage::Starting)), "0.3.7"),
            Verdict::HungAtLaunch {
                stage: Stage::Starting,
                auto_resets: 0
            }
        );
        assert_eq!(
            verdict_for(Some(&record(Stage::WindowUp)), "0.3.7"),
            Verdict::HungAtLaunch {
                stage: Stage::WindowUp,
                auto_resets: 0
            }
        );
        assert_eq!(
            verdict_for(Some(&record(Stage::UiReady)), "0.3.7"),
            Verdict::KilledWhileRunning
        );
    }

    #[test]
    fn an_interrupted_install_outranks_the_stage_it_died_at() {
        let mut r = record(Stage::UiReady);
        r.installing = Some("0.3.8".into());
        assert_eq!(
            verdict_for(Some(&r), "0.3.7"),
            Verdict::UpdateInterrupted {
                version: "0.3.8".into()
            }
        );
    }

    #[test]
    fn an_install_marker_naming_the_version_we_are_running_is_not_a_warning() {
        // The marker says an install was in flight; the running version says
        // whether it landed. Getting this wrong tells someone to reinstall the
        // copy they are already using — which is the state right after a
        // *successful* update whose marker never got cleared.
        let mut r = record(Stage::UiReady);
        r.installing = Some("0.3.8".into());
        assert_eq!(verdict_for(Some(&r), "0.3.8"), Verdict::KilledWhileRunning);
        assert_eq!(
            verdict_for(Some(&r), "0.3.7"),
            Verdict::UpdateInterrupted {
                version: "0.3.8".into()
            }
        );
    }

    #[test]
    fn a_second_instance_leaves_the_first_instance_alone() {
        let mut prev = record(Stage::UiReady);
        prev.pid = 4242;
        let me = "/Applications/SecuraCV Flasher.app/Contents/MacOS/securacv-flasher";

        // The record's owner is alive, running this same app: we are a second
        // instance and its sidecars are live because it is *using* them.
        assert!(owner_is_alive(&prev, Some(me), Some(me), 99));
        // Owner is gone.
        assert!(!owner_is_alive(&prev, None, Some(me), 99));
        // PID reused by something else entirely.
        assert!(!owner_is_alive(&prev, Some("/usr/bin/vim"), Some(me), 99));
        // The OS won't say what we are — never guess in the direction of
        // killing another instance's work.
        assert!(!owner_is_alive(&prev, Some(me), None, 99));
        // The PID came back around to us, so there is no other instance.
        assert!(!owner_is_alive(&prev, Some(me), Some(me), 4242));
        // A record with no owner recorded at all.
        prev.pid = 0;
        assert!(!owner_is_alive(&prev, Some(me), Some(me), 99));
    }

    #[test]
    fn a_sidecar_is_only_ours_when_the_live_executable_matches() {
        let rec = Sidecar {
            pid: 900,
            name: "espflash".into(),
        };
        assert!(is_our_sidecar(
            &rec,
            Some("/Applications/SecuraCV Flasher.app/Contents/MacOS/espflash")
        ));
        // PID reuse: alive, but it's somebody else's process now.
        assert!(!is_our_sidecar(&rec, Some("/usr/bin/vim")));
        // Dead, or an OS that won't say. Never kill on a guess.
        assert!(!is_our_sidecar(&rec, None));
    }

    #[test]
    fn orphans_are_the_live_ones_only() {
        let mut prev = record(Stage::UiReady);
        prev.sidecars = vec![
            Sidecar {
                pid: 1,
                name: "espflash".into(),
            },
            Sidecar {
                pid: 2,
                name: "rpiboot".into(),
            },
            Sidecar {
                pid: 3,
                name: "espflash".into(),
            },
        ];
        let table = |pid: u32| match pid {
            2 => Some("/Applications/SecuraCV Flasher.app/Contents/MacOS/rpiboot".to_string()),
            3 => Some("/usr/bin/something-else".to_string()),
            _ => None,
        };
        let orphans = orphans_of(&prev, table);
        assert_eq!(orphans.len(), 1);
        assert_eq!(orphans[0].pid, 2);
    }

    #[test]
    fn a_reset_only_ever_touches_paths_inside_the_bundle_identifier() {
        let home = Path::new("/Users/someone");
        let paths = webview_state_paths(home, IDENTIFIER);
        assert!(!paths.is_empty());
        for p in &paths {
            assert!(
                p.starts_with(home),
                "{} escaped the home directory",
                p.display()
            );
            assert!(
                p.to_string_lossy().contains(IDENTIFIER),
                "{} is not identifier-scoped",
                p.display()
            );
        }
    }

    #[test]
    fn a_launch_record_survives_a_round_trip_through_disk() {
        let mut r = record(Stage::WindowUp);
        r.sidecars.push(Sidecar {
            pid: 7,
            name: "espflash".into(),
        });
        r.auto_resets = 2;
        let text = serde_json::to_string(&r).expect("serialize");
        let back: LaunchRecord = serde_json::from_str(&text).expect("deserialize");
        assert_eq!(back.stage, Stage::WindowUp);
        assert_eq!(back.sidecars, r.sidecars);
        assert_eq!(back.auto_resets, 2);
    }

    #[test]
    fn a_record_from_a_newer_version_does_not_stop_the_launch() {
        // Forward compatibility matters more here than anywhere else in the
        // app: a record we can't read must degrade to "no record", never to a
        // failed startup.
        let text = r#"{"stage":"some-future-stage","pid":1}"#;
        assert!(serde_json::from_str::<LaunchRecord>(text).is_err());
        let parsed: Option<LaunchRecord> = serde_json::from_str(text).ok();
        assert_eq!(verdict_for(parsed.as_ref(), "0.3.7"), Verdict::Clean);
    }

    #[test]
    fn the_first_launch_writes_a_record_and_the_next_one_reads_it() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");
        let home = tmp.path().join("home");

        let first = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        assert_eq!(*first.verdict(), Verdict::Clean);
        first.note(Stage::WindowUp);
        first.note(Stage::UiReady);
        assert!(first.is_ready());
        drop(first);

        // Killed while running: the next launch says so and does not reset.
        let second = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        assert_eq!(*second.verdict(), Verdict::KilledWhileRunning);
        assert!(second.auto_reset.is_empty());
        second.mark_clean();
        drop(second);

        // A clean exit is silent.
        let third = LaunchGuard::open_at(Some(dir), Some(home), "0.3.7");
        assert_eq!(*third.verdict(), Verdict::Clean);
    }

    #[test]
    fn a_hung_launch_resets_once_and_then_stops_resetting() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");
        let home = tmp.path().join("home");

        // A launch that never reports ready leaves Stage::Starting behind.
        drop(LaunchGuard::open_at(
            Some(dir.clone()),
            Some(home.clone()),
            "0.3.7",
        ));

        let second = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        assert!(matches!(
            second.verdict(),
            Verdict::HungAtLaunch { auto_resets: 0, .. }
        ));
        drop(second);

        // Still hung after the reset: the store wasn't the problem, so the
        // third launch must not clear it again — that would be the loop.
        let third = LaunchGuard::open_at(Some(dir), Some(home), "0.3.7");
        assert!(matches!(
            third.verdict(),
            Verdict::HungAtLaunch { auto_resets: 1, .. }
        ));
        assert!(
            third.auto_reset.is_empty(),
            "the second reset attempt must not happen"
        );
    }

    #[test]
    fn asking_for_a_reset_by_hand_is_honored_even_after_the_automatic_one() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");
        let home = tmp.path().join("home");

        // Hang once (auto-repaired), then hang again — the point where the
        // automatic repair has spent itself.
        drop(LaunchGuard::open_at(
            Some(dir.clone()),
            Some(home.clone()),
            "0.3.7",
        ));
        let second = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        second.note(Stage::WindowUp);
        // The watchdog fires and the user clicks "Clear and reopen".
        second.request_reset();
        drop(second);

        // Their click has to actually clear something. Without request_reset
        // this launch would report `auto_resets: 1` and skip the repair.
        let third = LaunchGuard::open_at(Some(dir), Some(home), "0.3.7");
        assert!(matches!(
            third.verdict(),
            Verdict::HungAtLaunch { auto_resets: 0, .. }
        ));
    }

    #[test]
    fn a_launch_that_works_re_arms_the_one_repair_attempt() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");
        let home = tmp.path().join("home");

        // Hang, repair, and this time get all the way up — but exit by being
        // force quit again, never cleanly. That is the whole user population
        // this feature exists for, so the counter has to clear here.
        drop(LaunchGuard::open_at(
            Some(dir.clone()),
            Some(home.clone()),
            "0.3.7",
        ));
        let second = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        second.note(Stage::WindowUp);
        second.note(Stage::UiReady);
        drop(second);

        // A later hang gets its own repair attempt rather than inheriting the
        // earlier one's count and going straight to "reinstall".
        let third = LaunchGuard::open_at(Some(dir.clone()), Some(home.clone()), "0.3.7");
        assert_eq!(*third.verdict(), Verdict::KilledWhileRunning);
        drop(third);
        let fourth = LaunchGuard::open_at(Some(dir), Some(home), "0.3.7");
        assert!(matches!(
            fourth.verdict(),
            Verdict::HungAtLaunch { auto_resets: 0, .. }
        ));
    }

    #[test]
    fn an_install_marker_is_written_before_and_cleared_after() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");

        let g = LaunchGuard::open_at(Some(dir.clone()), None, "0.3.7");
        g.note(Stage::UiReady);
        g.begin_install("0.3.8");
        assert!(g.is_installing());

        // What a force quit mid-install leaves on disk, read back the way the
        // next launch would read it.
        let text = std::fs::read_to_string(dir.join("launch-state.json")).expect("record");
        let mid: LaunchRecord = serde_json::from_str(&text).expect("parse");
        assert_eq!(
            verdict_for(Some(&mid), "0.3.7"),
            Verdict::UpdateInterrupted {
                version: "0.3.8".into()
            }
        );

        g.end_install();
        let text = std::fs::read_to_string(dir.join("launch-state.json")).expect("record");
        let after: LaunchRecord = serde_json::from_str(&text).expect("parse");
        assert_eq!(
            verdict_for(Some(&after), "0.3.7"),
            Verdict::KilledWhileRunning
        );
    }

    #[test]
    fn a_tracked_sidecar_is_listed_while_it_runs_and_gone_after() {
        let tmp = tempfile::tempdir().expect("tempdir");
        let dir = tmp.path().join("state");
        let g = LaunchGuard::open_at(Some(dir.clone()), None, "0.3.7");

        g.track(4242, "rpiboot");
        let text = std::fs::read_to_string(dir.join("launch-state.json")).expect("record");
        let during: LaunchRecord = serde_json::from_str(&text).expect("parse");
        assert_eq!(during.sidecars.len(), 1);
        assert_eq!(during.sidecars[0].name, "rpiboot");

        g.forget(4242);
        let text = std::fs::read_to_string(dir.join("launch-state.json")).expect("record");
        let after: LaunchRecord = serde_json::from_str(&text).expect("parse");
        assert!(after.sidecars.is_empty());
    }
}
