// One scale of urgency: the level follows from the deadline, and every surface reads the same list.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { span, ago, dueIn, waiting, ticketLevel, orderLevel, attention, worst, WINDOWS } from '../src/attention.mjs';
import { todayLines } from '../src/modules/health.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { openTicket, reply } from '../src/modules/support.mjs';
import { raise as raiseException } from '../src/modules/exceptions.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const H = 3600000, D = 86400000;
const iso = (ms) => new Date(ms).toISOString();

test('a deadline becomes a level and a short clock: passed is now, inside the window is soon, else calm', () => {
  const now = Date.parse('2026-10-26T12:00:00Z');
  assert.deepEqual(dueIn(iso(now - 15 * H), { now }), { ms: -15 * H, text: 'overdue 15 h', level: 'now' });
  assert.equal(dueIn(iso(now + 2 * H), { now }).level, 'soon'); assert.equal(dueIn(iso(now + 2 * H), { now }).text, 'due in 2 h');
  assert.equal(dueIn(iso(now + 3 * D), { now }).level, 'calm'); assert.equal(dueIn(iso(now + 3 * D), { now }).text, 'due in 3 d');
  assert.equal(dueIn(iso(now - D), { now, done: true }).level, 'calm', 'a deadline already met has no clock');
  assert.equal(dueIn(null, { now }).level, 'calm'); assert.equal(dueIn('garbage', { now }).level, 'calm');
  assert.equal(span(30 * 1000), 'under a minute'); assert.equal(span(5 * 60000), '5 min'); assert.equal(span(20 * D), '3 wk'); assert.equal(span(90 * D), '3 mo');
  assert.equal(ago(iso(now - 10 * 1000), now), 'just now'); assert.equal(ago(iso(now - 3 * H), now), '3 h ago'); assert.equal(ago(null, now), '');
  const w = waiting(iso(now - 4 * D), { now, soonMs: 2 * D, nowMs: 5 * D }); assert.equal(w.level, 'soon'); assert.equal(w.text, 'waiting 4 d');
  assert.equal(waiting(iso(now - 6 * D), { now, soonMs: 2 * D, nowMs: 5 * D }).level, 'now');
  assert.equal(worst('calm', 'soon', 'now'), 'now'); assert.equal(worst('calm', 'soon'), 'soon'); assert.equal(worst(), 'calm');
});

test('a ticket row and an order row carry their level from their own columns and the stated windows', () => {
  const now = Date.parse('2026-10-26T12:00:00Z');
  assert.equal(ticketLevel({ status: 'open', first_response_at: null, sla_due_at: iso(now - H) }, now), 'now');
  assert.equal(ticketLevel({ status: 'waiting_on_us', first_response_at: null, sla_due_at: iso(now + 3 * H) }, now), 'soon');
  assert.equal(ticketLevel({ status: 'open', first_response_at: iso(now - H), sla_due_at: iso(now - 2 * H) }, now), 'calm', 'answered: the clock stopped');
  assert.equal(ticketLevel({ status: 'closed', first_response_at: null, sla_due_at: iso(now - 2 * H) }, now), 'calm');
  assert.equal(ticketLevel({ status: 'waiting_on_customer', first_response_at: null, sla_due_at: iso(now - 2 * H) }, now), 'calm');
  // an order's level follows the promise written on it (promise.mjs): calm before the ship day, soon on it, now past it
  const ymd = (ms) => iso(ms).slice(0, 10);
  assert.equal(orderLevel({ status: 'paid', paid_at: iso(now - 3 * H), promised_ship_on: ymd(now + 2 * D) }, now), 'calm');
  assert.equal(orderLevel({ status: 'packed', paid_at: iso(now - 4 * D), promised_ship_on: ymd(now) }, now), 'soon', 'the ship day itself');
  assert.equal(orderLevel({ status: 'paid', paid_at: iso(now - 6 * D), promised_ship_on: ymd(now - 2 * D) }, now), 'now', 'past the promise');
  // no promise and no payment time: the flat windows are the fallback
  assert.equal(orderLevel({ status: 'paid', paid_at: null }, now), 'calm');
  assert.equal(orderLevel({ status: 'shipped', paid_at: iso(now - 30 * D), tracking_status: 'alert' }, now), 'now', 'a carrier alert on a shipped order');
  assert.equal(orderLevel({ status: 'delivered', paid_at: iso(now - 30 * D) }, now), 'calm');
});

