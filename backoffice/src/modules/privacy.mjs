// The right to be forgotten, and the right to a copy. A company that keeps
// no footage should also be able to forget a person in one command: the
// contact becomes a numbered placeholder, every address and message body
// is blanked, attachments and waitlist rows go, mail to them is blanked —
// while order numbers, amounts and dates stay, because the books must still
// add up and the state is still a state.
import { nowIso } from '../db.mjs';
import { token } from '../http.mjs';
import { contactByEmail, contactById } from './contacts.mjs';

export function forgetContact(db, idOrEmail, { actor = 'web' } = {}) {
  const c = /^\d+$/.test(String(idOrEmail)) ? contactById(db, Number(idOrEmail)) : contactByEmail(db, idOrEmail);
  if (!c) throw new Error('no such contact'); if (c.forgotten_at) return { already: true, id: c.id };
  return db.tx(() => {
    const now = nowIso(); const ghost = `forgotten-${c.id}@forgotten.invalid`;
    const out = { id: c.id, orders: 0, tickets: 0, messages: 0, attachments: 0, mails: 0, waitlist: 0 };
    db.run('UPDATE contacts SET email = ?, name = ?, org = NULL, notes = NULL, tags = \'\', consent_marketing = 0, consent_at = NULL, consent_token = NULL, unsubscribe_token = ?, unsubscribed_at = COALESCE(unsubscribed_at, ?), bounced_at = NULL, forgotten_at = ? WHERE id = ?', ghost, 'Forgotten customer', token(18), now, now, c.id);
    out.orders = db.run('UPDATE orders SET ship_name = \'Forgotten\', ship_line1 = \'(forgotten)\', ship_line2 = \'\', ship_city = \'\', ship_zip = \'\', notes = NULL, tags = \'\' WHERE contact_id = ?', c.id).changes;
    db.run('UPDATE order_events SET text = \'(forgotten at the customer\'\'s request)\' WHERE order_id IN (SELECT id FROM orders WHERE contact_id = ?) AND kind IN (\'note\', \'address\', \'mail\')', c.id);
    const tickets = db.all('SELECT id FROM tickets WHERE contact_id = ?', c.id).map(t => t.id); out.tickets = tickets.length;
    for (const t of tickets) { out.attachments += db.run('DELETE FROM attachments WHERE ticket_message_id IN (SELECT id FROM ticket_messages WHERE ticket_id = ?)', t).changes; out.messages += db.run('UPDATE ticket_messages SET body = \'(forgotten at the customer\'\'s request)\', from_addr = NULL, to_addr = NULL WHERE ticket_id = ?', t).changes; db.run('UPDATE tickets SET subject = \'(forgotten)\', status = \'closed\', closed_at = COALESCE(closed_at, ?) WHERE id = ?', now, t); }
    out.mails = db.run('UPDATE mail_outbox SET to_addr = ?, body_text = \'(forgotten)\', subject = \'(forgotten)\' WHERE to_addr = ? OR contact_id = ?', ghost, c.email, c.id).changes;
    out.waitlist = db.run('DELETE FROM waitlist WHERE contact_id = ?', c.id).changes;
    db.run('DELETE FROM automation_runs WHERE contact_id = ? AND sent_at IS NULL', c.id);
    db.audit(actor, 'contact.forget', 'contact', c.id, out);
    return out;
  });
}
// Everything we hold about a person, as one JSON document, for a data-access request.
export function exportContact(db, idOrEmail) {
  const c = /^\d+$/.test(String(idOrEmail)) ? contactById(db, Number(idOrEmail)) : contactByEmail(db, idOrEmail);
  if (!c) throw new Error('no such contact');
  const orders = db.all('SELECT * FROM orders WHERE contact_id = ?', c.id).map(o => ({ ...o, items: db.all('SELECT sku, name, qty, unit_cents FROM order_items WHERE order_id = ?', o.id), events: db.all('SELECT at, kind, text FROM order_events WHERE order_id = ? ORDER BY id', o.id) }));
  const tickets = db.all('SELECT * FROM tickets WHERE contact_id = ?', c.id).map(t => ({ ...t, messages: db.all('SELECT direction, created_at, body FROM ticket_messages WHERE ticket_id = ? ORDER BY id', t.id) }));
  return { exported_at: nowIso(), contact: { ...c, consent_token: undefined, unsubscribe_token: undefined }, orders, tickets, waitlist: db.all('SELECT sku, qty, created_at, notified_at FROM waitlist WHERE contact_id = ?', c.id), mail: db.all('SELECT created_at, subject, kind, status FROM mail_outbox WHERE contact_id = ? OR to_addr = ? ORDER BY id', c.id, c.email), consent_history: db.all('SELECT at, action FROM audit_log WHERE entity = \'contact\' AND entity_id = ? ORDER BY id', String(c.id)) };
}
