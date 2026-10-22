// Demo mode: a clearly synthetic company in an empty database, and the
// scenarios that make every feature move without any outside service.
//
// Everything here is unmistakably made up: people are named after the fact
// that they are placeholders, every address is under the reserved
// `.example.test` domain (RFC 2606 — it can never resolve), the school is
// "Example Academy (fictional)", money is play money. While the database
// carries the `demo` flag the app refuses to reach anything real: no SMTP,
// no IMAP, no GitHub, no Stripe; probes only probe the app itself; the
// outbox marks mail "sent (demo)" instead of sending it; every page shows
// the banner. `backoffice demo --reset` wipes and reseeds.
import { nowIso } from '../db.mjs';
import { normalize } from './catalog.mjs';
import { upsertContact, requestConsent, confirmConsent, joinWaitlist } from './contacts.mjs';
import { recordOrder, transition, refund } from './orders.mjs';
import { upsertItem, move, setRecipe } from './inventory.mjs';
import { registerDevice, assignToOrder } from './devices.mjs';
import { openClaim, reship, resolveClaim } from './warranty.mjs';
import { openTicket, ingestEmail, reply } from './support.mjs';
import { createInvoice, issueInvoice } from './invoices.mjs';
import { postExpense, postPayout } from './ledger.mjs';
import { upsertProbe, openIncident, updateIncident, runProbe, listProbes } from './status.mjs';
import { upsertSubscription } from './subscriptions.mjs';
import { createCampaign } from './campaigns.mjs';
import { raise } from './exceptions.mjs';
import { signStripePayload } from './stripe.mjs';

export const DEMO_PASSWORD = 'demo-password-change-me';
export const DEMO_WEBHOOK_SECRET = 'whsec_demo_not_a_real_secret';
export const isDemo = (db) => db.setting('demo') === '1';
export const isEmpty = (db) => db.get('SELECT COUNT(*) AS n FROM orders').n === 0 && db.get('SELECT COUNT(*) AS n FROM contacts').n === 0 && db.get('SELECT COUNT(*) AS n FROM tickets').n === 0;

// Demo traffic goes to this process's own loopback listener and nowhere
// else — never to BACKOFFICE_BASE_URL, which may name a real deployment.
export const localBase = (cfg) => {
  try { const u = new URL(cfg.baseUrl || ''); if ((u.hostname === '127.0.0.1' || u.hostname === 'localhost' || u.hostname === '[::1]') && u.port) return u.origin; } catch { /* fall through */ }
  return `http://127.0.0.1:${cfg.port}`;
};

export const DEMO_STORE = { mode: 'live', shipping_flat: 8, groups: [
  { id: 'Kits', products: [
    { id: 'wap-kit', name: 'Canary WAP Kit', price: 69, radio: true, ship: { oz: 5, box: 'small' }, cogs: { devices: { 'canary-wap': 1 }, extra_usd: 9 } },
    { id: 'wap-kit-outdoor', name: 'Canary WAP Kit — Outdoor', price: 84, radio: true, ship: { oz: 7, box: 'small' }, cogs: { devices: { 'canary-wap': 1 }, extra_usd: 21 } },
    { id: 'vision-kit', name: 'Canary Vision Kit', price: 119, radio: true, ship: { oz: 6, box: 'small' }, cogs: { devices: { 'canary-vision': 1 }, extra_usd: 17 } } ] },
  { id: 'Bundles', products: [
    { id: 'witness-pair', name: 'Witness Pair — 2× Canary WAP', price: 129, radio: true, ship: { oz: 10, box: 'medium' }, cogs: { devices: { 'canary-wap': 2 }, extra_usd: 18 } } ] },
  { id: 'Printed enclosure sets', products: [
    { id: 'wap-parts', name: 'WAP Enclosure Set', price: 15, radio: false, ship: { oz: 3, box: 'mailer' }, cogs: { devices: {}, extra_usd: 6.5 } },
    { id: 'vision-parts', name: 'Vision Enclosure Set', price: 19, radio: false, ship: { oz: 4, box: 'mailer' }, cogs: { devices: {}, extra_usd: 8.5 } } ] } ] };
export const DEMO_CATALOG = normalize(DEMO_STORE);
const BOM = { 'canary-wap': 21.4, 'canary-vision': 38.9 };

