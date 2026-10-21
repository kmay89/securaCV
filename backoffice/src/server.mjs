// The HTTP surface: public endpoints the website and Stripe call, a JSON
// API for the CLI / an AI operator, and the server-rendered admin.
import { createServer } from 'node:http';
import { randomBytes } from 'node:crypto';
import { Router, HttpError, parseCookies, cookie, readBody, parseBody, RateLimiter, html, json, text, redirect, clientIp, escapeHtml as e } from './http.mjs';
import { layout } from './render.mjs';
import { sessionFor, login, logout, apiTokenFor, hasOperator } from './auth.mjs';
import { loadCatalog } from './modules/catalog.mjs';
import { verifyStripeSignature, eventToOrder } from './modules/stripe.mjs';
import { recordOrder, refund, orderByNumber, transition, listOrders } from './modules/orders.mjs';
import { upsertSubscription, postSubscriptionPayment } from './modules/subscriptions.mjs';
import { postPayout, profitAndLoss, postExpense } from './modules/ledger.mjs';
import { confirmConsent, unsubscribe, upsertContact, joinWaitlist, validEmail, searchContacts } from './modules/contacts.mjs';
import { sendConsentRequest } from './modules/campaigns.mjs';
import { openTicket, listTickets, ticketByNumber, reply as ticketReply, ingestEmail } from './modules/support.mjs';
import { publicStatus, listIncidents } from './modules/status.mjs';
import { invoiceByNumber, invoiceToken } from './modules/invoices.mjs';
import { listItems, move as invMove } from './modules/inventory.mjs';
import { listDevices, registerDevice } from './modules/devices.mjs';
import { openExceptions, resolve as resolveException } from './modules/exceptions.mjs';
import { snapshot } from './modules/dashboard.mjs';
import { registerAdmin } from './admin.mjs';
import { runJob, JOBS } from './jobs.mjs';

