use anyhow::{anyhow, Result};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::time::Duration;

const DEFAULT_DB_PATH: &str = "witness.db";
const DEFAULT_RULESET_ID: &str = "ruleset:v0.1";
const DEFAULT_API_ADDR: &str = "127.0.0.1:8799";
const DEFAULT_RTSP_URL: &str = "stub://front_camera";
const DEFAULT_RTSP_FPS: u32 = 10;
const DEFAULT_RTSP_WIDTH: u32 = 640;
const DEFAULT_RTSP_HEIGHT: u32 = 480;
const DEFAULT_RETENTION_SECS: u64 = 60 * 60 * 24 * 7;
const DEFAULT_MODULE_ZONE_ID: &str = "zone:front_boundary";

#[derive(Debug, Deserialize, Default)]
struct WitnessdConfigFile {
    db_path: Option<String>,
    ruleset_id: Option<String>,
    api: Option<ApiConfigFile>,
    rtsp: Option<RtspConfigFile>,
    zones: Option<ZoneConfigFile>,
    retention: Option<RetentionConfigFile>,
}

#[derive(Debug, Deserialize, Default)]
struct WitnessApiConfigFile {
    db_path: Option<String>,
    ruleset_id: Option<String>,
    api: Option<ApiConfigFile>,
    zones: Option<ZoneConfigFile>,
    retention: Option<RetentionConfigFile>,
}

#[derive(Debug, Deserialize, Default)]
struct ApiConfigFile {
    addr: Option<String>,
    token_path: Option<PathBuf>,
}

#[derive(Debug, Deserialize, Default)]
struct RtspConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
struct ZoneConfigFile {
    module_zone_id: Option<String>,
    sensitive: Option<Vec<String>>,
}

#[derive(Debug, Deserialize, Default)]
struct RetentionConfigFile {
    seconds: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct WitnessdConfig {
    pub db_path: String,
    pub ruleset_id: String,
    pub api_addr: String,
    pub api_token_path: Option<PathBuf>,
    pub rtsp: RtspSettings,
    pub zones: ZoneSettings,
    pub retention: Duration,
}

#[derive(Debug, Clone)]
pub struct WitnessApiConfig {
    pub db_path: String,
    pub ruleset_id: String,
    pub api_addr: String,
    pub api_token_path: Option<PathBuf>,
    pub sensitive_zones: Vec<String>,
    pub retention: Duration,
}

#[derive(Debug, Clone)]
pub struct RtspSettings {
    pub url: String,
    pub target_fps: u32,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Clone)]
pub struct ZoneSettings {
    pub module_zone_id: String,
    pub sensitive_zones: Vec<String>,
}

impl WitnessdConfig {
    pub fn load() -> Result<Self> {
        let config_path = std::env::var("WITNESS_CONFIG").ok();
        let file_cfg = match config_path.as_deref() {
            Some(path) => Some(read_config_file(Path::new(path))?),
            None => None,
        };
        let mut cfg = Self::from_file(file_cfg.unwrap_or_default())?;
        cfg.apply_env()?;
        cfg.validate()?;
        Ok(cfg)
    }

    fn from_file(file: WitnessdConfigFile) -> Result<Self> {
        let db_path = file.db_path.unwrap_or_else(|| DEFAULT_DB_PATH.to_string());
        let ruleset_id = file
            .ruleset_id
            .unwrap_or_else(|| DEFAULT_RULESET_ID.to_string());
        let api_addr = file
            .api
            .as_ref()
            .and_then(|api| api.addr.clone())
            .unwrap_or_else(|| DEFAULT_API_ADDR.to_string());
        let api_token_path = file.api.and_then(|api| api.token_path);
        let rtsp = RtspSettings {
            url: file
                .rtsp
                .as_ref()
                .and_then(|rtsp| rtsp.url.clone())
                .unwrap_or_else(|| DEFAULT_RTSP_URL.to_string()),
            target_fps: file
                .rtsp
                .as_ref()
                .and_then(|rtsp| rtsp.target_fps)
                .unwrap_or(DEFAULT_RTSP_FPS),
            width: file
                .rtsp
                .as_ref()
                .and_then(|rtsp| rtsp.width)
                .unwrap_or(DEFAULT_RTSP_WIDTH),
            height: file
                .rtsp
                .as_ref()
                .and_then(|rtsp| rtsp.height)
                .unwrap_or(DEFAULT_RTSP_HEIGHT),
        };
        let zones = ZoneSettings {
            module_zone_id: file
                .zones
                .as_ref()
                .and_then(|zones| zones.module_zone_id.clone())
                .unwrap_or_else(|| DEFAULT_MODULE_ZONE_ID.to_string()),
            sensitive_zones: file
                .zones
                .and_then(|zones| zones.sensitive)
                .unwrap_or_else(|| vec![DEFAULT_MODULE_ZONE_ID.to_string()]),
        };
        let retention = Duration::from_secs(
            file.retention
                .and_then(|retention| retention.seconds)
                .unwrap_or(DEFAULT_RETENTION_SECS),
        );
        Ok(Self {
            db_path,
            ruleset_id,
            api_addr,
            api_token_path,
            rtsp,
            zones,
            retention,
        })
    }

