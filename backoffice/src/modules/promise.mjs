// What the customer was promised, what is true now, and what they have been told.
//
// The store makes one promise at checkout — the day the order ships and the window it arrives in —
// and the confirmation mail repeats it. That promise is written on the order (promised_ship_on,
// promised_arrive_by) so the status page, the order page, the attention scale and the late mail all
// read the same dates, whatever the batch settings are later. An order from before the column
// existed gets the same promise recomputed from its payment time.
//
// Every state has one honest line for the customer (customerLine) and the mails that state owes
// (a cancel, a refund) go out by themselves; the ones that are an apology or a question (late,
// carrier problem, doubtful address) are drafted here and sent by a person's click, once per order.
import { nextShipDate, stateForZip, zoneParts, dayLabel } from './storefront.mjs';
import { estimateDelivery, deliveryLine, orderStatusUrl, trackingUrl } from './delivery.mjs';
import { enqueue, fill } from './mail.mjs';
import { addEvent, orderById, addressIssues } from './orders.mjs';
import { currentTz } from '../render.mjs';

const D = 86400000;
const batchOpts = (cfg) => ({ tz: cfg?.tz || currentTz(), closeDay: cfg?.batchCloseDay ?? 0, closeHour: cfg?.batchCloseHour ?? 23, shipDay: cfg?.batchShipDay ?? 2 });
const usd = (c) => `$${(c / 100).toFixed(2)}`;
export const ymdParts = (ymd) => { const [y, m, d] = String(ymd).split('-').map(Number); return { y, m, d, weekday: new Date(Date.UTC(y, m - 1, d)).getUTCDay() }; };
export const labelOf = (ymd) => ymd ? dayLabel(ymdParts(ymd)) : '';
const today = (cfg, now = Date.now()) => { const p = zoneParts(new Date(now), cfg?.tz || currentTz()); return `${p.y}-${String(p.m).padStart(2, '0')}-${String(p.d).padStart(2, '0')}`; };

// The promise for an order paid at `paidAt` going to `zip`: the batch it fell into and the arrival window.
export function promiseFor(paidAt, { zip = null, state = null, cfg = {} } = {}) {
  const ships = nextShipDate(new Date(paidAt || Date.now()), batchOpts(cfg));
  const to = state || stateForZip(zip);
  const d = estimateDelivery(new Date(`${ships.ships_on}T16:00:00Z`), to, { fromState: cfg.shipFromState, carrier: 'USPS' });
  return { ship_on: ships.ships_on, closes_on: ships.closes_on, arrive_by: d ? d.latest : null, arrive_from: d ? d.earliest : null, arrive_label: d ? d.label : '' };
}

// The order's promise as stored, else recomputed; with where it stands against the clock today.
export function orderPromise(o, cfg = {}, { now = Date.now() } = {}) {
  if (!o) return null;
  const stored = o.promised_ship_on ? { ship_on: o.promised_ship_on, arrive_by: o.promised_arrive_by || null } : null;
  const p = stored || promiseFor(o.paid_at || o.created_at, { zip: o.ship_zip, state: o.ship_state, cfg });
  const t = today(cfg, now); const open = ['paid', 'packed'].includes(o.status);
  const daysLate = open && p.ship_on && t > p.ship_on ? Math.round((Date.parse(t) - Date.parse(p.ship_on)) / D) : 0;
  return { ...p, stored: !!stored, ships_label: labelOf(p.ship_on), arrive_label: p.arrive_by ? `${labelOf(p.arrive_from || p.arrive_by)}–${labelOf(p.arrive_by)}`.replace(/^–/, '') : (p.arrive_label || ''),
    open, late: daysLate > 0, days_late: daysLate, ships_today: open && p.ship_on === t };
}

