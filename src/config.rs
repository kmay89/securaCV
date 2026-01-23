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
        let api_token_path = file.api.and_then(|api| api.token_path).or_else(|| {
            std::env::var("WITNESS_API_TOKEN_PATH")
                .ok()
                .map(PathBuf::from)
        });
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

fn read_config_file(path: &Path) -> Result<WitnessdConfigFile> {
    let raw = std::fs::read_to_string(path)
        .map_err(|e| anyhow!("failed to read config file {}: {}", path.display(), e))?;
    let cfg = serde_json::from_str(&raw)
        .map_err(|e| anyhow!("invalid config file {}: {}", path.display(), e))?;
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
