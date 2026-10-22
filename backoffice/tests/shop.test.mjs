import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';
import { recordOrder, transition, refund, timeline, addNote, setTags, updateShipping, resendTracking, statusCounts, batches, bulkTransition, listOrders } from '../src/modules/orders.mjs';
import { createDiscount, evaluate, listDiscounts, setDiscountStatus } from '../src/modules/discounts.mjs';
import { trialBalance, profitAndLoss } from '../src/modules/ledger.mjs';
import { eventToOrder, resolveDiscountCode } from '../src/modules/stripe.mjs';
import { createApp } from '../src/server.mjs';
import { createApiToken } from '../src/auth.mjs';
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import { listCustomers, customerStats, setContactTags, normalizeTags, customerCounts } from '../src/modules/contacts.mjs';
import { analytics, analyticsCsv } from '../src/modules/analytics.mjs';
import { listProducts, productDetail } from '../src/modules/products.mjs';
import { search } from '../src/modules/search.mjs';
import { seedDemo, DEMO_CATALOG } from '../src/modules/demo.mjs';
import { columns, line, hbars, tile } from '../src/charts.mjs';

test('discount codes: rules, pricing on a manual order, the contra account, the books still balance', async () => {
  const db = testDb(); const cat = catalog();
  createDiscount(db, { code: 'launch10', kind: 'percent', value: 10, max_uses: 2, note: 'launch' });
  createDiscount(db, { code: 'BIG15', kind: 'percent', value: 15, min_subtotal_cents: 12000 });
  createDiscount(db, { code: 'SHIPFREE', kind: 'free_shipping' });
  createDiscount(db, { code: 'FIVE', kind: 'fixed', value: 500, ends_at: '2020-01-01T00:00:00.000Z' });
  assert.throws(() => createDiscount(db, { code: 'launch10', kind: 'percent', value: 10 }), /already exists/);
  assert.throws(() => createDiscount(db, { code: 'x', kind: 'percent', value: 10 }), /2–30/);
  assert.throws(() => createDiscount(db, { code: 'PCT', kind: 'percent', value: 150 }), /1–100/);
  assert.equal(evaluate(db, 'LAUNCH10', { subtotal_cents: 6900 }).discount_cents, 690, 'percent, case-insensitive');
  assert.match(evaluate(db, 'BIG15', { subtotal_cents: 6900 }).reason, /needs a subtotal/);
  assert.equal(evaluate(db, 'BIG15', { subtotal_cents: 12900 }).discount_cents, 1935);
  assert.match(evaluate(db, 'FIVE', { subtotal_cents: 6900 }).reason, /ended 2020/);
  assert.equal(evaluate(db, 'NOPE', {}).reason, 'unknown code');
  const fs = evaluate(db, 'SHIPFREE', { subtotal_cents: 6900, shipping_cents: 800 }); assert.ok(fs.ok && fs.free_shipping && fs.discount_cents === 800);
  // a manual order names the code; the rules price it and the total follows
  const { order } = recordOrder(db, { source: 'manual', email: 'd@example.test', name: 'D', subtotal_cents: 6900, shipping_cents: 800, tax_cents: 0, total_cents: 7700, discount_code: 'launch10', items: [{ sku: 'wap-kit', qty: 1, unit_cents: 6900 }] }, { catalog: cat });
  assert.equal(order.discount_code, 'LAUNCH10'); assert.equal(order.discount_cents, 690); assert.equal(order.total_cents, 7700 - 690);
  assert.ok(timeline(db, order.id).some(ev => ev.kind === 'discount' && /LAUNCH10/.test(ev.text)));
  assert.ok(trialBalance(db).balanced, 'the discount is a contra-income leg, the sale still balances');
  const pnl = profitAndLoss(db, { from: '0000-00-00', to: '9999-99-99' }); assert.equal(pnl.lines.find(l => l.account === 'income:discounts').cents, -690);
  const fsOrder = recordOrder(db, { source: 'manual', email: 'd@example.test', subtotal_cents: 6900, shipping_cents: 800, total_cents: 7700, discount_code: 'SHIPFREE', items: [{ sku: 'wap-kit', qty: 1, unit_cents: 6900 }] }, { catalog: cat }).order;
  assert.equal(fsOrder.shipping_cents, 0); assert.equal(fsOrder.total_cents, 6900); assert.equal(fsOrder.discount_cents, 0);
  // the cap counts non-canceled orders; disabling stops it outright
  recordOrder(db, { source: 'manual', email: 'e@example.test', subtotal_cents: 6900, total_cents: 6900, discount_code: 'LAUNCH10', items: [{ sku: 'wap-kit', qty: 1, unit_cents: 6900 }] }, { catalog: cat });
  assert.match(evaluate(db, 'LAUNCH10', { subtotal_cents: 6900 }).reason, /used 2 of 2/);
  setDiscountStatus(db, 'BIG15', 'disabled'); assert.match(evaluate(db, 'big15', { subtotal_cents: 20000 }).reason, /disabled/);
  const list = listDiscounts(db); assert.equal(list.find(d => d.code === 'LAUNCH10').uses, 2); assert.equal(list.find(d => d.code === 'LAUNCH10').given_cents, 1380);
  // Stripe carries the discount it already applied
  const m = eventToOrder({ type: 'checkout.session.completed', data: { object: { id: 'cs_1', payment_status: 'paid', amount_subtotal: 6900, amount_total: 7010, total_details: { amount_discount: 690, amount_shipping: 800, amount_tax: 0 }, discounts: [{ promotion_code: { code: 'LAUNCH10' } }], customer_details: { email: 'f@example.test' }, line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] } } } });
  assert.equal(m.discount_cents, 690); assert.equal(m.discount_code, 'LAUNCH10');
  const so = recordOrder(db, m, { catalog: cat }).order; assert.equal(so.total_cents, 7010); assert.equal(so.discount_cents, 690); assert.ok(trialBalance(db).balanced);
  // a plain webhook carries a promotion id, and a free-shipping promotion takes $0 off: the order still records, and the local rules are never run on Stripe's id
  const fs2 = eventToOrder({ type: 'checkout.session.completed', data: { object: { id: 'cs_2', payment_status: 'paid', amount_subtotal: 6900, amount_total: 6900, total_details: { amount_discount: 0, amount_shipping: 0, amount_tax: 0 }, discounts: [{ promotion_code: 'promo_1AbC' }], customer_details: { email: 'g@example.test' }, line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] } } } });
  assert.equal(fs2.discount_code, 'promo_1AbC'); assert.equal(fs2.discount_cents, 0);
  const po = recordOrder(db, fs2, { catalog: cat }).order; assert.equal(po.discount_code, 'PROMO_1ABC'); assert.equal(po.total_cents, 6900);
  assert.equal(await resolveDiscountCode({ stripeApiKey: '' }, 'promo_1AbC'), 'promo_1AbC', 'no key: the id stays');
  assert.equal(await resolveDiscountCode({ stripeApiKey: 'sk_test' }, 'promo_1AbC', { fetchImpl: async (url) => ({ ok: true, json: async () => url.includes('/promotion_codes/promo_1AbC') ? { code: 'SHIPFREE' } : {} }) }), 'SHIPFREE');
  assert.equal(await resolveDiscountCode({ stripeApiKey: 'sk_test' }, 'Z4y6abcd', { fetchImpl: async (url) => ({ ok: true, json: async () => url.includes('/coupons/Z4y6abcd') ? { id: 'Z4y6abcd', name: 'LAUNCH10' } : {} }) }), 'LAUNCH10');
  assert.equal(await resolveDiscountCode({ stripeApiKey: 'sk_test' }, 'promo_gone', { fetchImpl: async () => ({ ok: false }) }), 'promo_gone');
});

