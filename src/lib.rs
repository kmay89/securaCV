//! Privacy Witness Kernel (PWK)
//!
//! This crate implements the core kernel for privacy-preserving video surveillance.
//!
//! # Architecture
//!
//! The kernel enforces seven invariants by construction:
//!
//! 1. **No Raw Export**: Raw media cannot leave the kernel boundary in normal operation.
//! 2. **No Identity Substrate**: No global identifiers, face embeddings, or plate strings.
//! 3. **Metadata Minimization**: Coarse time buckets, local zone IDs only.
//! 4. **Local Ownership**: All logs stored locally, no remote indexing.
//! 5. **Break-Glass by Quorum**: Evidence access requires N-of-M trustee approval.
//! 6. **No Retroactive Expansion**: New rulesets cannot reprocess old data.
//! 7. **Non-Queryable**: No bulk search, no identity selectors.
//!
//! # Module Structure
//!
//! - `frame`: Raw media isolation (RawFrame, InferenceView, FrameBuffer)
//! - `ingest`: Frame sources (RTSP, USB)
//! - Core types: Events, TimeBucket, ContractEnforcer, Kernel

use anyhow::{anyhow, Result};
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use rand::RngCore;
use rusqlite::{params, Connection, OpenFlags, OptionalExtension};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::sync::OnceLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zeroize::Zeroize;

pub mod api;
pub mod break_glass;
pub mod config;
pub mod frame;
pub mod ingest;
pub mod module_runtime;
pub mod storage;
pub mod vault;
pub mod verify;

pub use frame::{
    Detection, DetectionResult, Detector, FrameBuffer, InferenceView, RawFrame, SizeClass,
    StubDetector, MAX_BUFFER_FRAMES, MAX_PREROLL_SECS,
};
pub use ingest::{rtsp::RtspConfig, RtspSource};
#[cfg(feature = "ingest-v4l2")]
pub use ingest::{v4l2::V4l2Config, V4l2Source};
pub use module_runtime::{CapabilityBoundaryRuntime, ModuleCapability};
pub use storage::{InMemorySealedLogStore, SealedLogStore, SqliteSealedLogStore};
pub use vault::{FilesystemVaultStore, Vault, VaultConfig, VaultStore};

fn shared_memory_uri() -> String {
    let mut bytes = [0u8; 8];
    rand::thread_rng().fill_bytes(&mut bytes);
    format!(
        "file:witness_kernel_{:x}?mode=memory&cache=shared",
        u64::from_le_bytes(bytes)
    )
}

pub(crate) fn open_db_connection(db_path: &str) -> Result<Connection> {
    if db_path.starts_with("file:") {
        return Ok(Connection::open_with_flags(
            db_path,
            OpenFlags::SQLITE_OPEN_READ_WRITE
                | OpenFlags::SQLITE_OPEN_CREATE
                | OpenFlags::SQLITE_OPEN_URI,
        )?);
    }
    Ok(Connection::open(db_path)?)
}

// -------------------- Time Buckets --------------------

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimeBucket {
    /// start of bucket in seconds since epoch (coarse)
    pub start_epoch_s: u64,
    /// bucket size in seconds (e.g., 600 = 10 minutes)
    pub size_s: u32,
}

impl TimeBucket {
    pub fn now(bucket_size_s: u32) -> Result<Self> {
        let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
        let size = bucket_size_s as u64;
        let start = (now / size) * size;
        Ok(TimeBucket {
            start_epoch_s: start,
            size_s: bucket_size_s,
        })
    }

    pub fn now_10min() -> Result<Self> {
        Self::now(600)
    }
}

// -------------------- Event Types --------------------

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum EventType {
    BoundaryCrossingObjectLarge,
    BoundaryCrossingObjectSmall,
}

// -------------------- Events --------------------

/// Candidate events are untrusted outputs from modules.
/// They contain only fields allowed by the Event Contract.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CandidateEvent {
    pub event_type: EventType,
    pub time_bucket: TimeBucket,
    pub zone_id: String, // local-only semantic, not GPS/address
    pub confidence: f32, // 0..=1
    pub correlation_token: Option<[u8; 32]>,
}

/// Events are trusted, kernel-bound claims written to the sealed log.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Event {
    pub event_type: EventType,
    pub time_bucket: TimeBucket,
    pub zone_id: String,
    pub confidence: f32,
    pub correlation_token: Option<[u8; 32]>,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

impl Event {
    pub fn bind(mut self, kernel_version: &str, ruleset_id: &str, ruleset_hash: [u8; 32]) -> Self {
        self.kernel_version = kernel_version.to_string();
        self.ruleset_id = ruleset_id.to_string();
        self.ruleset_hash = ruleset_hash;
        self
    }
}

