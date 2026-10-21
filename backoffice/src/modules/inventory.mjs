// Inventory: parts, printed sets, packaging, finished units. Every change is
// a move with a reason, so on-hand is always explainable. Recipes say what a
// finished SKU consumes when it is packed; the reorder point raises an
// exception (doc 20: exceptions summon humans, schedules never do).
import { nowIso } from '../db.mjs';

export function upsertItem(db, it) {
  db.run(`INSERT INTO inventory_items (sku, name, kind, on_hand, reorder_point, reorder_qty, unit_cost_cents, supplier, supplier_url, location, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(sku) DO UPDATE SET name = excluded.name, kind = excluded.kind, reorder_point = excluded.reorder_point, reorder_qty = excluded.reorder_qty,
      unit_cost_cents = excluded.unit_cost_cents, supplier = excluded.supplier, supplier_url = excluded.supplier_url, location = excluded.location, updated_at = excluded.updated_at`,
    it.sku, it.name, it.kind || 'part', it.on_hand || 0, it.reorder_point || 0, it.reorder_qty || 0, it.unit_cost_cents || 0, it.supplier || null, it.supplier_url || null, it.location || null, nowIso());
  return item(db, it.sku);
}
export const item = (db, sku) => db.get('SELECT * FROM inventory_items WHERE sku = ?', sku);
export const listItems = (db) => db.all('SELECT *, (on_hand <= reorder_point AND reorder_point > 0) AS low FROM inventory_items ORDER BY kind, sku');

export function move(db, sku, delta, reason, ref = null) {
  const it = item(db, sku); if (!it) throw new Error(`unknown inventory sku: ${sku}`);
  if (it.on_hand + delta < 0) throw new Error(`${sku}: on hand ${it.on_hand}, cannot move ${delta}`);
  db.run('UPDATE inventory_items SET on_hand = on_hand + ?, updated_at = ? WHERE sku = ?', delta, nowIso(), sku);
  db.run('INSERT INTO inventory_moves (sku, delta, reason, ref, at) VALUES (?, ?, ?, ?, ?)', sku, delta, reason, ref, nowIso());
  return item(db, sku);
}
export const moves = (db, sku = '', limit = 200) => db.all('SELECT * FROM inventory_moves WHERE (? = \'\' OR sku = ?) ORDER BY id DESC LIMIT ?', sku, sku, limit);

export function setRecipe(db, finishedSku, parts) {
  db.tx(() => {
    db.run('DELETE FROM recipes WHERE finished_sku = ?', finishedSku);
    for (const [part, qty] of Object.entries(parts)) { if (!item(db, part)) throw new Error(`recipe part not in inventory: ${part}`); db.run('INSERT INTO recipes (finished_sku, part_sku, qty) VALUES (?, ?, ?)', finishedSku, part, Number(qty) || 1); }
  });
}
export const recipe = (db, finishedSku) => db.all('SELECT r.part_sku, r.qty, i.name, i.on_hand FROM recipes r JOIN inventory_items i ON i.sku = r.part_sku WHERE r.finished_sku = ?', finishedSku);
export const recipes = (db) => db.all('SELECT finished_sku, GROUP_CONCAT(qty || \'× \' || part_sku, \', \') AS parts FROM recipes GROUP BY finished_sku');

// Packing an order: a finished SKU with its own stock row is taken from
// that; otherwise its recipe parts are. A SKU with neither is a no-op
// (printed-on-demand, or not tracked yet) — the move log says nothing
// happened, which is the honest answer.
export function consumeForOrder(db, order) {
  const done = [];
  for (const it of order.items || []) {
    const fin = item(db, it.sku);
    if (fin && fin.kind === 'finished') { move(db, it.sku, -it.qty, 'pack', `order:${order.number}`); done.push([it.sku, it.qty]); continue; }
    for (const p of recipe(db, it.sku)) { move(db, p.part_sku, -(p.qty * it.qty), 'pack', `order:${order.number}`); done.push([p.part_sku, p.qty * it.qty]); }
  }
  return done;
}

// Undoing a pack puts back exactly what packing took: the net of this
// order's pack/unpack moves per SKU, so pack → unpack → pack never double-counts.
export function releaseForOrder(db, order) {
  const net = db.all('SELECT sku, SUM(delta) AS d FROM inventory_moves WHERE ref = ? AND reason IN (\'pack\',\'unpack\') GROUP BY sku HAVING SUM(delta) <> 0', `order:${order.number}`);
  for (const r of net) move(db, r.sku, -r.d, 'unpack', `order:${order.number}`);
  return net.map(r => [r.sku, -r.d]);
}

// How many of each sellable SKU can be built from what is on the shelf —
// the honest number behind store.json's per-batch `stock`.
export function buildable(db, finishedSku) {
  const fin = item(db, finishedSku);
  const parts = recipe(db, finishedSku);
  if (!parts.length) return fin ? fin.on_hand : null;
  return Math.min(...parts.map(p => Math.floor(p.on_hand / p.qty)));
}

export const lowStock = (db) => listItems(db).filter(i => i.low);
export const inventoryValueCents = (db) => db.get('SELECT COALESCE(SUM(on_hand * unit_cost_cents),0) AS c FROM inventory_items').c;
