// The storefront's server half. The site is static HTML; what it cannot know
// on its own, it asks here, over three public GETs and one POST:
//
//   GET  /store/catalog        the live catalog: price, what is sellable now and how many,
//                              when the current batch closes and ships
//   GET  /store/estimate?zip=  "order by Sunday night, ships Tue Oct 28, arrives Oct 30 – Nov 1"
//                              for that ZIP, before anyone pays
//   POST /store/checkout       a hosted Stripe Checkout Session for the cart — the only place a
//                              card is ever typed is Stripe's page; we see an address and an email
//   GET  /store/session/:id    what the thanks page shows once the webhook has recorded the order
//
// Stock is not a number typed in store.json: it is the batch cap in store.json minus what this
// batch has already sold, and never more than the parts on the shelf can build. The ship date is
// the batch cadence (BATCH_CLOSE_DAY / BATCH_SHIP_DAY in OPERATOR_TZ); the arrival window is the
// same estimate the order page and the tracking mail use after shipping (delivery.mjs), counted
// from the ship date — one method, before and after the sale.
import { estimateDelivery, orderStatusUrl } from './delivery.mjs';
import { weekLabel, orderByNumber } from './orders.mjs';
import { buildable } from './inventory.mjs';
import { currentTz } from '../render.mjs';
import { isOn } from '../flags.mjs';
import { timedFetch } from '../net.mjs';
import { recordSession } from './recovery.mjs';

const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const SHORT = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

