//! The app's secret drawer: profile passwords and per-device API tokens,
//! kept in the OS credential store — the macOS Keychain, the Windows
//! Credential Manager, or on Linux the freedesktop Secret Service (GNOME
//! Keyring, KDE Wallet's bridge) — never in a file this app writes itself.
//!
//! Why here and not localStorage: the setup-profile feature remembers the
//! Wi-Fi and broker passwords so a reflash fills itself in, and the fleet
//! book keeps each Canary's local-API bearer token so it can read status and
//! start an update later. Those are real secrets. The OS store encrypts them
//! at rest, scopes them to this app, and survives an app reinstall; the
//! frontend asks `secret_backend` first and words its "Remember" consent to
//! match what this platform actually offers. On Linux that answer is
//! PROBED, not assumed: a headless or minimal desktop has no Secret Service
//! on its session bus, and there the drawer answers "none" and the frontend
//! falls back to its local settings and SAYS so — the same trade the browser
//! flasher's opt-in Wi-Fi memory makes. A capability must never light a
//! path that can only fail, and the consent note must never promise a store
//! that isn't there.
//!
//! Callers never log values. Keys are namespaced by the frontend
//! (`wifi:<ssid>`, `mqtt:<host>:<user>`, `canary:<mac>:token`,
//! `hub:account`) so one drawer serves every feature without collisions.

/// One service name for every entry, so the OS store shows a single coherent
/// app identity ("SecuraCV Flasher") rather than a scatter of rows.
#[cfg(any(target_os = "macos", target_os = "windows", target_os = "linux"))]
const SERVICE: &str = "SecuraCV Flasher";

/// The four namespaces the frontend actually uses (see the module doc). A key
/// outside them is refused by every command below, so `secret_get` is a
/// lookup in the drawer this app fills — not a read-anything primitive for
/// whatever runs in the webview.
const NAMESPACES: [&str; 4] = ["wifi:", "mqtt:", "canary:", "hub:"];

/// Keys are frontend-chosen but bounded: printable ASCII, no whitespace,
/// short, and inside a known namespace with something after the colon.
/// Refusing junk here keeps the OS store browsable by a human.
fn key_ok(key: &str) -> bool {
    if key.is_empty() || key.len() > 120 {
        return false;
    }
    if !key.bytes().all(|b| (0x21..=0x7e).contains(&b)) {
        return false;
    }
    NAMESPACES
        .iter()
        .any(|ns| key.starts_with(ns) && key.len() > ns.len())
}

/// Which secret store this build can reach: "keychain" (macOS),
/// "credential-manager" (Windows), "secret-service" (Linux, when a
/// freedesktop Secret Service answers on the session bus), or "none" — the
/// frontend words its consent copy from this answer instead of guessing the
/// platform. On Linux the answer is computed ONCE per process (`probe_backend`):
/// the consent note is worded from it, so it must not flip mid-session.
#[tauri::command]
pub async fn secret_backend() -> &'static str {
    // A probe that could not even run is no store at all: fail closed.
    off_ui(backend).await.unwrap_or("none")
}

fn backend() -> &'static str {
    #[cfg(target_os = "macos")]
    {
        "keychain"
    }
    #[cfg(target_os = "windows")]
    {
        "credential-manager"
    }
    #[cfg(target_os = "linux")]
    {
        static BACKEND: std::sync::OnceLock<&'static str> = std::sync::OnceLock::new();
        BACKEND.get_or_init(probe_backend)
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
    {
        "none"
    }
}

/// Ask the session bus whether a Secret Service is really there. One read of
/// an entry this app never writes: `Ok` or `NoEntry` means a daemon answered
/// the search (the store exists, whether or not it holds anything);
/// anything else — no `DBUS_SESSION_BUS_ADDRESS`, no daemon behind it, the
/// service refusing — is "none". Fails CLOSED on purpose: a wrong "none"
/// costs the user the OS store for a session and says so in the consent
/// note; a wrong "secret-service" would make that note a promise the app
/// cannot keep. A search for a missing item never touches a locked
/// collection, so this probe can never raise an unlock prompt, and each
/// D-Bus call it makes is bounded (dbus-secret-service's 2 s proxy timeout).
#[cfg(target_os = "linux")]
fn probe_backend() -> &'static str {
    match keyring::Entry::new(SERVICE, "probe").map(|e| e.get_password()) {
        Ok(Ok(_)) | Ok(Err(keyring::Error::NoEntry)) => "secret-service",
        _ => "none",
    }
}

