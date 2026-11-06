// People. Each person has a password (scrypt) and an authenticator (TOTP,
// RFC 6238), a role that says what they may do, a session cookie with a
// CSRF token, and a name every action is written down under. Invitations
// are links; nobody ever receives a password from someone else. Bearer
// tokens for the API/CLI are separate and owner-made.
import { scrypt, scryptSync, randomBytes, createHmac, createHash, timingSafeEqual } from 'node:crypto';
import { netOf } from './guard.mjs';
import { nowIso } from './db.mjs';
import { token, safeEqual } from './http.mjs';

// --- the KDF: scrypt, run off the event loop. `crypto.scrypt` hands the work to libuv's
// threadpool, so a burst of logins or bearer calls never stalls the Stripe webhook, the
// store, /healthz (the container's health check) or the admin: the request pipeline awaits
// each verification. A lane caps how many derivations run at once (the rest wait in order,
// the loop free meanwhile): one password at a time, since each holds 128 MiB while it runs
// and the demo lives on a 512 MB box; two token checks (4 MiB each), so two of the pool's
// four threads stay free for DNS and the file system. The sync forms exist for the CLI
// (`backoffice setup`) and the demo seed, which run before the server listens — never for
// a request.
// Costs: passwords N=2^17, r=8, p=1 (OWASP's floor for scrypt); API tokens N=2^12 (the same
// KDF, a lighter cost: a token is 256 random bits, not a phrase anyone chose). A hash says its
// own cost (`$scrypt$ln=17,r=8,p=1$<salt>$<key>`, the PHC string format), so the cost can rise
// again without a migration; a hash from before 0.9.0 (`scrypt$<salt>$<key>`, N=2^14) still
// verifies and is re-hashed at the current cost on the next successful login. The Health page
// counts the ones still waiting.
export const PASSWORD_SCRYPT = { N: 2 ** 17, r: 8, p: 1 };
const LEGACY_PASSWORD_SCRYPT = { N: 2 ** 14, r: 8, p: 1 };
const TOKEN_SCRYPT = { N: 2 ** 12, r: 8, p: 1 };
const lane = (width) => { let running = 0; const waiting = []; const next = () => { running--; if (waiting.length) waiting.shift()(); }; return (job) => new Promise((resolve, reject) => { const run = () => { running++; job().then(resolve, reject).finally(next); }; running < width ? run() : waiting.push(run); }); };
const PASSWORD_LANE = lane(1), TOKEN_LANE = lane(2);
const scryptOpts = ({ N, r, p }) => ({ N, r, p, maxmem: 256 * N * r });   // twice what the derivation needs (128·N·r); a bound, not an allocation
const kdf = (secret, salt, params) => new Promise((resolve, reject) => { try { scrypt(String(secret), salt, 32, scryptOpts(params), (err, key) => err ? reject(err) : resolve(key)); } catch (err) { reject(err); } });
const formatHash = ({ N, r, p }, salt, key) => `$scrypt$ln=${Math.log2(N)},r=${r},p=${p}$${salt.toString('base64')}$${key.toString('base64')}`;
// The cost, salt and key a stored hash carries; null for anything that is not one of ours. A bare
// `scrypt$<salt>$<key>` from before the cost was written down is read at `legacy` (the password's old
// cost unless the caller says otherwise). The cost is bounded (2^10 ≤ N ≤ 2^20, r ≤ 8, p ≤ 16): a row
// edited to say ln=40 is refused, not derived.
export function parseHash(stored, legacy = LEGACY_PASSWORD_SCRYPT) {
  const s = String(stored || ''); const f = s.split('$');
  if (s.startsWith('$scrypt$') && f.length === 5) {
    const o = Object.fromEntries(f[2].split(',').map(kv => kv.split('='))); const ln = Number(o.ln), r = Number(o.r), p = Number(o.p);
    if (!Number.isInteger(ln) || ln < 10 || ln > 20 || !Number.isInteger(r) || r < 1 || r > 8 || !Number.isInteger(p) || p < 1 || p > 16 || !f[3] || !f[4]) return null;
    return { N: 2 ** ln, r, p, salt: f[3], key: f[4] };
  }
  if (s.startsWith('scrypt$') && f.length === 3 && f[1] && f[2]) return { ...legacy, salt: f[1], key: f[2] };
  return null;
}
const sameCost = (h, c) => !!h && h.N === c.N && h.r === c.r && h.p === c.p;
// Whether a stored password hash is at today's cost; false for a hash from before, or none.
export const passwordHashCurrent = (stored) => sameCost(parseHash(stored), PASSWORD_SCRYPT);
const match = (got, stored) => { const want = Buffer.from(stored, 'base64'); return got.length === want.length && timingSafeEqual(got, want); };
export const hashPasswordSync = (pw, salt = randomBytes(16)) => formatHash(PASSWORD_SCRYPT, salt, scryptSync(String(pw), salt, 32, scryptOpts(PASSWORD_SCRYPT)));
export const hashPassword = (pw, salt = randomBytes(16)) => PASSWORD_LANE(async () => formatHash(PASSWORD_SCRYPT, salt, await kdf(pw, salt, PASSWORD_SCRYPT)));
export async function verifyPassword(pw, stored) {
  const h = parseHash(stored); if (!h) return false;
  return match(await PASSWORD_LANE(() => kdf(pw, Buffer.from(h.salt, 'base64'), h)), h.key);
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
const OWNER_ONLY = /^\/(settings|users|books|books\.csv|api\/books|api\/jobs|api\/exceptions|api\/flags|settings\.json)(\/|$|\?)|^\/contacts\/[^/]+\/(forget|export\.json)(\/|$)|^\/api\/customers\/[^/]+\/(forget|export)(\/|$)/;
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
// Sync (the hash is derived on this thread): it runs from the CLI, before anything listens.
export function setupOperator(db, password, { email = null, name = 'Operator', actor = 'cli' } = {}) {
  if (String(password).length < 12) throw new Error('use at least 12 characters');
  const secret = base32Encode(randomBytes(20)); const em = normEmail(email || 'owner@localhost'); const now = nowIso();
  db.run(`INSERT INTO users (email, name, role, status, password_hash, totp_secret, password_set_at, created_at) VALUES (?, ?, 'owner', 'active', ?, ?, ?, ?)
    ON CONFLICT(email) DO UPDATE SET name = excluded.name, role = 'owner', status = 'active', password_hash = excluded.password_hash, totp_secret = excluded.totp_secret, totp_pending = NULL, password_set_at = excluded.password_set_at, invite_token = NULL, failed_logins = 0, locked_until = NULL`, em, name, hashPasswordSync(password), secret, now, now);
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
export async function acceptInvite(db, tok, password) {
  const u = inviteFor(db, tok); if (!u) throw new Error('this invitation is not valid any more — ask for a new one'); if (String(password).length < 12) throw new Error('use at least 12 characters');
  const secret = base32Encode(randomBytes(20)); const hash = await hashPassword(password);
  if (!inviteFor(db, tok)) throw new Error('this invitation is not valid any more — ask for a new one');   // withdrawn while the hash was derived
  db.run('UPDATE users SET password_hash = ?, totp_pending = ?, password_set_at = ? WHERE id = ?', hash, secret, nowIso(), u.id);
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
export async function changePassword(db, userId, current, next) {
  const r = secretRow(db, userId); if (!r) throw new Error('no such person'); if (!(await verifyPassword(current, r.password_hash))) throw new Error('the current password is wrong');
  if (String(next).length < 12) throw new Error('use at least 12 characters'); if (next === current) throw new Error('that is the same password');
  db.run('UPDATE users SET password_hash = ?, password_set_at = ? WHERE id = ?', await hashPassword(next), nowIso(), userId); db.audit(r.email, 'auth.password', 'user', r.email); return true;
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
// takes the same time either way and the list of emails cannot be read off the clock. The dummy is derived at
// today's cost, once, the first time it is needed (a person whose hash is still at the old cost answers faster
// until their next login re-hashes it; the Health page counts them).
const LOCKS = new Map();
let dummy = null; const dummyHash = () => (dummy ??= hashPassword('the same work for a name nobody has'));
const lockKey = (u, ip) => `${u.id}:${u.email}|${ip || '-'}`;
export function lockState(user, ip, now = Date.now()) { const k = lockKey(user, ip); const l = LOCKS.get(k); if (l && l.until && l.until <= now) { LOCKS.delete(k); return null; } return l || null; }
export const clearLocks = () => LOCKS.clear();
// A wrong try, counted against the person for this address (eight lock the pair for fifteen minutes) and in the audit log.
function recordFailure(db, r, ip, now, why, via = 'password') {
  if (r) {
    db.run('UPDATE users SET failed_logins = failed_logins + 1 WHERE id = ?', r.id);
    const l = lockState(r, ip, now) || { n: 0, until: null }; l.n++; if (l.n >= 8) { l.until = now + 15 * 60_000; db.run('UPDATE users SET locked_until = ? WHERE id = ?', new Date(l.until).toISOString(), r.id); } LOCKS.set(lockKey(r, ip), l);
    if (LOCKS.size > 10_000) for (const [kk, v] of LOCKS) if (!v.until || v.until <= now) LOCKS.delete(kk);
  }
  db.audit('web', via === 'link' ? 'auth.link.failed' : 'auth.login.failed', 'user', r?.email || null, { ip, why }); return null;
}
// The session itself: a new random id, the person's record brought current, the audit row with the address.
function openSession(db, r, { sessionHours, now, ip, step, via = 'password' }) {
  const id = token(32); const csrf = token(24); const exp = new Date(now + sessionHours * 3600_000).toISOString();
  db.run('INSERT INTO sessions (id, created_at, expires_at, csrf, user_id, net) VALUES (?, ?, ?, ?, ?, ?)', id, nowIso(), exp, csrf, r.id, netOf(ip));
  LOCKS.delete(lockKey(r, ip));
  db.run('DELETE FROM sessions WHERE expires_at < ?', nowIso()); db.run('UPDATE users SET last_login_at = ?, failed_logins = 0, locked_until = NULL, totp_last_step = ? WHERE id = ?', nowIso(), step, r.id);
  db.audit(r.email, via === 'link' ? 'auth.link.login' : 'auth.login', 'user', r.email, { ip, via });
  return { id, csrf, expires_at: exp, user_id: r.id, email: r.email, name: r.name, role: r.role, via };
}
// Async: the password is verified off the event loop (see the KDF note at the top). The lock, the replay rule,
// the dummy check and the audit rows are the same as they ever were; the loop is free while scrypt runs.
// `admit` is the caller's last word, asked after the waits and before the session opens (the pipeline asks its
// per-address lockout again there); false refuses without a session and without spending the code, audited as a locked login.
export async function login(db, { email = '', password, code }, { sessionHours = 12, ip = null, now = Date.now(), admit = () => true } = {}) {
  const em = normEmail(email);
  const r = em ? db.get('SELECT * FROM users WHERE email = ?', em) : (() => { const rows = db.all("SELECT * FROM users WHERE status = 'active' AND password_hash IS NOT NULL LIMIT 2"); return rows.length === 1 ? rows[0] : null; })();
  const fail = (why) => { if (!r) db.audit('web', 'auth.login.failed', 'user', em || null, { ip, why }); return recordFailure(db, r, ip, now, why); };
  if (!r || r.status !== 'active' || !r.password_hash || !r.totp_secret) { await verifyPassword(String(password || ''), await dummyHash()); return fail(!r ? (em ? 'unknown' : 'email required') : r.status); }
  if (lockState(r, ip, now)?.until) { db.audit('web', 'auth.login.locked', 'user', r.email, { ip }); return null; }
  if (!(await verifyPassword(password, r.password_hash))) return fail('password');
  // The password is proven. A hash from before today's cost is derived now, before the final checks, so that
  // no wait follows them; it is written only on a full login, and only if the row still holds the hash that
  // was checked (a password changed in another session meanwhile is not overwritten).
  const upgrade = passwordHashCurrent(r.password_hash) ? null : await hashPassword(password);
  // After the waits, what was true at arrival is read again, because the loop no longer serializes logins:
  // the lock (eight wrong tries queued ahead of this one may have armed it), the caller's own lock, the
  // person's status (disabled meanwhile), and the step the replay rule compares against (a login that
  // finished during the wait wrote it — two requests carrying one code yield one session, as before).
  if (lockState(r, ip, now)?.until) { db.audit('web', 'auth.login.locked', 'user', r.email, { ip }); return null; }
  if (!admit()) { db.audit('web', 'auth.login.locked', 'user', r.email, { ip }); return null; }
  const live = db.get('SELECT status, totp_secret, totp_last_step FROM users WHERE id = ?', r.id);
  if (!live || live.status !== 'active' || !live.totp_secret) return fail(live ? live.status : 'unknown');
  const step = verifyTotp(live.totp_secret, code, { time: now, lastStep: live.totp_last_step ?? null });
  if (!step) return fail('code');
  if (upgrade) db.run('UPDATE users SET password_hash = ? WHERE id = ? AND password_hash = ?', upgrade, r.id, r.password_hash);
  return openSession(db, r, { sessionHours, now, ip, step });
}

// ---- the sign-in link: stands in for the password, never for the authenticator ----
// Asked for by email. The answer is the same whether the address is a person here or not (nothing to enumerate);
// a person gets a mail with a link good for a few minutes, once, and the browser that asked gets a nonce cookie
// the link checks — so a link read out of a mailbox somewhere else opens nothing. Both are stored hashed.
const sha = (s) => createHash('sha256').update(String(s)).digest('base64url');
export function requestSignInLink(db, email, { ip = null, now = Date.now(), minutes = 15 } = {}) {
  const em = normEmail(email); const nonce = token(24);
  const r = em ? db.get("SELECT * FROM users WHERE email = ? AND status = 'active' AND totp_secret IS NOT NULL", em) : null;
  db.run('DELETE FROM login_links WHERE expires_at < ?', new Date(now).toISOString());
  if (!r) { db.audit('web', 'auth.link.requested', 'user', em || null, { ip, known: false }); return { nonce, token: null, user: null, minutes }; }
  const t = token(32);
  db.run('INSERT INTO login_links (id, user_id, token_hash, nonce_hash, created_at, expires_at, ip) VALUES (?, ?, ?, ?, ?, ?, ?)', token(12), r.id, sha(t), sha(nonce), new Date(now).toISOString(), new Date(now + minutes * 60_000).toISOString(), ip);
  db.audit('web', 'auth.link.requested', 'user', r.email, { ip, known: true });
  return { nonce, token: t, user: { id: r.id, email: r.email, name: r.name }, minutes };
}
// The link's row, if it is live: not used, not expired.
export const signInLink = (db, t, { now = Date.now() } = {}) => t ? db.get('SELECT l.*, u.email, u.name FROM login_links l JOIN users u ON u.id = l.user_id WHERE l.token_hash = ? AND l.used_at IS NULL AND l.expires_at > ?', sha(t), new Date(now).toISOString()) || null : null;
// The link plus the browser's nonce plus a code from the authenticator: a session. Failures count like a wrong password.
export function loginWithLink(db, { token: t, code, nonce }, { sessionHours = 12, ip = null, now = Date.now() } = {}) {
  const row = signInLink(db, t, { now });
  if (!row) { db.audit('web', 'auth.link.failed', 'user', null, { ip, why: 'no such link' }); return null; }
  const r = db.get('SELECT * FROM users WHERE id = ?', row.user_id);
  if (!r || r.status !== 'active' || !r.totp_secret) return recordFailure(db, r, ip, now, r ? r.status : 'unknown', 'link');
  if (!nonce || !safeEqual(sha(nonce), row.nonce_hash)) return recordFailure(db, r, ip, now, 'browser', 'link');   // opened somewhere other than where it was asked for
  if (lockState(r, ip, now)?.until) { db.audit('web', 'auth.login.locked', 'user', r.email, { ip }); return null; }
  const step = verifyTotp(r.totp_secret, code, { time: now, lastStep: r.totp_last_step ?? null });
  if (!step) return recordFailure(db, r, ip, now, 'code', 'link');
  db.run('UPDATE login_links SET used_at = ? WHERE id = ?', new Date(now).toISOString(), row.id);
  return openSession(db, r, { sessionHours, now, ip, step, via: 'link' });
}
export const sessionFor = (db, id) => id ? db.get("SELECT s.*, u.email, u.name, u.role FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.id = ? AND s.expires_at > ? AND u.status = 'active'", id, nowIso()) : null;
export const logout = (db, id) => db.run('DELETE FROM sessions WHERE id = ?', id);
export const sessionsFor = (db, userId) => db.all('SELECT id, created_at, expires_at FROM sessions WHERE user_id = ? ORDER BY created_at DESC', userId);
export const endOtherSessions = (db, userId, keep) => db.run('DELETE FROM sessions WHERE user_id = ? AND id != ?', userId, keep).changes;

// --- API tokens: `bo_<id>.<secret>`. The id finds the row; the secret is
// verified against a scrypt hash (the same KDF as the password, a lighter
// cost, off the loop like the password: TOKEN_SCRYPT at the top). A copy of
// the table verifies nothing. The hash carries its cost like a password's;
// a row from before (`scrypt$…`, the same N=2^12) is read as that cost.
const tokenHash = (secret, salt = randomBytes(16)) => formatHash(TOKEN_SCRYPT, salt, scryptSync(String(secret), salt, 32, scryptOpts(TOKEN_SCRYPT)));
async function tokenVerify(secret, stored) {
  const h = parseHash(stored, TOKEN_SCRYPT); if (!h) return false;
  return match(await TOKEN_LANE(() => kdf(secret, Buffer.from(h.salt, 'base64'), h)), h.key);
}
// scope 'ro' is read-only: GET only, for a dashboard or a monitor that must never change anything.
export function createApiToken(db, name, { scope = 'rw' } = {}) {
  if (!['rw', 'ro'].includes(scope)) throw new Error('scope is rw or ro');
  const id = token(9); const secret = token(32);
  db.run('INSERT INTO api_tokens (name, token_id, token_hash, scope, created_at) VALUES (?, ?, ?, ?, ?)', name, id, tokenHash(secret), scope, nowIso());
  db.audit('cli', 'auth.token.create', 'api_token', name, { scope });
  return `bo_${id}.${secret}`;
}
export async function apiTokenFor(db, bearer) {
  const m = /^bo_([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)$/.exec(String(bearer || '')); if (!m) return null;
  const row = db.get('SELECT * FROM api_tokens WHERE token_id = ?', m[1]);
  if (!row || !(await tokenVerify(m[2], row.token_hash))) return null;
  if (!db.get('SELECT 1 FROM api_tokens WHERE id = ?', row.id)) return null;   // revoked while the secret was being checked
  db.run('UPDATE api_tokens SET last_used_at = ? WHERE id = ?', nowIso(), row.id);
  return row;
}
export const revokeApiToken = (db, name) => db.run('DELETE FROM api_tokens WHERE name = ?', name).changes;
