// BO-A2: one password job per person, and per invitation link, at a time. The KDF lane is the door's (one derivation
// at a time), so a session or a link that could queue a hundred derivations would hold every login behind them; a
// second job while one is pending is refused (429), not queued. The work is counted here, not timed: crypto.scrypt and
// crypto.createHmac are wrapped and syncBuiltinESMExports() points auth.mjs's imports at the wrappers (this file runs
// in its own process under `node --test`, so nothing else sees them). `gate` holds every derivation until it is
// opened, so what answers while one derivation is held is exactly what was refused without queueing behind it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { syncBuiltinESMExports } from 'node:module';
import { testDb, testCfg } from './helpers.mjs';
import { setupOperator, inviteUser, acceptInvite, confirmTotp, beginTotpRotation, checkPassword, changePassword, login, sessionFor, totp, inviteFor, pendingTotp, userByEmail } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';

const work = { kdf: 0, sha1: 0, failAt: 0 };   // derivations started, TOTP HMACs (three per wrong code), and the derivation to fail
const gate = { closed: false, held: [], open() { this.closed = false; for (const run of this.held.splice(0)) run(); } };
const realScrypt = crypto.scrypt, realHmac = crypto.createHmac;
crypto.scrypt = function (...args) {
  const n = ++work.kdf;
  if (n === work.failAt) { const cb = args[args.length - 1]; return process.nextTick(cb, new Error('the KDF failed (injected by the test)')); }
  if (gate.closed) return void gate.held.push(() => realScrypt.apply(this, args));
  return realScrypt.apply(this, args);
};
crypto.createHmac = function (alg, ...rest) { if (alg === 'sha1') work.sha1++; return realHmac.call(this, alg, ...rest); };
syncBuiltinESMExports();

