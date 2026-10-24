// The HTTP surface: public endpoints the website and Stripe call, a JSON
// API for the CLI / an AI operator, and the server-rendered admin.
import { createServer } from 'node:http';
import { randomBytes } from 'node:crypto';
import { Router, HttpError, parseCookies, cookie, readBody, parseBody, RateLimiter, Lockout, html, json, text, redirect, clientIp, escapeHtml, safeEqual, int, asArray } from './http.mjs';
const e = escapeHtml;
import { layout, setTimezone, dt as fmtDt, day as fmtDay } from './render.mjs';
import { orderToken, deliveryLine, trackingUrl } from './modules/delivery.mjs';
import { editItems } from './modules/orders.mjs';
import { sessionFor, login, logout, apiTokenFor, hasOperator, carryLegacyOperator, can } from './auth.mjs';
import { actorStore, nowIso } from './db.mjs';
import { registerUsers } from './admin_users.mjs';
import { loadCatalog, loadShipping } from './modules/catalog.mjs';
import { verifyStripeSignature, eventToOrder, fetchLineItems, resolveDiscountCode } from './modules/stripe.mjs';
import { recordOrder, refund, orderByNumber, transition, listOrders, timeline, addNote, setTags, batches, statusCounts } from './modules/orders.mjs';
import { listCustomers, contactById, customerStats, setContactTags } from './modules/contacts.mjs';
import { listDiscounts, createDiscount, setDiscountStatus, evaluate as evaluateDiscount } from './modules/discounts.mjs';
import { analytics } from './modules/analytics.mjs';
import { listProducts, productDetail } from './modules/products.mjs';
import { search } from './modules/search.mjs';
import { upsertSubscription, postSubscriptionPayment } from './modules/subscriptions.mjs';
import { postPayout, profitAndLoss, postExpense } from './modules/ledger.mjs';
import { confirmConsent, unsubscribe, upsertContact, joinWaitlist, validEmail, searchContacts } from './modules/contacts.mjs';
import { sendConsentRequest } from './modules/campaigns.mjs';
import { openTicket, listTickets, ticketByNumber, reply as ticketReply, ingestEmail } from './modules/support.mjs';
import { publicStatus, listIncidents } from './modules/status.mjs';
import { invoiceByNumber, invoiceToken } from './modules/invoices.mjs';
import { listItems, move as invMove } from './modules/inventory.mjs';
import { listDevices, registerDevice } from './modules/devices.mjs';
import { openExceptions, resolve as resolveException, raise as raiseException } from './modules/exceptions.mjs';
import { snapshot } from './modules/dashboard.mjs';
import { registerAdmin } from './admin.mjs';
import { runJob, JOBS } from './jobs.mjs';
import { isDemo, DEMO_PASSWORD, DEMO_USERS, DEMO_TOTP_SECRET, DEMO_WEBHOOK_SECRET, DEMO_CATALOG, DEMO_SHIPPING, SCENARIOS, localBase, seedDemoUsers } from './modules/demo.mjs';
import { totp } from './auth.mjs';
import { weekLabel as weekLabelNow } from './modules/orders.mjs';

