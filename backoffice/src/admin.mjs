// The admin: one page per module, plain forms, no client JavaScript.
import { html, redirect, text, HttpError, escapeHtml as e } from './http.mjs';
import { layout, table, money, dt, day, statusPill, field, form, button, cards, tags as tagsHtml, tabs } from './render.mjs';
import { snapshot } from './modules/dashboard.mjs';
import { listOrders, orderById, transition, refund, recordOrder, fulfillCsv, weekLabel, STATUSES, timeline, addNote, setTags, updateShipping, resendTracking, statusCounts } from './modules/orders.mjs';
import { customerStats, tagList } from './modules/contacts.mjs';
import { registerShop } from './admin_shop.mjs';
import { registerOps } from './admin_ops.mjs';
import { registerMail } from './admin_mail.mjs';
import { doctor, todayLines } from './modules/health.mjs';
import { line, tile } from './charts.mjs';
import { analytics } from './modules/analytics.mjs';
import { parseCsv, detectColumns, csvRowToOrder } from './modules/stripe.mjs';
import { listTickets, ticketById, reply, setStatus, update as updateTicket, openTicket, STATUSES as TSTATUS, KINDS } from './modules/support.mjs';
import { contactById, upsertContact, setContactTags as setContactTagsFn } from './modules/contacts.mjs';
import { sendConsentRequest, notifyWaitlist } from './modules/campaigns.mjs';
import { listItems, upsertItem, move, setRecipe, recipes, moves, buildable } from './modules/inventory.mjs';
import { listDevices, registerDevice, importManifest, assignToOrder } from './modules/devices.mjs';
import { listClaims, claimById, openClaim, reship, resolveClaim, ANSWERS } from './modules/warranty.mjs';
import { listInvoices, invoiceById, createInvoice, issueInvoice, markPaid, voidInvoice, invoiceUrl } from './modules/invoices.mjs';
import { monthly, trialBalance, salesTaxByState, postExpense, postPayout, ledgerCsv, ACCOUNTS, EXPENSE_ACCOUNTS } from './modules/ledger.mjs';
import { listSubscriptions } from './modules/subscriptions.mjs';
import { listProbes, upsertProbe, deleteProbe, runDueProbes, uptime, listIncidents, openIncident, updateIncident } from './modules/status.mjs';
import { listExceptions, resolve as resolveException } from './modules/exceptions.mjs';
import { createApiToken, revokeApiToken } from './auth.mjs';
import { backup, listBackups } from './modules/backup.mjs';
import { outboxStats, fill } from './modules/mail.mjs';
import { jobRuns, runJob, JOBS } from './jobs.mjs';
import { SCENARIOS, resetDemo, PEOPLE } from './modules/demo.mjs';
import { join } from 'node:path';

