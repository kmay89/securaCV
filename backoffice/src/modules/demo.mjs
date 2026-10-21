// `backoffice demo`: a realistic week of the company in an empty database,
// so the admin can be explored before a single real order exists. Nothing
// here leaves the box — no mail is sent (no SMTP_URL), no probe runs.
import { nowIso } from '../db.mjs';
import { normalize } from './catalog.mjs';
import { upsertContact, requestConsent, confirmConsent, joinWaitlist } from './contacts.mjs';
import { recordOrder, transition, refund } from './orders.mjs';
import { upsertItem, move, setRecipe } from './inventory.mjs';
import { registerDevice, assignToOrder } from './devices.mjs';
import { openClaim } from './warranty.mjs';
import { openTicket, ingestEmail, reply, addMessage } from './support.mjs';
import { createInvoice, issueInvoice } from './invoices.mjs';
import { postExpense, postPayout } from './ledger.mjs';
import { upsertProbe, openIncident, updateIncident } from './status.mjs';
import { upsertSubscription } from './subscriptions.mjs';
import { createCampaign } from './campaigns.mjs';
import { raise } from './exceptions.mjs';

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

const daysAgo = (n, h = 10) => new Date(Date.now() - n * 86400000 - (24 - h) * 3600000).toISOString();

export async function seedDemo(db, cfg) {
  if (db.get('SELECT COUNT(*) AS n FROM orders').n) throw new Error('the database already has orders — demo data only goes into an empty one (point BACKOFFICE_DATA_DIR somewhere fresh)');
  const catalog = normalize(DEMO_STORE);
  const bom = { 'canary-wap': 21.4, 'canary-vision': 38.9 };

  // inventory + recipes
  for (const it of [
    ['xiao-esp32s3', 'XIAO ESP32S3 (Seeed)', 'part', 14, 8, 25, 750, 'Seeed / Digi-Key'],
    ['xiao-esp32s3-sense', 'XIAO ESP32S3 Sense (camera)', 'part', 3, 4, 12, 1390, 'Seeed'],
    ['grove-vision-ai-v2', 'Grove Vision AI V2', 'part', 5, 3, 10, 1600, 'Seeed'],
    ['cable-usbc-1m', 'USB-C cable, 1 m', 'part', 40, 15, 50, 140, 'Amazon Business'],
    ['sd-32gb', 'microSD 32 GB (Sandisk HE)', 'part', 9, 6, 20, 690, 'B&H'],
    ['wap-shell-petg', 'WAP shell + lid, PETG (printed)', 'printed', 6, 4, 12, 180, 'in-house'],
    ['vision-shell-petg', 'Vision shell + hinge, PETG (printed)', 'printed', 2, 3, 8, 260, 'in-house'],
    ['hardware-pack-wap', 'Fastener pack, WAP (M2 ×6, standoffs)', 'part', 30, 10, 50, 45, 'McMaster'],
    ['box-small', 'Shipping box, small', 'packaging', 22, 10, 50, 62, 'Uline'],
    ['qr-card', 'First-boot QR card', 'packaging', 60, 20, 100, 8, 'in-house'],
  ]) upsertItem(db, { sku: it[0], name: it[1], kind: it[2], on_hand: it[3], reorder_point: it[4], reorder_qty: it[5], unit_cost_cents: it[6], supplier: it[7] });
  setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'wap-kit-outdoor', { 'xiao-esp32s3': 1, 'wap-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'hardware-pack-wap': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'vision-kit', { 'xiao-esp32s3-sense': 1, 'grove-vision-ai-v2': 1, 'vision-shell-petg': 1, 'cable-usbc-1m': 1, 'sd-32gb': 1, 'box-small': 1, 'qr-card': 1 });
  setRecipe(db, 'witness-pair', { 'xiao-esp32s3': 2, 'wap-shell-petg': 2, 'cable-usbc-1m': 2, 'sd-32gb': 2, 'hardware-pack-wap': 2, 'box-small': 1, 'qr-card': 2 });
  setRecipe(db, 'wap-parts', { 'wap-shell-petg': 1, 'hardware-pack-wap': 1 });

  // devices from the jig
  const batch = 'J-2026-10-A';
  for (let i = 1; i <= 8; i++) registerDevice(db, { serial: `CW-26A-${String(i).padStart(4, '0')}`, mac: `84:FC:E6:10:2${i % 10}:${String(10 + i).padStart(2, '0')}`, board: 'xiao-esp32s3', product_sku: 'wap-kit', fw_version: '2.5.0', fw_sha256: 'c0ffee11' + 'ab'.repeat(28), flashed_at: daysAgo(5), batch });
  for (let i = 1; i <= 2; i++) registerDevice(db, { serial: `CV-26A-${String(i).padStart(4, '0')}`, mac: `84:FC:E6:20:1${i}:4${i}`, board: 'xiao-esp32s3-sense', product_sku: 'vision-kit', fw_version: '2.5.0', fw_sha256: 'facade22' + 'cd'.repeat(28), flashed_at: daysAgo(5), batch });

  // orders across the week
  const people = [
    ['ada@example.test', 'Ada Lovelace', '1 Analytical Way', 'Boston', 'MA', '02134'],
    ['grace@example.test', 'Grace Hopper', '220 Compiler Ct', 'Arlington', 'VA', '22201'],
    ['katherine@example.test', 'Katherine Johnson', '9 Orbit Lane', 'Hampton', 'VA', '23666'],
    ['lin@example.test', 'Lin Tanaka', '48 Harbor St Apt 3', 'Portland', 'OR', '97209'],
    ['sam@example.test', 'Sam Okafor', '7700 Mesa Rd', 'Austin', 'TX', '78731'],
    ['june@example.test', 'June Park', '15 Maple Ave', 'Madison', 'WI', '53703'],
  ];
  const mk = (i, items, days, extra = {}) => {
    const p = people[i]; const sub = items.reduce((s, it) => s + catalog.products.get(it.sku).price_cents * it.qty, 0);
    return recordOrder(db, { external_id: `cs_demo_${days}_${i}`, payment_intent: `pi_demo_${days}_${i}`, source: 'stripe', email: p[0], name: p[1], subtotal_cents: sub, shipping_cents: 800, tax_cents: Math.round(sub * 0.0625), total_cents: sub + 800 + Math.round(sub * 0.0625),
      ship: { line1: p[2], city: p[3], state: p[4], zip: p[5] }, items: items.map(it => ({ ...it, name: catalog.products.get(it.sku).name, unit_cents: catalog.products.get(it.sku).price_cents })), paid_at: daysAgo(days), ...extra }, { catalog, bomTotals: bom, actor: 'demo' }).order;
  };
  const o1 = mk(0, [{ sku: 'wap-kit', qty: 1 }], 9);               // delivered, then a warranty claim
  const o2 = mk(1, [{ sku: 'witness-pair', qty: 1 }], 8);          // shipped
  const o3 = mk(2, [{ sku: 'vision-kit', qty: 1 }, { sku: 'wap-parts', qty: 2 }], 6); // shipped
  const o4 = mk(3, [{ sku: 'wap-kit-outdoor', qty: 1 }], 2);       // packed, waiting on a label
  const o5 = mk(4, [{ sku: 'wap-kit', qty: 2 }], 1);               // paid, this batch
  const o6 = mk(5, [{ sku: 'vision-parts', qty: 1 }], 0);          // paid, this batch
  const silent = { ...cfg, supportAddr: '' };
  for (const [o, serials] of [[o1, ['CW-26A-0001']], [o2, ['CW-26A-0002', 'CW-26A-0003']], [o3, ['CV-26A-0001']], [o4, ['CW-26A-0004']]]) {
    transition(db, o.id, 'packed', { actor: 'demo' });
    for (const s of serials) assignToOrder(db, s, o.id, { actor: 'demo' });
  }
  transition(db, o1.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: '9400 1000 0000 0000 0001 11', cfg: silent }); transition(db, o1.id, 'delivered', { actor: 'demo' });
  transition(db, o2.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: '9400 1000 0000 0000 0002 22', cfg: silent });
  transition(db, o3.id, 'shipped', { actor: 'demo', carrier: 'USPS', tracking: '9400 1000 0000 0000 0003 33', cfg: silent });
  refund(db, o3.id, 1500, { actor: 'demo', reason: 'one parts pack short — shipped the other, refunded this one' });
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ?, delivered_at = ? WHERE id = ?', daysAgo(8), daysAgo(7), daysAgo(4), o1.id);
  db.run('UPDATE orders SET packed_at = ?, shipped_at = ? WHERE id IN (?, ?)', daysAgo(5), daysAgo(4), o2.id, o3.id);
  void o5; void o6;

  // contacts, consent, waitlist
  const nour = upsertContact(db, { email: 'nour@example.test', name: 'Nour Haddad', source: 'waitlist' }); joinWaitlist(db, nour.id, 'vision-kit', 1); confirmConsent(db, requestConsent(db, nour.id));
  const dev = upsertContact(db, { email: 'devi@example.test', name: 'Devi Raman', source: 'waitlist' }); joinWaitlist(db, dev.id, 'vision-kit', 2);
  const tomas = upsertContact(db, { email: 'tomas@example.test', name: 'Tomás Ruiz', source: 'waitlist' }); joinWaitlist(db, tomas.id, 'witness-pair', 1); confirmConsent(db, requestConsent(db, tomas.id));
  confirmConsent(db, requestConsent(db, db.get('SELECT id FROM contacts WHERE email = ?', 'ada@example.test').id));
  upsertContact(db, { email: 'procurement@lincoln-hs.example', name: 'Marcus Bell', org: 'Lincoln High School', kind: 'school', source: 'invoice' });

  // support: a thread in progress, a fresh one, a press ask, a closed one
  const t1 = openTicket(db, { email: 'grace@example.test', name: 'Grace Hopper', subject: 'Second Canary not showing in the fleet', body: 'Both WAPs flashed fine and joined Wi-Fi, but only one shows on the Witness Wall. The LED on the second blinks twice every few seconds. Do I need to pair them?', cfg: silent, autoAck: false, channel: 'email', messageId: '<a1@example.test>' });
  reply(db, t1.id, 'Hi Grace — two blinks is the "no hub found" pattern, not a pairing step (Canaries never pair with each other). Two quick checks: is the second one on the same Wi-Fi band as the hub, and does the hub\'s /api/fleet list its MAC? If the band is the issue, the Lab\'s Wi-Fi bench has the fix in two taps. Reply with what /api/fleet shows and I\'ll take it from there.', { cfg: silent, actor: 'demo' });
  ingestEmail(db, { from: 'Grace Hopper <grace@example.test>', subject: `Re: [${t1.number} ${t1.thread_token}] Second Canary not showing in the fleet`, text: 'That was it — the router had put it on 5 GHz. Moved it, both are on the Wall now. Thank you!', messageId: '<a2@example.test>' }, { cfg: silent });
  const t2 = openTicket(db, { email: 'lin@example.test', name: 'Lin Tanaka', subject: 'Order EL-000004 — any shipping update?', body: 'Paid on Sunday, hoping it ships this week. No rush, just checking the batch cadence.', cfg: silent, autoAck: false, channel: 'form' });
  db.run('UPDATE tickets SET created_at = ?, updated_at = ?, sla_due_at = ? WHERE id = ?', daysAgo(1, 9), daysAgo(1, 9), daysAgo(0, 9), t2.id);
  openTicket(db, { email: 'reporter@paper.example', name: 'R. Osei', subject: 'Press: piece on privacy-first home cameras', body: 'Writing a feature for next month. Could you say a few words on why "no footage" and how the witness log holds up in court?', cfg: silent, autoAck: false, channel: 'email', kind: 'press' });
  const t4 = openTicket(db, { email: 'sam@example.test', name: 'Sam Okafor', subject: 'Does it do face recognition?', body: 'Asking before I buy — my partner is nervous about cameras.', cfg: silent, autoAck: false, channel: 'form' });
  reply(db, t4.id, 'No, and it can\'t: the recognition code was never written, so there is no setting to turn off. ObjectClass is Person / Vehicle / Animal / Package, nothing finer. The FAQ has the long answer. (And welcome, Sam — your order landed this morning.)', { cfg: silent, actor: 'demo', close: true });
  db.run('INSERT INTO canned_replies (slug, title, body) VALUES (?, ?, ?), (?, ?, ?)',
    'batch-cadence', 'Batch cadence', 'Hi {{name}},\n\nOrders in by Sunday night ship the following week in one batch — yours is in this one. You\'ll get the tracking number the moment the label prints.\n\n— Errer Labs',
    'no-face-rec', 'Face recognition (no, and can\'t)', 'Hi {{name}},\n\nNo — and it structurally can\'t. The recognition code was never written, so there\'s no setting to turn off. The FAQ at securacv.com/faq has the long answer.\n\n— Errer Labs');

  // warranty: a dead board, reshipped and resolved; a fresh warped print
  const c1 = openClaim(db, { orderNumber: o1.number, serial: 'CW-26A-0001', category: 'dead_board', description: 'Board never enumerates over USB, no LED at all. Tried two cables.', cfg: silent, actor: 'demo' });
  const { reship, resolveClaim } = await import('./warranty.mjs');
  reship(db, c1.id, [{ sku: 'xiao-esp32s3', name: 'XIAO ESP32S3', qty: 1 }], { recordOrderFn: recordOrder, catalog: null, actor: 'demo' });
  resolveClaim(db, c1.id, { costCents: 1090, note: 'New board shipped first class; serial CW-26A-0005 is yours now.', cfg: silent, actor: 'demo' });
  openClaim(db, { orderNumber: o3.number, category: 'warped_print', description: 'Lid is bowed ~2 mm and won\'t seat on the shell.', cfg: silent, actor: 'demo' });

  // invoice to a school, support plan, books
  const inv = createInvoice(db, { email: 'procurement@lincoln-hs.example', name: 'Marcus Bell', org: 'Lincoln High School', po_number: 'PO-2026-1188', terms_days: 30, bill_to: 'Lincoln High School\nAttn: Marcus Bell, Procurement\n400 Lincoln Way\nMadison, WI 53703', items: [{ sku: 'wap-kit', description: 'Canary WAP Kit (education)', qty: 20, unit_cents: 6200 }, { sku: 'vision-kit', description: 'Canary Vision Kit (education)', qty: 4, unit_cents: 10700 }], shipping_cents: 2400, notes: 'Education pricing per LICENSING.md §3. Net 30.' });
  issueInvoice(db, inv.id, silent, { actor: 'demo' });
  upsertSubscription(db, { external_id: 'sub_demo_1', email: 'ops@northfield-coop.example', plan: 'Support plan — small fleet', status: 'active', amount_cents: 9900, interval: 'month', current_period_end: new Date(Date.now() + 12 * 86400000).toISOString() });
  postPayout(db, { id: 'po_demo_1', date: daysAgo(3).slice(0, 10), amount_cents: 24866, fee_cents: 784 });
  postExpense(db, { date: daysAgo(6).slice(0, 10), account: 'expenses:supplies', cents: 6600, memo: 'PETG, 3 kg black' });
  postExpense(db, { date: daysAgo(5).slice(0, 10), account: 'expenses:postage', cents: 2170, memo: 'labels, batch W42' });
  postExpense(db, { date: daysAgo(10).slice(0, 10), account: 'expenses:software', cents: 25000, memo: 'Claude, monthly', paidFrom: 'equity:owner' });
  postExpense(db, { date: daysAgo(12).slice(0, 10), account: 'expenses:insurance', cents: 10000, memo: 'product liability, monthly' });

  // status, incidents, probes with a history
  const probes = [
    ['Website', cfg.siteOrigin + '/', 'SecuraCV'], ['Store catalog', cfg.siteOrigin + '/store.json', '"groups"'],
    ['The Lab (browser flasher)', 'https://kmay89.github.io/securaCV/canary-local/', null], ['Firmware releases (OTA host)', 'https://github.com/kmay89/securaCV/releases', null],
  ].map(([name, url, expect_text]) => upsertProbe(db, { name, url, expect_text }));
  for (const p of probes) {
    for (let h = 24 * 30; h > 0; h -= 2) { const ok = !(p.name.startsWith('The Lab') && h > 70 && h < 76); db.run('INSERT INTO probe_results (probe_id, at, ok, ms, detail) VALUES (?, ?, ?, ?, ?)', p.id, new Date(Date.now() - h * 3600000).toISOString(), ok ? 1 : 0, 180 + Math.round(Math.random() * 220), ok ? '200' : '503'); }
    db.run('UPDATE probes SET last_ok = 1, last_ms = ?, last_at = ?, last_detail = \'200\' WHERE id = ?', 210, nowIso(), p.id);
  }
  const inc = openIncident(db, { title: 'Lab pages slow to load from Europe', body: 'GitHub Pages CDN degradation; flashing still works, pages take 10–20 s. No device is affected — a Canary never talks to us.' });
  db.run('UPDATE incidents SET started_at = ?, updated_at = ? WHERE id = ?', daysAgo(3, 8), daysAgo(3, 11), inc.id); updateIncident(db, inc.id, { status: 'resolved' });

  // a draft campaign and one live exception
  createCampaign(db, { slug: 'oct-batch-note', subject: 'October batch: Vision kits back, and a note on the Outdoor shell', segment: 'all', body_text: 'Hi {{name}},\n\nTwo things from the bench this month:\n\n1. Vision kits are back in stock after the Grove board price drop — same kit, $119.\n2. The Outdoor shell moved to ASA; every kit since W41 ships with it.\n\nNothing to do if you already have yours. If you joined the waitlist, you\'ll get the one in-stock mail you asked for and nothing else.\n\n— Errer Labs\n\nUnsubscribe in one click: {{unsubscribe}}\n' });
  move(db, 'xiao-esp32s3-sense', -1, 'scrap', 'bench: bad USB-C port');
  await raise(db, { ...cfg, githubToken: '' }, 'stock:xiao-esp32s3-sense', 'inventory [reorder]: xiao-esp32s3-sense', 'XIAO ESP32S3 Sense (camera): 2 on hand, reorder point 4, reorder qty 12.\nSupplier: Seeed');
  db.audit('demo', 'demo.seed', null, null, { orders: 7, tickets: 4 });
  return { orders: db.get('SELECT COUNT(*) AS n FROM orders').n, tickets: db.get('SELECT COUNT(*) AS n FROM tickets').n, contacts: db.get('SELECT COUNT(*) AS n FROM contacts').n, items: db.get('SELECT COUNT(*) AS n FROM inventory_items').n };
}
