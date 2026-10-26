// The store listing edited here, the phone set in Settings, and where sales tax is owed — the three things that stood
// between this and "no reason to pay a platform", held to their contracts.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setOverride, clearOverride, applyOverrides, listingState, overridesPatch, listOverrides } from '../src/modules/listing.mjs';
import { storefrontCatalog, createCheckout, StoreError } from '../src/modules/storefront.mjs';
import { nexusWatch, taxVerdict, thresholdFor, NO_SALES_TAX } from '../src/modules/nexus.mjs';
import { ntfyUrlFor, newOrderOn, savePhone, testPush, newOrderNudge, pushConfigured } from '../src/modules/notify.mjs';
import { doctor } from '../src/modules/health.mjs';
import { setFlag } from '../src/flags.mjs';
import { recordOrder } from '../src/modules/orders.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const D = 86400000; const iso = (ms) => new Date(ms).toISOString();

test('a listing edited here lays over the file: price, name, cap, hidden, a window — the same as the file is no difference, and the patch says what the file would have to say', () => {
  const db = testDb(); const cat = catalog(); const sku = [...cat.products.keys()][0]; const base = cat.products.get(sku);
  assert.equal(applyOverrides(db, cat).products.get(sku).listing, null, 'nothing edited: nothing laid over');
  const r = setOverride(db, sku, { name: base.name, price: (base.price_cents / 100).toFixed(2), stock: base.stock ?? '', hidden: '', note: 'same as the file' }, { actor: 'test', catalog: cat });
  assert.equal(r, null, 'values equal to the file are not a difference'); assert.equal(listOverrides(db).length, 0);
  const saved = setOverride(db, sku, { name: 'Canary Kit, second run', price: '79.50', stock: '12', hidden: '', note: 'price after the parts cost rose' }, { actor: 'test', catalog: cat });
  assert.equal(saved.price_cents, 7950); assert.equal(saved.stock, 12); assert.equal(saved.name, 'Canary Kit, second run');
  const live = applyOverrides(db, cat); const p = live.products.get(sku);
  assert.equal(p.price_cents, 7950); assert.equal(p.name, 'Canary Kit, second run'); assert.equal(p.stock, 12); assert.equal(p.hidden, false); assert.equal(p.listing.edited, true); assert.equal(p.listing.file.price_cents, base.price_cents);
  assert.equal(cat.products.get(sku).price_cents, base.price_cents, 'the loader\'s cache is not touched');
  assert.deepEqual(overridesPatch(db, cat)[sku], { name: 'Canary Kit, second run', price: 79.5, stock: 12 });
  assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'product.listing' AND entity_id = ?", sku));
  // a window: not yet, then open, then ended; hidden beats all
  const now = Date.parse('2026-11-01T12:00:00Z');
  setOverride(db, sku, { price: '79.50', available_from: '2026-11-03T16:00', available_until: '2026-12-01T00:00' }, { actor: 'test', catalog: cat });
  const w = applyOverrides(db, cat).products.get(sku); assert.equal(w.available_from, '2026-11-03T16:00:00.000Z', 'a local-looking stamp is read as UTC');
  assert.equal(listingState(w, now), 'not-yet'); assert.equal(listingState(w, Date.parse('2026-11-10T00:00:00Z')), null); assert.equal(listingState(w, Date.parse('2026-12-02T00:00:00Z')), 'ended');
  assert.throws(() => setOverride(db, sku, { available_from: '2026-12-01T00:00', available_until: '2026-11-01T00:00' }, { catalog: cat }), /ends before it starts/);
  assert.throws(() => setOverride(db, sku, { price: 'free' }, { catalog: cat }), /price is dollars/);
  assert.throws(() => setOverride(db, 'nope', { price: '1' }, { catalog: cat }), /not in the catalog/);
  setOverride(db, sku, { hidden: '1' }, { actor: 'test', catalog: cat }); assert.equal(listingState(applyOverrides(db, cat).products.get(sku)), 'hidden');
  assert.match(overridesPatch(db, cat)[sku]._note, /hidden here/);
  assert.equal(clearOverride(db, sku, { actor: 'test' }), true); assert.equal(applyOverrides(db, cat).products.get(sku).listing, null);
});

