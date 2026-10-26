// One scale of urgency for the whole admin, computed from deadlines, never typed by hand.
//
//   now   — a deadline has passed or a person is already being failed (an SLA breached, an
//           exception open, an order waiting longer than the promise, a carrier alert, a check
//           that guards the books or the backups failing). Red, everywhere it appears.
//   soon  — it will become `now` inside a known window (an SLA due within hours, an order paid
//           two days ago and still on the shelf, a part under its reorder point). Amber.
//   calm  — real work, no clock on it yet. Neutral.
//
// Every surface that shows urgency reads it from here — the sidebar badges, the home cards, the
// Decide queue, a table row's left edge, a due date in a cell — so the same ticket is the same
// color on every page, and a new kind of work gets a level by stating its deadline, not by
// picking a color. The windows are the operator's promises, stated once below.
import { orderStats } from './modules/orders.mjs';
import { supportStats } from './modules/support.mjs';
import { lowStock } from './modules/inventory.mjs';
import { openExceptions } from './modules/exceptions.mjs';
import { doctor } from './modules/health.mjs';

export const LEVELS = ['now', 'soon', 'calm'];
const RANK = { now: 0, soon: 1, calm: 2 };
export const worst = (...levels) => levels.filter(Boolean).sort((a, b) => RANK[a] - RANK[b])[0] || 'calm';

// The promises. An order ships inside two days of payment; a ticket's SLA is its own column; a
// warranty claim is answered inside a week; "soon" starts six hours before a clock runs out.
export const WINDOWS = Object.freeze({
  orderShipHours: 48,          // paid and not shipped for longer than this is `soon`
  orderShipHardHours: 120,     // ... and longer than this is `now`: the customer is writing to ask
  slaSoonHours: 6,             // an SLA due inside this many hours is `soon`
  warrantyDays: 7,             // a claim open longer than this is `soon`
  campaignHours: 36,           // a scheduled send inside this window is worth one more read
  fatalChecks: ['integrity', 'books', 'backup', 'schema', 'environment'],   // a failing check here is `now`, any other `soon`
});

const H = 3600000, D = 86400000;
const KIND_ORDER = ['exceptions', 'tickets', 'orders', 'warranty', 'health', 'invoices', 'inventory'];

// A human span, coarse on purpose: the admin needs "2 d", not "47 h 12 m".
export function span(ms) {
  const a = Math.abs(ms);
  if (a < 60000) return 'under a minute';
  if (a < H) return `${Math.round(a / 60000)} min`;
  if (a < D) return `${Math.round(a / H)} h`;
  if (a < 14 * D) return `${Math.round(a / D)} d`;
  if (a < 60 * D) return `${Math.round(a / (7 * D))} wk`;
  return `${Math.round(a / (30 * D))} mo`;
}

// How long ago something happened: "3 h ago", "just now"; a bad or missing stamp reads as nothing.
export function ago(iso, now = Date.now()) {
  if (!iso) return ''; const t = Date.parse(iso); if (Number.isNaN(t)) return '';
  const ms = now - t; if (ms < 60000) return 'just now';
  return `${span(ms)} ago`;
}

// A deadline against the clock: its text ("overdue 15 h", "due in 2 h", "due in 3 d") and the level
// that follows from it. `soonMs` is how far ahead `soon` starts; a passed deadline is always `now`.
export function dueIn(iso, { now = Date.now(), soonMs = WINDOWS.slaSoonHours * H, done = false } = {}) {
  if (!iso || done) return { ms: null, text: '', level: 'calm' };
  const t = Date.parse(iso); if (Number.isNaN(t)) return { ms: null, text: '', level: 'calm' };
  const ms = t - now;
  if (ms < 0) return { ms, text: `overdue ${span(ms)}`, level: 'now' };
  return { ms, text: `due in ${span(ms)}`, level: ms <= soonMs ? 'soon' : 'calm' };
}

// How long something has been waiting, against a soft and a hard limit: the level and a text
// ("waiting 3 d"). An unshipped order, an unanswered claim.
export function waiting(iso, { now = Date.now(), soonMs, nowMs = Infinity } = {}) {
  if (!iso) return { ms: null, text: '', level: 'calm' };
  const t = Date.parse(iso); if (Number.isNaN(t)) return { ms: null, text: '', level: 'calm' };
  const ms = now - t;
  return { ms, text: `waiting ${span(ms)}`, level: ms >= nowMs ? 'now' : ms >= soonMs ? 'soon' : 'calm' };
}

