// Orders: paid → packed → shipped → delivered. One row per shipment, items
// resolved against the catalog (an unknown SKU is a loud error), COGS frozen
// at the time of sale so the books do not move when the BOM does. Packing an
// order consumes inventory through the recipes; shipping mails the tracking.
import { nowIso, currentActor } from '../db.mjs';
import { upsertContact } from './contacts.mjs';
import { resolveSku, cogsCents } from './catalog.mjs';
import { consumeForOrder, releaseForOrder } from './inventory.mjs';
import { enqueue, fill } from './mail.mjs';
import { postSale, postRefund } from './ledger.mjs';
import { evaluate as evaluateDiscount } from './discounts.mjs';
import { normalizeTags } from './contacts.mjs';
import { estimateDelivery, orderStatusUrl } from './delivery.mjs';

export const STATUSES = ['pending', 'paid', 'packed', 'shipped', 'delivered', 'refunded', 'canceled'];

export function weekLabel(d = new Date()) {
  // ISO week, e.g. 2026-W43 — the weekly batch the store-README describes.
  const date = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()));
  const dayNum = date.getUTCDay() || 7; date.setUTCDate(date.getUTCDate() + 4 - dayNum);
  const yearStart = new Date(Date.UTC(date.getUTCFullYear(), 0, 1));
  return `${date.getUTCFullYear()}-W${String(Math.ceil((((date - yearStart) / 86400000) + 1) / 7)).padStart(2, '0')}`;
}

