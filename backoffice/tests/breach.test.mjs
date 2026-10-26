// Knowing it has not been breached: the audit chain, the key outside the database, the blocks that survive a
// restart, the tripwires, the lockdown, the verify job, and what the doctor and the digest say about all of it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, statSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { openDb, auditMac, CHAIN_GENESIS } from '../src/db.mjs';
import { rechain, verifyChain, chainHead, securitySummary, securityText, lockdown, preflight, nodeSupport, TRIPWIRE, lastVerify } from '../src/integrity.mjs';
import { Watch } from '../src/guard.mjs';
import { setupOperator, totp, createApiToken, requestSignInLink, clearLocks } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';
import { runJob } from '../src/jobs.mjs';
import { doctor, digestText } from '../src/modules/health.mjs';
import { testDb, testCfg } from './helpers.mjs';
import { seedDemo } from '../src/modules/demo.mjs';

const form = (o) => new URLSearchParams(o).toString();
const cookieOf = (r, name) => (r.headers.getSetCookie?.() || [r.headers.get('set-cookie')].filter(Boolean)).map(c => c.split(';')[0]).find(c => c.startsWith(name + '='));

test('the audit chain: every row marked under the key, chained to the one before; a change, a removal or an insertion breaks it; the anchor holds', () => {
  const db = testDb();
  for (let i = 0; i < 5; i++) db.audit('cli', `t.${i}`, 'x', String(i), { i });
  const v = verifyChain(db); assert.equal(v.ok, true); assert.equal(v.chained, 5); assert.equal(v.first_id, 1); assert.equal(v.head.id, 5);
  assert.equal(db.get('SELECT prev FROM audit_log WHERE id = 1').prev, CHAIN_GENESIS); assert.equal(db.get('SELECT prev FROM audit_log WHERE id = 2').prev, db.get('SELECT mac FROM audit_log WHERE id = 1').mac);
  const anchor = `${v.head.id}:${v.head.mac}`; assert.equal(verifyChain(db, { anchor }).anchor.ok, true, 'the anchor is on the chain');
  // a row changed after the fact
  db.run("UPDATE audit_log SET detail = '{\"i\":99}' WHERE id = 3"); const c = verifyChain(db); assert.equal(c.ok, false); assert.equal(c.broken_at, 3); assert.match(c.reason, /mark does not match/);
  db.run("UPDATE audit_log SET detail = '{\"i\":2}' WHERE id = 3"); assert.equal(verifyChain(db).ok, true, 'put back: whole again');
  // a row re-marked under another key: still caught, because the key is not in the database
  const row = db.get('SELECT * FROM audit_log WHERE id = 3'); db.run('UPDATE audit_log SET detail = ?, mac = ? WHERE id = 3', '{"i":7}', auditMac(Buffer.from('an attacker\'s key'), { ...row, detail: '{"i":7}' }, row.prev));
  assert.equal(verifyChain(db).broken_at, 3); db.run('UPDATE audit_log SET detail = ?, mac = ? WHERE id = 3', row.detail, row.mac);
  // a row removed from the middle
  db.run('DELETE FROM audit_log WHERE id = 4'); const d = verifyChain(db); assert.equal(d.ok, false); assert.equal(d.broken_at, 5); assert.match(d.reason, /prev does not match/);
  // the tail cut off: the walk is clean, the anchor is not
  db.run('DELETE FROM audit_log WHERE id = 5'); assert.equal(verifyChain(db).ok, true, 'a cut tail looks clean on its own'); const a = verifyChain(db, { anchor }); assert.equal(a.ok, false); assert.equal(a.anchor.ok, false); assert.match(a.anchor.reason, /no such row/);
  // the anchor's row carrying another mark
  const a2 = verifyChain(db, { anchor: `3:${'x'.repeat(43)}` }); assert.equal(a2.anchor.ok, false); assert.match(a2.anchor.reason, /another mark/);
  assert.equal(verifyChain(db, { anchor: 'nonsense' }).anchor.ok, false);
  // rows from before the chain began (no mark) are counted, not failed; one slipped in after it is
  const db2 = testDb(); db2.run("INSERT INTO audit_log (at, actor, action) VALUES ('2026-01-01T00:00:00.000Z', 'old', 'legacy.row')"); db2.audit('cli', 'new.row');
  const o = verifyChain(db2); assert.equal(o.ok, true); assert.equal(o.unchained, 1); assert.equal(o.chained, 1); assert.equal(o.first_id, 2);
  db2.run("INSERT INTO audit_log (at, actor, action) VALUES ('2026-01-02T00:00:00.000Z', 'x', 'slipped.in')"); const sl = verifyChain(db2); assert.equal(sl.ok, false); assert.match(sl.reason, /unmarked row after/);
  assert.equal(chainHead(db2).id, 2);
});

