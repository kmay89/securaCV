// The rules that keep a mistake from becoming a wrong box, a double charge
// or a broken row: whole numbers only, one label per order, one form per
// submit, a refund that is money not a status, a webhook applied once.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { int, csvCell } from '../src/http.mjs';
import { recordOrder, transition, refund, updateShipping, fulfillCsv, orderById, addressIssues, normalizeTracking } from '../src/modules/orders.mjs';
import { upsertItem, setRecipe, item, move } from '../src/modules/inventory.mjs';
import { createPo, markOrdered, receivePo } from '../src/modules/purchasing.mjs';
import { createInvoice } from '../src/modules/invoices.mjs';
import { postExpense, postPayout, trialBalance } from '../src/modules/ledger.mjs';
import { openClaim, reship } from '../src/modules/warranty.mjs';
import { openTicket, update as updateTicket } from '../src/modules/support.mjs';
import { registerDevice, assignToOrder } from '../src/modules/devices.mjs';
import { eventToOrder, signStripePayload } from '../src/modules/stripe.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

test('int() admits whole numbers only; csvCell() never hands a spreadsheet a formula', () => {
  assert.equal(int('12', { name: 'n' }), 12); assert.equal(int(' 7 ', { name: 'n' }), 7); assert.equal(int('', { name: 'n', fallback: 3 }), 3); assert.equal(int(-4, { name: 'n' }), -4);
  for (const bad of ['1.5', 'abc', 'NaN', 'Infinity', '', null, '1e3x']) assert.throws(() => int(bad, { name: 'n' }), /whole number/, String(bad));
  assert.throws(() => int('0', { name: 'qty', min: 1 }), /at least 1/); assert.throws(() => int('999', { name: 'x', max: 10 }), /at most 10/);
  assert.equal(csvCell('=HYPERLINK("http://x")'), `"'=HYPERLINK(""http://x"")"`); assert.equal(csvCell('+1 555'), "'+1 555"); assert.equal(csvCell('@me'), "'@me"); assert.equal(csvCell('-12.50'), '-12.50'); assert.equal(csvCell('-sub'), "'-sub");
  assert.equal(csvCell('a,b'), '"a,b"'); assert.equal(csvCell(null), ''); assert.equal(csvCell(5), '5');
});

test('an order refuses a zero quantity, a fractional price and negative money; a hand-made duplicate inside two minutes is named', () => {
  const db = testDb(); const cat = catalog();
  assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'q0', items: [{ sku: 'wap-kit', name: 'x', qty: 0, unit_cents: 6900 }] }), { catalog: cat }), /qty must be a whole number of at least 1/);
  assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'q1', items: [{ sku: 'wap-kit', name: 'x', qty: 1, unit_cents: 69.5 }] }), { catalog: cat }), /unit price must be a whole number/);
  assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'q2', total_cents: -1 }), { catalog: cat }), /total_cents must be a whole number of cents, 0 or more/);
  assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'q3', shipping_cents: 'eight' }), { catalog: cat }), /shipping_cents/);
  const manual = (over = {}) => ({ source: 'manual', email: 'repeat@example.test', name: 'Re Peat', subtotal_cents: 6900, total_cents: 6900, ship: { line1: '1 Loop Rd', city: 'Boston', state: 'MA', zip: '02134' }, items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 1, unit_cents: 6900 }], ...over });
  const first = recordOrder(db, manual(), { catalog: cat }).order;
  assert.throws(() => recordOrder(db, manual(), { catalog: cat }), new RegExp(`duplicate of ${first.number}`));
  assert.ok(recordOrder(db, manual({ total_cents: 7700, subtotal_cents: 7700 }), { catalog: cat }).order.id !== first.id, 'a different total is a different order');
  assert.ok(recordOrder(db, manual({ allow_duplicate: true }), { catalog: cat }).order.id !== first.id, 'a caller that means it can say so');
  assert.ok(recordOrder(db, sampleOrder({ external_id: 'cs_x' }), { catalog: cat }).created, 'a Stripe order is never second-guessed');
  assert.ok(trialBalance(db).balanced);
});

