import { test } from 'node:test';
import assert from 'node:assert/strict';
import { openClaim, reship, resolveClaim, ANSWERS, claimById } from '../src/modules/warranty.mjs';
import { recordOrder, orderById } from '../src/modules/orders.mjs';
import { createInvoice, issueInvoice, markPaid, voidInvoice, invoiceToken, overdue } from '../src/modules/invoices.mjs';
import { trialBalance, postExpense, postPayout, postTransaction, monthly, unreconciledStripeCents } from '../src/modules/ledger.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

test('a warranty claim has a defined answer, opens a thread, reships at $0 and resolves with a mail', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog();
  const { order } = recordOrder(db, sampleOrder(), { catalog: cat });
  assert.throws(() => openClaim(db, { orderNumber: order.number, category: 'nope', cfg }), /unknown warranty category/);
  const claim = openClaim(db, { orderNumber: order.number, category: 'dead_board', description: 'no LED', cfg });
  assert.equal(claim.number, 'WC-000001'); assert.equal(claim.resolution, ANSWERS.dead_board.action); assert.equal(claim.email, 'ada@example.test'); assert.ok(claim.ticket_id);
  const declined = openClaim(db, { orderNumber: order.number, category: 'not_covered', cfg });
  assert.equal(declined.status, 'declined');
  const shipped = reship(db, claim.id, [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 1 }], { recordOrderFn: recordOrder, catalog: cat });
  const re = orderById(db, shipped.reship_order_id);
  assert.equal(re.total_cents, 0); assert.equal(re.ship_zip, '02134'); assert.ok(re.notes.includes('WC-000001')); assert.equal(shipped.status, 'shipping');
  resolveClaim(db, claim.id, { costCents: 1200, note: 'New board shipped.', cfg });
  const done = claimById(db, claim.id);
  assert.equal(done.status, 'resolved'); assert.equal(done.cost_cents, 1200);
  assert.equal(db.get('SELECT status FROM tickets WHERE id = ?', claim.ticket_id).status, 'closed');
  assert.ok(db.get('SELECT * FROM mail_outbox WHERE ref = ?', 'warranty:WC-000001').body_text.includes('keep or recycle'));
  assert.ok(trialBalance(db).balanced, 'a $0 reship with COGS still balances');
});

test('invoices: draft → issued (receivable) → paid (bank); void reverses; overdue is derived; tokens gate the public link', () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' });
  const inv = createInvoice(db, { email: 'buyer@school.test', org: 'Ada High', po_number: 'PO-77', items: [{ sku: 'wap-kit', description: 'Canary WAP Kit', qty: 20, unit_cents: 6200 }], shipping_cents: 2500 });
  assert.equal(inv.number, `INV-${new Date().getUTCFullYear()}-0001`); assert.equal(inv.total_cents, 20 * 6200 + 2500); assert.equal(inv.status, 'draft');
  assert.throws(() => markPaid(db, inv.id), /issue it first/);
  const issued = issueInvoice(db, inv.id, cfg);
  assert.equal(issued.status, 'sent'); assert.ok(issued.due_at > issued.issued_at);
  assert.ok(db.get('SELECT * FROM mail_outbox WHERE ref = ?', `invoice:${inv.number}`).body_text.includes(invoiceToken('s', inv.number)));
  assert.equal(trialBalance(db).rows.find(r => r.account === 'assets:receivable').balance_cents, inv.total_cents);
  markPaid(db, inv.id);
  const tb = trialBalance(db); assert.ok(tb.balanced);
  assert.equal(tb.rows.find(r => r.account === 'assets:receivable').balance_cents, 0); assert.equal(tb.rows.find(r => r.account === 'assets:bank').balance_cents, inv.total_cents);
  assert.throws(() => voidInvoice(db, inv.id), /paid invoices/);
  const inv2 = createInvoice(db, { email: 'b@c.test', items: [{ description: 'thing', qty: 1, unit_cents: 100 }] });
  issueInvoice(db, inv2.id, cfg); db.run('UPDATE invoices SET due_at = ? WHERE id = ?', '2000-01-01', inv2.id);
  assert.equal(overdue(db)[0].number, inv2.number);
  voidInvoice(db, inv2.id);
  assert.ok(trialBalance(db).balanced); assert.equal(trialBalance(db).rows.find(r => r.account === 'assets:receivable').balance_cents, 0);
});

test('ledger: expenses, payouts with fees, unbalanced legs refused, monthly P&L', () => {
  const db = testDb();
  recordOrder(db, sampleOrder({ paid_at: new Date().toISOString() }), { catalog: catalog() });
  assert.equal(unreconciledStripeCents(db), 7700);
  postPayout(db, { id: 'po_1', amount_cents: 7447, fee_cents: 253 });
  assert.equal(unreconciledStripeCents(db), 0);
  assert.equal(postPayout(db, { id: 'po_1', amount_cents: 7447, fee_cents: 253 }), null, 'a payout posts once');
  postExpense(db, { account: 'expenses:supplies', cents: 2200, memo: 'filament' });
  assert.throws(() => postExpense(db, { account: 'income:sales', cents: 1 }), /expense account/);
  assert.throws(() => postTransaction(db, { legs: [{ account: 'assets:bank', debit: 5 }, { account: 'income:sales', credit: 4 }] }), /unbalanced/);
  assert.throws(() => postTransaction(db, { legs: [{ account: 'assets:petty', debit: 5 }, { account: 'income:sales', credit: 5 }] }), /unknown account/);
  const m = monthly(db, 2).at(-1);
  assert.equal(m.income_cents, 7700); assert.equal(m.expenses_cents, 900 + 253 + 2200, 'cogs + fee + supplies');
  assert.ok(trialBalance(db).balanced);
});