// Calendar parts of an instant in a zone — the only way to count "Sunday night" where the operator is.
export function zoneParts(d, tz) {
  try {
    const p = Object.fromEntries(new Intl.DateTimeFormat('en-US', { timeZone: tz || 'UTC', year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', weekday: 'short', hourCycle: 'h23' }).formatToParts(d).map(x => [x.type, x.value]));
    return { y: +p.year, m: +p.month, d: +p.day, hour: +p.hour, minute: +p.minute, weekday: SHORT.indexOf(p.weekday) };
  } catch { const u = new Date(d); return { y: u.getUTCFullYear(), m: u.getUTCMonth() + 1, d: u.getUTCDate(), hour: u.getUTCHours(), minute: u.getUTCMinutes(), weekday: u.getUTCDay() }; }
}
const ymd = ({ y, m, d }) => `${y}-${String(m).padStart(2, '0')}-${String(d).padStart(2, '0')}`;
const plusDays = (parts, n) => { const u = new Date(Date.UTC(parts.y, parts.m - 1, parts.d + n)); return { y: u.getUTCFullYear(), m: u.getUTCMonth() + 1, d: u.getUTCDate(), weekday: u.getUTCDay() }; };
export const dayLabel = (p, { year = false } = {}) => `${SHORT[p.weekday]}, ${MONTHS[p.m - 1]} ${p.d}${year ? ` ${p.y}` : ''}`;

// The batch cadence: orders in by <closeDay> <closeHour>:59 (operator time) ship on the next
// <shipDay>. Past the close, the next batch. Returns calendar dates in the operator's zone and
// the sentence the store prints.
export function nextShipDate(now = new Date(), { tz = currentTz(), closeDay = 0, closeHour = 23, shipDay = 2 } = {}) {
  const p = zoneParts(now, tz);
  let untilClose = (closeDay - p.weekday + 7) % 7;
  if (untilClose === 0 && p.hour >= closeHour) untilClose = 7;         // the close is the top of that hour; past it, next week
  const close = plusDays(p, untilClose);
  let untilShip = (shipDay - close.weekday + 7) % 7; if (untilShip === 0) untilShip = 7;   // ships strictly after the close
  const ship = plusDays(close, untilShip);
  const closesTonight = untilClose === 0;
  return { closes_on: ymd(close), closes_weekday: DAYS[closeDay], closes_hour: closeHour, ships_on: ymd(ship), ships_weekday: DAYS[shipDay],
    label: `Order by ${closesTonight ? 'tonight' : DAYS[closeDay]} ${closeHour >= 12 ? `${closeHour === 12 ? 12 : closeHour - 12} pm` : `${closeHour || 12} am`} · ships ${dayLabel(ship)}`,
    ships_label: dayLabel(ship), closes_label: `${closesTonight ? 'tonight' : DAYS[closeDay]}, ${MONTHS[close.m - 1]} ${close.d}`, tz };
}

// ZIP → state, by the three-digit prefix ranges the USPS assigns. Enough for an estimate, and
// never for an address — Stripe collects and validates the address itself.
export const ZIP_RANGES = [[5, 5, 'NY'], [6, 9, 'PR'], [10, 27, 'MA'], [28, 29, 'RI'], [30, 38, 'NH'], [39, 49, 'ME'], [50, 59, 'VT'], [60, 69, 'CT'], [70, 89, 'NJ'], [100, 149, 'NY'], [150, 196, 'PA'], [197, 199, 'DE'],
  [200, 205, 'DC'], [206, 219, 'MD'], [220, 246, 'VA'], [247, 268, 'WV'], [270, 289, 'NC'], [290, 299, 'SC'], [300, 319, 'GA'], [320, 349, 'FL'], [350, 369, 'AL'], [370, 385, 'TN'], [386, 397, 'MS'], [398, 399, 'GA'],
  [400, 427, 'KY'], [430, 459, 'OH'], [460, 479, 'IN'], [480, 499, 'MI'], [500, 528, 'IA'], [530, 549, 'WI'], [550, 567, 'MN'], [570, 577, 'SD'], [580, 588, 'ND'], [590, 599, 'MT'], [600, 629, 'IL'], [630, 658, 'MO'],
  [660, 679, 'KS'], [680, 693, 'NE'], [700, 715, 'LA'], [716, 729, 'AR'], [730, 749, 'OK'], [750, 799, 'TX'], [800, 816, 'CO'], [820, 831, 'WY'], [832, 838, 'ID'], [840, 847, 'UT'], [850, 865, 'AZ'], [870, 884, 'NM'],
  [889, 898, 'NV'], [900, 961, 'CA'], [967, 968, 'HI'], [970, 979, 'OR'], [980, 994, 'WA'], [995, 999, 'AK']];
export function stateForZip(zip) {
  const m = String(zip || '').trim().match(/^(\d{3})\d{2}(?:-?\d{4})?$/); if (!m) return null;
  const n = Number(m[1]); const hit = ZIP_RANGES.find(([a, b]) => n >= a && n <= b); return hit ? hit[2] : null;
}

// What a shopper sees before paying: when the batch ships and when it lands at that ZIP. With no
// ZIP (or one we cannot place) the window is the honest middle of the country.
export function estimateForZip(zip, { now = new Date(), cfg = {} } = {}) {
  const ships = nextShipDate(now, { tz: cfg.tz, closeDay: cfg.batchCloseDay, closeHour: cfg.batchCloseHour, shipDay: cfg.batchShipDay });
  const state = stateForZip(zip);
  const shipAt = new Date(`${ships.ships_on}T16:00:00Z`);
  const d = estimateDelivery(shipAt, state, { fromState: cfg.shipFromState, carrier: 'USPS' });
  return { zip: zip ? String(zip).trim().slice(0, 10) : null, state, ships, delivers: d ? { min_days: d.min_days, max_days: d.max_days, earliest: d.earliest, latest: d.latest, label: d.label } : null, carrier: 'USPS Ground Advantage',
    label: d ? `${ships.label} · arrives ${d.label}` : ships.label, basis: 'estimate' };
}

// How many of a SKU this batch can still take: the batch cap in store.json minus what this batch
// has sold (this ISO week in the operator's zone), and never more than the shelf can build once
// the orders already paid but not yet packed have taken their parts (packing is what consumes
// them, so a paid order is a claim on the shelf until then). null means nothing limits it.
export function availableNow(db, product, { now = new Date(), tz = currentTz() } = {}) {
  const limits = [];
  if (product.stock != null && Number.isFinite(Number(product.stock))) {
    const taken = db.get("SELECT COALESCE(SUM(i.qty), 0) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = ? AND o.batch = ? AND o.status IN ('paid', 'packed', 'shipped', 'delivered')", product.sku, weekLabel(now, tz)).n;
    limits.push(Math.max(0, Number(product.stock) - taken));
  }
  const b = buildable(db, product.sku);
  if (b != null) { const committed = db.get("SELECT COALESCE(SUM(i.qty), 0) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = ? AND o.status = 'paid'", product.sku).n; limits.push(Math.max(0, b - committed)); }
  return limits.length ? Math.min(...limits) : null;
}

// After an order is recorded: each line whose SKU is now committed past what the week's cap or the
// shelf can cover. The store refuses a checkout for more than availableNow() says, but two checkouts
// can pay for the last unit in the same minute, and a Payment Link bypasses the store entirely.
export function oversoldLines(db, order, catalog, { now = new Date(), tz = currentTz() } = {}) {
  const out = [];
  for (const it of order.items || []) {
    const p = catalog?.products?.get?.(it.sku); if (!p) continue;
    if (p.stock != null && Number.isFinite(Number(p.stock))) {
      const taken = db.get("SELECT COALESCE(SUM(i.qty), 0) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = ? AND o.batch = ? AND o.status IN ('paid', 'packed', 'shipped', 'delivered')", it.sku, order.batch || weekLabel(now, tz)).n;
      if (taken > Number(p.stock)) { out.push({ sku: it.sku, short: taken - Number(p.stock), why: `${taken} sold this week against a cap of ${Number(p.stock)}` }); continue; }
    }
    const b = buildable(db, it.sku);
    if (b != null) { const committed = db.get("SELECT COALESCE(SUM(i.qty), 0) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = ? AND o.status = 'paid'", it.sku).n; if (committed > b) out.push({ sku: it.sku, short: committed - b, why: `${committed} paid and waiting, parts on the shelf for ${b}` }); }
  }
  return out;
}

export function storefrontCatalog(db, catalog, { now = new Date(), cfg = {} } = {}) {
  const ships = nextShipDate(now, { tz: cfg.tz, closeDay: cfg.batchCloseDay, closeHour: cfg.batchCloseHour, shipDay: cfg.batchShipDay });
  const products = [];
  for (const p of catalog.products.values()) {
    const available = availableNow(db, p, { now, tz: cfg.tz });
    const reason = p.authorization === 'pending-fcc' ? 'pending-fcc' : available === 0 ? 'batch-full' : null;
    products.push({ sku: p.sku, name: p.name, price_cents: p.price_cents, group: p.group, available, sellable: !reason, reason, link: p.link || '' });
  }
  return { mode: catalog.mode, shipping_flat_cents: catalog.shipping_flat_cents, ships, products, checkout: cfg.demo ? 'demo' : cfg.stripeApiKey ? 'hosted' : 'links', generated_at: now.toISOString() };
}

// Stripe's form encoding: nested keys in brackets, one pair per leaf.
export function formEncode(obj, prefix = '', out = []) {
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? `${prefix}[${k}]` : k;
    if (v == null) continue;
    if (typeof v === 'object') formEncode(v, key, out); else out.push(`${encodeURIComponent(key)}=${encodeURIComponent(String(v))}`);
  }
  return out.join('&');
}

// The cart → a Checkout Session. Every line is checked against the live catalog (a product that
// is pending authorization, or more than the batch can take, is refused by name), the shipping
// option carries the batch's ship date and the arrival window for the ZIP as Stripe's own
// delivery estimate, and the success page gets the session id to look its order up.
export async function createCheckout(db, cfg, catalog, { items, zip = null, email = null } = {}, { fetchImpl = timedFetch, now = new Date() } = {}) {
  if (!cfg.stripeApiKey) throw new StoreError(503, 'checkout is not configured (STRIPE_API_KEY)');
  if (!Array.isArray(items) || !items.length) throw new StoreError(400, 'the cart is empty');
  if (items.length > 20) throw new StoreError(400, 'that is more lines than one order carries');
  const lines = []; let units = 0;
  for (const it of items) {
    const sku = String(it?.sku || '').trim(); const qty = Number(it?.qty);
    if (!Number.isInteger(qty) || qty < 1 || qty > 10) throw new StoreError(400, `${sku || 'a line'}: quantity is a whole number from 1 to 10`);
    const p = catalog.products.get(sku); if (!p) throw new StoreError(400, `${sku}: not in the catalog`);
    if (p.authorization === 'pending-fcc') throw new StoreError(400, `${p.name} is not for sale until its FCC authorization is complete`);
    const same = lines.find(l => l.sku === sku); if (same) same.qty += qty; else lines.push({ sku, qty, name: p.name, unit_cents: p.price_cents, product: p });
    units += qty;
  }
  if (units > 50) throw new StoreError(400, 'that is more units than one order carries — write to us for a wholesale order');
  for (const l of lines) {   // the merged quantity against what is left, so two lines of the same thing cannot slip past the cap
    const available = availableNow(db, l.product, { now, tz: cfg.tz });
    if (available != null && l.qty > available) throw new StoreError(409, available ? `${l.name}: only ${available} left in this batch` : `${l.name}: this batch is full — the next one opens within a week`);
    delete l.product;
  }
  const est = estimateForZip(zip, { now, cfg });
  const site = (cfg.siteOrigin || '').replace(/\/$/, '');
  const session = {
    mode: 'payment',
    line_items: Object.fromEntries(lines.map((l, i) => [i, { quantity: l.qty, price_data: { currency: 'usd', unit_amount: l.unit_cents, product_data: { name: l.name, metadata: { sku: l.sku } } } }])),
    shipping_address_collection: { allowed_countries: { 0: 'US' } },
    shipping_options: { 0: { shipping_rate_data: { type: 'fixed_amount', display_name: `${est.carrier} · ships ${est.ships.ships_label}`, fixed_amount: { amount: catalog.shipping_flat_cents, currency: 'usd' },
      delivery_estimate: est.delivers ? { minimum: { unit: 'business_day', value: est.delivers.min_days }, maximum: { unit: 'business_day', value: est.delivers.max_days } } : undefined } } },
    allow_promotion_codes: 'true',
    success_url: `${site}${cfg.checkoutSuccessPath || '/store/thanks'}?session_id={CHECKOUT_SESSION_ID}`,
    cancel_url: `${site}/store`,
    expires_at: Math.floor(now.getTime() / 1000) + 30 * 60,
    // if it times out, Stripe mints a link that reopens this cart for 30 days (the one follow-up mail carries it; recovery.mjs)
    after_expiration: { recovery: { enabled: 'true', allow_promotion_codes: 'true' } },
    metadata: { source: 'site', skus: lines.map(l => `${l.qty}x ${l.sku}`).join(', '), ships_on: est.ships.ships_on, batch: weekLabel(now, cfg.tz) },
    payment_intent_data: { metadata: { skus: lines.map(l => `${l.qty}x ${l.sku}`).join(', ') } },
    ...(isOn(db, cfg, 'stripe_tax') ? { automatic_tax: { enabled: 'true' } } : {}),
    ...(email && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email) ? { customer_email: email } : {}),
  };
  const r = await fetchImpl('https://api.stripe.com/v1/checkout/sessions', { method: 'POST', headers: { authorization: `Bearer ${cfg.stripeApiKey}`, 'content-type': 'application/x-www-form-urlencoded', 'idempotency-key': `cart-${now.getTime()}-${Math.random().toString(36).slice(2, 10)}` }, body: formEncode(session) });
  const body = await r.json().catch(() => ({}));
  if (!r.ok || !body.url) { db.audit('public', 'store.checkout.failed', 'stripe', body?.error?.type || String(r.status), { message: String(body?.error?.message || '').slice(0, 200) }); throw new StoreError(502, 'the payment page could not be opened — nothing was charged; try again in a moment'); }
  db.audit('public', 'store.checkout', 'stripe', body.id, { lines: lines.map(l => `${l.qty}x ${l.sku}`), zip: est.state, ships_on: est.ships.ships_on });
  recordSession(db, { id: body.id, amount_cents: lines.reduce((s, l) => s + l.qty * l.unit_cents, 0) + (catalog.shipping_flat_cents || 0), lines: lines.map(l => `${l.qty}x ${l.sku}`).join(', '), state: est.state, email: email && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email) ? email : null, created_at: now.toISOString() });
  return { id: body.id, url: body.url, ships: est.ships, delivers: est.delivers, lines };
}

// The thanks page: the order the webhook recorded for that session, or "not yet" (Stripe's webhook
// can land a few seconds after the redirect; the page asks again).
export function sessionStatus(db, cfg, sessionId, { now = new Date() } = {}) {
  if (!/^cs_[A-Za-z0-9_]{8,}$/.test(String(sessionId || ''))) throw new StoreError(400, 'that is not a checkout session id');
  const o = orderByNumber(db, sessionId); if (!o) return { pending: true };
  const est = estimateForZip(o.ship_zip, { now, cfg });
  return { pending: false, order: o.number, status: o.status, items: o.items.map(i => ({ sku: i.sku, name: i.name, qty: i.qty })), total_cents: o.total_cents, ships: est.ships, delivers: est.delivers, status_url: cfg.secret ? orderStatusUrl(cfg, o) : null };
}

export class StoreError extends Error { constructor(status, message) { super(message); this.status = status; } }
