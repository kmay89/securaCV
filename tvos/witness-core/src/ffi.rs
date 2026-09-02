//! The C ABI the SwiftUI app calls across.
//!
//! Deliberately the narrowest possible surface: **JSON in, JSON out, one
//! free function.** No structs cross the boundary, so adding a field to a
//! report can never desync a Swift struct layout from a Rust one — the only
//! contract is the JSON, which both sides already parse. The one plain-string
//! exception is [`scv_normalize_source_host`]: a hostname is a string, not a
//! document, and null there is an answer ("skip this advert"), not a failure.
//!
//! Every entry point is panic-safe. The release profile builds with
//! `panic = "abort"`, so a panic unwinding into Swift is not merely undefined,
//! it is fatal — the code below is written to return a report instead. Rust
//! owns every string it returns; Swift must hand each one back to
//! `scv_string_free` exactly once (`WitnessCore.swift` wraps that so no call
//! site has to remember).

use std::ffi::{c_char, CStr, CString};

/// Turn a Rust `String` into a heap C string Swift can read.
/// Returns null only if the string contains an interior NUL, which our JSON
/// never does — but null is checked on the Swift side regardless.
fn into_c_string(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Read a caller-supplied C string. `None` for null or non-UTF-8 input.
///
/// # Safety
/// `ptr` must be null or a valid NUL-terminated C string.
unsafe fn from_c_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }
    CStr::from_ptr(ptr).to_str().ok().map(str::to_owned)
}

/// A malformed-input report, in the same JSON shape as a real one, so Swift
/// has exactly one thing to decode on every path.
fn malformed(detail: &str) -> String {
    format!(
        r#"{{"ok":false,"verified":0,"head":"{zeros}","kind":"malformed","detail":{detail},"message":"The record couldn't be read in the expected format."}}"#,
        zeros = "0".repeat(64),
        detail = serde_json::to_string(detail).unwrap_or_else(|_| "\"\"".to_string()),
    )
}

/// Verify a sealed-log document. Takes the JSON described by
/// [`crate::SealedLogDocument`], returns [`crate::VerifyReport`] as JSON.
///
/// Never returns null except on allocation failure, and never panics: a bad
/// pointer, bad UTF-8, or bad JSON all come back as an `ok:false` report.
///
/// # Safety
/// `json` must be null or a valid NUL-terminated UTF-8 C string. The returned
/// pointer must be released with [`scv_string_free`].
#[no_mangle]
pub unsafe extern "C" fn scv_verify_sealed_log(json: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| {
        let Some(input) = from_c_string(json) else {
            return malformed("the sealed log was empty or not valid UTF-8");
        };
        let report = crate::verify_json(&input);
        serde_json::to_string(&report)
            .unwrap_or_else(|e| malformed(&format!("could not encode the report: {e}")))
    });
    into_c_string(result.unwrap_or_else(|_| malformed("the verifier stopped unexpectedly")))
}

