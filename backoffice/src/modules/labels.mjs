// The label, bought from the ship bench: one button that asks Shippo for a USPS label for the box the
// order was packed in, writes the tracking number and the postage back onto the order, ships it (the
// customer gets their mail) and hands back the PDF to print. Nothing is typed.
//
// Shippo is the same rail the website's fulfill.mjs uses (its API Starter plan: 30 labels a month free,
// then 7¢ each, commercial USPS rates, no monthly fee). The request it builds is the one that script
// builds; the only new thing is that the bench presses the button. In demo mode nothing leaves the
// machine: a stand-in label with a synthetic tracking number, printable from /orders/:id/label.
import { orderById, transition, addressIssues, packPlanOf } from './orders.mjs';
import { packOrder, weightCheck } from './packing.mjs';
import { timedFetch } from '../net.mjs';

// shipping.json names a service in words; Shippo wants its token.
export const SERVICE_TOKENS = { 'usps ground advantage': 'usps_ground_advantage', 'ground advantage': 'usps_ground_advantage', 'usps priority': 'usps_priority', 'usps priority mail': 'usps_priority', 'priority mail': 'usps_priority', 'usps priority mail express': 'usps_priority_express', 'usps first class': 'usps_first' };
export const serviceToken = (name) => SERVICE_TOKENS[String(name || '').toLowerCase().trim()] || 'usps_ground_advantage';

// Labels can be bought when a Shippo token is set (or in the demo, where they are pretend).
export const labelsConfigured = (cfg) => !!(cfg?.demo || cfg?.shippoToken);
// The return address every label carries: SHIP_FROM_* in the environment. A blank one blocks every purchase.
export function shipFromIssues(cfg) {
  const f = cfg?.shipFrom || {}; const out = [];
  for (const k of ['name', 'line1', 'city', 'state', 'zip']) if (!String(f[k] || '').trim()) out.push(`SHIP_FROM_${k.toUpperCase()} is empty`);
  if (f.zip && !/^\d{5}(-\d{4})?$/.test(String(f.zip).trim())) out.push('SHIP_FROM_ZIP is not a US ZIP');
  return out;
}

const shippoAddress = (a) => ({ name: a.name || '', company: a.company || '', street1: a.line1 || '', street2: a.line2 || '', city: a.city || '', state: a.state || '', zip: a.zip || '', country: a.country || 'US', phone: a.phone || '', email: a.email || '' });
// The parcel is the box the order was packed in: its outer dimensions when shipping.json gives them, else the
// inner ones plus half an inch of cardboard; its weight the scale's reading when there is one, else the plan's.
export function parcelFor(box, weightOz) {
  const d = box?.outer || (box?.inner ? { l: box.inner.l + 0.5, w: box.inner.w + 0.5, h: box.inner.h + 0.5 } : { l: 9, w: 6, h: 4 });
  return { length: String(d.l), width: String(d.w), height: String(d.h), distance_unit: 'in', weight: String(Math.max(1, Math.round((weightOz || 1) * 100) / 100)), mass_unit: 'oz' };
}
// The exact object posted to Shippo. Pure, so a test can hold it.
export function shipmentFor(o, plan, cfg, { measuredOz = null, shipping = null } = {}) {
  const b = plan?.boxes?.[0] || null; const boxDef = shipping?.boxes?.find?.(x => x.id === b?.box) || null;
  const weight = measuredOz != null && Number(measuredOz) > 0 ? Number(measuredOz) : (plan?.total_oz ?? b?.weight_oz ?? 1);
  return {
    address_from: shippoAddress({ ...cfg.shipFrom, email: cfg.shipFrom?.email || cfg.supportAddr || '' }),
    address_to: shippoAddress({ name: o.ship_name, line1: o.ship_line1, line2: o.ship_line2, city: o.ship_city, state: o.ship_state, zip: o.ship_zip, country: o.ship_country || 'US', email: o.email || '' }),
    parcels: [parcelFor(boxDef ? { inner: boxDef.inner, outer: boxDef.outer } : b ? { inner: b.inner } : null, weight)],
    async: false,
    extra: { reference_1: o.number, reference_2: (o.items || []).map(i => `${i.qty}x ${i.sku}`).join(', ').slice(0, 50) },
    service: serviceToken(b?.service || cfg.defaultService || 'usps_ground_advantage'),
  };
}