export function createApp({ db, cfg }) {
  cfg.tz = setTimezone(cfg.tz);
  carryLegacyOperator(db, { email: cfg.supportAddr || 'owner@localhost' });
  cfg.secret = cfg.secret || db.setting('app:secret') || (() => { const s = randomBytes(32).toString('base64url'); db.setSetting('app:secret', s); return s; })();
  // Demo mode: synthetic data, and a hard rule that nothing reaches a real service.
  if (cfg.demo || isDemo(db)) {
    cfg.demo = true; cfg.githubToken = ''; cfg.imapUrl = ''; cfg.smtpUrl = ''; cfg.stripeApiKey = '';
    cfg.stripeWebhookSecret = DEMO_WEBHOOK_SECRET;
    cfg.mailFrom = cfg.mailFrom || 'Errer Labs (demo) <demo@backoffice.example.test>';
    cfg.baseUrl = localBase(cfg); cfg.siteOrigin = cfg.baseUrl;   // every link, probe and scenario stays on this machine
    // A demo database from before people existed (or between resets) must have the demo people the login page names, on every boot.
    if (isDemo(db)) seedDemoUsers(db);
  }
  // In demo mode the catalog is the demo's own, never fetched from the live site.
  const app = { db, cfg, json, router: new Router(), limiter: new RateLimiter({ limit: 30, windowMs: 60_000 }), formLimiter: new RateLimiter({ limit: 5, windowMs: 10 * 60_000 }), lockout: new Lockout({ limit: 8, windowMs: 15 * 60_000 }), once: new Map(), catalog: cfg.demo ? async () => DEMO_CATALOG : (o) => loadCatalog(cfg, o), shipping: cfg.demo ? async () => DEMO_SHIPPING : (o) => loadShipping(cfg, o) };
  // The website's fulfillment block knows where parcels leave from and how far a scale may drift; the environment can override either.
  app.catalog().then(cat => { const f = cat?.fulfillment || {}; if (f.ship_from?.state && !process.env.SHIP_FROM_STATE) cfg.shipFromState = String(f.ship_from.state).toUpperCase(); if (Number.isFinite(Number(f.weight_tolerance_oz)) && cfg.weightToleranceOz == null) cfg.weightToleranceOz = Number(f.weight_tolerance_oz); }).catch(() => {});
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
    if (!inv || !safeEqual(invoiceToken(cfg.secret, inv.number), c.params.token)) throw new HttpError(404, 'no such invoice');
    html(c.res, invoicePage(cfg, inv), 200, { 'Cache-Control': 'private, no-store' });
  });

  // The customer's own page: where it is, when it lands, what was in it. A signed token, nothing to log into.
  router.get('/o/:number/:token', (c) => {
    const o = orderByNumber(db, c.params.number);
    if (!o || !safeEqual(orderToken(cfg.secret, o.number), c.params.token)) throw new HttpError(404, 'no such order');
    html(c.res, orderStatusPage(cfg, o), 200, { 'Cache-Control': 'private, no-store' });
  });

  router.post('/webhooks/stripe', async (c) => {
    if (!cfg.stripeWebhookSecret) throw new HttpError(503, 'STRIPE_WEBHOOK_SECRET not set');
    if (!verifyStripeSignature(c.raw, c.req.headers['stripe-signature'], cfg.stripeWebhookSecret)) throw new HttpError(400, 'bad signature');
    let event; try { event = JSON.parse(c.raw.toString('utf8')); } catch { throw new HttpError(400, 'bad JSON'); }
    // Stripe redelivers, sometimes twice at once. The event id is claimed BEFORE the work (an
    // insert that only one delivery wins) and marked applied AFTER it; a failure releases the
    // claim, and a claim left in flight (the process died mid-way) is taken over by the next
    // redelivery once it is a minute old. Only an applied event answers "duplicate".
    if (event.id && !claimStripeEvent(db, event)) return json(c.res, { received: true, duplicate: true });
    const mapped = eventToOrder(event);
    let result;
    try { result = await applyStripeEvent(app, mapped, event); }
    catch (err) { if (event.id) db.run('DELETE FROM stripe_events WHERE id = ? AND applied_at IS NULL', event.id); throw err; }
    if (event.id) db.run('UPDATE stripe_events SET applied_at = ? WHERE id = ?', nowIso(), event.id);
    json(c.res, { received: true, ...result });
  }, { public: true, raw: true });

  router.post('/forms/:kind', async (c) => {
    const b = c.body || {};
    if (b.website || b._hp) return json(c.res, { ok: true }); // honeypot filled → pretend success, store nothing
    if (!app.limiter.allow(`forms:${clientIp(c.req)}`)) throw new HttpError(429, 'slow down');
    const email = String(b.email || '').trim();
    if (!validEmail(email)) throw new HttpError(400, 'a valid email is required');
    if (!app.formLimiter.allow(`forms:${email.toLowerCase()}`)) throw new HttpError(429, 'that address has sent enough forms for now — try again in ten minutes');
    const kind = c.params.kind;
    let out;
    if (kind === 'waitlist') {
      const sku = String(b.sku || '').trim().slice(0, 60); if (!sku) throw new HttpError(400, 'sku required');
      const contact = upsertContact(db, { email, name: b.name || null, source: 'waitlist' });
      joinWaitlist(db, contact.id, sku, b.qty);
      if (b.newsletter && !contact.consent_marketing) sendConsentRequest(db, contact.id, cfg);
      out = { ok: true, waitlist: sku };
    } else if (kind === 'newsletter') {
      const contact = upsertContact(db, { email, name: b.name || null, source: 'newsletter' });
      if (!contact.consent_marketing) sendConsentRequest(db, contact.id, cfg);
      out = { ok: true, newsletter: contact.consent_marketing ? 'already confirmed' : 'confirmation mail sent' };
    } else if (['contact', 'request', 'feedback', 'support', 'press', 'wholesale'].includes(kind)) {
      const msg = String(b.message || b.body || '').trim().slice(0, 20_000); if (!msg) throw new HttpError(400, 'message required');
      const subject = String(b.subject || ({ request: 'Build request', feedback: 'Feedback from the site', contact: 'Contact form', press: 'Press inquiry', wholesale: 'Wholesale / distribution' }[kind]) || 'Contact form').slice(0, 200);
      const t = openTicket(db, { email, name: b.name || null, subject, body: msg + (b.extra ? `\n\n${JSON.stringify(b.extra)}` : ''), kind: { request: 'build_request', feedback: 'feedback', press: 'press', wholesale: 'wholesale', support: 'support' }[kind] || null, channel: 'form', cfg });
      out = { ok: true, ticket: t.number };
    } else throw new HttpError(404, 'unknown form');
    const back = sameOriginUrl(String(b._redirect || ''), cfg.siteOrigin);
    if (back) return redirect(c.res, back);
    if ((c.req.headers.accept || '').includes('text/html')) return html(c.res, plain(cfg, 'Thanks', out.ticket ? `Got it — ticket ${e(out.ticket)}. A person reads every one; keep an eye on ${e(email)}.` : out.newsletter ? `Check ${e(email)} for a confirmation link — nothing is sent without the click.` : `You're on the list for ${e(out.waitlist)}. One mail when it's in stock, nothing else.`));
    json(c.res, out, 200, corsHeaders(cfg));
  }, { public: true });
  router.add('OPTIONS', '/forms/:kind', (c) => { c.res.writeHead(204, corsHeaders(cfg)); c.res.end(); }, { public: true });

  // ---------- auth
  router.get('/login', (c) => html(c.res, loginPage(cfg, c.query.get('err'), hasOperator(db), c.query.get('ok'), cfg.demo ? { password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET), email: c.query.get('as') || DEMO_USERS[0].email, users: DEMO_USERS } : null)));
  router.post('/login', (c) => {
    const ip = clientIp(c.req);
    if (!app.limiter.allow(`login:${ip}`)) throw new HttpError(429, 'slow down');
    // Eight wrong attempts in fifteen minutes and this address waits the window out; the audit log keeps every attempt.
    if (app.lockout.locked(ip)) { db.audit('web', 'auth.login.locked', 'session', null, { ip }); throw new HttpError(429, 'too many failed logins from this address — try again in fifteen minutes'); }
    const s = login(db, { email: c.body.email, password: c.body.password, code: c.body.code }, { sessionHours: cfg.sessionHours, ip });
    if (!s) { app.lockout.fail(ip); return redirect(c.res, '/login?err=1'); }
    app.lockout.clear(ip);
    redirect(c.res, '/', { 'Set-Cookie': cookie('bo_session', s.id, { maxAge: cfg.sessionHours * 3600, secure: cfg.secure }) });
  }, { public: true, csrf: false });
  router.post('/logout', (c) => { if (c.session) logout(db, c.session.id); redirect(c.res, '/login', { 'Set-Cookie': [cookie('bo_session', '', { maxAge: -1, secure: cfg.secure }), cookie('bo_bench', '', { maxAge: -1, secure: cfg.secure })] }); });

  // ---------- JSON API (bearer token or a logged-in session)
  router.get('/api', (c) => json(c.res, { name: 'securacv-backoffice', endpoints: API_INDEX }));
  router.get('/api/snapshot', (c) => json(c.res, snapshot(db)));
  router.get('/api/orders', (c) => json(c.res, listOrders(db, Object.fromEntries(['status', 'batch', 'q', 'tag', 'from', 'to', 'discount', 'sku'].map(k => [k, c.query.get(k) || ''])))));
  router.get('/api/orders/:number', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, o); });
  router.post('/api/orders', async (c) => json(c.res, recordOrder(db, c.body, { catalog: await app.catalog().catch(() => null), actor: c.actor }), 201));
  router.post('/api/orders/:number/transition', async (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, transition(db, o.id, c.body.to, { actor: c.actor, carrier: c.body.carrier, tracking: c.body.tracking, cfg, shipping: c.body.to === 'packed' ? await app.shipping().catch(() => null) : null, measuredOz: c.body.measured_oz ?? null, postageCents: int(c.body.postage_cents, { name: 'postage_cents', min: 0, fallback: 0 }), shipAnyway: !!c.body.ship_anyway, check: c.body.check && typeof c.body.check === 'object' && !Array.isArray(c.body.check) ? { ticked: asArray(c.body.check.ticked ?? [], 'check.ticked'), measuredOz: c.body.check.measured_oz ?? null, anyway: !!c.body.check.anyway, reason: c.body.check.reason } : null })); });
  router.post('/api/orders/:number/refund', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, refund(db, o.id, int(c.body.cents, { name: 'cents', min: 1 }), { actor: c.actor, reason: c.body.reason })); });
  router.get('/api/tickets', (c) => json(c.res, listTickets(db, { status: c.query.get('status') || '', kind: c.query.get('kind') || '', q: c.query.get('q') || '' })));
  router.get('/api/tickets/:number', (c) => { const t = ticketByNumber(db, c.params.number); if (!t) throw new HttpError(404, 'no such ticket'); json(c.res, t); });
  router.post('/api/tickets', (c) => json(c.res, openTicket(db, { ...c.body, channel: 'api', cfg }), 201));
  router.post('/api/tickets/:number/reply', (c) => { const t = ticketByNumber(db, c.params.number); if (!t) throw new HttpError(404, 'no such ticket'); json(c.res, ticketReply(db, t.id, c.body.body, { cfg, actor: c.actor, close: !!c.body.close })); });
  router.post('/api/mail/ingest', (c) => json(c.res, ingestEmail(db, c.body, { cfg })), { raw: false });
  router.post('/api/orders/:number/items', async (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, editItems(db, o.id, asArray(c.body.items, 'items'), { catalog: await app.catalog().catch(() => null), actor: c.actor })); });
  router.get('/api/orders/:number/packing', async (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); const { packOrder } = await import('./modules/packing.mjs'); json(c.res, { frozen: o.pack_plan ? JSON.parse(o.pack_plan) : null, now: packOrder(await app.shipping().catch(() => null), o.items) }); });
  router.get('/api/orders/:number/events', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, timeline(db, o.id)); });
  router.post('/api/orders/:number/note', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, addNote(db, o.id, c.body.body, { actor: c.actor }), 201); });
  router.post('/api/orders/:number/tags', (c) => { const o = orderByNumber(db, c.params.number); if (!o) throw new HttpError(404, 'no such order'); json(c.res, setTags(db, o.id, c.body.tags, { actor: c.actor })); });
  router.get('/api/batches', (c) => json(c.res, { this_week: weekLabelNow(), counts: statusCounts(db), batches: batches(db) }));
  router.get('/api/contacts', (c) => json(c.res, searchContacts(db, c.query.get('q') || '')));
  router.get('/api/customers', (c) => json(c.res, listCustomers(db, { q: c.query.get('q') || '', tag: c.query.get('tag') || '', segment: c.query.get('segment') || '', sort: c.query.get('sort') || 'recent' })));
  router.get('/api/customers/:id', (c) => { const ct = contactById(db, c.params.id); if (!ct) throw new HttpError(404, 'no such customer'); json(c.res, { ...ct, stats: customerStats(db, ct.id), orders: listOrders(db, { q: ct.email, limit: 100 }).filter(o => o.contact_id === ct.id) }); });
  router.post('/api/customers/:id/tags', (c) => json(c.res, setContactTags(db, Number(c.params.id), c.body.tags)));
  router.get('/api/products', async (c) => json(c.res, listProducts(db, await app.catalog(), { days: Number(c.query.get('days')) || 30 })));
  router.get('/api/products/:sku', async (c) => { const p = productDetail(db, await app.catalog(), c.params.sku); if (!p) throw new HttpError(404, 'no such product'); json(c.res, p); });
  router.get('/api/discounts', (c) => json(c.res, listDiscounts(db)));
  router.post('/api/discounts', (c) => json(c.res, createDiscount(db, c.body), 201));
  router.post('/api/discounts/:code/status', (c) => json(c.res, setDiscountStatus(db, c.params.code, c.body.status)));
  router.get('/api/discounts/:code/evaluate', (c) => json(c.res, evaluateDiscount(db, c.params.code, { subtotal_cents: Number(c.query.get('subtotal') || 0), shipping_cents: Number(c.query.get('shipping') || 0) })));
  router.get('/api/analytics', (c) => json(c.res, analytics(db, { days: Number(c.query.get('days')) || 30 })));
  router.get('/api/search', (c) => json(c.res, search(db, c.query.get('q') || '')));
  router.get('/api/inventory', (c) => json(c.res, listItems(db)));
  router.post('/api/inventory/move', (c) => json(c.res, invMove(db, String(c.body.sku || ''), int(c.body.delta, { name: 'delta' }), String(c.body.reason || 'adjust'), c.body.ref == null ? null : String(c.body.ref).slice(0, 120))));
  router.get('/api/devices', (c) => json(c.res, listDevices(db, { q: c.query.get('q') || '' })));
  router.post('/api/devices', (c) => json(c.res, registerDevice(db, c.body), 201));
  router.get('/api/books/pnl', (c) => json(c.res, profitAndLoss(db, { from: c.query.get('from') || '0000-00-00', to: c.query.get('to') || '9999-99-99' })));
  router.post('/api/books/expense', (c) => json(c.res, { txn: postExpense(db, { ...c.body, cents: Number(c.body.cents) }) }, 201));
  router.get('/api/exceptions', (c) => json(c.res, openExceptions(db)));
  router.post('/api/exceptions/:key/resolve', async (c) => json(c.res, await resolveException(db, cfg, c.params.key, c.body.note || 'Resolved by operator.')));
  router.get('/api/jobs', (c) => json(c.res, Object.keys(JOBS)));
  router.post('/api/jobs/:name', async (c) => json(c.res, await runJob(app, c.params.name)));
  router.get('/api/demo/scenarios', (c) => { if (!cfg.demo) throw new HttpError(404, 'not in demo mode'); json(c.res, Object.fromEntries(Object.entries(SCENARIOS).map(([k, v]) => [k, v.label]))); });
  router.post('/api/demo/:scenario', async (c) => { if (!cfg.demo) throw new HttpError(404, 'not in demo mode'); const sc = SCENARIOS[c.params.scenario]; if (!sc) throw new HttpError(404, 'unknown scenario'); json(c.res, await sc.run(app)); });

  registerAdmin(app);
  registerUsers(app);

  // ---------- the pipeline
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://x');
    const route = router.match(req.method, url.pathname);
    let onceKey = null;
    try {
      if (!route) throw new HttpError(404, 'not found');
      if (cfg.secure) res.setHeader('Strict-Transport-Security', 'max-age=31536000; includeSubDomains');
      // A browser says where a request came from; a state change from another site is refused before the body is read.
      const site = req.headers['sec-fetch-site'];
      if (site && !['same-origin', 'same-site', 'none'].includes(site) && req.method !== 'GET' && req.method !== 'HEAD' && !route.opts.public) throw new HttpError(403, 'cross-site request refused');
      const cookies = parseCookies(req);
      const session = sessionFor(db, cookies.bo_session);
      const bearer = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
      const tokenRow = url.pathname.startsWith('/api') ? apiTokenFor(db, bearer) : null;
      const c = { req, res, url, params: route.params, query: url.searchParams, session, user: session ? { id: session.user_id, email: session.email, name: session.name, role: session.role } : null, csrf: session?.csrf, actor: tokenRow ? `token:${tokenRow.name}` : session ? session.email : 'public', app, db, cfg, body: {}, raw: null };
      if (req.method === 'POST' || req.method === 'PUT') { c.raw = await readBody(req); if (!route.opts.raw) c.body = parseBody(c.raw, req.headers['content-type'] || ''); }
      if (!route.opts.public && !url.pathname.startsWith('/login') && !['/healthz', '/status', '/status.json'].includes(url.pathname) && !url.pathname.startsWith('/confirm/') && !url.pathname.startsWith('/u/') && !url.pathname.startsWith('/i/') && !url.pathname.startsWith('/o/') && !url.pathname.startsWith('/invite/')) {
        if (url.pathname.startsWith('/api')) { if (!tokenRow && !session) throw new HttpError(401, 'bearer token or session required'); if (tokenRow?.scope === 'ro' && !['GET', 'HEAD'].includes(req.method)) throw new HttpError(403, 'this token is read-only'); }
        else if (!session) return redirect(res, '/login');
        // A person's role decides, by path and method; an owner-made API token is not bound to a role.
        if (session && !tokenRow && !can(session.role, req.method, url.pathname)) throw new HttpError(403, `your role (${session.role}) cannot do this — an owner can`);
        if (session && !tokenRow && req.method === 'POST' && route.opts.csrf !== false && !url.pathname.startsWith('/api')) {
          if (!safeEqual(c.body._csrf || '', session.csrf)) throw new HttpError(403, 'bad CSRF token');
          // A form carries a one-time token: the first POST with it runs, a second (a double click, a
          // repeated submit) is answered with "already done" and changes nothing.
          if (typeof c.body._once === 'string' && c.body._once) {
            const key = `${session.id}:${c.body._once}`; const now = Date.now();
            if (app.once.size > 5000) for (const [k, t] of app.once) if (now - t > 7_200_000) app.once.delete(k);
            if (app.once.has(key)) return redirect(res, `${safeLocalPath(req.headers.referer) || '/'}?ok=${encodeURIComponent('that form was already submitted — nothing was done twice')}`);
            app.once.set(key, now); onceKey = key;
          }
        }
      }
      await actorStore.run({ actor: c.actor }, () => route.handler(c));
    } catch (err) {
      if (onceKey) app.once.delete(onceKey);   // a refused submission may be corrected and sent again
      // A business rule ("cannot move", "unknown SKU") is the caller's mistake: 400. A thrown
      // TypeError or a database error is ours: 500, and it is logged.
      // A row that would collide with one that exists (a second device with the same MAC, a slug
      // taken) is the caller's to resolve: 409, with the constraint named, never a 500.
      const conflict = /SQLITE_CONSTRAINT|constraint failed/i.test(String(err.code || '') + ' ' + String(err.message || ''));
      const status = err.status || (conflict ? 409 : err instanceof Error && !(err instanceof TypeError || err instanceof RangeError || err instanceof ReferenceError || err instanceof SyntaxError) && !/SQLITE|sqlite/i.test(String(err.code || err.message)) ? 400 : 500);
      // Our own bug never reaches the page or the API body: it is logged here, raised as an
      // exception (so it shows on Home and, with a token, as an issue), and the client gets a
      // generic line. A refused request (4xx) carries the rule it broke.
      if (status === 500) { console.error('[backoffice]', req.method, JSON.stringify(url.pathname), err); const where = `${req.method} ${url.pathname.replace(/\/\d+(?=\/|$)/g, '/:id')}`; raiseException(db, cfg, `bug:${where}`, `backoffice [bug]: ${where}`, `${String(err.stack || err).slice(0, 1500)}\n\nThe person saw a generic error; the request was refused, nothing half-written. Fix the code, then resolve this.`).catch(() => {}); }
      const message = status === 500 ? 'Something went wrong on our side; it has been logged.' : conflict ? `that would collide with a record that already exists (${String(err.message).replace(/^.*constraint failed:\s*/i, '')})` : String(err.message);
      if ((req.headers.accept || '').includes('text/html') && !url.pathname.startsWith('/api')) {
        // Back to the admin page the form came from — only ever a local admin path.
        const back = safeLocalPath(req.headers.referer);
        if (status < 500 && req.method === 'POST' && back && !back.startsWith('/login')) return redirect(res, `${back}?err=${encodeURIComponent(message)}`);
        const who = (() => { try { return sessionFor(db, parseCookies(req).bo_session); } catch { return null; } })();
        if (who) return html(res, layout({ title: `${status}`, body: `<p>${escapeHtml(message)}</p><p><a href="/">← Home</a></p>`, path: url.pathname, company: cfg.company, csrf: who.csrf, user: { id: who.user_id, email: who.email, name: who.name, role: who.role }, demo: cfg.demo, demoResetMinutes: cfg.demoResetMinutes }), status);
        return html(res, plain(cfg, `${status}`, escapeHtml(message)), status);
      }
      json(res, { error: message, status }, status);
    }
  });
  app.server = server;
  app.listen = () => new Promise(r => server.listen(cfg.port, cfg.host, () => { const a = server.address(); if (cfg.demo) { cfg.port = a.port; cfg.baseUrl = `http://127.0.0.1:${a.port}`; cfg.siteOrigin = cfg.baseUrl; } r(a); }));
  app.close = () => new Promise(r => server.close(r));
  return app;
}

