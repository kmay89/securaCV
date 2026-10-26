// People. Each person has a password (scrypt) and an authenticator (TOTP,
// RFC 6238), a role that says what they may do, a session cookie with a
// CSRF token, and a name every action is written down under. Invitations
// are links; nobody ever receives a password from someone else. Bearer
// tokens for the API/CLI are separate and owner-made.
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
// Returns the 30-second step the code matched (a number, truthy) or false. A caller that remembers
// the last accepted step passes it, and a code at or before it is refused: a code seen over someone's
// shoulder is good for one login, not two.
export function verifyTotp(secretB32, code, { time = Date.now(), window = 1, lastStep = null } = {}) {
  if (!/^\d{6}$/.test(String(code))) return false;
  for (let w = -window; w <= window; w++) {
    const t = time + w * 30_000; const step = Math.floor(t / 30_000);
    if (safeEqual(totp(secretB32, { time: t }), String(code))) return lastStep != null && step <= lastStep ? false : step;
  }
  return false;
}
export const totpUri = (secret, account, issuer) => `otpauth://totp/${encodeURIComponent(issuer)}:${encodeURIComponent(account)}?secret=${secret}&issuer=${encodeURIComponent(issuer)}&algorithm=SHA1&digits=6&period=30`;

// --- roles: what each may do. Checked by the pipeline on every request, by path and method.
export const ROLES = ['owner', 'ops', 'support', 'viewer'];
export const ROLE_TEXT = {
  owner: 'everything — including people, API tokens, settings, the books, and forgetting a customer',
  ops: 'runs the shop: orders and the benches, inventory and purchasing, devices, warranty, invoices, support, mail, health and exceptions — not people, tokens, settings or the books',
  support: 'answers people: tickets, canned replies, customers (tags, notes, opt-in mail), warranty claims, notes and tags on orders, the waitlist mail; reads the rest',
  viewer: 'reads everything a role can look at, changes nothing',
};
// Id segments match any one segment, never only digits: the handler coerces whatever the
// router decoded (`%31`, `+1`), so the rule must refuse by position, not by spelling.
const OWNER_ONLY = /^\/(settings|users|books|books\.csv|api\/books|api\/jobs|api\/exceptions|settings\.json)(\/|$|\?)|^\/contacts\/[^/]+\/(forget|export\.json)(\/|$)|^\/api\/customers\/[^/]+\/(forget|export)(\/|$)/;
const SUPPORT_WRITES = /^\/(tickets|canned|warranty|waitlist|contacts\/(new|[^/]+\/(tags|notes|consent)))(\/|$)|^\/orders\/[^/]+\/(note|tags)\/?$|^\/api\/(tickets|mail\/ingest|customers\/[^/]+\/tags|orders\/[^/]+\/(note|tags))(\/|$)/;
// The path is checked decoded (what the handler will see), and a path that does not decode is refused.
export function can(role, method, rawPath) {
  let path; try { path = decodeURIComponent(String(rawPath)); } catch { return false; }
  if (role === 'owner') return true;
  if (!ROLES.includes(role)) return false;
  if (OWNER_ONLY.test(path)) return false;
  if (/^\/(me|logout)(\/|$)/.test(path)) return true;
  if (role === 'ops') return true;
  if (method === 'GET' || method === 'HEAD') return true;
  return role === 'support' && SUPPORT_WRITES.test(path);
}

// --- people
const normEmail = (s) => String(s || '').trim().toLowerCase();
const PUBLIC = 'id, email, name, role, status, password_set_at, last_login_at, failed_logins, locked_until, invited_by, created_at, invite_expires_at';
export const listUsers = (db) => db.all(`SELECT ${PUBLIC}, invite_token IS NOT NULL AS has_invite FROM users ORDER BY CASE role WHEN 'owner' THEN 0 WHEN 'ops' THEN 1 WHEN 'support' THEN 2 ELSE 3 END, name`);
export const userById = (db, id) => db.get(`SELECT ${PUBLIC} FROM users WHERE id = ?`, id);
export const userByEmail = (db, email) => db.get(`SELECT ${PUBLIC} FROM users WHERE email = ?`, normEmail(email));
const secretRow = (db, id) => db.get('SELECT * FROM users WHERE id = ?', id);
const activeOwners = (db) => db.get("SELECT COUNT(*) AS n FROM users WHERE role = 'owner' AND status = 'active'").n;

