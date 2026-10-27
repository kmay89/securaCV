// The workflow review: what a day's orders can do to the shop when they arrive in the wrong order, past the
// shelf, charged back, or packed from the wrong place — and what the books, the bench and the devices say after.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { execFile, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createApp, applyStripeEvent } from '../src/server.mjs';
import { eventToOrder, signStripePayload } from '../src/modules/stripe.mjs';
import { recordOrder, transition, refund, timeline, orderById } from '../src/modules/orders.mjs';
import { registerDevice, assignToOrder } from '../src/modules/devices.mjs';
import { upsertItem, move, setRecipe, item } from '../src/modules/inventory.mjs';
import { salesTaxByState, refundTaxShare } from '../src/modules/ledger.mjs';
import { openTicket, reply, setStatus, addMessage } from '../src/modules/support.mjs';
import { createDiscount, evaluate } from '../src/modules/discounts.mjs';
import { reconcile } from '../src/modules/reconcile.mjs';
import { backup } from '../src/modules/backup.mjs';
import { openDb } from '../src/db.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const CLI = fileURLToPath(new URL('../bin/backoffice.mjs', import.meta.url));
const stripeOk = (data) => ({ ok: true, status: 200, json: async () => ({ data, has_more: false }) });
const ev = (type, object, id = `evt_${type.replace(/\W/g, '_')}_${Math.random().toString(36).slice(2, 8)}`) => ({ id, type, data: { object } });
const session = (o = {}) => ({ id: 'cs_w1', payment_intent: 'pi_w1', mode: 'payment', payment_status: 'paid', currency: 'usd', amount_subtotal: 6900, amount_total: 7700, total_details: { amount_shipping: 800, amount_tax: 0 }, customer_details: { email: 'w@example.test', name: 'W' }, shipping_details: { name: 'W', address: { line1: '1 W', city: 'B', state: 'MA', postal_code: '02134', country: 'US' } }, line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] }, ...o });

test('a Checkout in subscription or setup mode is never a shop order; a payment session is', () => {
  assert.equal(eventToOrder(ev('checkout.session.completed', session({ mode: 'subscription' }))).kind, 'ignore');
  assert.equal(eventToOrder(ev('checkout.session.completed', session({ mode: 'setup', payment_status: 'no_payment_required' }))).kind, 'ignore');
  assert.equal(eventToOrder(ev('checkout.session.completed', session())).kind, 'order');
  assert.equal(eventToOrder(ev('checkout.session.completed', session({ mode: undefined }))).kind, 'order', 'an old event without a mode is a Payment Link');
});

test('a refund that arrives before its order is refused so Stripe retries it, and applies once the order is here', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg });
  try {
    const refundEv = ev('charge.refunded', { id: 'ch_w1', payment_intent: 'pi_w1', amount_refunded: 7700 });
    await assert.rejects(applyStripeEvent(app, eventToOrder(refundEv), refundEv), /no order carries that PaymentIntent yet/);
    const orderEv = ev('checkout.session.completed', session());
    assert.equal((await applyStripeEvent(app, eventToOrder(orderEv), orderEv)).created, true);
    const r = await applyStripeEvent(app, eventToOrder(refundEv), refundEv);
    assert.equal(r.refunded_cents, 7700); assert.equal(orderById(db, 1).status, 'refunded');
  } finally { await app.close(); }
});