test('an order has a timeline, notes, tags, an editable address and a re-sendable tracking mail', async () => {
  const db = testDb(); const cat = catalog(); const cfg = testCfg();
  const { order } = recordOrder(db, sampleOrder({ tags: 'Gift, gift , RUSH' }), { catalog: cat });
  assert.equal(order.tags, 'gift,rush');
  assert.deepEqual(timeline(db, order.id).map(e => e.kind), ['placed']);
  addNote(db, order.id, 'Leave at the side door'); setTags(db, order.id, 'gift,rush,porch');
  updateShipping(db, order.id, { line1: '2 Difference Engine Rd', city: 'Boston', state: 'ma', zip: '02134' });
  transition(db, order.id, 'packed', { at: '2026-10-20T10:00:00.000Z' }); transition(db, order.id, 'shipped', { carrier: 'USPS', tracking: '9400', cfg, at: '2026-10-21T10:00:00.000Z' });
  assert.throws(() => updateShipping(db, order.id, { line1: 'x', city: 'y', zip: 'z' }), /already shipped/);
  refund(db, order.id, 700, { reason: 'late', actor: 'stripe' });
  const kinds = timeline(db, order.id).map(e => e.kind);
  assert.deepEqual(kinds, ['refund', 'address', 'tags', 'note', 'shipped', 'packed', 'placed'], 'newest first, by when it happened — the backdated pack and ship sit under this morning\'s note');
  assert.ok(timeline(db, order.id).find(e => e.kind === 'shipped').at === '2026-10-21T10:00:00.000Z', 'a backdated step keeps its time');
  const before = db.get('SELECT COUNT(*) AS n FROM mail_outbox').n; resendTracking(db, order.id, { cfg });
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox').n, before + 1); assert.equal(timeline(db, order.id)[0].kind, 'mail');
  assert.throws(() => addNote(db, order.id, '   '), /empty/);
  // list filters: tag, sku, date window; status tabs; batches; bulk
  assert.equal(listOrders(db, { tag: 'porch' }).length, 1); assert.equal(listOrders(db, { tag: 'nope' }).length, 0);
  assert.equal(listOrders(db, { sku: 'wap-kit' }).length, 1); assert.equal(listOrders(db, { from: '2026-10-20', to: '2026-10-21' }).length, 0, 'paid 2026-10-19 is outside');
  const o2 = recordOrder(db, sampleOrder({ external_id: 'cs_2', paid_at: '2026-10-19T13:00:00.000Z' }), { catalog: cat }).order;
  const o3 = recordOrder(db, sampleOrder({ external_id: 'cs_3', paid_at: '2026-10-19T14:00:00.000Z' }), { catalog: cat }).order;
  const n = statusCounts(db); assert.equal(n.paid, 2); assert.equal(n.shipped, 1); assert.equal(n.open, 2); assert.equal(n.all, 3);
  const b = batches(db); assert.equal(b.length, 1); assert.equal(b[0].to_pack, 2); assert.equal(b[0].shipped, 1);
  const r = bulkTransition(db, [o2.id, o3.id, order.id], 'packed'); assert.equal(r.filter(x => !x.error).length, 2); assert.match(r[2].error, /cannot move/);
  assert.equal(statusCounts(db).packed, 2);
  // every filter the API advertises reaches the query
  const app = createApp({ db, cfg }); const addr = await app.listen(); const h = { authorization: `Bearer ${createApiToken(db, 't')}` };
  try {
    const get = async (qs) => (await fetch(`http://127.0.0.1:${addr.port}/api/orders?${qs}`, { headers: h })).json();
    assert.equal((await get('tag=porch')).length, 1); assert.equal((await get('tag=nope')).length, 0); assert.equal((await get('sku=wap-kit')).length, 3); assert.equal((await get('status=packed')).length, 2);
    assert.equal((await get('from=2026-10-19&to=2026-10-19')).length, 3); assert.equal((await get('from=2026-10-20')).length, 0); assert.equal((await get('discount=none')).length, 0);
  } finally { await app.close(); }
});

