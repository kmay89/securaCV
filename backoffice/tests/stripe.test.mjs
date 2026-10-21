import { test } from 'node:test';
import assert from 'node:assert/strict';
import { verifyStripeSignature, signStripePayload, eventToOrder, parseCsv, detectColumns, csvRowToOrder } from '../src/modules/stripe.mjs';

test('webhook signature: good, tampered, wrong secret, stale', () => {
  const body = '{"id":"evt_1","type":"checkout.session.completed"}';
  const t = 1_700_000_000;
  const h = signStripePayload(body, 'whsec_a', t);
  assert.ok(verifyStripeSignature(body, h, 'whsec_a', { now: t + 10 }));
  assert.ok(!verifyStripeSignature(body + ' ', h, 'whsec_a', { now: t + 10 }), 'tampered body');
  assert.ok(!verifyStripeSignature(body, h, 'whsec_b', { now: t + 10 }), 'wrong secret');
  assert.ok(!verifyStripeSignature(body, h, 'whsec_a', { now: t + 1000 }), 'outside tolerance (replay)');
  assert.ok(!verifyStripeSignature(body, '', 'whsec_a'));
  assert.ok(!verifyStripeSignature(body, h, ''));
});

test('checkout.session.completed maps to an order with items, address and totals', () => {
  const ev = { type: 'checkout.session.completed', data: { object: { id: 'cs_1', payment_status: 'paid', currency: 'usd', amount_subtotal: 6900, amount_total: 7700, created: 1760000000,
    total_details: { amount_shipping: 800, amount_tax: 0 }, customer_details: { email: 'ada@example.test', name: 'Ada' },
    shipping_details: { name: 'Ada Lovelace', address: { line1: '1 Analytical Way', city: 'Boston', state: 'MA', postal_code: '02134', country: 'US' } },
    line_items: { data: [{ description: 'Canary WAP Kit', quantity: 1, price: { unit_amount: 6900, product: { metadata: { sku: 'wap-kit' } } } }] } } } };
  const o = eventToOrder(ev);
  assert.equal(o.kind, 'order'); assert.equal(o.external_id, 'cs_1'); assert.equal(o.total_cents, 7700); assert.equal(o.shipping_cents, 800);
  assert.deepEqual(o.items, [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 1, unit_cents: 6900 }]);
  assert.equal(o.ship.zip, '02134'); assert.equal(o.name, 'Ada Lovelace');
  assert.equal(eventToOrder({ type: 'checkout.session.completed', data: { object: { payment_status: 'unpaid' } } }).kind, 'ignore');
  assert.equal(eventToOrder({ type: 'charge.refunded', data: { object: { payment_intent: 'pi_1', amount_refunded: 500 } } }).refunded_cents, 500);
  assert.equal(eventToOrder({ type: 'payout.paid', data: { object: { id: 'po_1', amount: 12345, arrival_date: 1760000000 } } }).amount_cents, 12345);
  const sub = eventToOrder({ type: 'customer.subscription.deleted', data: { object: { id: 'sub_1', status: 'canceled', items: { data: [{ price: { nickname: 'Support', unit_amount: 9900, recurring: { interval: 'month' } } }] } } } });
  assert.equal(sub.kind, 'subscription'); assert.ok(sub.deleted); assert.equal(sub.plan, 'Support');
});

test('CSV: quoted commas, CRLF, BOM, Stripe export headers', () => {
  const csv = '﻿id,Amount,Status,Customer Email,Shipping Name,Shipping Address Line1,Shipping Address City,Shipping Address State,Shipping Address Postal Code,Shipping Address Country,Description,Quantity,Created date (UTC)\r\npi_1,"77.00",Paid,ada@example.test,"Lovelace, Ada","1 Analytical Way, Apt 2",Boston,MA,02134,US,Canary WAP Kit,1,2026-10-19 12:00\r\n';
  const rows = parseCsv(csv);
  assert.equal(rows.length, 1);
  const map = detectColumns(rows[0]);
  assert.equal(map.external_id, 'id'); assert.equal(map.email, 'Customer Email'); assert.equal(map.line1, 'Shipping Address Line1');
  const o = csvRowToOrder(rows[0], map);
  assert.equal(o.external_id, 'pi_1'); assert.equal(o.total_cents, 7700); assert.equal(o.name, 'Lovelace, Ada'); assert.equal(o.ship.line1, '1 Analytical Way, Apt 2');
  assert.equal(o.items[0].sku, 'Canary WAP Kit'); assert.equal(o.status_hint, 'Paid');
});
