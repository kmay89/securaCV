// The nudge: a new order reaches the operator's phone the minute the webhook lands. By mail to
// OPERATOR_EMAIL through the outbox (no new connection), and, if NTFY_URL is set, one POST to that
// ntfy topic — the operator's own choice of a push channel, off by default, documented in SECURITY.md.
// The nudge carries the order number, the money, the lines and the ship day; never the address.
import { enqueue } from './mail.mjs';
import { timedFetch } from '../net.mjs';

const usd = (c) => `$${(c / 100).toFixed(2)}`;
export function orderNudgeText(o, cfg, { promise = null } = {}) {
  const lines = (o.items || []).map(i => `${i.qty}× ${i.sku}`).join(', ') || 'no line items';
  const ships = promise?.ship_on ? ` · ships ${promise.ship_on}` : '';
  return { title: `New order ${o.number} · ${usd(o.total_cents)}`, body: `${lines}${ships}${o.ship_state ? ` · to ${o.ship_state}` : ''}${o.discount_code ? ` · code ${o.discount_code}` : ''}\n${cfg.baseUrl}/orders/${o.id}` };
}
export async function newOrderNudge(db, cfg, o, { fetchImpl = timedFetch, promise = null } = {}) {
  const out = { mail: false, push: false };
  if (!o || cfg.notifyNewOrder === false) return out;
  const t = orderNudgeText(o, cfg, { promise });
  if (cfg.operatorEmail) { enqueue(db, { to: cfg.operatorEmail, subject: t.title, kind: 'transactional', ref: `operator:order:${o.number}`, text: `${t.body}\n\n— the backoffice` }); out.mail = true; }
  if (cfg.ntfyUrl && /^https:\/\//.test(cfg.ntfyUrl)) {
    try { const r = await fetchImpl(cfg.ntfyUrl, { method: 'POST', headers: { title: t.title, tags: 'package', ...(cfg.ntfyToken ? { authorization: `Bearer ${cfg.ntfyToken}` } : {}) }, body: t.body }); out.push = !!r.ok; if (!r.ok) db.audit('system', 'notify.push.failed', 'order', o.number, { status: r.status }); }
    catch (err) { db.audit('system', 'notify.push.failed', 'order', o.number, { error: String(err.message || err).slice(0, 160) }); }
  }
  return out;
}
