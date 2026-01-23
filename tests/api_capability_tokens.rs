use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiServer};
use witness_kernel::{
    CandidateEvent, EventType, Kernel, KernelConfig, ModuleDescriptor, TimeBucket,
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

#[test]
fn api_rejects_missing_token() -> Result<()> {
    let dir = tempdir()?;
    let db_path = dir.path().join("witness.db");
    let cfg = KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test".to_string(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    drop(kernel);

    let api_config = ApiConfig {
        addr: "127.0.0.1:0".to_string(),
        ..ApiConfig::default()
    };
    let api_handle = ApiServer::new(api_config, cfg.clone()).spawn()?;

    let mut stream = TcpStream::connect(api_handle.addr)?;
    let request = "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n";
    stream.write_all(request.as_bytes())?;
    let (headers, _body) = read_response(&mut stream)?;
    assert!(headers.contains("401 Unauthorized"));

    api_handle.stop()?;
    Ok(())
}

#[test]
fn api_returns_export_events_without_identifiers() -> Result<()> {
    let dir = tempdir()?;
    let db_path = dir.path().join("witness.db");
    let cfg = KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test".to_string(),
    };
    let mut kernel = Kernel::open(&cfg)?;
    add_test_event(&mut kernel, &cfg)?;
    drop(kernel);

    let api_config = ApiConfig {
        addr: "127.0.0.1:0".to_string(),
        ..ApiConfig::default()
    };
    let api_handle = ApiServer::new(api_config, cfg.clone()).spawn()?;
    let token = api_handle.token.clone();

    let mut stream = TcpStream::connect(api_handle.addr)?;
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

    api_handle.stop()?;
    Ok(())
}
