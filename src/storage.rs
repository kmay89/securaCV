use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};
use std::collections::{HashSet, VecDeque};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

use crate::crypto::signatures::{SignatureKeys, DOMAIN_CHECKPOINT, DOMAIN_SEALED_LOG_ENTRY};
use crate::{
    hash_entry, now_s, open_db_connection, open_db_connection_with_key, sign_entry, ReprocessGuard,
    SealedLogRecord,
};

pub trait SealedLogStore {
    fn append_record(
        &mut self,
        record: &SealedLogRecord,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()>;

    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()>;

    fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
        log_alarm: &mut dyn FnMut(&str, &str) -> Result<()>,
    ) -> Result<Vec<SealedLogRecord>>;
}

/// Sampling interval of [`MonotonicAgeFloor`]: one sample per 10-minute
/// bucket, the coarsest unit the log itself keeps. The interval is also the
/// floor's slack (see `protected_from`).
const AGE_FLOOR_SAMPLE_INTERVAL: Duration = Duration::from_secs(600);
/// Ring capacity: 2016 ten-minute slots cover 14 days. Bounded by
/// construction (FR-4); a longer run simply forgets its oldest samples and
/// those rows fall back to the wall-clock rule.
const AGE_FLOOR_SAMPLES: usize = 2016;
/// How far the wall clock may run ahead of the monotonic clock across a
/// sample before the stamps after it stop being trusted for expiry. Matches
/// `witnessd`'s default clock-skew tolerance, so any step the clock monitor
/// would seal also engages the floor; a smaller step prunes at most that
/// many seconds early, well inside a bucket. Ordinary crystal drift can
/// exceed this over the ring's window and engage the floor spuriously —
/// harmless, because the floor only ever ages a row by the clock that
/// cannot be stepped.
const AGE_FLOOR_STEP_TOLERANCE: Duration = Duration::from_secs(30);

/// A lower bound on the age of the rows this process appended, kept on the
/// **monotonic** clock so a wall-clock step cannot shorten it.
///
/// Retention ages events by their sealed `created_at`, which is wall-clock
/// time at the moment of sealing. A host whose clock steps forward (an RTC-less
/// Pi taking its first NTP sync after boot; an operator correcting a clock
/// that was years behind) suddenly finds every row it sealed minutes ago
/// "older than retention" and prunes them. The monotonic clock cannot be
/// stepped, so anything appended less than `retention` ago by *that* clock is
/// provably still inside retention whatever the wall clock now says.
///
/// The ring records `(monotonic instant, wall time, rowid)` for the first
/// append of each sample interval. At prune time each sample is checked for a
/// **forward step**: the wall clock now, minus the wall clock at the sample,
/// exceeds the monotonic elapsed time by more than `AGE_FLOOR_STEP_TOLERANCE`.
/// Only such a sample vouches for the rows after it — when the two clocks
/// agree, the stamps are trustworthy and the floor stays out of the way (a
/// backward step makes rows look *younger*, which is the safe direction and
/// is handled by the prefix rule in `prune_upper_bound`). `protected_from`
/// returns the rowid of the oldest stepped-over sample still inside
/// `retention`; every row at or after it was appended after that instant and
/// is therefore younger. Rows appended *within one interval before* that
/// sample are not covered, so the exact guarantee is: **after a forward
/// wall-clock step, a row appended less than `retention −
/// AGE_FLOOR_SAMPLE_INTERVAL` ago by the monotonic clock is never pruned.**
/// Rows sealed by an earlier process (before this daemon started) have no
/// monotonic history and are governed by the wall-clock rule alone.
#[derive(Clone, Debug, Default)]
pub(crate) struct MonotonicAgeFloor {
    samples: VecDeque<AgeSample>,
}

#[derive(Clone, Copy, Debug)]
struct AgeSample {
    mono: Instant,
    wall: SystemTime,
    rowid: i64,
}

