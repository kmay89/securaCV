// Stripe stays the processor (that is the one thing nobody self-hosts). What
// we replace is everything around it: the order queue, refunds ledger,
// subscription state. Two inputs: the signed webhook (live) and the CSV
// export the weekly batch already uses (catch-up / offline).
import { createHmac, timingSafeEqual } from 'node:crypto';
import { timedFetch } from '../net.mjs';

// Stripe-Signature: t=<ts>,v1=<hex>[,v1=<hex>…]; signed_payload = `${t}.${body}`
export function verifyStripeSignature(rawBody, header, secret, { now = Date.now() / 1000, toleranceS = 300 } = {}) {
  if (!secret || !header) return false;
  const parts = Object.create(null); const v1 = [];
  for (const kv of String(header).split(',')) { const [k, v] = kv.split('=').map(s => s.trim()); if (k === 't') parts.t = v; if (k === 'v1') v1.push(v); }
  if (!parts.t || !v1.length) return false;
  if (Math.abs(now - Number(parts.t)) > toleranceS) return false;
  const expected = createHmac('sha256', secret).update(`${parts.t}.${Buffer.isBuffer(rawBody) ? rawBody.toString('utf8') : rawBody}`).digest('hex');
  const e = Buffer.from(expected);
  return v1.some(s => { const b = Buffer.from(s); return b.length === e.length && timingSafeEqual(b, e); });
}

// Used by tests and by `backoffice stripe:sign` to craft a header.
export function signStripePayload(rawBody, secret, t = Math.floor(Date.now() / 1000)) {
  return `t=${t},v1=${createHmac('sha256', secret).update(`${t}.${rawBody}`).digest('hex')}`;
}

