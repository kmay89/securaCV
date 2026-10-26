// The store listing, edited here. The website's store.json is the base; what the admin changes about a
// product — its price, its name, this batch's cap, whether it shows at all, the window it sells in — is a
// row here, served by /store/catalog the moment it is saved. The site reads the live catalog, so the price
// on the page and the price Stripe charges are the same number. The file catches up when someone pastes
// the patch this page prints; until then the doctor says how many products the store says differently.
//
// A launch window is the Plus tier's "Launchpad": a product can be listed today and sell from a date.
import { nowIso } from '../db.mjs';

export const FIELDS = ['name', 'price_cents', 'stock', 'hidden', 'available_from', 'available_until', 'note'];
export const listOverrides = (db) => db.all('SELECT * FROM product_overrides ORDER BY sku');
export const overrideFor = (db, sku) => db.get('SELECT * FROM product_overrides WHERE sku = ?', sku) || null;

const isoOrNull = (v, what) => {
  const s = String(v ?? '').trim(); if (!s) return null;
  // a datetime-local value has no zone: it is read as UTC, and the form says so
  const t = Date.parse(/[zZ]|[+-]\d\d:\d\d$/.test(s) ? s : `${s}${s.length === 16 ? ':00' : ''}Z`);
  if (Number.isNaN(t)) throw new Error(`${what} is not a date`);
  return new Date(t).toISOString();
};
const intOrNull = (v, what, { min = 0, max = 10_000_000 } = {}) => {
  const s = String(v ?? '').trim(); if (s === '') return null;
  const n = Number(s); if (!Number.isInteger(n) || n < min || n > max) throw new Error(`${what} is a whole number from ${min} to ${max}`);
  return n;
};

// Save what differs from the file; a row with nothing in it is removed.
export function setOverride(db, sku, fields, { actor = 'web', catalog = null } = {}) {
  const base = catalog?.products?.get(sku) || null;
  if (catalog && !base) throw new Error(`${sku} is not in the catalog`);
  const name = String(fields.name ?? '').trim().slice(0, 120) || null;
  const priceUsd = String(fields.price ?? '').trim();
  const price_cents = fields.price_cents != null && String(fields.price_cents).trim() !== '' ? intOrNull(fields.price_cents, 'price (cents)') : priceUsd === '' ? null : (() => { const n = Number(priceUsd); if (!Number.isFinite(n) || n < 0 || n > 100000) throw new Error('price is dollars, 0 to 100000'); return Math.round(n * 100); })();
  const stock = intOrNull(fields.stock, 'the batch cap', { min: 0, max: 100000 });
  const hidden = fields.hidden === true || fields.hidden === 1 || fields.hidden === '1' || fields.hidden === 'on' ? 1 : 0;
  const available_from = isoOrNull(fields.available_from, 'available from'); const available_until = isoOrNull(fields.available_until, 'available until');
  if (available_from && available_until && available_until <= available_from) throw new Error('the window ends before it starts');
  const note = String(fields.note ?? '').trim().slice(0, 300) || null;
  // the same as the file is not a difference
  const row = { name: base && name === base.name ? null : name, price_cents: base && price_cents === base.price_cents ? null : price_cents, stock: base && stock === (base.stock ?? null) ? null : stock, hidden, available_from, available_until, note };
  const empty = row.name == null && row.price_cents == null && row.stock == null && !row.hidden && !row.available_from && !row.available_until;
  const before = overrideFor(db, sku);
  if (empty) { if (before) { db.run('DELETE FROM product_overrides WHERE sku = ?', sku); db.audit(actor, 'product.listing.clear', 'product', sku, {}); } return null; }
  db.run(`INSERT INTO product_overrides (sku, name, price_cents, stock, hidden, available_from, available_until, note, updated_at, updated_by) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(sku) DO UPDATE SET name = excluded.name, price_cents = excluded.price_cents, stock = excluded.stock, hidden = excluded.hidden, available_from = excluded.available_from, available_until = excluded.available_until, note = excluded.note, updated_at = excluded.updated_at, updated_by = excluded.updated_by`,
    sku, row.name, row.price_cents, row.stock, row.hidden, row.available_from, row.available_until, row.note, nowIso(), actor);
  const changed = Object.fromEntries(Object.entries(row).filter(([k, v]) => v != null && v !== 0 && (before?.[k] ?? null) !== v));
  db.audit(actor, 'product.listing', 'product', sku, { ...changed, was: before ? Object.fromEntries(Object.entries(before).filter(([k]) => FIELDS.includes(k) && before[k] != null && before[k] !== 0)) : undefined });
  return overrideFor(db, sku);
}
export function clearOverride(db, sku, { actor = 'web' } = {}) {
  const n = db.run('DELETE FROM product_overrides WHERE sku = ?', sku).changes; if (n) db.audit(actor, 'product.listing.clear', 'product', sku, {}); return n > 0;
}

// Where a listing stands against its window right now.
export function listingState(p, now = Date.now()) {
  if (!p) return null;
  if (p.hidden) return 'hidden';
  if (p.available_from && Date.parse(p.available_from) > now) return 'not-yet';
  if (p.available_until && Date.parse(p.available_until) <= now) return 'ended';
  return null;
}

// The catalog the store serves: the file, with the rows here laid over it. A new object; the loader's cache is not touched.
export function applyOverrides(db, catalog) {
  if (!catalog?.products) return catalog;
  const rows = new Map(listOverrides(db).map(r => [r.sku, r]));
  if (!rows.size) return { ...catalog, products: new Map([...catalog.products].map(([k, p]) => [k, { ...p, hidden: false, available_from: null, available_until: null, listing: null }])) };
  const products = new Map();
  for (const [sku, p] of catalog.products) {
    const o = rows.get(sku);
    products.set(sku, o ? { ...p, name: o.name ?? p.name, price_cents: o.price_cents ?? p.price_cents, stock: o.stock ?? p.stock, hidden: !!o.hidden, available_from: o.available_from, available_until: o.available_until,
      listing: { edited: true, file: { name: p.name, price_cents: p.price_cents, stock: p.stock ?? null }, note: o.note, updated_at: o.updated_at, updated_by: o.updated_by } }
      : { ...p, hidden: false, available_from: null, available_until: null, listing: null });
  }
  return { ...catalog, products };
}

// What store.json would have to say to match the store: a patch per product, in the file's own units.
export function overridesPatch(db, catalog) {
  const out = {};
  for (const o of listOverrides(db)) {
    const base = catalog?.products?.get(o.sku); const patch = {};
    if (o.name != null) patch.name = o.name;
    if (o.price_cents != null) patch.price = o.price_cents / 100;
    if (o.stock != null) patch.stock = o.stock;
    const notes = [];
    if (o.hidden) notes.push('hidden here — remove the product from the file, or keep it listed and clear this');
    if (o.available_from) notes.push(`sells from ${o.available_from}`); if (o.available_until) notes.push(`until ${o.available_until}`);
    if (!base) notes.push('not in the file any more');
    if (notes.length) patch._note = notes.join('; ');
    out[o.sku] = patch;
  }
  return out;
}
