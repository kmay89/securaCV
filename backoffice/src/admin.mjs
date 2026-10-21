// The admin: one page per module, plain forms, no client JavaScript.
import { html, redirect, text, HttpError, escapeHtml as e } from './http.mjs';
import { layout, table, money, dt, day, statusPill, field, form, button, cards } from './render.mjs';
import { snapshot } from './modules/dashboard.mjs';
import { listOrders, orderById, transition, refund, recordOrder, fulfillCsv, weekLabel, STATUSES } from './modules/orders.mjs';
import { parseCsv, detectColumns, csvRowToOrder } from './modules/stripe.mjs';
import { listTickets, ticketById, reply, setStatus, update as updateTicket, openTicket, STATUSES as TSTATUS, KINDS } from './modules/support.mjs';
import { searchContacts, contactById, upsertContact, waitlistSummary, waitlistFor } from './modules/contacts.mjs';
import { sendConsentRequest, notifyWaitlist, listCampaigns, createCampaign, queueCampaign, campaign } from './modules/campaigns.mjs';
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
import { join } from 'node:path';

export function registerAdmin(app) {
  const { db, cfg, router } = app;
  const page = (c, title, body) => html(c.res, layout({ title, body, path: c.url.pathname, company: cfg.company, csrf: c.csrf, flash: c.query.get('ok') ? { text: c.query.get('ok') } : c.query.get('err') ? { kind: 'err', text: c.query.get('err') } : null }));
  const back = (c, to, ok) => redirect(c.res, `${to}?ok=${encodeURIComponent(ok)}`);
  const link = (href, label) => `<a href="${e(href)}">${e(label)}</a>`;
  const filters = (c, fields) => `<form method="get" class="row" style="margin:.6rem 0">${fields.map(([n, l, opts]) => opts ? field(n, l, { options: [['', 'any'], ...opts], value: c.query.get(n) || '' }) : field(n, l, { value: c.query.get(n) || '' })).join('')}<div><label>&nbsp;</label><button class="quiet">Filter</button></div></form>`;

  // ---------- dashboard
  router.get('/', (c) => {
    const s = snapshot(db);
    const m = s.books.months.at(-1) || { income_cents: 0, expenses_cents: 0, net_cents: 0 };
    const body = cards([
      ['Orders to pack/ship', s.orders.open, s.orders.open ? 'warn' : ''], ['This batch', `${s.orders.thisBatch} <span class="muted" style="font-size:.8rem">${weekLabel()}</span>`],
      ['Revenue, 30 days', money(s.orders.revenue30)], ['Units, 30 days', s.orders.units30],
      ['Support needing an answer', s.support.open, s.support.breached ? 'bad' : ''], ['SLA breaches', s.support.breached, s.support.breached ? 'bad' : 'ok'],
      ['Warranty open', s.warranty.open], ['Warranty rate, 90 d', `${(s.warranty.rate90 * 100).toFixed(1)}%`],
      ['Low stock', s.inventory.low.length, s.inventory.low.length ? 'warn' : 'ok'], ['Inventory at cost', money(s.inventory.value_cents)],
      ['This month net', money(m.net_cents), m.net_cents < 0 ? 'bad' : 'ok'], ['MRR (plans)', money(s.books.mrr_cents)],
      ['Receivable', money(s.books.receivable_cents)], ['In Stripe, not paid out', money(s.books.stripe_unpaid_out_cents)],
      ['Open exceptions', s.exceptions.length, s.exceptions.length ? 'bad' : 'ok'], ['Probes down', s.probes.filter(p => p.last_ok === 0).length, s.probes.some(p => p.last_ok === 0) ? 'bad' : 'ok'],
    ]) +
    `<h2>Exceptions (what a human must decide)</h2>${table(s.exceptions, [{ label: 'Opened', get: r => dt(r.opened_at) }, { label: 'Title', html: r => link('/exceptions', r.title) }, { label: 'Issue', html: r => r.github_issue ? link(`https://github.com/${cfg.githubRepo}/issues/${r.github_issue}`, '#' + r.github_issue) : '—' }], { empty: 'Silence means healthy.' })}` +
    `<h2>Waitlist</h2>${table(s.waitlist, [{ label: 'SKU', key: 'sku' }, { label: 'People', key: 'people', num: true }, { label: 'Units', key: 'units', num: true }, { label: 'Not yet notified', key: 'waiting', num: true }], { empty: 'Nobody waiting.' })}` +
    `<h2>Mail outbox</h2><p>${Object.entries(s.mail).map(([k, v]) => `${e(k)}: <b>${v}</b>`).join(' · ') || 'empty'} · contacts ${s.contacts.n} (${s.contacts.consented || 0} consented)</p>`;
    page(c, 'Dashboard', body);
  });

  // ---------- orders
  router.get('/orders', (c) => {
    const rows = listOrders(db, { status: c.query.get('status') || '', batch: c.query.get('batch') || '', q: c.query.get('q') || '' });
    const body = filters(c, [['status', 'Status', STATUSES.map(s => [s, s])], ['batch', 'Batch (e.g. ' + weekLabel() + ')'], ['q', 'Search']]) +
      `<p>${link(`/orders.csv?batch=${e(c.query.get('batch') || '')}&status=${e(c.query.get('status') || 'paid')}`, 'Download fulfill.mjs CSV for this filter')} · ${link('/orders/import', 'Import a Stripe CSV')} · ${link('/orders/new', 'Manual order')}</p>` +
      table(rows, [{ label: '#', html: r => link(`/orders/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Paid', get: r => day(r.paid_at) }, { label: 'Batch', key: 'batch' }, { label: 'Customer', get: r => `${r.ship_name || ''} ${r.email ? `<${r.email}>` : ''}` }, { label: 'Items', key: 'items_text' }, { label: 'Total', get: r => money(r.total_cents, r.currency), num: true }, { label: 'Tracking', key: 'tracking' }]);
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
  router.get('/orders/new', (c) => page(c, 'Manual order', form('/orders/new', c.csrf, `<div class="row">${field('email', 'Email', { type: 'email', required: true })}${field('name', 'Ship to name', { required: true })}</div><div class="row">${field('sku', 'SKU', { required: true })}${field('qty', 'Qty', { type: 'number', value: 1 })}${field('cents', 'Total cents (0 for a free reship)', { type: 'number', value: 0 })}</div><div class="row">${field('line1', 'Address line 1', { required: true })}${field('line2', 'Line 2')}</div><div class="row">${field('city', 'City', { required: true })}${field('state', 'State', { required: true })}${field('zip', 'ZIP', { required: true })}</div>${field('notes', 'Notes')}`, { submit: 'Create order' })));
  router.post('/orders/new', async (c) => {
    const b = c.body; const cents = Number(b.cents || 0); const qty = Number(b.qty || 1);
    const { order } = recordOrder(db, { source: 'manual', email: b.email, name: b.name, subtotal_cents: cents, total_cents: cents, ship: { line1: b.line1, line2: b.line2, city: b.city, state: b.state, zip: b.zip }, items: [{ sku: b.sku, name: b.sku, qty, unit_cents: Math.round(cents / qty) }], notes: b.notes }, { catalog: await app.catalog().catch(() => null), actor: 'web' });
    back(c, `/orders/${order.id}`, `created ${order.number}`);
  });
  router.get('/orders/:id', (c) => {
    const o = orderById(db, c.params.id); if (!o) throw new HttpError(404, 'no such order');
    const devices = db.all('SELECT * FROM devices WHERE order_id = ?', o.id);
    const next = { paid: 'packed', packed: 'shipped', shipped: 'delivered' }[o.status];
    const act = next === 'shipped' ? form(`/orders/${o.id}/transition`, c.csrf, `<input type="hidden" name="to" value="shipped"><div class="row">${field('carrier', 'Carrier', { value: 'USPS' })}${field('tracking', 'Tracking number')}</div>`, { submit: 'Mark shipped + email tracking' })
      : next ? `<form method="post" action="/orders/${o.id}/transition" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="to" value="${next}"><button>Mark ${next}</button></form>` : '';
    const body = `<p>${statusPill(o.status)} · ${e(o.source)} ${o.external_id ? `· <span class="muted">${e(o.external_id)}</span>` : ''} · batch ${e(o.batch || '')} · paid ${dt(o.paid_at)} · packed ${dt(o.packed_at)} · shipped ${dt(o.shipped_at)} ${o.tracking ? `· ${e(o.carrier)} ${e(o.tracking)}` : ''}</p>
<div class="row"><div><b>Ship to</b><br>${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)} ${e(o.ship_country)}<br>${o.email ? link(`/contacts?q=${encodeURIComponent(o.email)}`, o.email) : ''}</div>
<div><b>Money</b><br>subtotal ${money(o.subtotal_cents)} · shipping ${money(o.shipping_cents)} · tax ${money(o.tax_cents)}<br><b>total ${money(o.total_cents)}</b>${o.refunded_cents ? ` · refunded ${money(o.refunded_cents)}` : ''}<br>COGS ${money(o.items.reduce((s, i) => s + i.cogs_cents * i.qty, 0))}</div></div>
${table(o.items, [{ label: 'SKU', key: 'sku' }, { label: 'Item', key: 'name' }, { label: 'Qty', key: 'qty', num: true }, { label: 'Unit', get: r => money(r.unit_cents), num: true }, { label: 'COGS/unit', get: r => money(r.cogs_cents), num: true }])}
<p style="margin-top:1rem">${act} ${o.status === 'paid' ? button(`/orders/${o.id}/transition`, c.csrf, 'Cancel', { quiet: true }).replace('<button', '<input type="hidden" name="to" value="canceled"><button') : ''}</p>
<h2>Devices in this box</h2>${table(devices, [{ label: 'Serial', key: 'serial' }, { label: 'MAC', key: 'mac' }, { label: 'Firmware', key: 'fw_version' }, { label: 'Status', key: 'status' }], { empty: 'No serial scanned yet — the packing-slip QA line.' })}
${form(`/orders/${o.id}/assign`, c.csrf, field('serial', 'Scan a board serial to tie it to this order'), { submit: 'Assign' })}
<h2>Refund</h2>${form(`/orders/${o.id}/refund`, c.csrf, `<div class="row">${field('cents', 'Cents', { type: 'number', value: o.total_cents - o.refunded_cents })}${field('reason', 'Reason')}</div><p class="muted">Records the refund in the books. Issue the actual refund in Stripe (money stays a human action).</p>`, { submit: 'Record refund' })}
<p class="muted">Notes: ${e(o.notes || '')}</p>`;
    page(c, `Order ${o.number}`, body);
  });
  router.post('/orders/:id/transition', (c) => { const o = transition(db, c.params.id, c.body.to, { actor: 'web', carrier: c.body.carrier, tracking: c.body.tracking, cfg }); back(c, `/orders/${o.id}`, `${o.number} is now ${o.status}`); });
  router.post('/orders/:id/refund', (c) => { const o = refund(db, c.params.id, c.body.cents, { actor: 'web', reason: c.body.reason }); back(c, `/orders/${o.id}`, `refund recorded`); });
  router.post('/orders/:id/assign', (c) => { assignToOrder(db, c.body.serial, Number(c.params.id)); back(c, `/orders/${c.params.id}`, `assigned ${c.body.serial}`); });

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
  router.get('/contacts', (c) => {
    const rows = searchContacts(db, c.query.get('q') || '');
    page(c, 'Contacts', filters(c, [['q', 'Search email / name / org']]) + table(rows, [{ label: 'Email', html: r => link(`/contacts/${r.id}`, r.email) }, { label: 'Name', key: 'name' }, { label: 'Org', key: 'org' }, { label: 'Kind', key: 'kind' }, { label: 'Source', key: 'source' }, { label: 'Marketing', html: r => r.consent_marketing && !r.unsubscribed_at ? '<span class="ok">consented</span>' : r.unsubscribed_at ? '<span class="bad">unsubscribed</span>' : '<span class="muted">no</span>' }, { label: 'Since', get: r => day(r.created_at) }]) +
      `<h2>Waitlist</h2>${table(waitlistSummary(db), [{ label: 'SKU', key: 'sku' }, { label: 'People', key: 'people', num: true }, { label: 'Units', key: 'units', num: true }, { label: 'Waiting', key: 'waiting', num: true }, { label: '', html: r => `<form method="post" action="/waitlist/notify" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="sku" value="${e(r.sku)}"><button class="quiet">Notify "in stock"</button></form>` }])}` +
      form('/contacts/new', c.csrf, `<div class="row">${field('email', 'Email', { type: 'email', required: true })}${field('name', 'Name')}${field('org', 'Org')}${field('kind', 'Kind', { options: ['person', 'school', 'business', 'nonprofit', 'press', 'vendor'] })}</div>`, { submit: 'Add contact' }));
  });
  router.post('/contacts/new', (c) => { const ct = upsertContact(db, { ...c.body, source: 'manual' }); back(c, `/contacts/${ct.id}`, 'saved'); });
  router.get('/contacts/:id', (c) => {
    const ct = contactById(db, c.params.id); if (!ct) throw new HttpError(404, 'no such contact');
    const orders = db.all('SELECT * FROM orders WHERE contact_id = ? ORDER BY id DESC', ct.id); const tickets = db.all('SELECT * FROM tickets WHERE contact_id = ? ORDER BY id DESC', ct.id);
    const wl = db.all('SELECT * FROM waitlist WHERE contact_id = ?', ct.id);
    page(c, ct.email, `<p>${e(ct.name || '')} ${ct.org ? '· ' + e(ct.org) : ''} · ${e(ct.kind)} · via ${e(ct.source || '')} · since ${day(ct.created_at)} · marketing: ${ct.consent_marketing && !ct.unsubscribed_at ? `<span class="ok">consented ${day(ct.consent_at)}</span>` : ct.unsubscribed_at ? `<span class="bad">unsubscribed ${day(ct.unsubscribed_at)}</span>` : 'no'} ${!ct.consent_marketing ? button(`/contacts/${ct.id}/consent`, c.csrf, 'Send double opt-in mail', { quiet: true }) : ''}</p>
<h2>Orders</h2>${table(orders, [{ label: '#', html: r => link(`/orders/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Total', get: r => money(r.total_cents), num: true }, { label: 'Paid', get: r => day(r.paid_at) }])}
<h2>Tickets</h2>${table(tickets, [{ label: '#', html: r => link(`/tickets/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Subject', key: 'subject' }])}
<h2>Waitlist</h2>${table(wl, [{ label: 'SKU', key: 'sku' }, { label: 'Qty', key: 'qty' }, { label: 'Notified', get: r => dt(r.notified_at) }])}
${form(`/contacts/${ct.id}/notes`, c.csrf, field('notes', 'Notes', { type: 'textarea', value: ct.notes || '' }), { submit: 'Save notes' })}`);
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

  // ---------- mail & campaigns
  router.get('/campaigns', (c) => page(c, 'Mail', `<p>Outbox: ${Object.entries(outboxStats(db)).map(([k, v]) => `${e(k)} <b>${v}</b>`).join(' · ') || 'empty'} · ${button('/campaigns/flush', c.csrf, 'Send queued now', { quiet: true })} · ${link('/campaigns/outbox', 'recent mail')}</p>` +
    table(listCampaigns(db), [{ label: 'Slug', key: 'slug' }, { label: 'Subject', key: 'subject' }, { label: 'Segment', key: 'segment' }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Recipients', key: 'recipients', num: true }, { label: '', html: r => r.status === 'draft' ? `<form method="post" action="/campaigns/${e(r.slug)}/send" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><button>Send to segment</button></form>` : day(r.sent_at) }]) +
    form('/campaigns/new', c.csrf, `<b>New campaign</b><p class="muted">Only double-opt-in contacts receive it; every mail carries one-click unsubscribe. {{name}} and {{unsubscribe}} are filled in.</p><div class="row">${field('slug', 'Slug', { required: true })}${field('subject', 'Subject', { required: true })}${field('segment', 'Segment', { value: 'all', placeholder: 'all | customers | waitlist:<sku> | kind:<kind>' })}</div>${field('body_text', 'Body (plain text)', { type: 'textarea', required: true, rows: 12 })}`, { submit: 'Save draft' })));
  router.post('/campaigns/new', (c) => { createCampaign(db, c.body); back(c, '/campaigns', 'draft saved'); });
  router.post('/campaigns/:slug/send', (c) => { const n = queueCampaign(db, c.params.slug, cfg); back(c, '/campaigns', `queued for ${n} people`); });
  router.post('/campaigns/flush', async (c) => { const r = await runJob(app, 'outbox'); back(c, '/campaigns', JSON.stringify(r.result || r.error)); });
  router.get('/campaigns/outbox', (c) => page(c, 'Recent mail', table(db.all('SELECT * FROM mail_outbox ORDER BY id DESC LIMIT 200'), [{ label: 'When', get: r => dt(r.created_at) }, { label: 'To', key: 'to_addr' }, { label: 'Subject', key: 'subject' }, { label: 'Kind', key: 'kind' }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Error', key: 'last_error' }])));

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
<h2>API tokens</h2>${table(db.all('SELECT name, created_at, last_used_at FROM api_tokens'), [{ label: 'Name', key: 'name' }, { label: 'Created', get: r => dt(r.created_at) }, { label: 'Last used', get: r => dt(r.last_used_at) }, { label: '', html: r => `<form method="post" action="/settings/token/revoke" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="name" value="${e(r.name)}"><button class="quiet">revoke</button></form>` }])}
${form('/settings/token', c.csrf, field('name', 'New token name', { required: true }), { submit: 'Create token' })}
<h2>Jobs</h2>${table(Object.keys(JOBS).map(n => ({ name: n, every: JOBS[n][0], ...(jobRuns(db).find(r => r.name === n) || {}) })), [{ label: 'Job', key: 'name' }, { label: 'Every', get: r => r.every + ' s' }, { label: 'Last run', get: r => dt(r.last_run_at) }, { label: 'OK', html: r => r.last_ok == null ? '—' : r.last_ok ? '<span class="ok">yes</span>' : '<span class="bad">no</span>' }, { label: 'Detail', key: 'last_detail' }, { label: '', html: r => `<form method="post" action="/settings/jobs/run" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="name" value="${e(r.name)}"><button class="quiet">run</button></form>` }])}
<h2>Backups</h2>${table(listBackups(join(cfg.dataDir, 'backups')), [{ label: 'File', key: 'name' }, { label: 'Bytes', key: 'bytes', num: true }])}${button('/settings/backup', c.csrf, 'Back up now', { quiet: true })}
<h2>Audit log (last 100)</h2>${table(db.all('SELECT * FROM audit_log ORDER BY id DESC LIMIT 100'), [{ label: 'When', get: r => dt(r.at) }, { label: 'Actor', key: 'actor' }, { label: 'Action', key: 'action' }, { label: 'Entity', get: r => `${r.entity || ''} ${r.entity_id || ''}` }, { label: 'Detail', key: 'detail' }])}`));
  router.post('/settings/token', (c) => { const t = createApiToken(db, c.body.name); html(c.res, layout({ title: 'Token created', path: '/settings', company: cfg.company, csrf: c.csrf, body: `<p>Copy it now; it is not shown again.</p><pre>${e(t)}</pre><p>Use: <code>Authorization: Bearer …</code> against ${e(cfg.baseUrl)}/api</p><p>${link('/settings', 'Back')}</p>` })); });
  router.post('/settings/token/revoke', (c) => { revokeApiToken(db, c.body.name); back(c, '/settings', 'revoked'); });
  router.post('/settings/jobs/run', async (c) => { const r = await runJob(app, c.body.name); back(c, '/settings', `${r.job}: ${r.ok ? JSON.stringify(r.result) : r.error}`); });
  router.post('/settings/backup', (c) => { const r = backup(db, join(cfg.dataDir, 'backups'), { keep: cfg.backupKeep }); back(c, '/settings', `wrote ${r.path}`); });
}
