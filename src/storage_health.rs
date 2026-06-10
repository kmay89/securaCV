//! Storage endurance and health monitoring for SD-card-backed deployments.
//!
//! SD cards are consumables with finite write endurance and no SMART
//! reporting over the SD interface. This module gives operators proactive
//! visibility instead: free space, database footprint, whole-device write
//! tracking (via `/sys/block/<dev>/stat`), a conservative wear estimate
//! against a configurable endurance rating (TBW), SoC temperature, and a
//! computed status with hysteresis so alerts never flap:
//!
//! `Good -> Degraded -> ReplacementRecommended -> Critical`
//!
//! The wear estimate counts *all* writes to the device (OS included), which
//! is correct for card wear but means it is a whole-system figure, and it is
//! an estimate, not a measurement — see docs/sd_card_health.md.
//!
//! All probes are Linux-specific and degrade to `None` on other platforms or
//! inside containers where `/sys` is not visible; the report is still
//! produced with whatever could be observed.

use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::TimeBucket;

/// Snapshot shared with the API thread. `None` until the first sample.
pub type SharedStorageHealth = Arc<RwLock<Option<StorageHealthReport>>>;

/// Bytes per sector in `/sys/block/<dev>/stat` (fixed by the kernel ABI,
/// independent of the device's logical sector size).
const SECTOR_BYTES: u64 = 512;

/// Window for write-rate estimation and write-error classification.
const RATE_WINDOW_S: u64 = 86_400;

/// Minimum sample span before a write rate is reported (avoids wild
/// extrapolation from two samples a few seconds apart).
const MIN_RATE_SPAN_S: u64 = 600;

/// Free-space recovery margin: a status driven by low free space only clears
/// once free space exceeds the threshold by this many percentage points.
const FREE_SPACE_RECOVERY_MARGIN_PCT: f64 = 2.0;

/// Consecutive samples required before the published status changes
/// (in either direction). Prevents alarm flapping on transient conditions.
const STATUS_CONFIRM_SAMPLES: u32 = 3;

/// Bound on the persisted sample ring (covers > 24h at the default
/// 10-minute check interval).
const MAX_SAMPLES: usize = 288;

// -------------------- Status types --------------------

/// Overall storage health. Ordering is severity: `Good < Degraded <
/// ReplacementRecommended < Critical`.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "snake_case")]
pub enum StorageHealthStatus {
    Good,
    Degraded,
    ReplacementRecommended,
    Critical,
}

impl StorageHealthStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Good => "good",
            Self::Degraded => "degraded",
            Self::ReplacementRecommended => "replacement_recommended",
            Self::Critical => "critical",
        }
    }
}

/// SoC thermal classification. Surfaced alongside (not folded into) the
/// storage status: heat accelerates flash wear and triggers Pi throttling,
/// but a hot afternoon is not a storage fault.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ThermalStatus {
    Ok,
    Warm,
    Hot,
}

// -------------------- Thresholds --------------------

/// Classification thresholds. Defaults are conservative; all are
/// operator-tunable via `[storage_health]` in the config file.
#[derive(Debug, Clone)]
pub struct StorageHealthThresholds {
    pub free_space_warn_pct: f64,
    pub free_space_critical_pct: f64,
    pub wear_warn_pct: f64,
    pub wear_critical_pct: f64,
    pub temp_warn_c: f64,
    pub temp_hot_c: f64,
}

impl Default for StorageHealthThresholds {
    fn default() -> Self {
        Self {
            free_space_warn_pct: 15.0,
            free_space_critical_pct: 5.0,
            wear_warn_pct: 80.0,
            wear_critical_pct: 95.0,
            temp_warn_c: 70.0,
            temp_hot_c: 80.0,
        }
    }
}

impl StorageHealthThresholds {
    /// Thresholds used when testing for recovery to a better status: free
    /// space must clear its threshold by a margin. Wear thresholds are
    /// unchanged because wear is monotonic and never recovers.
    fn with_recovery_margin(&self) -> Self {
        Self {
            free_space_warn_pct: self.free_space_warn_pct + FREE_SPACE_RECOVERY_MARGIN_PCT,
            free_space_critical_pct: self.free_space_critical_pct + FREE_SPACE_RECOVERY_MARGIN_PCT,
            ..self.clone()
        }
    }
}