// The level of one ticket row, from its own columns.
export function ticketLevel(t, now = Date.now()) {
  if (!t || t.status === 'closed' || t.status === 'waiting_on_customer') return 'calm';
  if (t.first_response_at) return 'calm';
  return dueIn(t.sla_due_at, { now }).level;
}
// The level of one order row: a paid or packed order against the shipping promise.
export function orderLevel(o, now = Date.now()) {
  if (!o || !['paid', 'packed'].includes(o.status)) return o?.tracking_status === 'alert' ? 'now' : 'calm';
  return waiting(o.paid_at, { now, soonMs: WINDOWS.orderShipHours * H, nowMs: WINDOWS.orderShipHardHours * H }).level;
}

// Everything that wants the admin today, as one list, each item with its level, its count, its
// deadline text and where to go. The sidebar badges, the home page and the morning digest are
// all views of this list, so they can never disagree about what is urgent.
export function attention(db, cfg, { now = Date.now() } = {}) {
  const items = [];
  const o = orderStats(db);
  if (o.open) {
    const oldest = db.get("SELECT MIN(paid_at) AS t FROM orders WHERE status IN ('paid','packed')").t;
    const w = waiting(oldest, { now, soonMs: WINDOWS.orderShipHours * H, nowMs: WINDOWS.orderShipHardHours * H });
    items.push({ key: 'orders', n: o.open, level: w.level, what: `order${o.open === 1 ? '' : 's'} to pack or ship`, due: w.ms != null && w.level !== 'calm' ? `oldest ${w.text}` : '', href: '/pack', nav: '/pack', action: 'Open the pack bench' });
  }
  const s = supportStats(db);
  if (s.open) {
    const next = db.get("SELECT MIN(sla_due_at) AS t FROM tickets WHERE status IN ('open','waiting_on_us') AND first_response_at IS NULL").t;
    const d = dueIn(next, { now });
    items.push({ key: 'tickets', n: s.open, level: s.breached ? 'now' : d.level, what: `support ticket${s.open === 1 ? '' : 's'} waiting on us${s.breached ? ` (${s.breached} past SLA)` : ''}`, due: s.breached ? `first reply ${d.text}` : d.text ? `next SLA ${d.text}` : '', href: '/tickets?status=open', nav: '/tickets', action: 'Answer' });
  }
  const ex = openExceptions(db);
  if (ex.length) items.push({ key: 'exceptions', n: ex.length, level: 'now', what: `exception${ex.length === 1 ? '' : 's'} need a decision: ${ex.map(x => x.key).join(', ')}`, due: '', href: '/exceptions', nav: '/exceptions', action: 'Decide' });
  const low = lowStock(db);
  if (low.length) items.push({ key: 'inventory', n: low.length, level: 'soon', what: `part${low.length === 1 ? '' : 's'} at or under the reorder point: ${low.map(i => i.sku).join(', ')}`, due: '', href: '/inventory', nav: '/inventory', action: 'Reorder' });
  const w = db.get("SELECT COUNT(*) AS n, MIN(created_at) AS t FROM warranty_claims WHERE status IN ('open','shipping')");
  if (w.n) { const wt = waiting(w.t, { now, soonMs: WINDOWS.warrantyDays * D }); items.push({ key: 'warranty', n: w.n, level: wt.level, what: `warranty claim${w.n === 1 ? '' : 's'} open`, due: wt.level !== 'calm' ? `oldest ${wt.text}` : '', href: '/warranty', nav: '/warranty', action: 'Resolve' }); }
  const inv = db.get("SELECT COALESCE(SUM(total_cents),0) AS c, COUNT(*) AS n, SUM(CASE WHEN due_at < ? THEN 1 ELSE 0 END) AS late FROM invoices WHERE status IN ('sent','overdue')", new Date(now).toISOString().slice(0, 10));
  if (inv.n) items.push({ key: 'invoices', n: inv.n, level: inv.late ? 'soon' : 'calm', what: `invoice${inv.n === 1 ? '' : 's'} outstanding, $${(inv.c / 100).toFixed(2)}`, due: inv.late ? `${inv.late} past due` : '', href: '/invoices', nav: '/invoices', action: 'Chase' });
  const h = doctor(db, cfg);
  if (!h.ok) items.push({ key: 'health', n: h.failing.length, level: h.failing.some(k => WINDOWS.fatalChecks.includes(k)) ? 'now' : 'soon', what: `health check${h.failing.length === 1 ? '' : 's'} need attention: ${h.failing.join(', ')}`, due: '', href: '/health', nav: '/health', action: 'Look', quiet: true });
  // at the same level, the kind that has a person waiting on the other end comes first
  items.sort((a, b) => RANK[a.level] - RANK[b.level] || KIND_ORDER.indexOf(a.key) - KIND_ORDER.indexOf(b.key));
  // the sidebar: one count per section, colored by the worst thing inside it
  const nav = {};
  for (const i of items) { const cur = nav[i.nav]; nav[i.nav] = { n: (cur?.n || 0) + i.n, level: worst(cur?.level, i.level) }; }
  return { items, nav, level: worst(...items.map(i => i.level)), health: h };
}