// Shippo: create the shipment, pick the rate for the service, buy. Returns { tracking, label_url, amount_cents, service }.
export async function shippoBuy(shipment, token, { fetchImpl = timedFetch, base = 'https://api.goshippo.com' } = {}) {
  const H = { authorization: `ShippoToken ${token}`, 'content-type': 'application/json' };
  const { service, ...body } = shipment;
  const sRes = await fetchImpl(`${base}/shipments/`, { method: 'POST', headers: H, body: JSON.stringify(body) });
  if (!sRes.ok) throw new Error(`Shippo refused the shipment (HTTP ${sRes.status})`);
  const s = await sRes.json();
  const bad = (s.messages || []).filter(m => /error/i.test(m.source || '') || /invalid|not found|unable/i.test(m.text || ''));
  const rate = (s.rates || []).find(r => r.servicelevel?.token === service) || (s.rates || [])[0];
  if (!rate) throw new Error(`Shippo returned no rate for this shipment${bad.length ? `: ${bad.map(m => m.text).join('; ')}` : ''}`);
  const tRes = await fetchImpl(`${base}/transactions/`, { method: 'POST', headers: H, body: JSON.stringify({ rate: rate.object_id, label_file_type: 'PDF_4x6', async: false }) });
  if (!tRes.ok) throw new Error(`Shippo refused the label (HTTP ${tRes.status})`);
  const t = await tRes.json();
  if (t.status !== 'SUCCESS') throw new Error(`Shippo label ${t.status || 'failed'}: ${(t.messages || []).map(m => m.text).join('; ') || 'no reason given'}`);
  return { tracking: t.tracking_number, label_url: t.label_url, amount_cents: Math.round(Number(rate.amount || 0) * 100), service: rate.servicelevel?.name || service, provider: 'shippo' };
}

// The demo's stand-in: a synthetic tracking number and a label this server draws itself. Costs nothing, goes nowhere.
export function demoBuy(o, plan) {
  const digits = String(9400_1000_0000_0000_0000_00n + BigInt(o.id * 7919)).padStart(22, '0');
  return { tracking: digits, label_url: `/orders/${o.id}/label`, amount_cents: Math.round((plan?.total_cost_est ?? 6.75) * 100), service: 'USPS Ground Advantage (demo)', provider: 'demo' };
}

