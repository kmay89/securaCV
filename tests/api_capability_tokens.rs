use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer};
use witness_kernel::{
    CandidateEvent, EventType, Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

fn add_test_event(kernel: &mut Kernel, cfg: &KernelConfig) -> Result<()> {
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        },
        zone_id: "zone:test".to_string(),
        confidence: 0.5,
        correlation_token: None,
    };
    kernel.append_event_checked(
        &desc,
        cand,
        &cfg.kernel_version,
        &cfg.ruleset_id,
        cfg.ruleset_hash,
    )?;
    Ok(())
}

fn kernel_config(db_path: &std::path::Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn contains_key(value: &Value, key: &str) -> bool {
    match value {
        Value::Object(map) => map.iter().any(|(k, v)| k == key || contains_key(v, key)),
        Value::Array(items) => items.iter().any(|v| contains_key(v, key)),
        _ => false,
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
    fn new(setup_kernel: impl FnOnce(&mut Kernel, &KernelConfig) -> Result<()>) -> Result<Self> {
        let dir = tempdir()?;
        let db_path = dir.path().join("witness.db");
        let cfg = kernel_config(&db_path);
        let mut kernel = Kernel::open(&cfg)?;
        setup_kernel(&mut kernel, &cfg)?;
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
}

impl Drop for TestApi {
    fn drop(&mut self) {
        if let Some(handle) = self.api_handle.take() {
            handle.stop().expect("failed to stop API server");
        }
    }
}

#[test]
fn api_rejects_missing_token() -> Result<()> {
    let api = TestApi::new(|kernel, cfg| add_test_event(kernel, cfg))?;

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request = "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n";
    stream.write_all(request.as_bytes())?;
    let (headers, _body) = read_response(&mut stream)?;
    assert!(headers.contains("401 Unauthorized"));

    Ok(())
}

#[test]
fn api_returns_export_events_without_identifiers() -> Result<()> {
    let api = TestApi::new(|kernel, cfg| add_test_event(kernel, cfg))?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request =
        format!("GET /events HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n");
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("200 OK"));

    let value: Value = serde_json::from_str(&body)?;
    assert!(value.get("batches").is_some());
    assert!(!contains_key(&value, "correlation_token"));
    assert!(!contains_key(&value, "created_at"));
    assert!(!contains_key(&value, "event_id"));

    Ok(())
}

#[test]
fn api_health_endpoint_is_public() -> Result<()> {
    let api = TestApi::new(|_kernel, _cfg| Ok(()))?;

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("200 OK"));
    assert!(body.contains(r#""status":"ok""#));

    Ok(())
}

#[test]
fn api_latest_event_endpoint_returns_event() -> Result<()> {
    let api = TestApi::new(|kernel, cfg| add_test_event(kernel, cfg))?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request = format!(
        "GET /events/latest HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n"
    );
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("200 OK"));

    let value: Value = serde_json::from_str(&body)?;
    assert_eq!(value["event_type"], "BoundaryCrossingObjectLarge");
    assert_eq!(value["zone_id"], "zone:test");

    Ok(())
}

#[test]
fn api_latest_event_endpoint_returns_not_found_when_empty() -> Result<()> {
    let api = TestApi::new(|_kernel, _cfg| Ok(()))?;
    let token = api.handle().token.clone();

    let mut stream = TcpStream::connect(api.handle().addr)?;
    let request = format!(
        "GET /events/latest HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {token}\r\n\r\n"
    );
    stream.write_all(request.as_bytes())?;
    let (headers, body) = read_response(&mut stream)?;
    assert!(headers.contains("404 Not Found"));
    assert!(body.contains(r#""error":"no_events""#));

    Ok(())
}
