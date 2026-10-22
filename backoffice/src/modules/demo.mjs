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
import { recordOrder, transition, refund, setTags, addNote } from './orders.mjs';
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
import { createPo, markOrdered, receivePo, draftReorder } from './purchasing.mjs';
import { saveAttachments } from './support.mjs';
import { deflateSync } from 'node:zlib';

// A real, tiny PNG (a flat color with a darker band), so an attachment renders in the browser without any file on disk.
export function tinyPng(w = 64, h = 48, rgb = [214, 196, 150]) {
  const crc = (buf) => { let c = ~0; for (const b of buf) { c ^= b; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); } return (~c) >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; for (let x = 0; x < w; x++) { const dark = y > h * 0.4 && y < h * 0.6 ? 0.55 : 1; const o = y * (w * 3 + 1) + 1 + x * 3; raw[o] = Math.round(rgb[0] * dark); raw[o + 1] = Math.round(rgb[1] * dark); raw[o + 2] = Math.round(rgb[2] * dark); } }
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

export const DEMO_PASSWORD = 'demo1234';
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
  ['jules@customer.example.test', 'Jules Exemplar', '900 Boilerplate Blvd', 'Bristol', 'CT', '06010'],
  ['kai@customer.example.test', 'Kai Mockingbird', '1010 Scaffold Street', 'Georgetown', 'WA', '98101'],
  ['lane@customer.example.test', 'Lane Template', '1100 Stand-In Road', 'Dayton', 'OH', '45402'],
  ['morgan@customer.example.test', 'Morgan Dummy', '1200 Filler Lane', 'Auburn', 'GA', '30301'],
  ['noor@customer.example.test', 'Noor Sampleworth', '1300 Pseudo Place', 'Lebanon', 'PA', '17042'],
  ['oakley@customer.example.test', 'Oakley Testcase', '1400 Surrogate Square', 'Burlington', 'VT', '05401'],
  ['parker@customer.example.test', 'Parker Pretend', '1500 Facsimile Drive', 'Ashland', 'IL', '62001'],
  ['quinn@customer.example.test', 'Quinn Mockup', '1600 Draft Avenue', 'Jackson', 'AZ', '85001'],
  ['reese@customer.example.test', 'Reese Specimenson', '1700 Proxy Court', 'Manchester', 'NH', '03101'],
  ['sage@customer.example.test', 'Sage Fictitious', '1800 Simulacrum Street', 'Kingston', 'NY', '12401'],
  ['tatum@customer.example.test', 'Tatum Placeholder', '1900 Hypothetical Hwy', 'Milford', 'MN', '55401'],
  ['uma@customer.example.test', 'Uma Exampleton', '2000 Nominal Lane', 'Oxford', 'FL', '32003'],
  ['vic@customer.example.test', 'Vic Stubbs', '2100 Mock Mews', 'Centerville', 'UT', '84010'],
  ['wren@customer.example.test', 'Wren Nonesuch', '2200 Notional Road', 'Newport', 'TN', '37821'],
];
const daysAgo = (n, h = 10) => new Date(Date.now() - n * 86400000 - (24 - h) * 3600000).toISOString();
const silent = (cfg) => ({ ...cfg, supportAddr: '', githubToken: '' });
// The same numbers every time: a seeded generator, so screenshots, tests and two people's demos agree.
const prng = (seed) => () => { seed |= 0; seed = seed + 0x6D2B79F5 | 0; let t = Math.imul(seed ^ seed >>> 15, 1 | seed); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };

// Probes in demo mode point at the app itself — never at the live site.
export const demoProbes = (cfg) => [
  { name: 'Backoffice (this app)', url: localBase(cfg) + '/healthz', expect_text: 'ok' },
  { name: 'Public status page', url: localBase(cfg) + '/status', expect_text: 'status' },
  { name: 'Status JSON', url: localBase(cfg) + '/status.json', expect_text: '"probes"' },
  { name: 'Login page', url: localBase(cfg) + '/login', expect_status: 200 },
];

export const DEMO_DISCOUNTS = [
  { code: 'LAUNCH10', kind: 'percent', value: 10, max_uses: 50, note: 'Launch month — 10% off anything' },
  { code: 'FLEET15', kind: 'percent', value: 15, min_subtotal_cents: 12000, note: 'Two or more Canaries' },
  { code: 'FREESHIP', kind: 'free_shipping', note: 'Newsletter readers' },
  { code: 'WELCOME5', kind: 'fixed', value: 500, note: 'First order from the waitlist mail' },
  { code: 'SPRING20', kind: 'percent', value: 20, ends_at: daysAgo(30), note: 'Ended — kept for the books' },
];

export function makeOrderInput(i, items, { days = 0, external = `cs_demo_${Date.now().toString(36)}_${i}`, discount = null, tags = '' } = {}) {
  const p = PEOPLE[i % PEOPLE.length];
  const sub = items.reduce((s, it) => s + DEMO_CATALOG.products.get(it.sku).price_cents * it.qty, 0);
  const d = discount ? (discount.kind === 'percent' ? Math.round(sub * discount.value / 100) : discount.kind === 'fixed' ? Math.min(discount.value, sub) : 0) : 0;
  const shipping = discount?.kind === 'free_shipping' ? 0 : 800;
  const tax = Math.round((sub - d) * 0.0625);
  return { external_id: external, payment_intent: external.replace(/^cs_/, 'pi_'), source: 'stripe', email: p[0], name: p[1], currency: 'usd',
    subtotal_cents: sub, discount_cents: d, discount_code: discount?.code || null, shipping_cents: shipping, tax_cents: tax, total_cents: sub - d + shipping + tax, tags,
    ship: { line1: p[2], city: p[3], state: p[4], zip: p[5], country: 'US' },
    items: items.map(it => ({ ...it, name: DEMO_CATALOG.products.get(it.sku).name, unit_cents: DEMO_CATALOG.products.get(it.sku).price_cents })), paid_at: daysAgo(days, 9 + (i % 9)) };
}

