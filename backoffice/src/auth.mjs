// One operator. A password (scrypt) plus a TOTP code (RFC 6238), a session
// cookie, a CSRF token per session, and bearer tokens for the API/CLI.
// No user table: a company of one does not need roles, and adding them
// later is a migration, not a redesign.
import { scryptSync, randomBytes, createHmac, createHash, timingSafeEqual } from 'node:crypto';
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
  db.audit('cli', 'auth.setup');
  return secret;
}
export const hasOperator = (db) => !!db.setting('auth:password');

export function login(db, password, code, { sessionHours = 12 } = {}) {
  const ok = verifyPassword(password, db.setting('auth:password'));
  const secret = db.setting('auth:totp');
  const codeOk = secret ? verifyTotp(secret, code) : true;
  if (!ok || !codeOk) { db.audit('web', 'auth.login.failed'); return null; }
  const id = token(32); const csrf = token(24);
  const exp = new Date(Date.now() + sessionHours * 3600_000).toISOString();
  db.run('INSERT INTO sessions (id, created_at, expires_at, csrf) VALUES (?, ?, ?, ?)', id, nowIso(), exp, csrf);
  db.run('DELETE FROM sessions WHERE expires_at < ?', nowIso());
  db.audit('web', 'auth.login');
  return { id, csrf, expires_at: exp };
}
export const sessionFor = (db, id) => id ? db.get('SELECT * FROM sessions WHERE id = ? AND expires_at > ?', id, nowIso()) : null;
export const logout = (db, id) => db.run('DELETE FROM sessions WHERE id = ?', id);

// --- API tokens
const sha = (s) => createHash('sha256').update(String(s)).digest('hex');
export function createApiToken(db, name) {
  const t = 'bo_' + token(32);
  db.run('INSERT INTO api_tokens (name, token_hash, created_at) VALUES (?, ?, ?)', name, sha(t), nowIso());
  db.audit('cli', 'auth.token.create', 'api_token', name);
  return t;
}
export function apiTokenFor(db, bearer) {
  if (!bearer) return null;
  const row = db.get('SELECT * FROM api_tokens WHERE token_hash = ?', sha(bearer));
  if (row) db.run('UPDATE api_tokens SET last_used_at = ? WHERE id = ?', nowIso(), row.id);
  return row;
}
export const revokeApiToken = (db, name) => db.run('DELETE FROM api_tokens WHERE name = ?', name).changes;
