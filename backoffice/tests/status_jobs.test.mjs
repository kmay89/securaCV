import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { upsertProbe, runProbe, runDueProbes, publicStatus, openIncident, updateIncident, uptime } from '../src/modules/status.mjs';
import { raise, resolve, openExceptions } from '../src/modules/exceptions.mjs';
import { backup, listBackups } from '../src/modules/backup.mjs';
import { createApp } from '../src/server.mjs';
import { runJob, ingestInbox } from '../src/jobs.mjs';
import { createServer } from 'node:http';
import { isPrivateAddress } from '../src/net.mjs';
import { upsertItem } from '../src/modules/inventory.mjs';
import { testDb, testCfg } from './helpers.mjs';

const fakeFetch = (status, body = '') => async () => ({ status, text: async () => body });
const publicName = async () => [{ address: '203.0.113.9', family: 4 }];   // a documentation address: what a public hostname resolves to, for the fakes

test('probes: up/down by status and body text; three failures raise an exception; recovery resolves it', async () => {
  const db = testDb(); const cfg = testCfg();
  const p = upsertProbe(db, { name: 'Website', url: 'https://x.test/', expect_text: 'SecuraCV' });
  assert.ok((await runProbe(db, p, { lookup: publicName, fetchImpl: fakeFetch(200, '<h1>SecuraCV</h1>'), cfg })).ok);
  assert.ok(!(await runProbe(db, p, { lookup: publicName, fetchImpl: fakeFetch(200, 'parked domain'), cfg })).ok);
  assert.match((await runProbe(db, p, { lookup: publicName, fetchImpl: fakeFetch(503), cfg })).detail, /^503/);
  assert.equal(openExceptions(db).length, 0, 'two failures are not yet an exception');
  assert.ok(!(await runProbe(db, p, { lookup: publicName, fetchImpl: async () => { throw new Error('ECONNREFUSED'); }, cfg })).ok);
  assert.equal(openExceptions(db)[0].key, 'probe:Website');
  assert.equal(publicStatus(db).overall, 'degraded');
  await runProbe(db, p, { lookup: publicName, fetchImpl: fakeFetch(200, 'SecuraCV'), cfg });
  assert.equal(openExceptions(db).length, 0);
  assert.equal(uptime(db, p.id), 40);
  assert.equal(publicStatus(db).overall, 'operational');
  const inc = openIncident(db, { title: 'Lab slow', body: 'CDN issue' });
  assert.equal(publicStatus(db).overall, 'degraded');
  updateIncident(db, inc.id, { status: 'resolved' });
  assert.equal(publicStatus(db).overall, 'operational');
  db.run('UPDATE probes SET last_at = NULL');
  assert.equal((await runDueProbes(db, { lookup: publicName, fetchImpl: fakeFetch(200, 'SecuraCV'), cfg })).length, 1);
});

test('exceptions dedupe by key and mirror to GitHub when a token is set', async () => {
  const db = testDb(); const calls = [];
  const fetchImpl = async (url, opts) => { calls.push([opts.method, url]); return { ok: true, json: async () => ({ number: 42 }) }; };
  const cfg = testCfg({ githubToken: 'ghp_x', githubRepo: 'o/r' });
  const e1 = await raise(db, cfg, 'stock:x', 'low x', 'body', { fetchImpl });
  assert.equal(e1.github_issue, 42);
  const e2 = await raise(db, cfg, 'stock:x', 'low x', 'body2', { fetchImpl });
  assert.ok(e2.bumped); assert.equal(calls.length, 1, 'no second issue');
  await resolve(db, cfg, 'stock:x', 'restocked', { fetchImpl });
  assert.equal(calls.length, 3); assert.equal(calls[2][0], 'PATCH');
  assert.equal(openExceptions(db).length, 0);
  assert.equal(await resolve(db, cfg, 'stock:x', 'again', { fetchImpl }), null);
});

