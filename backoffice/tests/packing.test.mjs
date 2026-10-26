// The box is planned, not guessed. The planner is a port of the website's
// scripts/box-fit.mjs and is held to that script's own output; the order
// flow freezes the plan at pack, checks the scale against it at ship, and
// books the label. Then the ordering edge cases: the week an order belongs
// to is the operator's week, a SKU twice is one line, a bad currency or a
// bad status is refused with its name.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { pack, explode, packOrder, weightCheck, normalizeShipping, MAX_UNITS } from '../src/modules/packing.mjs';
import { recordOrder, transition, weekLabel, orderById, fulfillCsv, editItems, MAX_LINE_QTY } from '../src/modules/orders.mjs';
import { upsertItem, setRecipe, item, move } from '../src/modules/inventory.mjs';
import { trialBalance } from '../src/modules/ledger.mjs';
import { setTimezone } from '../src/render.mjs';
import { DEMO_SHIPPING } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { createApiToken } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder, fixture } from './helpers.mjs';

const ship = normalizeShipping(JSON.parse(readFileSync(fixture('shipping.json'), 'utf8')));
const expected = JSON.parse(readFileSync(fixture('boxfit_expected.json'), 'utf8'));
const strip = (b) => ({ box: b.box, box_name: b.box_name, service: b.service, items: b.items, weight_oz: b.weight_oz, billable_oz: b.billable_oz, fill: b.fill, cost_est: b.cost_est });

test('the planner matches the website\'s box-fit on eighteen orders, box for box, cent for cent', () => {
  for (const ex of expected) {
    const { parcels, missing } = explode(ex.lines, ship); const p = pack(parcels, ship);
    assert.deepEqual(p.parcels.map(strip), ex.parcels.map(strip), JSON.stringify(ex.lines));
    assert.deepEqual(p.oversize, ex.oversize); assert.equal(p.total_cost_est, ex.total_cost_est); assert.deepEqual(missing, ex.unknown);
  }
  // the demo's shipping data is the same file, restricted to the demo's SKUs
  assert.deepEqual(pack(explode({ 'wap-kit': 3, 'vision-parts': 1 }, DEMO_SHIPPING).parcels, DEMO_SHIPPING).parcels.map(strip), pack(explode({ 'wap-kit': 3, 'vision-parts': 1 }, ship).parcels, ship).parcels.map(strip));
});

test('packOrder: boxes, weight and postage for an order; every reason a human should look is a warning, never a throw', () => {
  const one = packOrder(ship, [{ sku: 'wap-kit', qty: 1 }]);
  assert.equal(one.boxes.length, 1); assert.equal(one.boxes[0].box, 'ga-box-s'); assert.equal(one.total_oz, 7); assert.equal(one.total_cost_est, 4.8); assert.equal(one.pieces, 1); assert.ok(one.complete); assert.deepEqual(one.warnings, []);
  const ten = packOrder(ship, [{ sku: 'wap-kit', qty: 10 }]);
  assert.equal(ten.boxes.length, 2); assert.match(ten.warnings[0], /2 boxes — one order, 2 labels/); assert.equal(ten.total_oz, 70);
  const odd = packOrder(ship, [{ sku: 'wap-kit', qty: 1 }, { sku: 'mystery', qty: 1 }]);
  assert.equal(odd.boxes.length, 1); assert.match(odd.warnings[0], /mystery has no parcel/); assert.ok(!odd.complete, 'a plan with a missing parcel is not one the scale can check');
  const huge = packOrder(ship, [{ sku: 'wap-kit', qty: MAX_UNITS + 1 }]);
  assert.equal(huge.boxes.length, 0); assert.match(huge.warnings[0], new RegExp(`more than ${MAX_UNITS} units`));
  const big = packOrder(normalizeShipping({ ...JSON.parse(readFileSync(fixture('shipping.json'), 'utf8')), parcels: { crate: { l: 40, w: 40, h: 40, weight_oz: 10 } } }), [{ sku: 'crate', qty: 1 }]);
  assert.match(big.warnings[0], /no box in the catalog fits/);
  // a parcel whose sides and weight pass but whose volume beats every box's usable volume is oversize, not a crash
  const snug = normalizeShipping({ fill_efficiency: 0.78, dim_weight_divisor: 166, boxes: [{ id: 'cube', name: 'Cube', inner: { l: 10, w: 10, h: 10 }, max_weight_oz: 100 }], parcels: { block: { l: 10, w: 10, h: 10, weight_oz: 5 } }, ground_advantage_rate_tiers: [{ max_oz: 100, usd: 5 }] });
  const tight = packOrder(snug, [{ sku: 'block', qty: 1 }]); assert.equal(tight.boxes.length, 0); assert.match(tight.warnings[0], /block: no box in the catalog fits/);
  const none = packOrder(null, [{ sku: 'wap-kit', qty: 1 }]);
  assert.equal(none.boxes.length, 0); assert.match(none.warnings[0], /no shipping data/);
  // the same SKU on two lines is one count
  assert.equal(packOrder(ship, [{ sku: 'wap-kit', qty: 1 }, { sku: 'wap-kit', qty: 1 }]).pieces, 2);
});

