// Products: the catalog (store.json, not ours to edit) joined to what this
// database knows about each SKU — stock that can be built, units sold, money
// made, who is waiting, what came back. A storefront admin's Products page,
// minus the editor: price and copy change on the website, in git.
import { buildable, recipe } from './inventory.mjs';

export function listProducts(db, catalog, { days = 30, now = Date.now() } = {}) {
  const since = new Date(now - days * 86400000).toISOString();
  const sales = Object.fromEntries(db.all(`SELECT i.sku, SUM(CASE WHEN o.paid_at >= ? THEN i.qty ELSE 0 END) AS units_recent, SUM(CASE WHEN o.paid_at >= ? THEN i.qty * i.unit_cents ELSE 0 END) AS gross_recent, SUM(i.qty) AS units_all, SUM(i.qty * i.unit_cents) AS gross_all,
      MAX(o.paid_at) AS last_sold_at, MAX(CASE WHEN i.cogs_cents > 0 THEN i.cogs_cents ELSE NULL END) AS cogs_cents FROM order_items i JOIN orders o ON o.id = i.order_id WHERE o.status != 'canceled' AND o.total_cents > 0 GROUP BY i.sku`, since, since).map(r => [r.sku, r]));
  const open = Object.fromEntries(db.all('SELECT i.sku, SUM(i.qty) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE o.status IN (\'paid\',\'packed\') GROUP BY i.sku').map(r => [r.sku, r.n]));
  const waiting = Object.fromEntries(db.all('SELECT sku, COUNT(*) AS people, SUM(qty) AS units FROM waitlist WHERE notified_at IS NULL GROUP BY sku').map(r => [r.sku, r]));
  const claims = Object.fromEntries(db.all('SELECT i.sku, COUNT(DISTINCT w.id) AS n FROM warranty_claims w JOIN order_items i ON i.order_id = w.order_id GROUP BY i.sku').map(r => [r.sku, r.n]));
  return [...catalog.products.values()].map(p => {
    const s = sales[p.sku] || {}; const b = buildable(db, p.sku);
    const cogs = s.cogs_cents ?? cogsGuess(p);
    return { sku: p.sku, name: p.name, group: p.group, price_cents: p.price_cents, radio: p.radio, cogs_cents: cogs, margin: p.price_cents ? (p.price_cents - cogs) / p.price_cents : 0,
      buildable: b, stock: b == null ? 'untracked' : b === 0 ? 'out' : b <= 3 ? 'low' : 'in', open_units: open[p.sku] || 0,
      units_recent: s.units_recent || 0, gross_recent: s.gross_recent || 0, units_all: s.units_all || 0, gross_all: s.gross_all || 0, last_sold_at: s.last_sold_at || null,
      waitlist_people: waiting[p.sku]?.people || 0, waitlist_units: waiting[p.sku]?.units || 0, claims: claims[p.sku] || 0 };
  });
}
// Without a BOM table, the catalog's own `extra_usd` is the floor of the cost.
const cogsGuess = (p) => Math.round(Number(p.cogs?.extra_usd || 0) * 100);

export function productDetail(db, catalog, sku, { weeks = 12, now = Date.now() } = {}) {
  const p = catalog.products.get(sku); if (!p) return null;
  const row = listProducts(db, catalog, { now }).find(x => x.sku === sku);
  const parts = recipe(db, sku).map(r => ({ ...r, builds: Math.floor(r.on_hand / r.qty) }));
  const orders = db.all('SELECT o.*, c.email, i.qty AS qty FROM order_items i JOIN orders o ON o.id = i.order_id LEFT JOIN contacts c ON c.id = o.contact_id WHERE i.sku = ? ORDER BY o.id DESC LIMIT 25', sku);
  const start = now - weeks * 7 * 86400000;
  const buckets = Array.from({ length: weeks }, (_, i) => ({ label: new Date(start + i * 7 * 86400000).toISOString().slice(0, 10), units: 0 }));
  for (const r of db.all('SELECT o.paid_at, i.qty FROM order_items i JOIN orders o ON o.id = i.order_id WHERE i.sku = ? AND o.paid_at >= ? AND o.status != \'canceled\' AND o.total_cents > 0', sku, new Date(start).toISOString())) {
    const b = buckets[Math.min(weeks - 1, Math.floor((Date.parse(r.paid_at) - start) / (7 * 86400000)))]; if (b) b.units += r.qty;
  }
  const waitlist = db.all('SELECT w.*, c.email, c.name FROM waitlist w JOIN contacts c ON c.id = w.contact_id WHERE w.sku = ? ORDER BY w.created_at', sku);
  const claims = db.all('SELECT DISTINCT w.* FROM warranty_claims w JOIN order_items i ON i.order_id = w.order_id WHERE i.sku = ? ORDER BY w.id DESC', sku);
  return { product: p, ...row, parts, orders, weekly: buckets, waitlist, claimRows: claims };
}
