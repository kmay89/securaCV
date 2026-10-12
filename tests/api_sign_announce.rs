//! Integration tests for `POST /api/sign-announce`: token-gated, body
//! bounded, signs only a body that names this kernel, and the signature is
//! plain Ed25519 over `securacv-announce|v1|` + the exact body bytes under
//! the key `/api/identity` serves — what the integration verifies against
//! its pinned key.

use anyhow::Result;
use ed25519_dalek::VerifyingKey;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use tempfile::tempdir;
use witness_kernel::api::announce::{verify_announce, SIGN_ANNOUNCE_MAX_BYTES};
use witness_kernel::api::{
    ApiConfig, ApiHandle, ApiServer, ViewerTokenSet, VIEWER_TOKEN_FILE_NAME,
};
use witness_kernel::{Kernel, KernelConfig, ZonePolicy};

const SEED: &str = "devkey:test:announce-0123456789abcdef";

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

struct TestApi {
    dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
}

impl TestApi {
    fn new() -> Result<Self> {
        let dir = tempdir()?;
        let cfg = kernel_config(&dir.path().join("witness.db"));
        drop(Kernel::open(&cfg)?);
        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            viewer_token_path: Some(dir.path().join(VIEWER_TOKEN_FILE_NAME)),
            ..ApiConfig::default()
        };
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
        Ok(Self {
            dir,
            api_handle: Some(api_handle),
        })
    }

    fn handle(&self) -> &ApiHandle {
        self.api_handle.as_ref().expect("api handle")
    }

    fn request(
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
        Ok((
            parts.next().unwrap_or("").to_string(),
            parts.next().unwrap_or("").to_string(),
        ))
    }

    fn sign(&self, body: &str) -> Result<(String, String)> {
        let token = self.handle().token.clone();
        self.request(
            "POST",
            "/api/sign-announce",
            Some(&token),
            Some((body.len(), body)),
        )
    }

    fn identity(&self) -> Result<Value> {
        let token = self.handle().token.clone();
        let (headers, body) = self.request("GET", "/api/identity", Some(&token), None)?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        Ok(serde_json::from_str(&body)?)
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

/// A sig-less announce naming `identity`, as the bridge builds it.
fn announce_for(identity: &Value) -> String {
    format!(
        r#"{{"v":1,"device_id":"{}","kernel_version":"{}","verifying_key":"{}","api":{{"scheme":"http","bind":"all","port":8799}},"pairing":"required","mqtt_topic_prefix":"witness","discovery_prefix":"homeassistant","availability_topic":"witness/status","announced_bucket_start":1760140800,"announced_bucket_size":600}}"#,
        identity["device_id"].as_str().unwrap(),
        identity["kernel_version"].as_str().unwrap(),
        identity["verifying_key"].as_str().unwrap(),
    )
}

#[test]
fn sign_announce_signs_a_self_naming_body_under_the_served_key() -> Result<()> {
    let api = TestApi::new()?;
    let identity = api.identity()?;
    assert_eq!(identity["device_id"], witness_kernel::ha_device_id(SEED));
    let body = announce_for(&identity);

    let (headers, response) = api.sign(&body)?;
    assert!(
        headers.contains("200 OK"),
        "headers: {headers} body: {response}"
    );
    let response: Value = serde_json::from_str(&response)?;
    let sig = response["sig"].as_str().expect("sig");
    assert_eq!(sig.len(), 128);
    assert!(sig.bytes().all(|b| b.is_ascii_hexdigit()));
    assert_eq!(response.as_object().map(|o| o.len()), Some(1));

    // The integration's check: the pinned key, the prefix, the exact bytes.
    let key_bytes: [u8; 32] = hex::decode(identity["verifying_key"].as_str().unwrap())?
        .try_into()
        .expect("32 bytes");
    let key = VerifyingKey::from_bytes(&key_bytes)?;
    verify_announce(&key, body.as_bytes(), sig)?;
    // The signed bytes are the bytes sent: a re-spaced body is another message.
    let respaced = body.replace(",\"pairing\"", ", \"pairing\"");
    assert!(verify_announce(&key, respaced.as_bytes(), sig).is_err());
    // Signing is deterministic (Ed25519), so the bridge may re-sign freely.
    let (_, again) = api.sign(&body)?;
    let again: Value = serde_json::from_str(&again)?;
    assert_eq!(again["sig"], sig);
    Ok(())
}

#[test]
fn sign_announce_is_token_gated_and_body_bounded() -> Result<()> {
    let api = TestApi::new()?;
    let identity = api.identity()?;
    let body = announce_for(&identity);

    let (headers, response) = api.request(
        "POST",
        "/api/sign-announce",
        None,
        Some((body.len(), &body)),
    )?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(response.contains("missing_token"));

    // A viewer token is not a signing credential.
    let viewer = ViewerTokenSet::mint(
        &api.dir.path().join(VIEWER_TOKEN_FILE_NAME),
        "tv",
        "11".repeat(32),
        None,
    )?
    .sealed_log_token;
    let (headers, response) = api.request(
        "POST",
        "/api/sign-announce",
        Some(&viewer),
        Some((body.len(), &body)),
    )?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(response.contains("invalid_token"));

    let token = api.handle().token.clone();
    let (headers, _) = api.request("GET", "/api/sign-announce", Some(&token), None)?;
    assert!(
        headers.contains("405 Method Not Allowed"),
        "headers: {headers}"
    );

    let (headers, response) = api.request("POST", "/api/sign-announce", Some(&token), None)?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    assert!(response.contains("body_required"), "body: {response}");

    let padded = format!(
        r#"{{"device_id":"x","verifying_key":"y","pad":"{}"}}"#,
        "p".repeat(SIGN_ANNOUNCE_MAX_BYTES)
    );
    let (headers, response) = api.sign(&padded)?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    assert!(response.contains("body_too_large"), "body: {response}");
    Ok(())
}

#[test]
fn sign_announce_refuses_a_body_that_is_not_this_kernels_announce() -> Result<()> {
    let api = TestApi::new()?;
    let identity = api.identity()?;
    let good = announce_for(&identity);
    let other_key = "7f".repeat(32);

    let cases = [
        ("[1,2,3]".to_string(), "must be a JSON object"),
        ("not json".to_string(), "must be a JSON object"),
        (
            good.replace(
                r#""announced_bucket_size":600}"#,
                r#""announced_bucket_size":600,"sig":"00"}"#,
            ),
            "must not carry a sig",
        ),
        (
            good.replace(identity["device_id"].as_str().unwrap(), "pwk_ffffffff"),
            "must name this kernel",
        ),
        (
            good.replace(identity["verifying_key"].as_str().unwrap(), &other_key),
            "must name this kernel",
        ),
        (r#"{"v":1}"#.to_string(), "must name this kernel"),
    ];
    for (body, detail) in cases {
        let (headers, response) = api.sign(&body)?;
        assert!(headers.contains("400 Bad Request"), "{body}: {headers}");
        let v: Value = serde_json::from_str(&response)?;
        assert_eq!(v["error"], "bad_request", "{body}: {response}");
        assert!(
            v["detail"].as_str().unwrap_or_default().contains(detail),
            "{body}: {response}"
        );
    }
    // None of those were strikes against the token holder: a signing of
    // the good body still works, and no lockout has built up.
    let (headers, _) = api.sign(&good)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    Ok(())
}
