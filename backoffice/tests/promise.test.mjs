// The customer always knows where their order is: the promise is written on the order, every state has one
// honest line, the mails a state owes go by themselves, the apologies go by a click, once, and the admin sees it all.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { promiseFor, orderPromise, customerLine, customerMails, lateOrders, tellLate, tellCarrier, tellAddress, told } from '../src/modules/promise.mjs';
import { recordOrder, transition, refund, orderById } from '../src/modules/orders.mjs';
import { suggestions } from '../src/modules/suggestions.mjs';
import { orderLevel, attention } from '../src/attention.mjs';
import { createApp } from '../src/server.mjs';
import { orderStatusUrl } from '../src/modules/delivery.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const D = 86400000; const iso = (ms) => new Date(ms).toISOString(); const ymd = (ms) => iso(ms).slice(0, 10);
const mails = (db, ref) => db.all('SELECT * FROM mail_outbox WHERE ref = ? OR ref LIKE ? ORDER BY id', ref, `${ref}:%`);

test('the promise is the batch the payment fell into and the arrival window; it is written on the order and read back, stored or recomputed', () => {
  const cfg = testCfg({ secret: 's', tz: 'UTC', batchCloseDay: 0, batchCloseHour: 23, batchShipDay: 2 });
  // paid on a Wednesday: closes Sunday, ships Tuesday
  const paid = '2026-10-21T15:00:00Z'; const p = promiseFor(paid, { zip: '02139', cfg });
  assert.equal(p.closes_on, '2026-10-25'); assert.equal(p.ship_on, '2026-10-27'); assert.ok(p.arrive_by > p.ship_on);
  const db = testDb();
  const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_p', paid_at: paid }), { catalog: catalog(), actor: 'test', promise: { ship_on: p.ship_on, arrive_by: p.arrive_by } });
  const o = orderById(db, order.id); assert.equal(o.promised_ship_on, '2026-10-27'); assert.equal(o.promised_arrive_by, p.arrive_by);
  const now = Date.parse('2026-10-23T12:00:00Z');
  const op = orderPromise(o, cfg, { now }); assert.equal(op.stored, true); assert.equal(op.late, false); assert.equal(op.ships_today, false); assert.equal(op.ships_label, 'Tue, Oct 27');
  assert.equal(orderPromise(o, cfg, { now: Date.parse('2026-10-27T09:00:00Z') }).ships_today, true, 'the ship day itself');
  const lateP = orderPromise(o, cfg, { now: Date.parse('2026-10-30T09:00:00Z') }); assert.equal(lateP.late, true); assert.equal(lateP.days_late, 3);
  // an order from before the column: the same promise, recomputed from the payment time
  db.run('UPDATE orders SET promised_ship_on = NULL, promised_arrive_by = NULL WHERE id = ?', o.id);
  const re = orderPromise(orderById(db, o.id), cfg, { now }); assert.equal(re.stored, false); assert.equal(re.ship_on, '2026-10-27');
  // the level follows the promise, not a flat window: four days old on a weekly batch is calm
  assert.equal(orderLevel(orderById(db, o.id), Date.parse('2026-10-25T12:00:00Z'), cfg), 'calm');
  assert.equal(orderLevel(orderById(db, o.id), Date.parse('2026-10-27T12:00:00Z'), cfg), 'soon');
  assert.equal(orderLevel(orderById(db, o.id), Date.parse('2026-10-28T12:00:00Z'), cfg), 'now');
});

test('one honest line per state for the customer, never a bare status word', () => {
  const cfg = testCfg({ secret: 's', tz: 'UTC' }); const db = testDb();
  const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_l', paid_at: '2026-10-21T15:00:00Z' }), { catalog: catalog(), actor: 'test', promise: { ship_on: '2026-10-27', arrive_by: '2026-11-02' } });
  const o = () => orderById(db, order.id);
  let l = customerLine(o(), cfg, { now: Date.parse('2026-10-23T12:00:00Z') }); assert.equal(l.state, 'paid'); assert.match(l.text, /^ships Tue, Oct 27/); assert.match(l.detail, /weekly batches/);
  l = customerLine(o(), cfg, { now: Date.parse('2026-10-30T12:00:00Z') }); assert.equal(l.state, 'late'); assert.match(l.text, /Running later than planned — it was due to ship Tue, Oct 27/); assert.match(l.detail, /refund in full/);
  transition(db, order.id, 'packed', { actor: 'test', cfg });
  l = customerLine(o(), cfg, { now: Date.parse('2026-10-23T12:00:00Z') }); assert.equal(l.state, 'packed'); assert.match(l.text, /^Packed and waiting for the label — ships Tue, Oct 27/);
  transition(db, order.id, 'shipped', { actor: 'test', cfg, carrier: 'USPS', tracking: '9400111111111111111111' });
  l = customerLine(o(), cfg); assert.match(l.text, /On its way/); assert.match(l.detail, /estimate/);
  db.run("UPDATE orders SET tracking_status = 'alert', tracking_detail = 'Addressee unknown' WHERE id = ?", order.id);
  l = customerLine(o(), cfg); assert.equal(l.state, 'alert'); assert.match(l.detail, /Addressee unknown.*two business days/);
  db.run("UPDATE orders SET tracking_status = NULL WHERE id = ?", order.id);
  transition(db, order.id, 'delivered', { actor: 'test', cfg });
  l = customerLine(o(), cfg); assert.equal(l.state, 'delivered'); assert.match(l.text, /^Delivered/);
  refund(db, order.id, 1500, { actor: 'test', reason: 'one pack short', cfg });
  l = customerLine(o(), cfg); assert.equal(l.state, 'delivered', 'a partial refund on a delivered order: still delivered');
  // a canceled order, money on its way back
  const { order: o2 } = recordOrder(db, sampleOrder({ external_id: 'cs_c', paid_at: '2026-10-21T15:00:00Z' }), { catalog: catalog(), actor: 'test' });
  transition(db, o2.id, 'canceled', { actor: 'test', cfg });
  l = customerLine(orderById(db, o2.id), cfg); assert.equal(l.state, 'canceled'); assert.match(l.text, /^Canceled/); assert.match(l.detail, /is being refunded to your card/);
  refund(db, o2.id, orderById(db, o2.id).total_cents, { actor: 'test', cfg });
  l = customerLine(orderById(db, o2.id), cfg); assert.equal(l.state, 'refunded'); assert.match(l.text, /^Refunded \$/);
});

