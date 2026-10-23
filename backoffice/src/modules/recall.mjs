// Recalls and safety notices: by batch, firmware version or SKU, every
// device, the order it shipped in and the customer who has it — then one
// transactional notice each, with their serials filled in. Transactional
// because a safety notice is about something they own, not marketing.
import { nowIso } from '../db.mjs';
import { enqueue, fill } from './mail.mjs';

export function recallQuery(db, { batch = '', fw = '', sku = '' } = {}) {
  const devices = db.all(`SELECT d.*, o.number AS order_number, o.id AS order_id, o.ship_state, c.id AS contact_id, c.email, c.name FROM devices d LEFT JOIN orders o ON o.id = d.order_id LEFT JOIN contacts c ON c.id = o.contact_id
    WHERE (? = '' OR d.batch = ?) AND (? = '' OR d.fw_version = ?) AND (? = '' OR d.product_sku = ?) ORDER BY d.serial`, batch, batch, fw, fw, sku, sku);
  const byContact = new Map();
  for (const d of devices) { if (!d.contact_id || d.forgotten_at) continue; if (!byContact.has(d.contact_id)) byContact.set(d.contact_id, { contact_id: d.contact_id, email: d.email, name: d.name, serials: [], orders: new Set() }); const c = byContact.get(d.contact_id); c.serials.push(d.serial); c.orders.add(d.order_number); }
  const customers = [...byContact.values()].map(c => ({ ...c, orders: [...c.orders] }));
  return { filter: { batch, fw, sku }, devices, customers, in_stock: devices.filter(d => !d.order_id).length, shipped: devices.filter(d => d.order_id).length };
}
export const batches = (db) => db.all('SELECT batch, fw_version, product_sku, COUNT(*) AS n, SUM(order_id IS NOT NULL) AS shipped FROM devices GROUP BY batch, fw_version, product_sku ORDER BY batch DESC, fw_version DESC');
export const DEFAULT_BODY = `Hi {{first_name}},

This is a notice about the Canary board{{plural}} you have from us, serial{{plural}} {{serials}}.

[What we found, in two sentences. What it affects. What it does not affect.]

What to do: [nothing / take the firmware update from the Lab / reply to this mail and we send a replacement board]. There is nothing to send back.

Reply to this mail with any question — a person reads every one.

— {{company}}`;
export function createRecall(db, { title, body, filter, actor = 'web' }) {
  const q = recallQuery(db, filter); const number = db.nextNumber('RC');
  const r = db.run('INSERT INTO recalls (number, title, body, filter, devices, customers, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)', number, title, body, JSON.stringify(q.filter), q.devices.length, q.customers.length, nowIso());
  db.audit(actor, 'recall.create', 'recall', number, { ...q.filter, devices: q.devices.length, customers: q.customers.length });
  return recallById(db, r.lastInsertRowid);
}
export const recallById = (db, id) => { const r = db.get('SELECT * FROM recalls WHERE id = ?', id); if (r) r.filter = JSON.parse(r.filter); return r; };
export const listRecalls = (db) => db.all('SELECT * FROM recalls ORDER BY id DESC').map(r => ({ ...r, filter: JSON.parse(r.filter) }));
export function sendRecall(db, id, cfg, { actor = 'web' } = {}) {
  const r = recallById(db, id); if (!r) throw new Error('no such recall'); if (r.sent_at) throw new Error('already sent');
  const q = recallQuery(db, r.filter); let n = 0;
  db.tx(() => {
    for (const c of q.customers) {
      const vars = { name: c.name || 'there', first_name: (c.name || 'there').split(' ')[0], serials: c.serials.join(', '), plural: c.serials.length === 1 ? '' : 's', orders: c.orders.join(', '), company: cfg.company || 'Errer Labs', site: cfg.siteOrigin };
      enqueue(db, { to: c.email, subject: fill(r.title, vars), kind: 'transactional', ref: `recall:${r.number}`, contactId: c.contact_id, text: fill(r.body, vars) }); n++;
      for (const num of c.orders) { const o = db.get('SELECT id FROM orders WHERE number = ?', num); if (o) db.run('INSERT INTO order_events (order_id, at, actor, kind, text) VALUES (?, ?, ?, ?, ?)', o.id, nowIso(), actor, 'mail', `Safety notice ${r.number} sent: ${r.title}`); }
    }
    db.run('UPDATE recalls SET sent_at = ?, customers = ?, devices = ? WHERE id = ?', nowIso(), n, q.devices.length, id);
  });
  db.audit(actor, 'recall.send', 'recall', r.number, { customers: n }); return { ...recallById(db, id), queued: n };
}