test('jobs run, record their result, and the stock job raises/resolves reorder exceptions', async () => {
  const db = testDb(); const cfg = testCfg({ githubToken: '' });
  const app = createApp({ db, cfg });
  upsertItem(db, { sku: 'cable', name: 'USB-C cable', on_hand: 1, reorder_point: 5, reorder_qty: 50, supplier: 'Digi-Key' });
  const r = await runJob(app, 'stock');
  assert.ok(r.ok); assert.equal(r.result.low, 1); assert.equal(openExceptions(db)[0].key, 'stock:cable');
  db.run('UPDATE inventory_items SET on_hand = 100');
  await runJob(app, 'stock');
  assert.equal(openExceptions(db).length, 0);
  assert.equal(db.get('SELECT last_ok FROM job_runs WHERE name = ?', 'stock').last_ok, 1);
  const bad = await runJob(app, 'nope').catch(e => e);
  assert.match(bad.message, /unknown job/);
  assert.equal((await runJob(app, 'inbox')).result.skipped, 'IMAP_URL not set');
});

test('backups are consistent copies with retention', () => {
  const db = testDb(); const dir = mkdtempSync(join(tmpdir(), 'bo-'));
  db.setSetting('k', 'v');
  for (let i = 0; i < 4; i++) backup(db, dir, { keep: 2, now: new Date(2026, 0, 1 + i) });
  const files = listBackups(dir);
  assert.equal(files.length, 2);
  const { openDb } = await_import_sync();
  const copy = openDb(join(dir, files[0].name));
  assert.equal(copy.setting('k'), 'v');
  copy.close(); rmSync(dir, { recursive: true });
});
import { openDb as _openDb } from "../src/db.mjs"; function await_import_sync() { return { openDb: _openDb }; }

test('a probe target that stalls while sending its body hits the same deadline', async () => {
  const db = testDb(); const cfg = testCfg();
  const p = upsertProbe(db, { name: 'Stall', url: 'https://x.test/', expect_text: 'ok' });
  const stalled = async () => ({ status: 200, text: () => new Promise(() => {}) });
  const r = await runProbe(db, p, { lookup: publicName, fetchImpl: stalled, cfg, timeoutMs: 50 });
  assert.ok(!r.ok); assert.equal(r.detail, 'timeout');
});

