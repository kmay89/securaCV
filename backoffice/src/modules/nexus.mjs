// Where sales tax is owed, from the orders themselves. A seller owes a state's sales tax once it has
// "nexus" there: always in the state it ships from, and in any other state once its sales into that
// state pass the state's economic threshold in a year. The thresholds below are the common ones as of
// 2026 (most states: $100,000 in a year, some also 200 orders, a few $250,000 or $500,000; five states
// have no sales tax) — they move, and the state's own page is the word. This watch exists so the
// doctor can say "you owe tax in MA and are 80% of the way in NY" instead of staying quiet until a
// letter arrives. Collecting is the stripe_tax flag; registering is a human's call.
import { isOn } from '../flags.mjs';

const K = 100_00 * 1000;   // $100,000 in cents
export const NO_SALES_TAX = new Set(['AK', 'DE', 'MT', 'NH', 'OR']);
export const THRESHOLDS = {
  default: { sales_cents: K, orders: null, rule: 'or' },
  // states that kept a transaction count beside the dollar threshold
  AR: { sales_cents: K, orders: 200, rule: 'or' }, CT: { sales_cents: K, orders: 200, rule: 'and' }, GA: { sales_cents: K, orders: 200, rule: 'or' }, HI: { sales_cents: K, orders: 200, rule: 'or' },
  IL: { sales_cents: K, orders: 200, rule: 'or' }, KY: { sales_cents: K, orders: 200, rule: 'or' }, MD: { sales_cents: K, orders: 200, rule: 'or' }, MI: { sales_cents: K, orders: 200, rule: 'or' },
  MN: { sales_cents: K, orders: 200, rule: 'or' }, NE: { sales_cents: K, orders: 200, rule: 'or' }, NV: { sales_cents: K, orders: 200, rule: 'or' }, NJ: { sales_cents: K, orders: 200, rule: 'or' },
  OH: { sales_cents: K, orders: 200, rule: 'or' }, RI: { sales_cents: K, orders: 200, rule: 'or' }, VA: { sales_cents: K, orders: 200, rule: 'or' }, VT: { sales_cents: K, orders: 200, rule: 'or' },
  WV: { sales_cents: K, orders: 200, rule: 'or' }, WI: { sales_cents: K, orders: 200, rule: 'or' }, WY: { sales_cents: K, orders: 200, rule: 'or' }, DC: { sales_cents: K, orders: 200, rule: 'or' },
  // higher dollar thresholds
  AL: { sales_cents: 250_00 * 1000, orders: null, rule: 'or' }, MS: { sales_cents: 250_00 * 1000, orders: null, rule: 'or' },
  CA: { sales_cents: 500_00 * 1000, orders: null, rule: 'or' }, TX: { sales_cents: 500_00 * 1000, orders: null, rule: 'or' }, NY: { sales_cents: 500_00 * 1000, orders: 100, rule: 'and' },
};
export const thresholdFor = (state) => THRESHOLDS[String(state || '').toUpperCase()] || THRESHOLDS.default;

// Every ship-to state with sales in the trailing year: how far along it is, and whether tax is owed there.
export function nexusWatch(db, cfg, { now = Date.now(), days = 365 } = {}) {
  const since = new Date(now - days * 86400000).toISOString(); const home = String(cfg.shipFromState || '').toUpperCase();
  const rows = db.all("SELECT ship_state AS state, COUNT(*) AS orders, SUM(total_cents - refunded_cents) AS sales_cents, SUM(tax_cents) AS tax_cents FROM orders WHERE paid_at >= ? AND status != 'canceled' AND total_cents > 0 AND ship_state != '' GROUP BY ship_state", since)
    .map(r => ({ ...r, state: String(r.state).toUpperCase() }));
  if (home && !rows.some(r => r.state === home)) rows.push({ state: home, orders: 0, sales_cents: 0, tax_cents: 0 });
  return rows.map(r => {
    const t = thresholdFor(r.state); const none = NO_SALES_TAX.has(r.state);
    const shareSales = t.sales_cents ? r.sales_cents / t.sales_cents : 0; const shareOrders = t.orders ? r.orders / t.orders : 0;
    const share = t.rule === 'and' ? Math.min(shareSales, t.orders ? shareOrders : shareSales) : Math.max(shareSales, shareOrders);
    const status = none ? 'none' : r.state === home ? 'home' : share >= 1 ? 'over' : share >= 0.8 ? 'near' : 'under';
    return { ...r, home: r.state === home, threshold: t, share, status, collected: r.tax_cents > 0 };
  }).sort((a, b) => ({ home: 0, over: 1, near: 2, under: 3, none: 4 }[a.status] - { home: 0, over: 1, near: 2, under: 3, none: 4 }[b.status]) || b.sales_cents - a.sales_cents);
}

// The one sentence the doctor says, and whether it is a failure.
export function taxVerdict(db, cfg, { now = Date.now() } = {}) {
  const rows = nexusWatch(db, cfg, { now }); const collecting = isOn(db, cfg, 'stripe_tax');
  // the home state once anything has shipped into it; any other state once it is over its line
  const owe = rows.filter(r => (r.status === 'home' && r.orders > 0) || r.status === 'over').map(r => r.state);
  const near = rows.filter(r => r.status === 'near').map(r => r.state);
  const anySales = rows.some(r => r.orders > 0);
  const home = String(cfg.shipFromState || '').toUpperCase();
  if (!anySales) return { ok: null, collecting, owe, near, rows, text: `no sales in the last year · ${collecting ? 'Stripe Tax collects at checkout' : 'collecting nothing'}${home ? ` · home state ${home}` : ''}` };
  if (collecting) return { ok: true, collecting, owe, near, rows, text: `Stripe Tax collects at checkout · owed in ${owe.join(', ') || 'nowhere yet'}${near.length ? ` · near the threshold in ${near.join(', ')}` : ''} — register in each state you owe` };
  if (owe.length) return { ok: false, collecting, owe, near, rows, text: `collecting nothing, but tax is owed in ${owe.join(', ')} (${owe.includes(home) ? 'your home state' + (owe.length > 1 ? ', and over the threshold elsewhere' : '') : 'over the threshold'})` };
  if (near.length) return { ok: false, collecting, owe, near, rows, text: `collecting nothing · ${near.join(', ')} within 20% of the threshold — set up collection before it crosses` };
  return { ok: true, collecting, owe, near, rows, text: `collecting nothing, and owed nowhere yet${home ? ` (no orders into ${home})` : ''} · ${rows.filter(r => r.orders > 0).length} state${rows.filter(r => r.orders > 0).length === 1 ? '' : 's'} under their thresholds` };
}
