// The loop closes: delivery estimates and the public status page, carrier
// tracking through a fake USPS, purchase orders feeding "buildable", recalls
// by firmware, the right to be forgotten, attachments on tickets, line edits,
// the Decide queue, Marketing with the tile map, and the operator's timezone.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';
import { recordOrder, transition, timeline, editItems } from '../src/modules/orders.mjs';
import { estimateDelivery, trackingUrl, orderToken, orderStatusUrl, deliveryLine } from '../src/modules/delivery.mjs';
import { mapUsps, pollTracking, resetTokenCache, trackingConfigured } from '../src/modules/shipping.mjs';
import { createPo, markOrdered, receivePo, cancelPo, listPos, onOrder, draftReorder, buildableAhead, draftedFor } from '../src/modules/purchasing.mjs';
import { upsertItem, move, setRecipe, buildable, item } from '../src/modules/inventory.mjs';
import { trialBalance } from '../src/modules/ledger.mjs';
import { registerDevice, assignToOrder } from '../src/modules/devices.mjs';
import { recallQuery, batches, createRecall, sendRecall, listRecalls, DEFAULT_BODY } from '../src/modules/recall.mjs';
import { forgetContact, exportContact } from '../src/modules/privacy.mjs';
import { contactById, contactByEmail, joinWaitlist, upsertContact } from '../src/modules/contacts.mjs';
import { openTicket, ingestEmail, parseEmail, extractAttachments, attachmentsFor, attachment, addMessage } from '../src/modules/support.mjs';
import { suggestions } from '../src/modules/suggestions.mjs';
import { marketing } from '../src/modules/marketing.mjs';
import { stateTileMap } from '../src/statemap.mjs';
import { setTimezone, dt, day, zonedToUtc, hourIn } from '../src/render.mjs';
import { createInvoice, issueInvoice, remindInvoice } from '../src/modules/invoices.mjs';
import { openExceptions } from '../src/modules/exceptions.mjs';
import { createApp } from '../src/server.mjs';
import { createApiToken, setupOperator, totp } from '../src/auth.mjs';
import { tinyPng } from '../src/modules/demo.mjs';

test('delivery: the estimate crosses the country a band a day, the token signs the number, the line says where it is', () => {
  const near = estimateDelivery('2026-10-19T15:00:00.000Z', 'NY', { fromState: 'MA' });   // Monday
  assert.equal(near.min_days, 2); assert.equal(near.max_days, 4); assert.equal(near.earliest, '2026-10-21'); assert.equal(near.latest, '2026-10-23'); assert.match(near.label, /^Oct 21–Oct 23$/);
  const far = estimateDelivery('2026-10-19T15:00:00.000Z', 'ca', { fromState: 'MA' });
  assert.equal(far.min_days, 4); assert.equal(far.max_days, 6); assert.equal(far.earliest, '2026-10-23', 'business days: the weekend does not count');
  assert.equal(estimateDelivery('2026-10-23T15:00:00.000Z', 'MA', { fromState: 'MA' }).earliest, '2026-10-27', 'a Friday shipment lands the next week');
  assert.equal(estimateDelivery(null, 'MA'), null); assert.equal(estimateDelivery('2026-10-19T15:00:00.000Z', 'ZZ').min_days, 3, 'an unknown state gets the middle band');
  assert.match(trackingUrl('USPS', '9400 1234'), /usps\.com.*94001234$/); assert.match(trackingUrl('UPS Ground', '1Z'), /ups\.com/); assert.match(trackingUrl('FedEx', '1'), /fedex\.com/); assert.equal(trackingUrl('USPS', ''), null);
  const a = orderToken('secret', 'EL-000001'); assert.equal(a.length, 24); assert.equal(a, orderToken('secret', 'EL-000001')); assert.notEqual(a, orderToken('secret', 'EL-000002')); assert.notEqual(a, orderToken('other', 'EL-000001'));
  const cfg = testCfg(); cfg.secret = 's'; assert.equal(orderStatusUrl(cfg, { number: 'EL-000009' }), `${cfg.baseUrl}/o/EL-000009/${orderToken('s', 'EL-000009')}`);
  assert.equal(deliveryLine({ status: 'paid' }, cfg).text, 'Paid — packed in the next batch');
  assert.match(deliveryLine({ status: 'shipped', shipped_at: '2026-10-19T15:00:00.000Z', ship_state: 'NY' }, cfg).text, /On its way · estimated Oct 21–Oct 23/);
  assert.equal(deliveryLine({ status: 'shipped', shipped_at: '2026-10-19T15:00:00.000Z', ship_state: 'NY', tracking_status: 'out_for_delivery', expected_delivery_at: '2026-10-22' }, cfg).text, 'Out for delivery · expected 2026-10-22');
  assert.equal(deliveryLine({ status: 'delivered', delivered_at: '2026-10-22T10:00:00.000Z' }, cfg).text, 'Delivered 2026-10-22');
});

