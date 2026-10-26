// Each card may carry `due_at`: the clock the admin is deciding against (an SLA, an invoice's
// date, a send time); the home page ranks and colors the queue by it (attention.mjs).
// The Decide queue: the system drafts the next move and says why; a person
// approves it with one click or ignores it. Nothing here acts on its own.
// Every suggestion is recomputed from the data each time, so there is no
// stale queue to tend — when the condition clears, the card disappears.
import { lowStock, buildable } from './inventory.mjs';
import { waitlistSummary } from './contacts.mjs';
import { onOrder, draftedFor } from './purchasing.mjs';
import { classify } from './support.mjs';

const CANNED_RULES = [[/blink|no hub|not showing|fleet|join/i, 'no-hub-blink'], [/ship|batch|update|when|tracking/i, 'batch-cadence'], [/face|recogni/i, 'no-face-rec']];

export function suggestions(db, cfg, { now = Date.now() } = {}) {
  const out = []; const money = (c) => `$${(c / 100).toFixed(2)}`;
  // parts to reorder → a drafted PO, or an existing draft to approve
  const oo = onOrder(db); const low = lowStock(db).filter(i => !oo[i.sku]);
  const drafts = new Map(); const toDraft = [];
  for (const i of low) { const d = draftedFor(db, i.sku); if (d) drafts.set(d.id, d); else toDraft.push(i); }
  for (const d of drafts.values()) { const items = db.all('SELECT sku, qty, unit_cost_cents FROM purchase_order_items WHERE po_id = ?', d.id); out.push({ key: `po:${d.number}`, kind: 'purchasing', title: `Approve ${d.number} to ${d.supplier}`, detail: `${items.map(i => `${i.qty}× ${i.sku}`).join(', ')} · ${money(items.reduce((s, i) => s + i.qty * i.unit_cost_cents, 0))}`, why: 'drafted from the reorder points; marking it ordered lets "buildable" look ahead', action: { href: `/purchasing/${d.id}/order`, label: 'Mark ordered', fields: { _back: '/' } }, link: `/purchasing/${d.id}` }); }
  if (toDraft.length) out.push({ key: 'po:draft', kind: 'purchasing', title: `Draft a purchase order for ${toDraft.length} low part${toDraft.length === 1 ? '' : 's'}`, detail: toDraft.map(i => `${i.sku} (${i.on_hand} left, reorder ${i.reorder_qty})`).join(', '), why: 'each is at or under its reorder point and nothing is on order', action: { href: '/purchasing/draft', label: 'Draft it', fields: { _back: '/' } }, link: '/inventory' });
  // people waiting for something that can be built
  for (const w of waitlistSummary(db)) { if (!w.waiting) continue; const b = buildable(db, w.sku); if (b == null || b < 1) continue; out.push({ key: `waitlist:${w.sku}`, kind: 'sales', title: `Tell ${w.waiting} ${w.waiting === 1 ? 'person' : 'people'} ${w.sku} is back`, detail: `${b} can be built from the shelf right now`, why: 'they asked for exactly one mail when it is in stock, and it is', action: { href: '/waitlist/notify', label: 'Send the one mail', fields: { sku: w.sku, _back: '/' } }, link: `/products/${encodeURIComponent(w.sku)}` }); }
  // a first reply drafted from a canned answer
  const canned = new Set(db.all('SELECT slug FROM canned_replies').map(r => r.slug));
  for (const t of db.all("SELECT t.*, c.name FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id WHERE t.status IN ('open','waiting_on_us') AND t.first_response_at IS NULL AND t.kind IN ('support','warranty') ORDER BY t.sla_due_at LIMIT 10")) {
    const first = db.get('SELECT body FROM ticket_messages WHERE ticket_id = ? AND direction = \'in\' ORDER BY id LIMIT 1', t.id)?.body || '';
    const rule = CANNED_RULES.find(([re]) => re.test(`${t.subject} ${first}`)); if (!rule || !canned.has(rule[1])) continue;
    const title = db.get('SELECT title FROM canned_replies WHERE slug = ?', rule[1]).title;
    out.push({ key: `reply:${t.number}`, kind: 'support', title: `Answer ${t.number} with "${title}"`, detail: `${t.name || t.email || 'someone'}: ${t.subject}`, why: `the question matches the canned reply; ${classify(t.subject, first).kind} ticket`, due_at: t.sla_due_at, action: { href: `/tickets/${t.id}/reply`, label: 'Send it', fields: { canned: rule[1], body: '', _back: '/' } }, link: `/tickets/${t.id}` });
  }
  // shipped long ago, carrier not consulted: probably delivered
  for (const o of db.all("SELECT id, number, ship_name, shipped_at FROM orders WHERE status = 'shipped' AND tracking_status IS NULL AND shipped_at < ? ORDER BY shipped_at LIMIT 5", new Date(now - 7 * 86400000).toISOString())) out.push({ key: `delivered:${o.number}`, kind: 'orders', title: `Mark ${o.number} delivered?`, detail: `${o.ship_name} · shipped ${o.shipped_at.slice(0, 10)}, ${Math.floor((now - Date.parse(o.shipped_at)) / 86400000)} days ago`, why: 'no carrier lookup is configured; after a week a USPS parcel has almost always landed, and "delivered" starts the settled-in note', action: { href: `/orders/${o.id}/transition`, label: 'Mark delivered', fields: { to: 'delivered', _back: '/' } }, link: `/orders/${o.id}` });
  // an invoice past due with no reminder in a week
  for (const i of db.all("SELECT i.*, c.email FROM invoices i LEFT JOIN contacts c ON c.id = i.contact_id WHERE i.status IN ('sent','overdue') AND i.due_at < ? ORDER BY i.due_at LIMIT 5", new Date(now).toISOString().slice(0, 10))) {
    const reminded = db.get("SELECT created_at FROM mail_outbox WHERE ref = ? AND created_at > ?", `invoice-reminder:${i.number}`, new Date(now - 7 * 86400000).toISOString()); if (reminded) continue;
    out.push({ key: `invoice:${i.number}`, kind: 'books', title: `Remind ${i.email || 'the buyer'} about ${i.number}`, detail: `${money(i.total_cents)} was due ${i.due_at}`, why: 'overdue, and no reminder went out this week', due_at: `${i.due_at}T23:59:59Z`, action: { href: `/invoices/${i.id}/remind`, label: 'Send a reminder', fields: { _back: '/' } }, link: `/invoices/${i.id}` });
  }
  // a campaign about to go out
  for (const cp of db.all("SELECT slug, subject, scheduled_at, segment FROM campaigns WHERE status = 'scheduled' AND scheduled_at < ?", new Date(now + 36 * 3600000).toISOString())) out.push({ key: `campaign:${cp.slug}`, kind: 'mail', title: `"${cp.subject}" goes out ${cp.scheduled_at.slice(0, 16).replace('T', ' ')} UTC`, detail: `to ${cp.segment} — last chance to read it once more`, why: 'scheduled within the next day and a half', due_at: cp.scheduled_at, link: `/campaigns/${cp.slug}` });
  return out;
}