export async function applyStripeEvent(app, m, event = null) {
  const { db, cfg } = app;
  if (m.kind === 'order') {
    const catalog = await app.catalog().catch(() => null);
    if (!m.line_items_present) { try { const items = await fetchLineItems(cfg, m.external_id, { fetchImpl: app.fetchImpl || fetch }); if (items) m.items = items; } catch (e) { db.audit('stripe', 'order.line_items.failed', 'order', m.external_id, { error: String(e.message || e) }); } }
    if (m.discount_code && /^promo_|^[A-Za-z0-9]{8,}$/.test(m.discount_code) && !db.get('SELECT code FROM discounts WHERE code = ?', m.discount_code)) { try { m.discount_code = await resolveDiscountCode(cfg, m.discount_code, { fetchImpl: app.fetchImpl || fetch }); } catch { /* the id stays */ } }
    const { order, created } = recordOrder(db, m, { catalog, actor: 'stripe' });
    if (created && order.currency !== 'usd') await raiseException(db, cfg, `currency:${order.number}`, `orders [currency]: ${order.number} is in ${order.currency.toUpperCase()}`, `Stripe charged ${order.currency.toUpperCase()} for ${order.number}; the books and the sales-tax table count every order as USD cents. Check the Payment Link's currency, and correct the ledger by hand if this was real: ${cfg.baseUrl}/orders/${order.id}`);
    if (created && !order.items.length) await raiseException(db, cfg, `stripe:line-items:${order.number}`, `orders [no line items]: ${order.number}`, `Stripe's webhook carried no line_items for ${m.external_id} and they could not be fetched${cfg.stripeApiKey ? '' : ' (STRIPE_API_KEY is not set)'}. The order is recorded with no items and cannot be packed until they are added — see ${cfg.baseUrl}/orders/${order.id}.`);
    return { order: order.number, created, items: order.items.length };
  }
  if (m.kind === 'refund') {
    // A refund names the PaymentIntent (or the charge); a Checkout order is stored under its session id too.
    const o = db.get('SELECT * FROM orders WHERE payment_intent = ? OR external_id = ? OR external_id = ?', m.external_id, m.external_id, m.charge || '');
    if (!o) return { ignored: 'unknown order' };
    const delta = m.refunded_cents - o.refunded_cents; if (delta > 0) refund(db, o.id, delta, { actor: 'stripe', reason: 'stripe' });
    return { order: o.number, refunded_cents: m.refunded_cents };
  }
  if (m.kind === 'subscription') { const s = upsertSubscription(db, m); return { subscription: s.id, status: s.status }; }
  if (m.kind === 'payout') { postPayout(db, { id: m.external_id, date: m.arrival || undefined, amount_cents: m.amount_cents }); return { payout: m.external_id }; }
  if (event?.type === 'invoice.paid' && event.data?.object?.subscription) {
    const o = event.data.object; const sub = typeof o.subscription === 'string' ? o.subscription : o.subscription?.id;
    const posted = postSubscriptionPayment(db, { external_id: sub, invoice_id: o.id, amount_cents: o.amount_paid || 0, date: new Date((o.created || Date.now() / 1000) * 1000).toISOString().slice(0, 10) });
    return { subscription_payment: sub, posted: !!posted };
  }
  return { ignored: m.type || m.kind };
}

