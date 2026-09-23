//! The menubar fleet companion: a tray icon that says how many of your
//! Canaries are online, and a native notification when one of them changes —
//! driven from RUST, so the webview holds no notification grant
//! (capabilities/default.json stays core + opener).
//!
//! Where the facts come from, every 30 s, from a background routine the app
//! starts at launch:
//!   - the mDNS browse (`fleet::fleet_scan`, the Flasher's twin) — a board
//!     that announced itself in this window is heard, which is evidence it
//!     is up; and
//!   - the `/api/fleet` poll (`witness_discover`) over the kernel bases the
//!     frontend hands `companion_set_bases` — the fleet's own coarse report.
//!
//! What it may say is deliberately small. Coarse presence and health words
//! only — online, offline, hub down, chain not ok — from the fields the
//! DISCOVERY.md contract names (`name`, `online`, `chain`, `hub`), and
//! nothing else from the document: never the wellbeing keys, never the
//! device's self-stamped `verified_through`, never anything from the sealed
//! log (this module never fetches it). And never the word "verified": that
//! word belongs to a chain walked against a key pinned at pairing, which the
//! Lab does not hold yet (the signed timeline waits on pairing).
//!
//! A silent field is never a claim (tvos/discovery/DISCOVERY.md). A row whose
//! report did not say `online` reads "hasn't said", a board that stopped
//! announcing reads "not heard lately", and neither ever raises a
//! notification: a missed multicast window or a thin report is not an outage.
//! Only a change between two things the network actually SAID notifies.

use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::Value;
use tauri::menu::{CheckMenuItem, IsMenuItem, Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Manager, State, Wry};
use tauri_plugin_notification::NotificationExt;

/// First poll shortly after launch, then one every `POLL_EVERY`.
pub const FIRST_POLL_DELAY: Duration = Duration::from_secs(5);
pub const POLL_EVERY: Duration = Duration::from_secs(30);
/// The browse window each poll spends listening (the frontend's own value).
const BROWSE_MS: u64 = 2500;
/// A board unheard this long drops off the tray entirely.
const FORGET_AFTER: Duration = Duration::from_secs(10 * 60);
const TRAY_ID: &str = "fleet";
const MAX_BASES: usize = 8;
const MAX_ROWS: usize = 12;
/// Where the companion looks for a kernel until the frontend names one — the
/// same defaults the Witness Wall host starts from (witness-host.js), in the
/// Apple TV's order (tvos WallModel.wellKnownCandidates): the hub convention
/// port, the kernel's own API port, then the bare device.
/// canary-local/tests/desktop_parity.test.js holds the three lists together.
const DEFAULT_BASES: [&str; 3] = [
    "http://canary.local:8099",
    "http://canary.local:8799",
    "http://canary.local",
];
const STATE_FILE: &str = "companion.json";

/// Which report a row came from — it decides how silence reads.
#[derive(Serialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    /// The kernel's `/api/fleet` document.
    Kernel,
    /// A board's own mDNS announcement.
    Mdns,
}

/// One device as the companion understands it. Every field but the name is
/// an `Option`: `None` means the report did not say, and is never read as
/// "fine" or "online".
#[derive(Serialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct DeviceRow {
    pub name: String,
    pub source: Source,
    pub online: Option<bool>,
    pub chain: Option<String>,
    pub hub: Option<String>,
}

/// The whole picture after one poll, keyed `k:<name>` (kernel rows) or
/// `m:<device_id>` (mDNS rows) so the two sources never collide.
#[derive(Serialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct FleetSnapshot {
    pub devices: BTreeMap<String, DeviceRow>,
}

/// A change between two things the network actually said.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Transition {
    Online(String),
    Offline(String),
    ChainNotOk(String, String),
    HubDown(String),
}

