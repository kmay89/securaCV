//! Integration tests for pairing (`POST /api/pair`, `DELETE /api/pair/<id>`)
//! and the paired bearer it mints: the code is single-use with a per-code
//! budget, every refusal is one constant 401 and a strike, malformed input
//! is a 400 and not a strike, the bearer opens every token-gated route and
//! labels its exports `paired` + `token_id`, revocation follows the
//! self-only rule, and a revocation on disk lands on the next request.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use tempfile::tempdir;
use witness_kernel::api::pairing::{
    PairingCodeFile, PAIRED_TOKEN_FILE_NAME, PAIRING_CODE_ATTEMPTS, PAIRING_CODE_FILE_NAME,
};
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer, PairingPaths, ViewerTokenSet};
use witness_kernel::{
    verify_export_bundle, CandidateEvent, EventType, ExportAuthMode, ExportBundle,
    InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:test:pairing-0123456789abcdef";
const INVALID: &str = r#"{"error":"invalid_pairing_code"}"#;

fn kernel_config(db_path: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig) -> Result<()> {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    kernel.append_event_checked(
        &desc,
        CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket::now(600)?,
            zone_id: "zone:porch".to_string(),
            confidence: 0.7,
            correlation_token: None,
            attestation: None,
        },
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;
    Ok(())
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

struct TestApi {
    dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
    pairing: Option<PairingPaths>,
}

impl TestApi {
    /// Spawn over a kernel holding one sealed event, with pairing files in
    /// the temp dir when `with_pairing`.
    fn new(with_pairing: bool) -> Result<Self> {
        let dir = tempdir()?;
        let cfg = kernel_config(&dir.path().join("witness.db"));
        let mut kernel = Kernel::open(&cfg)?;
        seal_event(&mut kernel, &cfg)?;
        drop(kernel);
        let pairing = with_pairing.then(|| PairingPaths {
            code_path: dir.path().join(PAIRING_CODE_FILE_NAME),
            tokens_path: dir.path().join(PAIRED_TOKEN_FILE_NAME),
        });
        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            pairing: pairing.clone(),
            ..ApiConfig::default()
        };
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
        Ok(Self {
            dir,
            api_handle: Some(api_handle),
            pairing,
        })
    }

    fn handle(&self) -> &ApiHandle {
        self.api_handle.as_ref().expect("api handle")
    }

    fn capability(&self) -> String {
        self.handle().token.clone()
    }

    fn paths(&self) -> &PairingPaths {
        self.pairing.as_ref().expect("pairing configured")
    }

    /// Issue a code the way `witness_api issue-pairing-code` does.
    fn issue_code(&self) -> Result<String> {
        Ok(PairingCodeFile::issue(&self.paths().code_path, 600, "test", now())?.code)
    }

    /// Raw HTTP: `token` becomes an `x-witness-token` header; `body` adds
    /// `Content-Length`.
    fn request(
        &self,
        method: &str,
        path: &str,
        token: Option<&str>,
        body: Option<&str>,
    ) -> Result<(String, String)> {
        self.request_raw(method, path, token, body.map(|b| (b.len(), b)))
    }

    /// Like `request`, with the declared `Content-Length` chosen by the
    /// caller (to lie about it).
    fn request_raw(
        &self,
        method: &str,
        path: &str,
        token: Option<&str>,
        body: Option<(usize, &str)>,
    ) -> Result<(String, String)> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let token_header = token
            .map(|t| format!("X-Witness-Token: {t}\r\n"))
            .unwrap_or_default();
        let length_header = body
            .map(|(len, _)| format!("Content-Length: {len}\r\n"))
            .unwrap_or_default();
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}{length_header}\r\n{}",
            body.map(|(_, b)| b).unwrap_or("")
        );
        stream.write_all(request.as_bytes())?;
        let mut response = String::new();
        stream.read_to_string(&mut response)?;
        let mut parts = response.splitn(2, "\r\n\r\n");
        let headers = parts.next().unwrap_or("").to_string();
        let body = parts.next().unwrap_or("").to_string();
        Ok((headers, body))
    }

    fn pair(&self, body: &str) -> Result<(String, String)> {
        self.request("POST", "/api/pair", None, Some(body))
    }

    /// The locked-out answer: the lock is written before the request is
    /// read, so this probe connects and only listens (the kernel's own
    /// lockout tests do the same — a request written into a socket the
    /// server closes unread could come back as a reset, not a 429).
    fn assert_locked(&self) -> Result<()> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let mut response = String::new();
        stream.read_to_string(&mut response)?;
        assert!(
            response.contains("429 Too Many Requests"),
            "response: {response}"
        );
        assert!(response.contains("auth_locked"), "response: {response}");
        Ok(())
    }

    /// Redeem `code` and return the bearer and its id.
    fn pair_ok(&self, code: &str, label: &str) -> Result<(String, String)> {
        let (headers, body) = self.pair(&format!(r#"{{"code":"{code}","label":"{label}"}}"#))?;
        assert!(
            headers.contains("200 OK"),
            "headers: {headers} body: {body}"
        );
        let v: Value = serde_json::from_str(&body)?;
        Ok((
            v["token"].as_str().unwrap_or_default().to_string(),
            v["token_id"].as_str().unwrap_or_default().to_string(),
        ))
    }

    fn served_key(&self) -> Result<String> {
        let (_, body) = self.request("GET", "/api/sealed-log", Some(&self.capability()), None)?;
        let v: Value = serde_json::from_str(&body)?;
        Ok(v["verifying_key"].as_str().unwrap_or_default().to_string())
    }
}

impl Drop for TestApi {
    fn drop(&mut self) {
        if let Some(handle) = self.api_handle.take() {
            if let Err(err) = handle.stop() {
                eprintln!("Failed to stop API server: {err}");
            }
        }
    }
}

fn wrong_code(right: &str) -> &'static str {
    if right == "000000" {
        "000001"
    } else {
        "000000"
    }
}

