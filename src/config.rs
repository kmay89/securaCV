use anyhow::{anyhow, Result};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::time::Duration;

const DEFAULT_DB_PATH: &str = "witness.db";
const DEFAULT_RULESET_ID: &str = "ruleset:v0.1";
const DEFAULT_API_ADDR: &str = "127.0.0.1:8799";
const DEFAULT_INGEST_BACKEND: &str = "rtsp";
const DEFAULT_RTSP_URL: &str = "";
const DEFAULT_RTSP_FPS: u32 = 10;
const DEFAULT_RTSP_WIDTH: u32 = 640;
const DEFAULT_RTSP_HEIGHT: u32 = 480;
const DEFAULT_RTSP_BACKEND: &str = "auto";
const DEFAULT_FILE_PATH: &str = "";
const DEFAULT_FILE_FPS: u32 = 10;
const DEFAULT_V4L2_DEVICE: &str = "/dev/video0";
const DEFAULT_V4L2_FPS: u32 = 10;
const DEFAULT_V4L2_WIDTH: u32 = 640;
const DEFAULT_V4L2_HEIGHT: u32 = 480;
const DEFAULT_ESP32_URL: &str = "http://127.0.0.1:81/stream";
const DEFAULT_ESP32_FPS: u32 = 10;
const DEFAULT_RETENTION_SECS: u64 = 60 * 60 * 24 * 7;
const DEFAULT_MODULE_ZONE_ID: &str = "zone:front_boundary";
const DEFAULT_DETECT_BACKEND: &str = "auto";

fn config_string(value: Option<String>, default: &str) -> String {
    value.unwrap_or_else(|| default.to_string())
}

fn config_u32(value: Option<u32>, default: u32) -> u32 {
    value.unwrap_or(default)
}