impl Transition {
    /// Title and body for the notification — coarse words, framed as the
    /// fleet's own report (it is not verified, and never says it is).
    pub fn words(&self) -> (String, String) {
        match self {
            Transition::Online(n) => (
                format!("{n} is back online"),
                format!("Your fleet reports {n} online again."),
            ),
            Transition::Offline(n) => (
                format!("{n} went offline"),
                format!("Your fleet reports {n} offline."),
            ),
            Transition::ChainNotOk(n, w) => (
                format!("{n}: chain not ok"),
                format!("{n} reports its chain as \"{w}\". Open the Lab to look."),
            ),
            Transition::HubDown(n) => (
                format!("{n} lost its hub"),
                format!("{n} reports its hub as down."),
            ),
        }
    }
}

/// Every change worth a notification between two snapshots. Only rows in
/// BOTH snapshots can change; a row that appeared or vanished is silence on
/// one side, and silence is never a claim.
pub fn transitions(prev: &FleetSnapshot, next: &FleetSnapshot) -> Vec<Transition> {
    let mut out = Vec::new();
    for (key, n) in &next.devices {
        let Some(p) = prev.devices.get(key) else {
            continue;
        };
        match (p.online, n.online) {
            (Some(false), Some(true)) => out.push(Transition::Online(n.name.clone())),
            (Some(true), Some(false)) => out.push(Transition::Offline(n.name.clone())),
            _ => {}
        }
        if let (Some(pc), Some(nc)) = (&p.chain, &n.chain) {
            if pc == "ok" && nc != "ok" {
                out.push(Transition::ChainNotOk(n.name.clone(), nc.clone()));
            }
        }
        if let (Some(ph), Some(nh)) = (&p.hub, &n.hub) {
            if ph != "down" && nh == "down" {
                out.push(Transition::HubDown(n.name.clone()));
            }
        }
    }
    out
}

/// A device name as the network typed it, made safe to show: no control
/// characters, trimmed, at most 40 characters. Empty means "no name".
fn clean_name(raw: &str) -> String {
    let s: String = raw.chars().filter(|c| !c.is_control()).collect();
    s.trim().chars().take(40).collect()
}

/// A status word (`chain`, `hub`) the companion may repeat: short, lowercase
/// ASCII. Anything else is "cannot say" — an unknown value falls back to
/// unknown instead of being misread (DISCOVERY.md).
fn word(v: Option<&Value>) -> Option<String> {
    let s = v?.as_str()?;
    let ok = !s.is_empty()
        && s.len() <= 16
        && s.bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_');
    ok.then(|| s.to_string())
}

/// The kernel rows of one `/api/fleet` answer: `devices`, `canaries`, or a
/// bare array — the shapes witness-host.js and the Wall accept. Reads
/// `name`, `online`, `chain` and `hub`, and nothing else.
pub fn kernel_rows(doc: &Value) -> Vec<(String, DeviceRow)> {
    let list = doc
        .get("devices")
        .or_else(|| doc.get("canaries"))
        .unwrap_or(doc)
        .as_array();
    let mut out = Vec::new();
    for d in list.into_iter().flatten() {
        let name = clean_name(d.get("name").and_then(Value::as_str).unwrap_or(""));
        if name.is_empty() {
            continue; // `name` is the one required field
        }
        out.push((
            format!("k:{name}"),
            DeviceRow {
                name,
                source: Source::Kernel,
                online: d.get("online").and_then(Value::as_bool),
                chain: word(d.get("chain")),
                hub: word(d.get("hub")),
            },
        ));
    }
    out
}

/// Candidate kernel bases, as the frontend sent them: each must pass the
/// same local-host gate `witness_discover` enforces (lib.rs `base_ok`),
/// duplicates dropped, at most `MAX_BASES`.
pub fn accept_bases(bases: Vec<String>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for b in bases {
        let b = b.trim().to_string();
        if crate::base_ok(&b) && !out.contains(&b) {
            out.push(b);
        }
        if out.len() == MAX_BASES {
            break;
        }
    }
    out
}

/// The companion's managed state.
pub struct Companion {
    snapshot: Mutex<Option<FleetSnapshot>>,
    bases: Mutex<Vec<String>>,
    /// mDNS rows by device_id, with when each was last heard.
    heard: Mutex<HashMap<String, (DeviceRow, Instant)>>,
    paused: AtomicBool,
    /// True once the tray icon exists — only then may closing the window
    /// hide it instead of quitting.
    tray_live: AtomicBool,
    told_hidden: AtomicBool,
}

