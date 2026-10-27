-- v5: read-only API tokens, bounce suppression, scheduled campaigns with a
-- preheader, and automations (the mail that goes out because something
-- happened, with a record of every send so nothing goes twice).
ALTER TABLE api_tokens ADD COLUMN scope TEXT NOT NULL DEFAULT 'rw';   -- rw | ro
ALTER TABLE contacts ADD COLUMN bounced_at TEXT;
ALTER TABLE campaigns ADD COLUMN scheduled_at TEXT;
ALTER TABLE campaigns ADD COLUMN preheader TEXT;
ALTER TABLE mail_outbox ADD COLUMN contact_id INTEGER;

CREATE TABLE IF NOT EXISTS automations (
  slug TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  trigger TEXT NOT NULL,                         -- consent_confirmed | order_delivered | waitlist_joined | idle_days
  trigger_arg INTEGER NOT NULL DEFAULT 0,        -- idle_days: the days
  delay_hours INTEGER NOT NULL DEFAULT 0,
  subject TEXT NOT NULL,
  body_text TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS automation_runs (
  id INTEGER PRIMARY KEY,
  automation_slug TEXT NOT NULL REFERENCES automations(slug) ON DELETE CASCADE,
  contact_id INTEGER NOT NULL REFERENCES contacts(id),
  ref TEXT NOT NULL,                             -- what fired it: consent:<at> | order:<number> | waitlist:<sku> | idle
  due_at TEXT NOT NULL,
  sent_at TEXT,
  skipped TEXT,                                  -- why it did not go: no consent | bounced | disabled
  UNIQUE(automation_slug, contact_id, ref)
);
CREATE INDEX IF NOT EXISTS automation_runs_due ON automation_runs(sent_at, due_at);
