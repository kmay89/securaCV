// Delivery: the estimate a customer sees before the carrier says anything,
// the tracking link, and the signed token behind the public order-status
// page. No carrier is contacted here (that is shipping.mjs, opt-in).
import { createHmac } from 'node:crypto';

// Four bands west→east; a parcel crosses about one band a day on USPS Ground Advantage.
const BAND = { WA: 0, OR: 0, CA: 0, NV: 0, AK: 0, HI: 0, ID: 1, MT: 1, WY: 1, UT: 1, CO: 1, AZ: 1, NM: 1, ND: 2, SD: 2, NE: 2, KS: 2, OK: 2, TX: 2, MN: 2, IA: 2, MO: 2, AR: 2, LA: 2, WI: 2, IL: 2, MS: 2,
  MI: 3, IN: 3, OH: 3, KY: 3, TN: 3, AL: 3, GA: 3, FL: 3, SC: 3, NC: 3, VA: 3, WV: 3, PA: 3, NY: 3, NJ: 3, DE: 3, MD: 3, DC: 3, CT: 3, RI: 3, MA: 3, VT: 3, NH: 3, ME: 3 };
const addBusinessDays = (d, n) => { const x = new Date(d); let left = n; while (left > 0) { x.setUTCDate(x.getUTCDate() + 1); const wd = x.getUTCDay(); if (wd !== 0 && wd !== 6) left--; } return x; };
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const short = (d) => `${MONTHS[d.getUTCMonth()]} ${d.getUTCDate()}`;

export function estimateDelivery(shippedAt, toState, { fromState = 'MA', carrier = 'USPS' } = {}) {
  if (!shippedAt) return null;
  const from = BAND[String(fromState || '').toUpperCase()], to = BAND[String(toState || '').toUpperCase()];
  const hop = from == null || to == null ? 2 : Math.abs(from - to);
  const min = Math.min(5, 2 + Math.max(0, hop - 1)), max = Math.min(7, min + 2);   // 2–4 near, up to 5–7 coast to coast
  const start = new Date(shippedAt); const earliest = addBusinessDays(start, min), latest = addBusinessDays(start, max);
  return { min_days: min, max_days: max, earliest: earliest.toISOString().slice(0, 10), latest: latest.toISOString().slice(0, 10), label: `${short(earliest)}–${short(latest)}`, carrier, basis: 'estimate' };
}
export function trackingUrl(carrier, tracking) {
  if (!tracking) return null; const t = encodeURIComponent(String(tracking).replace(/\s+/g, ''));
  const c = String(carrier || 'USPS').toUpperCase();
  if (c.includes('UPS')) return `https://www.ups.com/track?tracknum=${t}`;
  if (c.includes('FEDEX')) return `https://www.fedex.com/fedextrack/?trknbr=${t}`;
  if (c.includes('DHL')) return `https://www.dhl.com/us-en/home/tracking.html?tracking-id=${t}`;
  return `https://tools.usps.com/go/TrackConfirmAction?tLabels=${t}`;
}
export const orderToken = (secret, number) => createHmac('sha256', String(secret)).update(`order:${number}`).digest('base64url').slice(0, 24);
export const orderStatusUrl = (cfg, order) => `${cfg.baseUrl}/o/${order.number}/${orderToken(cfg.secret, order.number)}`;
// The one line a customer wants: where is it, when will it land.
export function deliveryLine(o, cfg) {
  if (o.status === 'delivered') return { state: 'delivered', text: `Delivered ${o.delivered_at ? o.delivered_at.slice(0, 10) : ''}` };
  if (o.status !== 'shipped') return { state: o.status, text: o.status === 'packed' ? 'Packed — the label prints next' : o.status === 'paid' ? 'Paid — packed in the next batch' : o.status };
  const est = estimateDelivery(o.shipped_at, o.ship_state, { fromState: cfg.shipFromState, carrier: o.carrier });
  if (o.expected_delivery_at) return { state: o.tracking_status || 'in_transit', text: `${{ out_for_delivery: 'Out for delivery', alert: 'Carrier reports a problem', label: 'Label printed', delivered: 'Delivered' }[o.tracking_status] || 'In transit'} · expected ${o.expected_delivery_at}`, carrier: true };
  return { state: o.tracking_status || 'in_transit', text: `${o.tracking_status === 'out_for_delivery' ? 'Out for delivery' : 'On its way'} · estimated ${est?.label || 'soon'}`, estimate: est };
}