// Synthetic people. The surname says what they are; the domain cannot exist.
export const PEOPLE = [
  ['avery@customer.example.test', 'Avery Placeholder', '100 Sample Street', 'Springfield', 'MA', '01101'],
  ['blake@customer.example.test', 'Blake Fixture', '200 Mock Avenue Apt 2', 'Riverside', 'VA', '22201'],
  ['casey@customer.example.test', 'Casey Specimen', '300 Dummy Drive', 'Fairview', 'OR', '97209'],
  ['dana@customer.example.test', 'Dana Stub', '400 Example Lane', 'Franklin', 'TX', '78731'],
  ['ellis@customer.example.test', 'Ellis Mockery', '500 Test Terrace', 'Greenville', 'WI', '53703'],
  ['finley@customer.example.test', 'Finley Sampleton', '600 Synthetic Way', 'Clinton', 'CO', '80202'],
  ['harper@customer.example.test', 'Harper Prototype', '700 Lorem Loop', 'Madison', 'NC', '27601'],
  ['indigo@customer.example.test', 'Indigo Ipsum', '800 Dolor Court', 'Salem', 'MI', '48201'],
];
const daysAgo = (n, h = 10) => new Date(Date.now() - n * 86400000 - (24 - h) * 3600000).toISOString();
const silent = (cfg) => ({ ...cfg, supportAddr: '', githubToken: '' });

// Probes in demo mode point at the app itself — never at the live site.
export const demoProbes = (cfg) => [
  { name: 'Backoffice (this app)', url: localBase(cfg) + '/healthz', expect_text: 'ok' },
  { name: 'Public status page', url: localBase(cfg) + '/status', expect_text: 'status' },
  { name: 'Status JSON', url: localBase(cfg) + '/status.json', expect_text: '"probes"' },
  { name: 'Login page', url: localBase(cfg) + '/login', expect_status: 200 },
];

export function makeOrderInput(i, items, { days = 0, external = `cs_demo_${Date.now().toString(36)}_${i}` } = {}) {
  const p = PEOPLE[i % PEOPLE.length];
  const sub = items.reduce((s, it) => s + DEMO_CATALOG.products.get(it.sku).price_cents * it.qty, 0);
  const tax = Math.round(sub * 0.0625);
  return { external_id: external, payment_intent: external.replace(/^cs_/, 'pi_'), source: 'stripe', email: p[0], name: p[1], currency: 'usd',
    subtotal_cents: sub, shipping_cents: 800, tax_cents: tax, total_cents: sub + 800 + tax,
    ship: { line1: p[2], city: p[3], state: p[4], zip: p[5], country: 'US' },
    items: items.map(it => ({ ...it, name: DEMO_CATALOG.products.get(it.sku).name, unit_cents: DEMO_CATALOG.products.get(it.sku).price_cents })), paid_at: daysAgo(days) };
}