impl Default for Companion {
    fn default() -> Self {
        Companion {
            snapshot: Mutex::new(None),
            bases: Mutex::new(DEFAULT_BASES.iter().map(|b| b.to_string()).collect()),
            heard: Mutex::new(HashMap::new()),
            paused: AtomicBool::new(false),
            tray_live: AtomicBool::new(false),
            told_hidden: AtomicBool::new(false),
        }
    }
}

#[derive(Serialize, serde::Deserialize, Default)]
struct Persisted {
    bases: Vec<String>,
}

/// Tell the companion which kernel addresses to poll (the Witness Wall host
/// sends the one the user typed, then the defaults). Every entry passes the
/// local-host gate or is dropped; the list survives a relaunch.
#[tauri::command]
pub fn companion_set_bases(
    app: AppHandle,
    state: State<'_, Companion>,
    bases: Vec<String>,
) -> Result<Vec<String>, String> {
    let accepted = accept_bases(bases);
    if accepted.is_empty() {
        return Err(
            "no local device address to try — device addresses must be local/private hosts"
                .to_string(),
        );
    }
    *state.bases.lock().unwrap_or_else(|e| e.into_inner()) = accepted.clone();
    if let Ok(dir) = app.path().app_data_dir() {
        let _ = std::fs::create_dir_all(&dir);
        let body = serde_json::to_vec(&Persisted {
            bases: accepted.clone(),
        })
        .unwrap_or_default();
        let _ = std::fs::write(dir.join(STATE_FILE), body);
    }
    Ok(accepted)
}

/// The companion's latest picture (None before the first poll finishes).
#[tauri::command]
pub fn companion_snapshot(state: State<'_, Companion>) -> Option<FleetSnapshot> {
    state
        .snapshot
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
}

/// Load the stored bases, build the tray, and start the poll routine. A tray
/// that cannot be built (no tray host) costs the menubar, never the app.
pub fn start(app: &AppHandle) {
    if let Ok(dir) = app.path().app_data_dir() {
        if let Ok(bytes) = std::fs::read(dir.join(STATE_FILE)) {
            if let Ok(p) = serde_json::from_slice::<Persisted>(&bytes) {
                let accepted = accept_bases(p.bases);
                if !accepted.is_empty() {
                    *app.state::<Companion>()
                        .bases
                        .lock()
                        .unwrap_or_else(|e| e.into_inner()) = accepted;
                }
            }
        }
    }
    if !tray_library_present() {
        eprintln!("companion: no AppIndicator library (libayatana-appindicator3) — no tray icon");
    } else {
        match build_tray(app) {
            Ok(()) => app
                .state::<Companion>()
                .tray_live
                .store(true, Ordering::SeqCst),
            Err(e) => eprintln!("companion: no tray icon on this desktop ({e})"),
        }
    }
    let handle = app.clone();
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(FIRST_POLL_DELAY).await;
        loop {
            poll_once(&handle).await;
            tokio::time::sleep(POLL_EVERY).await;
        }
    });
}

/// On Linux the tray is libappindicator, which the tray crate dlopens the
/// moment a tray is built — and PANICS when the library is missing, which
/// with this crate's `panic = "abort"` is a crash at launch (an AppImage on
/// a desktop without it, a `.deb` forced in without its depends). Probe the
/// same names in the same order first: no library, no tray, the app runs on.
#[cfg(target_os = "linux")]
fn tray_library_present() -> bool {
    [
        "libayatana-appindicator3.so.1",
        "libappindicator3.so.1",
        "libayatana-appindicator3.so",
        "libappindicator3.so",
    ]
    .iter()
    // SAFETY: these are exactly the libraries tray-icon itself loads as soon
    // as the tray is built; loading one early runs nothing it would not.
    .any(|name| unsafe { libloading::Library::new(name) }.is_ok())
}

