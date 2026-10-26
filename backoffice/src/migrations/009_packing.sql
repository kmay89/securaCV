-- v9: the box. Packing an order freezes its plan (which boxes, expected
-- weight, estimated postage — from the website's shipping.json through the
-- box-fit method); shipping it may record what the scale read and what the
-- label cost, so a wrong count is caught before the tape and the books see
-- postage next to the shipping that was charged.
ALTER TABLE orders ADD COLUMN pack_plan TEXT;                       -- JSON, frozen when packed
ALTER TABLE orders ADD COLUMN measured_oz REAL;                     -- the scale, at the ship bench
ALTER TABLE orders ADD COLUMN postage_cents INTEGER NOT NULL DEFAULT 0;   -- what the label cost