// Map a Stripe event to the neutral shape orders.mjs consumes. We only read
// fields that exist on every account; metadata.sku is what a Payment Link
// carries when the product's metadata is set (store-README step 1).
export function eventToOrder(event) {
  const type = event.type || '';
  const o = event.data?.object || {};
  // A card pays inside `completed`; ACH / bank debit pay later and arrive as `async_payment_succeeded`;
  // a 100 % code pays nothing and says `no_payment_required`. All three are an order.
  // A Checkout in `subscription` or `setup` mode is a plan or a saved card, never a box to pack: those
  // arrive on their own events below. Only a `payment` session (the mode a Payment Link uses) is an order.
  if (o.mode && o.mode !== 'payment' && type.startsWith('checkout.session.')) return { kind: 'ignore', type, why: `checkout mode ${o.mode}` };
  // A session that timed out: Stripe says whether an email was typed and mints a link that reopens the same cart (recovery.mjs).
  if (type === 'checkout.session.expired') return { kind: 'expired', type, external_id: o.id, email: o.customer_details?.email || o.customer_email || '', amount_cents: o.amount_total ?? 0, recovery_url: o.after_expiration?.recovery?.url || null, recovery_expires_at: o.after_expiration?.recovery?.expires_at ? new Date(o.after_expiration.recovery.expires_at * 1000).toISOString() : null };
  if ((type === 'checkout.session.completed' && ['paid', 'no_payment_required'].includes(o.payment_status)) || type === 'checkout.session.async_payment_succeeded') {
    const addr = o.shipping_details?.address || o.customer_details?.address || {};
    const items = (o.line_items?.data || []).map(li => ({
      sku: li.price?.product?.metadata?.sku || li.price?.metadata?.sku || li.price?.product?.name || li.description,
      name: li.description || li.price?.product?.name || 'item', qty: li.quantity || 1,
      unit_cents: li.price?.unit_amount ?? Math.round((li.amount_subtotal || 0) / (li.quantity || 1)),
    }));
    return {
      kind: 'order', external_id: o.id, payment_intent: typeof o.payment_intent === 'string' ? o.payment_intent : o.payment_intent?.id || null, source: 'stripe',
      line_items_present: Array.isArray(o.line_items?.data),
      email: o.customer_details?.email || o.customer_email || '', name: o.shipping_details?.name || o.customer_details?.name || '',
      currency: o.currency || 'usd',
      subtotal_cents: o.amount_subtotal ?? 0, shipping_cents: o.total_details?.amount_shipping ?? o.shipping_cost?.amount_total ?? 0,
      tax_cents: o.total_details?.amount_tax ?? 0, total_cents: o.amount_total ?? 0,
      discount_cents: o.total_details?.amount_discount ?? 0,
      // Expanded objects carry the customer-facing code; a plain webhook carries ids (promo_…, a
      // coupon id) which resolveDiscountCode() turns into the code when an API key is set.
      discount_code: (() => { const d = o.discounts?.[0]; if (!d) return null; const pc = d.promotion_code; const cp = d.coupon; return (typeof pc === 'object' && pc?.code) || (typeof cp === 'object' && (cp?.name || cp?.id)) || (typeof pc === 'string' ? pc : null) || (typeof cp === 'string' ? cp : null); })(),
      ship: { line1: addr.line1 || '', line2: addr.line2 || '', city: addr.city || '', state: addr.state || '', zip: addr.postal_code || '', country: addr.country || 'US' },
      items, paid_at: new Date((o.created || Date.now() / 1000) * 1000).toISOString(),
      recovered_from: o.recovered_from?.checkout_session_id || null,   // the expired session this one reopened, if any
    };
  }
  if (type === 'charge.dispute.created' || type === 'charge.dispute.closed' || type === 'charge.dispute.updated') {
    return { kind: 'dispute', type, external_id: (typeof o.payment_intent === 'string' ? o.payment_intent : o.payment_intent?.id) || null, charge: typeof o.charge === 'string' ? o.charge : o.charge?.id || null,
      dispute: o.id, status: o.status || '', reason: o.reason || '', amount_cents: o.amount ?? 0, due: o.evidence_details?.due_by ? new Date(o.evidence_details.due_by * 1000).toISOString().slice(0, 10) : null };
  }
  if (type === 'charge.refunded') return { kind: 'refund', external_id: (typeof o.payment_intent === 'string' ? o.payment_intent : o.payment_intent?.id) || o.id, charge: o.id, refunded_cents: o.amount_refunded ?? 0 };
  if (type.startsWith('customer.subscription.')) {
    const item = o.items?.data?.[0];
    return { kind: 'subscription', external_id: o.id, customer: o.customer, status: o.status,
      plan: item?.price?.nickname || item?.price?.product?.name || item?.price?.id || 'support',
      amount_cents: item?.price?.unit_amount ?? 0, interval: item?.price?.recurring?.interval || 'month',
      current_period_end: o.current_period_end ? new Date(o.current_period_end * 1000).toISOString() : null,
      email: o.customer_email || null, deleted: type.endsWith('.deleted') };
  }
  if (type === 'payout.paid') return { kind: 'payout', external_id: o.id, amount_cents: o.amount ?? 0, arrival: o.arrival_date ? new Date(o.arrival_date * 1000).toISOString().slice(0, 10) : null, currency: o.currency || 'usd' };
  return { kind: 'ignore', type };
}

// A plain checkout.session.completed webhook does not carry line_items (they
// are expandable, never inline). With an API key we fetch them; without one
// the order is recorded empty and an exception names it (orders.mjs refuses
// to pack an order with no items, so nothing ships blind).
export async function fetchLineItems(cfg, sessionId, { fetchImpl = timedFetch } = {}) {
  if (!cfg.stripeApiKey) return null;
  const r = await fetchImpl(`https://api.stripe.com/v1/checkout/sessions/${encodeURIComponent(sessionId)}/line_items?limit=100&expand[]=data.price.product`, { headers: { authorization: `Bearer ${cfg.stripeApiKey}` } });
  if (!r.ok) throw new Error(`stripe line_items ${r.status} for ${sessionId}`);
  const body = await r.json();
  return (body.data || []).map(li => ({
    sku: li.price?.product?.metadata?.sku || li.price?.metadata?.sku || li.price?.product?.name || li.description,
    name: li.description || li.price?.product?.name || 'item', qty: li.quantity || 1,
    unit_cents: li.price?.unit_amount ?? Math.round((li.amount_subtotal || 0) / (li.quantity || 1)),
  }));
}

