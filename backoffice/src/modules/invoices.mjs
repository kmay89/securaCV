// Invoices for the buyers who pay on a purchase order (schools, districts,
// a business with procurement): numbered, dated, net-N terms, a printable
// page, and a paid state that posts to the books. Replaces invoicing SaaS.
import { createHmac } from 'node:crypto';
import { nowIso, today } from '../db.mjs';
import { upsertContact } from './contacts.mjs';
import { enqueue } from './mail.mjs';
import { postInvoicePaid, postTransaction } from './ledger.mjs';

export function createInvoice(db, { email, name = null, org = null, bill_to = '', po_number = '', terms_days = 30, items, shipping_cents = 0, tax_cents = 0, notes = '' }) {
  if (!items?.length) throw new Error('an invoice needs at least one line');
  const whole = (v, what, min = 0) => { const n = v == null || v === '' ? min : Number(v); if (!Number.isInteger(n) || n < min) throw new Error(`${what} must be a whole number${min ? ` of at least ${min}` : ', 0 or more'}`); return n; };
  items = items.map(i => ({ ...i, qty: whole(i.qty, `qty on ${i.sku || i.description || 'a line'}`, 1), unit_cents: whole(i.unit_cents, `unit cents on ${i.sku || i.description || 'a line'}`) }));
  shipping_cents = whole(shipping_cents, 'shipping cents'); tax_cents = whole(tax_cents, 'tax cents'); terms_days = whole(terms_days === '' || terms_days == null ? 30 : terms_days, 'terms (days)');
  const contact = email ? upsertContact(db, { email, name, org, kind: org ? 'business' : 'person', source: 'invoice' }) : null;
  const subtotal = items.reduce((s, i) => s + i.unit_cents * i.qty, 0);
  return db.tx(() => {
    const number = db.nextNumber('INV', { year: true });
    const r = db.run(`INSERT INTO invoices (number, contact_id, po_number, status, terms_days, bill_to, subtotal_cents, shipping_cents, tax_cents, total_cents, notes, created_at)
      VALUES (?, ?, ?, 'draft', ?, ?, ?, ?, ?, ?, ?, ?)`, number, contact?.id ?? null, po_number, terms_days, bill_to || [name, org].filter(Boolean).join('\n'), subtotal, shipping_cents, tax_cents, subtotal + shipping_cents + tax_cents, notes, nowIso());
    for (const i of items) db.run('INSERT INTO invoice_items (invoice_id, sku, description, qty, unit_cents) VALUES (?, ?, ?, ?, ?)', r.lastInsertRowid, i.sku || null, i.description || i.sku, i.qty, i.unit_cents);
    db.audit('web', 'invoice.create', 'invoice', number);
    return invoiceById(db, r.lastInsertRowid);
  });
}
export const invoiceById = (db, id) => { const i = db.get('SELECT i.*, c.email, c.name AS contact_name, c.org FROM invoices i LEFT JOIN contacts c ON c.id = i.contact_id WHERE i.id = ?', id); if (i) i.items = db.all('SELECT * FROM invoice_items WHERE invoice_id = ? ORDER BY id', id); return i; };
export const invoiceByNumber = (db, n) => { const i = db.get('SELECT id FROM invoices WHERE number = ?', n); return i ? invoiceById(db, i.id) : null; };
export const listInvoices = (db) => db.all('SELECT i.*, c.email FROM invoices i LEFT JOIN contacts c ON c.id = i.contact_id ORDER BY i.id DESC');

// The public link is unguessable: HMAC of the number under the app secret.
export function remindInvoice(db, id, cfg, { actor = 'web' } = {}) {
  const inv = invoiceById(db, id); if (!inv) throw new Error('no such invoice'); if (!['sent', 'overdue'].includes(inv.status)) throw new Error(`${inv.number} is ${inv.status}`); if (!inv.email) throw new Error('no email on the invoice');
  const m = enqueue(db, { to: inv.email, subject: `Reminder: invoice ${inv.number} (${inv.po_number ? 'PO ' + inv.po_number + ', ' : ''}due ${inv.due_at})`, kind: 'transactional', ref: `invoice-reminder:${inv.number}`, contactId: inv.contact_id, text: `Hi ${inv.contact_name || 'there'},\n\nA gentle reminder that invoice ${inv.number} for $${(inv.total_cents / 100).toFixed(2)} was due ${inv.due_at}.\n\nThe invoice, printable: ${invoiceUrl(cfg, inv)}\n\nIf it is already on its way, thank you — reply and we will note it. If something on it is wrong, reply and we will fix it.\n\n— ${cfg.company || 'Errer Labs'}\n` });
  db.audit(actor, 'invoice.remind', 'invoice', inv.number); return m;
}
export const invoiceToken = (secret, number) => createHmac('sha256', String(secret)).update(String(number)).digest('base64url').slice(0, 24);
export const invoiceUrl = (cfg, inv) => `${cfg.baseUrl}/i/${inv.number}/${invoiceToken(cfg.secret, inv.number)}`;