// The operator from before people existed becomes the first owner, once, on boot.
export function carryLegacyOperator(db, { email = 'owner@localhost', name = 'Operator' } = {}) {
  if (!db.setting('auth:password') || db.get('SELECT COUNT(*) AS n FROM users').n) return null;
  db.run('INSERT INTO users (email, name, role, status, password_hash, totp_secret, password_set_at, created_at) VALUES (?, ?, \'owner\', \'active\', ?, ?, ?, ?)', normEmail(email), name, db.setting('auth:password'), db.setting('auth:totp'), db.setting('auth:password_set_at') || nowIso(), nowIso());
  db.audit('system', 'user.carry', 'user', normEmail(email), { role: 'owner' });
  return userByEmail(db, email);
}
// First run (and `backoffice setup --force`): the owner, with a fresh authenticator. Returns the TOTP secret for the QR.
export function setupOperator(db, password, { email = null, name = 'Operator', actor = 'cli' } = {}) {
  if (String(password).length < 12) throw new Error('use at least 12 characters');
  const secret = base32Encode(randomBytes(20)); const em = normEmail(email || 'owner@localhost'); const now = nowIso();
  db.run(`INSERT INTO users (email, name, role, status, password_hash, totp_secret, password_set_at, created_at) VALUES (?, ?, 'owner', 'active', ?, ?, ?, ?)
    ON CONFLICT(email) DO UPDATE SET name = excluded.name, role = 'owner', status = 'active', password_hash = excluded.password_hash, totp_secret = excluded.totp_secret, totp_pending = NULL, password_set_at = excluded.password_set_at, invite_token = NULL, failed_logins = 0, locked_until = NULL`, em, name, hashPassword(password), secret, now, now);
  db.setSetting('auth:password', 'users'); db.setSetting('auth:password_set_at', now);
  db.audit(actor, 'auth.setup', 'user', em);
  return secret;
}
export const hasOperator = (db) => db.get("SELECT COUNT(*) AS n FROM users WHERE role = 'owner' AND status = 'active' AND password_hash IS NOT NULL").n > 0;

