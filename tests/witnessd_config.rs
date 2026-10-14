use std::sync::Mutex;

use tempfile::NamedTempFile;

use witness_kernel::config::WitnessdConfig;

static ENV_LOCK: Mutex<()> = Mutex::new(());

fn clear_env() {
    for key in [
        "WITNESS_CONFIG",
        "WITNESS_API_ADDR",
        "WITNESS_API_TOKEN_PATH",
        "WITNESS_RTSP_URL",
        "WITNESS_ZONE_ID",
        "WITNESS_SENSITIVE_ZONES",
        "WITNESS_RETENTION_SECS",
        "WITNESS_RETENTION_CHECK_INTERVAL_SECS",
        "WITNESS_STORAGE_HEALTH_ENABLED",
        "WITNESS_STORAGE_HEALTH_INTERVAL_SECS",
        "WITNESS_STORAGE_HEALTH_TBW",
        "WITNESS_STORAGE_HEALTH_DEVICE",
        "WITNESS_SQLITE_SYNCHRONOUS",
        "WITNESS_API_PAIRED_TOKEN_PATH",
        "WITNESS_API_PAIRING_CODE_PATH",
        "WITNESS_API_PAIRING_TTL_SECS",
        "WITNESS_API_FLEET_ROLLCALL",
    ] {
        std::env::remove_var(key);
    }
}

#[test]
fn api_access_env_round_trips_and_defaults_beside_the_token() {
    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();
    std::env::set_var("WITNESS_RTSP_URL", "rtsp://camera-1");

    // Defaults: full roll-call, 600 s codes, pairing files beside the token.
    std::env::set_var("WITNESS_API_TOKEN_PATH", "/data/api_token");
    let cfg = WitnessdConfig::load().expect("load config");
    assert_eq!(
        cfg.api_access.fleet_rollcall,
        witness_kernel::api::FleetRollcall::Full
    );
    assert_eq!(cfg.api_access.pairing_ttl_secs, 600);
    assert_eq!(
        cfg.api_access.paired_token_path.as_deref(),
        Some(std::path::Path::new("/data/paired_tokens.json"))
    );
    assert_eq!(
        cfg.api_access.pairing_code_path.as_deref(),
        Some(std::path::Path::new("/data/pairing_code"))
    );
    let paths = cfg.api_access.pairing_paths().expect("pairing on");
    assert_eq!(
        paths.code_path,
        std::path::PathBuf::from("/data/pairing_code")
    );

    // Env names win over the defaults.
    std::env::set_var("WITNESS_API_PAIRED_TOKEN_PATH", "/secrets/paired.json");
    std::env::set_var("WITNESS_API_PAIRING_CODE_PATH", "/secrets/code");
    std::env::set_var("WITNESS_API_PAIRING_TTL_SECS", "300");
    std::env::set_var("WITNESS_API_FLEET_ROLLCALL", "self");
    let cfg = WitnessdConfig::load().expect("load config with env");
    assert_eq!(
        cfg.api_access.paired_token_path.as_deref(),
        Some(std::path::Path::new("/secrets/paired.json"))
    );
    assert_eq!(
        cfg.api_access.pairing_code_path.as_deref(),
        Some(std::path::Path::new("/secrets/code"))
    );
    assert_eq!(cfg.api_access.pairing_ttl_secs, 300);
    assert_eq!(
        cfg.api_access.fleet_rollcall,
        witness_kernel::api::FleetRollcall::SelfOnly
    );

    // No token path and a lone half: refused (a redeemable code with
    // nowhere to keep the bearer). A bad roll-call word and a zero TTL too.
    std::env::remove_var("WITNESS_API_TOKEN_PATH");
    std::env::remove_var("WITNESS_API_PAIRED_TOKEN_PATH");
    let err = WitnessdConfig::load().expect_err("a lone pairing_code_path must be rejected");
    assert!(err.to_string().contains("go together"), "got: {err}");
    std::env::remove_var("WITNESS_API_PAIRING_CODE_PATH");
    let cfg = WitnessdConfig::load().expect("neither half is fine");
    assert_eq!(cfg.api_access.pairing_paths(), None);
    std::env::set_var("WITNESS_API_FLEET_ROLLCALL", "everyone");
    let err = WitnessdConfig::load().expect_err("unknown roll-call word");
    assert!(
        err.to_string().contains("WITNESS_API_FLEET_ROLLCALL"),
        "got: {err}"
    );
    std::env::set_var("WITNESS_API_FLEET_ROLLCALL", "full");
    std::env::set_var("WITNESS_API_PAIRING_TTL_SECS", "0");
    let err = WitnessdConfig::load().expect_err("zero ttl");
    assert!(err.to_string().contains("pairing_ttl_secs"), "got: {err}");

    clear_env();
}

