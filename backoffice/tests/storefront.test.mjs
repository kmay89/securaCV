// The storefront's server half: the batch cadence in the operator's zone, ZIP → state, the
// estimate a shopper sees before paying, what is sellable now (cap minus sold minus what the
// shelf can build), the Checkout Session Stripe is asked for (every line checked, the ship date
// and arrival window on the shipping option), and the public routes with CORS to the site.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { nextShipDate, stateForZip, estimateForZip, availableNow, storefrontCatalog, createCheckout, sessionStatus, formEncode, StoreError } from '../src/modules/storefront.mjs';
import { recordOrder, transition } from '../src/modules/orders.mjs';
import { upsertItem, setRecipe } from '../src/modules/inventory.mjs';
import { DEMO_CATALOG, seedDemo } from '../src/modules/demo.mjs';
import { normalize } from '../src/modules/catalog.mjs';
import { createApp } from '../src/server.mjs';
import { signStripePayload } from '../src/modules/stripe.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const boston = { tz: 'America/New_York', batchCloseDay: 0, batchCloseHour: 23, batchShipDay: 2, shipFromState: 'MA' };

test('the batch cadence: orders by Sunday night ship Tuesday, in the operator\'s zone, and a Sunday past the close is next week', () => {
  // Wed Oct 21 2026 15:00 Boston → closes Sun Oct 25, ships Tue Oct 27
  let s = nextShipDate(new Date('2026-10-21T19:00:00Z'), boston); assert.equal(s.closes_on, '2026-10-25'); assert.equal(s.ships_on, '2026-10-27'); assert.match(s.label, /^Order by Sunday 11 pm · ships Tue, Oct 27$/);
  // Sun Oct 25 22:30 Boston: still this batch, "tonight"
  s = nextShipDate(new Date('2026-10-26T02:30:00Z'), boston); assert.equal(s.closes_on, '2026-10-25'); assert.equal(s.ships_on, '2026-10-27'); assert.match(s.label, /^Order by tonight 11 pm/);
  // Sun Oct 25 23:30 Boston (03:30 Monday UTC): past the close, next week's batch
  s = nextShipDate(new Date('2026-10-26T03:30:00Z'), boston); assert.equal(s.closes_on, '2026-11-01'); assert.equal(s.ships_on, '2026-11-03');
  // the same instant counted in UTC is Monday: next Sunday
  s = nextShipDate(new Date('2026-10-26T03:30:00Z'), { ...boston, tz: 'UTC' }); assert.equal(s.closes_on, '2026-11-01');
  // a Friday close that ships Monday
  s = nextShipDate(new Date('2026-10-21T19:00:00Z'), { ...boston, closeDay: 5, closeHour: 17, shipDay: 1 }); assert.equal(s.closes_on, '2026-10-23'); assert.equal(s.ships_on, '2026-10-26'); assert.match(s.label, /Friday 5 pm · ships Mon, Oct 26/);
  // ship day equal to close day ships a week later, never the same day
  s = nextShipDate(new Date('2026-10-21T19:00:00Z'), { ...boston, closeDay: 0, shipDay: 0 }); assert.equal(s.ships_on, '2026-11-01');
});

test('ZIP → state by prefix; a bad ZIP is null, not a guess', () => {
  assert.equal(stateForZip('02134'), 'MA'); assert.equal(stateForZip('53703'), 'WI'); assert.equal(stateForZip('94110-1234'), 'CA'); assert.equal(stateForZip('99501'), 'AK'); assert.equal(stateForZip('20500'), 'DC'); assert.equal(stateForZip('00501'), 'NY');
  assert.equal(stateForZip('1234'), null); assert.equal(stateForZip('abcde'), null); assert.equal(stateForZip(''), null); assert.equal(stateForZip('00001'), null);
});

test('the estimate before paying: ship date from the cadence, arrival window from the same bands the order page uses', () => {
  const now = new Date('2026-10-21T19:00:00Z'); const cfg = { ...boston };
  const near = estimateForZip('02134', { now, cfg }); assert.equal(near.state, 'MA'); assert.equal(near.ships.ships_on, '2026-10-27'); assert.equal(near.delivers.min_days, 2); assert.equal(near.delivers.max_days, 4); assert.match(near.label, /Order by Sunday 11 pm · ships Tue, Oct 27 · arrives /);
  const far = estimateForZip('98101', { now, cfg }); assert.equal(far.state, 'WA'); assert.ok(far.delivers.min_days > near.delivers.min_days, 'the west coast takes longer from Boston');
  const none = estimateForZip('', { now, cfg }); assert.equal(none.state, null); assert.ok(none.delivers, 'no ZIP still gets a window (the honest middle)'); assert.equal(none.basis, 'estimate');
});