test('the key lives outside the database: made beside the file at 0600, read back on the next open, or taken from the environment', () => {
  const dir = mkdtempSync(join(tmpdir(), 'bo-chain-')); const path = join(dir, 'backoffice.db');
  try {
    const db = openDb(path); db.audit('cli', 'first'); assert.equal(db.auditKey.source, join(dir, 'audit.key')); assert.equal(db.auditKey.created, true);
    assert.equal(statSync(join(dir, 'audit.key')).mode & 0o777, 0o600); const key = readFileSync(join(dir, 'audit.key'), 'utf8').trim(); assert.equal(key.length, 64);
    const head = chainHead(db); db.close();
    const again = openDb(path); assert.equal(again.auditKey.created, undefined); again.audit('cli', 'second'); const v = verifyChain(again); assert.equal(v.ok, true); assert.equal(v.chained, 2); again.close();
    assert.equal(head.mac, db.get ? head.mac : head.mac);
    const env = openDb(join(dir, 'other.db'), { auditKey: 'from-the-environment' }); assert.equal(env.auditKey.source, 'BACKOFFICE_AUDIT_KEY'); env.audit('cli', 'x'); assert.equal(verifyChain(env).ok, true); assert.equal(verifyChain(env, { key: Buffer.from('wrong') }).ok, false, 'another key verifies nothing'); env.close();
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('a block survives a restart; a tripwire path blocks at once and says so; the customer paths stay open', async () => {
  const db = testDb(); const t0 = Date.now();
  const w = new Watch({ limit: 2, db }); w.miss('9.9.9.9', t0); assert.equal(w.miss('9.9.9.9', t0 + 1), true); assert.equal(db.get('SELECT reason FROM guard_blocks WHERE ip = ?', '9.9.9.9').reason, 'scanner');
  w.block('8.8.8.8', t0, 'tripwire', 60_000);
  const w2 = new Watch({ db }); assert.equal(w2.blocked('9.9.9.9', t0 + 1000), true, 'the block was read back'); assert.equal(w2.blocked('8.8.8.8', t0 + 1000), true);
  assert.equal(w2.blocked('8.8.8.8', t0 + 61_000), false, 'and forgotten when it lapses'); assert.equal(db.get('SELECT ip FROM guard_blocks WHERE ip = ?', '8.8.8.8'), null);
  assert.ok(TRIPWIRE.test('/.env') && TRIPWIRE.test('/wp-login.php') && TRIPWIRE.test('/.git/config') && TRIPWIRE.test('/phpmyadmin/index.php')); assert.ok(!TRIPWIRE.test('/orders') && !TRIPWIRE.test('/o/EL-1/tok') && !TRIPWIRE.test('/login'));
  // over HTTP
  const dbh = testDb(); const cfg = testCfg({ trustedProxies: 'loopback', attentionTtlMs: 0, ntfyUrl: 'https://ntfy.example.test/t' }); const app = createApp({ db: dbh, cfg }); const pushes = []; app.fetchImpl = async (url, o) => { pushes.push({ url, title: o.headers.title, body: o.body }); return { ok: true, status: 200 }; };
  const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; const from = (ip) => ({ 'x-forwarded-for': ip });
  try {
    assert.equal((await fetch(base + '/.env', { headers: from('203.0.113.9') })).status, 404);
    assert.equal((await fetch(base + '/login', { headers: from('203.0.113.9') })).status, 429, 'blocked at once');
    assert.equal((await fetch(base + '/healthz', { headers: from('203.0.113.9') })).status, 200, 'the public endpoints never');
    assert.equal((await fetch(base + '/login', { headers: from('203.0.113.10') })).status, 200, 'another address is not');
    assert.ok(dbh.get("SELECT id FROM audit_log WHERE action = 'guard.tripwire' AND entity_id = '203.0.113.9'")); assert.ok(pushes.some(p => /tripped a wire/.test(p.title)));
    assert.equal(dbh.get("SELECT reason FROM guard_blocks WHERE ip = '203.0.113.9'").reason, 'tripwire');
    await fetch(base + '/wp-login.php', { headers: from('203.0.113.9') }); assert.equal(dbh.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'guard.tripwire'").n, 1, 'said once while the block holds');
  } finally { await app.close(); }
});

test('lockdown: every session ended, every token revoked, every link spent, in one audited step — from the CLI and from Settings', async () => {
  const db = testDb(); const cfg = testCfg({ attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' }); createApiToken(db, 'bot'); requestSignInLink(db, 'o@errer.test', { ip: '1.1.1.1' });
    const login = async () => { const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) }); assert.equal(r.status, 303); return cookieOf(r, 'bo_session'); };
    const s1 = await login();
    const r = lockdown(db, { actor: 'cli', why: 'a test' }); assert.equal(r.sessions, 1); assert.deepEqual(r.tokens, ['bot']); assert.equal(r.links, 1);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions').n, 0); assert.equal(db.get('SELECT COUNT(*) AS n FROM api_tokens').n, 0); assert.equal(db.get('SELECT COUNT(*) AS n FROM login_links WHERE used_at IS NULL').n, 0);
    const a = db.get("SELECT detail FROM audit_log WHERE action = 'lockdown'"); assert.match(a.detail, /a test/);
    assert.equal((await fetch(base + '/orders', { redirect: 'manual', headers: { cookie: s1, accept: 'text/html' } })).status, 303, 'the old session is gone');
    // from Settings, as the owner: the lockdown ends the owner's own session too and sends them to the door
    clearLocks(); db.run('UPDATE users SET totp_last_step = NULL');   // the authenticator's one-time step, reset for a second login in the same half minute
    const s2 = await login(); assert.ok(s2, 'signed in again'); const page = await (await fetch(base + '/settings', { headers: { cookie: s2 } })).text(); assert.ok(page.includes('Lock down now')); const csrf = /name="_csrf" value="([^"]+)"/.exec(page)[1];
    const ld = await fetch(base + '/settings/lockdown', { method: 'POST', redirect: 'manual', headers: { cookie: s2, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, why: 'from the page' }) });
    assert.equal(ld.status, 303); assert.ok(decodeURIComponent(ld.headers.get('location')).includes('Locked down: 1 sessions ended')); assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions').n, 0);
    assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'lockdown' AND actor = 'o@errer.test'").n, 1);
  } finally { await app.close(); clearLocks(); }
});

