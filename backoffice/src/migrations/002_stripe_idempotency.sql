-- Stripe redelivers events; every one is applied once, by id. And an order
-- made through Checkout is found by its PaymentIntent too, which is how a
-- refund names it.
CREATE TABLE IF NOT EXISTS stripe_events (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  received_at TEXT NOT NULL
);
ALTER TABLE orders ADD COLUMN payment_intent TEXT;
CREATE INDEX IF NOT EXISTS orders_payment_intent ON orders(payment_intent);