    fn apply_env(&mut self) -> Result<()> {
        if let Ok(addr) = std::env::var("WITNESS_API_ADDR") {
            if !addr.trim().is_empty() {
                self.api_addr = addr;
            }
        }
        if let Ok(path) = std::env::var("WITNESS_API_TOKEN_PATH") {
            if !path.trim().is_empty() {
                self.api_token_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(url) = std::env::var("WITNESS_RTSP_URL") {
            if !url.trim().is_empty() {
                self.rtsp.url = url;
            }
        }
        if let Ok(zone_id) = std::env::var("WITNESS_ZONE_ID") {
            if !zone_id.trim().is_empty() {
                self.zones.module_zone_id = zone_id;
            }
        }
        if let Ok(zones) = std::env::var("WITNESS_SENSITIVE_ZONES") {
            let parsed = split_csv(&zones);
            if !parsed.is_empty() {
                self.zones.sensitive_zones = parsed;
            }
        }
        if let Ok(retention) = std::env::var("WITNESS_RETENTION_SECS") {
            let seconds: u64 = retention.parse().map_err(|_| {
                anyhow!("WITNESS_RETENTION_SECS must be an integer number of seconds")
            })?;
            self.retention = Duration::from_secs(seconds);
        }
        Ok(())
    }

    fn validate(&mut self) -> Result<()> {
        crate::validate_zone_id(&self.zones.module_zone_id)?;
        self.zones.module_zone_id = self.zones.module_zone_id.to_lowercase();

        let policy = crate::ZonePolicy::new(self.zones.sensitive_zones.clone())?;
        self.zones.sensitive_zones = policy.sensitive_zones;

        if self.retention.as_secs() == 0 {
            return Err(anyhow!("retention must be greater than zero"));
        }
        Ok(())
    }
}

impl WitnessApiConfig {
    pub fn load() -> Result<Self> {
        let config_path = std::env::var("WITNESS_CONFIG").ok();
        let file_cfg = match config_path.as_deref() {
            Some(path) => Some(read_config_file::<WitnessApiConfigFile>(Path::new(path))?),
            None => None,
        };
        let mut cfg = Self::from_file(file_cfg.unwrap_or_default())?;
        cfg.apply_env()?;
        cfg.validate()?;
        Ok(cfg)
    }

    fn from_file(file: WitnessApiConfigFile) -> Result<Self> {
        let db_path = file.db_path.unwrap_or_else(|| DEFAULT_DB_PATH.to_string());
        let ruleset_id = file
            .ruleset_id
            .unwrap_or_else(|| DEFAULT_RULESET_ID.to_string());
        let api_addr = file
            .api
            .as_ref()
            .and_then(|api| api.addr.clone())
            .unwrap_or_else(|| DEFAULT_API_ADDR.to_string());
        let api_token_path = file.api.and_then(|api| api.token_path);
        let sensitive_zones = file
            .zones
            .and_then(|zones| zones.sensitive)
            .unwrap_or_default();
        let retention = Duration::from_secs(
            file.retention
                .and_then(|retention| retention.seconds)
                .unwrap_or(DEFAULT_RETENTION_SECS),
        );
        Ok(Self {
            db_path,
            ruleset_id,
            api_addr,
            api_token_path,
            sensitive_zones,
            retention,
        })
    }

    fn apply_env(&mut self) -> Result<()> {
        if let Ok(addr) = std::env::var("WITNESS_API_ADDR") {
            if !addr.trim().is_empty() {
                self.api_addr = addr;
            }
        }
        if let Ok(path) = std::env::var("WITNESS_API_TOKEN_PATH") {
            if !path.trim().is_empty() {
                self.api_token_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(zones) = std::env::var("WITNESS_SENSITIVE_ZONES") {
            let parsed = split_csv(&zones);
            if !parsed.is_empty() {
                self.sensitive_zones = parsed;
            }
        }
        if let Ok(retention) = std::env::var("WITNESS_RETENTION_SECS") {
            let seconds: u64 = retention.parse().map_err(|_| {
                anyhow!("WITNESS_RETENTION_SECS must be an integer number of seconds")
            })?;
            self.retention = Duration::from_secs(seconds);
        }
        Ok(())
    }

    fn validate(&mut self) -> Result<()> {
        let policy = crate::ZonePolicy::new(self.sensitive_zones.clone())?;
        self.sensitive_zones = policy.sensitive_zones;

        if self.retention.as_secs() == 0 {
            return Err(anyhow!("retention must be greater than zero"));
        }
        Ok(())
    }
}

fn read_config_file<T>(path: &Path) -> Result<T>
where
    T: for<'de> Deserialize<'de>,
{
    let raw = std::fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read config file {}: {}", path.display(), e))?;

    let cfg = if path.extension().map(|e| e == "toml").unwrap_or(false) {
        toml::from_str(&raw)
            .map_err(|e| anyhow!("invalid TOML config file {}: {}", path.display(), e))?
    } else if path.extension().map(|e| e == "json").unwrap_or(false) {
        serde_json::from_str(&raw)
            .map_err(|e| anyhow!("invalid JSON config file {}: {}", path.display(), e))?
    } else {
        match serde_json::from_str(&raw) {
            Ok(cfg) => cfg,
            Err(json_err) => match toml::from_str(&raw) {
                Ok(cfg) => cfg,
                Err(toml_err) => {
                    return Err(anyhow!(
                        "invalid config file {} (tried JSON and TOML): json error: {}; toml error: {}",
                        path.display(),
                        json_err,
                        toml_err
                    ));
                }
            },
        }
    };
    Ok(cfg)
}

fn split_csv(value: &str) -> Vec<String> {
    value
        .split(',')
        .map(|entry| entry.trim())
        .filter(|entry| !entry.is_empty())
        .map(|entry| entry.to_string())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::read_config_file;
    use serde::Deserialize;
    use tempfile::tempdir;

    #[derive(Debug, Deserialize, PartialEq)]
    struct TestConfig {
        name: String,
        count: u32,
    }

    fn write_file(path: &std::path::Path, contents: &str) {
        std::fs::write(path, contents).expect("write temp config");
    }

    fn assert_reads_config(filename: &str, contents: &str, expected: TestConfig) {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join(filename);
        write_file(&path, contents);

        let cfg: TestConfig = read_config_file(&path).expect("read config");
        assert_eq!(cfg, expected);
    }

    #[test]
    fn reads_toml_config_by_extension() {
        assert_reads_config(
            "config.toml",
            "name = \"alpha\"\ncount = 3\n",
            TestConfig {
                name: "alpha".to_string(),
                count: 3,
            },
        );
    }

    #[test]
    fn reads_json_config_by_extension() {
        assert_reads_config(
            "config.json",
            r#"{"name":"beta","count":7}"#,
            TestConfig {
                name: "beta".to_string(),
                count: 7,
            },
        );
    }

    #[test]
    fn auto_detects_toml_without_extension() {
        assert_reads_config(
            "config",
            "name = \"gamma\"\ncount = 11\n",
            TestConfig {
                name: "gamma".to_string(),
                count: 11,
            },
        );
    }

    #[test]
    fn auto_detects_json_without_extension() {
        assert_reads_config(
            "config",
            r#"{"name":"delta","count":13}"#,
            TestConfig {
                name: "delta".to_string(),
                count: 13,
            },
        );
    }

    #[test]
    fn reports_errors_when_parsing_fails_for_both_formats() {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("config");
        write_file(&path, "{not: json");

        let err = read_config_file::<TestConfig>(&path).expect_err("parse should fail");
        let message = err.to_string();
        assert!(message.contains("invalid config file"));
        assert!(message.contains("json error"));
        assert!(message.contains("toml error"));
    }
}