impl MonotonicAgeFloor {
    /// Record an append. Cheap: one comparison, and a push once per interval.
    pub(crate) fn note_append(&mut self, rowid: i64, mono: Instant, wall: SystemTime) {
        if let Some(last) = self.samples.back() {
            if mono.duration_since(last.mono) < AGE_FLOOR_SAMPLE_INTERVAL {
                return;
            }
        }
        if self.samples.len() >= AGE_FLOOR_SAMPLES {
            self.samples.pop_front();
        }
        self.samples.push_back(AgeSample { mono, wall, rowid });
    }

    /// The smallest rowid provably younger than `retention` by the monotonic
    /// clock **and** stamped by a wall clock that has since stepped forward,
    /// or `None` when no such row exists (no step, or nothing that young).
    pub(crate) fn protected_from(
        &self,
        retention: Duration,
        mono_now: Instant,
        wall_now: SystemTime,
    ) -> Option<i64> {
        self.samples
            .iter()
            .find(|sample| {
                let mono_elapsed = mono_now.duration_since(sample.mono);
                if mono_elapsed >= retention {
                    return false;
                }
                // A wall clock that went backwards since the sample errors
                // here; that is the safe direction, so it never vouches.
                let Ok(wall_elapsed) = wall_now.duration_since(sample.wall) else {
                    return false;
                };
                wall_elapsed > mono_elapsed + AGE_FLOOR_STEP_TOLERANCE
            })
            .map(|sample| sample.rowid)
    }
}

/// The highest rowid a retention pass may prune, given the wall-clock prefix
/// bound and the monotonic floor. `None` means "no bound from that side".
///
/// - `first_kept`: the smallest rowid whose `created_at` is still inside
///   retention. Everything **before** it is expired by the wall clock; the
///   pruned range is a prefix of the chain, so a row that is older by id but
///   younger by stamp (a clock regression between two appends) stops the
///   prefix instead of being swept along with it.
/// - `mono_floor`: from [`MonotonicAgeFloor::protected_from`].
///
/// Returns `Some(upper)` when the prune is bounded above by `upper`
/// (inclusive; `upper < 1` prunes nothing), or `None` when every row is
/// expired by both clocks.
pub(crate) fn prune_upper_bound(first_kept: Option<i64>, mono_floor: Option<i64>) -> Option<i64> {
    let bounds = [first_kept, mono_floor];
    bounds
        .iter()
        .flatten()
        .map(|first_protected| first_protected - 1)
        .min()
}

pub struct SqliteSealedLogStore {
    conn: Connection,
    /// Monotonic-clock lower bound on the age of rows appended by this
    /// process (see [`MonotonicAgeFloor`]).
    age_floor: MonotonicAgeFloor,
    /// When set, the store maintains a device-signed monotonic high-water-mark
    /// `(seq, head)` at this path on every append (`crate::log::high_water_mark`,
    /// `docs/security/ENTERPRISE_CUSTODY.md` §2). `None` ⇒ no mark is written and
    /// behavior is byte-identical to before this feature.
    high_water_mark_path: Option<PathBuf>,
}

impl SqliteSealedLogStore {
    pub fn open(db_path: &str) -> Result<Self> {
        let conn = open_db_connection(db_path)?;
        let mut store = Self {
            conn,
            age_floor: MonotonicAgeFloor::default(),
            high_water_mark_path: None,
        };
        store.ensure_schema()?;
        Ok(store)
    }

    pub fn open_with_key(db_path: &str, encryption_key: Option<&str>) -> Result<Self> {
        let conn = open_db_connection_with_key(db_path, encryption_key)?;
        let mut store = Self {
            conn,
            age_floor: MonotonicAgeFloor::default(),
            high_water_mark_path: None,
        };
        store.ensure_schema()?;
        Ok(store)
    }

    /// Enable (or disable, with `None`) the signed external high-water-mark.
    /// Chainable on any of the `open*` constructors.
    pub fn with_high_water_mark(mut self, path: Option<PathBuf>) -> Self {
        self.high_water_mark_path = path;
        self
    }