test('weightCheck: within tolerance passes, past it fails with the delta, and an incomplete plan is not checked', () => {
  const plan = packOrder(ship, [{ sku: 'wap-kit', qty: 2 }]);
  assert.deepEqual(weightCheck(plan, 14.9), { checked: true, expected: 14, measured: 14.9, delta: 0.9, ok: true, tolerance: 1.5 });
  assert.equal(weightCheck(plan, 7).ok, false, 'one kit short'); assert.equal(weightCheck(plan, 7).delta, -7);
  assert.equal(weightCheck(plan, null).checked, false); assert.equal(weightCheck(plan, NaN).checked, false);
  assert.equal(weightCheck(packOrder(ship, [{ sku: 'ghost', qty: 1 }]), 5).checked, false);
  assert.equal(weightCheck(plan, 16, { tolerance: 3 }).ok, true);
});

test('normalizeShipping refuses data the bench cannot use, and reads the dimensional-weight floor when it is there', () => {
  assert.throws(() => normalizeShipping(null), /not an object/); assert.throws(() => normalizeShipping({ boxes: [] }), /no boxes/);
  assert.throws(() => normalizeShipping({ boxes: [{ id: 'b', inner: { l: 1, w: 1 }, max_weight_oz: 5 }], ground_advantage_rate_tiers: [{ max_oz: 4, usd: 1 }] }), /box b inner.h/);
  assert.throws(() => normalizeShipping({ boxes: [{ id: 'b', inner: { l: 1, w: 1, h: 1 }, max_weight_oz: 5 }], ground_advantage_rate_tiers: [{ max_oz: 4, usd: 1 }, { max_oz: 2, usd: 2 }] }), /ascend/);
  assert.throws(() => normalizeShipping({ boxes: [{ id: 'b', inner: { l: 1, w: 1, h: 1 }, max_weight_oz: 5 }], parcels: { x: { l: 1, w: 1, h: 1, weight_oz: 0 } }, ground_advantage_rate_tiers: [{ max_oz: 4, usd: 1 }] }), /parcel x weight_oz/);
  const raw = JSON.parse(readFileSync(fixture('shipping.json'), 'utf8'));
  assert.equal(normalizeShipping(raw).dim_weight_min_cubic_in, 0, 'absent: dimensional weight applies to every box, as the website computes it today');
  const floor = normalizeShipping({ ...raw, dim_weight_min_cubic_in: 1728 });
  assert.equal(pack(explode({ 'wap-parts': 1 }, floor).parcels, floor).parcels[0].cost_est, 3.8, 'with the USPS one-cubic-foot floor a 4 oz mailer is billed at 4 oz, not at its air');
  assert.equal(pack(explode({ 'wap-parts': 1 }, ship).parcels, ship).parcels[0].cost_est, 4.8);
});