test('the v4 migration gives orders from before the timeline the events their timestamps prove', () => {
  const raw = new DatabaseSync(':memory:');
  for (const f of ['001_init.sql', '002_stripe_idempotency.sql', '003_api_token_ids.sql']) raw.exec(readFileSync(new URL(`../src/migrations/${f}`, import.meta.url), 'utf8'));
  raw.exec(`INSERT INTO orders (id, number, source, status, total_cents, refunded_cents, created_at, paid_at, packed_at, shipped_at, delivered_at, carrier, tracking) VALUES
    (1, 'EL-000001', 'stripe', 'delivered', 7700, 0, '2026-09-01T10:00:00Z', '2026-09-01T10:00:00Z', '2026-09-02T10:00:00Z', '2026-09-03T10:00:00Z', '2026-09-06T10:00:00Z', 'USPS', '9400'),
    (2, 'EL-000002', 'manual', 'paid', 0, 0, '2026-09-10T10:00:00Z', '2026-09-10T10:00:00Z', NULL, NULL, NULL, NULL, NULL),
    (3, 'EL-000003', 'stripe', 'refunded', 7700, 7700, '2026-09-11T10:00:00Z', '2026-09-11T10:00:00Z', NULL, NULL, NULL, NULL, NULL),
    (4, 'EL-000004', 'stripe', 'canceled', 7700, 0, '2026-09-12T10:00:00Z', '2026-09-12T10:00:00Z', NULL, NULL, NULL, NULL, NULL)`);
  raw.exec(readFileSync(new URL('../src/migrations/004_shop.sql', import.meta.url), 'utf8'));
  const ev = (id) => raw.prepare('SELECT kind, at, text FROM order_events WHERE order_id = ? ORDER BY at, id').all(id);
  assert.deepEqual(ev(1).map(e => e.kind), ['placed', 'packed', 'shipped', 'delivered']); assert.match(ev(1)[2].text, /USPS · 9400/);
  assert.deepEqual(ev(2).map(e => e.kind), ['placed']);
  assert.deepEqual(ev(3).map(e => e.kind), ['placed', 'refund']); assert.match(ev(3)[1].text, /Full refund of \$77\.00/);
  assert.deepEqual(ev(4).map(e => e.kind), ['placed', 'canceled']);
  assert.equal(raw.prepare('SELECT COUNT(*) AS n FROM order_events WHERE actor = \'migration\'').get().n, 9);
  // a fresh database gets no phantom events
  const fresh = testDb(); assert.equal(fresh.get('SELECT COUNT(*) AS n FROM order_events').n, 0);
});

