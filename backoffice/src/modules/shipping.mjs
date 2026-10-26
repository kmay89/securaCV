// Carrier tracking, opt-in: with USPS API credentials the hourly job asks
// USPS where each open shipment is, writes the answer on the order (status,
// expected date, the carrier's own words), marks delivered when the carrier
// says so — which starts the "settled in?" automation from the real day —
// and raises an exception on a carrier alert. Without credentials nothing is
// contacted and the customer sees the estimate.
import { nowIso } from '../db.mjs';
import { transition, addEvent } from './orders.mjs';
import { raise, resolve } from './exceptions.mjs';
import { timedFetch } from '../net.mjs';

export const trackingConfigured = (cfg) => !!(cfg.uspsClientId && cfg.uspsClientSecret);
let tokenCache = { token: null, until: 0 };
async function uspsToken(cfg, fetchImpl) {
  if (tokenCache.token && Date.now() < tokenCache.until) return tokenCache.token;
  const r = await fetchImpl(`${cfg.uspsApiBase}/oauth2/v3/token`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ client_id: cfg.uspsClientId, client_secret: cfg.uspsClientSecret, grant_type: 'client_credentials' }) });
  if (!r.ok) throw new Error(`usps token ${r.status}`);
  const b = await r.json(); tokenCache = { token: b.access_token, until: Date.now() + Math.max(60, (b.expires_in || 3600) - 60) * 1000 }; return tokenCache.token;
}
export const resetTokenCache = () => { tokenCache = { token: null, until: 0 }; };

// The carrier's vocabulary → ours. Defensive: a word match, not a code table that changes under us.
export function mapUsps(body) {
  const s = `${body.statusCategory || ''} ${body.status || ''} ${body.statusSummary || ''}`;
  const status = /delivered to agent|delivered/i.test(s) && !/not delivered|undeliverable/i.test(s) ? 'delivered' : /out for delivery/i.test(s) ? 'out_for_delivery' : /alert|exception|undeliverable|return to sender|not delivered|held/i.test(s) ? 'alert' : /pre-?shipment|label created|shipping label/i.test(s) ? 'label' : 'in_transit';
  const ev = (body.trackingEvents || [])[0] || {};
  const detail = [body.statusSummary || body.status || body.statusCategory, ev.eventCity && `${ev.eventCity}${ev.eventState ? ', ' + ev.eventState : ''}`].filter(Boolean).join(' · ').slice(0, 200);
  const expected = (body.expectedDeliveryDate || body.expectedDeliveryTimeStamp || '').slice(0, 10) || null;
  const deliveredAt = status === 'delivered' ? (ev.eventTimestamp || null) : null;
  return { status, detail, expected, deliveredAt };
}

export async function pollTracking(db, cfg, { fetchImpl = timedFetch, now = Date.now(), limit = 50, minAgeH = 4 } = {}) {
  if (!trackingConfigured(cfg)) return { skipped: 'USPS_CLIENT_ID / USPS_CLIENT_SECRET not set — customers see the estimate' };
  const since = new Date(now - minAgeH * 3600000).toISOString();
  const open = db.all("SELECT * FROM orders WHERE status = 'shipped' AND tracking IS NOT NULL AND tracking != '' AND (carrier IS NULL OR carrier LIKE '%USPS%') AND (tracking_checked_at IS NULL OR tracking_checked_at < ?) ORDER BY shipped_at LIMIT ?", since, limit);
  const out = { checked: 0, changed: 0, delivered: 0, alerts: 0, errors: 0 };
  if (!open.length) return out;
  const token = await uspsToken(cfg, fetchImpl);
  for (const o of open) {
    out.checked++;
    let m;
    try {
      const r = await fetchImpl(`${cfg.uspsApiBase}/tracking/v3/tracking/${encodeURIComponent(o.tracking.replace(/\s+/g, ''))}?expand=DETAIL`, { headers: { authorization: `Bearer ${token}` } });
      if (!r.ok) throw new Error(`usps ${r.status}`); m = mapUsps(await r.json());
    } catch (e) { out.errors++; db.run('UPDATE orders SET tracking_checked_at = ?, tracking_detail = ? WHERE id = ?', nowIso(), `carrier lookup failed: ${String(e.message || e).slice(0, 120)}`, o.id); continue; }
    const changed = m.status !== o.tracking_status || (m.expected && m.expected !== o.expected_delivery_at);
    db.run('UPDATE orders SET tracking_status = ?, tracking_detail = ?, tracking_checked_at = ?, expected_delivery_at = COALESCE(?, expected_delivery_at) WHERE id = ?', m.status, m.detail, nowIso(), m.expected, o.id);
    if (changed) { out.changed++; addEvent(db, o.id, { kind: 'tracking', actor: 'carrier', text: `${{ delivered: 'Delivered', out_for_delivery: 'Out for delivery', alert: 'Carrier alert', label: 'Label printed, not yet in the mail', in_transit: 'In transit' }[m.status]}${m.detail ? ` — ${m.detail}` : ''}${m.expected ? ` · expected ${m.expected}` : ''}` }); }
    if (m.status === 'delivered') { transition(db, o.id, 'delivered', { actor: 'carrier', at: m.deliveredAt || nowIso() }); out.delivered++; await resolve(db, cfg, `tracking:${o.number}`, 'Delivered.'); }
    else if (m.status === 'alert') { out.alerts++; await raise(db, cfg, `tracking:${o.number}`, `shipping [carrier alert]: ${o.number}`, `${o.carrier || 'USPS'} ${o.tracking}: ${m.detail}\n\nThe customer sees "Carrier reports a problem" on their status page. Look it up, then write to ${o.email || 'the customer'}: ${cfg.baseUrl}/orders/${o.id}`); }
    else await resolve(db, cfg, `tracking:${o.number}`, 'Moving again.');
  }
  return out;
}
