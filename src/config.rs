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
// The file there is held to DEFAULT_TRACT_MODEL_SHA256 below.
// HOST-ONLY: this model runs in tract on a Pi/x86 host, not on the ESP32-S3 (which uses the
// Grove Vision AI V2 board). See scripts/fetch_detection_model.sh.
const DEFAULT_TRACT_MODEL: &str = "vendor/models/tinyyolov2-8.onnx";
/// The SHA-256 `scripts/fetch_detection_model.sh` pins the default tract model to (its
/// `MODEL_SHA256`; the unit test `default_tract_model_pin_is_the_fetch_scripts` reads the script
/// and holds the two equal). When nothing names a model, witnessd reads the default file once,
/// refuses it unless those bytes hash to this, and builds the backend from the same bytes: see
/// [`TractModelFile`].
pub const DEFAULT_TRACT_MODEL_SHA256: &str =
    "583fb7fdc948435ceac9fa82efc7708701efe8382a859a3dd46526b155f5f2ae";
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

/// Where the viewer credential file lives when nothing names it: beside the
/// capability token (`viewer_tokens.json` in `token_path`'s directory), the
/// one place the operator already keeps a secret the kernel writes. No
/// token path, no default — a kernel that persists no token has no secret
/// directory to put a second one in.
fn default_viewer_token_path(token_path: Option<&Path>) -> Option<PathBuf> {
    default_sibling_path(token_path, crate::api::VIEWER_TOKEN_FILE_NAME)
}

/// A file that lives beside the capability token when nothing names it
/// (the viewer set, the paired set, the pairing code): same directory, same
/// "no token path, no default" rule.
fn default_sibling_path(token_path: Option<&Path>, name: &str) -> Option<PathBuf> {
    let dir = token_path?.parent()?;
    Some(dir.join(name))
}

/// The event API's credential and exposure settings beyond the capability
/// token itself: where the paired-bearer set and the pairing-code file live
/// (`crate::api::pairing`), how long an issued code is redeemable, and what
/// the open `/api/fleet` discloses. One struct for both daemons' configs —
/// one `api` block, one set of keys, one set of environment names.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApiAccessSettings {
    /// `api.paired_token_path` / `WITNESS_API_PAIRED_TOKEN_PATH`: the
    /// paired-bearer set. Defaults to `paired_tokens.json` beside the
    /// capability token; `None` (no token path) turns pairing off.
    pub paired_token_path: Option<PathBuf>,
    /// `api.pairing_code_path` / `WITNESS_API_PAIRING_CODE_PATH`: where
    /// `issue-pairing-code` writes and `POST /api/pair` reads. Defaults to
    /// `pairing_code` beside the capability token.
    pub pairing_code_path: Option<PathBuf>,
    /// `api.pairing_ttl_secs` / `WITNESS_API_PAIRING_TTL_SECS`: how long an
    /// issued code lives (600 s unless `issue-pairing-code --ttl-secs`
    /// says otherwise for one code).
    pub pairing_ttl_secs: u64,
    /// `api.fleet_rollcall` / `WITNESS_API_FLEET_ROLLCALL`: `full` (the
    /// kernel plus every Canary the bridge heard) or `self` (the kernel's
    /// own row only — what a sidecar with a published port serves).
    pub fleet_rollcall: crate::api::FleetRollcall,
}

impl Default for ApiAccessSettings {
    fn default() -> Self {
        Self {
            paired_token_path: None,
            pairing_code_path: None,
            pairing_ttl_secs: crate::api::pairing::DEFAULT_PAIRING_TTL_SECS,
            fleet_rollcall: crate::api::FleetRollcall::Full,
        }
    }
}

impl ApiAccessSettings {
    fn from_file(api: Option<&ApiConfigFile>) -> Result<Self> {
        let mut settings = Self::default();
        if let Some(api) = api {
            settings.paired_token_path = api.paired_token_path.clone();
            settings.pairing_code_path = api.pairing_code_path.clone();
            if let Some(ttl) = api.pairing_ttl_secs {
                settings.pairing_ttl_secs = ttl;
            }
            if let Some(word) = &api.fleet_rollcall {
                settings.fleet_rollcall = crate::api::FleetRollcall::parse(word)
                    .map_err(|e| anyhow!("api.fleet_rollcall: {e}"))?;
            }
        }
        Ok(settings)
    }

