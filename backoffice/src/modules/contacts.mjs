// Contacts, consent and the waitlist. Replaces: the CRM and the audience half
// of a newsletter tool. Rules: double opt-in before any marketing mail; a
// one-click unsubscribe link in every campaign; a contact exists because
// they bought, wrote, or asked — never because we scraped.
import { nowIso } from '../db.mjs';
import { token } from '../http.mjs';

export const normEmail = (s) => String(s || '').trim().toLowerCase();
export function validEmail(s) {
  const em = normEmail(s);
  if (em.length < 3 || em.length > 254 || /\s/.test(em)) return false;
  const at = em.indexOf('@'); if (at < 1 || at !== em.lastIndexOf('@')) return false;
  const domain = em.slice(at + 1); const dot = domain.lastIndexOf('.');
  return dot > 0 && dot < domain.length - 1;
}

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
// Tags are a comma-separated list, trimmed, deduplicated, stored lowercase.
export const normalizeTags = (t) => [...new Set((Array.isArray(t) ? t : String(t || '').split(',')).map(x => String(x).trim().toLowerCase().replace(/\s+/g, '-').slice(0, 40)).filter(Boolean))].join(',');
export const tagList = (t) => String(t || '').split(',').filter(Boolean);
export function setContactTags(db, id, tags) { const c = contactById(db, id); if (!c) throw new Error('no such contact'); const t = normalizeTags(tags); db.run('UPDATE contacts SET tags = ? WHERE id = ?', t, id); db.audit('web', 'contact.tags', 'contact', id, { tags: t }); return contactById(db, id); }

// The customers list a storefront admin shows: who they are, how many orders,
// what they spent, when they last bought. Spend excludes canceled orders and
// refunds, so it is money actually kept.
export function listCustomers(db, { q = '', tag = '', segment = '', sort = 'recent', limit = 300 } = {}) {
  const like = `%${q}%`;
  const order = { spent: 'spent_cents DESC, id DESC', orders: 'orders DESC, id DESC', name: 'name COLLATE NOCASE, email', recent: 'COALESCE(last_order_at, created_at) DESC' }[sort] || 'id DESC';
  const seg = { repeat: 'WHERE orders >= 2', consented: 'WHERE consent_marketing = 1 AND unsubscribed_at IS NULL', waitlist: 'WHERE id IN (SELECT contact_id FROM waitlist WHERE notified_at IS NULL)', orgs: "WHERE kind IN ('school','business','nonprofit')", never: 'WHERE orders = 0' }[segment] || '';
  return db.all(`SELECT * FROM (SELECT c.*,
      (SELECT COUNT(*) FROM orders o WHERE o.contact_id = c.id AND o.status != 'canceled' AND o.total_cents > 0) AS orders,
      (SELECT COALESCE(SUM(o.total_cents - o.refunded_cents),0) FROM orders o WHERE o.contact_id = c.id AND o.status != 'canceled') AS spent_cents,
      (SELECT MAX(o.paid_at) FROM orders o WHERE o.contact_id = c.id AND o.status != 'canceled') AS last_order_at,
      (SELECT MIN(o.paid_at) FROM orders o WHERE o.contact_id = c.id AND o.status != 'canceled') AS first_order_at,
      (SELECT COUNT(*) FROM tickets t WHERE t.contact_id = c.id) AS tickets
    FROM contacts c WHERE (? = '' OR c.email LIKE ? OR c.name LIKE ? OR c.org LIKE ?) AND (? = '' OR (',' || c.tags || ',') LIKE ?)) ${seg} ORDER BY ${order} LIMIT ?`, q, like, like, like, tag, `%,${tag},%`, limit);
}
export function customerStats(db, id) {
  const r = db.get(`SELECT COUNT(*) AS orders, COALESCE(SUM(total_cents - refunded_cents),0) AS spent_cents, COALESCE(SUM(refunded_cents),0) AS refunded_cents, MIN(paid_at) AS first_order_at, MAX(paid_at) AS last_order_at,
    COALESCE(SUM((SELECT SUM(qty) FROM order_items i WHERE i.order_id = o.id)),0) AS units FROM orders o WHERE contact_id = ? AND status != 'canceled' AND total_cents > 0`, id);
  const t = db.get('SELECT COUNT(*) AS tickets, SUM(CASE WHEN status != \'closed\' THEN 1 ELSE 0 END) AS open_tickets FROM tickets WHERE contact_id = ?', id);
  const w = db.get('SELECT COUNT(*) AS claims FROM warranty_claims WHERE contact_id = ? OR order_id IN (SELECT id FROM orders WHERE contact_id = ?)', id, id);
  return { ...r, aov_cents: r.orders ? Math.round(r.spent_cents / r.orders) : 0, tickets: t.tickets, open_tickets: t.open_tickets || 0, claims: w.claims,
    top_sku: db.get('SELECT i.sku, SUM(i.qty) AS n FROM order_items i JOIN orders o ON o.id = i.order_id WHERE o.contact_id = ? AND o.status != \'canceled\' GROUP BY i.sku ORDER BY n DESC LIMIT 1', id)?.sku || null };
}
export const customerCounts = (db) => ({ all: db.get('SELECT COUNT(*) AS n FROM contacts').n, repeat: listCustomers(db, { segment: 'repeat', limit: 100000 }).length, consented: db.get('SELECT COUNT(*) AS n FROM contacts WHERE consent_marketing = 1 AND unsubscribed_at IS NULL').n, waitlist: db.get('SELECT COUNT(DISTINCT contact_id) AS n FROM waitlist WHERE notified_at IS NULL').n, orgs: db.get("SELECT COUNT(*) AS n FROM contacts WHERE kind IN ('school','business','nonprofit')").n });

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