// An invitation is a link that works for 48 hours and once. The person sets their own password and authenticator.
export function inviteUser(db, { email, name, role = 'ops', actor = 'web', hours = 48 }) {
  const em = normEmail(email); if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(em)) throw new Error('a valid email is required'); if (!name?.trim()) throw new Error('a name is required'); if (!ROLES.includes(role)) throw new Error(`role is one of ${ROLES.join(', ')}`);
  const existing = userByEmail(db, em); if (existing && existing.status === 'active') throw new Error(`${em} is already a person here`);
  const tok = token(24); const exp = new Date(Date.now() + hours * 3600000).toISOString();
  if (existing) db.run('UPDATE users SET name = ?, role = ?, status = \'invited\', invite_token = ?, invite_expires_at = ?, invited_by = ?, password_hash = NULL, totp_secret = NULL, totp_pending = NULL WHERE id = ?', name.trim(), role, tok, exp, actor, existing.id);
  else db.run('INSERT INTO users (email, name, role, status, invite_token, invite_expires_at, invited_by, created_at) VALUES (?, ?, ?, \'invited\', ?, ?, ?, ?)', em, name.trim(), role, tok, exp, actor, nowIso());
  db.audit(actor, existing ? 'user.reinvite' : 'user.invite', 'user', em, { role, expires_at: exp });
  return { ...userByEmail(db, em), invite_token: tok };
}
export const inviteFor = (db, tok) => { const u = db.get(`SELECT ${PUBLIC} FROM users WHERE invite_token = ? AND status = 'invited'`, String(tok || '')); return u && u.invite_expires_at > nowIso() ? u : null; };
// Step one of accepting: the person's own password. Step two (confirmTotp) proves the authenticator before the account opens.
export function acceptInvite(db, tok, password) {
  const u = inviteFor(db, tok); if (!u) throw new Error('this invitation is not valid any more — ask for a new one'); if (String(password).length < 12) throw new Error('use at least 12 characters');
  const secret = base32Encode(randomBytes(20));
  db.run('UPDATE users SET password_hash = ?, totp_pending = ?, password_set_at = ? WHERE id = ?', hashPassword(password), secret, nowIso(), u.id);
  db.audit(u.email, 'user.accept', 'user', u.email); return { ...u, totp_pending: secret };
}
// A code from the pending secret makes it the real one; for an invitee that also opens the account and burns the link.
export function confirmTotp(db, userId, code) {
  const r = secretRow(db, userId); if (!r?.totp_pending) throw new Error('nothing to confirm');
  if (!verifyTotp(r.totp_pending, code)) return false;
  db.run('UPDATE users SET totp_secret = totp_pending, totp_pending = NULL, status = CASE WHEN status = \'invited\' THEN \'active\' ELSE status END, invite_token = NULL, invite_expires_at = NULL WHERE id = ?', userId);
  db.audit(r.email, r.status === 'invited' ? 'user.activate' : 'auth.totp', 'user', r.email); return true;
}
export const pendingTotp = (db, userId) => secretRow(db, userId)?.totp_pending || null;
export function beginTotpRotation(db, userId) { const secret = base32Encode(randomBytes(20)); db.run('UPDATE users SET totp_pending = ? WHERE id = ?', secret, userId); return secret; }
export function changePassword(db, userId, current, next) {
  const r = secretRow(db, userId); if (!r) throw new Error('no such person'); if (!verifyPassword(current, r.password_hash)) throw new Error('the current password is wrong');
  if (String(next).length < 12) throw new Error('use at least 12 characters'); if (next === current) throw new Error('that is the same password');
  db.run('UPDATE users SET password_hash = ?, password_set_at = ? WHERE id = ?', hashPassword(next), nowIso(), userId); db.audit(r.email, 'auth.password', 'user', r.email); return true;
}
export function setRole(db, userId, role, { actor = 'web' } = {}) {
  const u = userById(db, userId); if (!u) throw new Error('no such person'); if (!ROLES.includes(role)) throw new Error('unknown role');
  if (u.role === 'owner' && role !== 'owner' && u.status === 'active' && activeOwners(db) <= 1) throw new Error('that is the last owner — make someone else an owner first');
  db.run('UPDATE users SET role = ? WHERE id = ?', role, userId); db.audit(actor, 'user.role', 'user', u.email, { from: u.role, to: role }); return userById(db, userId);
}
export function setStatus(db, userId, status, { actor = 'web' } = {}) {
  const u = userById(db, userId); if (!u) throw new Error('no such person'); if (!['active', 'disabled'].includes(status)) throw new Error('status is active or disabled');
  if (status === 'disabled' && u.role === 'owner' && u.status === 'active' && activeOwners(db) <= 1) throw new Error('that is the last owner');
  if (status === 'active' && u.status === 'invited') throw new Error('they have not accepted the invitation yet');
  db.run('UPDATE users SET status = ?, failed_logins = 0, locked_until = NULL WHERE id = ?', status, userId); if (status === 'disabled') db.run('DELETE FROM sessions WHERE user_id = ?', userId);
  db.audit(actor, status === 'disabled' ? 'user.disable' : 'user.enable', 'user', u.email); return userById(db, userId);
}
export function removeUser(db, userId, { actor = 'web' } = {}) {
  const u = userById(db, userId); if (!u) throw new Error('no such person');
  if (u.role === 'owner' && u.status === 'active' && activeOwners(db) <= 1) throw new Error('that is the last owner');
  db.run('DELETE FROM sessions WHERE user_id = ?', userId); db.run('DELETE FROM users WHERE id = ?', userId);
  db.audit(actor, 'user.remove', 'user', u.email, { role: u.role }); return u;
}