test('tracking is normalized, one label never ships two boxes, and "refunded" is not a status anyone sets', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog();
  assert.equal(normalizeTracking(' 9400 1111 2222 3333 4444 55 '), '9400111122223333444455'); assert.equal(normalizeTracking(''), '');
  assert.throws(() => normalizeTracking('9400<script>'), /characters a carrier never prints/);
  const a = recordOrder(db, sampleOrder({ external_id: 'a' }), { catalog: cat }).order; const b = recordOrder(db, sampleOrder({ external_id: 'b' }), { catalog: cat }).order;
  transition(db, a.id, 'packed'); transition(db, b.id, 'packed');
  transition(db, a.id, 'shipped', { carrier: ' usps ', tracking: '9400 1111 2222 3333 4444 55', cfg });
  assert.equal(orderById(db, a.id).tracking, '9400111122223333444455'); assert.equal(orderById(db, a.id).carrier, 'usps');
  assert.throws(() => transition(db, b.id, 'shipped', { tracking: '9400 1111 2222 3333 4444 55', cfg }), new RegExp(`already on order ${a.number}`), 'the same number with spaces is the same label');
  assert.equal(orderById(db, b.id).status, 'packed', 'the refused ship changed nothing');
  transition(db, b.id, 'shipped', { tracking: '', cfg }); assert.equal(orderById(db, b.id).tracking, null, 'no tracking is stored as none, not as an empty label');
  assert.throws(() => transition(db, a.id, 'refunded'), /record the refund instead/);
  assert.equal(orderById(db, a.id).status, 'shipped');
  assert.throws(() => refund(db, a.id, 10.5), /whole number of cents/); assert.throws(() => refund(db, a.id, -5), /above zero/);
  refund(db, a.id, a.total_cents); assert.equal(orderById(db, a.id).status, 'refunded');
});

test('a full refund before shipping puts the parts back; a canceled paid order says to refund it', () => {
  const db = testDb(); const cat = catalog();
  upsertItem(db, { sku: 'xiao-esp32s3', name: 'board', on_hand: 3 }); setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1 });
  const o = recordOrder(db, sampleOrder(), { catalog: cat }).order; transition(db, o.id, 'packed');
  assert.equal(item(db, 'xiao-esp32s3').on_hand, 2);
  refund(db, o.id, 100); assert.equal(item(db, 'xiao-esp32s3').on_hand, 2, 'a partial refund keeps the box packed');
  refund(db, o.id, o.total_cents); assert.equal(orderById(db, o.id).status, 'refunded'); assert.equal(item(db, 'xiao-esp32s3').on_hand, 3, 'the full refund released the part');
  assert.ok(db.get("SELECT 1 AS x FROM order_events WHERE order_id = ? AND kind = 'unpacked' AND text LIKE '%parts returned to stock%'", o.id));
  const p = recordOrder(db, sampleOrder({ external_id: 'pay' }), { catalog: cat }).order; transition(db, p.id, 'canceled');
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'canceled'", p.id).text, /refund it in Stripe/);
});

