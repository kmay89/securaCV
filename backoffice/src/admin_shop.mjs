// The storefront half of the admin: products, customers, discounts,
// analytics, batches, search, packing slips. Same conventions as admin.mjs —
// server-rendered, plain forms, no client script.
import { redirect, text, HttpError, escapeHtml as e, csvLine } from './http.mjs';
import { table, money, day, statusPill, field, form, cards, tags as tagsHtml, tabs } from './render.mjs';
import { columns, line, hbars, tile } from './charts.mjs';
import { listOrders, orderById, bulkTransition, batches, setTags, weekLabel } from './modules/orders.mjs';
import { listCustomers, contactById, customerStats, customerCounts, setContactTags } from './modules/contacts.mjs';
import { listDiscounts, createDiscount, setDiscountStatus, evaluate, describe, KINDS } from './modules/discounts.mjs';
import { analytics, analyticsCsv } from './modules/analytics.mjs';
import { buildableAhead } from './modules/purchasing.mjs';
import { item } from './modules/inventory.mjs';
import { listProducts, productDetail } from './modules/products.mjs';
import { search } from './modules/search.mjs';

export function registerShop(app, { page, back, link, filters }) {
  const { db, cfg, router } = app;
  const pct = (x) => `${Math.round((x || 0) * 100)}%`;
  const customerLink = (r) => r.contact_id ? link(`/contacts/${r.contact_id}`, r.ship_name || r.email || 'customer') : e(r.ship_name || '');

  // ---------- products
  router.get('/products', async (c) => {
    const catalog = await app.catalog().catch(() => null); if (!catalog) throw new HttpError(503, 'the catalog (store.json) could not be loaded');
    const rows = listProducts(db, catalog);
    const stockPill = (s) => ({ in: '<span class="ok">● in stock</span>', low: '<span class="warn">● low</span>', out: '<span class="bad">● out</span>', untracked: '<span class="muted">— untracked</span>' }[s]);
    page(c, 'Products', `<p class="muted">The catalog is the website's <code>store.json</code> — prices and copy change there, in git. This page is what the business knows about each SKU: what can be built from the shelf, what sold, who is waiting. ${link('/inventory', 'Parts and recipes')} · ${link('/products.csv', 'CSV')}</p>` +
      cards([['Products', rows.length], ['Units sold, 30 days', rows.reduce((s, r) => s + r.units_recent, 0)], ['Gross, 30 days', money(rows.reduce((s, r) => s + r.gross_recent, 0))], ['Out or low', rows.filter(r => r.stock === 'out' || r.stock === 'low').length, rows.some(r => r.stock === 'out') ? 'bad' : rows.some(r => r.stock === 'low') ? 'warn' : 'ok'], ['People waiting', rows.reduce((s, r) => s + r.waitlist_people, 0)]]) +
      table(rows, [{ label: 'Product', html: r => `${link(`/products/${encodeURIComponent(r.sku)}`, r.name)}<br><span class="muted">${e(r.sku)} · ${e(r.group)}</span>` }, { label: 'Price', get: r => money(r.price_cents), num: true }, { label: 'Margin', html: r => `<span class="${r.margin < 0.3 ? 'warn' : ''}">${pct(r.margin)}</span>`, num: true }, { label: 'Stock', html: r => `${stockPill(r.stock)}${r.buildable != null ? ` <span class="muted">${r.buildable} buildable</span>` : ''}` }, { label: 'Open', key: 'open_units', num: true }, { label: '30 d', get: r => r.units_recent, num: true }, { label: 'Gross 30 d', get: r => money(r.gross_recent), num: true }, { label: 'All time', get: r => `${r.units_all}`, num: true }, { label: 'Waiting', get: r => r.waitlist_people ? `${r.waitlist_people} (${r.waitlist_units} u)` : '', num: true }, { label: 'Claims', get: r => r.claims || '', num: true }]));
  });
  router.get('/products.csv', async (c) => { const catalog = await app.catalog(); const rows = listProducts(db, catalog); text(c.res, ['sku,name,group,price_usd,cogs_usd,margin,buildable,open_units,units_30d,gross_30d_usd,units_all,gross_all_usd,waitlist_people,claims', ...rows.map(r => csvLine([r.sku, r.name, r.group, (r.price_cents / 100).toFixed(2), (r.cogs_cents / 100).toFixed(2), r.margin.toFixed(3), r.buildable ?? '', r.open_units, r.units_recent, (r.gross_recent / 100).toFixed(2), r.units_all, (r.gross_all / 100).toFixed(2), r.waitlist_people, r.claims]))].join('\n') + '\n', 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="products.csv"' }); });
  router.get('/products/:sku', async (c) => {
    const catalog = await app.catalog().catch(() => null); if (!catalog) throw new HttpError(503, 'catalog unavailable');
    const p = productDetail(db, catalog, c.params.sku); if (!p) { if (item(db, c.params.sku)) return redirect(c.res, `/inventory/${encodeURIComponent(c.params.sku)}`); throw new HttpError(404, 'no such product'); }
    page(c, p.name, `<p>${e(p.sku)} · ${e(p.group)} · ${money(p.price_cents)}${p.radio ? ' · <span class="pill">radio</span>' : ''} · ${link(`/orders?sku=${encodeURIComponent(p.sku)}`, 'orders with it')}</p>` +
      cards([['Buildable now', p.buildable ?? '—', p.stock === 'out' ? 'bad' : p.stock === 'low' ? 'warn' : ''], ...(() => { const b = buildableAhead(db, p.sku); return b.later > (b.now ?? 0) ? [['After parts on order land', `${b.later} <span class="muted" style="font-size:.8rem">${b.expected_at ? 'by ' + e(b.expected_at) : ''}</span>`, 'ok']] : []; })(), ['Open orders (units)', p.open_units], ['Sold, 30 days', p.units_recent], ['Gross, 30 days', money(p.gross_recent)], ['Sold, all time', p.units_all], ['Margin', pct(p.margin), p.margin < 0.3 ? 'warn' : 'ok'], ['Waiting', p.waitlist_people], ['Warranty claims', p.claims, p.claims ? 'warn' : 'ok']]) +
      columns({ points: p.weekly.map(w => ({ label: w.label, value: w.units })), title: 'Units sold per week, 12 weeks', color: 'var(--s2)' }) +
      `<h2>What one unit consumes</h2>${table(p.parts, [{ label: 'Part', html: r => `${e(r.name)} <span class="muted">${e(r.part_sku)}</span>` }, { label: 'Per unit', key: 'qty', num: true }, { label: 'On hand', key: 'on_hand', num: true }, { label: 'Builds', html: r => `<span class="${r.builds === 0 ? 'bad' : r.builds <= 3 ? 'warn' : ''}">${r.builds}</span>`, num: true }], { empty: 'No recipe — set one on the Inventory page so packing pulls parts.' })}
<h2>Recent orders</h2>${table(p.orders, [{ label: '#', html: r => link(`/orders/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Paid', get: r => day(r.paid_at) }, { label: 'Customer', html: customerLink }, { label: 'Qty', key: 'qty', num: true }, { label: 'Total', get: r => money(r.total_cents), num: true }])}
<h2>Waiting for stock</h2>${table(p.waitlist, [{ label: 'Email', html: r => link(`/contacts/${r.contact_id}`, r.email) }, { label: 'Name', key: 'name' }, { label: 'Qty', key: 'qty', num: true }, { label: 'Since', get: r => day(r.created_at) }, { label: 'Notified', get: r => r.notified_at ? day(r.notified_at) : 'not yet' }], { empty: 'Nobody waiting.' })}
<h2>Warranty claims</h2>${table(p.claimRows, [{ label: '#', html: r => link(`/warranty/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Category', key: 'category' }, { label: 'Opened', get: r => day(r.created_at) }], { empty: 'None.' })}`);
  });

  // ---------- customers
  router.get('/contacts', (c) => {
    const seg = c.query.get('segment') || ''; const sort = c.query.get('sort') || 'recent';
    const rows = listCustomers(db, { q: c.query.get('q') || '', tag: c.query.get('tag') || '', segment: seg, sort });
    const n = customerCounts(db);
    const qs = (o) => '/contacts?' + new URLSearchParams({ segment: seg, sort, q: c.query.get('q') || '', tag: c.query.get('tag') || '', ...o }).toString();
    page(c, 'Customers', tabs([[qs({ segment: '' }), 'All', n.all, ''], [qs({ segment: 'repeat' }), 'Repeat buyers', n.repeat, 'repeat'], [qs({ segment: 'consented' }), 'Marketing opt-in', n.consented, 'consented'], [qs({ segment: 'waitlist' }), 'On a waitlist', n.waitlist, 'waitlist'], [qs({ segment: 'orgs' }), 'Schools & orgs', n.orgs, 'orgs'], [qs({ segment: 'never' }), 'Never ordered', null, 'never']], seg) +
      filters(c, [['q', 'Search email / name / org'], ['tag', 'Tag'], ['sort', 'Sort', [['recent', 'most recent'], ['spent', 'highest spend'], ['orders', 'most orders'], ['name', 'name']]]]) +
      `<p class="muted">${rows.length} shown · ${link('/contacts.csv', 'CSV')} · ${link('/campaigns', 'Mail a segment')}</p>` +
      table(rows, [{ label: 'Customer', html: r => `${link(`/contacts/${r.id}`, r.name || r.email)}<br><span class="muted">${e(r.email)}${r.org ? ' · ' + e(r.org) : ''}</span>` }, { label: 'Tags', html: r => tagsHtml(r.tags) + (r.kind !== 'person' && !String(r.tags || '').split(',').includes(r.kind) ? `<span class="pill">${e(r.kind)}</span>` : '') }, { label: 'Orders', key: 'orders', num: true }, { label: 'Spent', get: r => money(r.spent_cents), num: true }, { label: 'Last order', get: r => r.last_order_at ? day(r.last_order_at) : '—' }, { label: 'Tickets', get: r => r.tickets || '', num: true }, { label: 'Marketing', html: r => r.consent_marketing && !r.unsubscribed_at ? '<span class="ok">opted in</span>' : r.unsubscribed_at ? '<span class="bad">unsubscribed</span>' : '<span class="muted">no</span>' }, { label: 'Since', get: r => day(r.created_at) }]) +
      form('/contacts/new', c.csrf, `<b>Add a customer</b><div class="row">${field('email', 'Email', { type: 'email', required: true })}${field('name', 'Name')}${field('org', 'Org')}${field('kind', 'Kind', { options: ['person', 'school', 'business', 'nonprofit', 'press', 'vendor'] })}${field('tags', 'Tags (comma-separated)')}</div>`, { submit: 'Add' }));
  });
  router.get('/contacts.csv', (c) => { const rows = listCustomers(db, { limit: 100000 }); text(c.res, ['email,name,org,kind,tags,orders,spent_usd,first_order,last_order,marketing_opt_in,since', ...rows.map(r => csvLine([r.email, r.name || '', r.org || '', r.kind, r.tags, r.orders, (r.spent_cents / 100).toFixed(2), day(r.first_order_at), day(r.last_order_at), r.consent_marketing && !r.unsubscribed_at ? 'yes' : 'no', day(r.created_at)]))].join('\n') + '\n', 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="customers.csv"' }); });
  router.post('/contacts/:id/tags', (c) => { setContactTags(db, Number(c.params.id), c.body.tags || ''); back(c, `/contacts/${c.params.id}`, 'tags saved'); });

  // ---------- discounts
  router.get('/discounts', (c) => {
    const rows = listDiscounts(db);
    const tryQ = c.query.get('try'); let tried = '';
    if (tryQ) { const ev = evaluate(db, tryQ, { subtotal_cents: Number(c.query.get('subtotal') || 0), shipping_cents: 800 }); tried = `<div class="flash ${ev.ok ? '' : 'err'}"><b>${e(tryQ.toUpperCase())}</b> on a $${(Number(c.query.get('subtotal') || 0) / 100).toFixed(2)} subtotal: ${ev.ok ? `takes off ${money(ev.discount_cents)}${ev.free_shipping ? ' (ships free)' : ''} — ${e(ev.reason)}` : e(ev.reason)}</div>`; }
    page(c, 'Discounts', `<p class="muted">Codes Stripe applies at checkout, mirrored here with their rules so the books know what each one cost (the <code>income:discounts</code> account). A manual order or the API can apply one too, by the same rules. ${link('/analytics', 'Performance by code')}</p>${tried}` +
      cards([['Active codes', rows.filter(r => r.status === 'active').length], ['Uses, all time', rows.reduce((s, r) => s + r.uses, 0)], ['Given away', money(rows.reduce((s, r) => s + r.given_cents, 0))], ['Sales with a code', money(rows.reduce((s, r) => s + r.sales_cents, 0))]]) +
      table(rows, [{ label: 'Code', html: r => `<b>${e(r.code)}</b>${r.note ? `<br><span class="muted">${e(r.note)}</span>` : ''}` }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Takes off', get: r => describe(r) }, { label: 'Minimum', get: r => r.min_subtotal_cents ? money(r.min_subtotal_cents) : '—', num: true }, { label: 'Runs', get: r => `${r.starts_at ? day(r.starts_at) : 'now'} → ${r.ends_at ? day(r.ends_at) : 'open'}` }, { label: 'Uses', get: r => `${r.uses}${r.max_uses ? ` / ${r.max_uses}` : ''}`, num: true }, { label: 'Given', get: r => money(r.given_cents), num: true }, { label: 'Sales', get: r => money(r.sales_cents), num: true }, { label: '', html: r => `${link(`/orders?discount=${encodeURIComponent(r.code)}`, 'orders')} · <form method="post" action="/discounts/${e(r.code)}/status" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="status" value="${r.status === 'active' ? 'disabled' : 'active'}"><button class="quiet">${r.status === 'active' ? 'disable' : 'enable'}</button></form>` }], { empty: 'No codes yet.' }) +
      `<div class="two">${form('/discounts', c.csrf, `<b>New code</b><div class="row">${field('code', 'Code', { required: true, placeholder: 'LAUNCH10' })}${field('kind', 'Kind', { options: KINDS.map(k => [k, { percent: 'percent off', fixed: 'fixed amount off', free_shipping: 'free shipping' }[k]]) })}${field('value', 'Value (percent, or cents for fixed)', { type: 'number', value: 10 })}</div><div class="row">${field('min_subtotal_cents', 'Minimum subtotal (cents)', { type: 'number', value: 0 })}${field('max_uses', 'Max uses (blank = unlimited)', { type: 'number' })}</div><div class="row">${field('starts_at', 'Starts', { type: 'date' })}${field('ends_at', 'Ends', { type: 'date' })}</div>${field('note', 'Note (what it is for)')}<p class="muted">Create the same promotion code in Stripe so Checkout honors it; this side keeps the rules and the cost.</p>`, { submit: 'Create code' })}
<form method="get" action="/discounts" class="box"><b>Try a code</b><div class="row">${field('try', 'Code', { required: true })}${field('subtotal', 'Subtotal (cents)', { type: 'number', value: 12900 })}</div><div style="margin-top:.8rem"><button class="quiet">Evaluate</button></div></form></div>`);
  });
  router.post('/discounts', (c) => { const d = createDiscount(db, { ...c.body, starts_at: c.body.starts_at ? c.body.starts_at + 'T00:00:00.000Z' : null, ends_at: c.body.ends_at ? c.body.ends_at + 'T23:59:59.999Z' : null }); back(c, '/discounts', `created ${d.code}`); });
  router.post('/discounts/:code/status', (c) => { setDiscountStatus(db, c.params.code, c.body.status); back(c, '/discounts', `${c.params.code.toUpperCase()} ${c.body.status}`); });

  // ---------- analytics
  router.get('/analytics', (c) => {
    const days = [7, 30, 90, 365].includes(Number(c.query.get('days'))) ? Number(c.query.get('days')) : 30;
    const a = analytics(db, { days }); const t = a.totals, p = a.previous;
    const period = tabs([7, 30, 90, 365].map(d => [`/analytics?days=${d}`, { 7: 'Last 7 days', 30: 'Last 30 days', 90: 'Last 90 days', 365: 'Last year' }[d], null, d]), days);
    const tiles = `<div class="cards">${tile('Total sales', money(t.total_cents), { prev: p.total_cents, cur: t.total_cents })}${tile('Net sales', money(t.net_cents), { prev: p.net_cents, cur: t.net_cents })}${tile('Orders', t.orders, { prev: p.orders, cur: t.orders })}${tile('Average order', money(t.aov_cents), { prev: p.aov_cents, cur: t.aov_cents })}${tile('Units', t.units, { prev: p.units, cur: t.units })}${tile('Gross margin', pct(t.gross_margin), { prev: p.gross_margin, cur: t.gross_margin })}${tile('Returning customers', pct(t.returning_rate), { prev: p.returning_rate, cur: t.returning_rate })}${tile('Refunds', money(t.refunds_cents), { prev: p.refunds_cents, cur: t.refunds_cents, invert: true })}${tile('Discounts', money(t.discounts_cents), { prev: p.discounts_cents, cur: t.discounts_cents, invert: true })}</div>`;
    const pts = (k) => a.series.buckets.map(b => ({ label: b.label, value: b[k] }));
    const body = `${period}<p class="muted">${day(a.from)} → ${day(a.to)}, compared with the ${days} days before. Gross sales are subtotals before discounts; net = gross − discounts − refunds; total = net + shipping + tax. Canceled orders and $0 reships are out. No visitor numbers: the site keeps none. ${link(`/analytics.csv?days=${days}`, 'CSV')}</p>${tiles}
<div class="two">${line({ points: pts('net_cents'), title: `Net sales per ${a.series.step}`, money: true })}${columns({ points: pts('orders'), title: `Orders per ${a.series.step}`, color: 'var(--s2)' })}</div>
<div class="two">${hbars({ rows: a.topProducts.slice(0, 8).map(r => ({ label: r.name, value: r.gross_cents, extra: `${r.units} u · ${pct(r.share)}` })), title: 'Top products by gross sales', money: true })}${hbars({ rows: a.byState.slice(0, 8).map(r => ({ label: r.state || '—', value: r.sales_cents, extra: `${r.orders} orders` })), title: 'Sales by ship-to state', money: true, color: 'var(--s3)' })}</div>
<div class="two">${columns({ points: pts('tickets'), title: `Support tickets opened per ${a.series.step}`, color: 'var(--s4)' })}${hbars({ rows: a.byStatus.map(r => ({ label: r.status, value: r.n })), title: 'Orders in the period, by status now', color: 'var(--s2)' })}</div>
<div class="cards">${tile('Shipped', a.fulfillment.shipped)}${tile('Paid → shipped, median', a.fulfillment.median_hours == null ? '—' : `${Math.round(a.fulfillment.median_hours / 24 * 10) / 10} d`)}${tile('Shipped within 7 days', a.fulfillment.within_7d == null ? '—' : pct(a.fulfillment.within_7d))}${tile('Tickets opened / closed', `${a.support.opened} / ${a.support.closed}`)}${tile('First response, median', a.support.median_first_response_h == null ? '—' : `${a.support.median_first_response_h} h`)}${tile('Warranty claims', `${a.warranty.claims} (${pct(a.warranty.rate)})`)}${tile('New customers', a.customers.new)}${tile('Waitlist joins', a.customers.waitlist_joins)}</div>
<div class="two"><div><h2>Discount codes</h2>${table(a.discounts, [{ label: 'Code', html: r => link(`/orders?discount=${encodeURIComponent(r.code)}`, r.code) }, { label: 'Uses', key: 'uses', num: true }, { label: 'Given', get: r => money(r.discount_cents), num: true }, { label: 'Sales', get: r => money(r.sales_cents), num: true }], { empty: 'No code used in this period.' })}</div>
<div><h2>By channel</h2>${table(a.bySource, [{ label: 'Source', key: 'source' }, { label: 'Orders', key: 'orders', num: true }, { label: 'Sales', get: r => money(r.sales_cents), num: true }], { empty: 'No sales in this period.' })}</div></div>
<h2>Every product</h2>${table(a.topProducts, [{ label: 'Product', html: r => `${link(`/products/${encodeURIComponent(r.sku)}`, r.name)} <span class="muted">${e(r.sku)}</span>` }, { label: 'Orders', key: 'orders', num: true }, { label: 'Units', key: 'units', num: true }, { label: 'Gross', get: r => money(r.gross_cents), num: true }, { label: 'Share', get: r => pct(r.share), num: true }], { empty: 'Nothing sold in this period.' })}`;
    page(c, 'Analytics', body);
  });
  router.get('/analytics.csv', (c) => text(c.res, analyticsCsv(db, { days: Number(c.query.get('days')) || 30 }), 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="analytics.csv"' }));

  // ---------- batches (the weekly ship day)
  router.get('/batches', (c) => {
    const rows = batches(db); const cur = weekLabel();
    page(c, 'Batches', `<p class="muted">Orders in by Sunday night ship the following week in one batch (the ISO week of payment). This week is <b>${e(cur)}</b>. ${link('/orders', 'All orders')}</p>` +
      table(rows, [{ label: 'Batch', html: r => `${link(`/orders?batch=${e(r.batch)}`, r.batch)}${r.batch === cur ? ' <span class="pill warn">this week</span>' : ''}` }, { label: 'Orders', key: 'orders', num: true }, { label: 'Units', key: 'units', num: true }, { label: 'To pack', html: r => `<span class="${r.to_pack ? 'warn' : 'muted'}">${r.to_pack}</span>`, num: true }, { label: 'To ship', html: r => `<span class="${r.to_ship ? 'warn' : 'muted'}">${r.to_ship}</span>`, num: true }, { label: 'Shipped', key: 'shipped', num: true }, { label: 'Dropped', get: r => r.dropped || '', num: true }, { label: 'Sales', get: r => money(r.sales_cents), num: true }, { label: 'First paid', get: r => day(r.first_paid_at) }, { label: 'Last shipped', get: r => day(r.last_shipped_at) },
        { label: '', html: r => `${link(`/orders.csv?batch=${e(r.batch)}&status=paid`, 'fulfill CSV')} · ${link(`/orders/packing-slips?batch=${e(r.batch)}`, 'packing slips')}${r.to_pack ? ` · <form method="post" action="/orders/bulk" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="batch" value="${e(r.batch)}"><input type="hidden" name="action" value="packed"><button class="quiet">pack all ${r.to_pack}</button></form>` : ''}` }], { empty: 'No orders yet.' }));
  });

  // ---------- packing slips (one page per order; print = PDF)
  const slip = (o) => { const devices = db.all('SELECT serial, fw_version FROM devices WHERE order_id = ?', o.id);
    return `<div class="slip"><div class="row"><div><b style="font-size:1.2rem">${e(cfg.company)}</b><br><span class="muted">${e(cfg.supportAddr || '')}</span></div><div style="text-align:right"><b>Packing slip</b><br>Order <b>${e(o.number)}</b><br>Paid ${day(o.paid_at)} · batch ${e(o.batch || '')}</div></div>
<h2>Ship to</h2><p>${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)} ${o.ship_country !== 'US' ? e(o.ship_country) : ''}<br><span class="muted">${e(o.email || '')}</span></p>
<table><thead><tr><th>Item</th><th>SKU</th><th class="num">Qty</th><th>Packed ☐</th></tr></thead><tbody>${o.items.map(i => `<tr><td>${e(i.name)}</td><td>${e(i.sku)}</td><td class="num">${i.qty}</td><td>☐</td></tr>`).join('')}</tbody></table>
${devices.length ? `<p><b>Boards in this box:</b> ${devices.map(d => `${e(d.serial)} (fw ${e(d.fw_version || '?')})`).join(', ')}</p>` : '<p class="muted">Scan each board serial into the order before sealing (the QA line).</p>'}
${o.notes ? `<div class="box"><b>Notes</b><br>${e(o.notes)}</div>` : ''}${o.tags.includes('gift') ? '<div class="box"><b>Gift</b> — no prices in the box.</div>' : ''}
<p class="muted">Prices are on the receipt Stripe emailed. Your Canary provisions itself at home: power it on and point your phone's camera at the QR card. Anything wrong, missing or broken: reply to any of our emails — we fix, we don't ask you to send things back (terms of sale §7).</p></div>`; };
  router.get('/orders/packing-slips', (c) => {
    const ids = (c.query.get('ids') || '').split(',').filter(Boolean).map(Number);
    const orders = ids.length ? ids.map(id => orderById(db, id)).filter(Boolean) : listOrders(db, { batch: c.query.get('batch') || weekLabel(), status: c.query.get('status') || '', limit: 500 }).filter(o => ['paid', 'packed', 'shipped', 'delivered'].includes(o.status)).map(o => orderById(db, o.id));
    page(c, `Packing slips (${orders.length})`, `<p class="noprint muted">Print this page: one slip per page. ${link('/batches', 'Batches')}</p>${orders.map(slip).join('') || '<p class="muted">Nothing to print.</p>'}`);
  });
  router.get('/orders/:id/packing-slip', (c) => { const o = orderById(db, c.params.id); if (!o) throw new HttpError(404, 'no such order'); page(c, `Packing slip ${o.number}`, `<p class="noprint muted">Print this page for the box. ${link(`/orders/${o.id}`, 'Back to the order')}</p>${slip(o)}`); });

  // ---------- bulk actions on the orders list
  router.post('/orders/bulk', (c) => {
    let ids = [].concat(c.body.ids || []).map(Number).filter(Boolean);
    if (!ids.length && c.body.batch) ids = listOrders(db, { batch: c.body.batch, status: c.body.action === 'packed' ? 'paid' : '', limit: 500 }).map(o => o.id);
    if (!ids.length) throw new HttpError(400, 'select at least one order');
    const action = c.body.action;
    if (action === 'slips') return redirect(c.res, `/orders/packing-slips?ids=${ids.join(',')}`);
    if (action === 'tag') { for (const id of ids) { const o = orderById(db, id); if (o) setTags(db, id, `${o.tags},${c.body.tag || ''}`); } return back(c, '/orders', `tagged ${ids.length}`); }
    if (!['packed', 'delivered', 'canceled'].includes(action)) throw new HttpError(400, 'unknown bulk action');
    const r = bulkTransition(db, ids, action, { actor: 'web', cfg });
    const ok = r.filter(x => !x.error).length, bad = r.filter(x => x.error);
    back(c, c.body.batch ? '/batches' : '/orders', `${ok} marked ${action}${bad.length ? `; ${bad.map(x => `${x.number || x.id}: ${x.error}`).join('; ')}` : ''}`);
  });

  // ---------- search
  router.get('/search', async (c) => {
    const q = c.query.get('q') || ''; const r = search(db, q);
    const catalog = q ? await app.catalog().catch(() => null) : null;
    const products = catalog ? [...catalog.products.values()].filter(p => p.sku.includes(q.toLowerCase()) || p.name.toLowerCase().includes(q.toLowerCase())).slice(0, 8) : [];
    const sec = (title, rows, cols, more) => rows.length ? `<h2>${e(title)} <span class="muted">${rows.length}${more ? ` · ${link(more, 'all')}` : ''}</span></h2>${table(rows, cols)}` : '';
    const g = Object.fromEntries(r.groups.map(x => [x.kind, x.rows]));
    const body = !q ? '<p class="muted">Type an order number, a name, an email, a tracking number, a board serial, an invoice or a ticket.</p>' : (r.total + products.length ? '' : `<p class="muted">Nothing matches <b>${e(q)}</b>.</p>`) +
      sec('Orders', g.orders || [], [{ label: '#', html: x => link(`/orders/${x.id}`, x.number) }, { label: 'Status', html: x => statusPill(x.status) }, { label: 'Customer', get: x => `${x.ship_name || ''} ${x.email ? `<${x.email}>` : ''}` }, { label: 'Total', get: x => money(x.total_cents), num: true }, { label: 'Tracking', key: 'tracking' }], `/orders?q=${encodeURIComponent(q)}`) +
      sec('Customers', g.customers || [], [{ label: 'Email', html: x => link(`/contacts/${x.id}`, x.email) }, { label: 'Name', key: 'name' }, { label: 'Org', key: 'org' }, { label: 'Tags', html: x => tagsHtml(x.tags) }], `/contacts?q=${encodeURIComponent(q)}`) +
      sec('Products', products, [{ label: 'SKU', html: x => link(`/products/${encodeURIComponent(x.sku)}`, x.sku) }, { label: 'Name', key: 'name' }, { label: 'Price', get: x => money(x.price_cents), num: true }]) +
      sec('Tickets', g.tickets || [], [{ label: '#', html: x => link(`/tickets/${x.id}`, x.number) }, { label: 'Status', html: x => statusPill(x.status) }, { label: 'From', key: 'email' }, { label: 'Subject', key: 'subject' }], `/tickets?q=${encodeURIComponent(q)}`) +
      sec('Devices', g.devices || [], [{ label: 'Serial', html: x => link(`/devices?q=${encodeURIComponent(x.serial)}`, x.serial) }, { label: 'MAC', key: 'mac' }, { label: 'Status', key: 'status' }, { label: 'Order', html: x => x.order_id ? link(`/orders/${x.order_id}`, x.order_number) : '—' }]) +
      sec('Invoices', g.invoices || [], [{ label: '#', html: x => link(`/invoices/${x.id}`, x.number) }, { label: 'Status', html: x => statusPill(x.status) }, { label: 'To', key: 'email' }, { label: 'PO', key: 'po_number' }, { label: 'Total', get: x => money(x.total_cents), num: true }]) +
      sec('Warranty claims', g.claims || [], [{ label: '#', html: x => link(`/warranty/${x.id}`, x.number) }, { label: 'Status', html: x => statusPill(x.status) }, { label: 'Category', key: 'category' }, { label: 'Serial', key: 'serial' }, { label: 'Order', key: 'order_number' }]) +
      sec('Discount codes', g.discounts || [], [{ label: 'Code', html: x => link('/discounts', x.code) }, { label: 'Kind', key: 'kind' }, { label: 'Status', html: x => statusPill(x.status) }]);
    page(c, q ? `Search: ${q}` : 'Search', body);
  });
}