    /// Run `f` inside a `BEGIN IMMEDIATE` transaction. The sealed log is a
    /// hash chain: every append is a read-modify-write of the chain head, so
    /// the head read and the insert must be one critical section — otherwise a
    /// second writer sharing the DB (witnessd plus a bridge is one compose
    /// edit away) can read the same head and fork the chain. IMMEDIATE takes
    /// the write lock at BEGIN, so the head read inside `f` already sees
    /// every committed append.
    fn in_immediate_write_tx<T>(&mut self, f: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        self.conn.execute_batch("BEGIN IMMEDIATE")?;
        match f(self) {
            Ok(value) => {
                self.conn.execute_batch("COMMIT")?;
                Ok(value)
            }
            Err(err) => {
                // Best-effort: the caller must see the original failure, and a
                // transaction left open here is discarded when the connection
                // closes or the next BEGIN fails loudly.
                let _ = self.conn.execute_batch("ROLLBACK");
                Err(err)
            }
        }
    }

    fn ensure_schema(&mut self) -> Result<()> {
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

            CREATE INDEX IF NOT EXISTS idx_events_created ON sealed_events(created_at);
            "#,
        )?;
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
                // enforce_retention_with_checkpoint INSERTs this column, so the
                // standalone store must create it itself — previously only
                // Kernel::ensure_schema did, and a store opened on a fresh path
                // failed its own checkpoint INSERT.
                ("signer_public_key", "BLOB"),
            ],
        )?;
        Ok(())
    }

    fn last_chain_head(&self) -> Result<[u8; 32]> {
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

    fn last_event_hash_or_checkpoint_head(&self) -> Result<[u8; 32]> {
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
            self.last_chain_head()
        }
    }
}

/// SECURITY: Validates that a SQL identifier contains only safe characters.
/// Prevents SQL injection when table/column names are interpolated into DDL
/// statements (which cannot use parameterized queries for identifiers).
fn validate_sql_identifier(name: &str, label: &str) -> Result<()> {
    if name.is_empty() || name.len() > 64 {
        return Err(anyhow!("{} must be 1-64 characters", label));
    }
    if !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
        return Err(anyhow!(
            "{} '{}' contains unsafe characters (only [a-zA-Z0-9_] allowed)",
            label,
            name
        ));
    }
    if name.bytes().next().is_none_or(|b| b.is_ascii_digit()) {
        return Err(anyhow!("{} '{}' must not start with a digit", label, name));
    }
    Ok(())
}

/// SECURITY: Validates a SQL column type expression. More permissive than
/// identifier validation — allows spaces, parentheses, and commas for types
/// like `INTEGER PRIMARY KEY`, `VARCHAR(255)`, or `DECIMAL(10,2)` — while
/// still blocking injection characters (`;`, `--`, `'`, `"`).
fn validate_sql_column_type(type_expr: &str, label: &str) -> Result<()> {
    if type_expr.is_empty() || type_expr.len() > 64 {
        return Err(anyhow!("{} must be 1-64 characters", label));
    }
    if type_expr.contains(';')
        || type_expr.contains("--")
        || type_expr.contains('\'')
        || type_expr.contains('"')
    {
        return Err(anyhow!(
            "{} '{}' contains unsafe characters",
            label,
            type_expr
        ));
    }
    if !type_expr
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b"_ (),".contains(&b))
    {
        return Err(anyhow!(
            "{} '{}' contains disallowed characters (only [a-zA-Z0-9_ (),] allowed)",
            label,
            type_expr
        ));
    }
    Ok(())
}

pub fn ensure_columns(
    conn: &Connection,
    table: &str,
    columns_to_add: &[(&str, &str)],
) -> Result<()> {
    // SECURITY: Validate all identifiers before interpolation into SQL.
    validate_sql_identifier(table, "table name")?;
    for (column, column_type) in columns_to_add {
        validate_sql_identifier(column, "column name")?;
        validate_sql_column_type(column_type, "column type")?;
    }

    let mut stmt = conn.prepare(&format!("PRAGMA table_info({})", table))?;
    let existing_columns: HashSet<String> = stmt
        .query_map([], |row| row.get(1))?
        .collect::<std::result::Result<Vec<String>, _>>()?
        .into_iter()
        .collect();

    for (column, column_type) in columns_to_add {
        if !existing_columns.contains(*column) {
            conn.execute(
                &format!(
                    "ALTER TABLE {} ADD COLUMN {} {}",
                    table, column, column_type
                ),
                [],
            )?;
        }
    }
    Ok(())
}

