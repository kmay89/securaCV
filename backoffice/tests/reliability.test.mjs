import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';
import { backup, checkBackup, liveCounts, restore, signS3, uploadOffsite, restoreDrill, listBackups } from '../src/modules/backup.mjs';
import { reconcile, lastReconcile } from '../src/modules/reconcile.mjs';
import { doctor, digestText, todayLines } from '../src/modules/health.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { runJob } from '../src/jobs.mjs';
import { openDb } from '../src/db.mjs';
import { setupOperator, totp, createApiToken } from '../src/auth.mjs';
import { openExceptions } from '../src/modules/exceptions.mjs';

const tmp = () => mkdtempSync(join(tmpdir(), 'bo-rel-'));

test('a backup is opened and proven; a damaged copy fails; restore puts a checked copy in place and keeps the old file', async () => {
  const dir = tmp(); const cfg = testCfg({ dataDir: dir, dbPath: join(dir, 'live.db') });
  const db = openDb(cfg.dbPath); await seedDemo(db, cfg);
  const b = backup(db, join(dir, 'backups'), { keep: 3 });
  const v = checkBackup(b.path, { expect: liveCounts(db) });
  assert.ok(v.ok, v.errors.join('; ')); assert.equal(v.integrity, 'ok'); assert.ok(v.migrations >= 5); assert.ok(v.counts.orders >= 40); assert.ok(v.balanced); assert.ok(v.latest_at);
  // a copy that drifted from the live database is named, not waved through
  recordOrder(db, sampleOrder(), { catalog: catalog() });
  const v2 = checkBackup(b.path, { expect: liveCounts(db) }); assert.ok(!v2.ok); assert.match(v2.errors[0], /orders: copy has \d+, live has \d+/);
  // a damaged file fails loudly
  const bad = join(dir, 'backups', 'backoffice-2026-01-01T00-00-00.db'); writeFileSync(bad, Buffer.alloc(4096, 7));
  const v3 = checkBackup(bad); assert.ok(!v3.ok && v3.errors.length);
  assert.throws(() => restore(bad, join(dir, 'x.db')), /refusing to restore an unchecked copy/);
  // restore: the copy is in place, the old database kept beside it
  const target = join(dir, 'restored.db'); writeFileSync(target, 'old');
  const r = restore(b.path, target); assert.equal(r.restored, target); assert.ok(r.previous && statSync(r.previous).size === 3); assert.ok(r.check.ok);
  const again = openDb(target); assert.equal(again.get('SELECT COUNT(*) AS n FROM orders').n, v.counts.orders); again.close();
  // keep=N prunes
  for (let i = 0; i < 4; i++) backup(db, join(dir, 'backups'), { keep: 3, now: new Date(Date.now() + i * 1000 + 1000) });
  assert.equal(listBackups(join(dir, 'backups')).length, 3);
  db.close(); rmSync(dir, { recursive: true, force: true });
});

