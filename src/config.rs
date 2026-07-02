use crate::storage_health::StorageHealthThresholds;
use crate::SqliteSynchronous;
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
// SD-card endurance: each retention pass that finds prunable events writes a
// signed checkpoint transaction, so the cadence directly sets the steady-state
// write rate. 5 minutes (288 passes/day vs 8,640 at the old 10s) keeps pruning
// timely against a 7-day retention while minimizing flash wear.
const DEFAULT_RETENTION_CHECK_INTERVAL_SECS: u64 = 300;
const DEFAULT_STORAGE_HEALTH_ENABLED: bool = true;
const DEFAULT_STORAGE_HEALTH_INTERVAL_SECS: u64 = 600;
// Conservative endurance budget in TB written. High-endurance microSD cards
// commonly carry ratings well above this; operators should set it to the
// purchased card's rating (see docs/sd_card_health.md).
const DEFAULT_STORAGE_HEALTH_TBW: f64 = 64.0;
const DEFAULT_MODULE_ZONE_ID: &str = "zone:front_boundary";
const DEFAULT_DETECT_BACKEND: &str = "auto";
// Minimum confidence for a detection to be reported. Keep in sync with the tract backend's
// DEFAULT_CONFIDENCE_THRESHOLD (that backend is feature-gated, so the default lives here too).
const DEFAULT_DETECT_CONFIDENCE: f32 = 0.5;
// Default location for the tract object-detection model, populated by
// scripts/fetch_detection_model.sh. Used when detect.backend=tract and detect.tract_model is
// unset, so operators only set the backend after fetching the model — no manual path needed.
// HOST-ONLY: this model runs in tract on a Pi/x86 host, not on the ESP32-S3 (which uses the
// Grove Vision AI V2 board). See scripts/fetch_detection_model.sh.
const DEFAULT_TRACT_MODEL: &str = "vendor/models/tinyyolov2-8.onnx";
// Default post-processing format for the tract model. The bundled model is tiny-YOLOv2, whose
// raw grid output is decoded + NMS'd on the host; set to "postnms" for models that already
// emit final boxes.
const DEFAULT_TRACT_FORMAT: &str = "yolov2";
// Ingest resilience: how long the source may stay unhealthy before one
// GapMissingData failure record is sealed for the outage, and the reconnect
// backoff ceiling. The threshold must exceed the source's own stall timeout
// so a single slow frame doesn't count as an outage.
const DEFAULT_INGEST_FAILURE_THRESHOLD_S: u64 = 30;
const DEFAULT_INGEST_RECONNECT_BACKOFF_MAX_S: u64 = 60;
// System trace: heartbeat records are bucket-locked (one per 10-minute coarse
// bucket); the flag only disables them. INFO counter summaries default to 60s
// (the previous 5s dump is DEBUG-only).
const DEFAULT_HEALTH_HEARTBEAT: bool = true;
const DEFAULT_HEALTH_LOG_INTERVAL_S: u64 = 60;
// Storage preflight: free-space floor below which a StorageFull failure record
// is sealed (once per transition), and how often to check.
const DEFAULT_STORAGE_MIN_FREE_MB: u64 = 256;
const DEFAULT_STORAGE_CHECK_INTERVAL_S: u64 = 60;
// Clock monitoring: monotonic-vs-wallclock drift beyond this seals one
// ClockSkew failure record per excursion.
const DEFAULT_CLOCK_SKEW_TOLERANCE_S: u64 = 30;

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
    health: Option<HealthConfigFile>,
    storage: Option<StorageConfigFile>,
    clock: Option<ClockConfigFile>,
    storage_health: Option<StorageHealthConfigFile>,
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
    rate_limit_per_minute: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