test('the storefront serves the edit and names the state; the checkout refuses a hidden, not-yet or ended listing', async () => {
  const db = testDb(); const cat = catalog(); const cfg = testCfg({ secret: 's', stripeApiKey: 'sk_test_x', tz: 'UTC' }); const sku = [...cat.products.keys()].find(k => cat.products.get(k).authorization !== 'pending-fcc');
  const now = new Date('2026-11-01T12:00:00Z');
  setOverride(db, sku, { price: '99.00', available_from: '2026-11-03T16:00' }, { actor: 'test', catalog: cat });
  const live = applyOverrides(db, cat); const sf = storefrontCatalog(db, live, { now, cfg }); const p = sf.products.find(x => x.sku === sku);
  assert.equal(p.price_cents, 9900); assert.equal(p.sellable, false); assert.equal(p.reason, 'not-yet'); assert.equal(p.opens, '2026-11-03T16:00:00.000Z'); assert.equal(p.hidden, false);
  await assert.rejects(createCheckout(db, cfg, live, { items: [{ sku, qty: 1 }] }, { now }), (e) => e instanceof StoreError && /not on sale yet/.test(e.message));
  setOverride(db, sku, { hidden: '1' }, { actor: 'test', catalog: cat });
  const h = storefrontCatalog(db, applyOverrides(db, cat), { now, cfg }).products.find(x => x.sku === sku); assert.equal(h.hidden, true); assert.equal(h.reason, 'hidden');
  await assert.rejects(createCheckout(db, cfg, applyOverrides(db, cat), { items: [{ sku, qty: 1 }] }, { now }), /not in the store/);
  clearOverride(db, sku); setOverride(db, sku, { price: '99.00' }, { actor: 'test', catalog: cat });
  // the price Stripe is asked for is the edited one
  let posted = null; const fetchImpl = async (url, init) => { posted = init.body; return { ok: true, json: async () => ({ id: 'cs_test_1', url: 'https://checkout.stripe.com/x' }) }; };
  await createCheckout(db, cfg, applyOverrides(db, cat), { items: [{ sku, qty: 1 }] }, { fetchImpl, now });
  assert.ok(decodeURIComponent(posted).includes('unit_amount]=9900'), 'the edited price is what is charged');
});

test('where sales tax is owed: the home state always, any state over its threshold, a warning within 20%; the doctor says it in one sentence', () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', shipFromState: 'MA' }); const now = Date.parse('2026-10-26T12:00:00Z');
  assert.equal(thresholdFor('CA').sales_cents, 500_00 * 1000); assert.equal(thresholdFor('ZZ').sales_cents, 100_00 * 1000); assert.ok(NO_SALES_TAX.has('OR'));
  let v = taxVerdict(db, cfg, { now }); assert.equal(v.ok, null, 'no sales: nothing to say');
  const order = (state, cents, n = 1, ago = 10) => { for (let i = 0; i < n; i++) recordOrder(db, sampleOrder({ external_id: `cs_${state}_${cents}_${i}_${Math.random()}`, paid_at: iso(now - ago * D), total_cents: cents, subtotal_cents: cents - 800, ship: { line1: '1 St', city: 'X', state, zip: '02139', country: 'US' } }), { catalog: catalog(), actor: 'test' }); };
  order('NY', 12000);
  v = taxVerdict(db, cfg, { now }); assert.equal(v.ok, true, 'one order into NY, nothing into MA: owed nowhere yet'); assert.match(v.text, /owed nowhere yet/);
  order('MA', 12000);
  v = taxVerdict(db, cfg, { now }); assert.equal(v.ok, false); assert.deepEqual(v.owe, ['MA']); assert.match(v.text, /tax is owed in MA \(your home state\)/);
  const rows = nexusWatch(db, cfg, { now }); assert.equal(rows[0].state, 'MA'); assert.equal(rows[0].status, 'home');
  // over the threshold elsewhere by order count (NJ keeps 200 orders), near by dollars (VT at 85%)
  order('NJ', 5000, 200, 20);
  order('VT', 85_000_00, 1, 30);
  v = taxVerdict(db, cfg, { now }); assert.deepEqual(v.owe.sort(), ['MA', 'NJ']); assert.deepEqual(v.near, ['VT']); assert.match(v.text, /over the threshold elsewhere/);
  assert.equal(nexusWatch(db, cfg, { now }).find(r => r.state === 'VT').status, 'near');
  // a year-old order is history
  order('CA', 600_000_00, 1, 400); assert.ok(!nexusWatch(db, cfg, { now }).some(r => r.state === 'CA' && r.status === 'over'));
  // collecting: the row turns informational, with the list to register in
  setFlag(db, cfg, 'stripe_tax', true, { actor: 'test' }); v = taxVerdict(db, cfg, { now }); assert.equal(v.ok, true); assert.match(v.text, /Stripe Tax collects at checkout · owed in MA, NJ · near the threshold in VT/);
  const row = doctor(db, cfg).checks.find(c => c.key === 'sales_tax'); assert.ok(row); assert.equal(row.ok, true);
});