test('Signature V4 matches the AWS published example; upload PUTs, HEADs and writes the pointer; the drill pulls the latest copy back and proves it', async () => {
  const h = signS3({ method: 'GET', url: 'https://examplebucket.s3.amazonaws.com/test.txt', headers: { range: 'bytes=0-9' }, payloadHash: 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855', key: 'AKIAIOSFODNN7EXAMPLE', secret: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY', region: 'us-east-1', date: new Date('2013-05-24T00:00:00Z') });
  assert.equal(h.signature, 'f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41');
  assert.match(h.authorization, /^AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE\/20130524\/us-east-1\/s3\/aws4_request, SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, Signature=/);
  // a fake bucket: PUT stores, HEAD answers with the stored length, GET returns the bytes
  const store = new Map(); const calls = [];
  const fetchImpl = async (url, { method, headers, body }) => {
    const key = new URL(url).pathname; calls.push([method, key]);
    assert.match(headers.authorization, /^AWS4-HMAC-SHA256 Credential=k\/\d{8}\/auto\/s3\/aws4_request/); assert.equal(headers.host, 'files.example.test');
    if (method === 'PUT') { store.set(key, Buffer.from(body)); return { ok: true, status: 200, headers: new Headers({ etag: '"x"' }), text: async () => '' }; }
    if (method === 'HEAD') { const b = store.get(key); return { ok: !!b, status: b ? 200 : 404, headers: new Headers(b ? { 'content-length': String(b.length), etag: '"x"' } : {}) }; }
    if (method === 'GET') { const b = store.get(key); return { ok: !!b, status: b ? 200 : 404, text: async () => b.toString(), arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }; }
  };
  const dir = tmp(); const cfg = testCfg({ dataDir: dir, backupS3Url: 'https://files.example.test/bucket/backoffice', backupS3Key: 'k', backupS3Secret: 's', backupS3Region: 'auto' });
  const db = testDb(); await seedDemo(db, cfg);
  const b = backup(db, join(dir, 'backups'));
  const up = await uploadOffsite(cfg, b.path, { fetchImpl });
  assert.ok(up.read_back); assert.equal(up.bytes, statSync(b.path).size); assert.deepEqual(calls.map(c => c[0]), ['PUT', 'HEAD', 'PUT']); assert.equal(store.get('/bucket/backoffice/latest.txt').toString(), up.name);
  const drill = await restoreDrill(cfg, { fetchImpl });
  assert.ok(drill.ok, (drill.errors || []).join('; ')); assert.ok(drill.offsite); assert.equal(drill.counts.orders, db.get('SELECT COUNT(*) AS n FROM orders').n); assert.ok(drill.age_h < 1);
  // a stale off-site copy fails the drill
  const stale = await restoreDrill(cfg, { fetchImpl, now: new Date(Date.now() + 3 * 86400000) }); assert.ok(!stale.ok); assert.match(stale.errors.at(-1), /h old/);
  // a failing HEAD is an error, not a success
  const broken = async (url, o) => { const r = await fetchImpl(url, o); return o.method === 'HEAD' ? { ...r, headers: new Headers({ 'content-length': '1' }) } : r; };
  await assert.rejects(uploadOffsite(cfg, b.path, { fetchImpl: broken }), /size 1 ≠/);
  // without the bucket the drill proves the latest local copy instead
  const local = await restoreDrill(testCfg({ dataDir: dir }), { localDir: join(dir, 'backups') }); assert.ok(local.ok && !local.offsite);
  db.close(); rmSync(dir, { recursive: true, force: true });
});

test('the backup job checks, uploads, and raises/resolves exceptions; the drill job records; the digest goes out once a day', async () => {
  const dir = join(tmp(), 'a-rather-long-data-directory-name-so-the-backup-path-alone-fills-a-good-part-of-the-old-five-hundred-character-cap'); const store = new Map();
  const fetchImpl = async (url, { method, body }) => { const key = new URL(url).pathname; if (method === 'PUT') { store.set(key, Buffer.from(body)); return { ok: true, status: 200, headers: new Headers(), text: async () => '' }; } const b = store.get(key); if (method === 'HEAD') return { ok: !!b, status: b ? 200 : 404, headers: new Headers(b ? { 'content-length': String(b.length) } : {}) }; return { ok: !!b, status: b ? 200 : 404, text: async () => b.toString(), arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }; };
  const cfg = testCfg({ dataDir: dir, backupS3Url: 'https://files.example.test/b', backupS3Key: 'k', backupS3Secret: 's', operatorEmail: 'owner@errer.test', digestHour: 0 });
  const db = testDb(); await seedDemo(db, cfg); const app = createApp({ db, cfg }); app.fetchImpl = fetchImpl;
  let r = await runJob(app, 'backup'); assert.ok(r.ok, r.error); assert.ok(r.result.checked.ok && r.result.offsite.read_back);
  assert.ok(db.get('SELECT id FROM audit_log WHERE action = \'backup.taken\''));
  r = await runJob(app, 'restore_drill'); assert.ok(r.ok, r.error); assert.ok(r.result.ok && r.result.offsite);
  // the doctor reads the compact records, so a long path or a big result never breaks the checks
  const by = Object.fromEntries(doctor(db, cfg).checks.map(c => [c.key, c])); assert.equal(by.backup.ok, true, by.backup.detail); assert.equal(by.offsite.ok, true, by.offsite.detail); assert.equal(by.drill.ok, true, by.drill.detail); assert.ok(db.get('SELECT last_detail FROM job_runs WHERE name = \'backup\'').last_detail.length > 500, 'the page detail is longer than the old cap and is not what the doctor parses');
  // the bucket goes away: the local backup still checks out, the off-site exception opens, and closes again when it is back
  app.fetchImpl = async () => ({ ok: false, status: 403, headers: new Headers(), text: async () => 'denied' });
  r = await runJob(app, 'backup'); assert.ok(r.ok && r.result.checked.ok && r.result.offsite.error);
  assert.ok(openExceptions(db).some(x => x.key === 'backup:offsite'));
  app.fetchImpl = fetchImpl; r = await runJob(app, 'backup'); assert.ok(r.result.offsite.read_back); assert.ok(!openExceptions(db).some(x => x.key === 'backup:offsite'));
  // the digest: once per day, to the operator, with today's list and the checklist
  r = await runJob(app, 'digest'); assert.ok(r.ok && r.result.queued, JSON.stringify(r));
  const m = db.get('SELECT * FROM mail_outbox WHERE ref LIKE \'digest:%\''); assert.equal(m.to_addr, 'owner@errer.test'); assert.match(m.body_text, /TODAY[\s\S]*LAST 30 DAYS[\s\S]*HEALTH/); assert.match(m.body_text, /orders? to pack or ship/);
  r = await runJob(app, 'digest'); assert.equal(r.result.sent, 'already today');
  assert.equal((await runJob(createApp({ db: testDb(), cfg: testCfg({ operatorEmail: '' }) }), 'digest')).result.skipped, 'OPERATOR_EMAIL not set');
  await app.close(); db.close(); rmSync(dir, { recursive: true, force: true });
});