fn table_exists(conn: &Connection, name: &str) -> Result<bool> {
    let mut stmt =
        conn.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1")?;
    let mut rows = stmt.query(params![name])?;
    Ok(rows.next()?.is_some())
}

/// Write one device-signed checkpoint row `(cutoff_id, head)`. The cutoff is
/// bound into the signature (`log::checkpoint_message`): after a
/// full-retention prune it is what the high-water-mark check reconciles
/// against, so it cannot stay an unsigned column. Used once for the real
/// cutoff and once per anchored head inside the prune range — same signer,
/// same `created_at`, identical PQ columns.
fn write_checkpoint(
    store: &mut SqliteSealedLogStore,
    head: &[u8; 32],
    cutoff_id: i64,
    created_at: i64,
    signature_keys: &SignatureKeys<'_>,
) -> Result<()> {
    let checkpoint_sig = sign_entry(
        signature_keys,
        &crate::log::checkpoint_message(head, cutoff_id),
        DOMAIN_CHECKPOINT,
    )?;
    let checkpoint_pq_signature = checkpoint_sig
        .pq_signature
        .as_ref()
        .map(|sig| sig.signature.clone());
    let checkpoint_pq_scheme = checkpoint_sig
        .pq_signature
        .as_ref()
        .map(|sig| sig.scheme_id.clone());

    // Record which device key signed this checkpoint so verification picks the correct
    // key after a signing-key rotation (and can seed the chain when earlier events are pruned).
    let signer_public_key = signature_keys.ed25519.verifying_key().to_bytes().to_vec();

    store.conn.execute(
        r#"
        INSERT INTO checkpoints(
            created_at,
            cutoff_event_id,
            chain_head_hash,
            signature,
            pq_signature,
            pq_scheme,
            signer_public_key
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
        "#,
        params![
            created_at,
            cutoff_id,
            head.to_vec(),
            checkpoint_sig.ed25519_signature,
            checkpoint_pq_signature,
            checkpoint_pq_scheme,
            signer_public_key
        ],
    )?;
    Ok(())
}

impl SealedLogStore for SqliteSealedLogStore {
    fn append_record(
        &mut self,
        record: &SealedLogRecord,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let created_at = i64::try_from(record.time_bucket().start_epoch_s)
            .map_err(|_| anyhow!("time bucket start exceeds i64 range"))?;
        let payload_json = serde_json::to_string(record)?;

        let (rowid, entry_hash) = self.in_immediate_write_tx(|store| {
            let prev_hash = store.last_event_hash_or_checkpoint_head()?;
            let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
            let signature_set = sign_entry(signature_keys, &entry_hash, DOMAIN_SEALED_LOG_ENTRY)?;
            let pq_signature = signature_set
                .pq_signature
                .as_ref()
                .map(|sig| sig.signature.clone());
            let pq_scheme = signature_set
                .pq_signature
                .as_ref()
                .map(|sig| sig.scheme_id.clone());

            store.conn.execute(
                r#"
                INSERT INTO sealed_events(
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
                    pq_scheme
                ],
            )?;

            Ok((store.conn.last_insert_rowid(), entry_hash))
        })?;
        self.age_floor
            .note_append(rowid, Instant::now(), SystemTime::now());

        // Advance the signed external high-water-mark past the row just sealed.
        // The event is already committed; this runs *after* the transaction so a
        // mark on separate/append-only media never blocks the DB write. It is
        // best-effort by design: the mark only ever moves forward, so a failed
        // advance leaves the mark lagging — which weakens detection of the very
        // newest truncation but can never cause a false verify failure (verify
        // only fails when the live log is *behind* a recorded mark). We surface
        // the failure loudly rather than fail the append, since refusing here
        // would turn a flaky mark mount into a witnessing outage.
        if let Some(path) = &self.high_water_mark_path {
            if let Err(e) =
                crate::log::high_water_mark::advance(path, signature_keys, rowid as u64, entry_hash)
            {
                eprintln!(
                    "warning: sealed-log high-water-mark advance to seq {} failed: {:#}",
                    rowid, e
                );
            }
        }

        Ok(())
    }

    /// Prune events older than `retention` behind a device-signed checkpoint.
    ///
    /// An anchored chain head is a hash a third party countersigned; pruning
    /// must not make it unrecognizable, so it survives as a signed checkpoint
    /// row — the structure every verifier already treats as chain history.
    /// One extra checkpoint `(cutoff_event_id = e, chain_head_hash = H)` is
    /// written for every anchored hash `H` at event `e` inside the prune
    /// range, in ascending id order, before the real cutoff checkpoint, all
    /// inside the one transaction. Only hashes that already have an anchor
    /// row are preserved (an offline query with no response yet is not). The
    /// store never creates `tsa_anchors`; its absence means no anchors. The
    /// pruner never consults anchors to decide *whether* to prune.
    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;
        let mono_floor =
            self.age_floor
                .protected_from(retention, Instant::now(), SystemTime::now());

        // Checkpoint INSERT and event DELETE are one atomic unit: a crash
        // between them previously left a checkpoint with un-pruned events and
        // a duplicate checkpoint on the retention re-run after restart.
        self.in_immediate_write_tx(|store| {
            // The prune is a PREFIX of the chain, bounded by two clocks. The
            // wall clock names the first row still inside retention; every row
            // before it is expired. This used to be "the newest expired row, and
            // everything below it", which after a clock regression (a row sealed
            // later with an older stamp) swept in-retention rows out with it. The
            // monotonic floor then caps the prefix at the oldest row this process
            // appended less than `retention` ago, so a forward clock step cannot
            // expire rows that were sealed minutes ago (`MonotonicAgeFloor`).
            let first_kept: Option<i64> = store.conn.query_row(
                "SELECT MIN(id) FROM sealed_events WHERE created_at >= ?1",
                params![cutoff],
                |r| r.get(0),
            )?;
            let upper = prune_upper_bound(first_kept, mono_floor);
            let mut stmt = match upper {
            Some(upper) if upper < 1 => return Ok(()),
            Some(_) => store.conn.prepare(
                "SELECT id, entry_hash FROM sealed_events WHERE id <= ?1 ORDER BY id DESC LIMIT 1",
            )?,
            None => store
                .conn
                .prepare("SELECT id, entry_hash FROM sealed_events ORDER BY id DESC LIMIT 1")?,
        };
            let mut rows = match upper {
                Some(upper) => stmt.query(params![upper])?,
                None => stmt.query([])?,
            };
            let Some(row) = rows.next()? else {
                return Ok(());
            };

            let cutoff_id: i64 = row.get(0)?;
            let head_bytes: Vec<u8> = row.get(1)?;
            if head_bytes.len() != 32 {
                return Err(anyhow!("corrupt sealed log: entry_hash size"));
            }
            let mut head = [0u8; 32];
            head.copy_from_slice(&head_bytes);
            drop(rows);
            drop(stmt);

            let created_at = now_s()? as i64;

            // Anchored heads inside the prune range (any subject: a `digest` row
            // over a chain head is still a countersigned head), ascending, so
            // the real cutoff checkpoint keeps the highest id and stays what
            // `latest_checkpoint` reads.
            let anchored: Vec<(i64, [u8; 32])> = if table_exists(&store.conn, "tsa_anchors")? {
                let mut stmt = store.conn.prepare(
                    "SELECT DISTINCT e.id, e.entry_hash FROM sealed_events e
                 WHERE e.id < ?1
                   AND EXISTS (SELECT 1 FROM tsa_anchors a WHERE a.subject_hash = e.entry_hash)
                 ORDER BY e.id ASC",
                )?;
                let rows = stmt.query_map(params![cutoff_id], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, Vec<u8>>(1)?))
                })?;
                let mut out = Vec::new();
                for row in rows {
                    let (id, hash) = row?;
                    let hash: [u8; 32] = hash
                        .try_into()
                        .map_err(|_| anyhow!("corrupt sealed log: entry_hash size"))?;
                    out.push((id, hash));
                }
                out
            } else {
                Vec::new()
            };
            for (event_id, anchored_head) in anchored {
                write_checkpoint(store, &anchored_head, event_id, created_at, signature_keys)?;
            }
            write_checkpoint(store, &head, cutoff_id, created_at, signature_keys)?;

            store.conn.execute(
                "DELETE FROM sealed_events WHERE id <= ?1",
                params![cutoff_id],
            )?;

            Ok(())
        })
    }

    fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
        log_alarm: &mut dyn FnMut(&str, &str) -> Result<()>,
    ) -> Result<Vec<SealedLogRecord>> {
        // The limit counts RETURNED records, so it is applied after the
        // rotation skip, not in SQL — a LIMIT in the query would let each
        // rotation row consume a result slot (an [event, rotation, event] log
        // read with limit 2 would return one event, and a window of rotations
        // would come back empty despite eligible later events). Rows stream
        // and the scan stops as soon as `limit` records are collected; the
        // reprocess guard runs afterwards because the alarm sink may need the
        // connection this statement borrows.
        let records = {
            let mut stmt = self
                .conn
                .prepare("SELECT payload_json FROM sealed_events ORDER BY id ASC")?;
            let mut rows = stmt.query([])?;
            let mut records = Vec::new();

            while records.len() < limit {
                let Some(row) = rows.next()? else { break };
                let payload: String = row.get(0)?;
                let record = SealedLogRecord::deserialize_compat(&payload)?;

                // Key rotations are identity-administration records with no
                // ruleset binding (their ruleset_hash is all-zero), so they are
                // exempt from the reprocess guard and excluded from the read —
                // otherwise a single rotation would fail every subsequent read
                // with a false CONFORMANCE_REPROCESS_VIOLATION. Mirrors the
                // export path's skip.
                if matches!(record, SealedLogRecord::KeyRotation(_)) {
                    continue;
                }
                records.push(record);
            }

            records
        };

        let mut out = Vec::with_capacity(records.len());
        for record in records {
            if let Err(e) =
                ReprocessGuard::assert_same_ruleset(expected_ruleset_hash, record.ruleset_hash())
            {
                log_alarm("CONFORMANCE_REPROCESS_VIOLATION", &format!("{}", e))?;
                return Err(e);
            }

            out.push(record);
        }
        Ok(out)
    }
}

