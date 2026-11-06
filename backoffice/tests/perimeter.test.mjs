// The perimeter, reviewed: which address a request is judged by (X-Forwarded-For read from the right), which
// unit the per-address controls count (a v6 /64) and how they shed memory (the oldest half, never everything),
// what counts as the inside for the probe (every v6 spelling of a private address), the chain's ids and
// encoding, the email the door writes down, the hashed session and invitation rows and the migration that
// ends the clear ones, the roles the API mirrors, and the door's own cross-site refusal.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, rmSync, readdirSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { clientIp, setTrustedProxies, Lockout, RateLimiter } from '../src/http.mjs';
import { limiterKey, trimMap, Watch } from '../src/guard.mjs';
import { isPrivateAddress } from '../src/net.mjs';
import { isInternalHost } from '../src/admin.mjs';
import { openDb, auditMac, auditMacV1, CHAIN_GENESIS } from '../src/db.mjs';
import { verifyChain } from '../src/integrity.mjs';
import { setupOperator, login, requestSignInLink, can, clearLocks, beginTotpRotation, confirmTotp, pendingTotp, totp, userByEmail } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';
import { testDb, testCfg } from './helpers.mjs';

const form = (o) => new URLSearchParams(o).toString();
const req = (peer, fwd) => ({ socket: { remoteAddress: peer }, headers: fwd == null ? {} : { 'x-forwarded-for': fwd } });

test('X-Forwarded-For is read from the right behind a trusted proxy: the hop it appended, trusted hops skipped, the client\'s own entries never reached', () => {
  setTrustedProxies(['loopback', '10.0.0.2']);
  try {
    assert.equal(clientIp(req('127.0.0.1', '1.2.3.4, 203.0.113.9')), '203.0.113.9', 'the last hop is what the proxy appended; 1.2.3.4 is what the client wrote');
    assert.equal(clientIp(req('203.0.113.50', '1.2.3.4, 203.0.113.9')), '203.0.113.50', 'an untrusted peer: the header is just a header');
    assert.equal(clientIp(req('127.0.0.1', '1.2.3.4, 10.0.0.2')), '1.2.3.4', 'a trusted hop in the chain (a proxy behind a proxy) is skipped');
    assert.equal(clientIp(req('127.0.0.1', '10.0.0.2, 127.0.0.1')), '127.0.0.1', 'every hop trusted: the peer');
    assert.equal(clientIp(req('127.0.0.1', '  203.0.113.9:4431 ')), '203.0.113.9', 'trimmed, a v4 port dropped');
    assert.equal(clientIp(req('127.0.0.1', '1.2.3.4, [2001:db8::9]:443')), '2001:db8::9', 'a bracketed v6 with a port');
    assert.equal(clientIp(req('127.0.0.1', 'fe80::1%eth0')), 'fe80::1', 'a zone id dropped');
    assert.equal(clientIp(req('127.0.0.1', 'unknown, 203.0.113.9')), '203.0.113.9', 'what is not an address is not a hop');
    assert.equal(clientIp(req('127.0.0.1', 'unknown')), '127.0.0.1');
    assert.equal(clientIp(req('127.0.0.1', null)), '127.0.0.1', 'no header: the peer');
  } finally { setTrustedProxies(['loopback', 'private']); }
});