test('the phone is set in Settings, the environment wins, staging and the demo push nowhere, and a test push goes to the topic', async () => {
  const db = testDb(); const calls = []; const fetchImpl = async (url, init) => { calls.push({ url, init }); return { ok: true, status: 200 }; };
  let cfg = testCfg({ secret: 's' }); assert.equal(ntfyUrlFor(db, cfg), ''); assert.equal(newOrderOn(db, cfg), true, 'on by default');
  assert.throws(() => savePhone(db, { ntfy_url: 'http://ntfy.sh/plain' }), /https URL/);
  savePhone(db, { ntfy_url: 'https://ntfy.sh/errer-orders-9f3', ntfy_token: 'tk_secret', new_order: true }, { actor: 'test' });
  assert.equal(ntfyUrlFor(db, cfg), 'https://ntfy.sh/errer-orders-9f3'); assert.equal(pushConfigured(db, cfg), true);
  const r = await testPush(db, cfg, { fetchImpl }); assert.deepEqual(r, { ok: true, status: 200 }); assert.equal(calls[0].url, 'https://ntfy.sh/errer-orders-9f3'); assert.equal(calls[0].init.headers.authorization, 'Bearer tk_secret'); assert.match(calls[0].init.headers.title, /test push/);
  savePhone(db, { new_order: false }); assert.equal(newOrderOn(db, cfg), false);
  const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_q' }), { catalog: catalog(), actor: 'test' });
  assert.deepEqual(await newOrderNudge(db, testCfg({ secret: 's', operatorEmail: 'o@errer.test' }), order, { fetchImpl }), { mail: false, push: false }, 'quiet is quiet');
  assert.equal(newOrderOn(db, testCfg({ notifyNewOrder: true })), true, 'the environment pins it on');
  assert.equal(ntfyUrlFor(db, testCfg({ ntfyUrl: 'https://ntfy.sh/from-env' })), 'https://ntfy.sh/from-env', 'the environment wins');
  assert.equal(ntfyUrlFor(db, testCfg({ env: 'staging', ntfyUrl: 'https://ntfy.sh/from-env' })), '', 'staging pushes nowhere'); assert.equal(ntfyUrlFor(db, testCfg({ demo: true })), '');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'notify.settings'").n, 2);
  assert.ok(!JSON.stringify(db.all("SELECT detail FROM audit_log WHERE action = 'notify.settings'")).includes('tk_secret'), 'the token is never in the log');
});

test('over HTTP: the product page edits the listing, the Products page prints the patch, Settings saves the phone, Books shows where tax is owed', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', attentionTtlMs: 0, shipFromState: 'MA' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }).toString() });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    const get = async (p) => (await fetch(base + p, { headers: { cookie, accept: 'text/html' } })).text();
    const post = (p, body) => fetch(base + p, { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ _csrf: csrf, ...body }).toString() });
    const cat = await app.catalog(); const sku = [...cat.products.keys()][0];
    let page = await get(`/products/${sku}`); assert.ok(page.includes('The store listing') && page.includes('Save the listing'));
    assert.equal((await post(`/products/${sku}/listing`, { name: cat.products.get(sku).name, price: '88.00', stock: '', available_from: '', available_until: '', note: 'test' })).status, 303);
    page = await get(`/products/${sku}`); assert.ok(page.includes('Edited here') && page.includes('edited here') && page.includes('Back to the file'));
    const list = await get('/products'); assert.ok(list.includes('1 product is edited here') && list.includes('&quot;price&quot;: 88'), 'the pill and the patch');
    const live = await (await fetch(base + '/store/catalog')).json(); assert.equal(live.products.find(p => p.sku === sku).price_cents, 8800, 'the store serves the edit');
    assert.equal((await post(`/products/${sku}/listing/clear`, {})).status, 303); assert.equal((await (await fetch(base + '/store/catalog')).json()).products.find(p => p.sku === sku).price_cents, cat.products.get(sku).price_cents);
    assert.equal((await post('/settings/phone', { ntfy_url: 'https://ntfy.sh/t-abc', ntfy_token: '', new_order: '1' })).status, 303);
    const settings = await get('/settings'); assert.ok(settings.includes('Your phone') && settings.includes('pushes go to https://ntfy.sh/t-abc') && settings.includes('Send a test push'));
    const books = await get('/books'); assert.ok(books.includes('Where sales tax is owed') && books.includes('ships from here'));
  } finally { await app.close(); }
});
