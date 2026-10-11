//! D5: which routes leave an export receipt. The state reads
//! (`/events/latest`, `/digest`, `/events?since_bucket=`, `/api/sealed-log`,
//! `/api/identity`) are receipt-less — N of them add zero rows to the
//! receipt ledger — while one `/export/bundle` adds exactly one and the
//! bulk `/events` adds one more. Counted through `POST /verify`'s
//! `export_receipts_verified`, which walks the whole receipt ledger.
//!
//! Also pins the paired receipt's wire form: `auth_mode` stays a plain
//! string and `token_id` is the last optional field, so the offline
//! verifiers that re-serialize receipts field by field stay honest.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use tempfile::tempdir;
use witness_kernel::api::{ApiConfig, ApiHandle, ApiServer};
use witness_kernel::{
    CandidateEvent, EventType, ExportAuthMode, ExportReceipt, ExportWindow, InferenceBackend,
    Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

fn kernel_config(db_path: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:receipts-0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

struct TestApi {
    _dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
}

impl TestApi {
    fn new() -> Result<Self> {
        let dir = tempdir()?;
        let cfg = kernel_config(&dir.path().join("witness.db"));
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        for zone in ["zone:a", "zone:b"] {
            kernel.append_event_checked(
                &desc,
                CandidateEvent {
                    event_type: EventType::BoundaryCrossingObjectLarge,
                    time_bucket: TimeBucket::now(600)?,
                    zone_id: zone.to_string(),
                    confidence: 0.6,
                    correlation_token: None,
                    attestation: None,
                },
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )?;
        }
        drop(kernel);
        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            rate_limit_per_minute: 0,
            ..ApiConfig::default()
        };
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
        Ok(Self {
            _dir: dir,
            api_handle: Some(api_handle),
        })
    }

    fn handle(&self) -> &ApiHandle {
        self.api_handle.as_ref().expect("api handle")
    }

    fn request(&self, method: &str, path: &str) -> Result<(String, String)> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: localhost\r\nX-Witness-Token: {}\r\n\r\n",
            self.handle().token
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

    /// Rows in the export-receipt ledger, as the kernel's own verify
    /// counts them.
    fn receipt_rows(&self) -> Result<u64> {
        let (headers, body) = self.request("POST", "/verify")?;
        assert!(headers.contains("200 OK"), "headers: {headers}");
        let report: Value = serde_json::from_str(&body)?;
        assert_eq!(report["chain_valid"], true, "report: {body}");
        Ok(report["export_receipts_verified"].as_u64().expect("count"))
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
fn state_reads_add_no_receipt_rows_and_an_export_adds_exactly_one() -> Result<()> {
    let api = TestApi::new()?;
    assert_eq!(api.receipt_rows()?, 0, "a fresh log has no receipts");

    // N state reads of every receipt-less route.
    let mut latest_seq = None;
    for _ in 0..5 {
        for path in [
            "/events/latest",
            "/digest",
            "/events?since_bucket=0",
            "/api/sealed-log",
            "/api/sealed-log?from_seq=1",
            "/api/identity",
        ] {
            let (headers, body) = api.request("GET", path)?;
            assert!(headers.contains("200 OK"), "{path}: {headers}");
            if path == "/events/latest" {
                let v: Value = serde_json::from_str(&body)?;
                latest_seq = v.get("seq").and_then(|s| s.as_u64());
            }
        }
    }
    assert_eq!(
        api.receipt_rows()?,
        0,
        "thirty state reads must not add a single receipt row"
    );
    assert_eq!(
        latest_seq,
        Some(2),
        "the state view carries each event's sealed-log seq"
    );

    // One disclosure act: one receipt.
    let (headers, _) = api.request("GET", "/export/bundle")?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    assert_eq!(api.receipt_rows()?, 1);

    // The bulk /events export is an artifact hand-over: one more receipt.
    let (headers, body) = api.request("GET", "/events")?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    assert_eq!(api.receipt_rows()?, 2);
    assert!(
        !body.contains("\"seq\""),
        "a receipted artifact never carries seq (the offline verifiers re-serialize it): {body}"
    );
    // More state reads afterwards: still two.
    for _ in 0..3 {
        api.request("GET", "/events?since_bucket=0")?;
        api.request("GET", "/digest")?;
    }
    assert_eq!(api.receipt_rows()?, 2);
    Ok(())
}

#[test]
fn since_bucket_filters_on_true_bucket_starts_and_rejects_junk() -> Result<()> {
    let api = TestApi::new()?;
    let (_, body) = api.request("GET", "/events?since_bucket=0")?;
    let all: Value = serde_json::from_str(&body)?;
    let count = |v: &Value| -> usize {
        v["batches"]
            .as_array()
            .map(|batches| {
                batches
                    .iter()
                    .flat_map(|b| b["buckets"].as_array().cloned().unwrap_or_default())
                    .map(|bk| bk["events"].as_array().map(Vec::len).unwrap_or(0))
                    .sum()
            })
            .unwrap_or(0)
    };
    assert_eq!(count(&all), 2);

    // A cursor at the current bucket's true start keeps the bucket; one a
    // bucket past it drops everything.
    let now_bucket = TimeBucket::now_10min()?.start_epoch_s;
    let (_, body) = api.request("GET", &format!("/events?since_bucket={now_bucket}"))?;
    assert_eq!(count(&serde_json::from_str(&body)?), 2);
    let (_, body) = api.request("GET", &format!("/events?since_bucket={}", now_bucket + 600))?;
    assert_eq!(count(&serde_json::from_str(&body)?), 0);

    for bad in ["abc", "-5", "1e3"] {
        let (headers, body) = api.request("GET", &format!("/events?since_bucket={bad}"))?;
        assert!(headers.contains("400 Bad Request"), "{bad}: {headers}");
        assert!(body.contains("bad_request"), "{bad}: {body}");
    }
    Ok(())
}

#[test]
fn paired_receipt_keeps_auth_mode_a_string_and_token_id_last() -> Result<()> {
    let receipt = ExportReceipt {
        time_bucket: TimeBucket {
            start_epoch_s: 600,
            size_s: 600,
        },
        ruleset_hash: [1u8; 32],
        batch_size: 50,
        artifact_hash: [2u8; 32],
        auth_mode: Some(ExportAuthMode::Paired),
        window: Some(ExportWindow {
            start_epoch_s: 600,
            end_epoch_s: 1200,
        }),
        token_id: Some("deadbeef".to_string()),
    };
    let json = serde_json::to_string(&receipt)?;
    assert!(
        json.ends_with(
            r#""auth_mode":"paired","window":{"start_epoch_s":600,"end_epoch_s":1200},"token_id":"deadbeef"}"#
        ),
        "paired receipt wire form: {json}"
    );
    let back: ExportReceipt = serde_json::from_str(&json)?;
    assert_eq!(back.auth_mode, Some(ExportAuthMode::Paired));
    assert_eq!(back.token_id.as_deref(), Some("deadbeef"));
    assert_eq!(
        serde_json::to_string(&back)?,
        json,
        "byte-stable round trip"
    );

    // Every other credential class leaves token_id off the wire entirely.
    let api = ExportReceipt {
        auth_mode: Some(ExportAuthMode::Api),
        token_id: None,
        ..receipt
    };
    let json = serde_json::to_string(&api)?;
    assert!(!json.contains("token_id"), "{json}");
    assert!(json.contains(r#""auth_mode":"api""#));
    Ok(())
}