test('the shipped mail carries the estimate and the status link; the public page needs the token and no login', async () => {
  const db = testDb(); const cfg = testCfg({ shipFromState: 'MA' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const { order } = recordOrder(db, sampleOrder({ ship: { line1: '1 Analytical Way', city: 'Albany', state: 'NY', zip: '12207', country: 'US' } }), { catalog: catalog() });
    transition(db, order.id, 'packed'); transition(db, order.id, 'shipped', { carrier: 'USPS', tracking: '9400111111', cfg, at: '2026-10-19T15:00:00.000Z' });
    const mail = db.get('SELECT body_text FROM mail_outbox WHERE ref = ?', `order:${order.number}`).body_text;
    assert.match(mail, /Oct 21–Oct 23/, 'the estimate is in the tracking mail'); assert.ok(mail.includes(`/o/${order.number}/${orderToken(cfg.secret, order.number)}`), 'and the signed status link');
    let r = await fetch(`${base}/o/${order.number}/${orderToken(cfg.secret, order.number)}`); assert.equal(r.status, 200); const page = await r.text();
    assert.ok(page.includes('On its way') && page.includes('Albany') && page.includes('9400111111') && page.includes('Canary WAP Kit'), page.slice(0, 400));
    assert.ok(!page.includes('ada@example.test'), 'the email is not shown on the public page'); assert.ok(!r.headers.get('set-cookie'), 'no cookie');
    assert.equal((await fetch(`${base}/o/${order.number}/wrongtoken`)).status, 404); assert.equal((await fetch(`${base}/o/EL-999999/${orderToken(cfg.secret, 'EL-999999')}`)).status, 404);
    const h = { authorization: `Bearer ${createApiToken(db, 't')}` };
    const u = await (await fetch(`${base}/api/orders/${order.number}/status-url`, { headers: h })).json(); assert.equal(u.url, orderStatusUrl(cfg, order));
  } finally { await app.close(); db.close(); }
});

test('carrier tracking: USPS words map to our states; the poll updates orders, delivers, raises and resolves alerts, and never runs without credentials', async () => {
  assert.equal(mapUsps({ statusCategory: 'Delivered', status: 'Delivered, In/At Mailbox', trackingEvents: [{ eventCity: 'ALBANY', eventState: 'NY', eventTimestamp: '2026-10-22T14:03:00Z' }] }).status, 'delivered');
  assert.equal(mapUsps({ status: 'Out for Delivery' }).status, 'out_for_delivery'); assert.equal(mapUsps({ statusCategory: 'Alert', status: 'Undeliverable as Addressed' }).status, 'alert');
  assert.equal(mapUsps({ status: 'Pre-Shipment, Shipping Label Created' }).status, 'label'); assert.equal(mapUsps({ status: 'In Transit to Next Facility', expectedDeliveryDate: '2026-10-23T00:00:00' }).expected, '2026-10-23');
  assert.equal(mapUsps({ status: 'Not Delivered' }).status, 'alert', '"not delivered" is not "delivered"');
  const db = testDb(); const noCfg = testCfg(); assert.equal(trackingConfigured(noCfg), false); assert.match((await pollTracking(db, noCfg, { fetchImpl: () => { throw new Error('must not be called'); } })).skipped, /USPS_CLIENT_ID/);
  const cfg = testCfg({ uspsClientId: 'id', uspsClientSecret: 'secret', uspsApiBase: 'https://usps.example.test' }); resetTokenCache();
  const cat = catalog(); const mk = (ext, tracking) => { const { order } = recordOrder(db, sampleOrder({ external_id: ext }), { catalog: cat }); transition(db, order.id, 'packed'); transition(db, order.id, 'shipped', { carrier: 'USPS', tracking, cfg, at: '2026-10-19T15:00:00.000Z' }); return order; };
  const a = mk('cs_a', 'TRK-A'), b = mk('cs_b', 'TRK-B'), c = mk('cs_c', 'TRK-C');
  const answers = { 'TRK-A': { status: 'In Transit to Next Facility', expectedDeliveryDate: '2026-10-23', trackingEvents: [{ eventCity: 'SPRINGFIELD', eventState: 'MA' }] }, 'TRK-B': { status: 'Delivered, Front Door', trackingEvents: [{ eventTimestamp: '2026-10-22T14:03:00Z' }] }, 'TRK-C': { statusCategory: 'Alert', status: 'Return to Sender' } };
  const calls = []; const fakeFetch = async (url, opts = {}) => { calls.push(url); if (url.endsWith('/oauth2/v3/token')) { assert.equal(JSON.parse(opts.body).client_id, 'id'); return { ok: true, status: 200, json: async () => ({ access_token: 'tok', expires_in: 3600 }) }; } assert.equal(opts.headers.authorization, 'Bearer tok'); const t = decodeURIComponent(url.split('/tracking/').pop().split('?')[0]); return { ok: true, status: 200, json: async () => answers[t] }; };
  const r = await pollTracking(db, cfg, { fetchImpl: fakeFetch, minAgeH: 0 });
  assert.deepEqual(r, { checked: 3, changed: 3, delivered: 1, alerts: 1, errors: 0 });
  assert.equal(calls.filter(u => u.endsWith('/token')).length, 1, 'one token for the whole poll');
  const A = db.get('SELECT * FROM orders WHERE id = ?', a.id); assert.equal(A.tracking_status, 'in_transit'); assert.equal(A.expected_delivery_at, '2026-10-23'); assert.match(A.tracking_detail, /SPRINGFIELD, MA/); assert.ok(A.tracking_checked_at);
  const B = db.get('SELECT * FROM orders WHERE id = ?', b.id); assert.equal(B.status, 'delivered'); assert.equal(B.delivered_at, '2026-10-22T14:03:00Z', 'delivered at the carrier\'s time, not ours'); assert.equal(timeline(db, b.id)[0].actor, 'carrier');
  assert.equal(db.get('SELECT tracking_status FROM orders WHERE id = ?', c.id).tracking_status, 'alert'); assert.ok(openExceptions(db).some(x => x.key === `tracking:${c.number}`), 'a carrier alert is an exception');
  answers['TRK-C'] = { status: 'Delivered' }; const r2 = await pollTracking(db, cfg, { fetchImpl: fakeFetch, minAgeH: 0, now: Date.now() + 1000 }); assert.equal(r2.checked, 2, 'delivered orders are not polled again');
  assert.ok(!openExceptions(db).some(x => x.key === `tracking:${c.number}`), 'and it resolves itself when the parcel lands');
  assert.equal((await pollTracking(db, cfg, { fetchImpl: fakeFetch, minAgeH: 4 })).checked, 0, 'just checked: not asked again within the window');
  // a carrier error is recorded on the order, not thrown, and the next poll tries again
  const r3 = await pollTracking(db, cfg, { fetchImpl: async (u, o) => u.endsWith('/token') ? fakeFetch(u, o) : { ok: false, status: 503 }, minAgeH: 0, now: Date.now() + 2000 }); assert.equal(r3.errors, 1); assert.match(db.get('SELECT tracking_detail FROM orders WHERE id = ?', a.id).tracking_detail, /usps 503/);
  db.close();
});

test('purchasing: a draft from the reorder points, ordered, received into stock and booked; buildable looks ahead', () => {
  const db = testDb();
  upsertItem(db, { sku: 'board', name: 'Board', kind: 'part', on_hand: 2, reorder_point: 4, reorder_qty: 20, unit_cost_cents: 750, supplier: 'Distributor', supplier_url: 'https://d.example.test' });
  upsertItem(db, { sku: 'shell', name: 'Shell', kind: 'printed', on_hand: 10, reorder_point: 3, reorder_qty: 10, unit_cost_cents: 180, supplier: 'in-house' });
  upsertItem(db, { sku: 'cable', name: 'Cable', kind: 'part', on_hand: 1, reorder_point: 5, reorder_qty: 50, unit_cost_cents: 140, supplier: 'Distributor' });
  setRecipe(db, 'kit', { board: 1, shell: 1, cable: 1 });
  assert.equal(buildable(db, 'kit'), 1); assert.deepEqual(buildableAhead(db, 'kit'), { now: 1, later: 1, expected_at: null });
  const drafts = draftReorder(db, { actor: 'system' }); assert.equal(drafts.length, 1, 'two low parts, one supplier, one PO');
  const po = drafts[0]; assert.equal(po.status, 'draft'); assert.match(po.number, /^PO-\d{6}$/); assert.deepEqual(po.items.map(i => [i.sku, i.qty, i.unit_cost_cents]), [['board', 20, 750], ['cable', 50, 140]]); assert.equal(po.total_cents, 20 * 750 + 50 * 140);
  assert.equal(draftedFor(db, 'board').id, po.id); assert.equal(draftReorder(db).length, 0, 'already drafted: nothing new');
  assert.deepEqual(onOrder(db), {}, 'a draft is not on order'); assert.equal(buildableAhead(db, 'kit').later, 1);
  assert.throws(() => receivePo(db, 999), /no such/); assert.throws(() => createPo(db, { supplier: 'x', items: [{ sku: 'nope', qty: 1 }] }), /unknown inventory sku/); assert.throws(() => createPo(db, { supplier: 'x', items: [{ sku: 'board', qty: 0 }] }), /positive/);
  markOrdered(db, po.id, { expected_at: '2026-10-30' }); assert.equal(onOrder(db).board.qty, 20); assert.equal(onOrder(db).board.expected_at, '2026-10-30');
  assert.deepEqual(buildableAhead(db, 'kit'), { now: 1, later: 10, expected_at: '2026-10-30' }, 'after the parts land the shell is the limit');
  assert.throws(() => markOrdered(db, po.id), /is ordered/);
  const bank = () => trialBalance(db).rows.find(r => r.account === 'assets:bank')?.balance_cents || 0; const before = bank();
  const part = receivePo(db, po.id, { received: { board: 8 } }); assert.equal(part.status, 'ordered', 'a partial receipt keeps it open'); assert.equal(item(db, 'board').on_hand, 10); assert.equal(part.items[0].received_qty, 8); assert.equal(onOrder(db).board.qty, 12);
  assert.equal(db.get('SELECT delta, reason, ref FROM inventory_moves ORDER BY id DESC LIMIT 1').ref, `po:${po.number}`);
  const full = receivePo(db, po.id); assert.equal(full.status, 'received'); assert.ok(full.received_at); assert.equal(item(db, 'board').on_hand, 22); assert.equal(item(db, 'cable').on_hand, 51); assert.deepEqual(onOrder(db), {});
  assert.equal(bank() - before, -(20 * 750 + 50 * 140), 'the stock is booked as inventory, paid from the bank'); assert.ok(trialBalance(db).balanced);
  assert.equal(trialBalance(db).rows.find(r => r.account === 'assets:inventory').balance_cents, 20 * 750 + 50 * 140);
  assert.throws(() => receivePo(db, po.id), /is received/); assert.throws(() => cancelPo(db, po.id), /already received/);
  const p2 = createPo(db, { supplier: 'Distributor', items: [{ sku: 'board', qty: 5, unit_cost_cents: 700 }] }); cancelPo(db, p2.id); assert.equal(listPos(db, { status: 'canceled' }).length, 1); assert.equal(listPos(db).length, 2);
  assert.ok(db.all("SELECT action FROM audit_log WHERE entity = 'po'").map(r => r.action).includes('po.receive'));
  db.close();
});

test('a recall by firmware finds every board, where it went and who has it; one transactional notice each, serials filled in', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog();
  for (let i = 1; i <= 4; i++) registerDevice(db, { serial: `S-${i}`, mac: `02:00:00:00:00:0${i}`, board: 'xiao', product_sku: 'wap-kit', fw_version: i <= 3 ? '2.4.1' : '2.5.0', batch: i <= 2 ? 'B1' : 'B2' });
  const o1 = recordOrder(db, sampleOrder({ external_id: 'o1', items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }] }), { catalog: cat }).order; const o2 = recordOrder(db, sampleOrder({ external_id: 'o2', email: 'bob@example.test', name: 'Bob Fixture' }), { catalog: cat }).order;
  assignToOrder(db, 'S-1', o1.id); assignToOrder(db, 'S-2', o1.id); assignToOrder(db, 'S-3', o2.id);
  const q = recallQuery(db, { fw: '2.4.1' }); assert.equal(q.devices.length, 3); assert.equal(q.shipped, 3); assert.equal(q.in_stock, 0);
  assert.deepEqual(q.customers.map(c => [c.email, c.serials, c.orders]), [['ada@example.test', ['S-1', 'S-2'], [o1.number]], ['bob@example.test', ['S-3'], [o2.number]]]);
  assert.equal(recallQuery(db, { batch: 'B2' }).customers.length, 1); assert.equal(recallQuery(db, { fw: '2.5.0' }).in_stock, 1); assert.equal(batches(db).length, 3);
  const r = createRecall(db, { title: 'A notice about your Canary board{{plural}}', body: DEFAULT_BODY, filter: { fw: '2.4.1' } }); assert.match(r.number, /^RC-\d{6}$/); assert.equal(r.customers, 2); assert.equal(r.sent_at, null);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE ref = ?', `recall:${r.number}`).n, 0, 'creating sends nothing');
  const sent = sendRecall(db, r.id, cfg); assert.equal(sent.queued, 2); assert.ok(sent.sent_at);
  const mails = db.all('SELECT to_addr, subject, body_text, kind FROM mail_outbox WHERE ref = ? ORDER BY id', `recall:${r.number}`);
  assert.equal(mails[0].kind, 'transactional', 'a safety notice is not marketing'); assert.equal(mails[0].subject, 'A notice about your Canary boards'); assert.match(mails[0].body_text, /serials S-1, S-2/); assert.match(mails[0].body_text, /^Hi Ada,/);
  assert.equal(mails[1].subject, 'A notice about your Canary board'); assert.match(mails[1].body_text, /serial S-3\./);
  assert.ok(timeline(db, o1.id).some(ev => ev.kind === 'mail' && ev.text.includes(r.number)), 'noted on the order');
  assert.throws(() => sendRecall(db, r.id, cfg), /already sent/); assert.equal(listRecalls(db)[0].filter.fw, '2.4.1');
  db.close();
});

