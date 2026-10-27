// The first order, from nothing. A business that has never sold anything
// walks its first customer through the whole chain: parts on the shelf,
// boards flashed, a question answered, a Checkout sale, the bench, the
// label, the carrier, the follow-up, a warranty fix, a refund, a payout,
// the week's end. Each step runs the REAL code path (the signed webhook,
// the public forms, the bench's own transition with its checks, the
// carrier poller fed a synthetic carrier) against an empty database, and
// says what it did, where to look, and what it checked.
//
// Two readers, one list. The demo page (/demo/first-order) runs it a step
// at a time so each page can be watched changing; tests/first_order.test.mjs
// runs it end to end and holds every check. A new flow is a new step here —
// add it with its checks and both readers get it. `backoffice first-order`
// runs it from the shell.
import { nowIso } from '../db.mjs';
import { DEMO_SHIPPING, DEMO_CATALOG, DEMO_USERS, seedDemoUsers, localBase, isDemo, isEmpty } from './demo.mjs';
import { packOrder } from './packing.mjs';
import { signStripePayload } from './stripe.mjs';
import { createPo, markOrdered, receivePo, poById, draftReorder, listPos } from './purchasing.mjs';
import { upsertItem, move, setRecipe, item } from './inventory.mjs';
import { importManifest, assignToOrder, deviceBySerial } from './devices.mjs';
import { orderByNumber, orderById, transition, importLabels, timeline, boardsNeeded, packPlanOf, recordOrder } from './orders.mjs';
import { ticketByNumber, reply } from './support.mjs';
import { openClaim, reship, resolveClaim, claimById } from './warranty.mjs';
import { sendOutbox } from './mail.mjs';
import { pollTracking, resetTokenCache } from './shipping.mjs';
import { orderStatusUrl } from './delivery.mjs';
import { trialBalance } from './ledger.mjs';
import { openExceptions } from './exceptions.mjs';
import { exportContact } from './privacy.mjs';
import { runAutomations, seedAutomations } from './automations.mjs';
import { doctor } from './health.mjs';
import { customerStats } from './contacts.mjs';
import { createCheckout, estimateForZip, storefrontCatalog } from './storefront.mjs';
import { normalize } from './catalog.mjs';

// The one customer. A placeholder name on a reserved domain, like everything in the demo.
export const FIRST = { email: 'first.customer@customer.example.test', name: 'First Customer', line1: '1 Example Lane', city: 'Madison', state: 'WI', zip: '53703' };
const SERIALS = ['FIRST-W-0001', 'FIRST-W-0002', 'FIRST-W-0003'];
const TRACKING = ['9400 1000 0000 0000 0001 01', '9400 1000 0000 0000 0001 02'];
const silent = (cfg) => ({ ...cfg, githubToken: '', smtpUrl: '' });

// Day zero: the demo people and the demo flag, nothing else. The guard is resetDemo's: a database
// that is neither flagged as a demo nor empty is never wiped from here.
export function startEmpty(db) {
  if (!isDemo(db) && !isEmpty(db)) throw new Error('refusing to empty: this database is not flagged as a demo and is not empty');
  const tables = ['attachments', 'purchase_order_items', 'purchase_orders', 'recalls', 'ticket_messages', 'tickets', 'warranty_claims', 'order_events', 'order_items', 'orders', 'discounts', 'invoice_items', 'invoices', 'subscriptions', 'inventory_moves', 'recipes', 'inventory_items', 'devices', 'waitlist', 'contacts', 'mail_outbox', 'mail_seen', 'campaigns', 'probe_results', 'probes', 'incidents', 'ledger', 'exceptions', 'canned_replies', 'stripe_events', 'job_runs', 'audit_log', 'checkout_sessions'];
  for (const t of ['automation_runs']) if (db.get("SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?", t)) tables.push(t);
  db.exec('PRAGMA foreign_keys = OFF');
  try { db.tx(() => { for (const t of tables) db.run(`DELETE FROM ${t}`); db.run("DELETE FROM settings WHERE key LIKE 'seq:%' OR key LIKE 'walkthrough:%' OR key LIKE 'backup:%' OR key LIKE 'drill:%' OR key LIKE 'reconcile:%'"); }); }
  finally { db.exec('PRAGMA foreign_keys = ON'); }
  db.setSetting('demo', '1'); db.setSetting('demo:seeded_at', nowIso()); db.setSetting('demo:empty', '1');
  seedDemoUsers(db); seedAutomations(db);
  return { orders: 0, contacts: 0, users: DEMO_USERS.length };
}

