// Purchase orders: what is on order, from whom, arriving when. A reorder
// exception becomes a drafted PO in one click; receiving it moves stock in
// and books the inventory. "Buildable" can then look ahead: now, and when
// the parts land.
import { nowIso } from '../db.mjs';
import { item, move, recipe, lowStock } from './inventory.mjs';
import { postExpense } from './ledger.mjs';

export const STATUSES = ['draft', 'ordered', 'received', 'canceled'];
export function createPo(db, { supplier, supplier_url = null, items, expected_at = null, notes = null, actor = 'web' }) {
  if (!supplier) throw new Error('a supplier is required'); if (!items?.length) throw new Error('a purchase order needs at least one line');
  return db.tx(() => {
    const number = db.nextNumber('PO');
    const r = db.run('INSERT INTO purchase_orders (number, supplier, supplier_url, status, expected_at, notes, created_at) VALUES (?, ?, ?, \'draft\', ?, ?, ?)', number, supplier, supplier_url, expected_at || null, notes, nowIso());
    for (const it of items) {
      const inv = item(db, it.sku); if (!inv) throw new Error(`unknown inventory sku: ${it.sku}`);
      const q = Number(it.qty); if (!Number.isInteger(q) || q <= 0) throw new Error(`${it.sku}: qty must be a positive whole number`);
      const unit = it.unit_cost_cents == null || it.unit_cost_cents === '' ? inv.unit_cost_cents : Number(it.unit_cost_cents); if (!Number.isInteger(unit) || unit < 0) throw new Error(`${it.sku}: unit cost must be a whole number of cents`);
      db.run('INSERT INTO purchase_order_items (po_id, sku, qty, unit_cost_cents) VALUES (?, ?, ?, ?)', r.lastInsertRowid, it.sku, q, unit);
    }
    db.audit(actor, 'po.create', 'po', number, { supplier, lines: items.length });
    return poById(db, r.lastInsertRowid);
  });
}
export const poById = (db, id) => { const p = db.get('SELECT * FROM purchase_orders WHERE id = ?', id); if (p) { p.items = db.all('SELECT i.*, inv.name FROM purchase_order_items i JOIN inventory_items inv ON inv.sku = i.sku WHERE i.po_id = ? ORDER BY i.id', id); p.total_cents = p.items.reduce((s, i) => s + i.qty * i.unit_cost_cents, 0); } return p; };
export const poByNumber = (db, n) => { const p = db.get('SELECT id FROM purchase_orders WHERE number = ?', n); return p ? poById(db, p.id) : null; };
export const listPos = (db, { status = '' } = {}) => db.all(`SELECT p.*, (SELECT COUNT(*) FROM purchase_order_items i WHERE i.po_id = p.id) AS lines, (SELECT COALESCE(SUM(i.qty * i.unit_cost_cents),0) FROM purchase_order_items i WHERE i.po_id = p.id) AS total_cents, (SELECT GROUP_CONCAT(i.qty || '× ' || i.sku, ', ') FROM purchase_order_items i WHERE i.po_id = p.id) AS items_text FROM purchase_orders p WHERE (? = '' OR p.status = ?) ORDER BY CASE p.status WHEN 'draft' THEN 0 WHEN 'ordered' THEN 1 ELSE 2 END, p.id DESC`, status, status);
export function markOrdered(db, id, { expected_at = null, actor = 'web' } = {}) {
  const p = poById(db, id); if (!p) throw new Error('no such purchase order'); if (p.status !== 'draft') throw new Error(`${p.number} is ${p.status}`);
  db.run('UPDATE purchase_orders SET status = \'ordered\', ordered_at = ?, expected_at = COALESCE(?, expected_at) WHERE id = ?', nowIso(), expected_at || null, id);
  db.audit(actor, 'po.order', 'po', p.number, { expected_at }); return poById(db, id);
}
export function cancelPo(db, id, { actor = 'web' } = {}) { const p = poById(db, id); if (!p) throw new Error('no such purchase order'); if (p.status === 'received') throw new Error('already received'); db.run('UPDATE purchase_orders SET status = \'canceled\' WHERE id = ?', id); db.audit(actor, 'po.cancel', 'po', p.number); return poById(db, id); }
// Receiving: each line's quantity moves into stock (ref po:PO-…) and the value is booked as inventory; partial receipts are fine.
export function receivePo(db, id, { received = null, actor = 'web', paidFrom = 'assets:bank' } = {}) {
  const p = poById(db, id); if (!p) throw new Error('no such purchase order'); if (!['ordered', 'draft'].includes(p.status)) throw new Error(`${p.number} is ${p.status}`);
  return db.tx(() => {
    let cents = 0;
    for (const it of p.items) {
      const want = received ? Number(received[it.sku] ?? 0) : it.qty - it.received_qty;
      if (!Number.isInteger(want) || want < 0) throw new Error(`${it.sku}: a received quantity is a whole number, 0 or more`);
      if (want <= 0) continue;
      const q = Math.min(want, it.qty - it.received_qty); if (q <= 0) continue;
      move(db, it.sku, q, 'receive', `po:${p.number}`); db.run('UPDATE purchase_order_items SET received_qty = received_qty + ? WHERE id = ?', q, it.id); cents += q * it.unit_cost_cents;
    }
    if (cents) postExpense(db, { account: 'assets:inventory', cents, memo: `received ${p.number} from ${p.supplier}`, paidFrom, ref: `po:${p.number}` });
    const done = db.all('SELECT qty, received_qty FROM purchase_order_items WHERE po_id = ?', id).every(i => i.received_qty >= i.qty);
    db.run('UPDATE purchase_orders SET status = ?, received_at = CASE WHEN ? THEN ? ELSE received_at END WHERE id = ?', done ? 'received' : 'ordered', done ? 1 : 0, nowIso(), id);
    db.audit(actor, 'po.receive', 'po', p.number, { cents, complete: done }); return poById(db, id);
  });
}
// What is on order per SKU, with the earliest expected date.
export const onOrder = (db) => Object.fromEntries(db.all("SELECT i.sku, SUM(i.qty - i.received_qty) AS qty, MIN(p.expected_at) AS expected_at FROM purchase_order_items i JOIN purchase_orders p ON p.id = i.po_id WHERE p.status = 'ordered' AND i.qty > i.received_qty GROUP BY i.sku").map(r => [r.sku, { qty: r.qty, expected_at: r.expected_at }]));
export const draftedFor = (db, sku) => db.get("SELECT p.* FROM purchase_orders p JOIN purchase_order_items i ON i.po_id = p.id WHERE i.sku = ? AND p.status = 'draft' ORDER BY p.id DESC LIMIT 1", sku);
// A reorder, drafted: every low part grouped by supplier, at the reorder quantity and the last unit cost. A human approves it.
export function draftReorder(db, { skus = null, actor = 'system' } = {}) {
  const low = lowStock(db).filter(i => !skus || skus.includes(i.sku)).filter(i => !onOrder(db)[i.sku] && !draftedFor(db, i.sku));
  const bySupplier = new Map(); for (const i of low) { const k = i.supplier || 'unknown supplier'; if (!bySupplier.has(k)) bySupplier.set(k, { supplier: k, supplier_url: i.supplier_url, items: [] }); bySupplier.get(k).items.push({ sku: i.sku, qty: i.reorder_qty || Math.max(1, i.reorder_point * 2), unit_cost_cents: i.unit_cost_cents }); }
  return [...bySupplier.values()].map(g => createPo(db, { ...g, notes: 'Drafted from the reorder points; approve to mark it ordered.', actor }));
}
// Buildable now, and after what is on order lands.
export function buildableAhead(db, finishedSku) {
  const parts = recipe(db, finishedSku); const oo = onOrder(db);
  if (!parts.length) { const fin = item(db, finishedSku); return { now: fin ? fin.on_hand : null, later: fin ? fin.on_hand + (oo[finishedSku]?.qty || 0) : null, expected_at: oo[finishedSku]?.expected_at || null }; }
  const now = Math.min(...parts.map(p => Math.floor(p.on_hand / p.qty)));
  const later = Math.min(...parts.map(p => Math.floor((p.on_hand + (oo[p.part_sku]?.qty || 0)) / p.qty)));
  const dates = parts.map(p => oo[p.part_sku]?.expected_at).filter(Boolean).sort();
  return { now, later, expected_at: later > now ? dates.at(-1) || null : null };
}
