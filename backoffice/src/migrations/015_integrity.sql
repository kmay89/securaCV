-- The audit log becomes a chain: every row carries the previous row's mark and its own, an HMAC under a key
-- that lives outside the database (audit.key beside it, or BACKOFFICE_AUDIT_KEY). A row changed, removed or
-- slipped in afterwards breaks the chain; `backoffice verify` walks it. Rows from before this migration have no
-- mark and are reported as "before the chain began".
ALTER TABLE audit_log ADD COLUMN prev TEXT;
ALTER TABLE audit_log ADD COLUMN mac TEXT;
-- The watch's blocks survive a restart: a scanner blocked at 03:00 is still blocked after the 03:01 deploy.
CREATE TABLE IF NOT EXISTS guard_blocks (
  ip TEXT PRIMARY KEY,
  until TEXT NOT NULL,
  reason TEXT NOT NULL,
  at TEXT NOT NULL
);
-- The security summary and the digest read the log by time; without this every page's doctor scanned the table.
CREATE INDEX IF NOT EXISTS idx_audit_at ON audit_log (at);