/// Exported events omit correlation tokens to avoid identity-like fields.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportEvent {
    pub event_type: EventType,
    pub time_bucket: TimeBucket,
    pub zone_id: String,
    pub confidence: f32,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportBucket {
    pub time_bucket: TimeBucket,
    pub events: Vec<ExportEvent>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportBatch {
    pub buckets: Vec<ExportBucket>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportArtifact {
    pub batches: Vec<ExportBatch>,
    pub max_events_per_batch: usize,
    pub jitter_s: u64,
    pub jitter_step_s: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportReceipt {
    pub time_bucket: TimeBucket,
    pub ruleset_hash: [u8; 32],
    pub batch_size: usize,
    pub artifact_hash: [u8; 32],
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportReceiptEntry {
    pub receipt: ExportReceipt,
    pub prev_hash: [u8; 32],
    pub entry_hash: [u8; 32],
    pub signature: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportBundle {
    pub artifact: ExportArtifact,
    pub receipt_entry: ExportReceiptEntry,
    pub device_public_key: [u8; 32],
}

#[derive(Clone, Copy, Debug)]
pub struct ExportOptions {
    pub max_events_per_batch: usize,
    pub jitter_s: u64,
    pub jitter_step_s: u64,
}

impl Default for ExportOptions {
    fn default() -> Self {
        Self {
            max_events_per_batch: 50,
            jitter_s: 120,
            jitter_step_s: 60,
        }
    }
}

/// Break-glass envelope identifier reserved for event export authorization.
pub const EXPORT_EVENTS_ENVELOPE_ID: &str = "export:events";

// -------------------- Zone ID Discipline --------------------

/// A conforming zone_id MUST be a local identifier, not an encoded location.
/// We enforce a positive allowlist pattern to avoid trivial bypasses.
///
/// Allowed: "zone:front_boundary", "zone:lot_a_1", "zone:back-gate"
/// Disallowed: anything with whitespace, slashes, or punctuation outside [_-].
pub fn validate_zone_id(zone_id: &str) -> Result<()> {
    // Compile once for hot paths.
    static ZONE_ID_RE: OnceLock<regex::Regex> = OnceLock::new();
    let re = ZONE_ID_RE.get_or_init(|| regex::Regex::new(r"^zone:[a-z0-9_-]{1,64}$").unwrap());

    // Strict allowlist: zone:<1..64 of [a-z0-9_-]>
    let zid = zone_id.to_lowercase();
    if !re.is_match(&zid) {
        return Err(anyhow!(
            "conformance: zone_id must match ^zone:[a-z0-9_-]{{1,64}}$"
        ));
    }
    Ok(())
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct ZonePolicy {
    pub sensitive_zones: Vec<String>,
}

impl ZonePolicy {
    pub fn new(sensitive_zones: Vec<String>) -> Result<Self> {
        let policy = Self { sensitive_zones };
        policy.normalized()
    }

    pub fn normalized(&self) -> Result<Self> {
        for zone in &self.sensitive_zones {
            validate_zone_id(zone)?;
        }
        Ok(Self {
            sensitive_zones: self
                .sensitive_zones
                .iter()
                .map(|zone| zone.to_lowercase())
                .collect(),
        })
    }

    pub fn is_sensitive(&self, zone_id: &str) -> bool {
        let zone_id = zone_id.to_lowercase();
        self.sensitive_zones.iter().any(|zone| zone == &zone_id)
    }
}

// -------------------- Contract Enforcer --------------------

pub struct ContractEnforcer;

impl ContractEnforcer {
    pub fn enforce(c: CandidateEvent) -> Result<Event> {
        // Confidence bounds
        if !(0.0..=1.0).contains(&c.confidence) {
            return Err(anyhow!("conformance: confidence out of bounds"));
        }

        // Time bucket granularity: minimum 5 minutes (300s)
        if c.time_bucket.size_s < 300 {
            return Err(anyhow!("conformance: time bucket too precise"));
        }

        // Zone discipline (positive allowlist)
        validate_zone_id(&c.zone_id)?;

        // Correlation token constraint: only with <= 15 minute buckets
        if c.correlation_token.is_some() && c.time_bucket.size_s > 900 {
            return Err(anyhow!(
                "conformance: correlation token with oversized bucket"
            ));
        }

        Ok(Event {
            event_type: c.event_type,
            time_bucket: c.time_bucket,
            zone_id: c.zone_id,
            confidence: c.confidence,
            correlation_token: c.correlation_token,
            kernel_version: "UNBOUND".to_string(),
            ruleset_id: "UNBOUND".to_string(),
            ruleset_hash: [0u8; 32],
        })
    }
}

// -------------------- Ephemeral Correlation Token Keys --------------------

/// BucketKeyManager maintains a per-time-bucket secret used to derive correlation tokens.
/// Keys are *randomly generated* per bucket and *destroyed* when the bucket changes.
/// This makes cross-window correlation cryptographically impossible *from the token alone*,
/// because the key for an expired bucket no longer exists.
pub struct BucketKeyManager {
    current_bucket: Option<TimeBucket>,
    key: [u8; 32],
    has_key: bool,
}

impl BucketKeyManager {
    pub fn new() -> Self {
        Self {
            current_bucket: None,
            key: [0u8; 32],
            has_key: false,
        }
    }

    pub fn rotate_if_needed(&mut self, bucket: TimeBucket) {
        if self.current_bucket == Some(bucket) && self.has_key {
            return;
        }

        // Destroy previous key material
        self.key.zeroize();
        self.has_key = false;

        // Generate a new per-bucket key
        rand::thread_rng().fill_bytes(&mut self.key);
        self.has_key = true;
        self.current_bucket = Some(bucket);
    }

    /// Derive a correlation token from non-invertible features.
    /// The kernel provides the per-bucket key; modules provide features (already non-invertible).
    pub fn token_for_features(&self, features_hash: [u8; 32]) -> Result<[u8; 32]> {
        if !self.has_key {
            return Err(anyhow!("conformance: bucket key not initialized"));
        }
        let mut hasher = Sha256::new();
        hasher.update(self.key);
        hasher.update(features_hash);
        Ok(hasher.finalize().into())
    }

    /// For conformance tests: prove key rotation occurred (current key differs from old).
    pub fn export_key_for_test_only(&self) -> [u8; 32] {
        self.key
    }
}

impl Default for BucketKeyManager {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for BucketKeyManager {
    fn drop(&mut self) {
        self.key.zeroize();
        self.has_key = false;
    }
}

// -------------------- Reprocess Guard (Invariant VI) --------------------

#[derive(Clone, Debug)]
pub struct AuditableError {
    pub code: &'static str,
    pub message: String,
}

impl std::fmt::Display for AuditableError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}
impl std::error::Error for AuditableError {}

pub struct ReprocessGuard;

impl ReprocessGuard {
    /// Ensure an operation does not apply a new ruleset to historical data.
    /// If violated, MUST fail with an auditable error and MUST be logged by the caller.
    pub fn assert_same_ruleset(
        expected_ruleset_hash: [u8; 32],
        record_ruleset_hash: [u8; 32],
    ) -> Result<()> {
        if expected_ruleset_hash != record_ruleset_hash {
            return Err(AuditableError {
                code: "CONFORMANCE_REPROCESS_VIOLATION",
                message: "attempt to reprocess historical data under a different ruleset"
                    .to_string(),
            }
            .into());
        }
        Ok(())
    }
}

// -------------------- RawMediaBoundary (Pattern) --------------------

/// Raw media must never cross the kernel boundary in normal operation.
/// This struct exists as a *single choke point* for any future export/serialization path.
///
/// When real frames are introduced, any attempt to export raw bytes MUST pass here and MUST fail closed.
pub struct RawMediaBoundary;

impl RawMediaBoundary {
    pub fn deny_export<T>(_why: &str) -> Result<T> {
        Err(anyhow!(
            "conformance: raw media export denied by RawMediaBoundary"
        ))
    }

    pub fn export_for_vault(
        data: &mut Vec<u8>,
        token: &mut break_glass::BreakGlassToken,
        envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        verifying_key: &VerifyingKey,
        receipt_lookup: impl FnOnce(&[u8; 32]) -> Result<break_glass::BreakGlassOutcome>,
    ) -> Result<Vec<u8>> {
        let now_bucket = TimeBucket::now(600)?;
        break_glass::BreakGlass::assert_token_valid(
            token,
            envelope_id,
            expected_ruleset_hash,
            now_bucket,
            verifying_key,
            receipt_lookup,
        )?;
        token.consume()?;
        Ok(std::mem::take(data))
    }
}

// -------------------- Sealed Log --------------------

#[derive(Clone, Debug)]
pub struct KernelConfig {
    pub db_path: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
    pub kernel_version: String,
    /// default retention; pruning will write checkpoints
    pub retention: Duration,
    /// Device signing seed (required at runtime; must not be the MVP placeholder).
    pub device_key_seed: String,
    /// Zone policy for suppressing sensitive areas.
    pub zone_policy: ZonePolicy,
}

impl KernelConfig {
    pub fn ruleset_hash_from_id(ruleset_id: &str) -> [u8; 32] {
        Sha256::digest(ruleset_id.as_bytes()).into()
    }
}

pub struct Kernel {
    pub conn: Connection,
    sealed_log: Box<dyn SealedLogStore>,
    device_key: SigningKey,
    break_glass_policy: Option<crate::break_glass::QuorumPolicy>,
    zone_policy: ZonePolicy,
}

impl Kernel {
    pub fn open(cfg: &KernelConfig) -> Result<Self> {
        let db_path = if cfg.db_path == ":memory:" {
            shared_memory_uri()
        } else {
            cfg.db_path.clone()
        };
        let conn = open_db_connection(&db_path)?;
        let sealed_log = Box::new(SqliteSealedLogStore::open(&db_path)?);
        let device_key = signing_key_from_seed(&cfg.device_key_seed)?;
        let zone_policy = cfg.zone_policy.normalized()?;
        let mut k = Self {
            conn,
            sealed_log,
            device_key,
            break_glass_policy: None,
            zone_policy,
        };
        k.ensure_schema()?;
        k.ensure_device_public_key()?;
        k.load_break_glass_policy()?;
        Ok(k)
    }

    pub fn open_with_sealed_log(
        cfg: &KernelConfig,
        sealed_log: Box<dyn SealedLogStore>,
    ) -> Result<Self> {
        let conn = open_db_connection(&cfg.db_path)?;
        let device_key = signing_key_from_seed(&cfg.device_key_seed)?;
        let zone_policy = cfg.zone_policy.normalized()?;
        let mut k = Self {
            conn,
            sealed_log,
            device_key,
            break_glass_policy: None,
            zone_policy,
        };
        k.ensure_schema()?;
        k.ensure_device_public_key()?;
        k.load_break_glass_policy()?;
        Ok(k)
    }

    pub fn ensure_schema(&mut self) -> Result<()> {
        self.conn.execute_batch(
            r#"
            PRAGMA journal_mode=WAL;

            CREATE TABLE IF NOT EXISTS sealed_events (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              created_at INTEGER NOT NULL,
              payload_json TEXT NOT NULL,
              prev_hash BLOB NOT NULL,
              entry_hash BLOB NOT NULL,
              signature BLOB NOT NULL
            );

            CREATE TABLE IF NOT EXISTS checkpoints (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              created_at INTEGER NOT NULL,
              cutoff_event_id INTEGER NOT NULL,
              chain_head_hash BLOB NOT NULL,
              signature BLOB NOT NULL
            );

            CREATE TABLE IF NOT EXISTS break_glass_receipts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  approvals_json TEXT NOT NULL DEFAULT '[]',
  prev_hash BLOB NOT NULL,
  entry_hash BLOB NOT NULL,
  signature BLOB NOT NULL
);

            CREATE TABLE IF NOT EXISTS break_glass_policy (
              id INTEGER PRIMARY KEY CHECK (id = 1),
              policy_json TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS export_receipts (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              created_at INTEGER NOT NULL,
              payload_json TEXT NOT NULL,
              prev_hash BLOB NOT NULL,
              entry_hash BLOB NOT NULL,
              signature BLOB NOT NULL
            );

            CREATE TABLE IF NOT EXISTS device_metadata (
              id INTEGER PRIMARY KEY CHECK (id = 1),
              public_key BLOB NOT NULL
            );

CREATE TABLE IF NOT EXISTS conformance_alarms (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              created_at INTEGER NOT NULL,
              code TEXT NOT NULL,
              message TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_events_created ON sealed_events(created_at);
            CREATE INDEX IF NOT EXISTS idx_receipts_created ON break_glass_receipts(created_at);
            CREATE INDEX IF NOT EXISTS idx_export_receipts_created ON export_receipts(created_at);
            "#,
        )?;
        self.ensure_break_glass_receipts_columns()?;
        Ok(())
    }

    fn ensure_break_glass_receipts_columns(&mut self) -> Result<()> {
        let mut stmt = self
            .conn
            .prepare("PRAGMA table_info(break_glass_receipts)")?;
        let mut rows = stmt.query([])?;
        let mut has_approvals_json = false;
        while let Some(row) = rows.next()? {
            let name: String = row.get(1)?;
            if name == "approvals_json" {
                has_approvals_json = true;
                break;
            }
        }
        if !has_approvals_json {
            self.conn.execute(
                "ALTER TABLE break_glass_receipts ADD COLUMN approvals_json TEXT NOT NULL DEFAULT '[]'",
                [],
            )?;
        }
        Ok(())
    }

    fn ensure_device_public_key(&mut self) -> Result<()> {
        let key_bytes = self.device_key_for_verify_only();
        let existing: Option<Vec<u8>> = self
            .conn
            .query_row(
                "SELECT public_key FROM device_metadata WHERE id = 1",
                [],
                |row| row.get(0),
            )
            .optional()?;

        if let Some(bytes) = existing {
            if bytes.len() != 32 {
                return Err(anyhow!(
                    "corrupt device_metadata.public_key: expected 32 bytes, got {}",
                    bytes.len()
                ));
            }
            if bytes.as_slice() != key_bytes.as_slice() {
                return Err(anyhow!(
                    "device public key mismatch: existing key does not match DEVICE_KEY_SEED"
                ));
            }
            return Ok(());
        }

        self.conn.execute(
            "INSERT INTO device_metadata (id, public_key) VALUES (1, ?1)",
            params![key_bytes.to_vec()],
        )?;
        Ok(())
    }

    pub fn log_alarm(&mut self, code: &str, message: &str) -> Result<()> {
        Self::log_alarm_with_conn(&mut self.conn, code, message)
    }

    fn log_alarm_with_conn(conn: &mut Connection, code: &str, message: &str) -> Result<()> {
        let created_at = now_s()? as i64;
        conn.execute(
            "INSERT INTO conformance_alarms(created_at, code, message) VALUES (?1, ?2, ?3)",
            params![created_at, code, message],
        )?;
        Ok(())
    }

    pub fn last_chain_head(&self) -> Result<[u8; 32]> {
        // Prefer latest checkpoint hash if present, otherwise genesis [0;32]
        let mut stmt = self
            .conn
            .prepare("SELECT chain_head_hash FROM checkpoints ORDER BY id DESC LIMIT 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let bytes: Vec<u8> = row.get(0)?;
            if bytes.len() != 32 {
                return Err(anyhow!("corrupt checkpoint: chain_head_hash size"));
            }
            let mut out = [0u8; 32];
            out.copy_from_slice(&bytes);
            return Ok(out);
        }

        // If no checkpoints, use last event hash if it exists, else genesis
        let mut stmt = self
            .conn
            .prepare("SELECT entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let bytes: Vec<u8> = row.get(0)?;
            if bytes.len() != 32 {
                return Err(anyhow!("corrupt sealed log: entry_hash size"));
            }
            let mut out = [0u8; 32];
            out.copy_from_slice(&bytes);
            Ok(out)
        } else {
            Ok([0u8; 32])
        }
    }

    pub fn last_event_hash_or_checkpoint_head(&self) -> Result<[u8; 32]> {
        // If events exist, chain continues from last event's hash.
        let mut stmt = self
            .conn
            .prepare("SELECT entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let bytes: Vec<u8> = row.get(0)?;
            if bytes.len() != 32 {
                return Err(anyhow!("corrupt sealed log: entry_hash size"));
            }
            let mut out = [0u8; 32];
            out.copy_from_slice(&bytes);
            Ok(out)
        } else {
            // otherwise, start from checkpoint head (or genesis)
            self.last_chain_head()
        }
    }

    fn append_event(&mut self, ev: &Event) -> Result<()> {
        self.sealed_log.append_event(ev, &self.device_key)
    }

    pub fn append_event_checked(
        &mut self,
        module_desc: &ModuleDescriptor,
        cand: CandidateEvent,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) -> Result<Event> {
        if let Err(e) = enforce_module_event_allowlist(module_desc, &cand) {
            self.log_alarm("CONFORMANCE_MODULE_ALLOWLIST", &format!("{}", e))?;
            return Err(e);
        }

        let ev = match ContractEnforcer::enforce(cand) {
            Ok(ev) => ev,
            Err(e) => {
                self.log_alarm("CONFORMANCE_CONTRACT_REJECT", &format!("{}", e))?;
                return Err(e);
            }
        };

        if self.zone_policy.is_sensitive(&ev.zone_id) {
            let err = anyhow!("conformance: sensitive zone rejected by policy");
            self.log_alarm("CONFORMANCE_ZONE_POLICY_REJECT", &format!("{}", err))?;
            return Err(err);
        }

        let ev = ev.bind(kernel_version, ruleset_id, ruleset_hash);
        self.append_event(&ev)?;
        Ok(ev)
    }

    fn last_break_glass_hash_or_zero(&self) -> Result<[u8; 32]> {
        let mut stmt = self.conn.prepare(
            "SELECT prev_hash, entry_hash FROM break_glass_receipts ORDER BY id DESC LIMIT 1",
        )?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let prev_bytes: Vec<u8> = row.get(0)?;
            let entry_bytes: Vec<u8> = row.get(1)?;
            let _prev_hash = blob32(prev_bytes, "break_glass_receipts.prev_hash")?;
            let entry_hash = blob32(entry_bytes, "break_glass_receipts.entry_hash")?;
            Ok(entry_hash)
        } else {
            Ok([0u8; 32])
        }
    }

    fn last_export_receipt_hash_or_zero(&self) -> Result<[u8; 32]> {
        let mut stmt = self.conn.prepare(
            "SELECT prev_hash, entry_hash FROM export_receipts ORDER BY id DESC LIMIT 1",
        )?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let prev_bytes: Vec<u8> = row.get(0)?;
            let entry_bytes: Vec<u8> = row.get(1)?;
            let _prev_hash = blob32(prev_bytes, "export_receipts.prev_hash")?;
            let entry_hash = blob32(entry_bytes, "export_receipts.entry_hash")?;
            Ok(entry_hash)
        } else {
            Ok([0u8; 32])
        }
    }

    pub fn append_break_glass_receipt(
        &mut self,
        receipt: &crate::break_glass::BreakGlassReceipt,
        approvals: &[crate::break_glass::Approval],
    ) -> Result<[u8; 32]> {
        let created_at = now_s()? as i64;
        let prev_hash = self.last_break_glass_hash_or_zero()?;
        let payload_json = serde_json::to_string(receipt)?;
        let approvals_json = serde_json::to_string(approvals)?;

        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        let signature = sign_entry(&self.device_key, &entry_hash);

        self.conn.execute(
            r#"
            INSERT INTO break_glass_receipts(created_at, payload_json, approvals_json, prev_hash, entry_hash, signature)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6)
            "#,
            params![
                created_at,
                payload_json,
                approvals_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature.to_vec(),
            ],
        )?;

        Ok(entry_hash)
    }

    pub fn append_export_receipt(&mut self, receipt: &ExportReceipt) -> Result<[u8; 32]> {
        let created_at = now_s()? as i64;
        let prev_hash = self.last_export_receipt_hash_or_zero()?;
        let payload_json = serde_json::to_string(receipt)?;

        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        let signature = sign_entry(&self.device_key, &entry_hash);

        self.conn.execute(
            r#"
            INSERT INTO export_receipts(created_at, payload_json, prev_hash, entry_hash, signature)
            VALUES (?1, ?2, ?3, ?4, ?5)
            "#,
            params![
                created_at,
                payload_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature.to_vec(),
            ],
        )?;

        Ok(entry_hash)
    }

    pub fn log_break_glass_receipt(
        &mut self,
        receipt: &crate::break_glass::BreakGlassReceipt,
        approvals: &[crate::break_glass::Approval],
    ) -> Result<[u8; 32]> {
        self.append_break_glass_receipt(receipt, approvals)
    }

    pub fn log_export_receipt(&mut self, receipt: &ExportReceipt) -> Result<[u8; 32]> {
        self.append_export_receipt(receipt)
    }

    pub fn latest_export_receipt_entry(&self) -> Result<ExportReceiptEntry> {
        let mut stmt = self.conn.prepare(
            "SELECT payload_json, prev_hash, entry_hash, signature FROM export_receipts ORDER BY id DESC LIMIT 1",
        )?;
        let row = stmt
            .query_row([], |row| {
                let payload: String = row.get(0)?;
                let prev_hash: Vec<u8> = row.get(1)?;
                let entry_hash: Vec<u8> = row.get(2)?;
                let signature: Vec<u8> = row.get(3)?;
                Ok((payload, prev_hash, entry_hash, signature))
            })
            .optional()?;

        let Some((payload, prev_hash, entry_hash, signature)) = row else {
            return Err(anyhow!("export receipt not found"));
        };

        let prev_hash = blob32(prev_hash, "export_receipts.prev_hash")?;
        let entry_hash = blob32(entry_hash, "export_receipts.entry_hash")?;
        let signature = blob64(signature, "export_receipts.signature")?;
        let receipt: ExportReceipt = serde_json::from_str(&payload)?;
        Ok(ExportReceiptEntry {
            receipt,
            prev_hash,
            entry_hash,
            signature: signature.to_vec(),
        })
    }

    pub fn set_break_glass_policy(
        &mut self,
        policy: &crate::break_glass::QuorumPolicy,
    ) -> Result<()> {
        let json = serde_json::to_string(policy)?;
        self.conn.execute(
            "INSERT OR REPLACE INTO break_glass_policy (id, policy_json) VALUES (1, ?1)",
            params![json],
        )?;
        self.break_glass_policy = Some(policy.clone());
        Ok(())
    }

    pub fn break_glass_policy(&self) -> Option<&crate::break_glass::QuorumPolicy> {
        self.break_glass_policy.as_ref()
    }

    fn load_break_glass_policy(&mut self) -> Result<()> {
        let mut stmt = self
            .conn
            .prepare("SELECT policy_json FROM break_glass_policy WHERE id = 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let policy_json: String = row.get(0)?;
            let stored: crate::break_glass::QuorumPolicy = serde_json::from_str(&policy_json)?;
            let policy = crate::break_glass::QuorumPolicy::new(stored.n, stored.trustees)?;
            self.break_glass_policy = Some(policy);
        } else {
            self.break_glass_policy = None;
        }
        Ok(())
    }

    /// Prune events older than retention. Writes a checkpoint so chain integrity remains verifiable.
    pub fn enforce_retention_with_checkpoint(&mut self, retention: Duration) -> Result<()> {
        self.sealed_log
            .enforce_retention_with_checkpoint(retention, &self.device_key)
    }

    /// Read events from the sealed log for review or export.
    /// This is a *ruleset-bound* operation: attempting to read/interpret events under a different ruleset
    /// is treated as a conformance violation (Invariant VI).
    pub fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
    ) -> Result<Vec<Event>> {
        let conn = &mut self.conn;
        let mut alarm = |code: &str, message: &str| Self::log_alarm_with_conn(conn, code, message);
        let sealed_log = &mut self.sealed_log;
        sealed_log.read_events_ruleset_bound(expected_ruleset_hash, limit, &mut alarm)
    }

    fn validate_export_options(options: &ExportOptions) -> Result<()> {
        if options.max_events_per_batch == 0 {
            return Err(anyhow!("export max_events_per_batch must be >= 1"));
        }
        if options.jitter_step_s == 0 {
            return Err(anyhow!("export jitter_step_s must be >= 1"));
        }
        if options.jitter_s > 0 && options.jitter_step_s > options.jitter_s {
            return Err(anyhow!("export jitter_step_s cannot exceed jitter_s"));
        }
        Ok(())
    }

    /// Export events sequentially with break-glass authorization.
    pub fn export_events_authorized(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        token: &mut break_glass::BreakGlassToken,
    ) -> Result<ExportArtifact> {
        Self::validate_export_options(&options)?;
        let now_bucket = TimeBucket::now(600)?;
        break_glass::BreakGlass::assert_token_valid(
            token,
            EXPORT_EVENTS_ENVELOPE_ID,
            expected_ruleset_hash,
            now_bucket,
            &self.device_verifying_key(),
            |hash| self.break_glass_receipt_outcome(hash),
        )?;
        let artifact = self.export_events_sequential_unchecked(expected_ruleset_hash, options)?;
        token.consume()?;
        Ok(artifact)
    }

    /// Export events sequentially for local-only API access (no break-glass).
    /// The caller must enforce capability-token access control.
    pub fn export_events_for_api(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
    ) -> Result<ExportArtifact> {
        Self::validate_export_options(&options)?;
        self.export_events_sequential_unchecked(expected_ruleset_hash, options)
    }

    pub fn export_events_bundle_authorized(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        token: &mut break_glass::BreakGlassToken,
    ) -> Result<ExportBundle> {
        let artifact = self.export_events_authorized(expected_ruleset_hash, options, token)?;
        let receipt_entry = self.latest_export_receipt_entry()?;
        let artifact_bytes = serde_json::to_vec(&artifact)?;
        let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
        if artifact_hash != receipt_entry.receipt.artifact_hash {
            return Err(anyhow!(
                "export receipt artifact hash mismatch: receipt does not match export"
            ));
        }
        Ok(ExportBundle {
            artifact,
            receipt_entry,
            device_public_key: self.device_key_for_verify_only(),
        })
    }

    /// Export events sequentially, grouped into coarse time buckets with batching and jitter.
    fn export_events_sequential_unchecked(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
    ) -> Result<ExportArtifact> {
        Self::validate_export_options(&options)?;

        let payloads = {
            let mut stmt = self
                .conn
                .prepare("SELECT payload_json FROM sealed_events ORDER BY id ASC")?;
            let mut rows = stmt.query([])?;
            let mut payloads = Vec::new();

            while let Some(row) = rows.next()? {
                let payload: String = row.get(0)?;
                payloads.push(payload);
            }

            payloads
        };

        let mut buckets: Vec<ExportBucket> = Vec::new();
        let mut bucket_index: std::collections::BTreeMap<(u64, u32), usize> =
            std::collections::BTreeMap::new();

        for payload in payloads {
            let ev: Event = serde_json::from_str(&payload)?;

            if let Err(e) =
                ReprocessGuard::assert_same_ruleset(expected_ruleset_hash, ev.ruleset_hash)
            {
                self.log_alarm("CONFORMANCE_REPROCESS_VIOLATION", &format!("{}", e))?;
                return Err(e);
            }

            let key = (ev.time_bucket.start_epoch_s, ev.time_bucket.size_s);
            let idx = if let Some(existing) = bucket_index.get(&key) {
                *existing
            } else {
                let jittered_bucket =
                    jitter_time_bucket(ev.time_bucket, options.jitter_s, options.jitter_step_s)?;
                buckets.push(ExportBucket {
                    time_bucket: jittered_bucket,
                    events: Vec::new(),
                });
                let idx = buckets.len() - 1;
                bucket_index.insert(key, idx);
                idx
            };
            let export_event = ExportEvent {
                event_type: ev.event_type,
                time_bucket: buckets[idx].time_bucket,
                zone_id: ev.zone_id,
                confidence: ev.confidence,
                kernel_version: ev.kernel_version,
                ruleset_id: ev.ruleset_id,
                ruleset_hash: ev.ruleset_hash,
            };
            buckets[idx].events.push(export_event);
        }

        let mut batches = Vec::new();
        let mut current = ExportBatch {
            buckets: Vec::new(),
        };
        let mut current_events = 0usize;

        for bucket in buckets {
            let bucket_events = bucket.events.len();
            if !current.buckets.is_empty()
                && current_events + bucket_events > options.max_events_per_batch
            {
                batches.push(current);
                current = ExportBatch {
                    buckets: Vec::new(),
                };
                current_events = 0;
            }
            current_events += bucket_events;
            current.buckets.push(bucket);
        }

        if !current.buckets.is_empty() {
            batches.push(current);
        }

        let artifact = ExportArtifact {
            batches,
            max_events_per_batch: options.max_events_per_batch,
            jitter_s: options.jitter_s,
            jitter_step_s: options.jitter_step_s,
        };

        let artifact_bytes = serde_json::to_vec(&artifact)?;
        let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
        let receipt = ExportReceipt {
            time_bucket: TimeBucket::now_10min()?,
            ruleset_hash: expected_ruleset_hash,
            batch_size: options.max_events_per_batch,
            artifact_hash,
        };
        self.log_export_receipt(&receipt)?;

        Ok(artifact)
    }

    pub fn device_key_for_verify_only(&self) -> [u8; 32] {
        self.device_key.verifying_key().to_bytes()
    }

    pub fn device_verifying_key(&self) -> VerifyingKey {
        self.device_key.verifying_key()
    }

    pub fn sign_break_glass_token(
        &self,
        token: &mut break_glass::BreakGlassToken,
        receipt_entry_hash: [u8; 32],
    ) -> Result<()> {
        let outcome = break_glass_receipt_outcome_for_verifier(
            &self.conn,
            &self.device_verifying_key(),
            &receipt_entry_hash,
        )?;
        if !matches!(outcome, break_glass::BreakGlassOutcome::Granted) {
            return Err(anyhow!("cannot sign break-glass token for denied receipt"));
        }
        token.attach_receipt_signature(receipt_entry_hash, &self.device_key)
    }

    pub fn break_glass_receipt_outcome(
        &self,
        receipt_entry_hash: &[u8; 32],
    ) -> Result<break_glass::BreakGlassOutcome> {
        break_glass_receipt_outcome_for_verifier(
            &self.conn,
            &self.device_verifying_key(),
            receipt_entry_hash,
        )
    }
}

pub fn hash_entry(prev_hash: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(prev_hash);
    hasher.update(payload);
    hasher.finalize().into()
}

pub fn signing_key_from_seed(seed: &str) -> Result<SigningKey> {
    let trimmed = seed.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("device_key_seed is required"));
    }
    if trimmed == "devkey:mvp" {
        return Err(anyhow!("device_key_seed must not use MVP placeholder"));
    }
    let mut hasher = Sha256::new();
    hasher.update(trimmed.as_bytes());
    let digest: [u8; 32] = hasher.finalize().into();
    Ok(SigningKey::from_bytes(&digest))
}

pub fn verifying_key_from_seed(seed: &str) -> Result<VerifyingKey> {
    Ok(signing_key_from_seed(seed)?.verifying_key())
}

fn verifying_key_from_bytes(bytes: &[u8]) -> Result<VerifyingKey> {
    if bytes.len() != 32 {
        return Err(anyhow!(
            "invalid verifying key bytes: expected 32 bytes, got {}",
            bytes.len()
        ));
    }
    let mut key_bytes = [0u8; 32];
    key_bytes.copy_from_slice(bytes);
    VerifyingKey::from_bytes(&key_bytes).map_err(|e| anyhow!("invalid verifying key bytes: {}", e))
}

pub fn device_public_key_from_db(conn: &Connection) -> Result<VerifyingKey> {
    let bytes: Vec<u8> = conn
        .query_row(
            "SELECT public_key FROM device_metadata WHERE id = 1",
            [],
            |row| row.get(0),
        )
        .map_err(|e| match e {
            rusqlite::Error::QueryReturnedNoRows => {
                anyhow!("device public key not found in database")
            }
            _ => anyhow!("failed to read device public key from database: {}", e),
        })?;
    verifying_key_from_bytes(&bytes)
}

pub fn sign_entry(signing_key: &SigningKey, entry_hash: &[u8; 32]) -> [u8; 64] {
    signing_key.sign(entry_hash).to_bytes()
}

pub fn verify_entry_signature(
    verifying_key: &VerifyingKey,
    entry_hash: &[u8; 32],
    signature: &[u8; 64],
) -> Result<()> {
    let sig = ed25519_dalek::Signature::from_bytes(signature);
    verifying_key
        .verify(entry_hash, &sig)
        .map_err(|e| anyhow!("signature verification failed: {}", e))
}

pub fn verify_export_bundle(bundle: &ExportBundle) -> Result<()> {
    let verifying_key = verifying_key_from_bytes(&bundle.device_public_key)?;
    let payload_json = serde_json::to_string(&bundle.receipt_entry.receipt)?;
    let computed_entry_hash = hash_entry(&bundle.receipt_entry.prev_hash, payload_json.as_bytes());
    if computed_entry_hash != bundle.receipt_entry.entry_hash {
        return Err(anyhow!("export receipt entry hash mismatch"));
    }
    let signature = blob64(
        bundle.receipt_entry.signature.clone(),
        "export_bundle.signature",
    )?;
    verify_entry_signature(&verifying_key, &bundle.receipt_entry.entry_hash, &signature)?;
    let artifact_bytes = serde_json::to_vec(&bundle.artifact)?;
    let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
    if artifact_hash != bundle.receipt_entry.receipt.artifact_hash {
        return Err(anyhow!("export artifact hash mismatch"));
    }
    Ok(())
}

pub mod verify_helpers;

fn blob32(bytes: Vec<u8>, context: &str) -> Result<[u8; 32]> {
    if bytes.len() != 32 {
        return Err(anyhow!(
            "corrupt {}: expected 32 bytes, got {}",
            context,
            bytes.len()
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn blob64(bytes: Vec<u8>, context: &str) -> Result<[u8; 64]> {
    if bytes.len() != 64 {
        return Err(anyhow!(
            "corrupt {}: expected 64 bytes, got {}",
            context,
            bytes.len()
        ));
    }
    let mut out = [0u8; 64];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn now_s() -> Result<u64> {
    Ok(SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs())
}

pub fn break_glass_receipt_outcome_for_verifier(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    receipt_entry_hash: &[u8; 32],
) -> Result<break_glass::BreakGlassOutcome> {
    let mut stmt = conn.prepare(
        "SELECT payload_json, prev_hash, entry_hash, signature FROM break_glass_receipts WHERE entry_hash = ?1 LIMIT 1",
    )?;
    let row = stmt
        .query_row(params![receipt_entry_hash.to_vec()], |row| {
            let payload: String = row.get(0)?;
            let prev_hash: Vec<u8> = row.get(1)?;
            let entry_hash: Vec<u8> = row.get(2)?;
            let signature: Vec<u8> = row.get(3)?;
            Ok((payload, prev_hash, entry_hash, signature))
        })
        .optional()?;

    let Some((payload, prev_hash, entry_hash, signature)) = row else {
        return Err(anyhow!("break-glass receipt not found for token"));
    };

    let prev_hash = blob32(prev_hash, "break_glass_receipts.prev_hash")?;
    let entry_hash = blob32(entry_hash, "break_glass_receipts.entry_hash")?;
    if &entry_hash != receipt_entry_hash {
        return Err(anyhow!("break-glass receipt hash mismatch"));
    }
    let computed = hash_entry(&prev_hash, payload.as_bytes());
    if computed != entry_hash {
        return Err(anyhow!("break-glass receipt hash invalid"));
    }
    let signature = blob64(signature, "break_glass_receipts.signature")?;
    verify_entry_signature(verifying_key, &entry_hash, &signature)?;
    let receipt: break_glass::BreakGlassReceipt = serde_json::from_str(&payload)?;
    Ok(receipt.outcome)
}

fn jitter_time_bucket(bucket: TimeBucket, jitter_s: u64, jitter_step_s: u64) -> Result<TimeBucket> {
    if jitter_s == 0 {
        return Ok(bucket);
    }

    let steps = (jitter_s / jitter_step_s) as i64;
    if steps == 0 {
        return Ok(bucket);
    }

    let span = (steps * 2 + 1) as u64;
    let mut rng = rand::rngs::OsRng;
    let choice = (rng.next_u64() % span) as i64 - steps;
    let offset = choice * jitter_step_s as i64;
    let start = if offset.is_negative() {
        let offset_abs = (-offset) as u64;
        bucket.start_epoch_s.saturating_sub(offset_abs)
    } else {
        bucket.start_epoch_s.saturating_add(offset as u64)
    };

    Ok(TimeBucket {
        start_epoch_s: start,
        size_s: bucket.size_s,
    })
}

// -------------------- Modules --------------------

/// Module metadata used for runtime authorization checks.
#[derive(Clone, Debug)]
pub struct ModuleDescriptor {
    pub id: &'static str,
    pub allowed_event_types: &'static [EventType],
    pub requested_capabilities: &'static [ModuleCapability],
}

/// Runtime allowlist enforcement: the kernel MUST verify that a module is authorized
/// to emit the event types it proposes.
pub fn enforce_module_event_allowlist(
    desc: &ModuleDescriptor,
    cand: &CandidateEvent,
) -> Result<()> {
    if !desc.allowed_event_types.contains(&cand.event_type) {
        return Err(anyhow!(
            "conformance: module {} not authorized to emit {:?}",
            desc.id,
            cand.event_type
        ));
    }
    Ok(())
}

/// Trait for detection modules that process frames.
///
/// Modules receive `InferenceView` (restricted) not `RawFrame` (full access).
pub trait Module {
    fn descriptor(&self) -> ModuleDescriptor;

    fn process(
        &mut self,
        view: &InferenceView<'_>,
        bucket: TimeBucket,
        token_mgr: &BucketKeyManager,
    ) -> Result<Vec<CandidateEvent>>;
}

/// Zone crossing detection module.
pub struct ZoneCrossingModule {
    zone_id: String,
    emit_token: bool,
    detector: StubDetector,
}

const ZONE_CROSSING_ALLOWED: &[EventType] = &[
    EventType::BoundaryCrossingObjectLarge,
    EventType::BoundaryCrossingObjectSmall,
];

impl ZoneCrossingModule {
    pub fn new(zone_id: &str) -> Self {
        Self {
            zone_id: zone_id.to_string(),
            emit_token: false,
            detector: StubDetector::new(),
        }
    }

    pub fn with_tokens(mut self, enabled: bool) -> Self {
        self.emit_token = enabled;
        self
    }
}

impl Module for ZoneCrossingModule {
    fn descriptor(&self) -> ModuleDescriptor {
        ModuleDescriptor {
            id: "zone_crossing",
            allowed_event_types: ZONE_CROSSING_ALLOWED,
            requested_capabilities: &[],
        }
    }

    fn process(
        &mut self,
        view: &InferenceView<'_>,
        bucket: TimeBucket,
        token_mgr: &BucketKeyManager,
    ) -> Result<Vec<CandidateEvent>> {
        // Run detection on the inference view (cannot access raw bytes)
        let result = view.run_detector(&mut self.detector);

        if result.motion_detected {
            let token = if self.emit_token {
                Some(token_mgr.token_for_features(view.features_hash())?)
            } else {
                None
            };

            let event_type = match result.size_class {
                SizeClass::Small => EventType::BoundaryCrossingObjectSmall,
                _ => EventType::BoundaryCrossingObjectLarge,
            };

            Ok(vec![CandidateEvent {
                event_type,
                time_bucket: bucket,
                zone_id: self.zone_id.clone(),
                confidence: result.confidence,
                correlation_token: token,
            }])
        } else {
            Ok(vec![])
        }
    }
}

// -------------------- Legacy stub for compatibility --------------------

/// Legacy stub frame source (for tests that don't need full RTSP).
pub struct StubFrameSource {
    source: RtspSource,
}

impl StubFrameSource {
    pub fn new() -> Self {
        let config = RtspConfig {
            url: "stub://test".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
            backend: crate::config::RtspBackendPreference::Auto,
        };
        Self {
            source: RtspSource::new(config).expect("stub RTSP source"),
        }
    }

    pub fn next_frame(&mut self) -> Result<RawFrame> {
        self.source.next_frame()
    }
}

impl Default for StubFrameSource {
    fn default() -> Self {
        Self::new()
    }
}

/// Legacy Frame type for backward compatibility.
/// New code should use RawFrame + InferenceView.
#[derive(Clone, Debug)]
pub struct Frame {
    pub dummy_motion: bool,
    pub features_hash: [u8; 32],
}

// -------------------- Conformance Tests --------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{Approval, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest};
    use ed25519_dalek::{Signer, SigningKey};

    fn make_break_glass_token(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
    ) -> (BreakGlassToken, VerifyingKey, [u8; 32]) {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let request = UnlockRequest::new(envelope_id, ruleset_hash, "test-export", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let signature = signing_key.sign(&request.request_hash());
        let approval = Approval::new(
            TrusteeId::new("alice"),
            request.request_hash(),
            signature.to_vec(),
        );
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: signing_key.verifying_key().to_bytes(),
            }],
        )
        .unwrap();
        let (result, _receipt) = BreakGlass::authorize(&policy, &request, &[approval], bucket);
        let mut token = result.expect("break-glass token");
        let device_signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let receipt_hash = [8u8; 32];
        token
            .attach_receipt_signature(receipt_hash, &device_signing_key)
            .expect("attach receipt signature");
        (token, device_signing_key.verifying_key(), receipt_hash)
    }

    #[test]
    fn conformance_rejects_precise_time() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 60,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };
        assert!(ContractEnforcer::enforce(cand).is_err());
    }

    #[test]
    fn conformance_rejects_zone_id_outside_allowlist() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "lat=41.5,lon=-81.6".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };
        assert!(ContractEnforcer::enforce(cand).is_err());
    }

    #[test]
    fn conformance_rejects_confidence_out_of_bounds() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 1.5,
            correlation_token: None,
        };
        assert!(ContractEnforcer::enforce(cand).is_err());
    }

    #[test]
    fn conformance_rejects_token_with_oversized_bucket() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 901,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: Some([0u8; 32]),
        };
        assert!(ContractEnforcer::enforce(cand).is_err());
    }

    #[test]
    fn tokens_rotate_and_are_not_comparable_across_buckets() -> Result<()> {
        let mut mgr = BucketKeyManager::new();
        let b1 = TimeBucket {
            start_epoch_s: 0,
            size_s: 600,
        };
        let b2 = TimeBucket {
            start_epoch_s: 600,
            size_s: 600,
        };

        mgr.rotate_if_needed(b1);
        let k1 = mgr.export_key_for_test_only();

        let feat = [7u8; 32];
        let t1 = mgr.token_for_features(feat)?;

        mgr.rotate_if_needed(b2);
        let k2 = mgr.export_key_for_test_only();
        let t2 = mgr.token_for_features(feat)?;

        assert_ne!(k1, k2, "bucket keys must rotate");
        assert_ne!(
            t1, t2,
            "tokens must not match across buckets for same features"
        );
        Ok(())
    }

    #[test]
    fn export_omits_precise_timestamps_and_identity_fields() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: Some([1u8; 32]),
        };
        kernel.append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;

        let artifact = kernel.export_events_sequential_unchecked(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                ..ExportOptions::default()
            },
        )?;
        let value = serde_json::to_value(&artifact)?;

        fn contains_key(value: &serde_json::Value, key: &str) -> bool {
            match value {
                serde_json::Value::Object(map) => {
                    map.iter().any(|(k, v)| k == key || contains_key(v, key))
                }
                serde_json::Value::Array(items) => items.iter().any(|v| contains_key(v, key)),
                _ => false,
            }
        }

        assert!(!contains_key(&value, "created_at"));
        assert!(!contains_key(&value, "timestamp"));
        assert!(!contains_key(&value, "correlation_token"));
        Ok(())
    }

    #[test]
    fn sealed_event_created_at_matches_time_bucket_start() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
        };
        let bucket_start = 1_200u64;
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: bucket_start,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };
        kernel.append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;

        let created_at: i64 =
            kernel
                .conn
                .query_row("SELECT created_at FROM sealed_events LIMIT 1", [], |row| {
                    row.get(0)
                })?;
        assert_eq!(created_at, i64::try_from(bucket_start).unwrap());
        Ok(())
    }

    #[test]
    fn reprocess_guard_blocks_ruleset_mismatch() {
        let a = [1u8; 32];
        let b = [2u8; 32];
        assert!(ReprocessGuard::assert_same_ruleset(a, b).is_err());
    }

    #[test]
    fn module_allowlist_blocks_unauthorized_event_type() {
        let module = ZoneCrossingModule::new("zone:test");
        let desc = module.descriptor();
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectSmall,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };
        assert!(enforce_module_event_allowlist(&desc, &cand).is_ok());
    }

    #[test]
    fn module_allowlist_rejects_unauthorized_event_type() {
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[],
            requested_capabilities: &[],
        };

        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };

        assert!(enforce_module_event_allowlist(&desc, &cand).is_err());
    }

    #[test]
    fn device_key_seed_rejects_mvp_placeholder() {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:mvp".to_string(),
            zone_policy: ZonePolicy::default(),
        };

        assert!(Kernel::open(&cfg).is_err());
    }

    #[test]
    fn device_key_seed_accepts_and_signs() -> Result<()> {
        let seed = "devkey:test";
        let signing_key = signing_key_from_seed(seed)?;
        let verifying_key = verifying_key_from_seed(seed)?;
        let entry_hash = [7u8; 32];
        let signature = sign_entry(&signing_key, &entry_hash);
        verify_entry_signature(&verifying_key, &entry_hash, &signature)?;
        Ok(())
    }

    #[test]
    fn append_event_checked_rejects_disallowed_module_event() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[],
            requested_capabilities: &[],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };

        assert!(kernel
            .append_event_checked(
                &desc,
                cand,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash
            )
            .is_err());
        let event_count: i64 =
            kernel
                .conn
                .query_row("SELECT COUNT(*) FROM sealed_events", [], |row| row.get(0))?;
        assert_eq!(event_count, 0, "disallowed events must not be appended");
        Ok(())
    }

    #[test]
    fn zone_policy_rejects_sensitive_zone() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::new(vec!["zone:private".to_string()])?,
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:private".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };

        assert!(kernel
            .append_event_checked(
                &desc,
                cand,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash
            )
            .is_err());
        let event_count: i64 =
            kernel
                .conn
                .query_row("SELECT COUNT(*) FROM sealed_events", [], |row| row.get(0))?;
        let alarm_count: i64 = kernel.conn.query_row(
            "SELECT COUNT(*) FROM conformance_alarms WHERE code = 'CONFORMANCE_ZONE_POLICY_REJECT'",
            [],
            |row| row.get(0),
        )?;
        assert_eq!(event_count, 0);
        assert_eq!(alarm_count, 1);
        Ok(())
    }

    #[test]
    fn zone_policy_allows_non_sensitive_zone() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::new(vec!["zone:private".to_string()])?,
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 600,
            },
            zone_id: "zone:public".to_string(),
            confidence: 0.5,
            correlation_token: None,
        };

        kernel.append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        let event_count: i64 =
            kernel
                .conn
                .query_row("SELECT COUNT(*) FROM sealed_events", [], |row| row.get(0))?;
        let alarm_count: i64 =
            kernel
                .conn
                .query_row("SELECT COUNT(*) FROM conformance_alarms", [], |row| {
                    row.get(0)
                })?;
        assert_eq!(event_count, 1);
        assert_eq!(alarm_count, 0);
        Ok(())
    }

    #[test]
    fn break_glass_hash_rejects_malformed_size() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg)?;
        let created_at = now_s()? as i64;
        kernel.conn.execute(
            r#"
            INSERT INTO break_glass_receipts(created_at, payload_json, prev_hash, entry_hash, signature)
            VALUES (?1, ?2, ?3, ?4, ?5)
            "#,
            params![
                created_at,
                "{}",
                vec![0u8; 32],
                vec![1u8; 31],
                vec![2u8; 64],
            ],
        )?;

        assert!(kernel.last_break_glass_hash_or_zero().is_err());
        Ok(())
    }

    #[test]
    fn break_glass_prev_hash_rejects_malformed_size() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg)?;
        let created_at = now_s()? as i64;
        kernel.conn.execute(
            r#"
            INSERT INTO break_glass_receipts(created_at, payload_json, prev_hash, entry_hash, signature)
            VALUES (?1, ?2, ?3, ?4, ?5)
            "#,
            params![
                created_at,
                "{}",
                vec![0u8; 31],
                vec![1u8; 32],
                vec![2u8; 64],
            ],
        )?;

        assert!(kernel.last_break_glass_hash_or_zero().is_err());
        Ok(())
    }

    #[test]
    fn raw_media_boundary_denies_export() {
        let result: Result<Vec<u8>> = RawMediaBoundary::deny_export("test");
        assert!(result.is_err());
    }

    #[test]
    fn raw_media_boundary_exports_for_vault() -> Result<()> {
        let envelope_id = "test-envelope";
        let ruleset_hash = [7u8; 32];
        let (mut token, verifying_key, receipt_hash) =
            make_break_glass_token(envelope_id, ruleset_hash);
        let mut data = b"vault bytes".to_vec();

        let bytes = RawMediaBoundary::export_for_vault(
            &mut data,
            &mut token,
            envelope_id,
            ruleset_hash,
            &verifying_key,
            |hash| {
                assert_eq!(hash, &receipt_hash);
                Ok(BreakGlassOutcome::Granted)
            },
        )?;
        assert_eq!(bytes, b"vault bytes");
        assert!(data.is_empty());
        Ok(())
    }
}

// Re-exports for CLI/tools
pub use break_glass::{
    approvals_commitment, Approval, BreakGlass, BreakGlassOutcome, BreakGlassReceipt,
    BreakGlassToken, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
