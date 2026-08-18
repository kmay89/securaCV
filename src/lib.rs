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
//! - `ingest`: Frame sources (RTSP, USB, local files)
//! - Core types: Events, TimeBucket, ContractEnforcer, Kernel

use anyhow::{anyhow, Result};
use ed25519_dalek::{SigningKey, VerifyingKey};
use hkdf::Hkdf;
use rusqlite::{params, Connection, OpenFlags, OptionalExtension};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::sync::OnceLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zeroize::Zeroizing;
// zeroize::Zeroizing used directly in BucketKeyManager

use crate::crypto::signatures::{
    sign_rotation_attestation, sign_rotation_authorization, verify_rotation_attestation,
    verify_rotation_authorization, PqPublicKey, SignatureKeys, SignatureMode, SignatureSet,
    DOMAIN_BREAK_GLASS_RECEIPT, DOMAIN_EXPORT_RECEIPT, DOMAIN_POLICY_CHANGE_RECORD,
    DOMAIN_SEALED_LOG_ENTRY,
};
use crate::storage::ensure_columns;

#[cfg(feature = "pqc-signatures")]
use crate::crypto::signatures::PqKeypair;
#[cfg(feature = "pqc-signatures")]
use crate::crypto::signatures::PqSecretKey;
#[cfg(feature = "pqc-signatures")]
use pqcrypto_traits::sign::PublicKey as PqPublicKeyTrait;

pub mod adapter;
pub mod api;
pub mod break_glass;
pub mod bridge;
#[cfg(feature = "c2pa-export")]
pub mod c2pa_export;
pub mod canonical_json;
pub mod config;
pub mod crypto;
pub mod detect;
pub mod envelope;
pub mod eval;
pub mod frame;
pub mod ingest;
pub mod inspect;
pub mod log;
pub mod module_runtime;
#[cfg(feature = "alert-relay")]
pub mod relay;
pub mod storage;
pub mod storage_health;
pub mod thumbnail;
pub mod transport;
pub mod tsa;
pub mod vault;
pub mod verify;
pub mod verify_explain;

pub use adapter::{
    claim_kind_to_event_type, AdapterDescriptor, AdapterHost, AdapterRegistry, Claim, ClaimKind,
    SensorAdapter,
};
pub use detect::{Detection, DetectionResult, SizeClass};
pub use envelope::{verify_envelope, EnvelopeReport, EvidenceEnvelope, IntegrityStatus};
pub use frame::{
    select_inference_backend, BackendSelection, CpuDetector, Detector, DetectorBackend,
    DeviceCapabilities, FrameBuffer, InferenceBackend, InferenceView, RawFrame, StubDetector,
    MAX_BUFFER_FRAMES, MAX_PREROLL_SECS,
};
#[cfg(feature = "ingest-esp32")]
pub use ingest::{esp32::Esp32Config, Esp32Source};
pub use ingest::{file::FileConfig, FileSource};
pub use ingest::{rtsp::RtspConfig, RtspSource};
#[cfg(feature = "ingest-v4l2")]
pub use ingest::{v4l2::V4l2Config, V4l2Source};
pub use log::{hash_entry, sign_entry, verify_entry_signature};
pub use module_runtime::{CapabilityBoundaryRuntime, ModuleCapability};
pub use storage::{InMemorySealedLogStore, SealedLogStore, SqliteSealedLogStore};
pub use storage_health::{
    SharedStorageHealth, StorageHealthMonitor, StorageHealthReport, StorageHealthStatus,
};
pub use vault::crypto::VaultCryptoMode;
pub use vault::{FilesystemVaultStore, Vault, VaultConfig, VaultStore};

pub fn shared_memory_uri() -> String {
    let mut bytes = [0u8; 8];
    rand::fill(&mut bytes[..]);
    format!(
        "file:witness_kernel_{:x}?mode=memory&cache=shared",
        u64::from_le_bytes(bytes)
    )
}

/// Domain separation context for deriving the DB encryption key via HKDF-SHA256.
/// This keeps the DB key cryptographically independent from signing operations.
const DB_ENCRYPTION_HKDF_CONTEXT: &[u8] = b"securacv-db-encryption-v1";

/// Environment variable holding an **independent** DB-encryption secret. When set
/// (non-empty), the SQLCipher key is derived from this secret instead of the device
/// signing key, decoupling the database key from the device identity so the signing
/// key can be rotated without re-encrypting the database. Distinct from
/// `SECURACV_DB_KEY`, which supplies the already-derived hex key directly.
pub const DB_KEY_SEED_ENV: &str = "SECURACV_DB_KEY_SEED";

/// Derive a hex-encoded SQLCipher key from an arbitrary independent secret using
/// HKDF-SHA256 with domain separation. Use this to key the database from a secret
/// that is **not** the device signing key (enables identity rotation without
/// losing database access, and independent rotation of the DB key itself).
pub fn derive_db_encryption_key_from_secret(secret: &[u8]) -> Zeroizing<String> {
    let hk = Hkdf::<Sha256>::new(None, secret);
    let mut okm = Zeroizing::new([0u8; 32]);
    hk.expand(DB_ENCRYPTION_HKDF_CONTEXT, okm.as_mut())
        .expect("HKDF-SHA256 expand with 32-byte output cannot fail");
    Zeroizing::new(hex::encode(okm.as_ref()))
}

/// Derive a hex-encoded SQLCipher key from an Ed25519 signing key using
/// HKDF-SHA256 with domain separation.
///
/// This is the legacy default: the DB key is a deterministic function of the
/// signing key, so rotating the signing key changes the DB key. To decouple them,
/// provide an independent secret via [`DB_KEY_SEED_ENV`] / [`resolve_db_encryption_key`].
pub fn derive_db_encryption_key(signing_key: &SigningKey) -> Zeroizing<String> {
    // Byte-identical to `derive_db_encryption_key_from_secret(signing_key.as_bytes())`,
    // so existing databases keyed this way continue to open unchanged.
    derive_db_encryption_key_from_secret(signing_key.as_bytes())
}

/// Resolve the SQLCipher key for opening a database: derive from the independent
/// `db_key_seed` when one is supplied (non-empty), otherwise fall back to the
/// signing-key derivation. Centralising this keeps the kernel and the verifier
/// CLIs consistent about how the DB key is chosen.
pub fn resolve_db_encryption_key(
    signing_key: &SigningKey,
    db_key_seed: Option<&str>,
) -> Zeroizing<String> {
    match db_key_seed.map(str::trim) {
        Some(seed) if !seed.is_empty() => derive_db_encryption_key_from_secret(seed.as_bytes()),
        _ => derive_db_encryption_key(signing_key),
    }
}

/// Read the independent DB-key seed from the environment, if set and non-empty.
/// Wrapped in `Zeroizing` so the secret seed is wiped from memory when dropped.
pub fn db_key_seed_from_env() -> Option<Zeroizing<String>> {
    match std::env::var(DB_KEY_SEED_ENV) {
        Ok(s) if !s.trim().is_empty() => Some(Zeroizing::new(s)),
        _ => None,
    }
}

/// Re-encrypt a SQLCipher database file from `old_key_hex` to `new_key_hex`
/// (offline — the database must not be open elsewhere). This is the rotation
/// primitive: an operator can migrate a database from a signing-key-derived key
/// to an independent secret (or rotate the independent secret) without data loss.
/// Both keys are 64-char hex (32 raw bytes), as produced by the `derive_*` helpers.
pub fn rekey_database_file(db_path: &str, old_key_hex: &str, new_key_hex: &str) -> Result<()> {
    let conn = Connection::open(db_path)?;
    // Authenticate with the current key (also verifies it is correct).
    apply_sqlcipher_key(&conn, old_key_hex)?;
    // SQLCipher re-encrypts every page in place with the new key. The PRAGMA
    // argument embeds the raw key hex, so keep it in a Zeroizing buffer.
    let rekey_cmd = Zeroizing::new(format!("x'{}'", new_key_hex));
    conn.pragma_update(None, "rekey", &*rekey_cmd)
        .map_err(|e| anyhow!("SQLCipher rekey failed: {}", e))?;
    // Confirm the new key now authenticates.
    conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
        .map_err(|_| anyhow!("post-rekey verification failed"))?;
    drop(conn);
    enforce_db_file_permissions(db_path);
    Ok(())
}

/// Apply the SQLCipher encryption key to an open database connection.
fn apply_sqlcipher_key(conn: &Connection, hex_key: &str) -> Result<()> {
    conn.pragma_update(None, "key", format!("x'{}'", hex_key))?;
    // Verify the key works by reading a page
    conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()))
        .map_err(|_| {
            anyhow!("SQLCipher key verification failed — wrong key or corrupt database")
        })?;
    Ok(())
}

/// Check if a database file is unencrypted (plaintext SQLite header).
fn is_unencrypted_db(db_path: &str) -> bool {
    let path = std::path::Path::new(db_path);
    if !path.exists() {
        return false;
    }
    // SQLite databases start with "SQLite format 3\0" (16 bytes).
    // Encrypted databases have random-looking bytes at offset 0.
    match std::fs::read(path) {
        Ok(data) if data.len() >= 16 => data.starts_with(b"SQLite format 3\0"),
        _ => false,
    }
}

/// Migrate an unencrypted SQLite database to SQLCipher encryption in-place.
/// Uses ATTACH + sqlcipher_export() to re-encrypt all data.
fn migrate_unencrypted_to_encrypted(db_path: &str, hex_key: &str) -> Result<()> {
    eprintln!(
        "[SECURITY] Migrating unencrypted database '{}' to SQLCipher encryption",
        db_path
    );

    let encrypted_path = format!("{}.encrypted", db_path);

    // Open the unencrypted database (no key)
    let plain_conn = Connection::open(db_path)?;

    // Attach the new encrypted database
    plain_conn.execute_batch(&format!(
        "ATTACH DATABASE '{}' AS encrypted KEY \"x'{}'\";",
        encrypted_path, hex_key
    ))?;

    // Export all data to the encrypted database
    plain_conn.query_row("SELECT sqlcipher_export('encrypted')", [], |_| Ok(()))?;

    // Detach and close
    plain_conn.execute_batch("DETACH DATABASE encrypted;")?;
    drop(plain_conn);

    // Replace the original file with the encrypted one
    std::fs::rename(&encrypted_path, db_path)?;
    enforce_db_file_permissions(db_path);

    eprintln!(
        "[SECURITY] Database migration to SQLCipher complete: '{}'",
        db_path
    );

    Ok(())
}

/// SQLite `synchronous` preference for sealed-log connections.
///
/// `Full` (the default) syncs on every transaction commit: a power cut can
/// never lose an acknowledged sealed event. `Normal` reduces fsync traffic —
/// an SD-card endurance optimization — and in WAL mode can never corrupt the
/// database or break hash-chain verifiability (a power cut merely truncates
/// the WAL tail), but the most recent sealed events may be lost. Because a
/// witness device's threat model includes being powered off mid-incident,
/// `Normal` is strictly opt-in (`storage_health.sqlite_synchronous` /
/// `WITNESS_SQLITE_SYNCHRONOUS`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SqliteSynchronous {
    #[default]
    Full,
    Normal,
}

impl SqliteSynchronous {
    pub fn parse(raw: &str) -> Result<Self> {
        match raw.trim().to_lowercase().as_str() {
            "full" => Ok(Self::Full),
            "normal" => Ok(Self::Normal),
            other => Err(anyhow!(
                "unsupported sqlite_synchronous '{}'; expected 'full' or 'normal'",
                other
            )),
        }
    }
}

static SQLITE_SYNCHRONOUS: OnceLock<SqliteSynchronous> = OnceLock::new();

/// Set the process-wide SQLite `synchronous` preference. Call once at
/// startup, before any kernel/store is opened; later calls are ignored
/// (every connection in the process must behave identically).
pub fn set_sqlite_synchronous(mode: SqliteSynchronous) {
    let _ = SQLITE_SYNCHRONOUS.set(mode);
}

pub(crate) fn open_db_connection(db_path: &str) -> Result<Connection> {
    open_db_connection_with_key(db_path, None)
}

/// How long a connection waits for SQLite's write lock before an operation
/// fails with `SQLITE_BUSY`. rusqlite installs the same 5 s handler at open,
/// but the sealed log must own its busy policy rather than inherit a library
/// default silently: an append that exhausts this wait is a failed evidence
/// seal, so the value is stated here and asserted by a test.
pub(crate) const DB_BUSY_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

/// Apply per-connection durability/endurance pragmas. `synchronous` is a
/// per-connection setting, so it must be applied on every open, not in
/// `ensure_schema`.
fn apply_connection_pragmas(conn: &Connection) -> Result<()> {
    conn.busy_timeout(DB_BUSY_TIMEOUT)?;
    if SQLITE_SYNCHRONOUS.get().copied().unwrap_or_default() == SqliteSynchronous::Normal {
        conn.pragma_update(None, "synchronous", "NORMAL")?;
    }
    Ok(())
}

pub(crate) fn open_db_connection_with_key(
    db_path: &str,
    encryption_key: Option<&str>,
) -> Result<Connection> {
    if db_path.starts_with("file:") {
        let conn = Connection::open_with_flags(
            db_path,
            OpenFlags::SQLITE_OPEN_READ_WRITE
                | OpenFlags::SQLITE_OPEN_CREATE
                | OpenFlags::SQLITE_OPEN_URI,
        )?;
        if let Some(key) = encryption_key {
            apply_sqlcipher_key(&conn, key)?;
        }
        apply_connection_pragmas(&conn)?;
        return Ok(conn);
    }
    // Pre-create the DB file with 0600 permissions before SQLite opens it.
    // This prevents a TOCTOU race where the file briefly exists with the
    // default umask (e.g. 0022 → 0644, world-readable).
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        let path = std::path::Path::new(db_path);
        if !path.exists() {
            if let Err(e) = std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(path)
            {
                eprintln!(
                    "[CONFORMANCE] failed to pre-create DB with 0600 permissions: {}",
                    e
                );
            }
        }
    }

    // Migration: if database exists and is unencrypted, encrypt it in-place
    if let Some(key) = encryption_key {
        if is_unencrypted_db(db_path) {
            migrate_unencrypted_to_encrypted(db_path, key)?;
        }
    }

    let conn = Connection::open(db_path)?;

    // Apply encryption key if provided
    if let Some(key) = encryption_key {
        apply_sqlcipher_key(&conn, key)?;
    }

    apply_connection_pragmas(&conn)?;

    // Also tighten permissions on existing files that may have been created
    // with a lax umask by an older version of the kernel.
    enforce_db_file_permissions(db_path);
    Ok(conn)
}

/// Enforce restrictive file permissions on the SQLite database.
/// Logs a warning on failure rather than failing hard (the DB may be
/// in-memory or on a filesystem that doesn't support Unix permissions).
fn enforce_db_file_permissions(db_path: &str) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let path = std::path::Path::new(db_path);
        if path.exists() {
            if let Err(e) = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600)) {
                eprintln!(
                    "[CONFORMANCE] failed to set 0600 permissions on database '{}': {}",
                    db_path, e
                );
            }
        }
    }
}

// -------------------- Time Buckets --------------------
// Per spec/event_contract.md §3: minimum bucket 5 minutes, typical 10-15 minutes.
// These parameters are conformance-critical and MUST NOT be narrowed.

/// Minimum allowed bucket size per spec/event_contract.md §3 (5 minutes).
pub const MIN_BUCKET_SIZE_S: u32 = 300;
const TEN_MINUTES_S: u32 = 600;
const FIFTEEN_MINUTES_S: u32 = 900;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimeBucket {
    /// start of bucket in seconds since epoch (coarse)
    pub start_epoch_s: u64,
    /// bucket size in seconds (e.g., 600 = 10 minutes)
    pub size_s: u32,
}

impl TimeBucket {
    /// Validate that the bucket size meets the minimum requirement.
    ///
    /// # Errors
    /// Returns an error if `bucket_size_s` is less than `MIN_BUCKET_SIZE_S` (300 seconds)
    /// per spec/event_contract.md §3: "Minimum bucket: 5 minutes".
    pub fn validate_bucket_size(bucket_size_s: u32) -> Result<()> {
        if bucket_size_s < MIN_BUCKET_SIZE_S {
            return Err(anyhow!(
                "time bucket size {} is below minimum {} seconds (5 minutes) per spec/event_contract.md §3",
                bucket_size_s,
                MIN_BUCKET_SIZE_S
            ));
        }
        Ok(())
    }

    /// Create a time bucket for the current time with the given bucket size.
    ///
    /// # Errors
    /// Returns an error if `bucket_size_s` is less than `MIN_BUCKET_SIZE_S` (300 seconds)
    /// per spec/event_contract.md §3: "Minimum bucket: 5 minutes".
    pub fn now(bucket_size_s: u32) -> Result<Self> {
        Self::validate_bucket_size(bucket_size_s)?;
        let now = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
        let size = bucket_size_s as u64;
        let start = (now / size) * size;
        Ok(TimeBucket {
            start_epoch_s: start,
            size_s: bucket_size_s,
        })
    }

    pub fn now_10min() -> Result<Self> {
        Self::now(TEN_MINUTES_S)
    }

    /// Coarsen to a larger bucket. The target bucket size must be >= MIN_BUCKET_SIZE_S.
    pub fn coarsen_to(self, bucket_size_s: u32) -> Result<Self> {
        Self::validate_bucket_size(bucket_size_s)?;
        let size = bucket_size_s as u64;
        let start = (self.start_epoch_s / size) * size;
        Ok(TimeBucket {
            start_epoch_s: start,
            size_s: bucket_size_s,
        })
    }
}

// -------------------- Event Types --------------------

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum EventType {
    BoundaryCrossingObjectLarge,
    BoundaryCrossingObjectSmall,
    /// A coarse acoustic impulse (e.g. impact/discharge-like sound) was sensed in a zone.
    /// No waveform, direction, or precise time is ever retained — only the claim.
    AcousticImpulseInZone,
    /// A presence was sensed in an operator-designated restricted zone.
    /// Carries no identity, appearance, or trajectory.
    PresenceInRestrictedZone,
    /// A vehicle-sized presence was sensed during operator-configured "after hours".
    /// Carries no plate, make, model, color, or cross-zone linkage.
    VehiclePresenceAfterHours,
    /// A binary contact/open-close state change (door, gate, window, enclosure).
    ContactStateChange,
    /// An object previously present in a zone is no longer present (removal).
    ObjectRemovedFromZone,
    /// Tampering with the witnessing device itself was detected: enclosure
    /// opened, camera covered/blinded, or thermal-attack temperature drift.
    /// Carries no detail beyond the coarse claim — the zone names the device
    /// location, never the attacker.
    TamperDetected,
    /// A vehicle arrived at or departed a zone (e.g. ignition on/off sensed
    /// passively off a vehicle's own CAN bus). Carries no plate, make,
    /// model, trip data, or GPS trail — a binary state change, same
    /// coarseness as `ContactStateChange`, scoped to a named zone (e.g.
    /// `zone:garage`) rather than a door.
    VehicleArrivalDeparture,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum FailureType {
    StorageFull,
    StorageWriteFailed,
    CryptoFailure,
    ClockSkew,
    SensorDisagreement,
    PowerLoss,
    FirmwareIntegrity,
    GapMissingData,
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
    /// How the underlying claim was attested before reaching the kernel.
    /// `None` (the default, and the only value the in-process camera pipeline
    /// produces) renders as device-attested downstream. A closed enum — not a
    /// free string — so this field can never smuggle identity or free text
    /// into the sealed log.
    pub attestation: Option<Attestation>,
}

/// Provenance of a claim relative to the kernel signature that seals it.
/// Serialized values match the Home Assistant integration's attestation
/// contract (`custom_components/securacv/const.py`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Attestation {
    /// Claim was produced by an out-of-process adapter and is only
    /// kernel-signed at ingest (Track B), not device-signed at source.
    #[serde(rename = "adapter")]
    Adapter,
    /// Claim additionally transited Home Assistant (e.g. an
    /// `mqtt_statestream` bridge) before reaching the adapter.
    #[serde(rename = "ha-bridged")]
    HaBridged,
}

/// Events are trusted, kernel-bound claims written to the sealed log.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Event {
    pub event_type: EventType,
    pub time_bucket: TimeBucket,
    pub zone_id: String,
    pub confidence: f32,
    pub correlation_token: Option<[u8; 32]>,
    /// Optional provenance marker (see [`Attestation`]). Skipped when absent
    /// so records sealed before this field existed round-trip byte-identically
    /// and their signatures keep verifying.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attestation: Option<Attestation>,
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

/// Explicit failure/gap artifacts recorded in the sealed log.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FailureEvent {
    pub failure_type: FailureType,
    pub time_bucket: TimeBucket,
    pub details: Option<String>,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

impl FailureEvent {
    pub fn bind(mut self, kernel_version: &str, ruleset_id: &str, ruleset_hash: [u8; 32]) -> Self {
        self.kernel_version = kernel_version.to_string();
        self.ruleset_id = ruleset_id.to_string();
        self.ruleset_hash = ruleset_hash;
        self
    }
}

/// A periodic system-trace record sealed once per coarse time bucket.
///
/// Heartbeats anchor the tail of the hash chain: without them, deleting the
/// most recent N entries leaves a chain that still verifies (prev-hash
/// continuity only protects interior records). A verifier that knows the
/// cadence can flag any bucket between `lifecycle:start` and
/// `lifecycle:shutdown_clean` that has no heartbeat.
///
/// Content is restricted to per-bucket aggregate deltas and a boolean health
/// flag — no identifiers, no cumulative counters, nothing finer than the
/// bucket (see spec/event_contract.md "System Trace Records").
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct HeartbeatRecord {
    pub time_bucket: TimeBucket,
    /// Whether the ingest source was healthy when this heartbeat was sealed.
    pub ingest_healthy: bool,
    /// Frames captured since the previous heartbeat (delta, not cumulative).
    /// Heartbeats fire on bucket rollover (plus a final one at clean
    /// shutdown), so this covers roughly the preceding bucket.
    pub frames_captured_delta: u64,
    /// Events appended to the sealed log since the previous heartbeat.
    pub events_appended_delta: u64,
    /// Failure records appended since the previous heartbeat.
    pub failures_appended_delta: u64,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

/// Daemon lifecycle phase recorded in the sealed log.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum LifecyclePhase {
    /// The witnessing process started and opened the kernel.
    #[serde(rename = "start")]
    Start,
    /// The witnessing process shut down deliberately (signal-handled exit).
    #[serde(rename = "shutdown_clean")]
    ShutdownClean,
}