test('the right to be forgotten blanks the person and keeps the books; the export is everything we hold', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog();
  const { order } = recordOrder(db, sampleOrder(), { catalog: cat }); const ct = contactById(db, order.contact_id); joinWaitlist(db, ct.id, 'vision-kit');
  const t = openTicket(db, { email: ct.email, subject: 'Help', body: 'My address is 1 Analytical Way', cfg, autoAck: false }); addMessage(db, t.id, { direction: 'in', body: 'photo', from_addr: ct.email }); saveAtt(db, t);
  const before = trialBalance(db); const x = exportContact(db, ct.email);
  assert.equal(x.contact.email, 'ada@example.test'); assert.equal(x.contact.consent_token, undefined, 'tokens are not exported'); assert.equal(x.orders.length, 1); assert.equal(x.orders[0].items[0].sku, 'wap-kit'); assert.equal(x.tickets.length, 1); assert.equal(x.waitlist.length, 1); assert.ok(x.mail.length >= 0); assert.ok(x.exported_at);
  const r = forgetContact(db, ct.email, { actor: 'cli' }); assert.deepEqual([r.orders, r.tickets, r.attachments, r.waitlist], [1, 1, 1, 1]);
  const g = contactById(db, ct.id); assert.equal(g.email, `forgotten-${ct.id}@forgotten.invalid`); assert.equal(g.name, 'Forgotten customer'); assert.ok(g.forgotten_at); assert.equal(g.consent_marketing, 0); assert.equal(contactByEmail(db, 'ada@example.test') ?? null, null);
  const o = db.get('SELECT * FROM orders WHERE id = ?', order.id); assert.equal(o.ship_line1, '(forgotten)'); assert.equal(o.ship_city, ''); assert.equal(o.ship_state, 'MA', 'the state stays for the sales-tax table'); assert.equal(o.total_cents, 7700); assert.equal(o.number, order.number);
  assert.equal(db.get('SELECT body FROM ticket_messages WHERE ticket_id = ? ORDER BY id LIMIT 1', t.id).body, "(forgotten at the customer's request)"); assert.equal(attachmentsFor(db, t.id).length, 0); assert.equal(db.get('SELECT subject, status FROM tickets WHERE id = ?', t.id).status, 'closed');
  assert.deepEqual(trialBalance(db).rows, before.rows, 'the ledger is untouched');
  assert.equal(forgetContact(db, ct.id).already, true); assert.throws(() => forgetContact(db, 'nobody@example.test'), /no such/);
  assert.ok(db.get("SELECT 1 AS ok FROM audit_log WHERE action = 'contact.forget' AND entity_id = ?", String(ct.id)), 'logged');
  db.close();
  function saveAtt(db, t) { const m = db.get('SELECT id FROM ticket_messages WHERE ticket_id = ? ORDER BY id DESC LIMIT 1', t.id); db.run('INSERT INTO attachments (ticket_message_id, filename, mime, size, bytes, created_at) VALUES (?, ?, ?, ?, ?, ?)', m.id, 'a.png', 'image/png', 3, Buffer.from([1, 2, 3]), '2026-10-20T00:00:00.000Z'); }
});

