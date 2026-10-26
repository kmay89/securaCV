-- v6: the loop closes — carrier tracking on orders, purchase orders feeding
-- "buildable", recalls by batch or firmware, attachments on tickets, and the
-- right to be forgotten.
ALTER TABLE orders ADD COLUMN tracking_status TEXT;          -- label | in_transit | out_for_delivery | delivered | alert
ALTER TABLE orders ADD COLUMN tracking_detail TEXT;
ALTER TABLE orders ADD COLUMN tracking_checked_at TEXT;
ALTER TABLE orders ADD COLUMN expected_delivery_at TEXT;     -- YYYY-MM-DD from the carrier
ALTER TABLE contacts ADD COLUMN forgotten_at TEXT;

CREATE TABLE IF NOT EXISTS attachments (
  id INTEGER PRIMARY KEY,
  ticket_message_id INTEGER NOT NULL REFERENCES ticket_messages(id) ON DELETE CASCADE,
  filename TEXT NOT NULL,
  mime TEXT NOT NULL,
  size INTEGER NOT NULL,
  bytes BLOB NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS attachments_message ON attachments(ticket_message_id);

CREATE TABLE IF NOT EXISTS purchase_orders (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- PO-000001
  supplier TEXT NOT NULL,
  supplier_url TEXT,
  status TEXT NOT NULL DEFAULT 'draft',          -- draft | ordered | received | canceled
  ordered_at TEXT, expected_at TEXT, received_at TEXT,
  notes TEXT,
  created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS purchase_order_items (
  id INTEGER PRIMARY KEY,
  po_id INTEGER NOT NULL REFERENCES purchase_orders(id) ON DELETE CASCADE,
  sku TEXT NOT NULL REFERENCES inventory_items(sku),
  qty INTEGER NOT NULL,
  unit_cost_cents INTEGER NOT NULL DEFAULT 0,
  received_qty INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS recalls (
  id INTEGER PRIMARY KEY,
  number TEXT NOT NULL UNIQUE,                   -- RC-000001
  title TEXT NOT NULL,
  body TEXT NOT NULL,
  filter TEXT NOT NULL,                          -- JSON: { batch, fw, sku }
  devices INTEGER NOT NULL DEFAULT 0,
  customers INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  sent_at TEXT
);
