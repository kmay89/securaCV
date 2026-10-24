// The loop closes: Marketing in one view, purchase orders that feed
// "buildable", recalls by batch or firmware, ticket attachments, the right to
// be forgotten, and the Decide queue on Home — the system drafts, a person
// approves. Same conventions: server-rendered, plain forms, no script.
import { text, json, HttpError, escapeHtml as e, int } from './http.mjs';
import { table, money, dt, day, statusPill, field, form, button, cards, tabs, meta } from './render.mjs';
import { tile, hbars } from './charts.mjs';
import { stateTileMap } from './statemap.mjs';
import { marketing } from './modules/marketing.mjs';
import { suggestions } from './modules/suggestions.mjs';
import { listPos, poById, createPo, markOrdered, cancelPo, receivePo, draftReorder, STATUSES as PO_STATUSES } from './modules/purchasing.mjs';
import { recallQuery, batches as deviceBatches, DEFAULT_BODY, createRecall, recallById, listRecalls, sendRecall } from './modules/recall.mjs';
import { attachment } from './modules/support.mjs';
import { forgetContact, exportContact } from './modules/privacy.mjs';
import { contactById } from './modules/contacts.mjs';
import { remindInvoice } from './modules/invoices.mjs';
import { editItems } from './modules/orders.mjs';
import { trackingConfigured, pollTracking } from './modules/shipping.mjs';
import { orderToken } from './modules/delivery.mjs';
import { listItems } from './modules/inventory.mjs';

const KIND_LABEL = { purchasing: 'Purchasing', sales: 'Sales', support: 'Support', orders: 'Orders', books: 'Books', mail: 'Mail' };

// The Decide queue as cards: what the system would do, why, one button to let it — and a link to look first.
export function decideCards(db, cfg, csrf) {
  const s = suggestions(db, cfg); if (!s.length) return '';
  const card = (x) => `<div class="dc"><div class="dk">${e(KIND_LABEL[x.kind] || x.kind)}</div><div class="dt">${e(x.title)}</div><div class="dd">${e(x.detail)}</div><div class="dw">${e(x.why)}</div><div class="da">${x.action ? `<form method="post" action="${e(x.action.href)}" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}">${Object.entries(x.action.fields || {}).map(([k, v]) => `<input type="hidden" name="${e(k)}" value="${e(v)}">`).join('')}<button>${e(x.action.label)}</button></form>` : ''}${x.link ? `<a href="${e(x.link)}" class="btn quiet">Look first</a>` : ''}</div></div>`;
  return `<h2 style="margin-top:.2rem">Decide <span class="muted" style="font-weight:400;font-size:13px">· ${s.length} drafted by the system, each waiting for a person</span></h2><div class="decide">${s.map(card).join('')}</div>`;
}

