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

-- Orders from before the timeline existed get the events their timestamps
-- prove, so a historical order is not blank and a customer's timeline shows
-- every purchase. Refunds are dated at the last known step (the ledger has
-- the exact date).
INSERT INTO order_events (order_id, at, actor, kind, text)
  SELECT id, COALESCE(paid_at, created_at), 'migration', 'placed', 'Order placed via ' || source || ' (before the timeline; from the order record)' FROM orders;
INSERT INTO order_events (order_id, at, actor, kind, text) SELECT id, packed_at, 'migration', 'packed', 'Packed' FROM orders WHERE packed_at IS NOT NULL;
INSERT INTO order_events (order_id, at, actor, kind, text) SELECT id, shipped_at, 'migration', 'shipped', 'Shipped via ' || COALESCE(carrier, 'USPS') || COALESCE(' · ' || tracking, '') FROM orders WHERE shipped_at IS NOT NULL;
INSERT INTO order_events (order_id, at, actor, kind, text) SELECT id, delivered_at, 'migration', 'delivered', 'Delivered' FROM orders WHERE delivered_at IS NOT NULL;
INSERT INTO order_events (order_id, at, actor, kind, text) SELECT id, COALESCE(delivered_at, shipped_at, packed_at, paid_at, created_at), 'migration', 'canceled', 'Canceled' FROM orders WHERE status = 'canceled';
INSERT INTO order_events (order_id, at, actor, kind, text) SELECT id, COALESCE(delivered_at, shipped_at, packed_at, paid_at, created_at), 'migration', 'refund', CASE WHEN refunded_cents >= total_cents THEN 'Full' ELSE 'Partial' END || ' refund of $' || printf('%.2f', refunded_cents / 100.0) || ' (before the timeline; the ledger has the date)' FROM orders WHERE refunded_cents > 0;
