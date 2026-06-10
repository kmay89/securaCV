//! Integration tests for the token-gated `GET /status` storage-health
//! endpoint: auth posture matches `/events`, payload shape is stable, and
//! deployments without monitoring degrade to a clean 404.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::{Arc, RwLock};
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer};
use witness_kernel::storage_health::{
    MonitorSettings, SharedStorageHealth, StorageHealthMonitor, StorageHealthThresholds,
};
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

fn read_response(stream: &mut TcpStream) -> Result<(String, String)> {
    let mut response = String::new();
    stream.read_to_string(&mut response)?;
    let mut parts = response.splitn(2, "\r\n\r\n");
    let headers = parts.next().unwrap_or("").to_string();
    let body = parts.next().unwrap_or("").to_string();
    Ok((headers, body))
}

struct TestApi {
    _dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
}

impl TestApi {
    fn new(storage_health: Option<SharedStorageHealth>) -> Result<Self> {
        let dir = tempdir()?;
        let db_path = dir.path().join("witness.db");
        let cfg = kernel_config(&db_path);
        let kernel = Kernel::open(&cfg)?;
        drop(kernel);

        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            ..ApiConfig::default()
        };
        let mut server = ApiServer::new(api_config, cfg);
        if let Some(shared) = storage_health {
            server = server.with_storage_health(shared);
        }
        let api_handle = server.spawn()?;

        Ok(Self {
            _dir: dir,
            api_handle: Some(api_handle),
        })
    }

    fn handle(&self) -> &ApiHandle {
        self.api_handle
            .as_ref()
            .expect("test API handle should be initialized")
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

/// Build a populated snapshot by sampling a real monitor against a temp DB.
fn populated_snapshot() -> Result<(tempfile::TempDir, SharedStorageHealth)> {
    let dir = tempdir()?;
    let db_path = dir.path().join("witness.db");
    std::fs::write(&db_path, b"stub-db")?;
    let mut monitor = StorageHealthMonitor::new(
        MonitorSettings {
            endurance_tbw: 64.0,
            block_device: Some("securacv-test-nonexistent".to_string()),
            state_path: Some(dir.path().join("state.json")),
            thresholds: StorageHealthThresholds::default(),
        },
        db_path.to_str().unwrap(),
    );
    let report = monitor.sample()?;
    let shared: SharedStorageHealth = Arc::new(RwLock::new(Some(report)));
    Ok((dir, shared))
}

#[test]
fn status_requires_token() -> Result<()> {
    let (_state_dir, shared) = populated_snapshot()?;
    let api = TestApi::new(Some(shared))?;

    let mut stream = TcpStream::connect(api.handle().addr)?;
    stream.write_all(b"GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n")?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("missing_token"));

    Ok(())
}

#[test]
fn status_returns_report_with_token() -> Result<()> {
    let (_state_dir, shared) = populated_snapshot()?;
    let api = TestApi::new(Some(shared))?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request =
        format!("GET /status HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n");
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");

    let value: Value = serde_json::from_str(&body)?;
    assert_eq!(value["kernel_version"], "0.0.0-test");
    let storage = &value["storage"];
    assert_eq!(storage["status"], "good");
    assert_eq!(storage["db_bytes"], 7);
    assert_eq!(storage["endurance_tbw"], 64.0);
    assert!(storage.get("write_errors").is_some());
    assert!(storage.get("wear_pct").is_some());
    assert!(value["thermal"].get("status").is_some());
    // Invariant III posture: sample time is a coarse bucket, never a
    // precise timestamp.
    assert!(value["time_bucket"].get("start_epoch_s").is_some());
    assert_eq!(value["time_bucket"]["size_s"], 600);

    Ok(())
}

#[test]
fn status_unavailable_when_monitoring_not_attached() -> Result<()> {
    let api = TestApi::new(None)?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request =
        format!("GET /status HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n");
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("404 Not Found"), "headers: {headers}");
    assert!(body.contains("not_available"));

    Ok(())
}

#[test]
fn status_unavailable_before_first_sample() -> Result<()> {
    let shared: SharedStorageHealth = Arc::new(RwLock::new(None));
    let api = TestApi::new(Some(shared))?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request =
        format!("GET /status HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n");
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("404 Not Found"), "headers: {headers}");
    assert!(body.contains("not_available"));

    Ok(())
}

#[test]
fn health_endpoint_stays_public() -> Result<()> {
    let (_state_dir, shared) = populated_snapshot()?;
    let api = TestApi::new(Some(shared))?;

    let mut stream = TcpStream::connect(api.handle().addr)?;
    stream.write_all(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("200 OK"));
    assert!(body.contains(r#""status":"ok""#));

    Ok(())
}
