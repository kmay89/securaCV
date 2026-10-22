// One search box over everything with a number, a name or a serial: orders,
// customers, tickets, devices, invoices, warranty claims, discount codes.
// Each group is capped; the page links through to the full lists.
export function search(db, q, { limit = 8 } = {}) {
  const s = String(q || '').trim(); if (!s) return { q: s, total: 0, groups: [] };
  const like = `%${s}%`;
  const groups = [
    ['orders', db.all('SELECT o.id, o.number, o.status, o.ship_name, o.total_cents, o.tracking, c.email FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id WHERE o.number LIKE ? OR o.ship_name LIKE ? OR c.email LIKE ? OR o.tracking LIKE ? OR o.external_id LIKE ? OR o.payment_intent LIKE ? OR (\',\' || o.tags || \',\') LIKE ? ORDER BY o.id DESC LIMIT ?', like, like, like, like, like, like, like, limit)],
    ['customers', db.all('SELECT id, email, name, org, kind, tags FROM contacts WHERE email LIKE ? OR name LIKE ? OR org LIKE ? OR (\',\' || tags || \',\') LIKE ? ORDER BY id DESC LIMIT ?', like, like, like, like, limit)],
    ['tickets', db.all('SELECT t.id, t.number, t.subject, t.status, c.email FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id WHERE t.number LIKE ? OR t.subject LIKE ? OR c.email LIKE ? OR t.id IN (SELECT ticket_id FROM ticket_messages WHERE body LIKE ?) ORDER BY t.updated_at DESC LIMIT ?', like, like, like, like, limit)],
    ['devices', db.all('SELECT d.id, d.serial, d.mac, d.status, d.product_sku, o.number AS order_number, o.id AS order_id FROM devices d LEFT JOIN orders o ON o.id = d.order_id WHERE d.serial LIKE ? OR d.mac LIKE ? OR d.batch LIKE ? LIMIT ?', like, like, like, limit)],
    ['invoices', db.all('SELECT i.id, i.number, i.status, i.total_cents, i.po_number, c.email FROM invoices i LEFT JOIN contacts c ON c.id = i.contact_id WHERE i.number LIKE ? OR i.po_number LIKE ? OR c.email LIKE ? OR i.bill_to LIKE ? LIMIT ?', like, like, like, like, limit)],
    ['claims', db.all('SELECT w.id, w.number, w.status, w.category, d.serial, o.number AS order_number FROM warranty_claims w LEFT JOIN orders o ON o.id = w.order_id LEFT JOIN devices d ON d.id = w.device_id WHERE w.number LIKE ? OR d.serial LIKE ? OR o.number LIKE ? OR w.description LIKE ? LIMIT ?', like, like, like, like, limit)],
    ['discounts', db.all('SELECT code, kind, value, status FROM discounts WHERE code LIKE ? LIMIT ?', like, limit)],
  ].filter(([, rows]) => rows.length).map(([kind, rows]) => ({ kind, rows }));
  return { q: s, total: groups.reduce((n, g) => n + g.rows.length, 0), groups };
}