fn file_mode(path: &Path) -> u32 {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::metadata(path)
            .expect("metadata")
            .permissions()
            .mode()
            & 0o777
    }
    #[cfg(not(unix))]
    {
        let _ = path;
        0o600
    }
}

#[test]
fn pair_redeems_a_code_for_a_bearer_that_opens_every_route() -> Result<()> {
    let api = TestApi::new(true)?;
    let code = api.issue_code()?;
    let code_path: PathBuf = api.paths().code_path.clone();
    assert_eq!(file_mode(&code_path), 0o600);
    assert!(
        !std::fs::read_to_string(&code_path)?.contains(&code),
        "the clear code is never on disk"
    );

    let (headers, body) = api.pair(&format!(r#"{{"code":"{code}","label":"home-assistant"}}"#))?;
    assert!(
        headers.contains("200 OK"),
        "headers: {headers} body: {body}"
    );
    let v: Value = serde_json::from_str(&body)?;
    let token = v["token"].as_str().expect("token");
    let token_id = v["token_id"].as_str().expect("token_id");
    assert_eq!(token.len(), 64);
    assert!(token.bytes().all(|b| b.is_ascii_hexdigit()));
    assert_eq!(token_id.len(), 8);
    assert_eq!(v["device_id"], witness_kernel::ha_device_id(SEED));
    assert_eq!(v["verifying_key"], api.served_key()?);
    assert_eq!(v["kernel_version"], "0.0.0-test");
    assert_eq!(
        v.as_object().map(|o| o.len()),
        Some(5),
        "exactly the five contract keys: {body}"
    );

    // The code is consumed; the bearer is hashed at rest, 0600.
    assert!(!code_path.exists(), "a redeemed code is deleted");
    let tokens_path = &api.paths().tokens_path;
    assert_eq!(file_mode(tokens_path), 0o600);
    assert!(!std::fs::read_to_string(tokens_path)?.contains(token));
    let set = ViewerTokenSet::load_kind(tokens_path, "paired token")?;
    assert_eq!(set.tokens.len(), 1);
    assert_eq!(set.tokens[0].id, token_id);
    assert_eq!(set.tokens[0].label, "home-assistant");
    assert!(set.matches(token).is_some());

    // Every door the capability token opens.
    for (method, path, want) in [
        ("GET", "/events", "200 OK"),
        ("GET", "/events?since_bucket=0", "200 OK"),
        ("GET", "/events/latest", "200 OK"),
        ("GET", "/digest", "200 OK"),
        ("GET", "/status", "404 Not Found"), // no sampler attached: past auth, not available
        ("POST", "/verify", "200 OK"),
        ("GET", "/api/sealed-log", "200 OK"),
        ("GET", "/api/sealed-log?from_seq=0", "200 OK"),
        ("GET", "/api/identity", "200 OK"),
    ] {
        let (headers, body) = api.request(method, path, Some(token), None)?;
        assert!(
            headers.contains(want),
            "{method} {path}: headers {headers} body {body}"
        );
    }
    let (_, identity) = api.request("GET", "/api/identity", Some(token), None)?;
    let identity: Value = serde_json::from_str(&identity)?;
    assert_eq!(identity["paired_tokens"], 1);

    // An export under the bearer is receipted as `paired` + its id, and the
    // bundle still verifies offline (the receipt is inside the signed bytes).
    let (headers, body) = api.request("GET", "/export/bundle", Some(token), None)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    let bundle: ExportBundle = serde_json::from_str(&body)?;
    verify_export_bundle(&bundle)?;
    assert_eq!(
        bundle.receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::Paired)
    );
    assert_eq!(
        bundle.receipt_entry.receipt.token_id.as_deref(),
        Some(token_id)
    );
    // And the capability token's export stays `api` with no token_id.
    let (_, body) = api.request("GET", "/export/bundle", Some(&api.capability()), None)?;
    let bundle: ExportBundle = serde_json::from_str(&body)?;
    assert_eq!(
        bundle.receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::Api)
    );
    assert_eq!(bundle.receipt_entry.receipt.token_id, None);
    Ok(())
}