// True when this delivery owns the event: a fresh id, or a claim another delivery left in flight
// for over a minute. An applied event, or a claim younger than that, belongs to someone else.
export function claimStripeEvent(db, event, { staleMs = 60_000, now = Date.now() } = {}) {
  const at = new Date(now).toISOString();
  if (db.run('INSERT OR IGNORE INTO stripe_events (id, type, received_at) VALUES (?, ?, ?)', event.id, event.type || '', at).changes) return true;
  return db.run('UPDATE stripe_events SET received_at = ? WHERE id = ? AND applied_at IS NULL AND received_at < ?', at, event.id, new Date(now - staleMs).toISOString()).changes > 0;
}

// A redirect target is accepted only when it parses and its origin IS the site's.
export function sameOriginUrl(candidate, origin) {
  if (!candidate) return null;
  let u; try { u = new URL(candidate); } catch { return null; }
  if (u.origin !== origin || (u.protocol !== 'https:' && u.protocol !== 'http:')) return null;
  return u.href;
}
// A Referer is accepted only as a plain local path: one leading slash, path characters, nothing else.
export function safeLocalPath(referer) {
  if (!referer) return null;
  let u; try { u = new URL(referer); } catch { return null; }
  return /^\/(?!\/)[A-Za-z0-9/._~-]*$/.test(u.pathname) ? u.pathname : null;
}

