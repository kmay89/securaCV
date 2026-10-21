import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp, createApiToken } from '../src/auth.mjs';
import { signStripePayload } from '../src/modules/stripe.mjs';
import { createInvoice, invoiceToken } from '../src/modules/invoices.mjs';
import { testDb, testCfg } from './helpers.mjs';

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
    assert.match(r.headers.get('set-cookie'), /HttpOnly; SameSite=Strict/);
    const csrf = db.get('SELECT csrf FROM sessions').csrf;
    r = await fetch(base + '/', { headers: { cookie } }); assert.equal(r.status, 200); assert.ok((await r.text()).includes('Dashboard'));
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

    // double opt-in click and one-click unsubscribe (RFC 8058 POST)
    const c = db.get('SELECT * FROM contacts WHERE email = ?', 'ada@example.test');
    assert.equal((await fetch(base + '/confirm/' + c.consent_token)).status, 200);
    assert.equal(db.get('SELECT consent_marketing FROM contacts WHERE id = ?', c.id).consent_marketing, 1);
    assert.equal((await fetch(base + '/u/' + c.unsubscribe_token, { method: 'POST', body: 'List-Unsubscribe=One-Click', headers: { 'content-type': 'application/x-www-form-urlencoded' } })).status, 200);
    assert.ok(db.get('SELECT unsubscribed_at FROM contacts WHERE id = ?', c.id).unsubscribed_at);
    assert.equal((await fetch(base + '/u/bogus')).status, 404);

    // Stripe webhook: unsigned 400, signed creates the order once
    const event = JSON.stringify({ type: 'checkout.session.completed', data: { object: { id: 'cs_live_1', payment_status: 'paid', currency: 'usd', amount_subtotal: 6900, amount_total: 7700, created: 1760000000, total_details: { amount_shipping: 800, amount_tax: 0 },
      customer_details: { email: 'ada@example.test', name: 'Ada' }, shipping_details: { name: 'Ada Lovelace', address: { line1: '1 Analytical Way', city: 'Boston', state: 'MA', postal_code: '02134', country: 'US' } },
      line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] } } } });
    assert.equal((await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json' } })).status, 400);
    const sig = signStripePayload(event, 'whsec_test');
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json', 'stripe-signature': sig } });
    assert.deepEqual(await r.json(), { received: true, order: 'EL-000001', created: true });
    r = await fetch(base + '/webhooks/stripe', { method: 'POST', body: event, headers: { 'content-type': 'application/json', 'stripe-signature': sig } });
    assert.equal((await r.json()).created, false, 'idempotent on redelivery');

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
    r = await fetch(base + '/api/snapshot', { headers: h }); assert.equal((await r.json()).orders.open, 0);
    r = await fetch(base + '/api/jobs/probes', { method: 'POST', headers: h }); assert.ok((await r.json()).ok);

    // invoice public page: token gated
    const inv = createInvoice(db, { email: 'b@c.test', items: [{ description: 'thing', qty: 1, unit_cents: 100 }] });
    assert.equal((await fetch(`${base}/i/${inv.number}/wrong`)).status, 404);
    r = await fetch(`${base}/i/${inv.number}/${invoiceToken('sekret', inv.number)}`); assert.equal(r.status, 200); assert.ok((await r.text()).includes(inv.number));

    // admin pages all render for a logged-in operator
    for (const p of ['/orders', '/orders/1', '/tickets', '/tickets/1', '/contacts', '/contacts/1', '/inventory', '/devices', '/warranty', '/invoices', '/invoices/1', '/campaigns', '/campaigns/outbox', '/books', '/status-admin', '/exceptions', '/settings', '/canned', '/orders/new', '/orders/import', '/tickets/new', '/orders.csv', '/books.csv']) {
      r = await fetch(base + p, { headers: { cookie } }); assert.equal(r.status, 200, p);
    }
    assert.equal((await fetch(base + '/orders/999', { headers: { cookie, accept: 'text/html' } })).status, 404);
    // logout kills the session
    r = await fetch(base + '/logout', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf }) });
    assert.equal(r.headers.get('location'), '/login');
    assert.equal((await fetch(base + '/orders', { redirect: 'manual', headers: { cookie } })).status, 303);
  } finally { await app.close(); db.close(); }
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
});