export function createApp({ db, cfg }) {
  cfg.secret = cfg.secret || db.setting('app:secret') || (() => { const s = randomBytes(32).toString('base64url'); db.setSetting('app:secret', s); return s; })();
  const app = { db, cfg, router: new Router(), limiter: new RateLimiter({ limit: 30, windowMs: 60_000 }), catalog: (o) => loadCatalog(cfg, o) };
  const { router } = app;

  // ---------- public
  router.get('/healthz', (c) => text(c.res, 'ok'));
  router.get('/status', (c) => html(c.res, statusPage(db, cfg), 200, { 'Cache-Control': 'public, max-age=60' }));
  router.get('/status.json', (c) => json(c.res, publicStatus(db), 200, { 'Cache-Control': 'public, max-age=60', 'Access-Control-Allow-Origin': '*' }));
  router.get('/confirm/:token', (c) => { const ok = confirmConsent(db, c.params.token); html(c.res, plain(cfg, ok ? 'Confirmed' : 'Link expired', ok ? `Thanks — ${e(ok.email)} is confirmed for occasional news. One-click unsubscribe is in every mail.` : 'That confirmation link is not valid any more. Sign up again if you still want the news.'), ok ? 200 : 404); });
  const unsub = (c) => { const r = unsubscribe(db, c.params.token); html(c.res, plain(cfg, r ? 'Unsubscribed' : 'Unknown link', r ? `${e(r.email)} will not get another newsletter. Order and support mail still arrives — those are about things you asked for.` : 'That unsubscribe link is not one we issued.'), r ? 200 : 404); };
  router.get('/u/:token', unsub); router.post('/u/:token', unsub, { public: true });
  router.get('/i/:number/:token', (c) => {
    const inv = invoiceByNumber(db, c.params.number);
    if (!inv || invoiceToken(cfg.secret, inv.number) !== c.params.token) throw new HttpError(404, 'no such invoice');
    html(c.res, invoicePage(cfg, inv), 200, { 'Cache-Control': 'private, no-store' });
  });

  router.post('/webhooks/stripe', async (c) => {
    if (!cfg.stripeWebhookSecret) throw new HttpError(503, 'STRIPE_WEBHOOK_SECRET not set');
    if (!verifyStripeSignature(c.raw, c.req.headers['stripe-signature'], cfg.stripeWebhookSecret)) throw new HttpError(400, 'bad signature');
    let event; try { event = JSON.parse(c.raw.toString('utf8')); } catch { throw new HttpError(400, 'bad JSON'); }
    const mapped = eventToOrder(event);
    const result = await applyStripeEvent(app, mapped, event);
    json(c.res, { received: true, ...result });
  }, { public: true, raw: true });

  router.post('/forms/:kind', async (c) => {
    const b = c.body || {};
    if (b.website || b._hp) return json(c.res, { ok: true }); // honeypot filled → pretend success, store nothing
    if (!app.limiter.allow(`forms:${clientIp(c.req)}`)) throw new HttpError(429, 'slow down');
    const email = String(b.email || '').trim();
    if (!validEmail(email)) throw new HttpError(400, 'a valid email is required');
    const kind = c.params.kind;
    let out;
    if (kind === 'waitlist') {
      const sku = String(b.sku || '').trim().slice(0, 60); if (!sku) throw new HttpError(400, 'sku required');
      const contact = upsertContact(db, { email, name: b.name || null, source: 'waitlist' });
      joinWaitlist(db, contact.id, sku, b.qty);
      if (b.newsletter && !contact.consent_marketing) sendConsentRequest(db, contact.id, cfg);
      out = { ok: true, waitlist: sku };
    } else if (['contact', 'request', 'feedback', 'support', 'press', 'wholesale'].includes(kind)) {
      const msg = String(b.message || b.body || '').trim().slice(0, 20_000); if (!msg) throw new HttpError(400, 'message required');
      const subject = String(b.subject || ({ request: 'Build request', feedback: 'Feedback from the site', contact: 'Contact form', press: 'Press inquiry', wholesale: 'Wholesale / distribution' }[kind]) || 'Contact form').slice(0, 200);
      const t = openTicket(db, { email, name: b.name || null, subject, body: msg + (b.extra ? `\n\n${JSON.stringify(b.extra)}` : ''), kind: { request: 'build_request', feedback: 'feedback', press: 'press', wholesale: 'wholesale', support: 'support' }[kind] || null, channel: 'form', cfg });
      out = { ok: true, ticket: t.number };
    } else throw new HttpError(404, 'unknown form');
    const back = String(b._redirect || '');
    if (back && back.startsWith(cfg.siteOrigin + '/')) return redirect(c.res, back);
    if ((c.req.headers.accept || '').includes('text/html')) return html(c.res, plain(cfg, 'Thanks', out.ticket ? `Got it — ticket ${e(out.ticket)}. A person reads every one; keep an eye on ${e(email)}.` : `You're on the list for ${e(out.waitlist)}. One mail when it's in stock, nothing else.`));
    json(c.res, out, 200, corsHeaders(cfg));
  }, { public: true });
  router.add('OPTIONS', '/forms/:kind', (c) => { c.res.writeHead(204, corsHeaders(cfg)); c.res.end(); }, { public: true });

  // ---------- auth
  router.get('/login', (c) => html(c.res, loginPage(cfg, c.query.get('err'), hasOperator(db))));
  router.post('/login', (c) => {
    if (!app.limiter.allow(`login:${clientIp(c.req)}`)) throw new HttpError(429, 'slow down');
    const s = login(db, c.body.password, c.body.code, { sessionHours: cfg.sessionHours });
    if (!s) return redirect(c.res, '/login?err=1');
    redirect(c.res, '/', { 'Set-Cookie': cookie('bo_session', s.id, { maxAge: cfg.sessionHours * 3600, secure: cfg.secure }) });
  }, { public: true, csrf: false });
  router.post('/logout', (c) => { if (c.session) logout(db, c.session.id); redirect(c.res, '/login', { 'Set-Cookie': cookie('bo_session', '', { maxAge: -1, secure: cfg.secure }) }); });

  // ---------- JSON API (bearer token or a logged-in session)
  router.get('/api', (c) => json(c.res, { name: 'securacv-backoffice', endpoints: API_INDEX }));
  router.get('/api/snapshot', (c) => json(c.res, snapshot(db)));
  router.get('/api/orders', (c) => json(c.res, listOrders(db, { status: c.query.get('status') || '', batch: c.query.get('batch') || '', q: c.query.get('q') || '' })));
  router.get('/api/orders/:number', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, o); });
  router.post('/api/orders', async (c) => json(c.res, recordOrder(db, c.body, { catalog: await app.catalog().catch(() => null), actor: c.actor }), 201));
  router.post('/api/orders/:number/transition', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, transition(db, o.id, c.body.to, { actor: c.actor, carrier: c.body.carrier, tracking: c.body.tracking, cfg })); });
  router.post('/api/orders/:number/refund', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, refund(db, o.id, c.body.cents, { actor: c.actor, reason: c.body.reason })); });
  router.get('/api/tickets', (c) => json(c.res, listTickets(db, { status: c.query.get('status') || '', kind: c.query.get('kind') || '', q: c.query.get('q') || '' })));
  router.get('/api/tickets/:number', (c) => { const t = ticketByNumber(db, c.params.number); if (!t) throw new HttpError(404, 'no such ticket'); json(c.res, t); });
  router.post('/api/tickets', (c) => json(c.res, openTicket(db, { ...c.body, channel: 'api', cfg }), 201));
  router.post('/api/tickets/:number/reply', (c) => { const t = ticketByNumber(db, c.params.number); if (!t) throw new HttpError(404, 'no such ticket'); json(c.res, ticketReply(db, t.id, c.body.body, { cfg, actor: c.actor, close: !!c.body.close })); });
  router.post('/api/mail/ingest', (c) => json(c.res, ingestEmail(db, c.body, { cfg })), { raw: false });
  router.get('/api/contacts', (c) => json(c.res, searchContacts(db, c.query.get('q') || '')));
  router.get('/api/inventory', (c) => json(c.res, listItems(db)));
  router.post('/api/inventory/move', (c) => json(c.res, invMove(db, c.body.sku, Number(c.body.delta), c.body.reason || 'adjust', c.body.ref || null)));
  router.get('/api/devices', (c) => json(c.res, listDevices(db, { q: c.query.get('q') || '' })));
  router.post('/api/devices', (c) => json(c.res, registerDevice(db, c.body), 201));
  router.get('/api/books/pnl', (c) => json(c.res, profitAndLoss(db, { from: c.query.get('from') || '0000-00-00', to: c.query.get('to') || '9999-99-99' })));
  router.post('/api/books/expense', (c) => json(c.res, { txn: postExpense(db, { ...c.body, cents: Number(c.body.cents) }) }, 201));
  router.get('/api/exceptions', (c) => json(c.res, openExceptions(db)));
  router.post('/api/exceptions/:key/resolve', async (c) => json(c.res, await resolveException(db, cfg, c.params.key, c.body.note || 'Resolved by operator.')));
  router.get('/api/jobs', (c) => json(c.res, Object.keys(JOBS)));
  router.post('/api/jobs/:name', async (c) => json(c.res, await runJob(app, c.params.name)));

  registerAdmin(app);

  // ---------- the pipeline
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://x');
    const route = router.match(req.method, url.pathname);
    try {
      if (!route) throw new HttpError(404, 'not found');
      const cookies = parseCookies(req);
      const session = sessionFor(db, cookies.bo_session);
      const bearer = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
      const tokenRow = url.pathname.startsWith('/api') ? apiTokenFor(db, bearer) : null;
      const c = { req, res, url, params: route.params, query: url.searchParams, session, csrf: session?.csrf, actor: tokenRow ? `token:${tokenRow.name}` : session ? 'web' : 'public', app, db, cfg, body: {}, raw: null };
      if (req.method === 'POST' || req.method === 'PUT') { c.raw = await readBody(req); if (!route.opts.raw) c.body = parseBody(c.raw, req.headers['content-type'] || ''); }
      if (!route.opts.public && !url.pathname.startsWith('/login') && !['/healthz', '/status', '/status.json'].includes(url.pathname) && !url.pathname.startsWith('/confirm/') && !url.pathname.startsWith('/u/') && !url.pathname.startsWith('/i/')) {
        if (url.pathname.startsWith('/api')) { if (!tokenRow && !session) throw new HttpError(401, 'bearer token or session required'); }
        else if (!session) return redirect(res, '/login');
        if (session && !tokenRow && req.method === 'POST' && route.opts.csrf !== false && !url.pathname.startsWith('/api')) {
          if (c.body._csrf !== session.csrf) throw new HttpError(403, 'bad CSRF token');
        }
      }
      await route.handler(c);
    } catch (err) {
      // A business rule ("cannot move", "unknown SKU") is the caller's mistake: 400. A thrown
      // TypeError or a database error is ours: 500, and it is logged.
      const status = err.status || (err instanceof Error && !(err instanceof TypeError || err instanceof RangeError || err instanceof ReferenceError || err instanceof SyntaxError) && !/SQLITE|sqlite/i.test(String(err.code || err.message)) ? 400 : 500);
      if (status === 500) console.error(`[backoffice] ${req.method} ${url.pathname}:`, err);
      if ((req.headers.accept || '').includes('text/html') && !url.pathname.startsWith('/api')) {
        const back = req.headers.referer && new URL(req.headers.referer, 'http://x').pathname;
        if (status < 500 && req.method === 'POST' && back && !back.startsWith('/login')) return redirect(res, `${back}?err=${encodeURIComponent(err.message)}`);
        return html(res, plain(cfg, `${status}`, e(err.message)), status);
      }
      json(res, { error: err.message, status }, status);
    }
  });
  app.server = server;
  app.listen = () => new Promise(r => server.listen(cfg.port, cfg.host, () => r(server.address())));
  app.close = () => new Promise(r => server.close(r));
  return app;
}