test('the attention list ranks what wants the admin, the sidebar counts follow it, and the digest reads the same list', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' });
  const now = Date.now();
  assert.deepEqual(attention(db, cfg).items.filter(i => i.key !== 'health'), [], 'an empty business wants nothing');
  // a fresh order (calm), one four days unshipped (soon), a ticket past its SLA (now)
  recordOrder(db, sampleOrder({ external_id: 'cs_a', paid_at: iso(now - H) }), { catalog: catalog(), actor: 'test' });
  recordOrder(db, sampleOrder({ external_id: 'cs_b', paid_at: iso(now - 4 * D) }), { catalog: catalog(), actor: 'test', promise: { ship_on: iso(now).slice(0, 10), arrive_by: null } });
  db.run("UPDATE orders SET paid_at = ? WHERE external_id = 'cs_b'", iso(now - 4 * D));
  const t = openTicket(db, { email: 'ada@example.test', subject: 'Any update?', body: 'hello', cfg, autoAck: false });
  db.run('UPDATE tickets SET sla_due_at = ? WHERE id = ?', iso(now - 15 * H), t.id);
  const a = attention(db, cfg, { now });
  const byKey = Object.fromEntries(a.items.map(i => [i.key, i]));
  assert.equal(byKey.tickets.level, 'now'); assert.match(byKey.tickets.due, /overdue 15 h/); assert.equal(byKey.tickets.n, 1);
  assert.equal(byKey.orders.level, 'soon'); assert.equal(byKey.orders.n, 2); assert.match(byKey.orders.due, /1 promised to ship today/);
  assert.equal(a.items[0].key, 'tickets', 'ranked: now before soon, a person waiting before a parcel'); assert.equal(a.level, 'now');
  assert.deepEqual(a.nav['/tickets'], { n: 1, level: 'now' }); assert.deepEqual(a.nav['/pack'], { n: 2, level: 'soon' });
  const lines = todayLines(db, cfg); assert.equal(lines[0].href, '/tickets?status=open'); assert.equal(lines[0].urgent, true); assert.equal(lines[0].level, 'now');
  // answering the ticket stops its clock: the section goes calm, the count stays until it is closed
  reply(db, t.id, 'On its way.', { cfg, actor: 'test' });
  const after = attention(db, cfg, { now: Date.now() });
  assert.notEqual(Object.fromEntries(after.items.map(i => [i.key, i])).tickets?.level, 'now', 'answered: no longer red (waiting on the customer leaves the list)');
  // an exception is always now
  await raiseException(db, cfg, 'stock:thing', 'thing is low', 'body');
  assert.equal(attention(db, cfg).nav['/exceptions'].level, 'now');
});

test('every page wears the same marks: a sidebar badge, the home focus strip, the row edge and the clock in a cell', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const now = Date.now();
    recordOrder(db, sampleOrder({ external_id: 'cs_old' }), { catalog: catalog(), actor: 'test', promise: { ship_on: iso(now - 3 * D).slice(0, 10), arrive_by: null } }); db.run("UPDATE orders SET paid_at = ? WHERE external_id = 'cs_old'", iso(now - 6 * D));
    const t = openTicket(db, { email: 'ada@example.test', subject: 'Where is it', body: 'hello', cfg, autoAck: false }); db.run('UPDATE tickets SET sla_due_at = ? WHERE id = ?', iso(now - 15 * H), t.id);
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }).toString() });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const get = async (p) => (await fetch(base + p, { headers: { cookie, accept: 'text/html' } })).text();
    const home = await get('/');
    assert.ok(home.includes('<div class="focus now">') && home.includes('overdue 15 h') && home.includes('Answer →'), 'the focus strip is the ticket past its SLA, with its clock and its one action');
    assert.ok(/href="\/tickets" class=""><span>Support<\/span> <span class="b now"[^>]*>1<\/span>/.test(home), 'the sidebar counts the ticket in red');
    assert.ok(/href="\/pack" class=""><span>Pack bench<\/span> <span class="b now"[^>]*>1<\/span>/.test(home), 'an order past the day it was promised to ship');
    assert.ok(home.includes('<a class="act now" href="/pack">') && home.includes('1 past the ship day it was promised'), 'the order card wears the same level and its clock');
    const tickets = await get('/tickets');
    assert.ok(tickets.includes('<tr class="now">') && tickets.includes('<span class="due now">overdue 15 h</span>') && tickets.includes('Needs us <b>1</b>'), 'the row edge, the cell, the default tab');
    assert.ok(!(await get('/tickets?status=closed')).includes('Where is it'), 'the closed tab does not show it');
    const detail = await get(`/tickets/${t.id}`); assert.ok(detail.includes('First reply') && detail.includes('<span class="due now">overdue 15 h</span>'), 'the fact strip says how late, not two stamps to subtract');
    const orders = await get('/orders');
    assert.ok(orders.includes('<tr class="now">') && orders.includes('waiting 6 d') && orders.includes('1 order is past the day it was promised to ship'), 'the late line and the row');
    // answering it clears the badge on the very next page (a write forgets the memo)
    const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    await fetch(base + `/tickets/${t.id}/reply`, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, body: 'On its way.' }).toString() });
    assert.ok(!/<span>Support<\/span> <span class="b now"/.test(await get('/tickets')), 'the red count on Support is gone (the Pack bench keeps its own)');
    assert.ok(!(await fetch(base + '/login')).ok || !(await (await fetch(base + '/login')).text()).includes('class="b '), 'a page without a person shows no badges');
  } finally { await app.close(); }
});
