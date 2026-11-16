import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp, createApiToken } from '../src/auth.mjs';
import { signStripePayload } from '../src/modules/stripe.mjs';
import { createInvoice, invoiceToken } from '../src/modules/invoices.mjs';
import { testDb, testCfg, catalog, sampleOrder, fixture } from './helpers.mjs';
import { recordOrder, orderById, timeline } from '../src/modules/orders.mjs';
import { orderToken } from '../src/modules/delivery.mjs';
import { mkdtempSync, rmSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';

async function boot() {
  const db = testDb(); const cfg = testCfg({ secret: 'sekret' });
  const app = createApp({ db, cfg }); const addr = await app.listen();
  const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const totpSecret = setupOperator(db, 'operator password 1');
  return { db, cfg, app, base, totpSecret };
}
const form = (o) => new URLSearchParams(o).toString();

test('end to end: login + CSRF, public forms, signed Stripe webhook, bearer API, unsubscribe, invoice link', async () => {
  const { db, app, base, totpSecret } = await boot();
  try {
    // unauthenticated admin → login; wrong code → back to login
    assert.equal((await fetch(base + '/orders', { redirect: 'manual' })).status, 303);
    let r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'operator password 1', code: '000000' }) });
    assert.equal(r.headers.get('location'), '/login?err=1');
    r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'operator password 1', code: totp(totpSecret) }) });
    assert.equal(r.headers.get('location'), '/');
    const cookie = r.headers.get('set-cookie').split(';')[0];
    assert.match(r.headers.get("set-cookie"), /HttpOnly; SameSite=Lax/); // Lax, not Strict: a link the phone scans must arrive logged in
    const csrf = db.get('SELECT csrf FROM sessions').csrf;
    r = await fetch(base + '/', { headers: { cookie } }); assert.equal(r.status, 200); const dash = await r.text(); assert.ok(dash.includes('class="hero"'));
    assert.ok(dash.includes(`action="/logout" class="inline"><input type="hidden" name="_csrf" value="${csrf}"`), 'the visible Log out button carries the CSRF token');
    // a POST without the CSRF token is refused; with it, it works
    r = await fetch(base + '/inventory/item', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded', accept: 'text/html' }, body: form({ sku: 'cable', name: 'cable', kind: 'part', reorder_point: 1, reorder_qty: 1, unit_cost_cents: 100 }) });
    assert.equal(r.status, 403);
    r = await fetch(base + '/inventory/item', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, sku: 'cable', name: 'cable', kind: 'part', reorder_point: 1, reorder_qty: 1, unit_cost_cents: 100 }) });
    assert.equal(r.status, 303); assert.ok(db.get('SELECT * FROM inventory_items WHERE sku = ?', 'cable'));

    // public forms: honeypot is swallowed; waitlist; contact → ticket; JSON + CORS
    r = await fetch(base + '/forms/waitlist', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'bot@x.test', sku: 'wap-kit', website: 'http://spam' }) });
    assert.equal(r.status, 200); assert.equal(db.get('SELECT COUNT(*) AS n FROM contacts').n, 0);
    r = await fetch(base + '/forms/waitlist', { method: 'POST', headers: { 'content-type': 'application/json', origin: 'https://example.test' }, body: JSON.stringify({ email: 'ada@example.test', sku: 'wap-kit', qty: 2, newsletter: true }) });
    assert.equal(r.status, 200); assert.equal(r.headers.get('access-control-allow-origin'), 'https://example.test');
    assert.equal(db.get('SELECT qty FROM waitlist').qty, 2);
    assert.ok(db.get('SELECT * FROM mail_outbox WHERE ref LIKE \'consent:%\''), 'double opt-in mail queued');
    r = await fetch(base + '/forms/contact', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'ada@example.test', name: 'Ada', subject: 'Does it do face recognition?', message: 'asking for a friend' }) });
    assert.equal((await r.json()).ticket, 'T-000001');
    r = await fetch(base + '/forms/contact', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'not-an-email', message: 'x' }) });
    assert.equal(r.status, 400);
    assert.equal((await fetch(base + '/forms/nope', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'a@b.test' }) })).status, 404);

    // the waitlist quantity is a whole number from 1 to 50 — never 1.5, 1e400 or a string the column would store as REAL
    for (const [i, qty] of [1.5, '1e400', 0, 51, 'x'].entries()) assert.equal((await fetch(base + '/forms/waitlist', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: `qty${i}@example.test`, sku: 'wap-kit', qty }) })).status, 400, `qty ${qty}`);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM waitlist').n, 1, 'none of them joined');
    // the form's acknowledgement has a fixed subject and no name: the form cannot relay a line into a stranger's inbox
    const ack = db.get("SELECT subject, body_text FROM mail_outbox WHERE ref = 'ticket:T-000001'"); assert.match(ack.subject, /^\[T-000001 [a-z0-9]{6}\] We got your message$/); assert.ok(!ack.subject.includes('face recognition')); assert.match(ack.body_text, /^Hi there,/);
    // double opt-in: the link's GET only asks (a scanner confirms nobody); the button's POST confirms; one-click unsubscribe (RFC 8058 POST)
    const c = db.get('SELECT * FROM contacts WHERE email = ?', 'ada@example.test');
    r = await fetch(base + '/confirm/' + c.consent_token); assert.equal(r.status, 200); assert.match(await r.text(), /<form method="post" action="\/confirm\//);
    assert.equal(db.get('SELECT consent_marketing FROM contacts WHERE id = ?', c.id).consent_marketing, 0, 'a GET changes nothing');
    assert.equal((await fetch(base + '/confirm/' + c.consent_token, { method: 'POST' })).status, 200);
    assert.equal(db.get('SELECT consent_marketing FROM contacts WHERE id = ?', c.id).consent_marketing, 1);
    assert.equal((await fetch(base + '/confirm/' + c.consent_token, { method: 'POST' })).status, 404, 'a token is spent by its confirmation');
    r = await fetch(base + '/u/' + c.unsubscribe_token); assert.equal(r.status, 200); assert.match(await r.text(), /<form method="post" action="\/u\//); assert.equal(db.get('SELECT unsubscribed_at FROM contacts WHERE id = ?', c.id).unsubscribed_at, null, 'a GET unsubscribes nobody');
    assert.equal((await fetch(base + '/u/' + c.unsubscribe_token, { method: 'POST', body: 'List-Unsubscribe=One-Click', headers: { 'content-type': 'application/x-www-form-urlencoded' } })).status, 200);
    assert.ok(db.get('SELECT unsubscribed_at FROM contacts WHERE id = ?', c.id).unsubscribed_at);
    assert.equal((await fetch(base + '/u/bogus')).status, 404); assert.equal((await fetch(base + '/u/bogus', { method: 'POST' })).status, 404);

    // Stripe webhook: unsigned 400, signed creates the order once
    const event = JSON.stringify({ type: 'checkout.session.completed', data: { object: { id: 'cs_live_1', payment_status: 'paid', currency: 'usd', amount_subtotal: 6900, amount_total: 7700, created: 1760000000, total_details: { amount_shipping: 800, amount_tax: 0 },
      customer_details: { email: 'ada@example.test', name: 'Ada' }, shipping_details: { name: 'Ada Lovelace', address: { line1: '1 Analytical Way', city: 'Boston', state: 'MA', postal_code: '02134', country: 'US' } },
      line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] } } } });
    assert.equal((await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json' } })).status, 400);
    const sig = signStripePayload(event, 'whsec_test');
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json', 'stripe-signature': sig } });
    assert.deepEqual(await r.json(), { received: true, order: 'EL-000001', created: true, items: 1 });
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json', 'stripe-signature': sig } });
    assert.equal((await r.json()).created, false, 'idempotent on redelivery (same session, no event id)');
    // a checkout session without inline line_items: they are fetched from Stripe (fake), the order gets items and its PaymentIntent
    app.cfg.stripeApiKey = 'sk_test_x';
    app.fetchImpl = async (url) => { assert.match(url, /\/v1\/checkout\/sessions\/cs_live_2\/line_items/); return { ok: true, json: async () => ({ data: [{ description: 'Canary WAP Kit', quantity: 2, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] }) }; };
    const bare = JSON.stringify({ id: 'evt_2', type: 'checkout.session.completed', data: { object: { id: 'cs_live_2', payment_intent: 'pi_live_2', payment_status: 'paid', currency: 'usd', amount_subtotal: 13800, amount_total: 14600, total_details: { amount_shipping: 800, amount_tax: 0 },
      customer_details: { email: 'ada@example.test', name: 'Ada' }, shipping_details: { name: 'Ada Lovelace', address: { line1: '1 Analytical Way', city: 'Boston', state: 'MA', postal_code: '02134', country: 'US' } } } } });
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: bare, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(bare, 'whsec_test') } });
    assert.deepEqual(await r.json(), { received: true, order: 'EL-000002', created: true, items: 1 });
    assert.equal(db.get('SELECT qty FROM order_items WHERE order_id = 2').qty, 2);
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: bare, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(bare, 'whsec_test') } });
    assert.deepEqual(await r.json(), { received: true, duplicate: true }, 'an event id is applied once');
    // a refund names the PaymentIntent, and still finds the Checkout order
    const refundEv = JSON.stringify({ id: 'evt_3', type: 'charge.refunded', data: { object: { id: 'ch_1', payment_intent: 'pi_live_2', amount_refunded: 800 } } });
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: refundEv, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(refundEv, 'whsec_test') } });
    assert.deepEqual(await r.json(), { received: true, order: 'EL-000002', refunded_cents: 800 });
    // invoice.paid for a subscription posts once, however often it is delivered
    const sub = JSON.stringify({ id: 'evt_4', type: 'invoice.paid', data: { object: { id: 'in_1', subscription: 'sub_1', amount_paid: 9900, created: 1760000000 } } });
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: sub, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(sub, 'whsec_test') } });
    assert.equal((await r.json()).posted, true);
    const sub2 = sub.replace('evt_4', 'evt_5');
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: sub2, headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(sub2, 'whsec_test') } });
    assert.equal((await r.json()).posted, false, 'same invoice under a new event id posts nothing');
    assert.equal(db.get('SELECT COUNT(*) AS n FROM ledger WHERE ref = ?', 'subscription:sub_1:in_1').n, 2, 'one balanced pair');

    // API: no token 401, token works, transition ships + queues mail
    assert.equal((await fetch(base + '/api/orders')).status, 401);
    const token = createApiToken(db, 'claude');
    const h = { authorization: `Bearer ${token}`, 'content-type': 'application/json' };
    r = await fetch(base + '/api/orders/EL-000001', { headers: h }); assert.equal((await r.json()).status, 'paid');
    r = await fetch(base + '/api/orders/EL-000001/transition', { method: 'POST', headers: h, body: JSON.stringify({ to: 'packed' }) }); assert.equal((await r.json()).status, 'packed');
    r = await fetch(base + '/api/orders/EL-000001/transition', { method: 'POST', headers: h, body: JSON.stringify({ to: 'shipped', carrier: 'USPS', tracking: '9400 1' }) }); assert.equal((await r.json()).status, 'shipped');
    assert.ok(db.get('SELECT * FROM mail_outbox WHERE ref = ?', 'order:EL-000001'));
    r = await fetch(base + '/api/orders/EL-000001/transition', { method: 'POST', headers: h, body: JSON.stringify({ to: 'paid' }) }); assert.equal(r.status, 400, 'a refused transition is the caller\'s error'); assert.match((await r.json()).error, /cannot move/);
    r = await fetch(base + '/api/tickets/T-000001/reply', { method: 'POST', headers: h, body: JSON.stringify({ body: 'No. It structurally cannot — see the FAQ.', close: true }) });
    assert.equal((await r.json()).status, 'closed');
    r = await fetch(base + '/api/snapshot', { headers: h }); assert.equal((await r.json()).orders.open, 1, 'EL-000002 is paid and waiting; EL-000001 shipped');
    r = await fetch(base + '/api/jobs/probes', { method: 'POST', headers: h }); assert.ok((await r.json()).ok);

    // invoice public page: token gated
    const inv = createInvoice(db, { email: 'b@c.test', items: [{ description: 'thing', qty: 1, unit_cents: 100 }] });
    assert.equal((await fetch(`${base}/i/${inv.number}/wrong`)).status, 404);
    r = await fetch(`${base}/i/${inv.number}/${invoiceToken('sekret', inv.number)}`); assert.equal(r.status, 200); assert.ok((await r.text()).includes(inv.number));

    // admin pages all render for a logged-in operator
    for (const p of ['/orders', '/orders?status=open&tag=rush&from=2020-01-01&to=2030-01-01', '/orders/1', '/orders/1/packing-slip', '/orders/packing-slips', '/batches', '/tickets', '/tickets/1', '/contacts', '/contacts?segment=repeat&sort=spent', '/contacts/1', '/products', '/products/wap-kit', '/discounts', '/discounts?try=launch10&subtotal=6900', '/analytics', '/analytics?days=90', '/search?q=EL-', '/inventory', '/devices', '/warranty', '/invoices', '/invoices/1', '/campaigns', '/campaigns/outbox', '/books', '/status-admin', '/exceptions', '/settings', '/canned', '/orders/new', '/orders/import', '/tickets/new', '/orders.csv', '/books.csv', '/products.csv', '/contacts.csv', '/analytics.csv']) {
      r = await fetch(base + p, { headers: { cookie } }); assert.equal(r.status, 200, p);
    }
    assert.equal((await fetch(base + '/orders/999', { headers: { cookie, accept: 'text/html' } })).status, 404);
    // logout kills the session
    r = await fetch(base + '/logout', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf }) });
    assert.equal(r.headers.get('location'), '/login');
    assert.equal((await fetch(base + '/orders', { redirect: 'manual', headers: { cookie } })).status, 303);
  } finally { await app.close(); db.close(); }
});