test('packing an order freezes its plan; shipping checks the scale, books the label and tells the customer nothing new', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog();
  upsertItem(db, { sku: 'xiao-esp32s3', name: 'board', on_hand: 5 }); setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1 });
  const o = recordOrder(db, sampleOrder({ items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }] }), { catalog: cat }).order;
  transition(db, o.id, 'packed', { shipping: DEMO_SHIPPING });
  const packed = orderById(db, o.id); const plan = JSON.parse(packed.pack_plan);
  assert.equal(plan.boxes[0].box, 'ga-box-m'); assert.equal(plan.total_oz, 14); assert.ok(plan.complete);
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'packed'", o.id).text, /Medium carton 14 oz · postage ≈ \$6\.75/);
  // the scale disagrees: refused, with the numbers; "ship anyway" overrides and says so
  assert.throws(() => transition(db, o.id, 'shipped', { tracking: '9400111', cfg, measuredOz: 7 }), /scale reads 7 oz, the plan expects 14 oz \(-7 oz, over the 1\.5 oz tolerance\)/);
  assert.equal(orderById(db, o.id).status, 'packed');
  assert.throws(() => transition(db, o.id, 'shipped', { tracking: '9400111', cfg, measuredOz: 'heavy' }), /number of ounces/);
  assert.throws(() => transition(db, o.id, 'shipped', { tracking: '9400111', cfg, postageCents: 4.5 }), /whole number of cents/);
  transition(db, o.id, 'shipped', { tracking: '9400111', cfg, measuredOz: 14.6, postageCents: 675 });
  const shipped = orderById(db, o.id);
  assert.equal(shipped.measured_oz, 14.6); assert.equal(shipped.postage_cents, 675);
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'shipped'", o.id).text, /scale 14\.6 oz vs 14 oz ✓ · label \$6\.75/);
  const tb = trialBalance(db); assert.ok(tb.balanced); assert.equal(tb.rows.find(r => r.account === 'expenses:postage').balance_cents, 675);
  assert.ok(!db.get("SELECT body_text FROM mail_outbox WHERE ref = ?", `order:${o.number}`).body_text.includes('oz'), 'the customer mail says nothing about the scale');
  // a second order shipped over tolerance with the override
  const p = recordOrder(db, sampleOrder({ external_id: 'p2' }), { catalog: cat }).order; transition(db, p.id, 'packed', { shipping: DEMO_SHIPPING });
  transition(db, p.id, 'shipped', { tracking: '9400222', cfg, measuredOz: 20, shipAnyway: true });
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'shipped'", p.id).text, /scale 20 oz vs 7 oz \(\+13 oz, shipped anyway\)/);
  // unpacking drops the frozen plan: the lines may change, and the next pack plans afresh
  move(db, 'xiao-esp32s3', 10, 'receive'); const u = recordOrder(db, sampleOrder({ external_id: 'u5' }), { catalog: cat }).order; transition(db, u.id, 'packed', { shipping: DEMO_SHIPPING });
  assert.equal(JSON.parse(orderById(db, u.id).pack_plan).total_oz, 7); transition(db, u.id, 'paid'); assert.equal(orderById(db, u.id).pack_plan, null);
  editItems(db, u.id, [{ sku: 'wap-kit', qty: 3 }], { catalog: cat }); transition(db, u.id, 'packed', { shipping: DEMO_SHIPPING });
  assert.equal(JSON.parse(orderById(db, u.id).pack_plan).total_oz, 21, 'the new plan is for the new lines');
  // no shipping data: packed without a plan, shipped without a check
  const q = recordOrder(db, sampleOrder({ external_id: 'q3' }), { catalog: cat }).order; transition(db, q.id, 'packed');
  assert.equal(orderById(db, q.id).pack_plan, null); transition(db, q.id, 'shipped', { tracking: '9400333', cfg, measuredOz: 99 });
  // the fulfill CSV carries the frozen plan beside the website's columns
  const r = recordOrder(db, sampleOrder({ external_id: 'r4', items: [{ sku: 'wap-kit', name: 'k', qty: 1, unit_cents: 6900 }, { sku: 'wap-parts', name: 'p', qty: 2, unit_cents: 1500 }] }), { catalog: cat }).order; transition(db, r.id, 'packed', { shipping: DEMO_SHIPPING });
  const csv = fulfillCsv(db, { status: 'packed' }); const lines = csv.trim().split('\n');
  assert.equal(lines[0], 'order_id,email,name,sku,qty,line1,line2,city,state,zip,country,order_lines,box,expected_oz,pieces,service');
  const rows = lines.slice(1).filter(l => l.startsWith(r.number)); assert.equal(rows.length, 2, 'one row per line, as the website reads it');
  assert.ok(rows.every(l => l.endsWith(',2,ga-box-m,15,3,USPS Ground Advantage')), rows.join('\n'));
});

