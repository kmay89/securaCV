// The nudge: a new order reaches the operator's phone the minute the webhook lands. By mail to
// OPERATOR_EMAIL through the outbox (no new connection), and, if NTFY_URL is set, one POST to that
// ntfy topic — the operator's own choice of a push channel, off by default, documented in SECURITY.md.
// The nudge carries the order number, the money, the lines and the ship day; never the address.
import { enqueue } from './mail.mjs';
import { timedFetch } from '../net.mjs';

const usd = (c) => `$${(c / 100).toFixed(2)}`;
// Where the push goes: the environment's topic if set, else the one saved in Settings; never in staging or the demo.
export const ntfyUrlFor = (db, cfg) => (cfg.demo || cfg.env === 'staging') ? '' : (cfg.ntfyUrl || db.setting('notify:ntfy_url') || '');
export const ntfyTokenFor = (db, cfg) => cfg.ntfyToken || db.setting('notify:ntfy_token') || '';
export const newOrderOn = (db, cfg) => cfg.notifyNewOrder === true ? true : cfg.notifyNewOrder === false ? false : (db.setting('notify:new_order') ?? '1') !== '0';
export const pushConfigured = (db, cfg) => /^https:\/\//.test(ntfyUrlFor(db, cfg));
export function savePhone(db, { ntfy_url = null, ntfy_token = null, new_order = null } = {}, { actor = 'web' } = {}) {
  if (ntfy_url != null) { const u = String(ntfy_url).trim(); if (u && !/^https:\/\/[a-z0-9.-]+(:\d+)?\/[A-Za-z0-9_.-]+$/i.test(u)) throw new Error('the topic is an https URL like https://ntfy.sh/your-topic'); db.setSetting('notify:ntfy_url', u); }
  if (ntfy_token != null && String(ntfy_token).trim() !== '') db.setSetting('notify:ntfy_token', String(ntfy_token).trim());
  if (new_order != null) db.setSetting('notify:new_order', new_order ? '1' : '0');
  db.audit(actor, 'notify.settings', 'settings', 'phone', { topic: !!ntfyUrlFor(db, cfg0), new_order: (db.setting('notify:new_order') ?? '1') !== '0' });
}
const cfg0 = {};
export async function push(db, cfg, { title, body, tags = 'package' }, { fetchImpl = timedFetch } = {}) {
  const url = ntfyUrlFor(db, cfg); if (!/^https:\/\//.test(url)) return { ok: false, reason: 'no topic' };
  const token = ntfyTokenFor(db, cfg);
  try { const r = await fetchImpl(url, { method: 'POST', headers: { title, tags, ...(token ? { authorization: `Bearer ${token}` } : {}) }, body }); return { ok: !!r.ok, status: r.status }; }
  catch (err) { return { ok: false, reason: String(err.message || err).slice(0, 160) }; }
}
export const testPush = (db, cfg, { fetchImpl = timedFetch } = {}) => push(db, cfg, { title: `${cfg.company || 'The backoffice'}: test push`, body: 'If you can read this, a new order will reach this phone the minute it lands.', tags: 'white_check_mark' }, { fetchImpl });
export function orderNudgeText(o, cfg, { promise = null } = {}) {
  const lines = (o.items || []).map(i => `${i.qty}× ${i.sku}`).join(', ') || 'no line items';
  const ships = promise?.ship_on ? ` · ships ${promise.ship_on}` : '';
  return { title: `New order ${o.number} · ${usd(o.total_cents)}`, body: `${lines}${ships}${o.ship_state ? ` · to ${o.ship_state}` : ''}${o.discount_code ? ` · code ${o.discount_code}` : ''}\n${cfg.baseUrl}/orders/${o.id}` };
}
export async function newOrderNudge(db, cfg, o, { fetchImpl = timedFetch, promise = null } = {}) {
  const out = { mail: false, push: false };
  if (!o || !newOrderOn(db, cfg)) return out;
  const t = orderNudgeText(o, cfg, { promise });
  if (cfg.operatorEmail) { enqueue(db, { to: cfg.operatorEmail, subject: t.title, kind: 'transactional', ref: `operator:order:${o.number}`, text: `${t.body}\n\n— the backoffice` }); out.mail = true; }
  if (pushConfigured(db, cfg)) { const r = await push(db, cfg, { title: t.title, body: t.body }, { fetchImpl }); out.push = r.ok; if (!r.ok) db.audit('system', 'notify.push.failed', 'order', o.number, { status: r.status, error: r.reason }); }
  return out;
}
