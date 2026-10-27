// The books. Double-entry in cents; every order, refund, payout and expense
// is a balanced transaction, so a trial balance that does not sum to zero
// is a bug the tests catch. Reports: P&L by month, sales tax by state, the
// expense categories a Schedule C asks for. This is bookkeeping, not tax
// advice — the year-end export goes to a human accountant.
import { nowIso, today } from '../db.mjs';
import { token, csvLine } from '../http.mjs';

export const ACCOUNTS = {
  'assets:stripe': 'Stripe balance (cleared, not yet paid out)',
  'assets:bank': 'Business checking',
  'assets:inventory': 'Inventory at cost',
  'assets:receivable': 'Invoices outstanding',
  'liabilities:sales_tax': 'Sales tax collected, owed to states',
  'income:sales': 'Hardware and parts sales',
  'income:shipping': 'Shipping charged',
  'income:services': 'Support plans, attestation, services',
  'income:refunds': 'Refunds (contra-income)',
  'income:discounts': 'Discounts given (contra-income)',
  'expenses:cogs': 'Cost of goods sold',
  'expenses:fees': 'Payment processing fees',
  'expenses:postage': 'Postage and labels',
  'expenses:supplies': 'Parts, filament, packaging bought',
  'expenses:software': 'Software and AI subscriptions',
  'expenses:insurance': 'Business and product liability insurance',
  'expenses:compliance': 'Testing labs, certifications, legal',
  'expenses:warranty': 'Warranty reships and claims',
  'expenses:other': 'Everything else',
  'equity:owner': "Owner's contributions and draws",
};
export const EXPENSE_ACCOUNTS = Object.keys(ACCOUNTS).filter(a => a.startsWith('expenses:'));

function post(db, { date = today(), legs, memo = '', ref = null, source = 'manual' }) {
  const d = legs.reduce((s, l) => s + (l.debit || 0), 0), c = legs.reduce((s, l) => s + (l.credit || 0), 0);
  if (d !== c) throw new Error(`unbalanced transaction: debit ${d} credit ${c} (${memo})`);
  const txn = token(8);
  for (const l of legs) { if (!ACCOUNTS[l.account]) throw new Error(`unknown account ${l.account}`); if (!(l.debit || l.credit)) continue;
    db.run('INSERT INTO ledger (txn, date, account, debit_cents, credit_cents, memo, ref, source, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)', txn, date, l.account, l.debit || 0, l.credit || 0, memo, ref, source, nowIso()); }
  return txn;
}
export const postTransaction = post;

export function postSale(db, order) {
  if (!order.total_cents && !order.items?.some(i => i.cogs_cents)) return null;
  const date = (order.paid_at || nowIso()).slice(0, 10);
  const cogs = (order.items || []).reduce((s, i) => s + i.cogs_cents * i.qty, 0);
  const legs = [];
  if (order.total_cents) legs.push({ account: order.source === 'invoice' ? 'assets:receivable' : 'assets:stripe', debit: order.total_cents },
    { account: 'income:sales', credit: order.subtotal_cents }, { account: 'income:shipping', credit: order.shipping_cents }, { account: 'liabilities:sales_tax', credit: order.tax_cents });
  if (order.total_cents && order.discount_cents) legs.push({ account: 'income:discounts', debit: order.discount_cents });
  if (cogs) legs.push({ account: 'expenses:cogs', debit: cogs }, { account: 'assets:inventory', credit: cogs });
  return post(db, { date, legs, memo: `sale ${order.number}`, ref: `order:${order.number}`, source: order.source });
}
// The tax share of a refund (the order's tax rate applied to the amount going back) is a reversal of
// the tax collected, not a sales refund: it comes off the liability, so the sales-tax table and the
// ledger agree on what was actually collected. The rest is income:refunds.
export const refundTaxShare = (order, cents) => order.total_cents > 0 && order.tax_cents > 0 ? Math.min(order.tax_cents, Math.round(cents * order.tax_cents / order.total_cents)) : 0;
export const postRefund = (db, order, cents, reason = '', date = today()) => { const tax = refundTaxShare(order, cents); return post(db, { date, legs: [{ account: 'income:refunds', debit: cents - tax }, { account: 'liabilities:sales_tax', debit: tax }, { account: 'assets:stripe', credit: cents }], memo: `refund ${order.number} ${reason}`.trim(), ref: `order:${order.number}`, source: 'refund' }); };