test('attachments: a photo in a multipart mail lands on the ticket and is served same-origin, sandboxed, never as a page', async () => {
  const png = tinyPng(8, 8); assert.equal(png.subarray(1, 4).toString(), 'PNG');
  const b64 = png.toString('base64').replace(/(.{76})/g, '$1\r\n');
  const raw = ['From: Ada <ada@example.test>', 'To: hello@errer.test', 'Subject: Lid is bowed', 'Message-ID: <att1@example.test>', 'Content-Type: multipart/mixed; boundary="outer"', '', '--outer', 'Content-Type: multipart/alternative; boundary="inner"', '', '--inner', 'Content-Type: text/plain', '', 'See the photo.', '--inner', 'Content-Type: text/html', '', '<p>See the photo.</p>', '--inner--', '--outer', 'Content-Type: image/png; name="lid.png"', 'Content-Transfer-Encoding: base64', 'Content-Disposition: attachment; filename="lid.png"', '', b64, '--outer', 'Content-Type: application/x-msdownload; name="setup.exe"', 'Content-Transfer-Encoding: base64', 'Content-Disposition: attachment; filename="setup.exe"', '', 'AAAA', '--outer--', ''].join('\r\n');
  const m = parseEmail(raw); assert.equal(m.text, 'See the photo.'); assert.equal(m.attachments.length, 1, 'the executable is dropped'); assert.equal(m.attachments[0].filename, 'lid.png'); assert.equal(m.attachments[0].mime, 'image/png'); assert.ok(m.attachments[0].bytes.equals(png));
  assert.equal(extractAttachments('x', 'text/plain').length, 0); assert.equal(extractAttachments(raw.split('\r\n\r\n').slice(1).join('\r\n\r\n'), 'multipart/mixed; boundary="outer"', { max: 0 }).length, 0);
  const db = testDb(); const cfg = testCfg(); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const r = ingestEmail(db, m, { cfg }); assert.equal(r.created, true); assert.equal(r.attachments, 1);
    const atts = attachmentsFor(db, r.ticket.id); assert.equal(atts.length, 1); assert.equal(atts[0].size, png.length); const a = attachment(db, atts[0].id); assert.ok(a.bytes.equals(png)); assert.equal(a.ticket_id, r.ticket.id);
    const re = ingestEmail(db, { ...m, subject: `Re: [${r.ticket.number} ${r.ticket.thread_token}] Lid is bowed`, messageId: '<att2@example.test>', text: 'another' }, { cfg }); assert.equal(re.created, false); assert.equal(re.attachments, 1); assert.equal(attachmentsFor(db, r.ticket.id).length, 2);
    assert.equal((await fetch(`${base}/tickets/${r.ticket.id}/att/${atts[0].id}`, { redirect: 'manual' })).status, 303, 'login first');
    const secret = setupOperator(db, 'operator password 1'); const lr = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ password: 'operator password 1', code: totp(secret) }).toString() });
    const h = { cookie: lr.headers.get('set-cookie').split(';')[0] };
    const got = await fetch(`${base}/tickets/${r.ticket.id}/att/${atts[0].id}`, { headers: h });
    assert.equal(got.status, 200); assert.equal(got.headers.get('content-type'), 'image/png'); assert.match(got.headers.get('content-disposition'), /^inline; filename="lid.png"/); assert.match(got.headers.get('content-security-policy'), /sandbox/); assert.equal(got.headers.get('x-content-type-options'), 'nosniff');
    assert.ok(Buffer.from(await got.arrayBuffer()).equals(png));
    assert.equal((await fetch(`${base}/tickets/${r.ticket.id + 1}/att/${atts[0].id}`, { headers: h })).status, 404, 'an attachment is reachable only under its own ticket');
    const pageHtml = await (await fetch(`${base}/tickets/${r.ticket.id}`, { headers: h })).text(); assert.ok(pageHtml.includes(`<img src="/tickets/${r.ticket.id}/att/${atts[0].id}"`), 'the ticket page shows the photo inline');
  } finally { await app.close(); db.close(); }
});