const corsHeaders = (cfg) => ({ 'Access-Control-Allow-Origin': cfg.siteOrigin, 'Access-Control-Allow-Methods': 'POST, OPTIONS', 'Access-Control-Allow-Headers': 'Content-Type', Vary: 'Origin' });

const API_INDEX = ['GET /api/snapshot', 'GET /api/analytics?days=7|30|90|365', 'GET /api/search?q=', 'GET /api/orders?status=&batch=&q=&tag=&from=&to=&discount=&sku=', 'GET /api/orders/:number', 'POST /api/orders {…, discount_code, tags}', 'POST /api/orders/:number/transition {to, carrier, tracking}', 'POST /api/orders/:number/refund {cents, reason}',
  'GET /api/orders/:number/events', 'POST /api/orders/:number/note {body}', 'POST /api/orders/:number/tags {tags}', 'GET /api/batches', 'GET /api/customers?q=&tag=&segment=&sort=', 'GET /api/customers/:id', 'POST /api/customers/:id/tags {tags}',
  'GET /api/products?days=', 'GET /api/products/:sku', 'GET /api/discounts', 'POST /api/discounts {code, kind, value, min_subtotal_cents, starts_at, ends_at, max_uses, note}', 'POST /api/discounts/:code/status {status}', 'GET /api/discounts/:code/evaluate?subtotal=&shipping=',
  'GET /api/tickets?status=&kind=&q=', 'GET /api/tickets/:number', 'POST /api/tickets {email, subject, body}', 'POST /api/tickets/:number/reply {body, close}', 'POST /api/mail/ingest {from, subject, text, messageId, inReplyTo}',
  'GET /api/contacts?q=', 'GET /api/inventory', 'POST /api/inventory/move {sku, delta, reason, ref}', 'GET /api/devices?q=', 'POST /api/devices {serial, mac, board, product_sku, fw_version, fw_sha256, batch}',
  'GET /api/books/pnl?from=&to=', 'POST /api/books/expense {date, account, cents, memo}', 'GET /api/exceptions', 'POST /api/exceptions/:key/resolve', 'GET /api/jobs', 'POST /api/jobs/:name',
  'GET /api/suggestions', 'GET /api/marketing?days=', 'POST /api/orders/:number/items {items: [{sku, qty}]}', 'GET /api/orders/:number/status-url', 'GET /api/purchasing?status=', 'POST /api/purchasing {supplier, supplier_url, expected_at, items: [{sku, qty, unit_cost_cents}]}', 'POST /api/purchasing/draft {skus}', 'POST /api/purchasing/:number/order|receive|cancel {expected_at | received: {sku: qty}}',
  'GET /api/recall?batch=&fw=&sku=', 'POST /api/recall {title, body, batch, fw, sku}', 'POST /api/recall/:number/send', 'POST /api/customers/:id/forget', 'GET /api/customers/:id/export', 'POST /api/invoices/:number/remind'];

