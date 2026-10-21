-- backoffice schema v1. One SQLite file is the whole company's operational
-- state. Money amounts are integer cents; timestamps are ISO-8601 UTC text.
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

CREATE TABLE IF NOT EXISTS sessions (
  id TEXT PRIMARY KEY,
  created_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  csrf TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS api_tokens (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL,
  last_used_at TEXT
);

CREATE TABLE IF NOT EXISTS audit_log (
  id INTEGER PRIMARY KEY,
  at TEXT NOT NULL,
  actor TEXT NOT NULL,
  action TEXT NOT NULL,
  entity TEXT,
  entity_id TEXT,
  detail TEXT
);

-- contacts & consent (replaces: a CRM, a newsletter tool's audience)
CREATE TABLE IF NOT EXISTS contacts (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  name TEXT,
  org TEXT,
  kind TEXT NOT NULL DEFAULT 'person',          -- person | school | business | nonprofit | press | vendor
  created_at TEXT NOT NULL,
  source TEXT,                                   -- store | form:contact | form:request | waitlist | import | support
  consent_marketing INTEGER NOT NULL DEFAULT 0,  -- 1 only after double opt-in
  consent_at TEXT,
  consent_token TEXT,                            -- pending double opt-in token
  unsubscribe_token TEXT NOT NULL,
  unsubscribed_at TEXT,
  notes TEXT
);

CREATE TABLE IF NOT EXISTS waitlist (
  id INTEGER PRIMARY KEY,
  contact_id INTEGER NOT NULL REFERENCES contacts(id),
  sku TEXT NOT NULL,
  qty INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL,
  notified_at TEXT,
  UNIQUE(contact_id, sku)
);

-- orders (replaces: a hosted storefront's order admin; Stripe stays the processor)
CREATE TABLE IF NOT EXISTS orders (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- EL-000123, human-facing
  external_id TEXT UNIQUE,                       -- Stripe checkout session / payment intent id
  source TEXT NOT NULL,                          -- stripe | invoice | manual | import
  contact_id INTEGER REFERENCES contacts(id),
  status TEXT NOT NULL DEFAULT 'paid',           -- pending | paid | packed | shipped | delivered | refunded | canceled
  currency TEXT NOT NULL DEFAULT 'usd',
  subtotal_cents INTEGER NOT NULL DEFAULT 0,
  shipping_cents INTEGER NOT NULL DEFAULT 0,
  tax_cents INTEGER NOT NULL DEFAULT 0,
  total_cents INTEGER NOT NULL DEFAULT 0,
  refunded_cents INTEGER NOT NULL DEFAULT 0,
  ship_name TEXT, ship_line1 TEXT, ship_line2 TEXT, ship_city TEXT, ship_state TEXT, ship_zip TEXT,
  ship_country TEXT NOT NULL DEFAULT 'US',
  created_at TEXT NOT NULL,
  paid_at TEXT, packed_at TEXT, shipped_at TEXT, delivered_at TEXT,
  carrier TEXT, tracking TEXT,
  batch TEXT,                                    -- the weekly batch label, e.g. 2026-W43
  notes TEXT
);

CREATE TABLE IF NOT EXISTS order_items (
  id INTEGER PRIMARY KEY,
  order_id INTEGER NOT NULL REFERENCES orders(id) ON DELETE CASCADE,
  sku TEXT NOT NULL,
  name TEXT NOT NULL,
  qty INTEGER NOT NULL DEFAULT 1,
  unit_cents INTEGER NOT NULL DEFAULT 0,
  cogs_cents INTEGER NOT NULL DEFAULT 0          -- per unit, at the time of sale
);

-- subscriptions: support plans / SLAs (LICENSING.md §2c), month-to-month
CREATE TABLE IF NOT EXISTS subscriptions (
  id INTEGER PRIMARY KEY,
  external_id TEXT UNIQUE,
  contact_id INTEGER REFERENCES contacts(id),
  plan TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'active',         -- active | past_due | canceled
  amount_cents INTEGER NOT NULL DEFAULT 0,
  interval TEXT NOT NULL DEFAULT 'month',
  current_period_end TEXT,
  created_at TEXT NOT NULL,
  canceled_at TEXT
);

-- invoices: for institutions that buy on a purchase order (replaces: invoicing SaaS)
CREATE TABLE IF NOT EXISTS invoices (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- INV-2026-0007
  contact_id INTEGER REFERENCES contacts(id),
  order_id INTEGER REFERENCES orders(id),
  po_number TEXT,
  status TEXT NOT NULL DEFAULT 'draft',          -- draft | sent | paid | void | overdue
  issued_at TEXT, due_at TEXT, paid_at TEXT,
  terms_days INTEGER NOT NULL DEFAULT 30,
  bill_to TEXT,                                  -- free-text block
  subtotal_cents INTEGER NOT NULL DEFAULT 0,
  shipping_cents INTEGER NOT NULL DEFAULT 0,
  tax_cents INTEGER NOT NULL DEFAULT 0,
  total_cents INTEGER NOT NULL DEFAULT 0,
  notes TEXT,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS invoice_items (
  id INTEGER PRIMARY KEY,
  invoice_id INTEGER NOT NULL REFERENCES invoices(id) ON DELETE CASCADE,
  sku TEXT, description TEXT NOT NULL,
  qty INTEGER NOT NULL DEFAULT 1,
  unit_cents INTEGER NOT NULL DEFAULT 0
);

-- inventory (replaces: an inventory app). kind: part | printed | finished | packaging
CREATE TABLE IF NOT EXISTS inventory_items (
  sku TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'part',
  on_hand INTEGER NOT NULL DEFAULT 0,
  reorder_point INTEGER NOT NULL DEFAULT 0,
  reorder_qty INTEGER NOT NULL DEFAULT 0,
  unit_cost_cents INTEGER NOT NULL DEFAULT 0,
  supplier TEXT, supplier_url TEXT, location TEXT,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS inventory_moves (
  id INTEGER PRIMARY KEY,
  sku TEXT NOT NULL REFERENCES inventory_items(sku),
  delta INTEGER NOT NULL,
  reason TEXT NOT NULL,                          -- receive | pack | adjust | warranty | scrap | build
  ref TEXT,
  at TEXT NOT NULL
);

-- what one finished SKU consumes when it is packed
CREATE TABLE IF NOT EXISTS recipes (
  finished_sku TEXT NOT NULL,
  part_sku TEXT NOT NULL REFERENCES inventory_items(sku),
  qty INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (finished_sku, part_sku)
);

-- devices: the serial registry (every pre-flashed board that leaves the bench)
CREATE TABLE IF NOT EXISTS devices (
  id INTEGER PRIMARY KEY,
  serial TEXT NOT NULL UNIQUE,
  mac TEXT UNIQUE,
  board TEXT, product_sku TEXT,
  fw_version TEXT, fw_sha256 TEXT,
  flashed_at TEXT,
  batch TEXT,
  order_id INTEGER REFERENCES orders(id),
  status TEXT NOT NULL DEFAULT 'stock',          -- stock | shipped | returned | scrapped
  notes TEXT
);

-- warranty claims: fix, not return (terms-of-sale §7)
CREATE TABLE IF NOT EXISTS warranty_claims (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- WC-000012
  order_id INTEGER REFERENCES orders(id),
  device_id INTEGER REFERENCES devices(id),
  contact_id INTEGER REFERENCES contacts(id),
  ticket_id INTEGER,
  category TEXT NOT NULL,                        -- one of warranty.mjs ANSWERS keys
  description TEXT,
  resolution TEXT NOT NULL,                      -- the defined answer, filled from category
  status TEXT NOT NULL DEFAULT 'open',           -- open | shipping | resolved | declined
  cost_cents INTEGER NOT NULL DEFAULT 0,
  reship_order_id INTEGER REFERENCES orders(id),
  created_at TEXT NOT NULL,
  resolved_at TEXT
);

-- support (replaces: a helpdesk)
CREATE TABLE IF NOT EXISTS tickets (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- T-000045
  contact_id INTEGER REFERENCES contacts(id),
  subject TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'open',           -- open | waiting_on_customer | waiting_on_us | closed
  priority TEXT NOT NULL DEFAULT 'normal',       -- low | normal | high | urgent
  channel TEXT NOT NULL DEFAULT 'email',         -- email | form | manual | api
  kind TEXT NOT NULL DEFAULT 'support',          -- support | sales | press | security | warranty | feedback | build_request
  labels TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  first_response_at TEXT,
  sla_due_at TEXT,
  closed_at TEXT,
  thread_token TEXT NOT NULL UNIQUE              -- rides in the subject: [T-000045 abcd12]
);

CREATE TABLE IF NOT EXISTS ticket_messages (
  id INTEGER PRIMARY KEY,
  ticket_id INTEGER NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,
  direction TEXT NOT NULL,                       -- in | out | note
  from_addr TEXT, to_addr TEXT,
  message_id TEXT UNIQUE,
  in_reply_to TEXT,
  body TEXT NOT NULL,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS canned_replies (
  slug TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  body TEXT NOT NULL
);

-- mail (replaces: transactional + newsletter senders). Outbox is the queue.
CREATE TABLE IF NOT EXISTS mail_outbox (
  id INTEGER PRIMARY KEY,
  to_addr TEXT NOT NULL,
  subject TEXT NOT NULL,
  body_text TEXT NOT NULL,
  headers TEXT NOT NULL DEFAULT '{}',            -- JSON extra headers (In-Reply-To, List-Unsubscribe…)
  kind TEXT NOT NULL DEFAULT 'transactional',    -- transactional | campaign | support
  ref TEXT,
  status TEXT NOT NULL DEFAULT 'queued',         -- queued | sent | failed | suppressed
  attempts INTEGER NOT NULL DEFAULT 0,
  last_error TEXT,
  message_id TEXT UNIQUE,
  created_at TEXT NOT NULL,
  sent_at TEXT
);

CREATE TABLE IF NOT EXISTS mail_seen (
  uid TEXT PRIMARY KEY,
  seen_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS campaigns (
  id INTEGER PRIMARY KEY,
  slug TEXT NOT NULL UNIQUE,
  subject TEXT NOT NULL,
  body_text TEXT NOT NULL,
  segment TEXT NOT NULL DEFAULT 'all',           -- all | waitlist:<sku> | kind:<kind>
  status TEXT NOT NULL DEFAULT 'draft',          -- draft | queued | sent
  created_at TEXT NOT NULL,
  sent_at TEXT,
  recipients INTEGER NOT NULL DEFAULT 0
);

-- status (replaces: a status page + an uptime monitor)
CREATE TABLE IF NOT EXISTS probes (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE,
  url TEXT NOT NULL,
  expect_status INTEGER NOT NULL DEFAULT 200,
  expect_text TEXT,
  interval_s INTEGER NOT NULL DEFAULT 300,
  enabled INTEGER NOT NULL DEFAULT 1,
  public INTEGER NOT NULL DEFAULT 1,
  last_ok INTEGER, last_ms INTEGER, last_at TEXT, last_detail TEXT,
  fails INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS probe_results (
  id INTEGER PRIMARY KEY,
  probe_id INTEGER NOT NULL REFERENCES probes(id) ON DELETE CASCADE,
  at TEXT NOT NULL,
  ok INTEGER NOT NULL,
  ms INTEGER NOT NULL,
  detail TEXT
);
CREATE INDEX IF NOT EXISTS probe_results_at ON probe_results(probe_id, at);

CREATE TABLE IF NOT EXISTS incidents (
  id INTEGER PRIMARY KEY,
  title TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'investigating',  -- investigating | identified | monitoring | resolved
  body TEXT,
  started_at TEXT NOT NULL,
  resolved_at TEXT,
  updated_at TEXT NOT NULL
);

-- books (replaces: bookkeeping software). Double-entry, cents, one row per leg.
CREATE TABLE IF NOT EXISTS ledger (
  id INTEGER PRIMARY KEY,
  txn TEXT NOT NULL,                             -- groups the legs of one transaction
  date TEXT NOT NULL,                            -- YYYY-MM-DD
  account TEXT NOT NULL,                         -- see ledger.mjs ACCOUNTS
  debit_cents INTEGER NOT NULL DEFAULT 0,
  credit_cents INTEGER NOT NULL DEFAULT 0,
  memo TEXT,
  ref TEXT,                                      -- order:EL-000123 | payout:po_… | expense:…
  source TEXT NOT NULL DEFAULT 'manual',
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS ledger_date ON ledger(date);
CREATE INDEX IF NOT EXISTS ledger_ref ON ledger(ref);

-- exceptions: the issues the systems open for a human (doc 20 principle 4)
CREATE TABLE IF NOT EXISTS exceptions (
  key TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  body TEXT NOT NULL,
  github_issue INTEGER,
  opened_at TEXT NOT NULL,
  resolved_at TEXT,
  last_seen_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS job_runs (
  name TEXT PRIMARY KEY,
  last_run_at TEXT NOT NULL,
  last_ok INTEGER NOT NULL,
  last_detail TEXT
);