test('an order\'s lines change until it is packed; the charge does not', async () => {
  const db = testDb(); const cat = catalog(); const { order } = recordOrder(db, sampleOrder(), { catalog: cat });
  const o = editItems(db, order.id, [{ sku: 'wap-kit', qty: 1 }, { sku: 'wap-parts', qty: 2 }], { catalog: cat });
  assert.deepEqual(o.items.map(i => [i.sku, i.qty]), [['wap-kit', 1], ['wap-parts', 2]]); assert.equal(o.total_cents, 7700); assert.ok(o.items[1].cogs_cents >= 0); assert.equal(timeline(db, order.id)[0].kind, 'items');
  assert.throws(() => editItems(db, order.id, [{ sku: 'nope', qty: 1 }], { catalog: cat }), /unknown SKU/); assert.throws(() => editItems(db, order.id, [], { catalog: cat }), /at least one/);
  transition(db, order.id, 'packed'); assert.throws(() => editItems(db, order.id, [{ sku: 'wap-kit', qty: 1 }], { catalog: cat }), /only before packing/);
  const cfg = testCfg(); const app = createApp({ db, cfg }); const addr = await app.listen();
  try { const h = { authorization: `Bearer ${createApiToken(db, 't')}`, 'content-type': 'application/json' }; const { order: o2 } = recordOrder(db, sampleOrder({ external_id: 'cs_2' }), { catalog: cat });
    const r = await fetch(`http://127.0.0.1:${addr.port}/api/orders/${o2.number}/items`, { method: 'POST', headers: h, body: JSON.stringify({ items: [{ sku: 'wap-parts', qty: 3 }] }) }); assert.equal(r.status, 200); assert.equal((await r.json()).items[0].qty, 3);
  } finally { await app.close(); db.close(); }
});

