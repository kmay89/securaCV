// Analytics: the sales numbers a storefront admin shows, computed from the
// orders table on every request (a company of one has thousands of orders,
// not millions). Definitions follow the common storefront vocabulary so the
// numbers compare: gross sales = subtotals before discounts; net sales =
// gross − discounts − refunds; total sales = net + shipping + tax. Canceled
// orders are out of everything; a $0 reship is an order with no sales.
// There are no visitors, sessions or conversion rates here: the site keeps
// none (store-README "No tracking"), so none can be shown.
const DAY = 86400000;
const iso = (ms) => new Date(ms).toISOString();

export function window_(days, now = Date.now()) {
  const to = now; const from = to - days * DAY;
  return { from: iso(from), to: iso(to), prev_from: iso(from - days * DAY), prev_to: iso(from), days };
}

function totals(db, from, to) {
  const r = db.get(`SELECT COUNT(*) AS orders, COALESCE(SUM(subtotal_cents),0) AS gross_cents, COALESCE(SUM(discount_cents),0) AS discounts_cents, COALESCE(SUM(refunded_cents),0) AS refunds_cents,
      COALESCE(SUM(shipping_cents),0) AS shipping_cents, COALESCE(SUM(tax_cents),0) AS tax_cents, COALESCE(SUM(total_cents),0) AS charged_cents, COUNT(DISTINCT contact_id) AS customers
    FROM orders WHERE paid_at >= ? AND paid_at < ? AND status != 'canceled' AND total_cents > 0`, from, to);
  const u = db.get('SELECT COALESCE(SUM(i.qty),0) AS units, COALESCE(SUM(i.qty * i.cogs_cents),0) AS cogs_cents FROM order_items i JOIN orders o ON o.id = i.order_id WHERE o.paid_at >= ? AND o.paid_at < ? AND o.status != \'canceled\' AND o.total_cents > 0', from, to);
  const returning = db.get(`SELECT COUNT(DISTINCT o.contact_id) AS n FROM orders o WHERE o.paid_at >= ? AND o.paid_at < ? AND o.status != 'canceled' AND o.total_cents > 0
    AND EXISTS (SELECT 1 FROM orders p WHERE p.contact_id = o.contact_id AND p.status != 'canceled' AND p.total_cents > 0 AND p.paid_at < o.paid_at)`, from, to).n;
  const net = r.gross_cents - r.discounts_cents - r.refunds_cents;
  return { ...r, units: u.units, cogs_cents: u.cogs_cents, net_cents: net, total_cents: net + r.shipping_cents + r.tax_cents,
    aov_cents: r.orders ? Math.round((r.charged_cents - r.refunds_cents) / r.orders) : 0, returning_customers: returning,
    returning_rate: r.customers ? returning / r.customers : 0, gross_margin: net ? (net - u.cogs_cents) / net : 0 };
}

// One row per bucket (a day under ~5 weeks, a week beyond), every bucket present even when empty.
export function series(db, { from, to, days }) {
  const step = days > 35 ? 7 * DAY : DAY; const start = Date.parse(from), end = Date.parse(to);
  const buckets = []; for (let t = start; t < end; t += step) buckets.push({ at: t, label: iso(t).slice(0, 10), orders: 0, units: 0, net_cents: 0, total_cents: 0, tickets: 0 });
  const idx = (ts) => Math.min(buckets.length - 1, Math.floor((Date.parse(ts) - start) / step));
  for (const o of db.all('SELECT paid_at, subtotal_cents - discount_cents - refunded_cents AS net, total_cents - refunded_cents AS total, (SELECT COALESCE(SUM(qty),0) FROM order_items i WHERE i.order_id = o.id) AS units FROM orders o WHERE paid_at >= ? AND paid_at < ? AND status != \'canceled\' AND total_cents > 0', from, to)) {
    const b = buckets[idx(o.paid_at)]; if (!b) continue; b.orders++; b.units += o.units; b.net_cents += o.net; b.total_cents += o.total;
  }
  for (const t of db.all('SELECT created_at FROM tickets WHERE created_at >= ? AND created_at < ?', from, to)) { const b = buckets[idx(t.created_at)]; if (b) b.tickets++; }
  return { step: step === DAY ? 'day' : 'week', buckets };
}

