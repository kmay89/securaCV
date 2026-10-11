//! Integration tests for the sealed-log incremental cursor
//! (`GET /api/sealed-log?from_seq=<n>`): every entry carries its `seq`, the
//! document names the chain `head`, a cursor serves only what follows it
//! and re-anchors the walk at the cursor row, and every credential that
//! may read the log (capability, viewer, paired) gets the same bytes.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use tempfile::tempdir;
use witness_kernel::api::pairing::{
    PairingCodeFile, PAIRED_TOKEN_FILE_NAME, PAIRING_CODE_FILE_NAME,
};
use witness_kernel::api::{
    ApiConfig, ApiHandle, ApiServer, PairingPaths, ViewerTokenSet, VIEWER_TOKEN_FILE_NAME,
};
use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    SealedLogDocument, TimeBucket, ZonePolicy,
};

fn kernel_config(db_path: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(60),
        device_key_seed: "devkey:test:cursor-0123456789abcdef".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) -> Result<()> {
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
            zone_id: zone.to_string(),
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

struct TestApi {
    dir: tempfile::TempDir,
    api_handle: Option<ApiHandle>,
}

impl TestApi {
    fn new(events: usize) -> Result<Self> {
        let dir = tempdir()?;
        let cfg = kernel_config(&dir.path().join("witness.db"));
        let mut kernel = Kernel::open(&cfg)?;
        for i in 0..events {
            seal_event(&mut kernel, &cfg, &format!("zone:z{i}"))?;
        }
        drop(kernel);
        let api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            viewer_token_path: Some(dir.path().join(VIEWER_TOKEN_FILE_NAME)),
            pairing: Some(PairingPaths {
                code_path: dir.path().join(PAIRING_CODE_FILE_NAME),
                tokens_path: dir.path().join(PAIRED_TOKEN_FILE_NAME),
            }),
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
        body: Option<&str>,
    ) -> Result<(String, String)> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let token_header = token
            .map(|t| format!("X-Witness-Token: {t}\r\n"))
            .unwrap_or_default();
        let length_header = body
            .map(|b| format!("Content-Length: {}\r\n", b.len()))
            .unwrap_or_default();
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}{length_header}\r\n{}",
            body.unwrap_or("")
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

    fn get(&self, path: &str) -> Result<(String, String)> {
        let token = self.handle().token.clone();
        self.request("GET", path, Some(&token), None)
    }

    fn document(&self, path: &str) -> Result<SealedLogDocument> {
        let (headers, body) = self.get(path)?;
        assert!(headers.contains("200 OK"), "{path}: {headers}");
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

#[test]
fn every_entry_carries_seq_and_the_document_names_the_head() -> Result<()> {
    let api = TestApi::new(3)?;
    let (_, body) = api.get("/api/sealed-log")?;
    let raw: Value = serde_json::from_str(&body)?;
    let doc: SealedLogDocument = serde_json::from_str(&body)?;
    assert_eq!(doc.entries.len(), 3);
    for (i, entry) in doc.entries.iter().enumerate() {
        assert_eq!(entry.seq, i as u64 + 1);
        assert_eq!(entry.seq, entry.id as u64, "seq is the row id");
        assert_eq!(raw["entries"][i]["seq"], i as u64 + 1);
    }
    assert_eq!(doc.head.seq, 3);
    assert_eq!(doc.head.entry_hash, doc.entries[2].entry_hash);
    assert_eq!(doc.from_seq, None);
    assert!(raw.get("from_seq").is_none(), "absent on a full document");
    assert!(raw.get("head").is_some());
    assert!(doc.checkpoint_head.is_none(), "a from-genesis walk");

    // Empty log: genesis head.
    let empty = TestApi::new(0)?;
    let doc = empty.document("/api/sealed-log")?;
    assert_eq!(doc.head.seq, 0);
    assert_eq!(doc.head.entry_hash, "0".repeat(64));
    assert!(doc.entries.is_empty());
    Ok(())
}

#[test]
fn from_seq_serves_the_rows_after_the_cursor_anchored_at_it() -> Result<()> {
    let api = TestApi::new(4)?;
    let full = api.document("/api/sealed-log")?;

    // From row 2: rows 3 and 4, anchored at row 2's hash, head unchanged.
    let doc = api.document("/api/sealed-log?from_seq=2")?;
    assert_eq!(
        doc.entries.iter().map(|e| e.seq).collect::<Vec<_>>(),
        vec![3, 4]
    );
    assert_eq!(doc.from_seq, Some(2));
    assert_eq!(
        doc.checkpoint_head.as_deref(),
        Some(full.entries[1].entry_hash.as_str())
    );
    assert_eq!(
        doc.entries[0].prev_hash, full.entries[1].entry_hash,
        "the first served row links to the anchor"
    );
    assert_eq!(doc.head, full.head);
    assert_eq!(doc.verifying_key, full.verifying_key);

    // From the head: nothing new, head still says where the log ends.
    let doc = api.document("/api/sealed-log?from_seq=4")?;
    assert!(doc.entries.is_empty());
    assert_eq!(doc.head.seq, 4);
    assert_eq!(
        doc.checkpoint_head.as_deref(),
        Some(full.entries[3].entry_hash.as_str())
    );

    // Ahead of the head (a rolled-back log): nothing served, head below the
    // cursor is the tell.
    let doc = api.document("/api/sealed-log?from_seq=9")?;
    assert!(doc.entries.is_empty());
    assert_eq!(doc.head.seq, 4);
    assert!(doc.head.seq < 9);

    // from_seq=0 is the full walk, echoed.
    let doc = api.document("/api/sealed-log?from_seq=0")?;
    assert_eq!(doc.entries.len(), 4);
    assert_eq!(doc.from_seq, Some(0));
    assert!(doc.checkpoint_head.is_none());

    // Unknown query keys are still ignored; a malformed cursor is a 400.
    let (_, plain) = api.get("/api/sealed-log")?;
    let (headers, queried) = api.get("/api/sealed-log?last=24h&zone=zone:a")?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    assert_eq!(plain, queried);
    for bad in ["abc", "-1", "1.5", ""] {
        let (headers, body) = api.get(&format!("/api/sealed-log?from_seq={bad}"))?;
        assert!(headers.contains("400 Bad Request"), "{bad:?}: {headers}");
        assert!(body.contains("bad_request"), "{bad:?}: {body}");
    }
    Ok(())
}

#[test]
fn every_reading_credential_gets_the_same_cursor_document() -> Result<()> {
    let api = TestApi::new(3)?;
    let capability = api.handle().token.clone();
    let viewer = ViewerTokenSet::mint(
        &api.dir.path().join(VIEWER_TOKEN_FILE_NAME),
        "tv",
        "11".repeat(32),
        None,
    )?
    .sealed_log_token;
    let code = PairingCodeFile::issue(
        &api.dir.path().join(PAIRING_CODE_FILE_NAME),
        600,
        "test",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)?
            .as_secs(),
    )?
    .code;
    let (headers, body) = api.request(
        "POST",
        "/api/pair",
        None,
        Some(&format!(r#"{{"code":"{code}"}}"#)),
    )?;
    assert!(
        headers.contains("200 OK"),
        "headers: {headers} body: {body}"
    );
    let paired: Value = serde_json::from_str(&body)?;
    let paired = paired["token"].as_str().expect("token").to_string();

    for path in ["/api/sealed-log", "/api/sealed-log?from_seq=1"] {
        let (_, by_capability) = api.request("GET", path, Some(&capability), None)?;
        let (headers, by_viewer) = api.request("GET", path, Some(&viewer), None)?;
        assert!(headers.contains("200 OK"), "viewer {path}: {headers}");
        let (headers, by_paired) = api.request("GET", path, Some(&paired), None)?;
        assert!(headers.contains("200 OK"), "paired {path}: {headers}");
        assert_eq!(by_capability, by_viewer, "{path}");
        assert_eq!(by_capability, by_paired, "{path}");
    }
    // The viewer's one door takes the cursor; a malformed cursor is its 400 too.
    let (headers, _) = api.request("GET", "/api/sealed-log?from_seq=x", Some(&viewer), None)?;
    assert!(headers.contains("400 Bad Request"), "headers: {headers}");
    Ok(())
}
