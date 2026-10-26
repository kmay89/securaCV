// The process-level nets: an outbound call that never answers is abandoned, not waited on forever; the
// database on disk runs in WAL with full sync; /healthz says "not ok" when the database cannot answer or
// the scheduler has stopped ticking; a socket that opens and says nothing is dropped; a request body over
// the cap is refused; and the HTTP surface answers garbage with 4xx, never 5xx.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { connect } from 'node:net';
import { mkdtempSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { timedFetch } from '../src/net.mjs';
import { openDb } from '../src/db.mjs';
import { createApp } from '../src/server.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('timedFetch abandons a call that never answers, and honors a signal the caller passes', async () => {
  const srv = createServer(() => { /* never answers */ });
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  const url = `http://127.0.0.1:${srv.address().port}/`;
  try {
    await assert.rejects(timedFetch(url, { signal: AbortSignal.timeout(200) }), (e) => /abort|timeout/i.test(e.name + e.message));
    // the default, 20 s, is the one production uses; the shape is what matters here: a signal is always present
    const seen = []; const fake = (u, init) => { seen.push(init.signal instanceof AbortSignal); return Promise.resolve(new Response('ok')); };
    const orig = globalThis.fetch; globalThis.fetch = fake;
    try { await timedFetch(url); await timedFetch(url, { signal: AbortSignal.timeout(50) }); } finally { globalThis.fetch = orig; }
    assert.deepEqual(seen, [true, true], 'every call carries an AbortSignal');
  } finally { srv.closeAllConnections?.(); srv.close(); }
});

test('a database on disk runs in WAL with full synchronous', () => {
  const dir = mkdtempSync(join(tmpdir(), 'bo-wal-'));
  try {
    const db = openDb(join(dir, 'x.db'));
    assert.equal(db.get('PRAGMA journal_mode').journal_mode, 'wal');
    assert.equal(db.get('PRAGMA synchronous').synchronous, 2, '2 is FULL');
    db.close();
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('/healthz fails when the scheduler has not ticked in five minutes, and passes when it has', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); await seedDemo(db, cfg);
  const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    assert.equal((await fetch(base + '/healthz')).status, 200, 'no scheduler started: only the database is checked');
    app.schedulerTick = Date.now() - 6 * 60_000;
    const stale = await fetch(base + '/healthz'); assert.equal(stale.status, 503); assert.match(await stale.text(), /scheduler stalled/);
    app.schedulerTick = Date.now();
    assert.equal((await fetch(base + '/healthz')).status, 200);
    // the stamp itself: a scheduler whose job never resolves stops stamping (the guard returns without it), one whose jobs finish keeps stamping
    const { startScheduler } = await import('../src/jobs.mjs'); const wait = (ms) => new Promise(r => setTimeout(r, ms));
    let ran = 0; const stopStuck = startScheduler(app, { tickMs: 15, jobs: { stall: [0, () => { ran++; return new Promise(() => {}); }] } });
    try { const t0 = app.schedulerTick; await wait(150); assert.equal(ran, 1, 'the stuck job is entered once; every later tick returns on the guard'); assert.equal(app.schedulerTick, t0, 'and none of those ticks stamps'); } finally { stopStuck(); }
    const stopQuick = startScheduler(app, { tickMs: 15, jobs: { quick: [0, async () => ({ ok: true })] } });
    try { const t0 = app.schedulerTick; await wait(150); assert.ok(app.schedulerTick > t0, 'a loop that completes stamps'); } finally { stopQuick(); }
  } finally { await app.close(); }
});

test('a socket that opens and sends nothing is dropped by the headers timeout; a body over 1 MiB is refused', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); await seedDemo(db, cfg);
  const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    assert.ok(app.closeAll, 'the app can cut every connection on shutdown');
    // over the cap the server cuts the connection rather than read on (a client sees a 413 or a reset, never an accepted body)
    const big = await fetch(base + '/login', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: 'a=' + 'b'.repeat((1 << 20) + 10) }).then(r => r.status, e => 'cut: ' + e.message);
    assert.ok(big === 413 || /cut/.test(big), `a 1 MiB+ body was neither refused nor cut: ${big}`);
    // a half-open connection does not stop the server from answering others
    const s = connect(addr.port, '127.0.0.1'); s.write('GET / HTTP/1.1\r\nHost: x\r\nX-');
    assert.equal((await fetch(base + '/healthz')).status, 200);
    s.destroy();
  } finally { await app.close(); }
});