export const plain = (cfg, title, bodyHtml) => layout({ title, body: `<p>${bodyHtml}</p><p><a href="${e(cfg.siteOrigin)}">← back to ${e(cfg.product || 'the site')}</a></p>`, company: cfg.company, nav: false, demo: cfg.demo });

function loginPage(cfg, err, ready, ok = null, demo = null) {
  const hint = demo ? `<div class="flash"><b>Demo mode — synthetic data, nothing leaves this machine.</b><br>The form is filled in: password <code>${e(demo.password)}</code>, authenticator code <code>${e(demo.code)}</code> right now (it changes every 30 s — reload if a login is refused). Just press <b>Log in</b>.<br><span class="muted">Every demo person shares that password and authenticator, so you can see what each role may do: ${demo.users.map(u => `<a href="/login?as=${encodeURIComponent(u.email)}">${e(u.name)}</a> (${e(u.role)})`).join(' · ')}.</span></div>` : '';
  return layout({ title: 'Log in', company: cfg.company, nav: false, demo: !!demo, demoResetMinutes: cfg.demoResetMinutes, flash: ok ? { text: ok } : null, body: hint + (ready
    ? `${err ? '<div class="flash err">Wrong password or code.' + (demo ? ' The code has probably rolled over — this page now carries the current one; press Log in again.' : '') + '</div>' : ''}<form method="post" action="/login" class="box"><label for="email">Email <span class="muted">(leave empty if you are the only person here)</span></label><input id="email" type="email" name="email" value="${demo ? e(demo.email) : ''}" autocomplete="username"><label for="pw">Password</label><input id="pw" type="${demo ? 'text' : 'password'}" name="password" value="${demo ? e(demo.password) : ''}" required autocomplete="current-password"><label for="code">Authenticator code</label><input id="code" name="code" value="${demo ? e(demo.code) : ''}" inputmode="numeric" pattern="[0-9]{6}" required autocomplete="one-time-code"><div style="margin-top:.8rem"><button>Log in</button></div></form>`
    : '<p>No operator yet. On the server, run <code>backoffice setup</code> to set the password and get the authenticator QR.</p>') });
}