// The button: refuse anything that would make a wrong label, buy, write it down, ship.
export async function buyLabel(db, cfg, orderId, { fetchImpl = timedFetch, measuredOz = null, shipAnyway = false, shipping = null, actor = 'web', now = null } = {}) {
  if (!labelsConfigured(cfg)) throw new Error('no label provider: set SHIPPO_API_TOKEN');
  const o = orderById(db, orderId); if (!o) throw new Error('no such order');
  if (o.status !== 'packed') throw new Error(`${o.number} is ${o.status} — a label is for a packed order`);
  if (o.label_url && o.tracking) throw new Error(`${o.number} already has a label (${o.tracking})`);
  const issues = addressIssues({ line1: o.ship_line1, city: o.ship_city, state: o.ship_state, zip: o.ship_zip, country: o.ship_country }); if (issues.length) throw new Error(`the address will not make a label: ${issues.join('; ')}`);
  if (!cfg.demo) { const from = shipFromIssues(cfg); if (from.length) throw new Error(`the return address is incomplete: ${from.join(', ')}`); }
  const plan = packPlanOf(o) || (shipping ? packOrder(shipping, o.items) : null);
  // The scale's verdict comes before the purchase: a box that weighs wrong is recounted, not posted.
  const scale = weightCheck(packPlanOf(o), measuredOz, { tolerance: cfg?.weightToleranceOz ?? 1.5 });
  if (scale.checked && !scale.ok && !shipAnyway) throw new Error(`the scale reads ${scale.measured} oz, the plan expects ${scale.expected} oz (${scale.delta > 0 ? '+' : ''}${scale.delta} oz, over the ${cfg?.weightToleranceOz ?? 1.5} oz tolerance) — recount the box, or tick "ship anyway"; no label was bought`);
  const shipment = shipmentFor(o, plan, cfg, { measuredOz, shipping });
  const bought = cfg.demo ? demoBuy(o, plan) : await shippoBuy(shipment, cfg.shippoToken, { fetchImpl });
  db.run('UPDATE orders SET label_url = ? WHERE id = ?', bought.label_url, o.id);
  db.audit(actor, 'order.label', 'order', o.number, { provider: bought.provider, service: bought.service, amount_cents: bought.amount_cents, tracking: bought.tracking });
  const shipped = transition(db, o.id, 'shipped', { actor, carrier: 'USPS', tracking: bought.tracking, cfg, at: now, measuredOz, postageCents: bought.amount_cents, shipAnyway });
  return { order: shipped, ...bought, shipment };
}

// A label this server draws: the demo's, and a human-readable fallback for any shipped order.
export function labelHtml(o, cfg) {
  const e = (s) => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const from = cfg.shipFrom || {}; const demo = cfg.demo || !cfg.shippoToken;
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Label ${e(o.number)}</title><style>@page{size:4in 6in;margin:0}body{margin:0;font:13px/1.35 system-ui,sans-serif;color:#000;background:#fff}.l{width:4in;height:6in;box-sizing:border-box;padding:.25in;border:1px dashed #999;position:relative}.from{font-size:10px;color:#333}.to{margin-top:.35in;font-size:17px;line-height:1.3}.to b{font-size:19px}.svc{position:absolute;top:.25in;right:.25in;border:2px solid #000;padding:.05in .12in;font-weight:700;font-size:12px}.trk{position:absolute;bottom:.4in;left:.25in;right:.25in;text-align:center}.trk .bars{height:.55in;background:repeating-linear-gradient(90deg,#000 0 2px,#fff 2px 4px,#000 4px 5px,#fff 5px 8px);margin-bottom:.06in}.trk .n{font:600 13px/1.2 ui-monospace,monospace;letter-spacing:.08em}.ref{position:absolute;bottom:.12in;left:.25in;font-size:10px;color:#333}${demo ? '.demo{position:absolute;top:1.9in;left:0;right:0;text-align:center;font-size:26px;font-weight:800;color:#c33;opacity:.35;transform:rotate(-15deg)}' : ''}@media print{.l{border:0}}</style></head>
<body><div class="l"><div class="from">${e(from.name || cfg.company || '')}${from.company ? `<br>${e(from.company)}` : ''}<br>${e(from.line1 || '')}${from.line2 ? `<br>${e(from.line2)}` : ''}<br>${e(from.city || '')}${from.city ? ',' : ''} ${e(from.state || '')} ${e(from.zip || '')}</div><div class="svc">USPS GROUND ADVANTAGE</div>
<div class="to"><b>${e(o.ship_name)}</b><br>${e(o.ship_line1)}${o.ship_line2 ? `<br>${e(o.ship_line2)}` : ''}<br>${e(o.ship_city)}, ${e(o.ship_state)} ${e(o.ship_zip)}</div>
${demo ? '<div class="demo">DEMO — NOT POSTAGE</div>' : ''}
<div class="trk"><div class="bars"></div><div class="n">${e(String(o.tracking || '').replace(/(\d{4})(?=\d)/g, '$1 '))}</div></div><div class="ref">${e(o.number)} · ${e((o.items || []).map(i => `${i.qty}× ${i.sku}`).join(', '))}</div></div></body></html>`;
}
