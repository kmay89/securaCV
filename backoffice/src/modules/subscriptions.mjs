// Support plans / SLAs (LICENSING.md §2c): month-to-month, cancel anytime,
// and a lapse never disables anything. Stripe Billing is the source; we
// mirror state so renewals, lapses and the services revenue line are visible
// without a dashboard login.
import { nowIso } from '../db.mjs';
import { upsertContact } from './contacts.mjs';
import { postTransaction } from './ledger.mjs';

export function upsertSubscription(db, s) {
  const contact = s.email ? upsertContact(db, { email: s.email, source: 'subscription' }) : null;
  const existing = s.external_id ? db.get('SELECT * FROM subscriptions WHERE external_id = ?', s.external_id) : null;
  if (existing) {
    db.run('UPDATE subscriptions SET status = ?, plan = ?, amount_cents = ?, interval = ?, current_period_end = ?, canceled_at = CASE WHEN ? = \'canceled\' THEN COALESCE(canceled_at, ?) ELSE NULL END, contact_id = COALESCE(?, contact_id) WHERE id = ?',
      s.deleted ? 'canceled' : (s.status || existing.status), s.plan || existing.plan, s.amount_cents ?? existing.amount_cents, s.interval || existing.interval, s.current_period_end || existing.current_period_end, s.deleted ? 'canceled' : (s.status || ''), nowIso(), contact?.id ?? null, existing.id);
    return db.get('SELECT * FROM subscriptions WHERE id = ?', existing.id);
  }
  const r = db.run('INSERT INTO subscriptions (external_id, contact_id, plan, status, amount_cents, interval, current_period_end, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
    s.external_id || null, contact?.id ?? null, s.plan || 'support', s.deleted ? 'canceled' : (s.status || 'active'), s.amount_cents || 0, s.interval || 'month', s.current_period_end || null, nowIso());
  return db.get('SELECT * FROM subscriptions WHERE id = ?', r.lastInsertRowid);
}
// A paid invoice for a subscription is services revenue.
// Keyed by the Stripe invoice id, so a redelivered invoice.paid posts nothing twice.
export function postSubscriptionPayment(db, { external_id, invoice_id, amount_cents, date }) {
  const ref = `subscription:${external_id}:${invoice_id || date}`;
  if (db.get('SELECT id FROM ledger WHERE ref = ?', ref)) return null;
  return postTransaction(db, { date, legs: [{ account: 'assets:stripe', debit: amount_cents }, { account: 'income:services', credit: amount_cents }], memo: `subscription ${external_id}`, ref, source: 'stripe' });
}
export const listSubscriptions = (db) => db.all('SELECT s.*, c.email, c.org FROM subscriptions s LEFT JOIN contacts c ON c.id = s.contact_id ORDER BY s.status = \'active\' DESC, s.current_period_end');
export const mrrCents = (db) => db.get('SELECT COALESCE(SUM(CASE WHEN interval = \'year\' THEN amount_cents / 12 ELSE amount_cents END),0) AS c FROM subscriptions WHERE status = \'active\'').c;