function orderStatusPage(cfg, o) {
  const steps = ['paid', 'packed', 'shipped', 'delivered']; const i = steps.indexOf(o.status); const at = { paid: o.paid_at, packed: o.packed_at, shipped: o.shipped_at, delivered: o.delivered_at };
  const dl = deliveryLine(o, cfg); const turl = trackingUrl(o.carrier, o.tracking);
  const stepper = ['canceled', 'refunded'].includes(o.status) ? `<p><span class="pill">${e(o.status)}</span></p>` : `<ol class="steps">${steps.map((st, j) => `<li class="${j < i ? 'done' : j === i ? 'now' : ''}">${st[0].toUpperCase() + st.slice(1)}${at[st] && j <= i ? `<span class="when">${e(fmtDay(at[st]))}</span>` : ''}</li>`).join('')}</ol>`;
  return layout({ title: `Order ${o.number}`, company: cfg.company, nav: false, demo: cfg.demo, body: `${stepper}
<div class="card" style="max-width:720px"><div class="k muted" style="font-size:12px;text-transform:uppercase;letter-spacing:.06em">Where it is</div><p style="font-size:1.15rem;margin:.3rem 0 .2rem"><b>${e(dl.text)}</b></p>${dl.estimate ? `<p class="muted" style="margin:0">An estimate from the ship date and the distance, not the carrier's word; ${e(dl.estimate.carrier)} usually takes ${dl.estimate.min_days}–${dl.estimate.max_days} business days on this route.</p>` : dl.carrier ? `<p class="muted" style="margin:0">From the carrier${o.tracking_detail ? `: ${e(o.tracking_detail)}` : ''}${o.tracking_checked_at ? `, checked ${e(fmtDt(o.tracking_checked_at))}` : ''}.</p>` : ''}${turl ? `<p style="margin:.6rem 0 0">${e(o.carrier || 'USPS')} <a href="${e(turl)}" rel="noopener">${e(o.tracking)}</a> <span class="muted">(opens the carrier's site)</span></p>` : ''}</div>
<h2>In the box</h2><table style="max-width:720px"><thead><tr><th>Item</th><th class="num">Qty</th></tr></thead><tbody>${o.items.map(it => `<tr><td>${e(it.name)}</td><td class="num">${it.qty}</td></tr>`).join('')}</tbody></table>
<h2>Shipping to</h2><p>${e(o.ship_name)}<br>${e(o.ship_line1)}${o.ship_line2 ? '<br>' + e(o.ship_line2) : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)}</p>
<p class="muted">Anything wrong, missing or broken: reply to any email from us — a person reads every one, and we fix rather than ask you to send things back. Your Canary provisions itself at home: power it on and point your phone's camera at the QR card in the box.</p><p><a href="${e(cfg.siteOrigin)}">← ${e(cfg.product || 'the site')}</a></p>` });
}