#[test]
fn loads_config_from_file_and_env_overrides() {
    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();

    let mut file = NamedTempFile::new().expect("temp config");
    let token_path = file.path().with_extension("token");
    let json = format!(
        r#"{{
            "db_path": "witness_prod.db",
            "ruleset_id": "ruleset:v0.2",
            "api": {{
                "addr": "0.0.0.0:9000",
                "token_path": "{}"
            }},
            "rtsp": {{
                "url": "rtsp://camera-1",
                "target_fps": 12,
                "width": 800,
                "height": 600
            }},
            "zones": {{
                "module_zone_id": "zone:front_boundary",
                "sensitive": ["zone:front_boundary", "zone:loading_bay"]
            }},
            "retention": {{
                "seconds": 43200
            }}
        }}"#,
        token_path.display()
    );
    std::io::Write::write_all(&mut file, json.as_bytes()).expect("write config");

    std::env::set_var("WITNESS_CONFIG", file.path());
    std::env::set_var("WITNESS_ZONE_ID", "zone:rear_gate");
    std::env::set_var("WITNESS_RETENTION_SECS", "86400");

    let cfg = WitnessdConfig::load().expect("load config");

    assert_eq!(cfg.db_path, "witness_prod.db");
    assert_eq!(cfg.ruleset_id, "ruleset:v0.2");
    assert_eq!(cfg.api_addr, "0.0.0.0:9000");
    assert_eq!(cfg.api_token_path.unwrap(), token_path);
    assert_eq!(cfg.rtsp.url, "rtsp://camera-1");
    assert_eq!(cfg.rtsp.target_fps, 12);
    assert_eq!(cfg.rtsp.width, 800);
    assert_eq!(cfg.rtsp.height, 600);
    assert_eq!(cfg.zones.module_zone_id, "zone:rear_gate");
    assert_eq!(
        cfg.zones.sensitive_zones,
        vec!["zone:front_boundary", "zone:loading_bay"]
    );
    assert_eq!(cfg.retention.as_secs(), 86400);

    clear_env();
}

#[test]
fn storage_health_defaults_apply_without_config() {
    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();

    // Satisfy the default rtsp backend's url requirement; unrelated to
    // storage health.
    std::env::set_var("WITNESS_RTSP_URL", "rtsp://camera-1");
    let cfg = WitnessdConfig::load().expect("load config");
    std::env::remove_var("WITNESS_RTSP_URL");

    assert!(cfg.storage_health.enabled);
    assert_eq!(cfg.storage_health.check_interval.as_secs(), 600);
    assert_eq!(cfg.storage_health.endurance_tbw, 64.0);
    assert_eq!(cfg.storage_health.block_device, None);
    assert_eq!(cfg.retention_check_interval.as_secs(), 300);
    assert_eq!(cfg.storage_health.thresholds.free_space_warn_pct, 15.0);
    assert_eq!(cfg.storage_health.thresholds.wear_warn_pct, 80.0);
    assert_eq!(
        cfg.storage_health.sqlite_synchronous,
        witness_kernel::SqliteSynchronous::Full
    );

    clear_env();
}

