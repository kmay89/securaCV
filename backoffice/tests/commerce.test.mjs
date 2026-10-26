// The Shopify gaps, closed: a label from the bench, the customer's own address fix, abandoned checkouts counted and
// followed up once, and the new-order nudge. Every outbound call is a fake here; nothing leaves the test.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { shipmentFor, shippoBuy, buyLabel, parcelFor, serviceToken, shipFromIssues, labelHtml } from '../src/modules/labels.mjs';
import { recordSession, markSessionPaid, markSessionExpired, recoverableSessions, runRecovery, checkoutStats } from '../src/modules/recovery.mjs';
import { newOrderNudge } from '../src/modules/notify.mjs';
import { eventToOrder } from '../src/modules/stripe.mjs';
import { recordOrder, transition, orderById, timeline } from '../src/modules/orders.mjs';
import { createApp } from '../src/server.mjs';
import { orderStatusUrl } from '../src/modules/delivery.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { DEMO_SHIPPING } from '../src/modules/demo.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';
import { upsertContact } from '../src/modules/contacts.mjs';

const H = 3600000; const iso = (ms) => new Date(ms).toISOString();
const shipFrom = { name: 'Errer Labs', company: 'Errer Labs', line1: '1 Bench Way', line2: '', city: 'Somerville', state: 'MA', zip: '02143', phone: '', email: '' };
const fakeShippo = (log, { rate = '6.75', fail = null } = {}) => async (url, init) => {
  log.push({ url, body: JSON.parse(init.body) });
  if (url.endsWith('/shipments/')) return { ok: true, status: 201, json: async () => ({ rates: [{ object_id: 'rate_pm', amount: '9.10', servicelevel: { token: 'usps_priority', name: 'Priority Mail' } }, { object_id: 'rate_ga', amount: rate, servicelevel: { token: 'usps_ground_advantage', name: 'Ground Advantage' } }] }) };
  if (url.endsWith('/transactions/')) return fail ? { ok: true, status: 201, json: async () => ({ status: 'ERROR', messages: [{ text: fail }] }) } : { ok: true, status: 201, json: async () => ({ status: 'SUCCESS', tracking_number: '9400111899223456789012', label_url: 'https://deliver.goshippo.com/label.pdf' }) };
  return { ok: false, status: 404, json: async () => ({}) };
};
const packed = (db, cfg, over = {}) => { const { order } = recordOrder(db, sampleOrder({ external_id: `cs_${Math.random().toString(36).slice(2, 8)}`, ...over }), { catalog: catalog(), actor: 'test' }); transition(db, order.id, 'packed', { actor: 'test', cfg, shipping: DEMO_SHIPPING }); return orderById(db, order.id); };

test('the shipment is the box the order was packed in, the service its token, the return address the environment\'s; a blank one is named', () => {
  const cfg = testCfg({ secret: 's', shippoToken: 'shippo_test', shipFrom }); const db = testDb(); const o = packed(db, cfg);
  const sh = shipmentFor(o, JSON.parse(o.pack_plan), cfg, { shipping: DEMO_SHIPPING });
  assert.equal(sh.address_from.street1, '1 Bench Way'); assert.equal(sh.address_to.name, o.ship_name); assert.equal(sh.address_to.zip, o.ship_zip);
  assert.equal(sh.parcels.length, 1); assert.equal(sh.parcels[0].mass_unit, 'oz'); assert.ok(Number(sh.parcels[0].weight) > 0); assert.equal(sh.service, 'usps_ground_advantage'); assert.equal(sh.extra.reference_1, o.number);
  assert.equal(shipmentFor(o, JSON.parse(o.pack_plan), cfg, { measuredOz: 11.5 }).parcels[0].weight, '11.5', 'the scale wins over the plan');
  assert.deepEqual(parcelFor({ inner: { l: 9, w: 6, h: 4 } }, 10), { length: '9.5', width: '6.5', height: '4.5', distance_unit: 'in', weight: '10', mass_unit: 'oz' });
  assert.equal(serviceToken('USPS Priority Mail'), 'usps_priority'); assert.equal(serviceToken('anything else'), 'usps_ground_advantage');
  assert.deepEqual(shipFromIssues(testCfg({ shipFrom: { ...shipFrom, zip: '' } })), ['SHIP_FROM_ZIP is empty']);
  assert.deepEqual(shipFromIssues(testCfg({ shipFrom: { ...shipFrom, zip: 'abc' } })), ['SHIP_FROM_ZIP is not a US ZIP']);
});

