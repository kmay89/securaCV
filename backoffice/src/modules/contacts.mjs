// Contacts, consent and the waitlist. Replaces: the CRM and the audience half
// of a newsletter tool. Rules: double opt-in before any marketing mail; a
// one-click unsubscribe link in every campaign; a contact exists because
// they bought, wrote, or asked — never because we scraped.
import { nowIso } from '../db.mjs';
import { token } from '../http.mjs';

export const normEmail = (s) => String(s || '').trim().toLowerCase();
export const validEmail = (s) => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(normEmail(s)) && normEmail(s).length <= 254;

export function upsertContact(db, { email, name = null, org = null, kind = 'person', source = 'manual' }) {
  const em = normEmail(email);
  if (!validEmail(em)) throw new Error(`invalid email: ${email}`);
  const existing = db.get('SELECT * FROM contacts WHERE email = ?', em);
  if (existing) {
    if (name && !existing.name) db.run('UPDATE contacts SET name = ? WHERE id = ?', name, existing.id);
    if (org && !existing.org) db.run('UPDATE contacts SET org = ? WHERE id = ?', org, existing.id);
    return db.get('SELECT * FROM contacts WHERE id = ?', existing.id);
  }
  const r = db.run('INSERT INTO contacts (email, name, org, kind, created_at, source, unsubscribe_token) VALUES (?, ?, ?, ?, ?, ?, ?)',
    em, name, org, kind, nowIso(), source, token(18));
  return db.get('SELECT * FROM contacts WHERE id = ?', r.lastInsertRowid);
}

export const contactById = (db, id) => db.get('SELECT * FROM contacts WHERE id = ?', id);
export const contactByEmail = (db, email) => db.get('SELECT * FROM contacts WHERE email = ?', normEmail(email));

// Step 1 of double opt-in: record the ask, hand back the token the confirm mail carries.
export function requestConsent(db, contactId) {
  const t = token(18);
  db.run('UPDATE contacts SET consent_token = ? WHERE id = ?', t, contactId);
  return t;
}
// Step 2: the link in the mail was clicked.
export function confirmConsent(db, t) {
  const c = t ? db.get('SELECT * FROM contacts WHERE consent_token = ?', t) : null;
  if (!c) return null;
  db.run('UPDATE contacts SET consent_marketing = 1, consent_at = ?, consent_token = NULL, unsubscribed_at = NULL WHERE id = ?', nowIso(), c.id);
  db.audit('public', 'contact.consent', 'contact', c.id);
  return contactById(db, c.id);
}
export function unsubscribe(db, t) {
  const c = t ? db.get('SELECT * FROM contacts WHERE unsubscribe_token = ?', t) : null;
  if (!c) return null;
  db.run('UPDATE contacts SET consent_marketing = 0, unsubscribed_at = ? WHERE id = ?', nowIso(), c.id);
  db.audit('public', 'contact.unsubscribe', 'contact', c.id);
  return contactById(db, c.id);
}
export const canMarket = (c) => !!c && c.consent_marketing === 1 && !c.unsubscribed_at;

export function joinWaitlist(db, contactId, sku, qty = 1) {
  db.run('INSERT INTO waitlist (contact_id, sku, qty, created_at) VALUES (?, ?, ?, ?) ON CONFLICT(contact_id, sku) DO UPDATE SET qty = excluded.qty',
    contactId, sku, Math.max(1, Number(qty) || 1), nowIso());
  return db.get('SELECT * FROM waitlist WHERE contact_id = ? AND sku = ?', contactId, sku);
}
export const waitlistFor = (db, sku) => db.all('SELECT w.*, c.email, c.name, c.consent_marketing, c.unsubscribed_at FROM waitlist w JOIN contacts c ON c.id = w.contact_id WHERE w.sku = ? AND w.notified_at IS NULL ORDER BY w.created_at', sku);
export const waitlistSummary = (db) => db.all('SELECT sku, COUNT(*) AS people, SUM(qty) AS units, SUM(CASE WHEN notified_at IS NULL THEN 1 ELSE 0 END) AS waiting FROM waitlist GROUP BY sku ORDER BY waiting DESC');

export function searchContacts(db, q = '', limit = 200) {
  const like = `%${q}%`;
  return db.all('SELECT * FROM contacts WHERE (? = \'\' OR email LIKE ? OR name LIKE ? OR org LIKE ?) ORDER BY created_at DESC LIMIT ?', q, like, like, like, limit);
}

export function segment(db, seg = 'all') {
  // Only contacts who may be marketed to, by segment. Transactional mail never uses this.
  const base = 'SELECT c.* FROM contacts c WHERE c.consent_marketing = 1 AND c.unsubscribed_at IS NULL';
  if (seg === 'all') return db.all(base + ' ORDER BY c.id');
  const [k, v] = seg.split(':');
  if (k === 'waitlist') return db.all(base + ' AND c.id IN (SELECT contact_id FROM waitlist WHERE sku = ?) ORDER BY c.id', v);
  if (k === 'kind') return db.all(base + ' AND c.kind = ? ORDER BY c.id', v);
  if (k === 'customers') return db.all(base + ' AND c.id IN (SELECT contact_id FROM orders WHERE status NOT IN (\'canceled\',\'refunded\')) ORDER BY c.id');
  throw new Error(`unknown segment: ${seg}`);
}