// Each step: what it does in one line (`tell`), and run(ctx) → { facts, links, checks }. `ctx.state`
// carries what later steps need (the order number, the serials, the ticket) and is persisted.
const check = (label, ok, detail = '') => ({ label, ok: !!ok, detail: String(detail || '') });
const money = (c) => `$${(c / 100).toFixed(2)}`;

async function postWebhook(ctx, event) {
  const body = JSON.stringify(event);
  const r = await fetch(`${localBase(ctx.cfg)}/webhooks/stripe`, { method: 'POST', headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(body, ctx.cfg.stripeWebhookSecret) }, body });
  return { status: r.status, ...(await r.json().catch(() => ({}))) };
}
const checkoutEvent = (id, { email, name, items, subtotal, shipping, tax, line1, city, state, zip }) => ({ id: `evt_first_${id}`, type: 'checkout.session.completed', data: { object: { id: `cs_first_${id}`, payment_intent: `pi_first_${id}`, payment_status: 'paid', currency: 'usd', amount_subtotal: subtotal, amount_total: subtotal + shipping + tax, created: Math.floor(Date.now() / 1000),
  total_details: { amount_shipping: shipping, amount_tax: tax }, customer_details: { email, name }, shipping_details: { name, address: { line1, city, state, postal_code: zip, country: 'US' } },
  line_items: { data: items.map(it => ({ description: it.name, quantity: it.qty, price: { unit_amount: it.unit_cents, product: { metadata: { sku: it.sku } } } })) } } } });

// The bench's pack, exactly as the form posts it: the lines ticked, the board scanned, the scale read.
function packAtBench(ctx, order, serial) {
  const { db, cfg } = ctx;
  assignToOrder(db, serial, order.id, { actor: 'web' });
  const expected = packOrder(DEMO_SHIPPING, order.items).total_oz;   // what the scale should read; the bench's own number
  const packed = transition(db, order.id, 'packed', { actor: 'web', cfg, shipping: DEMO_SHIPPING, check: { ticked: order.items.map(i => i.sku), measuredOz: expected == null ? null : expected + 0.2 } });
  return orderById(db, packed.id);
}