export async function applyStripeEvent(app, m, event = null) {
  const { db, cfg } = app;
  if (m.kind === 'order') { const catalog = await app.catalog().catch(() => null); const { order, created } = recordOrder(db, m, { catalog, actor: 'stripe' }); return { order: order.number, created }; }
  if (m.kind === 'refund') { const o = db.get('SELECT * FROM orders WHERE external_id = ?', m.external_id); if (!o) return { ignored: 'unknown order' }; const delta = m.refunded_cents - o.refunded_cents; if (delta > 0) refund(db, o.id, delta, { actor: 'stripe', reason: 'stripe' }); return { order: o.number, refunded_cents: m.refunded_cents }; }
  if (m.kind === 'subscription') { const s = upsertSubscription(db, m); return { subscription: s.id, status: s.status }; }
  if (m.kind === 'payout') { postPayout(db, { id: m.external_id, date: m.arrival || undefined, amount_cents: m.amount_cents }); return { payout: m.external_id }; }
  if (event?.type === 'invoice.paid' && event.data?.object?.subscription) { const o = event.data.object; postSubscriptionPayment(db, { external_id: o.subscription, amount_cents: o.amount_paid || 0, date: new Date((o.created || Date.now() / 1000) * 1000).toISOString().slice(0, 10) }); return { subscription_payment: o.subscription }; }
  return { ignored: m.type || m.kind };
}