test('an address a label cannot carry is refused by hand and flagged otherwise; the fulfill CSV only ever lists unshipped orders, formula-safe', () => {
  const db = testDb(); const cat = catalog(); const cfg = testCfg();
  assert.deepEqual(addressIssues({ line1: '1 A St', city: 'Boston', state: 'MA', zip: '02134' }), []);
  assert.deepEqual(addressIssues({ line1: '1 A St', city: 'Boston', state: 'Mass', zip: '2134' }), ['state is not a two-letter code', 'ZIP is not 5 digits (or 5+4)']);
  assert.deepEqual(addressIssues({ line1: '', city: '', state: 'MA', zip: '02134-1234' }), ['no street line', 'no city']);
  assert.deepEqual(addressIssues({ line1: '10 Downing St', city: 'London', state: '', zip: 'SW1A 2AA', country: 'GB' }), [], 'abroad, no US rules');
  const o = recordOrder(db, sampleOrder(), { catalog: cat }).order;
  assert.throws(() => updateShipping(db, o.id, { zip: '021' }), /ZIP is not 5 digits/);
  assert.throws(() => updateShipping(db, o.id, { state: 'Massachusetts' }), /two-letter/);
  updateShipping(db, o.id, { state: 'ma', zip: '02134-0001' }); assert.equal(orderById(db, o.id).ship_state, 'MA');
  // Stripe may deliver an address a label cannot carry: it is recorded (the money is taken) and flagged, never refused
  const odd = recordOrder(db, sampleOrder({ external_id: 'odd', name: '=HYPERLINK("http://evil.example.test")', ship: { line1: '2 B St', city: 'Boston', state: 'Massachusetts', zip: '021', country: 'US' } }), { catalog: cat }).order;
  assert.equal(addressIssues({ line1: odd.ship_line1, city: odd.ship_city, state: odd.ship_state, zip: odd.ship_zip }).length, 2);
  const shipped = recordOrder(db, sampleOrder({ external_id: 's' }), { catalog: cat }).order; transition(db, shipped.id, 'packed'); transition(db, shipped.id, 'shipped', { tracking: '9400111', cfg });
  const canceled = recordOrder(db, sampleOrder({ external_id: 'c' }), { catalog: cat }).order; transition(db, canceled.id, 'canceled');
  const csv = fulfillCsv(db, { status: '' });
  assert.ok(csv.includes(o.number) && csv.includes(odd.number), 'paid orders are in');
  assert.ok(!csv.includes(shipped.number) && !csv.includes(canceled.number), 'shipped and canceled orders never get a label');
  assert.ok(csv.includes(`"'=HYPERLINK(""http://evil.example.test"")"`), 'a formula in a name is neutralized');
});

test('stock, purchase, invoice and ledger numbers are whole or refused — never NaN into SQLite', () => {
  const db = testDb();
  upsertItem(db, { sku: 'board', name: 'Board', on_hand: 2, unit_cost_cents: 750 });
  for (const bad of [NaN, 0, 1.5, Infinity, '3']) assert.throws(() => move(db, 'board', bad, 'adjust'), /whole number of units/, String(bad));
  assert.throws(() => upsertItem(db, { sku: 'x', name: 'x', reorder_point: 1.5 }), /whole number/); assert.throws(() => upsertItem(db, { sku: '', name: 'x' }), /SKU is required/);
  assert.throws(() => setRecipe(db, 'kit', { board: 0 }), /whole number of at least 1/);
  assert.throws(() => createPo(db, { supplier: 's', items: [{ sku: 'board', qty: 2.5 }] }), /positive whole number/);
  assert.throws(() => createPo(db, { supplier: 's', items: [{ sku: 'board', qty: 2, unit_cost_cents: 'free' }] }), /unit cost must be a whole number/);
  const po = createPo(db, { supplier: 's', items: [{ sku: 'board', qty: 10 }] }); markOrdered(db, po.id);
  assert.throws(() => receivePo(db, po.id, { received: { board: 1.5 } }), /whole number, 0 or more/); assert.throws(() => receivePo(db, po.id, { received: { board: NaN } }), /whole number/);
  assert.equal(item(db, 'board').on_hand, 2, 'a refused receipt moved nothing');
  assert.throws(() => createInvoice(db, { email: 'a@b.test', items: [] }), /at least one line/);
  assert.throws(() => createInvoice(db, { email: 'a@b.test', items: [{ sku: 'k', description: 'k', qty: 0, unit_cents: 100 }] }), /qty on k must be a whole number of at least 1/);
  assert.throws(() => createInvoice(db, { email: 'a@b.test', items: [{ sku: 'k', description: 'k', qty: 1, unit_cents: 1.5 }] }), /unit cents on k/);
  assert.throws(() => createInvoice(db, { email: 'a@b.test', items: [{ sku: 'k', description: 'k', qty: 1, unit_cents: 100 }], shipping_cents: NaN }), /shipping cents/);
  assert.equal(createInvoice(db, { email: 'a@b.test', items: [{ sku: 'k', description: 'k', qty: '2', unit_cents: '100' }], terms_days: '' }).total_cents, 200);
  assert.throws(() => postExpense(db, { account: 'expenses:postage', cents: NaN }), /whole number of cents above zero/);
  assert.throws(() => postExpense(db, { account: 'expenses:postage', cents: 100, paidFrom: 'assets:moon' }), /unknown account/);
  assert.throws(() => postPayout(db, { id: 'po_1', amount_cents: 10.5 }), /whole numbers of cents/);
  assert.ok(trialBalance(db).balanced);
});

