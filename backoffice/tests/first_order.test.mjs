// The first order, from nothing, end to end — the walkthrough the demo page
// runs a step at a time, run here in one go with every check held. This is
// the harness for new flows: add a step to src/modules/walkthrough.mjs with
// what must be true afterwards, and it runs here, on the demo page, and from
// the shell. A step that throws stops the chain; the failure names the step.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { STEPS, startEmpty, runAll, runStep, walkthroughState, nextStep, FIRST } from '../src/modules/walkthrough.mjs';
import { seedDemo, isDemo, DEMO_USERS, DEMO_PASSWORD, DEMO_TOTP_SECRET } from '../src/modules/demo.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { trialBalance } from '../src/modules/ledger.mjs';
import { createApp } from '../src/server.mjs';
import { totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

test('from an empty database, the first customer walks the whole chain and every check holds', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'first-order-')); const cfg = testCfg({ demo: true, dataDir: dir }); const db = testDb();
  const emptied = startEmpty(db); assert.deepEqual(emptied, { orders: 0, contacts: 0, users: DEMO_USERS.length }); assert.ok(isDemo(db));
  const app = createApp({ db, cfg }); await app.listen();
  try {
    const results = await runAll(app);
    assert.equal(results.length, STEPS.length, 'every step ran');
    for (const r of results) {
      assert.ok(!r.error, `${r.id} threw: ${r.error}`);
      for (const ch of r.checks) assert.ok(ch.ok, `${r.id}: ${ch.label}${ch.detail ? ` — ${ch.detail}` : ''}`);
    }
    assert.equal(nextStep(db), null); assert.equal(walkthroughState(db).done.length, STEPS.length);
    // what the chain leaves behind, read back from the database rather than the steps' own reports
    assert.equal(db.get('SELECT COUNT(*) n FROM orders').n, 2, 'the sale and the warranty replacement');
    assert.deepEqual(db.all('SELECT number, status, total_cents FROM orders ORDER BY id').map(o => [o.number, o.status, o.total_cents > 0]), [['EL-000001', 'delivered', true], ['EL-000002', 'shipped', false]]);
    assert.equal(db.get('SELECT COUNT(*) n FROM contacts').n, 1, 'one customer'); assert.equal(db.get('SELECT email FROM contacts').email, FIRST.email);
    assert.deepEqual(db.all("SELECT serial, status, order_id IS NOT NULL AS boxed FROM devices ORDER BY serial").map(d => [d.serial, d.status, !!d.boxed]), [['FIRST-W-0001', 'shipped', true], ['FIRST-W-0002', 'shipped', true], ['FIRST-W-0003', 'stock', false]]);
    assert.ok(trialBalance(db).balanced);
    assert.equal(db.get("SELECT COUNT(*) n FROM mail_outbox WHERE status = 'queued'").n, 0, 'nothing left in the outbox after the week\'s jobs');
    assert.ok(db.get("SELECT COUNT(*) n FROM audit_log WHERE action LIKE 'order.%'").n >= 6);
    // the admin shows it: the order page, the claim, the customer
    const login = await fetch(`http://127.0.0.1:${cfg.port}/login`, { method: 'POST', body: new URLSearchParams({ email: DEMO_USERS[0].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), redirect: 'manual' });
    const cookie = login.headers.get('set-cookie').split(';')[0]; const get = (p) => fetch(`http://127.0.0.1:${cfg.port}${p}`, { headers: { cookie } });
    const o = db.get("SELECT id FROM orders WHERE number = 'EL-000001'"); const html = await (await get(`/orders/${o.id}`)).text();
    assert.ok(html.includes('Delivered') && html.includes('FIRST-W-0001') && html.includes('every line ticked'), 'the order page tells the story');
    const wt = await (await get('/demo/first-order')).text(); assert.equal((wt.match(/class="wt ok"/g) || []).length, STEPS.length, 'the walkthrough page shows every step green');
  } finally { await app.close(); db.close(); rmSync(dir, { recursive: true, force: true }); }
});

test('the walkthrough is a step at a time, remembers where it is, and starting over empties a full demo but never a real database', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'first-order-')); const cfg = testCfg({ demo: true, dataDir: dir }); const db = testDb(); await seedDemo(db, cfg);
  assert.ok(db.get('SELECT COUNT(*) n FROM orders').n > 10, 'the full demo is in');
  const app = createApp({ db, cfg }); await app.listen();
  try {
    startEmpty(db); assert.equal(db.get('SELECT COUNT(*) n FROM orders').n, 0); assert.equal(db.get("SELECT COUNT(*) n FROM users WHERE status = 'active'").n, DEMO_USERS.length);
    assert.equal(nextStep(db).id, 'day_zero');
    const r1 = await runStep(app, 'day_zero'); assert.ok(r1.ok); assert.equal(nextStep(db).id, 'shelf');
    const r2 = await runStep(app, 'shelf'); assert.ok(r2.ok, JSON.stringify(r2.checks)); assert.equal(walkthroughState(db).state.po, 'PO-000001');
    assert.equal(nextStep(db).id, 'boards'); assert.equal(walkthroughState(db).log.length, 2);
    // an unknown step is refused; a step that throws is a failed step, not a crash, and the chain stops there
    await assert.rejects(() => runStep(app, 'nope'), /no step nope/);
    const bad = await runStep(app, 'pack'); assert.ok(!bad.ok && bad.error, 'packing before there is an order fails in the log');
    assert.equal(walkthroughState(db).log.at(-1).id, 'pack');
  } finally { await app.close(); db.close(); rmSync(dir, { recursive: true, force: true }); }
  // a database that is neither a demo nor empty is never emptied
  const real = testDb(); recordOrder(real, sampleOrder(), { catalog: catalog() });
  assert.throws(() => startEmpty(real), /refusing to empty/); assert.equal(real.get('SELECT COUNT(*) n FROM orders').n, 1); real.close();
});