#[derive(Debug, Deserialize, Default)]
struct WitnessdConfigFile {
    db_path: Option<String>,
    ruleset_id: Option<String>,
    api: Option<ApiConfigFile>,
    ingest: Option<IngestConfigFile>,
    rtsp: Option<RtspConfigFile>,
    file: Option<FileConfigFile>,
    v4l2: Option<V4l2ConfigFile>,
    esp32: Option<Esp32ConfigFile>,
    detect: Option<DetectConfigFile>,
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
struct IngestConfigFile {
    backend: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
struct RtspConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
    backend: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
struct FileConfigFile {
    path: Option<String>,
    target_fps: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
struct V4l2ConfigFile {
    device: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
struct Esp32ConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
struct DetectConfigFile {
    backend: Option<String>,
    tract_model: Option<PathBuf>,
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
    pub ingest: IngestSettings,
    pub rtsp: RtspSettings,
    pub file: FileSettings,
    pub v4l2: V4l2Settings,
    pub esp32: Esp32Settings,
    pub detect: DetectSettings,
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
    pub backend: RtspBackendPreference,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RtspBackendPreference {
    Auto,
    Gstreamer,
    Ffmpeg,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IngestBackend {
    File,
    Rtsp,
    V4l2,
    Esp32,
}

#[derive(Debug, Clone)]
pub struct IngestSettings {
    pub backend: IngestBackend,
}

#[derive(Debug, Clone)]
pub struct FileSettings {
    pub path: String,
    pub target_fps: u32,
}

#[derive(Debug, Clone)]
pub struct V4l2Settings {
    pub device: String,
    pub target_fps: u32,
    pub width: u32,
    pub height: u32,
}

#[derive(Debug, Clone)]
pub struct Esp32Settings {
    pub url: String,
    pub target_fps: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DetectBackendPreference {
    Auto,
    Stub,
    Cpu,
    Tract,
}

#[derive(Debug, Clone)]
pub struct DetectSettings {
    pub backend: DetectBackendPreference,
    pub tract_model: Option<PathBuf>,
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
        let ingest_backend = file
            .ingest
            .and_then(|ingest| ingest.backend)
            .unwrap_or_else(|| DEFAULT_INGEST_BACKEND.to_string());
        let ingest = IngestSettings {
            backend: IngestBackend::parse(&ingest_backend)?,
        };
        let rtsp = RtspSettings {
            url: config_string(
                file.rtsp.as_ref().and_then(|rtsp| rtsp.url.clone()),
                DEFAULT_RTSP_URL,
            ),
            target_fps: config_u32(
                file.rtsp.as_ref().and_then(|rtsp| rtsp.target_fps),
                DEFAULT_RTSP_FPS,
            ),
            width: config_u32(
                file.rtsp.as_ref().and_then(|rtsp| rtsp.width),
                DEFAULT_RTSP_WIDTH,
            ),
            height: config_u32(
                file.rtsp.as_ref().and_then(|rtsp| rtsp.height),
                DEFAULT_RTSP_HEIGHT,
            ),
            backend: RtspBackendPreference::parse(
                file.rtsp
                    .as_ref()
                    .and_then(|rtsp| rtsp.backend.as_deref())
                    .unwrap_or(DEFAULT_RTSP_BACKEND),
            )?,
        };
        let file_source = FileSettings {
            path: config_string(
                file.file.as_ref().and_then(|file| file.path.clone()),
                DEFAULT_FILE_PATH,
            ),
            target_fps: config_u32(
                file.file.as_ref().and_then(|file| file.target_fps),
                DEFAULT_FILE_FPS,
            ),
        };
        let v4l2 = V4l2Settings {
            device: config_string(
                file.v4l2.as_ref().and_then(|v4l2| v4l2.device.clone()),
                DEFAULT_V4L2_DEVICE,
            ),
            target_fps: config_u32(
                file.v4l2.as_ref().and_then(|v4l2| v4l2.target_fps),
                DEFAULT_V4L2_FPS,
            ),
            width: config_u32(
                file.v4l2.as_ref().and_then(|v4l2| v4l2.width),
                DEFAULT_V4L2_WIDTH,
            ),
            height: config_u32(
                file.v4l2.as_ref().and_then(|v4l2| v4l2.height),
                DEFAULT_V4L2_HEIGHT,
            ),
        };
        let esp32 = Esp32Settings {
            url: config_string(
                file.esp32.as_ref().and_then(|esp32| esp32.url.clone()),
                DEFAULT_ESP32_URL,
            ),
            target_fps: config_u32(
                file.esp32.as_ref().and_then(|esp32| esp32.target_fps),
                DEFAULT_ESP32_FPS,
            ),
        };
        let detect_config = file.detect.unwrap_or_default();
        let detect = DetectSettings {
            backend: DetectBackendPreference::parse(
                detect_config
                    .backend
                    .as_deref()
                    .unwrap_or(DEFAULT_DETECT_BACKEND),
            )?,
            tract_model: detect_config.tract_model,
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
            ingest,
            rtsp,
            file: file_source,
            v4l2,
            esp32,
            detect,
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
        if let Ok(backend) = std::env::var("WITNESS_INGEST_BACKEND") {
            if !backend.trim().is_empty() {
                self.ingest.backend = IngestBackend::parse(&backend)?;
            }
        }
        if let Ok(path) = std::env::var("WITNESS_FILE_PATH") {
            if !path.trim().is_empty() {
                self.file.path = path;
            }
        }
        if let Ok(fps) = std::env::var("WITNESS_FILE_FPS") {
            if !fps.trim().is_empty() {
                self.file.target_fps = fps
                    .parse()
                    .map_err(|_| anyhow!("WITNESS_FILE_FPS must be an integer"))?;
            }
        }
        if let Ok(url) = std::env::var("WITNESS_RTSP_URL") {
            if !url.trim().is_empty() {
                self.rtsp.url = url;
            }
        }
        if let Ok(backend) = std::env::var("WITNESS_RTSP_BACKEND") {
            if !backend.trim().is_empty() {
                self.rtsp.backend = RtspBackendPreference::parse(&backend)?;
            }
        }
        if let Ok(device) = std::env::var("WITNESS_V4L2_DEVICE") {
            if !device.trim().is_empty() {
                self.v4l2.device = device;
            }
        }
        if let Ok(url) = std::env::var("WITNESS_ESP32_URL") {
            if !url.trim().is_empty() {
                self.esp32.url = url;
            }
        }
        if let Ok(fps) = std::env::var("WITNESS_ESP32_FPS") {
            if !fps.trim().is_empty() {
                let parsed: u32 = fps
                    .parse()
                    .map_err(|_| anyhow!("WITNESS_ESP32_FPS must be an integer"))?;
                self.esp32.target_fps = parsed;
            }
        }
        if let Ok(backend) = std::env::var("WITNESS_DETECT_BACKEND") {
            if !backend.trim().is_empty() {
                self.detect.backend = DetectBackendPreference::parse(&backend)?;
            }
        }
        if let Ok(path) = std::env::var("WITNESS_TRACT_MODEL") {
            if !path.trim().is_empty() {
                self.detect.tract_model = Some(PathBuf::from(path));
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
        if self.detect.backend == DetectBackendPreference::Tract {
            let Some(path) = &self.detect.tract_model else {
                return Err(anyhow!(
                    "detect.backend=tract requires detect.tract_model to be set"
                ));
            };
            if path.as_os_str().is_empty() {
                return Err(anyhow!(
                    "detect.tract_model must not be empty when detect.backend=tract"
                ));
            }
        }
        match self.ingest.backend {
            IngestBackend::File => {
                if self.file.path.trim().is_empty() {
                    return Err(anyhow!("file.path must not be empty"));
                }
                if self.file.path.trim().starts_with("stub://") && !stub_urls_allowed() {
                    return Err(anyhow!(
                        "file.path uses stub:// which is only allowed for local dev/test builds"
                    ));
                }
                if self.file.path.contains("://") && !self.file.path.trim().starts_with("stub://") {
                    return Err(anyhow!(
                        "file.path must be a local filesystem path (no URL schemes)"
                    ));
                }
            }
            IngestBackend::V4l2 => {
                if self.v4l2.device.trim().is_empty() {
                    return Err(anyhow!("v4l2.device must not be empty"));
                }
            }
            IngestBackend::Esp32 => {
                if self.esp32.url.trim().is_empty() {
                    return Err(anyhow!("esp32.url must not be empty"));
                }
            }
            IngestBackend::Rtsp => {
                if self.rtsp.url.trim().is_empty() {
                    return Err(anyhow!("rtsp.url must not be empty"));
                }
                if self.rtsp.url.trim().starts_with("stub://") && !stub_urls_allowed() {
                    return Err(anyhow!(
                        "rtsp.url uses stub:// which is only allowed for local dev/test builds"
                    ));
                }
            }
        }
        Ok(())
    }
}

fn stub_urls_allowed() -> bool {
    cfg!(test) || cfg!(debug_assertions)
}

impl IngestBackend {
    fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "file" => Ok(Self::File),
            "rtsp" => Ok(Self::Rtsp),
            "v4l2" => Ok(Self::V4l2),
            "esp32" => Ok(Self::Esp32),
            other => Err(anyhow!(
                "unsupported ingest backend '{}'; expected 'file', 'rtsp', 'v4l2', or 'esp32'",
                other
            )),
        }
    }
}

impl DetectBackendPreference {
    fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "auto" => Ok(Self::Auto),
            "stub" => Ok(Self::Stub),
            "cpu" => Ok(Self::Cpu),
            "tract" => Ok(Self::Tract),
            other => Err(anyhow!(
                "unsupported detect backend '{}'; expected 'auto', 'stub', 'cpu', or 'tract'",
                other
            )),
        }
    }
}

impl RtspBackendPreference {
    fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "auto" => Ok(Self::Auto),
            "gstreamer" => Ok(Self::Gstreamer),
            "ffmpeg" => Ok(Self::Ffmpeg),
            other => Err(anyhow!(
                "unsupported rtsp backend '{}'; expected 'auto', 'gstreamer', or 'ffmpeg'",
                other
            )),
        }
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
    use super::*;
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