// -------------------- Report --------------------

/// Storage metrics for one sample. Fields are `None` when the underlying
/// probe is unavailable (non-Linux, container without /sys, etc.).
#[derive(Clone, Debug, Serialize)]
pub struct StorageMetrics {
    pub status: StorageHealthStatus,
    pub free_bytes: Option<u64>,
    pub total_bytes: Option<u64>,
    pub free_pct: Option<f64>,
    pub db_bytes: u64,
    pub wal_bytes: u64,
    /// Cumulative sealed-log write failures observed this process lifetime.
    pub write_errors: u64,
    pub write_rate_bytes_per_day: Option<u64>,
    /// Whole-device bytes written, accumulated across reboots.
    pub lifetime_bytes_written: Option<u64>,
    /// Configured card endurance rating, in TB written.
    pub endurance_tbw: f64,
    pub wear_pct: Option<f64>,
    /// Projected days until the endurance rating is consumed at the current
    /// write rate. `None` when the rate is unknown or effectively zero.
    pub estimated_days_remaining: Option<u64>,
    pub source_device: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
pub struct ThermalMetrics {
    pub soc_temp_c: Option<f32>,
    pub status: ThermalStatus,
}

/// One published health sample. Carries a coarse time bucket rather than a
/// precise timestamp, consistent with Invariant III (metadata minimization).
#[derive(Clone, Debug, Serialize)]
pub struct StorageHealthReport {
    pub storage: StorageMetrics,
    pub thermal: ThermalMetrics,
    pub time_bucket: TimeBucket,
}

// -------------------- Pure classification --------------------

/// The observed values that drive status classification.
#[derive(Debug, Clone, Default)]
pub struct Observation {
    pub free_pct: Option<f64>,
    pub wear_pct: Option<f64>,
    pub write_errors_24h: u64,
}

/// Worst-of classification across free space, wear, and recent write errors.
pub fn classify(obs: &Observation, t: &StorageHealthThresholds) -> StorageHealthStatus {
    let mut status = StorageHealthStatus::Good;
    if let Some(free) = obs.free_pct {
        if free < t.free_space_critical_pct {
            status = status.max(StorageHealthStatus::Critical);
        } else if free < t.free_space_warn_pct {
            status = status.max(StorageHealthStatus::Degraded);
        }
    }
    if let Some(wear) = obs.wear_pct {
        if wear >= t.wear_critical_pct {
            status = status.max(StorageHealthStatus::Critical);
        } else if wear >= t.wear_warn_pct {
            status = status.max(StorageHealthStatus::ReplacementRecommended);
        }
    }
    if obs.write_errors_24h > 10 {
        status = status.max(StorageHealthStatus::Critical);
    } else if obs.write_errors_24h > 0 {
        status = status.max(StorageHealthStatus::Degraded);
    }
    status
}

pub fn classify_thermal(temp_c: Option<f32>, t: &StorageHealthThresholds) -> ThermalStatus {
    match temp_c {
        Some(temp) if f64::from(temp) >= t.temp_hot_c => ThermalStatus::Hot,
        Some(temp) if f64::from(temp) >= t.temp_warn_c => ThermalStatus::Warm,
        _ => ThermalStatus::Ok,
    }
}

/// Hysteresis tracker: the published status only moves after
/// [`STATUS_CONFIRM_SAMPLES`] consecutive samples agree, and recovery
/// additionally requires clearing thresholds by a margin (applied by the
/// caller via [`StorageHealthThresholds::with_recovery_margin`]).
#[derive(Debug, Clone)]
pub struct StatusTracker {
    current: StorageHealthStatus,
    pending: Option<(StorageHealthStatus, u32)>,
}

impl Default for StatusTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl StatusTracker {
    pub fn new() -> Self {
        Self {
            current: StorageHealthStatus::Good,
            pending: None,
        }
    }

    pub fn current(&self) -> StorageHealthStatus {
        self.current
    }