#[derive(Clone, Debug)]
struct InMemorySealedEventEntry {
    /// Monotone 1-based id, never reused after a prune (mirrors AUTOINCREMENT).
    id: i64,
    created_at: i64,
    payload_json: String,
    entry_hash: [u8; 32],
}

#[derive(Clone, Debug)]
struct InMemoryCheckpointEntry {
    chain_head_hash: [u8; 32],
}

#[derive(Clone, Debug, Default)]
pub struct InMemorySealedLogStore {
    events: Vec<InMemorySealedEventEntry>,
    checkpoints: Vec<InMemoryCheckpointEntry>,
    next_id: i64,
    age_floor: MonotonicAgeFloor,
}

impl InMemorySealedLogStore {
    fn last_chain_head(&self) -> Result<[u8; 32]> {
        if let Some(checkpoint) = self.checkpoints.last() {
            return Ok(checkpoint.chain_head_hash);
        }

        if let Some(event) = self.events.last() {
            return Ok(event.entry_hash);
        }

        Ok([0u8; 32])
    }

    fn last_event_hash_or_checkpoint_head(&self) -> Result<[u8; 32]> {
        if let Some(event) = self.events.last() {
            return Ok(event.entry_hash);
        }
        self.last_chain_head()
    }
}

impl SealedLogStore for InMemorySealedLogStore {
    fn append_record(
        &mut self,
        record: &SealedLogRecord,
        _signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let created_at = i64::try_from(record.time_bucket().start_epoch_s)
            .map_err(|_| anyhow!("time bucket start exceeds i64 range"))?;
        let prev_hash = self.last_event_hash_or_checkpoint_head()?;
        let payload_json = serde_json::to_string(record)?;
        let entry_hash = hash_entry(&prev_hash, payload_json.as_bytes());
        self.next_id += 1;
        let id = self.next_id;
        self.events.push(InMemorySealedEventEntry {
            id,
            created_at,
            payload_json,
            entry_hash,
        });
        self.age_floor
            .note_append(id, Instant::now(), SystemTime::now());
        Ok(())
    }

    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        _signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;