/// A sealed record marking daemon start / clean shutdown.
///
/// On boot, if the most recent lifecycle record is `start` (no intervening
/// `shutdown_clean`), the previous run ended uncleanly and a
/// `FailureType::PowerLoss` record is sealed — a deliberate proxy covering
/// power loss, crash, or kill (documented in docs/failure_semantics.md).
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct LifecycleRecord {
    pub phase: LifecyclePhase,
    pub time_bucket: TimeBucket,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

/// Sealed log records include normal events and explicit failure/gap artifacts.
///
/// Uses tagged serialization (`record_type` discriminator) to prevent silent
/// type confusion during deserialization. `untagged` was previously used but
/// could misclassify malformed JSON blobs, affecting sealed log integrity.
///
/// For backwards compatibility with existing databases that used `untagged`,
/// [`SealedLogRecord::deserialize_compat`] falls back to untagged parsing.
#[derive(Clone, Debug, Serialize)]
#[serde(tag = "record_type")]
pub enum SealedLogRecord {
    #[serde(rename = "event")]
    Event(Event),
    #[serde(rename = "failure")]
    Failure(FailureEvent),
    #[serde(rename = "key_rotation")]
    KeyRotation(KeyRotation),
    #[serde(rename = "heartbeat")]
    Heartbeat(HeartbeatRecord),
    #[serde(rename = "lifecycle")]
    Lifecycle(LifecycleRecord),
}

/// Tagged deserialization helper (new format with `record_type` field).
#[derive(Deserialize)]
#[serde(tag = "record_type")]
enum SealedLogRecordTagged {
    #[serde(rename = "event")]
    Event(Event),
    #[serde(rename = "failure")]
    Failure(FailureEvent),
    #[serde(rename = "key_rotation")]
    KeyRotation(KeyRotation),
    #[serde(rename = "heartbeat")]
    Heartbeat(HeartbeatRecord),
    #[serde(rename = "lifecycle")]
    Lifecycle(LifecycleRecord),
}

/// Untagged deserialization helper (legacy format without `record_type` field).
#[derive(Deserialize)]
#[serde(untagged)]
enum SealedLogRecordLegacy {
    Event(Event),
    Failure(FailureEvent),
}

impl SealedLogRecord {
    /// Deserialize with backward compatibility: tries tagged format first,
    /// then falls back to untagged for records written before M1 migration.
    pub fn deserialize_compat(json_str: &str) -> Result<Self> {
        // Try tagged format first (new records)
        if let Ok(tagged) = serde_json::from_str::<SealedLogRecordTagged>(json_str) {
            return Ok(match tagged {
                SealedLogRecordTagged::Event(e) => SealedLogRecord::Event(e),
                SealedLogRecordTagged::Failure(f) => SealedLogRecord::Failure(f),
                SealedLogRecordTagged::KeyRotation(r) => SealedLogRecord::KeyRotation(r),
                SealedLogRecordTagged::Heartbeat(h) => SealedLogRecord::Heartbeat(h),
                SealedLogRecordTagged::Lifecycle(l) => SealedLogRecord::Lifecycle(l),
            });
        }
        // Fall back to untagged for existing databases
        let legacy: SealedLogRecordLegacy = serde_json::from_str(json_str)
            .map_err(|e| anyhow!("failed to deserialize SealedLogRecord: {}", e))?;
        Ok(match legacy {
            SealedLogRecordLegacy::Event(e) => SealedLogRecord::Event(e),
            SealedLogRecordLegacy::Failure(f) => SealedLogRecord::Failure(f),
        })
    }
}

impl SealedLogRecord {
    pub fn time_bucket(&self) -> TimeBucket {
        match self {
            SealedLogRecord::Event(ev) => ev.time_bucket,
            SealedLogRecord::Failure(ev) => ev.time_bucket,
            SealedLogRecord::KeyRotation(r) => r.time_bucket,
            SealedLogRecord::Heartbeat(h) => h.time_bucket,
            SealedLogRecord::Lifecycle(l) => l.time_bucket,
        }
    }

    pub fn ruleset_hash(&self) -> [u8; 32] {
        match self {
            SealedLogRecord::Event(ev) => ev.ruleset_hash,
            SealedLogRecord::Failure(ev) => ev.ruleset_hash,
            // A key rotation is an identity-administration record, not ruleset-bound.
            SealedLogRecord::KeyRotation(_) => [0u8; 32],
            SealedLogRecord::Heartbeat(h) => h.ruleset_hash,
            SealedLogRecord::Lifecycle(l) => l.ruleset_hash,
        }
    }
}

/// Schema discriminator for `KeyRotation` records, allowing forward-compatible evolution.
pub const KEY_ROTATION_SCHEMA_V1: &str = "securacv/device_key_rotation/v1";

/// An in-chain, signed record announcing a device signing-key rotation.
///
/// The record is appended to the sealed log like any other entry — hash-chained and
/// signed by the **retiring (old)** device key, which proves the legitimate key holder
/// authorized the rotation and makes the rotation itself tamper-evident. The payload
/// additionally carries `new_key_attestation`: the **new** key's signature over
/// `(prev_public_key ‖ new_public_key)`, proving possession of the incoming private key.
/// A verifier following the chain from the genesis key can therefore validate each
/// rotation (old-key entry signature + new-key attestation) and switch its expected
/// verifying key, keeping the entire log verifiable across identity changes.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct KeyRotation {
    /// Schema discriminator (`KEY_ROTATION_SCHEMA_V1`).
    pub schema: String,
    /// The retiring device Ed25519 public key (must equal the verifier's current key).
    pub prev_public_key: [u8; 32],
    /// The incoming device Ed25519 public key.
    pub new_public_key: [u8; 32],
    /// New key's possession attestation over `(prev_public_key ‖ new_public_key)`.
    pub new_key_attestation: Vec<u8>,
    /// Retiring (old) key's authorization over `(prev_public_key ‖ new_public_key)`. This is
    /// the genesis-anchored proof that the predecessor approved this successor; it is what
    /// lets the key lineage be reconstructed and trusted from the durable history table even
    /// after the in-chain rotation records have been pruned.
    ///
    /// Empty for legacy rotation records written before this field existed; those are instead
    /// anchored by their old-key *entry* signature (see the legacy-recovery path in
    /// [`reconstruct_device_key_lineage_from`]). `#[serde(default)]` keeps them deserializable.
    #[serde(default)]
    pub prev_key_authorization: Vec<u8>,
    /// Coarse time bucket — carries no precise time, satisfying the record contract.
    pub time_bucket: TimeBucket,
}

/// One epoch in the device key lineage, reconstructed and cryptographically validated from
/// the genesis anchor. See [`reconstruct_device_key_lineage_from`].
#[derive(Clone, Copy, Debug)]
pub struct DeviceKeyEpoch {
    pub epoch: i64,
    pub public_key: [u8; 32],
    /// The sealed-event id at/after which this key signs entries (0 for genesis).
    pub activated_at_event_id: i64,
}

/// Exported events omit correlation tokens to avoid identity-like fields.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportEvent {
    pub event_type: EventType,
    pub time_bucket: TimeBucket,
    pub zone_id: String,
    pub confidence: f32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attestation: Option<Attestation>,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportFailureEvent {
    pub failure_type: FailureType,
    pub time_bucket: TimeBucket,
    pub details: Option<String>,
    pub kernel_version: String,
    pub ruleset_id: String,
    pub ruleset_hash: [u8; 32],
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportBucket {
    pub time_bucket: TimeBucket,
    pub events: Vec<ExportEvent>,
    pub failures: Vec<ExportFailureEvent>,
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

/// How an export was authorized. Recorded on the signed export receipt so a
/// verifier can distinguish owner-authorized disclosure from trustee-quorum
/// disclosure. Absent on receipts written before this field existed.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ExportAuthMode {
    /// Trustee-quorum break-glass token (`export:events` envelope).
    BreakGlass,
    /// Owner self-export, authenticated by possession of the device key seed.
    SelfExport,
    /// Local capability-token API access.
    Api,
}

/// Half-open time window `[start_epoch_s, end_epoch_s)` restricting an export
/// to records whose true (pre-jitter) bucket start falls inside it. Both
/// bounds MUST be aligned to the 600 s bucket grid so the window itself never
/// discloses finer-than-bucket timing.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ExportWindow {
    pub start_epoch_s: u64,
    pub end_epoch_s: u64,
}

impl ExportWindow {
    /// Build a bucket-aligned window from raw epoch bounds: start floored,
    /// end ceiled to the 600 s grid, so callers can pass arbitrary times and
    /// the window still discloses nothing finer than the buckets.
    pub fn aligned(raw_start: u64, raw_end: u64) -> Result<Self> {
        if raw_start >= raw_end {
            return Err(anyhow!("export window start must be before end"));
        }
        let bucket = u64::from(TEN_MINUTES_S);
        let end_epoch_s = raw_end
            .div_ceil(bucket)
            .checked_mul(bucket)
            .ok_or_else(|| anyhow!("export window end is too large"))?;
        Ok(Self {
            start_epoch_s: raw_start / bucket * bucket,
            end_epoch_s,
        })
    }

    /// The trailing window covering the last `duration_s` seconds from now,
    /// bucket-aligned outward.
    pub fn last(duration_s: u64) -> Result<Self> {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)?
            .as_secs();
        Self::aligned(now.saturating_sub(duration_s), now)
    }
}

/// Parse `24h` / `7d` / `90m` / `3600s` / `3600` into seconds (shared by the
/// export CLI's `--last` and the event API's `?last=` query parameter).
pub fn parse_duration_s(s: &str) -> Result<u64> {
    let s = s.trim();
    let (num, mult) = match s.chars().last() {
        Some('s') => (&s[..s.len() - 1], 1),
        Some('m') => (&s[..s.len() - 1], 60),
        Some('h') => (&s[..s.len() - 1], 3600),
        Some('d') => (&s[..s.len() - 1], 86_400),
        Some(c) if c.is_ascii_digit() => (s, 1),
        _ => return Err(anyhow!("invalid duration '{}': use e.g. 24h, 7d, 90m", s)),
    };
    let n: u64 = num
        .parse()
        .map_err(|_| anyhow!("invalid duration '{}': use e.g. 24h, 7d, 90m", s))?;
    n.checked_mul(mult)
        .ok_or_else(|| anyhow!("duration '{}' overflows", s))
}