    /// Feed one sample. `candidate` is classified with the configured
    /// thresholds; `recovery_candidate` with margin-shifted thresholds.
    /// Returns the (possibly updated) published status.
    pub fn observe(
        &mut self,
        candidate: StorageHealthStatus,
        recovery_candidate: StorageHealthStatus,
    ) -> StorageHealthStatus {
        let proposal = if candidate > self.current {
            Some(candidate)
        } else if recovery_candidate < self.current {
            Some(recovery_candidate)
        } else {
            None
        };
        match proposal {
            Some(next) => {
                let count = match self.pending {
                    Some((pending, count)) if pending == next => count + 1,
                    _ => 1,
                };
                if count >= STATUS_CONFIRM_SAMPLES {
                    self.current = next;
                    self.pending = None;
                } else {
                    self.pending = Some((next, count));
                }
            }
            None => self.pending = None,
        }
        self.current
    }
}

// -------------------- Pure metric helpers --------------------

/// Parse `/sys/block/<dev>/stat` contents and return sectors written
/// (field 7, 1-based) since boot.
pub fn parse_block_stat_sectors_written(contents: &str) -> Option<u64> {
    contents.split_whitespace().nth(6)?.parse().ok()
}

/// Fold a new raw byte counter reading into the persisted lifetime total.
/// The raw counter resets on reboot; a reading lower than the previous one
/// means the whole reading is new writes since boot.
pub fn accumulate_lifetime(lifetime: u64, last_raw: u64, current_raw: u64) -> u64 {
    if current_raw >= last_raw {
        lifetime.saturating_add(current_raw - last_raw)
    } else {
        lifetime.saturating_add(current_raw)
    }
}

pub fn wear_pct(lifetime_bytes: u64, endurance_tbw: f64) -> Option<f64> {
    if endurance_tbw <= 0.0 {
        return None;
    }
    Some(lifetime_bytes as f64 / (endurance_tbw * 1e12) * 100.0)
}

/// Write rate over the most recent 24h of samples. Requires at least two
/// samples spanning [`MIN_RATE_SPAN_S`].
pub fn write_rate_bytes_per_day(samples: &[HealthSample], now_s: u64) -> Option<u64> {
    let cutoff = now_s.saturating_sub(RATE_WINDOW_S);
    let window: Vec<&HealthSample> = samples.iter().filter(|s| s.epoch_s >= cutoff).collect();
    let (first, last) = match (window.first(), window.last()) {
        (Some(first), Some(last)) => (*first, *last),
        _ => return None,
    };
    let span = last.epoch_s.saturating_sub(first.epoch_s);
    if span < MIN_RATE_SPAN_S {
        return None;
    }
    let written = last.lifetime_bytes.saturating_sub(first.lifetime_bytes);
    Some(written.saturating_mul(RATE_WINDOW_S) / span)
}

pub fn estimated_days_remaining(
    lifetime_bytes: u64,
    endurance_tbw: f64,
    rate_bytes_per_day: Option<u64>,
) -> Option<u64> {
    let rate = rate_bytes_per_day?;
    if rate == 0 || endurance_tbw <= 0.0 {
        return None;
    }
    let budget = (endurance_tbw * 1e12) as u64;
    let remaining = budget.saturating_sub(lifetime_bytes);
    Some(remaining / rate)
}

/// Resolve a partition name to its parent block device by longest-prefix
/// match against `/sys/block` entries (e.g. `mmcblk0p2` -> `mmcblk0`,
/// `sda1` -> `sda`, `mmcblk0` -> `mmcblk0`).
pub fn resolve_parent_device(partition: &str, block_devices: &[String]) -> Option<String> {
    block_devices
        .iter()
        .filter(|dev| partition.starts_with(dev.as_str()))
        .max_by_key(|dev| dev.len())
        .cloned()
}

// -------------------- Persisted state --------------------

/// One persisted write-tracking sample.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct HealthSample {
    pub epoch_s: u64,
    pub lifetime_bytes: u64,
}