        // Same two-clock prefix rule as the SQLite store above.
        let first_kept = self
            .events
            .iter()
            .find(|entry| entry.created_at >= cutoff)
            .map(|entry| entry.id);
        let mono_floor =
            self.age_floor
                .protected_from(retention, Instant::now(), SystemTime::now());
        let upper = prune_upper_bound(first_kept, mono_floor);
        let cutoff_index = match upper {
            Some(upper) => self.events.iter().rposition(|entry| entry.id <= upper),
            None => self.events.len().checked_sub(1),
        };
        let Some(cutoff_index) = cutoff_index else {
            return Ok(());
        };

        let head = self.events[cutoff_index].entry_hash;
        self.checkpoints.push(InMemoryCheckpointEntry {
            chain_head_hash: head,
        });

        self.events.drain(..=cutoff_index);
        Ok(())
    }

    fn read_events_ruleset_bound(
        &mut self,
        expected_ruleset_hash: [u8; 32],
        limit: usize,
        log_alarm: &mut dyn FnMut(&str, &str) -> Result<()>,
    ) -> Result<Vec<SealedLogRecord>> {
        // As in the SQLite store: the limit counts RETURNED records, so the
        // rotation skip runs before `take` — otherwise rotations would consume
        // result slots.
        let mut out = Vec::new();
        for entry in &self.events {
            if out.len() >= limit {
                break;
            }
            let record = SealedLogRecord::deserialize_compat(&entry.payload_json)?;

            // Key rotations carry no ruleset binding — exempt and excluded,
            // matching the SQLite store above.
            if matches!(record, SealedLogRecord::KeyRotation(_)) {
                continue;
            }

            if let Err(e) =
                ReprocessGuard::assert_same_ruleset(expected_ruleset_hash, record.ruleset_hash())
            {
                log_alarm("CONFORMANCE_REPROCESS_VIOLATION", &format!("{}", e))?;
                return Err(e);
            }

            out.push(record);
        }
        Ok(out)
    }
}