test('what is sellable now: the batch cap minus this batch\'s orders, never more than the shelf builds; pending-FCC never', () => {
  // `now` sits in the same ISO week as sampleOrder's paid date, so "this batch" is the batch the sample order lands in whatever day the test runs
  const db = testDb(); const cat = DEMO_CATALOG; const cfg = { ...boston, tz: 'UTC' }; const now = new Date('2026-10-21T12:00:00Z');
  const parts = cat.products.get('wap-parts'); assert.equal(parts.stock, 15);
  assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 15, 'the cap, with nothing sold and no recipe');
  recordOrder(db, sampleOrder({ external_id: 'a', items: [{ sku: 'wap-parts', name: 'p', qty: 4, unit_cents: 1500 }] }), { catalog: cat });
  assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 11, 'four sold this batch');
  upsertItem(db, { sku: 'wap-shell-petg', name: 'shell', on_hand: 10 }); upsertItem(db, { sku: 'hardware-pack-wap', name: 'hw', on_hand: 30 }); setRecipe(db, 'wap-parts', { 'wap-shell-petg': 1, 'hardware-pack-wap': 1 });
  assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 6, 'ten shells, four of them spoken for by the paid order that is not packed yet');
  const sf = storefrontCatalog(db, cat, { now, cfg });
  const p = sf.products.find(x => x.sku === 'wap-parts'); assert.equal(p.available, 6); assert.ok(p.sellable); assert.equal(p.reason, null);
  const gated = storefrontCatalog(db, normalize({ mode: 'live', shipping_flat: 8, groups: [{ products: [{ id: 'vision-kit', name: 'Vision', price: 119, authorization: 'pending-fcc' }] }] }), { now, cfg });
  assert.equal(gated.products[0].sellable, false); assert.equal(gated.products[0].reason, 'pending-fcc');
  assert.ok(sf.ships.ships_on && sf.shipping_flat_cents === 800 && sf.checkout === 'links', 'no API key: the site falls back to payment links');
  // a canceled order gives its units back; sold out says so
  const o2 = recordOrder(db, sampleOrder({ external_id: 'b', items: [{ sku: 'wap-parts', name: 'p', qty: 6, unit_cents: 1500 }] }), { catalog: cat }).order;
  assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 0); assert.equal(storefrontCatalog(db, cat, { now, cfg }).products.find(x => x.sku === 'wap-parts').reason, 'batch-full');
  transition(db, o2.id, 'canceled'); assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 6);
  // packing the first order moves its claim from "committed" to "consumed": the shelf drops, the number holds
  transition(db, recordOrder(db, sampleOrder({ external_id: 'a' }), { catalog: cat }).order.id, 'packed'); assert.equal(availableNow(db, parts, { now, tz: 'UTC' }), 6);
  db.close();
});