test('a chargeback tags the order and raises an exception with the evidence date; a lost one is a refund; a closed one resolves', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg });
  try {
    const { order } = recordOrder(db, sampleOrder({ payment_intent: 'pi_d1', external_id: 'cs_d1' }), { catalog: catalog() });
    const created = ev('charge.dispute.created', { id: 'dp_1', charge: 'ch_d1', payment_intent: 'pi_d1', amount: 7700, reason: 'fraudulent', status: 'needs_response', evidence_details: { due_by: Math.floor(Date.parse('2026-11-05T00:00:00Z') / 1000) } });
    const r1 = await applyStripeEvent(app, eventToOrder(created), created);
    assert.equal(r1.order, order.number);
    assert.match(orderById(db, order.id).tags, /disputed/);
    const ex = db.get('SELECT * FROM exceptions WHERE key = ?', `dispute:${order.number}`); assert.ok(ex && !ex.resolved_at); assert.match(ex.body, /due 2026-11-05/);
    assert.match(timeline(db, order.id)[0].text, /Disputed — fraudulent · 77.00 USD · evidence due 2026-11-05/);
    const lost = ev('charge.dispute.closed', { id: 'dp_1', charge: 'ch_d1', payment_intent: 'pi_d1', amount: 7700, status: 'lost' });
    await applyStripeEvent(app, eventToOrder(lost), lost);
    const after = orderById(db, order.id); assert.equal(after.refunded_cents, 7700); assert.equal(after.status, 'refunded');
    assert.ok(db.get('SELECT resolved_at FROM exceptions WHERE key = ?', `dispute:${order.number}`).resolved_at, 'the exception closes with the dispute');
    assert.match(timeline(db, order.id).map(t => t.text).join('\n'), /Dispute lost — 77.00 USD taken back/);
    const unknown = ev('charge.dispute.created', { id: 'dp_2', charge: 'ch_nope', payment_intent: 'pi_nope', amount: 100 });
    assert.equal((await applyStripeEvent(app, eventToOrder(unknown), unknown)).ignored, 'dispute on an unknown charge');
  } finally { await app.close(); }
});

test('a payout from the webhook carries the fee Stripe kept when an API key can read it; reconcile does not post it twice', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', stripeApiKey: 'sk_test_x' }); const app = createApp({ db, cfg });
  const calls = [];
  app.fetchImpl = async (url) => { calls.push(url); if (url.includes('balance_transactions')) return stripeOk([{ id: 'txn_1', type: 'charge', fee: 254 }, { id: 'txn_2', type: 'charge', fee: 230 }, { id: 'txn_p', type: 'payout', fee: 0 }]); if (url.includes('/payouts')) return stripeOk([{ id: 'po_1', amount: 18450, arrival_date: Math.floor(Date.now() / 1000) }]); return stripeOk([]); };
  try {
    const payout = ev('payout.paid', { id: 'po_1', amount: 18450, arrival_date: Math.floor(Date.now() / 1000) });
    const r = await applyStripeEvent(app, eventToOrder(payout), payout);
    assert.deepEqual([r.posted, r.fee_cents], [true, 484]);
    assert.equal(db.get("SELECT SUM(debit_cents) AS c FROM ledger WHERE account = 'expenses:fees' AND ref = 'payout:po_1'").c, 484);
    assert.equal(db.get("SELECT SUM(credit_cents) AS c FROM ledger WHERE account = 'assets:stripe' AND ref = 'payout:po_1'").c, 18934, 'gross off the Stripe balance');
    const rc = await reconcile(db, cfg, { fetchImpl: app.fetchImpl });
    assert.equal(rc.payouts_posted, 0, 'already in the books');
    assert.equal(db.get("SELECT COUNT(*) AS n FROM ledger WHERE ref = 'payout:po_1'").n, 3);
    // without a key the webhook posts the net and no fee, and never calls out
    const quiet = createApp({ db, cfg: testCfg({ secret: 's' }) }); quiet.fetchImpl = async () => { throw new Error('must not be called'); };
    const p2 = ev('payout.paid', { id: 'po_2', amount: 100, arrival_date: Math.floor(Date.now() / 1000) });
    assert.equal((await applyStripeEvent(quiet, eventToOrder(p2), p2)).fee_cents, 0); await quiet.close();
  } finally { await app.close(); }
});

test('reconcile ignores a charge on an invoice (a subscription payment), and a shop charge without an order is reported', async () => {
  const db = testDb(); const cfg = testCfg({ stripeApiKey: 'sk_test_x' });
  const fetchImpl = async (url) => url.includes('/charges') ? stripeOk([{ id: 'ch_sub', status: 'succeeded', paid: true, invoice: 'in_1', payment_intent: 'pi_sub', amount: 500, created: Date.now() / 1000 }, { id: 'ch_shop', status: 'succeeded', paid: true, payment_intent: 'pi_shop', amount: 7700, created: Date.now() / 1000 }]) : stripeOk([]);
  const r = await reconcile(db, cfg, { fetchImpl });
  assert.deepEqual(r.missing.map(m => m.payment_intent), ['pi_shop']);
});

