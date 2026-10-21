import { test } from 'node:test';
import assert from 'node:assert/strict';
import { upsertItem, move, item, lowStock, moves, setRecipe } from '../src/modules/inventory.mjs';
import { upsertContact, requestConsent, confirmConsent, unsubscribe, canMarket, joinWaitlist, waitlistFor, segment, validEmail } from '../src/modules/contacts.mjs';
import { testDb } from './helpers.mjs';

test('inventory moves are logged, never go negative, and low stock is derived', () => {
  const db = testDb();
  upsertItem(db, { sku: 'filament-petg-black', name: 'PETG black 1kg', on_hand: 3, reorder_point: 2, reorder_qty: 5, unit_cost_cents: 2200 });
  move(db, 'filament-petg-black', -2, 'build', 'batch:2026-W43');
  assert.equal(item(db, 'filament-petg-black').on_hand, 1);
  assert.throws(() => move(db, 'filament-petg-black', -5, 'scrap'), /cannot move/);
  assert.throws(() => move(db, 'nope', 1, 'receive'), /unknown inventory sku/);
  assert.equal(lowStock(db)[0].sku, 'filament-petg-black');
  move(db, 'filament-petg-black', 10, 'receive', 'po:1');
  assert.equal(lowStock(db).length, 0);
  assert.equal(moves(db, 'filament-petg-black').length, 2);
  assert.throws(() => setRecipe(db, 'wap-kit', { 'not-an-item': 1 }), /not in inventory/);
});

test('contacts: double opt-in, one-click unsubscribe, waitlist, segments', () => {
  const db = testDb();
  assert.ok(validEmail('a@b.co')); assert.ok(!validEmail('nope'));
  const c = upsertContact(db, { email: 'Ada@Example.TEST', name: 'Ada', source: 'waitlist' });
  assert.equal(c.email, 'ada@example.test');
  assert.equal(upsertContact(db, { email: 'ada@example.test' }).id, c.id, 'idempotent by email');
  assert.ok(!canMarket(c));
  const t = requestConsent(db, c.id);
  assert.equal(confirmConsent(db, 'bad'), null);
  assert.ok(canMarket(confirmConsent(db, t)));
  joinWaitlist(db, c.id, 'wap-kit', 2); joinWaitlist(db, c.id, 'wap-kit', 3);
  assert.equal(waitlistFor(db, 'wap-kit').length, 1); assert.equal(waitlistFor(db, 'wap-kit')[0].qty, 3);
  assert.equal(segment(db, 'all').length, 1); assert.equal(segment(db, 'waitlist:wap-kit').length, 1); assert.equal(segment(db, 'waitlist:vision-kit').length, 0);
  assert.throws(() => segment(db, 'bogus'), /unknown segment/);
  const u = unsubscribe(db, c.unsubscribe_token);
  assert.ok(u && !canMarket(u)); assert.equal(segment(db, 'all').length, 0);
});