#[cfg(not(target_os = "linux"))]
fn tray_library_present() -> bool {
    true
}

/// Whether closing the window should hide it instead of quitting: only when
/// the tray exists AND the platform always shows one (the macOS menu bar;
/// the Windows notification area). A Linux desktop may have no tray host at
/// all (stock GNOME shows none), and hiding the only window there would
/// strand an invisible app — so on Linux, closing quits, as it always has.
pub fn keeps_running(app: &AppHandle) -> bool {
    cfg!(any(target_os = "macos", target_os = "windows"))
        && app.state::<Companion>().tray_live.load(Ordering::SeqCst)
}

/// The first time a close hides the window, say where the app went.
pub fn told_hidden_once(app: &AppHandle) {
    if !app
        .state::<Companion>()
        .told_hidden
        .swap(true, Ordering::SeqCst)
    {
        notify(
            app,
            "SecuraCV Lab is still running".to_string(),
            "It keeps an eye on your fleet from the menu bar. Quit from there.".to_string(),
        );
    }
}

/// Bring the Lab's window back (tray "Open the Lab", or the macOS Dock).
pub fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
}

async fn poll_once(app: &AppHandle) {
    let state = app.state::<Companion>();
    // The browse runs on the blocking pool inside fleet_scan itself.
    let sightings = crate::fleet::fleet_scan(Some(BROWSE_MS))
        .await
        .unwrap_or_default();
    let bases = state
        .bases
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone();
    let doc = crate::witness_discover(bases).await.ok();

    let mut next = FleetSnapshot::default();
    if let Some(doc) = &doc {
        next.devices.extend(kernel_rows(doc));
    }
    let now = Instant::now();
    {
        let mut heard = state.heard.lock().unwrap_or_else(|e| e.into_inner());
        for s in &sightings {
            let name = clean_name(if s.name.is_empty() {
                &s.device_id
            } else {
                &s.name
            });
            if name.is_empty() || s.device_id.is_empty() {
                continue;
            }
            let row = DeviceRow {
                name,
                source: Source::Mdns,
                online: Some(true),
                chain: None,
                hub: None,
            };
            heard.insert(s.device_id.clone(), (row, now));
        }
        heard.retain(|_, (_, at)| now.duration_since(*at) < FORGET_AFTER);
        let kernel_names: Vec<String> = next
            .devices
            .values()
            .map(|r| r.name.to_lowercase())
            .collect();
        for (id, (row, at)) in heard.iter() {
            // The kernel's report is the richer one; don't list a board twice.
            if kernel_names.contains(&row.name.to_lowercase()) {
                continue;
            }
            let mut row = row.clone();
            // Heard in THIS window is evidence; older silence is "not heard
            // lately" — no claim either way.
            if *at != now {
                row.online = None;
            }
            next.devices.insert(format!("m:{id}"), row);
        }
    }

    let prev = state
        .snapshot
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .replace(next.clone());
    if let Some(prev) = prev {
        if !state.paused.load(Ordering::SeqCst) {
            for t in transitions(&prev, &next) {
                let (title, body) = t.words();
                notify(app, title, body);
            }
        }
    }
    refresh_tray(app);
}

/// Post one notification off the async workers: notify-rust's D-Bus call is
/// blocking.
fn notify(app: &AppHandle, title: String, body: String) {
    let app = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let _ = app.notification().builder().title(title).body(body).show();
    });
}

/// "N of M online" — only an explicit `online: true` (or a board heard in
/// this window) counts.
pub fn tooltip(snap: Option<&FleetSnapshot>) -> String {
    match snap {
        None => "SecuraCV fleet — looking…".to_string(),
        Some(s) if s.devices.is_empty() => "SecuraCV fleet — no Canaries found yet".to_string(),
        Some(s) => {
            let online = s
                .devices
                .values()
                .filter(|r| r.online == Some(true))
                .count();
            format!("SecuraCV fleet — {online} of {} online", s.devices.len())
        }
    }
}