const form = (o) => new URLSearchParams(o).toString();
const until = async (cond, ms = 5000) => { const end = Date.now() + ms; while (!cond() && Date.now() < end) await new Promise(r => setTimeout(r, 10)); return cond(); };
const wrongCode = (secret) => { const near = new Set([-1, 0, 1].map(w => totp(secret, { time: Date.now() + w * 30_000 }))); for (let n = 0; ; n++) { const c = String(n).padStart(6, '0'); if (!near.has(c)) return c; } };
async function boot() {
  const db = testDb(); const cfg = testCfg(); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  const secret = setupOperator(db, 'owner password 1234', { email: 'owner@errer.test', name: 'Olive Owner' });
  const inv = inviteUser(db, { email: 'vic@errer.test', name: 'Vic Viewer', role: 'viewer' }); const acc = await acceptInvite(db, inv.invite_token, 'viewer password 1234'); confirmTotp(db, acc.id, totp(acc.totp_pending));
  const post = (p, body, h = {}) => fetch(base + p, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded', ...h }, body: form(body) });
  db.run('UPDATE users SET totp_last_step = NULL');
  const v = await post('/login', { email: 'vic@errer.test', password: 'viewer password 1234', code: totp(acc.totp_pending) }); const cookie = v.headers.get('set-cookie').split(';')[0]; const csrf = sessionFor(db, cookie.split('=')[1]).csrf;
  const asViewer = (p, body, ip) => post(p, { _csrf: csrf, ...body }, { cookie, 'x-forwarded-for': ip });
  const ownerLogin = async (ip = '198.51.100.7') => { db.run('UPDATE users SET totp_last_step = NULL'); return post('/login', { email: 'owner@errer.test', password: 'owner password 1234', code: totp(secret) }, { 'x-forwarded-for': ip }); };
  return { db, app, post, asViewer, ownerLogin, viewer: { id: acc.id, secret: acc.totp_pending }, close: async () => { gate.open(); await app.close(); db.close(); } };
}

test('a session holds one password check at a time, and an invitation link one hash: the rest are refused, not queued ahead of the door', async () => {
  const b = await boot();
  try {
    const k0 = work.kdf;
    const flood = await Promise.all(Array.from({ length: 6 }, (_, i) => b.asViewer('/me/password', { current: `guess ${i}`, next: 'whatever whatever 1' }, '203.0.113.40').then(r => r.status)));
    assert.deepEqual(flood.sort(), [400, 429, 429, 429, 429, 429], 'one check runs, the rest are refused while it is pending');
    assert.equal(work.kdf - k0, 1, 'one derivation, not six');
    assert.equal(b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.password.failed'").n, 1);
    // the same rule for an invitation link: one hash at a time
    const inv2 = inviteUser(b.db, { email: 'new@errer.test', name: 'New Hire', role: 'support' }); const k1 = work.kdf;
    const acc2 = await Promise.allSettled(Array.from({ length: 4 }, (_, i) => acceptInvite(b.db, inv2.invite_token, `a password of mine ${i}`)));
    assert.equal(acc2.filter(x => x.status === 'fulfilled').length, 1); assert.ok(acc2.filter(x => x.status === 'rejected').every(x => x.reason.status === 429 && x.reason.message === 'this invitation is already being set up — reload this page in a moment'));
    assert.equal(work.kdf - k1, 1); assert.ok(inviteFor(b.db, inv2.invite_token), 'the link still works');
    // the owner's own login is untouched
    const o = await b.ownerLogin(); assert.equal(o.status, 303); assert.equal(o.headers.get('location'), '/');
  } finally { await b.close(); }
});

test('fifty wrong secrets from one stolen cookie on two addresses, with an invitation link posted among them: what runs is within the bound, and a refusal says nothing about the guess', async () => {
  const b = await boot();
  try {
    const A = '203.0.113.50', B = '203.0.113.51';
    assert.equal((await b.asViewer('/me/totp', { current: 'viewer password 1234' }, '198.51.100.60')).status, 303, 'a rotation begun from a third address, so /me/totp/confirm compares the current code');
    const next = pendingTotp(b.db, b.viewer.id); const bad = wrongCode(b.viewer.secret), nextCode = totp(next);
    const inv = inviteUser(b.db, { email: 'new@errer.test', name: 'New Hire', role: 'support' }); const link = `/invite/${inv.invite_token}`;
    const k0 = work.kdf, h0 = work.sha1; gate.closed = true;
    // one password check gets in first and is held in its derivation; forty-nine more arrive while it is
    const first = b.asViewer('/me/password', { current: 'guess p0', next: 'whatever whatever 1' }, A);
    assert.ok(await until(() => work.kdf - k0 === 1), 'the first check is in its derivation');
    const sends = [
      ...Array.from({ length: 30 }, (_, i) => () => b.asViewer('/me/totp/confirm', { current_code: bad, code: nextCode }, i % 2 ? B : A)),
      ...Array.from({ length: 4 }, (_, i) => () => b.asViewer('/me/password', { current: `guess p${i + 1}`, next: 'whatever whatever 1' }, i % 2 ? A : B)),
      ...Array.from({ length: 5 }, (_, i) => () => b.asViewer('/me/totp', { current: `guess t${i}` }, i % 2 ? B : A)),
      ...Array.from({ length: 5 }, (_, i) => () => b.asViewer('/me/sessions/end', { current: `guess s${i}` }, i % 2 ? B : A)),
      ...Array.from({ length: 5 }, (_, i) => () => b.post(link, { password: `a password of mine ${i}`, again: `a password of mine ${i}` })),
    ];
    const done = []; const all = sends.map((send, i) => send().then(r => { done.push(i); return r; }));
    assert.ok(await until(() => done.length === 48), `48 of the 49 answered while that derivation was held, nothing queued behind it but the link's one hash (answered: ${done.length})`);
    assert.equal(work.kdf - k0, 1, 'still the one derivation');
    assert.equal(work.sha1 - h0, 16 * 3, 'sixteen code compares, eight per address (three HMACs each), none after each lock armed');
    gate.open(); const res = [await first, ...await Promise.all(all)];
    assert.equal(work.kdf - k0, 2, 'two derivations in all: the session\'s one password check and the link\'s one hash');
    const st = res.map(r => r.status); const confirm = st.slice(1, 31), pw = [st[0], ...st.slice(31, 45)], invite = res.slice(45);
    assert.deepEqual([confirm.filter(x => x === 400).length, confirm.filter(x => x === 429).length], [14, 16], 'per address: seven wrong codes answered, the eighth arms the lock, the rest refused by it');
    assert.deepEqual(pw, Array(15).fill(429), 'the held check answered with the lock it found when it finished; the other fourteen were refused at once');
    assert.equal(invite.filter(r => r.headers.get('location') === link).length, 1, 'one post of the link set the password');
    assert.ok(invite.every(r => r.status === 303) && invite.filter(r => /already%20being%20set%20up/.test(r.headers.get('location') || '')).length === 4, 'four were told the link is busy');
    const rows = (ip) => b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action IN ('auth.totp.failed', 'auth.password.failed') AND json_extract(detail, '$.ip') = ?", ip).n;
    assert.deepEqual([rows(A), rows(B)], [8, 8], 'eight failures per address: the check in flight at A when its lock armed is not a ninth (its record is the refusal\'s)');
    assert.equal(b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.locked' AND json_extract(detail, '$.ip') = ? AND json_extract(detail, '$.via') = 'first factor'", A).n, 1, 'the held check wrote one locked attempt at A');
    // locked now at both addresses: the right secret and a wrong one get one answer, and neither runs any work
    b.db.run('UPDATE users SET totp_last_step = NULL'); const curCode = totp(b.viewer.secret); const k1 = work.kdf, h1 = work.sha1;
    const answer = async (r) => [r.status, await r.json()];
    const right = await answer(await b.asViewer('/me/sessions/end', { current: 'viewer password 1234' }, A)), wrong = await answer(await b.asViewer('/me/sessions/end', { current: 'not it at all' }, A));
    const rightCode = await answer(await b.asViewer('/me/totp/confirm', { current_code: curCode, code: nextCode }, B)), wrongC = await answer(await b.asViewer('/me/totp/confirm', { current_code: bad, code: nextCode }, B));
    assert.deepEqual(right, wrong); assert.deepEqual(rightCode, wrongC); assert.deepEqual(right, rightCode); assert.equal(right[0], 429);
    assert.deepEqual([work.kdf - k1, work.sha1 - h1], [0, 0], 'no derivation and no code compare for any of the four');
    assert.equal(pendingTotp(b.db, b.viewer.id), next, 'nothing was swapped'); assert.equal(b.db.get('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?', b.viewer.id).n, 1, 'nothing was ended');
    const o = await b.ownerLogin(); assert.equal(o.status, 303); assert.equal(o.headers.get('location'), '/', 'and the owner still signs in');
  } finally { await b.close(); }
});

test('the key is let go on every way out: a wrong password, a short or a same new one, a failed derivation, an invitation withdrawn mid-hash; a re-sent link is never refused for the old one', async () => {
  const b = await boot();
  try {
    const id = b.viewer.id; const pw = 'viewer password 1234';
    await assert.rejects(checkPassword(b.db, id, 'not it'), /current password is wrong/); assert.ok(await checkPassword(b.db, id, pw), 'after a wrong password');
    await assert.rejects(changePassword(b.db, id, pw, 'short'), /12 characters/); await assert.rejects(changePassword(b.db, id, pw, pw), /same password/);
    assert.ok(await beginTotpRotation(b.db, id, pw), 'after a short and a same new password');
    work.failAt = work.kdf + 1; await assert.rejects(checkPassword(b.db, id, pw), /KDF failed/); assert.ok(await checkPassword(b.db, id, pw), 'after the check\'s derivation failed');
    work.failAt = work.kdf + 2; await assert.rejects(changePassword(b.db, id, pw, 'a brand new password 9'), /KDF failed/); assert.ok(await checkPassword(b.db, id, pw), 'after the new hash\'s derivation failed, and the password is unchanged');
    // an invitation: withdrawn (a new link sent) while its hash is derived; the new link is taken at once, not refused for the old one
    const inv = inviteUser(b.db, { email: 'new@errer.test', name: 'New Hire', role: 'support' }); gate.closed = true;
    const old = acceptInvite(b.db, inv.invite_token, 'a password of mine 1'); const re = inviteUser(b.db, { email: 'new@errer.test', name: 'New Hire', role: 'support' });
    const fresh = acceptInvite(b.db, re.invite_token, 'a password of mine 2');
    gate.open(); await assert.rejects(old, /not valid any more/); assert.ok(await fresh, 'the re-sent link worked while the old one\'s hash was pending');
    work.failAt = work.kdf + 1; await assert.rejects(acceptInvite(b.db, re.invite_token, 'a password of mine 3'), /KDF failed/);
    assert.ok(await acceptInvite(b.db, re.invite_token, 'a password of mine 4'), 'after a failed derivation the link takes the next post');
    assert.equal(b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.password.failed'").n, 1, 'only the one wrong password was a failure');
  } finally { work.failAt = 0; await b.close(); }
});

test('a new database never inherits a lock or a pending key from another in the same process, even for the same id and email', async () => {
  const one = testDb(), two = testDb();
  try {
    const s1 = setupOperator(one, 'owner password 1234', { email: 'owner@errer.test' }), s2 = setupOperator(two, 'owner password 1234', { email: 'owner@errer.test' });
    const u1 = userByEmail(one, 'owner@errer.test'), u2 = userByEmail(two, 'owner@errer.test'); assert.deepEqual([u1.id, u1.email], [u2.id, u2.email], 'the same id and email in both');
    const X = '203.0.113.70';
    for (let i = 0; i < 8; i++) assert.equal(await login(one, { email: 'owner@errer.test', password: `wrong ${i} wrong`, code: '000000' }, { ip: X }), null);
    assert.equal(await login(one, { email: 'owner@errer.test', password: 'owner password 1234', code: totp(s1) }, { ip: X }), null, 'the first database\'s owner is locked at X');
    await assert.rejects(checkPassword(one, u1.id, 'owner password 1234', { ip: X }), (e) => e.status === 429, 'and so are its re-checks');
    assert.ok(await login(two, { email: 'owner@errer.test', password: 'owner password 1234', code: totp(s2) }, { ip: X }), 'the second database\'s owner is not');
    gate.closed = true; const held = checkPassword(one, u1.id, 'owner password 1234', { ip: '203.0.113.71' });
    const other = checkPassword(two, u2.id, 'owner password 1234', { ip: '203.0.113.71' });
    gate.open(); assert.ok(await held); assert.ok(await other, 'a job pending in one database is no pending key in the other');
  } finally { gate.open(); one.close(); two.close(); }
});

// The check that is mid-derivation when the eighth wrong try arms the lock: its answer, its audit row, the users row
// and the lock's end are the same whichever password it carried (every role reads the audit log, so a row that differed
// would tell the cookie what the answer does not), and a right one does nothing it asked for. Held on each of the three
// routes that ask for the password, each from its own address, once with the right password and once with a wrong one.
test('a password check in flight when the lock arms is the refusal in every way, whichever was typed: one answer, one record, the count and the lock\'s end untouched; the right one ends, changes and begins nothing', async () => {
  const pw = 'viewer password 1234'; const seen = {};
  for (const right of [true, false]) {
    const b = await boot();
    try {
      b.db.run('UPDATE users SET totp_last_step = NULL');
      assert.equal((await b.post('/login', { email: 'vic@errer.test', password: pw, code: totp(b.viewer.secret) }, { 'x-forwarded-for': '198.51.100.61' })).status, 303, 'a second session of the same person, for /me/sessions/end to end');
      assert.equal((await b.asViewer('/me/totp', { current: pw }, '198.51.100.60')).status, 303, 'a rotation begun from a third address, so /me/totp/confirm compares the current code');
      const next = pendingTotp(b.db, b.viewer.id); const bad = wrongCode(b.viewer.secret);
      const sessions = () => b.db.get('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?', b.viewer.id).n; assert.equal(sessions(), 2);
      const record = [];
      for (const [k, [route, body]] of [['/me/sessions/end', (p) => ({ current: p })], ['/me/password', (p) => ({ current: p, next: 'a brand new password 9' })], ['/me/totp', (p) => ({ current: p })]].entries()) {
        const A = `203.0.113.8${k}`; const confirm = () => b.asViewer('/me/totp/confirm', { current_code: bad, code: totp(next) }, A);
        for (let i = 0; i < 7; i++) assert.equal((await confirm()).status, 400, `${route}: seven wrong codes from ${A} are answered`);
        const k0 = work.kdf; gate.closed = true;
        const held = b.asViewer(route, body(right ? pw : 'not the password at all'), A);
        assert.ok(await until(() => work.kdf - k0 === 1), `${route}: the check is in its derivation`);
        assert.equal((await confirm()).status, 429, `${route}: the eighth wrong code arms the lock while the check is held`);
        const user = () => b.db.get('SELECT failed_logins, locked_until FROM users WHERE id = ?', b.viewer.id); const before = user();
        const last = b.db.get('SELECT MAX(id) AS id FROM audit_log').id;
        gate.open(); const r = await held;
        assert.equal(r.status, 429, `${route}: the held check is answered by the lock`); const said = await r.json(); assert.match(said.error, /too many wrong tries from this address/);
        assert.deepEqual(user(), before, `${route}: the count and the lock's end are as the eighth left them`);
        const rows = b.db.all('SELECT actor, action, detail FROM audit_log WHERE id > ? ORDER BY id', last).map(x => `${x.actor} ${x.action} ${x.detail}`);
        assert.deepEqual(rows, [`vic@errer.test auth.login.locked {"ip":"${A}","via":"first factor"}`], `${route}: one locked attempt, nothing else`);
        record.push([route, r.status, said, rows]);
      }
      seen[right] = record;
      assert.equal(sessions(), 2, 'no session was ended'); assert.equal(pendingTotp(b.db, b.viewer.id), next, 'no new rotation was begun');
      assert.ok(await checkPassword(b.db, b.viewer.id, pw, { ip: '198.51.100.62' }), 'the password is unchanged');
    } finally { await b.close(); }
  }
  assert.deepEqual(seen.true, seen.false, 'the right password and a wrong one leave the very same trace');
});

// Not a fix: a guard on what SECURITY.md says of the count the re-checks share with the door. It has no window (it
// stands until a full sign-in from that address, the lock running out, or a restart), and a full sign-in clears it
// only for the address it came from.
test('the count is the door\'s, as SECURITY.md states it: no window (seven wrong, and the eighth a day later locks), and a full sign-in clears only its own address', async () => {
  const db = testDb();
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'owner@errer.test' }); const u = userByEmail(db, 'owner@errer.test');
    const X = '203.0.113.90', Y = '198.51.100.90', Z = '203.0.113.91'; const T = Date.now(); const day = 24 * 3600_000;
    const tryAt = (ip, now, pw = 'a wrong guess here') => checkPassword(db, u.id, pw, { ip, now }).then(() => 'ok', (e) => e.status || 400);
    const seven = async (ip) => { const out = []; for (let i = 0; i < 7; i++) out.push(await tryAt(ip, T)); return out; };
    assert.deepEqual(await seven(X), Array(7).fill(400));
    assert.equal(await tryAt(X, T + day), 429, 'the eighth, a day later, arms the lock: the count has no window');
    assert.equal(await tryAt(X, T + day + 1000, 'owner password 1234'), 429, 'and the right password is refused there');
    assert.equal(await tryAt(X, T + day + 16 * 60_000, 'owner password 1234'), 'ok', 'until the lock runs out');
    assert.deepEqual(await seven(Z), Array(7).fill(400));
    db.run('UPDATE users SET totp_last_step = NULL'); assert.ok(await login(db, { email: 'owner@errer.test', password: 'owner password 1234', code: totp(secret) }, { ip: Y, now: T }), 'a full sign-in from another address');
    assert.equal(await tryAt(Z, T), 429, 'clears nothing at this one: the eighth here still locks');
  } finally { db.close(); }
});
