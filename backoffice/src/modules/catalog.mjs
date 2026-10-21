// The catalog is NOT ours to own: the website's store.json is the single
// source for SKUs, prices, COGS recipes and ship weights (store-README.md).
// We read it — from a path or the live URL — and cache it. A SKU we don't
// recognize is an error, never a guess (the same rule fulfill.mjs keeps).
import { readFileSync } from 'node:fs';

let cache = { at: 0, data: null, from: null };

export async function loadCatalog(cfg, { force = false, fetchImpl = fetch } = {}) {
  const src = cfg.storeJson;
  if (!force && cache.data && cache.from === src && Date.now() - cache.at < 10 * 60_000) return cache.data;
  let json;
  if (/^https?:\/\//.test(src)) {
    const r = await fetchImpl(src, { headers: { accept: 'application/json' } });
    if (!r.ok) throw new Error(`catalog fetch ${r.status} from ${src}`);
    json = await r.json();
  } else json = JSON.parse(readFileSync(src, 'utf8'));
  const data = normalize(json);
  cache = { at: Date.now(), data, from: src };
  return data;
}

export function normalize(store) {
  const products = new Map();
  for (const g of store.groups || []) for (const p of g.products || []) {
    products.set(p.id, {
      sku: p.id, name: p.name, price_cents: Math.round((p.price || 0) * 100), radio: !!p.radio,
      group: g.id || g.title || '', ship: p.ship || null, cogs: p.cogs || null, link: p.link || '', stock: p.stock ?? null,
      authorization: p.authorization || null,
    });
  }
  return { mode: store.mode, products, shipping_flat_cents: Math.round((store.shipping_flat || 0) * 100), fulfillment: store.fulfillment || {}, compliance: store.compliance || {} };
}

export function resolveSku(catalog, skuOrName) {
  if (!skuOrName) return null;
  const s = String(skuOrName).trim();
  if (catalog.products.has(s)) return catalog.products.get(s);
  const low = s.toLowerCase();
  for (const p of catalog.products.values()) if (p.name.toLowerCase() === low) return p;
  return null;
}

// COGS per unit in cents: the operator's `extra_usd` plus each device BOM's
// required total when a build.json is available (the margin guard's join).
export function cogsCents(product, bomTotals = {}) {
  if (!product?.cogs) return 0;
  let usd = Number(product.cogs.extra_usd || 0);
  for (const [dev, n] of Object.entries(product.cogs.devices || {})) usd += Number(bomTotals[dev] || 0) * n;
  return Math.round(usd * 100);
}
