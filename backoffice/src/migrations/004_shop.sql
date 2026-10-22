-- v4: what a storefront admin shows that v1 did not — an order's timeline,
-- tags on orders and customers, and discount codes.
ALTER TABLE orders ADD COLUMN discount_code TEXT;
ALTER TABLE orders ADD COLUMN discount_cents INTEGER NOT NULL DEFAULT 0;
ALTER TABLE orders ADD COLUMN tags TEXT NOT NULL DEFAULT '';
ALTER TABLE contacts ADD COLUMN tags TEXT NOT NULL DEFAULT '';

-- every change to an order, in order: placed, packed, shipped, a note, a refund…
CREATE TABLE IF NOT EXISTS order_events (
  id INTEGER PRIMARY KEY,
  order_id INTEGER NOT NULL REFERENCES orders(id) ON DELETE CASCADE,
  at TEXT NOT NULL,
  actor TEXT NOT NULL,
  kind TEXT NOT NULL,                            -- placed | packed | shipped | delivered | canceled | unpacked | refund | note | tags | address | device | mail
  text TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS order_events_order ON order_events(order_id, id);

-- discount codes (Stripe applies them at checkout; this is the ledger of what they cost)
CREATE TABLE IF NOT EXISTS discounts (
  code TEXT PRIMARY KEY COLLATE NOCASE,
  kind TEXT NOT NULL,                            -- percent | fixed | free_shipping
  value INTEGER NOT NULL DEFAULT 0,              -- percent: 1–100; fixed: cents
  min_subtotal_cents INTEGER NOT NULL DEFAULT 0,
  starts_at TEXT, ends_at TEXT,
  max_uses INTEGER,
  status TEXT NOT NULL DEFAULT 'active',         -- active | disabled
  note TEXT,
  created_at TEXT NOT NULL
);
