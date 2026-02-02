use anyhow::{anyhow, Result};
use rusqlite::{params, Connection};
use std::time::Duration;

use crate::crypto::signatures::{SignatureKeys, DOMAIN_CHECKPOINT, DOMAIN_SEALED_LOG_ENTRY};
use crate::{hash_entry, now_s, open_db_connection, sign_entry, ReprocessGuard, SealedLogRecord};

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

    fn ensure_schema(&mut self) -> Result<()> {
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

            CREATE INDEX IF NOT EXISTS idx_events_created ON sealed_events(created_at);
            "#,
        )?;
        self.ensure_signature_columns("sealed_events")?;
        self.ensure_signature_columns("checkpoints")?;
        Ok(())
    }

    fn ensure_signature_columns(&mut self, table: &str) -> Result<()> {
        let mut stmt = self.conn.prepare(&format!("PRAGMA table_info({})", table))?;
        let mut rows = stmt.query([])?;
        let mut has_pq_signature = false;
        let mut has_pq_scheme = false;
        while let Some(row) = rows.next()? {
            let name: String = row.get(1)?;
            if name == "pq_signature" {
                has_pq_signature = true;
            } else if name == "pq_scheme" {
                has_pq_scheme = true;
            }
        }
        if !has_pq_signature {
            self.conn.execute(
                &format!("ALTER TABLE {} ADD COLUMN pq_signature BLOB", table),
                [],
            )?;
        }
        if !has_pq_scheme {
            self.conn.execute(
                &format!("ALTER TABLE {} ADD COLUMN pq_scheme TEXT", table),
                [],
            )?;
        }
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

impl SealedLogStore for SqliteSealedLogStore {
    fn append_record(
        &mut self,
        record: &SealedLogRecord,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let created_at = i64::try_from(record.time_bucket().start_epoch_s)
            .map_err(|_| anyhow!("time bucket start exceeds i64 range"))?;
        let prev_hash = self.last_event_hash_or_checkpoint_head()?;
        let payload_json = serde_json::to_string(record)?;

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

        self.conn.execute(
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
    }

    fn enforce_retention_with_checkpoint(
        &mut self,
        retention: Duration,
        signature_keys: &SignatureKeys<'_>,
    ) -> Result<()> {
        let now = now_s()? as i64;
        let cutoff = now - retention.as_secs() as i64;

        let mut stmt = self.conn.prepare(
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

        self.conn.execute(
            r#"
            INSERT INTO checkpoints(
                created_at,
                cutoff_event_id,
                chain_head_hash,
                signature,
                pq_signature,
                pq_scheme
            )
            VALUES (?1, ?2, ?3, ?4, ?5, ?6)
            "#,
            params![
                created_at,
                cutoff_id,
                head.to_vec(),
                checkpoint_sig.ed25519_signature,
                checkpoint_pq_signature,
                checkpoint_pq_scheme
            ],
        )?;

        self.conn.execute(
            "DELETE FROM sealed_events WHERE id <= ?1",
            params![cutoff_id],
        )?;

        Ok(())
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
            let record: SealedLogRecord = serde_json::from_str(&payload)?;

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
            let record: SealedLogRecord = serde_json::from_str(&payload)?;

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