test('createCheckout: every line checked, the session Stripe is asked for carries the sku, the ship date and the window, and a Stripe failure charges nothing', async () => {
  const db = testDb(); const cfg = { ...boston, stripeApiKey: 'sk_test_x', siteOrigin: 'https://securacv.com', checkoutSuccessPath: '/store/thanks', product: 'SecuraCV' }; const now = new Date('2026-10-21T19:00:00Z');
  const asked = []; const stripe = async (url, init) => { asked.push({ url, headers: init.headers, body: Object.fromEntries(new URLSearchParams(init.body)) }); return { ok: true, json: async () => ({ id: 'cs_test_abc123', url: 'https://checkout.stripe.com/c/pay/cs_test_abc123' }) }; };
  const r = await createCheckout(db, cfg, DEMO_CATALOG, { items: [{ sku: 'wap-parts', qty: 2 }, { sku: 'vision-parts', qty: 1 }, { sku: 'wap-parts', qty: 1 }], zip: '53703', email: 'first.customer@customer.example.test' }, { fetchImpl: stripe, now });
  assert.equal(r.url, 'https://checkout.stripe.com/c/pay/cs_test_abc123'); assert.deepEqual(r.lines.map(l => [l.sku, l.qty]), [['wap-parts', 3], ['vision-parts', 1]], 'the same sku twice is one line');
  const b = asked[0].body; assert.equal(asked[0].url, 'https://api.stripe.com/v1/checkout/sessions'); assert.equal(asked[0].headers.authorization, 'Bearer sk_test_x'); assert.ok(asked[0].headers['idempotency-key']);
  assert.equal(b.mode, 'payment'); assert.equal(b['line_items[0][price_data][unit_amount]'], '1500'); assert.equal(b['line_items[0][price_data][product_data][metadata][sku]'], 'wap-parts'); assert.equal(b['line_items[0][quantity]'], '3'); assert.equal(b['line_items[1][price_data][product_data][metadata][sku]'], 'vision-parts');
  assert.equal(b['shipping_address_collection[allowed_countries][0]'], 'US'); assert.equal(b['shipping_options[0][shipping_rate_data][fixed_amount][amount]'], '800'); assert.match(b['shipping_options[0][shipping_rate_data][display_name]'], /USPS Ground Advantage · ships Tue, Oct 27/);
  assert.equal(b['shipping_options[0][shipping_rate_data][delivery_estimate][minimum][unit]'], 'business_day'); assert.ok(Number(b['shipping_options[0][shipping_rate_data][delivery_estimate][maximum][value]']) >= Number(b['shipping_options[0][shipping_rate_data][delivery_estimate][minimum][value]']));
  assert.equal(b.success_url, 'https://securacv.com/store/thanks?session_id={CHECKOUT_SESSION_ID}'); assert.equal(b.cancel_url, 'https://securacv.com/store'); assert.equal(b.customer_email, 'first.customer@customer.example.test'); assert.equal(b.allow_promotion_codes, 'true');
  assert.equal(b['metadata[skus]'], '3x wap-parts, 1x vision-parts'); assert.equal(b['metadata[ships_on]'], '2026-10-27'); assert.equal(b['automatic_tax[enabled]'], undefined, 'TAX_MODE none asks Stripe for no tax');
  const taxed = await createCheckout(db, { ...cfg, taxMode: 'stripe' }, DEMO_CATALOG, { items: [{ sku: 'wap-parts', qty: 1 }] }, { fetchImpl: stripe, now }); assert.ok(taxed.id); assert.equal(asked.at(-1).body['automatic_tax[enabled]'], 'true');
  assert.ok(db.get("SELECT 1 FROM audit_log WHERE action = 'store.checkout'"));
  // refusals, by name, before Stripe is asked
  const n = asked.length; const refuse = async (items, re) => { await assert.rejects(() => createCheckout(db, cfg, DEMO_CATALOG, { items }, { fetchImpl: stripe, now }), (e) => e instanceof StoreError && re.test(e.message)); };
  await refuse([], /cart is empty/); await refuse([{ sku: 'wap-parts', qty: 0 }], /quantity is a whole number from 1 to 10/); await refuse([{ sku: 'wap-parts', qty: 1.5 }], /whole number/); await refuse([{ sku: 'wap-parts', qty: 11 }], /from 1 to 10/);
  await refuse([{ sku: 'mystery', qty: 1 }], /not in the catalog/);
  const gated = normalize({ mode: 'live', shipping_flat: 8, groups: [{ products: [{ id: 'wap-kit', name: 'Canary WAP Kit', price: 69, authorization: 'pending-fcc' }] }] });
  await assert.rejects(() => createCheckout(db, cfg, gated, { items: [{ sku: 'wap-kit', qty: 1 }] }, { fetchImpl: stripe, now }), /not for sale until its FCC authorization/);
  await refuse(Array.from({ length: 21 }, () => ({ sku: 'wap-parts', qty: 1 })), /more lines/); await refuse([{ sku: 'wap-parts', qty: 10 }, { sku: 'wap-parts', qty: 10 }], /only 15 left in this batch|more units/);
  assert.equal(asked.length, n, 'none of those reached Stripe');
  await assert.rejects(() => createCheckout(db, { ...cfg, stripeApiKey: '' }, DEMO_CATALOG, { items: [{ sku: 'wap-parts', qty: 1 }] }, { fetchImpl: stripe, now }), /not configured/);
  // Stripe says no: the shopper hears "nothing was charged", the audit log has the reason
  const down = async () => ({ ok: false, status: 400, json: async () => ({ error: { type: 'invalid_request_error', message: 'synthetic failure' } }) });
  await assert.rejects(() => createCheckout(db, cfg, DEMO_CATALOG, { items: [{ sku: 'wap-parts', qty: 1 }] }, { fetchImpl: down, now }), /nothing was charged/);
  assert.match(db.get("SELECT detail FROM audit_log WHERE action = 'store.checkout.failed' ORDER BY id DESC LIMIT 1").detail, /synthetic failure/);
  // the thanks page before and after the webhook
  assert.deepEqual(sessionStatus(db, { ...cfg, secret: 's' }, 'cs_test_abc123'), { pending: true }); assert.throws(() => sessionStatus(db, cfg, 'nope'), /not a checkout session id/);
  const o = recordOrder(db, sampleOrder({ external_id: 'cs_test_abc123', items: [{ sku: 'wap-parts', name: 'p', qty: 3, unit_cents: 1500 }] }), { catalog: DEMO_CATALOG }).order;
  const st = sessionStatus(db, { ...cfg, secret: 's', baseUrl: 'https://ops.example.test' }, 'cs_test_abc123', { now }); assert.equal(st.pending, false); assert.equal(st.order, o.number); assert.equal(st.ships.ships_on, '2026-10-27'); assert.match(st.status_url, /^https:\/\/ops\.example\.test\/o\/EL-000001\//); assert.equal(st.email, undefined, 'the thanks page never echoes the address');
  assert.equal(formEncode({ a: { b: [1, 2], c: null }, d: 'x y' }), 'a%5Bb%5D%5B0%5D=1&a%5Bb%5D%5B1%5D=2&d=x%20y');
  db.close();
});

test('over HTTP: the store routes answer the site with CORS, the demo takes no payments, the webhook records a site order with its "we have it" mail', async () => {
  const cfg = testCfg({ demo: true, siteOrigin: 'https://securacv.com' }); const db = testDb(); await seedDemo(db, cfg); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    let r = await fetch(base + '/store/catalog', { headers: { origin: 'https://securacv.com' } }); assert.equal(r.status, 200); assert.equal(r.headers.get('access-control-allow-origin'), cfg.siteOrigin, 'CORS to the site origin (the demo listener points it at itself)'); const cat = await r.json();
    assert.ok(cat.products.length >= 6 && cat.ships.ships_on && typeof cat.shipping_flat_cents === 'number'); assert.equal(cat.checkout, 'demo'); assert.ok(cat.products.every(p => typeof p.sellable === 'boolean'));
    r = await fetch(base + '/store/estimate?zip=94110'); const est = await r.json(); assert.equal(est.state, 'CA'); assert.ok(est.delivers.max_days >= est.delivers.min_days); assert.match(est.label, /ships .* arrives/);
    r = await fetch(base + '/store/checkout', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ items: [{ sku: 'wap-parts', qty: 1 }] }) }); assert.equal(r.status, 503); assert.match((await r.json()).error, /demo takes no payments/);
    r = await fetch(base + '/store/session/cs_demo_nothing'); assert.deepEqual(await r.json(), { pending: true }); assert.equal((await fetch(base + '/store/session/x')).status, 400);
    r = await fetch(base + '/store/checkout', { method: 'OPTIONS' }); assert.equal(r.status, 204); assert.match(r.headers.get('access-control-allow-methods'), /GET, POST/);
    // a site order lands by webhook: the confirmation mail carries the ship date, the window and the status link
    const ev = { id: 'evt_site_1', type: 'checkout.session.completed', data: { object: { id: 'cs_site_0001', payment_intent: 'pi_site_0001', payment_status: 'paid', currency: 'usd', amount_subtotal: 1500, amount_total: 2300, created: Math.floor(Date.now() / 1000), total_details: { amount_shipping: 800, amount_tax: 0 }, customer_details: { email: 'shopper@customer.example.test', name: 'Shopper Fixture' }, shipping_details: { name: 'Shopper Fixture', address: { line1: '1 Example Lane', city: 'Madison', state: 'WI', postal_code: '53703', country: 'US' } }, line_items: { data: [{ description: 'WAP Enclosure Set', quantity: 1, price: { unit_amount: 1500, product: { metadata: { sku: 'wap-parts' } } } }] } } } };
    const body = JSON.stringify(ev); r = await fetch(base + '/webhooks/stripe', { method: 'POST', headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(body, cfg.stripeWebhookSecret) }, body }); assert.equal(r.status, 200);
    const mail = db.get("SELECT subject, body_text FROM mail_outbox WHERE to_addr = 'shopper@customer.example.test' AND subject LIKE 'We have your%'"); assert.ok(mail, 'the confirmation is queued'); assert.match(mail.body_text, /ships (Mon|Tue|Wed|Thu|Fri|Sat|Sun), /); assert.match(mail.body_text, /expect it .* — an estimate/); assert.match(mail.body_text, /\/o\/EL-\d+\//);
    r = await fetch(base + '/store/session/cs_site_0001'); const st = await r.json(); assert.equal(st.pending, false); assert.equal(st.items[0].sku, 'wap-parts'); assert.ok(st.status_url);
  } finally { await app.close(); db.close(); }
});