test('ordering: the batch week is the operator\'s week; a SKU twice is one line; 1000 on a line, a bad currency and a bad status are refused by name', () => {
  // 23:30 Sunday in Boston is 03:30 Monday UTC: the operator's week, not UTC's
  const sundayNightBoston = new Date('2026-10-26T03:30:00Z');
  assert.equal(weekLabel(sundayNightBoston, 'UTC'), '2026-W44'); assert.equal(weekLabel(sundayNightBoston, 'America/New_York'), '2026-W43');
  assert.equal(weekLabel(sundayNightBoston, 'Not/AZone'), '2026-W44', 'an unknown zone falls back to UTC instead of throwing');
  setTimezone('America/New_York');
  try {
    const db = testDb(); const cat = catalog();
    const o = recordOrder(db, sampleOrder({ paid_at: '2026-10-26T03:30:00.000Z' }), { catalog: cat }).order; assert.equal(o.batch, '2026-W43');
    const two = recordOrder(db, sampleOrder({ external_id: 'two', items: [{ sku: 'wap-kit', name: 'k', qty: 1, unit_cents: 6900 }, { sku: 'wap-kit', name: 'k', qty: 2, unit_cents: 6900 }, { sku: 'wap-kit', name: 'k', qty: 1, unit_cents: 5900 }] }), { catalog: cat }).order;
    assert.deepEqual(two.items.map(i => [i.sku, i.qty, i.unit_cents]), [['wap-kit', 3, 6900], ['wap-kit', 1, 5900]], 'same SKU, same price: one line; a different price keeps its own');
    assert.deepEqual(editItems(db, two.id, [{ sku: 'wap-parts', qty: 1 }, { sku: 'wap-parts', qty: 1 }], { catalog: cat }).items.map(i => [i.sku, i.qty]), [['wap-parts', 2]]);
    assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'big', items: [{ sku: 'wap-kit', name: 'k', qty: MAX_LINE_QTY + 1, unit_cents: 1 }] }), { catalog: cat }), /more than 999 on one line/);
    assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'cur', currency: 'dollars' }), { catalog: cat }), /currency must be a three-letter code/);
    assert.equal(recordOrder(db, sampleOrder({ external_id: 'eur', currency: 'EUR' }), { catalog: cat }).order.currency, 'eur', 'a real code is kept, lower case');
    assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'str', items: 'wap-kit' }), { catalog: cat }), /items must be a list/);
    assert.throws(() => transition(db, o.id, 'shiped'), /"shiped" is not an order status/); assert.throws(() => transition(db, o.id, { to: 'packed' }), /is not an order status/);
  } finally { setTimezone('UTC'); }
});

test('over the API: lists must be lists, numbers whole, a status named; the packing endpoint shows the frozen plan beside today\'s', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const h = { authorization: `Bearer ${createApiToken(db, 't')}`, 'content-type': 'application/json' };
    const post = (p, b) => fetch(base + p, { method: 'POST', headers: h, body: JSON.stringify(b) });
    const o = recordOrder(db, sampleOrder({ items: [{ sku: 'wap-kit', name: 'k', qty: 1, unit_cents: 6900 }] }), { catalog: app.cfg.demo ? (await app.catalog()) : null }).order;
    let r = await post(`/api/orders/${o.number}/items`, { items: 'wap-kit' }); assert.equal(r.status, 400); assert.match((await r.json()).error, /items must be an array/);
    r = await post('/api/purchasing', { supplier: 'x', items: { sku: 'a' } }); assert.equal(r.status, 400);
    r = await post('/api/inventory/move', { sku: 'x', delta: '1.5' }); assert.equal(r.status, 400); assert.match((await r.json()).error, /delta must be a whole number/);
    r = await post(`/api/orders/${o.number}/refund`, { cents: 'all' }); assert.equal(r.status, 400);
    r = await post(`/api/orders/${o.number}/transition`, { to: ['packed'] }); assert.equal(r.status, 400); assert.match((await r.json()).error, /not an order status/);
    r = await post(`/api/orders/${o.number}/transition`, { to: 'packed' }); assert.equal(r.status, 200);
    r = await fetch(base + `/api/orders/${o.number}/packing`, { headers: h }); const pk = await r.json();
    assert.equal(pk.frozen.boxes[0].box, 'ga-box-s'); assert.equal(pk.now.boxes[0].box, 'ga-box-s');
    r = await post(`/api/orders/${o.number}/transition`, { to: 'shipped', tracking: '9400 1', measured_oz: 30 }); assert.equal(r.status, 400); assert.match((await r.json()).error, /scale reads 30 oz/);
    r = await post(`/api/orders/${o.number}/transition`, { to: 'shipped', tracking: '9400 1', measured_oz: 7.4, postage_cents: 480 }); assert.equal(r.status, 200); assert.equal((await r.json()).postage_cents, 480);
  } finally { await app.close(); db.close(); }
});
