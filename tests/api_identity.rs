//! Integration tests for `GET /api/identity` and the `/api/fleet` roll-call
//! mode: who the kernel is (id, key, version, chain head), for a client
//! that pins it — token-gated (capability or paired, never viewer), in
//! contract field order, never the seed or a token.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use tempfile::tempdir;
use witness_kernel::api::{
    ApiConfig, ApiHandle, ApiServer, FleetRollcall, IdentityDocument, LastVerifySummary,
    ViewerTokenSet, VIEWER_TOKEN_FILE_NAME,
};
use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:test:identity-0123456789abcdef";

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
    fn new(
        prepare: impl FnOnce(&mut Kernel, &KernelConfig) -> Result<()>,
        configure: impl FnOnce(&mut ApiConfig, &Path),
    ) -> Result<Self> {
        let dir = tempdir()?;
        let cfg = kernel_config(&dir.path().join("witness.db"));
        let mut kernel = Kernel::open(&cfg)?;
        prepare(&mut kernel, &cfg)?;
        drop(kernel);
        let mut api_config = ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            ..ApiConfig::default()
        };
        configure(&mut api_config, dir.path());
        let api_handle = ApiServer::new(api_config, cfg).spawn()?;
        Ok(Self {
            dir,
            api_handle: Some(api_handle),
        })
    }

    fn handle(&self) -> &ApiHandle {
        self.api_handle.as_ref().expect("api handle")
    }

    fn request(&self, method: &str, path: &str, token: Option<&str>) -> Result<(String, String)> {
        let mut stream = TcpStream::connect(self.handle().addr)?;
        let token_header = token
            .map(|t| format!("X-Witness-Token: {t}\r\n"))
            .unwrap_or_default();
        let request = format!("{method} {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}\r\n");
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
        self.request("GET", path, Some(&token))
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
fn api_identity_requires_token() -> Result<()> {
    let api = TestApi::new(
        |kernel, cfg| seal_event(kernel, cfg, "zone:a"),
        |cfg, dir| cfg.viewer_token_path = Some(dir.join(VIEWER_TOKEN_FILE_NAME)),
    )?;
    let (headers, body) = api.request("GET", "/api/identity", None)?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("missing_token"), "body: {body}");

    // The viewer credential opens exactly one door, and this is not it.
    let receipt = ViewerTokenSet::mint(
        &api.dir.path().join(VIEWER_TOKEN_FILE_NAME),
        "tv",
        "11".repeat(32),
        None,
    )?;
    let (headers, body) = api.request("GET", "/api/identity", Some(&receipt.sealed_log_token))?;
    assert!(headers.contains("401 Unauthorized"), "headers: {headers}");
    assert!(body.contains("invalid_token"), "body: {body}");

    let (headers, _) = api.request("POST", "/api/identity", Some(&api.handle().token))?;
    assert!(
        headers.contains("405 Method Not Allowed"),
        "headers: {headers}"
    );
    Ok(())
}

#[test]
fn api_identity_reports_key_and_chain_head() -> Result<()> {
    let api = TestApi::new(
        |kernel, cfg| {
            seal_event(kernel, cfg, "zone:a")?;
            seal_event(kernel, cfg, "zone:b")
        },
        |_, _| {},
    )?;
    let (headers, body) = api.get("/api/identity")?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    let v: Value = serde_json::from_str(&body)?;

    let (_, doc) = api.get("/api/sealed-log")?;
    let doc: Value = serde_json::from_str(&doc)?;
    let entries = doc["entries"].as_array().expect("entries");
    assert_eq!(entries.len(), 2);

    assert_eq!(v["v"], 1);
    assert_eq!(v["device_id"], witness_kernel::ha_device_id(SEED));
    assert_eq!(v["verifying_key"], doc["verifying_key"]);
    assert_eq!(v["key_algorithm"], "ed25519");
    assert_eq!(v["kernel_version"], "0.0.0-test");
    assert_eq!(v["ruleset_id"], "ruleset:test");
    assert_eq!(v["chain_length"], 2);
    assert_eq!(v["latest_hash"], entries[1]["entry_hash"]);
    assert_eq!(v["latest_hash"], doc["head"]["entry_hash"]);
    assert!(v["last_verify"].is_null(), "no verify has run: {body}");
    assert_eq!(v["fleet_rollcall"], "full");
    assert_eq!(v["paired_tokens"], 0);
    for forbidden in ["seed", "token", "sha256", "zones", "events", "lan_policy"] {
        assert!(
            !body.contains(&format!("\"{forbidden}\"")),
            "{forbidden} must not be in the identity: {body}"
        );
    }
    // Contract field order on the wire.
    let order = [
        "v",
        "device_id",
        "verifying_key",
        "key_algorithm",
        "kernel_version",
        "ruleset_id",
        "chain_length",
        "latest_hash",
        "last_verify",
        "fleet_rollcall",
        "paired_tokens",
    ];
    let positions: Vec<usize> = order
        .iter()
        .map(|k| {
            body.find(&format!("\"{k}\":"))
                .unwrap_or_else(|| panic!("{k}: {body}"))
        })
        .collect();
    assert!(positions.windows(2).all(|w| w[0] < w[1]), "order: {body}");

    // After a verify, the kernel's self-report rides along — coarse `at`.
    let (headers, report) = api.request("POST", "/verify", Some(&api.handle().token))?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    let report: Value = serde_json::from_str(&report)?;
    let (_, body) = api.get("/api/identity")?;
    let v: Value = serde_json::from_str(&body)?;
    assert_eq!(v["last_verify"]["ok"], true);
    assert_eq!(v["last_verify"]["at"], report["verified_at_bucket_start"]);
    assert_eq!(
        v["last_verify"]["bucket_size"],
        report["verified_at_bucket_size"]
    );
    assert_eq!(v["last_verify"]["verdict"], report["verdict"]);
    assert_eq!(v["last_verify"]["identity_verified"], false);
    assert_eq!(v["last_verify"]["at"].as_u64().unwrap_or(1) % 600, 0);

    // An empty log: genesis head, length 0.
    let empty = TestApi::new(|_, _| Ok(()), |_, _| {})?;
    let (_, body) = empty.get("/api/identity")?;
    let v: Value = serde_json::from_str(&body)?;
    assert_eq!(v["chain_length"], 0);
    assert_eq!(v["latest_hash"], "0".repeat(64));
    Ok(())
}