/// One tray line per device, by name: its presence word, then any health
/// word the device reported.
pub fn tray_rows(snap: Option<&FleetSnapshot>) -> Vec<String> {
    let Some(s) = snap else {
        return vec!["Looking for your Canaries…".to_string()];
    };
    if s.devices.is_empty() {
        return vec!["No Canaries found yet".to_string()];
    }
    let mut rows: Vec<&DeviceRow> = s.devices.values().collect();
    rows.sort_by_key(|r| r.name.to_lowercase());
    let mut out: Vec<String> = rows
        .iter()
        .take(MAX_ROWS)
        .map(|r| {
            let presence = match (r.online, r.source) {
                (Some(true), _) => "online",
                (Some(false), _) => "offline",
                (None, Source::Kernel) => "hasn't said",
                (None, Source::Mdns) => "not heard lately",
            };
            let mut line = format!("{} — {presence}", r.name);
            if r.hub.as_deref() == Some("down") {
                line.push_str(" · hub down");
            }
            if r.chain.as_deref().is_some_and(|c| c != "ok") {
                line.push_str(" · chain not ok");
            }
            line
        })
        .collect();
    if rows.len() > MAX_ROWS {
        out.push(format!("+{} more", rows.len() - MAX_ROWS));
    }
    out
}

fn menu(app: &AppHandle) -> tauri::Result<Menu<Wry>> {
    let state = app.state::<Companion>();
    let snap = state
        .snapshot
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone();
    let paused = state.paused.load(Ordering::SeqCst);
    let open = MenuItem::with_id(app, "open", "Open the Lab", true, None::<&str>)?;
    let pause = CheckMenuItem::with_id(
        app,
        "pause",
        "Pause notifications",
        true,
        paused,
        None::<&str>,
    )?;
    let sep1 = PredefinedMenuItem::separator(app)?;
    let rows = tray_rows(snap.as_ref())
        .into_iter()
        .enumerate()
        .map(|(i, text)| MenuItem::with_id(app, format!("row-{i}"), text, false, None::<&str>))
        .collect::<tauri::Result<Vec<_>>>()?;
    let sep2 = PredefinedMenuItem::separator(app)?;
    let quit = MenuItem::with_id(app, "quit", "Quit SecuraCV Lab", true, None::<&str>)?;
    let mut items: Vec<&dyn IsMenuItem<Wry>> = vec![&open, &pause, &sep1];
    for r in &rows {
        items.push(r);
    }
    items.push(&sep2);
    items.push(&quit);
    Menu::with_items(app, &items)
}

fn build_tray(app: &AppHandle) -> tauri::Result<()> {
    let mut tray = TrayIconBuilder::with_id(TRAY_ID)
        .tooltip(tooltip(None))
        .menu(&menu(app)?)
        .show_menu_on_left_click(true)
        .on_menu_event(|app, event| match event.id().as_ref() {
            "open" => show_main(app),
            "pause" => {
                let state = app.state::<Companion>();
                let now = !state.paused.load(Ordering::SeqCst);
                state.paused.store(now, Ordering::SeqCst);
                refresh_tray(app);
            }
            // The update-install guard in lib.rs's ExitRequested handler
            // still holds this exit while an update is being written.
            "quit" => app.exit(0),
            _ => {}
        });
    if let Some(icon) = app.default_window_icon() {
        tray = tray.icon(icon.clone());
    }
    tray.build(app)?;
    Ok(())
}