export function issueInvoice(db, id, cfg, { actor = 'web' } = {}) {
  const inv = invoiceById(db, id); if (!inv) throw new Error('no such invoice');
  if (inv.status !== 'draft') throw new Error('already issued');
  const issued = today(); const due = new Date(Date.now() + inv.terms_days * 86400000).toISOString().slice(0, 10);
  db.run('UPDATE invoices SET status = \'sent\', issued_at = ?, due_at = ? WHERE id = ?', issued, due, id);
  postTransaction(db, { date: issued, legs: [{ account: 'assets:receivable', debit: inv.total_cents }, { account: 'income:sales', credit: inv.subtotal_cents }, { account: 'income:shipping', credit: inv.shipping_cents }, { account: 'liabilities:sales_tax', credit: inv.tax_cents }], memo: `invoice ${inv.number} issued`, ref: `invoice:${inv.number}`, source: 'invoice' });
  if (inv.email) enqueue(db, { to: inv.email, subject: `Invoice ${inv.number} from ${cfg.company || 'Errer Labs'}${inv.po_number ? ` (PO ${inv.po_number})` : ''}`, kind: 'transactional', ref: `invoice:${inv.number}`,
    text: `Invoice ${inv.number} — ${(inv.total_cents / 100).toFixed(2)} ${'USD'}, due ${due} (net ${inv.terms_days}).\n\nView and print: ${invoiceUrl(cfg, inv)}\n\nPay by ACH or check to the details on the invoice. Reply to this mail with any question.\n\n— ${cfg.company || 'Errer Labs'}\n` });
  db.audit(actor, 'invoice.issue', 'invoice', inv.number);
  return invoiceById(db, id);
}
export function markPaid(db, id, { date = today(), actor = 'web' } = {}) {
  const inv = invoiceById(db, id); if (!inv) throw new Error('no such invoice');
  if (inv.status === 'paid') return inv;
  if (inv.status === 'draft') throw new Error('issue it first');
  db.run('UPDATE invoices SET status = \'paid\', paid_at = ? WHERE id = ?', date, id);
  postInvoicePaid(db, inv, date);
  db.audit(actor, 'invoice.paid', 'invoice', inv.number);
  return invoiceById(db, id);
}
export function voidInvoice(db, id, { actor = 'web' } = {}) {
  const inv = invoiceById(db, id); if (!inv) throw new Error('no such invoice');
  if (inv.status === 'paid') throw new Error('paid invoices cannot be voided — refund instead');
  if (inv.status === 'sent' || inv.status === 'overdue') postTransaction(db, { legs: [{ account: 'income:sales', debit: inv.subtotal_cents }, { account: 'income:shipping', debit: inv.shipping_cents }, { account: 'liabilities:sales_tax', debit: inv.tax_cents }, { account: 'assets:receivable', credit: inv.total_cents }], memo: `invoice ${inv.number} voided`, ref: `invoice:${inv.number}`, source: 'invoice' });
  db.run('UPDATE invoices SET status = \'void\' WHERE id = ?', id);
  db.audit(actor, 'invoice.void', 'invoice', inv.number);
  return invoiceById(db, id);
}
export const overdue = (db) => { const t = today(); db.run('UPDATE invoices SET status = \'overdue\' WHERE status = \'sent\' AND due_at < ?', t); return db.all('SELECT i.*, c.email FROM invoices i LEFT JOIN contacts c ON c.id = i.contact_id WHERE i.status = \'overdue\''); };
export const receivableCents = (db) => db.get('SELECT COALESCE(SUM(total_cents),0) AS c FROM invoices WHERE status IN (\'sent\',\'overdue\')').c;
