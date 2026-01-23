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
    ] {
        std::env::remove_var(key);
    }
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
    assert_eq!(cfg.zones.sensitive_zones, vec!["zone:front_boundary", "zone:loading_bay"]);
    assert_eq!(cfg.retention.as_secs(), 86400);

    clear_env();
}