test('a cancel and a refund mail by themselves, whoever recorded them; the apologies go by a click, once per order, and the timeline says so', () => {
  const cfg = testCfg({ secret: 's', tz: 'UTC', supportAddr: 'help@errer.test' }); const db = testDb();
  const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_m', paid_at: '2026-10-21T15:00:00Z' }), { catalog: catalog(), actor: 'test', promise: { ship_on: '2026-10-27', arrive_by: '2026-11-02' } });
  // late, told once
  const now = Date.parse('2026-10-30T12:00:00Z');
  assert.equal(lateOrders(db, cfg, { now }).length, 1);
  const s1 = suggestions(db, cfg, { now }); const card = s1.find(x => x.key === 'late'); assert.ok(card, 'the Decide card'); assert.match(card.title, /Tell 1 customer the order is running late/); assert.equal(card.action.href, '/orders/late/notify'); assert.equal(card.due_at, '2026-10-27T23:59:59Z');
  const r = tellLate(db, cfg, order.id, { actor: 'test', now }); assert.equal(r.sent, true);
  const m = mails(db, `order:${order.number}`).find(x => x.ref.endsWith(':late')); assert.ok(m); assert.match(m.subject, /running late — sorry/); assert.match(m.body_text, /due to ship Tue, Oct 27/); assert.match(m.body_text, /refund in full/); assert.ok(m.body_text.includes(orderStatusUrl(cfg, orderById(db, order.id))));
  assert.equal(tellLate(db, cfg, order.id, { actor: 'test', now }).sent, false, 'once');
  assert.equal(told(db, order.number, 'late'), true); assert.ok(!suggestions(db, cfg, { now }).some(x => x.key === 'late'), 'the card goes once everyone is told');
  assert.ok(db.get("SELECT 1 AS x FROM order_events WHERE order_id = ? AND kind = 'mail' AND text LIKE 'Told % the order is late%'", order.id));
  assert.throws(() => tellLate(db, cfg, order.id, { actor: 'test', now: Date.parse('2026-10-23T12:00:00Z') }), /is not late/);
  // a doubtful address: asked once
  db.run("UPDATE orders SET ship_zip = '1234' WHERE id = ?", order.id);
  const addr = suggestions(db, cfg, { now }).find(x => x.key === `address:${order.number}`); assert.ok(addr); assert.match(addr.title, /confirm the address/);
  assert.equal(tellAddress(db, cfg, order.id, { actor: 'test' }).sent, true); assert.equal(tellAddress(db, cfg, order.id, { actor: 'test' }).sent, false);
  assert.match(mails(db, `order:${order.number}`).find(x => x.ref.endsWith(':address')).body_text, /does not look right[\s\S]*Reply to this mail with the correct address/);
  db.run("UPDATE orders SET ship_zip = '02139' WHERE id = ?", order.id);
  // shipped, then the carrier reports a problem: written to once
  transition(db, order.id, 'packed', { actor: 'test', cfg }); transition(db, order.id, 'shipped', { actor: 'test', cfg, carrier: 'USPS', tracking: '9400111111111111111111' });
  assert.throws(() => tellCarrier(db, cfg, order.id, { actor: 'test' }), /reports no problem/);
  db.run("UPDATE orders SET tracking_status = 'alert', tracking_detail = 'Addressee unknown' WHERE id = ?", order.id);
  const cc = suggestions(db, cfg, { now }).find(x => x.key === `carrier:${order.number}`); assert.ok(cc); assert.equal(cc.action.href, `/orders/${order.id}/tell-carrier`);
  assert.equal(tellCarrier(db, cfg, order.id, { actor: 'test' }).sent, true); assert.equal(tellCarrier(db, cfg, order.id, { actor: 'test' }).sent, false);
  assert.match(mails(db, `order:${order.number}`).find(x => x.ref.endsWith(':carrier')).body_text, /Addressee unknown[\s\S]*two business days/);
  assert.equal(attention(db, cfg, { now }).nav['/orders'].level, 'now', 'a carrier alert is now');
  // a refund, from the record itself
  refund(db, order.id, 1500, { actor: 'stripe', reason: 'stripe', cfg });
  const rm = mails(db, `order:${order.number}`).find(x => x.ref.includes(':refund:')); assert.ok(rm); assert.match(rm.subject, /^\$15\.00 refunded/); assert.match(rm.body_text, /Keep what arrived/); assert.ok(!/\(stripe\)/.test(rm.body_text), 'a bare "stripe" reason is not a reason the customer reads');
  // a cancel, from the transition itself; and no mail when there is no address to send it to
  const { order: o2 } = recordOrder(db, sampleOrder({ external_id: 'cs_c2', paid_at: '2026-10-21T15:00:00Z' }), { catalog: catalog(), actor: 'test' });
  transition(db, o2.id, 'canceled', { actor: 'test', cfg });
  const cm = mails(db, `order:${o2.number}`).find(x => x.ref.endsWith(':canceled')); assert.ok(cm); assert.match(cm.body_text, /is being refunded to your card/);
  const { order: o3 } = recordOrder(db, { ...sampleOrder({ external_id: 'cs_c3', paid_at: '2026-10-21T15:00:00Z' }), email: null }, { catalog: catalog(), actor: 'test' });
  transition(db, o3.id, 'canceled', { actor: 'test', cfg }); assert.equal(mails(db, `order:${o3.number}`).length, 0);
  // what the customer has been told, as the order page lists it
  const list = customerMails(db, order.number); assert.deepEqual(list.map(x => x.ref.split(':')[2] || 'shipped'), ['late', 'address', 'shipped', 'carrier', 'refund']);
});