// The signed link shows the order; it does not prove who holds it. Changing where the parcel goes also takes the
// email the order was placed with, which the page never prints.
test('the signed status link alone moves no parcel: the address form asks for the order\'s email, and without it the order is unchanged', async () => {
  const { db, cfg, app, base } = await boot();
  try {
    const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_addr_email' }), { catalog: catalog(), actor: 'test' });
    const url = `${base}/o/${order.number}/${orderToken(cfg.secret, order.number)}`;
    const page = await (await fetch(url)).text();
    assert.ok(page.includes('Need to change the address?') && page.includes('name="email"'), 'the form asks for the email'); assert.ok(!page.includes('ada@example.test'), 'and the page does not print it');
    const moved = { name: 'Mallory', line1: '9 Elsewhere Rd', city: 'Albany', state: 'NY', zip: '12207' };
    const post = (extra) => fetch(url + '/address', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ ...moved, ...extra }) });
    for (const [what, extra] of [['no email', {}], ['a blank email', { email: '  ' }], ['somebody else\'s email', { email: 'mallory@example.test' }]]) {
      const r = await post(extra); assert.equal(r.status, 400, what); assert.match(await r.text(), /not the email this order was placed with/, what);
      const o = orderById(db, order.id); assert.deepEqual([o.ship_name, o.ship_line1, o.ship_city, o.ship_zip], [order.ship_name, '1 Analytical Way', 'Boston', '02134'], `${what}: the order is unchanged`);
    }
    assert.ok(!timeline(db, order.id).some(ev => ev.kind === 'address'), 'nothing on the timeline'); assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'order.address'").n, 0, 'nothing in the audit log');
    // the email it was placed with, however it is typed
    const r = await post({ email: ' ADA@Example.Test ' }); assert.equal(r.status, 303); assert.ok(r.headers.get('location').endsWith('?ok=address'));
    assert.equal(orderById(db, order.id).ship_line1, '9 Elsewhere Rd'); assert.ok(timeline(db, order.id).some(ev => ev.kind === 'address' && ev.actor === 'customer'));
    // an order with no email on file has nothing to compare with: no form, and the POST is refused
    const { order: bare } = recordOrder(db, sampleOrder({ external_id: 'cs_addr_bare' }), { catalog: catalog(), actor: 'test' }); db.run('UPDATE orders SET contact_id = NULL WHERE id = ?', bare.id);
    const bareUrl = `${base}/o/${bare.number}/${orderToken(cfg.secret, bare.number)}`;
    assert.ok(!(await (await fetch(bareUrl)).text()).includes('Need to change the address?'));
    const rb = await fetch(bareUrl + '/address', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ ...moved, email: '' }) });
    assert.equal(rb.status, 400); assert.equal(orderById(db, bare.id).ship_line1, '1 Analytical Way');
  } finally { await app.close(); db.close(); }
});