// New receipt fields must be `Option` + `default` + `skip_serializing_if` and
// stay LAST: the envelope verifiers (Rust `envelope.rs` and JS
// `viewer/verify_core.js`) re-serialize this struct byte-for-byte to recompute
// the receipt entry hash, so field order and absence semantics are part of the
// signed format. Absent => legacy receipt.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportReceipt {
    pub time_bucket: TimeBucket,
    pub ruleset_hash: [u8; 32],
    pub batch_size: usize,
    pub artifact_hash: [u8; 32],
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub auth_mode: Option<ExportAuthMode>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub window: Option<ExportWindow>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportReceiptEntry {
    pub receipt: ExportReceipt,
    pub prev_hash: [u8; 32],
    pub entry_hash: [u8; 32],
    pub signatures: SignatureSet,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ExportBundle {
    pub artifact: ExportArtifact,
    pub receipt_entry: ExportReceiptEntry,
    pub device_public_key: [u8; 32],
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pq_public_key: Option<Vec<u8>>,
}

#[derive(Clone, Copy, Debug)]
pub struct ExportOptions {
    pub max_events_per_batch: usize,
    pub jitter_s: u64,
    pub jitter_step_s: u64,
    /// Optional bucket-aligned time window restricting which records are
    /// exported. `None` exports the full retained history.
    pub window: Option<ExportWindow>,
}

impl Default for ExportOptions {
    fn default() -> Self {
        Self {
            max_events_per_batch: 50,
            jitter_s: 120,
            jitter_step_s: 60,
            window: None,
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
    let re = ZONE_ID_RE.get_or_init(|| {
        regex::Regex::new(r"^zone:[a-z0-9_-]{1,64}$").expect("valid zone ID regex")
    });

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

        // Coarsen timestamps to 10-minute buckets before logging.
        let time_bucket = c.time_bucket.coarsen_to(TEN_MINUTES_S)?;

        // Zone discipline (positive allowlist)
        validate_zone_id(&c.zone_id)?;

        // Correlation token constraint: only with <= 15 minute buckets
        if c.correlation_token.is_some() && c.time_bucket.size_s > FIFTEEN_MINUTES_S {
            return Err(anyhow!(
                "conformance: correlation token with oversized bucket"
            ));
        }

        Ok(Event {
            event_type: c.event_type,
            time_bucket,
            zone_id: c.zone_id,
            confidence: c.confidence,
            correlation_token: c.correlation_token,
            attestation: c.attestation,
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
///
/// Key material uses `zeroize::Zeroizing` wrapper to ensure automatic zeroization on drop
/// and prevent the compiler from optimizing away the zeroization.
pub struct BucketKeyManager {
    current_bucket: Option<TimeBucket>,
    key: zeroize::Zeroizing<[u8; 32]>,
    has_key: bool,
}

impl BucketKeyManager {
    pub fn new() -> Self {
        Self {
            current_bucket: None,
            key: zeroize::Zeroizing::new([0u8; 32]),
            has_key: false,
        }
    }

    pub fn rotate_if_needed(&mut self, bucket: TimeBucket) {
        if self.current_bucket == Some(bucket) && self.has_key {
            return;
        }

        // Destroy previous key material (Zeroizing handles zeroization)
        self.key = zeroize::Zeroizing::new([0u8; 32]);
        self.has_key = false;

        // Generate a new per-bucket key
        rand::fill(&mut self.key[..]);
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
        hasher.update(self.key.as_ref());
        hasher.update(features_hash);
        Ok(hasher.finalize().into())
    }

    /// For conformance tests: prove key rotation occurred (current key differs from old).
    pub fn export_key_for_test_only(&self) -> [u8; 32] {
        *self.key
    }
}

impl Default for BucketKeyManager {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for BucketKeyManager {
    fn drop(&mut self) {
        // Zeroizing wrapper already handles zeroization on drop,
        // but we also clear the has_key flag explicitly.
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
    #[cfg(feature = "pqc-signatures")]
    device_pq_key: Option<PqKeypair>,
    break_glass_policy: Option<crate::break_glass::QuorumPolicy>,
    zone_policy: ZonePolicy,
}

struct SignatureKeyMaterial {
    device_key: SigningKey,
    #[cfg(feature = "pqc-signatures")]
    pq_secret_key: Option<PqSecretKey>,
}

impl SignatureKeyMaterial {
    fn signature_keys(&self) -> SignatureKeys<'_> {
        #[cfg(feature = "pqc-signatures")]
        {
            return SignatureKeys::with_pq(&self.device_key, self.pq_secret_key.as_ref());
        }
        #[cfg(not(feature = "pqc-signatures"))]
        {
            SignatureKeys::new(&self.device_key)
        }
    }
}

impl Kernel {
    pub fn open(cfg: &KernelConfig) -> Result<Self> {
        let db_path = if cfg.db_path == ":memory:" {
            shared_memory_uri()
        } else {
            cfg.db_path.clone()
        };
        let device_key = signing_key_from_seed(&cfg.device_key_seed)?;
        let db_key = resolve_db_encryption_key(
            &device_key,
            db_key_seed_from_env().as_ref().map(|s| s.as_str()),
        );
        let conn = open_db_connection_with_key(&db_path, Some(&db_key))?;
        let sealed_log = Box::new(SqliteSealedLogStore::open_with_key(
            &db_path,
            Some(&db_key),
        )?);
        let zone_policy = cfg.zone_policy.normalized()?;
        let mut k = Self {
            conn,
            sealed_log,
            device_key,
            #[cfg(feature = "pqc-signatures")]
            device_pq_key: None,
            break_glass_policy: None,
            zone_policy,
        };
        k.ensure_schema()?;
        k.ensure_device_public_key()?;
        #[cfg(feature = "pqc-signatures")]
        k.ensure_device_pq_keys()?;
        k.load_break_glass_policy()?;
        Ok(k)
    }

    pub fn open_with_sealed_log(
        cfg: &KernelConfig,
        sealed_log: Box<dyn SealedLogStore>,
    ) -> Result<Self> {
        if cfg.db_path == ":memory:" {
            return Err(anyhow!(concat!(
                "Using ':memory:' with `open_with_sealed_log` is ambiguous and not supported. ",
                "For shared in-memory databases, call `shared_memory_uri()` ",
                "and pass it explicitly in `KernelConfig::db_path`."
            )));
        }
        let device_key = signing_key_from_seed(&cfg.device_key_seed)?;
        let db_key = resolve_db_encryption_key(
            &device_key,
            db_key_seed_from_env().as_ref().map(|s| s.as_str()),
        );
        let conn = open_db_connection_with_key(&cfg.db_path, Some(&db_key))?;
        let zone_policy = cfg.zone_policy.normalized()?;
        let mut k = Self {
            conn,
            sealed_log,
            device_key,
            #[cfg(feature = "pqc-signatures")]
            device_pq_key: None,
            break_glass_policy: None,
            zone_policy,
        };
        k.ensure_schema()?;
        k.ensure_device_public_key()?;
        #[cfg(feature = "pqc-signatures")]
        k.ensure_device_pq_keys()?;
        k.load_break_glass_policy()?;
        Ok(k)
    }

    pub fn ensure_schema(&mut self) -> Result<()> {
        self.conn.execute_batch(
            r#"
            PRAGMA journal_mode=WAL;
            -- SD-card endurance: truncate the WAL back to 4 MB after
            -- checkpoints so it cannot grow unbounded and amplify rewrites.
            PRAGMA journal_size_limit=4194304;

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

            CREATE TABLE IF NOT EXISTS policy_change_history (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              created_at INTEGER NOT NULL,
              payload_json TEXT NOT NULL,
              approvals_json TEXT NOT NULL DEFAULT '[]',
              prev_hash BLOB NOT NULL,
              entry_hash BLOB NOT NULL,
              signature BLOB NOT NULL,
              pq_signature BLOB,
              pq_scheme TEXT
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

            CREATE TABLE IF NOT EXISTS device_key_history (
              epoch INTEGER PRIMARY KEY,
              public_key BLOB NOT NULL,
              prev_public_key BLOB,
              activated_at_event_id INTEGER NOT NULL,
              attestation BLOB,
              authorization BLOB
            );

            CREATE TABLE IF NOT EXISTS consumed_break_glass_tokens (
              token_nonce BLOB PRIMARY KEY,
              consumed_at INTEGER NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_events_created ON sealed_events(created_at);
            CREATE INDEX IF NOT EXISTS idx_receipts_created ON break_glass_receipts(created_at);
            CREATE INDEX IF NOT EXISTS idx_export_receipts_created ON export_receipts(created_at);
            "#,
        )?;
        self.ensure_break_glass_receipts_columns()?;
        ensure_columns(
            &self.conn,
            "sealed_events",
            &[("pq_signature", "BLOB"), ("pq_scheme", "TEXT")],
        )?;
        ensure_columns(
            &self.conn,
            "checkpoints",
            &[
                ("pq_signature", "BLOB"),
                ("pq_scheme", "TEXT"),
                // Records which device key signed the checkpoint, so verification picks the
                // correct key after a signing-key rotation (NULL on pre-rotation databases).
                ("signer_public_key", "BLOB"),
            ],
        )?;
        ensure_columns(
            &self.conn,
            "device_key_history",
            &[("authorization", "BLOB")],
        )?;
        ensure_columns(
            &self.conn,
            "break_glass_receipts",
            &[("pq_signature", "BLOB"), ("pq_scheme", "TEXT")],
        )?;
        ensure_columns(
            &self.conn,
            "export_receipts",
            &[("pq_signature", "BLOB"), ("pq_scheme", "TEXT")],
        )?;
        self.ensure_device_metadata_columns()?;
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

    fn ensure_device_metadata_columns(&mut self) -> Result<()> {
        ensure_columns(
            &self.conn,
            "device_metadata",
            &[("pq_public_key", "BLOB"), ("pq_secret_key", "BLOB")],
        )
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
            // `device_metadata.public_key` pins the immutable *genesis* identity (the
            // verification anchor). The *current* signing key may differ after one or more
            // rotations; reopen must validate the seed against the current key, not genesis.
            self.backfill_genesis_key_history(&bytes)?;
            let current = current_device_public_key(&self.conn)?;
            if current != key_bytes {
                return Err(anyhow!(
                    "device public key mismatch: DEVICE_KEY_SEED does not derive the current \
                     device key (a retired key cannot reopen the log; use the latest seed)"
                ));
            }
            return Ok(());
        }

        // First open: record the genesis identity as both the anchor and key-history epoch 0.
        self.conn.execute(
            "INSERT INTO device_metadata (id, public_key) VALUES (1, ?1)",
            params![key_bytes.to_vec()],
        )?;
        self.conn.execute(
            "INSERT OR IGNORE INTO device_key_history \
             (epoch, public_key, prev_public_key, activated_at_event_id, attestation) \
             VALUES (0, ?1, NULL, 0, NULL)",
            params![key_bytes.to_vec()],
        )?;
        Ok(())
    }

    /// Backfill the genesis key-history row for databases created before key-history
    /// existed, so the current-key lookup and verifier seeding work uniformly.
    fn backfill_genesis_key_history(&mut self, genesis_public_key: &[u8]) -> Result<()> {
        let has_history: bool = self.conn.query_row(
            "SELECT EXISTS(SELECT 1 FROM device_key_history)",
            [],
            |row| row.get(0),
        )?;
        if !has_history {
            self.conn.execute(
                "INSERT INTO device_key_history \
                 (epoch, public_key, prev_public_key, activated_at_event_id, attestation) \
                 VALUES (0, ?1, NULL, 0, NULL)",
                params![genesis_public_key.to_vec()],
            )?;
        }
        Ok(())
    }

    /// Rotate the device signing identity.
    ///
    /// Appends a hash-chained, **old-key-signed** [`KeyRotation`] record carrying the new
    /// key's possession attestation, records the new key in the durable key history, and
    /// switches the kernel's active signing key. Subsequent events are signed by the new
    /// key; verifiers following the chain from genesis pick up the new key at this record.
    /// Reopen the database afterwards with the new seed.
    pub fn rotate_device_identity(&mut self, new_seed: &str) -> Result<()> {
        let new_key = signing_key_from_seed(new_seed)?;
        let prev_public_key = self.device_key.verifying_key().to_bytes();
        let new_public_key = new_key.verifying_key().to_bytes();
        if prev_public_key == new_public_key {
            return Err(anyhow!(
                "device key rotation: new seed derives the current key (nothing to rotate)"
            ));
        }

        // Possession proof by the NEW key, authorization by the retiring (CURRENT) key. The
        // authorization is the genesis-anchored link that keeps the lineage trustworthy in the
        // durable history table even after the in-chain record is pruned.
        let attestation =
            sign_rotation_attestation(&new_key, &prev_public_key, &new_public_key).to_vec();
        let authorization =
            sign_rotation_authorization(&self.device_key, &prev_public_key, &new_public_key)
                .to_vec();
        let rotation = KeyRotation {
            schema: KEY_ROTATION_SCHEMA_V1.to_string(),
            prev_public_key,
            new_public_key,
            new_key_attestation: attestation.clone(),
            prev_key_authorization: authorization.clone(),
            time_bucket: Self::coarsen_or_now(TimeBucket::now_10min()?)?,
        };

        // Append signed by the CURRENT (retiring) key — proves the rotation was authorized
        // by the legitimate holder and chains it into the tamper-evident log.
        self.append_rotation(&rotation)?;

        // The rotation record is the new chain head; capture its row id for the lineage.
        let event_id: i64 = self.conn.query_row(
            "SELECT COALESCE(MAX(id), 0) FROM sealed_events",
            [],
            |row| row.get(0),
        )?;
        let next_epoch: i64 = self.conn.query_row(
            "SELECT COALESCE(MAX(epoch), -1) + 1 FROM device_key_history",
            [],
            |row| row.get(0),
        )?;
        self.conn.execute(
            "INSERT INTO device_key_history \
             (epoch, public_key, prev_public_key, activated_at_event_id, attestation, authorization) \
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![
                next_epoch,
                new_public_key.to_vec(),
                prev_public_key.to_vec(),
                event_id,
                attestation,
                authorization,
            ],
        )?;

        // Switch the active signing key. Genesis (device_metadata) stays as the anchor.
        self.device_key = new_key;
        Ok(())
    }

    fn append_rotation(&mut self, rotation: &KeyRotation) -> Result<()> {
        let record = SealedLogRecord::KeyRotation(rotation.clone());
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log.append_record(&record, &signature_keys)
    }

    #[cfg(feature = "pqc-signatures")]
    fn ensure_device_pq_keys(&mut self) -> Result<()> {
        let row: Option<(Option<Vec<u8>>, Option<Vec<u8>>)> = self
            .conn
            .query_row(
                "SELECT pq_public_key, pq_secret_key FROM device_metadata WHERE id = 1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;

        if let Some((Some(public_key), Some(secret_key))) = row {
            let keypair = PqKeypair::from_bytes(&public_key, &secret_key)?;
            self.device_pq_key = Some(keypair);
            return Ok(());
        }

        let keypair = PqKeypair::generate();
        let public_key = keypair.public_key_bytes();
        let secret_key = keypair.secret_key_bytes();
        self.conn.execute(
            "UPDATE device_metadata SET pq_public_key = ?1, pq_secret_key = ?2 WHERE id = 1",
            params![public_key, secret_key],
        )?;
        self.device_pq_key = Some(keypair);
        Ok(())
    }

    fn signature_key_material(&self) -> SignatureKeyMaterial {
        #[cfg(feature = "pqc-signatures")]
        {
            return SignatureKeyMaterial {
                device_key: self.device_key.clone(),
                pq_secret_key: self.device_pq_key.as_ref().map(|key| key.secret_key),
            };
        }
        #[cfg(not(feature = "pqc-signatures"))]
        {
            SignatureKeyMaterial {
                device_key: self.device_key.clone(),
            }
        }
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
        let record = SealedLogRecord::Event(ev.clone());
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log.append_record(&record, &signature_keys)
    }

    fn append_failure(&mut self, failure: &FailureEvent) -> Result<()> {
        let record = SealedLogRecord::Failure(failure.clone());
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log.append_record(&record, &signature_keys)
    }

    fn coarsen_or_now(bucket: TimeBucket) -> Result<TimeBucket> {
        bucket
            .coarsen_to(TEN_MINUTES_S)
            .or_else(|_| TimeBucket::now_10min())
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
            let failure = FailureEvent {
                failure_type: FailureType::GapMissingData,
                time_bucket: Self::coarsen_or_now(cand.time_bucket)?,
                details: Some(format!("CONFORMANCE_MODULE_ALLOWLIST: {}", e)),
                kernel_version: "UNBOUND".to_string(),
                ruleset_id: "UNBOUND".to_string(),
                ruleset_hash: [0u8; 32],
            }
            .bind(kernel_version, ruleset_id, ruleset_hash);
            self.append_failure(&failure)?;
            return Err(e);
        }

        let cand_bucket = cand.time_bucket;
        let ev = match ContractEnforcer::enforce(cand) {
            Ok(ev) => ev,
            Err(e) => {
                self.log_alarm("CONFORMANCE_CONTRACT_REJECT", &format!("{}", e))?;
                let failure = FailureEvent {
                    failure_type: FailureType::GapMissingData,
                    time_bucket: Self::coarsen_or_now(cand_bucket)?,
                    details: Some(format!("CONFORMANCE_CONTRACT_REJECT: {}", e)),
                    kernel_version: "UNBOUND".to_string(),
                    ruleset_id: "UNBOUND".to_string(),
                    ruleset_hash: [0u8; 32],
                }
                .bind(kernel_version, ruleset_id, ruleset_hash);
                self.append_failure(&failure)?;
                return Err(e);
            }
        };

        if self.zone_policy.is_sensitive(&ev.zone_id) {
            let err = anyhow!("conformance: sensitive zone rejected by policy");
            self.log_alarm("CONFORMANCE_ZONE_POLICY_REJECT", &format!("{}", err))?;
            let failure = FailureEvent {
                failure_type: FailureType::GapMissingData,
                time_bucket: ev.time_bucket,
                details: Some(format!("CONFORMANCE_ZONE_POLICY_REJECT: {}", err)),
                kernel_version: "UNBOUND".to_string(),
                ruleset_id: "UNBOUND".to_string(),
                ruleset_hash: [0u8; 32],
            }
            .bind(kernel_version, ruleset_id, ruleset_hash);
            self.append_failure(&failure)?;
            return Err(err);
        }

        let ev = ev.bind(kernel_version, ruleset_id, ruleset_hash);
        self.append_event_with_failure_semantics(&ev, kernel_version, ruleset_id, ruleset_hash)?;
        Ok(ev)
    }

    pub fn append_failure_event(
        &mut self,
        failure_type: FailureType,
        time_bucket: TimeBucket,
        details: Option<String>,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) -> Result<FailureEvent> {
        let time_bucket = time_bucket.coarsen_to(TEN_MINUTES_S)?;
        let failure = FailureEvent {
            failure_type,
            time_bucket,
            details,
            kernel_version: "UNBOUND".to_string(),
            ruleset_id: "UNBOUND".to_string(),
            ruleset_hash: [0u8; 32],
        }
        .bind(kernel_version, ruleset_id, ruleset_hash);
        self.append_failure(&failure)?;
        Ok(failure)
    }

    /// Seals a periodic heartbeat (system trace) record into the chain.
    ///
    /// Called once per coarse time bucket; anchors the chain tail so that
    /// truncation of recent entries is detectable by verifiers.
    #[allow(clippy::too_many_arguments)]
    pub fn append_heartbeat(
        &mut self,
        time_bucket: TimeBucket,
        ingest_healthy: bool,
        frames_captured_delta: u64,
        events_appended_delta: u64,
        failures_appended_delta: u64,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) -> Result<()> {
        let heartbeat = HeartbeatRecord {
            time_bucket: time_bucket.coarsen_to(TEN_MINUTES_S)?,
            ingest_healthy,
            frames_captured_delta,
            events_appended_delta,
            failures_appended_delta,
            kernel_version: kernel_version.to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash,
        };
        let record = SealedLogRecord::Heartbeat(heartbeat);
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log.append_record(&record, &signature_keys)
    }

    /// Seals a daemon lifecycle record (start / clean shutdown) into the chain.
    pub fn append_lifecycle(
        &mut self,
        phase: LifecyclePhase,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) -> Result<()> {
        let lifecycle = LifecycleRecord {
            phase,
            time_bucket: TimeBucket::now_10min()?,
            kernel_version: kernel_version.to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash,
        };
        let record = SealedLogRecord::Lifecycle(lifecycle);
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log.append_record(&record, &signature_keys)
    }

    /// Returns the phase of the most recent lifecycle record, if any.
    ///
    /// Used at boot for unclean-shutdown detection: a trailing `Start` with no
    /// `ShutdownClean` means the previous run died without sealing a shutdown
    /// record (power loss, crash, or kill).
    pub fn last_lifecycle_phase(&self) -> Result<Option<LifecyclePhase>> {
        // Anchored to the start of the payload: serde serializes the tag field
        // first, so this cannot false-positive on records whose free-form
        // `details` text merely mentions the tag.
        let mut stmt = self.conn.prepare(
            "SELECT payload_json FROM sealed_events \
             WHERE payload_json LIKE '{\"record_type\":\"lifecycle\"%' \
             ORDER BY id DESC LIMIT 1",
        )?;
        let mut rows = stmt.query([])?;
        let Some(row) = rows.next()? else {
            return Ok(None);
        };
        let payload: String = row.get(0)?;
        // The LIKE filter is a coarse pre-filter; confirm via real deserialization.
        match SealedLogRecord::deserialize_compat(&payload)? {
            SealedLogRecord::Lifecycle(l) => Ok(Some(l.phase)),
            _ => Ok(None),
        }
    }

    /// Attempts to record a failure event with graceful degradation.
    ///
    /// When the primary storage fails, this method:
    /// 1. Tries to record the failure event to the sealed log
    /// 2. If that fails, logs an alarm to the conformance_alarms table
    /// 3. If that also fails, logs to stderr as a last resort
    ///
    /// This implements the fail-closed auditability guarantee from failure_semantics.md:
    /// failure conditions must produce explicit failure events, not silent gaps.
    fn try_record_failure_best_effort(
        &mut self,
        failure_type: FailureType,
        details: Option<String>,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        let time_bucket = match TimeBucket::now_10min() {
            Ok(tb) => tb,
            Err(e) => {
                eprintln!(
                    "[FAILURE_SEMANTICS] Cannot get time bucket for {:?}: {}",
                    failure_type, e
                );
                return;
            }
        };

        let failure = FailureEvent {
            failure_type: failure_type.clone(),
            time_bucket,
            details: details.clone(),
            kernel_version: "UNBOUND".to_string(),
            ruleset_id: "UNBOUND".to_string(),
            ruleset_hash: [0u8; 32],
        }
        .bind(kernel_version, ruleset_id, ruleset_hash);

        // Try to record to sealed log
        if let Err(e) = self.append_failure(&failure) {
            // Sealed log write failed, try alarm table
            let alarm_msg = format!(
                "{:?}: {} (sealed log write failed: {})",
                failure_type,
                details.as_deref().unwrap_or("no details"),
                e
            );

            if let Err(alarm_err) = self.log_alarm("FAILURE_EVENT_DEGRADED", &alarm_msg) {
                // Even alarm table failed, last resort: stderr
                eprintln!(
                    "[FAILURE_SEMANTICS] {:?}: {} (both sealed log and alarm failed: {}, {})",
                    failure_type,
                    details.as_deref().unwrap_or("no details"),
                    e,
                    alarm_err
                );
            }
        }
    }

    /// Classifies an error as crypto-related based on error message patterns.
    fn is_crypto_error(err: &anyhow::Error) -> bool {
        let msg = err.to_string().to_lowercase();
        msg.contains("signature")
            || msg.contains("signing")
            || msg.contains("verification")
            || msg.contains("crypto")
            || msg.contains("key")
            || msg.contains("decrypt")
            || msg.contains("encrypt")
    }

    /// Reports a storage write failure per failure_semantics.md.
    ///
    /// Call this when a storage operation fails to ensure explicit failure events
    /// are recorded for auditability.
    pub fn report_storage_failure(
        &mut self,
        details: &str,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        self.try_record_failure_best_effort(
            FailureType::StorageWriteFailed,
            Some(details.to_string()),
            kernel_version,
            ruleset_id,
            ruleset_hash,
        );
    }

    /// Reports a cryptographic failure per failure_semantics.md.
    ///
    /// Call this when a cryptographic operation (signing, verification, key loading) fails
    /// to ensure explicit failure events are recorded for auditability.
    pub fn report_crypto_failure(
        &mut self,
        details: &str,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        self.try_record_failure_best_effort(
            FailureType::CryptoFailure,
            Some(details.to_string()),
            kernel_version,
            ruleset_id,
            ruleset_hash,
        );
    }

    /// Reports clock desynchronization per failure_semantics.md.
    ///
    /// Call this when monotonic-vs-wallclock drift exceeds tolerance or the
    /// coarse time bucket regresses.
    pub fn report_clock_skew(
        &mut self,
        details: &str,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) {
        self.try_record_failure_best_effort(
            FailureType::ClockSkew,
            Some(details.to_string()),
            kernel_version,
            ruleset_id,
            ruleset_hash,
        );
    }

    /// Appends an event to the sealed log, emitting failure events on error.
    ///
    /// This is the fail-closed wrapper around `append_event` that ensures storage
    /// and crypto failures are explicitly recorded per failure_semantics.md.
    pub fn append_event_with_failure_semantics(
        &mut self,
        ev: &Event,
        kernel_version: &str,
        ruleset_id: &str,
        ruleset_hash: [u8; 32],
    ) -> Result<()> {
        match self.append_event(ev) {
            Ok(()) => Ok(()),
            Err(e) => {
                let (failure_type, alarm_code) = if Self::is_crypto_error(&e) {
                    (FailureType::CryptoFailure, "CRYPTO_FAILURE")
                } else {
                    (FailureType::StorageWriteFailed, "STORAGE_WRITE_FAILED")
                };

                let details = format!("append_event failed: {}", e);

                // Log alarm first (simpler write, more likely to succeed)
                let _ = self.log_alarm(alarm_code, &details);

                // Try to record failure event
                self.try_record_failure_best_effort(
                    failure_type,
                    Some(details),
                    kernel_version,
                    ruleset_id,
                    ruleset_hash,
                );

                Err(e)
            }
        }
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
        let key_material = self.signature_key_material();
        let signature_set = sign_entry(
            &key_material.signature_keys(),
            &entry_hash,
            DOMAIN_BREAK_GLASS_RECEIPT,
        )?;
        let pq_signature = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.signature.clone());
        let pq_scheme = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.scheme_id.clone());

        self.conn.execute(
            r#"
            INSERT INTO break_glass_receipts(
                created_at,
                payload_json,
                approvals_json,
                prev_hash,
                entry_hash,
                signature,
                pq_signature,
                pq_scheme
            )
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
            "#,
            params![
                created_at,
                payload_json,
                approvals_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature_set.ed25519_signature,
                pq_signature,
                pq_scheme,
            ],
        )?;

        Ok(entry_hash)
    }

    pub fn append_export_receipt(&mut self, receipt: &ExportReceipt) -> Result<[u8; 32]> {
        let created_at = now_s()? as i64;
        let prev_hash = self.last_export_receipt_hash_or_zero()?;
        let payload_json = serde_json::to_string(receipt)?;

        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        let key_material = self.signature_key_material();
        let signature_set = sign_entry(
            &key_material.signature_keys(),
            &entry_hash,
            DOMAIN_EXPORT_RECEIPT,
        )?;
        let pq_signature = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.signature.clone());
        let pq_scheme = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.scheme_id.clone());

        self.conn.execute(
            r#"
            INSERT INTO export_receipts(
                created_at,
                payload_json,
                prev_hash,
                entry_hash,
                signature,
                pq_signature,
                pq_scheme
            )
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
            "#,
            params![
                created_at,
                payload_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature_set.ed25519_signature,
                pq_signature,
                pq_scheme,
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
            "SELECT payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM export_receipts ORDER BY id DESC LIMIT 1",
        )?;
        let row = stmt
            .query_row([], |row| {
                let payload: String = row.get(0)?;
                let prev_hash: Vec<u8> = row.get(1)?;
                let entry_hash: Vec<u8> = row.get(2)?;
                let signature: Vec<u8> = row.get(3)?;
                let pq_signature: Option<Vec<u8>> = row.get(4)?;
                let pq_scheme: Option<String> = row.get(5)?;
                Ok((
                    payload,
                    prev_hash,
                    entry_hash,
                    signature,
                    pq_signature,
                    pq_scheme,
                ))
            })
            .optional()?;

        let Some((payload, prev_hash, entry_hash, signature, pq_signature, pq_scheme)) = row else {
            return Err(anyhow!("export receipt not found"));
        };

        let prev_hash = blob32(prev_hash, "export_receipts.prev_hash")?;
        let entry_hash = blob32(entry_hash, "export_receipts.entry_hash")?;
        let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
        let receipt: ExportReceipt = serde_json::from_str(&payload)?;
        Ok(ExportReceiptEntry {
            receipt,
            prev_hash,
            entry_hash,
            signatures: signature_set,
        })
    }

    /// RAW policy write: replaces the stored quorum policy with no quorum
    /// check and no history record. For provisioning fresh databases and
    /// fixtures only — every user-facing mutation path (CLI `policy set`,
    /// guided setup, the break-glass backend) MUST go through
    /// [`Kernel::set_break_glass_policy_gated`] so a live roster can only be
    /// changed with its own consent (Invariant V).
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

    /// Replace the stored quorum policy through the QUORUM-GATED path
    /// (Invariant V; spec/quorum_unseal_v2.md §3.1): when a policy already
    /// exists and differs, the change requires `current.n` distinct
    /// current-trustee approvals over the policy-change hash, signed under
    /// `DOMAIN_POLICY_CHANGE_APPROVAL`. On an empty database the change is a
    /// bootstrap (there is no quorum yet to consult) and is recorded as such.
    /// Every accepted change appends a chained, device-signed history record.
    ///
    /// Honest scope note: this is a procedural + auditability control inside
    /// the host-trust boundary, not a cryptographic lock — an actor with host
    /// access and the device seed can still rewrite the policy row out of
    /// band, but such a rewrite leaves no valid history record for an audit
    /// to find. The cryptographic lock is the threshold-custody tier
    /// (`docs/security/ENTERPRISE_CUSTODY.md` §1).
    pub fn set_break_glass_policy_gated(
        &mut self,
        new_policy: &crate::break_glass::QuorumPolicy,
        approvals: &[crate::break_glass::Approval],
        now_bucket: TimeBucket,
    ) -> Result<PolicyChangeOutcome> {
        new_policy.validate()?;
        let current = self.break_glass_policy.clone();

        let (prev_commitment, bootstrap) = match &current {
            None => ([0u8; 32], true),
            Some(cur) => {
                if cur.full_commitment() == new_policy.full_commitment() {
                    return Ok(PolicyChangeOutcome::Unchanged);
                }
                (cur.full_commitment(), false)
            }
        };

        let proposal = crate::break_glass::PolicyChangeProposal {
            prev_policy_commitment: prev_commitment,
            new_policy: new_policy.clone(),
            time_bucket: now_bucket,
        };
        let change_hash = proposal.change_hash();

        if let Some(cur) = &current {
            let valid = crate::break_glass::count_valid_distinct_policy_change_approvals(
                cur,
                &change_hash,
                approvals,
            );
            if valid < cur.n as usize {
                return Err(anyhow!(
                    "policy change denied: {} of {} required approvals from the CURRENT quorum \
                     are valid — collect consent with `break_glass policy propose` and \
                     `break_glass policy approve`, then re-run with --approvals",
                    valid,
                    cur.n
                ));
            }
        }

        let record = PolicyChangeRecord {
            prev_policy: current,
            new_policy: new_policy.clone(),
            change_hash,
            time_bucket: now_bucket,
            bootstrap,
            // Bind the authorizing approvals into the signed, hash-chained
            // record: their commitment travels inside payload_json, so a
            // tampered `approvals_json` fails verification.
            approvals_commitment: crate::break_glass::approvals_commitment(approvals),
        };

        // History row and policy row land atomically: a change that cannot be
        // recorded is a change that does not happen (fail closed).
        self.conn.execute_batch("SAVEPOINT policy_change;")?;
        let applied = self.append_policy_change_record(&record, approvals);
        let applied = applied.and_then(|_| {
            let json = serde_json::to_string(new_policy)?;
            self.conn.execute(
                "INSERT OR REPLACE INTO break_glass_policy (id, policy_json) VALUES (1, ?1)",
                params![json],
            )?;
            Ok(())
        });
        match applied {
            Ok(()) => {
                self.conn.execute_batch("RELEASE policy_change;")?;
                self.break_glass_policy = Some(new_policy.clone());
                Ok(if bootstrap {
                    PolicyChangeOutcome::Bootstrapped
                } else {
                    PolicyChangeOutcome::Replaced
                })
            }
            Err(e) => {
                let _ = self
                    .conn
                    .execute_batch("ROLLBACK TO policy_change; RELEASE policy_change;");
                Err(e)
            }
        }
    }

    fn last_policy_change_hash_or_zero(&self) -> Result<[u8; 32]> {
        let mut stmt = self
            .conn
            .prepare("SELECT entry_hash FROM policy_change_history ORDER BY id DESC LIMIT 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let entry_bytes: Vec<u8> = row.get(0)?;
            blob32(entry_bytes, "policy_change_history.entry_hash")
        } else {
            Ok([0u8; 32])
        }
    }

    fn append_policy_change_record(
        &mut self,
        record: &PolicyChangeRecord,
        approvals: &[crate::break_glass::Approval],
    ) -> Result<[u8; 32]> {
        let created_at = now_s()? as i64;
        let prev_hash = self.last_policy_change_hash_or_zero()?;
        let payload_json = serde_json::to_string(record)?;
        let approvals_json = serde_json::to_string(approvals)?;

        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        let key_material = self.signature_key_material();
        let signature_set = sign_entry(
            &key_material.signature_keys(),
            &entry_hash,
            DOMAIN_POLICY_CHANGE_RECORD,
        )?;
        let pq_signature = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.signature.clone());
        let pq_scheme = signature_set
            .pq_signature
            .as_ref()
            .map(|sig| sig.scheme_id.clone());

        self.conn.execute(
            r#"
            INSERT INTO policy_change_history(
                created_at,
                payload_json,
                approvals_json,
                prev_hash,
                entry_hash,
                signature,
                pq_signature,
                pq_scheme
            )
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
            "#,
            params![
                created_at,
                payload_json,
                approvals_json,
                prev_hash.to_vec(),
                entry_hash.to_vec(),
                signature_set.ed25519_signature,
                pq_signature,
                pq_scheme,
            ],
        )?;

        Ok(entry_hash)
    }

    /// Read the policy-change history ledger in order. Auxiliary evidence
    /// (like the anchors table): chained and device-signed, but not part of
    /// the evidence envelope's ledgers.
    pub fn policy_change_history(&self) -> Result<Vec<PolicyChangeHistoryEntry>> {
        let mut stmt = self.conn.prepare(
            "SELECT payload_json, approvals_json, prev_hash, entry_hash, signature \
             FROM policy_change_history ORDER BY id ASC",
        )?;
        let mut rows = stmt.query([])?;
        let mut out = Vec::new();
        while let Some(row) = rows.next()? {
            let payload_json: String = row.get(0)?;
            let approvals_json: String = row.get(1)?;
            let prev_hash: Vec<u8> = row.get(2)?;
            let entry_hash: Vec<u8> = row.get(3)?;
            let signature: Vec<u8> = row.get(4)?;
            out.push(PolicyChangeHistoryEntry {
                record: serde_json::from_str(&payload_json)?,
                payload_json,
                approvals: serde_json::from_str(&approvals_json)?,
                prev_hash: blob32(prev_hash, "policy_change_history.prev_hash")?,
                entry_hash: blob32(entry_hash, "policy_change_history.entry_hash")?,
                signature,
            });
        }
        Ok(out)
    }

    fn load_break_glass_policy(&mut self) -> Result<()> {
        let mut stmt = self
            .conn
            .prepare("SELECT policy_json FROM break_glass_policy WHERE id = 1")?;
        let mut rows = stmt.query([])?;
        if let Some(row) = rows.next()? {
            let policy_json: String = row.get(0)?;
            let policy: crate::break_glass::QuorumPolicy = serde_json::from_str(&policy_json)?;
            policy.validate()?;
            self.break_glass_policy = Some(policy);
        } else {
            self.break_glass_policy = None;
        }
        Ok(())
    }

    /// Prune events older than retention. Writes a checkpoint so chain integrity remains verifiable.
    pub fn enforce_retention_with_checkpoint(&mut self, retention: Duration) -> Result<()> {
        let key_material = self.signature_key_material();
        let signature_keys = key_material.signature_keys();
        self.sealed_log
            .enforce_retention_with_checkpoint(retention, &signature_keys)
    }

    /// Verify the sealed log — the latest checkpoint signature and the event
    /// chain from that checkpoint through the tail — using the database's own
    /// trusted key lineage. Intended for a caller that holds a live [`Kernel`]
    /// and needs to confirm the log is intact, e.g. `witnessd`'s boot-time
    /// tail check before it extends the chain.
    ///
    /// Mirrors the `POST /verify` path (`SignatureMode::Compat`, keys taken
    /// from the trusted lineage). A structural failure — bad hash link, bad
    /// signature, untrusted checkpoint signer — is reported as
    /// `chain_valid: false`, not an `Err`; `Err` is reserved for being unable
    /// to attempt verification at all.
    pub fn verify_sealed_log(&self) -> Result<crate::verify_runner::VerifyReport> {
        crate::verify_runner::run_full_verify(&self.conn, None, None, SignatureMode::Compat, |_| {})
    }

    /// Read events from the sealed log for review or export.
    /// This is a *ruleset-bound* operation: attempting to read/interpret events under a different ruleset
    /// is treated as a conformance violation (Invariant VI).
    pub fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
    ) -> Result<Vec<SealedLogRecord>> {
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
        if let Some(window) = &options.window {
            if window.start_epoch_s % u64::from(TEN_MINUTES_S) != 0
                || window.end_epoch_s % u64::from(TEN_MINUTES_S) != 0
            {
                return Err(anyhow!(
                    "export window bounds must be aligned to {}s bucket boundaries",
                    TEN_MINUTES_S
                ));
            }
            if window.start_epoch_s >= window.end_epoch_s {
                return Err(anyhow!("export window start must be before end"));
            }
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
            |hash| {
                self.break_glass_receipt_outcome(
                    EXPORT_EVENTS_ENVELOPE_ID,
                    expected_ruleset_hash,
                    hash,
                )
            },
        )?;
        // Burn-first (durable): a token file re-parses as unconsumed, so
        // without this ledger a granted token could authorize repeated
        // exports across separate invocations within its validity bucket.
        self.consume_token_durably(token)?;
        let artifact = self.export_events_sequential_unchecked(
            expected_ruleset_hash,
            options,
            ExportAuthMode::BreakGlass,
        )?;
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
        self.export_events_sequential_unchecked(expected_ruleset_hash, options, ExportAuthMode::Api)
    }

    /// Owner self-export of the privacy-filtered event artifact (no break-glass).
    ///
    /// The caller is authenticated by possession of the device key seed, which
    /// is required to open/decrypt the database and to sign the export receipt
    /// — the same artifact the capability-token API already serves. A signed,
    /// hash-chained receipt labeled `self_export` is always appended
    /// (Invariant IV). Sealed-vault evidence and unsealing remain quorum-only.
    pub fn export_events_self(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
    ) -> Result<ExportArtifact> {
        Self::validate_export_options(&options)?;
        self.export_events_sequential_unchecked(
            expected_ruleset_hash,
            options,
            ExportAuthMode::SelfExport,
        )
    }

    pub fn export_events_bundle_authorized(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        token: &mut break_glass::BreakGlassToken,
    ) -> Result<ExportBundle> {
        let artifact = self.export_events_authorized(expected_ruleset_hash, options, token)?;
        self.bundle_from_artifact(artifact)
    }

    /// Owner self-export bundle (artifact + signed receipt + verifying keys).
    /// Mirrors [`Self::export_events_bundle_authorized`] without the quorum gate;
    /// see [`Self::export_events_self`] for the authorization rationale.
    pub fn export_events_bundle_self(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
    ) -> Result<ExportBundle> {
        let artifact = self.export_events_self(expected_ruleset_hash, options)?;
        self.bundle_from_artifact(artifact)
    }

    /// Bundle for local-only API access (no break-glass): same shape as the
    /// other bundles, receipt labeled `api` because the caller's credential is
    /// the capability token. The caller MUST enforce token access control.
    pub fn export_events_bundle_for_api(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
    ) -> Result<ExportBundle> {
        let artifact = self.export_events_for_api(expected_ruleset_hash, options)?;
        self.bundle_from_artifact(artifact)
    }

    fn bundle_from_artifact(&self, artifact: ExportArtifact) -> Result<ExportBundle> {
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
            pq_public_key: self.device_pq_public_key_for_verify_only(),
        })
    }

    /// Assemble a canonical [`EvidenceEnvelope`] (`profile: "full"`) from an already-built
    /// export bundle plus the device's four ledgers. See `spec/evidence_envelope.md`.
    pub fn evidence_envelope_from_bundle(
        &self,
        bundle: ExportBundle,
        ruleset_id: &str,
        kernel_version: &str,
        expected_ruleset_hash: [u8; 32],
    ) -> Result<EvidenceEnvelope> {
        let parts = envelope::EnvelopeParts {
            kernel_version: kernel_version.to_string(),
            ruleset_id: ruleset_id.to_string(),
            ruleset_hash: expected_ruleset_hash,
            device_public_key: bundle.device_public_key,
            pq_public_key: bundle.pq_public_key,
            artifact: bundle.artifact,
            export_receipt_entry: bundle.receipt_entry,
            sealed_events: envelope::read_sealed_events(&self.conn)?,
            break_glass_receipts: envelope::read_break_glass_receipts(&self.conn)?,
            export_receipts: envelope::read_export_receipts(&self.conn)?,
            checkpoint: envelope::read_checkpoint(&self.conn)?,
        };
        EvidenceEnvelope::assemble(parts)
    }

    /// Build a canonical evidence envelope under break-glass authorization (the deliberate,
    /// receipted disclosure path). Mirrors [`Self::export_events_bundle_authorized`].
    pub fn build_evidence_envelope_authorized(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        ruleset_id: &str,
        kernel_version: &str,
        token: &mut break_glass::BreakGlassToken,
    ) -> Result<EvidenceEnvelope> {
        let bundle = self.export_events_bundle_authorized(expected_ruleset_hash, options, token)?;
        self.evidence_envelope_from_bundle(
            bundle,
            ruleset_id,
            kernel_version,
            expected_ruleset_hash,
        )
    }

    /// Build a canonical evidence envelope for local-only API access (no break-glass).
    /// The caller MUST enforce capability-token access control.
    pub fn build_evidence_envelope_for_api(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        ruleset_id: &str,
        kernel_version: &str,
    ) -> Result<EvidenceEnvelope> {
        let artifact = self.export_events_for_api(expected_ruleset_hash, options)?;
        let receipt_entry = self.latest_export_receipt_entry()?;
        let bundle = ExportBundle {
            artifact,
            receipt_entry,
            device_public_key: self.device_key_for_verify_only(),
            pq_public_key: self.device_pq_public_key_for_verify_only(),
        };
        self.evidence_envelope_from_bundle(
            bundle,
            ruleset_id,
            kernel_version,
            expected_ruleset_hash,
        )
    }

    /// Build a canonical evidence envelope via owner self-export (no break-glass).
    /// See [`Self::export_events_self`] for the authorization rationale.
    pub fn build_evidence_envelope_self(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        ruleset_id: &str,
        kernel_version: &str,
    ) -> Result<EvidenceEnvelope> {
        let bundle = self.export_events_bundle_self(expected_ruleset_hash, options)?;
        self.evidence_envelope_from_bundle(
            bundle,
            ruleset_id,
            kernel_version,
            expected_ruleset_hash,
        )
    }

    /// Export events sequentially, grouped into coarse time buckets with batching and jitter.
    fn export_events_sequential_unchecked(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        options: ExportOptions,
        auth_mode: ExportAuthMode,
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
            let record = SealedLogRecord::deserialize_compat(&payload)?;

            // Key-rotation records are identity-administration entries, not exportable
            // semantic events; they carry no ruleset binding and are skipped here.
            // Heartbeat/lifecycle system-trace records are likewise non-semantic and
            // excluded from the export contract (events + failures only).
            match record {
                SealedLogRecord::KeyRotation(_)
                | SealedLogRecord::Heartbeat(_)
                | SealedLogRecord::Lifecycle(_) => continue,
                _ => {}
            }

            if let Err(e) =
                ReprocessGuard::assert_same_ruleset(expected_ruleset_hash, record.ruleset_hash())
            {
                self.log_alarm("CONFORMANCE_REPROCESS_VIOLATION", &format!("{}", e))?;
                return Err(e);
            }

            let record_bucket = record.time_bucket();
            // Window filtering uses the true (pre-jitter) bucket start so the
            // selection is exact; only bucket-aligned bounds are accepted, so
            // the window discloses nothing finer than the buckets themselves.
            if let Some(window) = &options.window {
                if record_bucket.start_epoch_s < window.start_epoch_s
                    || record_bucket.start_epoch_s >= window.end_epoch_s
                {
                    continue;
                }
            }
            let key = (record_bucket.start_epoch_s, record_bucket.size_s);
            let idx = if let Some(existing) = bucket_index.get(&key) {
                *existing
            } else {
                let jittered_bucket =
                    jitter_time_bucket(record_bucket, options.jitter_s, options.jitter_step_s)?;
                buckets.push(ExportBucket {
                    time_bucket: jittered_bucket,
                    events: Vec::new(),
                    failures: Vec::new(),
                });
                let idx = buckets.len() - 1;
                bucket_index.insert(key, idx);
                idx
            };
            match record {
                SealedLogRecord::Event(ev) => {
                    let export_event = ExportEvent {
                        event_type: ev.event_type,
                        time_bucket: buckets[idx].time_bucket,
                        zone_id: ev.zone_id,
                        confidence: ev.confidence,
                        attestation: ev.attestation,
                        kernel_version: ev.kernel_version,
                        ruleset_id: ev.ruleset_id,
                        ruleset_hash: ev.ruleset_hash,
                    };
                    buckets[idx].events.push(export_event);
                }
                SealedLogRecord::Failure(ev) => {
                    let export_failure = ExportFailureEvent {
                        failure_type: ev.failure_type,
                        time_bucket: buckets[idx].time_bucket,
                        details: ev.details,
                        kernel_version: ev.kernel_version,
                        ruleset_id: ev.ruleset_id,
                        ruleset_hash: ev.ruleset_hash,
                    };
                    buckets[idx].failures.push(export_failure);
                }
                // Skipped above (system-trace and key-rotation records are not exportable).
                SealedLogRecord::KeyRotation(_)
                | SealedLogRecord::Heartbeat(_)
                | SealedLogRecord::Lifecycle(_) => unreachable!(),
            }
        }

        let mut batches = Vec::new();
        let mut current = ExportBatch {
            buckets: Vec::new(),
        };
        let mut current_events = 0usize;

        for bucket in buckets {
            let bucket_events = bucket.events.len() + bucket.failures.len();
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
            auth_mode: Some(auth_mode),
            window: options.window,
        };
        self.log_export_receipt(&receipt)?;

        Ok(artifact)
    }

    pub fn device_key_for_verify_only(&self) -> [u8; 32] {
        self.device_key.verifying_key().to_bytes()
    }

    pub fn device_pq_public_key_ref(&self) -> Option<&PqPublicKey> {
        #[cfg(feature = "pqc-signatures")]
        {
            return self.device_pq_key.as_ref().map(|key| &key.public_key);
        }
        #[cfg(not(feature = "pqc-signatures"))]
        {
            None
        }
    }

    pub fn device_pq_public_key_for_verify_only(&self) -> Option<Vec<u8>> {
        #[cfg(feature = "pqc-signatures")]
        {
            return self
                .device_pq_key
                .as_ref()
                .map(|key| key.public_key_bytes());
        }
        #[cfg(not(feature = "pqc-signatures"))]
        {
            None
        }
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
            token.vault_envelope_id(),
            token.ruleset_hash(),
            &receipt_entry_hash,
            self.device_pq_public_key_ref(),
        )?;
        if !matches!(outcome, break_glass::BreakGlassOutcome::Granted) {
            return Err(anyhow!("cannot sign break-glass token for denied receipt"));
        }
        token.attach_receipt_signature(receipt_entry_hash, &self.device_key)
    }

    /// Burn the token's nonce in the durable anti-replay ledger — see
    /// `consume_break_glass_token_durably`. Call BEFORE releasing any
    /// cleartext or export artifact.
    pub fn consume_token_durably(&self, token: &break_glass::BreakGlassToken) -> Result<()> {
        consume_break_glass_token_durably(&self.conn, token)
    }

    pub fn break_glass_receipt_outcome(
        &self,
        expected_envelope_id: &str,
        expected_ruleset_hash: [u8; 32],
        receipt_entry_hash: &[u8; 32],
    ) -> Result<break_glass::BreakGlassOutcome> {
        break_glass_receipt_outcome_for_verifier(
            &self.conn,
            &self.device_verifying_key(),
            expected_envelope_id,
            expected_ruleset_hash,
            receipt_entry_hash,
            self.device_pq_public_key_ref(),
        )
    }
}