test('a warranty claim needs a real order number and ships one replacement; a board goes into one open box; one ack per address per hour', () => {
  const db = testDb(); const cat = catalog(); const cfg = testCfg();
  assert.throws(() => openClaim(db, { orderNumber: 'EL-999999', category: 'dead_board', cfg }), /no order EL-999999/);
  const o = recordOrder(db, sampleOrder(), { catalog: cat }).order;
  const cl = openClaim(db, { orderNumber: o.number, category: 'dead_board', description: 'no lights', cfg });
  assert.throws(() => reship(db, cl.id, [], { recordOrderFn: recordOrder, catalog: cat }), /at least one SKU/);
  const shipped = reship(db, cl.id, [{ sku: 'wap-kit', name: 'wap-kit', qty: 1 }], { recordOrderFn: recordOrder, catalog: cat });
  assert.ok(shipped.reship_order_id); assert.equal(shipped.status, 'shipping');
  assert.throws(() => reship(db, cl.id, [{ sku: 'wap-kit', name: 'wap-kit', qty: 1 }], { recordOrderFn: recordOrder, catalog: cat }), /already has its reship order/);
  assert.equal(db.get("SELECT COUNT(*) AS n FROM orders WHERE source = 'manual'").n, 1, 'one $0 reship, not two');
  // boards
  registerDevice(db, { serial: 'sn-1', mac: 'aa:bb:cc:dd:ee:01' });
  assert.throws(() => registerDevice(db, { serial: 'sn-2', mac: 'AABBCCDDEE01' }), /MAC AA:BB:CC:DD:EE:01 is already registered to serial SN-1/);
  assert.equal(registerDevice(db, { serial: 'sn-1', mac: 'aa:bb:cc:dd:ee:01', fw_version: '2.0' }).fw_version, '2.0', 'the same board again is an update');
  transition(db, o.id, 'packed'); transition(db, o.id, 'shipped', { tracking: '9400111', cfg });
  assert.throws(() => assignToOrder(db, 'sn-1', o.id), /is shipped — boards go into orders that have not shipped/);
  assert.throws(() => assignToOrder(db, 'sn-1', 424242), /no such order/);
  const open = recordOrder(db, sampleOrder({ external_id: 'open' }), { catalog: cat }).order; assignToOrder(db, 'sn-1', open.id);
  assert.equal(db.get('SELECT order_id FROM devices WHERE serial = ?', 'SN-1').order_id, open.id);
  // acks
  openTicket(db, { email: 'loop@example.test', subject: 'one', body: 'x', cfg }); openTicket(db, { email: 'loop@example.test', subject: 'two', body: 'y', cfg });
  assert.equal(db.get("SELECT COUNT(*) AS n FROM mail_outbox WHERE to_addr = 'loop@example.test' AND kind = 'support'").n, 1, 'the second ticket in the hour sends no second acknowledgement');
  const t = openTicket(db, { email: 'p@example.test', subject: 's', body: 'b', cfg });
  assert.throws(() => updateTicket(db, t.id, { priority: 'meh' }), /priority is one of/); assert.equal(updateTicket(db, t.id, { priority: 'urgent' }).priority, 'urgent');
});