test('the Decide queue drafts the next move from the data and says why; every card disappears when its condition clears', async () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog(); const now = Date.parse('2026-10-22T12:00:00.000Z');
  assert.deepEqual(suggestions(db, cfg, { now }), [], 'an empty company has nothing to decide');
  upsertItem(db, { sku: 'board', name: 'Board', kind: 'part', on_hand: 1, reorder_point: 4, reorder_qty: 20, unit_cost_cents: 750, supplier: 'Distributor' });
  let s = suggestions(db, cfg, { now }); assert.equal(s.length, 1); assert.equal(s[0].key, 'po:draft'); assert.equal(s[0].action.href, '/purchasing/draft'); assert.match(s[0].why, /reorder point/);
  const [po] = draftReorder(db); s = suggestions(db, cfg, { now }); assert.equal(s[0].key, `po:${po.number}`); assert.equal(s[0].action.href, `/purchasing/${po.id}/order`); assert.equal(s[0].link, `/purchasing/${po.id}`);
  markOrdered(db, po.id); assert.equal(suggestions(db, cfg, { now }).length, 0, 'on order: nothing to decide');
  // a waitlist with something buildable
  setRecipe(db, 'wap-kit', { board: 1 }); receivePo(db, po.id); const w = upsertContact(db, { email: 'w@example.test', name: 'W' }); joinWaitlist(db, w.id, 'wap-kit');
  s = suggestions(db, cfg, { now }); assert.equal(s.length, 1); assert.equal(s[0].key, 'waitlist:wap-kit'); assert.deepEqual(s[0].action.fields, { sku: 'wap-kit', _back: '/' }); assert.match(s[0].detail, /^21 can be built/);
  db.run('UPDATE waitlist SET notified_at = ?', '2026-10-22T00:00:00.000Z'); assert.equal(suggestions(db, cfg, { now }).length, 0);
  // a ticket that a canned reply answers; the card names the reply and carries it
  db.run("INSERT INTO canned_replies (slug, title, body) VALUES ('batch-cadence', 'Batch cadence', 'Hi {{name}}, it ships this week.')");
  const t = openTicket(db, { email: 'q@example.test', subject: 'Any shipping update on my order?', body: 'When does the batch go out?', cfg, autoAck: false });
  s = suggestions(db, cfg, { now }); assert.equal(s[0].key, `reply:${t.number}`); assert.equal(s[0].action.href, `/tickets/${t.id}/reply`); assert.equal(s[0].action.fields.canned, 'batch-cadence'); assert.match(s[0].title, /Batch cadence/);
  db.run('UPDATE tickets SET first_response_at = ? WHERE id = ?', '2026-10-22T01:00:00.000Z', t.id); assert.equal(suggestions(db, cfg, { now }).length, 0);
  // shipped a week ago with no carrier answer
  const { order } = recordOrder(db, sampleOrder(), { catalog: cat }); transition(db, order.id, 'packed'); transition(db, order.id, 'shipped', { carrier: 'USPS', tracking: 'T', cfg, at: '2026-10-10T10:00:00.000Z' });
  s = suggestions(db, cfg, { now }); assert.equal(s[0].key, `delivered:${order.number}`); assert.deepEqual(s[0].action.fields, { to: 'delivered', _back: '/' }); assert.match(s[0].detail, /12 days ago/);
  db.run("UPDATE orders SET tracking_status = 'in_transit' WHERE id = ?", order.id); assert.equal(suggestions(db, cfg, { now }).length, 0, 'the carrier is answering: not our guess to make');
  // an overdue invoice without a reminder this week
  const inv = createInvoice(db, { email: 'buyer@example.test', name: 'B', items: [{ sku: 'wap-kit', description: 'kit', qty: 1, unit_cents: 6900 }], terms_days: 1 }); issueInvoice(db, inv.id, cfg); db.run("UPDATE invoices SET due_at = '2026-10-01' WHERE id = ?", inv.id);
  s = suggestions(db, cfg, { now }); assert.equal(s[0].key, `invoice:${inv.number}`); assert.equal(s[0].action.href, `/invoices/${inv.id}/remind`);
  remindInvoice(db, inv.id, cfg); assert.equal(suggestions(db, cfg, { now }).length, 0, 'reminded this week'); assert.equal(db.get('SELECT ref FROM mail_outbox ORDER BY id DESC LIMIT 1').ref, `invoice-reminder:${inv.number}`);
  // the queue is reachable over the API and each action posts with _back honored by the admin
  const app = createApp({ db, cfg }); const addr = await app.listen();
  try { const r = await fetch(`http://127.0.0.1:${addr.port}/api/suggestions`, { headers: { authorization: `Bearer ${createApiToken(db, 't')}` } }); assert.equal(r.status, 200); assert.deepEqual(await r.json(), []); } finally { await app.close(); db.close(); }
});