export const STEPS = [
  { id: 'day_zero', title: 'Day zero', tell: 'Nothing has happened yet: no orders, no customers, no parts. The books are empty and balanced.',
    run: async ({ db }) => {
      const orders = db.get('SELECT COUNT(*) n FROM orders').n, contacts = db.get('SELECT COUNT(*) n FROM contacts').n, parts = db.get('SELECT COUNT(*) n FROM inventory_items').n;
      const tb = trialBalance(db);
      return { facts: { orders, contacts, parts, people: db.get('SELECT COUNT(*) n FROM users WHERE status = \'active\'').n }, links: [['Home', '/'], ['Health', '/health']],
        checks: [check('no orders', orders === 0), check('no customers', contacts === 0), check('no parts on the shelf', parts === 0), check('books balanced (at zero)', tb.balanced)] };
    } },

  { id: 'shelf', title: 'The first parts land', tell: 'A purchase order to the distributor is received into stock; the printed shells and packaging are counted in; the kit\'s recipe is written down.',
    run: async ({ db, state }) => {
      const parts = [['xiao-esp32s3', 'XIAO ESP32S3', 'part', 750, 'Example Distributor'], ['cable-usbc-1m', 'USB-C cable, 1 m', 'part', 140, 'Example Distributor'], ['sd-32gb', 'microSD 32 GB', 'part', 690, 'Example Distributor'], ['hardware-pack-wap', 'Fastener pack, WAP', 'part', 45, 'Example Distributor'],
        ['wap-shell-petg', 'WAP shell + lid, PETG (printed)', 'printed', 180, 'in-house'], ['box-small', 'Shipping box, small', 'packaging', 62, 'Example Packaging Co.'], ['qr-card', 'First-boot QR card', 'packaging', 8, 'in-house']];
      for (const [sku, name, kind, cost, supplier] of parts) upsertItem(db, { sku, name, kind, on_hand: 0, reorder_point: 3, reorder_qty: 10, unit_cost_cents: cost, supplier });
      const po = createPo(db, { supplier: 'Example Distributor', items: [{ sku: 'xiao-esp32s3', qty: 5 }, { sku: 'cable-usbc-1m', qty: 10 }, { sku: 'sd-32gb', qty: 5 }, { sku: 'hardware-pack-wap', qty: 10 }], notes: 'The first boards.', actor: 'web' });
      markOrdered(db, po.id, { actor: 'web' }); receivePo(db, po.id, { actor: 'web' });
      for (const [sku, n] of [['wap-shell-petg', 5], ['box-small', 10], ['qr-card', 20]]) move(db, sku, n, 'receive', 'first-batch');
      setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 });
      setRecipe(db, 'wap-parts', { 'wap-shell-petg': 1, 'hardware-pack-wap': 1 });
      state.po = po.number; const tb = trialBalance(db); const inv = tb.rows.find(r => r.account === 'assets:inventory')?.balance_cents || 0;
      return { facts: { purchase_order: po.number, boards_on_hand: item(db, 'xiao-esp32s3').on_hand, inventory_value: money(inv) }, links: [['Inventory', '/inventory'], ['Purchasing', '/purchasing'], ['Books', '/books']],
        checks: [check('the PO is received', poById(db, po.id).status === 'received'), check('5 boards on the shelf', item(db, 'xiao-esp32s3').on_hand === 5), check('inventory is on the books', inv > 0, money(inv)), check('books balanced', tb.balanced)] };
    } },

  { id: 'boards', title: 'The first boards are flashed', tell: 'The provisioning jig\'s manifest registers three serials, flashed for the WAP kit, firmware 2.5.0.',
    run: async ({ db, state }) => {
      const n = importManifest(db, { version: 1, devices: SERIALS.map((serial, i) => ({ serial, mac: `02:00:00:00:10:0${i + 1}`, firmware_version: '2.5.0', firmware_sha256: 'ab'.repeat(32), flashed_at: nowIso() })) }, { batch: 'FIRST-BATCH', product_sku: 'wap-kit', board: 'xiao-esp32s3' });
      state.serials = SERIALS;
      return { facts: { registered: n, serials: SERIALS.join(', ') }, links: [['Devices', '/devices'], ['Print the labels', '/devices/labels?status=stock']],
        checks: [check('three boards registered', n === 3), check('in stock, in no box', SERIALS.every(s => { const d = deviceBySerial(db, s); return d && d.status === 'stock' && !d.order_id; }))] };
    } },

  { id: 'question', title: 'The first customer asks a question', tell: 'The website\'s contact form posts to this backoffice: a ticket opens, the customer gets an acknowledgment, the sender becomes a contact.',
    run: async (ctx) => {
      const { db, state } = ctx;
      const r = await fetch(`${localBase(ctx.cfg)}/forms/contact`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: FIRST.email, name: FIRST.name, subject: 'Does the WAP kit need a hub?', message: 'I have one door and no Home Assistant. Will it work on its own?\n\n(the first customer, synthetic)' }) });
      const j = await r.json().catch(() => ({}));
      const t = db.get('SELECT number, status, kind FROM tickets ORDER BY id DESC LIMIT 1'); state.ticket = t?.number;
      const contact = db.get('SELECT id, email FROM contacts WHERE email = ?', FIRST.email); const ack = db.get('SELECT subject FROM mail_outbox WHERE to_addr = ? ORDER BY id DESC LIMIT 1', FIRST.email);
      return { facts: { form_status: r.status, ticket: t?.number, kind: t?.kind, acknowledgment: ack?.subject || '(none)' }, links: [['Support queue', '/tickets'], ['The customer', contact ? `/contacts/${contact.id}` : '/customers']],
        checks: [check('the form was accepted', r.status < 300, `${r.status} ${JSON.stringify(j).slice(0, 80)}`), check('a ticket is in the queue', !!t && ['open', 'waiting_on_us'].includes(t.status), t?.status), check('the sender is a contact', !!contact), check('an acknowledgment is queued', !!ack)] };
    } },

  { id: 'answer', title: 'We answer', tell: 'A reply from the support desk goes out; the ticket waits on the customer.',
    run: async ({ db, cfg, state }) => {
      const t = ticketByNumber(db, state.ticket); reply(db, t.id, 'Yes — the WAP kit runs on its own; a hub only adds the Witness Wall. Power it on, point your phone at the QR card, done.\n\n(synthetic reply)', { cfg: silent(cfg), actor: 'web' });
      const after = ticketByNumber(db, state.ticket); const mail = db.get('SELECT subject FROM mail_outbox WHERE to_addr = ? AND subject LIKE ? ORDER BY id DESC LIMIT 1', FIRST.email, `%${state.ticket}%`);
      return { facts: { ticket: state.ticket, status: after.status, mail: mail?.subject || '(none)' }, links: [['The ticket', `/tickets/${t.id}`]],
        checks: [check('the ticket waits on the customer', after.status === 'waiting_on_customer', after.status), check('the reply is queued to the customer', !!mail)] };
    } },

  { id: 'news', title: 'They ask for news: double opt-in', tell: 'The website\'s newsletter form records the ask and mails a confirmation link; nothing marketing goes out until that link is clicked. Then it is.',
    run: async (ctx) => {
      const { db } = ctx; const base = localBase(ctx.cfg);
      const r = await fetch(`${base}/forms/newsletter`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: FIRST.email, name: FIRST.name }) });
      const j = await r.json().catch(() => ({})); const before = db.get('SELECT consent_marketing FROM contacts WHERE email = ?', FIRST.email)?.consent_marketing;
      const mail = db.get("SELECT subject, body_text FROM mail_outbox WHERE to_addr = ? AND body_text LIKE '%/confirm/%' ORDER BY id DESC LIMIT 1", FIRST.email);
      const link = (mail?.body_text || '').match(/https?:\/\/\S+\/confirm\/[A-Za-z0-9_-]+/)?.[0] || '';
      const clicked = link ? await fetch(link.replace(ctx.cfg.baseUrl, base)) : null; const after = db.get('SELECT consent_marketing, consent_at FROM contacts WHERE email = ?', FIRST.email);
      return { facts: { form: `${r.status} ${j.newsletter || ''}`, confirmation_mail: mail?.subject || '(none)', link_clicked: clicked ? clicked.status : 'no link', consent: after?.consent_marketing ? `confirmed ${after.consent_at}` : 'not confirmed' }, links: [['The customer', `/customers?q=${encodeURIComponent(FIRST.email)}`], ['Mail', '/mail']],
        checks: [check('the form was accepted', r.status < 300, String(r.status)), check('a confirmation mail is queued with a link', !!link), check('no consent until the click', before === 0), check('the click confirms consent', after?.consent_marketing === 1), check('the consent is audited', !!db.get("SELECT 1 FROM audit_log WHERE action = 'contact.consent'"))] };
    } },

  { id: 'order', title: 'They buy: the cart becomes a Checkout Session, Stripe completes it', tell: 'The site asks for the estimate at their ZIP, then sends the cart here; a hosted Checkout Session is created with the batch\'s ship date and the arrival window as Stripe\'s own delivery estimate (Stripe is stood in for by a stub). Stripe\'s signed checkout.session.completed webhook then records the order once — its retry is applied once too — the customer gets "we have it", and the sale is on the books.',
    run: async (ctx) => {
      const { db, cfg, state } = ctx; const kit = DEMO_CATALOG.products.get('wap-kit');
      const est = estimateForZip(FIRST.zip, { cfg }); const cat = storefrontCatalog(db, DEMO_CATALOG, { cfg });
      const before = cat.products.find(p => p.sku === 'wap-kit');
      // Stripe, stood in: the stub records what was asked and answers a session, as the API would
      let asked = null; const stripe = async (url, init) => { asked = { url, body: Object.fromEntries(new URLSearchParams(init.body)) }; return { ok: true, json: async () => ({ id: 'cs_first_0001', url: 'https://checkout.stripe.com/c/pay/cs_first_0001' }) }; };
      const co = await createCheckout(db, { ...cfg, stripeApiKey: 'sk_test_synthetic' }, DEMO_CATALOG, { items: [{ sku: 'wap-kit', qty: 1 }], zip: FIRST.zip, email: FIRST.email }, { fetchImpl: stripe });
      // the store's FCC gate, on a catalog that carries one (the demo's own does not model authorization)
      const gated = normalize({ mode: 'live', shipping_flat: 8, groups: [{ products: [{ id: 'vision-kit', name: 'Canary Vision Kit', price: 119, authorization: 'pending-fcc' }] }] });
      let refusedFcc = ''; try { await createCheckout(db, { ...cfg, stripeApiKey: 'sk_test_synthetic' }, gated, { items: [{ sku: 'vision-kit', qty: 1 }] }, { fetchImpl: stripe }); } catch (err) { refusedFcc = err.message; }
      const ev = checkoutEvent('0001', { ...FIRST, items: [{ sku: 'wap-kit', name: kit.name, qty: 1, unit_cents: kit.price_cents }], subtotal: kit.price_cents, shipping: 800, tax: 0 });
      const first = await postWebhook(ctx, ev); const again = await postWebhook(ctx, ev);
      const o = db.get('SELECT * FROM orders ORDER BY id DESC LIMIT 1'); state.order = o?.number;
      const items = o ? db.all('SELECT sku, qty FROM order_items WHERE order_id = ?', o.id) : []; const tb = trialBalance(db); const cs = o?.contact_id ? customerStats(db, o.contact_id) : null;
      const mail = db.get("SELECT subject, body_text FROM mail_outbox WHERE to_addr = ? AND subject LIKE 'We have your%' ORDER BY id DESC LIMIT 1", FIRST.email);
      const after = storefrontCatalog(db, DEMO_CATALOG, { cfg }).products.find(p => p.sku === 'wap-kit');
      return { facts: { estimate: est.label, available_before: before?.available, checkout_url: co.url, shipping_option: asked?.body['shipping_options[0][shipping_rate_data][display_name]'], delivery_estimate_days: `${asked?.body['shipping_options[0][shipping_rate_data][delivery_estimate][minimum][value]']}–${asked?.body['shipping_options[0][shipping_rate_data][delivery_estimate][maximum][value]']}`, fcc_refusal: refusedFcc.slice(0, 80), order: o?.number, total: o ? money(o.total_cents) : null, first_delivery: first.status, redelivery: `${again.status} ${again.duplicate ? 'duplicate' : ''}`, confirmation_mail: mail?.subject || '(none)', available_after: after?.available },
        links: [['The order', o ? `/orders/${o.id}` : '/orders'], ['Pack bench', o ? `/pack?order=${o.id}` : '/pack'], ['Books', '/books'], ['Mail', '/mail']],
        checks: [check('the estimate names the ship date and the arrival window', !!est.delivers && /ships .* arrives/.test(est.label), est.label), check('the session carried the kit as a line with its sku', asked?.body['line_items[0][price_data][product_data][metadata][sku]'] === 'wap-kit' && asked?.body['line_items[0][quantity]'] === '1'),
          check('the shipping option carries the ship date and the delivery estimate', /ships/.test(asked?.body['shipping_options[0][shipping_rate_data][display_name]'] || '') && asked?.body['shipping_options[0][shipping_rate_data][delivery_estimate][minimum][unit]'] === 'business_day'), check('the success URL carries the session id back to the site', /session_id=\{CHECKOUT_SESSION_ID\}/.test(asked?.body.success_url || '')),
          check('a kit pending FCC authorization is refused at checkout', /not for sale until its FCC authorization/.test(refusedFcc), refusedFcc), check('the webhook was accepted', first.status === 200, String(first.status)), check('one order, paid', !!o && o.status === 'paid' && db.get('SELECT COUNT(*) n FROM orders').n === 1), check('the line is the kit', items.length === 1 && items[0].sku === 'wap-kit' && items[0].qty === 1),
          check('the same event twice is one order', db.get('SELECT COUNT(*) n FROM orders').n === 1 && again.status === 200), check('the customer has one order', cs?.orders === 1), check('"we have your order" is queued with the ship date and the window', !!mail && mail.body_text.includes(est.ships.ships_label) && mail.body_text.includes('/o/'), mail?.body_text?.slice(0, 80)), check('the batch has one fewer to sell', before?.available != null && after?.available === before.available - 1, `${before?.available} → ${after?.available}`), check('books balanced', tb.balanced)] };
    } },

  { id: 'pack', title: 'Packed at the bench', tell: 'The bench refuses an unticked line, then the board is scanned in, every line ticked, the box weighed, and the pack goes through: parts leave the shelf, the plan is frozen, the timeline says who checked what.',
    run: async (ctx) => {
      const { db, cfg, state } = ctx; const o = orderByNumber(db, state.order); const before = item(db, 'xiao-esp32s3').on_hand;
      let refused = ''; try { transition(db, o.id, 'packed', { actor: 'web', cfg, shipping: DEMO_SHIPPING, check: { ticked: [] } }); } catch (err) { refused = err.message; }
      const need = boardsNeeded(db, o).needed;
      const packed = packAtBench(ctx, o, state.serials[0]); const plan = packPlanOf(packed); const ev = timeline(db, o.id).find(e => e.kind === 'packed');
      return { facts: { refused_first: refused.slice(0, 90), boards_needed: need, board: state.serials[0], box: plan?.boxes?.[0]?.box_name, expected_oz: plan?.total_oz, boards_left_on_shelf: item(db, 'xiao-esp32s3').on_hand },
        links: [['The order', `/orders/${o.id}`], ['Ship bench', `/ship?order=${o.id}`], ['Packing slip', `/orders/${o.id}/packing-slip`]],
        checks: [check('an unticked line is refused', /not every line is ticked/.test(refused), refused), check('the kit needs one board', need === 1), check('packed', packed.status === 'packed'), check('the plan is frozen with a box', !!plan?.boxes?.length), check('one board left the shelf', item(db, 'xiao-esp32s3').on_hand === before - 1), check('the board is in the box', deviceBySerial(db, state.serials[0]).order_id === o.id), check('the timeline says every line ticked and the scale', !!ev && /every line ticked/.test(ev.text) && /scale/.test(ev.text), ev?.text)] };
    } },

  { id: 'label', title: 'The label is bought; the batch ships from the manifest', tell: 'The website\'s label tool wrote manifest.json with the tracking number and the rate. Pasted at the ship bench, the order ships: tracking, postage on the books, the customer\'s mail queued.',
    run: async ({ db, cfg, state }) => {
      const r = importLabels(db, [{ order_id: state.order, tracking: TRACKING[0], rate: 4.8, status: 'labeled' }], { cfg: silent(cfg), actor: 'web' });
      const o = orderByNumber(db, state.order); state.tracking = o.tracking; const mail = db.get('SELECT subject, body_text FROM mail_outbox WHERE ref = ? ORDER BY id DESC LIMIT 1', `order:${state.order}`); const tb = trialBalance(db);
      return { facts: { shipped: r.shipped.join('; '), problems: r.problems.join('; ') || 'none', tracking: o.tracking, postage: money(o.postage_cents), mail: mail?.subject }, links: [['The order', `/orders/${o.id}`], ['Mail outbox', '/mail'], ['Books', '/books']],
        checks: [check('shipped from the manifest', o.status === 'shipped' && r.shipped.length === 1), check('tracking on the order', o.tracking === TRACKING[0].replace(/\s/g, '')), check('postage booked', o.postage_cents === 480 && tb.rows.find(x => x.account === 'expenses:postage')?.balance_cents === 480), check('the tracking mail is queued with the number', !!mail && mail.body_text.includes(o.tracking)), check('books balanced', tb.balanced)] };
    } },

  { id: 'outbox', title: 'The mail goes out', tell: 'The outbox is delivered. In the demo the transport marks each mail sent and nothing leaves the machine; in production this is SMTP.',
    run: async ({ db, cfg }) => {
      const r = await sendOutbox(db, silent(cfg)); const left = db.get("SELECT COUNT(*) n FROM mail_outbox WHERE status = 'queued'").n; const sent = db.get("SELECT COUNT(*) n FROM mail_outbox WHERE status = 'sent'").n;
      return { facts: { sent: r.sent ?? sent, still_queued: left, suppressed: r.suppressed || 0 }, links: [['Mail', '/mail']], checks: [check('every queued mail went', left === 0, `${left} left`), check('the customer got the acknowledgment, the reply and the tracking', sent >= 3, `${sent} sent`)] };
    } },

  { id: 'delivered', title: 'The carrier says delivered', tell: 'The hourly tracking poll asks the carrier (a synthetic USPS here) and the carrier says delivered: the order moves, the customer\'s status page says so.',
    run: async (ctx) => {
      const { db, cfg, state } = ctx; const o = orderByNumber(db, state.order);
      db.run("UPDATE orders SET shipped_at = ?, tracking_checked_at = NULL WHERE id = ?", new Date(Date.now() - 3 * 86400000).toISOString(), o.id);
      const usps = { ...silent(cfg), uspsClientId: 'first-order-demo', uspsClientSecret: 'synthetic', uspsApiBase: 'https://usps.example.test' };
      const fetchImpl = async (url) => /oauth2/.test(url) ? { ok: true, json: async () => ({ access_token: 'synthetic', expires_in: 3600 }) } : { ok: true, json: async () => ({ status: 'Delivered', statusCategory: 'Delivered', statusSummary: 'Your item was delivered in or at the mailbox.', trackingEvents: [{ eventTimestamp: nowIso(), eventCity: FIRST.city, eventState: FIRST.state }] }) };
      resetTokenCache(); const r = await pollTracking(db, usps, { fetchImpl }); resetTokenCache();
      const after = orderByNumber(db, state.order); const page = await fetch(orderStatusUrl(cfg, after).replace(cfg.baseUrl, localBase(cfg))); const text = await page.text();
      return { facts: { polled: r.checked, delivered: r.delivered, status: after.status, status_page: page.status }, links: [['The order', `/orders/${after.id}`], ['The customer\'s status page', orderStatusUrl(cfg, after).replace(cfg.baseUrl, '')]],
        checks: [check('the carrier was asked once', r.checked === 1), check('delivered', after.status === 'delivered'), check('the status page says delivered', page.status === 200 && /Delivered/.test(text)), check('no carrier alert open', !openExceptions(db).some(e => e.key === `tracking:${state.order}`))] };
    } },

  { id: 'settled', title: 'Three days later: settled in?', tell: 'The automation that follows a delivery by 72 hours fires once the day has come, and never twice.',
    run: async ({ db, cfg, state }) => {
      const o = orderByNumber(db, state.order); db.run('UPDATE orders SET delivered_at = ? WHERE id = ?', new Date(Date.now() - 4 * 86400000).toISOString(), o.id);
      const r1 = runAutomations(db, silent(cfg)); const r2 = runAutomations(db, silent(cfg));
      const mail = db.get("SELECT subject FROM mail_outbox WHERE to_addr = ? AND subject LIKE '%settled%' ORDER BY id DESC", FIRST.email); const n = db.get("SELECT COUNT(*) n FROM mail_outbox WHERE to_addr = ? AND subject LIKE '%settled%'", FIRST.email).n;
      return { facts: { first_run: JSON.stringify(r1).slice(0, 80), second_run: JSON.stringify(r2).slice(0, 80), mail: mail?.subject || '(none)' }, links: [['Automations', '/automations'], ['Mail', '/mail']],
        checks: [check('the follow-up is queued', !!mail), check('once, not twice', n === 1, `${n}`)] };
    } },

  { id: 'warranty', title: 'A board dies; we fix it, nothing comes back', tell: 'A warranty claim opens against the order and its serial. The defined answer ships a replacement: a $0 order through the same bench and the same manifest, the claim resolved with its cost.',
    run: async (ctx) => {
      const { db, cfg, state } = ctx;
      const claim = openClaim(db, { orderNumber: state.order, email: FIRST.email, serial: state.serials[0], category: 'dead_board', description: 'No LED, no USB. Two cables tried.', cfg: silent(cfg), actor: 'web' }); state.claim = claim.number;
      const shipped = reship(db, claim.id, [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 1 }], { recordOrderFn: recordOrder, catalog: DEMO_CATALOG, actor: 'web' });
      const re = orderById(db, shipped.reship_order_id); state.reship = re.number;
      packAtBench(ctx, re, state.serials[1]);
      importLabels(db, [{ order_id: re.number, tracking: TRACKING[1], rate: 4.8, status: 'labeled' }], { cfg: silent(cfg), actor: 'web' });
      resolveClaim(db, claim.id, { costCents: 1090, note: `Replacement ${state.serials[1]} shipped.`, cfg: silent(cfg), actor: 'web' });
      const c = claimById(db, claim.id); const reAfter = orderById(db, re.id); const t = ticketByNumber(db, db.get('SELECT number FROM tickets WHERE id = ?', c.ticket_id).number);
      return { facts: { claim: c.number, status: c.status, cost: money(c.cost_cents), replacement_order: reAfter.number, replacement_total: money(reAfter.total_cents), replacement_board: state.serials[1], tracking: reAfter.tracking }, links: [['The claim', `/warranty/${c.id}`], ['The replacement order', `/orders/${re.id}`]],
        checks: [check('the claim is resolved', c.status === 'resolved'), check('the replacement is a $0 order, shipped', reAfter.total_cents === 0 && reAfter.status === 'shipped'), check('the second board went into it', deviceBySerial(db, state.serials[1]).order_id === re.id), check('the claim\'s ticket closed', t.status === 'closed'), check('the customer was told', !!db.get('SELECT 1 FROM mail_outbox WHERE ref = ?', `warranty:${c.number}`)), check('books balanced', trialBalance(db).balanced)] };
    } },

  { id: 'refund', title: 'Stripe reports a partial refund', tell: 'The shipping was refunded in the Stripe dashboard; the charge.refunded webhook writes it here, on the order and in the books.',
    run: async (ctx) => {
      const { db, state } = ctx; const o = orderByNumber(db, state.order);
      const r = await postWebhook(ctx, { id: 'evt_first_refund', type: 'charge.refunded', data: { object: { id: 'ch_first_0001', payment_intent: o.payment_intent, amount_refunded: 800 } } });
      const after = orderByNumber(db, state.order); const tb = trialBalance(db);
      return { facts: { webhook: r.status, refunded: money(after.refunded_cents), status: after.status }, links: [['The order', `/orders/${o.id}`], ['Books', '/books']],
        checks: [check('the refund is on the order', after.refunded_cents === 800), check('a partial refund keeps the order delivered', after.status === 'delivered', after.status), check('books balanced', tb.balanced)] };
    } },

  { id: 'payout', title: 'Stripe pays out to the bank', tell: 'The payout.paid webhook moves the money from Stripe\'s balance to the bank account on the books.',
    run: async (ctx) => {
      const { db } = ctx; const r = await postWebhook(ctx, { id: 'evt_first_payout', type: 'payout.paid', data: { object: { id: 'po_first_0001', amount: 7000, arrival_date: Math.floor(Date.now() / 1000) } } });
      const tb = trialBalance(db); const bank = tb.rows.find(x => x.account === 'assets:bank')?.balance_cents;
      return { facts: { webhook: r.status, bank: money(bank || 0) }, links: [['Books', '/books']], checks: [check('the payout was accepted', r.status === 200), check('books balanced', tb.balanced)] };
    } },

  { id: 'week_end', title: 'The week closes', tell: 'Every scheduled job runs once (backup, stock, SLA, the digest). Two kits left the shelf, so the stock job flags the parts at their reorder point; the Home page\'s "draft the reorder" covers them and the next run clears the flags. The doctor checks the database and the books; the customer\'s own export shows what we hold.',
    run: async (ctx) => {
      const { db, cfg } = ctx; const { JOBS, runJob } = await import('../jobs.mjs');
      const jobs = {}; for (const name of Object.keys(JOBS)) { try { jobs[name] = await runJob(ctx.app, name); } catch (err) { jobs[name] = { error: err.message }; } }
      const low = openExceptions(db).filter(e => e.key.startsWith('stock:')).map(e => e.key.slice(6));
      const drafted = draftReorder(db, { actor: 'web' }); const drafts = listPos(db, { status: 'draft' });
      await runJob(ctx.app, 'stock'); const ex = openExceptions(db);
      const h = doctor(db, silent(cfg)); const by = Object.fromEntries(h.checks.map(c => [c.key, c]));
      const exp = exportContact(db, FIRST.email); const audit = db.get('SELECT COUNT(*) n FROM audit_log').n;
      return { facts: { jobs_run: Object.keys(jobs).length, job_errors: Object.entries(jobs).filter(([, v]) => v?.error).map(([k, v]) => `${k}: ${v.error}`).join('; ') || 'none', flagged_low: low.join(', ') || 'none', reorder_drafted: drafts.map(p => `${p.number} (${p.supplier})`).join(', ') || 'none', open_exceptions_after: ex.length, integrity: by.integrity?.detail, customer_export_orders: exp?.orders?.length, audit_lines: audit },
        links: [['Home', '/'], ['Purchasing', '/purchasing'], ['Health', '/health'], ['Exceptions', '/exceptions'], ['The customer', `/customers?q=${encodeURIComponent(FIRST.email)}`], ['Audit log', '/audit']],
        checks: [check('no job threw', !Object.values(jobs).some(v => v?.error)), check('the stock job flagged the parts two kits used up', low.length >= 1, low.join(', ')), check('the reorder is drafted, one PO per supplier', drafts.length >= 1, JSON.stringify(drafted).slice(0, 80)), check('the drafted PO clears the flags', !ex.some(e => e.key.startsWith('stock:')), ex.map(e => e.key).join(', ')), check('no other exception is open', ex.length === 0, ex.map(e => e.key).join(', ')), check('database integrity ok', by.integrity?.ok === true), check('books balanced', by.books?.ok === true), check('a backup was taken and checked', by.backup?.ok === true, by.backup?.detail), check('the customer\'s export holds both orders', exp?.orders?.length === 2, `${exp?.orders?.length}`), check('the audit log tells the story', audit >= 20, `${audit}`)] };
    } },
];

