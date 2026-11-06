// Away mode (away.mjs): one date the store, the promise, the acknowledgment, the SLA clock, the
// SLA job, the doctor, the admin and the CLI all read — and that ends by itself on the return day.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { awayState, setAway, clearAway, slaDueAt, ackWhen, awayUntilFor } from '../src/modules/away.mjs';
import { nextShipDate, estimateForZip, storefrontCatalog, createCheckout, sessionStatus } from '../src/modules/storefront.mjs';
import { openTicket, slaBreaches } from '../src/modules/support.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { orderPromise } from '../src/modules/promise.mjs';
import { doctor, digestText } from '../src/modules/health.mjs';
import { runJob } from '../src/jobs.mjs';
import { openExceptions } from '../src/modules/exceptions.mjs';
import { createApp, applyStripeEvent } from '../src/server.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const boston = { tz: 'America/New_York', batchCloseDay: 0, batchCloseHour: 23, batchShipDay: 2, shipFromState: 'MA' };
// Wed Oct 21 2026 15:00 Boston: the normal cadence closes Sun Oct 25 and ships Tue Oct 27
const NOW = new Date('2026-10-21T19:00:00Z');

test('the cadence counts from the return day while away: the first close on or after it, the ship day after that, and the sentence says so', () => {
  // back Mon Nov 2 → closes Sun Nov 8, ships Tue Nov 10
  let s = nextShipDate(NOW, { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: '2026-11-02' });
  assert.equal(s.closes_on, '2026-11-08'); assert.equal(s.ships_on, '2026-11-10'); assert.equal(s.label, 'Back Mon, Nov 2 · ships Tue, Nov 10'); assert.deepEqual(s.away, { until: '2026-11-02', label: 'Mon, Nov 2' });
  assert.equal(s.closes_label, 'Sunday, Nov 8', 'never "tonight" for a close weeks away');
  // back on the close day itself: that night closes, ships two days on
  s = nextShipDate(NOW, { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: '2026-11-01' }); assert.equal(s.closes_on, '2026-11-01'); assert.equal(s.ships_on, '2026-11-03');
  // a return day that is today or past is not away: the normal cadence, no mark
  s = nextShipDate(NOW, { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: '2026-10-21' }); assert.equal(s.ships_on, '2026-10-27'); assert.equal(s.away, null); assert.match(s.label, /^Order by Sunday 11 pm/);
  s = nextShipDate(NOW, { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: '2026-10-01' }); assert.equal(s.ships_on, '2026-10-27'); assert.equal(s.away, null);
  // garbage is ignored, not thrown
  s = nextShipDate(NOW, { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: 'next week' }); assert.equal(s.ships_on, '2026-10-27'); assert.equal(s.away, null);
  // a Friday 5 pm close that ships Monday, back on a Wednesday → closes Fri, ships Mon
  s = nextShipDate(NOW, { ...boston, closeDay: 5, closeHour: 17, shipDay: 1, awayUntil: '2026-11-04' }); assert.equal(s.closes_on, '2026-11-06'); assert.equal(s.ships_on, '2026-11-09');
});