const corsHeaders = (cfg) => ({ 'Access-Control-Allow-Origin': cfg.siteOrigin, 'Access-Control-Allow-Methods': 'POST, OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type', Vary: 'Origin' });

const API_INDEX = ['GET /api/snapshot', 'GET /api/orders?status=&batch=&q=', 'GET /api/orders/:number', 'POST /api/orders', 'POST /api/orders/:number/transition {to, carrier, tracking}', 'POST /api/orders/:number/refund {cents, reason}',
  'GET /api/tickets?status=&kind=&q=', 'GET /api/tickets/:number', 'POST /api/tickets {email, subject, body}', 'POST /api/tickets/:number/reply {body, close}', 'POST /api/mail/ingest {from, subject, text, messageId, inReplyTo}',
  'GET /api/contacts?q=', 'GET /api/inventory', 'POST /api/inventory/move {sku, delta, reason, ref}', 'GET /api/devices?q=', 'POST /api/devices {serial, mac, board, product_sku, fw_version, fw_sha256, batch}',
  'GET /api/books/pnl?from=&to=', 'POST /api/books/expense {date, account, cents, memo}', 'GET /api/exceptions', 'POST /api/exceptions/:key/resolve', 'GET /api/jobs', 'POST /api/jobs/:name'];

export const plain = (cfg, title, bodyHtml) => layout({ title, body: `<p>${bodyHtml}</p><p><a href="${e(cfg.siteOrigin)}">← back to ${e(cfg.product || 'the site')}</a></p>`, company: cfg.company, nav: false });