struct IngestConfigFile {
    backend: Option<String>,
    failure_threshold_s: Option<u64>,
    reconnect_backoff_max_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
struct HealthConfigFile {
    heartbeat: Option<bool>,
    log_interval_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
struct StorageConfigFile {
    min_free_mb: Option<u64>,
    check_interval_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
struct ClockConfigFile {
    skew_tolerance_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
struct RtspConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
    backend: Option<String>,
    transport: Option<String>,
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
    tract_format: Option<String>,
    confidence: Option<f32>,
}

#[derive(Debug, Deserialize, Default)]
struct ZoneConfigFile {
    module_zone_id: Option<String>,
    sensitive: Option<Vec<String>>,
}

#[derive(Debug, Deserialize, Default)]
struct RetentionConfigFile {
    seconds: Option<u64>,
    check_interval_seconds: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
struct StorageHealthConfigFile {
    enabled: Option<bool>,
    check_interval_seconds: Option<u64>,
    endurance_tbw: Option<f64>,
    block_device: Option<String>,
    state_path: Option<String>,
    free_space_warn_pct: Option<f64>,
    free_space_critical_pct: Option<f64>,
    wear_warn_pct: Option<f64>,
    wear_critical_pct: Option<f64>,
    temp_warn_c: Option<f64>,
    temp_hot_c: Option<f64>,
    sqlite_synchronous: Option<String>,
}

/// Storage endurance & health monitoring settings (`[storage_health]`).
#[derive(Debug, Clone)]
pub struct StorageHealthSettings {
    pub enabled: bool,
    pub check_interval: Duration,
    /// Card endurance rating in TB written; set to the purchased card's rating.
    pub endurance_tbw: f64,
    /// Parent block device (e.g. "mmcblk0"); auto-detected when `None`.
    pub block_device: Option<String>,
    /// Wear-tracking state file; defaults to `<db_path>.health.json`.
    pub state_path: Option<PathBuf>,
    pub thresholds: StorageHealthThresholds,
    /// SQLite synchronous mode for sealed-log connections (default Full;
    /// Normal is an opt-in endurance optimization — see lib.rs docs).
    pub sqlite_synchronous: SqliteSynchronous,
}

impl Default for StorageHealthSettings {
    fn default() -> Self {
        Self {
            enabled: DEFAULT_STORAGE_HEALTH_ENABLED,
            check_interval: Duration::from_secs(DEFAULT_STORAGE_HEALTH_INTERVAL_SECS),
            endurance_tbw: DEFAULT_STORAGE_HEALTH_TBW,
            block_device: None,
            state_path: None,
            thresholds: StorageHealthThresholds::default(),
            sqlite_synchronous: SqliteSynchronous::default(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct WitnessdConfig {
    pub db_path: String,
    pub ruleset_id: String,
    pub api_addr: String,
    pub api_token_path: Option<PathBuf>,
    /// Per-IP request cap for the event API (0 disables). See
    /// [`crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE`].
    pub api_rate_limit_per_minute: u32,
    pub ingest: IngestSettings,
    pub rtsp: RtspSettings,
    pub file: FileSettings,
    pub v4l2: V4l2Settings,
    pub esp32: Esp32Settings,
    pub detect: DetectSettings,
    pub zones: ZoneSettings,
    pub retention: Duration,
    pub health: HealthSettings,
    pub storage: StorageSettings,
    pub clock: ClockSettings,
    /// How often witnessd runs retention enforcement (each pass that prunes
    /// writes a signed checkpoint transaction — see
    /// `DEFAULT_RETENTION_CHECK_INTERVAL_SECS`).
    pub retention_check_interval: Duration,
    pub storage_health: StorageHealthSettings,
}

#[derive(Debug, Clone)]
pub struct WitnessApiConfig {
    pub db_path: String,
    pub ruleset_id: String,
    pub api_addr: String,
    pub api_token_path: Option<PathBuf>,
    /// Per-IP request cap for the event API (0 disables). See
    /// [`crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE`].
    pub api_rate_limit_per_minute: u32,
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
    /// Optional RTSP lower transport override (e.g. "tcp"/"udp"). `None` uses the
    /// decoder default (libav: UDP-first with TCP fallback). Many cameras/NVRs
    /// only stream reliably over interleaved TCP.
    pub transport: Option<String>,
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
    /// How long the source may stay unhealthy before one GapMissingData
    /// failure record is sealed for the outage.
    pub failure_threshold: Duration,
    /// Ceiling for the ingest reconnect backoff (starts at 1s, doubles).
    pub reconnect_backoff_max: Duration,
}

#[derive(Debug, Clone)]
pub struct HealthSettings {
    /// Seal one heartbeat record per 10-minute bucket (anchors the chain tail).
    pub heartbeat: bool,
    /// Cadence of the INFO-level pipeline counter summary.
    pub log_interval: Duration,
}

#[derive(Debug, Clone)]
pub struct StorageSettings {
    /// Free-space floor (bytes) below which a StorageFull record is sealed.
    pub min_free_bytes: u64,
    /// How often to run the free-space preflight check.
    pub check_interval: Duration,
}

#[derive(Debug, Clone)]
pub struct ClockSettings {
    /// Monotonic-vs-wallclock drift beyond which a ClockSkew record is sealed.
    pub skew_tolerance: Duration,
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

/// How the tract model's raw output is post-processed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TractFormat {
    /// Raw YOLOv2 grid decoded + NMS'd on the host (the bundled tiny-YOLOv2 model).
    Yolov2,
    /// Model already emits final boxes (`[N,6]` or separate boxes/scores/classes tensors).
    PostNms,
}

#[derive(Debug, Clone)]
pub struct DetectSettings {
    pub backend: DetectBackendPreference,
    pub tract_model: Option<PathBuf>,
    pub tract_format: TractFormat,
    /// Minimum confidence [0.0, 1.0] for a detection to be reported.
    pub confidence_threshold: f32,
}

impl DetectSettings {
    /// Resolved path to the tract model: the configured `tract_model`, or the default bundled
    /// location ([`DEFAULT_TRACT_MODEL`], populated by `scripts/fetch_detection_model.sh`) when
    /// unset. Lets operators enable object detection with just `detect.backend = "tract"`.
    pub fn tract_model_path(&self) -> PathBuf {
        self.tract_model
            .clone()
            .unwrap_or_else(|| PathBuf::from(DEFAULT_TRACT_MODEL))
    }
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
        let api_rate_limit_per_minute = file
            .api
            .as_ref()
            .and_then(|api| api.rate_limit_per_minute)
            .unwrap_or(crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE);
        let api_token_path = file.api.and_then(|api| api.token_path);
        let ingest_config = file.ingest.unwrap_or_default();
        let ingest_backend = ingest_config
            .backend
            .unwrap_or_else(|| DEFAULT_INGEST_BACKEND.to_string());
        let ingest = IngestSettings {
            backend: IngestBackend::parse(&ingest_backend)?,
            failure_threshold: Duration::from_secs(
                ingest_config
                    .failure_threshold_s
                    .unwrap_or(DEFAULT_INGEST_FAILURE_THRESHOLD_S),
            ),
            reconnect_backoff_max: Duration::from_secs(
                ingest_config
                    .reconnect_backoff_max_s
                    .unwrap_or(DEFAULT_INGEST_RECONNECT_BACKOFF_MAX_S),
            ),
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
            transport: file
                .rtsp
                .as_ref()
                .and_then(|rtsp| rtsp.transport.clone())
                .filter(|t| !t.trim().is_empty()),
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
            tract_format: TractFormat::parse(
                detect_config
                    .tract_format
                    .as_deref()
                    .unwrap_or(DEFAULT_TRACT_FORMAT),
            )?,
            confidence_threshold: detect_config
                .confidence
                .unwrap_or(DEFAULT_DETECT_CONFIDENCE),
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
                .as_ref()
                .and_then(|retention| retention.seconds)
                .unwrap_or(DEFAULT_RETENTION_SECS),
        );
        let health_config = file.health.unwrap_or_default();
        let health = HealthSettings {
            heartbeat: health_config.heartbeat.unwrap_or(DEFAULT_HEALTH_HEARTBEAT),
            log_interval: Duration::from_secs(
                health_config
                    .log_interval_s
                    .unwrap_or(DEFAULT_HEALTH_LOG_INTERVAL_S),
            ),
        };
        let storage_config = file.storage.unwrap_or_default();
        let storage = StorageSettings {
            min_free_bytes: storage_config
                .min_free_mb
                .unwrap_or(DEFAULT_STORAGE_MIN_FREE_MB)
                .saturating_mul(1024 * 1024),
            check_interval: Duration::from_secs(
                storage_config
                    .check_interval_s
                    .unwrap_or(DEFAULT_STORAGE_CHECK_INTERVAL_S),
            ),
        };
        let clock = ClockSettings {
            skew_tolerance: Duration::from_secs(
                file.clock
                    .and_then(|clock| clock.skew_tolerance_s)
                    .unwrap_or(DEFAULT_CLOCK_SKEW_TOLERANCE_S),
            ),
        };
        let retention_check_interval = Duration::from_secs(
            file.retention
                .and_then(|retention| retention.check_interval_seconds)
                .unwrap_or(DEFAULT_RETENTION_CHECK_INTERVAL_SECS),
        );
        let sh = file.storage_health.unwrap_or_default();
        let defaults = StorageHealthSettings::default();
        let default_thresholds = StorageHealthThresholds::default();
        let storage_health = StorageHealthSettings {
            enabled: sh.enabled.unwrap_or(defaults.enabled),
            check_interval: sh
                .check_interval_seconds
                .map(Duration::from_secs)
                .unwrap_or(defaults.check_interval),
            endurance_tbw: sh.endurance_tbw.unwrap_or(defaults.endurance_tbw),
            block_device: sh.block_device.filter(|d| !d.trim().is_empty()),
            state_path: sh
                .state_path
                .filter(|p| !p.trim().is_empty())
                .map(PathBuf::from),
            thresholds: StorageHealthThresholds {
                free_space_warn_pct: sh
                    .free_space_warn_pct
                    .unwrap_or(default_thresholds.free_space_warn_pct),
                free_space_critical_pct: sh
                    .free_space_critical_pct
                    .unwrap_or(default_thresholds.free_space_critical_pct),
                wear_warn_pct: sh.wear_warn_pct.unwrap_or(default_thresholds.wear_warn_pct),
                wear_critical_pct: sh
                    .wear_critical_pct
                    .unwrap_or(default_thresholds.wear_critical_pct),
                temp_warn_c: sh.temp_warn_c.unwrap_or(default_thresholds.temp_warn_c),
                temp_hot_c: sh.temp_hot_c.unwrap_or(default_thresholds.temp_hot_c),
            },
            sqlite_synchronous: match sh.sqlite_synchronous {
                Some(raw) => SqliteSynchronous::parse(&raw)?,
                None => SqliteSynchronous::default(),
            },
        };
        Ok(Self {
            db_path,
            ruleset_id,
            api_addr,
            api_token_path,
            api_rate_limit_per_minute,
            ingest,
            rtsp,
            file: file_source,
            v4l2,
            esp32,
            detect,
            zones,
            retention,
            health,
            storage,
            clock,
            retention_check_interval,
            storage_health,
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
        if let Ok(raw) = std::env::var("WITNESS_API_RATE_LIMIT_PER_MINUTE") {
            if !raw.trim().is_empty() {
                self.api_rate_limit_per_minute = raw.trim().parse().map_err(|_| {
                    anyhow!("WITNESS_API_RATE_LIMIT_PER_MINUTE must be a non-negative integer")
                })?;
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
        if let Ok(transport) = std::env::var("WITNESS_RTSP_TRANSPORT") {
            let transport = transport.trim();
            self.rtsp.transport = (!transport.is_empty()).then(|| transport.to_string());
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
        if let Ok(format) = std::env::var("WITNESS_TRACT_FORMAT") {
            if !format.trim().is_empty() {
                self.detect.tract_format = TractFormat::parse(&format)?;
            }
        }
        if let Ok(conf) = std::env::var("WITNESS_DETECT_CONFIDENCE") {
            if !conf.trim().is_empty() {
                self.detect.confidence_threshold = conf.trim().parse().map_err(|_| {
                    anyhow!("WITNESS_DETECT_CONFIDENCE must be a valid floating-point number")
                })?;
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
        if let Ok(interval) = std::env::var("WITNESS_RETENTION_CHECK_INTERVAL_SECS") {
            let seconds: u64 = interval.parse().map_err(|_| {
                anyhow!(
                    "WITNESS_RETENTION_CHECK_INTERVAL_SECS must be an integer number of seconds"
                )
            })?;
            self.retention_check_interval = Duration::from_secs(seconds);
        }
        if let Ok(enabled) = std::env::var("WITNESS_STORAGE_HEALTH_ENABLED") {
            let enabled = enabled.trim();
            if !enabled.is_empty() {
                self.storage_health.enabled = match enabled.to_lowercase().as_str() {
                    "1" | "true" | "yes" => true,
                    "0" | "false" | "no" => false,
                    other => {
                        return Err(anyhow!(
                            "WITNESS_STORAGE_HEALTH_ENABLED must be true/false (got '{}')",
                            other
                        ))
                    }
                };
            }
        }
        if let Ok(interval) = std::env::var("WITNESS_STORAGE_HEALTH_INTERVAL_SECS") {
            let seconds: u64 = interval.parse().map_err(|_| {
                anyhow!("WITNESS_STORAGE_HEALTH_INTERVAL_SECS must be an integer number of seconds")
            })?;
            self.storage_health.check_interval = Duration::from_secs(seconds);
        }
        if let Ok(tbw) = std::env::var("WITNESS_STORAGE_HEALTH_TBW") {
            if !tbw.trim().is_empty() {
                self.storage_health.endurance_tbw = tbw.trim().parse().map_err(|_| {
                    anyhow!("WITNESS_STORAGE_HEALTH_TBW must be a number of terabytes written")
                })?;
            }
        }
        if let Ok(device) = std::env::var("WITNESS_STORAGE_HEALTH_DEVICE") {
            let device = device.trim();
            self.storage_health.block_device = (!device.is_empty()).then(|| device.to_string());
        }
        if let Ok(mode) = std::env::var("WITNESS_SQLITE_SYNCHRONOUS") {
            if !mode.trim().is_empty() {
                self.storage_health.sqlite_synchronous = SqliteSynchronous::parse(&mode)?;
            }
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
        if self.ingest.failure_threshold.as_secs() == 0 {
            return Err(anyhow!(
                "ingest.failure_threshold_s must be greater than zero"
            ));
        }
        if self.ingest.reconnect_backoff_max.as_secs() == 0 {
            return Err(anyhow!(
                "ingest.reconnect_backoff_max_s must be greater than zero"
            ));
        }
        if self.health.log_interval.as_secs() == 0 {
            return Err(anyhow!("health.log_interval_s must be greater than zero"));
        }
        if self.storage.check_interval.as_secs() == 0 {
            return Err(anyhow!(
                "storage.check_interval_s must be greater than zero"
            ));
        }
        if self.clock.skew_tolerance.as_secs() == 0 {
            return Err(anyhow!("clock.skew_tolerance_s must be greater than zero"));
        }
        if self.retention_check_interval.as_secs() == 0 {
            return Err(anyhow!(
                "retention.check_interval_seconds must be greater than zero"
            ));
        }
        if self.storage_health.check_interval.as_secs() == 0 {
            return Err(anyhow!(
                "storage_health.check_interval_seconds must be greater than zero"
            ));
        }
        if self.storage_health.endurance_tbw <= 0.0 {
            return Err(anyhow!(
                "storage_health.endurance_tbw must be greater than zero (got {})",
                self.storage_health.endurance_tbw
            ));
        }
        let t = &self.storage_health.thresholds;
        if t.free_space_critical_pct >= t.free_space_warn_pct {
            return Err(anyhow!(
                "storage_health.free_space_critical_pct ({}) must be below free_space_warn_pct ({})",
                t.free_space_critical_pct,
                t.free_space_warn_pct
            ));
        }
        if t.wear_warn_pct >= t.wear_critical_pct {
            return Err(anyhow!(
                "storage_health.wear_warn_pct ({}) must be below wear_critical_pct ({})",
                t.wear_warn_pct,
                t.wear_critical_pct
            ));
        }
        if !(0.0..=1.0).contains(&self.detect.confidence_threshold) {
            return Err(anyhow!(
                "detect.confidence must be between 0.0 and 1.0 (got {})",
                self.detect.confidence_threshold
            ));
        }
        if self.detect.backend == DetectBackendPreference::Tract {
            // tract_model is optional: when unset it defaults to DEFAULT_TRACT_MODEL (populated by
            // scripts/fetch_detection_model.sh). Only an explicitly-empty path is invalid; a missing
            // model file surfaces a clear load error from the tract backend at startup.
            if let Some(path) = &self.detect.tract_model {
                if path.as_os_str().is_empty() {
                    return Err(anyhow!(
                        "detect.tract_model must not be empty when detect.backend=tract"
                    ));
                }
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

impl TractFormat {
    pub fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "yolov2" => Ok(Self::Yolov2),
            "postnms" | "post_nms" => Ok(Self::PostNms),
            other => Err(anyhow!(
                "unsupported detect.tract_format '{}'; expected 'yolov2' or 'postnms'",
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
        let api_rate_limit_per_minute = file
            .api
            .as_ref()
            .and_then(|api| api.rate_limit_per_minute)
            .unwrap_or(crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE);
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
            api_rate_limit_per_minute,
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
        if let Ok(raw) = std::env::var("WITNESS_API_RATE_LIMIT_PER_MINUTE") {
            if !raw.trim().is_empty() {
                self.api_rate_limit_per_minute = raw.trim().parse().map_err(|_| {
                    anyhow!("WITNESS_API_RATE_LIMIT_PER_MINUTE must be a non-negative integer")
                })?;
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
        let file = WitnessdConfigFile {
            detect: Some(DetectConfigFile {
                backend: Some("tract".to_string()),
                tract_model: Some(PathBuf::from("/tmp/model.onnx")),
                tract_format: None,
                confidence: None,
            }),
            ingest: Some(IngestConfigFile {
                backend: Some("rtsp".to_string()),
                failure_threshold_s: None,
                reconnect_backoff_max_s: None,
            }),
            rtsp: Some(RtspConfigFile {
                url: Some("rtsp://example.com/stream".to_string()),
                target_fps: Some(DEFAULT_RTSP_FPS),
                width: Some(DEFAULT_RTSP_WIDTH),
                height: Some(DEFAULT_RTSP_HEIGHT),
                backend: Some(DEFAULT_RTSP_BACKEND.to_string()),
                transport: None,
            }),
            ..WitnessdConfigFile::default()
        };
        let config = WitnessdConfig::from_file(file).expect("config should parse");
        assert_eq!(config.detect.backend, DetectBackendPreference::Tract);
        assert_eq!(
            config.detect.tract_model,
            Some(PathBuf::from("/tmp/model.onnx"))
        );
    }

    #[test]
    fn detect_config_defaults_confidence() {
        let config =
            WitnessdConfig::from_file(WitnessdConfigFile::default()).expect("config should parse");
        assert_eq!(
            config.detect.confidence_threshold,
            DEFAULT_DETECT_CONFIDENCE
        );
    }

    #[test]
    fn detect_config_accepts_in_range_confidence() {
        let file = WitnessdConfigFile {
            detect: Some(DetectConfigFile {
                backend: None,
                tract_model: None,
                tract_format: None,
                confidence: Some(0.8),
            }),
            rtsp: Some(RtspConfigFile {
                url: Some("rtsp://example.com/stream".to_string()),
                target_fps: Some(DEFAULT_RTSP_FPS),
                width: Some(DEFAULT_RTSP_WIDTH),
                height: Some(DEFAULT_RTSP_HEIGHT),
                backend: Some(DEFAULT_RTSP_BACKEND.to_string()),
                transport: None,
            }),
            ..WitnessdConfigFile::default()
        };
        let mut config = WitnessdConfig::from_file(file).expect("config should parse");
        assert_eq!(config.detect.confidence_threshold, 0.8);
        config.validate().expect("0.8 is a valid confidence");
    }

    #[test]
    fn detect_confidence_out_of_range_is_rejected() {
        let file = WitnessdConfigFile {
            detect: Some(DetectConfigFile {
                backend: None,
                tract_model: None,
                tract_format: None,
                confidence: Some(1.5),
            }),
            ..WitnessdConfigFile::default()
        };
        // from_file does not range-check; validate() must reject it (covers file and env sources).
        let mut config = WitnessdConfig::from_file(file).expect("from_file should parse");
        let err = config.validate().expect_err("1.5 is out of range");
        assert!(
            err.to_string().contains("detect.confidence"),
            "unexpected error: {err}"
        );
    }

    #[test]
    fn detect_tract_model_defaults_to_bundled_path() {
        let config =
            WitnessdConfig::from_file(WitnessdConfigFile::default()).expect("config should parse");
        assert!(config.detect.tract_model.is_none());
        assert_eq!(
            config.detect.tract_model_path(),
            PathBuf::from(DEFAULT_TRACT_MODEL)
        );
    }

    #[test]
    fn detect_tract_model_path_prefers_explicit_value() {
        let file = WitnessdConfigFile {
            detect: Some(DetectConfigFile {
                backend: Some("tract".to_string()),
                tract_model: Some(PathBuf::from("/models/custom.onnx")),
                tract_format: None,
                confidence: None,
            }),
            ..WitnessdConfigFile::default()
        };
        let config = WitnessdConfig::from_file(file).expect("config should parse");
        assert_eq!(
            config.detect.tract_model_path(),
            PathBuf::from("/models/custom.onnx")
        );
    }

    #[test]
    fn detect_tract_without_explicit_model_passes_validation() {
        // Previously detect.backend=tract required detect.tract_model; it now defaults to the
        // bundled path, so backend=tract alone (after fetching the model) is valid.
        let file = WitnessdConfigFile {
            detect: Some(DetectConfigFile {
                backend: Some("tract".to_string()),
                tract_model: None,
                tract_format: None,
                confidence: None,
            }),
            rtsp: Some(RtspConfigFile {
                url: Some("rtsp://example.com/stream".to_string()),
                target_fps: Some(DEFAULT_RTSP_FPS),
                width: Some(DEFAULT_RTSP_WIDTH),
                height: Some(DEFAULT_RTSP_HEIGHT),
                backend: Some(DEFAULT_RTSP_BACKEND.to_string()),
                transport: None,
            }),
            ..WitnessdConfigFile::default()
        };
        let mut config = WitnessdConfig::from_file(file).expect("config should parse");
        config
            .validate()
            .expect("tract without explicit model is now valid");
        assert_eq!(config.detect.backend, DetectBackendPreference::Tract);
    }

    #[test]
    fn health_storage_clock_defaults_apply() {
        let config =
            WitnessdConfig::from_file(WitnessdConfigFile::default()).expect("config should parse");
        assert!(config.health.heartbeat);
        assert_eq!(config.health.log_interval, Duration::from_secs(60));
        assert_eq!(config.storage.min_free_bytes, 256 * 1024 * 1024);
        assert_eq!(config.storage.check_interval, Duration::from_secs(60));
        assert_eq!(config.clock.skew_tolerance, Duration::from_secs(30));
        assert_eq!(config.ingest.failure_threshold, Duration::from_secs(30));
        assert_eq!(config.ingest.reconnect_backoff_max, Duration::from_secs(60));
    }

    #[test]
    fn health_storage_clock_keys_parse_from_toml() {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("config.toml");
        write_file(
            &path,
            r#"
[ingest]
backend = "rtsp"
failure_threshold_s = 45
reconnect_backoff_max_s = 120

[rtsp]
url = "rtsp://example.com/stream"

[health]
heartbeat = false
log_interval_s = 30

[storage]
min_free_mb = 512
check_interval_s = 90

[clock]
skew_tolerance_s = 10
"#,
        );

        let file: WitnessdConfigFile = read_config_file(&path).expect("read config");
        let config = WitnessdConfig::from_file(file).expect("config should parse");
        assert_eq!(config.ingest.failure_threshold, Duration::from_secs(45));
        assert_eq!(
            config.ingest.reconnect_backoff_max,
            Duration::from_secs(120)
        );
        assert!(!config.health.heartbeat);
        assert_eq!(config.health.log_interval, Duration::from_secs(30));
        assert_eq!(config.storage.min_free_bytes, 512 * 1024 * 1024);
        assert_eq!(config.storage.check_interval, Duration::from_secs(90));
        assert_eq!(config.clock.skew_tolerance, Duration::from_secs(10));
    }

    #[test]
    fn zero_intervals_are_rejected() {
        let file = WitnessdConfigFile {
            health: Some(HealthConfigFile {
                heartbeat: None,
                log_interval_s: Some(0),
            }),
            rtsp: Some(RtspConfigFile {
                url: Some("rtsp://example.com/stream".to_string()),
                target_fps: None,
                width: None,
                height: None,
                backend: None,
                transport: None,
            }),
            ..WitnessdConfigFile::default()
        };
        let mut config = WitnessdConfig::from_file(file).expect("from_file should parse");
        let err = config.validate().expect_err("zero interval is invalid");
        assert!(
            err.to_string().contains("health.log_interval_s"),
            "unexpected error: {err}"
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