test('setAway validates, awayState ends by itself on the return day, clearAway ends it early; every end is an audit row', () => {
  const db = testDb(); const cfg = testCfg(boston); const now = NOW.getTime();
  assert.equal(awayState(db, cfg, { now }).on, false); assert.equal(awayUntilFor(db, cfg, now), null);
  assert.throws(() => setAway(db, { until: 'soon' }, { cfg, now }), /YYYY-MM-DD/);
  assert.throws(() => setAway(db, { until: '2026-10-21' }, { cfg, now }), /after today \(2026-10-21\)/);
  assert.throws(() => setAway(db, { until: '2026-02-30' }, { cfg, now }), /YYYY-MM-DD/);
  assert.throws(() => setAway(db, { until: '2028-01-01' }, { cfg, now }), /more than a year/);
  const a = setAway(db, { until: '2026-11-02', note: '  Back from​ the show  ' }, { actor: 'cli', cfg, now });
  assert.equal(a.on, true); assert.equal(a.until, '2026-11-02'); assert.equal(a.until_label, 'Mon, Nov 2'); assert.equal(a.days_left, 12); assert.equal(a.note, 'Back from the show', 'the note is scrubbed like any outsider text and collapsed');
  assert.equal(a.resume_at, '2026-11-02T14:00:00.000Z', '9 am Boston on the return day, in UTC (EST by then)');
  assert.equal(awayUntilFor(db, cfg, now), '2026-11-02');
  // the morning of the return: over, by the calendar alone
  const back = Date.parse('2026-11-02T05:30:00Z');   // 00:30 Boston on Nov 2
  assert.equal(awayState(db, cfg, { now: back }).on, false); assert.equal(awayState(db, cfg, { now: back }).expired, true); assert.equal(awayUntilFor(db, cfg, back), null);
  assert.equal(awayState(db, cfg, { now: back - 3600_000 }).on, true, 'an hour before midnight Boston it is still away');
  // early return
  const c = clearAway(db, { actor: 'web', cfg, now }); assert.equal(c.on, false); assert.equal(c.until, null);
  assert.equal(clearAway(db, { cfg, now }).until, null, 'clearing twice is quiet');
  assert.deepEqual(db.all("SELECT actor, action, detail FROM audit_log WHERE action LIKE 'away.%' ORDER BY id").map(r => [r.actor, r.action, JSON.parse(r.detail)]),
    [['cli', 'away.set', { until: '2026-11-02', note: 'Back from the show' }], ['web', 'away.clear', { was_until: '2026-11-02', early: true }]]);
});

test('the store follows it: the catalog, the estimate, the Checkout Session Stripe is asked for, the promise written on a webhook order, and the thanks page', async () => {
  const db = testDb(); const cfg = testCfg({ ...boston, stripeApiKey: 'sk_test_x', secret: 'sekret' }); const cat = catalog();
  setAway(db, { until: '2026-11-02', note: 'Back from the show' }, { cfg, now: NOW.getTime() });
  const sc = storefrontCatalog(db, cat, { now: NOW, cfg });
  assert.equal(sc.ships.ships_on, '2026-11-10'); assert.deepEqual(sc.away, { until: '2026-11-02', label: 'Mon, Nov 2', note: 'Back from the show' });
  const est = estimateForZip('02134', { now: NOW, cfg, db }); assert.equal(est.ships.ships_on, '2026-11-10'); assert.match(est.label, /^Back Mon, Nov 2 · ships Tue, Nov 10 · arrives /);
  assert.equal(estimateForZip('02134', { now: NOW, cfg }).ships.ships_on, '2026-10-27', 'without the database the cadence is the normal one (the caller said nothing about being away)');
  // the session Stripe is asked for carries the away ship date on its shipping option and in its metadata
  let posted = null; const fetchImpl = async (url, init) => { posted = new URLSearchParams(init.body); return { ok: true, json: async () => ({ id: 'cs_test_away1', url: 'https://checkout.stripe.test/s' }) }; };
  const r = await createCheckout(db, cfg, cat, { items: [{ sku: 'wap-kit', qty: 1 }], zip: '02134' }, { fetchImpl, now: NOW });
  assert.equal(r.ships.ships_on, '2026-11-10'); assert.equal(posted.get('shipping_options[0][shipping_rate_data][display_name]'), 'USPS Ground Advantage · ships Tue, Nov 10'); assert.equal(posted.get('metadata[ships_on]'), '2026-11-10');
  // the webhook's order is promised the same day, and the "we have it" mail says it
  const app = createApp({ db, cfg });
  await applyStripeEvent(app, { kind: 'order', ...sampleOrder({ external_id: 'cs_test_away1', line_items_present: true }) });
  const o = db.get('SELECT * FROM orders WHERE external_id = ?', 'cs_test_away1'); assert.ok(o); assert.equal(o.promised_ship_on, '2026-11-10');
  assert.equal(orderPromise(o, cfg).ship_on, '2026-11-10');
  const mail = db.get("SELECT body_text FROM mail_outbox WHERE ref = ? ORDER BY id DESC LIMIT 1", `order:${o.number}`); assert.match(mail.body_text, /Tue, Nov 10/);
  const st = sessionStatus(db, cfg, 'cs_test_away1', { now: NOW }); assert.equal(st.ships.ships_on, '2026-11-10');
  // back early: the next catalog is on the normal cadence; the order keeps the promise it was made
  clearAway(db, { cfg, now: NOW.getTime() });
  assert.equal(storefrontCatalog(db, cat, { now: NOW, cfg }).ships.ships_on, '2026-10-27'); assert.equal(storefrontCatalog(db, cat, { now: NOW, cfg }).away, null);
  assert.equal(orderPromise(db.get('SELECT * FROM orders WHERE external_id = ?', 'cs_test_away1'), cfg).ship_on, '2026-11-10', 'the promise is stored on the order, not recomputed');
});

