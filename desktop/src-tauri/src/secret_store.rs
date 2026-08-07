//! The app's secret drawer: profile passwords and per-device API tokens,
//! kept in the OS credential store — the macOS Keychain or the Windows
//! Credential Manager — never in a file this app writes itself.
//!
//! Why here and not localStorage: the setup-profile feature remembers the
//! Wi-Fi and broker passwords so a reflash fills itself in, and the fleet
//! book keeps each Canary's local-API bearer token so it can read status and
//! start an update later. Those are real secrets. The OS store encrypts them
//! at rest, scopes them to this app, and survives an app reinstall; the
//! frontend asks `secret_backend` first and words its "Remember" consent to
//! match what this platform actually offers (Linux has no packaged backend
//! here — the frontend falls back to its local prefs file and SAYS so, the
//! same trade the browser flasher's opt-in Wi-Fi memory makes).
//!
//! Callers never log values. Keys are namespaced by the frontend
//! (`wifi:<ssid>`, `mqtt:<host>:<user>`, `canary:<mac>:token`,
//! `hub:account`) so one drawer serves every feature without collisions.

/// One service name for every entry, so the OS store shows a single coherent
/// app identity ("SecuraCV Flasher") rather than a scatter of rows.
#[cfg(any(target_os = "macos", target_os = "windows"))]
const SERVICE: &str = "SecuraCV Flasher";

/// Keys are frontend-chosen but bounded: printable ASCII, no whitespace,
/// short. Refusing junk here keeps the OS store browsable by a human.
fn key_ok(key: &str) -> bool {
    !key.is_empty() && key.len() <= 120 && key.bytes().all(|b| (0x21..=0x7e).contains(&b))
}

/// Which secret store this build can reach: "keychain" (macOS),
/// "credential-manager" (Windows), or "none" — the frontend words its
/// consent copy from this answer instead of guessing the platform.
#[tauri::command]
pub fn secret_backend() -> &'static str {
    #[cfg(target_os = "macos")]
    {
        "keychain"
    }
    #[cfg(target_os = "windows")]
    {
        "credential-manager"
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        "none"
    }
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
fn entry(key: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new(SERVICE, key).map_err(|e| format!("couldn't open the secret store: {e}"))
}

/// Store one secret under `key`, replacing any previous value.
#[tauri::command]
pub fn secret_set(key: String, value: String) -> Result<(), String> {
    if !key_ok(&key) {
        return Err("bad secret key".into());
    }
    if value.is_empty() {
        // Storing "" is a delete in disguise — do the honest thing.
        return secret_delete(key);
    }
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    {
        entry(&key)?
            .set_password(&value)
            .map_err(|e| format!("couldn't save to the secret store: {e}"))
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        let _ = value;
        Err("no OS secret store on this platform".into())
    }
}

/// Read one secret. `Ok(None)` means "nothing stored" — a real error means
/// the store itself refused (locked, permission denied).
#[tauri::command]
pub fn secret_get(key: String) -> Result<Option<String>, String> {
    if !key_ok(&key) {
        return Err("bad secret key".into());
    }
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    {
        match entry(&key)?.get_password() {
            Ok(v) => Ok(Some(v)),
            Err(keyring::Error::NoEntry) => Ok(None),
            Err(e) => Err(format!("couldn't read the secret store: {e}")),
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        Ok(None)
    }
}

/// Forget one secret. Deleting something already absent is success — the
/// user asked for it to be gone, and it is.
#[tauri::command]
pub fn secret_delete(key: String) -> Result<(), String> {
    if !key_ok(&key) {
        return Err("bad secret key".into());
    }
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    {
        match entry(&key)?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(e) => Err(format!("couldn't remove from the secret store: {e}")),
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
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
        assert!(!key_ok(""));
        assert!(!key_ok("has space"));
        assert!(!key_ok("has\nnewline"));
        assert!(!key_ok(&"x".repeat(121)));
    }

    #[test]
    fn backend_names_are_the_contract() {
        // app.js words its consent copy from these exact strings.
        assert!(matches!(
            secret_backend(),
            "keychain" | "credential-manager" | "none"
        ));
    }
}