const PARTS = [
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
];
const RECIPES = {
  'wap-kit': { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 },
  'wap-kit-outdoor': { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 },
  'vision-kit': { 'xiao-esp32s3-sense': 1, 'grove-vision-ai-v2': 1, 'vision-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'box-small': 1, 'qr-card': 1 },
  'witness-pair': { 'xiao-esp32s3': 2, 'wap-shell-petg': 2, 'cable-usbc-1m': 2, 'sd-32gb': 2, 'hardware-pack-wap': 2, 'box-small': 1, 'qr-card': 2 },
  'wap-parts': { 'wap-shell-petg': 1, 'hardware-pack-wap': 1 },
};

// Sixty days of a small company: ~45 orders, repeat buyers, codes, a refund,
// a cancellation, the weekly batches, tickets with answers — all fixed by
// the seed, so every demo is the same demo.
function planOrders() {
  const rnd = prng(20261022);
  const pick = (xs) => xs[Math.floor(rnd() * xs.length)];
  const weighted = (pairs) => { const r = rnd() * pairs.reduce((s, [, w]) => s + w, 0); let acc = 0; for (const [v, w] of pairs) { acc += w; if (r < acc) return v; } return pairs[0][0]; };
  const plan = [];
  for (let i = 0; i < 40; i++) {
    const age = 59 - Math.floor(i * 57 / 39) + (rnd() < 0.5 ? 0 : -1);
    const who = rnd() < 0.35 ? Math.floor(rnd() * 6) : 6 + Math.floor(rnd() * 16);       // a third of orders come from six repeat buyers
    const sku = weighted([['wap-kit', 35], ['vision-kit', 20], ['witness-pair', 15], ['wap-kit-outdoor', 12], ['wap-parts', 10], ['vision-parts', 8]]);
    const items = [{ sku, qty: sku.endsWith('-parts') ? 1 + Math.floor(rnd() * 2) : rnd() < 0.15 ? 2 : 1 }];
    if (rnd() < 0.15) items.push({ sku: pick(['wap-parts', 'vision-parts']), qty: 1 });
    const sub = items.reduce((s, it) => s + DEMO_CATALOG.products.get(it.sku).price_cents * it.qty, 0);
    const discount = rnd() < 0.3 ? (age > 40 ? DEMO_DISCOUNTS[0] : sub >= 12000 ? DEMO_DISCOUNTS[1] : rnd() < 0.5 ? DEMO_DISCOUNTS[2] : DEMO_DISCOUNTS[3]) : null;
    const tags = [rnd() < 0.08 ? 'gift' : '', rnd() < 0.05 ? 'rush' : '', rnd() < 0.05 ? 'wholesale-interest' : ''].filter(Boolean).join(',');
    const fate = i === 12 ? 'canceled' : i === 20 ? 'refunded' : age >= 10 ? 'delivered' : age >= 5 ? 'shipped' : age >= 2 ? 'packed' : 'paid';
    plan.push({ i, age: Math.max(3, age), who, items, discount, tags, fate });
  }
  return plan;
}

export async function seedDemo(db, cfg) {
  if (db.get('SELECT COUNT(*) AS n FROM orders').n) throw new Error('the database already has orders — demo data only goes into an empty one (`backoffice demo --reset` wipes it, or point BACKOFFICE_DATA_DIR somewhere fresh)');
  db.setSetting('demo', '1'); db.setSetting('demo:seeded_at', nowIso());
  const q = silent(cfg);
  const { createDiscount, setDiscountStatus } = await import('./discounts.mjs');
  const { setContactTags } = await import('./contacts.mjs');
  const { unreconciledStripeCents } = await import('./ledger.mjs');
  for (const d of DEMO_DISCOUNTS) createDiscount(db, d);
  setDiscountStatus(db, 'SPRING20', 'disabled');

  // Stock: what the two months of packing will consume, plus what is on the shelf today.
  const plan = planOrders();
  const story = [[0, [{ sku: 'wap-kit', qty: 1 }], 9, 'delivered'], [1, [{ sku: 'witness-pair', qty: 1 }], 8, 'shipped'], [2, [{ sku: 'vision-kit', qty: 1 }, { sku: 'wap-parts', qty: 2 }], 6, 'shipped'], [3, [{ sku: 'wap-kit-outdoor', qty: 1 }], 2, 'packed'], [4, [{ sku: 'wap-kit', qty: 2 }], 1, 'paid'], [5, [{ sku: 'vision-parts', qty: 1 }], 0, 'paid']];
  const need = {};
  const consume = (items) => { for (const it of items) for (const [part, n] of Object.entries(RECIPES[it.sku] || {})) need[part] = (need[part] || 0) + n * it.qty; };
  for (const o of plan) if (['packed', 'shipped', 'delivered', 'refunded'].includes(o.fate)) consume(o.items);
  for (const [, items, , fate] of story) if (fate !== 'paid') consume(items);
  for (const it of PARTS) upsertItem(db, { sku: it[0], name: it[1], kind: it[2], on_hand: 0, reorder_point: it[4], reorder_qty: it[5], unit_cost_cents: it[6], supplier: it[7] });
  const PO1 = { 'xiao-esp32s3': 25, 'cable-usbc-1m': 50 };   // this much of the shelf came in on a purchase order three weeks ago
  for (const it of PARTS) { const n = it[3] + (need[it[0]] || 0) - (PO1[it[0]] || 0); if (n > 0) move(db, it[0], n, 'receive', 'po:DEMO-0001'); }
  db.run('UPDATE inventory_moves SET at = ?', daysAgo(62));
  const po1 = createPo(db, { supplier: 'Example Distributor', supplier_url: 'https://distributor.example.test/order', items: Object.entries(PO1).map(([sku, qty]) => ({ sku, qty })), notes: 'Boards and cables for the autumn batches.', actor: 'web' });
  markOrdered(db, po1.id, { expected_at: daysAgo(21).slice(0, 10), actor: 'web' }); receivePo(db, po1.id, { actor: 'web' });
  db.run('UPDATE purchase_orders SET created_at = ?, ordered_at = ?, received_at = ? WHERE id = ?', daysAgo(30), daysAgo(29), daysAgo(20), po1.id);
  db.run('UPDATE inventory_moves SET at = ? WHERE ref = ?', daysAgo(20), `po:${po1.number}`); db.run('UPDATE ledger SET date = ? WHERE ref = ?', daysAgo(20).slice(0, 10), `po:${po1.number}`);
  for (const [sku, parts] of Object.entries(RECIPES)) setRecipe(db, sku, parts);

  const batch = 'DEMO-BATCH-A';
  for (let i = 1; i <= 8; i++) registerDevice(db, { serial: `DEMO-W-${String(i).padStart(4, '0')}`, mac: `02:00:00:00:00:${String(i).padStart(2, '0')}`, board: 'xiao-esp32s3', product_sku: 'wap-kit', fw_version: '2.5.0', fw_sha256: '00'.repeat(32), flashed_at: daysAgo(5), batch });
  for (let i = 1; i <= 2; i++) registerDevice(db, { serial: `DEMO-V-${String(i).padStart(4, '0')}`, mac: `02:00:00:00:01:${String(i).padStart(2, '0')}`, board: 'xiao-esp32s3-sense', product_sku: 'vision-kit', fw_version: '2.5.0', fw_sha256: '11'.repeat(32), flashed_at: daysAgo(5), batch });

  for (let i = 1; i <= 6; i++) registerDevice(db, { serial: `DEMO-W-01${String(i).padStart(2, '0')}`, mac: `02:00:00:00:02:${String(i).padStart(2, '0')}`, board: 'xiao-esp32s3', product_sku: 'wap-kit', fw_version: '2.4.1', fw_sha256: '24'.repeat(32), flashed_at: daysAgo(55), batch: 'DEMO-BATCH-0' });

  // The background two months, oldest first, every step stamped at the hour it would have happened.
  const mk = (i, items, days, extra = {}) => recordOrder(db, makeOrderInput(i, items, { days, external: `cs_demo_seed_${extra.external || i}`, discount: extra.discount || null, tags: extra.tags || '' }), { catalog: DEMO_CATALOG, bomTotals: BOM, actor: 'stripe' }).order;
  const after = (iso, h) => new Date(Date.parse(iso) + h * 3600000).toISOString();
  const tracking = (n) => `DEMO${String(n).padStart(16, '0')}`;
  let tn = 100;
  for (const o of plan) {
    const ord = mk(o.who, o.items, o.age, { external: `bg_${o.i}`, discount: o.discount, tags: o.tags });
    const paid = ord.paid_at;
    if (o.fate === 'canceled') { transition(db, ord.id, 'canceled', { actor: 'web', at: after(paid, 5) }); db.run('UPDATE orders SET notes = ? WHERE id = ?', 'Customer wrote within the hour: ordered twice by mistake. Refunded in Stripe, nothing packed.', ord.id); continue; }
    const packedAt = after(paid, 20 + (o.i % 30)), shippedAt = after(packedAt, 18 + (o.i % 20)), deliveredAt = after(shippedAt, 60 + (o.i % 48));
    transition(db, ord.id, 'packed', { actor: 'web', at: packedAt });
    if (['shipped', 'delivered', 'refunded'].includes(o.fate)) transition(db, ord.id, 'shipped', { actor: 'web', at: shippedAt, carrier: 'USPS', tracking: tracking(++tn), cfg: q });
    if (['delivered', 'refunded'].includes(o.fate)) transition(db, ord.id, 'delivered', { actor: 'web', at: deliveredAt });
    if (o.fate === 'refunded') refund(db, ord.id, ord.total_cents, { actor: 'stripe', reason: 'changed their mind within 30 days — kept the kit as a spare, we refunded (terms §6)', at: after(deliveredAt, 30) });
    db.run('UPDATE orders SET packed_at = ?, shipped_at = CASE WHEN shipped_at IS NULL THEN NULL ELSE ? END, delivered_at = CASE WHEN delivered_at IS NULL THEN NULL ELSE ? END WHERE id = ?', packedAt, shippedAt, deliveredAt, ord.id);
  }
  // The older boards went out in the first weeks, one per delivered WAP order, scanned at packing.
  { let k = 1; for (const o of db.all("SELECT o.id FROM orders o JOIN order_items i ON i.order_id = o.id WHERE o.status = 'delivered' AND i.sku IN ('wap-kit', 'wap-kit-outdoor') ORDER BY o.paid_at LIMIT 6")) { assignToOrder(db, `DEMO-W-01${String(k++).padStart(2, '0')}`, o.id, { actor: 'web' }); db.run('UPDATE order_events SET at = (SELECT packed_at FROM orders WHERE id = ?) WHERE order_id = ? AND kind = \'device\'', o.id, o.id); } }
  // The tracking mails are dated when the label printed; the ones older than the live week went out (demo transport, nothing left the machine).
  db.run("UPDATE mail_outbox SET created_at = COALESCE((SELECT o.shipped_at FROM orders o WHERE 'order:' || o.number = mail_outbox.ref), created_at) WHERE ref LIKE 'order:%'");
  // Last week's story: the orders the tickets, claims and serials refer to.
  const [o1, o2, o3, o4] = story.map(([i, items, days]) => mk(i, items, days, { external: i }));
  for (const [o, serials, pk] of [[o1, ['DEMO-W-0001'], 8], [o2, ['DEMO-W-0002', 'DEMO-W-0003'], 5], [o3, ['DEMO-V-0001'], 5], [o4, ['DEMO-W-0004'], 1]]) {
    transition(db, o.id, 'packed', { actor: 'web', at: daysAgo(pk, 14) });
    for (const s of serials) { assignToOrder(db, s, o.id, { actor: 'web' }); db.run('UPDATE order_events SET at = ? WHERE order_id = ? AND kind = \'device\'', daysAgo(pk, 15), o.id); }
  }
  transition(db, o1.id, 'shipped', { actor: 'web', at: daysAgo(7, 11), carrier: 'USPS', tracking: 'DEMO0000000000000001', cfg: q }); transition(db, o1.id, 'delivered', { actor: 'web', at: daysAgo(4, 15) });
  transition(db, o2.id, 'shipped', { actor: 'web', at: daysAgo(4, 11), carrier: 'USPS', tracking: 'DEMO0000000000000002', cfg: q });
  transition(db, o3.id, 'shipped', { actor: 'web', at: daysAgo(4, 11), carrier: 'USPS', tracking: 'DEMO0000000000000003', cfg: q });
  refund(db, o3.id, 1500, { actor: 'web', reason: 'one parts pack short — shipped the other, refunded this one', at: daysAgo(3, 16) });
  setTags(db, o4.id, 'rush', { actor: 'web' }); addNote(db, o4.id, 'Customer asked on T-ticket whether it ships this week — it does; label prints Monday.', { actor: 'web' });
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ?, delivered_at = ? WHERE id = ?', daysAgo(8), daysAgo(7), daysAgo(4), o1.id);
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ? WHERE id IN (?, ?)', daysAgo(5), daysAgo(4), o2.id, o3.id);
  db.run('UPDATE orders SET packed_at = ? WHERE id = ?', daysAgo(1, 14), o4.id);

  const w1 = upsertContact(db, { email: 'harper@customer.example.test', name: 'Harper Prototype', source: 'waitlist' }); joinWaitlist(db, w1.id, 'vision-kit', 1); confirmConsent(db, requestConsent(db, w1.id));
  const w2 = upsertContact(db, { email: 'indigo@customer.example.test', name: 'Indigo Ipsum', source: 'waitlist' }); joinWaitlist(db, w2.id, 'vision-kit', 2);
  const w3 = upsertContact(db, { email: 'juniper@customer.example.test', name: 'Juniper Example', source: 'waitlist' }); joinWaitlist(db, w3.id, 'witness-pair', 1); confirmConsent(db, requestConsent(db, w3.id));
  for (const i of [0, 2, 5, 9, 14]) confirmConsent(db, requestConsent(db, upsertContact(db, { email: PEOPLE[i][0], name: PEOPLE[i][1], source: 'store' }).id));
  const academy = upsertContact(db, { email: 'purchasing@example-academy.example.test', name: 'Morgan Buyer', org: 'Example Academy (fictional)', kind: 'school', source: 'invoice' });
  setContactTags(db, academy.id, 'school,purchase-order');
  setContactTags(db, db.get('SELECT id FROM contacts WHERE email = ?', PEOPLE[0][0]).id, 'vip,repeat');
  setContactTags(db, db.get('SELECT id FROM contacts WHERE email = ?', PEOPLE[1][0]).id, 'repeat');
  db.run('UPDATE contacts SET created_at = (SELECT MIN(paid_at) FROM orders o WHERE o.contact_id = contacts.id) WHERE id IN (SELECT contact_id FROM orders)');

  // Support: last week's live threads plus two months of answered ones.
  const backdate = (id, created, responded, closed = null) => { db.run('UPDATE tickets SET created_at = ?, updated_at = ?, first_response_at = ?, closed_at = ?, sla_due_at = ? WHERE id = ?', created, closed || responded || created, responded, closed, after(created, 24), id); db.run('UPDATE ticket_messages SET created_at = CASE direction WHEN \'in\' THEN ? ELSE ? END WHERE ticket_id = ?', created, responded || created, id); };
  const t1 = openTicket(db, { email: PEOPLE[1][0], name: PEOPLE[1][1], subject: 'Second Canary not showing in the fleet', body: 'Both WAPs flashed fine and joined Wi-Fi, but only one shows on the Witness Wall. The LED on the second blinks twice every few seconds. Do I need to pair them?', cfg: q, autoAck: false, channel: 'email', messageId: '<demo-a1@customer.example.test>' });
  reply(db, t1.id, 'Hi Blake — two blinks is the "no hub found" pattern, not a pairing step (Canaries never pair with each other). Two quick checks: is the second one on the same Wi-Fi band as the hub, and does the hub\'s /api/fleet list its MAC? Reply with what /api/fleet shows and I\'ll take it from there.', { cfg: q, actor: 'web' });
  ingestEmail(db, { from: `${PEOPLE[1][1]} <${PEOPLE[1][0]}>`, subject: `Re: [${t1.number} ${t1.thread_token}] Second Canary not showing in the fleet`, text: 'That was it — the router had put it on 5 GHz. Moved it, both are on the Wall now. Thank you!', messageId: '<demo-a2@customer.example.test>' }, { cfg: q });
  const t2 = openTicket(db, { email: PEOPLE[3][0], name: PEOPLE[3][1], subject: `Order ${o4.number} — any shipping update?`, body: 'Paid on Sunday, hoping it ships this week. No rush, just checking the batch cadence.', cfg: q, autoAck: false, channel: 'form' });
  db.run('UPDATE tickets SET created_at = ?, updated_at = ?, sla_due_at = ? WHERE id = ?', daysAgo(1, 9), daysAgo(1, 9), daysAgo(0, 9), t2.id);
  openTicket(db, { email: 'newsdesk@fictional-paper.example.test', name: 'Quinn Byline', subject: 'Press: piece on privacy-first home cameras', body: 'Writing a feature for next month. Could you say a few words on why "no footage" and how the witness log holds up in court?', cfg: q, autoAck: false, channel: 'email', kind: 'press' });
  const t4 = openTicket(db, { email: PEOPLE[4][0], name: PEOPLE[4][1], subject: 'Does it do face recognition?', body: 'Asking before I buy — my partner is nervous about cameras.', cfg: q, autoAck: false, channel: 'form' });
  reply(db, t4.id, 'No, and it can\'t: the recognition code was never written, so there is no setting to turn off. ObjectClass is Person / Vehicle / Animal / Package, nothing finer. The FAQ has the long answer.', { cfg: q, actor: 'web', close: true });
  const OLD = [
    [6, 'Can I run two hubs?', 'One in the house, one in the workshop. Do the Canaries pick one?', 'Yes — each Canary follows the hub it was provisioned against; the Wall on either hub shows its own fleet. Two hubs is two fleets, by design.', 52, 50],
    [9, 'Outdoor shell — is PETG OK in the rain?', 'Eaves, not direct rain, but it gets wet.', 'PETG under eaves is fine for years; direct sun is what fades it. The Outdoor kit\'s shell is ASA for exactly that case.', 47, 46],
    [10, 'Witness Wall says "chain verified" — what does that mean?', 'Just want to understand the green tick.', 'Every entry carries a hash of the one before it, so a gap or an edit shows. Green means the chain from the first entry to now is unbroken.', 41, 41],
    [12, 'Will this work with Home Assistant Green?', 'Bought one last month.', 'Yes: install the SecuraCV integration through HACS; it discovers the hub on the LAN. Nothing leaves the house.', 36, 35],
    [1, 'Can I buy a spare QR card?', 'Lost the one from the box before first boot.', 'Sent one first class, no charge. The card is also printable from the hub\'s Fleet page.', 31, 31],
    [15, 'Witness Pair — one box or two?', 'Giving one to my parents.', 'One box, two Canaries, two QR cards — split them however you like. Each provisions itself against its own hub.', 27, 26],
    [7, 'Firmware 2.5 release notes?', 'Saw the LED pattern change.', 'They are in the Lab under Firmware → Releases; 2.5 adds the two-blink "no hub" pattern you noticed.', 22, 21],
    [18, 'Wholesale for a hardware store', 'We have three locations in Vermont.', 'Happy to — reply with the quantities and we will quote on a purchase order (net 30).', 17, 16],
    [3, 'Does the Vision kit record video?', 'Want to be sure before it goes on the porch.', 'No. The camera is read by the detector on the board; frames are never stored or sent. What leaves the board is an ObjectClass and a time.', 13, 12],
    [11, 'Order arrived — thank you', 'Just saying thanks, set-up took four minutes.', 'That is the number we design for. Enjoy it.', 8, 8],
  ];
  for (const [who, subject, body, answer, cAge, rAge] of OLD) {
    const t = openTicket(db, { email: PEOPLE[who][0], name: PEOPLE[who][1], subject, body, cfg: q, autoAck: false, channel: who % 2 ? 'form' : 'email' });
    reply(db, t.id, `Hi ${PEOPLE[who][1].split(' ')[0]} — ${answer}\n\n— Errer Labs`, { cfg: q, actor: 'web', close: true });
    backdate(t.id, daysAgo(cAge, 9 + (who % 8)), daysAgo(rAge, 13 + (who % 5)), daysAgo(rAge, 13 + (who % 5)));
  }
  db.run("UPDATE mail_outbox SET created_at = COALESCE((SELECT t.first_response_at FROM tickets t WHERE 'ticket:' || t.number = mail_outbox.ref), created_at) WHERE ref LIKE 'ticket:%'");
  db.run("UPDATE mail_outbox SET status = 'sent', sent_at = created_at WHERE created_at < ?", daysAgo(2));
  db.run('INSERT OR REPLACE INTO canned_replies (slug, title, body) VALUES (?, ?, ?), (?, ?, ?), (?, ?, ?)',
    'batch-cadence', 'Batch cadence', 'Hi {{name}},\n\nOrders in by Sunday night ship the following week in one batch — yours is in this one. You\'ll get the tracking number the moment the label prints.\n\n— Errer Labs',
    'no-face-rec', 'Face recognition (no, and can\'t)', 'Hi {{name}},\n\nNo — and it structurally can\'t. The recognition code was never written, so there\'s no setting to turn off. The FAQ has the long answer.\n\n— Errer Labs',
    'no-hub-blink', 'Two blinks = no hub found', 'Hi {{name}},\n\nTwo blinks every few seconds is the "no hub found" pattern. Check the Canary is on the same Wi-Fi band as the hub (2.4 GHz) and that the hub\'s /api/fleet lists its MAC. Reply with what you see and I\'ll take it from there.\n\n— Errer Labs');

  const c1 = openClaim(db, { orderNumber: o1.number, serial: 'DEMO-W-0001', category: 'dead_board', description: 'Board never enumerates over USB, no LED at all. Tried two cables.', cfg: q, actor: 'web' });
  reship(db, c1.id, [{ sku: 'xiao-esp32s3', name: 'XIAO ESP32S3', qty: 1 }], { recordOrderFn: recordOrder, catalog: null, actor: 'web' });
  resolveClaim(db, c1.id, { costCents: 1090, note: 'New board shipped first class; serial DEMO-W-0005 is yours now.', cfg: q, actor: 'web' });
  openClaim(db, { orderNumber: o3.number, category: 'warped_print', description: 'Lid is bowed ~2 mm and won\'t seat on the shell.', cfg: q, actor: 'web' });
  const oldClaim = db.get('SELECT o.id, o.number FROM orders o WHERE o.status = \'delivered\' AND o.source = \'stripe\' ORDER BY o.paid_at LIMIT 1');
  if (oldClaim) { const c0 = openClaim(db, { orderNumber: oldClaim.number, category: 'missing_part', description: 'Fastener pack was not in the box.', cfg: q, actor: 'web' }); resolveClaim(db, c0.id, { costCents: 145, note: 'Fastener pack mailed in a bubble envelope.', cfg: q, actor: 'web' }); db.run('UPDATE warranty_claims SET created_at = ?, resolved_at = ? WHERE id = ?', daysAgo(44), daysAgo(43), c0.id); const ct0 = db.get('SELECT ticket_id FROM warranty_claims WHERE id = ?', c0.id)?.ticket_id; if (ct0) backdate(ct0, daysAgo(44, 11), daysAgo(43, 10), daysAgo(43, 10)); }

  const inv = createInvoice(db, { email: 'purchasing@example-academy.example.test', name: 'Morgan Buyer', org: 'Example Academy (fictional)', po_number: 'PO-DEMO-0001', terms_days: 30, bill_to: 'Example Academy (fictional)\nAttn: Morgan Buyer, Purchasing\n1 Example Campus Road\nSpringfield, MA 01101', items: [{ sku: 'wap-kit', description: 'Canary WAP Kit (education)', qty: 20, unit_cents: 6200 }, { sku: 'vision-kit', description: 'Canary Vision Kit (education)', qty: 4, unit_cents: 10700 }], shipping_cents: 2400, notes: 'Education pricing. Net 30. (Demo invoice — not a real sale.)' });
  issueInvoice(db, inv.id, q, { actor: 'web' });
  upsertSubscription(db, { external_id: 'sub_demo_0001', email: 'ops@example-coop.example.test', plan: 'Support plan — small fleet', status: 'active', amount_cents: 9900, interval: 'month', current_period_end: new Date(Date.now() + 12 * 86400000).toISOString() });
  upsertSubscription(db, { external_id: 'sub_demo_0002', email: 'facilities@example-academy.example.test', plan: 'Support plan — campus', status: 'active', amount_cents: 24900, interval: 'month', current_period_end: new Date(Date.now() + 20 * 86400000).toISOString() });
  // Payouts: Stripe paid most of the two months out weekly; this week's balance is still in Stripe.
  const owed = unreconciledStripeCents(db) - 18450; const per = Math.floor(owed / 8 / 1.03);
  for (let w = 8; w >= 1; w--) postPayout(db, { id: `po_demo_${String(w).padStart(4, '0')}`, date: daysAgo(w * 7 - 3).slice(0, 10), amount_cents: per, fee_cents: Math.round(per * 0.03) });
  for (const m of [0, 1]) {
    postExpense(db, { date: daysAgo(m * 30 + 10).slice(0, 10), account: 'expenses:software', cents: 25000, memo: 'AI operator, monthly', paidFrom: 'equity:owner' });
    postExpense(db, { date: daysAgo(m * 30 + 12).slice(0, 10), account: 'expenses:insurance', cents: 10000, memo: 'product liability, monthly' });
    postExpense(db, { date: daysAgo(m * 30 + 6).slice(0, 10), account: 'expenses:supplies', cents: 6600 + m * 1200, memo: 'PETG, 3 kg black' });
    postExpense(db, { date: daysAgo(m * 30 + 5).slice(0, 10), account: 'expenses:postage', cents: 2170 + m * 900, memo: 'labels, last batch' });
  }
  postExpense(db, { date: daysAgo(40).slice(0, 10), account: 'expenses:supplies', cents: 38900, memo: 'XIAO boards ×50, Example Distributor' });
  postExpense(db, { date: daysAgo(25).slice(0, 10), account: 'expenses:compliance', cents: 45000, memo: 'FCC pre-scan, Example Test Lab' });

  const probes = demoProbes(cfg).map(p => upsertProbe(db, p));
  for (const p of probes) {
    for (let h = 24 * 30; h > 0; h -= 2) { const ok = !(p.name.startsWith('Status JSON') && h > 70 && h < 76); db.run('INSERT INTO probe_results (probe_id, at, ok, ms, detail) VALUES (?, ?, ?, ?, ?)', p.id, new Date(Date.now() - h * 3600000).toISOString(), ok ? 1 : 0, 20 + ((h * 37) % 60), ok ? '200' : '503'); }
    db.run('UPDATE probes SET last_ok = 1, last_ms = ?, last_at = ?, last_detail = \'200\' WHERE id = ?', 24, nowIso(), p.id);
  }
  const inc = openIncident(db, { title: 'Status JSON slow for a few hours (demo incident)', body: 'A synthetic incident so the public page has history. No device is affected — a Canary never talks to this service.' });
  db.run('UPDATE incidents SET started_at = ?, updated_at = ? WHERE id = ?', daysAgo(3, 8), daysAgo(3, 11), inc.id); updateIncident(db, inc.id, { status: 'resolved' });

  // Mail: a newsletter that went out three weeks ago (so its report has a week of orders and replies to count), one scheduled, one draft; the three default automations with their history.
  const { queueCampaign, scheduleCampaign } = await import('./campaigns.mjs'); const { seedAutomations, runAutomations } = await import('./automations.mjs');
  createCampaign(db, { slug: 'october-from-the-bench', subject: 'October from the bench', preheader: 'Vision kits are back, the Outdoor shell moved to ASA', segment: 'all', body_text: 'Hi {{first_name}},\n\nTwo things from the bench this month.\n\n# Vision kits are back\n\nThe camera boards landed, the shells are printed, and the kit is in stock for this batch: {{site}}/store\n\n# The Outdoor shell moved to ASA\n\nPETG was fine under eaves and faded in direct sun. ASA does not. Every Outdoor kit from this batch ships in it, and the old shell\'s STL stays in the Lab for anyone printing their own.\n\n- Firmware 2.5 adds the two-blink "no hub found" pattern\n- The Witness Wall shows the verified chain in green\n\nIf you joined the waitlist, you\'ll get the one in-stock mail you asked for and nothing else.\n\n— {{company}}' });
  queueCampaign(db, 'october-from-the-bench', q, { actor: 'web' });
  // it went out on the day, two to six weeks back, after which the most recipients ordered — so the report has something honest to count
  let bestDay = 21, best = -1;
  for (let d = 14; d <= 42; d++) { const n = db.get("SELECT COUNT(*) AS n FROM orders o JOIN contacts c ON c.id = o.contact_id WHERE c.email IN (SELECT to_addr FROM mail_outbox WHERE ref = 'campaign:october-from-the-bench') AND o.paid_at >= ? AND o.paid_at < ?", daysAgo(d, 9), daysAgo(d - 7, 9)).n; if (n > best) { best = n; bestDay = d; } }
  db.run("UPDATE campaigns SET sent_at = ? WHERE slug = 'october-from-the-bench'", daysAgo(bestDay, 9));
  db.run("UPDATE mail_outbox SET created_at = ?, status = 'sent', sent_at = ? WHERE ref = 'campaign:october-from-the-bench'", daysAgo(bestDay, 9), daysAgo(bestDay, 9));
  createCampaign(db, { slug: 'november-batch-note', subject: 'November: the Witness Pair, and a question', preheader: 'One question for everyone who runs two', segment: 'customers', body_text: 'Hi {{first_name}},\n\nThe November batch ships next week. Two things:\n\n# The Witness Pair now provisions as a pair\n\nTwo QR cards, one scan each, both on the Wall in under a minute.\n\n# A question\n\nIf you run two or more Canaries: what would you want the Wall to show about them together? Reply to this mail — one line is plenty.\n\n— {{company}}' });
  scheduleCampaign(db, 'november-batch-note', new Date(Date.now() + 26 * 3600000).toISOString(), { actor: 'web' });
  createCampaign(db, { slug: 'vision-restock-waitlist', subject: 'Draft: the Vision kit is back', segment: 'waitlist:vision-kit', body_text: 'Hi {{first_name}},\n\nYou asked to hear when the Vision kit came back. It is back: {{site}}/store\n\n— {{company}}' });
  seedAutomations(db);
  runAutomations(db, q, { now: Date.now() });
  db.run('UPDATE automation_runs SET sent_at = due_at WHERE sent_at IS NOT NULL');
  db.run("UPDATE mail_outbox SET created_at = COALESCE((SELECT r.due_at FROM automation_runs r WHERE 'automation:' || r.automation_slug || ':' || r.ref = mail_outbox.ref), created_at) WHERE kind = 'automation'");
  db.run("UPDATE mail_outbox SET status = 'sent', sent_at = created_at WHERE kind = 'automation' AND created_at < ?", daysAgo(2));
  db.run("UPDATE audit_log SET at = ? WHERE action = 'campaign.queue'", daysAgo(bestDay, 9));
  // What the carrier said about this week's parcels (a demo never calls USPS; the shape is the real one).
  db.run("UPDATE orders SET tracking_status = 'out_for_delivery', tracking_detail = 'Out for Delivery, Springfield MA', tracking_checked_at = ?, expected_delivery_at = ? WHERE id = ?", daysAgo(0, 8), nowIso().slice(0, 10), o2.id);
  db.run("UPDATE orders SET tracking_status = 'in_transit', tracking_detail = 'In Transit to Next Facility', tracking_checked_at = ?, expected_delivery_at = ? WHERE id = ?", daysAgo(0, 8), new Date(Date.now() + 86400000).toISOString().slice(0, 10), o3.id);
  for (const [o, t] of [[o2, 'Out for delivery — Springfield MA'], [o3, 'In transit — next facility']]) db.run('INSERT INTO order_events (order_id, at, actor, kind, text) VALUES (?, ?, ?, ?, ?)', o.id, daysAgo(0, 8), 'carrier', 'tracking', t);
  // The warped-lid claim came with a photo.
  { const wt = db.get("SELECT t.id FROM tickets t JOIN warranty_claims w ON w.ticket_id = t.id WHERE w.category = 'warped_print'"); const m = wt && db.get("SELECT id FROM ticket_messages WHERE ticket_id = ? AND direction = 'in' ORDER BY id LIMIT 1", wt.id); if (m) saveAttachments(db, m.id, [{ filename: 'lid-bowed.png', mime: 'image/png', bytes: tinyPng(96, 64) }]); }
  // Parts on order for the Vision kits, due next week; the printed shells drafted as a print run, waiting for a yes.
  const po2 = createPo(db, { supplier: 'Example Distributor', supplier_url: 'https://distributor.example.test/order', items: [{ sku: 'xiao-esp32s3-sense', qty: 12 }, { sku: 'grove-vision-ai-v2', qty: 10 }], notes: 'Vision kit boards for the November batches.', actor: 'web' });
  markOrdered(db, po2.id, { expected_at: new Date(Date.now() + 6 * 86400000).toISOString().slice(0, 10), actor: 'web' });
  db.run('UPDATE purchase_orders SET created_at = ?, ordered_at = ? WHERE id = ?', daysAgo(3), daysAgo(3, 14), po2.id);
  // The school's invoice is past due — the reminder is drafted on Home, not sent.
  db.run('UPDATE invoices SET issued_at = ?, due_at = ? WHERE id = ?', daysAgo(36).slice(0, 10), daysAgo(6).slice(0, 10), inv.id);
  db.run("UPDATE mail_outbox SET created_at = ?, status = 'sent', sent_at = ? WHERE ref = ?", daysAgo(36), daysAgo(36), `invoice:${inv.number}`);
  move(db, 'xiao-esp32s3-sense', -1, 'scrap', 'bench: bad USB-C port');
  await raise(db, q, 'stock:xiao-esp32s3-sense', 'inventory [reorder]: xiao-esp32s3-sense', 'XIAO ESP32S3 Sense (camera): 2 on hand, reorder point 4, reorder qty 12.\nSupplier: Example Distributor');
  for (const po of draftReorder(db, { actor: 'system' })) db.run('UPDATE purchase_orders SET created_at = ? WHERE id = ?', daysAgo(1, 6), po.id);
  // The audit log says when each thing happened, not when the seed ran.
  db.run(`UPDATE audit_log SET at = COALESCE((SELECT ev.at FROM order_events ev JOIN orders o ON o.id = ev.order_id WHERE o.number = audit_log.entity_id AND ev.kind = CASE audit_log.action WHEN 'order.create' THEN 'placed' WHEN 'order.refund' THEN 'refund' ELSE replace(audit_log.action, 'order.', '') END ORDER BY ev.id LIMIT 1), at) WHERE entity = 'order'`);
  db.run(`UPDATE audit_log SET at = COALESCE((SELECT CASE audit_log.action WHEN 'ticket.open' THEN t.created_at WHEN 'ticket.reply' THEN t.first_response_at ELSE t.closed_at END FROM tickets t WHERE t.number = audit_log.entity_id OR CAST(t.id AS TEXT) = audit_log.entity_id), at) WHERE entity = 'ticket'`);
  db.run(`UPDATE audit_log SET at = COALESCE((SELECT CASE audit_log.action WHEN 'warranty.resolve' THEN w.resolved_at ELSE w.created_at END FROM warranty_claims w WHERE w.number = audit_log.entity_id), at) WHERE entity = 'warranty'`);
  db.run('DELETE FROM audit_log WHERE action IN (\'auth.setup\')');
  const n = { orders: db.get('SELECT COUNT(*) AS n FROM orders').n, tickets: db.get('SELECT COUNT(*) AS n FROM tickets').n, contacts: db.get('SELECT COUNT(*) AS n FROM contacts').n, items: db.get('SELECT COUNT(*) AS n FROM inventory_items').n, discounts: db.get('SELECT COUNT(*) AS n FROM discounts').n };
  db.audit('demo', 'demo.seed', null, null, n);
  return n;
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
  const tables = ['attachments', 'purchase_order_items', 'purchase_orders', 'recalls', 'ticket_messages', 'tickets', 'warranty_claims', 'order_events', 'order_items', 'orders', 'discounts', 'invoice_items', 'invoices', 'subscriptions', 'inventory_moves', 'recipes', 'inventory_items', 'devices', 'waitlist', 'contacts', 'mail_outbox', 'mail_seen', 'campaigns', 'probe_results', 'probes', 'incidents', 'ledger', 'exceptions', 'canned_replies', 'stripe_events', 'job_runs', 'audit_log'];
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