test('the per-address controls count a v6 address by its /64, and past ten thousand keys shed the oldest half — never the count against the address that is guessing', () => {
  assert.equal(limiterKey('1.2.3.4'), '1.2.3.4'); assert.equal(limiterKey('2001:db8::1'), '2001:0db8:0000:0000::/64'); assert.equal(limiterKey('2001:db8:0:0:ffff::9'), '2001:0db8:0000:0000::/64');
  assert.equal(limiterKey('login:2001:db8::1'), 'login:2001:0db8:0000:0000::/64'); assert.equal(limiterKey('forms:::ffff:1.2.3.4'), 'forms:1.2.3.4'); assert.equal(limiterKey('::ffff:1.2.3.4'), '1.2.3.4');
  assert.equal(limiterKey('link:ada@example.test'), 'link:ada@example.test'); assert.equal(limiterKey('-'), '-'); assert.equal(limiterKey('addr:EL-000123'), 'addr:EL-000123');
  const t0 = 1_000_000;
  const lock = new Lockout({ limit: 8 });
  for (let i = 0; i < 8; i++) lock.fail(`2001:db8::${i.toString(16)}`, t0 + i);
  assert.ok(lock.locked('2001:db8::ffff', t0 + 9), 'eight failures across one /64 lock the /64'); assert.ok(!lock.locked('2001:db8:0:1::1', t0 + 9), 'the next /64 is another address');
  const rl = new RateLimiter({ limit: 2 }); assert.ok(rl.allow('login:2001:db8::1', t0)); assert.ok(rl.allow('login:2001:db8::2', t0)); assert.ok(!rl.allow('login:2001:db8::3', t0)); assert.ok(rl.allow('login:1.2.3.4', t0));
  const w = new Watch({ limit: 2 }); w.miss('2001:db8::1', t0); assert.equal(w.miss('2001:db8::2', t0 + 1), true); assert.ok(w.blocked('2001:db8::abcd', t0 + 2)); assert.equal(w.list(t0 + 2)[0].ip, '2001:0db8:0000:0000::/64');
  // the flood: a guesser's own count survives the table filling up behind it
  const big = new Lockout({ limit: 8 });
  for (let i = 0; i < 7; i++) big.fail('203.0.113.9', t0 + i);
  for (let i = 0; i < 9_995; i++) big.fail(`10.${i >> 16}.${(i >> 8) & 255}.${i & 255}`, t0 + 100 + i);
  big.fail('203.0.113.9', t0 + 20_000); assert.ok(big.locked('203.0.113.9', t0 + 20_001), 'the eighth failure locks'); assert.equal(big.fails.size, 9996);
  for (let i = 0; i < 5; i++) big.fail(`192.0.2.${i}`, t0 + 30_000 + i);
  assert.equal(big.fails.size, 5000, 'the oldest half went, not everything'); assert.ok(big.locked('203.0.113.9', t0 + 30_010), 'and the lock on the guesser held'); assert.equal(big.fail('192.0.2.4', t0 + 30_011), 2, 'the newest keys kept their counts');
  const m = new Map(); for (let i = 0; i < 12; i++) m.set(`k${i}`, i); trimMap(m, { max: 10, stale: v => v < 2 }); assert.deepEqual([...m.keys()], ['k7', 'k8', 'k9', 'k10', 'k11'], 'stale first, then the oldest, down to half');
});

test('isPrivateAddress: every v4 and v6 spelling of the inside is the inside; the probe refuses them all', () => {
  for (const ip of ['127.0.0.1', '10.1.2.3', '172.16.0.1', '172.31.255.255', '192.168.1.1', '169.254.169.254', '100.64.0.1', '100.127.255.254', '0.0.0.0', '192.0.0.1', '198.18.0.1', '224.0.0.1', '255.255.255.255',
    '::1', '::', '[::1]', '::ffff:127.0.0.1', '[::ffff:127.0.0.1]', '::ffff:7f00:1', '::ffff:169.254.169.254', '::ffff:a9fe:a9fe', '::ffff:10.0.0.1', '::7f00:1', '64:ff9b::7f00:1', '64:ff9b::a9fe:a9fe', '64:ff9b:1::1', '2002:7f00:1::1', '2002:a9fe:a9fe::1',
    'fc00::1', 'fd12:3456:789a::1', 'fe80::1', 'fe80::1%eth0', 'fec0::1', 'ff02::1', '2001:db8::1', 'not an address', '', '1.2.3']) assert.equal(isPrivateAddress(ip), true, ip);
  for (const ip of ['8.8.8.8', '203.0.113.9', '172.32.0.1', '100.128.0.1', '11.0.0.1', '2606:4700::1111', '::ffff:8.8.8.8', '::ffff:808:808', '64:ff9b::808:808', '2002:808:808::1', '2a00:1450:4001::1']) assert.equal(isPrivateAddress(ip), false, ip);
  for (const u of ['http://[::ffff:127.0.0.1]/', 'http://[::ffff:169.254.169.254]/', 'http://[::]/', 'http://[64:ff9b::a9fe:a9fe]/', 'http://[fc00::1]/', 'http://[fe80::1]/', 'http://[::1]/', 'http://0x7f.1/', 'http://2130706433/', 'http://169.254.169.254/', 'http://localhost/', 'http://a.internal/', 'not a url']) assert.equal(isInternalHost(u), true, u);
  for (const u of ['https://example.com/', 'http://[2606:4700::1111]/', 'http://8.8.8.8/', 'http://[::ffff:8.8.8.8]/']) assert.equal(isInternalHost(u), false, u);
});