test('reconciliation: Stripe payouts land in the books once, a charge without an order raises, and resolves when the order appears', async () => {
  const db = testDb(); const cfg = testCfg({ stripeApiKey: 'sk_test_x' }); const cat = catalog();
  const { order } = recordOrder(db, sampleOrder({ payment_intent: 'pi_known' }), { catalog: cat });
  const stripe = { payouts: [{ id: 'po_1', amount: 12345, arrival_date: Math.floor(Date.now() / 1000) - 86400 }], charges: [{ id: 'ch_1', status: 'succeeded', paid: true, refunded: false, payment_intent: 'pi_known', amount: 7700, created: Math.floor(Date.now() / 1000) - 3600 }, { id: 'ch_2', status: 'succeeded', paid: true, refunded: false, payment_intent: 'pi_missing', amount: 11900, created: Math.floor(Date.now() / 1000) - 7200, billing_details: { email: 'who@example.test' } }, { id: 'ch_3', status: 'failed', paid: false, refunded: false, payment_intent: 'pi_failed', amount: 1, created: Math.floor(Date.now() / 1000) }] };
  stripe.balance_transactions = [{ id: 'txn_po', type: 'payout', amount: -12345, fee: 0 }, { id: 'txn_1', type: 'charge', amount: 7700, fee: 253 }, { id: 'txn_2', type: 'charge', amount: 5000, fee: 175 }];
  const fetchImpl = async (url, { headers }) => { assert.equal(headers.authorization, 'Bearer sk_test_x'); const kind = url.includes('/payouts') ? 'payouts' : url.includes('/balance_transactions') ? 'balance_transactions' : 'charges'; if (kind === 'balance_transactions') assert.match(url, /payout=po_1/); return { ok: true, json: async () => ({ data: stripe[kind], has_more: false }) }; };
  let r = await reconcile(db, cfg, { fetchImpl });
  assert.equal(r.payouts_posted, 1); assert.equal(r.charges_seen, 2, 'the failed charge is not counted'); assert.equal(r.orders_missing, 1); assert.equal(r.missing[0].payment_intent, 'pi_missing');
  assert.ok(db.get('SELECT id FROM ledger WHERE ref = \'payout:po_1\'')); assert.ok(openExceptions(db).some(x => x.key === 'reconcile:orders' && /pi_missing/.test(x.body)));
  r = await reconcile(db, cfg, { fetchImpl }); assert.equal(r.payouts_posted, 0, 'posted once, not twice'); assert.equal(db.get('SELECT COUNT(*) AS n FROM ledger WHERE ref = \'payout:po_1\'').n, 3, 'bank, fees and Stripe legs, one transaction');
  assert.equal(db.get('SELECT debit_cents FROM ledger WHERE ref = \'payout:po_1\' AND account = \'expenses:fees\'').debit_cents, 428, 'the fees Stripe kept, from the balance transactions');
  assert.equal(db.get('SELECT credit_cents FROM ledger WHERE ref = \'payout:po_1\' AND account = \'assets:stripe\'').credit_cents, 12345 + 428, 'Stripe is relieved of net plus fees, so nothing is stranded');
  recordOrder(db, sampleOrder({ external_id: 'cs_2', payment_intent: 'pi_missing' }), { catalog: cat });
  r = await reconcile(db, cfg, { fetchImpl }); assert.equal(r.orders_missing, 0); assert.ok(!openExceptions(db).some(x => x.key === 'reconcile:orders'));
  assert.equal(lastReconcile(db).orders_missing, 0);
  assert.equal((await reconcile(db, testCfg())).skipped.startsWith('STRIPE_API_KEY'), true);
  assert.ok(order);
});

