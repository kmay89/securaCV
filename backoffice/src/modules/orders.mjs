// Orders: paid → packed → shipped → delivered. One row per shipment, items
// resolved against the catalog (an unknown SKU is a loud error), COGS frozen
// at the time of sale so the books do not move when the BOM does. Packing an
// order consumes inventory through the recipes; shipping mails the tracking.
import { nowIso } from '../db.mjs';
import { upsertContact } from './contacts.mjs';
import { resolveSku, cogsCents } from './catalog.mjs';
import { consumeForOrder, releaseForOrder } from './inventory.mjs';
import { enqueue, fill } from './mail.mjs';
import { postSale, postRefund } from './ledger.mjs';

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
    const r = db.run(`INSERT INTO orders (number, external_id, payment_intent, source, contact_id, status, currency, subtotal_cents, shipping_cents, tax_cents, total_cents,
        ship_name, ship_line1, ship_line2, ship_city, ship_state, ship_zip, ship_country, created_at, paid_at, batch, notes)
      VALUES (?, ?, ?, ?, ?, 'paid', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      number, o.external_id || null, o.payment_intent || null, o.source || 'manual', contact?.id ?? null, o.currency || 'usd',
      o.subtotal_cents || 0, o.shipping_cents || 0, o.tax_cents || 0, o.total_cents || 0,
      o.name || contact?.name || '', o.ship?.line1 || '', o.ship?.line2 || '', o.ship?.city || '', (o.ship?.state || '').toUpperCase(), o.ship?.zip || '', (o.ship?.country || 'US').toUpperCase(),
      nowIso(), paidAt, weekLabel(new Date(paidAt)), o.notes || null);
    const id = r.lastInsertRowid;
    for (const it of items) db.run('INSERT INTO order_items (order_id, sku, name, qty, unit_cents, cogs_cents) VALUES (?, ?, ?, ?, ?, ?)', id, it.sku, it.name, it.qty, it.unit_cents, it.cogs_cents);
    const order = orderById(db, id);
    postSale(db, order);
    db.audit(actor, 'order.create', 'order', number, { external_id: o.external_id, total_cents: o.total_cents });
    return { order, created: true };
  });
}

export const orderById = (db, id) => { const o = db.get('SELECT o.*, c.email FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id WHERE o.id = ?', id); if (o) o.items = db.all('SELECT * FROM order_items WHERE order_id = ? ORDER BY id', id); return o; };
export const orderByNumber = (db, n) => { const o = db.get('SELECT id FROM orders WHERE number = ? OR external_id = ? OR payment_intent = ?', n, n, n); return o ? orderById(db, o.id) : null; };
export const listOrders = (db, { status = '', batch = '', q = '', limit = 300 } = {}) => db.all(
  `SELECT o.*, c.email, (SELECT GROUP_CONCAT(qty || '× ' || sku, ', ') FROM order_items i WHERE i.order_id = o.id) AS items_text FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id
   WHERE (? = '' OR o.status = ?) AND (? = '' OR o.batch = ?) AND (? = '' OR o.number LIKE ? OR o.ship_name LIKE ? OR c.email LIKE ? OR o.tracking LIKE ?)
   ORDER BY o.id DESC LIMIT ?`, status, status, batch, batch, q, `%${q}%`, `%${q}%`, `%${q}%`, `%${q}%`, limit);

export function transition(db, id, to, { actor = 'web', carrier = null, tracking = null, cfg = null } = {}) {
  const o = orderById(db, id); if (!o) throw new Error('no such order');
  const allowed = { paid: ['packed', 'canceled', 'refunded'], pending: ['paid', 'canceled'], packed: ['shipped', 'paid'], shipped: ['delivered', 'refunded'], delivered: ['refunded'] };
  if (!(allowed[o.status] || []).includes(to)) throw new Error(`cannot move ${o.number} from ${o.status} to ${to}`);
  return db.tx(() => {
    const now = nowIso();
    db.run(`UPDATE orders SET status = ?, paid_at = CASE WHEN ? = 'paid' THEN ? ELSE paid_at END, packed_at = CASE WHEN ? = 'packed' THEN ? ELSE packed_at END,
            shipped_at = CASE WHEN ? = 'shipped' THEN ? ELSE shipped_at END, delivered_at = CASE WHEN ? = 'delivered' THEN ? ELSE delivered_at END WHERE id = ?`,
      to, to, now, to, now, to, now, to, now, id);
    if (to === 'packed') { if (!o.items.length) throw new Error(`${o.number} has no line items — fetch them from Stripe (STRIPE_API_KEY) or add them before packing`); consumeForOrder(db, o); }
    if (to === 'paid' && o.status === 'packed') releaseForOrder(db, o);
    if (to === 'shipped') {
      db.run('UPDATE orders SET carrier = ?, tracking = ? WHERE id = ?', carrier, tracking, id);
      db.run('UPDATE devices SET status = \'shipped\' WHERE order_id = ?', id);
      if (o.email) enqueue(db, { to: o.email, subject: `Your ${cfg?.product || 'SecuraCV'} order ${o.number} is on its way`, kind: 'transactional', ref: `order:${o.number}`,
        text: fill(SHIPPED_TPL, { name: o.ship_name || 'there', number: o.number, carrier: carrier || 'USPS', tracking: tracking || '(no tracking)', company: cfg?.company || 'Errer Labs', support: cfg?.supportAddr || '' }) });
    }
    db.audit(actor, `order.${to}`, 'order', o.number, { carrier, tracking });
    return orderById(db, id);
  });
}

export function refund(db, id, cents, { actor = 'web', reason = '' } = {}) {
  const o = orderById(db, id); if (!o) throw new Error('no such order');
  const amt = Math.min(Number(cents) || 0, o.total_cents - o.refunded_cents);
  if (amt <= 0) throw new Error('nothing to refund');
  return db.tx(() => {
    const total = o.refunded_cents + amt;
    db.run('UPDATE orders SET refunded_cents = ?, status = CASE WHEN ? >= total_cents THEN \'refunded\' ELSE status END, notes = COALESCE(notes, \'\') || ? WHERE id = ?', total, total, reason ? `\nrefund ${amt}: ${reason}` : '', id);
    postRefund(db, o, amt, reason);
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