/// State persisted across restarts in a small JSON file next to the DB.
/// One ~300-byte write per check interval — negligible wear.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
struct HealthState {
    lifetime_bytes_written: u64,
    /// Last raw byte counter read from /sys (resets on reboot).
    last_raw_bytes: u64,
    samples: Vec<HealthSample>,
}

fn load_state(path: &Path) -> HealthState {
    match std::fs::read_to_string(path) {
        Ok(raw) => serde_json::from_str(&raw).unwrap_or_default(),
        Err(_) => HealthState::default(),
    }
}

fn store_state(path: &Path, state: &HealthState) -> Result<()> {
    let raw = serde_json::to_string(state)?;
    #[cfg(unix)]
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(path)?;
        f.write_all(raw.as_bytes())?;
    }
    #[cfg(not(unix))]
    std::fs::write(path, raw)?;
    Ok(())
}

// -------------------- Monitor --------------------

/// Settings the monitor needs; built from `config::StorageHealthSettings`.
#[derive(Debug, Clone)]
pub struct MonitorSettings {
    pub endurance_tbw: f64,
    /// Parent block device (e.g. `mmcblk0`); auto-detected when `None`.
    pub block_device: Option<String>,
    /// State file path; defaults to `<db_path>.health.json`.
    pub state_path: Option<PathBuf>,
    pub thresholds: StorageHealthThresholds,
}

pub struct StorageHealthMonitor {
    db_path: PathBuf,
    state_path: PathBuf,
    device: Option<String>,
    endurance_tbw: f64,
    thresholds: StorageHealthThresholds,
    write_errors: Arc<AtomicU64>,
    /// (epoch_s, error-count delta) per sample, kept for the 24h window.
    error_history: VecDeque<(u64, u64)>,
    last_error_total: u64,
    tracker: StatusTracker,
    state: HealthState,
    state_dirty_logged: bool,
}

impl StorageHealthMonitor {
    pub fn new(settings: MonitorSettings, db_path: &str) -> Self {
        let db_path = PathBuf::from(db_path);
        let state_path = settings
            .state_path
            .unwrap_or_else(|| PathBuf::from(format!("{}.health.json", db_path.display())));
        let device = settings
            .block_device
            .filter(|d| !d.trim().is_empty())
            .or_else(|| detect_block_device(&db_path));
        match &device {
            Some(dev) => log::info!(
                "storage health: tracking device '{}', endurance rating {} TBW",
                dev,
                settings.endurance_tbw
            ),
            None => log::info!(
                "storage health: no block device resolved; wear tracking disabled \
                 (set storage_health.block_device to enable)"
            ),
        }
        let state = load_state(&state_path);
        Self {
            db_path,
            state_path,
            device,
            endurance_tbw: settings.endurance_tbw,
            thresholds: settings.thresholds,
            write_errors: Arc::new(AtomicU64::new(0)),
            error_history: VecDeque::new(),
            last_error_total: 0,
            tracker: StatusTracker::new(),
            state,
            state_dirty_logged: false,
        }
    }

    /// Handle for recording sealed-log write failures from the main loop.
    pub fn error_counter(&self) -> Arc<AtomicU64> {
        self.write_errors.clone()
    }

    pub fn current_status(&self) -> StorageHealthStatus {
        self.tracker.current()
    }