export async function seedDemo(db, cfg) {
  if (db.get('SELECT COUNT(*) AS n FROM orders').n) throw new Error('the database already has orders — demo data only goes into an empty one (`backoffice demo --reset` wipes it, or point BACKOFFICE_DATA_DIR somewhere fresh)');
  db.setSetting('demo', '1'); db.setSetting('demo:seeded_at', nowIso());
  const q = silent(cfg);

  for (const it of [
    ['xiao-esp32s3', 'XIAO ESP32S3', 'part', 14, 8, 25, 750, 'Example Distributor'],
    ['xiao-esp32s3-sense', 'XIAO ESP32S3 Sense (camera)', 'part', 3, 4, 12, 1390, 'Example Distributor'],
    ['grove-vision-ai-v2', 'Grove Vision AI V2', 'part', 5, 3, 10, 1600, 'Example Distributor'],
    ['cable-usbc-1m', 'USB-C cable, 1 m', 'part', 40, 15, 50, 140, 'Example Distributor'],
    ['sd-32gb', 'microSD 32 GB', 'part', 9, 6, 20, 690, 'Example Distributor'],
    ['wap-shell-petg', 'WAP shell + lid, PETG (printed)', 'printed', 6, 4, 12, 180, 'in-house'],
    ['vision-shell-petg', 'Vision shell + hinge, PETG (printed)', 'printed', 2, 3, 8, 260, 'in-house'],
    ['hardware-pack-wap', 'Fastener pack, WAP', 'part', 30, 10, 50, 45, 'Example Distributor'],
    ['box-small', 'Shipping box, small', 'packaging', 22, 10, 50, 62, 'Example Packaging Co.'],
    ['qr-card', 'First-boot QR card', 'packaging', 60, 20, 100, 8, 'in-house'],
  ]) upsertItem(db, { sku: it[0], name: it[1], kind: it[2], on_hand: it[3], reorder_point: it[4], reorder_qty: it[5], unit_cost_cents: it[6], supplier: it[7] });
  setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'wap-kit-outdoor', { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'vision-kit', { 'xiao-esp32s3-sense': 1, 'grove-vision-ai-v2': 1, 'vision-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'witness-pair', { 'xiao-esp32s3': 2, 'wap-shell-petg': 2, 'cable-usbc-1m': 2, 'sd-32gb': 2, 'hardware-pack-wap': 2, 'box-small': 1, 'qr-card': 2 });
  setRecipe(db, 'wap-parts', { 'wap-shell-petg': 1, 'hardware-pack-wap': 1 });

  const batch = 'DEMO-BATCH-A';
  for (let i = 1; i <= 8; i++) registerDevice(db, { serial: `DEMO-W-${String(i).padStart(4, '0')}`, mac: `02:00:00:00:00:${String(i).padStart(2, '0')}`, board: 'xiao-esp32s3', product_sku: 'wap-kit', fw_version: '2.5.0', fw_sha256: '00'.repeat(32), flashed_at: daysAgo(5), batch });
  for (let i = 1; i <= 2; i++) registerDevice(db, { serial: `DEMO-V-${String(i).padStart(4, '0')}`, mac: `02:00:00:00:01:${String(i).padStart(2, '0')}`, board: 'xiao-esp32s3-sense', product_sku: 'vision-kit', fw_version: '2.5.0', fw_sha256: '11'.repeat(32), flashed_at: daysAgo(5), batch });

  const mk = (i, items, days) => recordOrder(db, makeOrderInput(i, items, { days, external: `cs_demo_seed_${i}` }), { catalog: DEMO_CATALOG, bomTotals: BOM, actor: 'demo' }).order;
  const o1 = mk(0, [{ sku: 'wap-kit', qty: 1 }], 9);
  const o2 = mk(1, [{ sku: 'witness-pair', qty: 1 }], 8);
  const o3 = mk(2, [{ sku: 'vision-kit', qty: 1 }, { sku: 'wap-parts', qty: 2 }], 6);
  const o4 = mk(3, [{ sku: 'wap-kit-outdoor', qty: 1 }], 2);
  mk(4, [{ sku: 'wap-kit', qty: 2 }], 1);
  mk(5, [{ sku: 'vision-parts', qty: 1 }], 0);
  for (const [o, serials] of [[o1, ['DEMO-W-0001']], [o2, ['DEMO-W-0002', 'DEMO-W-0003']], [o3, ['DEMO-V-0001']], [o4, ['DEMO-W-0004']]]) {
    transition(db, o.id, 'packed', { actor: 'demo' });
    for (const s of serials) assignToOrder(db, s, o.id, { actor: 'demo' });
  }
  transition(db, o1.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: 'DEMO0000000000000001', cfg: q }); transition(db, o1.id, 'delivered', { actor: 'demo' });
  transition(db, o2.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: 'DEMO0000000000000002', cfg: q });
  transition(db, o3.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: 'DEMO0000000000000003', cfg: q });
  refund(db, o3.id, 1500, { actor: 'demo', reason: 'one parts pack short — shipped the other, refunded this one' });
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ?, delivered_at = ? WHERE id = ?', daysAgo(8), daysAgo(7), daysAgo(4), o1.id);
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ? WHERE id IN (?, ?)', daysAgo(5), daysAgo(4), o2.id, o3.id);

  const w1 = upsertContact(db, { email: 'harper@customer.example.test', name: 'Harper Prototype', source: 'waitlist' }); joinWaitlist(db, w1.id, 'vision-kit', 1); confirmConsent(db, requestConsent(db, w1.id));
  const w2 = upsertContact(db, { email: 'indigo@customer.example.test', name: 'Indigo Ipsum', source: 'waitlist' }); joinWaitlist(db, w2.id, 'vision-kit', 2);
  const w3 = upsertContact(db, { email: 'juniper@customer.example.test', name: 'Juniper Example', source: 'waitlist' }); joinWaitlist(db, w3.id, 'witness-pair', 1); confirmConsent(db, requestConsent(db, w3.id));
  confirmConsent(db, requestConsent(db, db.get('SELECT id FROM contacts WHERE email = ?', PEOPLE[0][0]).id));
  upsertContact(db, { email: 'purchasing@example-academy.example.test', name: 'Morgan Buyer', org: 'Example Academy (fictional)', kind: 'school', source: 'invoice' });

  const t1 = openTicket(db, { email: PEOPLE[1][0], name: PEOPLE[1][1], subject: 'Second Canary not showing in the fleet', body: 'Both WAPs flashed fine and joined Wi-Fi, but only one shows on the Witness Wall. The LED on the second blinks twice every few seconds. Do I need to pair them?', cfg: q, autoAck: false, channel: 'email', messageId: '<demo-a1@customer.example.test>' });
  reply(db, t1.id, 'Hi Blake — two blinks is the "no hub found" pattern, not a pairing step (Canaries never pair with each other). Two quick checks: is the second one on the same Wi-Fi band as the hub, and does the hub\'s /api/fleet list its MAC? Reply with what /api/fleet shows and I\'ll take it from there.', { cfg: q, actor: 'demo' });
  ingestEmail(db, { from: `${PEOPLE[1][1]} <${PEOPLE[1][0]}>`, subject: `Re: [${t1.number} ${t1.thread_token}] Second Canary not showing in the fleet`, text: 'That was it — the router had put it on 5 GHz. Moved it, both are on the Wall now. Thank you!', messageId: '<demo-a2@customer.example.test>' }, { cfg: q });
  const t2 = openTicket(db, { email: PEOPLE[3][0], name: PEOPLE[3][1], subject: `Order ${o4.number} — any shipping update?`, body: 'Paid on Sunday, hoping it ships this week. No rush, just checking the batch cadence.', cfg: q, autoAck: false, channel: 'form' });
  db.run('UPDATE tickets SET created_at = ?, updated_at = ?, sla_due_at = ? WHERE id = ?', daysAgo(1, 9), daysAgo(1, 9), daysAgo(0, 9), t2.id);
  openTicket(db, { email: 'newsdesk@fictional-paper.example.test', name: 'Quinn Byline', subject: 'Press: piece on privacy-first home cameras', body: 'Writing a feature for next month. Could you say a few words on why "no footage" and how the witness log holds up in court?', cfg: q, autoAck: false, channel: 'email', kind: 'press' });
  const t4 = openTicket(db, { email: PEOPLE[4][0], name: PEOPLE[4][1], subject: 'Does it do face recognition?', body: 'Asking before I buy — my partner is nervous about cameras.', cfg: q, autoAck: false, channel: 'form' });
  reply(db, t4.id, 'No, and it can\'t: the recognition code was never written, so there is no setting to turn off. ObjectClass is Person / Vehicle / Animal / Package, nothing finer. The FAQ has the long answer.', { cfg: q, actor: 'demo', close: true });
  db.run('INSERT OR REPLACE INTO canned_replies (slug, title, body) VALUES (?, ?, ?), (?, ?, ?)',
    'batch-cadence', 'Batch cadence', 'Hi {{name}},\n\nOrders in by Sunday night ship the following week in one batch — yours is in this one. You\'ll get the tracking number the moment the label prints.\n\n— Errer Labs',
    'no-face-rec', 'Face recognition (no, and can\'t)', 'Hi {{name}},\n\nNo — and it structurally can\'t. The recognition code was never written, so there\'s no setting to turn off. The FAQ has the long answer.\n\n— Errer Labs');

  const c1 = openClaim(db, { orderNumber: o1.number, serial: 'DEMO-W-0001', category: 'dead_board', description: 'Board never enumerates over USB, no LED at all. Tried two cables.', cfg: q, actor: 'demo' });
  reship(db, c1.id, [{ sku: 'xiao-esp32s3', name: 'XIAO ESP32S3', qty: 1 }], { recordOrderFn: recordOrder, catalog: null, actor: 'demo' });
  resolveClaim(db, c1.id, { costCents: 1090, note: 'New board shipped first class; serial DEMO-W-0005 is yours now.', cfg: q, actor: 'demo' });
  openClaim(db, { orderNumber: o3.number, category: 'warped_print', description: 'Lid is bowed ~2 mm and won\'t seat on the shell.', cfg: q, actor: 'demo' });

  const inv = createInvoice(db, { email: 'purchasing@example-academy.example.test', name: 'Morgan Buyer', org: 'Example Academy (fictional)', po_number: 'PO-DEMO-0001', terms_days: 30, bill_to: 'Example Academy (fictional)\nAttn: Morgan Buyer, Purchasing\n1 Example Campus Road\nSpringfield, MA 01101', items: [{ sku: 'wap-kit', description: 'Canary WAP Kit (education)', qty: 20, unit_cents: 6200 }, { sku: 'vision-kit', description: 'Canary Vision Kit (education)', qty: 4, unit_cents: 10700 }], shipping_cents: 2400, notes: 'Education pricing. Net 30. (Demo invoice — not a real sale.)' });
  issueInvoice(db, inv.id, q, { actor: 'demo' });
  upsertSubscription(db, { external_id: 'sub_demo_0001', email: 'ops@example-coop.example.test', plan: 'Support plan — small fleet', status: 'active', amount_cents: 9900, interval: 'month', current_period_end: new Date(Date.now() + 12 * 86400000).toISOString() });
  postPayout(db, { id: 'po_demo_0001', date: daysAgo(3).slice(0, 10), amount_cents: 24866, fee_cents: 784 });
  postExpense(db, { date: daysAgo(6).slice(0, 10), account: 'expenses:supplies', cents: 6600, memo: 'PETG, 3 kg black' });
  postExpense(db, { date: daysAgo(5).slice(0, 10), account: 'expenses:postage', cents: 2170, memo: 'labels, last batch' });
  postExpense(db, { date: daysAgo(10).slice(0, 10), account: 'expenses:software', cents: 25000, memo: 'AI operator, monthly', paidFrom: 'equity:owner' });
  postExpense(db, { date: daysAgo(12).slice(0, 10), account: 'expenses:insurance', cents: 10000, memo: 'product liability, monthly' });

  const probes = demoProbes(cfg).map(p => upsertProbe(db, p));
  for (const p of probes) {
    for (let h = 24 * 30; h > 0; h -= 2) { const ok = !(p.name.startsWith('Status JSON') && h > 70 && h < 76); db.run('INSERT INTO probe_results (probe_id, at, ok, ms, detail) VALUES (?, ?, ?, ?, ?)', p.id, new Date(Date.now() - h * 3600000).toISOString(), ok ? 1 : 0, 20 + ((h * 37) % 60), ok ? '200' : '503'); }
    db.run('UPDATE probes SET last_ok = 1, last_ms = ?, last_at = ?, last_detail = \'200\' WHERE id = ?', 24, nowIso(), p.id);
  }
  const inc = openIncident(db, { title: 'Status JSON slow for a few hours (demo incident)', body: 'A synthetic incident so the public page has history. No device is affected — a Canary never talks to this service.' });
  db.run('UPDATE incidents SET started_at = ?, updated_at = ? WHERE id = ?', daysAgo(3, 8), daysAgo(3, 11), inc.id); updateIncident(db, inc.id, { status: 'resolved' });

  createCampaign(db, { slug: 'demo-batch-note', subject: 'This month from the bench (demo newsletter)', segment: 'all', body_text: 'Hi {{name}},\n\nTwo things from the bench this month:\n\n1. Vision kits are back in stock.\n2. The Outdoor shell moved to ASA.\n\nIf you joined the waitlist, you\'ll get the one in-stock mail you asked for and nothing else.\n\n— Errer Labs\n\nUnsubscribe in one click: {{unsubscribe}}\n' });
  move(db, 'xiao-esp32s3-sense', -1, 'scrap', 'bench: bad USB-C port');
  await raise(db, q, 'stock:xiao-esp32s3-sense', 'inventory [reorder]: xiao-esp32s3-sense', 'XIAO ESP32S3 Sense (camera): 2 on hand, reorder point 4, reorder qty 12.\nSupplier: Example Distributor');
  db.audit('demo', 'demo.seed', null, null, { orders: 7, tickets: 4 });
  return { orders: db.get('SELECT COUNT(*) AS n FROM orders').n, tickets: db.get('SELECT COUNT(*) AS n FROM tickets').n, contacts: db.get('SELECT COUNT(*) AS n FROM contacts').n, items: db.get('SELECT COUNT(*) AS n FROM inventory_items').n };
}