function statusPage(db, cfg) {
  const s = publicStatus(db);
  const rows = s.probes.map(p => `<tr><td>${e(p.name)}</td><td class="${p.ok === false ? 'bad' : p.ok ? 'ok' : 'muted'}">${p.ok == null ? 'not checked yet' : p.ok ? '● operational' : '● down'}</td><td class="num">${p.uptime_90d == null ? '—' : p.uptime_90d + '%'}</td><td class="muted">${e(p.checked_at || '')}</td></tr>`).join('');
  const inc = s.incidents.map(i => `<div class="msg"><div class="meta">${e(i.started_at)} · ${e(i.status)}${i.resolved_at ? ` · resolved ${e(i.resolved_at)}` : ''}</div><b>${e(i.title)}</b>\n${e(i.body || '')}</div>`).join('') || '<p class="muted">No incidents on record.</p>';
  return layout({ title: `${cfg.product || 'SecuraCV'} status — ${s.overall}`, company: cfg.company, nav: false, demo: cfg.demo, body: `<p class="${s.overall === 'operational' ? 'ok' : 'bad'}"><b>${s.overall === 'operational' ? 'All systems operational' : 'Something is degraded'}</b> · checked from one probe, every few minutes · <a href="/status.json">JSON</a></p>
<table><thead><tr><th>Service</th><th>Now</th><th class="num">90-day uptime</th><th>Last check (UTC)</th></tr></thead><tbody>${rows}</tbody></table><h2>Incidents</h2>${inc}
<p class="muted">Nothing on this page phones home: a Canary on your wall talks to your hub, not to us, so an outage here never touches a device.</p>` });
}

function invoicePage(cfg, inv) {
  const rows = inv.items.map(i => `<tr><td>${e(i.description)}${i.sku ? ` <span class="muted">(${e(i.sku)})</span>` : ''}</td><td class="num">${i.qty}</td><td class="num">${(i.unit_cents / 100).toFixed(2)}</td><td class="num">${(i.qty * i.unit_cents / 100).toFixed(2)}</td></tr>`).join('');
  const money = (c) => `$${(c / 100).toFixed(2)}`;
  return layout({ title: `Invoice ${inv.number}`, company: cfg.company, nav: false, demo: cfg.demo, body: `<p><b>${e(cfg.company)}</b> · ${e(cfg.supportAddr || '')}</p>
<div class="row"><div><b>Bill to</b><br>${e(inv.bill_to || inv.contact_name || inv.email || '').replace(/\n/g, '<br>')}</div><div><b>Invoice</b> ${e(inv.number)}<br>${inv.po_number ? `<b>PO</b> ${e(inv.po_number)}<br>` : ''}<b>Issued</b> ${e(inv.issued_at || '—')}<br><b>Due</b> ${e(inv.due_at || '—')} (net ${inv.terms_days})<br><b>Status</b> ${e(inv.status)}</div></div>
<table><thead><tr><th>Item</th><th class="num">Qty</th><th class="num">Unit</th><th class="num">Amount</th></tr></thead><tbody>${rows}</tbody>
<tfoot><tr><td colspan="3" class="num">Subtotal</td><td class="num">${money(inv.subtotal_cents)}</td></tr><tr><td colspan="3" class="num">Shipping</td><td class="num">${money(inv.shipping_cents)}</td></tr><tr><td colspan="3" class="num">Tax</td><td class="num">${money(inv.tax_cents)}</td></tr><tr><td colspan="3" class="num"><b>Total (USD)</b></td><td class="num"><b>${money(inv.total_cents)}</b></td></tr></tfoot></table>
<p>${e(inv.notes || '')}</p><p class="muted">Pay by ACH or check to the remittance details ${cfg.remittance ? 'below' : 'in the covering email'}.</p>${cfg.remittance ? `<pre>${e(cfg.remittance)}</pre>` : ''}<p class="muted">Print this page for a PDF.</p>` });
}