    /// Take one health sample. Probe failures degrade to `None` fields; the
    /// report itself is always produced.
    pub fn sample(&mut self) -> Result<StorageHealthReport> {
        let now_s = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let space = read_fs_space(self.db_path.parent().unwrap_or(Path::new(".")));
        let (free_bytes, total_bytes) = match space {
            Some((free, total)) => (Some(free), Some(total)),
            None => (None, None),
        };
        let free_pct = match (free_bytes, total_bytes) {
            (Some(free), Some(total)) if total > 0 => Some(free as f64 / total as f64 * 100.0),
            _ => None,
        };

        // Whole-device write tracking, accumulated across reboots.
        let lifetime_bytes = match self.device.as_deref().and_then(read_device_bytes_written) {
            Some(raw) => {
                self.state.lifetime_bytes_written = accumulate_lifetime(
                    self.state.lifetime_bytes_written,
                    self.state.last_raw_bytes,
                    raw,
                );
                self.state.last_raw_bytes = raw;
                self.state.samples.push(HealthSample {
                    epoch_s: now_s,
                    lifetime_bytes: self.state.lifetime_bytes_written,
                });
                if self.state.samples.len() > MAX_SAMPLES {
                    let excess = self.state.samples.len() - MAX_SAMPLES;
                    self.state.samples.drain(..excess);
                }
                if let Err(err) = store_state(&self.state_path, &self.state) {
                    // A failed write on the DB's filesystem is itself an
                    // early storage-fault signal — count it.
                    self.write_errors.fetch_add(1, Ordering::Relaxed);
                    // Log once, not every interval: the state file failing is
                    // itself a storage symptom and shouldn't flood the log.
                    if !self.state_dirty_logged {
                        log::warn!(
                            "storage health: could not persist state to {}: {}",
                            self.state_path.display(),
                            err
                        );
                        self.state_dirty_logged = true;
                    }
                }
                Some(self.state.lifetime_bytes_written)
            }
            None => None,
        };

        let write_rate = write_rate_bytes_per_day(&self.state.samples, now_s);
        let wear = lifetime_bytes.and_then(|bytes| wear_pct(bytes, self.endurance_tbw));
        let days_remaining = lifetime_bytes
            .and_then(|bytes| estimated_days_remaining(bytes, self.endurance_tbw, write_rate));

        // Write-error window: record the delta since the previous sample and
        // sum deltas within the last 24h.
        let error_total = self.write_errors.load(Ordering::Relaxed);
        let delta = error_total.saturating_sub(self.last_error_total);
        self.last_error_total = error_total;
        if delta > 0 {
            self.error_history.push_back((now_s, delta));
        }
        let cutoff = now_s.saturating_sub(RATE_WINDOW_S);
        while matches!(self.error_history.front(), Some((t, _)) if *t < cutoff) {
            self.error_history.pop_front();
        }
        let errors_24h: u64 = self.error_history.iter().map(|(_, d)| d).sum();

        let obs = Observation {
            free_pct,
            wear_pct: wear,
            write_errors_24h: errors_24h,
        };
        let candidate = classify(&obs, &self.thresholds);
        let recovery = classify(&obs, &self.thresholds.with_recovery_margin());
        let status = self.tracker.observe(candidate, recovery);

        let soc_temp_c = read_soc_temp_c();
        let report = StorageHealthReport {
            storage: StorageMetrics {
                status,
                free_bytes,
                total_bytes,
                free_pct,
                db_bytes: file_size(&self.db_path),
                wal_bytes: file_size(&PathBuf::from(format!("{}-wal", self.db_path.display()))),
                write_errors: error_total,
                write_rate_bytes_per_day: write_rate,
                lifetime_bytes_written: lifetime_bytes,
                endurance_tbw: self.endurance_tbw,
                wear_pct: wear,
                estimated_days_remaining: days_remaining,
                source_device: self.device.clone(),
            },
            thermal: ThermalMetrics {
                soc_temp_c,
                status: classify_thermal(soc_temp_c, &self.thresholds),
            },
            time_bucket: TimeBucket::now_10min()?,
        };
        Ok(report)
    }
}

fn file_size(path: &Path) -> u64 {
    std::fs::metadata(path).map(|m| m.len()).unwrap_or(0)
}

// -------------------- Linux probes --------------------

#[cfg(target_os = "linux")]
// statvfs field widths differ across targets (u32 on 32-bit Pi OS), so the
// casts below are required even when they are no-ops on 64-bit hosts.
#[allow(clippy::unnecessary_cast)]
fn read_fs_space(dir: &Path) -> Option<(u64, u64)> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(dir.as_os_str().as_bytes()).ok()?;
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    // SAFETY: `c` is a valid NUL-terminated path and `st` is a zeroed
    // statvfs the kernel fills in on success.
    let rc = unsafe { libc::statvfs(c.as_ptr(), &mut st) };
    if rc != 0 {
        return None;
    }
    let frsize = if st.f_frsize > 0 {
        st.f_frsize
    } else {
        st.f_bsize
    } as u64;
    let total = (st.f_blocks as u64).saturating_mul(frsize);
    // f_bavail: space available to unprivileged users (matches `df`).
    let free = (st.f_bavail as u64).saturating_mul(frsize);
    Some((free, total))
}

