import { test } from 'node:test';
import assert from 'node:assert/strict';
import { scryptSync } from 'node:crypto';
import { hashPassword, hashPasswordSync, verifyPassword, parseHash, passwordHashCurrent, PASSWORD_SCRYPT, totp, verifyTotp, base32Encode, base32Decode, setupOperator, login, sessionFor, createApiToken, apiTokenFor, revokeApiToken, userByEmail } from '../src/auth.mjs';
import { testDb, testCfg } from './helpers.mjs';
import { doctor } from '../src/modules/health.mjs';

// a hash as 0.8 wrote it: `scrypt$<salt>$<key>`, N=2^14, the cost implied
const legacyHash = (pw, N = 16384, salt = Buffer.from('0123456789abcdef')) => `scrypt$${salt.toString('base64')}$${scryptSync(pw, salt, 32, { N, r: 8, p: 1 }).toString('base64')}`;

test('scrypt password hashes verify and do not match a wrong password; the hash says its own cost', async () => {
  const h = await hashPassword('correct horse battery staple');
  assert.ok(await verifyPassword('correct horse battery staple', h));
  assert.ok(!(await verifyPassword('correct horse battery stable', h)));
  assert.ok(!(await verifyPassword('x', null)));
  assert.equal(PASSWORD_SCRYPT.N, 2 ** 17, 'OWASP\'s floor for scrypt');
  assert.match(h, /^\$scrypt\$ln=17,r=8,p=1\$[A-Za-z0-9+/=]+\$[A-Za-z0-9+/=]+$/, 'the PHC string format, the cost written down');
  assert.ok(passwordHashCurrent(h)); assert.deepEqual({ N: parseHash(h).N, r: parseHash(h).r, p: parseHash(h).p }, PASSWORD_SCRYPT);
  assert.equal(hashPasswordSync('correct horse battery staple').split('$')[2], h.split('$')[2], 'the sync form (the CLI, the demo seed) writes the same cost');
  assert.ok(await verifyPassword('correct horse battery staple', hashPasswordSync('correct horse battery staple')));
  // a hash from before the cost was written down is read at the old cost, verifies, and is not current
  const old = legacyHash('correct horse battery staple');
  assert.ok(await verifyPassword('correct horse battery staple', old)); assert.ok(!(await verifyPassword('correct horse battery stable', old)));
  assert.ok(!passwordHashCurrent(old)); assert.equal(parseHash(old).N, 16384);
  // a cost outside the bounds, or a shape that is not ours, is refused without deriving anything
  for (const bad of ['$scrypt$ln=40,r=8,p=1$c2FsdA==$a2V5', '$scrypt$ln=17,r=64,p=1$c2FsdA==$a2V5', '$scrypt$ln=x,r=8,p=1$c2FsdA==$a2V5', '$scrypt$ln=17,r=8,p=1$$a2V5', 'scrypt$only', 'bcrypt$x$y', '', null]) { assert.equal(parseHash(bad), null, String(bad)); assert.ok(!(await verifyPassword('pw', bad)), String(bad)); }
});

test('verification runs off the event loop: a timer fires while three derivations are pending', async () => {
  const h = hashPasswordSync('a password to check');
  const order = [];
  const pending = Promise.all([1, 2, 3].map(() => verifyPassword('a password to check', h))).then((r) => { order.push('kdf'); return r; });
  await new Promise(r => setTimeout(() => { order.push('timer'); r(); }, 5));
  assert.ok((await pending).every(Boolean));
  assert.deepEqual(order, ['timer', 'kdf'], 'the loop answered the timer before scrypt was done; a synchronous KDF would have held it');
});