export function registerMore(app, { page, back, link, filters, backTo }) {
  const { db, cfg, router } = app;
  const pct = (x) => `${Math.round((x || 0) * 100)}%`;

  // ---------- marketing: the funnel, where people come from, what each send did, where the customers are
  router.get('/marketing', (c) => {
    const days = Number(c.query.get('days')) || 30; const m = marketing(db, { days }); const f = m.funnel, p = f.period;
    const funnel = `<div class="kpis">${tile('People we know', f.contacts, { prev: null })}${tile('Opted in to news', `${f.opted_in} <span class="muted" style="font-size:.8rem">${pct(f.contacts ? f.opted_in / f.contacts : 0)}</span>`)}${tile('Bought once', `${f.customers} <span class="muted" style="font-size:.8rem">${pct(f.contacts ? f.customers / f.contacts : 0)}</span>`)}${tile('Bought again', `${f.repeat} <span class="muted" style="font-size:.8rem">${pct(f.customers ? f.repeat / f.customers : 0)}</span>`)}${tile(`Attributed sales`, money(m.attributed_cents))}</div>
<p class="muted">In the last ${days} days: ${p.contacts} new people, ${p.opted_in} opt-ins, ${p.first_orders} first orders, ${p.repeat_orders} repeat orders. ${link('/marketing?days=7', '7')} · ${link('/marketing?days=30', '30')} · ${link('/marketing?days=90', '90')} · ${link('/marketing?days=365', '365')} days</p>`;
    const sources = hbars({ rows: m.sources.map(s => ({ label: s.source, value: s.contacts, extra: `${s.opted_in} opted in · ${s.customers} bought` })), title: 'Where people come from', color: 'var(--s2)' });
    const maps = `<div class="two">${stateTileMap(m.byState, { title: 'Customers by state', fmt: (v) => String(v), sub: Object.fromEntries(Object.entries(m.ordersByState).map(([k, v]) => [k, `${v} order${v === 1 ? '' : 's'}`])) })}${stateTileMap(m.salesByState, { title: 'Sales by state', fmt: (v) => money(v) })}</div>`;
    const cities = table(m.cities, [{ label: 'Place', key: 'place' }, { label: 'Customers', key: 'customers', num: true }, { label: 'Sales', get: r => money(r.sales), num: true }], { empty: 'No shipped orders yet.' });
    const camps = table(m.campaigns, [{ label: 'Campaign', html: r => link(`/campaigns/${encodeURIComponent(r.slug)}`, r.subject) }, { label: 'Sent', get: r => day(r.sent_at) }, { label: 'To', get: r => r.report.sent || 0, num: true }, { label: 'Replies', get: r => r.report.replies, num: true }, { label: 'Orders, 7 d', get: r => r.report.orders, num: true }, { label: 'Revenue', get: r => money(r.report.revenue_cents), num: true }, { label: 'Unsubscribed', get: r => r.report.unsubscribed, num: true }], { empty: 'Nothing sent yet.' });
    const autos = table(m.automations, [{ label: 'Automation', html: r => link('/campaigns/automations', r.name) }, { label: 'On', html: r => r.enabled ? '<span class="ok">yes</span>' : '<span class="muted">paused</span>' }, { label: 'Sent', get: r => r.report.sent, num: true }, { label: 'Replies', get: r => r.report.replies, num: true }, { label: 'Orders, 7 d', get: r => r.report.orders, num: true }, { label: 'Revenue', get: r => money(r.report.revenue_cents), num: true }], { empty: 'No automations.' });
    const codes = table(m.codes, [{ label: 'Code', html: r => link(`/orders?discount=${encodeURIComponent(r.code)}`, r.code) }, { label: 'Uses', key: 'uses', num: true }, { label: 'Given away', get: r => money(r.given_cents || 0), num: true }, { label: 'Sales with it', get: r => money(r.sales_cents || 0), num: true }], { empty: 'No code has been used.' });
    const a = m.audience;
    page(c, 'Marketing', `${funnel}<div class="two">${sources}<div class="card viz"><div class="k">The audience, ${days} days</div><table style="box-shadow:none"><tr><td>Subscribers (opted in, not bounced)</td><td class="num"><b>${a.subscribers}</b></td></tr><tr><td>Joined in the period</td><td class="num">${a.joined}</td></tr><tr><td>Left in the period</td><td class="num ${a.left ? 'warn' : ''}">${a.left}</td></tr><tr><td>Bounced, suppressed</td><td class="num ${a.bounced ? 'warn' : ''}">${a.bounced}</td></tr><tr><td>Awaiting double opt-in</td><td class="num">${a.pending}</td></tr></table><p class="muted">${link('/campaigns/audience', 'Segments and counts')} · ${link('/campaigns', 'Campaigns')} · ${link('/campaigns/automations', 'Automations')}</p></div></div>
${maps}
<div class="two"><div><h2>Where the customers are</h2>${cities}</div><div><h2>Discount codes</h2>${codes}</div></div>
<h2>What each campaign did <span class="muted" style="font-weight:400;font-size:13px">· orders and replies from recipients in the seven days after a send; no pixels, no click tracking</span></h2>${camps}
<h2>What each automation did</h2>${autos}`);
  });
  router.get('/api/marketing', (c) => json(c.res, marketing(db, { days: Number(c.query.get('days')) || 30 })));
  router.get('/api/suggestions', (c) => json(c.res, suggestions(db, cfg)));

  // ---------- purchasing: what is on order, from whom, arriving when
  const poLines = (s) => String(s || '').split('\n').map(l => l.split('|').map(x => x.trim())).filter(l => l[0]).map(([sku, qty, cents]) => ({ sku, qty: int(qty, { name: `${sku} qty`, min: 1, fallback: 1 }), unit_cost_cents: cents === undefined || cents === '' ? undefined : int(cents, { name: `${sku} unit cents`, min: 0 }) }));
  router.get('/purchasing', (c) => {
    const st = c.query.get('status') || ''; const rows = listPos(db, { status: st }); const all = listPos(db);
    const n = Object.fromEntries(PO_STATUSES.map(s => [s, all.filter(r => r.status === s).length]));
    const items = listItems(db);
    page(c, 'Purchasing', tabs([['/purchasing', 'All', all.length, ''], ...PO_STATUSES.map(s => [`/purchasing?status=${s}`, s[0].toUpperCase() + s.slice(1), n[s], s])], st) +
      `<p>${button('/purchasing/draft', c.csrf, 'Draft from the reorder points', { quiet: true })} <span class="muted">every part at or under its reorder point, grouped by supplier, at the reorder quantity — nothing is ordered until you say so</span></p>` +
      table(rows, [{ label: '#', html: r => link(`/purchasing/${r.id}`, r.number) }, { label: 'Status', html: r => statusPill(r.status) }, { label: 'Supplier', html: r => r.supplier_url ? `<a href="${e(r.supplier_url)}" rel="noopener noreferrer">${e(r.supplier)}</a>` : e(r.supplier) }, { label: 'Lines', key: 'items_text' }, { label: 'Total', get: r => money(r.total_cents), num: true }, { label: 'Ordered', get: r => day(r.ordered_at) }, { label: 'Expected', get: r => r.expected_at || '—' }, { label: 'Received', get: r => day(r.received_at) }], { empty: 'No purchase orders yet.' }) +
      form('/purchasing/new', c.csrf, `<b>New purchase order</b><div class="row">${field('supplier', 'Supplier', { required: true })}${field('supplier_url', 'Supplier URL')}${field('expected_at', 'Expected (YYYY-MM-DD)', { type: 'date' })}</div>${field('lines', 'Lines, one per row: sku | qty | unit cents (blank = last cost)', { type: 'textarea', rows: 4, required: true, placeholder: items.slice(0, 2).map(i => `${i.sku} | ${i.reorder_qty || 10} | ${i.unit_cost_cents}`).join('\n') })}${field('notes', 'Notes')}`, { submit: 'Create draft' }));
  });
  router.post('/purchasing/new', (c) => { const po = createPo(db, { supplier: c.body.supplier, supplier_url: c.body.supplier_url || null, expected_at: c.body.expected_at || null, notes: c.body.notes || null, items: poLines(c.body.lines), actor: 'web' }); back(c, `/purchasing/${po.id}`, `drafted ${po.number}`); });
  router.post('/purchasing/draft', (c) => { const pos = draftReorder(db, { actor: 'web' }); backTo(c, pos.length === 1 ? `/purchasing/${pos[0].id}` : '/purchasing', pos.length ? `drafted ${pos.map(p => `${p.number} (${p.supplier})`).join(', ')}` : 'nothing to draft — every low part is already drafted or on order'); });
  router.get('/purchasing/:id', (c) => {
    const p = poById(db, c.params.id); if (!p) throw new HttpError(404, 'no such purchase order');
    const lines = table(p.items, [{ label: 'SKU', key: 'sku' }, { label: 'Part', key: 'name' }, { label: 'Qty', key: 'qty', num: true }, { label: 'Received', html: r => `<span class="${r.received_qty >= r.qty ? 'ok' : r.received_qty ? 'warn' : 'muted'}">${r.received_qty}</span>`, num: true }, { label: 'Unit cost', get: r => money(r.unit_cost_cents), num: true }, { label: 'Line', get: r => money(r.unit_cost_cents * r.qty), num: true }]);
    const acts = [];
    if (p.status === 'draft') acts.push(form(`/purchasing/${p.id}/order`, c.csrf, `<b>Mark ordered</b><p class="muted">You placed it with the supplier (their site, an email, a phone call); this records it so "buildable" can look ahead.</p>${field('expected_at', 'Expected (YYYY-MM-DD)', { type: 'date', value: p.expected_at || '' })}`, { submit: 'Mark ordered' }));
    if (['ordered', 'draft'].includes(p.status)) acts.push(form(`/purchasing/${p.id}/receive`, c.csrf, `<b>Receive</b><p class="muted">Each line moves into stock (ref <code>po:${e(p.number)}</code>) and its value is booked as inventory. Leave a line at its remaining quantity for a full receipt; lower it for a partial one.</p><div class="row">${p.items.map(i => field(`recv_${i.sku}`, `${i.sku} (${i.qty - i.received_qty} open)`, { type: 'number', value: i.qty - i.received_qty })).join('')}</div>${field('paidFrom', 'Paid from', { options: [['assets:bank', 'bank'], ['equity:owner', 'owner (personal card)']] })}`, { submit: 'Receive into stock' }));
    if (p.status !== 'received' && p.status !== 'canceled') acts.push(button(`/purchasing/${p.id}/cancel`, c.csrf, 'Cancel this PO', { quiet: true }));
    page(c, `${p.number} · ${p.supplier}`, meta([['Status', statusPill(p.status)], ['Supplier', p.supplier_url ? `<a href="${e(p.supplier_url)}" rel="noopener noreferrer">${e(p.supplier)}</a>` : e(p.supplier)], ['Total', money(p.total_cents)], ['Created', dt(p.created_at)], ['Ordered', dt(p.ordered_at)], ['Expected', e(p.expected_at || '—')], ['Received', dt(p.received_at)]]) + `${p.notes ? `<p class="muted">${e(p.notes)}</p>` : ''}${lines}${acts.join('')}<p>${link('/purchasing', 'All purchase orders')} · ${link('/inventory', 'Inventory')}</p>`);
  });
  router.post('/purchasing/:id/order', (c) => { const p = markOrdered(db, Number(c.params.id), { expected_at: c.body.expected_at || null, actor: 'web' }); backTo(c, `/purchasing/${p.id}`, `${p.number} is ordered${p.expected_at ? `, expected ${p.expected_at}` : ''}`); });
  router.post('/purchasing/:id/receive', (c) => { const received = Object.fromEntries(Object.entries(c.body).filter(([k]) => k.startsWith('recv_')).map(([k, v]) => [k.slice(5), int(v, { name: `${k.slice(5)} received`, min: 0, fallback: 0 })])); const p = receivePo(db, Number(c.params.id), { received, paidFrom: c.body.paidFrom || 'assets:bank', actor: 'web' }); backTo(c, `/purchasing/${p.id}`, `${p.number} ${p.status === 'received' ? 'received in full' : 'partly received'} — stock moved, inventory booked`); });
  router.post('/purchasing/:id/cancel', (c) => { const p = cancelPo(db, Number(c.params.id), { actor: 'web' }); back(c, `/purchasing/${p.id}`, `${p.number} canceled`); });
  router.get('/api/purchasing', (c) => json(c.res, listPos(db, { status: c.query.get('status') || '' })));
  router.post('/api/purchasing', (c) => json(c.res, createPo(db, { ...c.body, actor: c.actor }), 201));
  router.post('/api/purchasing/draft', (c) => json(c.res, draftReorder(db, { skus: c.body.skus || null, actor: c.actor }), 201));
  router.post('/api/purchasing/:number/:action', (c) => { const p = db.get('SELECT id FROM purchase_orders WHERE number = ?', c.params.number); if (!p) throw new HttpError(404, 'no such purchase order'); const fn = { order: () => markOrdered(db, p.id, { expected_at: c.body.expected_at || null, actor: c.actor }), receive: () => receivePo(db, p.id, { received: c.body.received || null, actor: c.actor }), cancel: () => cancelPo(db, p.id, { actor: c.actor }) }[c.params.action]; if (!fn) throw new HttpError(404, 'action is order, receive or cancel'); json(c.res, fn()); });

  // ---------- recalls: who has what, then one honest notice each
  router.get('/devices/recall', (c) => {
    const f = { batch: c.query.get('batch') || '', fw: c.query.get('fw') || '', sku: c.query.get('sku') || '' }; const asked = !!(f.batch || f.fw || f.sku);
    const q = asked ? recallQuery(db, f) : null;
    const groups = table(deviceBatches(db), [{ label: 'Batch', html: r => link(`/devices/recall?batch=${encodeURIComponent(r.batch || '')}`, r.batch || '(none)') }, { label: 'Firmware', html: r => link(`/devices/recall?fw=${encodeURIComponent(r.fw_version || '')}`, r.fw_version || '—') }, { label: 'SKU', key: 'product_sku' }, { label: 'Boards', key: 'n', num: true }, { label: 'Shipped', key: 'shipped', num: true }], { empty: 'No devices registered.' });
    const preview = q ? `<h2>Who has them</h2>${cards([['Boards', q.devices.length], ['Shipped', q.shipped], ['Still on the shelf', q.in_stock], ['Customers to notify', q.customers.length, q.customers.length ? 'warn' : '']])}${table(q.customers, [{ label: 'Customer', html: r => link(`/contacts/${r.contact_id}`, r.name || r.email) }, { label: 'Email', key: 'email' }, { label: 'Serials', get: r => r.serials.join(', ') }, { label: 'Orders', get: r => r.orders.join(', ') }], { empty: 'None of these boards reached a customer.' })}
${q.customers.length ? form('/devices/recall', c.csrf, `<b>Write the notice</b><input type="hidden" name="batch" value="${e(f.batch)}"><input type="hidden" name="fw" value="${e(f.fw)}"><input type="hidden" name="sku" value="${e(f.sku)}">${field('title', 'Subject', { required: true, value: `A notice about your Canary board${f.batch ? ` (batch ${f.batch})` : f.fw ? ` (firmware ${f.fw})` : ''}` })}${field('body', 'Body ({{first_name}}, {{serials}}, {{plural}}, {{orders}}, {{company}} are filled in per customer)', { type: 'textarea', rows: 12, value: DEFAULT_BODY })}<p class="muted">Creating it sends nothing. The next page shows exactly who gets it; sending is a second, separate button.</p>`, { submit: 'Create the notice (nothing is sent yet)' }) : ''}` : '';
    page(c, 'Recall / safety notice', `<p class="muted">Pick a batch, a firmware version or a SKU; see every board, where it went, and who to tell. A notice goes out as transactional mail, one per customer with their serials filled in — it is about something they own, so consent to marketing is not the question.</p>
<form method="get" class="row" style="margin:.6rem 0">${field('batch', 'Batch', { value: f.batch })}${field('fw', 'Firmware version', { value: f.fw })}${field('sku', 'SKU', { value: f.sku })}<div><label>&nbsp;</label><button class="quiet">Look up</button></div></form>
<h2>Batches</h2>${groups}${preview}
<h2>Notices</h2>${table(listRecalls(db), [{ label: '#', html: r => link(`/devices/recall/${r.id}`, r.number) }, { label: 'Title', key: 'title' }, { label: 'Scope', get: r => Object.entries(r.filter).filter(([, v]) => v).map(([k, v]) => `${k} ${v}`).join(', ') || 'all' }, { label: 'Boards', key: 'devices', num: true }, { label: 'Customers', key: 'customers', num: true }, { label: 'Created', get: r => dt(r.created_at) }, { label: 'Sent', html: r => r.sent_at ? `<span class="ok">${e(dt(r.sent_at))}</span>` : '<span class="pill warn">not sent</span>' }], { empty: 'No notice has ever been needed.' })}`);
  });
  router.post('/devices/recall', (c) => { const r = createRecall(db, { title: c.body.title, body: c.body.body, filter: { batch: c.body.batch || '', fw: c.body.fw || '', sku: c.body.sku || '' }, actor: 'web' }); back(c, `/devices/recall/${r.id}`, `${r.number} created — nothing sent yet`); });
  router.get('/devices/recall/:id', (c) => {
    const r = recallById(db, c.params.id); if (!r) throw new HttpError(404, 'no such notice'); const q = recallQuery(db, r.filter);
    const sample = q.customers[0]; const first = sample ? (sample.name || 'there').split(' ')[0] : 'there';
    const filled = r.body.replace(/\{\{first_name\}\}/g, first).replace(/\{\{name\}\}/g, sample?.name || 'there').replace(/\{\{serials\}\}/g, sample ? sample.serials.join(', ') : 'DEMO-…').replace(/\{\{plural\}\}/g, sample && sample.serials.length === 1 ? '' : 's').replace(/\{\{orders\}\}/g, sample ? sample.orders.join(', ') : '').replace(/\{\{company\}\}/g, cfg.company || '');
    page(c, `${r.number} · ${r.title}`, `<p>${r.sent_at ? `<span class="pill ok">sent ${e(dt(r.sent_at))}</span>` : '<span class="pill warn">not sent</span>'} · scope ${e(Object.entries(r.filter).filter(([, v]) => v).map(([k, v]) => `${k} ${v}`).join(', ') || 'every board')} · ${q.devices.length} boards · ${q.customers.length} customers · created ${dt(r.created_at)}</p>
<div class="two"><div><h2>As ${e(sample?.name || 'a customer')} would read it</h2><div class="msg out"><div class="meta">Subject: ${e(r.title)}</div>${e(filled)}</div></div>
<div><h2>Who gets it</h2>${table(q.customers, [{ label: 'Customer', html: x => link(`/contacts/${x.contact_id}`, x.name || x.email) }, { label: 'Serials', get: x => x.serials.join(', ') }, { label: 'Orders', get: x => x.orders.join(', ') }], { empty: 'Nobody — none of these boards reached a customer.' })}
${!r.sent_at && q.customers.length ? `<form method="post" action="/devices/recall/${r.id}/send" class="box"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><b>Send to ${q.customers.length} customer${q.customers.length === 1 ? '' : 's'}</b><p class="muted">One transactional mail each, serials filled in, noted on every order's timeline. This cannot be unsent.</p><label><input type="checkbox" name="sure" value="1" style="width:auto" required> I have read the notice above and it is ready</label><div style="margin-top:.8rem"><button>Send the notice</button></div></form>` : ''}</div></div>
<p>${link('/devices/recall', 'All notices')} · ${link('/devices', 'Devices')}</p>`);
  });
  router.post('/devices/recall/:id/send', (c) => { if (!c.body.sure) throw new HttpError(400, 'tick the box first'); const r = sendRecall(db, Number(c.params.id), cfg, { actor: 'web' }); back(c, `/devices/recall/${r.id}`, `${r.number} queued to ${r.queued} customer${r.queued === 1 ? '' : 's'}`); });
  router.get('/api/recall', (c) => json(c.res, { query: recallQuery(db, { batch: c.query.get('batch') || '', fw: c.query.get('fw') || '', sku: c.query.get('sku') || '' }), notices: listRecalls(db) }));
  router.post('/api/recall', (c) => json(c.res, createRecall(db, { title: c.body.title, body: c.body.body || DEFAULT_BODY, filter: { batch: c.body.batch || '', fw: c.body.fw || '', sku: c.body.sku || '' }, actor: c.actor }), 201));
  router.post('/api/recall/:number/send', (c) => { const r = db.get('SELECT id FROM recalls WHERE number = ?', c.params.number); if (!r) throw new HttpError(404, 'no such notice'); json(c.res, sendRecall(db, r.id, cfg, { actor: c.actor })); });

  // ---------- ticket attachments: served same-origin, never executed
  router.get('/tickets/:id/att/:aid', (c) => {
    const a = attachment(db, c.params.aid); if (!a || String(a.ticket_id) !== String(c.params.id)) throw new HttpError(404, 'no such attachment');
    const inline = /^image\//.test(a.mime); const name = a.filename.replace(/[^\w. -]/g, '_').slice(0, 80);
    c.res.setHeader('Content-Security-Policy', "default-src 'none'; sandbox");
    text(c.res, a.bytes, 200, { 'Content-Type': a.mime, 'Content-Disposition': `${inline ? 'inline' : 'attachment'}; filename="${name}"`, 'Cache-Control': 'private, no-store' });
  });

  // ---------- the right to be forgotten, the right to a copy
  router.post('/contacts/:id/forget', (c) => { if (c.body.sure !== 'forget') throw new HttpError(400, 'type forget to confirm'); const r = forgetContact(db, Number(c.params.id), { actor: 'web' }); back(c, `/contacts/${c.params.id}`, r.already ? 'already forgotten' : `forgotten: ${r.orders} orders, ${r.tickets} tickets, ${r.messages} messages, ${r.attachments} attachments, ${r.mails} mails blanked`); });
  router.get('/contacts/:id/export.json', (c) => { const ct = contactById(db, c.params.id); if (!ct) throw new HttpError(404, 'no such contact'); json(c.res, exportContact(db, ct.id), 200, { 'Content-Disposition': `attachment; filename="contact-${ct.id}.json"` }); });
  router.post('/api/customers/:id/forget', (c) => json(c.res, forgetContact(db, Number(c.params.id), { actor: c.actor })));
  router.get('/api/customers/:id/export', (c) => json(c.res, exportContact(db, Number(c.params.id))));

  // ---------- an invoice reminder, an order's lines, a carrier poll
  router.post('/invoices/:id/remind', (c) => { const inv = remindInvoice(db, Number(c.params.id), cfg, { actor: 'web' }); backTo(c, `/invoices/${inv.id}`, `reminder queued for ${inv.number}`); });
  router.post('/api/invoices/:number/remind', (c) => { const inv = db.get('SELECT id FROM invoices WHERE number = ?', c.params.number); if (!inv) throw new HttpError(404, 'no such invoice'); json(c.res, remindInvoice(db, inv.id, cfg, { actor: c.actor })); });
  router.post('/orders/:id/items', async (c) => {
    const items = String(c.body.lines || '').split('\n').map(l => l.split('|').map(x => x.trim())).filter(l => l[0]).map(([sku, qty]) => ({ sku, qty: int(qty, { name: `${sku} qty`, min: 1, fallback: 1 }) }));
    const o = editItems(db, Number(c.params.id), items, { catalog: await app.catalog().catch(() => null), actor: 'web' }); back(c, `/orders/${o.id}`, `items on ${o.number} changed`);
  });
  router.post('/orders/track', async (c) => { if (!trackingConfigured(cfg)) throw new HttpError(400, 'set USPS_CLIENT_ID and USPS_CLIENT_SECRET first (Settings explains)'); const r = await pollTracking(db, cfg, { fetchImpl: app.fetchImpl || fetch, minAgeH: 0 }); backTo(c, '/ship', `checked ${r.checked} parcel${r.checked === 1 ? '' : 's'}: ${r.delivered} delivered, ${r.alerts} alert${r.alerts === 1 ? '' : 's'}`); });
  router.get('/api/orders/:number/status-url', (c) => { const o = db.get('SELECT * FROM orders WHERE number = ?', c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, { url: `${cfg.baseUrl}/o/${o.number}/${orderToken(cfg.secret, o.number)}` }); });
}