test('sold past the shelf: the order stands, is tagged oversold, says so on its timeline and raises an exception', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg });
  try {
    upsertItem(db, { sku: 'esp32', name: 'board' }); setRecipe(db, 'wap-kit', { esp32: 1 });   // nothing on the shelf: zero buildable
    const e1 = ev('checkout.session.completed', session());
    const r = await applyStripeEvent(app, eventToOrder(e1), e1); assert.equal(r.created, true);
    const o = orderById(db, 1); assert.match(o.tags, /oversold/); assert.equal(o.status, 'paid', 'the customer paid; the order is not refused');
    assert.match(timeline(db, o.id)[0].text, /Oversold — wap-kit: 1 short \(1 paid and waiting, parts on the shelf for 0\)/);
    const ex = db.get('SELECT * FROM exceptions WHERE key = ?', `oversold:${o.number}`); assert.ok(ex); assert.match(ex.body, /refund it in Stripe/);
    // with parts on the shelf the next order is fine
    move(db, 'esp32', 5, 'receive');
    const e2 = ev('checkout.session.completed', session({ id: 'cs_w2', payment_intent: 'pi_w2' }));
    await applyStripeEvent(app, eventToOrder(e2), e2); assert.ok(!/oversold/.test(orderById(db, 2).tags || ''));
  } finally { await app.close(); }
});

test('packing is one order at a time: the bulk menu refuses it, and a pack from the API without the bench check says so on the timeline', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    const { order } = recordOrder(db, sampleOrder(), { catalog: catalog() });
    const bulk = await fetch(base + '/orders/bulk', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, 'ids[]': String(order.id), action: 'packed' }) });
    assert.equal(bulk.status, 400); assert.match((await bulk.json()).error, /one at a time at the Pack bench/);
    assert.equal(orderById(db, order.id).status, 'paid');
    const list = await (await fetch(base + '/orders?status=paid', { headers: { cookie, accept: 'text/html' } })).text();
    assert.ok(!/<option value="packed"/.test(list), 'the bulk menu does not offer it');
    transition(db, order.id, 'packed', { actor: 'api' });
    assert.match(timeline(db, order.id)[0].text, /bench check not run/);
    // the audit log names the person for a stock move, an item, a recipe
    for (const [path, body] of [['/inventory/item', { sku: 'esp32', name: 'board', reorder_point: 2, reorder_qty: 10, unit_cost_cents: 300 }], ['/inventory/move', { sku: 'esp32', delta: 4, reason: 'receive', ref: 'PO-1' }], ['/inventory/recipe', { finished_sku: 'wap-kit', parts: 'esp32=1' }]]) {
      const w = await fetch(base + path, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, ...body }) }); assert.equal(w.status, 303, path);
    }
    const rows = db.all("SELECT actor, action, entity_id FROM audit_log WHERE action LIKE 'inventory.%' ORDER BY id").map(r => ({ ...r }));
    assert.deepEqual(rows, [{ actor: 'o@errer.test', action: 'inventory.item', entity_id: 'esp32' }, { actor: 'o@errer.test', action: 'inventory.move', entity_id: 'esp32' }, { actor: 'o@errer.test', action: 'inventory.recipe', entity_id: 'wap-kit' }]);
    assert.equal(item(db, 'esp32').on_hand, 4);
  } finally { await app.close(); }
});