#[cfg(test)]
mod age_floor_tests {
    use super::*;

    const DAY: Duration = Duration::from_secs(86_400);

    fn floor_with_one_sample(base: Instant, wall: SystemTime) -> MonotonicAgeFloor {
        let mut floor = MonotonicAgeFloor::default();
        floor.note_append(1, base, wall);
        floor
    }

    #[test]
    fn agreeing_clocks_never_vouch() {
        // No step: the stamps are trustworthy, the floor stays out of the way
        // (this is what lets a test age rows by rewriting `created_at`).
        let base = Instant::now();
        let wall = SystemTime::now();
        let floor = floor_with_one_sample(base, wall);
        let later = Duration::from_secs(3_000);
        assert_eq!(floor.protected_from(DAY, base + later, wall + later), None);
    }

    #[test]
    fn a_forward_step_vouches_for_rows_after_the_sample() {
        let base = Instant::now();
        let wall = SystemTime::now();
        let floor = floor_with_one_sample(base, wall);
        let later = Duration::from_secs(3_000);
        // Wall clock jumped two hours further than the monotonic clock moved.
        let stepped = wall + later + Duration::from_secs(7_200);
        assert_eq!(floor.protected_from(DAY, base + later, stepped), Some(1));
    }

    #[test]
    fn a_step_inside_tolerance_is_slew_not_a_step() {
        let base = Instant::now();
        let wall = SystemTime::now();
        let floor = floor_with_one_sample(base, wall);
        let later = Duration::from_secs(3_000);
        let slewed = wall + later + AGE_FLOOR_STEP_TOLERANCE;
        assert_eq!(floor.protected_from(DAY, base + later, slewed), None);
        // ...and one second past the tolerance is a step: the retention pass
        // with the default 30 s skew tolerance would have sealed a ClockSkew
        // for it, and the floor must cover the same steps the hold does.
        let stepped = slewed + Duration::from_secs(1);
        assert_eq!(floor.protected_from(DAY, base + later, stepped), Some(1));
    }