test('the doctor names what is missing and what to do; the health and audit pages and the benches work over HTTP', async () => {
  const dir = tmp(); const cfg = testCfg({ dataDir: dir, secure: false, smtpUrl: '', operatorEmail: '' }); const db = testDb(); await seedDemo(db, cfg);
  const h = doctor(db, cfg);
  const by = Object.fromEntries(h.checks.map(c => [c.key, c]));
  assert.equal(by.integrity.ok, true); assert.equal(by.books.ok, true); assert.equal(by.backup.ok, false); assert.equal(by.offsite.ok, false); assert.equal(by.drill.ok, false); assert.equal(by.external.ok, false); assert.equal(by.exceptions.ok, false);
  assert.match(by.offsite.fix, /BACKUP_S3_URL/); assert.match(by.operator_email.fix, /OPERATOR_EMAIL/); assert.ok(!h.ok && h.failing.includes('backup'));
  assert.ok(todayLines(db, cfg).some(l => /to pack or ship/.test(l.what) && l.href === '/pack'));
  assert.match(digestText(db, cfg), /HEALTH — \d+ failing/);
  const totpSecret = setupOperator(db, 'reliability-test-pw-1'); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const login = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ email: 'owner@localhost', password: 'reliability-test-pw-1', code: totp(totpSecret) }), redirect: 'manual' });
    const cookie = login.headers.get('set-cookie').split(';')[0]; const get = (p) => fetch(base + p, { headers: { cookie } });
    for (const p of ['/', '/health', '/audit', '/audit?actor=web&action=order.&from=2020-01-01&to=2030-12-31', '/audit.csv', '/pack', '/ship', '/pack?batch=2020-W01', '/ship?batch=2020-W01']) { const r = await get(p); assert.equal(r.status, 200, p); }
    const home = await (await get('/')).text(); assert.ok(home.includes('class="hp') && home.includes('class="act') && home.includes('Pack bench') && !home.includes('<h1>Home</h1>'), 'the home page has the health pill and action cards');
    const health = await (await get('/health')).text(); assert.ok(health.includes('Back up now') && health.includes('✗'));
    const token = createApiToken(db, 't'); const api = (p, o = {}) => fetch(base + p, { ...o, headers: { authorization: `Bearer ${token}`, ...(o.headers || {}) } });
    assert.equal((await api('/api/health')).status, 503, 'unhealthy answers 503 so a monitor can see it');
    const made = await (await api('/api/backups', { method: 'POST' })).json(); assert.ok(made.ok && made.result.checked.ok);
    const bk = await (await api('/api/backups')).json(); assert.equal(bk.backups.length, 1); assert.ok(bk.live.orders > 0);
    const au = await (await api('/api/audit?limit=5')).json(); assert.equal(au.length, 5);
    // the pack bench walks the batch: each "Packed — next" pulls parts and advances
    const packHtml = await (await get('/pack')).text(); const idm = packHtml.match(/action="\/orders\/(\d+)\/transition"/); assert.ok(idm, 'the bench shows an order to pack');
    const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const r = await fetch(`${base}/orders/${idm[1]}/transition`, { method: 'POST', headers: { cookie }, body: new URLSearchParams({ _csrf: csrf, to: 'packed', _back: '/pack?batch=x' }), redirect: 'manual' });
    assert.equal(r.status, 303); assert.match(r.headers.get('location'), /^\/pack\?batch=x&ok=/, 'the bench keeps its query and gains the flash'); assert.equal(db.get('SELECT status FROM orders WHERE id = ?', idm[1]).status, 'packed');
    const evil = await fetch(`${base}/orders/${idm[1]}/transition`, { method: 'POST', headers: { cookie }, body: new URLSearchParams({ _csrf: csrf, to: 'shipped', tracking: 't', _back: 'https://evil.example.test/x' }), redirect: 'manual' });
    assert.match(evil.headers.get('location'), /^\/orders\/\d+\?ok=/, 'an off-site _back is ignored');
  } finally { await app.close(); db.close(); rmSync(dir, { recursive: true, force: true }); }
});