/// Minimum seed length for production use (32 hex chars = 16 bytes of entropy).
/// Seeds shorter than this are brute-forceable.
pub const MIN_SEED_LENGTH: usize = 32;

pub fn signing_key_from_seed(seed: &str) -> Result<SigningKey> {
    let trimmed = seed.trim();
    if trimmed.is_empty() {
        return Err(anyhow!("device_key_seed is required"));
    }
    if trimmed == "devkey:mvp" {
        return Err(anyhow!("device_key_seed must not use MVP placeholder"));
    }
    // Enforce minimum entropy: firmware uses hardware RNG (32+ bytes).
    // Seeds shorter than MIN_SEED_LENGTH are likely dictionary words or
    // short passphrases and are brute-forceable.
    if trimmed.len() < MIN_SEED_LENGTH {
        return Err(anyhow!(
            "device_key_seed too short: {} chars (minimum {} required). \
             Use `dd if=/dev/urandom bs=32 count=1 | xxd -p` to generate a secure seed.",
            trimmed.len(),
            MIN_SEED_LENGTH
        ));
    }
    let mut hasher = Sha256::new();
    hasher.update(trimmed.as_bytes());
    let digest: [u8; 32] = hasher.finalize().into();
    Ok(SigningKey::from_bytes(&digest))
}

pub fn verifying_key_from_seed(seed: &str) -> Result<VerifyingKey> {
    Ok(signing_key_from_seed(seed)?.verifying_key())
}