test('marketing in one view: the funnel, sources, sends, codes, and a tile map that fetches nothing', () => {
  const db = testDb(); const cat = catalog();
  recordOrder(db, sampleOrder(), { catalog: cat }); recordOrder(db, sampleOrder({ external_id: 'cs_2', paid_at: '2026-10-20T12:00:00.000Z' }), { catalog: cat });
  recordOrder(db, sampleOrder({ external_id: 'cs_3', email: 'bob@example.test', name: 'Bob', ship: { line1: '1 Way', city: 'Albany', state: 'NY', zip: '12207', country: 'US' } }), { catalog: cat });
  upsertContact(db, { email: 'lurker@example.test', source: 'waitlist' });
  const m = marketing(db, { days: 30 });
  assert.deepEqual([m.funnel.contacts, m.funnel.customers, m.funnel.repeat], [3, 2, 1]); assert.equal(m.sources.find(s => s.source === 'waitlist').contacts, 1);
  assert.deepEqual(m.byState, { MA: 1, NY: 1 }); assert.equal(m.salesByState.MA, 15400); assert.equal(m.ordersByState.MA, 2); assert.equal(m.cities[0].place, 'Boston, MA');
  assert.equal(m.attributed_cents, 0); assert.ok(Array.isArray(m.campaigns) && Array.isArray(m.automations) && Array.isArray(m.codes));
  const svg = stateTileMap(m.byState, { title: 'Customers by state' });
  assert.ok(svg.includes('<svg') && svg.includes('>MA<') && svg.includes('>NY<') && svg.includes('>WY<'), 'every state is a tile'); assert.ok(!/https?:\/\//.test(svg), 'nothing is fetched'); assert.ok(svg.includes('<title>MA: 1'), 'hover says the value'); assert.ok(svg.includes('viztable'), 'a table view exists');
  assert.ok(stateTileMap({}).includes('<svg'), 'an empty map still draws');
  db.close();
});

test('the operator\'s timezone: times render in it, a typed wall-clock time becomes the right UTC instant, and an unknown zone falls back to UTC', () => {
  assert.equal(setTimezone('America/New_York'), 'America/New_York');
  assert.equal(dt('2026-10-22T12:30:00.000Z'), '2026-10-22 08:30 EDT'); assert.equal(day('2026-10-22T02:30:00.000Z'), '2026-10-21', 'late evening in New York is still yesterday');
  assert.equal(zonedToUtc('2026-10-22T09:00'), '2026-10-22T13:00:00.000Z'); assert.equal(zonedToUtc('2026-01-22T09:00'), '2026-01-22T14:00:00.000Z', 'standard time in January');
  assert.equal(zonedToUtc('2026-03-08T02:30'), '2026-03-08T06:30:00.000Z', 'a time that does not exist on the spring-forward night resolves forward to an instant rather than failing');
  assert.equal(hourIn('America/New_York', new Date('2026-10-22T03:30:00.000Z')), 23); assert.equal(zonedToUtc('garbage'), null);
  assert.equal(setTimezone('Mars/Olympus_Mons'), 'UTC'); assert.equal(dt('2026-10-22T12:30:00.000Z'), '2026-10-22 12:30 UTC'); assert.equal(dt(null), '—');
  setTimezone('UTC');
});

test('the stock job treats a part a purchase order covers as decided, not as an alarm', async () => {
  const { runJob } = await import('../src/jobs.mjs');
  const db = testDb(); const cfg = testCfg(); const app = createApp({ db, cfg });
  upsertItem(db, { sku: 'board', name: 'Board', kind: 'part', on_hand: 1, reorder_point: 4, reorder_qty: 20, unit_cost_cents: 750, supplier: 'Distributor' });
  await runJob(app, 'stock'); assert.ok(openExceptions(db).some(x => x.key === 'stock:board'), 'low and uncovered: raised');
  const [po] = draftReorder(db); await runJob(app, 'stock'); assert.ok(!openExceptions(db).some(x => x.key === 'stock:board'), 'drafted: resolved'); assert.match(db.get("SELECT body FROM exceptions WHERE key = 'stock:board'").body || '', /./);
  cancelPo(db, po.id); await runJob(app, 'stock'); assert.ok(openExceptions(db).some(x => x.key === 'stock:board'), 'the draft was canceled: raised again');
  const p2 = createPo(db, { supplier: 'Distributor', items: [{ sku: 'board', qty: 20 }] }); markOrdered(db, p2.id); await runJob(app, 'stock'); assert.ok(!openExceptions(db).some(x => x.key === 'stock:board'), 'on order: resolved');
  db.close();
});