test('customers: aggregates, segments, tags; products: catalog joined to stock and sales; search finds everything by its number', async () => {
  const db = testDb(); const cfg = testCfg();
  await seedDemo(db, cfg);
  const all = listCustomers(db, { limit: 1000 }); assert.ok(all.length >= 20);
  const repeat = listCustomers(db, { segment: 'repeat', sort: 'spent' }); assert.ok(repeat.length >= 3); assert.ok(repeat.every(c => c.orders >= 2)); assert.ok(repeat[0].spent_cents >= repeat[1].spent_cents);
  const top = repeat[0]; const st = customerStats(db, top.id);
  assert.equal(st.orders, top.orders); assert.equal(st.spent_cents, top.spent_cents); assert.equal(st.aov_cents, Math.round(top.spent_cents / top.orders)); assert.ok(st.first_order_at <= st.last_order_at);
  const direct = db.get('SELECT COALESCE(SUM(total_cents - refunded_cents),0) AS c FROM orders WHERE contact_id = ? AND status != \'canceled\'', top.id).c; assert.equal(st.spent_cents, direct);
  assert.equal(normalizeTags(' VIP, vip ,  School Board '), 'vip,school-board'); setContactTags(db, top.id, 'VIP, whale'); assert.equal(listCustomers(db, { tag: 'whale' })[0].id, top.id);
  const counts = customerCounts(db); assert.equal(counts.repeat, listCustomers(db, { segment: 'repeat', limit: 1000 }).length); assert.ok(counts.orgs >= 1 && counts.waitlist >= 2);
  assert.ok(listCustomers(db, { segment: 'orgs' }).every(c => c.kind !== 'person'));
  // products
  const prods = listProducts(db, DEMO_CATALOG); assert.equal(prods.length, DEMO_CATALOG.products.size);
  const wap = prods.find(p => p.sku === 'wap-kit'); assert.ok(wap.units_all > 0 && wap.gross_all === db.get('SELECT SUM(qty * unit_cents) AS c FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = \'wap-kit\' AND o.status != \'canceled\' AND o.total_cents > 0').c);
  assert.equal(wap.buildable, Math.min(14, 6, 40, 9, 30, 22, 60), 'buildable is the tightest part'); assert.equal(wap.stock, 'in'); assert.ok(wap.margin > 0.3 && wap.margin < 1);
  const vision = prods.find(p => p.sku === 'vision-kit'); assert.equal(vision.stock, 'low'); assert.equal(vision.waitlist_people, 2);
  const d = productDetail(db, DEMO_CATALOG, 'vision-kit'); assert.equal(d.parts.length, 7); assert.equal(d.weekly.length, 12); assert.ok(d.orders.length > 0); assert.equal(productDetail(db, DEMO_CATALOG, 'nope'), null);
  // search
  const o = db.get('SELECT number, tracking FROM orders WHERE tracking IS NOT NULL LIMIT 1');
  assert.equal(search(db, o.number).groups.find(g => g.kind === 'orders').rows[0].number, o.number);
  assert.equal(search(db, o.tracking).groups.find(g => g.kind === 'orders').rows[0].number, o.number);
  assert.ok(search(db, 'DEMO-W-0002').groups.find(g => g.kind === 'devices').rows[0].order_number);
  assert.ok(search(db, 'avery@').groups.find(g => g.kind === 'customers').rows.length === 1);
  assert.ok(search(db, 'PO-DEMO').groups.find(g => g.kind === 'invoices').rows.length === 1);
  assert.ok(search(db, 'fleet15').groups.find(g => g.kind === 'discounts').rows.length === 1);
  assert.equal(search(db, '').total, 0); assert.equal(search(db, 'zzz-no-such-thing').total, 0);
});