export function recordOrder(db, o, { catalog = null, bomTotals = {}, actor = 'system' } = {}) {
  if (o.external_id) { const dup = db.get('SELECT id FROM orders WHERE external_id = ?', o.external_id); if (dup) return { order: orderById(db, dup.id), created: false }; }
  const unknown = [];
  const items = (o.items || []).map(it => {
    const p = catalog ? resolveSku(catalog, it.sku) || resolveSku(catalog, it.name) : null;
    if (catalog && !p) unknown.push(it.sku || it.name);
    return { sku: p?.sku || it.sku || it.name, name: p?.name || it.name, qty: it.qty || 1, unit_cents: it.unit_cents ?? p?.price_cents ?? 0, cogs_cents: p ? cogsCents(p, bomTotals) : 0 };
  });
  if (unknown.length) throw new Error(`unknown SKU(s): ${unknown.join(', ')} — add them to store.json or fix the Payment Link metadata`);
  return db.tx(() => {
    const contact = o.email ? upsertContact(db, { email: o.email, name: o.name || null, source: 'store' }) : null;
    const number = db.nextNumber('EL');
    const paidAt = o.paid_at || nowIso();
    // A discount: Stripe already took it off the total (it says what it took, even when that is
    // $0 for free shipping, so `discount_cents` arrives set); a manual order names a code with no
    // amount, and the rules in discounts.mjs price it, then the total follows.
    let subtotal = o.subtotal_cents || 0, shipping = o.shipping_cents || 0, tax = o.tax_cents || 0, total = o.total_cents || 0;
    let discountCode = o.discount_code ? String(o.discount_code).trim().toUpperCase() : null, discountCents = Number(o.discount_cents) || 0;
    let discountNote = '';
    if (discountCode && o.discount_cents == null) {
      const ev = evaluateDiscount(db, discountCode, { subtotal_cents: subtotal, shipping_cents: shipping, at: paidAt });
      if (!ev.ok) throw new Error(`discount ${discountCode}: ${ev.reason}`);
      discountCents = ev.discount_cents; if (ev.free_shipping) shipping = 0; discountNote = ev.reason;
      total = Math.max(0, subtotal - (ev.free_shipping ? 0 : discountCents) + shipping + tax);
      if (ev.free_shipping) discountCents = 0;
    }
    const r = db.run(`INSERT INTO orders (number, external_id, payment_intent, source, contact_id, status, currency, subtotal_cents, shipping_cents, tax_cents, total_cents, discount_code, discount_cents, tags,
        ship_name, ship_line1, ship_line2, ship_city, ship_state, ship_zip, ship_country, created_at, paid_at, batch, notes)
      VALUES (?, ?, ?, ?, ?, 'paid', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      number, o.external_id || null, o.payment_intent || null, o.source || 'manual', contact?.id ?? null, o.currency || 'usd',
      subtotal, shipping, tax, total, discountCode, discountCents, normalizeTags(o.tags),
      o.name || contact?.name || '', o.ship?.line1 || '', o.ship?.line2 || '', o.ship?.city || '', (o.ship?.state || '').toUpperCase(), o.ship?.zip || '', (o.ship?.country || 'US').toUpperCase(),
      o.created_at || paidAt, paidAt, weekLabel(new Date(paidAt)), o.notes || null);
    const id = r.lastInsertRowid;
    for (const it of items) db.run('INSERT INTO order_items (order_id, sku, name, qty, unit_cents, cogs_cents) VALUES (?, ?, ?, ?, ?, ?)', id, it.sku, it.name, it.qty, it.unit_cents, it.cogs_cents);
    const order = orderById(db, id);
    postSale(db, order);
    const via = { stripe: 'Stripe Checkout', manual: 'the admin, by hand', invoice: 'an invoice', import: 'a Stripe CSV import' }[order.source] || order.source;
    addEvent(db, id, { kind: 'placed', actor, at: paidAt, text: `Order placed via ${via} · ${items.reduce((s, i) => s + i.qty, 0)} item${items.reduce((s, i) => s + i.qty, 0) === 1 ? '' : 's'} · ${usd(total)}${total === 0 ? ' (no charge)' : ''}` });
    if (discountCode) addEvent(db, id, { kind: 'discount', actor, at: paidAt, text: `Discount ${discountCode} applied${discountNote ? ` (${discountNote})` : ''}: −${usd(discountCents)}${shipping === 0 && discountNote.includes('shipping') ? ', shipping free' : ''}` });
    if (o.notes) addEvent(db, id, { kind: 'note', actor, at: paidAt, text: o.notes });
    db.audit(actor, 'order.create', 'order', number, { external_id: o.external_id, total_cents: total, discount_code: discountCode });
    return { order, created: true };
  });
}

export const usd = (c) => `$${((Number(c) || 0) / 100).toFixed(2)}`;

// ---------- the timeline: one line per thing that happened to the order
export function addEvent(db, orderId, { kind, text, actor = 'system', at = nowIso() }) {
  db.run('INSERT INTO order_events (order_id, at, actor, kind, text) VALUES (?, ?, ?, ?, ?)', orderId, at, actor === 'web' ? currentActor('web') : actor, kind, String(text || '').slice(0, 4000));
  return db.get('SELECT * FROM order_events WHERE id = last_insert_rowid()');
}
export const timeline = (db, orderId) => db.all('SELECT * FROM order_events WHERE order_id = ? ORDER BY at DESC, id DESC', orderId);
export function addNote(db, orderId, text, { actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  const t = String(text || '').trim(); if (!t) throw new Error('the note is empty');
  const ev = addEvent(db, o.id, { kind: 'note', text: t, actor });
  db.audit(actor, 'order.note', 'order', o.number); return ev;
}
export function setTags(db, orderId, tags, { actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  const t = normalizeTags(tags);
  if (t !== o.tags) { db.run('UPDATE orders SET tags = ? WHERE id = ?', t, o.id); addEvent(db, o.id, { kind: 'tags', actor, text: t ? `Tags: ${t}` : 'Tags cleared' }); db.audit(actor, 'order.tags', 'order', o.number, { tags: t }); }
  return orderById(db, orderId);
}
export function updateShipping(db, orderId, ship, { actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (['shipped', 'delivered'].includes(o.status)) throw new Error(`${o.number} already shipped — the address on the label is the address`);
  const next = { name: ship.name ?? o.ship_name, line1: ship.line1 ?? o.ship_line1, line2: ship.line2 ?? o.ship_line2, city: ship.city ?? o.ship_city, state: String(ship.state ?? o.ship_state ?? '').toUpperCase(), zip: ship.zip ?? o.ship_zip, country: String(ship.country ?? o.ship_country ?? 'US').toUpperCase() };
  if (!next.line1 || !next.city || !next.zip) throw new Error('line 1, city and ZIP are required');
  db.run('UPDATE orders SET ship_name = ?, ship_line1 = ?, ship_line2 = ?, ship_city = ?, ship_state = ?, ship_zip = ?, ship_country = ? WHERE id = ?', next.name, next.line1, next.line2 || '', next.city, next.state, next.zip, next.country, o.id);
  addEvent(db, o.id, { kind: 'address', actor, text: `Shipping address changed to ${next.line1}${next.line2 ? ', ' + next.line2 : ''}, ${next.city}, ${next.state} ${next.zip}` });
  db.audit(actor, 'order.address', 'order', o.number); return orderById(db, orderId);
}
export function resendTracking(db, orderId, { cfg = null, actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (!['shipped', 'delivered'].includes(o.status)) throw new Error(`${o.number} has not shipped`); if (!o.email) throw new Error('no email on the order');
  const est = estimateDelivery(o.shipped_at, o.ship_state, { fromState: cfg?.shipFromState, carrier: o.carrier || 'USPS' });
  enqueue(db, { to: o.email, subject: `Your ${cfg?.product || 'SecuraCV'} order ${o.number} is on its way`, kind: 'transactional', ref: `order:${o.number}`, contactId: o.contact_id, text: fill(SHIPPED_TPL, { name: o.ship_name || 'there', number: o.number, carrier: o.carrier || 'USPS', tracking: o.tracking || '(no tracking)', company: cfg?.company || 'Errer Labs', support: cfg?.supportAddr || '', estimate: est ? `Expect it ${est.label}.` : '', status_url: cfg?.secret ? orderStatusUrl(cfg, o) : '' }) });
  addEvent(db, o.id, { kind: 'mail', actor, text: `Tracking email re-sent to ${o.email}` }); db.audit(actor, 'order.resend_tracking', 'order', o.number);
  return o;
}

// The line items can change until the order is packed (a customer adds a parts pack, Stripe sent none):
// the money stays what was charged; COGS and the pick list follow the new lines.
export function editItems(db, orderId, items, { catalog = null, bomTotals = {}, actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (o.status !== 'paid') throw new Error(`${o.number} is ${o.status} — items change only before packing (unpack it first)`);
  const unknown = []; const next = (items || []).filter(it => it && (it.sku || it.name)).map(it => { const p = catalog ? resolveSku(catalog, it.sku) || resolveSku(catalog, it.name) : null; if (catalog && !p) unknown.push(it.sku || it.name); const qty = Math.max(1, Number(it.qty) || 1); return { sku: p?.sku || it.sku || it.name, name: p?.name || it.name || it.sku, qty, unit_cents: it.unit_cents != null ? Number(it.unit_cents) : (p?.price_cents ?? 0), cogs_cents: p ? cogsCents(p, bomTotals) : Number(it.cogs_cents) || 0 }; });
  if (unknown.length) throw new Error(`unknown SKU(s): ${unknown.join(', ')}`); if (!next.length) throw new Error('an order needs at least one line');
  return db.tx(() => {
    db.run('DELETE FROM order_items WHERE order_id = ?', o.id);
    for (const it of next) db.run('INSERT INTO order_items (order_id, sku, name, qty, unit_cents, cogs_cents) VALUES (?, ?, ?, ?, ?, ?)', o.id, it.sku, it.name, it.qty, it.unit_cents, it.cogs_cents);
    addEvent(db, o.id, { kind: 'items', actor, text: `Items changed: ${o.items.map(i => `${i.qty}× ${i.sku}`).join(', ') || 'none'} → ${next.map(i => `${i.qty}× ${i.sku}`).join(', ')} (the charge is unchanged)` });
    db.audit(actor, 'order.items', 'order', o.number, { items: next.map(i => `${i.qty}× ${i.sku}`) });
    return orderById(db, o.id);
  });
}

// Shopify's status tabs: how many orders sit in each state right now.
export const statusCounts = (db) => { const out = Object.fromEntries(STATUSES.map(s => [s, 0])); for (const r of db.all('SELECT status, COUNT(*) AS n FROM orders GROUP BY status')) out[r.status] = r.n; out.all = Object.values(out).reduce((a, b) => a + b, 0); out.open = out.paid + out.packed; return out; };

// The weekly batches, newest first: what each contains and where it stands.
export const batches = (db) => db.all(`SELECT batch, COUNT(*) AS orders, SUM(CASE WHEN status = 'paid' THEN 1 ELSE 0 END) AS to_pack, SUM(CASE WHEN status = 'packed' THEN 1 ELSE 0 END) AS to_ship,
  SUM(CASE WHEN status IN ('shipped','delivered') THEN 1 ELSE 0 END) AS shipped, SUM(CASE WHEN status IN ('canceled','refunded') THEN 1 ELSE 0 END) AS dropped,
  (SELECT COALESCE(SUM(i.qty),0) FROM order_items i JOIN orders o2 ON o2.id = i.order_id WHERE o2.batch = o.batch AND o2.status NOT IN ('canceled','refunded')) AS units,
  SUM(CASE WHEN status NOT IN ('canceled') THEN total_cents - refunded_cents ELSE 0 END) AS sales_cents, MIN(paid_at) AS first_paid_at, MAX(shipped_at) AS last_shipped_at
  FROM orders o GROUP BY batch ORDER BY batch DESC`);

// Several orders at once (the "select all → mark packed" a storefront admin has). Each result says what happened to that order.
export function bulkTransition(db, ids, to, opts = {}) {
  const out = [];
  for (const id of ids) { try { const o = transition(db, Number(id), to, opts); out.push({ id: o.id, number: o.number, status: o.status }); } catch (e) { const o = orderById(db, id); out.push({ id: Number(id), number: o?.number, error: e.message }); } }
  return out;
}

export const orderById = (db, id) => { const o = db.get('SELECT o.*, c.email FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id WHERE o.id = ?', id); if (o) o.items = db.all('SELECT * FROM order_items WHERE order_id = ? ORDER BY id', id); return o; };
export const orderByNumber = (db, n) => { const o = db.get('SELECT id FROM orders WHERE number = ? OR external_id = ? OR payment_intent = ?', n, n, n); return o ? orderById(db, o.id) : null; };
export const listOrders = (db, { status = '', batch = '', q = '', tag = '', from = '', to = '', discount = '', sku = '', limit = 300 } = {}) => db.all(
  `SELECT o.*, c.email, (SELECT GROUP_CONCAT(qty || '× ' || sku, ', ') FROM order_items i WHERE i.order_id = o.id) AS items_text, (SELECT COALESCE(SUM(qty),0) FROM order_items i WHERE i.order_id = o.id) AS units FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id
   WHERE (? = '' OR o.status = ? OR (? = 'open' AND o.status IN ('paid','packed'))) AND (? = '' OR o.batch = ?) AND (? = '' OR o.number LIKE ? OR o.ship_name LIKE ? OR c.email LIKE ? OR o.tracking LIKE ? OR o.external_id LIKE ?)
     AND (? = '' OR (',' || o.tags || ',') LIKE ?) AND (? = '' OR o.paid_at >= ?) AND (? = '' OR o.paid_at < ?) AND (? = '' OR o.discount_code = ?) AND (? = '' OR o.id IN (SELECT order_id FROM order_items WHERE sku = ?))
   ORDER BY o.id DESC LIMIT ?`, status, status, status, batch, batch, q, `%${q}%`, `%${q}%`, `%${q}%`, `%${q}%`, `%${q}%`, tag, `%,${tag},%`, from, from, to, to ? to + 'T23:59:59.999Z' : '', discount, discount.toUpperCase(), sku, sku, limit);

export function transition(db, id, to, { actor = 'web', carrier = null, tracking = null, cfg = null, at = null } = {}) {
  const o = orderById(db, id); if (!o) throw new Error('no such order');
  const allowed = { paid: ['packed', 'canceled', 'refunded'], pending: ['paid', 'canceled'], packed: ['shipped', 'paid'], shipped: ['delivered', 'refunded'], delivered: ['refunded'] };
  if (!(allowed[o.status] || []).includes(to)) throw new Error(`cannot move ${o.number} from ${o.status} to ${to}`);
  return db.tx(() => {
    const now = at || nowIso();
    db.run(`UPDATE orders SET status = ?, paid_at = CASE WHEN ? = 'paid' THEN ? ELSE paid_at END, packed_at = CASE WHEN ? = 'packed' THEN ? ELSE packed_at END,
            shipped_at = CASE WHEN ? = 'shipped' THEN ? ELSE shipped_at END, delivered_at = CASE WHEN ? = 'delivered' THEN ? ELSE delivered_at END WHERE id = ?`,
      to, to, now, to, now, to, now, to, now, id);
    if (to === 'packed') { if (!o.items.length) throw new Error(`${o.number} has no line items — fetch them from Stripe (STRIPE_API_KEY) or add them before packing`); consumeForOrder(db, o); }
    if (to === 'paid' && o.status === 'packed') releaseForOrder(db, o);
    if (to === 'shipped') {
      db.run('UPDATE orders SET carrier = ?, tracking = ? WHERE id = ?', carrier, tracking, id);
      db.run('UPDATE devices SET status = \'shipped\' WHERE order_id = ?', id);
      if (o.email) { const est = estimateDelivery(now, o.ship_state, { fromState: cfg?.shipFromState, carrier: carrier || 'USPS' }); enqueue(db, { to: o.email, subject: `Your ${cfg?.product || 'SecuraCV'} order ${o.number} is on its way`, kind: 'transactional', ref: `order:${o.number}`, contactId: o.contact_id,
        text: fill(SHIPPED_TPL, { name: o.ship_name || 'there', number: o.number, carrier: carrier || 'USPS', tracking: tracking || '(no tracking)', company: cfg?.company || 'Errer Labs', support: cfg?.supportAddr || '', estimate: est ? `Expect it ${est.label} (${est.min_days}–${est.max_days} business days — an estimate, not a promise).` : '', status_url: cfg?.secret ? orderStatusUrl(cfg, o) : '' }) }); }
    }
    const line = { packed: `Packed${o.items.length ? ` · ${o.items.map(i => `${i.qty}× ${i.sku}`).join(', ')} pulled from stock` : ''}`, shipped: `Shipped via ${carrier || 'USPS'}${tracking ? ` · ${tracking}` : ''}${o.email ? ` · tracking emailed to ${o.email}` : ''}`, delivered: 'Delivered', canceled: 'Canceled', refunded: 'Refunded in full', paid: o.status === 'packed' ? 'Unpacked — parts returned to stock' : 'Marked paid' }[to] || `Now ${to}`;
    addEvent(db, id, { kind: to === 'paid' && o.status === 'packed' ? 'unpacked' : to, actor, at: now, text: line });
    db.audit(actor, `order.${to}`, 'order', o.number, { carrier, tracking });
    return orderById(db, id);
  });
}

export function refund(db, id, cents, { actor = 'web', reason = '', at = null } = {}) {
  const o = orderById(db, id); if (!o) throw new Error('no such order');
  const amt = Math.min(Number(cents) || 0, o.total_cents - o.refunded_cents);
  if (amt <= 0) throw new Error('nothing to refund');
  return db.tx(() => {
    const total = o.refunded_cents + amt;
    db.run('UPDATE orders SET refunded_cents = ?, status = CASE WHEN ? >= total_cents THEN \'refunded\' ELSE status END WHERE id = ?', total, total, id);
    postRefund(db, o, amt, reason, (at || nowIso()).slice(0, 10));
    addEvent(db, id, { kind: 'refund', actor, at: at || nowIso(), text: `${total >= o.total_cents ? 'Full' : 'Partial'} refund of ${usd(amt)}${reason ? ` — ${reason}` : ''}${actor === 'stripe' ? ' (reported by Stripe)' : ' (record only — issue it in Stripe)'}` });
    db.audit(actor, 'order.refund', 'order', o.number, { cents: amt, reason });
    return orderById(db, id);
  });
}

// The CSV the website's fulfill.mjs already understands (fulfill-README.md
// "The input CSV"): one row per order, catalog SKU, validated address.
export function fulfillCsv(db, { batch = '', status = 'paid' } = {}) {
  const rows = listOrders(db, { batch, status, limit: 5000 }).reverse();
  const esc = (v) => { const s = String(v ?? ''); return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s; };
  const head = ['order_id', 'email', 'name', 'sku', 'qty', 'line1', 'line2', 'city', 'state', 'zip', 'country'];
  const out = [head.join(',')];
  for (const o of rows) for (const it of db.all('SELECT * FROM order_items WHERE order_id = ?', o.id))
    out.push([o.number, o.email, o.ship_name, it.sku, it.qty, o.ship_line1, o.ship_line2, o.ship_city, o.ship_state, o.ship_zip, o.ship_country].map(esc).join(','));
  return out.join('\n') + '\n';
}

export const SHIPPED_TPL = `Hi {{name}},

Order {{number}} shipped today via {{carrier}}.
Tracking: {{tracking}}
{{estimate}}
Where it is, any time: {{status_url}}

Your Canary provisions itself at home — power it on and point your phone's camera at the QR card in the box. No account, no app store, no cloud.

If anything in the box is wrong, missing or broken, reply to this email. We fix, we don't ask you to send things back (terms of sale §7).

— {{company}}
`;

export const orderStats = (db) => ({
  open: db.get('SELECT COUNT(*) AS n FROM orders WHERE status IN (\'paid\',\'packed\')').n,
  thisBatch: db.get('SELECT COUNT(*) AS n FROM orders WHERE batch = ? AND status NOT IN (\'canceled\',\'refunded\')', weekLabel()).n,
  revenue30: db.get('SELECT COALESCE(SUM(total_cents - refunded_cents),0) AS c FROM orders WHERE paid_at >= ? AND status NOT IN (\'canceled\')', new Date(Date.now() - 30 * 86400000).toISOString()).c,
  units30: db.get('SELECT COALESCE(SUM(i.qty),0) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE o.paid_at >= ? AND o.status NOT IN (\'canceled\',\'refunded\')', new Date(Date.now() - 30 * 86400000).toISOString()).n,
});