test('a probe is checked at every run: a name that resolves inward is refused, a redirect is the result and never followed, a body past the budget fails it', async () => {
  const db = testDb(); const cfg = testCfg(); const hits = [];
  const srv = createServer((req, res) => { hits.push(req.url); if (req.url === '/bounce') { res.writeHead(302, { location: '/inside' }); return res.end(); } if (req.url === '/big') { res.writeHead(200); let n = 0; const push = () => { while (n < 3 * 1024 * 1024) { n += 65536; if (!res.write('x'.repeat(65536))) return res.once('drain', push); } res.end('SecuraCV'); }; return push(); } res.writeHead(200); res.end('SecuraCV ok'); });
  await new Promise(r => srv.listen(0, '127.0.0.1', r)); const base = `http://127.0.0.1:${srv.address().port}`;
  const pub = async () => [{ address: '203.0.113.9', family: 4 }];
  try {
    // the saved URL looks public; what it resolves to at run time is what counts
    for (const addr of ['127.0.0.1', '10.0.0.5', '169.254.169.254', '::1', '::ffff:192.168.1.1', '64:ff9b::a00:1', 'fd00::1', 'fe80::1']) {
      const p = upsertProbe(db, { name: `rebound ${addr}`, url: base + '/', expect_text: 'SecuraCV' });
      const r = await runProbe(db, p, { lookup: async () => [{ address: '203.0.113.9' }, { address: addr }], cfg }); assert.ok(!r.ok, addr); assert.match(r.detail, /private or internal/);
    }
    assert.deepEqual(hits, [], 'nothing was sent');
    // the literal loopback address with the real resolver
    const lit = upsertProbe(db, { name: 'literal', url: base + '/', expect_text: 'SecuraCV' }); assert.match((await runProbe(db, lit, { cfg })).detail, /private or internal/); assert.deepEqual(hits, []);
    // the demo's probes watch its own listener, by design
    assert.ok((await runProbe(db, lit, { cfg: { ...cfg, demo: true } })).ok); assert.deepEqual(hits, ['/']);
    // a redirect is recorded as the status it is; /inside is never asked for
    const b = upsertProbe(db, { name: 'bounce', url: base + '/bounce', expect_status: 200 }); const rb = await runProbe(db, b, { lookup: pub, cfg }); assert.ok(!rb.ok); assert.equal(rb.detail, '302'); assert.ok(!hits.includes('/inside'));
    const b2 = upsertProbe(db, { name: 'bounce-expected', url: base + '/bounce', expect_status: 302 }); assert.ok((await runProbe(db, b2, { lookup: pub, cfg })).ok);
    // a body past the budget is abandoned, so the text after three megabytes is never waited for
    const big = upsertProbe(db, { name: 'big', url: base + '/big', expect_text: 'SecuraCV' }); const rg = await runProbe(db, big, { lookup: pub, cfg }); assert.ok(!rg.ok); assert.match(rg.detail, /over 2097152 bytes/);
    assert.ok((await runProbe(db, big, { lookup: pub, cfg, maxBytes: 4 * 1024 * 1024 })).ok, 'under a larger budget the same page passes');
  } finally { srv.close(); db.close(); }
  for (const a of ['127.0.0.1', '10.1.1.1', '172.16.0.1', '192.168.0.1', '169.254.169.254', '100.64.0.1', '0.0.0.0', '::1', '::', '::ffff:127.0.0.1', '64:ff9b::7f00:1', 'fc00::1', 'fd12::1', 'fe80::1%eth0', 'fec0::1', 'not-an-address']) assert.ok(isPrivateAddress(a), a);
  for (const a of ['8.8.8.8', '203.0.113.9', '172.32.0.1', '::ffff:8.8.8.8', '64:ff9b::808:808', '2606:4700::1111', '[2606:4700::1111]']) assert.ok(!isPrivateAddress(a), a);
});

test('the inbox loop files each mail on its own: one the parser chokes on is written down as an exception and the rest of the batch still lands', async () => {
  const db = testDb(); const cfg = testCfg();
  const raw = (s, mid) => Buffer.from(`From: ada@example.test\r\nSubject: ${s}\r\nMessage-ID: <${mid}@x>\r\n\r\nhello\r\n`);
  const msgs = [{ uid: '7:1', raw: raw('first', 'a') }, { uid: '7:2', raw: raw('the poison one', 'b') }, { uid: '7:3', raw: raw('third', 'c') }, { uid: '7:4', raw: null, skipped: 'too big' }];
  const { ingestEmail, parseEmail } = await import('../src/modules/support.mjs');
  const r = await ingestInbox(db, cfg, msgs, { ingest: (b) => { if (b.includes('poison')) throw new TypeError('cannot read it'); return ingestEmail(db, parseEmail(b), { cfg }); } });
  assert.deepEqual(r, { fetched: 4, created: 2, appended: 0, ignored: 0, failed: 1 });
  assert.equal(db.get('SELECT COUNT(*) AS n FROM tickets').n, 2, 'the mails after the failure were filed');
  const ex = openExceptions(db).find(e => e.key === 'inbox:failed:7:2'); assert.ok(ex, 'the failure is an exception a person sees'); assert.match(ex.body, /Subject: the poison one/); assert.match(ex.body, /cannot read it/); assert.match(ex.body, /still in the mailbox/);
  assert.deepEqual(db.all('SELECT uid FROM mail_seen ORDER BY uid').map(x => x.uid), ['7:1', '7:2', '7:3'], 'the failed uid is remembered too, so it is never silently retried');
  assert.deepEqual(await ingestInbox(db, cfg, msgs), { fetched: 4, created: 0, appended: 0, ignored: 3 });
  db.close();
});
