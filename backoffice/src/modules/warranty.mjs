// Warranty — fix, not return (terms-of-sale.html §7). Every case has a
// defined answer before it happens, so a claim is categorized, not argued.
// A claim opens a ticket (the customer conversation), and when the answer
// is a reship it creates a $0 order so the batch, the labels and the
// inventory all see it like any other box.
import { nowIso } from '../db.mjs';
import { orderById, orderByNumber } from './orders.mjs';
import { upsertContact } from './contacts.mjs';
import { openTicket, addMessage } from './support.mjs';
import { enqueue, fill } from './mail.mjs';

export const ANSWERS = {
  warped_print:   { label: 'A printed part came out warped or wrong',  action: 'Reship just the parts pack',                       returns: false },
  missing_part:   { label: 'Something was missing from the box',       action: 'Reship the missing part',                          returns: false },
  dead_board:     { label: 'A board is dead on arrival',               action: 'Reship a replacement board — no questions',        returns: false },
  built_wrong:    { label: 'We built or flashed it wrong',             action: 'Reship a corrected full unit',                     returns: false },
  wrong_item:     { label: 'We shipped the wrong item',                action: 'Reship the correct item',                          returns: false },
  mail_damage:    { label: 'Crushed or lost in the mail',              action: 'Full replacement, and we file the USPS claim',     returns: false },
  not_covered:    { label: 'Misuse, modification, accident, or wear',  action: 'Not covered — point at replacement parts and printed enclosures in the store', returns: false },
};

export function openClaim(db, { orderNumber, email, serial = null, category, description = '', cfg = {}, actor = 'web' }) {
  if (!ANSWERS[category]) throw new Error(`unknown warranty category ${category}`);
  const order = orderNumber ? orderByNumber(db, orderNumber) : null;
  const contact = email ? upsertContact(db, { email, source: 'support' }) : (order?.contact_id ? db.get('SELECT * FROM contacts WHERE id = ?', order.contact_id) : null);
  const device = serial ? db.get('SELECT * FROM devices WHERE serial = ?', String(serial).toUpperCase()) : null;
  return db.tx(() => {
    const number = db.nextNumber('WC');
    const ticket = openTicket(db, { email: contact?.email || email, subject: `Warranty ${number}: ${ANSWERS[category].label}`, body: description || '(no description)', kind: 'warranty', channel: 'manual', cfg, autoAck: false });
    const r = db.run(`INSERT INTO warranty_claims (number, order_id, device_id, contact_id, ticket_id, category, description, resolution, status, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, number, order?.id ?? null, device?.id ?? null, contact?.id ?? null, ticket.id, category, description, ANSWERS[category].action, category === 'not_covered' ? 'declined' : 'open', nowIso());
    addMessage(db, ticket.id, { direction: 'note', body: `Claim ${number} opened. Defined answer: ${ANSWERS[category].action}. Send the original back? No.` });
    db.audit(actor, 'warranty.open', 'warranty', number, { category, order: order?.number, serial });
    return claimById(db, r.lastInsertRowid);
  });
}

export const claimById = (db, id) => db.get('SELECT w.*, o.number AS order_number, c.email, d.serial FROM warranty_claims w LEFT JOIN orders o ON o.id = w.order_id LEFT JOIN contacts c ON c.id = w.contact_id LEFT JOIN devices d ON d.id = w.device_id WHERE w.id = ?', id);
export const listClaims = (db, { status = '' } = {}) => db.all('SELECT w.*, o.number AS order_number, c.email FROM warranty_claims w LEFT JOIN orders o ON o.id = w.order_id LEFT JOIN contacts c ON c.id = w.contact_id WHERE (? = \'\' OR w.status = ?) ORDER BY w.id DESC', status, status);

// Create the $0 reship order with the items the answer calls for.
export function reship(db, claimId, items, { recordOrderFn, catalog = null, actor = 'web' } = {}) {
  const c = claimById(db, claimId); if (!c) throw new Error('no such claim');
  const orig = c.order_id ? orderById(db, c.order_id) : null;
  if (!orig) throw new Error('claim has no original order to copy the address from');
  const { order } = recordOrderFn(db, {
    source: 'manual', email: c.email, name: orig.ship_name, currency: orig.currency, subtotal_cents: 0, shipping_cents: 0, tax_cents: 0, total_cents: 0,
    ship: { line1: orig.ship_line1, line2: orig.ship_line2, city: orig.ship_city, state: orig.ship_state, zip: orig.ship_zip, country: orig.ship_country },
    items: items.map(it => ({ ...it, unit_cents: 0 })), notes: `warranty reship for ${c.number} (orig ${orig.number})`,
  }, { catalog, actor });
  db.run('UPDATE warranty_claims SET reship_order_id = ?, status = \'shipping\' WHERE id = ?', order.id, claimId);
  db.audit(actor, 'warranty.reship', 'warranty', c.number, { reship: order.number });
  return claimById(db, claimId);
}

export function resolveClaim(db, claimId, { costCents = 0, note = '', cfg = {}, actor = 'web' } = {}) {
  const c = claimById(db, claimId); if (!c) throw new Error('no such claim');
  db.run('UPDATE warranty_claims SET status = \'resolved\', resolved_at = ?, cost_cents = ? WHERE id = ?', nowIso(), Number(costCents) || 0, claimId);
  if (c.ticket_id) { addMessage(db, c.ticket_id, { direction: 'note', body: `Claim ${c.number} resolved. ${note}`.trim() }); db.run('UPDATE tickets SET status = \'closed\', closed_at = ?, updated_at = ? WHERE id = ?', nowIso(), nowIso(), c.ticket_id); }
  if (c.email) enqueue(db, { to: c.email, subject: `Warranty ${c.number} — resolved`, kind: 'transactional', ref: `warranty:${c.number}`,
    text: fill(RESOLVED_TPL, { number: c.number, action: c.resolution, note, company: cfg.company || 'Errer Labs' }) });
  db.audit(actor, 'warranty.resolve', 'warranty', c.number, { costCents, note });
  return claimById(db, claimId);
}

export const warrantyStats = (db) => ({
  open: db.get('SELECT COUNT(*) AS n FROM warranty_claims WHERE status IN (\'open\',\'shipping\')').n,
  cost90: db.get('SELECT COALESCE(SUM(cost_cents),0) AS c FROM warranty_claims WHERE created_at >= ?', new Date(Date.now() - 90 * 86400000).toISOString()).c,
  rate90: (() => { const o = db.get('SELECT COUNT(*) AS n FROM orders WHERE paid_at >= ? AND total_cents > 0', new Date(Date.now() - 90 * 86400000).toISOString()).n; const w = db.get('SELECT COUNT(*) AS n FROM warranty_claims WHERE created_at >= ? AND status != \'declined\'', new Date(Date.now() - 90 * 86400000).toISOString()).n; return o ? w / o : 0; })(),
});

const RESOLVED_TPL = `Your warranty claim {{number}} is resolved.

What we did: {{action}}
{{note}}

You don't need to send anything back — keep or recycle the original.

— {{company}}
`;
