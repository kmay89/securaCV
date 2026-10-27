-- The label bought from the ship bench: where its PDF lives (Shippo's URL, or /orders/:id/label for the demo's own).
ALTER TABLE orders ADD COLUMN label_url TEXT;
-- Every hosted Checkout Session the store opened, so the funnel's last step is a number (opened → paid), and an
-- abandoned one can be followed up once with Stripe's recovery link. Rows carry what Stripe already told us.
CREATE TABLE IF NOT EXISTS checkout_sessions (
  id TEXT PRIMARY KEY,                       -- cs_…
  created_at TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'open',       -- open | paid | expired | recovered
  amount_cents INTEGER NOT NULL DEFAULT 0,
  lines TEXT NOT NULL DEFAULT '',            -- "1x vision-kit, 2x wap-parts"
  state TEXT,                                -- the ZIP's state, for the estimate shown
  email TEXT,                                -- only once Stripe reports it (the shopper typed it on Stripe's page)
  expired_at TEXT,
  recovery_url TEXT,                         -- Stripe's link that reopens the same cart
  recovery_expires_at TEXT,
  recovery_mailed_at TEXT,                   -- the one follow-up, once
  order_number TEXT,                         -- the order this session became
  paid_at TEXT,
  recovered_from TEXT                        -- the expired session this one reopened
);
CREATE INDEX IF NOT EXISTS idx_checkout_sessions_status ON checkout_sessions(status, created_at);