export function registerAdmin(app) {
  const { db, cfg, router } = app;
  const page = (c, title, body) => html(c.res, layout({ title, body, path: c.url.pathname, company: cfg.company, csrf: c.csrf, demo: cfg.demo, demoResetMinutes: cfg.demoResetMinutes, q: c.url.pathname === '/search' ? c.query.get('q') || '' : '', flash: c.query.get('ok') ? { text: c.query.get('ok') } : c.query.get('err') ? { kind: 'err', text: c.query.get('err') } : null }));
  const back = (c, to, ok) => redirect(c.res, `${to}?ok=${encodeURIComponent(ok)}`);
  const link = (href, label) => `<a href="${e(href)}">${e(label)}</a>`;
  const filters = (c, fields) => `<form method="get" class="row" style="margin:.6rem 0">${fields.map(([n, l, opts]) => opts ? field(n, l, { options: [['', 'any'], ...opts], value: c.query.get(n) || '' }) : field(n, l, { value: c.query.get(n) || '' })).join('')}<div><label>&nbsp;</label><button class="quiet">Filter</button></div></form>`;

  registerShop(app, { page, back, link, filters });
  registerOps(app, { page, back, link, filters });
  // A form may say where to go afterwards; only a plain local path is honored.
  const backTo = (c, fallback, ok) => back(c, /^\/(?!\/)[A-Za-z0-9/._~?=&-]*$/.test(String(c.body._back || '')) ? c.body._back : fallback, ok);

  // ---------- home: today's work as cards, the thirty days at a glance, the health pill, what just happened
  router.get('/', (c) => {
    const s = snapshot(db);
    const m = s.books.months.at(-1) || { income_cents: 0, expenses_cents: 0, net_cents: 0 };
    const a = analytics(db, { days: 30 }); const t = a.totals, pv = a.previous;
    const h = doctor(db, cfg); const todo = todayLines(db, cfg);
    const date = new Date().toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', timeZone: 'UTC' });
    const lead = todo.length ? todo.slice(0, 3).map(l => `${l.n} ${l.what.replace(/:.*$/, '')}`).join(' · ') : 'Nothing is waiting on you.';
    const hero = `<div class="hero"><div><h1>${e(date)}</h1><p class="lead">${e(lead)}</p></div><a class="hp ${h.ok ? '' : h.failing.some(k => ['integrity', 'books', 'backup'].includes(k)) ? 'bad' : 'warn'}" href="/health">${h.ok ? 'All checks pass' : `${h.failing.length} health check${h.failing.length === 1 ? '' : 's'} need attention`}</a></div>`;
    const acts = todo.length ? `<div class="actions">${todo.map(l => `<a class="act ${l.urgent ? 'urgent' : ''}" href="${e(l.href)}"><span class="n">${l.n}</span><span class="w">${e(l.what)}</span><span class="go">${e(l.action)} →</span></a>`).join('')}</div>` : '<div class="actions"><div class="act calm"><span class="w">Silence means healthy. The next order, email or probe result will show up here.</span></div></div>';
    const kpis = `<div class="kpis">${tile('Total sales, 30 days', money(t.total_cents), { prev: pv.total_cents, cur: t.total_cents })}${tile('Orders', t.orders, { prev: pv.orders, cur: t.orders })}${tile('Average order', money(t.aov_cents), { prev: pv.aov_cents, cur: t.aov_cents })}${tile('Returning customers', `${Math.round(t.returning_rate * 100)}%`, { prev: pv.returning_rate, cur: t.returning_rate })}${tile('This month net', money(m.net_cents))}</div>`;
    const chart = line({ points: a.series.buckets.map(b => ({ label: b.label, value: b.net_cents })), title: 'Net sales per day, 30 days', money: true, height: 150 });
    const more = `<details class="more"><summary>All the numbers</summary>` + cards([
      ['This batch', `${s.orders.thisBatch} <span class="muted" style="font-size:.8rem">${weekLabel()}</span>`], ['Units, 30 days', s.orders.units30],
      ['SLA breaches', s.support.breached, s.support.breached ? 'bad' : 'ok'], ['Warranty rate, 90 d', `${(s.warranty.rate90 * 100).toFixed(1)}%`],
      ['Inventory at cost', money(s.inventory.value_cents)], ['MRR (plans)', money(s.books.mrr_cents)],
      ['Receivable', money(s.books.receivable_cents)], ['In Stripe, not paid out', money(s.books.stripe_unpaid_out_cents)],
      ['Probes down', s.probes.filter(p => p.last_ok === 0).length, s.probes.some(p => p.last_ok === 0) ? 'bad' : 'ok'], ['Mail queued', s.mail.queued || 0],
      ['Customers', `${s.contacts.n} <span class="muted" style="font-size:.8rem">${s.contacts.consented || 0} opted in</span>`], ['Waiting for stock', s.waitlist.reduce((n, w) => n + w.waiting, 0)],
    ]) + `<p class="muted">${link('/analytics', 'Full analytics')} · ${link('/batches', 'Batches')} · ${link('/products', 'Products')} · ${link('/books', 'Books')}</p></details>`;
    const activity = db.all('SELECT * FROM audit_log WHERE action NOT IN (\'demo.seed\') ORDER BY at DESC, id DESC LIMIT 10').map(describeAudit).filter(Boolean);
    const body = `${hero}${acts}${kpis}${chart}${more}<h2>Activity <span class="muted" style="font-weight:400;font-size:13px">· ${link('/audit', 'the whole log')}</span></h2><ul class="feed">${activity.map(x => `<li><span class="meta">${e(dt(x.at))} · ${e(x.actor)}</span><span>${x.html}</span></li>`).join('') || '<li class="muted">Nothing yet.</li>'}</ul>`;
    html(c.res, layout({ title: 'Home', body, path: '/', company: cfg.company, csrf: c.csrf, demo: cfg.demo, demoResetMinutes: cfg.demoResetMinutes, h1: false, flash: c.query.get('ok') ? { text: c.query.get('ok') } : c.query.get('err') ? { kind: 'err', text: c.query.get('err') } : null }));
  });

  // ---------- orders
  router.get('/orders', (c) => {
    const f = { status: c.query.get('status') || '', batch: c.query.get('batch') || '', q: c.query.get('q') || '', tag: c.query.get('tag') || '', from: c.query.get('from') || '', to: c.query.get('to') || '', discount: c.query.get('discount') || '', sku: c.query.get('sku') || '' };
    const rows = listOrders(db, f); const n = statusCounts(db);
    const qs = (o) => '/orders?' + new URLSearchParams({ ...f, ...o }).toString();
    const body = tabs([[qs({ status: '' }), 'All', n.all, ''], [qs({ status: 'open' }), 'Open', n.open, 'open'], [qs({ status: 'paid' }), 'To pack', n.paid, 'paid'], [qs({ status: 'packed' }), 'To ship', n.packed, 'packed'], [qs({ status: 'shipped' }), 'Shipped', n.shipped, 'shipped'], [qs({ status: 'delivered' }), 'Delivered', n.delivered, 'delivered'], [qs({ status: 'refunded' }), 'Refunded', n.refunded, 'refunded'], [qs({ status: 'canceled' }), 'Canceled', n.canceled, 'canceled']], f.status) +
      filters(c, [['q', 'Search # / name / email / tracking'], ['batch', 'Batch (e.g. ' + weekLabel() + ')'], ['tag', 'Tag'], ['from', 'Paid from (YYYY-MM-DD)'], ['to', 'Paid to'], ['discount', 'Discount code'], ['sku', 'SKU']]) +
      `<p>${rows.length} shown · ${link(`/orders.csv?batch=${e(f.batch)}&status=${e(f.status || 'paid')}`, 'fulfill.mjs CSV for this filter')} · ${link('/batches', 'Batches')} · ${link('/orders/import', 'Import a Stripe CSV')} · ${link('/orders/new', 'Manual order')}</p>` +
      `<form method="post" action="/orders/bulk"><input type="hidden" name="_csrf" value="${e(c.csrf)}">` +
      table(rows, [{ label: '', html: r => `<input type="checkbox" name="ids[]" value="${r.id}" style="width:auto" aria-label="select ${e(r.number)}">` }, { label: '#', html: r => link(`/orders/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Paid', get: r => day(r.paid_at) }, { label: 'Batch', key: 'batch' }, { label: 'Customer', html: r => `${r.contact_id ? link(`/contacts/${r.contact_id}`, r.ship_name || r.email) : e(r.ship_name || '')}${r.email ? `<br><span class="muted">${e(r.email)}</span>` : ''}` }, { label: 'Items', get: r => `${r.units} · ${r.items_text || ''}` }, { label: 'Total', html: r => `${money(r.total_cents, r.currency)}${r.discount_code ? `<br><span class="muted">${e(r.discount_code)}</span>` : ''}${r.refunded_cents ? `<br><span class="bad">−${money(r.refunded_cents)}</span>` : ''}`, num: true }, { label: 'Tags', html: r => tagsHtml(r.tags) }, { label: 'Tracking', key: 'tracking' }]) +
      (rows.length ? `<div class="row" style="margin:.6rem 0;align-items:end"><div>${field('action', 'With the selected orders', { options: [['packed', 'mark packed (pulls parts from stock)'], ['delivered', 'mark delivered'], ['slips', 'print packing slips'], ['tag', 'add a tag'], ['canceled', 'cancel (only paid, unpacked orders)']] })}</div><div>${field('tag', 'Tag (for "add a tag")')}</div><div><label>&nbsp;</label><button>Apply</button></div></div>` : '') + '</form>';
    page(c, 'Orders', body);
  });
  router.get('/orders.csv', (c) => text(c.res, fulfillCsv(db, { batch: c.query.get('batch') || '', status: c.query.get('status') || 'paid' }), 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="orders.csv"' }));
  router.get('/orders/import', (c) => page(c, 'Import a Stripe export', form('/orders/import', c.csrf, `<p class="muted">Paste the Payments → Export CSV (paid rows only are imported; rows already known by payment id are skipped). SKUs must resolve against store.json.</p>${field('csv', 'CSV', { type: 'textarea', rows: 12, required: true })}`, { submit: 'Import' })));
  router.post('/orders/import', async (c) => {
    const rows = parseCsv(c.body.csv || ''); if (!rows.length) throw new HttpError(400, 'no rows');
    const map = detectColumns(rows[0]); const catalog = await app.catalog().catch(() => null);
    const res = { created: 0, existing: 0, skipped: 0, errors: [] };
    for (const r of rows) { const o = csvRowToOrder(r, map); if (o.status_hint && !/paid|succeeded|complete/i.test(o.status_hint)) { res.skipped++; continue; } try { const { created } = recordOrder(db, o, { catalog, actor: 'web' }); created ? res.created++ : res.existing++; } catch (err) { res.errors.push(`${o.external_id || o.email}: ${err.message}`); } }
    back(c, '/orders', `created ${res.created}, already known ${res.existing}, skipped ${res.skipped}${res.errors.length ? `, errors: ${res.errors.join('; ')}` : ''}`);
  });
  router.get('/orders/new', (c) => page(c, 'Manual order', form('/orders/new', c.csrf, `<div class="row">${field('email', 'Email', { type: 'email', required: true })}${field('name', 'Ship to name', { required: true })}</div><div class="row">${field('sku', 'SKU', { required: true })}${field('qty', 'Qty', { type: 'number', value: 1 })}${field('cents', 'Total cents (0 for a free reship)', { type: 'number', value: 0 })}</div><div class="row">${field('line1', 'Address line 1', { required: true })}${field('line2', 'Line 2')}</div><div class="row">${field('city', 'City', { required: true })}${field('state', 'State', { required: true })}${field('zip', 'ZIP', { required: true })}</div><div class="row">${field('discount_code', 'Discount code (optional; priced by its rules)')}${field('tags', 'Tags')}</div>${field('notes', 'Notes')}`, { submit: 'Create order' })));
  router.post('/orders/new', async (c) => {
    const b = c.body; const cents = Number(b.cents || 0); const qty = Number(b.qty || 1);
    const { order } = recordOrder(db, { source: 'manual', email: b.email, name: b.name, subtotal_cents: cents, total_cents: cents, discount_code: b.discount_code || null, tags: b.tags || '', ship: { line1: b.line1, line2: b.line2, city: b.city, state: b.state, zip: b.zip }, items: [{ sku: b.sku, name: b.sku, qty, unit_cents: Math.round(cents / qty) }], notes: b.notes }, { catalog: await app.catalog().catch(() => null), actor: 'web' });
    back(c, `/orders/${order.id}`, `created ${order.number}`);
  });
  router.get('/orders/:id', (c) => {
    const o = orderById(db, c.params.id); if (!o) throw new HttpError(404, 'no such order');
    const devices = db.all('SELECT * FROM devices WHERE order_id = ?', o.id);
    const cust = o.contact_id ? customerStats(db, o.contact_id) : null;
    const events = timeline(db, o.id);
    const claims = db.all('SELECT * FROM warranty_claims WHERE order_id = ? OR reship_order_id = ?', o.id, o.id);
    const tickets = o.contact_id ? db.all('SELECT id, number, subject, status FROM tickets WHERE contact_id = ? ORDER BY id DESC LIMIT 5', o.contact_id) : [];
    const next = { paid: 'packed', packed: 'shipped', shipped: 'delivered' }[o.status];
    const act = next === 'shipped' ? form(`/orders/${o.id}/transition`, c.csrf, `<input type="hidden" name="to" value="shipped"><div class="row">${field('carrier', 'Carrier', { value: 'USPS' })}${field('tracking', 'Tracking number')}</div>`, { submit: 'Mark shipped + email tracking' })
      : next ? `<form method="post" action="/orders/${o.id}/transition" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="to" value="${next}"><button>Mark ${next}</button></form>` : '';
    const undo = o.status === 'packed' ? `<form method="post" action="/orders/${o.id}/transition" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="to" value="paid"><button class="quiet">Unpack (return parts)</button></form>` : '';
    const cancel = o.status === 'paid' ? `<form method="post" action="/orders/${o.id}/transition" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="to" value="canceled"><button class="quiet">Cancel</button></form>` : '';
    const resend = ['shipped', 'delivered'].includes(o.status) && o.email ? button(`/orders/${o.id}/resend-tracking`, c.csrf, 'Re-send tracking email', { quiet: true }) : '';
    const stepper = ['canceled', 'refunded'].includes(o.status) ? `<p>${statusPill(o.status)} <span class="muted">— this order is ${o.status}${o.refunded_cents ? ` (${money(o.refunded_cents)} back to the customer)` : ''}</span></p>` : (() => { const order = ['paid', 'packed', 'shipped', 'delivered']; const at = { paid: o.paid_at, packed: o.packed_at, shipped: o.shipped_at, delivered: o.delivered_at }; const i = order.indexOf(o.status); return `<ol class="steps">${order.map((st, j) => `<li class="${j < i ? 'done' : j === i ? 'now' : ''}">${st[0].toUpperCase() + st.slice(1)}${at[st] && j <= i ? `<span class="when">${e(day(at[st]))}</span>` : ''}</li>`).join('')}</ol>`; })();
    const body = `${stepper}<p class="muted">${e(o.source)} ${o.external_id ? `· ${e(o.external_id)}` : ''} · batch ${link(`/orders?batch=${e(o.batch || '')}`, o.batch || '')} · paid ${dt(o.paid_at)}${o.tracking ? ` · ${e(o.carrier)} <b>${e(o.tracking)}</b>` : ''} · ${link(`/orders/${o.id}/packing-slip`, 'Packing slip')}</p>
<p>${act} ${undo} ${cancel} ${resend}</p>
<div class="two"><div>
<div class="card"><div class="k">Customer</div>${o.contact_id ? `<b>${link(`/contacts/${o.contact_id}`, o.ship_name || o.email)}</b><br><span class="muted">${e(o.email || '')}</span><br>${cust.orders} order${cust.orders === 1 ? '' : 's'} · ${money(cust.spent_cents)} lifetime${cust.orders > 1 ? ' · <span class="pill ok">repeat</span>' : ' · first order'}${cust.open_tickets ? ` · <span class="warn">${cust.open_tickets} open ticket${cust.open_tickets === 1 ? '' : 's'}</span>` : ''}` : '<span class="muted">no customer record (no email on the order)</span>'}</div>
<div class="card" style="margin-top:.8rem"><div class="k">Ship to</div>${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)} ${e(o.ship_country)}${['paid', 'packed'].includes(o.status) ? `<details style="margin-top:.5rem"><summary class="muted">Edit address</summary>${form(`/orders/${o.id}/address`, c.csrf, `<div class="row">${field('name', 'Name', { value: o.ship_name })}${field('line1', 'Line 1', { value: o.ship_line1, required: true })}</div><div class="row">${field('line2', 'Line 2', { value: o.ship_line2 })}${field('city', 'City', { value: o.ship_city, required: true })}${field('state', 'State', { value: o.ship_state })}${field('zip', 'ZIP', { value: o.ship_zip, required: true })}</div>`, { submit: 'Save address' })}</details>` : ''}</div>
<div class="card" style="margin-top:.8rem"><div class="k">Payment</div><table><tr><td>Subtotal</td><td class="num">${money(o.subtotal_cents)}</td></tr>${o.discount_cents || o.discount_code ? `<tr><td>Discount ${o.discount_code ? link(`/orders?discount=${encodeURIComponent(o.discount_code)}`, o.discount_code) : ''}</td><td class="num">−${money(o.discount_cents)}</td></tr>` : ''}<tr><td>Shipping</td><td class="num">${money(o.shipping_cents)}</td></tr><tr><td>Tax</td><td class="num">${money(o.tax_cents)}</td></tr><tr><td><b>Total charged</b></td><td class="num"><b>${money(o.total_cents)}</b></td></tr>${o.refunded_cents ? `<tr><td class="bad">Refunded</td><td class="num bad">−${money(o.refunded_cents)}</td></tr>` : ''}<tr><td class="muted">Cost of goods</td><td class="num muted">${money(o.items.reduce((s, i) => s + i.cogs_cents * i.qty, 0))}</td></tr><tr><td class="muted">Margin</td><td class="num muted">${o.total_cents ? Math.round(100 * (o.subtotal_cents - o.discount_cents - o.refunded_cents - o.items.reduce((s, i) => s + i.cogs_cents * i.qty, 0)) / Math.max(1, o.subtotal_cents - o.discount_cents)) : 0}%</td></tr></table></div>
</div><div>
<h2 style="margin-top:0">Timeline</h2><ul class="tl">${events.map(ev => `<li class="${e(ev.kind)}"><span class="meta">${e(dt(ev.at))} · ${e(ev.actor)}</span><br>${e(ev.text)}</li>`).join('') || '<li class="muted">No events recorded.</li>'}</ul>
${form(`/orders/${o.id}/note`, c.csrf, field('body', 'Add a note (internal; the customer never sees it)', { type: 'textarea', rows: 2, required: true }), { submit: 'Add note' })}
</div></div>
<h2>Items</h2>${table(o.items, [{ label: 'SKU', html: r => link(`/products/${encodeURIComponent(r.sku)}`, r.sku) }, { label: 'Item', key: 'name' }, { label: 'Qty', key: 'qty', num: true }, { label: 'Unit', get: r => money(r.unit_cents), num: true }, { label: 'Line', get: r => money(r.unit_cents * r.qty), num: true }, { label: 'COGS/unit', get: r => money(r.cogs_cents), num: true }], { empty: 'No line items — Stripe did not send them and they could not be fetched. Add them before packing.' })}
<div class="two"><div><h2>Tags</h2><p>${tagsHtml(o.tags) || '<span class="muted">none</span>'}</p>${form(`/orders/${o.id}/tags`, c.csrf, field('tags', 'Tags, comma-separated (gift, rush, wholesale-interest…)', { value: o.tags }), { submit: 'Save tags' })}</div>
<div><h2>Devices in this box</h2>${table(devices, [{ label: 'Serial', key: 'serial' }, { label: 'MAC', key: 'mac' }, { label: 'Firmware', key: 'fw_version' }, { label: 'Status', key: 'status' }], { empty: 'No serial scanned yet — the packing-slip QA line.' })}
${form(`/orders/${o.id}/assign`, c.csrf, field('serial', 'Scan a board serial to tie it to this order'), { submit: 'Assign' })}</div></div>
${claims.length || tickets.length ? `<div class="two"><div><h2>Warranty</h2>${table(claims, [{ label: '#', html: r => link(`/warranty/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Category', key: 'category' }])}</div><div><h2>Customer's tickets</h2>${table(tickets, [{ label: '#', html: r => link(`/tickets/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Subject', key: 'subject' }])}</div></div>` : ''}
<h2>Refund</h2>${o.total_cents - o.refunded_cents > 0 ? form(`/orders/${o.id}/refund`, c.csrf, `<div class="row">${field('cents', 'Cents', { type: 'number', value: o.total_cents - o.refunded_cents })}${field('reason', 'Reason')}</div><p class="muted">Records the refund in the books and on the timeline. Issue the actual refund in Stripe (money stays a human action); the webhook then confirms it.</p>`, { submit: 'Record refund' }) : '<p class="muted">Fully refunded.</p>'}
${o.notes ? `<p class="muted">Order notes: ${e(o.notes)}</p>` : ''}`;
    page(c, `Order ${o.number}`, body);
  });
  router.post('/orders/:id/note', (c) => { addNote(db, Number(c.params.id), c.body.body, { actor: 'web' }); back(c, `/orders/${c.params.id}`, 'note added'); });
  router.post('/orders/:id/tags', (c) => { setTags(db, Number(c.params.id), c.body.tags || '', { actor: 'web' }); back(c, `/orders/${c.params.id}`, 'tags saved'); });
  router.post('/orders/:id/address', (c) => { updateShipping(db, Number(c.params.id), c.body, { actor: 'web' }); back(c, `/orders/${c.params.id}`, 'address updated'); });
  router.post('/orders/:id/resend-tracking', (c) => { resendTracking(db, Number(c.params.id), { cfg, actor: 'web' }); back(c, `/orders/${c.params.id}`, 'tracking email queued'); });
  router.post('/orders/:id/transition', (c) => { const o = transition(db, c.params.id, c.body.to, { actor: 'web', carrier: c.body.carrier, tracking: c.body.tracking, cfg }); backTo(c, `/orders/${o.id}`, `${o.number} is now ${o.status}`); });
  router.post('/orders/:id/refund', (c) => { const o = refund(db, c.params.id, c.body.cents, { actor: 'web', reason: c.body.reason }); back(c, `/orders/${o.id}`, `refund recorded`); });
  router.post('/orders/:id/assign', (c) => { assignToOrder(db, c.body.serial, Number(c.params.id)); backTo(c, `/orders/${c.params.id}`, `assigned ${c.body.serial}`); });

  // ---------- support
  router.get('/tickets', (c) => {
    const rows = listTickets(db, { status: c.query.get('status') || '', kind: c.query.get('kind') || '', q: c.query.get('q') || '' });
    page(c, 'Support', filters(c, [['status', 'Status', TSTATUS.map(s => [s, s])], ['kind', 'Kind', KINDS.map(k => [k, k])], ['q', 'Search']]) + `<p>${link('/tickets/new', 'Open a ticket by hand')} · ${link('/canned', 'Canned replies')}</p>` +
      table(rows, [{ label: '#', html: r => link(`/tickets/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Pri', key: 'priority' }, { label: 'Kind', key: 'kind' }, { label: 'From', key: 'email' }, { label: 'Subject', key: 'subject' }, { label: 'Updated', get: r => dt(r.updated_at) }, { label: 'SLA due', html: r => `<span class="${!r.first_response_at && r.sla_due_at < new Date().toISOString() && r.status !== 'closed' ? 'bad' : 'muted'}">${e(dt(r.sla_due_at))}</span>` }]));
  });
  router.get('/tickets/new', (c) => page(c, 'New ticket', form('/tickets/new', c.csrf, `<div class="row">${field('email', 'Email', { type: 'email' })}${field('name', 'Name')}</div>${field('subject', 'Subject', { required: true })}${field('body', 'Message', { type: 'textarea', required: true })}${field('kind', 'Kind', { options: KINDS })}`, { submit: 'Open' })));
  router.post('/tickets/new', (c) => { const t = openTicket(db, { email: c.body.email, name: c.body.name, subject: c.body.subject, body: c.body.body, kind: c.body.kind, channel: 'manual', cfg, autoAck: false }); back(c, `/tickets/${t.id}`, `opened ${t.number}`); });
  router.get('/tickets/:id', (c) => {
    const t = ticketById(db, c.params.id); if (!t) throw new HttpError(404, 'no such ticket');
    const canned = db.all('SELECT * FROM canned_replies ORDER BY title');
    const orders = t.email ? db.all('SELECT o.* FROM orders o JOIN contacts c ON c.id = o.contact_id WHERE c.email = ? ORDER BY o.id DESC LIMIT 5', t.email) : [];
    const msgs = t.messages.map(m => `<div class="msg ${m.direction}"><div class="meta">${m.direction === 'in' ? '← ' + e(m.from_addr || t.email || '') : m.direction === 'out' ? '→ ' + e(m.to_addr || '') : 'note'} · ${e(dt(m.created_at))}</div>${e(m.body)}</div>`).join('');
    const body = `<p>${statusPill(t.status)} ${statusPill(t.priority)} ${statusPill(t.kind)} · ${t.email ? link(`/contacts?q=${encodeURIComponent(t.email)}`, t.email) : 'no email'} · opened ${dt(t.created_at)} · first response ${dt(t.first_response_at)} · thread token <code>[${e(t.number)} ${e(t.thread_token)}]</code></p>
${orders.length ? `<p>Orders: ${orders.map(o => link(`/orders/${o.id}`, `${o.number} (${o.status})`)).join(', ')}</p>` : ''}
${msgs}
${form(`/tickets/${t.id}/reply`, c.csrf, `${canned.length ? field('canned', 'Start from a canned reply', { options: [['', '—'], ...canned.map(r => [r.slug, r.title])] }) : ''}${field('body', 'Reply (plain text; goes out from the support address — leave empty to send the canned reply as is)', { type: 'textarea' })}<label><input type="checkbox" name="close" value="1" style="width:auto"> close after sending</label>`, { submit: 'Send reply' })}
<p>${TSTATUS.map(s => `<form method="post" action="/tickets/${t.id}/status" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="status" value="${s}"><button class="quiet">${s}</button></form>`).join(' ')}</p>
${form(`/tickets/${t.id}/update`, c.csrf, `<div class="row">${field('kind', 'Kind', { options: KINDS, value: t.kind })}${field('priority', 'Priority', { options: ['low', 'normal', 'high', 'urgent'], value: t.priority })}${field('labels', 'Labels', { value: t.labels })}</div>`, { submit: 'Update' })}
${form(`/tickets/${t.id}/note`, c.csrf, field('body', 'Internal note', { type: 'textarea', rows: 3 }), { submit: 'Add note' })}`;
    page(c, `${t.number} · ${t.subject}`, body);
  });
  router.post('/tickets/:id/reply', (c) => {
    let body = c.body.body || '';
    if (c.body.canned && !body.trim()) { const t = ticketById(db, c.params.id); body = fill(db.get('SELECT body FROM canned_replies WHERE slug = ?', c.body.canned)?.body || '', { name: t?.contact_name || 'there', number: t?.number || '' }); }
    if (!body.trim()) throw new HttpError(400, 'the reply is empty — write one or pick a canned reply');
    reply(db, c.params.id, body, { cfg, actor: 'web', close: !!c.body.close }); back(c, `/tickets/${c.params.id}`, 'reply queued');
  });
  router.post('/tickets/:id/status', (c) => { setStatus(db, c.params.id, c.body.status); back(c, `/tickets/${c.params.id}`, `now ${c.body.status}`); });
  router.post('/tickets/:id/update', (c) => { updateTicket(db, c.params.id, c.body); back(c, `/tickets/${c.params.id}`, 'updated'); });
  router.post('/tickets/:id/note', async (c) => { const { addMessage } = await import('./modules/support.mjs'); addMessage(db, Number(c.params.id), { direction: 'note', body: c.body.body || '' }); back(c, `/tickets/${c.params.id}`, 'note added'); });
  router.get('/canned', (c) => page(c, 'Canned replies', table(db.all('SELECT * FROM canned_replies ORDER BY title'), [{ label: 'Slug', key: 'slug' }, { label: 'Title', key: 'title' }, { label: 'Body', key: 'body' }]) + form('/canned', c.csrf, `<div class="row">${field('slug', 'Slug', { required: true })}${field('title', 'Title', { required: true })}</div>${field('body', 'Body ({{name}} works)', { type: 'textarea', required: true })}`, { submit: 'Save' })));
  router.post('/canned', (c) => { db.run('INSERT INTO canned_replies (slug, title, body) VALUES (?, ?, ?) ON CONFLICT(slug) DO UPDATE SET title = excluded.title, body = excluded.body', c.body.slug, c.body.title, c.body.body); back(c, '/canned', 'saved'); });

  // ---------- contacts & waitlist
  router.post('/contacts/new', (c) => { const ct = upsertContact(db, { ...c.body, source: 'manual' }); if (c.body.tags) setContactTagsFn(db, ct.id, c.body.tags); back(c, `/contacts/${ct.id}`, 'saved'); });
  router.get('/contacts/:id', (c) => {
    const ct = contactById(db, c.params.id); if (!ct) throw new HttpError(404, 'no such contact');
    const st = customerStats(db, ct.id);
    const orders = db.all('SELECT o.*, (SELECT GROUP_CONCAT(qty || \'× \' || sku, \', \') FROM order_items i WHERE i.order_id = o.id) AS items_text FROM orders o WHERE contact_id = ? ORDER BY id DESC', ct.id); const tickets = db.all('SELECT * FROM tickets WHERE contact_id = ? ORDER BY id DESC', ct.id);
    const wl = db.all('SELECT * FROM waitlist WHERE contact_id = ?', ct.id);
    const invoices = db.all('SELECT * FROM invoices WHERE contact_id = ? ORDER BY id DESC', ct.id);
    const events = [
      ...db.all('SELECT ev.at, ev.text, o.number, o.id AS order_id FROM order_events ev JOIN orders o ON o.id = ev.order_id WHERE o.contact_id = ? AND ev.kind IN (\'placed\',\'shipped\',\'delivered\',\'refund\',\'canceled\')', ct.id).map(x => ({ at: x.at, html: `${link(`/orders/${x.order_id}`, x.number)} — ${e(x.text)}` })),
      ...db.all('SELECT m.created_at AS at, m.direction, t.number, t.id AS ticket_id, t.subject FROM ticket_messages m JOIN tickets t ON t.id = m.ticket_id WHERE t.contact_id = ?', ct.id).map(x => ({ at: x.at, html: `${link(`/tickets/${x.ticket_id}`, x.number)} — ${x.direction === 'in' ? 'wrote in' : x.direction === 'out' ? 'we replied' : 'note'}: ${e(x.subject)}` })),
      ...(ct.consent_at ? [{ at: ct.consent_at, html: 'Confirmed marketing opt-in (double opt-in)' }] : []), ...(ct.unsubscribed_at ? [{ at: ct.unsubscribed_at, html: 'Unsubscribed from marketing' }] : []),
      ...wl.map(w => ({ at: w.created_at, html: `Joined the waitlist for ${e(w.sku)} (${w.qty})` })), { at: ct.created_at, html: `Customer record created via ${e(ct.source || 'manual')}` },
    ].sort((a, b) => b.at.localeCompare(a.at));
    page(c, ct.name || ct.email, `<p>${e(ct.email)} ${ct.org ? '· ' + e(ct.org) : ''} · ${e(ct.kind)} · via ${e(ct.source || '')} · since ${day(ct.created_at)} · marketing: ${ct.consent_marketing && !ct.unsubscribed_at ? `<span class="ok">opted in ${day(ct.consent_at)}</span>` : ct.unsubscribed_at ? `<span class="bad">unsubscribed ${day(ct.unsubscribed_at)}</span>` : 'no'} ${!ct.consent_marketing ? button(`/contacts/${ct.id}/consent`, c.csrf, 'Send double opt-in mail', { quiet: true }) : ''}</p>
<p>${tagsHtml(ct.tags) || '<span class="muted">no tags</span>'}</p>
${cards([['Orders', st.orders], ['Lifetime spend', money(st.spent_cents)], ['Average order', money(st.aov_cents)], ['Units', st.units], ['First order', st.first_order_at ? day(st.first_order_at) : '—'], ['Last order', st.last_order_at ? day(st.last_order_at) : '—'], ['Tickets', `${st.tickets}${st.open_tickets ? ` (${st.open_tickets} open)` : ''}`, st.open_tickets ? 'warn' : ''], ['Warranty claims', st.claims, st.claims ? 'warn' : '']])}
<div class="two"><div>
<h2>Orders</h2>${table(orders, [{ label: '#', html: r => link(`/orders/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Items', key: 'items_text' }, { label: 'Total', get: r => money(r.total_cents), num: true }, { label: 'Paid', get: r => day(r.paid_at) }])}
<h2>Tickets</h2>${table(tickets, [{ label: '#', html: r => link(`/tickets/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Subject', key: 'subject' }])}
${invoices.length ? `<h2>Invoices</h2>${table(invoices, [{ label: '#', html: r => link(`/invoices/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Total', get: r => money(r.total_cents), num: true }])}` : ''}
<h2>Waitlist</h2>${table(wl, [{ label: 'SKU', html: r => link(`/products/${encodeURIComponent(r.sku)}`, r.sku) }, { label: 'Qty', key: 'qty' }, { label: 'Notified', get: r => dt(r.notified_at) }])}
</div><div>
<h2>Timeline</h2><ul class="tl">${events.map(x => `<li><span class="meta">${e(dt(x.at))}</span><br>${x.html}</li>`).join('')}</ul>
${form(`/contacts/${ct.id}/tags`, c.csrf, field('tags', 'Tags, comma-separated (vip, school, press…)', { value: ct.tags }), { submit: 'Save tags' })}
${form(`/contacts/${ct.id}/notes`, c.csrf, field('notes', 'Notes', { type: 'textarea', value: ct.notes || '' }), { submit: 'Save notes' })}
</div></div>`);
  });
  router.post('/contacts/:id/consent', (c) => { sendConsentRequest(db, Number(c.params.id), cfg); back(c, `/contacts/${c.params.id}`, 'confirmation mail queued'); });
  router.post('/contacts/:id/notes', (c) => { db.run('UPDATE contacts SET notes = ? WHERE id = ?', c.body.notes, c.params.id); back(c, `/contacts/${c.params.id}`, 'saved'); });
  router.post('/waitlist/notify', async (c) => { const cat = await app.catalog().catch(() => null); const name = cat?.products.get(c.body.sku)?.name || c.body.sku; const n = notifyWaitlist(db, c.body.sku, cfg, { productName: name }); back(c, '/contacts', `queued ${n} in-stock mails for ${c.body.sku}`); });

  // ---------- inventory
  router.get('/inventory', (c) => {
    const items = listItems(db);
    page(c, 'Inventory', table(items, [{ label: '', html: r => r.low ? '<span class="bad">●</span>' : '' }, { label: 'SKU', key: 'sku' }, { label: 'Name', key: 'name' }, { label: 'Kind', key: 'kind' }, { label: 'On hand', key: 'on_hand', num: true }, { label: 'Reorder at', key: 'reorder_point', num: true }, { label: 'Unit cost', get: r => money(r.unit_cost_cents), num: true }, { label: 'Supplier', html: r => r.supplier_url ? link(r.supplier_url, r.supplier || 'link') : e(r.supplier || '') }, { label: 'Where', key: 'location' }]) +
      `<h2>Recipes (what a packed SKU consumes)</h2>${table(recipes(db), [{ label: 'Finished SKU', key: 'finished_sku' }, { label: 'Parts', key: 'parts' }, { label: 'Buildable now', get: r => buildable(db, r.finished_sku), num: true }])}` +
      `<div class="row">${form('/inventory/move', c.csrf, `<b>Move stock</b><div class="row">${field('sku', 'SKU', { options: items.map(i => [i.sku, i.sku]) })}${field('delta', 'Δ (negative to remove)', { type: 'number', required: true })}${field('reason', 'Reason', { options: ['receive', 'adjust', 'scrap', 'build', 'warranty'] })}${field('ref', 'Ref')}</div>`, { submit: 'Move' })}
${form('/inventory/item', c.csrf, `<b>Add / edit an item</b><div class="row">${field('sku', 'SKU', { required: true })}${field('name', 'Name', { required: true })}${field('kind', 'Kind', { options: ['part', 'printed', 'finished', 'packaging'] })}</div><div class="row">${field('reorder_point', 'Reorder point', { type: 'number', value: 0 })}${field('reorder_qty', 'Reorder qty', { type: 'number', value: 0 })}${field('unit_cost_cents', 'Unit cost (cents)', { type: 'number', value: 0 })}</div><div class="row">${field('supplier', 'Supplier')}${field('supplier_url', 'Supplier URL')}${field('location', 'Location')}</div>`, { submit: 'Save item' })}</div>
${form('/inventory/recipe', c.csrf, `<b>Set a recipe</b><div class="row">${field('finished_sku', 'Finished SKU (store.json id)', { required: true })}${field('parts', 'Parts, e.g. xiao-esp32s3=1 wap-shell-petg=1 cable-usbc=1', { required: true })}</div>`, { submit: 'Save recipe' })}
<h2>Recent moves</h2>${table(moves(db, '', 50), [{ label: 'When', get: r => dt(r.at) }, { label: 'SKU', key: 'sku' }, { label: 'Δ', key: 'delta', num: true }, { label: 'Reason', key: 'reason' }, { label: 'Ref', key: 'ref' }])}`);
  });
  router.post('/inventory/item', (c) => { upsertItem(db, { ...c.body, reorder_point: Number(c.body.reorder_point), reorder_qty: Number(c.body.reorder_qty), unit_cost_cents: Number(c.body.unit_cost_cents) }); back(c, '/inventory', `saved ${c.body.sku}`); });
  router.post('/inventory/move', (c) => { move(db, c.body.sku, Number(c.body.delta), c.body.reason, c.body.ref || null); back(c, '/inventory', `moved ${c.body.sku} ${c.body.delta}`); });
  router.post('/inventory/recipe', (c) => { setRecipe(db, c.body.finished_sku, Object.fromEntries(String(c.body.parts).trim().split(/\s+/).map(p => p.split('=')))); back(c, '/inventory', `recipe saved for ${c.body.finished_sku}`); });

  // ---------- devices
  router.get('/devices', (c) => page(c, 'Devices (serial registry)', filters(c, [['q', 'Serial / MAC / batch / order']]) + table(listDevices(db, { q: c.query.get('q') || '' }), [{ label: 'Serial', key: 'serial' }, { label: 'MAC', key: 'mac' }, { label: 'Board', key: 'board' }, { label: 'SKU', key: 'product_sku' }, { label: 'Firmware', get: r => `${r.fw_version || ''} ${r.fw_sha256 ? r.fw_sha256.slice(0, 8) : ''}` }, { label: 'Flashed', get: r => day(r.flashed_at) }, { label: 'Batch', key: 'batch' }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Order', key: 'order_number' }]) +
    `<div class="row">${form('/devices/register', c.csrf, `<b>Register one</b><div class="row">${field('serial', 'Serial', { required: true })}${field('mac', 'MAC')}${field('board', 'Board')}${field('product_sku', 'SKU')}</div><div class="row">${field('fw_version', 'Firmware version')}${field('fw_sha256', 'Firmware sha256')}${field('batch', 'Batch')}</div>`, { submit: 'Register' })}
${form('/devices/import', c.csrf, `<b>Import the jig's manifest</b><p class="muted">create_manifest.py JSON ({ devices: [{ mac, fingerprint, firmware_version… }] }).</p>${field('json', 'JSON', { type: 'textarea', required: true })}<div class="row">${field('batch', 'Batch')}${field('product_sku', 'SKU')}${field('board', 'Board')}</div>`, { submit: 'Import' })}</div>`));
  router.post('/devices/register', (c) => { registerDevice(db, c.body); back(c, '/devices', `registered ${c.body.serial}`); });
  router.post('/devices/import', (c) => { let j; try { j = JSON.parse(c.body.json); } catch { throw new HttpError(400, 'bad JSON'); } const n = importManifest(db, j, { batch: c.body.batch || null, product_sku: c.body.product_sku || null, board: c.body.board || null }); back(c, '/devices', `imported ${n}`); });

  // ---------- warranty
  router.get('/warranty', (c) => page(c, 'Warranty — fix, not return', table(listClaims(db, { status: c.query.get('status') || '' }), [{ label: '#', html: r => link(`/warranty/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Category', get: r => ANSWERS[r.category]?.label || r.category }, { label: 'Order', key: 'order_number' }, { label: 'Email', key: 'email' }, { label: 'Answer', key: 'resolution' }, { label: 'Opened', get: r => day(r.created_at) }]) +
    form('/warranty/new', c.csrf, `<b>Open a claim</b><div class="row">${field('order', 'Order number', { required: true })}${field('email', 'Customer email (if not on the order)')}${field('serial', 'Board serial (optional)')}</div>${field('category', 'What went wrong', { options: Object.entries(ANSWERS).map(([k, v]) => [k, `${v.label} → ${v.action}`]) })}${field('description', 'What they said', { type: 'textarea' })}`, { submit: 'Open claim' }) +
    `<h2>The defined answers (terms of sale §7)</h2>${table(Object.entries(ANSWERS).map(([k, v]) => ({ k, ...v })), [{ label: 'Key', key: 'k' }, { label: 'What went wrong', key: 'label' }, { label: 'What we do', key: 'action' }, { label: 'Send it back?', get: r => r.returns ? 'yes' : 'no' }])}`));
  router.post('/warranty/new', (c) => { const cl = openClaim(db, { orderNumber: c.body.order, email: c.body.email, serial: c.body.serial, category: c.body.category, description: c.body.description, cfg }); back(c, `/warranty/${cl.id}`, `opened ${cl.number}`); });
  router.get('/warranty/:id', (c) => {
    const cl = claimById(db, c.params.id); if (!cl) throw new HttpError(404, 'no such claim');
    const orig = cl.order_id ? orderById(db, cl.order_id) : null;
    page(c, `${cl.number} · ${ANSWERS[cl.category]?.label || cl.category}`, `<p>${statusPill(cl.status)} · order ${orig ? link(`/orders/${orig.id}`, orig.number) : '—'} · ${cl.email ? e(cl.email) : ''} · serial ${e(cl.serial || '—')} · ticket ${cl.ticket_id ? link(`/tickets/${cl.ticket_id}`, 'thread') : '—'} · reship ${cl.reship_order_id ? link(`/orders/${cl.reship_order_id}`, 'order') : '—'}</p>
<p><b>Defined answer:</b> ${e(cl.resolution)} · send the original back: <b>no</b></p><p>${e(cl.description || '')}</p>
${cl.status === 'open' ? form(`/warranty/${cl.id}/reship`, c.csrf, `<b>Create the $0 reship order</b>${field('items', 'Items, e.g. wap-parts=1 or xiao-esp32s3=1 (store SKU or inventory SKU)', { required: true, value: orig?.items.map(i => `${i.sku}=${i.qty}`).join(' ') || '' })}`, { submit: 'Create reship' }) : ''}
${cl.status !== 'resolved' ? form(`/warranty/${cl.id}/resolve`, c.csrf, `<div class="row">${field('cost_cents', 'Our cost (cents)', { type: 'number', value: 0 })}${field('note', 'Note to the customer')}</div>`, { submit: 'Resolve + email' }) : ''}`);
  });
  router.post('/warranty/:id/reship', async (c) => { const items = String(c.body.items).trim().split(/\s+/).map(p => { const [sku, q] = p.split('='); return { sku, name: sku, qty: Number(q || 1) }; }); let catalog = await app.catalog().catch(() => null); if (catalog && items.some(i => !catalog.products.has(i.sku))) catalog = null; const cl = reship(db, Number(c.params.id), items, { recordOrderFn: recordOrder, catalog, actor: 'web' }); back(c, `/warranty/${cl.id}`, 'reship order created'); });
  router.post('/warranty/:id/resolve', (c) => { resolveClaim(db, Number(c.params.id), { costCents: Number(c.body.cost_cents), note: c.body.note, cfg }); back(c, `/warranty/${c.params.id}`, 'resolved'); });

  // ---------- invoices & plans
  router.get('/invoices', (c) => page(c, 'Invoices & plans', table(listInvoices(db), [{ label: '#', html: r => link(`/invoices/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'To', get: r => r.email }, { label: 'PO', key: 'po_number' }, { label: 'Total', get: r => money(r.total_cents), num: true }, { label: 'Issued', key: 'issued_at' }, { label: 'Due', key: 'due_at' }]) +
    form('/invoices/new', c.csrf, `<b>New invoice</b><div class="row">${field('email', 'Email', { type: 'email', required: true })}${field('name', 'Contact name')}${field('org', 'Organization')}</div>${field('bill_to', 'Bill-to block', { type: 'textarea', rows: 3 })}<div class="row">${field('po_number', 'PO number')}${field('terms_days', 'Terms (days)', { type: 'number', value: 30 })}${field('shipping_cents', 'Shipping (cents)', { type: 'number', value: 0 })}${field('tax_cents', 'Tax (cents)', { type: 'number', value: 0 })}</div>${field('items', 'Lines, one per row: sku | description | qty | unit cents', { type: 'textarea', required: true, placeholder: 'wap-kit | Canary WAP Kit | 20 | 6200' })}${field('notes', 'Notes')}`, { submit: 'Create draft' }) +
    `<h2>Support plans (Stripe Billing mirror)</h2>${table(listSubscriptions(db), [{ label: 'Plan', key: 'plan' }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Who', get: r => `${r.email || ''} ${r.org || ''}` }, { label: 'Amount', get: r => `${money(r.amount_cents)}/${r.interval}`, num: true }, { label: 'Renews', get: r => day(r.current_period_end) }])}`));
  router.post('/invoices/new', (c) => { const items = String(c.body.items).split('\n').map(l => l.split('|').map(s => s.trim())).filter(l => l.length >= 4).map(([sku, description, qty, unit_cents]) => ({ sku, description, qty: Number(qty), unit_cents: Number(unit_cents) })); const inv = createInvoice(db, { ...c.body, items, terms_days: Number(c.body.terms_days), shipping_cents: Number(c.body.shipping_cents || 0), tax_cents: Number(c.body.tax_cents || 0) }); back(c, `/invoices/${inv.id}`, `draft ${inv.number}`); });
  router.get('/invoices/:id', (c) => { const inv = invoiceById(db, c.params.id); if (!inv) throw new HttpError(404, 'no such invoice'); page(c, `Invoice ${inv.number}`, `<p>${statusPill(inv.status)} · ${e(inv.email || '')} · PO ${e(inv.po_number || '—')} · ${money(inv.total_cents)} · issued ${e(inv.issued_at || '—')} · due ${e(inv.due_at || '—')} · paid ${e(inv.paid_at || '—')}</p><p>${link(invoiceUrl(cfg, inv), 'Customer link (printable)')}</p>${table(inv.items, [{ label: 'SKU', key: 'sku' }, { label: 'Description', key: 'description' }, { label: 'Qty', key: 'qty', num: true }, { label: 'Unit', get: r => money(r.unit_cents), num: true }])}<p>${inv.status === 'draft' ? button(`/invoices/${inv.id}/issue`, c.csrf, 'Issue + email') : ''} ${['sent', 'overdue'].includes(inv.status) ? button(`/invoices/${inv.id}/paid`, c.csrf, 'Mark paid') : ''} ${inv.status !== 'paid' && inv.status !== 'void' ? button(`/invoices/${inv.id}/void`, c.csrf, 'Void', { quiet: true }) : ''}</p>`); });
  router.post('/invoices/:id/issue', (c) => { issueInvoice(db, Number(c.params.id), cfg); back(c, `/invoices/${c.params.id}`, 'issued'); });
  router.post('/invoices/:id/paid', (c) => { markPaid(db, Number(c.params.id)); back(c, `/invoices/${c.params.id}`, 'paid'); });
  router.post('/invoices/:id/void', (c) => { voidInvoice(db, Number(c.params.id)); back(c, `/invoices/${c.params.id}`, 'voided'); });

  registerMail(app, { page, back, link, filters });

  // ---------- books
  router.get('/books', (c) => {
    const y = new Date().getUTCFullYear(); const from = c.query.get('from') || `${y}-01-01`, to = c.query.get('to') || `${y}-12-31`;
    const tb = trialBalance(db); const months = monthly(db, 12);
    page(c, 'Books', `<p class="muted">Double-entry, cents, every order and refund posted automatically. Bookkeeping, not tax advice — hand the year-end CSV to an accountant. ${link(`/books.csv?from=${from}&to=${to}`, 'Ledger CSV')}</p>
<h2>Monthly P&amp;L</h2>${table(months, [{ label: 'Month', key: 'month' }, { label: 'Income', get: r => money(r.income_cents), num: true }, { label: 'Expenses', get: r => money(r.expenses_cents), num: true }, { label: 'Net', html: r => `<span class="${r.net_cents < 0 ? 'bad' : 'ok'}">${money(r.net_cents)}</span>`, num: true }])}
<h2>Trial balance ${tb.balanced ? '<span class="ok">(balanced)</span>' : '<span class="bad">(NOT balanced — a bug, report it)</span>'}</h2>${table(tb.rows, [{ label: 'Account', key: 'account' }, { label: '', key: 'label' }, { label: 'Balance', get: r => money(r.balance_cents), num: true }])}
<h2>Sales tax by ship-to state (${e(from)} → ${e(to)})</h2>${filters(c, [['from', 'From'], ['to', 'To']])}${table(salesTaxByState(db, { from, to }), [{ label: 'State', key: 'state' }, { label: 'Orders', key: 'orders', num: true }, { label: 'Taxable', get: r => money(r.taxable_cents), num: true }, { label: 'Tax collected', get: r => money(r.tax_cents), num: true }], { empty: 'No sales in range.' })}
<div class="row">${form('/books/expense', c.csrf, `<b>Record an expense</b><div class="row">${field('date', 'Date', { type: 'date', value: new Date().toISOString().slice(0, 10) })}${field('account', 'Category', { options: EXPENSE_ACCOUNTS.map(a => [a, `${a} — ${ACCOUNTS[a]}`]) })}</div><div class="row">${field('cents', 'Cents', { type: 'number', required: true })}${field('memo', 'Memo')}${field('paidFrom', 'Paid from', { options: [['assets:bank', 'bank'], ['equity:owner', 'owner (personal card)']] })}</div>`, { submit: 'Post' })}
${form('/books/payout', c.csrf, `<b>Record a Stripe payout</b><p class="muted">Automatic when the payout.paid webhook is on; otherwise from the Payouts page.</p><div class="row">${field('id', 'Payout id (po_…)', { required: true })}${field('date', 'Arrival date', { type: 'date' })}</div><div class="row">${field('cents', 'Amount (cents)', { type: 'number', required: true })}${field('fee_cents', 'Fees (cents)', { type: 'number', value: 0 })}</div>`, { submit: 'Post payout' })}</div>`);
  });
  router.get('/books.csv', (c) => text(c.res, ledgerCsv(db, { from: c.query.get('from') || undefined, to: c.query.get('to') || undefined }), 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="ledger.csv"' }));
  router.post('/books/expense', (c) => { postExpense(db, { date: c.body.date, account: c.body.account, cents: Number(c.body.cents), memo: c.body.memo, paidFrom: c.body.paidFrom }); back(c, '/books', 'expense posted'); });
  router.post('/books/payout', (c) => { postPayout(db, { id: c.body.id, date: c.body.date || undefined, amount_cents: Number(c.body.cents), fee_cents: Number(c.body.fee_cents || 0) }); back(c, '/books', 'payout posted'); });

  // ---------- status admin
  router.get('/status-admin', (c) => page(c, 'Status & probes', `<p>${link('/status', 'Public page')} · ${button('/status-admin/run', c.csrf, 'Run all probes now', { quiet: true })}</p>` +
    table(listProbes(db), [{ label: 'Name', key: 'name' }, { label: 'URL', key: 'url' }, { label: 'Now', html: r => r.last_ok == null ? '<span class="muted">—</span>' : r.last_ok ? '<span class="ok">up</span>' : `<span class="bad">down (${e(r.last_detail || '')})</span>` }, { label: 'ms', key: 'last_ms', num: true }, { label: '90 d', get: r => { const u = uptime(db, r.id); return u == null ? '—' : u + '%'; }, num: true }, { label: 'Every', get: r => r.interval_s + ' s' }, { label: 'Public', get: r => r.public ? 'yes' : 'no' }, { label: '', html: r => `<form method="post" action="/status-admin/probe/${r.id}/delete" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><button class="quiet">delete</button></form>` }]) +
    form('/status-admin/probe', c.csrf, `<b>Add / edit a probe</b><div class="row">${field('name', 'Name', { required: true })}${field('url', 'URL', { required: true })}</div><div class="row">${field('expect_status', 'Expect status', { type: 'number', value: 200 })}${field('expect_text', 'Expect text (optional)')}${field('interval_s', 'Interval (s)', { type: 'number', value: 300 })}${field('public', 'Public', { options: [['1', 'yes'], ['0', 'no']] })}</div>`, { submit: 'Save probe' }) +
    `<h2>Incidents</h2>${table(listIncidents(db), [{ label: 'Started', get: r => dt(r.started_at) }, { label: 'Title', key: 'title' }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Body', key: 'body' }, { label: '', html: r => r.status !== 'resolved' ? `<form method="post" action="/status-admin/incident/${r.id}" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="status" value="resolved"><button class="quiet">resolve</button></form>` : '' }])}` +
    form('/status-admin/incident', c.csrf, `<b>Open an incident</b>${field('title', 'Title', { required: true })}${field('body', 'What is happening, for the public page', { type: 'textarea', rows: 3 })}`, { submit: 'Post incident' })));
  router.post('/status-admin/probe', (c) => { upsertProbe(db, { ...c.body, expect_status: Number(c.body.expect_status), interval_s: Number(c.body.interval_s), public: Number(c.body.public) }); back(c, '/status-admin', 'probe saved'); });
  router.post('/status-admin/probe/:id/delete', (c) => { deleteProbe(db, c.params.id); back(c, '/status-admin', 'deleted'); });
  router.post('/status-admin/run', async (c) => { const r = await runDueProbes(db, { cfg, force: true }); back(c, '/status-admin', `ran ${r.length}: ${r.map(p => `${p.name}=${p.ok ? 'up' : 'down'}`).join(', ')}`); });
  router.post('/status-admin/incident', (c) => { openIncident(db, c.body); back(c, '/status-admin', 'incident posted'); });
  router.post('/status-admin/incident/:id', (c) => { updateIncident(db, c.params.id, c.body); back(c, '/status-admin', 'updated'); });

  // ---------- exceptions
  router.get('/exceptions', (c) => page(c, 'Exceptions', `<p class="muted">What the systems raised for a human. Each is deduplicated by key and mirrored to a GitHub issue when a token is set; they resolve themselves when the condition clears.</p>` +
    table(listExceptions(db), [{ label: 'Opened', get: r => dt(r.opened_at) }, { label: 'Key', key: 'key' }, { label: 'Title', key: 'title' }, { label: 'Body', html: r => `<pre style="white-space:pre-wrap;margin:0">${e(r.body)}</pre>` }, { label: 'Issue', html: r => r.github_issue ? link(`https://github.com/${cfg.githubRepo}/issues/${r.github_issue}`, '#' + r.github_issue) : '—' }, { label: 'Resolved', get: r => dt(r.resolved_at) }, { label: '', html: r => r.resolved_at ? '' : `<form method="post" action="/exceptions/resolve" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="key" value="${e(r.key)}"><button class="quiet">resolve</button></form>` }])));
  router.post('/exceptions/resolve', async (c) => { await resolveException(db, cfg, c.body.key, 'Resolved by the operator.'); back(c, '/exceptions', 'resolved'); });

  // ---------- settings
  router.get('/settings', (c) => page(c, 'Settings', `<h2>Configuration (from the environment)</h2><pre>${e(Object.entries(cfg).filter(([k]) => !/secret|token|url$/i.test(k) || /baseUrl|siteOrigin/.test(k)).map(([k, v]) => `${k} = ${v}`).join('\n'))}\nsmtp: ${cfg.smtpUrl ? 'configured' : 'NOT SET — mail is queued, not sent'}\nimap: ${cfg.imapUrl ? 'configured' : 'NOT SET — inbound mail not read'}\nstripe webhook: ${cfg.stripeWebhookSecret ? 'configured' : 'NOT SET'}\ngithub: ${cfg.githubToken ? 'configured' : 'NOT SET — exceptions stay local'}</pre>
<h2>API tokens</h2>${table(db.all('SELECT name, scope, created_at, last_used_at FROM api_tokens'), [{ label: 'Name', key: 'name' }, { label: 'Scope', html: r => r.scope === 'ro' ? '<span class="pill">read-only</span>' : '<span class="pill warn">read + write</span>' }, { label: 'Created', get: r => dt(r.created_at) }, { label: 'Last used', get: r => dt(r.last_used_at) }, { label: '', html: r => `<form method="post" action="/settings/token/revoke" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="name" value="${e(r.name)}"><button class="quiet">revoke</button></form>` }])}
${form('/settings/token', c.csrf, `<div class="row">${field('name', 'New token name', { required: true })}${field('scope', 'Scope', { options: [['rw', 'read + write (the CLI, an AI operator)'], ['ro', 'read-only (a dashboard, a monitor)']] })}</div>`, { submit: 'Create token' })}
<h2>Jobs</h2>${table(Object.keys(JOBS).map(n => ({ name: n, every: JOBS[n][0], ...(jobRuns(db).find(r => r.name === n) || {}) })), [{ label: 'Job', key: 'name' }, { label: 'Every', get: r => r.every + ' s' }, { label: 'Last run', get: r => dt(r.last_run_at) }, { label: 'OK', html: r => r.last_ok == null ? '—' : r.last_ok ? '<span class="ok">yes</span>' : '<span class="bad">no</span>' }, { label: 'Detail', key: 'last_detail' }, { label: '', html: r => `<form method="post" action="/settings/jobs/run" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="name" value="${e(r.name)}"><button class="quiet">run</button></form>` }])}
<h2>Backups</h2>${table(listBackups(join(cfg.dataDir, 'backups')), [{ label: 'File', key: 'name' }, { label: 'Bytes', key: 'bytes', num: true }])}${button('/settings/backup', c.csrf, 'Back up now', { quiet: true })}
<h2>Audit log (last 100)</h2>${table(db.all('SELECT * FROM audit_log ORDER BY id DESC LIMIT 100'), [{ label: 'When', get: r => dt(r.at) }, { label: 'Actor', key: 'actor' }, { label: 'Action', key: 'action' }, { label: 'Entity', get: r => `${r.entity || ''} ${r.entity_id || ''}` }, { label: 'Detail', key: 'detail' }])}`));
  router.post('/settings/token', (c) => { const t = createApiToken(db, c.body.name, { scope: c.body.scope === 'ro' ? 'ro' : 'rw' }); html(c.res, layout({ title: 'Token created', path: '/settings', company: cfg.company, csrf: c.csrf, demo: cfg.demo, body: `<p>Copy it now; it is not shown again.</p><pre>${e(t)}</pre><p>Use: <code>Authorization: Bearer …</code> against ${e(cfg.baseUrl)}/api</p><p>${link('/settings', 'Back')}</p>` })); });
  router.post('/settings/token/revoke', (c) => { revokeApiToken(db, c.body.name); back(c, '/settings', 'revoked'); });
  router.post('/settings/jobs/run', async (c) => { const r = await runJob(app, c.body.name); back(c, '/settings', `${r.job}: ${r.ok ? JSON.stringify(r.result) : r.error}`); });
  router.post('/settings/backup', (c) => { const r = backup(db, join(cfg.dataDir, 'backups'), { keep: cfg.backupKeep }); back(c, '/settings', `wrote ${r.path}`); });

  // ---------- demo scenarios (only when the database is a demo)
  router.get('/demo', (c) => {
    if (!cfg.demo) throw new HttpError(404, 'not in demo mode');
    const last = c.query.get('result');
    const body = `<p>Every button below pushes a synthetic event through the <b>real</b> code path — the signed Stripe webhook, the mail parser, the form endpoint, the probe runner — so each feature can be watched reacting. Names are placeholders, domains are <code>.example.test</code> (reserved, can never resolve), money is play money. Nothing here can reach a real service: SMTP, IMAP, GitHub and Stripe are disabled by the demo flag in the database.</p>
${last ? `<pre>${e(last)}</pre>` : ''}
<div class="scen">${Object.entries(SCENARIOS).map(([k, v]) => `<form method="post" action="/demo/${e(k)}"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><button>${e(v.label)}</button></form>`).join('')}</div>
<h2>Try the website forms</h2><p class="muted">The same markup the real site will carry (<code>deploy/website-forms.html</code>), posting to this instance.</p>
<div class="row">
<form method="post" action="/forms/waitlist" class="box"><input type="hidden" name="sku" value="vision-kit"><input type="hidden" name="_redirect" value=""><b>Notify me when it's in stock</b>${field('email', 'Email', { type: 'email', value: PEOPLE[6][0] })}<label><input type="checkbox" name="newsletter" value="1" style="width:auto"> also send occasional news (double opt-in)</label><div style="margin-top:.6rem"><button>Join the waitlist</button></div></form>
<form method="post" action="/forms/contact" class="box"><b>Contact</b>${field('name', 'Name', { value: 'Visitor Example' })}${field('email', 'Email', { type: 'email', value: 'visitor@customer.example.test' })}${field('subject', 'Subject', { value: 'A question from the demo page' })}${field('message', 'Message', { type: 'textarea', rows: 3, value: 'Hello from the demo.' })}<div style="margin-top:.6rem"><button>Send</button></div></form>
</div>
<h2>The API side</h2><pre>${e(`curl -X POST -H "Authorization: Bearer <token from Settings>" ${cfg.baseUrl}/api/demo/order
curl -H "Authorization: Bearer <token>" ${cfg.baseUrl}/api/demo/scenarios`)}</pre>
<h2>Start over</h2>${button('/demo/reset', c.csrf, 'Wipe and reseed the demo data', { quiet: true })} <span class="muted">keeps the operator login; everything else is regenerated</span>`;
    page(c, 'Demo scenarios', body);
  });
  router.post('/demo/reset', async (c) => { if (!cfg.demo) throw new HttpError(404, 'not in demo mode'); const r = await resetDemo(db, cfg); back(c, '/demo', `reseeded: ${JSON.stringify(r)}`); });
  router.post('/demo/:scenario', async (c) => {
    if (!cfg.demo) throw new HttpError(404, 'not in demo mode');
    const sc = SCENARIOS[c.params.scenario]; if (!sc) throw new HttpError(404, 'unknown scenario');
    const r = await sc.run(app);
    redirect(c.res, `/demo?ok=${encodeURIComponent(sc.label)}&result=${encodeURIComponent(JSON.stringify(r, null, 1).slice(0, 1500))}`);
  });
}

// The activity feed: an audit row as one readable line.
function describeAudit(r) {
  const d = (() => { try { return JSON.parse(r.detail || '{}'); } catch { return {}; } })();
  const ent = r.entity_id ? `<b>${e(r.entity_id)}</b>` : '';
  const text = {
    'order.create': `Order ${ent} placed${d.total_cents != null ? ` · $${(d.total_cents / 100).toFixed(2)}` : ''}${d.discount_code ? ` · code ${e(d.discount_code)}` : ''}`,
    'order.packed': `Order ${ent} packed`, 'order.shipped': `Order ${ent} shipped${d.tracking ? ` · ${e(d.carrier || '')} ${e(d.tracking)}` : ''}`, 'order.delivered': `Order ${ent} delivered`, 'order.canceled': `Order ${ent} canceled`,
    'order.refund': `Refund of $${((d.cents || 0) / 100).toFixed(2)} on ${ent}`, 'order.note': `Note added to ${ent}`, 'order.tags': `Tags on ${ent}: ${e(d.tags || '')}`, 'order.address': `Address changed on ${ent}`, 'order.resend_tracking': `Tracking re-sent for ${ent}`,
    'ticket.open': `Ticket ${ent} opened (${e(d.kind || '')} via ${e(d.channel || '')})`, 'ticket.reply': `Replied on ${ent}`, 'ticket.closed': 'Ticket closed', 'device.assign': `Board ${ent} scanned into an order`,
    'contact.consent': 'A customer confirmed marketing opt-in', 'contact.unsubscribe': 'A customer unsubscribed', 'contact.tags': `Customer tags: ${e(d.tags || '')}`,
    'campaign.queue': `Campaign ${ent} sent to ${d.recipients ?? '?'} people`, 'campaign.schedule': `Campaign ${ent} scheduled`, 'campaign.test': `Test of ${ent} sent`, 'automation.run': `Automations ran: ${d.enrolled || 0} enrolled, ${d.sent || 0} sent`, 'automation.save': `Automation ${ent} saved`, 'contact.bounce': `<span class="warn">A mail bounced: ${e(d.email || '')} — suppressed</span>`, 'contact.bounce.clear': 'A bounce was cleared',
    'discount.create': `Discount ${ent} created`, 'discount.disabled': `Discount ${ent} disabled`, 'discount.active': `Discount ${ent} enabled`,
    'auth.login': `Operator logged in${d.ip ? ` from ${e(d.ip)}` : ''}`, 'auth.login.failed': `<span class="bad">A login attempt failed${d.ip ? ` from ${e(d.ip)}` : ''}</span>`, 'auth.login.locked': `<span class="bad">An address was locked out after repeated failures${d.ip ? `: ${e(d.ip)}` : ''}</span>`, 'auth.token.create': `API token ${ent} created`,
    'warranty.open': `Warranty claim ${ent} opened`, 'warranty.reship': `Reship order created for claim ${ent}`, 'warranty.resolve': `Warranty claim ${ent} resolved`,
    'invoice.create': `Invoice ${ent} drafted`, 'invoice.issue': `Invoice ${ent} issued`, 'invoice.paid': `Invoice ${ent} paid`, 'invoice.void': `Invoice ${ent} voided`,
    'exception.raise': `<span class="warn">Exception raised: ${ent}</span>`, 'exception.resolve': `Exception resolved: ${ent}`, 'ticket.waiting_on_customer': 'Ticket waiting on the customer', 'ticket.open': `Ticket ${ent} opened (${e(d.kind || '')} via ${e(d.channel || '')})`,
    'inventory.move': `Stock moved: ${ent}`, 'campaign.queue': `Campaign ${ent} queued`, 'waitlist.notify': `In-stock mail sent for ${ent}`, 'contact.consent.request': 'Opt-in mail sent',
  }[r.action];
  return { at: r.at, actor: r.actor, html: text || `${e(r.action)} ${ent}` };
}
