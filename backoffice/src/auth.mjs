// One operator. A password (scrypt) plus a TOTP code (RFC 6238), a session
// cookie, a CSRF token per session, and bearer tokens for the API/CLI.
// No user table: a company of one does not need roles, and adding them
// later is a migration, not a redesign.
import { scryptSync, randomBytes, createHmac, timingSafeEqual } from 'node:crypto';
import { nowIso } from './db.mjs';
import { token, safeEqual } from './http.mjs';

export function hashPassword(pw, salt = randomBytes(16)) {
  const key = scryptSync(String(pw), salt, 32, { N: 16384, r: 8, p: 1 });
  return `scrypt$${salt.toString('base64')}$${key.toString('base64')}`;
}
export function verifyPassword(pw, stored) {
  if (!stored) return false;
  const [, salt, key] = stored.split('$');
  const got = scryptSync(String(pw), Buffer.from(salt, 'base64'), 32, { N: 16384, r: 8, p: 1 });
  const want = Buffer.from(key, 'base64');
  return got.length === want.length && timingSafeEqual(got, want);
}

// --- TOTP (SHA-1, 6 digits, 30 s), base32 secrets as every authenticator app expects.
const B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
export function base32Encode(buf) {
  let bits = 0, value = 0, out = '';
  for (const b of buf) { value = (value << 8) | b; bits += 8; while (bits >= 5) { out += B32[(value >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits > 0) out += B32[(value << (5 - bits)) & 31];
  return out;
}
export function base32Decode(s) {
  let bits = 0, value = 0; const out = [];
  for (const c of s.toUpperCase().replace(/=+$/, '')) { const i = B32.indexOf(c); if (i < 0) continue; value = (value << 5) | i; bits += 5; if (bits >= 8) { out.push((value >>> (bits - 8)) & 255); bits -= 8; } }
  return Buffer.from(out);
}
export function totp(secretB32, { time = Date.now(), step = 30, digits = 6 } = {}) {
  const counter = Math.floor(time / 1000 / step);
  const msg = Buffer.alloc(8); msg.writeBigUInt64BE(BigInt(counter));
  const h = createHmac('sha1', base32Decode(secretB32)).update(msg).digest();
  const off = h[h.length - 1] & 0xf;
  const code = ((h[off] & 0x7f) << 24 | h[off + 1] << 16 | h[off + 2] << 8 | h[off + 3]) % 10 ** digits;
  return String(code).padStart(digits, '0');
}
export function verifyTotp(secretB32, code, { time = Date.now(), window = 1 } = {}) {
  if (!/^\d{6}$/.test(String(code))) return false;
  for (let w = -window; w <= window; w++) if (safeEqual(totp(secretB32, { time: time + w * 30_000 }), String(code))) return true;
  return false;
}
export const totpUri = (secret, account, issuer) => `otpauth://totp/${encodeURIComponent(issuer)}:${encodeURIComponent(account)}?secret=${secret}&issuer=${encodeURIComponent(issuer)}&algorithm=SHA1&digits=6&period=30`;

// --- operator setup & login
export function setupOperator(db, password) {
  const secret = base32Encode(randomBytes(20));
  db.setSetting('auth:password', hashPassword(password));
  db.setSetting('auth:totp', secret);
  db.setSetting('auth:password_set_at', nowIso());
  db.audit('cli', 'auth.setup');
  return secret;
}
export const hasOperator = (db) => !!db.setting('auth:password');

export function login(db, password, code, { sessionHours = 12, ip = null } = {}) {
  const ok = verifyPassword(password, db.setting('auth:password'));
  const secret = db.setting('auth:totp');
  const codeOk = secret ? verifyTotp(secret, code) : true;
  if (!ok || !codeOk) { db.audit('web', 'auth.login.failed', 'session', null, { ip }); return null; }
  const id = token(32); const csrf = token(24);
  const exp = new Date(Date.now() + sessionHours * 3600_000).toISOString();
  db.run('INSERT INTO sessions (id, created_at, expires_at, csrf) VALUES (?, ?, ?, ?)', id, nowIso(), exp, csrf);
  db.run('DELETE FROM sessions WHERE expires_at < ?', nowIso());
  db.audit('web', 'auth.login', 'session', null, { ip });
  return { id, csrf, expires_at: exp };
}
export const sessionFor = (db, id) => id ? db.get('SELECT * FROM sessions WHERE id = ? AND expires_at > ?', id, nowIso()) : null;
export const logout = (db, id) => db.run('DELETE FROM sessions WHERE id = ?', id);

// --- API tokens: `bo_<id>.<secret>`. The id finds the row; the secret is
// verified against a scrypt hash (the same KDF as the password, a lighter
// cost: ~10 ms per API call). A copy of the table verifies nothing.
const TOKEN_SCRYPT = { N: 4096, r: 8, p: 1 };
const tokenHash = (secret, salt = randomBytes(16)) => `scrypt$${salt.toString('base64')}$${scryptSync(String(secret), salt, 32, TOKEN_SCRYPT).toString('base64')}`;
function tokenVerify(secret, stored) {
  const [, salt, key] = String(stored || '').split('$'); if (!salt || !key) return false;
  const got = scryptSync(String(secret), Buffer.from(salt, 'base64'), 32, TOKEN_SCRYPT); const want = Buffer.from(key, 'base64');
  return got.length === want.length && timingSafeEqual(got, want);
}
// scope 'ro' is read-only: GET only, for a dashboard or a monitor that must never change anything.
export function createApiToken(db, name, { scope = 'rw' } = {}) {
  if (!['rw', 'ro'].includes(scope)) throw new Error('scope is rw or ro');
  const id = token(9); const secret = token(32);
  db.run('INSERT INTO api_tokens (name, token_id, token_hash, scope, created_at) VALUES (?, ?, ?, ?, ?)', name, id, tokenHash(secret), scope, nowIso());
  db.audit('cli', 'auth.token.create', 'api_token', name, { scope });
  return `bo_${id}.${secret}`;
}
export function apiTokenFor(db, bearer) {
  const m = /^bo_([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)$/.exec(String(bearer || '')); if (!m) return null;
  const row = db.get('SELECT * FROM api_tokens WHERE token_id = ?', m[1]);
  if (!row || !tokenVerify(m[2], row.token_hash)) return null;
  db.run('UPDATE api_tokens SET last_used_at = ? WHERE id = ?', nowIso(), row.id);
  return row;
}
export const revokeApiToken = (db, name) => db.run('DELETE FROM api_tokens WHERE name = ?', name).changes;
