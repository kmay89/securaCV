import { test } from 'node:test';
import assert from 'node:assert/strict';
import { recordOrder, transition, refund, orderByNumber, fulfillCsv, weekLabel, listOrders } from '../src/modules/orders.mjs';
import { upsertItem, setRecipe, item, buildable } from '../src/modules/inventory.mjs';
import { trialBalance, profitAndLoss } from '../src/modules/ledger.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

test('an order is recorded once, SKUs resolve against the catalog, COGS freezes, books balance', () => {
  const db = testDb(); const cat = catalog();
  const { order, created } = recordOrder(db, sampleOrder(), { catalog: cat, bomTotals: { 'canary-wap': 20 } });
  assert.ok(created); assert.equal(order.number, 'EL-000001'); assert.equal(order.status, 'paid'); assert.equal(order.ship_state, 'MA');
  assert.equal(order.items[0].cogs_cents, 2900, 'extra_usd 9 + canary-wap BOM 20');
  assert.equal(order.batch, weekLabel(new Date('2026-10-19T12:00:00Z')));
  const again = recordOrder(db, sampleOrder(), { catalog: cat });
  assert.ok(!again.created); assert.equal(again.order.id, order.id);
  assert.throws(() => recordOrder(db, sampleOrder({ external_id: 'cs_2', items: [{ sku: 'mystery-box', name: 'Mystery', qty: 1, unit_cents: 1 }] }), { catalog: cat }), /unknown SKU/);
  assert.ok(recordOrder(db, sampleOrder({ external_id: 'cs_3', items: [{ sku: 'Canary WAP Kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }] }), { catalog: cat }).order.items[0].sku === 'wap-kit', 'resolves by exact name too');
  const tb = trialBalance(db); assert.ok(tb.balanced);
  assert.equal(tb.rows.find(r => r.account === 'income:sales').balance_cents, 6900 * 2, 'two orders recorded (cs_2 was refused)');
  assert.equal(tb.rows.find(r => r.account === 'expenses:cogs').balance_cents, 2900 + 900 * 2, 'bomTotals only on the first; the second is extra_usd × 2');
});

test('paid → packed consumes the recipe; shipped queues the tracking mail; refund posts to the books', () => {
  const db = testDb(); const cfg = testCfg();
  upsertItem(db, { sku: 'xiao-esp32s3', name: 'XIAO ESP32S3', on_hand: 5 }); upsertItem(db, { sku: 'wap-shell', name: 'printed shell', kind: 'printed', on_hand: 2 });
  setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1, 'wap-shell': 1 });
  assert.equal(buildable(db, 'wap-kit'), 2);
  const { order } = recordOrder(db, sampleOrder(), { catalog: catalog() });
  assert.throws(() => transition(db, order.id, 'shipped'), /cannot move/);
  transition(db, order.id, 'packed');
  assert.equal(item(db, 'xiao-esp32s3').on_hand, 4); assert.equal(item(db, 'wap-shell').on_hand, 1);
  transition(db, order.id, 'shipped', { carrier: 'USPS', tracking: '9400 1', cfg });
  const mail = db.get('SELECT * FROM mail_outbox WHERE ref = ?', 'order:EL-000001');
  assert.ok(mail && mail.to_addr === 'ada@example.test' && mail.body_text.includes('Tracking: 94001'), 'the tracking number is mailed as the carrier prints it (no spaces)');
  assert.equal(orderByNumber(db, 'EL-000001').status, 'shipped');
  refund(db, order.id, 800, { reason: 'shipping overcharge' });
  assert.equal(orderByNumber(db, 'EL-000001').refunded_cents, 800);
  refund(db, order.id, 99999);
  assert.equal(orderByNumber(db, 'EL-000001').status, 'refunded');
  assert.throws(() => refund(db, order.id, 1), /nothing to refund/);
  const tb = trialBalance(db); assert.ok(tb.balanced);
  assert.equal(profitAndLoss(db, { from: '2026-01-01', to: '2026-12-31' }).lines.find(l => l.account === 'income:refunds').cents, -7700);
});

test('the fulfill.mjs CSV carries one row per item with the catalog SKU', () => {
  const db = testDb();
  recordOrder(db, sampleOrder({ items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }, { sku: 'wap-parts', name: 'WAP Enclosure Set', qty: 1, unit_cents: 1500 }] }), { catalog: catalog() });
  const csv = fulfillCsv(db);
  const lines = csv.trim().split('\n');
  assert.equal(lines[0], 'order_id,email,name,sku,qty,line1,line2,city,state,zip,country');
  assert.equal(lines.length, 3);
  assert.ok(lines[1].startsWith('EL-000001,ada@example.test,Ada Lovelace,wap-kit,2,'));
  assert.equal(listOrders(db, { q: 'Lovelace' }).length, 1);
});

test('undoing a pack restores exactly what packing took; an order with no items cannot be packed', () => {
  const db = testDb();
  upsertItem(db, { sku: 'xiao-esp32s3', name: 'XIAO', on_hand: 5 }); setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1 });
  const { order } = recordOrder(db, sampleOrder({ items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }] }), { catalog: catalog() });
  transition(db, order.id, 'packed'); assert.equal(item(db, 'xiao-esp32s3').on_hand, 3);
  transition(db, order.id, 'paid'); assert.equal(item(db, 'xiao-esp32s3').on_hand, 5, 'unpack put both back');
  transition(db, order.id, 'packed'); assert.equal(item(db, 'xiao-esp32s3').on_hand, 3, 'a second pack consumes once, not twice');
  const empty = recordOrder(db, sampleOrder({ external_id: 'cs_empty', items: [] }), { catalog: catalog() }).order;
  assert.throws(() => transition(db, empty.id, 'packed'), /no line items/);
});