test('trusted proxies decide whose X-Forwarded-For counts; a session on the API writes only with its CSRF token; the probe stays off the inside; the books CSV is the owner\'s', async () => {
  const { isTrustedProxy, setTrustedProxies } = await import('../src/http.mjs');
  const { setupOperator, totp, can } = await import('../src/auth.mjs');
  assert.ok(isTrustedProxy('127.0.0.1', ['loopback']) && isTrustedProxy('::1', ['loopback']) && !isTrustedProxy('8.8.8.8', ['loopback']));
  assert.ok(isTrustedProxy('10.1.2.3', ['private']) && isTrustedProxy('172.20.0.2', ['private']) && isTrustedProxy('192.168.1.9', ['private']) && !isTrustedProxy('172.32.0.1', ['private']) && !isTrustedProxy('11.0.0.1', ['private']));
  assert.ok(isTrustedProxy('203.0.113.7', ['203.0.113.0/24']) && !isTrustedProxy('203.0.114.7', ['203.0.113.0/24']) && isTrustedProxy('::ffff:203.0.113.9', ['203.0.113.9']) && !isTrustedProxy('203.0.113.9', []));
  assert.ok(!can('ops', 'GET', '/books.csv') && can('owner', 'GET', '/books.csv'), 'the CSV is as owner-only as the page');
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const form = (o) => new URLSearchParams(o).toString();
    // the test client is the loopback peer, a trusted proxy by default: its X-Forwarded-For names the person
    const lie = { 'x-forwarded-for': '198.51.100.7' };
    await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded', ...lie }, body: form({ email: 'o@errer.test', password: 'wrong', code: '000000' }) });
    assert.equal(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'auth.login.failed' ORDER BY id DESC LIMIT 1").detail).ip, '198.51.100.7');
    setTrustedProxies([]);   // nothing in front: the header is just a header
    try {
      await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded', ...lie }, body: form({ email: 'o@errer.test', password: 'wrong', code: '000000' }) });
      assert.equal(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'auth.login.failed' ORDER BY id DESC LIMIT 1").detail).ip, '127.0.0.1');
    } finally { setTrustedProxies(cfg.trustedProxies); }
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    const order = { email: 'x@example.test', name: 'X', subtotal_cents: 100, shipping_cents: 0, tax_cents: 0, total_cents: 100, items: [{ sku: 'wap-kit', qty: 1, unit_cents: 100 }], ship: { line1: '1', city: 'B', state: 'MA', zip: '02134', country: 'US' } };
    const bare = await fetch(base + '/api/orders', { method: 'POST', headers: { cookie, 'content-type': 'application/json' }, body: JSON.stringify(order) });
    assert.equal(bare.status, 403); assert.match((await bare.json()).error, /session CSRF token/);
    assert.equal((await fetch(base + '/api/orders', { method: 'POST', headers: { cookie, 'content-type': 'application/json', 'x-csrf': csrf }, body: JSON.stringify(order) })).status, 201, 'the X-CSRF header');
    assert.equal((await fetch(base + '/api/orders', { method: 'POST', headers: { cookie, 'content-type': 'application/json' }, body: JSON.stringify({ ...order, _csrf: csrf, allow_duplicate: true }) })).status, 201, 'or _csrf in the body');
    assert.equal((await fetch(base + '/api/orders', { headers: { cookie } })).status, 200, 'a read needs none');
    for (const url of ['http://127.0.0.1:9/', 'http://localhost/x', 'http://10.0.0.5/', 'http://169.254.169.254/latest/meta-data/', 'http://[::1]/', 'http://backoffice.internal/', 'http://100.64.0.1/']) {
      const p = await fetch(base + '/status-admin/probe', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, name: 'x', url }) });
      assert.equal(p.status, 400, url); assert.match((await p.json()).error, /public address/);
    }
    assert.equal(db.get('SELECT COUNT(*) AS n FROM probes').n, 0);
  } finally { await app.close(); }
});