test('the bench button buys the label, writes tracking and postage onto the order, ships it and mails the customer — and refuses before money moves', async () => {
  const cfg = testCfg({ secret: 's', shippoToken: 'shippo_test', shipFrom }); const db = testDb(); const o = packed(db, cfg); const log = [];
  const heavy = []; await assert.rejects(buyLabel(db, cfg, o.id, { fetchImpl: fakeShippo(heavy), shipping: DEMO_SHIPPING, measuredOz: 30 }), /scale reads 30 oz[\s\S]*no label was bought/); assert.equal(heavy.length, 0, 'the scale refuses before any money moves'); assert.equal(orderById(db, o.id).status, 'packed');
  const r = await buyLabel(db, cfg, o.id, { fetchImpl: fakeShippo(log), shipping: DEMO_SHIPPING, measuredOz: JSON.parse(o.pack_plan).total_oz, actor: 'test' });
  assert.equal(log.length, 2); assert.equal(log[1].body.rate, 'rate_ga', 'the ground advantage rate, not the first one'); assert.equal(log[1].body.label_file_type, 'PDF_4x6');
  assert.equal(r.tracking, '9400111899223456789012'); assert.equal(r.amount_cents, 675);
  const after = orderById(db, o.id); assert.equal(after.status, 'shipped'); assert.equal(after.tracking, '9400111899223456789012'); assert.equal(after.carrier, 'USPS'); assert.equal(after.label_url, 'https://deliver.goshippo.com/label.pdf'); assert.equal(after.postage_cents, 675);
  assert.ok(db.get("SELECT id FROM mail_outbox WHERE ref = ? AND subject LIKE '%on its way%'", `order:${o.number}`), 'the customer got the tracking mail');
  assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'order.label' AND entity_id = ?", o.number));
  assert.ok(db.get("SELECT id FROM ledger WHERE account = 'expenses:postage' AND ref = ?", `order:${o.number}`), 'the postage is an expense in the books');
  await assert.rejects(buyLabel(db, cfg, o.id, { fetchImpl: fakeShippo([]) }), /is shipped|already has a label/, 'not twice');
  // refusals before any call
  const bad = packed(db, cfg); db.run("UPDATE orders SET ship_zip = '12' WHERE id = ?", bad.id); const l2 = [];
  await assert.rejects(buyLabel(db, cfg, bad.id, { fetchImpl: fakeShippo(l2) }), /will not make a label/); assert.equal(l2.length, 0);
  const noFrom = testCfg({ secret: 's', shippoToken: 'shippo_test', shipFrom: { ...shipFrom, line1: '' } }); const p2 = packed(db, noFrom);
  await assert.rejects(buyLabel(db, noFrom, p2.id, { fetchImpl: fakeShippo(l2) }), /return address is incomplete: SHIP_FROM_LINE1/); assert.equal(l2.length, 0);
  await assert.rejects(buyLabel(db, testCfg({ secret: 's' }), p2.id, { fetchImpl: fakeShippo(l2) }), /set SHIPPO_API_TOKEN/);
  // Shippo says no: nothing is written, the order stays packed
  const p3 = packed(db, cfg);
  await assert.rejects(buyLabel(db, cfg, p3.id, { fetchImpl: fakeShippo([], { fail: 'Address not found' }), shipping: DEMO_SHIPPING }), /Shippo label ERROR: Address not found/);
  assert.equal(orderById(db, p3.id).status, 'packed'); assert.equal(orderById(db, p3.id).label_url, null);
  // the demo: a pretend label, drawn here, no network
  const demoCfg = testCfg({ secret: 's', demo: true }); const p4 = packed(db, demoCfg);
  const d = await buyLabel(db, demoCfg, p4.id, { fetchImpl: () => { throw new Error('the demo must not call out'); }, shipping: DEMO_SHIPPING });
  assert.equal(d.provider, 'demo'); assert.equal(d.label_url, `/orders/${p4.id}/label`); assert.equal(orderById(db, p4.id).status, 'shipped');
  const page = labelHtml(orderById(db, p4.id), demoCfg); assert.ok(page.includes('DEMO — NOT POSTAGE') && page.includes(p4.ship_name) && page.includes('USPS GROUND ADVANTAGE'));
});