// A website form with a bad field is answered where it was posted, with the reason. Its Referer is a page on the
// website; redirecting to that path on this host was a 404 on the admin's origin, with the reason lost.
test('a public form with a bad field answers with the reason, never a redirect to the website\'s path on this host; an admin form still goes back to its page', async () => {
  const { db, app, base, totpSecret } = await boot();
  try {
    const bad = (path, body, referer) => fetch(base + path, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'text/html,application/xhtml+xml', ...(referer ? { referer } : {}) }, body: form(body) });
    // the Referer a cross-origin form sends by default (the origin only), and the full path an older policy sends
    for (const [i, referer] of ['https://example.test/', 'https://example.test/contact', 'https://example.test/store.html', null].entries()) {
      let r = await bad('/forms/contact', { email: 'not-an-email', message: 'hello' }, referer);
      assert.equal(r.status, 400, String(referer)); assert.equal(r.headers.get('location'), null, 'no redirect'); assert.match(await r.text(), /a valid email is required/);
      r = await bad('/forms/contact', { email: `ada${i}@example.test`, message: '' }, referer); assert.equal(r.status, 400); assert.equal(r.headers.get('location'), null); assert.match(await r.text(), /message required/);
      r = await bad('/forms/waitlist', { email: `ada${i}@example.test` }, referer); assert.equal(r.status, 400); assert.equal(r.headers.get('location'), null); assert.match(await r.text(), /sku required/);
    }
    // even a Referer that names this host is not followed without a session: the door does not bounce strangers around the admin
    let r = await bad('/forms/contact', { email: 'nope', message: 'x' }, `${base}/orders`); assert.equal(r.status, 400); assert.equal(r.headers.get('location'), null);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM tickets').n, 0); assert.equal(db.get('SELECT COUNT(*) AS n FROM contacts').n, 0);
    // a person with a session, posting an admin form from an admin page: back to that page with the reason, as before
    r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'operator password 1', code: totp(totpSecret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const admin = (referer) => fetch(base + '/inventory/move', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded', accept: 'text/html', referer }, body: form({ _csrf: csrf, sku: 'no-such-part', delta: '1', reason: 'test' }) });
    r = await admin(`${base}/inventory`); assert.equal(r.status, 303); assert.match(r.headers.get('location'), /^\/inventory\?err=/);
    // the same person, the same mistake, but a Referer from another site: the reason on this page, not a path that site chose
    r = await admin('https://example.test/inventory'); assert.notEqual(r.status, 303); assert.equal(r.headers.get('location'), null); assert.ok(r.status >= 400 && r.status < 500);
  } finally { await app.close(); db.close(); }
});