test('TOTP matches the RFC 6238 SHA-1 test vectors (6-digit truncation)', () => {
  const secret = base32Encode(Buffer.from('12345678901234567890'));
  assert.equal(base32Decode(secret).toString(), '12345678901234567890');
  // RFC 6238 Appendix B, SHA1: T=59 → 94287082; T=1111111109 → 07081804; T=1234567890 → 89005924
  assert.equal(totp(secret, { time: 59 * 1000 }), '287082');
  assert.equal(totp(secret, { time: 1111111109 * 1000 }), '081804');
  assert.equal(totp(secret, { time: 1234567890 * 1000 }), '005924');
  assert.ok(verifyTotp(secret, '287082', { time: 59 * 1000 }));
  assert.ok(verifyTotp(secret, '287082', { time: 59 * 1000 + 30_000 }), 'one step of drift is accepted');
  assert.ok(!verifyTotp(secret, '287082', { time: 59 * 1000 + 90_000 }));
  assert.ok(!verifyTotp(secret, 'abcdef', { time: 59 * 1000 }));
});

test('operator setup, login with password + code, sessions and API tokens', async () => {
  const db = testDb();
  const secret = setupOperator(db, 'a very long password');
  assert.equal(await login(db, { password: 'a very long password', code: '000000' }), null, 'wrong code fails');
  assert.equal(await login(db, { password: 'wrong', code: totp(secret) }), null, 'wrong password fails');
  const s = await login(db, { password: 'a very long password', code: totp(secret) });
  assert.ok(s && s.id && s.csrf && s.role === 'owner', 'the only person can log in without typing an email');
  // a code is good for one login: the same one again is refused, the next step's is not
  assert.equal(await login(db, { email: 'owner@localhost', password: 'a very long password', code: totp(secret) }), null, 'the code just used is spent');
  const later = Date.now() + 30_000;
  assert.ok(await login(db, { email: 'owner@localhost', password: 'a very long password', code: totp(secret, { time: later }) }, { now: later }), 'and with the email');
  assert.equal(await login(db, { email: 'nobody@localhost', password: 'a very long password', code: totp(secret, { time: later }) }, { now: later }), null);
  assert.ok(sessionFor(db, s.id));
  assert.equal(sessionFor(db, 'nope'), null);
  const t = createApiToken(db, 'cli');
  assert.ok(t.startsWith('bo_'));
  assert.equal((await apiTokenFor(db, t)).name, 'cli');
  assert.match(db.get('SELECT token_hash FROM api_tokens').token_hash, /^\$scrypt\$ln=12,r=8,p=1\$/, 'a token hash says its cost too');
  assert.equal(await apiTokenFor(db, 'bo_bad'), null);
  assert.equal(await apiTokenFor(db, t.replace(/.$/, c => c === 'a' ? 'b' : 'a')), null, 'a one-character change fails');
  assert.equal(await apiTokenFor(db, t.split('.')[0] + '.x'), null, 'the right id with the wrong secret fails');
  assert.equal(revokeApiToken(db, 'cli'), 1);
  assert.equal(await apiTokenFor(db, t), null);
  // a token row from before the cost was written down (`scrypt$…`, N=2^12 implied) still verifies
  const t2 = createApiToken(db, 'old'); const [, secret2] = t2.split('.'); db.run('UPDATE api_tokens SET token_hash = ? WHERE name = ?', legacyHash(secret2, 4096), 'old');
  assert.equal((await apiTokenFor(db, t2)).name, 'old'); assert.equal(await apiTokenFor(db, t2.slice(0, -1) + (t2.endsWith('a') ? 'b' : 'a')), null);
});