test('analytics: the storefront definitions, every bucket present, the previous period, CSV', async () => {
  const db = testDb(); await seedDemo(db, testCfg());
  const a = analytics(db, { days: 30 }); const t = a.totals;
  const from = a.from, to = a.to;
  const direct = db.get('SELECT COUNT(*) AS n, SUM(subtotal_cents) AS gross, SUM(discount_cents) AS disc, SUM(refunded_cents) AS ref, SUM(shipping_cents) AS ship, SUM(tax_cents) AS tax FROM orders WHERE paid_at >= ? AND paid_at < ? AND status != \'canceled\' AND total_cents > 0', from, to);
  assert.equal(t.orders, direct.n); assert.equal(t.gross_cents, direct.gross); assert.equal(t.net_cents, direct.gross - direct.disc - direct.ref); assert.equal(t.total_cents, t.net_cents + direct.ship + direct.tax);
  assert.ok(t.orders > 10 && t.aov_cents > 5000 && t.returning_rate > 0 && t.returning_rate <= 1 && t.gross_margin > 0.3);
  assert.equal(a.series.step, 'day'); assert.equal(a.series.buckets.length, 30); assert.equal(a.series.buckets.reduce((s, b) => s + b.orders, 0), t.orders, 'every order lands in exactly one bucket');
  assert.equal(a.series.buckets.reduce((s, b) => s + b.net_cents, 0), t.net_cents);
  assert.ok(a.previous.orders > 0, 'the 30 days before have orders too'); assert.ok(a.topProducts.length >= 4 && Math.abs(a.topProducts.reduce((s, p) => s + p.share, 0) - 1) < 1e-9);
  assert.ok(a.byState.length >= 5 && a.discounts.length >= 1 && a.fulfillment.median_hours > 0 && a.support.opened >= 3);
  // a refund counts in the period it was issued, not the period the order was paid
  const old = db.get('SELECT id, paid_at FROM orders WHERE status = \'delivered\' AND refunded_cents = 0 AND paid_at < ? ORDER BY paid_at LIMIT 1', new Date(Date.now() - 40 * 86400000).toISOString());
  const w7 = analytics(db, { days: 7 }); refund(db, old.id, 1234, { reason: 'late refund' }); const w7b = analytics(db, { days: 7 });
  assert.equal(w7b.totals.refunds_cents, w7.totals.refunds_cents + 1234, 'today\'s refund of a six-week-old order is in this week'); assert.equal(w7b.totals.net_cents, w7.totals.net_cents - 1234); assert.equal(w7b.totals.orders, w7.totals.orders, 'the old order itself is not');
  assert.equal(w7b.series.buckets.reduce((s, b) => s + b.net_cents, 0), w7b.totals.net_cents, 'the buckets still sum to the totals');
  const y = analytics(db, { days: 365 }); assert.equal(y.series.step, 'week'); assert.equal(y.series.buckets.length, 53); assert.equal(y.series.buckets.reduce((s, b) => s + b.orders, 0), y.totals.orders);
  assert.equal(y.totals.orders, db.get('SELECT COUNT(*) AS n FROM orders WHERE status != \'canceled\' AND total_cents > 0').n, 'a year holds every real order');
  const csv = analyticsCsv(db, { days: 7 }); assert.equal(csv.split('\n').length, 9); assert.match(csv, /^day,orders,units,net_sales_usd/);
  // charts render as SVG with a tooltip per point, and a table view
  const svg = columns({ points: a.series.buckets.map(b => ({ label: b.label, value: b.orders })), title: 'Orders' });
  assert.ok(svg.includes('<svg') && svg.split('class="col"').length === 31 && svg.includes('<details class="viztable">') && !svg.includes('<script'));
  const ln = line({ points: a.series.buckets.map(b => ({ label: b.label, value: b.net_cents })), title: 'Net', money: true }); assert.ok(ln.includes('stroke-width="2"') && ln.includes('$'));
  assert.ok(hbars({ rows: a.topProducts.map(p => ({ label: p.name, value: p.gross_cents })), title: 'Top', money: true }).includes('class="hf"'));
  assert.ok(tile('Orders', 10, { prev: 8, cur: 10 }).includes('▲ 25%') && tile('Refunds', 5, { prev: 10, cur: 5, invert: true }).includes('class="up"'));
  assert.ok(hbars({ rows: [{ label: '<b>x</b>', value: 1 }], title: 't' }).includes('&lt;b&gt;'), 'labels are escaped');
});