export function analytics(db, { days = 30, now = Date.now() } = {}) {
  const w = window_(days, now);
  const cur = totals(db, w.from, w.to), prev = totals(db, w.prev_from, w.prev_to);
  const topProducts = db.all(`SELECT i.sku, MAX(i.name) AS name, SUM(i.qty) AS units, SUM(i.qty * i.unit_cents) AS gross_cents, COUNT(DISTINCT o.id) AS orders FROM order_items i JOIN orders o ON o.id = i.order_id
    WHERE o.paid_at >= ? AND o.paid_at < ? AND o.status != 'canceled' AND o.total_cents > 0 GROUP BY i.sku ORDER BY gross_cents DESC, units DESC`, w.from, w.to);
  const gross = topProducts.reduce((s, p) => s + p.gross_cents, 0);
  for (const p of topProducts) p.share = gross ? p.gross_cents / gross : 0;
  const byState = db.all('SELECT ship_state AS state, COUNT(*) AS orders, SUM(total_cents - refunded_cents) AS sales_cents FROM orders WHERE paid_at >= ? AND paid_at < ? AND status != \'canceled\' AND total_cents > 0 GROUP BY ship_state ORDER BY sales_cents DESC', w.from, w.to);
  const byStatus = db.all('SELECT status, COUNT(*) AS n FROM orders WHERE paid_at >= ? AND paid_at < ? GROUP BY status ORDER BY n DESC', w.from, w.to);
  const bySource = db.all('SELECT source, COUNT(*) AS orders, SUM(total_cents - refunded_cents) AS sales_cents FROM orders WHERE paid_at >= ? AND paid_at < ? AND status != \'canceled\' GROUP BY source ORDER BY sales_cents DESC', w.from, w.to);
  const discounts = db.all('SELECT discount_code AS code, COUNT(*) AS uses, SUM(discount_cents) AS discount_cents, SUM(total_cents - refunded_cents) AS sales_cents FROM orders WHERE discount_code IS NOT NULL AND paid_at >= ? AND paid_at < ? AND status != \'canceled\' GROUP BY discount_code ORDER BY uses DESC', w.from, w.to);
  const ful = db.all('SELECT (julianday(shipped_at) - julianday(paid_at)) * 24 AS h FROM orders WHERE shipped_at IS NOT NULL AND paid_at >= ? AND paid_at < ? ORDER BY h', w.from, w.to).map(r => r.h);
  const sup = db.get('SELECT COUNT(*) AS opened, SUM(CASE WHEN status = \'closed\' THEN 1 ELSE 0 END) AS closed FROM tickets WHERE created_at >= ? AND created_at < ?', w.from, w.to);
  const fr = db.all('SELECT (julianday(first_response_at) - julianday(created_at)) * 24 AS h FROM tickets WHERE first_response_at IS NOT NULL AND created_at >= ? AND created_at < ? ORDER BY h', w.from, w.to).map(r => r.h);
  const median = (xs) => xs.length ? Math.round(xs[Math.floor(xs.length / 2)] * 10) / 10 : null;
  const claims = db.get('SELECT COUNT(*) AS n, COALESCE(SUM(cost_cents),0) AS cost_cents FROM warranty_claims WHERE created_at >= ? AND created_at < ?', w.from, w.to);
  const newCustomers = db.get('SELECT COUNT(*) AS n FROM contacts WHERE created_at >= ? AND created_at < ?', w.from, w.to).n;
  const waitlist = db.get('SELECT COUNT(*) AS n FROM waitlist WHERE created_at >= ? AND created_at < ?', w.from, w.to).n;
  return { ...w, totals: cur, previous: prev, series: series(db, w), topProducts, byState, byStatus, bySource, discounts,
    fulfillment: { shipped: ful.length, median_hours: median(ful), within_7d: ful.length ? ful.filter(h => h <= 168).length / ful.length : null },
    support: { opened: sup.opened, closed: sup.closed || 0, median_first_response_h: median(fr) }, warranty: { claims: claims.n, cost_cents: claims.cost_cents, rate: cur.orders ? claims.n / cur.orders : 0 },
    customers: { buying: cur.customers, new: newCustomers, returning: cur.returning_customers, returning_rate: cur.returning_rate, waitlist_joins: waitlist } };
}

export function analyticsCsv(db, opts) {
  const a = analytics(db, opts);
  return [`${a.series.step},orders,units,net_sales_usd,total_sales_usd,tickets`, ...a.series.buckets.map(b => [b.label, b.orders, b.units, (b.net_cents / 100).toFixed(2), (b.total_cents / 100).toFixed(2), b.tickets].join(','))].join('\n') + '\n';
}
