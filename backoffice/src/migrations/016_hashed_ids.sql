-- Session ids and invitation tokens are stored as their SHA-256 from this migration on (auth.mjs): a copy of the table
-- opens no session and no invitation. The rows from before carry the clear value, which nothing matches any
-- more, so they are ended here rather than left to look open: every session is closed (everyone signs in
-- again, password and authenticator), and every live invitation is expired (the owner presses "new link" on
-- the People page; the person's row and role stay). Both statements change nothing on a second run.
DELETE FROM sessions;
UPDATE users SET invite_token = NULL, invite_expires_at = NULL WHERE invite_token IS NOT NULL;