// A demo app that finds an empty database makes itself the demo: operator,
// flag, data. A used database is left exactly as it is.
export async function ensureDemo(db, cfg) {
  const { hasOperator, setupOperator } = await import('../auth.mjs');
  if (!isEmpty(db) || hasOperator(db)) return null;
  setupOperator(db, DEMO_PASSWORD);
  return seedDemo(db, cfg);
}

// Wipe every business table (the operator login and settings survive) and seed again.
export async function resetDemo(db, cfg) {
  // A stale BACKOFFICE_DATA_DIR must never turn this into a wipe of real data.
  if (!isDemo(db) && !isEmpty(db)) throw new Error('refusing to reset: this database is not flagged as a demo and is not empty — point BACKOFFICE_DATA_DIR at the demo directory (npm run demo uses ./data-demo)');
  const tables = ['ticket_messages', 'tickets', 'warranty_claims', 'order_items', 'orders', 'invoice_items', 'invoices', 'subscriptions', 'inventory_moves', 'recipes', 'inventory_items', 'devices', 'waitlist', 'contacts', 'mail_outbox', 'mail_seen', 'campaigns', 'probe_results', 'probes', 'incidents', 'ledger', 'exceptions', 'canned_replies', 'stripe_events', 'job_runs', 'audit_log'];
  db.exec('PRAGMA foreign_keys = OFF');
  try { db.tx(() => { for (const t of tables) db.run(`DELETE FROM ${t}`); db.run('DELETE FROM settings WHERE key LIKE \'seq:%\''); }); }
  finally { db.exec('PRAGMA foreign_keys = ON'); }
  return seedDemo(db, cfg);
}