// A promotion id (promo_…) or coupon id from the webhook → the code the customer typed, when an
// API key allows the lookup. Without one the id is kept as the code: still a stable label for
// the books, and never fed to the local discount rules (recordOrder sees discount_cents set).
export async function resolveDiscountCode(cfg, code, { fetchImpl = timedFetch } = {}) {
  if (!code || !cfg.stripeApiKey) return code;
  const isPromo = /^promo_[A-Za-z0-9]+$/.test(code);
  const r = await fetchImpl(`https://api.stripe.com/v1/${isPromo ? 'promotion_codes' : 'coupons'}/${encodeURIComponent(code)}`, { headers: { authorization: `Bearer ${cfg.stripeApiKey}` } });
  if (!r.ok) return code;
  const body = await r.json();
  return (isPromo ? body.code : body.name || body.id) || code;
}

// --- the CSV path. Stripe's Payments export headers vary by dashboard
// settings; we reuse the website's alias idea so one spelling fix lands once.
export function parseCsv(textIn) {
  const text = textIn.replace(/^﻿/, '');
  const rows = []; let row = [], cell = '', q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q) { if (c === '"') { if (text[i + 1] === '"') { cell += '"'; i++; } else q = false; } else cell += c; }
    else if (c === '"') q = true;
    else if (c === ',') { row.push(cell); cell = ''; }
    else if (c === '\n' || c === '\r') { if (c === '\r' && text[i + 1] === '\n') i++; row.push(cell); rows.push(row); row = []; cell = ''; }
    else cell += c;
  }
  if (cell.length || row.length) { row.push(cell); rows.push(row); }
  const [head, ...body] = rows.filter(r => r.length > 1 || (r[0] || '').trim());
  if (!head) return [];
  return body.map(r => Object.fromEntries(head.map((h, i) => [h.trim(), (r[i] ?? '').trim()])));
}

const ALIASES = {
  external_id: ['id', 'paymentid', 'paymentintentid', 'checkoutsessionid', 'orderid', 'order'],
  email: ['email', 'customeremail', 'receiptemail', 'customer'],
  name: ['shippingname', 'name', 'shipname', 'customername', 'cardname'],
  sku: ['sku', 'skuid', 'productid', 'metadatasku', 'skumetadata'],
  product: ['product', 'productname', 'description', 'item', 'lineitem'],
  qty: ['qty', 'quantity'],
  amount: ['amount', 'amountpaid', 'total', 'converted amount'],
  line1: ['shippingaddressline1', 'line1', 'address1', 'addressline1', 'shippingaddress', 'street1'],
  line2: ['shippingaddressline2', 'line2', 'address2', 'addressline2', 'street2'],
  city: ['shippingaddresscity', 'city', 'shippingcity'],
  state: ['shippingaddressstate', 'state', 'shippingstate', 'province'],
  zip: ['shippingaddresspostalcode', 'zip', 'zipcode', 'postalcode', 'shippingzip', 'postcode'],
  country: ['shippingaddresscountry', 'country', 'shippingcountry'],
  created: ['createddate(utc)', 'created', 'date', 'createdutc'],
  status: ['status'],
};
const norm = (h) => String(h).toLowerCase().replace(/[^a-z0-9()]/g, '');
export function detectColumns(row) {
  const byNorm = new Map(Object.keys(row || {}).map(h => [norm(h), h]));
  const map = {};
  for (const [k, names] of Object.entries(ALIASES)) for (const n of names) if (byNorm.has(n)) { map[k] = byNorm.get(n); break; }
  return map;
}
export function csvRowToOrder(row, map) {
  const g = (k) => (map[k] ? row[map[k]] : '') || '';
  const amount = Number(String(g('amount')).replace(/[^0-9.-]/g, '')) || 0;
  const qty = Number(g('qty')) || 1;
  return {
    kind: 'order', external_id: g('external_id') || null, source: 'import', email: g('email'), name: g('name'), currency: 'usd',
    subtotal_cents: Math.round(amount * 100), shipping_cents: 0, tax_cents: 0, total_cents: Math.round(amount * 100),
    ship: { line1: g('line1'), line2: g('line2'), city: g('city'), state: g('state'), zip: g('zip'), country: g('country') || 'US' },
    items: [{ sku: g('sku') || g('product'), name: g('product') || g('sku') || 'item', qty, unit_cents: Math.round(amount * 100 / qty) }],
    paid_at: g('created') ? new Date(g('created') + (g('created').endsWith('Z') ? '' : 'Z')).toISOString() : null,
    status_hint: g('status'),
  };
}