    #[test]
    fn a_backward_step_never_vouches() {
        // Rows look younger than they are: the safe direction, and the prefix
        // rule's job, not the floor's.
        let base = Instant::now();
        let wall = SystemTime::now();
        let floor = floor_with_one_sample(base, wall);
        let later = Duration::from_secs(3_000);
        let back = wall - Duration::from_secs(7_200);
        assert_eq!(floor.protected_from(DAY, base + later, back), None);
    }

    #[test]
    fn a_sample_older_than_retention_cannot_vouch() {
        let base = Instant::now();
        let wall = SystemTime::now();
        let floor = floor_with_one_sample(base, wall);
        let later = 2 * DAY;
        let stepped = wall + later + Duration::from_secs(7_200);
        assert_eq!(floor.protected_from(DAY, base + later, stepped), None);
    }

    #[test]
    fn the_oldest_vouching_sample_wins_and_the_ring_is_bounded() {
        let base = Instant::now();
        let wall = SystemTime::now();
        let mut floor = MonotonicAgeFloor::default();
        let extra = 5;
        for i in 0..(AGE_FLOOR_SAMPLES + extra) {
            let at = AGE_FLOOR_SAMPLE_INTERVAL * i as u32;
            floor.note_append(i as i64 + 1, base + at, wall + at);
            // Appends inside the interval are not sampled.
            floor.note_append(-1, base + at + Duration::from_secs(1), wall + at);
        }
        assert_eq!(
            floor.samples.len(),
            AGE_FLOOR_SAMPLES,
            "FR-4: the ring is capped"
        );
        assert_eq!(
            floor.samples.front().map(|s| s.rowid),
            Some(extra as i64 + 1)
        );

        let now_mono = base + AGE_FLOOR_SAMPLE_INTERVAL * (AGE_FLOOR_SAMPLES + extra) as u32;
        let now_wall = wall
            + AGE_FLOOR_SAMPLE_INTERVAL * (AGE_FLOOR_SAMPLES + extra) as u32
            + Duration::from_secs(7_200);
        // Retention of three intervals, read one interval after the newest
        // sample: the sample three intervals back is exactly at the edge and
        // does not count, so the oldest sample strictly inside retention is
        // the one before the newest, and it vouches for everything after it.
        let retention = AGE_FLOOR_SAMPLE_INTERVAL * 3;
        let newest = (AGE_FLOOR_SAMPLES + extra) as i64;
        assert_eq!(
            floor.protected_from(retention, now_mono, now_wall),
            Some(newest - 1)
        );
    }

    #[test]
    fn prune_upper_bound_takes_the_tighter_clock() {
        assert_eq!(prune_upper_bound(None, None), None, "everything expired");
        assert_eq!(prune_upper_bound(Some(5), None), Some(4));
        assert_eq!(prune_upper_bound(None, Some(3)), Some(2));
        assert_eq!(prune_upper_bound(Some(5), Some(3)), Some(2));
        assert_eq!(
            prune_upper_bound(Some(1), Some(9)),
            Some(0),
            "nothing to prune"
        );
    }
}