#[cfg(not(target_os = "linux"))]
fn read_fs_space(_dir: &Path) -> Option<(u64, u64)> {
    None
}

#[cfg(target_os = "linux")]
fn read_device_bytes_written(device: &str) -> Option<u64> {
    let contents = std::fs::read_to_string(format!("/sys/block/{device}/stat")).ok()?;
    parse_block_stat_sectors_written(&contents).map(|sectors| sectors * SECTOR_BYTES)
}

#[cfg(not(target_os = "linux"))]
fn read_device_bytes_written(_device: &str) -> Option<u64> {
    None
}

#[cfg(target_os = "linux")]
fn read_soc_temp_c() -> Option<f32> {
    let raw = std::fs::read_to_string("/sys/class/thermal/thermal_zone0/temp").ok()?;
    let millis: i64 = raw.trim().parse().ok()?;
    Some(millis as f32 / 1000.0)
}

#[cfg(not(target_os = "linux"))]
fn read_soc_temp_c() -> Option<f32> {
    None
}

/// Find the block device backing `db_path`'s filesystem: longest mount-point
/// prefix in `/proc/self/mounts`, then resolve the partition to its parent
/// device via `/sys/block`.
#[cfg(target_os = "linux")]
fn detect_block_device(db_path: &Path) -> Option<String> {
    let dir = db_path.parent().unwrap_or(Path::new("."));
    let canon = dir.canonicalize().ok()?;
    let mounts = std::fs::read_to_string("/proc/self/mounts").ok()?;
    let mut best: Option<(usize, String)> = None;
    for line in mounts.lines() {
        let mut parts = line.split_whitespace();
        let (Some(dev), Some(mount)) = (parts.next(), parts.next()) else {
            continue;
        };
        let Some(dev_name) = dev.strip_prefix("/dev/") else {
            continue;
        };
        if canon.starts_with(mount) && best.as_ref().is_none_or(|(len, _)| mount.len() > *len) {
            best = Some((mount.len(), dev_name.to_string()));
        }
    }
    let (_, partition) = best?;
    let block_devices: Vec<String> = std::fs::read_dir("/sys/block")
        .ok()?
        .filter_map(|entry| Some(entry.ok()?.file_name().to_string_lossy().into_owned()))
        .collect();
    resolve_parent_device(&partition, &block_devices)
}