/// Parse a `GET /api/fleet` response. Returns the normalized
/// [`crate::fleet::FleetSnapshot`] as JSON, or `{"error": "..."}`.
///
/// # Safety
/// Same contract as [`scv_verify_sealed_log`].
#[no_mangle]
pub unsafe extern "C" fn scv_parse_fleet(json: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| {
        let Some(input) = from_c_string(json) else {
            return r#"{"error":"the fleet response was empty or not valid UTF-8"}"#.to_string();
        };
        match crate::fleet::parse_fleet(&input) {
            Ok(snapshot) => serde_json::to_string(&snapshot)
                .unwrap_or_else(|e| format!(r#"{{"error":"could not encode the fleet: {e}"}}"#)),
            Err(e) => serde_json::to_string(&serde_json::json!({ "error": e }))
                .unwrap_or_else(|_| r#"{"error":"unreadable fleet response"}"#.to_string()),
        }
    });
    into_c_string(
        result
            .unwrap_or_else(|_| r#"{"error":"the fleet parser stopped unexpectedly"}"#.to_string()),
    )
}

/// Validate and normalize a Bonjour TXT `host` value into the hostname the
/// Wall may poll — [`crate::host::normalize_source_host`], the same gate the
/// iPhone's `DeviceAPI.isPrivate` applies. Returns the lower-cased host,
/// qualified with `.local` when the advert carried a bare label — or **null
/// when the advert must be skipped**: a public name or address, a malformed
/// label, empty input, a null pointer, or bytes that are not UTF-8. This is
/// the one entry point where null is an answer rather than an allocation
/// failure, because both mean the same thing to the caller: do not poll it.
///
/// # Safety
/// `host` must be null or a valid NUL-terminated C string. A non-null result
/// must be released with [`scv_string_free`].
#[no_mangle]
pub unsafe extern "C" fn scv_normalize_source_host(host: *const c_char) -> *mut c_char {
    let result = std::panic::catch_unwind(|| {
        let input = from_c_string(host)?;
        crate::host::normalize_source_host(&input)
    });
    match result {
        Ok(Some(normalized)) => into_c_string(normalized),
        Ok(None) | Err(_) => std::ptr::null_mut(),
    }
}

/// The core's version, so the TV's About panel can prove which core it links
/// rather than guessing. Static — the caller must NOT free it.
#[no_mangle]
pub extern "C" fn scv_core_version() -> *const c_char {
    // Built from CARGO_PKG_VERSION at compile time, NUL-terminated in the
    // literal so it is a genuine `'static` C string with no allocation.
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

/// Release a string returned by this library.
///
/// # Safety
/// `ptr` must be null, or a pointer returned by [`scv_verify_sealed_log`] /
/// [`scv_parse_fleet`] / [`scv_normalize_source_host`] that has not already
/// been freed. Never pass [`scv_core_version`]'s pointer here — it is static.
#[no_mangle]
pub unsafe extern "C" fn scv_string_free(ptr: *mut c_char) {
    if !ptr.is_null() {
        drop(CString::from_raw(ptr));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    /// Call an FFI entry point with a Rust `&str` and get the answer back as a
    /// `String`, freeing the C string the way Swift is required to.
    unsafe fn call(f: unsafe extern "C" fn(*const c_char) -> *mut c_char, input: &str) -> String {
        let c_in = CString::new(input).unwrap();
        let out = f(c_in.as_ptr());
        assert!(!out.is_null(), "FFI returned null");
        let s = CStr::from_ptr(out).to_str().unwrap().to_owned();
        scv_string_free(out);
        s
    }

    fn signed_document() -> String {
        let key = SigningKey::from_bytes(&[3u8; 32]);
        let payload = r#"{"kind":"heartbeat"}"#;
        let entry_hash = crate::hash_entry(&[0u8; 32], payload.as_bytes());
        let signing_hash =
            crate::domain_separated_hash(crate::DOMAIN_SEALED_LOG_ENTRY, &entry_hash);
        serde_json::json!({
            "verifying_key": hex::encode(key.verifying_key().to_bytes()),
            "entries": [{
                "id": 1,
                "payload": payload,
                "prev_hash": hex::encode([0u8; 32]),
                "entry_hash": hex::encode(entry_hash),
                "signature": hex::encode(key.sign(&signing_hash).to_bytes()),
            }],
        })
        .to_string()
    }

    #[test]
    fn a_good_log_verifies_across_the_boundary() {
        let out = unsafe { call(scv_verify_sealed_log, &signed_document()) };
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["ok"], true, "{out}");
        assert_eq!(v["verified"], 1);
    }

    #[test]
    fn a_null_pointer_is_a_report_not_a_crash() {
        let out = unsafe { scv_verify_sealed_log(std::ptr::null()) };
        assert!(!out.is_null());
        let s = unsafe { CStr::from_ptr(out).to_str().unwrap().to_owned() };
        unsafe { scv_string_free(out) };
        let v: serde_json::Value = serde_json::from_str(&s).unwrap();
        assert_eq!(v["ok"], false);
        assert_eq!(v["kind"], "malformed");
    }

    #[test]
    fn garbage_json_is_a_well_formed_report() {
        let out = unsafe { call(scv_verify_sealed_log, "{{{{") };
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["ok"], false);
        // Swift decodes the same struct on every path — these must always exist.
        assert!(v["message"].is_string());
        assert!(v["head"].is_string());
    }

    #[test]
    fn the_fleet_endpoint_normalizes_the_bare_array_form() {
        let out = unsafe { call(scv_parse_fleet, r#"[{"name":"Front Door"}]"#) };
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["devices"][0]["name"], "Front Door");
        assert_eq!(
            v["devices"][0]["online"], false,
            "a silent `online` is not a presence claim"
        );
    }

    #[test]
    fn a_bad_fleet_response_reports_an_error_field() {
        let out = unsafe { call(scv_parse_fleet, "<html>captive portal</html>") };
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert!(v["error"].is_string());
    }

    #[test]
    fn a_bare_advert_host_comes_back_qualified() {
        let out = unsafe { call(scv_normalize_source_host, "canary-nightstand7-001-a1b2c3") };
        assert_eq!(out, "canary-nightstand7-001-a1b2c3.local");
    }

    #[test]
    fn a_private_address_comes_back_as_itself() {
        let out = unsafe { call(scv_normalize_source_host, "192.168.1.20") };
        assert_eq!(
            out, "192.168.1.20",
            "an address must not grow a .local suffix"
        );
    }

    #[test]
    fn a_hostile_advert_host_is_null_not_a_host() {
        // Null is the answer here, by contract: Swift reads it as "skip".
        for bad in [
            "evil.example.com",
            "8.8.8.8",
            "10.0.0.1.attacker.com",
            "-canary",
            "",
        ] {
            let c_in = CString::new(bad).unwrap();
            let out = unsafe { scv_normalize_source_host(c_in.as_ptr()) };
            assert!(out.is_null(), "{bad:?} must be refused");
        }
        assert!(unsafe { scv_normalize_source_host(std::ptr::null()) }.is_null());
    }

    #[test]
    fn freeing_null_is_allowed() {
        unsafe { scv_string_free(std::ptr::null_mut()) };
    }

    #[test]
    fn the_core_reports_its_version() {
        let v = unsafe { CStr::from_ptr(scv_core_version()).to_str().unwrap() };
        assert_eq!(v, env!("CARGO_PKG_VERSION"));
    }
}
