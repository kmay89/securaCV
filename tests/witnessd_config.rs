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