#[cfg(not(target_os = "linux"))]
fn detect_block_device(_db_path: &Path) -> Option<String> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn thresholds() -> StorageHealthThresholds {
        StorageHealthThresholds::default()
    }

    #[test]
    fn parses_sys_block_stat_sectors_written() {
        // Real format: 11+ space-padded fields; sectors written is field 7.
        let line = "  104151    78211  6743519    65820   274825   228734  6483784   183974        0   126632   261551";
        assert_eq!(parse_block_stat_sectors_written(line), Some(6_483_784));
        assert_eq!(parse_block_stat_sectors_written("1 2 3"), None);
        assert_eq!(parse_block_stat_sectors_written(""), None);
    }

    #[test]
    fn lifetime_accumulates_across_reboots() {
        // Normal advance: counter grows.
        assert_eq!(accumulate_lifetime(1000, 400, 600), 1200);
        // Reboot: raw counter reset; the whole reading is new writes.
        assert_eq!(accumulate_lifetime(1200, 600, 50), 1250);
        // No movement.
        assert_eq!(accumulate_lifetime(1250, 50, 50), 1250);
    }

    #[test]
    fn wear_pct_math() {
        // 32 TB written against a 64 TBW card = 50%.
        let wear = wear_pct(32_000_000_000_000, 64.0).unwrap();
        assert!((wear - 50.0).abs() < 1e-9);
        assert_eq!(wear_pct(1, 0.0), None);
    }

    #[test]
    fn rate_requires_minimum_span() {
        let now = 1_000_000;
        let samples = [
            HealthSample {
                epoch_s: now - 60,
                lifetime_bytes: 0,
            },
            HealthSample {
                epoch_s: now,
                lifetime_bytes: 1_000_000,
            },
        ];
        assert_eq!(write_rate_bytes_per_day(&samples, now), None);
    }

    #[test]
    fn rate_extrapolates_to_a_day() {
        let now = 1_000_000;
        let samples = [
            HealthSample {
                epoch_s: now - 43_200, // 12h ago
                lifetime_bytes: 0,
            },
            HealthSample {
                epoch_s: now,
                lifetime_bytes: 1_000_000,
            },
        ];
        assert_eq!(write_rate_bytes_per_day(&samples, now), Some(2_000_000));
    }

    #[test]
    fn rate_ignores_samples_outside_window() {
        let now = 1_000_000;
        let samples = [
            HealthSample {
                epoch_s: now - 200_000, // outside 24h window
                lifetime_bytes: 0,
            },
            HealthSample {
                epoch_s: now - 43_200,
                lifetime_bytes: 500_000,
            },
            HealthSample {
                epoch_s: now,
                lifetime_bytes: 1_500_000,
            },
        ];
        assert_eq!(write_rate_bytes_per_day(&samples, now), Some(2_000_000));
    }

    #[test]
    fn days_remaining_handles_zero_rate() {
        assert_eq!(estimated_days_remaining(0, 64.0, None), None);
        assert_eq!(estimated_days_remaining(0, 64.0, Some(0)), None);
        // 64 TBW, 1 TB/day -> 64 days remaining from new.
        assert_eq!(
            estimated_days_remaining(0, 64.0, Some(1_000_000_000_000)),
            Some(64)
        );
    }

    #[test]
    fn resolves_partition_to_parent_device() {
        let devices = vec![
            "mmcblk0".to_string(),
            "sda".to_string(),
            "nvme0n1".to_string(),
        ];
        assert_eq!(
            resolve_parent_device("mmcblk0p2", &devices),
            Some("mmcblk0".to_string())
        );
        assert_eq!(
            resolve_parent_device("mmcblk0", &devices),
            Some("mmcblk0".to_string())
        );
        assert_eq!(
            resolve_parent_device("sda1", &devices),
            Some("sda".to_string())
        );
        assert_eq!(
            resolve_parent_device("nvme0n1p1", &devices),
            Some("nvme0n1".to_string())
        );
        assert_eq!(resolve_parent_device("vda1", &devices), None);
    }

    #[test]
    fn classify_worst_of_dimensions() {
        let t = thresholds();
        let good = Observation {
            free_pct: Some(60.0),
            wear_pct: Some(10.0),
            write_errors_24h: 0,
        };
        assert_eq!(classify(&good, &t), StorageHealthStatus::Good);

        let low_space = Observation {
            free_pct: Some(10.0),
            ..good.clone()
        };
        assert_eq!(classify(&low_space, &t), StorageHealthStatus::Degraded);

        let no_space = Observation {
            free_pct: Some(2.0),
            ..good.clone()
        };
        assert_eq!(classify(&no_space, &t), StorageHealthStatus::Critical);

        let worn = Observation {
            wear_pct: Some(85.0),
            ..good.clone()
        };
        assert_eq!(
            classify(&worn, &t),
            StorageHealthStatus::ReplacementRecommended
        );

        let worn_out = Observation {
            wear_pct: Some(96.0),
            ..good.clone()
        };
        assert_eq!(classify(&worn_out, &t), StorageHealthStatus::Critical);

        let erroring = Observation {
            write_errors_24h: 3,
            ..good.clone()
        };
        assert_eq!(classify(&erroring, &t), StorageHealthStatus::Degraded);

        let failing = Observation {
            write_errors_24h: 11,
            ..good
        };
        assert_eq!(classify(&failing, &t), StorageHealthStatus::Critical);
    }

    #[test]
    fn classify_thermal_thresholds() {
        let t = thresholds();
        assert_eq!(classify_thermal(None, &t), ThermalStatus::Ok);
        assert_eq!(classify_thermal(Some(50.0), &t), ThermalStatus::Ok);
        assert_eq!(classify_thermal(Some(72.0), &t), ThermalStatus::Warm);
        assert_eq!(classify_thermal(Some(81.0), &t), ThermalStatus::Hot);
    }

    #[test]
    fn tracker_requires_consecutive_samples_to_worsen() {
        let mut tracker = StatusTracker::new();
        let degraded = StorageHealthStatus::Degraded;
        let good = StorageHealthStatus::Good;
        // Two bad samples: no transition yet.
        assert_eq!(tracker.observe(degraded, degraded), good);
        assert_eq!(tracker.observe(degraded, degraded), good);
        // Third consecutive: transition.
        assert_eq!(tracker.observe(degraded, degraded), degraded);
    }

    #[test]
    fn tracker_resets_on_interrupted_streak() {
        let mut tracker = StatusTracker::new();
        let degraded = StorageHealthStatus::Degraded;
        let good = StorageHealthStatus::Good;
        tracker.observe(degraded, degraded);
        tracker.observe(degraded, degraded);
        // Streak interrupted by a clean sample.
        assert_eq!(tracker.observe(good, good), good);
        tracker.observe(degraded, degraded);
        assert_eq!(tracker.observe(degraded, degraded), good);
        assert_eq!(tracker.observe(degraded, degraded), degraded);
    }

    #[test]
    fn tracker_recovery_requires_margin_and_confirmation() {
        let mut tracker = StatusTracker::new();
        let degraded = StorageHealthStatus::Degraded;
        let good = StorageHealthStatus::Good;
        for _ in 0..3 {
            tracker.observe(degraded, degraded);
        }
        assert_eq!(tracker.current(), degraded);
        // Candidate recovered but recovery classification (with margin) has
        // not: status holds.
        for _ in 0..5 {
            assert_eq!(tracker.observe(good, degraded), degraded);
        }
        // Cleared with margin for 3 consecutive samples: recovers.
        tracker.observe(good, good);
        tracker.observe(good, good);
        assert_eq!(tracker.observe(good, good), good);
    }

    #[test]
    fn state_round_trips_through_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("witness.db.health.json");
        let state = HealthState {
            lifetime_bytes_written: 123_456,
            last_raw_bytes: 789,
            samples: vec![HealthSample {
                epoch_s: 1700000000,
                lifetime_bytes: 123_456,
            }],
        };
        store_state(&path, &state).unwrap();
        let loaded = load_state(&path);
        assert_eq!(loaded.lifetime_bytes_written, 123_456);
        assert_eq!(loaded.last_raw_bytes, 789);
        assert_eq!(loaded.samples, state.samples);
        // Corrupt or missing files degrade to a fresh default.
        std::fs::write(&path, "not json").unwrap();
        assert_eq!(load_state(&path).lifetime_bytes_written, 0);
        assert_eq!(
            load_state(&dir.path().join("missing.json")).lifetime_bytes_written,
            0
        );
    }

    #[test]
    fn monitor_samples_without_device() {
        let dir = tempfile::tempdir().unwrap();
        let db_path = dir.path().join("witness.db");
        std::fs::write(&db_path, b"stub").unwrap();
        let mut monitor = StorageHealthMonitor::new(
            MonitorSettings {
                endurance_tbw: 64.0,
                // A device name that never exists keeps the probe inert and
                // the test hermetic across host environments.
                block_device: Some("securacv-test-nonexistent".to_string()),
                state_path: Some(dir.path().join("state.json")),
                thresholds: StorageHealthThresholds::default(),
            },
            db_path.to_str().unwrap(),
        );
        let report = monitor.sample().unwrap();
        assert_eq!(report.storage.db_bytes, 4);
        assert_eq!(report.storage.lifetime_bytes_written, None);
        assert_eq!(report.storage.wear_pct, None);
        assert_eq!(report.storage.write_errors, 0);

        // Write errors recorded through the shared counter show up.
        let counter = monitor.error_counter();
        counter.fetch_add(1, Ordering::Relaxed);
        let report = monitor.sample().unwrap();
        assert_eq!(report.storage.write_errors, 1);
    }
}