#[cfg(any(target_os = "macos", target_os = "windows", target_os = "linux"))]
fn entry(key: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new(SERVICE, key).map_err(|e| format!("couldn't open the secret store: {e}"))
}

/// Where every store call runs: tauri's blocking pool — never the main
/// thread, never an async worker. A store call can WAIT ON THE USER: a
/// locked keyring collection makes the Secret Service raise its own unlock
/// prompt, and the synchronous backend blocks until that prompt is answered
/// (dbus-secret-service's `connect` sets no limit). On the main thread that
/// wait would freeze the window — GNOME would offer to force-quit the app
/// while its own password dialog is up — and on an async worker it would
/// stall every other async command. The macOS/Windows stores are
/// thread-safe, so one path serves all three.
async fn off_ui<T, F>(work: F) -> Result<T, String>
where
    T: Send + 'static,
    F: FnOnce() -> T + Send + 'static,
{
    tauri::async_runtime::spawn_blocking(work)
        .await
        .map_err(|e| format!("secret store thread failed: {e}"))
}

/// The commands below are thin: each hands its work to a `*_in(backend, …)`
/// twin that takes the backend name as a parameter, so the "none" branch —
/// the one a headless Linux box, and this crate's own tests, actually
/// exercise — is the SAME code on every platform and testable without a
/// bus. On macOS/Windows the backend is a compile-time constant and the
/// branch folds away.
const NO_STORE: &str = "no OS secret store on this platform";

/// Store one secret under `key`, replacing any previous value.
#[tauri::command]
pub async fn secret_set(key: String, value: String) -> Result<(), String> {
    off_ui(move || set_in(backend(), &key, &value)).await?
}

/// Read one secret. `Ok(None)` means "nothing stored" — a real error means
/// the store itself refused (locked, permission denied).
#[tauri::command]
pub async fn secret_get(key: String) -> Result<Option<String>, String> {
    off_ui(move || get_in(backend(), &key)).await?
}

/// Forget one secret. Deleting something already absent is success — the
/// user asked for it to be gone, and it is.
#[tauri::command]
pub async fn secret_delete(key: String) -> Result<(), String> {
    off_ui(move || delete_in(backend(), &key)).await?
}

