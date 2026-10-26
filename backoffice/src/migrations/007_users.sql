-- v7: people. Each person has their own password and authenticator, a role,
-- and a name every action is written down under. The first owner is the
-- operator from before this migration (carried over by auth.mjs on boot).
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  role TEXT NOT NULL DEFAULT 'ops',               -- owner | ops | support | viewer
  status TEXT NOT NULL DEFAULT 'invited',         -- invited | active | disabled
  password_hash TEXT,
  totp_secret TEXT,
  totp_pending TEXT,                              -- a new secret, until a code from it is seen
  invite_token TEXT,
  invite_expires_at TEXT,
  password_set_at TEXT,
  last_login_at TEXT,
  failed_logins INTEGER NOT NULL DEFAULT 0,
  locked_until TEXT,
  invited_by TEXT,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS users_invite ON users(invite_token);
ALTER TABLE sessions ADD COLUMN user_id INTEGER;
