-- A TOTP code is good once: the step it was accepted at is remembered and anything at or before it is refused.
ALTER TABLE users ADD COLUMN totp_last_step INTEGER;
-- Mail retries back off (30 s, 2 min, 8 min … up to 6 h) instead of five tries in two and a half minutes.
ALTER TABLE mail_outbox ADD COLUMN next_attempt_at TEXT;