test('a canceled or fully refunded order lets its boards go; a shipped one keeps them; cancel after packing returns the parts', () => {
  const db = testDb(); const cat = catalog();
  upsertItem(db, { sku: 'esp32', name: 'board' }); move(db, 'esp32', 10, 'receive'); setRecipe(db, 'wap-kit', { esp32: 1 });
  for (const sn of ['A1', 'A2', 'A3']) registerDevice(db, { serial: sn, product_sku: 'wap-kit' });
  const a = recordOrder(db, sampleOrder({ external_id: 'cs_a' }), { catalog: cat }).order; assignToOrder(db, 'A1', a.id);
  transition(db, a.id, 'canceled');
  assert.equal(db.get("SELECT order_id FROM devices WHERE serial = 'A1'").order_id, null, 'canceled: the board is free again');
  assert.match(timeline(db, a.id).map(t => t.text).join('\n'), /1 board released back to stock/);
  const b = recordOrder(db, sampleOrder({ external_id: 'cs_b' }), { catalog: cat }).order; assignToOrder(db, 'A2', b.id);
  refund(db, b.id, 7700);
  assert.equal(db.get("SELECT order_id FROM devices WHERE serial = 'A2'").order_id, null, 'refunded in full before shipping: free again');
  const c = recordOrder(db, sampleOrder({ external_id: 'cs_c' }), { catalog: cat }).order; assignToOrder(db, 'A3', c.id);
  transition(db, c.id, 'packed', { check: { ticked: ['wap-kit'] } }); transition(db, c.id, 'shipped', { tracking: '9400111111111111111111' }); refund(db, c.id, 7700);
  assert.equal(db.get("SELECT order_id, status FROM devices WHERE serial = 'A3'").order_id, c.id, 'shipped: the record keeps the board');
  // packed, then canceled: the parts go back on the shelf in the same move
  const d = recordOrder(db, sampleOrder({ external_id: 'cs_d' }), { catalog: cat }).order;
  transition(db, d.id, 'packed', { actor: 'api' }); assert.equal(item(db, 'esp32').on_hand, 8);
  transition(db, d.id, 'canceled'); assert.equal(item(db, 'esp32').on_hand, 9); assert.equal(orderById(db, d.id).status, 'canceled');
  assert.match(timeline(db, d.id).map(t => t.text).join('\n'), /Canceled after packing — parts returned to stock: 1× esp32/);
});

test('a refund gives the tax back too: the ledger reverses its share off the liability, and the sales-tax table is net of it', () => {
  const db = testDb(); const cat = catalog();
  const o = recordOrder(db, sampleOrder({ tax_cents: 500, total_cents: 8200, paid_at: '2026-10-19T12:00:00.000Z' }), { catalog: cat }).order;
  assert.equal(refundTaxShare(o, 4100), 250); assert.equal(refundTaxShare(o, 8200), 500); assert.equal(refundTaxShare({ total_cents: 100, tax_cents: 0 }, 50), 0);
  refund(db, o.id, 4100, { at: '2026-10-20T12:00:00.000Z' });
  const legs = db.all("SELECT account, debit_cents, credit_cents FROM ledger WHERE source = 'refund' ORDER BY account").map(r => ({ ...r }));
  assert.deepEqual(legs, [{ account: 'assets:stripe', debit_cents: 0, credit_cents: 4100 }, { account: 'income:refunds', debit_cents: 3850, credit_cents: 0 }, { account: 'liabilities:sales_tax', debit_cents: 250, credit_cents: 0 }]);
  const tax = salesTaxByState(db, { from: '2026-10-01', to: '2026-10-31' }).map(r => ({ ...r }));
  assert.deepEqual(tax, [{ state: 'MA', orders: 1, taxable_cents: 3450, tax_cents: 250 }]);
  refund(db, o.id, 4100, { at: '2026-10-21T12:00:00.000Z' });
  assert.equal(db.get("SELECT SUM(debit_cents) AS c FROM ledger WHERE account = 'liabilities:sales_tax'").c, 500, 'a full refund reverses all of it');
  assert.deepEqual(salesTaxByState(db, { from: '2026-10-01', to: '2026-10-31' }).map(r => ({ ...r })), [{ state: 'MA', orders: 1, taxable_cents: 0, tax_cents: 0 }]);
});