    fn assert_reads_config(filename: &str, contents: &str, expected: &TestConfig) {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join(filename);
        write_file(&path, contents);

        let cfg: TestConfig = read_config_file(&path).expect("read config");
        assert_eq!(&cfg, expected);
    }

    #[test]
    fn reads_toml_config_by_extension() {
        assert_reads_config(
            "config.toml",
            "name = \"alpha\"\ncount = 3\n",
            &TestConfig {
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
            &TestConfig {
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
            &TestConfig {
                name: "gamma".to_string(),
                count: 11,
            },
        );
    }

    #[test]
    fn detect_config_defaults_to_auto() {
        let config =
            WitnessdConfig::from_file(WitnessdConfigFile::default()).expect("config should parse");
        assert_eq!(config.detect.backend, DetectBackendPreference::Auto);
        assert!(config.detect.tract_model.is_none());
    }

    #[test]
    fn detect_config_accepts_tract_model() {
        let mut file = WitnessdConfigFile::default();
        file.detect = Some(DetectConfigFile {
            backend: Some("tract".to_string()),
            tract_model: Some(PathBuf::from("/tmp/model.onnx")),
        });
        file.ingest = Some(IngestConfigFile {
            backend: Some("rtsp".to_string()),
        });
        file.rtsp = Some(RtspConfigFile {
            url: Some("rtsp://example.com/stream".to_string()),
            target_fps: Some(DEFAULT_RTSP_FPS),
            width: Some(DEFAULT_RTSP_WIDTH),
            height: Some(DEFAULT_RTSP_HEIGHT),
            backend: Some(DEFAULT_RTSP_BACKEND.to_string()),
        });
        let config = WitnessdConfig::from_file(file).expect("config should parse");
        assert_eq!(config.detect.backend, DetectBackendPreference::Tract);
        assert_eq!(
            config.detect.tract_model,
            Some(PathBuf::from("/tmp/model.onnx"))
        );
    }

    #[test]
    fn auto_detects_json_without_extension() {
        assert_reads_config(
            "config",
            r#"{"name":"delta","count":13}"#,
            &TestConfig {
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