// The waitlist form posts an id; the thanks page names the product as the catalog does. An id from store.json's
// `waitlist.products` (what a form may enroll for that is not priced or sold yet) has a name and nothing else:
// it is never in the storefront's catalog.
test('the waitlist thanks page names the product, and a waitlist-only row is never listed or sold', async () => {
  const work = mkdtempSync(join(tmpdir(), 'bo-waitlist-'));
  const store = JSON.parse(readFileSync(fixture('store.json'), 'utf8'));
  store.waitlist = { products: [{ id: 'displays-kit', name: 'Watch Station & Canary Dash', link: '', radio: true, stock: 0, hidden: true, authorization: 'pending-fcc' }, { id: 'wap-kit', name: 'A name that must not replace the product\'s' }] };
  const storeJson = join(work, 'store.json'); writeFileSync(storeJson, JSON.stringify(store));
  const db = testDb(); const cfg = testCfg({ secret: 'sekret', storeJson }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const join_ = (body) => fetch(base + '/forms/waitlist', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'text/html' }, body: form(body) });
    let r = await join_({ email: 'ada@example.test', sku: 'displays-kit' }); assert.equal(r.status, 200); let page = await r.text();
    assert.ok(page.includes('on the list for Watch Station &amp; Canary Dash'), 'the name, escaped'); assert.ok(!page.includes('displays-kit'), 'not the raw id');
    assert.equal(db.get('SELECT sku FROM waitlist').sku, 'displays-kit', 'the id is what is stored');
    r = await join_({ email: 'ada@example.test', sku: 'wap-kit' }); page = await r.text(); assert.ok(page.includes('on the list for Canary WAP Kit'), 'a priced product keeps its own name');
    r = await join_({ email: 'ada@example.test', sku: 'no-such-thing' }); assert.ok((await r.text()).includes('on the list for no-such-thing'), 'an id the catalog does not know is printed as posted');
    // never listed, never sold: not in the storefront's catalog, not a line a checkout accepts
    const cat = await (await fetch(base + '/store/catalog')).json();
    assert.ok(cat.products.length > 0 && !cat.products.some(p => p.sku === 'displays-kit'), '/store/catalog does not list it'); assert.ok(!JSON.stringify(cat).includes('displays-kit'));
    const loaded = await app.catalog(); assert.equal(loaded.products.has('displays-kit'), false); assert.equal(loaded.waitlist.get('displays-kit').name, 'Watch Station & Canary Dash'); assert.equal(loaded.waitlist.has('wap-kit'), false);
    r = await fetch(base + '/store/checkout', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ items: [{ sku: 'displays-kit', qty: 1 }], email: 'ada@example.test' }) }); assert.ok(r.status >= 400, 'a checkout for it is refused');
    // nor on the admin's Products list or its CSV: there is no product to price, stock or pack
    const secret = setupOperator(db, 'operator password 1'); r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'operator password 1', code: totp(secret) }) }); const cookie = r.headers.get('set-cookie').split(';')[0];
    for (const p of ['/products', '/products.csv', '/api/products']) { r = await fetch(base + p, { headers: { cookie } }); assert.equal(r.status, 200, p); const body = await r.text(); assert.ok(body.includes('wap-kit') && !body.includes('displays-kit') && !body.includes('Watch Station'), `${p} does not list it`); }
    assert.equal((await fetch(base + '/products/displays-kit', { headers: { cookie } })).status, 404, 'and it has no product page');
  } finally { await app.close(); db.close(); rmSync(work, { recursive: true, force: true }); }
});

test('redirect targets: only the site origin for forms, only a local path for the Referer', async () => {
  const { sameOriginUrl, safeLocalPath } = await import('../src/server.mjs');
  assert.equal(sameOriginUrl('https://example.test/store?joined=1', 'https://example.test'), 'https://example.test/store?joined=1');
  assert.equal(sameOriginUrl('https://example.test.evil.net/', 'https://example.test'), null);
  assert.equal(sameOriginUrl('//evil.net/', 'https://example.test'), null);
  assert.equal(sameOriginUrl('javascript:alert(1)', 'https://example.test'), null);
  assert.equal(safeLocalPath('http://ops.test/orders/12'), '/orders/12');
  assert.equal(safeLocalPath('http://ops.test//evil.net/x'), null, 'a protocol-relative path never becomes a redirect');
  assert.equal(safeLocalPath('not a url'), null);
  // with this server's hosts named, a Referer from anywhere else is not a path here
  assert.equal(safeLocalPath('https://ops.test/orders/12', ['ops.test']), '/orders/12'); assert.equal(safeLocalPath('https://example.test/contact', ['ops.test']), null); assert.equal(safeLocalPath('https://ops.test.evil.net/orders', ['ops.test']), null);
});