// ---------- running and remembering
const KEY = 'walkthrough:state';
export const walkthroughState = (db) => { try { return JSON.parse(db.setting(KEY) || 'null') || { done: [], state: {}, log: [] }; } catch { return { done: [], state: {}, log: [] }; } };
const save = (db, s) => db.setSetting(KEY, JSON.stringify(s));
export const nextStep = (db) => { const s = walkthroughState(db); return STEPS.find(st => !s.done.includes(st.id)) || null; };

// Run one step by id (the demo page), carrying the persisted state. A step that throws is recorded as failed and the chain stops there.
export async function runStep(app, id) {
  const { db, cfg } = app; const s = walkthroughState(db); const step = STEPS.find(st => st.id === id); if (!step) throw new Error(`no step ${id}`);
  const ctx = { app, db, cfg, state: s.state };
  let result;
  try { result = await step.run(ctx); } catch (err) { result = { facts: {}, links: [], checks: [check('the step ran', false, err.message)], error: err.message }; }
  result.ok = result.checks.every(c => c.ok);
  s.state = ctx.state; s.log.push({ id, at: nowIso(), ...result }); if (!s.done.includes(id)) s.done.push(id); save(db, s);
  return result;
}
// Run every step not yet done, in order; stop at the first that throws (its failure is in the log).
export async function runAll(app) {
  const out = [];
  for (const step of STEPS) { if (walkthroughState(app.db).done.includes(step.id)) continue; const r = await runStep(app, step.id); out.push({ id: step.id, title: step.title, ...r }); if (r.error) break; }
  return out;
}
export const resetWalkthrough = (db) => { startEmpty(db); return walkthroughState(db); };
