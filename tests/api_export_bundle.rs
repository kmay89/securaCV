//! Integration tests for the token-gated `GET /export/bundle` one-click
//! export endpoint: auth posture matches `/events`, the response is a
//! self-verifying ExportBundle marked as a download, the receipt is labeled
//! `api`, and window queries are validated/bucket-aligned.

use anyhow::Result;
use std::io::{Read, Write};
use std::net::TcpStream;
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer};
use witness_kernel::{
    verify_export_bundle, CandidateEvent, EventType, ExportAuthMode, ExportBundle,
    InferenceBackend, Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

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
    fn new() -> Result<Self> {
        let dir = tempdir()?;
        let db_path = dir.path().join("witness.db");
        let cfg = kernel_config(&db_path);
        let mut kernel = Kernel::open(&cfg)?;
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
                zone_id: "zone:test".to_string(),
                confidence: 0.5,
                correlation_token: None,
            },
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        drop(kernel);

        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            ..ApiConfig::default()
        };
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
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

    fn get(&self, path: &str, with_token: bool) -> Result<(String, String)> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let token_header = if with_token {
            format!("X-Witness-Token: {}\r\n", self.handle().token)
        } else {
            String::new()
        };
        let request = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}\r\n");
        stream.write_all(request.as_bytes())?;
        read_response(&mut stream)
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
fn export_bundle_requires_token() -> Result<()> {
    let api = TestApi::new()?;
    let (headers, body) = api.get("/export/bundle", false)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("missing_token"));
    Ok(())
}

#[test]
fn export_bundle_downloads_self_verifying_bundle() -> Result<()> {
    let api = TestApi::new()?;
    let (headers, body) = api.get("/export/bundle", true)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    assert!(
        headers.contains("Content-Disposition: attachment; filename=\"securacv-events-"),
        "headers: {headers}"
    );

    let bundle: ExportBundle = serde_json::from_str(&body)?;
    // The downloaded file is self-verifying offline (signed receipt bound to
    // the artifact) and honestly labeled with the credential used.
    verify_export_bundle(&bundle)?;
    assert_eq!(
        bundle.receipt_entry.receipt.auth_mode,
        Some(ExportAuthMode::Api)
    );
    assert_eq!(bundle.receipt_entry.receipt.window, None);
    Ok(())
}

#[test]
fn export_bundle_window_is_recorded_and_aligned() -> Result<()> {
    let api = TestApi::new()?;
    let (headers, body) = api.get("/export/bundle?last=24h", true)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");

    let bundle: ExportBundle = serde_json::from_str(&body)?;
    verify_export_bundle(&bundle)?;
    let window = bundle
        .receipt_entry
        .receipt
        .window
        .expect("window recorded on the signed receipt");
    assert_eq!(window.start_epoch_s % 600, 0);
    assert_eq!(window.end_epoch_s % 600, 0);
    assert!(window.end_epoch_s - window.start_epoch_s >= 24 * 3600);
    Ok(())
}

#[test]
fn export_bundle_rejects_bad_windows() -> Result<()> {
    let api = TestApi::new()?;
    for query in [
        "?last=yesterday",
        "?start=600",                   // end missing
        "?start=1800&end=600",          // inverted
        "?last=24h&start=600&end=1200", // both forms
        "?unknown=1",
    ] {
        let (headers, body) = api.get(&format!("/export/bundle{query}"), true)?;
        assert!(
            headers.contains("400 Bad Request"),
            "query {query}: headers {headers}"
        );
        assert!(body.contains("bad_window"), "query {query}: body {body}");
    }
    Ok(())
}