// --- login: email (optional while there is one person), password, code.
// Eight wrong tries from one address lock that person for fifteen minutes — for that address. The
// lock lives in memory keyed by (person, address): a stranger guessing from anywhere cannot keep the
// owner out of their own backoffice (the old database-wide lock re-armed on every wrong try, forever),
// and the per-address limiter in the pipeline still slows the guesser down. The counter and the
// lock time in the users table stay as a record (the people page shows them); a restart clears
// the locks themselves, and the key carries the email so a fresh database's first person does not
// inherit a lock armed against a previous one. A password check is run even for an unknown email, so the answer
// takes the same time either way and the list of emails cannot be read off the clock.
const LOCKS = new Map();
const DUMMY_HASH = hashPassword('the same work for a name nobody has');
const lockKey = (u, ip) => `${u.id}:${u.email}|${ip || '-'}`;
export function lockState(user, ip, now = Date.now()) { const k = lockKey(user, ip); const l = LOCKS.get(k); if (l && l.until && l.until <= now) { LOCKS.delete(k); return null; } return l || null; }
export const clearLocks = () => LOCKS.clear();
export function login(db, { email = '', password, code }, { sessionHours = 12, ip = null, now = Date.now() } = {}) {
  const em = normEmail(email);
  const r = em ? db.get('SELECT * FROM users WHERE email = ?', em) : (() => { const rows = db.all("SELECT * FROM users WHERE status = 'active' AND password_hash IS NOT NULL LIMIT 2"); return rows.length === 1 ? rows[0] : null; })();
  const fail = (why) => {
    if (r) {
      db.run('UPDATE users SET failed_logins = failed_logins + 1 WHERE id = ?', r.id);
      const l = lockState(r, ip, now) || { n: 0, until: null }; l.n++; if (l.n >= 8) { l.until = now + 15 * 60_000; db.run('UPDATE users SET locked_until = ? WHERE id = ?', new Date(l.until).toISOString(), r.id); } LOCKS.set(lockKey(r, ip), l);
      if (LOCKS.size > 10_000) for (const [kk, v] of LOCKS) if (!v.until || v.until <= now) LOCKS.delete(kk);
    }
    db.audit('web', 'auth.login.failed', 'user', r?.email || em || null, { ip, why }); return null;
  };
  if (!r || r.status !== 'active' || !r.password_hash || !r.totp_secret) { verifyPassword(String(password || ''), DUMMY_HASH); return fail(!r ? (em ? 'unknown' : 'email required') : r.status); }
  if (lockState(r, ip, now)?.until) { db.audit('web', 'auth.login.locked', 'user', r.email, { ip }); return null; }
  if (!verifyPassword(password, r.password_hash)) return fail('password');
  const step = verifyTotp(r.totp_secret, code, { time: now, lastStep: r.totp_last_step ?? null });
  if (!step) return fail('code');
  const id = token(32); const csrf = token(24); const exp = new Date(now + sessionHours * 3600_000).toISOString();
  db.run('INSERT INTO sessions (id, created_at, expires_at, csrf, user_id) VALUES (?, ?, ?, ?, ?)', id, nowIso(), exp, csrf, r.id);
  LOCKS.delete(lockKey(r, ip));
  db.run('DELETE FROM sessions WHERE expires_at < ?', nowIso()); db.run('UPDATE users SET last_login_at = ?, failed_logins = 0, locked_until = NULL, totp_last_step = ? WHERE id = ?', nowIso(), step, r.id);
  db.audit(r.email, 'auth.login', 'user', r.email, { ip });
  return { id, csrf, expires_at: exp, user_id: r.id, email: r.email, name: r.name, role: r.role };
}
export const sessionFor = (db, id) => id ? db.get("SELECT s.*, u.email, u.name, u.role FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.id = ? AND s.expires_at > ? AND u.status = 'active'", id, nowIso()) : null;
export const logout = (db, id) => db.run('DELETE FROM sessions WHERE id = ?', id);
export const sessionsFor = (db, userId) => db.all('SELECT id, created_at, expires_at FROM sessions WHERE user_id = ? ORDER BY created_at DESC', userId);
export const endOtherSessions = (db, userId, keep) => db.run('DELETE FROM sessions WHERE user_id = ? AND id != ?', userId, keep).changes;

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
