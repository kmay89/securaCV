-- Sign-in links: a mailed link that stands in for the password (never for the authenticator), good for
-- a few minutes, once, and only in the browser that asked for it (its nonce rides in a cookie; both are
-- stored hashed, so a copy of the table opens nothing).
CREATE TABLE IF NOT EXISTS login_links (
  id TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  nonce_hash TEXT NOT NULL,
  created_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  used_at TEXT,
  ip TEXT
);
-- The network a session was opened from (a /24 or /48), so SESSION_BIND_IP can end a session that moves.
ALTER TABLE sessions ADD COLUMN net TEXT;