pub(crate) fn verifying_key_from_bytes(bytes: &[u8]) -> Result<VerifyingKey> {
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

#[cfg(feature = "pqc-signatures")]
fn pq_public_key_from_bytes(bytes: &[u8]) -> Result<PqPublicKey> {
    PqPublicKey::from_bytes(bytes).map_err(|e| anyhow!("invalid pq public key bytes: {}", e))
}

#[cfg(not(feature = "pqc-signatures"))]
fn pq_public_key_from_bytes(_bytes: &[u8]) -> Result<PqPublicKey> {
    Err(anyhow!(
        "pq public key parsing not available (pqc-signatures feature disabled)"
    ))
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

/// The device's *genesis* public key — the immutable verification anchor for the event
/// chain. After a rotation this differs from the current signing key; use
/// [`current_device_public_key`] for the active key.
pub fn genesis_device_public_key(conn: &Connection) -> Result<[u8; 32]> {
    let bytes: Vec<u8> = conn.query_row(
        "SELECT public_key FROM device_metadata WHERE id = 1",
        [],
        |row| row.get(0),
    )?;
    if bytes.len() != 32 {
        return Err(anyhow!("corrupt device_metadata.public_key length"));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

/// Reconstruct and cryptographically validate the device key lineage, anchored at the
/// genesis key stored in `device_metadata`.
///
/// Walking the append-only `device_key_history` table from epoch 0 (genesis), each rotation
/// epoch is verified: its `prev_public_key` must equal the running current key, the retiring
/// key's **authorization** and the new key's **attestation** must both verify over the
/// `(prev ‖ new)` binding. Because the chain is rooted at the genesis key — whose private
/// key a tamperer does not hold — a rewritten history cannot forge a valid lineage. This is
/// the trust anchor verification uses for key selection; it survives event pruning because
/// the history table is never pruned.
pub fn reconstruct_device_key_lineage(conn: &Connection) -> Result<Vec<DeviceKeyEpoch>> {
    let genesis = genesis_device_public_key(conn)?;
    reconstruct_device_key_lineage_from(conn, &genesis)
}

/// As [`reconstruct_device_key_lineage`] but with an explicit, caller-trusted genesis anchor
/// (e.g. a `--public-key` override supplied to an external verifier).
pub fn reconstruct_device_key_lineage_from(
    conn: &Connection,
    genesis: &[u8; 32],
) -> Result<Vec<DeviceKeyEpoch>> {
    let mut lineage = vec![DeviceKeyEpoch {
        epoch: 0,
        public_key: *genesis,
        activated_at_event_id: 0,
    }];

    let mut stmt = conn.prepare(
        "SELECT epoch, public_key, prev_public_key, activated_at_event_id, attestation, authorization \
         FROM device_key_history WHERE epoch >= 1 ORDER BY epoch ASC",
    )?;
    let mut rows = stmt.query([])?;
    let mut expected_epoch = 1i64;
    let mut current = verifying_key_from_bytes(genesis)?;
    let mut current_bytes = *genesis;

    while let Some(row) = rows.next()? {
        let epoch: i64 = row.get(0)?;
        let public_key: Vec<u8> = row.get(1)?;
        let prev_public_key: Option<Vec<u8>> = row.get(2)?;
        let activated_at_event_id: i64 = row.get(3)?;
        let attestation: Option<Vec<u8>> = row.get(4)?;
        let authorization: Option<Vec<u8>> = row.get(5)?;

        if epoch != expected_epoch {
            return Err(anyhow!(
                "device key lineage: non-contiguous epoch (expected {}, found {})",
                expected_epoch,
                epoch
            ));
        }
        let new_bytes = key32(&public_key, "device_key_history.public_key")?;
        let prev_bytes = key32(
            prev_public_key.as_deref().unwrap_or_default(),
            "device_key_history.prev_public_key",
        )?;
        if prev_bytes != current_bytes {
            return Err(anyhow!(
                "device key lineage: epoch {} prev_public_key does not chain from epoch {}",
                epoch,
                epoch - 1
            ));
        }
        let new_key = verifying_key_from_bytes(&new_bytes)?;
        let attestation = attestation
            .ok_or_else(|| anyhow!("device key lineage: epoch {} missing attestation", epoch))?;
        // Successor proves possession of the incoming key.
        verify_rotation_attestation(&new_key, &prev_bytes, &new_bytes, &attestation)
            .map_err(|e| anyhow!("device key lineage: epoch {}: {}", epoch, e))?;
        // Predecessor approved this successor (anchors the lineage to genesis). Records written
        // before the authorization field existed (legacy upgrades) carry no authorization here;
        // recover the same genesis-anchored guarantee from the retained in-chain rotation
        // record, whose entry is signed by the predecessor key.
        match authorization.as_deref() {
            Some(authz) if !authz.is_empty() => {
                verify_rotation_authorization(&current, &prev_bytes, &new_bytes, authz)
                    .map_err(|e| anyhow!("device key lineage: epoch {}: {}", epoch, e))?;
            }
            _ => {
                recover_legacy_rotation_authorization(
                    conn,
                    &current,
                    &prev_bytes,
                    &new_bytes,
                    activated_at_event_id,
                )
                .map_err(|e| anyhow!("device key lineage: epoch {}: {}", epoch, e))?;
            }
        }

        lineage.push(DeviceKeyEpoch {
            epoch,
            public_key: new_bytes,
            activated_at_event_id,
        });
        current = new_key;
        current_bytes = new_bytes;
        expected_epoch += 1;
    }

    Ok(lineage)
}

pub(crate) fn key32(bytes: &[u8], what: &str) -> Result<[u8; 32]> {
    if bytes.len() != 32 {
        return Err(anyhow!(
            "corrupt {}: expected 32 bytes, got {}",
            what,
            bytes.len()
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(bytes);
    Ok(out)
}

/// Stored columns of a sealed-event row needed to re-verify its entry signature:
/// `(payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme)`.
type SealedEntryRow = (
    String,
    Vec<u8>,
    Vec<u8>,
    Vec<u8>,
    Option<Vec<u8>>,
    Option<String>,
);

/// Recover the predecessor-authorization guarantee for a legacy key-history epoch (one
/// written before the explicit `authorization` field existed) from the retained in-chain
/// rotation record. The record's entry is hash-chained and signed by the predecessor key, so
/// validating that entry signature under `predecessor` is genesis-anchored authorization
/// equivalent. Fails closed if the record was pruned (the only genuinely unrecoverable case).
pub(crate) fn recover_legacy_rotation_authorization(
    conn: &Connection,
    predecessor: &VerifyingKey,
    prev_bytes: &[u8; 32],
    new_bytes: &[u8; 32],
    event_id: i64,
) -> Result<()> {
    let row: Option<SealedEntryRow> = conn
        .query_row(
            "SELECT payload_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme \
             FROM sealed_events WHERE id = ?1",
            params![event_id],
            |r| {
                Ok((
                    r.get(0)?,
                    r.get(1)?,
                    r.get(2)?,
                    r.get(3)?,
                    r.get(4)?,
                    r.get(5)?,
                ))
            },
        )
        .optional()?;
    let (payload, prev_hash, entry_hash, signature, pq_signature, pq_scheme) =
        row.ok_or_else(|| {
            anyhow!(
            "authorization missing and the in-chain rotation record (event {}) has been pruned; \
             cannot anchor this legacy rotation",
            event_id
        )
        })?;

    let record = SealedLogRecord::deserialize_compat(&payload)?;
    let SealedLogRecord::KeyRotation(rotation) = record else {
        return Err(anyhow!("event {} is not a rotation record", event_id));
    };
    if &rotation.prev_public_key != prev_bytes || &rotation.new_public_key != new_bytes {
        return Err(anyhow!(
            "in-chain rotation record (event {}) does not match the history epoch",
            event_id
        ));
    }

    let prev_hash = key32(&prev_hash, "sealed_events.prev_hash")?;
    let entry_hash = key32(&entry_hash, "sealed_events.entry_hash")?;
    if hash_entry(&prev_hash, payload.as_bytes()) != entry_hash {
        return Err(anyhow!(
            "in-chain rotation record (event {}) hash mismatch",
            event_id
        ));
    }
    let signatures = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
    // The predecessor's entry signature is the authorization-equivalent (it signed this
    // rotation into the chain). Ed25519 under the predecessor key is sufficient.
    verify_entry_signature(
        predecessor,
        &entry_hash,
        &signatures,
        SignatureMode::Compat,
        None,
        DOMAIN_SEALED_LOG_ENTRY,
    )
    .map_err(|e| {
        anyhow!(
            "legacy rotation record (event {}) entry signature does not verify under the \
             predecessor key: {}",
            event_id,
            e
        )
    })
}

/// The device's *current* signing public key — the last epoch of the validated lineage.
pub fn current_device_public_key(conn: &Connection) -> Result<[u8; 32]> {
    Ok(reconstruct_device_key_lineage(conn)?
        .last()
        .expect("lineage always contains genesis")
        .public_key)
}

/// The device public key active at (i.e., signing entries up to) `event_id` — the latest
/// validated lineage epoch whose `activated_at_event_id <= event_id`. Used to seed the
/// verifier and to bound the trusted checkpoint signer when earlier events were pruned.
pub fn device_key_active_at(conn: &Connection, event_id: i64) -> Result<[u8; 32]> {
    device_key_active_at_in(&reconstruct_device_key_lineage(conn)?, event_id)
}

/// As [`device_key_active_at`] but over an already-reconstructed lineage.
pub fn device_key_active_at_in(lineage: &[DeviceKeyEpoch], event_id: i64) -> Result<[u8; 32]> {
    lineage
        .iter()
        .filter(|e| e.activated_at_event_id <= event_id)
        .max_by_key(|e| e.epoch)
        .map(|e| e.public_key)
        .ok_or_else(|| anyhow!("device key lineage: no key active at event {}", event_id))
}

#[cfg(feature = "pqc-signatures")]
pub fn device_pq_public_key_from_db(conn: &Connection) -> Result<PqPublicKey> {
    let bytes: Vec<u8> = conn
        .query_row(
            "SELECT pq_public_key FROM device_metadata WHERE id = 1",
            [],
            |row| row.get(0),
        )
        .map_err(|e| match e {
            rusqlite::Error::QueryReturnedNoRows => {
                anyhow!("pq public key not found in database")
            }
            _ => anyhow!("failed to read pq public key from database: {}", e),
        })?;
    pq_public_key_from_bytes(&bytes)
}

pub fn verify_export_bundle(bundle: &ExportBundle) -> Result<()> {
    let verifying_key = verifying_key_from_bytes(&bundle.device_public_key)?;
    let payload_json = serde_json::to_string(&bundle.receipt_entry.receipt)?;
    let computed_entry_hash = hash_entry(&bundle.receipt_entry.prev_hash, payload_json.as_bytes());
    if computed_entry_hash != bundle.receipt_entry.entry_hash {
        return Err(anyhow!("export receipt entry hash mismatch"));
    }
    let pq_public_key = bundle
        .pq_public_key
        .as_ref()
        .and_then(|bytes| pq_public_key_from_bytes(bytes).ok());
    verify_entry_signature(
        &verifying_key,
        &bundle.receipt_entry.entry_hash,
        &bundle.receipt_entry.signatures,
        SignatureMode::Compat,
        pq_public_key.as_ref(),
        DOMAIN_EXPORT_RECEIPT,
    )?;
    let artifact_bytes = serde_json::to_vec(&bundle.artifact)?;
    let artifact_hash: [u8; 32] = Sha256::digest(&artifact_bytes).into();
    if artifact_hash != bundle.receipt_entry.receipt.artifact_hash {
        return Err(anyhow!("export artifact hash mismatch"));
    }
    Ok(())
}

pub mod verify_helpers;
pub mod verify_runner;

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

/// Durable anti-replay ledger for break-glass tokens.
///
/// The in-memory `BreakGlassToken` `consumed` flag dies with the process,
/// and a token FILE re-parses as unconsumed
/// (`BreakGlassTokenFile::into_token`), so within its 10-minute validity
/// bucket a granted token file could otherwise authorize repeated unseals
/// across separate CLI invocations. This burns the token's nonce in the
/// same SQLite database that holds the receipts: a single atomic
/// `INSERT OR IGNORE` — zero rows changed means the nonce was already
/// burned and the caller must refuse.
///
/// Callers burn the nonce BEFORE releasing any cleartext (fail closed): a
/// crash mid-unseal wastes the token — trustees re-approve — instead of
/// leaving it replayable.
/// Outcome of a gated policy write (`Kernel::set_break_glass_policy_gated`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PolicyChangeOutcome {
    /// No policy existed; the write is the bootstrap and needed no quorum.
    Bootstrapped,
    /// A policy existed and was replaced with current-quorum consent.
    Replaced,
    /// The proposed policy is identical to the stored one; nothing written.
    Unchanged,
}

/// The empty-approval-set commitment, used as the serde default for the
/// `approvals_commitment` field on policy-change records written before the
/// field existed.
pub(crate) fn empty_approvals_commitment_bytes() -> [u8; 32] {
    crate::break_glass::approvals_commitment(&[])
}

/// The payload of one policy-change history row: the full previous and new
/// policies, the change hash trustees consented to, a commitment to the
/// trustee approvals that authorized the change, and whether this was the
/// bootstrap write. Serialized verbatim into `payload_json` and hash-chained,
/// so the device signature covers all of it — including `approvals_commitment`,
/// which binds the recorded approvals into the signed material (a swapped or
/// deleted `approvals_json` no longer verifies).
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PolicyChangeRecord {
    pub prev_policy: Option<crate::break_glass::QuorumPolicy>,
    pub new_policy: crate::break_glass::QuorumPolicy,
    pub change_hash: [u8; 32],
    pub time_bucket: TimeBucket,
    pub bootstrap: bool,
    /// `approvals_commitment(approvals)` — the same commitment a break-glass
    /// receipt records. Defaults to the empty-set commitment for records
    /// written before this field existed.
    #[serde(default = "crate::empty_approvals_commitment_bytes")]
    pub approvals_commitment: [u8; 32],
}

/// One row of the policy-change history ledger, as read back for audit.
#[derive(Clone, Debug)]
pub struct PolicyChangeHistoryEntry {
    pub record: PolicyChangeRecord,
    /// The exact bytes that were hashed at append time.
    pub payload_json: String,
    pub approvals: Vec<crate::break_glass::Approval>,
    pub prev_hash: [u8; 32],
    pub entry_hash: [u8; 32],
    pub signature: Vec<u8>,
}

pub fn consume_break_glass_token_durably(
    conn: &Connection,
    token: &break_glass::BreakGlassToken,
) -> Result<()> {
    // Databases created before this table existed must still fail closed
    // when a CLI unseal runs against them — create on first use.
    conn.execute(
        "CREATE TABLE IF NOT EXISTS consumed_break_glass_tokens (
           token_nonce BLOB PRIMARY KEY,
           consumed_at INTEGER NOT NULL
         )",
        [],
    )?;
    let consumed_at = i64::try_from(now_s()?).unwrap_or(i64::MAX);
    let inserted = conn.execute(
        "INSERT OR IGNORE INTO consumed_break_glass_tokens \
         (token_nonce, consumed_at) VALUES (?1, ?2)",
        params![token.token_nonce().to_vec(), consumed_at],
    )?;
    if inserted == 0 {
        return Err(anyhow!(
            "break-glass token already consumed (nonce replay across invocations)"
        ));
    }
    Ok(())
}

/// Resolve and re-verify the break-glass receipt a token points at, binding it
/// to the disclosure act the token authorizes.
///
/// `expected_envelope_id` / `expected_ruleset_hash` are the token's own
/// (device-signed) envelope and ruleset. The receipt fetched by
/// `receipt_entry_hash` MUST carry the same envelope and ruleset: without this,
/// a genuine `Granted` receipt for envelope A (with real trustee approvals for
/// A) could be paired with a token for envelope B and unseal B — reusing
/// trustee consent across envelopes, defeating the per-disclosure binding
/// Invariant V promises even against a device-key holder. The binding is
/// checked before the quorum re-derivation so a cross-envelope replay fails
/// fast and unambiguously.
pub fn break_glass_receipt_outcome_for_verifier(
    conn: &Connection,
    verifying_key: &VerifyingKey,
    expected_envelope_id: &str,
    expected_ruleset_hash: [u8; 32],
    receipt_entry_hash: &[u8; 32],
    pq_public_key: Option<&PqPublicKey>,
) -> Result<break_glass::BreakGlassOutcome> {
    let mut stmt = conn.prepare(
        "SELECT payload_json, approvals_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme FROM break_glass_receipts WHERE entry_hash = ?1 LIMIT 1",
    )?;
    let row = stmt
        .query_row(params![receipt_entry_hash.to_vec()], |row| {
            let payload: String = row.get(0)?;
            let approvals_json: String = row.get(1)?;
            let prev_hash: Vec<u8> = row.get(2)?;
            let entry_hash: Vec<u8> = row.get(3)?;
            let signature: Vec<u8> = row.get(4)?;
            let pq_signature: Option<Vec<u8>> = row.get(5)?;
            let pq_scheme: Option<String> = row.get(6)?;
            Ok((
                payload,
                approvals_json,
                prev_hash,
                entry_hash,
                signature,
                pq_signature,
                pq_scheme,
            ))
        })
        .optional()?;

    let Some((payload, approvals_json, prev_hash, entry_hash, signature, pq_signature, pq_scheme)) =
        row
    else {
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
    let signature_set = SignatureSet::from_storage(&signature, pq_signature, pq_scheme)?;
    verify_entry_signature(
        verifying_key,
        &entry_hash,
        &signature_set,
        SignatureMode::Compat,
        pq_public_key,
        DOMAIN_BREAK_GLASS_RECEIPT,
    )?;
    let receipt: break_glass::BreakGlassReceipt = serde_json::from_str(&payload)?;

    // Bind the receipt to the disclosure act the token authorizes. A token is
    // device-signed over its envelope and ruleset; the receipt it references
    // must be the receipt for THAT envelope and ruleset. Rejecting a mismatch
    // stops trustee consent granted for one envelope from being replayed to
    // unseal another (Invariant V, per-disclosure binding).
    if receipt.vault_envelope_id != expected_envelope_id {
        return Err(anyhow!(
            "break-glass receipt is bound to envelope '{}' but the token authorizes '{}' — trustee consent is per-envelope and cannot be reused across disclosures",
            receipt.vault_envelope_id,
            expected_envelope_id
        ));
    }
    if receipt.ruleset_hash != expected_ruleset_hash {
        return Err(anyhow!(
            "break-glass receipt ruleset does not match the token's ruleset"
        ));
    }

    // Quorum re-derivation at the unseal gate (Invariant V). The receipt is
    // device-signed and hash-chained, but a device-key holder can still forge
    // a `Granted` receipt with an empty (or under-quorum) approval set. Before
    // this gate lets `assert_token_valid` release any cleartext, recompute the
    // quorum against the configured policy: a Granted receipt is honored only
    // if it actually carries >= policy.n distinct valid trustee approvals.
    // The signed receipt commits to its approvals via `approvals_commitment`,
    // so a swapped `approvals_json` is rejected before we count.
    if matches!(receipt.outcome, break_glass::BreakGlassOutcome::Granted) {
        let policy = crate::verify::load_break_glass_policy(conn)?
            .ok_or_else(|| anyhow!("break-glass quorum policy is not configured"))?;
        // Unlike the audit path (which tolerates historical policy eras), the
        // unseal gate must fail CLOSED: a token is minted under the policy in
        // force at authorize time, so a receipt whose recorded policy_commitment
        // no longer matches the active policy means the quorum was rotated out
        // from under this token. Refuse and require re-authorization rather than
        // release cleartext against a stale quorum decision. (A zero commitment
        // marks a pre-field receipt and is re-derived against the current
        // policy, preserving the original H1 check.)
        if receipt.policy_commitment != [0u8; 32]
            && receipt.policy_commitment != policy.commitment()
        {
            return Err(anyhow!(
                "break-glass receipt policy commitment does not match the active quorum policy (policy rotated since authorization; re-authorize)"
            ));
        }
        let approvals: Vec<break_glass::Approval> = serde_json::from_str(&approvals_json)?;
        if break_glass::approvals_commitment(&approvals) != receipt.approvals_commitment {
            return Err(anyhow!(
                "break-glass approvals commitment mismatch: receipt approvals were tampered"
            ));
        }
        let valid =
            break_glass::count_valid_distinct_approvals(&policy, &receipt.request_hash, &approvals);
        if valid < policy.n as usize {
            return Err(anyhow!(
                "break-glass receipt claims Granted but only {} of the required {} distinct trustee approvals are valid",
                valid,
                policy.n
            ));
        }
    }

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

    // Use gen_range() for rejection sampling instead of modulo to avoid bias.
    // Modulo bias is small for typical spans but this is a privacy-critical path
    // where jitter protects against timing correlation attacks.
    let choice = rand::random_range(-steps..=steps);
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
    pub supported_backends: &'static [InferenceBackend],
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

/// Select an inference backend for a module based on device capabilities.
pub fn select_module_backend(
    desc: &ModuleDescriptor,
    selection: BackendSelection,
    capabilities: &DeviceCapabilities,
) -> Result<InferenceBackend> {
    select_inference_backend(selection, capabilities, desc.supported_backends).map_err(|err| {
        anyhow!(
            "conformance: module {} cannot select backend: {}",
            desc.id,
            err
        )
    })
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
        registry: &detect::BackendRegistry,
    ) -> Result<Vec<CandidateEvent>>;
}

/// Zone crossing detection module.
pub struct ZoneCrossingModule {
    zone_id: String,
    emit_token: bool,
    backend: InferenceBackend,
}

const ZONE_CROSSING_ALLOWED: &[EventType] = &[
    EventType::BoundaryCrossingObjectLarge,
    EventType::BoundaryCrossingObjectSmall,
];
const ZONE_CROSSING_BACKENDS: &[InferenceBackend] = &[
    InferenceBackend::Cpu,
    InferenceBackend::Tract,
    InferenceBackend::Stub,
];

impl ZoneCrossingModule {
    pub fn new(zone_id: &str) -> Self {
        Self::with_backend_selection(
            zone_id,
            BackendSelection::Require(InferenceBackend::Stub),
            &DeviceCapabilities::stub_only(),
        )
        .expect("stub backend available")
    }

    pub fn with_backend_selection(
        zone_id: &str,
        selection: BackendSelection,
        capabilities: &DeviceCapabilities,
    ) -> Result<Self> {
        let desc = ModuleDescriptor {
            id: "zone_crossing",
            allowed_event_types: ZONE_CROSSING_ALLOWED,
            requested_capabilities: &[],
            supported_backends: ZONE_CROSSING_BACKENDS,
        };
        let backend = select_module_backend(&desc, selection, capabilities)?;
        Ok(Self {
            zone_id: zone_id.to_string(),
            emit_token: false,
            backend,
        })
    }

    pub fn with_tokens(mut self, enabled: bool) -> Self {
        self.emit_token = enabled;
        self
    }

    pub fn backend(&self) -> InferenceBackend {
        self.backend
    }
}

impl Module for ZoneCrossingModule {
    fn descriptor(&self) -> ModuleDescriptor {
        ModuleDescriptor {
            id: "zone_crossing",
            allowed_event_types: ZONE_CROSSING_ALLOWED,
            requested_capabilities: &[],
            supported_backends: ZONE_CROSSING_BACKENDS,
        }
    }

    fn process(
        &mut self,
        view: &InferenceView<'_>,
        bucket: TimeBucket,
        token_mgr: &BucketKeyManager,
        registry: &detect::BackendRegistry,
    ) -> Result<Vec<CandidateEvent>> {
        // Run detection on the inference view (cannot access raw bytes)
        let result = view.run_detector(registry)?;

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
                // In-process camera pipeline: the kernel itself is the sensor,
                // rendered downstream as device-attested (field absent).
                attestation: None,
            }])
        } else {
            Ok(vec![])
        }
    }
}

// -------------------- Legacy stub for compatibility --------------------

/// Legacy stub frame source (for tests or dev/demo builds that don't need full RTSP).
#[cfg(any(test, feature = "stub-frame-source"))]
pub struct StubFrameSource {
    source: RtspSource,
}

#[cfg(any(test, feature = "stub-frame-source"))]
impl StubFrameSource {
    pub fn new() -> Self {
        let config = RtspConfig {
            url: "stub://test".to_string(),
            target_fps: 10,
            width: 640,
            height: 480,
            backend: crate::config::RtspBackendPreference::Auto,
            transport: None,
        };
        Self {
            source: RtspSource::new(config).expect("stub RTSP source"),
        }
    }

    pub fn next_frame(&mut self) -> Result<RawFrame> {
        self.source.next_frame()
    }
}

#[cfg(any(test, feature = "stub-frame-source"))]
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

// Re-exports for CLI/tools
pub use break_glass::{
    approvals_commitment, sign_approval, verify_approval, Approval, BreakGlass, BreakGlassOutcome,
    BreakGlassReceipt, BreakGlassToken, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry, TrusteeId,
    UnlockRequest,
};

// -------------------- Conformance Tests --------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::break_glass::{Approval, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest};
    use ed25519_dalek::SigningKey;

    fn setup_test_kernel() -> Result<(Kernel, KernelConfig)> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let kernel = Kernel::open(&cfg)?;
        Ok((kernel, cfg))
    }

    fn make_break_glass_token(
        envelope_id: &str,
        ruleset_hash: [u8; 32],
    ) -> (BreakGlassToken, VerifyingKey, [u8; 32]) {
        let bucket = TimeBucket::now(600).expect("time bucket");
        let request = UnlockRequest::new(envelope_id, ruleset_hash, "test-export", bucket).unwrap();
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        let approval = Approval::signed(
            TrusteeId::new("alice"),
            request.request_hash(),
            &signing_key,
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
    fn conformance_coarsens_precise_time() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 601,
                size_s: 60,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
            attestation: None,
        };
        let ev = ContractEnforcer::enforce(cand).expect("coarsened event");
        assert_eq!(ev.time_bucket.size_s, TEN_MINUTES_S);
        assert_eq!(ev.time_bucket.start_epoch_s, TEN_MINUTES_S as u64);
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
            attestation: None,
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
            attestation: None,
        };
        assert!(ContractEnforcer::enforce(cand).is_err());
    }

    #[test]
    fn conformance_rejects_token_with_oversized_bucket() {
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: 3600,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: Some([0u8; 32]),
            attestation: None,
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
        let (mut kernel, cfg) = setup_test_kernel()?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 0,
                size_s: TEN_MINUTES_S,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: Some([1u8; 32]),
            attestation: None,
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
            ExportAuthMode::SelfExport,
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

    fn seal_one_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) -> Result<()> {
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 600,
                size_s: TEN_MINUTES_S,
            },
            zone_id: zone.to_string(),
            confidence: 0.5,
            correlation_token: None,
            attestation: None,
        };
        kernel.append_event_checked(
            &desc,
            cand,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        Ok(())
    }

    #[test]
    fn verify_sealed_log_accepts_good_chain_and_rejects_tampering() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        seal_one_event(&mut kernel, &cfg, "zone:b")?;

        // A freshly sealed chain verifies from the checkpoint through the tail.
        let report = kernel.verify_sealed_log()?;
        assert!(
            report.chain_valid,
            "intact chain must verify: {:?}",
            report.error
        );

        // Tampering the tail must be caught by the boot-time check (K4 / FR-8),
        // not laundered into the chain by the next append.
        kernel.conn.execute(
            "UPDATE sealed_events SET payload_json = '{\"tampered\":true}' \
             WHERE id = (SELECT MAX(id) FROM sealed_events)",
            [],
        )?;
        let report = kernel.verify_sealed_log()?;
        assert!(
            !report.chain_valid,
            "a tampered tail must fail verification"
        );
        Ok(())
    }

    proptest::proptest! {
        // TimeBucket::coarsen_to is aligned to the target size, never later
        // than the input, within one bucket of it, and idempotent — for any
        // epoch and any valid (>= MIN_BUCKET_SIZE_S) target size.
        #[test]
        fn coarsen_to_is_aligned_monotonic_and_idempotent(
            epoch in 0u64..4_000_000_000,
            size in MIN_BUCKET_SIZE_S..(7 * 24 * 3600),
        ) {
            let bucket = TimeBucket { start_epoch_s: epoch, size_s: 600 };
            let coarse = bucket.coarsen_to(size).expect("valid size must coarsen");
            proptest::prop_assert_eq!(coarse.size_s, size);
            proptest::prop_assert_eq!(coarse.start_epoch_s % u64::from(size), 0); // aligned
            proptest::prop_assert!(coarse.start_epoch_s <= epoch);               // never later
            proptest::prop_assert!(epoch - coarse.start_epoch_s < u64::from(size)); // within one bucket
            let again = coarse.coarsen_to(size).expect("idempotent");
            proptest::prop_assert_eq!(again.start_epoch_s, coarse.start_epoch_s);
        }

        // Any bucket size below the minimum bucket size floor is always rejected.
        #[test]
        fn coarsen_below_minimum_is_always_rejected(size in 0u32..MIN_BUCKET_SIZE_S) {
            let bucket = TimeBucket { start_epoch_s: 600, size_s: 600 };
            proptest::prop_assert!(bucket.coarsen_to(size).is_err());
        }
    }

    proptest::proptest! {
        // Kernel-backed (opens an in-memory DB per case), so keep the case
        // count modest.
        #![proptest_config(proptest::prelude::ProptestConfig::with_cases(48))]

        // Round-trip invariant: any sequence of validly-sealed events yields a
        // chain that verifies from the checkpoint through the tail.
        #[test]
        fn sealed_chain_of_any_length_verifies(
            zones in proptest::collection::vec("[a-z][a-z0-9_-]{0,7}", 0..12),
        ) {
            let (mut kernel, cfg) = setup_test_kernel().expect("kernel");
            for z in &zones {
                seal_one_event(&mut kernel, &cfg, &format!("zone:{z}")).expect("seal");
            }
            let report = kernel.verify_sealed_log().expect("verify runs");
            proptest::prop_assert!(
                report.chain_valid,
                "a chain of {} sealed events must verify: {:?}",
                zones.len(),
                report.error
            );
        }
    }

    fn build_test_envelope() -> Result<EvidenceEnvelope> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        seal_one_event(&mut kernel, &cfg, "zone:b")?;
        kernel.build_evidence_envelope_for_api(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                ..ExportOptions::default()
            },
            &cfg.ruleset_id,
            &cfg.kernel_version,
        )
    }

    #[test]
    fn evidence_envelope_builds_and_verifies() -> Result<()> {
        let envelope = build_test_envelope()?;
        assert_eq!(envelope.envelope_format, envelope::ENVELOPE_FORMAT);
        assert_eq!(envelope.envelope_version, envelope::ENVELOPE_VERSION);
        assert_eq!(envelope.ledgers.sealed_events.count, 2);
        assert_eq!(envelope.ledgers.export_receipts.count, 1);
        assert_eq!(envelope.disclosure.profile, "full");

        let report = verify_envelope(&envelope, SignatureMode::Compat)?;
        assert_eq!(report.sealed_events, 2);
        assert_eq!(report.export_receipts, 1);
        Ok(())
    }

    #[test]
    fn evidence_envelope_survives_json_round_trip() -> Result<()> {
        let envelope = build_test_envelope()?;
        let json = serde_json::to_string(&envelope)?;
        let parsed: EvidenceEnvelope = serde_json::from_str(&json)?;
        // Digest must be stable across (de)serialization, and verification must still pass.
        assert_eq!(parsed.whole_envelope_digest, envelope.whole_envelope_digest);
        verify_envelope(&parsed, SignatureMode::Compat)?;
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_metadata_tamper_via_digest() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        // Tamper a field WITHOUT recomputing the digest: the digest check must fail.
        envelope.provenance.kernel_version = "evil".to_string();
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("whole_envelope_digest mismatch"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_payload_tamper_at_chain_level() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        // Corrupt a sealed-event payload, then RE-COMPUTE the digest so the digest check
        // passes and we exercise the per-entry hash/chain verification path instead.
        envelope.ledgers.sealed_events.entries[0]
            .payload_json
            .push(' ');
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("sealed_events"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_broken_chain_link() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        // Flip a byte in the second entry's prev_hash, then refresh the digest.
        envelope.ledgers.sealed_events.entries[1].prev_hash[0] ^= 0xff;
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("sealed_events"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_forged_ledger_count() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        // Lie about the entry count while leaving the real entries intact, then refresh the
        // digest (which is not a signature). The summary check must still catch it.
        envelope.ledgers.sealed_events.count = 99;
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("sealed_events count mismatch"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_forged_head_hash() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        envelope.ledgers.sealed_events.head_hash = Some([0xab; 32]);
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("sealed_events head_hash"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_rejects_tampered_manifest() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        // Weaken a carried rule, then refresh the digest. Must be rejected before signatures.
        envelope.manifest.signature_domains.sealed_log_entry = "evil:domain".to_string();
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("manifest"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_detects_provenance_ruleset_mismatch() -> Result<()> {
        let mut envelope = build_test_envelope()?;
        envelope.provenance.ruleset_hash[0] ^= 0xff;
        envelope.whole_envelope_digest = envelope::compute_whole_envelope_digest(&envelope)?;
        let err = verify_envelope(&envelope, SignatureMode::Compat).unwrap_err();
        assert!(
            format!("{err}").contains("ruleset_hash"),
            "unexpected error: {err}"
        );
        Ok(())
    }

    #[test]
    fn evidence_envelope_omits_forbidden_fields() -> Result<()> {
        let envelope = build_test_envelope()?;

        fn contains_key(value: &serde_json::Value, key: &str) -> bool {
            match value {
                serde_json::Value::Object(map) => {
                    map.iter().any(|(k, v)| k == key || contains_key(v, key))
                }
                serde_json::Value::Array(items) => items.iter().any(|v| contains_key(v, key)),
                _ => false,
            }
        }

        // The coarse artifact view must never carry precise time or identity fields.
        let artifact_value = serde_json::to_value(&envelope.artifact)?;
        assert!(!contains_key(&artifact_value, "timestamp"));
        assert!(!contains_key(&artifact_value, "created_at"));
        assert!(!contains_key(&artifact_value, "correlation_token"));
        // The manifest must advertise these as forbidden.
        assert!(envelope
            .manifest
            .forbidden_fields
            .iter()
            .any(|f| f == "precise_timestamp"));
        Ok(())
    }

    #[test]
    fn sealed_event_created_at_matches_time_bucket_start() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let bucket_start = 1_200u64;
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: bucket_start,
                size_s: TEN_MINUTES_S,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
            attestation: None,
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
    fn event_attestation_is_optional_and_backward_compatible() -> Result<()> {
        // Records sealed before the attestation field existed must keep
        // deserializing (and, since the field is skip-when-none, an event
        // without provenance serializes byte-identically to the old shape).
        let legacy = r#"{
            "event_type": "PresenceInRestrictedZone",
            "time_bucket": {"start_epoch_s": 600, "size_s": 600},
            "zone_id": "zone:bedroom",
            "confidence": 0.9,
            "correlation_token": null,
            "kernel_version": "0.5.0",
            "ruleset_id": "ruleset:v1",
            "ruleset_hash": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
        }"#;
        let ev: Event = serde_json::from_str(legacy)?;
        assert_eq!(ev.attestation, None);
        assert!(!serde_json::to_string(&ev)?.contains("attestation"));

        // Provenance round-trips using the HA contract's wire values.
        let mut stamped = ev.clone();
        stamped.attestation = Some(Attestation::HaBridged);
        let json = serde_json::to_string(&stamped)?;
        assert!(json.contains(r#""attestation":"ha-bridged""#));
        let back: Event = serde_json::from_str(&json)?;
        assert_eq!(back.attestation, Some(Attestation::HaBridged));
        assert_eq!(
            serde_json::to_string(&Attestation::Adapter)?,
            r#""adapter""#
        );
        Ok(())
    }

    #[test]
    fn append_event_checked_coarsens_time_bucket_before_log_write() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
        };
        let cand = CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 901,
                size_s: 60,
            },
            zone_id: "zone:test".to_string(),
            confidence: 0.5,
            correlation_token: None,
            attestation: None,
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
        assert_eq!(created_at, i64::from(TEN_MINUTES_S));
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
            attestation: None,
        };
        assert!(enforce_module_event_allowlist(&desc, &cand).is_ok());
    }

    #[test]
    fn module_allowlist_rejects_unauthorized_event_type() {
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
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
            attestation: None,
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
        let seed = "devkey:test:a1b2c3d4e5f6a7b8c9d0";
        let signing_key = signing_key_from_seed(seed)?;
        let verifying_key = verifying_key_from_seed(seed)?;
        let entry_hash = [7u8; 32];
        let signature = sign_entry(
            &SignatureKeys::new(&signing_key),
            &entry_hash,
            crate::crypto::signatures::DOMAIN_SEALED_LOG_ENTRY,
        )?;
        verify_entry_signature(
            &verifying_key,
            &entry_hash,
            &signature,
            SignatureMode::Compat,
            None,
            crate::crypto::signatures::DOMAIN_SEALED_LOG_ENTRY,
        )?;
        Ok(())
    }

    #[test]
    fn open_with_sealed_log_rejects_memory_path() {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let sealed_log = Box::new(InMemorySealedLogStore::default());
        let err = Kernel::open_with_sealed_log(&cfg, sealed_log)
            .err()
            .expect("expected error");
        let message = err.to_string();
        let expected_message = "Using ':memory:' with `open_with_sealed_log` is ambiguous and not supported. For shared in-memory databases, call `shared_memory_uri()` and pass it explicitly in `KernelConfig::db_path`.";
        assert_eq!(message, expected_message);
    }

    #[test]
    fn append_event_checked_rejects_disallowed_module_event() -> Result<()> {
        let cfg = KernelConfig {
            db_path: ":memory:".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
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
            attestation: None,
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
        assert_eq!(
            event_count, 1,
            "disallowed events must emit explicit failure records"
        );
        let payload: String = kernel.conn.query_row(
            "SELECT payload_json FROM sealed_events LIMIT 1",
            [],
            |row| row.get(0),
        )?;
        let record = SealedLogRecord::deserialize_compat(&payload)?;
        match record {
            SealedLogRecord::Failure(ev) => {
                assert_eq!(ev.failure_type, FailureType::GapMissingData)
            }
            _ => panic!("expected failure record for disallowed module event"),
        }
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
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::new(vec!["zone:private".to_string()])?,
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
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
            attestation: None,
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
        assert_eq!(event_count, 1);
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
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::new(vec!["zone:private".to_string()])?,
        };
        let mut kernel = Kernel::open(&cfg)?;
        let desc = ModuleDescriptor {
            id: "test_module",
            allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
            requested_capabilities: &[],
            supported_backends: &[InferenceBackend::Stub],
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
            attestation: None,
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
    fn export_includes_failure_records() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        kernel.append_failure_event(
            FailureType::StorageFull,
            TimeBucket {
                start_epoch_s: 0,
                size_s: TEN_MINUTES_S,
            },
            Some("storage full".to_string()),
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
            ExportAuthMode::SelfExport,
        )?;
        let failures: Vec<&ExportFailureEvent> = artifact
            .batches
            .iter()
            .flat_map(|batch| &batch.buckets)
            .flat_map(|bucket| &bucket.failures)
            .collect();
        assert_eq!(failures.len(), 1);
        assert_eq!(failures[0].failure_type, FailureType::StorageFull);
        Ok(())
    }

    fn append_test_heartbeat(kernel: &mut Kernel, cfg: &KernelConfig, bucket_start: u64) {
        kernel
            .append_heartbeat(
                TimeBucket {
                    start_epoch_s: bucket_start,
                    size_s: TEN_MINUTES_S,
                },
                true,
                100,
                2,
                0,
                &cfg.kernel_version,
                &cfg.ruleset_id,
                cfg.ruleset_hash,
            )
            .expect("append heartbeat");
    }

    #[test]
    fn heartbeat_and_lifecycle_round_trip() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        append_test_heartbeat(&mut kernel, &cfg, 0);
        kernel.append_lifecycle(
            LifecyclePhase::ShutdownClean,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;

        let records = kernel.read_events_ruleset_bound(cfg.ruleset_hash, 100)?;
        assert_eq!(records.len(), 3);
        assert!(matches!(
            records[0],
            SealedLogRecord::Lifecycle(LifecycleRecord {
                phase: LifecyclePhase::Start,
                ..
            })
        ));
        let SealedLogRecord::Heartbeat(hb) = &records[1] else {
            panic!("expected heartbeat record, got {:?}", records[1]);
        };
        assert!(hb.ingest_healthy);
        assert_eq!(hb.frames_captured_delta, 100);
        assert_eq!(hb.events_appended_delta, 2);
        assert_eq!(hb.failures_appended_delta, 0);
        assert!(matches!(
            records[2],
            SealedLogRecord::Lifecycle(LifecycleRecord {
                phase: LifecyclePhase::ShutdownClean,
                ..
            })
        ));
        Ok(())
    }

    #[test]
    fn system_records_serialize_tagged_and_reject_untagged() -> Result<()> {
        let heartbeat = SealedLogRecord::Heartbeat(HeartbeatRecord {
            time_bucket: TimeBucket {
                start_epoch_s: 600,
                size_s: TEN_MINUTES_S,
            },
            ingest_healthy: false,
            frames_captured_delta: 0,
            events_appended_delta: 0,
            failures_appended_delta: 1,
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: [0u8; 32],
        });
        let json = serde_json::to_string(&heartbeat)?;
        assert!(json.contains("\"record_type\":\"heartbeat\""));
        assert!(matches!(
            SealedLogRecord::deserialize_compat(&json)?,
            SealedLogRecord::Heartbeat(_)
        ));

        // Without the tag, the payload must NOT fall back to a legacy
        // Event/Failure parse (system records lack event_type/failure_type, so
        // misclassification is structurally impossible). Pin that guarantee.
        let untagged = json.replacen("\"record_type\":\"heartbeat\",", "", 1);
        assert!(SealedLogRecord::deserialize_compat(&untagged).is_err());

        let lifecycle = SealedLogRecord::Lifecycle(LifecycleRecord {
            phase: LifecyclePhase::Start,
            time_bucket: TimeBucket {
                start_epoch_s: 600,
                size_s: TEN_MINUTES_S,
            },
            kernel_version: "0.0.0-test".to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: [0u8; 32],
        });
        let json = serde_json::to_string(&lifecycle)?;
        // The tag must be the FIRST field: last_lifecycle_phase's SQL pre-filter
        // is anchored to this prefix.
        assert!(json.starts_with("{\"record_type\":\"lifecycle\""));
        assert!(json.contains("\"phase\":\"start\""));
        let untagged = json.replacen("\"record_type\":\"lifecycle\",", "", 1);
        assert!(SealedLogRecord::deserialize_compat(&untagged).is_err());
        Ok(())
    }

    #[test]
    fn unclean_shutdown_detected_on_reopen() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("lifecycle.db");
        let cfg = KernelConfig {
            db_path: db_path.to_string_lossy().to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };

        let mut kernel = Kernel::open(&cfg)?;
        assert_eq!(kernel.last_lifecycle_phase()?, None);
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        // A later failure record whose free-form details mention the lifecycle
        // tag must NOT shadow the real lifecycle record (the SQL pre-filter is
        // anchored to the payload prefix).
        kernel.append_failure_event(
            FailureType::GapMissingData,
            TimeBucket {
                start_epoch_s: 0,
                size_s: TEN_MINUTES_S,
            },
            Some("contains \"record_type\":\"lifecycle\" in details".to_string()),
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        assert_eq!(kernel.last_lifecycle_phase()?, Some(LifecyclePhase::Start));
        drop(kernel);

        // Reopen without a clean shutdown: the trailing Start is the
        // unclean-shutdown signal witnessd reacts to at boot.
        let mut kernel = Kernel::open(&cfg)?;
        assert_eq!(kernel.last_lifecycle_phase()?, Some(LifecyclePhase::Start));

        kernel.append_lifecycle(
            LifecyclePhase::ShutdownClean,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        drop(kernel);

        let kernel = Kernel::open(&cfg)?;
        assert_eq!(
            kernel.last_lifecycle_phase()?,
            Some(LifecyclePhase::ShutdownClean)
        );
        Ok(())
    }

    #[test]
    fn export_skips_system_records() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        append_test_heartbeat(&mut kernel, &cfg, 0);
        kernel.append_failure_event(
            FailureType::GapMissingData,
            TimeBucket {
                start_epoch_s: 0,
                size_s: TEN_MINUTES_S,
            },
            Some("ingest_stalled backend=rtsp consecutive_errors=42".to_string()),
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
            ExportAuthMode::SelfExport,
        )?;
        let (events, failures): (usize, usize) = artifact
            .batches
            .iter()
            .flat_map(|batch| &batch.buckets)
            .fold((0, 0), |(e, f), bucket| {
                (e + bucket.events.len(), f + bucket.failures.len())
            });
        // Only the failure is exportable; heartbeat + lifecycle are system
        // trace records outside the export contract.
        assert_eq!(events, 0);
        assert_eq!(failures, 1);
        Ok(())
    }

    #[test]
    fn timeline_audit_clean_on_healthy_chain() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let now_bucket = TimeBucket::now_10min()?.start_epoch_s;
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        append_test_heartbeat(&mut kernel, &cfg, now_bucket);
        append_test_heartbeat(&mut kernel, &cfg, now_bucket + 600);

        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 600)?;
        assert!(warnings.is_empty(), "unexpected warnings: {warnings:?}");
        Ok(())
    }

    #[test]
    fn timeline_audit_warns_on_missing_heartbeat_bucket() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let now_bucket = TimeBucket::now_10min()?.start_epoch_s;
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        // Heartbeats for the start bucket and start+1200, nothing for
        // start+600 — the shape mid-chain record deletion leaves behind.
        append_test_heartbeat(&mut kernel, &cfg, now_bucket);
        append_test_heartbeat(&mut kernel, &cfg, now_bucket + 1200);

        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 1200)?;
        assert!(
            warnings.iter().any(|w| w.contains("no heartbeat")),
            "expected missing-heartbeat warning, got: {warnings:?}"
        );
        Ok(())
    }

    #[test]
    fn timeline_audit_warns_on_stale_tail() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let now_bucket = TimeBucket::now_10min()?.start_epoch_s;
        kernel.append_lifecycle(
            LifecyclePhase::Start,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        append_test_heartbeat(&mut kernel, &cfg, now_bucket);

        // Verifier runs "later": the daemon never sealed a shutdown record but
        // the chain stopped growing — tail truncation or a crash.
        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 3600)?;
        assert!(
            warnings.iter().any(|w| w.contains("chain tail is stale")),
            "expected stale-tail warning, got: {warnings:?}"
        );

        // A clean shutdown clears the suspicion.
        kernel.append_lifecycle(
            LifecyclePhase::ShutdownClean,
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 3600)?;
        assert!(
            !warnings.iter().any(|w| w.contains("chain tail is stale")),
            "stale-tail warning should clear after clean shutdown: {warnings:?}"
        );
        Ok(())
    }

    #[test]
    fn timeline_audit_flags_created_at_regression() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;
        let now_bucket = TimeBucket::now_10min()?.start_epoch_s;
        // Later id with an earlier bucket = created_at regression.
        append_test_heartbeat(&mut kernel, &cfg, now_bucket + 600);
        append_test_heartbeat(&mut kernel, &cfg, now_bucket);

        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 600)?;
        assert!(
            warnings
                .iter()
                .any(|w| w.contains("created_at regression") && w.contains("no ClockSkew")),
            "expected unexplained regression warning, got: {warnings:?}"
        );

        // The same regression accompanied by a ClockSkew failure record in the
        // bucket is downgraded to a note: the daemon witnessed the jump.
        let (mut kernel, cfg) = setup_test_kernel()?;
        append_test_heartbeat(&mut kernel, &cfg, now_bucket + 600);
        kernel.append_failure_event(
            FailureType::ClockSkew,
            TimeBucket {
                start_epoch_s: now_bucket,
                size_s: TEN_MINUTES_S,
            },
            Some("wallclock drift 120s vs monotonic".to_string()),
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )?;
        let warnings = crate::verify::audit_chain_timeline(&kernel.conn, now_bucket + 600)?;
        assert!(
            warnings
                .iter()
                .any(|w| w.starts_with("note:") && w.contains("ClockSkew")),
            "expected softened regression note, got: {warnings:?}"
        );
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
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
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
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
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

    /// H1: the runtime unseal gate re-derives the quorum against the policy
    /// and refuses a `Granted` receipt that does not actually carry
    /// `policy.n` distinct valid trustee approvals — even though the receipt
    /// is device-signed and correctly hash-chained. This is the load-bearing
    /// Invariant-V check: a device-key holder can append a forged receipt, but
    /// cannot manufacture the trustee signatures the quorum requires.
    #[test]
    fn runtime_gate_rejects_forged_granted_receipt() -> Result<()> {
        let (mut kernel, _cfg) = setup_test_kernel()?;
        let bucket = TimeBucket::now(600)?;
        let trustee = SigningKey::from_bytes(&[21u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: trustee.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&policy)?;
        let request = UnlockRequest::new("vault:rt", [4u8; 32], "incident", bucket)?;

        // Legit: a Granted receipt backed by one real trustee approval, stored
        // via the device-signed hash-chain path. The gate returns Granted.
        let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &trustee);
        let (_tok, legit) =
            BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
        let legit_hash =
            kernel.append_break_glass_receipt(&legit, std::slice::from_ref(&approval))?;
        let dev = kernel.device_verifying_key();
        let outcome = break_glass_receipt_outcome_for_verifier(
            &kernel.conn,
            &dev,
            "vault:rt",
            [4u8; 32],
            &legit_hash,
            kernel.device_pq_public_key_ref(),
        )?;
        assert!(matches!(outcome, BreakGlassOutcome::Granted));

        // Forged: outcome=Granted with ZERO approvals, appended through the
        // SAME device-signed hash-chain path (models a device-key holder). The
        // signature and chain are valid, but the quorum re-derivation finds
        // 0 < n valid approvals, so the gate rejects it.
        // Commitment matches the active policy so the gate runs the full quorum
        // re-derivation (rather than short-circuiting on a policy-era mismatch).
        let forged = BreakGlassReceipt {
            vault_envelope_id: "vault:rt".to_string(),
            request_hash: request.request_hash(),
            ruleset_hash: [4u8; 32],
            time_bucket: bucket,
            trustees_used: vec![],
            approvals_commitment: approvals_commitment(&[]),
            policy_commitment: policy.commitment(),
            outcome: BreakGlassOutcome::Granted,
        };
        let forged_hash = kernel.append_break_glass_receipt(&forged, &[])?;
        let dev = kernel.device_verifying_key();
        let result = break_glass_receipt_outcome_for_verifier(
            &kernel.conn,
            &dev,
            "vault:rt",
            [4u8; 32],
            &forged_hash,
            kernel.device_pq_public_key_ref(),
        );
        assert!(
            result.is_err(),
            "forged empty-approvals Granted receipt must be rejected at the unseal gate"
        );
        Ok(())
    }

    /// R1: the unseal gate fails CLOSED when the quorum policy was rotated
    /// after a receipt was authorized — a token backed by a receipt from the
    /// prior policy era must not release cleartext against the new policy.
    #[test]
    fn runtime_gate_refuses_receipt_from_rotated_policy() -> Result<()> {
        let (mut kernel, _cfg) = setup_test_kernel()?;
        let bucket = TimeBucket::now(600)?;
        let alice = SigningKey::from_bytes(&[21u8; 32]);
        let old_policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&old_policy)?;
        let request = UnlockRequest::new("vault:rt", [4u8; 32], "incident", bucket)?;
        let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &alice);
        let (_tok, receipt) = BreakGlass::authorize(
            &old_policy,
            &request,
            std::slice::from_ref(&approval),
            bucket,
        );
        let hash = kernel.append_break_glass_receipt(&receipt, std::slice::from_ref(&approval))?;

        // Rotate the policy (add a second trustee, raise the threshold).
        let bob = SigningKey::from_bytes(&[22u8; 32]);
        let new_policy = QuorumPolicy::new(
            2,
            vec![
                TrusteeEntry {
                    id: TrusteeId::new("alice"),
                    public_key: alice.verifying_key().to_bytes(),
                },
                TrusteeEntry {
                    id: TrusteeId::new("bob"),
                    public_key: bob.verifying_key().to_bytes(),
                },
            ],
        )?;
        kernel.set_break_glass_policy(&new_policy)?;

        let dev = kernel.device_verifying_key();
        let result = break_glass_receipt_outcome_for_verifier(
            &kernel.conn,
            &dev,
            "vault:rt",
            [4u8; 32],
            &hash,
            kernel.device_pq_public_key_ref(),
        );
        assert!(
            result.is_err(),
            "a receipt whose policy commitment no longer matches the active policy must be refused"
        );
        Ok(())
    }

    /// Per-envelope binding (Invariant V): a genuine Granted receipt with real
    /// trustee approvals for envelope A must NOT authorize unsealing envelope
    /// B. The gate binds the receipt to the token's envelope/ruleset, so
    /// trustee consent cannot be replayed across disclosures even by a
    /// device-key holder pairing a real receipt with a token for a different
    /// envelope.
    #[test]
    fn runtime_gate_binds_receipt_to_token_envelope() -> Result<()> {
        let (mut kernel, _cfg) = setup_test_kernel()?;
        let bucket = TimeBucket::now(600)?;
        let alice = SigningKey::from_bytes(&[21u8; 32]);
        let policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy(&policy)?;

        // A genuine, fully-approved Granted receipt for envelope A.
        let request_a = UnlockRequest::new("vault:A", [4u8; 32], "incident", bucket)?;
        let approval = Approval::signed(TrusteeId::new("alice"), request_a.request_hash(), &alice);
        let (_tok, receipt_a) =
            BreakGlass::authorize(&policy, &request_a, std::slice::from_ref(&approval), bucket);
        let hash_a =
            kernel.append_break_glass_receipt(&receipt_a, std::slice::from_ref(&approval))?;
        let dev = kernel.device_verifying_key();

        // Bound to envelope A (its real envelope): the gate honors it.
        assert!(matches!(
            break_glass_receipt_outcome_for_verifier(
                &kernel.conn,
                &dev,
                "vault:A",
                [4u8; 32],
                &hash_a,
                kernel.device_pq_public_key_ref(),
            )?,
            BreakGlassOutcome::Granted
        ));

        // Replayed against envelope B (a token authorizing a DIFFERENT
        // envelope): the binding check rejects it even though the receipt's
        // quorum is genuinely satisfied.
        let result = break_glass_receipt_outcome_for_verifier(
            &kernel.conn,
            &dev,
            "vault:B",
            [4u8; 32],
            &hash_a,
            kernel.device_pq_public_key_ref(),
        );
        assert!(
            result.is_err(),
            "a receipt for envelope A must not authorize unsealing envelope B"
        );

        // And a token claiming a different ruleset is likewise refused.
        let ruleset_mismatch = break_glass_receipt_outcome_for_verifier(
            &kernel.conn,
            &dev,
            "vault:A",
            [7u8; 32],
            &hash_a,
            kernel.device_pq_public_key_ref(),
        );
        assert!(
            ruleset_mismatch.is_err(),
            "a receipt for one ruleset must not authorize a token with another"
        );
        Ok(())
    }

    /// Invariant V (spec/quorum_unseal_v2.md §3.1): replacing a LIVE policy
    /// through the gated path requires the CURRENT quorum's consent —
    /// bootstrap is free, mutation is not, consent from the proposed roster
    /// or from the wrong signature domain never counts, and every accepted
    /// write chains a device-signed history record.
    #[test]
    fn gated_policy_set_bootstrap_then_requires_current_quorum() -> Result<()> {
        use crate::break_glass::{
            sign_approval, sign_policy_change_approval, PolicyChangeProposal,
        };

        let (mut kernel, _cfg) = setup_test_kernel()?;
        let bucket = TimeBucket::now(600)?;
        let alice = SigningKey::from_bytes(&[31u8; 32]);
        let bob = SigningKey::from_bytes(&[32u8; 32]);
        let policy_a = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
        let policy_b = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("bob"),
                public_key: bob.verifying_key().to_bytes(),
            }],
        )?;

        // Bootstrap on an empty database needs no approvals.
        assert_eq!(
            kernel.set_break_glass_policy_gated(&policy_a, &[], bucket)?,
            PolicyChangeOutcome::Bootstrapped
        );
        // Idempotent re-set writes nothing.
        assert_eq!(
            kernel.set_break_glass_policy_gated(&policy_a, &[], bucket)?,
            PolicyChangeOutcome::Unchanged
        );
        assert_eq!(kernel.policy_change_history()?.len(), 1);

        // Mutation with no approvals is refused and the policy is unchanged.
        assert!(kernel
            .set_break_glass_policy_gated(&policy_b, &[], bucket)
            .is_err());
        assert_eq!(
            kernel.break_glass_policy().unwrap().trustees[0].id.0,
            "alice"
        );

        let proposal = PolicyChangeProposal {
            prev_policy_commitment: policy_a.full_commitment(),
            new_policy: policy_b.clone(),
            time_bucket: bucket,
        };
        let change_hash = proposal.change_hash();

        // Consent from the PROPOSED roster must not count — the quorum being
        // replaced is the one that must consent.
        let bob_consent = Approval::new(
            TrusteeId::new("bob"),
            change_hash,
            sign_policy_change_approval(&bob, &change_hash).to_vec(),
        );
        assert!(kernel
            .set_break_glass_policy_gated(&policy_b, std::slice::from_ref(&bob_consent), bucket)
            .is_err());

        // An unlock-approval-domain signature from a current trustee must not
        // count as roster consent (disjoint domains).
        let wrong_domain = Approval::new(
            TrusteeId::new("alice"),
            change_hash,
            sign_approval(&alice, &change_hash).to_vec(),
        );
        assert!(kernel
            .set_break_glass_policy_gated(&policy_b, &[wrong_domain], bucket)
            .is_err());

        // Genuine current-trustee consent replaces the policy.
        let alice_consent = Approval::new(
            TrusteeId::new("alice"),
            change_hash,
            sign_policy_change_approval(&alice, &change_hash).to_vec(),
        );
        assert_eq!(
            kernel.set_break_glass_policy_gated(&policy_b, &[alice_consent], bucket)?,
            PolicyChangeOutcome::Replaced
        );
        assert_eq!(kernel.break_glass_policy().unwrap().trustees[0].id.0, "bob");

        // Consent is bucket-bound: an approval minted for one window does not
        // authorize the same change applied in a different window.
        let back_proposal = PolicyChangeProposal {
            prev_policy_commitment: policy_b.full_commitment(),
            new_policy: policy_a.clone(),
            time_bucket: bucket,
        };
        let back_hash = back_proposal.change_hash();
        let bob_back = Approval::new(
            TrusteeId::new("bob"),
            back_hash,
            sign_policy_change_approval(&bob, &back_hash).to_vec(),
        );
        let later_bucket = TimeBucket {
            start_epoch_s: bucket.start_epoch_s + 600,
            size_s: bucket.size_s,
        };
        assert!(kernel
            .set_break_glass_policy_gated(&policy_a, &[bob_back], later_bucket)
            .is_err());

        // The history ledger chains and every record is device-signed.
        let history = kernel.policy_change_history()?;
        assert_eq!(history.len(), 2);
        assert!(history[0].record.bootstrap);
        assert!(!history[1].record.bootstrap);
        let dev = kernel.device_verifying_key();
        let mut prev = [0u8; 32];
        for entry in &history {
            assert_eq!(entry.prev_hash, prev);
            assert_eq!(
                hash_entry(&entry.prev_hash, entry.payload_json.as_bytes()),
                entry.entry_hash
            );
            let sig = <[u8; 64]>::try_from(entry.signature.as_slice()).unwrap();
            crate::crypto::signatures::verify_ed25519_only(
                DOMAIN_POLICY_CHANGE_RECORD,
                &dev,
                &entry.entry_hash,
                &sig,
            )?;
            prev = entry.entry_hash;
        }
        Ok(())
    }

    /// The authenticated policy-change history (`load_authenticated_policy_eras`)
    /// is the ground truth for a receipt's era. A device-key holder can forge a
    /// chained, device-signed history row, but cannot manufacture the prior
    /// era's trustee consent — so a fabricated era without genuine prior-quorum
    /// approvals makes the whole ledger untrusted. Tampering the recorded
    /// approvals of a real row is likewise caught by the signed commitment.
    #[test]
    fn authenticated_history_rejects_forged_era_and_tampered_approvals() -> Result<()> {
        use crate::break_glass::{sign_policy_change_approval, PolicyChangeProposal};

        let (mut kernel, cfg) = setup_test_kernel()?;
        let bucket = TimeBucket::now(600)?;
        let alice = SigningKey::from_bytes(&[41u8; 32]);
        let policy_a = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("alice"),
                public_key: alice.verifying_key().to_bytes(),
            }],
        )?;
        kernel.set_break_glass_policy_gated(&policy_a, &[], bucket)?;
        let dev = kernel.device_verifying_key();
        let pq = kernel.device_pq_public_key_ref();

        // The genuine bootstrap authenticates.
        let eras = crate::verify::load_authenticated_policy_eras(&kernel.conn, &dev, pq)?;
        assert!(eras.iter().any(|p| p.commitment() == policy_a.commitment()));

        // Forge a chained, device-signed row claiming a new attacker-controlled
        // era, but with NO prior-quorum consent (empty approvals). The device
        // signature is valid (the forger holds the device key); the missing
        // consent is what the loader must catch.
        let attacker = SigningKey::from_bytes(&[99u8; 32]);
        let attacker_policy = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("attacker"),
                public_key: attacker.verifying_key().to_bytes(),
            }],
        )?;
        let proposal = PolicyChangeProposal {
            prev_policy_commitment: policy_a.full_commitment(),
            new_policy: attacker_policy.clone(),
            time_bucket: bucket,
        };
        let forged = PolicyChangeRecord {
            prev_policy: Some(policy_a.clone()),
            new_policy: attacker_policy.clone(),
            change_hash: proposal.change_hash(),
            time_bucket: bucket,
            bootstrap: false,
            approvals_commitment: crate::break_glass::approvals_commitment(&[]),
        };
        let payload = serde_json::to_string(&forged)?;
        let signing_key = crate::signing_key_from_seed(&cfg.device_key_seed)?;
        let prev_hash = kernel.last_policy_change_hash_or_zero()?;
        let entry_hash = hash_entry(&prev_hash, payload.as_bytes());
        let sig = crate::crypto::signatures::sign_ed25519_only(
            DOMAIN_POLICY_CHANGE_RECORD,
            &signing_key,
            &entry_hash,
        );
        kernel.conn.execute(
            "INSERT INTO policy_change_history(created_at, payload_json, approvals_json, prev_hash, entry_hash, signature) \
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![0i64, payload, "[]", prev_hash.to_vec(), entry_hash.to_vec(), sig.to_vec()],
        )?;
        assert!(
            crate::verify::load_authenticated_policy_eras(&kernel.conn, &dev, pq).is_err(),
            "a device-signed era with no prior-quorum consent must be rejected"
        );

        // Second scenario: a genuine change, then its recorded approvals are
        // swapped out — the signed commitment no longer matches.
        let (mut kernel2, _cfg2) = setup_test_kernel()?;
        kernel2.set_break_glass_policy_gated(&policy_a, &[], bucket)?;
        let policy_b = QuorumPolicy::new(
            1,
            vec![TrusteeEntry {
                id: TrusteeId::new("bob"),
                public_key: SigningKey::from_bytes(&[42u8; 32])
                    .verifying_key()
                    .to_bytes(),
            }],
        )?;
        let change = PolicyChangeProposal {
            prev_policy_commitment: policy_a.full_commitment(),
            new_policy: policy_b.clone(),
            time_bucket: bucket,
        };
        let alice_consent = Approval::new(
            TrusteeId::new("alice"),
            change.change_hash(),
            sign_policy_change_approval(&alice, &change.change_hash()).to_vec(),
        );
        kernel2.set_break_glass_policy_gated(&policy_b, &[alice_consent], bucket)?;
        let dev2 = kernel2.device_verifying_key();
        // Authentic history authenticates.
        assert!(crate::verify::load_authenticated_policy_eras(&kernel2.conn, &dev2, None).is_ok());
        // Swap the change row's approvals to the empty set (valid JSON) — the
        // approvals no longer match the signed commitment.
        kernel2.conn.execute(
            "UPDATE policy_change_history SET approvals_json = '[]' WHERE payload_json LIKE '%\"bootstrap\":false%'",
            [],
        )?;
        assert!(
            crate::verify::load_authenticated_policy_eras(&kernel2.conn, &dev2, None).is_err(),
            "swapped approvals must fail the signed-commitment binding"
        );
        Ok(())
    }

    #[test]
    fn is_crypto_error_classifies_errors_correctly() {
        // Crypto-related errors should be detected
        let crypto_err = anyhow::anyhow!("signature verification failed");
        assert!(Kernel::is_crypto_error(&crypto_err));

        let signing_err = anyhow::anyhow!("signing operation failed");
        assert!(Kernel::is_crypto_error(&signing_err));

        let key_err = anyhow::anyhow!("invalid key format");
        assert!(Kernel::is_crypto_error(&key_err));

        // Non-crypto errors should not be detected as crypto errors
        let storage_err = anyhow::anyhow!("database write failed");
        assert!(!Kernel::is_crypto_error(&storage_err));

        let generic_err = anyhow::anyhow!("operation failed");
        assert!(!Kernel::is_crypto_error(&generic_err));
    }

    #[test]
    fn report_storage_failure_records_alarm() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;

        // Report a storage failure
        kernel.report_storage_failure(
            "test storage failure",
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );

        // Verify failure event was recorded in sealed log
        let artifact = kernel.export_events_sequential_unchecked(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                ..ExportOptions::default()
            },
            ExportAuthMode::SelfExport,
        )?;
        let failures: Vec<&ExportFailureEvent> = artifact
            .batches
            .iter()
            .flat_map(|batch| &batch.buckets)
            .flat_map(|bucket| &bucket.failures)
            .collect();

        assert_eq!(failures.len(), 1);
        assert_eq!(failures[0].failure_type, FailureType::StorageWriteFailed);
        assert!(failures[0]
            .details
            .as_ref()
            .unwrap()
            .contains("test storage failure"));
        Ok(())
    }

    #[test]
    fn report_crypto_failure_records_alarm() -> Result<()> {
        let (mut kernel, cfg) = setup_test_kernel()?;

        // Report a crypto failure
        kernel.report_crypto_failure(
            "test crypto failure",
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        );

        // Verify failure event was recorded in sealed log
        let artifact = kernel.export_events_sequential_unchecked(
            cfg.ruleset_hash,
            ExportOptions {
                jitter_s: 0,
                ..ExportOptions::default()
            },
            ExportAuthMode::SelfExport,
        )?;
        let failures: Vec<&ExportFailureEvent> = artifact
            .batches
            .iter()
            .flat_map(|batch| &batch.buckets)
            .flat_map(|bucket| &bucket.failures)
            .collect();

        assert_eq!(failures.len(), 1);
        assert_eq!(failures[0].failure_type, FailureType::CryptoFailure);
        assert!(failures[0]
            .details
            .as_ref()
            .unwrap()
            .contains("test crypto failure"));
        Ok(())
    }

    #[test]
    fn sqlcipher_encrypted_db_not_plaintext_readable() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("encrypted_test.db");
        let db_path_str = db_path.to_string_lossy().to_string();

        // Create a kernel which will create an encrypted database
        let cfg = KernelConfig {
            db_path: db_path_str.clone(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(60),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };
        let _kernel = Kernel::open(&cfg)?;
        drop(_kernel);

        // Read the raw database file and verify it does NOT start with
        // the SQLite plaintext header "SQLite format 3\0"
        let raw = std::fs::read(&db_path)?;
        assert!(
            !raw.starts_with(b"SQLite format 3\0"),
            "encrypted database should not have a plaintext SQLite header"
        );

        // Opening without a key should fail
        let plain_conn = Connection::open(&db_path)?;
        let result = plain_conn.query_row("SELECT count(*) FROM sqlite_master", [], |_| Ok(()));
        assert!(
            result.is_err(),
            "opening encrypted DB without key should fail"
        );

        Ok(())
    }

    #[test]
    fn sqlcipher_migration_from_unencrypted() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("migrate_test.db");
        let db_path_str = db_path.to_string_lossy().to_string();

        // Step 1: Create an unencrypted database directly (simulating pre-SQLCipher state)
        {
            let conn = Connection::open(&db_path)?;
            conn.execute_batch(
                "CREATE TABLE test_data (id INTEGER PRIMARY KEY, val TEXT);
                 INSERT INTO test_data (val) VALUES ('migration_sentinel');",
            )?;
        }

        // Verify it's plaintext
        let raw = std::fs::read(&db_path)?;
        assert!(
            raw.starts_with(b"SQLite format 3\0"),
            "should start plaintext"
        );

        // Step 2: Open with a key — this should trigger migration
        let signing_key = signing_key_from_seed("devkey:test:a1b2c3d4e5f6a7b8c9d0")?;
        let db_key = derive_db_encryption_key(&signing_key);
        let conn = open_db_connection_with_key(&db_path_str, Some(&db_key))?;

        // Verify we can read the migrated data
        let val: String = conn.query_row("SELECT val FROM test_data WHERE id = 1", [], |row| {
            row.get(0)
        })?;
        assert_eq!(val, "migration_sentinel");
        drop(conn);

        // Verify the file is now encrypted (no plaintext header)
        let raw = std::fs::read(&db_path)?;
        assert!(
            !raw.starts_with(b"SQLite format 3\0"),
            "migrated database should be encrypted"
        );

        Ok(())
    }

    #[test]
    fn connection_busy_timeout_policy_is_owned() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("busy.db");
        let conn = open_db_connection(&db_path.to_string_lossy())?;
        let ms: i64 = conn.query_row("PRAGMA busy_timeout", [], |row| row.get(0))?;
        assert_eq!(ms as u128, DB_BUSY_TIMEOUT.as_millis());
        Ok(())
    }

    #[test]
    fn concurrent_writers_cannot_fork_the_chain() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("chain.db").to_string_lossy().to_string();
        let cfg = KernelConfig {
            db_path: db_path.clone(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(3600),
            device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
            zone_policy: ZonePolicy::default(),
        };

        // Create the DB (schema, key, any first-open records) before the race.
        let baseline: i64 = {
            let kernel = Kernel::open(&cfg)?;
            kernel
                .conn
                .query_row("SELECT COUNT(*) FROM sealed_events", [], |row| row.get(0))?
        };

        let mut handles = Vec::new();
        for _writer in 0..2 {
            let cfg = cfg.clone();
            handles.push(std::thread::spawn(move || -> Result<()> {
                let mut kernel = Kernel::open(&cfg)?;
                let desc = ModuleDescriptor {
                    id: "test_module",
                    allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
                    requested_capabilities: &[],
                    supported_backends: &[InferenceBackend::Stub],
                };
                for _ in 0..25 {
                    let cand = CandidateEvent {
                        event_type: EventType::BoundaryCrossingObjectLarge,
                        time_bucket: TimeBucket {
                            start_epoch_s: 0,
                            size_s: 600,
                        },
                        zone_id: "zone:public".to_string(),
                        confidence: 0.5,
                        correlation_token: None,
                        attestation: None,
                    };
                    kernel.append_event_checked(
                        &desc,
                        cand,
                        &cfg.kernel_version,
                        &cfg.ruleset_id,
                        cfg.ruleset_hash,
                    )?;
                }
                Ok(())
            }));
        }
        for handle in handles {
            handle.join().expect("writer thread panicked")?;
        }

        // Every record's prev_hash must equal the previous record's
        // entry_hash. A stale-head append — the fork `in_immediate_write_tx`
        // exists to prevent — breaks this walk.
        let kernel = Kernel::open(&cfg)?;
        let mut stmt = kernel
            .conn
            .prepare("SELECT prev_hash, entry_hash FROM sealed_events ORDER BY id")?;
        let mut rows = stmt.query([])?;
        let mut expected_prev: Option<Vec<u8>> = None;
        let mut count: i64 = 0;
        while let Some(row) = rows.next()? {
            let prev: Vec<u8> = row.get(0)?;
            let entry: Vec<u8> = row.get(1)?;
            if let Some(expected) = &expected_prev {
                assert_eq!(
                    &prev, expected,
                    "chain fork: record {count} does not extend the previous head"
                );
            }
            expected_prev = Some(entry);
            count += 1;
        }
        assert_eq!(count, baseline + 50);
        Ok(())
    }

    #[test]
    fn db_key_from_independent_secret_is_decoupled_from_signing_key() -> Result<()> {
        let sk = signing_key_from_seed("devkey:test:1111aaaa2222bbbb3333")?;
        // Backward compatibility: the signing-key derivation is byte-identical to the
        // secret derivation over the signing key's bytes, so existing databases open.
        assert_eq!(
            derive_db_encryption_key(&sk).as_str(),
            derive_db_encryption_key_from_secret(sk.as_bytes()).as_str()
        );
        // An independent secret yields a different key from the signing key.
        let indep = derive_db_encryption_key_from_secret(b"db-master-secret-001");
        assert_ne!(derive_db_encryption_key(&sk).as_str(), indep.as_str());
        // resolve(): a non-empty independent seed wins; None/blank falls back to the key.
        assert_eq!(
            resolve_db_encryption_key(&sk, Some("db-master-secret-001")).as_str(),
            indep.as_str()
        );
        assert_eq!(
            resolve_db_encryption_key(&sk, None).as_str(),
            derive_db_encryption_key(&sk).as_str()
        );
        assert_eq!(
            resolve_db_encryption_key(&sk, Some("   ")).as_str(),
            derive_db_encryption_key(&sk).as_str()
        );
        Ok(())
    }

    #[test]
    fn db_keyed_by_independent_secret_survives_signing_key_rotation() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("decoupled.db");
        let db_path_str = db_path.to_string_lossy().to_string();

        // Key the database by an independent secret and write a sentinel row.
        let db_key = derive_db_encryption_key_from_secret(b"independent-db-secret");
        {
            let conn = open_db_connection_with_key(&db_path_str, Some(&db_key))?;
            conn.execute_batch(
                "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
                 INSERT INTO t (v) VALUES ('sentinel');",
            )?;
        }

        // "Rotate" the device identity: two distinct signing keys.
        let sk_old = signing_key_from_seed("devkey:old:aaaa1111bbbb2222cccc3333dddd")?;
        let sk_new = signing_key_from_seed("devkey:new:eeee5555ffff6666aaaa7777bbbb")?;
        assert_ne!(sk_old.as_bytes(), sk_new.as_bytes());

        // The DB key (from the independent secret) is unchanged by rotation, so the
        // *encrypted database* still decrypts after the signing identity changes. This is
        // the storage-layer prerequisite for `Kernel::rotate_device_identity`, which
        // handles the signing-identity rotation itself (see docs/db_key_rotation.md).
        let conn = open_db_connection_with_key(&db_path_str, Some(&db_key))?;
        let v: String = conn.query_row("SELECT v FROM t WHERE id = 1", [], |r| r.get(0))?;
        assert_eq!(v, "sentinel");
        drop(conn);

        // Neither the old nor the new signing-key-derived key can open it — proof the
        // database key is decoupled from the device identity.
        for sk in [&sk_old, &sk_new] {
            let legacy_key = derive_db_encryption_key(sk);
            let conn = Connection::open(&db_path_str)?;
            assert!(
                apply_sqlcipher_key(&conn, &legacy_key).is_err(),
                "a signing-key-derived key must not open a secret-keyed database"
            );
        }
        Ok(())
    }

    #[test]
    fn rekey_database_file_rotates_the_db_key() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("rekey.db");
        let db_path_str = db_path.to_string_lossy().to_string();

        let key_a = derive_db_encryption_key_from_secret(b"secret-A");
        let key_b = derive_db_encryption_key_from_secret(b"secret-B");

        {
            let conn = open_db_connection_with_key(&db_path_str, Some(&key_a))?;
            conn.execute_batch(
                "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
                 INSERT INTO t (v) VALUES ('rotate-me');",
            )?;
        }

        // Rotate A -> B.
        rekey_database_file(&db_path_str, &key_a, &key_b)?;

        // Opens with B; data intact.
        let conn = open_db_connection_with_key(&db_path_str, Some(&key_b))?;
        let v: String = conn.query_row("SELECT v FROM t WHERE id = 1", [], |r| r.get(0))?;
        assert_eq!(v, "rotate-me");
        drop(conn);

        // No longer opens with A.
        let conn = Connection::open(&db_path_str)?;
        assert!(
            apply_sqlcipher_key(&conn, &key_a).is_err(),
            "the old key must be rejected after rekey"
        );
        drop(conn);

        // Rekey with the wrong current key is rejected (does not corrupt the DB).
        assert!(rekey_database_file(&db_path_str, &key_a, &key_a).is_err());
        // And the DB still opens with B afterwards.
        let conn = open_db_connection_with_key(&db_path_str, Some(&key_b))?;
        let v: String = conn.query_row("SELECT v FROM t WHERE id = 1", [], |r| r.get(0))?;
        assert_eq!(v, "rotate-me");
        Ok(())
    }

    // ===== Device signing-key rotation (F-04) =====

    fn rotation_cfg(db_path: &str, seed: &str) -> KernelConfig {
        KernelConfig {
            db_path: db_path.to_string(),
            ruleset_id: "ruleset:test".to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
            kernel_version: "0.0.0-test".to_string(),
            retention: Duration::from_secs(600),
            device_key_seed: seed.to_string(),
            zone_policy: ZonePolicy::new(vec![]).unwrap(),
        }
    }

    const ROT_OLD_SEED: &str = "devkey:rotate:old:1111111111111111111";
    const ROT_NEW_SEED: &str = "devkey:rotate:new:2222222222222222222";
    const ROT_THIRD_SEED: &str = "devkey:rotate:third:33333333333333333";

    fn verify_chain_from_genesis(conn: &Connection, genesis_pub: &[u8; 32]) -> Result<u64> {
        let genesis_key = ed25519_dalek::VerifyingKey::from_bytes(genesis_pub)?;
        crate::verify::verify_events_with(
            conn,
            &genesis_key,
            None,
            crate::crypto::signatures::SignatureMode::Compat,
            None,
            |_, _| {},
        )
    }

    #[test]
    fn device_key_rotation_keeps_log_verifiable_and_pins_identity() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("rotate.db").to_str().unwrap().to_string();

        let genesis_pub;
        {
            let cfg = rotation_cfg(&db_path, ROT_OLD_SEED);
            let mut kernel = Kernel::open(&cfg)?;
            genesis_pub = kernel.device_key_for_verify_only();
            seal_one_event(&mut kernel, &cfg, "zone:a")?;
            seal_one_event(&mut kernel, &cfg, "zone:b")?;
            kernel.rotate_device_identity(ROT_NEW_SEED)?;
            // Post-rotation event is signed by the NEW key.
            seal_one_event(&mut kernel, &cfg, "zone:c")?;

            // The whole chain — across the rotation — verifies from the genesis key.
            // 2 events + 1 rotation record + 1 event = 4 entries.
            let count = verify_chain_from_genesis(&kernel.conn, &genesis_pub)?;
            assert_eq!(count, 4, "full chain across rotation must verify");
        }

        // The retired (old) seed can no longer reopen the rotated log: the DB is still
        // encrypted under the old-seed-derived key (so it decrypts), but the identity pin
        // rejects it because the current device key is now the rotated one.
        // (Reopening under the *new* identity requires a decoupled DB key — see
        // docs/db_key_rotation.md; exercised separately by the decoupled-DB-key tests.)
        let err = Kernel::open(&rotation_cfg(&db_path, ROT_OLD_SEED))
            .err()
            .expect("a retired seed must be rejected");
        assert!(
            err.to_string().contains("device public key mismatch"),
            "a retired seed must be rejected, got: {err}"
        );
        Ok(())
    }

    #[test]
    fn device_key_rotation_supports_multiple_rotations() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let db_path = dir.path().join("rotate2.db").to_str().unwrap().to_string();
        let cfg = rotation_cfg(&db_path, ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis_pub = kernel.device_key_for_verify_only();

        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;
        seal_one_event(&mut kernel, &cfg, "zone:b")?;
        kernel.rotate_device_identity(ROT_THIRD_SEED)?;
        seal_one_event(&mut kernel, &cfg, "zone:c")?;

        // 1 + rot + 1 + rot + 1 = 5 entries, verifiable from genesis across two rotations.
        let count = verify_chain_from_genesis(&kernel.conn, &genesis_pub)?;
        assert_eq!(count, 5);

        // current key is the third seed's key; genesis is unchanged (the anchor).
        assert_eq!(
            current_device_public_key(&kernel.conn)?,
            signing_key_from_seed(ROT_THIRD_SEED)?
                .verifying_key()
                .to_bytes()
        );
        assert_eq!(genesis_device_public_key(&kernel.conn)?, genesis_pub);
        Ok(())
    }

    #[test]
    fn device_key_rotation_rejects_rotating_to_same_key() -> Result<()> {
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let err = kernel.rotate_device_identity(ROT_OLD_SEED).unwrap_err();
        assert!(err.to_string().contains("nothing to rotate"));
        Ok(())
    }

    #[test]
    fn verifier_detects_tamper_in_post_rotation_event() -> Result<()> {
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis_pub = kernel.device_key_for_verify_only();
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;
        seal_one_event(&mut kernel, &cfg, "zone:c")?;

        // Tamper the LAST (post-rotation) event's payload; verification must fail.
        let last_id: i64 = kernel
            .conn
            .query_row("SELECT MAX(id) FROM sealed_events", [], |r| r.get(0))?;
        kernel.conn.execute(
            "UPDATE sealed_events SET payload_json = replace(payload_json, 'zone:c', 'zone:X') WHERE id = ?1",
            params![last_id],
        )?;
        assert!(
            verify_chain_from_genesis(&kernel.conn, &genesis_pub).is_err(),
            "tampering a post-rotation event must be detected"
        );
        Ok(())
    }

    #[test]
    fn verifier_rejects_rotation_with_forged_new_key() -> Result<()> {
        // A rotation record whose announced new key lacks a valid possession attestation
        // must be rejected even though the (old-key) entry signature is otherwise valid.
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis_pub = kernel.device_key_for_verify_only();
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;

        // Corrupt the stored attestation inside the rotation record, then RE-SIGN the entry
        // with the (still-current-at-that-point) old key so only the attestation is bad.
        let (rot_id, payload): (i64, String) = kernel.conn.query_row(
            "SELECT id, payload_json FROM sealed_events WHERE payload_json LIKE '%key_rotation%' LIMIT 1",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        let mut record: serde_json::Value = serde_json::from_str(&payload)?;
        // Flip one byte of the attestation array.
        let att = record["new_key_attestation"].as_array().unwrap().clone();
        let mut att: Vec<u8> = att.iter().map(|v| v.as_u64().unwrap() as u8).collect();
        att[0] ^= 0xff;
        record["new_key_attestation"] = serde_json::json!(att);
        let new_payload = serde_json::to_string(&record)?;

        let prev_hash: Vec<u8> = kernel.conn.query_row(
            "SELECT prev_hash FROM sealed_events WHERE id = ?1",
            params![rot_id],
            |r| r.get(0),
        )?;
        let mut prev = [0u8; 32];
        prev.copy_from_slice(&prev_hash);
        let new_hash = hash_entry(&prev, new_payload.as_bytes());
        // Re-sign with the OLD key (the key that legitimately signed this rotation entry).
        let old_key = signing_key_from_seed(ROT_OLD_SEED)?;
        let sig = crate::crypto::signatures::sign_with_domain(
            crate::crypto::signatures::DOMAIN_SEALED_LOG_ENTRY,
            &crate::crypto::signatures::SignatureKeys::new(&old_key),
            &new_hash,
        )?;
        kernel.conn.execute(
            "UPDATE sealed_events SET payload_json = ?1, entry_hash = ?2, signature = ?3 WHERE id = ?4",
            params![new_payload, new_hash.to_vec(), sig.ed25519_signature.to_vec(), rot_id],
        )?;

        let err = verify_chain_from_genesis(&kernel.conn, &genesis_pub).unwrap_err();
        assert!(
            err.to_string().contains("rotation attestation"),
            "a forged new-key attestation must be rejected, got: {err}"
        );
        Ok(())
    }

    #[test]
    fn device_key_lineage_validates_and_selects_active_key() -> Result<()> {
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis = kernel.device_key_for_verify_only();
        seal_one_event(&mut kernel, &cfg, "zone:a")?; // id 1
        kernel.rotate_device_identity(ROT_NEW_SEED)?; // id 2 (rotation record)
        seal_one_event(&mut kernel, &cfg, "zone:b")?; // id 3

        let lineage = reconstruct_device_key_lineage(&kernel.conn)?;
        assert_eq!(lineage.len(), 2, "genesis + one rotation");
        assert_eq!(lineage[0].public_key, genesis);
        let new_pub = signing_key_from_seed(ROT_NEW_SEED)?
            .verifying_key()
            .to_bytes();
        assert_eq!(lineage[1].public_key, new_pub);

        // Seeding boundary (P2): the key active at a cutoff that prunes *before* the rotation
        // record is still the genesis key; at/after that boundary it is the new key. This is
        // exactly what seeds a pruned suffix, so a rotation straddling the cutoff verifies.
        let rot_id = lineage[1].activated_at_event_id;
        assert_eq!(device_key_active_at_in(&lineage, rot_id - 1)?, genesis);
        assert_eq!(device_key_active_at_in(&lineage, rot_id)?, new_pub);
        Ok(())
    }

    #[test]
    fn device_key_lineage_rejects_forged_history() -> Result<()> {
        // P1 root cause: the lineage must be unforgeable from an untrusted history table.
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;
        assert_eq!(reconstruct_device_key_lineage(&kernel.conn)?.len(), 2);

        // An attacker rewrites history to announce *their own* key with a valid self-attestation
        // — but cannot forge the genesis key's authorization over the new binding (they lack the
        // genesis private key). Reconstruction must therefore reject the tampered lineage.
        let attacker = signing_key_from_seed(ROT_THIRD_SEED)?;
        let attacker_pub = attacker.verifying_key().to_bytes();
        let genesis = genesis_device_public_key(&kernel.conn)?;
        let forged_attestation = crate::crypto::signatures::sign_rotation_attestation(
            &attacker,
            &genesis,
            &attacker_pub,
        )
        .to_vec();
        kernel.conn.execute(
            "UPDATE device_key_history SET public_key = ?1, attestation = ?2 WHERE epoch = 1",
            params![attacker_pub.to_vec(), forged_attestation],
        )?;

        let err = reconstruct_device_key_lineage(&kernel.conn)
            .expect_err("forged history must be rejected");
        assert!(
            err.to_string().contains("authorization"),
            "rejection must be due to the unforgeable genesis authorization, got: {err}"
        );
        Ok(())
    }

    #[test]
    fn end_to_end_rotation_survives_checkpoint_pruning() -> Result<()> {
        // Exercises the full log_verify key-selection path across a checkpoint that prunes the
        // chain after a rotation (P1 + P2): the suffix is seeded with the lineage key active at
        // the cutoff and the checkpoint is verified with its (lineage-anchored) signer.
        use crate::crypto::signatures::SignatureMode;
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis = kernel.device_key_for_verify_only();

        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;
        seal_one_event(&mut kernel, &cfg, "zone:b")?;
        // Retention=0 checkpoints at the current head and prunes everything up to it (so the
        // pruned prefix includes the rotation; the checkpoint is signed by the new key).
        kernel.enforce_retention_with_checkpoint(Duration::from_secs(0))?;
        seal_one_event(&mut kernel, &cfg, "zone:c")?; // post-checkpoint, new key

        // Replicate log_verify's trusted key selection.
        let lineage = reconstruct_device_key_lineage_from(&kernel.conn, &genesis)?;
        let checkpoint = crate::verify::latest_checkpoint(&kernel.conn)?;
        let cutoff = checkpoint
            .cutoff_event_id
            .expect("a checkpoint was created");
        let chain_key_bytes = device_key_active_at_in(&lineage, cutoff)?;
        let signer = checkpoint.signer_public_key.expect("signer recorded");
        assert!(
            lineage.iter().any(|e| e.public_key == signer),
            "checkpoint signer must be a genesis-anchored lineage key"
        );
        let chain_key = verifying_key_from_bytes(&chain_key_bytes)?;
        let signer_key = verifying_key_from_bytes(&signer)?;

        crate::verify::verify_checkpoint_signature(
            &signer_key,
            &checkpoint,
            SignatureMode::Compat,
            None,
        )?;
        let count = crate::verify::verify_events_with(
            &kernel.conn,
            &chain_key,
            checkpoint.chain_head_hash,
            SignatureMode::Compat,
            None,
            |_, _| {},
        )?;
        assert_eq!(
            count, 1,
            "only the post-checkpoint event remains after pruning"
        );
        Ok(())
    }

    #[test]
    fn device_key_lineage_recovers_legacy_rows_without_authorization() -> Result<()> {
        // Upgrade path: a database rotated under the previous release has history rows with no
        // `authorization`. Reconstruction must recover the genesis anchor from the retained
        // in-chain rotation record instead of locking the user out on open.
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        kernel.rotate_device_identity(ROT_NEW_SEED)?;
        // Simulate a legacy (pre-authorization) history row.
        kernel.conn.execute(
            "UPDATE device_key_history SET authorization = NULL WHERE epoch = 1",
            [],
        )?;

        let new_pub = signing_key_from_seed(ROT_NEW_SEED)?
            .verifying_key()
            .to_bytes();
        let lineage = reconstruct_device_key_lineage(&kernel.conn)?;
        assert_eq!(lineage.len(), 2);
        assert_eq!(lineage[1].public_key, new_pub);
        // current_device_public_key is what Kernel::open consults — it must not lock out.
        assert_eq!(current_device_public_key(&kernel.conn)?, new_pub);

        // If the in-chain record is also gone (pruned), recovery fails closed.
        let rot_id = lineage[1].activated_at_event_id;
        kernel
            .conn
            .execute("DELETE FROM sealed_events WHERE id = ?1", params![rot_id])?;
        let err = reconstruct_device_key_lineage(&kernel.conn)
            .expect_err("a pruned legacy rotation cannot be anchored");
        assert!(err.to_string().contains("pruned"), "got: {err}");
        Ok(())
    }

    #[test]
    fn verifier_accepts_legacy_rotation_record_without_authorization() -> Result<()> {
        // A rotation record written before the authorization field existed has an empty
        // `prev_key_authorization`; verification must accept it, anchored by its old-key entry
        // signature alone (the field is `#[serde(default)]` so it still deserializes).
        let cfg = rotation_cfg(":memory:", ROT_OLD_SEED);
        let mut kernel = Kernel::open(&cfg)?;
        let genesis = kernel.device_key_for_verify_only();
        seal_one_event(&mut kernel, &cfg, "zone:a")?;
        // Keep the rotation as the chain head so rewriting it does not break a successor's
        // prev_hash linkage (legacy realism is in the record shape, not its position).
        kernel.rotate_device_identity(ROT_NEW_SEED)?;

        // Rewrite the in-chain rotation record to drop the authorization field, then re-sign the
        // entry with the old key (exactly what a legacy record looks like).
        let (rot_id, payload): (i64, String) = kernel.conn.query_row(
            "SELECT id, payload_json FROM sealed_events WHERE payload_json LIKE '%key_rotation%' LIMIT 1",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        let mut record: serde_json::Value = serde_json::from_str(&payload)?;
        record
            .as_object_mut()
            .unwrap()
            .remove("prev_key_authorization");
        let new_payload = serde_json::to_string(&record)?;
        // Confirm it round-trips through the real deserializer (serde default fills the field).
        assert!(matches!(
            SealedLogRecord::deserialize_compat(&new_payload)?,
            SealedLogRecord::KeyRotation(_)
        ));

        let prev_hash: Vec<u8> = kernel.conn.query_row(
            "SELECT prev_hash FROM sealed_events WHERE id = ?1",
            params![rot_id],
            |r| r.get(0),
        )?;
        let mut prev = [0u8; 32];
        prev.copy_from_slice(&prev_hash);
        let new_hash = hash_entry(&prev, new_payload.as_bytes());
        let old_key = signing_key_from_seed(ROT_OLD_SEED)?;
        let sig = crate::crypto::signatures::sign_with_domain(
            DOMAIN_SEALED_LOG_ENTRY,
            &crate::crypto::signatures::SignatureKeys::new(&old_key),
            &new_hash,
        )?;
        kernel.conn.execute(
            "UPDATE sealed_events SET payload_json = ?1, entry_hash = ?2, signature = ?3 WHERE id = ?4",
            params![new_payload, new_hash.to_vec(), sig.ed25519_signature.to_vec(), rot_id],
        )?;
        // Also clear the history authorization so the lineage path takes the legacy branch.
        kernel.conn.execute(
            "UPDATE device_key_history SET authorization = NULL WHERE epoch = 1",
            [],
        )?;

        let count = verify_chain_from_genesis(&kernel.conn, &genesis)?;
        assert_eq!(count, 2, "legacy rotation record must verify on the chain");
        assert_eq!(reconstruct_device_key_lineage(&kernel.conn)?.len(), 2);
        Ok(())
    }
}