test('a customer writing to a closed ticket reopens it as a new question: the SLA clock and the first-response mark start over, at the configured hours', () => {
  const db = testDb(); const cfg = testCfg({ slaHours: 48 });
  const t = openTicket(db, { email: 'q@example.test', subject: 'hi', body: 'help', cfg });
  reply(db, t.id, 'done', { cfg }); setStatus(db, t.id, 'closed');
  const before = db.get('SELECT first_response_at, status FROM tickets WHERE id = ?', t.id); assert.ok(before.first_response_at); assert.equal(before.status, 'closed');
  addMessage(db, t.id, { direction: 'in', body: 'it broke again', slaHours: cfg.slaHours });
  const after = db.get('SELECT * FROM tickets WHERE id = ?', t.id);
  assert.equal(after.status, 'open'); assert.equal(after.first_response_at, null); assert.equal(after.closed_at, null);
  const dueH = (Date.parse(after.sla_due_at) - Date.now()) / 3600_000; assert.ok(dueH > 47.9 && dueH <= 48, `due in ${dueH}h`);
  // a reply while we wait on them resets the clock only
  reply(db, t.id, 'on it', { cfg }); addMessage(db, t.id, { direction: 'in', body: 'thanks', slaHours: 48 });
  const again = db.get('SELECT first_response_at, status FROM tickets WHERE id = ?', t.id); assert.ok(again.first_response_at); assert.equal(again.status, 'waiting_on_us');
});

test('a discount window given as dates is those whole days where the operator is', () => {
  const db = testDb();
  createDiscount(db, { code: 'FALL', kind: 'percent', value: 10, starts_at: '2026-10-20', ends_at: '2026-10-21' });
  const late21Boston = '2026-10-22T03:30:00.000Z';   // 23:30 on the 21st in Boston
  assert.equal(evaluate(db, 'FALL', { subtotal_cents: 10000, at: late21Boston, tz: 'America/New_York' }).ok, true, 'still the 21st in Boston');
  assert.match(evaluate(db, 'FALL', { subtotal_cents: 10000, at: late21Boston, tz: 'UTC' }).reason, /ended 2026-10-21/);
  assert.match(evaluate(db, 'FALL', { subtotal_cents: 10000, at: '2026-10-20T02:00:00.000Z', tz: 'America/New_York' }).reason, /starts 2026-10-20/, 'still the 19th in Boston');
  createDiscount(db, { code: 'HOUR', kind: 'percent', value: 10, ends_at: '2026-10-21T15:00:00.000Z' });
  assert.equal(evaluate(db, 'HOUR', { subtotal_cents: 10000, at: '2026-10-21T14:59:00.000Z', tz: 'America/New_York' }).ok, true);
  assert.equal(evaluate(db, 'HOUR', { subtotal_cents: 10000, at: '2026-10-21T15:01:00.000Z', tz: 'America/New_York' }).ok, false, 'a window with a time is that instant');
});

test('restore refuses while serve is running in this process space, and --force overrides a stale pid file', async () => {
  const work = mkdtempSync(join(tmpdir(), 'bo-pid-'));
  const child = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { stdio: 'ignore' });
  try {
    const src = openDb(join(work, 'src.db')); src.setSetting('marker', 'from-backup'); const b = backup(src, join(work, 'backups')); src.close();
    const dataDir = join(work, 'live'); const env = { ...process.env, BACKOFFICE_DATA_DIR: dataDir }; delete env.BACKOFFICE_DEMO;
    const run = (...a) => new Promise(resolve => execFile(process.execPath, ['--disable-warning=ExperimentalWarning', CLI, 'restore', ...a], { env, encoding: 'utf8', timeout: 60_000 }, (err, stdout, stderr) => resolve({ status: err ? (err.code ?? 1) : 0, stdout, stderr })));
    await run(b.path); writeFileSync(join(dataDir, 'backoffice.pid'), String(child.pid));   // the first run creates the data dir; now a "server" holds it
    const r1 = await run(b.path); assert.notEqual(r1.status, 0); assert.ok(r1.stderr.includes(`is running (pid ${child.pid})`), r1.stderr);
    const r2 = await run(b.path, '--force'); assert.equal(r2.status, 0, r2.stderr);
    child.kill(); await new Promise(r => child.on('exit', r));
    const r3 = await run(b.path); assert.equal(r3.status, 0, 'a pid that is gone is no server: ' + r3.stderr);
  } finally { try { child.kill(); } catch { /* gone */ } rmSync(work, { recursive: true, force: true }); }
});
