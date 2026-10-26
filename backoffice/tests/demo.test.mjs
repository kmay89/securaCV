import { test } from 'node:test';
import assert from 'node:assert/strict';
import { seedDemo, resetDemo, SCENARIOS, DEMO_PASSWORD, DEMO_TOTP_SECRET, localBase } from '../src/modules/demo.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { trialBalance } from '../src/modules/ledger.mjs';
import { snapshot } from '../src/modules/dashboard.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp, createApiToken } from '../src/auth.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('the demo seed fills an empty database consistently, refuses a used one, and reset starts over', async () => {
  const db = testDb(); const cfg = testCfg();
  const r = await seedDemo(db, cfg);
  assert.ok(r.orders >= 40 && r.tickets >= 14 && r.contacts >= 20 && r.items === 10 && r.discounts === 5, JSON.stringify(r));
  assert.ok(trialBalance(db).balanced, 'the demo books balance');
  const s = snapshot(db);
  assert.ok(s.orders.open >= 2); assert.equal(s.support.breached, 1, 'one ticket is past its SLA on purpose'); assert.equal(s.exceptions.length, 1);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE status = \'sent\' AND created_at >= ?', new Date(Date.now() - 2 * 86400000).toISOString()).n, 0, 'nothing is sent by a seed — old history is marked sent, this week\'s mail waits in the outbox');
  assert.ok(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE status = \'queued\'').n > 0, 'this week\'s mail is queued');
  assert.equal(db.get('SELECT COUNT(*) AS n FROM orders WHERE id NOT IN (SELECT order_id FROM order_events)').n, 0, 'every order has a timeline');
  assert.ok(db.get('SELECT COUNT(DISTINCT batch) AS n FROM orders').n >= 8, 'orders span the weekly batches');
  assert.equal(db.setting('demo'), '1');
  await assert.rejects(seedDemo(db, cfg), /already has orders/);
  // every synthetic identity is unmistakably synthetic
  for (const c of db.all('SELECT email FROM contacts')) assert.match(c.email, /\.example\.test$/, c.email);
  for (const p of db.all('SELECT url FROM probes')) assert.ok(p.url.startsWith(cfg.baseUrl), `probe ${p.url} points at the app itself`);
  // a database that is neither empty nor a demo is never wiped
  const real = testDb(); recordOrder(real, { source: 'manual', email: 'a@b.test', total_cents: 100, subtotal_cents: 100, items: [{ sku: 'x', name: 'x', qty: 1, unit_cents: 100 }] });
  await assert.rejects(resetDemo(real, cfg), /refusing to reset/); assert.equal(real.get('SELECT COUNT(*) AS n FROM orders').n, 1);
  const r2 = await resetDemo(db, cfg);
  assert.equal(r2.orders, r.orders); assert.equal(db.get('SELECT number FROM orders ORDER BY id LIMIT 1').number, 'EL-000001', 'numbering restarts');
});

test('demo mode is a hard guard: no real service is reachable, and every scenario runs the real code path', async () => {
  const db = testDb();
  const cfg = testCfg({ githubToken: 'ghp_would_be_real', smtpUrl: 'smtp://u:p@relay.example.test:587', imapUrl: 'imaps://u:p@imap.example.test', stripeApiKey: 'sk_live_x', baseUrl: 'https://ops.real-deployment.example.test', siteOrigin: 'https://real-site.example.test' });
  // the seed points its probes at cfg.baseUrl, so the app listens first and the seed runs with the real port
  db.setSetting('demo', '1');
  const app = createApp({ db, cfg }); const addr = await app.listen();
  await seedDemo(db, cfg);
  try {
    assert.equal(cfg.baseUrl, `http://127.0.0.1:${addr.port}`, 'a real-looking BASE_URL is replaced by the loopback listener'); assert.equal(cfg.siteOrigin, cfg.baseUrl); assert.equal(localBase(cfg), cfg.baseUrl);
    assert.ok(cfg.demo); assert.equal(cfg.githubToken, ''); assert.equal(cfg.smtpUrl, ''); assert.equal(cfg.imapUrl, ''); assert.equal(cfg.stripeApiKey, '');
    const login = await (await fetch(cfg.baseUrl + '/login')).text();
    assert.ok(login.includes(DEMO_PASSWORD) && login.includes(totp(DEMO_TOTP_SECRET)), 'the login page hands over the demo credentials');
    assert.ok(login.includes('class="demo"'), 'the banner is on public pages too');
    const token = createApiToken(db, 't'); const h = { authorization: `Bearer ${token}` };
    const list = await (await fetch(cfg.baseUrl + '/api/demo/scenarios', { headers: h })).json();
    assert.deepEqual(Object.keys(list), Object.keys(SCENARIOS));
    const before = db.get('SELECT COUNT(*) AS n FROM orders').n;
    for (const name of Object.keys(SCENARIOS)) {
      const r = await fetch(cfg.baseUrl + `/api/demo/${name}`, { method: 'POST', headers: h });
      assert.equal(r.status, 200, name); const body = await r.json(); assert.ok(!body.error, `${name}: ${JSON.stringify(body)}`);
    }
    assert.equal(db.get('SELECT COUNT(*) AS n FROM orders').n, before + 1, 'the order scenario created one order through the signed webhook');
    assert.ok(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE status = \'sent\'').n > 0, 'the flush scenario marked mail sent without a socket');
    assert.equal(db.get('SELECT COUNT(*) AS n FROM exceptions WHERE key LIKE \'probe:%\' AND resolved_at IS NULL').n, 0, 'probe_down raised, probe_up resolved');
    assert.ok(trialBalance(db).balanced);
    // outside demo mode the scenario surface does not exist
    const db2 = testDb(); const cfg2 = testCfg(); const app2 = createApp({ db: db2, cfg: cfg2 }); const a2 = await app2.listen();
    assert.equal((await fetch(`http://127.0.0.1:${a2.port}/api/demo/scenarios`, { headers: { authorization: `Bearer ${createApiToken(db2, 'x')}` } })).status, 404);
    await app2.close(); db2.close();
  } finally { await app.close(); db.close(); }
});

test('a demo app on an empty database seeds itself; a timed public demo resets on its cadence', async () => {
  const { ensureDemo } = await import('../src/modules/demo.mjs');
  const { runJob } = await import('../src/jobs.mjs');
  const db = testDb(); const cfg = testCfg({ demo: true, demoResetMinutes: 60 });
  const app = createApp({ db, cfg }); await app.listen();
  try {
    const seeded = await ensureDemo(db, cfg);
    assert.ok(seeded && seeded.orders >= 40, 'seeded on first boot');
    assert.equal(await ensureDemo(db, cfg), null, 'a used database is left alone');
    let r = await runJob(app, 'demo_reset'); assert.ok(r.ok); assert.ok(r.result.next_in_s > 3000, 'just seeded: not due');
    db.setSetting('demo:seeded_at', new Date(Date.now() - 61 * 60_000).toISOString());
    r = await runJob(app, 'demo_reset'); assert.ok(r.ok); assert.equal(r.result.reset, true);
    r = await runJob(app, 'demo_reset'); assert.ok(r.result.next_in_s > 3000, 'a reset restarts the clock');
    const plain = createApp({ db: testDb(), cfg: testCfg() });
    assert.equal((await runJob(plain, 'demo_reset')).result.skipped, 'not a timed public demo');
  } finally { await app.close(); db.close(); }
});