// ---------- scenarios: each one is the real code path, fed a synthetic event.
let n = 0; const stamp = () => `${Date.now().toString(36)}${(n++).toString(36)}`;
export const SCENARIOS = {
  order: { label: 'A new Checkout order arrives (signed Stripe webhook)', run: async (app) => {
    const i = Math.floor(Math.random() * PEOPLE.length); const skus = ['wap-kit', 'vision-kit', 'witness-pair', 'wap-parts'];
    const o = makeOrderInput(i, [{ sku: skus[Math.floor(Math.random() * skus.length)], qty: 1 + Math.floor(Math.random() * 2) }]);
    const event = { id: `evt_demo_${stamp()}`, type: 'checkout.session.completed', data: { object: { id: o.external_id, payment_intent: o.payment_intent, payment_status: 'paid', currency: 'usd', amount_subtotal: o.subtotal_cents, amount_total: o.total_cents, created: Math.floor(Date.now() / 1000),
      total_details: { amount_shipping: o.shipping_cents, amount_tax: o.tax_cents }, customer_details: { email: o.email, name: o.name }, shipping_details: { name: o.name, address: { line1: o.ship.line1, city: o.ship.city, state: o.ship.state, postal_code: o.ship.zip, country: 'US' } },
      line_items: { data: o.items.map(it => ({ description: it.name, quantity: it.qty, price: { unit_amount: it.unit_cents, product: { metadata: { sku: it.sku } } } })) } } } };
    return postWebhook(app, event);
  } },
  refund: { label: 'Stripe reports a partial refund on the latest shipped order', run: async (app) => {
    const o = app.db.get('SELECT * FROM orders WHERE status = \'shipped\' AND payment_intent IS NOT NULL ORDER BY id DESC LIMIT 1');
    if (!o) return { skipped: 'no shipped order with a PaymentIntent' };
    return postWebhook(app, { id: `evt_demo_${stamp()}`, type: 'charge.refunded', data: { object: { id: `ch_demo_${stamp()}`, payment_intent: o.payment_intent, amount_refunded: o.refunded_cents + 800 } } });
  } },
  payout: { label: 'A Stripe payout lands in the bank', run: async (app) => postWebhook(app, { id: `evt_demo_${stamp()}`, type: 'payout.paid', data: { object: { id: `po_demo_${stamp()}`, amount: 18450, arrival_date: Math.floor(Date.now() / 1000) } } }) },
  email: { label: 'A customer emails support (new ticket)', run: async ({ db, cfg }) => {
    const p = PEOPLE[Math.floor(Math.random() * PEOPLE.length)];
    const subjects = ['Can I run two hubs?', 'Outdoor shell — is PETG OK in the rain?', 'Witness Wall says "chain verified" — what does that mean?', 'Will this work with Home Assistant Green?'];
    const r = ingestEmail(db, { from: `${p[1]} <${p[0]}>`, subject: subjects[Math.floor(Math.random() * subjects.length)], text: 'Hi — quick question before I set mine up. Thanks!\n\n(synthetic demo email)', messageId: `<demo-${stamp()}@customer.example.test>` }, { cfg: silent(cfg) });
    return { ticket: r.ticket?.number, created: r.created };
  } },
  reply: { label: 'A customer replies to the oldest waiting ticket', run: async ({ db, cfg }) => {
    const t = db.get('SELECT * FROM tickets WHERE status = \'waiting_on_customer\' ORDER BY updated_at LIMIT 1');
    if (!t) return { skipped: 'no ticket is waiting on a customer — reply to one first' };
    const c = db.get('SELECT email, name FROM contacts WHERE id = ?', t.contact_id);
    const r = ingestEmail(db, { from: `${c?.name || 'Customer'} <${c?.email || 'someone@customer.example.test'}>`, subject: `Re: [${t.number} ${t.thread_token}] ${t.subject}`, text: 'Thanks — that worked. One more thing: can I get a second QR card?\n\n(synthetic demo reply)', messageId: `<demo-${stamp()}@customer.example.test>` }, { cfg: silent(cfg) });
    return { ticket: r.ticket?.number, status: r.ticket?.status };
  } },
  bounce: { label: 'An out-of-office auto-reply arrives (filed as a note, never answered)', run: async ({ db, cfg }) => {
    const t = db.get('SELECT * FROM tickets ORDER BY updated_at DESC LIMIT 1');
    const r = ingestEmail(db, { from: 'someone@customer.example.test', subject: `Automatic reply: [${t.number} ${t.thread_token}] ${t.subject}`, text: 'I am away until next week.', messageId: `<demo-${stamp()}@customer.example.test>`, headers: { 'auto-submitted': 'auto-replied' } }, { cfg: silent(cfg) });
    return { ticket: r.ticket?.number, filedAs: r.ticket?.messages?.at(-1)?.direction };
  } },
  waitlist: { label: 'Someone joins the waitlist through the website form (+ asks for news → opt-in mail)', run: async (app) => {
    const p = PEOPLE[Math.floor(Math.random() * PEOPLE.length)];
    const r = await fetch(`${localBase(app.cfg)}/forms/waitlist`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: p[0], name: p[1], sku: 'vision-kit', qty: 1, newsletter: true }) });
    return { status: r.status, ...(await r.json().catch(() => ({}))) };
  } },
  contact: { label: 'Someone sends the website contact form', run: async (app) => {
    const r = await fetch(`${localBase(app.cfg)}/forms/contact`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'visitor@customer.example.test', name: 'Visitor Example', subject: 'Which kit for a small apartment?', message: 'One room, one door. WAP or Vision?\n\n(synthetic demo form)' }) });
    return { status: r.status, ...(await r.json().catch(() => ({}))) };
  } },
  probe_down: { label: 'A probe target goes down three times in a row (raises an exception)', run: async ({ db, cfg }) => {
    const p = listProbes(db)[0]; const fetchImpl = async () => ({ status: 503, text: async () => 'maintenance' });
    for (let i = 0; i < 3; i++) await runProbe(db, p, { fetchImpl, cfg: silent(cfg) });
    return { probe: p.name, fails: db.get('SELECT fails FROM probes WHERE id = ?', p.id).fails };
  } },
  probe_up: { label: 'The probe target recovers (resolves the exception)', run: async ({ db, cfg }) => {
    const p = listProbes(db)[0]; const r = await runProbe(db, p, { cfg: silent(cfg) });
    return { probe: p.name, ok: r.ok, detail: r.detail };
  } },
  flush: { label: 'Deliver the outbox (demo transport: marked sent, nothing leaves the machine)', run: async (app) => { const { sendOutbox } = await import('./mail.mjs'); return sendOutbox(app.db, app.cfg); } },
  jobs: { label: 'Run every scheduled job once (SLA, stock, invoices, books, backup…)', run: async (app) => { const { JOBS, runJob } = await import('../jobs.mjs'); const out = {}; for (const name of Object.keys(JOBS)) out[name] = (await runJob(app, name)); return out; } },
};

async function postWebhook(app, event) {
  const body = JSON.stringify(event);
  const r = await fetch(`${localBase(app.cfg)}/webhooks/stripe`, { method: 'POST', headers: { 'content-type': 'application/json', 'stripe-signature': signStripePayload(body, app.cfg.stripeWebhookSecret) }, body });
  return { status: r.status, ...(await r.json().catch(() => ({}))) };
}