#[test]
fn storage_health_config_parses_and_env_overrides() {
    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();

    let mut file = NamedTempFile::new().expect("temp config");
    let json = r#"{
        "rtsp": {"url": "rtsp://camera-1"},
        "retention": {
            "seconds": 43200,
            "check_interval_seconds": 120
        },
        "storage_health": {
            "enabled": true,
            "check_interval_seconds": 900,
            "endurance_tbw": 128.0,
            "block_device": "mmcblk0",
            "free_space_warn_pct": 20.0,
            "free_space_critical_pct": 8.0,
            "wear_warn_pct": 70.0,
            "wear_critical_pct": 90.0,
            "sqlite_synchronous": "normal"
        }
    }"#;
    std::io::Write::write_all(&mut file, json.as_bytes()).expect("write config");

    std::env::set_var("WITNESS_CONFIG", file.path());
    let cfg = WitnessdConfig::load().expect("load config");
    assert_eq!(cfg.retention_check_interval.as_secs(), 120);
    assert_eq!(cfg.storage_health.check_interval.as_secs(), 900);
    assert_eq!(cfg.storage_health.endurance_tbw, 128.0);
    assert_eq!(cfg.storage_health.block_device.as_deref(), Some("mmcblk0"));
    assert_eq!(cfg.storage_health.thresholds.free_space_warn_pct, 20.0);
    assert_eq!(cfg.storage_health.thresholds.wear_warn_pct, 70.0);
    assert_eq!(
        cfg.storage_health.sqlite_synchronous,
        witness_kernel::SqliteSynchronous::Normal
    );

    // Env overrides win over the file.
    std::env::set_var("WITNESS_STORAGE_HEALTH_ENABLED", "false");
    std::env::set_var("WITNESS_STORAGE_HEALTH_TBW", "256");
    std::env::set_var("WITNESS_STORAGE_HEALTH_DEVICE", "sda");
    std::env::set_var("WITNESS_STORAGE_HEALTH_INTERVAL_SECS", "300");
    std::env::set_var("WITNESS_RETENTION_CHECK_INTERVAL_SECS", "600");
    std::env::set_var("WITNESS_SQLITE_SYNCHRONOUS", "full");
    let cfg = WitnessdConfig::load().expect("load config with env");
    assert!(!cfg.storage_health.enabled);
    assert_eq!(cfg.storage_health.endurance_tbw, 256.0);
    assert_eq!(cfg.storage_health.block_device.as_deref(), Some("sda"));
    assert_eq!(cfg.storage_health.check_interval.as_secs(), 300);
    assert_eq!(cfg.retention_check_interval.as_secs(), 600);
    assert_eq!(
        cfg.storage_health.sqlite_synchronous,
        witness_kernel::SqliteSynchronous::Full
    );

    clear_env();
}

#[test]
fn storage_health_rejects_invalid_values() {
    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();

    // Non-positive endurance rating.
    let mut file = NamedTempFile::new().expect("temp config");
    std::io::Write::write_all(
        &mut file,
        br#"{"rtsp": {"url": "rtsp://c"}, "storage_health": {"endurance_tbw": 0.0}}"#,
    )
    .expect("write config");
    std::env::set_var("WITNESS_CONFIG", file.path());
    let err = WitnessdConfig::load().expect_err("zero endurance_tbw must be rejected");
    assert!(err.to_string().contains("endurance_tbw"), "got: {err}");

    // Zero check interval.
    let mut file = NamedTempFile::new().expect("temp config");
    std::io::Write::write_all(
        &mut file,
        br#"{"rtsp": {"url": "rtsp://c"}, "storage_health": {"check_interval_seconds": 0}}"#,
    )
    .expect("write config");
    std::env::set_var("WITNESS_CONFIG", file.path());
    let err = WitnessdConfig::load().expect_err("zero interval must be rejected");
    assert!(
        err.to_string().contains("check_interval_seconds"),
        "got: {err}"
    );

    // Inverted free-space thresholds.
    let mut file = NamedTempFile::new().expect("temp config");
    std::io::Write::write_all(
        &mut file,
        br#"{"rtsp": {"url": "rtsp://c"}, "storage_health": {"free_space_warn_pct": 5.0, "free_space_critical_pct": 10.0}}"#,
    )
    .expect("write config");
    std::env::set_var("WITNESS_CONFIG", file.path());
    let err = WitnessdConfig::load().expect_err("inverted thresholds must be rejected");
    assert!(
        err.to_string().contains("free_space_critical_pct"),
        "got: {err}"
    );

    // Unknown sqlite_synchronous mode.
    let mut file = NamedTempFile::new().expect("temp config");
    std::io::Write::write_all(
        &mut file,
        br#"{"rtsp": {"url": "rtsp://c"}, "storage_health": {"sqlite_synchronous": "off"}}"#,
    )
    .expect("write config");
    std::env::set_var("WITNESS_CONFIG", file.path());
    let err = WitnessdConfig::load().expect_err("'off' must be rejected");
    assert!(err.to_string().contains("sqlite_synchronous"), "got: {err}");

    clear_env();
}