#[test]
fn pair_refusals_are_one_constant_401_and_each_is_a_strike() -> Result<()> {
    // No code ever issued.
    let never_issued = TestApi::new(true)?;
    let (headers, body) = never_issued.pair(r#"{"code":"123456"}"#)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert_eq!(body, INVALID);
    drop(never_issued);

    // Pairing not configured at all (the add-on): the same answer.
    let unpaired = TestApi::new(false)?;
    let (headers, body) = unpaired.pair(r#"{"code":"123456"}"#)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert_eq!(body, INVALID);
    drop(unpaired);

    // Wrong guesses, from a fresh server so the strike count starts at
    // zero: counted on the file, each a per-IP strike; the fifth burns the
    // code AND locks the address (5 failures = lockout).
    let api = TestApi::new(true)?;
    let code = api.issue_code()?;
    let wrong = wrong_code(&code);
    for guess in 1..PAIRING_CODE_ATTEMPTS {
        let (headers, body) = api.pair(&format!(r#"{{"code":"{wrong}"}}"#))?;
        assert!(
            headers.contains("401 Unauthorized"),
            "guess {guess}: {headers}"
        );
        assert_eq!(body, INVALID, "guess {guess}");
        let on_disk = PairingCodeFile::load(&api.paths().code_path)?.expect("still issued");
        assert_eq!(on_disk.attempts_left, PAIRING_CODE_ATTEMPTS - guess);
    }
    let (headers, body) = api.pair(&format!(r#"{{"code":"{wrong}"}}"#))?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert_eq!(body, INVALID);
    assert!(
        !api.paths().code_path.exists(),
        "the fifth wrong guess burns the code"
    );
    // Five refusals lock the address.
    api.assert_locked()?;

    // After the lockout: the right code is refused like any other guess —
    // the code is gone, and the refusal says nothing about why.
    std::thread::sleep(std::time::Duration::from_millis(2_200));
    let (headers, body) = api.pair(&format!(r#"{{"code":"{code}"}}"#))?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert_eq!(body, INVALID);

    // An expired code is the same 401 and is deleted on that first touch.
    let expired = PairingCodeFile {
        attempts_left: PAIRING_CODE_ATTEMPTS,
        ..PairingCodeFile::issue(&api.paths().code_path, 600, "old", now() - 1_200)?
            .let_file(&api.paths().code_path)?
    };
    assert!(expired.is_expired(now()));
    let (headers, body) = api.pair(r#"{"code":"123456"}"#)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert_eq!(body, INVALID);
    assert!(
        !api.paths().code_path.exists(),
        "an expired code is removed"
    );
    Ok(())
}

/// Read back the file an `issue` wrote (the issue result carries only the
/// clear code).
trait LetFile {
    fn let_file(self, path: &Path) -> Result<PairingCodeFile>;
}

impl LetFile for witness_kernel::api::pairing::IssuedPairingCode {
    fn let_file(self, path: &Path) -> Result<PairingCodeFile> {
        PairingCodeFile::load(path)?.ok_or_else(|| anyhow::anyhow!("issued file missing"))
    }
}

#[test]
fn pair_malformed_input_is_a_400_and_not_a_strike() -> Result<()> {
    let api = TestApi::new(true)?;
    let code = api.issue_code()?;

    // No body at all.
    let (headers, body) = api.request("POST", "/api/pair", None, None)?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    assert!(body.contains("body_required"), "body: {body}");
    // Too large, refused on the declared length before a byte is read.
    let big = format!(r#"{{"code":"123456","label":"{}"}}"#, "x".repeat(1_100));
    let (headers, body) = api.pair(&big)?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    assert!(body.contains("body_too_large"), "body: {body}");
    // Not JSON, wrong code shape, label of the wrong type.
    for bad in [
        "not json",
        r#"{"code":"12345"}"#,
        r#"{"code":"1234567"}"#,
        r#"{"code":"12345a"}"#,
        r#"{"code":123456}"#,
        r#"{"label":"x"}"#,
        r#"{"code":"123456","label":7}"#,
    ] {
        let (headers, body) = api.pair(bad)?;
        assert!(headers.contains("400 Bad Request"), "{bad}: {headers}");
        assert!(body.contains("bad_request"), "{bad}: {body}");
    }
    // A truncated body (declared longer than sent) is a 400 too.
    let (headers, body) =
        api.request_raw("POST", "/api/pair", None, Some((40, r#"{"code":"1"#)))?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    assert!(body.contains("bad_request"), "body: {body}");

    // None of that counted: eleven malformed requests later the address is
    // not locked and the code still redeems.
    let (_, _) = api.pair_ok(&code, "home-assistant")?;
    Ok(())
}

#[test]
fn pair_route_shape() -> Result<()> {
    let api = TestApi::new(true)?;
    for (method, path, want) in [
        ("GET", "/api/pair", "405 Method Not Allowed"),
        ("DELETE", "/api/pair", "405 Method Not Allowed"),
        ("PUT", "/api/pair/abcdef01", "405 Method Not Allowed"),
        ("GET", "/api/pair/abcdef01", "405 Method Not Allowed"),
        ("DELETE", "/api/pair/not-an-id", "404 Not Found"),
        ("DELETE", "/api/pair/ABCDEF01", "404 Not Found"),
        ("DELETE", "/api/pair/", "404 Not Found"),
    ] {
        let (headers, _) = api.request(method, path, Some(&api.capability()), None)?;
        assert!(headers.contains(want), "{method} {path}: {headers}");
    }
    // A well-formed id is token-gated like everything else.
    let (headers, body) = api.request("DELETE", "/api/pair/abcdef01", None, None)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("missing_token"));
    // `?token=` stays refused on the new routes too.
    let (headers, _) = api.request(
        "GET",
        "/api/identity?token=abc",
        Some(&api.capability()),
        None,
    )?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    Ok(())
}

#[test]
fn paired_revocation_follows_the_self_only_rule() -> Result<()> {
    let api = TestApi::new(true)?;
    let (token_a, id_a) = {
        let code = api.issue_code()?;
        api.pair_ok(&code, "ha-one")?
    };
    let (token_b, id_b) = {
        let code = api.issue_code()?;
        api.pair_ok(&code, "ha-two")?
    };
    assert_ne!(id_a, id_b);

    // A paired bearer may revoke ONLY itself.
    let (headers, body) =
        api.request("DELETE", &format!("/api/pair/{id_b}"), Some(&token_a), None)?;
    assert!(headers.contains("403 Forbidden"), "headers: {headers}");
    assert_eq!(body, r#"{"error":"forbidden"}"#);
    let (headers, body) =
        api.request("DELETE", &format!("/api/pair/{id_a}"), Some(&token_a), None)?;
    assert!(
        headers.contains("200 OK"),
        "headers: {headers} body: {body}"
    );
    assert_eq!(body, format!(r#"{{"revoked":"{id_a}"}}"#));
    // ...and is refused from then on — immediately, the file is read per request.
    let (headers, body) = api.request("GET", "/events/latest", Some(&token_a), None)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("invalid_token"), "body: {body}");

    // The capability token may revoke any id; an unknown id is a 404.
    let (headers, _) = api.request(
        "DELETE",
        &format!("/api/pair/{id_a}"),
        Some(&api.capability()),
        None,
    )?;
    assert!(headers.contains("404 Not Found"), "headers: {headers}");
    let (headers, body) = api.request(
        "DELETE",
        &format!("/api/pair/{id_b}"),
        Some(&api.capability()),
        None,
    )?;
    assert!(
        headers.contains("200 OK"),
        "headers: {headers} body: {body}"
    );
    let (headers, _) = api.request("GET", "/events/latest", Some(&token_b), None)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    let (_, identity) = api.request("GET", "/api/identity", Some(&api.capability()), None)?;
    let identity: Value = serde_json::from_str(&identity)?;
    assert_eq!(identity["paired_tokens"], 0);

    // Revocation from the CLI's side of the file is just as immediate.
    let code = api.issue_code()?;
    let (token_c, id_c) = api.pair_ok(&code, "ha-three")?;
    let (headers, _) = api.request("GET", "/digest", Some(&token_c), None)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    ViewerTokenSet::revoke_kind(&api.paths().tokens_path, &id_c, "paired token")?;
    let (headers, _) = api.request("GET", "/digest", Some(&token_c), None)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");

    // The paired set is its own file: nothing pairing did created or
    // touched a viewer-credential file beside it.
    assert!(!api.dir.path().join("viewer_tokens.json").exists());
    Ok(())
}

#[test]
fn pairing_success_does_not_clear_strikes_but_a_paired_request_does() -> Result<()> {
    let api = TestApi::new(true)?;
    let code = api.issue_code()?;
    let wrong = wrong_code(&code);
    // Two strikes from wrong guesses...
    for _ in 0..2 {
        let (headers, _) = api.pair(&format!(r#"{{"code":"{wrong}"}}"#))?;
        assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    }
    // ...a success, which must NOT reset them (the viewer rule)...
    let (token, _) = api.pair_ok(&code, "home-assistant")?;
    // ...so three bad tokens reach five and the sixth request is locked out.
    let bogus = "0".repeat(64);
    for i in 0..3 {
        let (headers, _) = api.request("GET", "/digest", Some(&bogus), None)?;
        assert!(
            headers.contains("401 Unauthorized"),
            "bad token {i}: {headers}"
        );
    }
    api.assert_locked()?;

    // A paired-bearer success is a full-credential success: it clears the
    // history, so four more bad tokens do not lock the address.
    std::thread::sleep(std::time::Duration::from_millis(2_200));
    let (headers, _) = api.request("GET", "/digest", Some(&token), None)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    for i in 0..4 {
        let (headers, _) = api.request("GET", "/digest", Some(&bogus), None)?;
        assert!(
            headers.contains("401 Unauthorized"),
            "bad token {i}: {headers}"
        );
    }
    Ok(())
}