test('the chain walks from the lowest id and names one below 1; the mark covers the id and cannot be read as two fields; marks from before still verify', () => {
  const db = testDb(); const key = db.auditKey.key;
  for (let i = 0; i < 3; i++) db.audit('cli', `t.${i}`);
  assert.equal(verifyChain(db).ok, true);
  db.run("INSERT INTO audit_log (id, at, actor, action) VALUES (0, '2026-01-01T00:00:00.000Z', 'x', 'slipped.in')");
  let v = verifyChain(db); assert.equal(v.ok, false); assert.equal(v.broken_at, 0); assert.match(v.reason, /below 1/); db.run('DELETE FROM audit_log WHERE id = 0');
  db.run("INSERT INTO audit_log (id, at, actor, action, prev, mac) VALUES (-5, '2026-01-01T00:00:00.000Z', 'x', 'slipped.in', 'genesis', 'x')");
  v = verifyChain(db); assert.equal(v.ok, false); assert.equal(v.broken_at, -5); db.run('DELETE FROM audit_log WHERE id = -5');
  // a field that holds the old separator: under the old mark the boundary could move into the neighbor field
  // (action 'x␟y' + entity 'z' reads the same as action 'x' + entity 'y␟z'); under the new one it cannot
  db.audit('cli', 'x\u001fy', 'z'); const r = db.get('SELECT * FROM audit_log WHERE id = 4'); assert.equal(r.action, 'x\u001fy'); assert.equal(verifyChain(db).ok, true);
  const moved = { ...r, action: 'x', entity: 'y\u001fz' };
  assert.equal(auditMacV1(key, moved, r.prev), auditMacV1(key, r, r.prev), 'the old form could not tell the two apart');
  assert.notEqual(auditMac(key, moved, r.prev), auditMac(key, r, r.prev), 'the new one can');
  db.run('UPDATE audit_log SET action = ?, entity = ? WHERE id = 4', moved.action, moved.entity); v = verifyChain(db); assert.equal(v.ok, false); assert.equal(v.broken_at, 4); assert.match(v.reason, /mark does not match/);
  db.run('UPDATE audit_log SET action = ?, entity = ? WHERE id = 4', r.action, r.entity); assert.equal(verifyChain(db).ok, true);
  // a row moved to another id fails its mark
  db.run('UPDATE audit_log SET id = 9 WHERE id = 4'); v = verifyChain(db); assert.equal(v.ok, false); assert.equal(v.broken_at, 9); db.run('UPDATE audit_log SET id = 4 WHERE id = 9'); assert.equal(verifyChain(db).ok, true);
  // the walk starts at the lowest id, so a verify asked `from` a later row still judges the same chain
  assert.equal(verifyChain(db, { from: 3 }).chained, 2); assert.equal(verifyChain(db).chained, 4);
  // rows marked the old way (before the id was in the mark) verify, and a new row chains from their head
  const old = testDb(); const k2 = old.auditKey.key; old.audit('cli', 'a'); old.audit('cli', 'b');
  let prev = CHAIN_GENESIS; for (const row of old.all('SELECT * FROM audit_log ORDER BY id')) { const mac = auditMacV1(k2, row, prev); old.run('UPDATE audit_log SET prev = ?, mac = ? WHERE id = ?', prev, mac, row.id); prev = mac; }
  assert.equal(verifyChain(old).ok, true, 'the old marks pass'); old.audit('cli', 'c'); v = verifyChain(old); assert.equal(v.ok, true); assert.equal(v.chained, 3); assert.equal(v.head.id, 3);
  assert.notEqual(auditMacV1(k2, old.get('SELECT * FROM audit_log WHERE id = 3'), old.get('SELECT mac FROM audit_log WHERE id = 2').mac), old.get('SELECT mac FROM audit_log WHERE id = 3').mac, 'the new row is marked the new way');
});

test('an email typed at the door is scrubbed and capped before the chain writes it down; a stranger\'s link request leaves no row and the same audit row', async () => {
  const db = testDb(); setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
  const long = 'a'.repeat(5000) + '‮​@example.test';
  assert.equal(await login(db, { email: long, password: 'x', code: '000000' }), null);
  const row = db.get("SELECT entity_id FROM audit_log WHERE action = 'auth.login.failed' AND entity_id IS NOT NULL ORDER BY id DESC LIMIT 1");
  assert.ok(row.entity_id.length <= 254 && !/[‮​]/.test(row.entity_id), `${row.entity_id.length} chars`);
  const r = requestSignInLink(db, long, { ip: '203.0.113.5' }); assert.equal(r.token, null);
  const link = db.get("SELECT entity_id, detail FROM audit_log WHERE action = 'auth.link.requested' ORDER BY id DESC LIMIT 1"); assert.ok(link.entity_id.length <= 254); assert.equal(JSON.parse(link.detail).known, false);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM login_links').n, 0, 'the scratch row is written and unwritten in one transaction');
  assert.ok(requestSignInLink(db, 'o@errer.test', { ip: '203.0.113.5' }).token); assert.equal(db.get('SELECT COUNT(*) AS n FROM login_links').n, 1);
  clearLocks();
});