function loginPage(cfg, err, ready) {
  return layout({ title: 'Log in', company: cfg.company, nav: false, body: ready
    ? `${err ? '<div class="flash err">Wrong password or code.</div>' : ''}<form method="post" action="/login" class="box"><label for="pw">Password</label><input id="pw" type="password" name="password" required autocomplete="current-password"><label for="code">Authenticator code</label><input id="code" name="code" inputmode="numeric" pattern="[0-9]{6}" required autocomplete="one-time-code"><div style="margin-top:.8rem"><button>Log in</button></div></form>`
    : '<p>No operator yet. On the server, run <code>backoffice setup</code> to set the password and get the authenticator QR.</p>' });
}

function statusPage(db, cfg) {
  const s = publicStatus(db);
  const rows = s.probes.map(p => `<tr><td>${e(p.name)}</td><td class="${p.ok === false ? 'bad' : p.ok ? 'ok' : 'muted'}">${p.ok == null ? 'not checked yet' : p.ok ? '● operational' : '● down'}</td><td class="num">${p.uptime_90d == null ? '—' : p.uptime_90d + '%'}</td><td class="muted">${e(p.checked_at || '')}</td></tr>`).join('');
  const inc = s.incidents.map(i => `<div class="msg"><div class="meta">${e(i.started_at)} · ${e(i.status)}${i.resolved_at ? ` · resolved ${e(i.resolved_at)}` : ''}</div><b>${e(i.title)}</b>\n${e(i.body || '')}</div>`).join('') || '<p class="muted">No incidents on record.</p>';
  return layout({ title: `${cfg.product || 'SecuraCV'} status — ${s.overall}`, company: cfg.company, nav: false, body: `<p class="${s.overall === 'operational' ? 'ok' : 'bad'}"><b>${s.overall === 'operational' ? 'All systems operational' : 'Something is degraded'}</b> · checked from one probe, every few minutes · <a href="/status.json">JSON</a></p>
<table><thead><tr><th>Service</th><th>Now</th><th class="num">90-day uptime</th><th>Last check (UTC)</th></tr></thead><tbody>${rows}</tbody></table><h2>Incidents</h2>${inc}
<p class="muted">Nothing on this page phones home: a Canary on your wall talks to your hub, not to us, so an outage here never touches a device.</p>` });
}

function invoicePage(cfg, inv) {
  const rows = inv.items.map(i => `<tr><td>${e(i.description)}${i.sku ? ` <span class="muted">(${e(i.sku)})</span>` : ''}</td><td class="num">${i.qty}</td><td class="num">${(i.unit_cents / 100).toFixed(2)}</td><td class="num">${(i.qty * i.unit_cents / 100).toFixed(2)}</td></tr>`).join('');
  const money = (c) => `$${(c / 100).toFixed(2)}`;
  return layout({ title: `Invoice ${inv.number}`, company: cfg.company, nav: false, body: `<p><b>${e(cfg.company)}</b> · ${e(cfg.supportAddr || '')}</p>
<div class="row"><div><b>Bill to</b><br>${e(inv.bill_to || inv.contact_name || inv.email || '').replace(/\n/g, '<br>')}</div><div><b>Invoice</b> ${e(inv.number)}<br>${inv.po_number ? `<b>PO</b> ${e(inv.po_number)}<br>` : ''}<b>Issued</b> ${e(inv.issued_at || '—')}<br><b>Due</b> ${e(inv.due_at || '—')} (net ${inv.terms_days})<br><b>Status</b> ${e(inv.status)}</div></div>
<table><thead><tr><th>Item</th><th class="num">Qty</th><th class="num">Unit</th><th class="num">Amount</th></tr></thead><tbody>${rows}</tbody>
<tfoot><tr><td colspan="3" class="num">Subtotal</td><td class="num">${money(inv.subtotal_cents)}</td></tr><tr><td colspan="3" class="num">Shipping</td><td class="num">${money(inv.shipping_cents)}</td></tr><tr><td colspan="3" class="num">Tax</td><td class="num">${money(inv.tax_cents)}</td></tr><tr><td colspan="3" class="num"><b>Total (USD)</b></td><td class="num"><b>${money(inv.total_cents)}</b></td></tr></tfoot></table>
<p>${e(inv.notes || '')}</p><p class="muted">Pay by ACH or check to the remittance details ${cfg.remittance ? 'below' : 'in the covering email'}.</p>${cfg.remittance ? `<pre>${e(cfg.remittance)}</pre>` : ''}<p class="muted">Print this page for a PDF.</p>` });
}