fn refresh_tray(app: &AppHandle) {
    let Some(tray) = app.tray_by_id(TRAY_ID) else {
        return;
    };
    let snap = app
        .state::<Companion>()
        .snapshot
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone();
    let _ = tray.set_tooltip(Some(tooltip(snap.as_ref())));
    if let Ok(m) = menu(app) {
        let _ = tray.set_menu(Some(m));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    /// name, online, chain, hub — one kernel row.
    type Row<'a> = (&'a str, Option<bool>, Option<&'a str>, Option<&'a str>);

    fn snap(rows: &[Row]) -> FleetSnapshot {
        let mut s = FleetSnapshot::default();
        for (name, online, chain, hub) in rows {
            s.devices.insert(
                format!("k:{name}"),
                DeviceRow {
                    name: name.to_string(),
                    source: Source::Kernel,
                    online: *online,
                    chain: chain.map(str::to_string),
                    hub: hub.map(str::to_string),
                },
            );
        }
        s
    }

    #[test]
    fn a_change_between_two_reports_notifies() {
        let prev = snap(&[
            ("Front Door", Some(true), Some("ok"), Some("ok")),
            ("Driveway", Some(false), None, None),
        ]);
        let next = snap(&[
            ("Front Door", Some(false), Some("broken"), Some("down")),
            ("Driveway", Some(true), None, None),
        ]);
        let t = transitions(&prev, &next);
        assert!(t.contains(&Transition::Offline("Front Door".into())));
        assert!(t.contains(&Transition::Online("Driveway".into())));
        assert!(t.contains(&Transition::ChainNotOk(
            "Front Door".into(),
            "broken".into()
        )));
        assert!(t.contains(&Transition::HubDown("Front Door".into())));
        assert_eq!(t.len(), 4);
    }

    #[test]
    fn silence_is_never_a_transition() {
        // A field that went quiet, a row that vanished, a row that appeared:
        // none of them is something the network SAID changed.
        let prev = snap(&[
            ("Front Door", Some(true), Some("ok"), Some("ok")),
            ("Studio", Some(true), None, None),
        ]);
        let next = snap(&[
            ("Front Door", None, None, None),
            ("Porch", Some(false), Some("broken"), Some("down")),
        ]);
        assert!(transitions(&prev, &next).is_empty());
        // And nothing changes, nothing notifies.
        assert!(transitions(&prev, &prev).is_empty());
        // A board that stopped announcing reads "not heard lately", no claim.
        let mut heard = FleetSnapshot::default();
        let mut row = DeviceRow {
            name: "Sense".into(),
            source: Source::Mdns,
            online: Some(true),
            chain: None,
            hub: None,
        };
        heard.devices.insert("m:a".into(), row.clone());
        let mut quiet = FleetSnapshot::default();
        row.online = None;
        quiet.devices.insert("m:a".into(), row);
        assert!(transitions(&heard, &quiet).is_empty());
        assert_eq!(tray_rows(Some(&quiet)), vec!["Sense — not heard lately"]);
    }

    #[test]
    fn recovery_and_repeat_do_not_notify() {
        // chain back to ok and hub back up are good news, not alarms; a hub
        // that stays down is not news twice.
        let prev = snap(&[("Front Door", Some(true), Some("broken"), Some("down"))]);
        let next = snap(&[("Front Door", Some(true), Some("ok"), Some("down"))]);
        assert!(transitions(&prev, &next).is_empty());
    }

    #[test]
    fn the_words_stay_coarse_and_never_claim_verification() {
        let all = [
            Transition::Online("Front Door".into()),
            Transition::Offline("Front Door".into()),
            Transition::ChainNotOk("Front Door".into(), "broken".into()),
            Transition::HubDown("Front Door".into()),
        ];
        for t in &all {
            let (title, body) = t.words();
            for text in [&title, &body] {
                let lower = text.to_lowercase();
                assert!(!lower.contains("verif"), "{text}");
                for w in ["person", "occupant", "breathing", "sealed", "evidence"] {
                    assert!(!lower.contains(w), "{text} carries {w}");
                }
            }
        }
        assert!(!tooltip(Some(&snap(&[("A", Some(true), None, None)])))
            .to_lowercase()
            .contains("verif"));
    }

    #[test]
    fn the_kernel_document_yields_only_the_contract_fields() {
        let doc = json!({
            "kernel": "kitchen-hub",
            "verified_through": "now",
            "devices": [
                { "name": "Front Door", "online": true, "chain": "ok", "hub": "ok",
                  "presence": "present", "occupants": "2+", "seeing": "person" },
                { "name": "Driveway" },
                { "name": "  ", "online": true },
                { "online": true },
                { "name": "Odd", "online": "yes", "chain": "✓ fine", "hub": "DOWN" }
            ]
        });
        let rows = kernel_rows(&doc);
        assert_eq!(rows.len(), 3, "nameless rows are dropped");
        let front = &rows[0].1;
        assert_eq!(front.online, Some(true));
        assert_eq!(front.chain.as_deref(), Some("ok"));
        // `online` absent, or not a boolean: cannot say — never "online".
        assert_eq!(rows[1].1.online, None);
        assert_eq!(rows[2].1.online, None);
        // Words outside the short lowercase set are "cannot say".
        assert_eq!(rows[2].1.chain, None);
        assert_eq!(rows[2].1.hub, None);
        // The serialized row carries the contract fields and nothing else.
        let v = serde_json::to_value(front).unwrap();
        let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(|k| k.as_str()).collect();
        keys.sort();
        assert_eq!(keys, ["chain", "hub", "name", "online", "source"]);
        // Bare arrays and `canaries` are accepted too.
        assert_eq!(kernel_rows(&json!([{ "name": "A" }])).len(), 1);
        assert_eq!(
            kernel_rows(&json!({ "canaries": [{ "name": "A" }] })).len(),
            1
        );
        assert!(kernel_rows(&json!({ "devices": "nope" })).is_empty());
    }

    #[test]
    fn the_tray_library_probe_answers_without_panicking() {
        // Present or not on this host, the probe is a bool — the panic lives
        // in the tray crate's own loader, which only runs after a `true`.
        let _ = tray_library_present();
    }

    #[test]
    fn names_from_the_network_are_made_safe_to_show() {
        assert_eq!(clean_name("  Front\u{7}Door\n "), "FrontDoor");
        assert_eq!(clean_name(&"x".repeat(80)).len(), 40);
        assert_eq!(clean_name("\u{0}\u{1b}"), "");
    }

    #[test]
    fn bases_are_gated_like_witness_discover() {
        let got = accept_bases(vec![
            "http://192.168.1.40:8099".into(),
            " http://canary.local ".into(),
            "http://example.com".into(),
            "ftp://10.0.0.1".into(),
            "http://192.168.1.40:8099".into(),
        ]);
        assert_eq!(got, ["http://192.168.1.40:8099", "http://canary.local"]);
        assert!(accept_bases(vec!["https://github.com".into()]).is_empty());
        let many: Vec<String> = (0..20).map(|i| format!("http://10.0.0.{i}")).collect();
        assert_eq!(accept_bases(many).len(), MAX_BASES);
    }

    #[test]
    fn every_default_base_survives_the_gate_in_order() {
        // A default the gate dropped would be a candidate the tray never
        // polls — the kernel's own port (8799) silently gone from the list.
        let defaults: Vec<String> = DEFAULT_BASES.iter().map(|b| b.to_string()).collect();
        assert_eq!(accept_bases(defaults.clone()), defaults);
        assert_eq!(*Companion::default().bases.lock().unwrap(), defaults);
    }

    #[test]
    fn the_tray_counts_only_what_was_said() {
        let s = snap(&[
            ("B", Some(true), None, Some("down")),
            ("A", Some(false), Some("broken"), None),
            ("C", None, None, None),
        ]);
        assert_eq!(tooltip(Some(&s)), "SecuraCV fleet — 1 of 3 online");
        assert_eq!(
            tray_rows(Some(&s)),
            [
                "A — offline · chain not ok",
                "B — online · hub down",
                "C — hasn't said"
            ]
        );
        assert_eq!(
            tooltip(Some(&FleetSnapshot::default())),
            "SecuraCV fleet — no Canaries found yet"
        );
        let mut many = FleetSnapshot::default();
        for i in 0..15 {
            let name = format!("Canary {i:02}");
            many.devices.insert(
                format!("k:{name}"),
                DeviceRow {
                    name,
                    source: Source::Kernel,
                    online: Some(true),
                    chain: None,
                    hub: None,
                },
            );
        }
        let rows = tray_rows(Some(&many));
        assert_eq!(rows.len(), MAX_ROWS + 1);
        assert_eq!(rows.last().unwrap(), "+3 more");
    }
}