test('a new authenticator needs the password to begin and a code from each to confirm; migration 016 ends the clear sessions and invitations from before', async () => {
  const db = testDb(); const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' }); const me = userByEmail(db, 'o@errer.test');
  await assert.rejects(() => beginTotpRotation(db, me.id, 'nope', { ip: '203.0.113.5' }), /current password/); assert.equal(pendingTotp(db, me.id), null);
  assert.equal(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'auth.password.failed'").detail).ip, '203.0.113.5');
  const next = await beginTotpRotation(db, me.id, 'owner password 1234', { ip: '203.0.113.5' }); assert.ok(next);
  assert.throws(() => confirmTotp(db, me.id, totp(next)), /current authenticator/); assert.throws(() => confirmTotp(db, me.id, totp(next), { current: '000000' }), /current authenticator/);
  assert.equal(confirmTotp(db, me.id, '000000', { current: totp(secret) }), false, 'the current code alone does not switch either');
  db.run('UPDATE users SET totp_last_step = NULL'); assert.equal(confirmTotp(db, me.id, totp(next), { current: totp(secret), ip: '203.0.113.5' }), true);
  assert.equal(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'auth.totp'").detail).ip, '203.0.113.5');
  assert.equal(await login(db, { email: 'o@errer.test', password: 'owner password 1234', code: totp(secret, { time: Date.now() + 60_000 }) }, { now: Date.now() + 60_000 }), null, 'the old authenticator is gone');
  assert.ok(await login(db, { email: 'o@errer.test', password: 'owner password 1234', code: totp(next, { time: Date.now() + 60_000 }) }, { now: Date.now() + 60_000 }));
  clearLocks();
  // the migration
  const DIR = fileURLToPath(new URL('../src/migrations', import.meta.url)); const work = mkdtempSync(join(tmpdir(), 'bo-016-')); const before = join(work, 'm'); mkdirSync(before);
  try {
    for (const f of readdirSync(DIR).filter(f => f.endsWith('.sql') && f < '016')) copyFileSync(join(DIR, f), join(before, f));
    const path = join(work, 'x.db'); const a = openDb(path, { migrationsDir: before }); const soon = new Date(Date.now() + 3600_000).toISOString();
    a.run("INSERT INTO sessions (id, created_at, expires_at, csrf, user_id) VALUES ('clear-id', ?, ?, 'c', 1)", new Date().toISOString(), soon);
    a.run("INSERT INTO users (email, name, status, invite_token, invite_expires_at, created_at) VALUES ('p@errer.test', 'P', 'invited', 'clear-token', ?, ?)", soon, new Date().toISOString()); a.close();
    const b = openDb(path); assert.equal(b.schema.pending_applied.at(-1), '016_hashed_ids.sql');
    assert.equal(b.get('SELECT COUNT(*) AS n FROM sessions').n, 0, 'every session from before is ended');
    const p = b.get('SELECT status, invite_token, invite_expires_at FROM users WHERE email = ?', 'p@errer.test'); assert.equal(p.status, 'invited'); assert.equal(p.invite_token, null); assert.equal(p.invite_expires_at, null, 'the open invitation is expired; the person stays to be re-invited');
    b.close();
  } finally { rmSync(work, { recursive: true, force: true }); }
});

test('the API answers to the same rule as the page it mirrors', () => {
  assert.ok(!can('ops', 'GET', '/api/users') && !can('support', 'GET', '/api/users') && can('owner', 'GET', '/api/users'));
  assert.ok(!can('ops', 'POST', '/health/external') && can('owner', 'POST', '/health/external') && can('ops', 'GET', '/health'));
  assert.ok(!can('ops', 'POST', '/api/backups') && can('ops', 'GET', '/api/backups') && can('viewer', 'GET', '/api/backups') && can('owner', 'POST', '/api/backups'));
  assert.ok(can('ops', 'GET', '/api/exceptions') && can('ops', 'POST', '/api/exceptions/x/resolve') && can('ops', 'POST', '/exceptions/resolve') && can('viewer', 'GET', '/api/exceptions') && !can('viewer', 'POST', '/api/exceptions/x/resolve') && !can('support', 'POST', '/api/exceptions/x/resolve'));
  assert.ok(!can('ops', 'GET', '/api/users/'), 'a trailing slash is the same path');
});

test('the login forms refuse a cross-site browser before the body is read, like every other POST', async () => {
  const db = testDb(); const cfg = testCfg({ attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const post = (path, site) => fetch(base + path, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded', ...(site ? { 'sec-fetch-site': site } : {}) }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: '000000' }) });
    for (const path of ['/login', '/login/link', '/login/link/sometoken']) {
      assert.equal((await post(path, 'cross-site')).status, 403, `${path} from another site`); assert.equal((await post(path, 'same-site')).status, 403, `${path} from a sibling host`);
      assert.notEqual((await post(path, 'same-origin')).status, 403, `${path} from the page itself`); assert.notEqual((await post(path, 'none')).status, 403, `${path} typed or from a bookmark`); assert.notEqual((await post(path)).status, 403, `${path} from a client that says nothing`);
    }
    assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.failed'").n, 3, 'the refused posts never reached the door: one failed try per accepted /login post');
  } finally { await app.close(); clearLocks(); }
});