test('the customer fixes their own address on the signed page while it is on the shelf, with the label\'s validation; after the label it is refused', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_addr' }), { catalog: catalog(), actor: 'test' });
    const url = orderStatusUrl(cfg, order).replace(cfg.baseUrl, base); const form = (o) => new URLSearchParams(o).toString();
    const page = await (await fetch(url)).text(); assert.ok(page.includes('Need to change the address?') && page.includes('action="' + url.replace(base, '') + '/address"'), 'the form is on the page while it waits');
    let r = await fetch(url + '/address', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ name: 'Ada Lovelace', line1: '12 Analytical Way', line2: 'Apt 2', city: 'Cambridge', state: 'ma', zip: '02139' }) });
    assert.equal(r.status, 303); assert.ok(r.headers.get('location').endsWith('?ok=address'));
    const o2 = orderById(db, order.id); assert.equal(o2.ship_line1, '12 Analytical Way'); assert.equal(o2.ship_state, 'MA');
    assert.ok(timeline(db, order.id).some(ev => ev.kind === 'address' && ev.actor === 'customer'), 'the timeline names the customer');
    assert.ok((await (await fetch(url + '?ok=address')).text()).includes('Address updated'));
    r = await fetch(url + '/address', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ name: 'Ada', line1: '12 Analytical Way', city: 'Cambridge', state: 'MA', zip: '1' }) });
    assert.equal(r.status, 400); assert.ok((await r.text()).includes('will not make a label'), 'the same validation the label uses, on the page');
    assert.equal((await fetch(url.replace(/\/[^/]+$/, '/nottoken') + '/address', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ line1: 'x' }) })).status, 404, 'a wrong token is nothing');
    transition(db, order.id, 'packed', { actor: 'test', cfg, shipping: DEMO_SHIPPING }); db.run('UPDATE orders SET label_url = ? WHERE id = ?', 'https://x/label.pdf', order.id);
    r = await fetch(url + '/address', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ name: 'Ada', line1: '1 Other St', city: 'Boston', state: 'MA', zip: '02110' }) });
    assert.equal(r.status, 400); assert.ok((await r.text()).includes('already has its label')); assert.equal(orderById(db, order.id).ship_line1, '12 Analytical Way');
    assert.ok(!(await (await fetch(url)).text()).includes('Need to change the address?'), 'no form once the label exists');
  } finally { await app.close(); }
});