fn set_in(backend: &str, key: &str, value: &str) -> Result<(), String> {
    if !key_ok(key) {
        return Err("bad secret key".into());
    }
    if value.is_empty() {
        // Storing "" is a delete in disguise — do the honest thing.
        return delete_in(backend, key);
    }
    if backend == "none" {
        return Err(NO_STORE.into());
    }
    #[cfg(any(target_os = "macos", target_os = "windows", target_os = "linux"))]
    {
        entry(key)?
            .set_password(value)
            .map_err(|e| format!("couldn't save to the secret store: {e}"))
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
    {
        Err(NO_STORE.into())
    }
}

fn get_in(backend: &str, key: &str) -> Result<Option<String>, String> {
    if !key_ok(key) {
        return Err("bad secret key".into());
    }
    if backend == "none" {
        return Ok(None);
    }
    #[cfg(any(target_os = "macos", target_os = "windows", target_os = "linux"))]
    {
        match entry(key)?.get_password() {
            Ok(v) => Ok(Some(v)),
            Err(keyring::Error::NoEntry) => Ok(None),
            Err(e) => Err(format!("couldn't read the secret store: {e}")),
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
    {
        Ok(None)
    }
}

fn delete_in(backend: &str, key: &str) -> Result<(), String> {
    if !key_ok(key) {
        return Err("bad secret key".into());
    }
    if backend == "none" {
        return Ok(());
    }
    #[cfg(any(target_os = "macos", target_os = "windows", target_os = "linux"))]
    {
        match entry(key)?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(e) => Err(format!("couldn't remove from the secret store: {e}")),
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
    {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_hygiene() {
        // The frontend URI-encodes variable parts (an SSID may hold spaces or
        // emoji), so a well-formed key is always printable ASCII, no spaces.
        assert!(key_ok("wifi:Bird%20House"));
        assert!(key_ok("canary:a1:b2:c3:d4:e5:f6:token"));
        assert!(key_ok("mqtt:homeassistant.local:canary"));
        assert!(key_ok("hub:account"));
        assert!(!key_ok(""));
        assert!(!key_ok("has space"));
        assert!(!key_ok("has\nnewline"));
        assert!(!key_ok(&"x".repeat(121)));
        // Outside the drawer this app fills: refused, however well-formed.
        assert!(!key_ok("keychain:anything"));
        assert!(!key_ok("account"));
        assert!(!key_ok("wifi:"));
        assert!(!key_ok(&format!("wifi:{}", "x".repeat(120))));
    }

    #[test]
    fn backend_names_are_the_contract() {
        // app.js words its consent copy from these exact strings
        // (desktop_parity.test.js pins its where() table to this set).
        assert!(matches!(
            backend(),
            "keychain" | "credential-manager" | "secret-service" | "none"
        ));
    }

    #[test]
    fn without_a_store_the_drawer_is_honest_and_empty() {
        // The branch every platform shares: "none" refuses a write out loud,
        // reads as empty, and treats a delete as already done — exactly the
        // values the frontend's prefs-file fallback is written against.
        assert_eq!(set_in("none", "wifi:Nest", "hunter2"), Err(NO_STORE.into()));
        assert_eq!(get_in("none", "wifi:Nest"), Ok(None));
        assert_eq!(delete_in("none", "wifi:Nest"), Ok(()));
        // Key hygiene comes first, whatever the backend.
        assert_eq!(get_in("none", "nope"), Err("bad secret key".into()));
        assert_eq!(set_in("none", "wifi:Nest", ""), Ok(()));
    }

    /// The probe must fail CLOSED where no Secret Service can answer: point
    /// the session bus at a socket that does not exist and the drawer must
    /// say "none" — never panic, never claim a store — and the live-backend
    /// paths must surface the bus failure as an error, not hang or unwind.
    ///
    /// The dead bus is set in a CHILD copy of this test binary, never with
    /// `set_var` here: the harness runs tests on parallel threads, and a
    /// `set_var` racing another thread's environment read (libdbus's own,
    /// any test that spawns a process) is undefined behavior in libc.
    #[cfg(target_os = "linux")]
    #[test]
    fn linux_probe_fails_closed_without_a_bus() {
        const CHILD: &str = "SECURACV_SECRET_PROBE_CHILD";
        if std::env::var_os(CHILD).is_some() {
            assert_eq!(probe_backend(), "none");
            // With the bus dead, a caller that somehow believes in the store
            // gets a typed refusal from libdbus, not a hang.
            assert!(set_in("secret-service", "wifi:Nest", "hunter2").is_err());
            assert!(get_in("secret-service", "wifi:Nest").is_err());
            assert!(delete_in("secret-service", "wifi:Nest").is_err());
            return;
        }
        // The harness names tests without the crate prefix.
        let module = module_path!().split_once("::").map_or("", |(_, rest)| rest);
        let name = format!("{module}::linux_probe_fails_closed_without_a_bus");
        let out = std::process::Command::new(std::env::current_exe().expect("test binary"))
            .args([name.as_str(), "--exact", "--test-threads=1"])
            .env(CHILD, "1")
            .env(
                "DBUS_SESSION_BUS_ADDRESS",
                "unix:path=/nonexistent/securacv-secret-store-probe",
            )
            .output()
            .expect("re-run this test with a dead session bus");
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(
            out.status.success(),
            "child failed:\n{stdout}\n{}",
            String::from_utf8_lossy(&out.stderr)
        );
        // A filter that matched nothing also exits 0 — insist it RAN.
        assert!(stdout.contains("1 passed"), "child ran no test:\n{stdout}");
    }
}