test('support follows it: the acknowledgment names the date, the SLA clock starts the morning of the return, and the SLA job raises nothing about a wait the customer was told about', async () => {
  const db = testDb(); const cfg = testCfg({ ...boston, slaHours: 24 }); const now = NOW.getTime();
  assert.equal(ackWhen(db, cfg, { now }), "you'll hear back within 24 hours, usually sooner.");
  assert.equal(slaDueAt(db, cfg, { now }), new Date(now + 24 * 3600_000).toISOString());
  setAway(db, { until: '2026-11-02' }, { cfg, now });
  assert.equal(ackWhen(db, cfg, { now }), "we're away until Mon, Nov 2, and you'll hear back within 24 hours of being back.");
  assert.equal(slaDueAt(db, cfg, { now }), '2026-11-03T14:00:00.000Z', '24 h after 9 am Boston on the return day');
  clearAway(db, { cfg, now });
  // openTicket and the job read the real clock: a return day twelve days from the real today
  const until = new Date(Date.now() + 12 * 86400000).toISOString().slice(0, 10); const a = setAway(db, { until }, { cfg });
  const t = openTicket(db, { email: 'ada@example.test', name: 'Ada', subject: 'Second door?', body: 'Can I add one later?', channel: 'web', cfg });
  assert.equal(t.sla_due_at, new Date(Date.parse(a.resume_at) + 24 * 3600_000).toISOString());
  const ack = db.get("SELECT body_text FROM mail_outbox WHERE to_addr = 'ada@example.test' ORDER BY id DESC LIMIT 1"); assert.match(ack.body_text, new RegExp(`we're away until ${a.until_label}, and you'll hear back within 24 hours of being back\\.`)); assert.doesNotMatch(ack.body_text, /\{\{/);
  // a ticket from before the trip whose clock ran out: breached in the queue, but not an alarm while away
  const old = openTicket(db, { email: 'bob@example.test', subject: 'Old one', body: '…', channel: 'web', cfg, autoAck: false });
  db.run('UPDATE tickets SET sla_due_at = ? WHERE id = ?', new Date(Date.now() - 86400000).toISOString(), old.id);
  assert.equal(slaBreaches(db).length, 1, 'the queue still shows it late');
  const app = createApp({ db, cfg });
  let r = await runJob(app, 'sla'); assert.ok(r.ok); assert.deepEqual(r.result, { breached: 0, deferred: 1, away_until: until }); assert.equal(openExceptions(db).filter(e => e.key.startsWith('sla:')).length, 0);
  // back: the alarm comes due
  clearAway(db, { cfg });
  r = await runJob(app, 'sla'); assert.deepEqual(r.result, { breached: 1 }); assert.deepEqual(openExceptions(db).map(e => e.key), [`sla:${old.number}`]);
  // an sla exception raised before the trip is not "Answered." by the deferral
  setAway(db, { until }, { cfg });
  r = await runJob(app, 'sla'); assert.equal(r.result.deferred, 1); assert.deepEqual(openExceptions(db).map(e => e.key), [`sla:${old.number}`], 'left open, not resolved by the trip');
});

test('the doctor has the row, the digest carries it, and the row is never a failure', () => {
  const db = testDb(); const cfg = testCfg(boston); setupOperator(db, 'operator password 1');
  let row = doctor(db, cfg).checks.find(c => c.key === 'away'); assert.equal(row.ok, true); assert.match(row.detail, /not away/);
  const until = new Date(Date.now() + 12 * 86400000).toISOString().slice(0, 10); const a = setAway(db, { until, note: 'the show' }, { cfg });
  openTicket(db, { email: 'ada@example.test', subject: 'q', body: 'b', channel: 'web', cfg, autoAck: false });
  const h = doctor(db, cfg); row = h.checks.find(c => c.key === 'away'); assert.equal(row.ok, null);
  assert.equal(row.detail, `until ${a.until_label} (${a.days_left} d) · the store says "${nextShipDate(new Date(), { ...boston, closeDay: 0, closeHour: 23, shipDay: 2, awayUntil: until }).label}" · 1 ticket waiting for you, SLA alarms wait too · "the show"`);
  assert.ok(!h.failing.includes('away'));
  assert.match(digestText(db, cfg), new RegExp(`--  Away: until ${a.until_label}`));
});

test('over HTTP: Settings sets and clears it (an owner\'s page), Home wears the strip with the one button, the public catalog and estimate say it', async () => {
  const db = testDb(); const cfg = testCfg({ ...boston, secret: 'sekret' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const totpSecret = setupOperator(db, 'operator password 1');
  try {
    let r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ password: 'operator password 1', code: totp(totpSecret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf; const post = (path, body) => fetch(base + path, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, ...body }) });
    const until = new Date(Date.now() + 10 * 86400000).toISOString().slice(0, 10);
    r = await fetch(base + '/settings', { headers: { cookie } }); let page = await r.text(); assert.ok(page.includes('<h2 id="away">Away</h2>')); assert.ok(page.includes('name="until"') && page.includes('type="date"')); assert.ok(!page.includes("I'm back"));
    r = await post('/settings/away', { until: 'yesterday', note: '' }); assert.equal(r.status, 303); assert.match(decodeURIComponent(r.headers.get('location')), /^\/settings\?err=the day you are back is a date, YYYY-MM-DD#away$/);
    r = await post('/settings/away', { until, note: 'Back from the show' }); assert.equal(r.status, 303); assert.equal(decodeURIComponent(r.headers.get('location')), `/settings?ok=away until ${awayState(db, cfg).until_label} — the store and the SLA clock follow it#away`);
    assert.equal(awayState(db, cfg).until, until);
    r = await fetch(base + '/settings', { headers: { cookie } }); page = await r.text(); assert.ok(page.includes("I'm back")); assert.ok(page.includes('Back from the show'));
    r = await fetch(base + '/', { headers: { cookie } }); page = await r.text(); assert.ok(page.includes('Away until')); assert.ok(page.includes('action="/settings/away/clear"')); assert.ok(page.includes(`value="${csrf}"`));
    // the public half, as the site asks it, says the same
    const catalog = await (await fetch(base + '/store/catalog')).json().catch(() => null);
    if (catalog) { assert.equal(catalog.away.until, until); assert.match(catalog.ships.label, /^Back /); }
    const est = await (await fetch(base + '/store/estimate?zip=02134')).json(); assert.match(est.label, /^Back /); assert.equal(est.ships.away.until, until);
    // the home button, with _back, lands on Home
    r = await post('/settings/away/clear', { _back: '/' }); assert.equal(r.status, 303); assert.match(decodeURIComponent(r.headers.get('location')), /^\/\?ok=welcome back/);
    assert.equal(awayState(db, cfg).on, false);
    assert.doesNotMatch((await (await fetch(base + '/store/estimate?zip=02134')).json()).label, /^Back /);
  } finally { await app.close?.(); }
});