// One honest line per state, for the customer: where it is and what happens next. Never a bare status word.
export function customerLine(o, cfg = {}, { now = Date.now() } = {}) {
  const p = orderPromise(o, cfg, { now });
  if (o.status === 'paid' || o.status === 'packed') {
    if (p.late) return { state: 'late', text: `Running later than planned — it was due to ship ${p.ships_label}`, detail: `We are sorry. It ships as soon as it can, and the tracking number comes the day it does. No need to write; if you would rather not wait, reply to any of our mails and we refund in full.` };
    const packed = o.status === 'packed' ? 'Packed and waiting for the label — ' : '';
    return { state: o.status, text: `${packed}ships ${p.ships_label}${p.arrive_label ? ` · arrives ${p.arrive_label}` : ''}`, detail: `${o.status === 'paid' ? 'Orders go out in weekly batches. ' : ''}You get the tracking number the day it ships. The arrival window is an estimate from the route, not the carrier's word.` };
  }
  if (o.status === 'canceled') { const back = o.total_cents - o.refunded_cents; return { state: 'canceled', text: `Canceled${o.canceled_at ? ` ${labelOf(o.canceled_at.slice(0, 10))}` : ''}`, detail: o.refunded_cents ? `${usd(o.refunded_cents)} went back to your card; banks take 5–10 business days to show it.` : back > 0 ? `${usd(back)} is being refunded to your card; banks take 5–10 business days to show it.` : 'Nothing was charged.' }; }
  if (o.status === 'refunded') return { state: 'refunded', text: `Refunded ${usd(o.refunded_cents)}`, detail: 'Banks take 5–10 business days to show it. Keep anything that already arrived.' };
  const dl = deliveryLine(o, cfg);
  if (o.status === 'delivered') return { state: 'delivered', text: dl.text, detail: 'Your Canary provisions itself at home: power it on and point your phone at the QR card in the box. Anything wrong, missing or broken: reply to any of our mails and we fix it.' };
  if (dl.state === 'alert') return { state: 'alert', text: 'The carrier reports a problem', detail: `${o.tracking_detail ? `${o.tracking_detail}. ` : ''}We are looking into it and will write to you; if it is not moving within two business days we send another.` };
  return { state: dl.state, text: dl.text, detail: dl.estimate ? `An estimate from the ship date and the distance, not the carrier's word; ${dl.estimate.carrier} usually takes ${dl.estimate.min_days}–${dl.estimate.max_days} business days on this route.` : o.tracking_detail ? `From the carrier: ${o.tracking_detail}${o.tracking_checked_at ? ` (checked ${o.tracking_checked_at.slice(0, 16).replace('T', ' ')} UTC)` : ''}.` : '' };
}

// Every mail this order's customer has been sent (or that is still queued, or failed), newest last.
export const customerMails = (db, number) => db.all("SELECT id, subject, status, created_at, sent_at, last_error, ref FROM mail_outbox WHERE ref = ? OR ref LIKE ? ORDER BY id", `order:${number}`, `order:${number}:%`);
// Paid or packed orders past the day they were promised to ship, and whether the customer has been told.
export function lateOrders(db, cfg, { now = Date.now() } = {}) {
  return db.all("SELECT o.*, c.email FROM orders o LEFT JOIN contacts c ON c.id = o.contact_id WHERE o.status IN ('paid','packed') AND c.email IS NOT NULL ORDER BY o.paid_at").map(o => ({ ...o, promise: orderPromise(o, cfg, { now }) })).filter(o => o.promise.late);
}
export const toldLate = (db, number) => !!db.get("SELECT id FROM mail_outbox WHERE ref = ?", `order:${number}:late`);
export const told = (db, number, what) => !!db.get("SELECT id FROM mail_outbox WHERE ref = ?", `order:${number}:${what}`);