test('Stripe: a delayed payment and a free checkout are orders; a redelivered event is applied once even when it arrives twice at the same instant; a failed one is not claimed', async () => {
  const base = (o) => ({ id: 'cs_async', payment_intent: 'pi_async', currency: 'usd', amount_subtotal: 6900, amount_total: 7700, total_details: { amount_shipping: 800, amount_tax: 0 }, customer_details: { email: 'ach@example.test', name: 'A' }, shipping_details: { name: 'A', address: { line1: '1 A', city: 'B', state: 'MA', postal_code: '02134', country: 'US' } }, line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] }, ...o });
  assert.equal(eventToOrder({ type: 'checkout.session.completed', data: { object: base({ payment_status: 'unpaid' }) } }).kind, 'ignore', 'an unpaid session waits');
  assert.equal(eventToOrder({ type: 'checkout.session.async_payment_succeeded', data: { object: base({ payment_status: 'paid' }) } }).kind, 'order', 'the bank debit cleared');
  assert.equal(eventToOrder({ type: 'checkout.session.completed', data: { object: base({ payment_status: 'no_payment_required', amount_total: 0 }) } }).kind, 'order', 'a 100 % code is still an order');
  const db = testDb(); const cfg = testCfg({ secret: 'sekret' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const url = `http://127.0.0.1:${addr.port}/webhooks/stripe`;
  try {
    const body = JSON.stringify({ id: 'evt_twice', type: 'checkout.session.completed', data: { object: base({ payment_status: 'paid' }) } });
    const h = { 'content-type': 'application/json', 'stripe-signature': signStripePayload(body, 'whsec_test') };
    const [r1, r2] = await Promise.all([fetch(url, { method: 'POST', body, headers: h }), fetch(url, { method: 'POST', body, headers: h })]);
    const outs = [await r1.json(), await r2.json()];
    assert.equal(outs.filter(o => o.duplicate).length, 1, 'one delivery is told it is the duplicate'); assert.equal(outs.filter(o => o.created).length, 1);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM orders').n, 1);
    const bad = JSON.stringify({ id: 'evt_bad', type: 'checkout.session.completed', data: { object: base({ id: 'cs_bad', payment_status: 'paid', line_items: { data: [{ description: 'Mystery', quantity: 1, price: { unit_amount: 1, product: { metadata: { sku: 'mystery' } } } }] } }) } });
    const rb = await fetch(url, { method: 'POST', body: bad, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(bad, 'whsec_test') } });
    assert.equal(rb.status, 400); assert.equal(db.get('SELECT COUNT(*) AS n FROM stripe_events WHERE id = ?', 'evt_bad').n, 0, 'a failed event is released for Stripe to retry');
    assert.ok(db.get('SELECT applied_at FROM stripe_events WHERE id = ?', 'evt_twice').applied_at, 'the applied event is marked so');
    // a claim the process died on: the row exists, never applied, a minute old — the redelivery takes it over and records the order
    db.run("INSERT INTO stripe_events (id, type, received_at) VALUES ('evt_crashed', 'checkout.session.completed', ?)", new Date(Date.now() - 120_000).toISOString());
    const crashed = JSON.stringify({ id: 'evt_crashed', type: 'checkout.session.completed', data: { object: base({ id: 'cs_crashed', payment_intent: 'pi_crashed', payment_status: 'paid' }) } });
    const rc = await (await fetch(url, { method: 'POST', body: crashed, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(crashed, 'whsec_test') } })).json();
    assert.equal(rc.created, true, 'an in-flight claim left behind is retaken, not answered as a duplicate'); assert.ok(db.get("SELECT applied_at FROM stripe_events WHERE id = 'evt_crashed'").applied_at);
    // a claim seconds old is another delivery still working: this one steps back
    db.run("INSERT INTO stripe_events (id, type, received_at) VALUES ('evt_live', 'checkout.session.completed', ?)", new Date().toISOString());
    const live = JSON.stringify({ id: 'evt_live', type: 'checkout.session.completed', data: { object: base({ id: 'cs_live9', payment_intent: 'pi_live9', payment_status: 'paid' }) } });
    assert.equal((await (await fetch(url, { method: 'POST', body: live, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(live, 'whsec_test') } })).json()).duplicate, true);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM orders').n, 2);
  } finally { await app.close(); db.close(); }
});

