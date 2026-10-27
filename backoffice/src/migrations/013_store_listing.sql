-- The store listing, edited here: what the admin changes about a product without a commit (listing.mjs).
-- store.json stays the base; a row here is the difference the store serves until the file catches up.
CREATE TABLE IF NOT EXISTS product_overrides (
  sku TEXT PRIMARY KEY,
  name TEXT,                                 -- NULL = the file's
  price_cents INTEGER,                       -- NULL = the file's
  stock INTEGER,                             -- the batch cap; NULL = the file's
  hidden INTEGER NOT NULL DEFAULT 0,         -- 1 = not shown, not sellable
  available_from TEXT,                       -- ISO; before this the listing says "opens …" and sells nothing (a launch)
  available_until TEXT,                      -- ISO; after this it is no longer offered
  note TEXT,                                 -- why, for the person who folds it back into the file
  updated_at TEXT NOT NULL,
  updated_by TEXT
);