// A payout moves money Stripe → bank; the fee is what Stripe kept.
export function postPayout(db, { id, date = today(), amount_cents, fee_cents = 0 }) {
  if (!Number.isInteger(amount_cents) || amount_cents < 0 || !Number.isInteger(fee_cents) || fee_cents < 0) throw new Error('a payout and its fee are whole numbers of cents, 0 or more');
  if (id && db.get('SELECT id FROM ledger WHERE ref = ?', `payout:${id}`)) return null;
  return post(db, { date, legs: [{ account: 'assets:bank', debit: amount_cents }, { account: 'expenses:fees', debit: fee_cents }, { account: 'assets:stripe', credit: amount_cents + fee_cents }], memo: `payout ${id || ''}`.trim(), ref: id ? `payout:${id}` : null, source: 'stripe' });
}
export function postExpense(db, { date = today(), account, cents, memo = '', paidFrom = 'assets:bank', ref = null }) {
  if (!String(account || '').startsWith('expenses:') && account !== 'assets:inventory') throw new Error('expense account required');
  if (!Number.isInteger(cents) || cents <= 0) throw new Error('an expense is a whole number of cents above zero');
  if (!ACCOUNTS[paidFrom]) throw new Error(`unknown account ${paidFrom}`);
  return post(db, { date, legs: [{ account, debit: cents }, { account: paidFrom, credit: cents }], memo, ref, source: 'manual' });
}
export const postInvoicePaid = (db, inv, date = today()) => post(db, { date, legs: [{ account: 'assets:bank', debit: inv.total_cents }, { account: 'assets:receivable', credit: inv.total_cents }], memo: `invoice ${inv.number} paid`, ref: `invoice:${inv.number}`, source: 'invoice' });

export function trialBalance(db, { from = '0000-00-00', to = '9999-99-99' } = {}) {
  const rows = db.all('SELECT account, SUM(debit_cents) AS d, SUM(credit_cents) AS c FROM ledger WHERE date BETWEEN ? AND ? GROUP BY account ORDER BY account', from, to);
  const total = rows.reduce((s, r) => s + r.d - r.c, 0);
  return { rows: rows.map(r => ({ account: r.account, label: ACCOUNTS[r.account], balance_cents: r.account.startsWith('assets') || r.account.startsWith('expenses') || r.account === 'income:refunds' || r.account === 'income:discounts' ? r.d - r.c : r.c - r.d })), balanced: total === 0 };
}

export function profitAndLoss(db, { from, to }) {
  const rows = db.all('SELECT account, SUM(credit_cents) - SUM(debit_cents) AS net FROM ledger WHERE date BETWEEN ? AND ? AND (account LIKE \'income:%\' OR account LIKE \'expenses:%\') GROUP BY account', from, to);
  const income = rows.filter(r => r.account.startsWith('income:')).reduce((s, r) => s + r.net, 0);
  const expenses = rows.filter(r => r.account.startsWith('expenses:')).reduce((s, r) => s - r.net, 0);
  return { from, to, income_cents: income, expenses_cents: expenses, net_cents: income - expenses, lines: rows.map(r => ({ account: r.account, label: ACCOUNTS[r.account], cents: r.account.startsWith('income:') ? r.net : -r.net })) };
}
export function monthly(db, months = 12) {
  const out = [];
  const d = new Date(); d.setUTCDate(1);
  for (let i = 0; i < months; i++) {
    const from = d.toISOString().slice(0, 10); const end = new Date(d); end.setUTCMonth(end.getUTCMonth() + 1); end.setUTCDate(0);
    out.unshift({ month: from.slice(0, 7), ...profitAndLoss(db, { from, to: end.toISOString().slice(0, 10) }) });
    d.setUTCMonth(d.getUTCMonth() - 1);
  }
  return out;
}
// Net of refunds: a refunded order's tax went back with the money (postRefund reverses the same share), so the
// table shows what was kept, which is what is owed. A canceled order was refunded in full, so it drops out.
export const salesTaxByState = (db, { from, to }) => db.all(`SELECT ship_state AS state, COUNT(*) AS orders,
  SUM(subtotal_cents - CAST(ROUND(subtotal_cents * 1.0 * refunded_cents / total_cents) AS INTEGER)) AS taxable_cents,
  SUM(tax_cents - MIN(tax_cents, CAST(ROUND(tax_cents * 1.0 * refunded_cents / total_cents) AS INTEGER))) AS tax_cents
  FROM orders WHERE paid_at >= ? AND paid_at < ? AND status NOT IN ('canceled') AND total_cents > 0 GROUP BY ship_state ORDER BY tax_cents DESC`, from, to + 'T23:59:59Z');

export function ledgerCsv(db, { from = '0000-00-00', to = '9999-99-99' } = {}) {
  const rows = db.all('SELECT * FROM ledger WHERE date BETWEEN ? AND ? ORDER BY date, txn, id', from, to);
  return ['date,txn,account,debit,credit,memo,ref,source', ...rows.map(r => csvLine([r.date, r.txn, r.account, (r.debit_cents / 100).toFixed(2), (r.credit_cents / 100).toFixed(2), r.memo, r.ref, r.source]))].join('\n') + '\n';
}
export const unreconciledStripeCents = (db) => { const r = db.get('SELECT SUM(debit_cents) - SUM(credit_cents) AS c FROM ledger WHERE account = \'assets:stripe\''); return r?.c || 0; };