test('over HTTP: a form submits once per token, a colliding row is a 409 with its name, a bug raises an exception, one address cannot flood the forms', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 'sekret' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const secret = setupOperator(db, 'operator password 1');
  const form = (o) => new URLSearchParams(o).toString();
  try {
    let r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'operator password 1', code: totp(secret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const post = (path, body, extra = {}) => fetch(base + path, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded', accept: 'text/html', referer: base + path, ...extra }, body: form({ _csrf: csrf, ...body }) });
    const o = recordOrder(db, sampleOrder(), { catalog: catalog() }).order;
    // the same note, the same token, twice: one note
    r = await post(`/orders/${o.id}/note`, { body: 'call before delivery', _once: 'tok-1' }); assert.equal(r.status, 303); assert.match(r.headers.get('location'), /ok=note/);
    r = await post(`/orders/${o.id}/note`, { body: 'call before delivery', _once: 'tok-1' }); assert.equal(r.status, 303); assert.match(decodeURIComponent(r.headers.get('location')), /already submitted/);
    assert.equal(db.get("SELECT COUNT(*) AS n FROM order_events WHERE order_id = ? AND kind = 'note'", o.id).n, 1);
    r = await post(`/orders/${o.id}/note`, { body: 'second note', _once: 'tok-2' }); assert.equal(db.get("SELECT COUNT(*) AS n FROM order_events WHERE order_id = ? AND kind = 'note'", o.id).n, 2, 'a fresh token is a fresh submit');
    // a refused submit frees its token, so the corrected form goes through
    r = await post(`/orders/${o.id}/note`, { body: '   ', _once: 'tok-3' }); assert.match(decodeURIComponent(r.headers.get('location')), /err=the note is empty/);
    r = await post(`/orders/${o.id}/note`, { body: 'now with words', _once: 'tok-3' }); assert.match(r.headers.get('location'), /ok=note/);
    // every rendered form and button carries a token
    const page = await (await fetch(base + `/orders/${o.id}`, { headers: { cookie } })).text(); assert.ok((page.match(/name="_once"/g) || []).length >= 3);
    // a row that collides: 409, named, not a 500
    r = await post('/campaigns/new', { slug: 'twice', subject: 's', body_text: 'b', segment: 'all' }); assert.equal(r.status, 303);
    r = await fetch(base + '/campaigns/new', { method: 'POST', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, slug: 'twice', subject: 's', body_text: 'b', segment: 'all' }) });
    assert.equal(r.status, 409); assert.match((await r.json()).error, /already exists \(campaigns\.slug\)/);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM exceptions').n, 0, 'a 409 is the caller\'s, not a bug');
    // a bug: the person sees a generic line, Home sees an exception with the route
    app.router.get('/boom', () => { throw new TypeError('a real bug'); });
    r = await fetch(base + '/boom', { headers: { cookie } }); assert.equal(r.status, 500); assert.match((await r.json()).error, /Something went wrong/);
    const ex = db.get("SELECT * FROM exceptions WHERE key = 'bug:GET /boom'"); assert.ok(ex, 'raised'); assert.match(ex.body, /a real bug/); assert.ok(!ex.resolved_at);
    // whole numbers at the door
    r = await post(`/orders/${o.id}/refund`, { cents: '12.5', reason: 'x' }); assert.match(decodeURIComponent(r.headers.get('location')), /refund \(cents\) must be a whole number/);
    r = await post('/inventory/move', { sku: 'nope', delta: 'abc', reason: 'adjust' }); assert.match(decodeURIComponent(r.headers.get('location')), /Δ must be a whole number/);
    r = await post('/orders/new', { email: 'n@example.test', name: 'N', sku: 'wap-kit', qty: '1', cents: '100', line1: '1 A', city: 'B', state: 'Mass', zip: '12' }); assert.match(decodeURIComponent(r.headers.get('location')), /will not make a label/);
    assert.equal(db.get("SELECT COUNT(*) AS n FROM orders WHERE source = 'manual'").n, 0);
    // the public forms: the sixth submission from one address in ten minutes waits
    let last; for (let i = 0; i < 6; i++) last = await fetch(base + '/forms/newsletter', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'flood@example.test' }) });
    assert.equal(last.status, 429);
  } finally { await app.close(); db.close(); }
});