test('over HTTP: the customer page says the honest line, the order page shows what they know, and the failed mail is red on the sidebar', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', tz: 'UTC', attentionTtlMs: 0, supportAddr: 'help@errer.test' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_h', paid_at: iso(Date.now() - 9 * D) }), { catalog: catalog(), actor: 'test', promise: { ship_on: ymd(Date.now() - 3 * D), arrive_by: ymd(Date.now() + 2 * D) } });
    const pub = await (await fetch(orderStatusUrl(cfg, order).replace(cfg.baseUrl, base))).text();
    assert.ok(pub.includes('Running later than planned') && pub.includes('refund in full') && pub.includes('mailto:help@errer.test'), 'the customer sees the truth and a way to reach a person');
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }).toString() });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    const get = async (p) => (await fetch(base + p, { headers: { cookie, accept: 'text/html' } })).text();
    const post = (p, body) => fetch(base + p, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, ...body }).toString() });
    let page = await get(`/orders/${order.id}`);
    assert.ok(page.includes('The customer knows') && page.includes('Their page says:</b> Running later than planned') && page.includes('Send the late apology') && page.includes('No mail has gone to this customer yet'), 'the card, the line, the nudge');
    assert.ok(page.includes('3 d late') && /class="when bad">was due /.test(page), 'the stepper names the missed promise');
    const home = await get('/'); assert.ok(home.includes('Tell 1 customer the order is running late') && home.includes('past the ship day it was promised'), 'the Decide card and the orders card');
    assert.equal((await post(`/orders/${order.id}/tell-late`, {})).status, 303);
    page = await get(`/orders/${order.id}`); assert.ok(page.includes('running late — sorry') && page.includes('<span class="pill warn">queued</span>') && !page.includes('Send the late apology'), 'the mail is listed as queued and the nudge is gone');
    // the mail fails to send: the customer was not told, and the sidebar says so in red
    db.run("UPDATE mail_outbox SET status = 'failed', last_error = 'relay refused' WHERE ref = ?", `order:${order.number}:late`);
    page = await get(`/orders/${order.id}`); assert.ok(page.includes('<span class="pill bad">failed</span>') && page.includes('relay refused'));
    assert.ok(/<span>Mail<\/span> <span class="b now"/.test(page) && page.includes('customer mail failed to send') === false, 'the Mail section is red');
    assert.ok((await get('/')).includes('customer mail failed to send'));
    // a cancel from the page mails by itself
    assert.equal((await post(`/orders/${order.id}/transition`, { to: 'canceled' })).status, 303);
    assert.ok(db.get('SELECT id FROM mail_outbox WHERE ref = ?', `order:${order.number}:canceled`));
    assert.ok((await fetch(orderStatusUrl(cfg, order).replace(cfg.baseUrl, base)).then(r => r.text())).includes('is being refunded to your card'));
  } finally { await app.close(); }
});
