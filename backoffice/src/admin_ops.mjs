// The operating half: the Pack and Ship benches (one order at a time, the
// weekly batch as a flow), the Health page (the doctor's checklist with the
// buttons that prove things), and the Audit log (every action, filterable,
// exportable). Same conventions: server-rendered, plain forms, no script.
import { text, HttpError, escapeHtml as e, csvLine } from './http.mjs';
import { table, money, dt, day, statusPill, field, form, button } from './render.mjs';
import { listOrders, orderById, weekLabel, statusCounts, addressIssues, packPlanOf, boardsNeeded, boardsOn } from './modules/orders.mjs';
import { packOrder } from './modules/packing.mjs';
import { doctor } from './modules/health.mjs';
import { backup, checkBackup, liveCounts, listBackups } from './modules/backup.mjs';
import { runJob, jobRuns } from './jobs.mjs';
import { join } from 'node:path';

export function registerOps(app, { page, back, link, filters }) {
  const { db, cfg, router } = app;

  // ---------- the pack bench: the next order to pack, its pick list, the serial scan, one button
  const nextIn = (status, batch, after) => {
    const rows = listOrders(db, { status, batch, limit: 500 }).reverse().filter(o => !after || o.id > after);
    return rows[0] || null;
  };
  const batchFor = (status, wanted) => { if (wanted) return wanted; const cur = weekLabel(); if (listOrders(db, { status, batch: cur, limit: 1 }).length) return cur; const any = listOrders(db, { status, limit: 1 }); return any.length ? any[any.length - 1].batch : cur; };
  const progress = (status, batch, doneStatuses) => { const all = listOrders(db, { batch, limit: 1000 }).filter(o => !['canceled', 'refunded'].includes(o.status)); const done = all.filter(o => doneStatuses.includes(o.status)).length; return { total: all.length, done, left: all.filter(o => o.status === status).length }; };
  const addrWarn = (o) => { const bad = addressIssues({ line1: o.ship_line1, city: o.ship_city, state: o.ship_state, zip: o.ship_zip, country: o.ship_country }); return bad.length ? `<p class="flash err" style="margin:.4rem 0"><b>This address will not make a label:</b> ${e(bad.join('; '))} — <a href="/orders/${o.id}">fix it on the order</a> first.</p>` : ''; };
  // One line for the bench: the carton(s), the expected weight, the postage estimate, and any warning.
  const packLine = (plan, atShip = false) => { if (!plan) return ''; const boxes = (plan.boxes || []).map(b => `<b>${e(b.box_name.replace(/ \(.*\)$/, ''))}</b> ${b.weight_oz} oz`).join(' + '); const warn = (plan.warnings || []).map(w => `<br><span class="warn">${e(w)}</span>`).join(''); return `<p class="muted" style="margin:.4rem 0">${boxes ? `${atShip ? 'Packed as' : 'Pack in'}: ${boxes}${plan.total_cost_est != null ? ` · postage ≈ ${money(Math.round(plan.total_cost_est * 100))}` : ''}${atShip && plan.total_oz != null ? ` · weigh it: within ${cfg.weightToleranceOz ?? 1.5} oz of ${plan.total_oz} oz, or recount` : ''}` : 'No box plan.'}${warn}</p>`; };
  const picklist = (o) => `<ul class="picklist">${o.items.map(i => `<li><span class="box" aria-hidden="true"></span><span class="q">${i.qty}×</span><span>${e(i.name)}<br><span class="sku">${e(i.sku)}</span></span></li>`).join('') || '<li class="muted">No line items — add them on the order before packing.</li>'}</ul>`;

  // The box card: the carton by name and inner size, what goes in it, and the weight it should read —
  // big enough to match against the stack by eye and against the scale without arithmetic.
  const boxCards = (plan) => (plan?.boxes || []).map((b, i) => `<div class="boxcard"><div><div class="nm">${plan.boxes.length > 1 ? `Box ${i + 1} of ${plan.boxes.length}: ` : ''}${e(b.box_name.replace(/ \(.*\)$/, ''))}</div><div class="muted">inside ${b.inner.l} × ${b.inner.w} × ${b.inner.h} in · ${b.items.length} piece${b.items.length === 1 ? '' : 's'}${b.cost_est != null ? ` · postage ≈ ${money(Math.round(b.cost_est * 100))}` : ''}</div></div><div class="oz">${b.weight_oz} <small>oz</small></div></div>`).join('') + (plan?.warnings || []).map(w => `<p class="warn" style="margin:.3rem 0">${e(w)}</p>`).join('');

  router.get('/pack', async (c) => {
    // `?order=` opens one order by choice (the order page sends you here); otherwise the next unpacked in the batch.
    const wanted = Number(c.query.get('order') || 0); const chosen = wanted ? orderById(db, wanted) : null;
    if (wanted && !chosen) throw new HttpError(404, 'no such order');
    const batch = chosen ? chosen.batch : batchFor('paid', c.query.get('batch') || ''); const after = Number(c.query.get('after') || 0);
    const o = chosen ? (chosen.status === 'paid' ? chosen : null) : nextIn('paid', batch, after) ? orderById(db, nextIn('paid', batch, after).id) : null;
    const p = progress('paid', batch, ['packed', 'shipped', 'delivered']);
    const head = `<p class="muted">Batch <b>${e(batch)}</b> · ${p.done} of ${p.total} packed · ${p.left} to go · ${link(`/orders?batch=${e(batch)}`, 'the list')} · ${link(`/orders/packing-slips?batch=${e(batch)}&status=paid`, 'print all slips')}</p><div class="progress"><i style="width:${p.total ? Math.round(100 * p.done / p.total) : 0}%"></i></div>`;
    if (chosen && !o) return page(c, 'Pack bench', head + `<div class="card"><p style="margin:0">${link(`/orders/${chosen.id}`, chosen.number)} is <b>${e(chosen.status)}</b> — nothing to pack. ${link('/pack', 'Next order in the batch →')}</p></div>`);
    if (!o) return page(c, 'Pack bench', head + `<div class="card"><p class="ok" style="font-size:1.1rem;margin:0"><b>Everything in ${e(batch)} is packed.</b></p><p>${link('/ship', 'Open the ship bench →')} ${p.total ? '' : '<span class="muted">(no orders in this batch yet)</span>'}</p>${statusCounts(db).paid ? `<p class="muted">Other batches still have ${statusCounts(db).paid} order${statusCounts(db).paid === 1 ? '' : 's'} to pack: ${link('/orders?status=paid', 'see them')}</p>` : ''}</div>`);
    const here = chosen ? `/pack?order=${o.id}` : `/pack?batch=${encodeURIComponent(batch)}`;
    const plan = packOrder(await app.shipping().catch(() => null), o.items);
    const need = boardsNeeded(db, o); const boards = boardsOn(db, o.id); const boardsDone = !need.needed || boards.length === need.needed;
    const csrf = `<input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="_back" value="${e(here)}">`;
    const picks = o.items.map(i => { const nl = need.lines.find(l => l.sku === i.sku); return `<label class="pick"><input type="checkbox" name="ticked[]" value="${e(i.sku)}"><span class="q">${i.qty}×</span><span>${e(i.name)}<br><span class="sku">${e(i.sku)}${nl ? ` · ${nl.boards} board${nl.boards === 1 ? '' : 's'} inside` : ''}</span></span></label>`; }).join('') || '<p class="muted">No line items — add them on the order before packing.</p>';
    const boardsHtml = `<h2 style="display:flex;justify-content:space-between;align-items:baseline;margin-top:0">Boards in the box <span class="${boardsDone ? 'ok' : 'warn'}" style="font-size:.95rem">${need.needed ? `${boards.length} of ${need.needed} scanned${boardsDone ? ' ✓' : ''}` : boards.length ? `${boards.length} scanned` : 'none needed'}</span></h2>
${boards.length ? `<div class="pills">${boards.map(d => `<form method="post" action="/orders/${o.id}/unassign" class="inline">${csrf}<input type="hidden" name="serial" value="${e(d.serial)}"><span class="pill ok">${e(d.serial)} · fw ${e(d.fw_version || '?')}</span> <button class="quiet x" title="Take ${e(d.serial)} back out">×</button></form>`).join(' ')}</div>` : ''}
${!need.needed || boards.length < need.needed ? `<form method="post" action="/orders/${o.id}/assign">${csrf}<label for="f_serial">Scan the next board's serial</label><div style="display:flex;gap:.5rem;align-items:stretch"><input id="f_serial" name="serial" ${o.items.length ? 'autofocus' : ''} autocomplete="off" placeholder="DEMO-W-0005" style="flex:1;min-width:0"><button class="quiet" style="flex:none">Into the box</button></div></form>` : ''}`;
    const body = head + `<div class="bench"><div class="card"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">Pack this</div><h2 style="margin:.2rem 0 .6rem;font-size:1.5rem">${link(`/orders/${o.id}`, o.number)} <span class="muted" style="font-weight:400;font-size:1.1rem">· ${e(o.ship_name)}</span></h2>
${o.notes || o.tags ? `<p class="warn"><b>Note:</b> ${e(o.notes || '')} ${o.tags ? `<span class="muted">tags: ${e(o.tags)}</span>` : ''}</p>` : ''}
${boxCards(plan)}
<form method="post" action="/orders/${o.id}/transition" class="gate">${csrf}<input type="hidden" name="to" value="packed"><input type="hidden" name="bench" value="1"><input type="checkbox" class="need" disabled hidden ${boardsDone ? 'checked' : ''}>
<h2>Tick each line as it goes in</h2>${picks}
<div class="row" style="margin-top:.6rem"><div><label for="f_measured_oz">Scale (oz)${plan.total_oz != null ? ` — should read about ${plan.total_oz}` : ''}</label><input id="f_measured_oz" name="measured_oz" inputmode="decimal" autocomplete="off" placeholder="${plan.total_oz != null ? plan.total_oz : 'optional'}"></div></div>
<button class="big">Packed — next order →</button>
<details class="anyway-box"><summary class="muted">Something is off but it has to go</summary><label><input type="checkbox" class="anyway" name="pack_anyway" value="1" style="width:auto"> pack anyway</label>${field('reason', 'Why (goes on the timeline)')}</details></form>
<p style="margin:.6rem 0 0"><a class="btn quiet" href="/pack?batch=${encodeURIComponent(batch)}&after=${o.id}">Skip for now</a> <a class="btn quiet" href="/orders/${o.id}/packing-slip">Packing slip</a></p></div>
<div><div class="card"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">Ship to</div><p class="addr">${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)}${o.ship_country !== 'US' ? '<br>' + e(o.ship_country) : ''}</p>${addrWarn(o)}<p class="muted">${e(o.email || '')} · paid ${day(o.paid_at)} · ${money(o.total_cents)}</p></div>
<div class="card" style="margin-top:.9rem">${boardsHtml}</div>
<div class="card" style="margin-top:.9rem"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">How this works</div><p class="muted" style="margin:.3rem 0 0;font-size:13.5px">Pick the box on the card, tick each line as it goes in, scan every board the lines call for, set the box on the scale. The button opens when all of that is done; the server checks the same things, so a wrong count, a missing board or a board for the wrong kit is refused by name. Marking it packed pulls the parts from stock and writes who checked what.</p></div></div></div>`;
    page(c, 'Pack bench', body);
  });

  // ---------- the ship bench: the next packed order, the tracking number, one button (the label comes from the fulfill CSV → PirateShip)
  router.get('/ship', async (c) => {
    const batch = batchFor('packed', c.query.get('batch') || ''); const after = Number(c.query.get('after') || 0);
    const o = nextIn('packed', batch, after) ? orderById(db, nextIn('packed', batch, after).id) : null;
    const p = progress('packed', batch, ['shipped', 'delivered']);
    const head = `<p class="muted">Batch <b>${e(batch)}</b> · ${p.done} of ${p.total} shipped · ${p.left} to go · ${link(`/orders.csv?batch=${e(batch)}&status=packed`, 'fulfill.mjs CSV for the labels')} · ${link(`/orders?batch=${e(batch)}&status=packed`, 'the list')}</p><div class="progress"><i style="width:${p.total ? Math.round(100 * p.done / p.total) : 0}%"></i></div>`;
    if (!o) return page(c, 'Ship bench', head + `<div class="card"><p class="ok" style="font-size:1.1rem;margin:0"><b>Everything packed in ${e(batch)} has shipped.</b></p><p>${statusCounts(db).paid ? link('/pack', `${statusCounts(db).paid} still to pack →`) : link('/', 'Back to Home')}</p></div>`);
    const body = head + `<div class="bench"><div class="card"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">Ship this</div><h2 style="margin:.2rem 0 .6rem">${link(`/orders/${o.id}`, o.number)} <span class="muted" style="font-weight:400">· ${e(o.ship_name)}</span></h2>
<p class="addr">${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)}</p>${addrWarn(o)}
<p class="muted">${o.items.map(i => `${i.qty}× ${e(i.name)}`).join(', ')} · packed ${dt(o.packed_at)}</p>
<form method="post" action="/orders/${o.id}/transition"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="to" value="shipped"><input type="hidden" name="_back" value="/ship?batch=${e(batch)}">${packLine(packPlanOf(o) || packOrder(await app.shipping().catch(() => null), o.items), true)}<div class="row">${field('carrier', 'Carrier', { value: 'USPS' })}<div><label for="f_tracking">Tracking number</label><input id="f_tracking" name="tracking" autofocus autocomplete="off" placeholder="9400 1000 0000 0000 0000 00"></div></div><div class="row">${field('measured_oz', 'Scale (oz)', { type: 'text', placeholder: (packPlanOf(o)?.total_oz ?? '') + '' })}${field('postage_cents', 'Label cost (cents)', { type: 'number' })}</div><label><input type="checkbox" name="ship_anyway" value="1" style="width:auto"> ship anyway if the scale disagrees</label><button class="big" style="margin-top:.6rem">Shipped — email tracking, next →</button></form>
<p style="margin:.6rem 0 0"><a class="btn quiet" href="/ship?batch=${e(batch)}&after=${o.id}">Skip for now</a></p></div>
<div><div class="card"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">How this works</div><p class="muted" style="margin:.3rem 0 0;font-size:13.5px">Buy the batch's labels in one go from the CSV, then paste each tracking number here. Marking shipped mails the customer their tracking, marks the boards in the box as shipped and writes the timeline.</p></div></div></div>`;
    page(c, 'Ship bench', body);
  });

  // ---------- health: the checklist, and the buttons that prove things
  router.get('/health', (c) => {
    const h = doctor(db, cfg); const runs = Object.fromEntries(jobRuns(db).map(r => [r.name, r]));
    const ic = (ok) => ok === true ? '<span class="ic ok">✓</span>' : ok === false ? '<span class="ic bad">✗</span>' : '<span class="ic muted">–</span>';
    const body = `<p><span class="hp ${h.ok ? '' : 'bad'}">${h.ok ? 'Every check passes' : `${h.failing.length} check${h.failing.length === 1 ? '' : 's'} failing`}</span> <span class="muted">checked ${dt(h.at)} · the same list is <code>backoffice doctor</code> and the top of the morning digest</span></p>
<ul class="checks">${h.checks.map(x => `<li>${ic(x.ok)}<b>${e(x.label)}</b><span>${e(x.detail)}${x.ok === false && x.fix ? `<br><span class="fix">→ ${e(x.fix)}</span>` : ''}</span></li>`).join('')}</ul>
<h2>Prove it now</h2><p>${button('/health/backup', c.csrf, 'Back up now (and check it)')} ${button('/health/job/restore_drill', c.csrf, 'Run the restore drill', { quiet: true })} ${button('/health/job/reconcile', c.csrf, 'Reconcile with Stripe', { quiet: true })} ${button('/health/job/digest', c.csrf, 'Queue the digest now', { quiet: true })} ${button('/health/job/probes', c.csrf, 'Run the probes', { quiet: true })}</p>
<div class="two"><div><h2>Backups on disk</h2>${table(listBackups(join(cfg.dataDir, 'backups')).slice(0, 10), [{ label: 'File', key: 'name' }, { label: 'Bytes', key: 'bytes', num: true }, { label: '', html: r => `<form method="post" action="/health/check" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="name" value="${e(r.name)}"><button class="quiet">check</button></form>` }], { empty: 'No backup yet — press the button above.' })}
<p class="muted">Restore is one command on the server: <code>backoffice restore &lt;file&gt;</code> (it checks the copy first and keeps the old file beside it).</p></div>
<div><h2>Watched from outside</h2>${form('/health/external', c.csrf, `<p class="muted">This app cannot report its own death. Point any uptime checker at <code>${e(cfg.baseUrl)}/healthz</code> (it answers <code>ok</code>) and record where, so the checklist knows a human set it up.</p>${field('url', 'The monitor (its dashboard URL or a note)', { value: db.setting('health:external_monitor', '') })}`, { submit: 'Record' })}
</div></div>
<h2>Jobs</h2>${table(Object.values(runs), [{ label: 'Job', key: 'name' }, { label: 'Last run', get: r => dt(r.last_run_at) }, { label: 'OK', html: r => r.last_ok ? '<span class="ok">yes</span>' : '<span class="bad">no</span>' }, { label: 'Detail', html: r => `<span class="muted" style="font-size:12px">${e(String(r.last_detail || '').slice(0, 220))}</span>` }], { empty: 'Nothing has run yet.' })}`;
    page(c, 'Health', body);
  });
  router.post('/health/backup', async (c) => { const r = await runJob(app, 'backup'); back(c, '/health', r.ok ? `backup ${r.result.checked.ok ? 'taken and checked' : 'taken but NOT checked'}: ${r.result.path}${r.result.offsite?.read_back ? ' · off-site copy read back' : r.result.offsite?.error ? ` · off-site FAILED: ${r.result.offsite.error}` : ''}` : `backup failed: ${r.error}`); });
  router.post('/health/check', (c) => { const name = String(c.body.name || ''); if (!/^backoffice-[0-9T-]+\.db$/.test(name)) throw new HttpError(400, 'bad file name'); const v = checkBackup(join(cfg.dataDir, 'backups', name), { expect: null }); back(c, '/health', v.ok ? `${name}: integrity ok, ${v.counts.orders} orders, books balanced` : `${name}: ${v.errors.join('; ')}`); });
  router.post('/health/job/:name', async (c) => { if (!['restore_drill', 'reconcile', 'digest', 'probes', 'backup'].includes(c.params.name)) throw new HttpError(400, 'not a health job'); const r = await runJob(app, c.params.name); back(c, '/health', `${r.job}: ${r.ok ? JSON.stringify(r.result).slice(0, 300) : r.error}`); });
  router.post('/health/external', (c) => { db.setSetting('health:external_monitor', String(c.body.url || '').slice(0, 300)); db.audit('web', 'health.external_monitor', 'health', null, { url: c.body.url }); back(c, '/health', 'recorded'); });

  // ---------- the audit log: every action, filterable, exportable
  const auditQuery = (c, limit) => { const f = { actor: c.query.get('actor') || '', action: c.query.get('action') || '', entity: c.query.get('entity') || '', q: c.query.get('q') || '', from: c.query.get('from') || '', to: c.query.get('to') || '' };
    return { f, rows: db.all(`SELECT * FROM audit_log WHERE (? = '' OR actor = ?) AND (? = '' OR action LIKE ?) AND (? = '' OR entity = ?) AND (? = '' OR entity_id LIKE ? OR detail LIKE ? OR action LIKE ?) AND (? = '' OR at >= ?) AND (? = '' OR at < ?) ORDER BY id DESC LIMIT ?`, f.actor, f.actor, f.action, `${f.action}%`, f.entity, f.entity, f.q, `%${f.q}%`, `%${f.q}%`, `%${f.q}%`, f.from, f.from, f.to, f.to ? f.to + 'T23:59:59.999Z' : '', limit) }; };
  router.get('/audit', (c) => {
    const { f, rows } = auditQuery(c, 500);
    const actors = db.all('SELECT DISTINCT actor FROM audit_log ORDER BY actor').map(r => r.actor); const entities = db.all('SELECT DISTINCT entity FROM audit_log WHERE entity IS NOT NULL ORDER BY entity').map(r => r.entity);
    page(c, 'Audit log', `<p class="muted">Every action anyone or anything took, in order, with who and what: logins, orders, refunds, replies, stock moves, backups, reconciliations. Append-only. ${link(`/audit.csv?${new URLSearchParams(f)}`, 'CSV of this filter')}</p>
${filters(c, [['actor', 'Actor', actors.map(a => [a, a])], ['action', 'Action starts with'], ['entity', 'Entity', entities.map(a => [a, a])], ['q', 'Contains'], ['from', 'From (YYYY-MM-DD)'], ['to', 'To']])}
<p class="muted">${rows.length} shown${rows.length === 500 ? ' (capped — narrow the filter or take the CSV)' : ''}</p>` +
      table(rows, [{ label: 'When', get: r => dt(r.at) }, { label: 'Actor', key: 'actor' }, { label: 'Action', key: 'action' }, { label: 'Entity', html: r => `${e(r.entity || '')} <span class="muted" title="${e(r.entity_id || '')}">${e(shortId(r.entity_id))}</span>`, nowrap: true }, { label: 'Detail', html: r => `<span class="muted" style="font-size:12.5px">${e(prettyDetail(r.detail))}</span>` }], { empty: 'Nothing matches.' }));
  });
  router.get('/audit.csv', (c) => { const { rows } = auditQuery(c, 100000); text(c.res, ['id,at,actor,action,entity,entity_id,detail', ...rows.map(r => csvLine([r.id, r.at, r.actor, r.action, r.entity, r.entity_id, r.detail]))].join('\n') + '\n', 200, { 'Content-Type': 'text/csv; charset=utf-8', 'Content-Disposition': 'attachment; filename="audit.csv"' }); });

  // ---------- a checked backup from the CLI's point of view is also an API call
  router.get('/api/health', (c) => { const h = doctor(db, cfg); c.res.statusCode = h.ok ? 200 : 503; c.res.setHeader('Content-Type', 'application/json; charset=utf-8'); c.res.end(JSON.stringify(h, null, 2)); });
  router.get('/api/backups', (c) => c.app.json(c.res, { backups: listBackups(join(cfg.dataDir, 'backups')), live: liveCounts(db) }));
  router.post('/api/backups', async (c) => { const r = await runJob(app, 'backup'); c.app.json(c.res, r, r.ok ? 201 : 500); });
  router.get('/api/audit', (c) => c.app.json(c.res, auditQuery(c, Number(c.query.get('limit')) || 200).rows));
}

// A long entity id (a backup's file path, say) shown by its tail.
const shortId = (id) => { const s = String(id ?? ''); return s.length > 32 ? '…' + s.slice(-30) : s; };
// An audit row's detail as readable pairs: nulls dropped, long values cut.
function prettyDetail(d) {
  if (!d) return '';
  let o; try { o = JSON.parse(d); } catch { return String(d).slice(0, 200); }
  if (o == null || typeof o !== 'object') return String(o).slice(0, 200);
  return Object.entries(o).filter(([, v]) => v != null && v !== '').map(([k, v]) => `${k}: ${Array.isArray(v) ? v.join(', ') : typeof v === 'object' ? JSON.stringify(v) : v}`).join(' · ').slice(0, 220);
}