test('a password hash at the old cost is re-hashed on the next successful login, and the doctor counts the ones still waiting', async () => {
  const db = testDb(); const cfg = testCfg(); const secret = setupOperator(db, 'a very long password', { email: 'o@errer.test', name: 'O' });
  const id = userByEmail(db, 'o@errer.test').id; db.run('UPDATE users SET password_hash = ? WHERE id = ?', legacyHash('a very long password'), id);
  const row = () => Object.fromEntries(doctor(db, cfg).checks.map(c => [c.key, c])).password_cost;
  assert.equal(row().ok, null, 'a note, never a failure'); assert.match(row().detail, /^1 of 1 still at the old cost — re-hashed on their next sign-in: o@errer\.test$/);
  assert.equal(await login(db, { email: 'o@errer.test', password: 'wrong wrong wrong', code: totp(secret) }), null); assert.ok(!passwordHashCurrent(db.get('SELECT password_hash FROM users WHERE id = ?', id).password_hash), 'a wrong password changes nothing');
  assert.equal(await login(db, { email: 'o@errer.test', password: 'a very long password', code: '000000' }), null); assert.ok(!passwordHashCurrent(db.get('SELECT password_hash FROM users WHERE id = ?', id).password_hash), 'nor a wrong code');
  assert.ok(await login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret) }));
  const h = db.get('SELECT password_hash, password_set_at FROM users WHERE id = ?', id); assert.ok(passwordHashCurrent(h.password_hash), 'brought up to the current cost'); assert.ok(await verifyPassword('a very long password', h.password_hash));
  assert.equal(row().ok, true); assert.equal(row().detail, 'all 1');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action LIKE 'auth.login%'").n, 3, 'two failures and one login: the audit rows are the ones they always were');
  const later = Date.now() + 30_000; assert.ok(await login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret, { time: later }) }, { now: later }), 'and the re-hashed password logs in');
});

test('one code, two logins at once: the replay rule holds across the wait, with a current hash and with one being upgraded', async () => {
  const db = testDb(); const secret = setupOperator(db, 'a very long password', { email: 'o@errer.test', name: 'O' }); const id = userByEmail(db, 'o@errer.test').id;
  const both = await Promise.all([0, 1].map(() => login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret) })));
  assert.equal(both.filter(Boolean).length, 1, 'the second read the step the first wrote');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.failed'").n, 1);
  // a hash at the old cost adds a derivation to the path; it runs before the final checks, so the second login still reads what the first wrote
  db.run('UPDATE users SET password_hash = ? WHERE id = ?', legacyHash('a very long password'), id); const now = Date.now() + 60_000;
  const legacy = await Promise.all([0, 1].map(() => login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret, { time: now }) }, { now })));
  assert.equal(legacy.filter(Boolean).length, 1, 'one session, not two');
  assert.ok(passwordHashCurrent(db.get('SELECT password_hash FROM users WHERE id = ?', id).password_hash), 'and the hash was brought up');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM sessions").n, 2);
});

test('a lock armed by failures queued ahead refuses the login that lands after them, and the caller has a last word before a session opens', async () => {
  const db = testDb(); const secret = setupOperator(db, 'a very long password', { email: 'o@errer.test', name: 'O' }); const ip = '203.0.113.9';
  // eight wrong tries and the right one, all at once: the right one's derivation is queued last, and by then the pair is locked
  const nine = await Promise.all([...Array(8).fill(null).map(() => login(db, { email: 'o@errer.test', password: 'wrong wrong wrong', code: '000000' }, { ip })), login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret) }, { ip })]);
  assert.ok(nine.every(s => s === null), 'none got in'); assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.failed'").n, 8);
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.locked'").n, 1, 'the ninth was refused as locked, after its wait'); assert.ok(userByEmail(db, 'o@errer.test').locked_until, 'and the lock stands');
  assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions').n, 0);
  // the lock is per address: the same person from another one gets in, and the code was not spent by the refusal
  assert.ok(await login(db, { email: 'o@errer.test', password: 'a very long password', code: totp(secret) }, { ip: '198.51.100.2' }));
  // the caller's last word (the pipeline's per-address lockout, asked again after the wait): a refusal opens nothing and spends nothing
  const later = Date.now() + 30_000; const code = totp(secret, { time: later });
  assert.equal(await login(db, { email: 'o@errer.test', password: 'a very long password', code }, { ip: '198.51.100.2', now: later, admit: () => false }), null);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions').n, 1); assert.ok(await login(db, { email: 'o@errer.test', password: 'a very long password', code }, { ip: '198.51.100.2', now: later }), 'the same code still works: it was never accepted');
});