    fn apply_env(&mut self) -> Result<()> {
        if let Ok(path) = std::env::var("WITNESS_API_PAIRED_TOKEN_PATH") {
            if !path.trim().is_empty() {
                self.paired_token_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(path) = std::env::var("WITNESS_API_PAIRING_CODE_PATH") {
            if !path.trim().is_empty() {
                self.pairing_code_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(raw) = std::env::var("WITNESS_API_PAIRING_TTL_SECS") {
            if !raw.trim().is_empty() {
                self.pairing_ttl_secs = raw.trim().parse().map_err(|_| {
                    anyhow!("WITNESS_API_PAIRING_TTL_SECS must be a positive integer")
                })?;
            }
        }
        if let Ok(word) = std::env::var("WITNESS_API_FLEET_ROLLCALL") {
            if !word.trim().is_empty() {
                self.fleet_rollcall = crate::api::FleetRollcall::parse(&word)
                    .map_err(|e| anyhow!("WITNESS_API_FLEET_ROLLCALL: {e}"))?;
            }
        }
        Ok(())
    }

    /// Unnamed files sit beside the capability token (the one directory the
    /// operator already keeps a kernel-written secret in). No token path,
    /// no default — and then no pairing.
    fn fill_defaults(&mut self, token_path: Option<&Path>) {
        if self.paired_token_path.is_none() {
            self.paired_token_path =
                default_sibling_path(token_path, crate::api::pairing::PAIRED_TOKEN_FILE_NAME);
        }
        if self.pairing_code_path.is_none() {
            self.pairing_code_path =
                default_sibling_path(token_path, crate::api::pairing::PAIRING_CODE_FILE_NAME);
        }
    }

    fn validate(&self) -> Result<()> {
        if self.pairing_ttl_secs == 0 {
            return Err(anyhow!("api.pairing_ttl_secs must be greater than zero"));
        }
        if self.paired_token_path.is_some() != self.pairing_code_path.is_some() {
            return Err(anyhow!(
                "api.paired_token_path and api.pairing_code_path go together: a kernel that \
                 redeems a pairing code must have somewhere to keep the bearer it mints \
                 (set api.token_path and both default beside it, or set both explicitly)"
            ));
        }
        Ok(())
    }

    /// The pairing files the API server takes, when pairing is on.
    pub fn pairing_paths(&self) -> Option<crate::api::PairingPaths> {
        Some(crate::api::PairingPaths {
            code_path: self.pairing_code_path.clone()?,
            tokens_path: self.paired_token_path.clone()?,
        })
    }
}

fn config_u32(value: Option<u32>, default: u32) -> u32 {
    value.unwrap_or(default)
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
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

/// What `witness_api` reads from `WITNESS_CONFIG`: the same file `witnessd` is started with, so
/// the operator subcommands (the viewer-token mint above all) run against the kernel's own
/// config. The keys it uses are typed exactly as `witnessd` types them (the same
/// `ApiConfigFile`, `ZoneConfigFile` and `RetentionConfigFile`, each `deny_unknown_fields`), so
/// a mistake in a block it reads is refused here as there. Every block only `witnessd` reads is
/// accepted and skipped: `IgnoredAny` takes any value without looking inside it, so a
/// misspelled key inside `[rtsp]`, or a `detect` that is not a table at all, passes here and is
/// `witnessd`'s to refuse when it loads the same file. A top-level key neither binary knows is
/// still refused (`deny_unknown_fields`), and the unit test
/// `both_binaries_take_the_same_top_level_keys` holds this struct's keys to
/// `WitnessdConfigFile`'s, so a block added there cannot make `witness_api` refuse the file
/// again.
#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct WitnessApiConfigFile {
    db_path: Option<String>,
    ruleset_id: Option<String>,
    api: Option<ApiConfigFile>,
    zones: Option<ZoneConfigFile>,
    retention: Option<RetentionConfigFile>,
    // witnessd-only blocks: accepted so the file parses, never read.
    #[allow(dead_code)]
    ingest: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    rtsp: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    file: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    v4l2: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    esp32: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    detect: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    health: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    storage: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    clock: Option<serde::de::IgnoredAny>,
    #[allow(dead_code)]
    storage_health: Option<serde::de::IgnoredAny>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct ApiConfigFile {
    addr: Option<String>,
    token_path: Option<PathBuf>,
    rate_limit_per_minute: Option<u32>,
    fleet_peers_path: Option<PathBuf>,
    viewer_token_path: Option<PathBuf>,
    paired_token_path: Option<PathBuf>,
    pairing_code_path: Option<PathBuf>,
    pairing_ttl_secs: Option<u64>,
    fleet_rollcall: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct IngestConfigFile {
    backend: Option<String>,
    failure_threshold_s: Option<u64>,
    reconnect_backoff_max_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct HealthConfigFile {
    heartbeat: Option<bool>,
    log_interval_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct StorageConfigFile {
    min_free_mb: Option<u64>,
    check_interval_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct ClockConfigFile {
    skew_tolerance_s: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct RtspConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
    backend: Option<String>,
    transport: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct FileConfigFile {
    path: Option<String>,
    target_fps: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct V4l2ConfigFile {
    device: Option<String>,
    target_fps: Option<u32>,
    width: Option<u32>,
    height: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct Esp32ConfigFile {
    url: Option<String>,
    target_fps: Option<u32>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct DetectConfigFile {
    backend: Option<String>,
    tract_model: Option<PathBuf>,
    tract_format: Option<String>,
    confidence: Option<f32>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct ZoneConfigFile {
    module_zone_id: Option<String>,
    sensitive: Option<Vec<String>>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct RetentionConfigFile {
    seconds: Option<u64>,
    check_interval_seconds: Option<u64>,
}

#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
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
    /// The fleet peer summary `event_mqtt_bridge --fleet-peers-path` writes;
    /// when set, `/api/fleet` lists the Canaries the bridge has heard. See
    /// [`crate::fleet_peers`].
    pub api_fleet_peers_path: Option<PathBuf>,
    /// The viewer credential file (`crate::api::ViewerTokenSet`): bearer
    /// tokens good for `GET /api/sealed-log` only, minted by
    /// `witness_api mint-viewer-token`. Defaults to `viewer_tokens.json`
    /// beside `api_token_path` when that is set; `None` keeps the route
    /// capability-only.
    pub api_viewer_token_path: Option<PathBuf>,
    /// Per-IP request cap for the event API (0 disables). See
    /// [`crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE`].
    pub api_rate_limit_per_minute: u32,
    /// Pairing files, pairing-code TTL and the `/api/fleet` roll-call mode.
    pub api_access: ApiAccessSettings,
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
    /// The fleet peer summary `event_mqtt_bridge --fleet-peers-path` writes;
    /// when set, `/api/fleet` lists the Canaries the bridge has heard. See
    /// [`crate::fleet_peers`].
    pub api_fleet_peers_path: Option<PathBuf>,
    /// The viewer credential file (`crate::api::ViewerTokenSet`): bearer
    /// tokens good for `GET /api/sealed-log` only, minted by
    /// `witness_api mint-viewer-token`. Defaults to `viewer_tokens.json`
    /// beside `api_token_path` when that is set; `None` keeps the route
    /// capability-only.
    pub api_viewer_token_path: Option<PathBuf>,
    /// Per-IP request cap for the event API (0 disables). See
    /// [`crate::api::DEFAULT_API_RATE_LIMIT_PER_MINUTE`].
    pub api_rate_limit_per_minute: u32,
    /// Pairing files, pairing-code TTL and the `/api/fleet` roll-call mode.
    pub api_access: ApiAccessSettings,
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
    /// location (the default bundled model path, populated by `scripts/fetch_detection_model.sh`) when
    /// unset. Lets operators enable object detection with just `detect.backend = "tract"`.
    pub fn tract_model_path(&self) -> PathBuf {
        self.tract_model
            .clone()
            .unwrap_or_else(|| PathBuf::from(DEFAULT_TRACT_MODEL))
    }

    /// Read the tract model witnessd builds its backend from: the default file held to
    /// [`DEFAULT_TRACT_MODEL_SHA256`] when nothing names a model, or the configured one as given.
    /// See [`TractModelFile`] for what each kind guarantees.
    pub fn tract_model_file(&self) -> Result<TractModelFile> {
        self.tract_model_file_with_default(
            Path::new(DEFAULT_TRACT_MODEL),
            DEFAULT_TRACT_MODEL_SHA256,
        )
    }

    /// [`tract_model_file`](Self::tract_model_file) with the default location and its pin given,
    /// so a test can stand a file in for the default without writing into `vendor/models/` (the
    /// real default is relative to witnessd's working directory).
    pub fn tract_model_file_with_default(
        &self,
        default_path: &Path,
        default_sha256: &str,
    ) -> Result<TractModelFile> {
        match &self.tract_model {
            Some(path) => {
                if path.as_os_str().is_empty() {
                    return Err(anyhow!("detect.tract_model must not be empty"));
                }
                let sha256 = sha256_of_file(path).map_err(|e| {
                    anyhow!(
                        "failed to read the configured tract model {}: {}",
                        path.display(),
                        e
                    )
                })?;
                let matches_pin = sha256 == default_sha256;
                Ok(TractModelFile {
                    path: path.clone(),
                    sha256,
                    pinned: None,
                    matches_pin,
                })
            }
            None => {
                // One open, one read: these bytes are hashed here and are the bytes the backend
                // is built from (TractModelFile::pinned_bytes), so the file is never opened
                // again and nothing can swap it between the check and the load.
                let bytes = std::fs::read(default_path).map_err(|e| {
                    anyhow!(
                        "failed to read the default tract model {} ({}): run \
                         scripts/fetch_detection_model.sh to fetch and verify it, or set \
                         detect.tract_model (or WITNESS_TRACT_MODEL) to a model of your own",
                        default_path.display(),
                        e
                    )
                })?;
                let sha256 = hex::encode(<sha2::Sha256 as sha2::Digest>::digest(&bytes));
                if sha256 != default_sha256 {
                    return Err(anyhow!(
                        "refusing the default tract model {}: its sha256 is {}, not the {} \
                         scripts/fetch_detection_model.sh pins, so it is not the model that \
                         script fetched (it was replaced or corrupted on disk). Run \
                         scripts/fetch_detection_model.sh to restore the verified model, or, \
                         to run a model of your own, name it with detect.tract_model (or \
                         WITNESS_TRACT_MODEL): a configured model loads as given and its \
                         sha256 is logged at startup",
                        default_path.display(),
                        sha256,
                        default_sha256
                    ));
                }
                Ok(TractModelFile {
                    path: default_path.to_path_buf(),
                    sha256,
                    pinned: Some(bytes),
                    matches_pin: true,
                })
            }
        }
    }
}

/// The tract model file witnessd builds its backend from, read by
/// [`DetectSettings::tract_model_file`]. Two kinds, by whether anything named the file:
///
/// * **Pinned default.** Neither `detect.tract_model` nor `WITNESS_TRACT_MODEL` names a model
///   (an empty `WITNESS_TRACT_MODEL` names none), so it is the file
///   `scripts/fetch_detection_model.sh` writes. It was opened once and read whole, its SHA-256
///   matched [`DEFAULT_TRACT_MODEL_SHA256`] (a mismatch is refused before this exists), and
///   [`pinned_bytes`](Self::pinned_bytes) are the bytes that were hashed. The backend is built
///   from them, never from the path again, so a file swapped after the read does not reach it.
///   A symlink at the default path is followed by that one open: its target's bytes are what
///   is hashed and built.
/// * **Configured.** The operator named a model, which is the operator's choice: it loads as
///   given, by path (tract opens it itself, so external data beside it still resolves).
///   [`sha256`](Self::sha256) is of the file as read here, for the startup log; it describes
///   the file and does not guard it. That holds when the configured path is the default path
///   too: naming the file opts out of the pin, and the log says whether it matches.
///
/// The boundary: the pin catches a default model file that was swapped or corrupted after the
/// fetch. It does not stop an account that can edit the config (it can name any model) or the
/// binary (it can change the pin).
#[derive(Debug)]
pub struct TractModelFile {
    path: PathBuf,
    sha256: String,
    /// The verified bytes of the pinned default; `None` for a configured model.
    pinned: Option<Vec<u8>>,
    /// Whether `sha256` is the pin (always, for the pinned default).
    matches_pin: bool,
}

impl TractModelFile {
    /// The file that was read.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Lowercase hex SHA-256 of the file as read.
    pub fn sha256(&self) -> &str {
        &self.sha256
    }

    /// Whether the file's SHA-256 is [`DEFAULT_TRACT_MODEL_SHA256`] (always true for the pinned
    /// default; for a configured model, what the startup line reports).
    pub fn matches_pin(&self) -> bool {
        self.matches_pin
    }

    /// The pinned default's bytes, the ones that were hashed: what the backend is built from.
    /// `None` for a configured model, which loads by path.
    pub fn pinned_bytes(&self) -> Option<&[u8]> {
        self.pinned.as_deref()
    }

    /// The line witnessd logs at startup for this model.
    pub fn startup_line(&self) -> String {
        if self.pinned.is_some() {
            format!(
                "tract model {}: sha256 {}, the pinned default (scripts/fetch_detection_model.sh), \
                 checked at load and built from the bytes checked",
                self.path.display(),
                self.sha256
            )
        } else {
            format!(
                "tract model {}: sha256 {}, named by detect.tract_model or WITNESS_TRACT_MODEL \
                 and loaded as given, not held to a pin ({} the pinned default model)",
                self.path.display(),
                self.sha256,
                if self.matches_pin {
                    "it is"
                } else {
                    "it is not"
                }
            )
        }
    }
}

/// Lowercase hex SHA-256 of a file, read in chunks (a model can be tens of megabytes).
fn sha256_of_file(path: &Path) -> std::io::Result<String> {
    use sha2::Digest;
    use std::io::Read;
    let mut file = std::fs::File::open(path)?;
    let mut hasher = sha2::Sha256::new();
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => hasher.update(&buf[..n]),
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(hex::encode(hasher.finalize()))
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
        let api_fleet_peers_path = file
            .api
            .as_ref()
            .and_then(|api| api.fleet_peers_path.clone());
        let api_viewer_token_path = file
            .api
            .as_ref()
            .and_then(|api| api.viewer_token_path.clone());
        let api_access = ApiAccessSettings::from_file(file.api.as_ref())?;
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
            api_fleet_peers_path,
            api_viewer_token_path,
            api_rate_limit_per_minute,
            api_access,
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
        if let Ok(path) = std::env::var("WITNESS_FLEET_PEERS_PATH") {
            if !path.trim().is_empty() {
                self.api_fleet_peers_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(path) = std::env::var("WITNESS_API_VIEWER_TOKEN_PATH") {
            if !path.trim().is_empty() {
                self.api_viewer_token_path = Some(PathBuf::from(path));
            }
        }
        if self.api_viewer_token_path.is_none() {
            self.api_viewer_token_path = default_viewer_token_path(self.api_token_path.as_deref());
        }
        self.api_access.apply_env()?;
        self.api_access
            .fill_defaults(self.api_token_path.as_deref());
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
        self.api_access.validate()?;
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
            // scripts/fetch_detection_model.sh), held to DEFAULT_TRACT_MODEL_SHA256 at startup
            // (DetectSettings::tract_model_file). Only an explicitly-empty path is invalid; a
            // missing or unpinned default is refused at startup with an error naming the script.
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
        let api_fleet_peers_path = file
            .api
            .as_ref()
            .and_then(|api| api.fleet_peers_path.clone());
        let api_viewer_token_path = file
            .api
            .as_ref()
            .and_then(|api| api.viewer_token_path.clone());
        let api_access = ApiAccessSettings::from_file(file.api.as_ref())?;
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
            api_fleet_peers_path,
            api_viewer_token_path,
            api_rate_limit_per_minute,
            api_access,
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
        if let Ok(path) = std::env::var("WITNESS_FLEET_PEERS_PATH") {
            if !path.trim().is_empty() {
                self.api_fleet_peers_path = Some(PathBuf::from(path));
            }
        }
        if let Ok(path) = std::env::var("WITNESS_API_VIEWER_TOKEN_PATH") {
            if !path.trim().is_empty() {
                self.api_viewer_token_path = Some(PathBuf::from(path));
            }
        }
        if self.api_viewer_token_path.is_none() {
            self.api_viewer_token_path = default_viewer_token_path(self.api_token_path.as_deref());
        }
        self.api_access.apply_env()?;
        self.api_access
            .fill_defaults(self.api_token_path.as_deref());
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
        self.api_access.validate()?;
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
    fn viewer_token_path_parses_in_both_api_blocks_and_defaults_beside_the_token() {
        // deny_unknown_fields: the key must be accepted by BOTH binaries'
        // api block, or a config naming it is a startup error.
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("config.json");
        write_file(
            &path,
            r#"{"api":{"token_path":"/data/api_token","viewer_token_path":"/secrets/viewers.json"}}"#,
        );
        let file: WitnessApiConfigFile = read_config_file(&path).expect("witness_api config");
        let config = WitnessApiConfig::from_file(file).expect("witness_api config should parse");
        assert_eq!(
            config.api_viewer_token_path,
            Some(PathBuf::from("/secrets/viewers.json"))
        );
        let file: WitnessdConfigFile = read_config_file(&path).expect("witnessd config");
        let config = WitnessdConfig::from_file(file).expect("witnessd config should parse");
        assert_eq!(
            config.api_viewer_token_path,
            Some(PathBuf::from("/secrets/viewers.json"))
        );

        // Unnamed, it sits beside the capability token; no token path, no
        // default (the route then stays capability-only).
        assert_eq!(
            default_viewer_token_path(Some(Path::new("/data/api_token"))),
            Some(PathBuf::from("/data").join(crate::api::VIEWER_TOKEN_FILE_NAME))
        );
        assert_eq!(default_viewer_token_path(None), None);
    }

    #[test]
    fn api_access_keys_parse_in_both_api_blocks_and_default_beside_the_token() {
        // deny_unknown_fields: every key must be accepted by BOTH binaries'
        // api block, or a config naming it is a startup error.
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("config.json");
        write_file(
            &path,
            r#"{"api":{"token_path":"/data/api_token","paired_token_path":"/secrets/paired.json","pairing_code_path":"/secrets/code","pairing_ttl_secs":120,"fleet_rollcall":"self"}}"#,
        );
        let file: WitnessApiConfigFile = read_config_file(&path).expect("witness_api config");
        let api = WitnessApiConfig::from_file(file).expect("witness_api config should parse");
        let file: WitnessdConfigFile = read_config_file(&path).expect("witnessd config");
        let daemon = WitnessdConfig::from_file(file).expect("witnessd config should parse");
        for access in [&api.api_access, &daemon.api_access] {
            assert_eq!(
                access.paired_token_path,
                Some(PathBuf::from("/secrets/paired.json"))
            );
            assert_eq!(
                access.pairing_code_path,
                Some(PathBuf::from("/secrets/code"))
            );
            assert_eq!(access.pairing_ttl_secs, 120);
            assert_eq!(access.fleet_rollcall, crate::api::FleetRollcall::SelfOnly);
            let paths = access.pairing_paths().expect("both paths configured");
            assert_eq!(paths.code_path, PathBuf::from("/secrets/code"));
            assert_eq!(paths.tokens_path, PathBuf::from("/secrets/paired.json"));
        }

        // Unnamed, both sit beside the capability token; no token path, no
        // default, no pairing.
        let mut access = ApiAccessSettings::default();
        assert_eq!(access.fleet_rollcall, crate::api::FleetRollcall::Full);
        assert_eq!(
            access.pairing_ttl_secs,
            crate::api::pairing::DEFAULT_PAIRING_TTL_SECS
        );
        access.fill_defaults(Some(Path::new("/data/api_token")));
        assert_eq!(
            access.paired_token_path,
            Some(PathBuf::from("/data").join(crate::api::pairing::PAIRED_TOKEN_FILE_NAME))
        );
        assert_eq!(
            access.pairing_code_path,
            Some(PathBuf::from("/data").join(crate::api::pairing::PAIRING_CODE_FILE_NAME))
        );
        access.validate().expect("both set validates");
        let mut none = ApiAccessSettings::default();
        none.fill_defaults(None);
        assert_eq!(none.pairing_paths(), None);
        none.validate().expect("neither set validates");

        // A lone half is refused: a redeemable code with nowhere to keep
        // the bearer is a misconfiguration, not a mode.
        let half = ApiAccessSettings {
            pairing_code_path: Some(PathBuf::from("/secrets/code")),
            ..ApiAccessSettings::default()
        };
        assert!(half.validate().is_err());
        let zero_ttl = ApiAccessSettings {
            pairing_ttl_secs: 0,
            ..ApiAccessSettings::default()
        };
        assert!(zero_ttl.validate().is_err());

        // An unknown roll-call word is a startup error, in either block.
        write_file(&path, r#"{"api":{"fleet_rollcall":"everyone"}}"#);
        let file: WitnessApiConfigFile = read_config_file(&path).expect("parse");
        assert!(WitnessApiConfig::from_file(file).is_err());
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

    // A misspelled key in the witnessd config must be a parse error, not a
    // silent fallback to the (weaker) default — `sensitive` zones and
    // `retention.seconds` are privacy controls, and a typo that quietly
    // disables them is a fail-open.

    fn parse_witnessd(toml_str: &str) -> Result<WitnessdConfigFile> {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("witness.toml");
        write_file(&path, toml_str);
        read_config_file::<WitnessdConfigFile>(&path)
    }

    #[test]
    fn zones_sensitive_typo_is_rejected_not_unprotected() {
        let err = parse_witnessd("[zones]\nsensitve = [\"front_door\"]\n")
            .expect_err("typo'd sensitive key must fail");
        assert!(err.to_string().contains("sensitve"), "{err}");
    }

    #[test]
    fn retention_seconds_typo_is_rejected_not_default_retention() {
        let err = parse_witnessd("[retention]\nsecond = 3600\n")
            .expect_err("typo'd retention key must fail");
        assert!(err.to_string().contains("second"), "{err}");
    }

    #[test]
    fn root_unknown_key_is_rejected() {
        let err =
            parse_witnessd("db_pth = \"witness.db\"\n").expect_err("typo'd root key must fail");
        assert!(err.to_string().contains("db_pth"), "{err}");
    }

    #[test]
    fn detect_confidence_typo_is_rejected() {
        let err =
            parse_witnessd("[detect]\nconfidnce = 0.9\n").expect_err("typo'd detect key must fail");
        assert!(err.to_string().contains("confidnce"), "{err}");
    }

    #[test]
    fn shipped_example_config_parses() {
        let raw =
            std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/config.example.toml"))
                .expect("read config.example.toml");
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("witness.toml");
        write_file(&path, &raw);
        read_config_file::<WitnessdConfigFile>(&path)
            .expect("shipped example config must parse under deny_unknown_fields");
    }

    // ── witness_api reads a witnessd file (sweep A103) ─────────────────────

    /// The field names a `#[derive(Deserialize)]` struct declares: the `fields` list serde
    /// hands `deserialize_struct`, captured by a deserializer that refuses right after, so
    /// nothing is built.
    fn declared_fields<T: for<'de> Deserialize<'de>>() -> &'static [&'static str] {
        struct Capture(Option<&'static [&'static str]>);
        impl<'de> serde::Deserializer<'de> for &mut Capture {
            type Error = serde::de::value::Error;
            fn deserialize_any<V: serde::de::Visitor<'de>>(
                self,
                _visitor: V,
            ) -> Result<V::Value, Self::Error> {
                Err(serde::de::Error::custom("only a struct is read"))
            }
            fn deserialize_struct<V: serde::de::Visitor<'de>>(
                self,
                _name: &'static str,
                fields: &'static [&'static str],
                _visitor: V,
            ) -> Result<V::Value, Self::Error> {
                self.0 = Some(fields);
                Err(serde::de::Error::custom("fields captured"))
            }
            serde::forward_to_deserialize_any! {
                bool i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 char str string bytes
                byte_buf option unit unit_struct newtype_struct seq tuple tuple_struct map enum
                identifier ignored_any
            }
        }
        let mut capture = Capture(None);
        let _ = T::deserialize(&mut capture);
        capture.0.expect("the type deserializes as a struct")
    }

    fn parse_witness_api(toml_str: &str) -> Result<WitnessApiConfigFile> {
        let dir = tempdir().expect("temp dir");
        let path = dir.path().join("witness.toml");
        write_file(&path, toml_str);
        read_config_file::<WitnessApiConfigFile>(&path)
    }

    /// docs/rtsp_setup.md's "Create `witness.toml`" example, read from the doc itself.
    fn rtsp_setup_example() -> String {
        let doc =
            std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/docs/rtsp_setup.md"))
                .expect("read docs/rtsp_setup.md");
        let at = doc
            .find("Create `witness.toml`:")
            .expect("docs/rtsp_setup.md still says \"Create `witness.toml`:\"");
        let open = at + doc[at..].find("```toml\n").expect("a toml block follows") + 8;
        let close = open + doc[open..].find("```").expect("the toml block closes");
        let example = doc[open..close].to_string();
        assert!(
            example.contains("[rtsp]"),
            "the example is a witnessd file: {example}"
        );
        example
    }

    #[test]
    fn witness_api_reads_the_rtsp_setup_example() {
        let example = rtsp_setup_example();
        parse_witnessd(&example).expect("the example is a witnessd config");
        let file = parse_witness_api(&example).expect("witness_api must accept the same file");
        let config = WitnessApiConfig::from_file(file).expect("witness_api config");
        // The shared [zones] block still reaches witness_api.
        assert_eq!(config.sensitive_zones, vec!["zone:front_door".to_string()]);
    }

    #[test]
    fn witness_api_reads_the_full_shipped_witnessd_file() {
        let raw =
            std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/config.example.toml"))
                .expect("read config.example.toml");
        let daemon = parse_witnessd(&raw).expect("the shipped example is a witnessd config");
        // It is a full file: every witnessd-only block is in it.
        for (name, present) in [
            ("ingest", daemon.ingest.is_some()),
            ("rtsp", daemon.rtsp.is_some()),
            ("file", daemon.file.is_some()),
            ("v4l2", daemon.v4l2.is_some()),
            ("esp32", daemon.esp32.is_some()),
            ("detect", daemon.detect.is_some()),
            ("health", daemon.health.is_some()),
            ("storage", daemon.storage.is_some()),
            ("clock", daemon.clock.is_some()),
            ("storage_health", daemon.storage_health.is_some()),
        ] {
            assert!(present, "config.example.toml has no [{name}] block");
        }
        let file = parse_witness_api(&raw).expect("witness_api must accept the full witnessd file");
        let api = WitnessApiConfig::from_file(file).expect("witness_api config");
        let daemon = WitnessdConfig::from_file(daemon).expect("witnessd config");
        // The keys both binaries read come out the same.
        assert_eq!(api.db_path, daemon.db_path);
        assert_eq!(api.ruleset_id, daemon.ruleset_id);
        assert_eq!(api.api_addr, daemon.api_addr);
        assert_eq!(api.sensitive_zones, daemon.zones.sensitive_zones);
        assert_eq!(api.retention, daemon.retention);
    }

    #[test]
    fn the_shared_api_block_reaches_witness_api_from_a_witnessd_file() {
        let file = parse_witness_api(
            "db_path = \"/data/witness.db\"\nruleset_id = \"ruleset:v9\"\n\
             [api]\naddr = \"0.0.0.0:8799\"\ntoken_path = \"/data/api_token\"\n\
             viewer_token_path = \"/secrets/viewers.json\"\n\
             paired_token_path = \"/secrets/paired.json\"\n\
             pairing_code_path = \"/secrets/code\"\nrate_limit_per_minute = 30\n\
             [rtsp]\nurl = \"rtsp://camera/stream\"\nwidth = 640\nheight = 480\n\
             [detect]\nbackend = \"tract\"\ntract_model = \"/models/x.onnx\"\n\
             [retention]\nseconds = 3600\n",
        )
        .expect("a witnessd file with a full [api] block");
        let config = WitnessApiConfig::from_file(file).expect("witness_api config");
        assert_eq!(config.db_path, "/data/witness.db");
        assert_eq!(config.ruleset_id, "ruleset:v9");
        assert_eq!(config.api_addr, "0.0.0.0:8799");
        assert_eq!(
            config.api_token_path,
            Some(PathBuf::from("/data/api_token"))
        );
        assert_eq!(
            config.api_viewer_token_path,
            Some(PathBuf::from("/secrets/viewers.json"))
        );
        assert_eq!(config.api_rate_limit_per_minute, 30);
        let pairing = config.api_access.pairing_paths().expect("pairing paths");
        assert_eq!(pairing.tokens_path, PathBuf::from("/secrets/paired.json"));
        assert_eq!(pairing.code_path, PathBuf::from("/secrets/code"));
        assert_eq!(config.retention, Duration::from_secs(3600));
    }

    #[test]
    fn witness_api_still_refuses_a_key_neither_binary_knows() {
        // A misspelled top-level key, and one inside the [api] block witness_api reads.
        for (bad, key) in [
            (
                "db_pth = \"witness.db\"\n[rtsp]\nurl = \"rtsp://c/s\"\n",
                "db_pth",
            ),
            (
                "[rtsp]\nurl = \"rtsp://c/s\"\n[api]\ntoken_pth = \"/data/t\"\n",
                "token_pth",
            ),
            ("[zones]\nsensitve = [\"zone:a\"]\n", "sensitve"),
            ("[retention]\nsecond = 60\n", "second"),
        ] {
            let err = parse_witness_api(bad).expect_err("an unknown key must fail witness_api");
            assert!(err.to_string().contains(key), "{key}: {err}");
            let err = parse_witnessd(bad).expect_err("and witnessd");
            assert!(err.to_string().contains(key), "{key}: {err}");
        }
    }

    #[test]
    fn a_mistake_inside_a_witnessd_only_block_is_witnessds_to_refuse() {
        // IgnoredAny takes any value without looking inside it: a misspelled key inside
        // [rtsp], or a detect that is not a table, passes witness_api, which reads neither,
        // and witnessd, which reads both, refuses the same file.
        for (bad, what) in [
            ("[rtsp]\nurll = \"rtsp://c/s\"\n", "urll"),
            ("detect = 5\n", "detect"),
            ("[[storage]]\nmin_free_mb = 1\n", "storage"),
        ] {
            parse_witness_api(bad)
                .unwrap_or_else(|e| panic!("witness_api skips {what}, but refused it: {e}"));
            let err = parse_witnessd(bad).expect_err("witnessd refuses it");
            assert!(err.to_string().contains(what), "{what}: {err}");
        }
    }

    #[test]
    fn both_binaries_take_the_same_top_level_keys() {
        let mut daemon = declared_fields::<WitnessdConfigFile>().to_vec();
        let mut api = declared_fields::<WitnessApiConfigFile>().to_vec();
        daemon.sort_unstable();
        api.sort_unstable();
        assert_eq!(
            api, daemon,
            "WitnessApiConfigFile must accept every top-level key WitnessdConfigFile does (as \
             IgnoredAny when witness_api does not read it) and no other, or witness_api refuses \
             the kernel's own config file"
        );
        // The capture reads real field lists, not an empty one.
        assert!(daemon.contains(&"rtsp") && daemon.contains(&"api"));
    }

    // ── The default tract model's pin (sweep A102) ─────────────────────────

    fn sha256_hex(bytes: &[u8]) -> String {
        hex::encode(<sha2::Sha256 as sha2::Digest>::digest(bytes))
    }

    fn detect_with_model(tract_model: Option<PathBuf>) -> DetectSettings {
        DetectSettings {
            backend: DetectBackendPreference::Tract,
            tract_model,
            tract_format: TractFormat::Yolov2,
            confidence_threshold: DEFAULT_DETECT_CONFIDENCE,
        }
    }

    #[test]
    fn default_tract_model_pin_is_the_fetch_scripts() {
        let script = std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/scripts/fetch_detection_model.sh"
        ))
        .expect("read scripts/fetch_detection_model.sh");
        let assigned = |name: &str| -> String {
            let prefix = format!("{name}=");
            let values: Vec<&str> = script
                .lines()
                .filter_map(|line| line.strip_prefix(prefix.as_str()))
                .collect();
            assert_eq!(
                values.len(),
                1,
                "one {name}= line in the script: {values:?}"
            );
            values[0].trim_matches('"').to_string()
        };
        assert_eq!(
            assigned("MODEL_SHA256"),
            DEFAULT_TRACT_MODEL_SHA256,
            "scripts/fetch_detection_model.sh's MODEL_SHA256 and src/config.rs's \
             DEFAULT_TRACT_MODEL_SHA256 change together"
        );
        assert!(
            DEFAULT_TRACT_MODEL_SHA256.len() == 64
                && DEFAULT_TRACT_MODEL_SHA256
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
            "the pin is 64 lowercase hex, the form the load-time check compares"
        );
        // The pin names the file the script writes.
        let dir = assigned("MODEL_DIR");
        let written = assigned("MODEL_PATH").replace("${MODEL_DIR}", &dir);
        assert_eq!(
            written.strip_prefix("${REPO_ROOT}/"),
            Some(DEFAULT_TRACT_MODEL),
            "the script writes {written}"
        );
    }

    #[test]
    fn an_unset_tract_model_is_the_default_read_once_and_held_to_the_pin() {
        let dir = tempdir().expect("temp dir");
        let default = dir.path().join("default.onnx");
        write_file(&default, "the pinned model");
        let pin = sha256_hex(b"the pinned model");
        let model = detect_with_model(None)
            .tract_model_file_with_default(&default, &pin)
            .expect("the pinned bytes load");
        assert_eq!(model.path(), default.as_path());
        assert_eq!(model.sha256(), pin);
        assert!(model.matches_pin());
        assert_eq!(model.pinned_bytes(), Some(&b"the pinned model"[..]));
        let line = model.startup_line();
        assert!(
            line.contains(&pin) && line.contains("pinned default"),
            "{line}"
        );
        // The bytes in hand are the bytes hashed: a swap after the read does not reach them.
        write_file(&default, "a swapped model");
        assert_eq!(model.pinned_bytes(), Some(&b"the pinned model"[..]));
    }

    #[test]
    fn a_default_tract_model_that_is_not_the_pinned_one_is_refused() {
        let dir = tempdir().expect("temp dir");
        let default = dir.path().join("default.onnx");
        write_file(&default, "the pinned modem");
        let pin = sha256_hex(b"the pinned model");
        let err = detect_with_model(None)
            .tract_model_file_with_default(&default, &pin)
            .expect_err("a one-byte change is refused");
        let msg = err.to_string();
        for needle in [
            default.display().to_string(),
            sha256_hex(b"the pinned modem"),
            pin,
            "scripts/fetch_detection_model.sh".to_string(),
            "detect.tract_model".to_string(),
            "WITNESS_TRACT_MODEL".to_string(),
        ] {
            assert!(msg.contains(&needle), "the refusal names {needle}: {msg}");
        }
        // The real pin refuses it too.
        assert!(detect_with_model(None)
            .tract_model_file_with_default(&default, DEFAULT_TRACT_MODEL_SHA256)
            .is_err());
    }

    #[test]
    fn a_missing_default_tract_model_names_the_fetch_script() {
        let dir = tempdir().expect("temp dir");
        let default = dir.path().join("absent.onnx");
        let err = detect_with_model(None)
            .tract_model_file_with_default(&default, DEFAULT_TRACT_MODEL_SHA256)
            .expect_err("no file, no model");
        let msg = err.to_string();
        assert!(msg.contains(&default.display().to_string()), "{msg}");
        assert!(msg.contains("scripts/fetch_detection_model.sh"), "{msg}");
    }

    #[cfg(unix)]
    #[test]
    fn a_symlink_at_the_default_path_is_held_to_its_targets_bytes() {
        let dir = tempdir().expect("temp dir");
        let good = dir.path().join("good.onnx");
        let bad = dir.path().join("bad.onnx");
        write_file(&good, "the pinned model");
        write_file(&bad, "another model");
        let pin = sha256_hex(b"the pinned model");
        let default = dir.path().join("default.onnx");
        std::os::unix::fs::symlink(&bad, &default).expect("symlink");
        assert!(detect_with_model(None)
            .tract_model_file_with_default(&default, &pin)
            .is_err());
        std::fs::remove_file(&default).expect("unlink");
        std::os::unix::fs::symlink(&good, &default).expect("symlink");
        let model = detect_with_model(None)
            .tract_model_file_with_default(&default, &pin)
            .expect("a link to the pinned bytes loads them");
        assert_eq!(model.pinned_bytes(), Some(&b"the pinned model"[..]));
    }

    #[test]
    fn a_configured_tract_model_loads_as_given_and_reports_its_hash() {
        let dir = tempdir().expect("temp dir");
        let default = dir.path().join("default.onnx");
        let custom = dir.path().join("custom.onnx");
        write_file(&custom, "an operator's model");
        let pin = sha256_hex(b"the pinned model");
        // The default location is not read at all (here it does not exist).
        let model = detect_with_model(Some(custom.clone()))
            .tract_model_file_with_default(&default, &pin)
            .expect("a configured model is the operator's choice");
        assert_eq!(model.path(), custom.as_path());
        assert_eq!(model.sha256(), sha256_hex(b"an operator's model"));
        assert!(!model.matches_pin());
        assert_eq!(
            model.pinned_bytes(),
            None,
            "a configured model loads by path"
        );
        let line = model.startup_line();
        for needle in [
            custom.display().to_string(),
            sha256_hex(b"an operator's model"),
            "loaded as given".to_string(),
            "it is not the pinned default model".to_string(),
        ] {
            assert!(
                line.contains(&needle),
                "the startup line names {needle}: {line}"
            );
        }
        // Naming the default path itself is configuring it: as given, and the line says
        // whether the file there is the pinned model.
        write_file(&default, "another model");
        let named = detect_with_model(Some(default.clone()))
            .tract_model_file_with_default(&default, &pin)
            .expect("named, the default path loads as given");
        assert_eq!(named.pinned_bytes(), None);
        assert!(named
            .startup_line()
            .contains("it is not the pinned default model"));
        write_file(&default, "the pinned model");
        let named = detect_with_model(Some(default.clone()))
            .tract_model_file_with_default(&default, &pin)
            .expect("named and matching");
        assert!(named.matches_pin());
        assert!(named
            .startup_line()
            .contains("it is the pinned default model"));
    }

    #[test]
    fn an_empty_or_unreadable_configured_tract_model_is_refused() {
        let dir = tempdir().expect("temp dir");
        let err = detect_with_model(Some(PathBuf::new()))
            .tract_model_file_with_default(&dir.path().join("d"), DEFAULT_TRACT_MODEL_SHA256)
            .expect_err("an empty path names nothing");
        assert!(err.to_string().contains("must not be empty"), "{err}");
        let absent = dir.path().join("absent.onnx");
        let err = detect_with_model(Some(absent.clone()))
            .tract_model_file_with_default(&dir.path().join("d"), DEFAULT_TRACT_MODEL_SHA256)
            .expect_err("a configured file that is not there");
        assert!(
            err.to_string().contains(&absent.display().to_string()),
            "{err}"
        );
    }
}
