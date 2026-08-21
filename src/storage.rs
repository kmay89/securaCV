use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};
use std::collections::HashSet;
use std::time::Duration;

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

pub struct SqliteSealedLogStore {
    conn: Connection,
}

impl SqliteSealedLogStore {
    pub fn open(db_path: &str) -> Result<Self> {
        let conn = open_db_connection(db_path)?;
        let mut store = Self { conn };
        store.ensure_schema()?;
        Ok(store)
    }

    pub fn open_with_key(db_path: &str, encryption_key: Option<&str>) -> Result<Self> {
        let conn = open_db_connection_with_key(db_path, encryption_key)?;
        let mut store = Self { conn };
        store.ensure_schema()?;
        Ok(store)
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
            &[("pq_signature", "BLOB"), ("pq_scheme", "TEXT")],
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

impl SealedLogStore for SqliteSealedLogStore {
    fn append_record(
        &mut self,
        record: &SealedLogRecord,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let created_at = i64::try_from(record.time_bucket().start_epoch_s)
            .map_err(|_| anyhow!("time bucket start exceeds i64 range"))?;
        let payload_json = serde_json::to_string(record)?;

        self.in_immediate_write_tx(|store| {
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

            Ok(())
        })
    }

    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;

        // Checkpoint INSERT and event DELETE are one atomic unit: a crash
        // between them previously left a checkpoint with un-pruned events and
        // a duplicate checkpoint on the retention re-run after restart.
        self.in_immediate_write_tx(|store| {
        let mut stmt = store.conn.prepare(
            "SELECT id, entry_hash FROM sealed_events WHERE created_at < ?1 ORDER BY id DESC LIMIT 1",
        )?;
        let mut rows = stmt.query(params![cutoff])?;
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

        let checkpoint_sig = sign_entry(signature_keys, &head, DOMAIN_CHECKPOINT)?;
        let checkpoint_pq_signature = checkpoint_sig
            .pq_signature
            .as_ref()
            .map(|sig| sig.signature.clone());
        let checkpoint_pq_scheme = checkpoint_sig
            .pq_signature
            .as_ref()
            .map(|sig| sig.scheme_id.clone());
        let created_at = now_s()? as i64;

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
            let record = SealedLogRecord::deserialize_compat(&payload)?;

            // Key rotations are identity-administration records with no ruleset
            // binding (their ruleset_hash is all-zero), so they are exempt from
            // the reprocess guard and excluded from the read — otherwise a
            // single rotation would fail every subsequent read with a false
            // CONFORMANCE_REPROCESS_VIOLATION. Mirrors the export path's skip.
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

#[derive(Clone, Debug)]
struct InMemorySealedEventEntry {
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
        self.events.push(InMemorySealedEventEntry {
            created_at,
            payload_json,
            entry_hash,
        });
        Ok(())
    }

    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        _signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;

        let cutoff_index = self
            .events
            .iter()
            .rposition(|entry| entry.created_at < cutoff);
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
        let payloads = self
            .events
            .iter()
            .take(limit)
            .map(|entry| entry.payload_json.clone())
            .collect::<Vec<_>>();

        let mut out = Vec::with_capacity(payloads.len());
        for payload in payloads {
            let record = SealedLogRecord::deserialize_compat(&payload)?;

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