export const LATE_TPL = `Hi {{name}},

Your order {{number}} is running later than we said. It was due to ship {{ship_on}}; it has not, and that is on us.

It ships as soon as it can, and you get the tracking number the day it does. If you would rather not wait, reply to this mail and we refund in full, no questions.

Where it is, any time: {{status_url}}

— {{company}}
{{support}}
`;
export const CANCELED_TPL = `Hi {{name}},

Order {{number}} is canceled.
{{money}}
If this is a surprise, reply to this mail and a person will sort it out.

— {{company}}
{{support}}
`;
export const REFUND_TPL = `Hi {{name}},

{{amount}} is on its way back to your card for order {{number}}{{reason}}. Banks take 5–10 business days to show it.
{{keep}}
— {{company}}
{{support}}
`;
export const CARRIER_TPL = `Hi {{name}},

The carrier reports a problem with order {{number}} ({{carrier}} {{tracking}}):

  {{detail}}

We are on it. If it is not moving again within two business days we send another, no need to ask. Track it here: {{tracking_url}}

Where it is, any time: {{status_url}}

— {{company}}
{{support}}
`;
export const ADDRESS_TPL = `Hi {{name}},

Before we print the label for order {{number}}, the shipping address as we have it does not look right:

  {{address}}

({{issues}})

Reply to this mail with the correct address and it goes out in the next batch. If it is right as written, reply "it's right" and we ship as is.

— {{company}}
{{support}}
`;

const vars = (o, cfg) => ({ name: o.ship_name || 'there', number: o.number, company: cfg.company || 'Errer Labs', support: cfg.supportAddr || '', status_url: cfg.secret ? orderStatusUrl(cfg, o) : '' });
const tell = (db, o, cfg, what, { subject, text, actor, event }) => {
  if (!o.email) throw new Error(`${o.number} has no email`);
  const ref = `order:${o.number}:${what}`;
  if (db.get('SELECT id FROM mail_outbox WHERE ref = ?', ref)) return { order: o, sent: false, reason: 'already told' };
  const m = enqueue(db, { to: o.email, subject, kind: 'transactional', ref, contactId: o.contact_id, text });
  addEvent(db, o.id, { kind: 'mail', actor, text: event });
  db.audit(actor, `order.told_${what}`, 'order', o.number, { to: o.email });
  return { order: o, sent: true, mail: m };
};

// The late apology: once per order, by a person's click.
export function tellLate(db, cfg, orderId, { actor = 'web', now = Date.now() } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  const p = orderPromise(o, cfg, { now }); if (!p.late) throw new Error(`${o.number} is not late`);
  return tell(db, o, cfg, 'late', { actor, subject: `Your ${cfg.product || 'SecuraCV'} order ${o.number} is running late — sorry`, event: `Told ${o.email} the order is late (was due to ship ${p.ships_label})`,
    text: fill(LATE_TPL, { ...vars(o, cfg), ship_on: p.ships_label }) });
}
// The carrier-problem note: once per order.
export function tellCarrier(db, cfg, orderId, { actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (o.tracking_status !== 'alert') throw new Error(`${o.number}: the carrier reports no problem`);
  return tell(db, o, cfg, 'carrier', { actor, subject: `Your ${cfg.product || 'SecuraCV'} order ${o.number}: the carrier reports a problem`, event: `Told ${o.email} about the carrier problem`,
    text: fill(CARRIER_TPL, { ...vars(o, cfg), carrier: o.carrier || 'USPS', tracking: o.tracking || '', detail: o.tracking_detail || 'no detail given', tracking_url: trackingUrl(o.carrier, o.tracking) || '' }) });
}
// The address question: once per order, while it is still on the shelf.
export function tellAddress(db, cfg, orderId, { actor = 'web' } = {}) {
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (!['paid', 'packed'].includes(o.status)) throw new Error(`${o.number} has already shipped`);
  const issues = addressIssues({ line1: o.ship_line1, city: o.ship_city, state: o.ship_state, zip: o.ship_zip, country: o.ship_country }); if (!issues.length) throw new Error(`${o.number}: the address looks fine`);
  const address = [o.ship_name, o.ship_line1, o.ship_line2, `${o.ship_city}, ${o.ship_state} ${o.ship_zip}`].filter(Boolean).join('\n  ');
  return tell(db, o, cfg, 'address', { actor, subject: `Your ${cfg.product || 'SecuraCV'} order ${o.number}: please check the address`, event: `Asked ${o.email} to confirm the address (${issues.join('; ')})`,
    text: fill(ADDRESS_TPL, { ...vars(o, cfg), address, issues: issues.join('; ') }) });
}