/// What `witnessd` SERVES follows what its config says: the pairing files
/// and the roll-call mode are wired into its event API, not merely parsed.
/// Runs the real `witnessd` binary (stub camera) with `fleet_rollcall:
/// "self"` and pairing paths beside the token, issues a code with
/// `witness_api issue-pairing-code` over the same config, redeems it on the
/// witnessd-served `POST /api/pair`, and reads `/api/identity` with the
/// paired bearer: `fleet_rollcall` is `self` and the bearer counts.
#[test]
fn witnessd_serves_the_pairing_and_roll_call_its_config_names() {
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};

    let _guard = ENV_LOCK.lock().unwrap();
    clear_env();

    const SEED: &str = "devkey:test:witnessd-pairing-0123456789abcdef";
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let token_path = dir.path().join("api_token");
    let code_path = dir.path().join("pairing_code");
    let tokens_path = dir.path().join("paired_tokens.json");
    let config_path = dir.path().join("witness_config.json");
    // A free loopback port, released before witnessd binds it.
    let port = TcpListener::bind("127.0.0.1:0")
        .expect("bind")
        .local_addr()
        .expect("addr")
        .port();
    let addr = format!("127.0.0.1:{port}");
    // One `api` block for both binaries: witnessd's file carries its camera
    // and storage settings too, and `witness_api`'s stricter schema (the
    // sidecar's and add-on's file) takes the api block alone.
    let api_block = serde_json::json!({
        "addr": addr,
        "token_path": token_path,
        "paired_token_path": tokens_path,
        "pairing_code_path": code_path,
        "fleet_rollcall": "self"
    });
    std::fs::write(
        &config_path,
        serde_json::json!({
            "db_path": db_path,
            "ruleset_id": "ruleset:test",
            "api": api_block,
            "rtsp": { "url": "stub://test", "target_fps": 2, "width": 64, "height": 64 },
            "storage_health": { "enabled": false }
        })
        .to_string(),
    )
    .expect("write config");
    let cli_config_path = dir.path().join("witness_api_config.json");
    std::fs::write(
        &cli_config_path,
        serde_json::json!({
            "db_path": db_path,
            "ruleset_id": "ruleset:test",
            "api": api_block,
        })
        .to_string(),
    )
    .expect("write cli config");

    let mut child = Command::new(env!("CARGO_BIN_EXE_witnessd"))
        .env("WITNESS_CONFIG", &config_path)
        .env("DEVICE_KEY_SEED", SEED)
        .env("RUST_LOG", "info")
        .env_remove("WITNESS_API_ADDR")
        .env_remove("WITNESS_API_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRED_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRING_CODE_PATH")
        .env_remove("WITNESS_API_FLEET_ROLLCALL")
        .env_remove("WITNESS_API_ALLOW_INSECURE")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .expect("spawn witnessd");
    // Drain stderr on a thread so a chatty daemon never blocks on a full
    // pipe; the lines are kept for the failure message.
    let stderr = child.stderr.take().expect("stderr piped");
    let log_lines = std::sync::Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
    let drain = {
        let log_lines = std::sync::Arc::clone(&log_lines);
        std::thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                log_lines.lock().unwrap().push(line);
            }
        })
    };
    let logs = || log_lines.lock().unwrap().join("\n");

    let request = |method: &str, path: &str, token: Option<&str>, body: Option<&str>| {
        let mut stream = TcpStream::connect(&addr)?;
        stream.set_read_timeout(Some(Duration::from_secs(10)))?;
        let token_header = token
            .map(|t| format!("X-Witness-Token: {t}\r\n"))
            .unwrap_or_default();
        let body = body.unwrap_or("");
        let request = format!(
            "{method} {path} HTTP/1.1\r\nHost: localhost\r\n{token_header}Content-Length: {}\r\n\r\n{body}",
            body.len()
        );
        stream.write_all(request.as_bytes())?;
        let mut response = String::new();
        stream.read_to_string(&mut response)?;
        let mut parts = response.splitn(2, "\r\n\r\n");
        Ok::<_, std::io::Error>((
            parts.next().unwrap_or("").to_string(),
            parts.next().unwrap_or("").to_string(),
        ))
    };

    // Up: /health answers and the capability token is on disk.
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        if let Ok((headers, _)) = request("GET", "/health", None, None) {
            if headers.starts_with("HTTP/1.1 200") && token_path.exists() {
                break;
            }
        }
        if let Ok(Some(status)) = child.try_wait() {
            panic!(
                "witnessd exited early ({status})\n--- stderr ---\n{}",
                logs()
            );
        }
        assert!(
            Instant::now() < deadline,
            "witnessd never answered /health on {addr}\n--- stderr ---\n{}",
            logs()
        );
        std::thread::sleep(Duration::from_millis(200));
    }

    // The operator's half over the same config: the code file lands where
    // witnessd reads it.
    let issued = Command::new(env!("CARGO_BIN_EXE_witness_api"))
        .args(["issue-pairing-code", "--ttl-secs", "120"])
        .env("WITNESS_CONFIG", &cli_config_path)
        .env("DEVICE_KEY_SEED", SEED)
        .env_remove("WITNESS_API_ADDR")
        .env_remove("WITNESS_API_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRED_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRING_CODE_PATH")
        .env_remove("WITNESS_API_FLEET_ROLLCALL")
        .output()
        .expect("witness_api runs");
    assert!(
        issued.status.success(),
        "issue-pairing-code failed: {}",
        String::from_utf8_lossy(&issued.stderr)
    );
    let receipt: serde_json::Value =
        serde_json::from_str(String::from_utf8_lossy(&issued.stdout).trim()).expect("receipt");
    let code = receipt["pairing_code"].as_str().expect("code").to_string();
    assert!(code_path.exists(), "the code file is beside the token");

    // witnessd redeems it: pairing is wired, not just parsed.
    let body = format!(r#"{{"code":"{code}","label":"home-assistant"}}"#);
    let (headers, body) = request("POST", "/api/pair", None, Some(&body)).expect("pair");
    assert!(
        headers.starts_with("HTTP/1.1 200"),
        "witnessd must redeem the code its config names: {headers}\n{body}\n--- stderr ---\n{}",
        logs()
    );
    let paired: serde_json::Value = serde_json::from_str(&body).expect("pair body");
    let token = paired["token"].as_str().expect("token").to_string();
    assert_eq!(paired["device_id"], witness_kernel::ha_device_id(SEED));
    assert!(!code_path.exists(), "a redeemed code is consumed");
    assert!(
        tokens_path.exists(),
        "the bearer set is written where configured"
    );

    // And the roll-call mode reaches the served identity document.
    let (headers, body) = request("GET", "/api/identity", Some(&token), None).expect("identity");
    assert!(headers.starts_with("HTTP/1.1 200"), "{headers}\n{body}");
    let identity: serde_json::Value = serde_json::from_str(&body).expect("identity body");
    assert_eq!(identity["fleet_rollcall"], "self", "{identity}");
    assert_eq!(identity["paired_tokens"], 1, "{identity}");

    // The daemon says what it serves.
    child.kill().expect("stop witnessd");
    let _ = child.wait();
    let _ = drain.join();
    let logs = logs();
    assert!(logs.contains("pairing enabled"), "--- stderr ---\n{logs}");
    assert!(
        logs.contains("fleet roll-call: self"),
        "--- stderr ---\n{logs}"
    );

    clear_env();
}
