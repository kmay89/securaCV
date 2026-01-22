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
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::sync::OnceLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zeroize::Zeroize;

pub mod break_glass;
pub mod frame;
pub mod ingest;
pub mod module_runtime;
pub mod vault;

pub use frame::{
    Detection, DetectionResult, Detector, FrameBuffer, InferenceView, RawFrame, SizeClass,
    StubDetector, MAX_BUFFER_FRAMES, MAX_PREROLL_SECS,
};
pub use ingest::{rtsp::RtspConfig, RtspSource};
pub use module_runtime::{CapabilityBoundaryRuntime, ModuleCapability};
pub use vault::{Vault, VaultConfig};

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
#[derive(Clone, Debug)]
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
    ) -> Result<Vec<u8>> {
        let now_bucket = TimeBucket::now(600)?;
        break_glass::BreakGlass::assert_token_valid(
            token,
            envelope_id,
            expected_ruleset_hash,
            now_bucket,
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
}

impl KernelConfig {
    pub fn ruleset_hash_from_id(ruleset_id: &str) -> [u8; 32] {
        Sha256::digest(ruleset_id.as_bytes()).into()
    }
}

pub struct Kernel {
    pub conn: Connection,
    device_key: SigningKey,
    break_glass_policy: Option<crate::break_glass::QuorumPolicy>,
}

impl Kernel {
    pub fn open(cfg: &KernelConfig) -> Result<Self> {
        let conn = Connection::open(&cfg.db_path)?;
        let device_key = signing_key_from_seed(&cfg.device_key_seed)?;
        let mut k = Self {
            conn,
            device_key,
            break_glass_policy: None,
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
        let created_at = now_s()? as i64;
        self.conn.execute(
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
        let created_at = now_s()? as i64;
        let prev_hash = self.last_event_hash_or_checkpoint_head()?;
        let payload_json = serde_json::to_string(ev)?;

        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        let signature = sign_entry(&self.device_key, &entry_hash);

        self.conn.execute(
            r#"
            INSERT INTO sealed_events(created_at, payload_json, prev_hash, entry_hash, signature)
            VALUES (?1, ?2, ?3, ?4, ?5)
            "#,
            params![
                created_at,
                payload_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature.to_vec()
            ],
        )?;

        Ok(())
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

    pub fn append_break_glass_receipt(
        &mut self,
        receipt: &crate::break_glass::BreakGlassReceipt,
        approvals: &[crate::break_glass::Approval],
    ) -> Result<()> {
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

        Ok(())
    }

    pub fn log_break_glass_receipt(
        &mut self,
        receipt: &crate::break_glass::BreakGlassReceipt,
        approvals: &[crate::break_glass::Approval],
    ) -> Result<()> {
        self.append_break_glass_receipt(receipt, approvals)
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
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;

        // Find the last event id strictly older than cutoff.
        let mut stmt = self.conn.prepare(
            "SELECT id, entry_hash FROM sealed_events WHERE created_at < ?1 ORDER BY id DESC LIMIT 1",
        )?;
        let mut rows = stmt.query(params![cutoff])?;
        let Some(row) = rows.next()? else {
            return Ok(()); // nothing to prune
        };

        let cutoff_id: i64 = row.get(0)?;
        let head_bytes: Vec<u8> = row.get(1)?;
        if head_bytes.len() != 32 {
            return Err(anyhow!("corrupt sealed log: entry_hash size"));
        }
        let mut head = [0u8; 32];
        head.copy_from_slice(&head_bytes);

        let checkpoint_sig = sign_entry(&self.device_key, &head);
        let created_at = now_s()? as i64;

        self.conn.execute(
            r#"
            INSERT INTO checkpoints(created_at, cutoff_event_id, chain_head_hash, signature)
            VALUES (?1, ?2, ?3, ?4)
            "#,
            params![
                created_at,
                cutoff_id,
                head.to_vec(),
                checkpoint_sig.to_vec()
            ],
        )?;

        // Delete all events up to and including cutoff_id.
        self.conn.execute(
            "DELETE FROM sealed_events WHERE id <= ?1",
            params![cutoff_id],
        )?;

        Ok(())
    }

    /// Read events from the sealed log for review or export.
    /// This is a *ruleset-bound* operation: attempting to read/interpret events under a different ruleset
    /// is treated as a conformance violation (Invariant VI).
    pub fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
    ) -> Result<Vec<Event>> {
        let payloads = {
            let mut stmt = self
                .conn
                .prepare("SELECT payload_json FROM sealed_events ORDER BY id ASC LIMIT ?1")?;
            let mut rows = stmt.query(params![limit as i64])?;
            let mut payloads = Vec::new();

            while let Some(row) = rows.next()? {
                let payload: String = row.get(0)?;
                payloads.push(payload);
            }

            payloads
        };

        let mut out = Vec::with_capacity(payloads.len());
        for payload in payloads {
            let ev: Event = serde_json::from_str(&payload)?;

            if let Err(e) =
                ReprocessGuard::assert_same_ruleset(expected_ruleset_hash, ev.ruleset_hash)
            {
                // Must fail audibly and verifiably: log alarm then return error.
                self.log_alarm("CONFORMANCE_REPROCESS_VIOLATION", &format!("{}", e))?;
                return Err(e);
            }

            out.push(ev);
        }
        Ok(out)
    }

    pub fn device_key_for_verify_only(&self) -> [u8; 32] {
        self.device_key.verifying_key().to_bytes()
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

fn now_s() -> Result<u64> {
    Ok(SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs())
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
        };
        Self {
            source: RtspSource::new(config),
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

    fn make_break_glass_token(envelope_id: &str, ruleset_hash: [u8; 32]) -> BreakGlassToken {
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
        result.expect("break-glass token")
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
    fn break_glass_hash_rejects_malformed_size() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test".to_string(),
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
        let mut token = make_break_glass_token(envelope_id, ruleset_hash);
        let mut data = b"vault bytes".to_vec();

        let bytes =
            RawMediaBoundary::export_for_vault(&mut data, &mut token, envelope_id, ruleset_hash)?;
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