// C4 against the shared vector (tests/fixtures/ha_contract/identity.json,
// written by gen_ha_contract_fixtures.py): the SAME bytes the integration's
// coordinator test feeds its backstop, so a renamed or reordered field shows
// on both sides from one file.
#[test]
fn api_identity_vector_is_the_document_the_integration_reads() -> Result<()> {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ha_contract/identity.json");
    let fixture: Value = serde_json::from_str(&std::fs::read_to_string(&path)?)?;
    let wire = fixture["payload"].as_str().expect("payload");
    let key = "d04ab232742bb4ab3a1368bd4615e4e6d0224ab71a016baf8520a332c9778737";
    let document = IdentityDocument {
        v: 1,
        device_id: "pwk_1a2b3c4d".to_string(),
        verifying_key: key.to_string(),
        key_algorithm: "ed25519".to_string(),
        kernel_version: "2.4.16".to_string(),
        ruleset_id: "ruleset:frigate_v1".to_string(),
        chain_length: 1287,
        latest_hash: "9c".repeat(32),
        last_verify: Some(LastVerifySummary {
            at: 1_760_140_200,
            bucket_size: 600,
            ok: true,
            verdict: "self-consistent; identity unverified".to_string(),
            identity_verified: false,
        }),
        fleet_rollcall: "self".to_string(),
        paired_tokens: 1,
    };
    assert_eq!(serde_json::to_string(&document)?, wire);
    // The fingerprint the pairing step, `pair` and the panel all show.
    assert_eq!(
        fixture["fingerprint"],
        witness_kernel::api::pairing::key_fingerprint(key)?.as_str()
    );
    // The document parses back (the coordinator's reads are a subset).
    let parsed: IdentityDocument = serde_json::from_str(wire)?;
    assert_eq!(parsed.chain_length, 1287);
    assert_eq!(parsed.last_verify.map(|v| v.at), Some(1_760_140_200));
    Ok(())
}

fn plant_peers(path: &Path) -> Result<()> {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs();
    let json = serde_json::json!({
        "schema": witness_kernel::fleet_peers::FLEET_PEERS_SCHEMA,
        "written_at_epoch_s": now,
        "peers": [
            {
                "device_id": "canary-sense-01",
                "name": "Bedroom",
                "device_type": "canary-sense",
                "last_seen_epoch_s": now - 10,
                "chain": "unknown",
                "presence": "present"
            }
        ]
    });
    std::fs::write(path, json.to_string())?;
    Ok(())
}

#[test]
fn fleet_rollcall_self_serves_only_the_kernel_row() -> Result<()> {
    // Full (the default): the planted peer is listed beside the kernel.
    let full = TestApi::new(
        |_, _| Ok(()),
        |cfg, dir| {
            let path = dir.join("fleet_peers.json");
            plant_peers(&path).expect("plant");
            cfg.fleet_peers_path = Some(path);
        },
    )?;
    let (headers, body) = full.request("GET", "/api/fleet", None)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    let v: Value = serde_json::from_str(&body)?;
    assert_eq!(
        v["devices"].as_array().map(Vec::len),
        Some(2),
        "body: {body}"
    );
    let (_, identity) = full.get("/api/identity")?;
    let identity: Value = serde_json::from_str(&identity)?;
    assert_eq!(identity["fleet_rollcall"], "full");

    // Self: the same file on disk, and the open read says nothing about
    // who else is home.
    let own = TestApi::new(
        |_, _| Ok(()),
        |cfg, dir| {
            let path = dir.join("fleet_peers.json");
            plant_peers(&path).expect("plant");
            cfg.fleet_peers_path = Some(path);
            cfg.fleet_rollcall = FleetRollcall::SelfOnly;
        },
    )?;
    let (headers, body) = own.request("GET", "/api/fleet", None)?;
    assert!(headers.contains("200 OK"), "headers: {headers}");
    let v: Value = serde_json::from_str(&body)?;
    assert_eq!(
        v["devices"].as_array().map(Vec::len),
        Some(1),
        "body: {body}"
    );
    assert_eq!(v["devices"][0]["product"], "witness-kernel");
    assert!(!body.contains("Bedroom"), "body: {body}");
    let (_, identity) = own.get("/api/identity")?;
    let identity: Value = serde_json::from_str(&identity)?;
    assert_eq!(identity["fleet_rollcall"], "self");

    assert_eq!(FleetRollcall::parse("full")?, FleetRollcall::Full);
    assert_eq!(FleetRollcall::parse(" SELF ")?, FleetRollcall::SelfOnly);
    assert!(FleetRollcall::parse("everyone").is_err());
    Ok(())
}