test('the verify job, the self-check, the doctor rows, and the digest\'s SECURITY block with its anchor', async () => {
  const db = testDb(); const cfg = testCfg({ operatorEmail: 'me@errer.test', attentionTtlMs: 0 }); const app = createApp({ db, cfg });
  setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
  const r = await runJob(app, 'verify'); assert.equal(r.ok, true); assert.equal(lastVerify(db).ok, true); assert.ok(lastVerify(db).chained > 0);
  const rows = doctor(db, cfg).checks; for (const k of ['audit_chain', 'preflight', 'node', 'security_day']) assert.ok(rows.find(x => x.key === k), k);
  assert.equal(rows.find(x => x.key === 'audit_chain').ok, true); assert.ok(rows.find(x => x.key === 'audit_chain').detail.includes('anchor'));
  const pf = preflight(db, cfg); assert.ok(pf.rows.find(x => x.key === 'chain_tail').ok); assert.equal(pf.rows.find(x => x.key === 'audit_key').ok, false, 'an in-memory key is not a key outside the database');
  assert.equal(preflight(db, { ...cfg, demo: true }).rows.find(x => x.key === 'audit_key').ok, null, 'the demo says it, does not fail it');
  assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'boot.preflight'"), 'a failing self-check at boot is written down');
  const n = nodeSupport('22.22.0', Date.parse('2026-10-26')); assert.equal(n.ok, true); assert.equal(nodeSupport('22.22.0', Date.parse('2027-03-01')).ok, null, '90 days before: a warning'); assert.equal(nodeSupport('22.22.0', Date.parse('2027-05-01')).ok, false); assert.equal(nodeSupport('18.0.0').ok, false);
  // break the chain: the job fails, raises, pushes
  db.run("UPDATE audit_log SET actor = 'someone' WHERE id = 1"); const pushes = []; app.fetchImpl = async (url, o) => { pushes.push(o.headers.title); return { ok: true, status: 200 }; }; cfg.ntfyUrl = 'https://ntfy.example.test/t';
  const bad = await runJob(app, 'verify'); assert.equal(bad.ok, false); assert.match(bad.error, /broken at row 1/); assert.ok(db.get("SELECT key FROM exceptions WHERE key = 'audit:chain' AND resolved_at IS NULL")); assert.ok(pushes.some(t => /BROKEN/.test(t)));
  assert.equal(doctor(db, cfg).checks.find(x => x.key === 'audit_chain').ok, false);
  db.run("UPDATE audit_log SET actor = 'system' WHERE id = 1");
  // the summary and the digest
  const s = securitySummary(db, cfg, { hours: 24 }); assert.ok(s.head); assert.ok(Array.isArray(s.logins)); const t = securityText(db, cfg); assert.match(t, /SECURITY — the last 24 h/); assert.match(t, new RegExp(`anchor\\s+${s.head.id}:${s.head.mac}`)); assert.match(t, /backoffice verify --anchor/);
  const d = digestText(db, cfg); assert.ok(d.includes('SECURITY — the last 24 h')); assert.ok(d.includes(`${s.head.id}:${s.head.mac}`), 'the anchor rides in the digest, off the box');
  // a verify run after the repair clears the exception
  const ok = await runJob(app, 'verify'); assert.equal(ok.ok, true);
});

test('the demo backdates its own history and re-marks the chain, saying so; a real record is never re-marked', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true }); await seedDemo(db, cfg);
  const v = verifyChain(db); assert.equal(v.ok, true, v.reason || ''); assert.ok(v.chained > 50);
  const rc = db.get("SELECT detail FROM audit_log WHERE action = 'audit.rechain'"); assert.ok(rc, 'the re-marking is written down'); assert.match(rc.detail, /demo seed/);
  assert.throws(() => rechain(db), /synthetic history only/);
});