test('every checkout is counted; an abandoned one with an email gets one follow-up, later, unless they bought meanwhile; a reopened one counts as recovered', () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', recoveryHours: 1, supportAddr: 'help@errer.test' }); const t0 = Date.parse('2026-10-26T10:00:00Z');
  // Stripe's expired event maps to what the table needs
  const ev = eventToOrder({ type: 'checkout.session.expired', data: { object: { id: 'cs_x1', mode: 'payment', amount_total: 13444, customer_details: { email: 'ada@example.test' }, after_expiration: { recovery: { url: 'https://checkout.stripe.com/c/pay/recover_1', expires_at: Math.floor(t0 / 1000) + 30 * 86400 } } } } });
  assert.equal(ev.kind, 'expired'); assert.equal(ev.email, 'ada@example.test'); assert.equal(ev.recovery_url, 'https://checkout.stripe.com/c/pay/recover_1'); assert.equal(ev.amount_cents, 13444);
  recordSession(db, { id: 'cs_x1', amount_cents: 13444, lines: '1x vision-kit', state: 'MA', created_at: iso(t0 - 40 * 60000) });
  recordSession(db, { id: 'cs_x2', amount_cents: 8131, lines: '1x wap-kit', state: 'CT', created_at: iso(t0 - 40 * 60000) });
  recordSession(db, { id: 'cs_x3', amount_cents: 8131, lines: '1x wap-kit', state: 'NY', created_at: iso(t0 - 40 * 60000) });
  markSessionExpired(db, { id: 'cs_x1', email: ev.email, recovery_url: ev.recovery_url, recovery_expires_at: ev.recovery_expires_at, amount_cents: ev.amount_cents, at: iso(t0) });
  markSessionExpired(db, { id: 'cs_x3', email: null, recovery_url: 'https://checkout.stripe.com/c/pay/recover_3', at: iso(t0), amount_cents: 8131 });   // no email typed: nobody to write to
  markSessionPaid(db, { id: 'cs_x2', order_number: 'EL-000001', at: iso(t0 - 30 * 60000) });
  assert.equal(recoverableSessions(db, cfg, { now: t0 + 30 * 60000 }).length, 0, 'not inside the hour');
  assert.equal(runRecovery(db, cfg, { now: t0 + 30 * 60000 }).waiting, 1);
  const due = recoverableSessions(db, cfg, { now: t0 + 2 * H }); assert.deepEqual(due.map(s => s.id), ['cs_x1']);
  assert.deepEqual(runRecovery(db, cfg, { now: t0 + 2 * H }), { mailed: 1, waiting: 0 });
  const m = db.get("SELECT * FROM mail_outbox WHERE ref = 'recovery:cs_x1'"); assert.ok(m); assert.equal(m.to_addr, 'ada@example.test'); assert.match(m.body_text, /1x vision-kit[\s\S]*recover_1[\s\S]*only mail we send/);
  assert.equal(runRecovery(db, cfg, { now: t0 + 5 * H }).mailed, 0, 'once');
  // a reopened session that pays: the old one is recovered, the new one paid
  markSessionPaid(db, { id: 'cs_x1b', order_number: 'EL-000002', recovered_from: 'cs_x1', at: iso(t0 + 3 * H) });
  assert.equal(db.get("SELECT status FROM checkout_sessions WHERE id = 'cs_x1'").status, 'recovered');
  const st = checkoutStats(db, { days: 30, now: t0 + 4 * H });
  assert.equal(st.opened, 4); assert.equal(st.paid, 2); assert.equal(st.abandoned, 2); assert.equal(st.recovered, 1); assert.equal(st.mailed, 1); assert.equal(st.conversion, 0.5);
  // bought meanwhile by other means: no mail
  recordSession(db, { id: 'cs_x4', amount_cents: 100, lines: '1x wap-parts', created_at: iso(t0) }); markSessionExpired(db, { id: 'cs_x4', email: 'grace@example.test', recovery_url: 'https://checkout.stripe.com/c/pay/recover_4', at: iso(t0 + 10 * H) });
  recordOrder(db, sampleOrder({ external_id: 'cs_other', email: 'grace@example.test', paid_at: iso(t0 + 11 * H) }), { catalog: catalog(), actor: 'test' });
  assert.equal(recoverableSessions(db, cfg, { now: t0 + 13 * H }).length, 0, 'an order from that address since: nothing to recover');
  // unsubscribed or bounced: no mail either
  recordSession(db, { id: 'cs_x5', amount_cents: 100, lines: '1x wap-parts', created_at: iso(t0) }); markSessionExpired(db, { id: 'cs_x5', email: 'quiet@example.test', recovery_url: 'https://checkout.stripe.com/c/pay/recover_5', at: iso(t0 + 10 * H) });
  const q = upsertContact(db, { email: 'quiet@example.test', name: 'Q', source: 'test' }); db.run('UPDATE contacts SET unsubscribed_at = ? WHERE id = ?', iso(t0), q.id);
  assert.equal(recoverableSessions(db, cfg, { now: t0 + 13 * H }).length, 0);
});

test('a new order nudges the operator: a mail through the outbox, and a push only when an ntfy topic is set — number, money, lines, never the address', async () => {
  const db = testDb(); const calls = [];
  const fetchImpl = async (url, init) => { calls.push({ url, init }); return { ok: true, status: 200 }; };
  const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_n1' }), { catalog: catalog(), actor: 'test' });
  let r = await newOrderNudge(db, testCfg({ operatorEmail: 'owner@errer.test' }), order, { fetchImpl, promise: { ship_on: '2026-10-27' } });
  assert.deepEqual(r, { mail: true, push: false }); assert.equal(calls.length, 0);
  const m = db.get("SELECT * FROM mail_outbox WHERE ref = ?", `operator:order:${order.number}`); assert.ok(m); assert.match(m.subject, new RegExp(`New order ${order.number} · \\$`)); assert.match(m.body_text, /ships 2026-10-27/); assert.ok(!m.body_text.includes(order.ship_line1), 'no address in the nudge');
  r = await newOrderNudge(db, testCfg({ operatorEmail: '', ntfyUrl: 'https://ntfy.sh/errer-orders' }), order, { fetchImpl });
  assert.deepEqual(r, { mail: false, push: true }); assert.equal(calls[0].url, 'https://ntfy.sh/errer-orders'); assert.equal(calls[0].init.method, 'POST'); assert.match(calls[0].init.headers.title, /New order/); assert.ok(!calls[0].init.body.includes(order.ship_line1));
  assert.deepEqual(await newOrderNudge(db, testCfg({ operatorEmail: 'owner@errer.test', notifyNewOrder: false }), order, { fetchImpl }), { mail: false, push: false }, 'off is off');
  assert.deepEqual(await newOrderNudge(db, testCfg({ ntfyUrl: 'http://plain.example/topic' }), order, { fetchImpl }), { mail: false, push: false }, 'a plain-http topic is refused');
});
