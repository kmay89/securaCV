// The checkout funnel's last step, and the one follow-up an abandoned cart gets.
//
// Every hosted Checkout Session the store opens is a row here; Stripe's webhooks move it on: paid
// (the order it became), expired (with the recovery link Stripe minted, which reopens the same cart
// for 30 days), recovered (an expired one that was reopened and paid). So "checkout conversion" is a
// number — opened against paid — with no pixel, no cookie and nothing the site records about a visitor.
//
// The follow-up: when an expired session carries an email (the shopper typed it on Stripe's page) and
// no order from that address has arrived since, one mail goes out after CHECKOUT_RECOVERY_HOURS with
// the link, once per session, never again. A transactional mail about a purchase they started; the
// mail says so and says it is the only one.
import { enqueue, fill } from './mail.mjs';
import { nowIso } from '../db.mjs';

export function recordSession(db, { id, amount_cents = 0, lines = '', state = null, email = null, created_at = nowIso() }) {
  db.run('INSERT OR IGNORE INTO checkout_sessions (id, created_at, status, amount_cents, lines, state, email) VALUES (?, ?, \'open\', ?, ?, ?, ?)', id, created_at, amount_cents, lines, state, email || null);
}
export function markSessionPaid(db, { id, order_number, recovered_from = null, at = nowIso() }) {
  if (!id) return;
  db.run("INSERT INTO checkout_sessions (id, created_at, status, order_number, paid_at, recovered_from) VALUES (?, ?, 'paid', ?, ?, ?) ON CONFLICT(id) DO UPDATE SET status = 'paid', order_number = excluded.order_number, paid_at = excluded.paid_at, recovered_from = COALESCE(excluded.recovered_from, checkout_sessions.recovered_from)", id, at, order_number, at, recovered_from);
  if (recovered_from) db.run("UPDATE checkout_sessions SET status = 'recovered', order_number = ? WHERE id = ? AND status != 'paid'", order_number, recovered_from);
}
export function markSessionExpired(db, { id, email = null, recovery_url = null, recovery_expires_at = null, amount_cents = null, at = nowIso() }) {
  if (!id) return;
  db.run("INSERT INTO checkout_sessions (id, created_at, status, amount_cents, email, expired_at, recovery_url, recovery_expires_at) VALUES (?, ?, 'expired', ?, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET status = CASE WHEN checkout_sessions.status IN ('paid','recovered') THEN checkout_sessions.status ELSE 'expired' END, email = COALESCE(excluded.email, checkout_sessions.email), expired_at = excluded.expired_at, recovery_url = COALESCE(excluded.recovery_url, checkout_sessions.recovery_url), recovery_expires_at = COALESCE(excluded.recovery_expires_at, checkout_sessions.recovery_expires_at), amount_cents = CASE WHEN excluded.amount_cents > 0 THEN excluded.amount_cents ELSE checkout_sessions.amount_cents END",
    id, at, amount_cents || 0, email || null, at, recovery_url, recovery_expires_at);
}

export const RECOVERY_TPL = `Hi there,

You started an order with us and the checkout page timed out before it finished — it happens. Your cart is kept exactly as it was:

{{lines}}

Pick it up where you left off (the link works for 30 days): {{url}}

{{ships}}

This is the only mail we send about it; if you changed your mind, there is nothing to do. Questions: reply to this mail and a person answers.

— {{company}}
{{support}}
`;

// Expired sessions worth one follow-up: an email, a live recovery link, the waiting time passed, no order
// from that address since, and no mail yet.
export function recoverableSessions(db, cfg, { now = Date.now() } = {}) {
  const waitMs = (cfg.recoveryHours ?? 1) * 3600000; const cutoff = new Date(now - waitMs).toISOString(); const t = new Date(now).toISOString();
  return db.all("SELECT * FROM checkout_sessions WHERE status = 'expired' AND email IS NOT NULL AND email != '' AND recovery_url IS NOT NULL AND recovery_mailed_at IS NULL AND expired_at <= ? AND (recovery_expires_at IS NULL OR recovery_expires_at > ?) AND expired_at >= ? ORDER BY expired_at", cutoff, t, new Date(now - 7 * 86400000).toISOString())
    .filter(s => !db.get('SELECT o.id FROM orders o JOIN contacts c ON c.id = o.contact_id WHERE lower(c.email) = lower(?) AND o.paid_at >= ?', s.email, s.expired_at))
    .filter(s => !db.get("SELECT id FROM contacts WHERE lower(email) = lower(?) AND (unsubscribed_at IS NOT NULL OR bounced_at IS NOT NULL OR forgotten_at IS NOT NULL)", s.email));
}
export function sendRecovery(db, cfg, s, { now = Date.now() } = {}) {
  const ref = `recovery:${s.id}`; if (db.get('SELECT id FROM mail_outbox WHERE ref = ?', ref)) return null;
  const m = enqueue(db, { to: s.email, subject: `Your ${cfg.product || 'SecuraCV'} cart is still here`, kind: 'transactional', ref,
    text: fill(RECOVERY_TPL, { lines: s.lines ? s.lines.split(', ').map(l => `  ${l}`).join('\n') : '  (your cart)', url: s.recovery_url, ships: 'Orders go out in weekly batches; the checkout page shows the ship day for your ZIP before you pay.', company: cfg.company || 'Errer Labs', support: cfg.supportAddr || '' }) });
  db.run('UPDATE checkout_sessions SET recovery_mailed_at = ? WHERE id = ?', new Date(now).toISOString(), s.id);
  db.audit('system', 'checkout.recovery', 'checkout', s.id, { to: s.email, amount_cents: s.amount_cents });
  return m;
}
export function runRecovery(db, cfg, { now = Date.now() } = {}) {
  const out = { mailed: 0, waiting: 0 };
  for (const s of recoverableSessions(db, cfg, { now })) { if (sendRecovery(db, cfg, s, { now })) out.mailed++; }
  out.waiting = db.get("SELECT COUNT(*) AS n FROM checkout_sessions WHERE status = 'expired' AND email IS NOT NULL AND recovery_mailed_at IS NULL AND expired_at > ?", new Date(now - (cfg.recoveryHours ?? 1) * 3600000).toISOString()).n;
  return out;
}

// The funnel's last step over a period: opened, paid (and the share), abandoned, how many of those were
// written to, how many came back. A session still open inside its half hour is neither yet.
export function checkoutStats(db, { days = 30, now = Date.now() } = {}) {
  const since = new Date(now - days * 86400000).toISOString();
  const r = db.get(`SELECT COUNT(*) AS opened, SUM(status = 'paid') AS paid, SUM(status IN ('expired','recovered')) AS abandoned, SUM(status = 'recovered') AS recovered, SUM(recovery_mailed_at IS NOT NULL) AS mailed,
    SUM(CASE WHEN status = 'paid' THEN amount_cents ELSE 0 END) AS paid_cents, SUM(CASE WHEN status = 'expired' THEN amount_cents ELSE 0 END) AS abandoned_cents FROM checkout_sessions WHERE created_at >= ?`, since);
  const decided = (r.paid || 0) + (r.abandoned || 0);
  return { days, opened: r.opened || 0, paid: r.paid || 0, abandoned: r.abandoned || 0, recovered: r.recovered || 0, mailed: r.mailed || 0, paid_cents: r.paid_cents || 0, abandoned_cents: r.abandoned_cents || 0, conversion: decided ? (r.paid || 0) / decided : null };
}
