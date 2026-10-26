// Reconciliation against Stripe, daily, with an API key: every paid payout
// Stripe knows about is in the ledger (posted automatically when missing —
// it is Stripe's own number), and every successful charge of the last week
// has an order here. A gap is an exception with the ids, never a silent
// drift. Without a key the job says so and does nothing; the demo never
// has one.
import { postPayout } from './ledger.mjs';
import { raise, resolve } from './exceptions.mjs';
import { timedFetch } from '../net.mjs';

export async function stripeList(cfg, path, { fetchImpl = timedFetch } = {}) {
  const out = []; let starting_after = null;
  for (let page = 0; page < 10; page++) {
    const r = await fetchImpl(`https://api.stripe.com/v1/${path}${path.includes('?') ? '&' : '?'}limit=100${starting_after ? `&starting_after=${starting_after}` : ''}`, { headers: { authorization: `Bearer ${cfg.stripeApiKey}` } });
    if (!r.ok) throw new Error(`stripe ${path.split('?')[0]} ${r.status}`);
    const body = await r.json(); out.push(...(body.data || []));
    if (!body.has_more || !body.data?.length) break; starting_after = body.data.at(-1).id;
  }
  return out;
}

// The fee Stripe kept out of a payout: the sum over the balance transactions it settled (the payout's own
// transaction excluded). Needs an API key; without one the caller posts the net and the fee is 0.
export async function payoutFee(cfg, payoutId, { fetchImpl = timedFetch } = {}) {
  if (!cfg.stripeApiKey) return null;
  const txns = await stripeList(cfg, `balance_transactions?payout=${encodeURIComponent(payoutId)}`, { fetchImpl });
  return { fee_cents: txns.filter(t => t.type !== 'payout').reduce((s, t) => s + (t.fee || 0), 0), transactions: txns.length };
}

export async function reconcile(db, cfg, { fetchImpl = timedFetch, now = Date.now(), days = 7 } = {}) {
  if (!cfg.stripeApiKey) return { skipped: 'STRIPE_API_KEY not set — reconciliation needs read access to payouts and charges' };
  const since = Math.floor((now - 30 * 86400000) / 1000);
  // payouts: Stripe's paid payouts of the last 30 days, each in the books exactly once
  const payouts = await stripeList(cfg, `payouts?status=paid&arrival_date[gte]=${since}`, { fetchImpl });
  let posted = 0;
  for (const p of payouts) {
    if (db.get('SELECT id FROM ledger WHERE ref = ?', `payout:${p.id}`)) continue;
    // p.amount is the net that reached the bank; the fees Stripe kept sit on the charges' balance transactions inside this payout
    const { fee_cents: fee, transactions } = await payoutFee(cfg, p.id, { fetchImpl });
    postPayout(db, { id: p.id, date: new Date(p.arrival_date * 1000).toISOString().slice(0, 10), amount_cents: p.amount, fee_cents: fee });
    db.audit('reconcile', 'payout.posted', 'payout', p.id, { amount_cents: p.amount, fee_cents: fee, transactions }); posted++;
  }
  // charges: every successful charge of the last `days` has an order with its PaymentIntent
  // a charge on an invoice is a subscription's (recorded by invoice.paid), not a shop order; it never has one
  const charges = (await stripeList(cfg, `charges?created[gte]=${Math.floor((now - days * 86400000) / 1000)}`, { fetchImpl })).filter(c => c.status === 'succeeded' && c.paid && !c.refunded && !c.invoice);
  const missing = charges.filter(c => c.payment_intent && !db.get('SELECT id FROM orders WHERE payment_intent = ? OR external_id = ?', c.payment_intent, c.payment_intent)).map(c => ({ payment_intent: c.payment_intent, amount_cents: c.amount, email: c.billing_details?.email || c.receipt_email || null, created: new Date(c.created * 1000).toISOString() }));
  if (missing.length) await raise(db, cfg, 'reconcile:orders', `reconcile [orders missing]: ${missing.length} Stripe charge${missing.length === 1 ? '' : 's'} without an order`, `These charges succeeded in Stripe in the last ${days} days but no order carries their PaymentIntent. The webhook may have been down; import them (Orders → Import a Stripe CSV) or replay the events from the Stripe dashboard.\n\n${missing.map(m => `${m.payment_intent}  ${(m.amount_cents / 100).toFixed(2)} USD  ${m.email || ''}  ${m.created}`).join('\n')}`, { fetchImpl });
  else await resolve(db, cfg, 'reconcile:orders', 'Every charge has its order.', { fetchImpl });
  const result = { at: new Date(now).toISOString(), payouts_seen: payouts.length, payouts_posted: posted, charges_seen: charges.length, orders_missing: missing.length, missing };
  db.setSetting('reconcile:last', JSON.stringify(result));
  return result;
}
export const lastReconcile = (db) => { try { return JSON.parse(db.setting('reconcile:last') || 'null'); } catch { return null; } };
