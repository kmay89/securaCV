//! Integration tests for the event API's per-IP request rate limit and the
//! at-rest database encryption default.
//!
//! Rate limit: the auth-failure tracker already throttles clients that FAIL
//! auth; these tests pin that a client that HOLDS the token (or none) is
//! capped too, that `/health` stays unlimited, and that 0 disables the cap.
//!
//! Encryption: `Kernel::open` keys SQLCipher from the device seed, so a
//! kernel-created database must never be readable as plaintext SQLite. This
//! pins the default so a dependency or feature-flag change can't silently
//! regress it.

use anyhow::Result;
use std::io::{Read, Write};
use std::net::TcpStream;
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer};
use witness_kernel::{Kernel, KernelConfig, ZonePolicy};

fn kernel_config(db_path: &std::path::Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

struct TestApi {
    _dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
}

impl TestApi {
    fn new(rate_limit_per_minute: u32) -> Result<Self> {
        let dir = tempdir()?;
        let db_path = dir.path().join("witness.db");
        let cfg = kernel_config(&db_path);
        drop(Kernel::open(&cfg)?);

        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            rate_limit_per_minute,
            ..ApiConfig::default()
        };
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
        Ok(Self {
            _dir: dir,
            api_handle: Some(api_handle),
        })
    }

    fn get(&self, path: &str, with_token: bool) -> Result<(String, String)> {
        let handle = self.api_handle.as_ref().expect("api handle");
        let mut stream = TcpStream::connect(handle.addr)?;
        let token_header = if with_token {
            format!("X-Witness-Token: {}\r\n", handle.token)
        } else {
            String::new()
        };
        let request = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}\r\n");
        stream.write_all(request.as_bytes())?;
        let mut response = String::new();
        stream.read_to_string(&mut response)?;
        let mut parts = response.splitn(2, "\r\n\r\n");
        Ok((
            parts.next().unwrap_or("").to_string(),
            parts.next().unwrap_or("").to_string(),
        ))
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

#[test]
fn requests_over_the_limit_get_429_with_retry_after() -> Result<()> {
    let api = TestApi::new(3)?;
    for i in 0..3 {
        let (headers, _) = api.get("/digest", true)?;
        assert!(headers.contains("200 OK"), "request {i}: {headers}");
    }
    let (headers, body) = api.get("/digest", true)?;
    assert!(
        headers.contains("429 Too Many Requests"),
        "headers: {headers}"
    );
    assert!(body.contains("rate_limited"), "body: {body}");
    assert!(body.contains("retry_after"), "body: {body}");
    Ok(())
}

#[test]
fn limit_applies_before_token_validation() -> Result<()> {
    // A tokenless client cannot spend the API's time either: after the cap
    // it sees 429, not another 401 round through the auth machinery.
    let api = TestApi::new(2)?;
    for _ in 0..2 {
        let (headers, _) = api.get("/digest", false)?;
        assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    }
    let (headers, body) = api.get("/digest", false)?;
    assert!(
        headers.contains("429 Too Many Requests"),
        "headers: {headers}"
    );
    assert!(body.contains("rate_limited"), "body: {body}");
    Ok(())
}

#[test]
fn health_endpoint_is_never_rate_limited() -> Result<()> {
    let api = TestApi::new(1)?;
    for i in 0..5 {
        let (headers, body) = api.get("/health", false)?;
        assert!(headers.contains("200 OK"), "probe {i}: {headers}");
        assert!(body.contains(r#""status":"ok""#));
    }
    Ok(())
}

#[test]
fn zero_disables_the_limit() -> Result<()> {
    let api = TestApi::new(0)?;
    for i in 0..10 {
        let (headers, _) = api.get("/digest", true)?;
        assert!(headers.contains("200 OK"), "request {i}: {headers}");
    }
    Ok(())
}

#[test]
fn kernel_database_is_encrypted_at_rest_by_default() -> Result<()> {
    let dir = tempdir()?;
    let db_path = dir.path().join("witness.db");
    drop(Kernel::open(&kernel_config(&db_path))?);

    // A plaintext SQLite file starts with the magic "SQLite format 3\0";
    // SQLCipher writes ciphertext from byte 0.
    let header = &std::fs::read(&db_path)?[..16];
    assert_ne!(
        header, b"SQLite format 3\0",
        "kernel database must not be plaintext SQLite"
    );

    // And opening it without the derived key must not expose any schema.
    let conn = rusqlite::Connection::open(&db_path)?;
    let result: rusqlite::Result<i64> =
        conn.query_row("SELECT COUNT(*) FROM sqlite_master", [], |row| row.get(0));
    assert!(
        result.is_err(),
        "reading a kernel database without the key must fail"
    );
    Ok(())
}
