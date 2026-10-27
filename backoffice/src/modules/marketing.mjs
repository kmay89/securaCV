// Marketing, in one view and without a visitor number: the funnel from a
// signup to a repeat buyer, where people come from, what every campaign and
// automation did in the week after, the codes, and where the customers are.
import { checkoutStats } from './recovery.mjs';
import { audienceStats } from './contacts.mjs';
import { listCampaigns, campaignReport } from './campaigns.mjs';
import { listAutomations, automationReport } from './automations.mjs';
import { listDiscounts } from './discounts.mjs';

export function marketing(db, { days = 30, now = Date.now() } = {}) {
  const since = new Date(now - days * 86400000).toISOString();
  const bought = "id IN (SELECT contact_id FROM orders WHERE status != 'canceled' AND total_cents > 0)";
  const funnel = {
    contacts: db.get('SELECT COUNT(*) AS n FROM contacts WHERE forgotten_at IS NULL').n,
    opted_in: db.get('SELECT COUNT(*) AS n FROM contacts WHERE consent_marketing = 1 AND unsubscribed_at IS NULL AND bounced_at IS NULL').n,
    customers: db.get(`SELECT COUNT(*) AS n FROM contacts WHERE ${bought}`).n,
    repeat: db.get("SELECT COUNT(*) AS n FROM (SELECT contact_id FROM orders WHERE status != 'canceled' AND total_cents > 0 GROUP BY contact_id HAVING COUNT(*) >= 2)").n,
    period: { contacts: db.get('SELECT COUNT(*) AS n FROM contacts WHERE created_at >= ?', since).n, opted_in: db.get('SELECT COUNT(*) AS n FROM contacts WHERE consent_at >= ?', since).n, first_orders: db.get("SELECT COUNT(*) AS n FROM (SELECT contact_id, MIN(paid_at) AS f FROM orders WHERE status != 'canceled' AND total_cents > 0 GROUP BY contact_id HAVING f >= ?)", since).n, repeat_orders: db.get("SELECT COUNT(*) AS n FROM orders o WHERE o.paid_at >= ? AND o.status != 'canceled' AND o.total_cents > 0 AND EXISTS (SELECT 1 FROM orders p WHERE p.contact_id = o.contact_id AND p.paid_at < o.paid_at AND p.status != 'canceled' AND p.total_cents > 0)", since).n },
  };
  const sources = db.all("SELECT COALESCE(NULLIF(source, ''), 'unknown') AS source, COUNT(*) AS contacts, SUM(consent_marketing = 1) AS opted_in, SUM(" + bought + ") AS customers FROM contacts WHERE forgotten_at IS NULL GROUP BY 1 ORDER BY contacts DESC");
  const campaigns = listCampaigns(db).filter(c => c.status === 'sent').map(c => ({ ...c, report: campaignReport(db, c.slug) }));
  const automations = listAutomations(db).map(a => ({ ...a, report: automationReport(db, a.slug) }));
  const codes = listDiscounts(db).filter(d => d.uses > 0);
  const byState = {}; const salesByState = {}; const ordersByState = {};
  for (const r of db.all("SELECT ship_state AS st, COUNT(DISTINCT contact_id) AS customers, COUNT(*) AS orders, SUM(total_cents - refunded_cents) AS sales FROM orders WHERE status != 'canceled' AND total_cents > 0 AND ship_state != '' GROUP BY ship_state")) { byState[r.st] = r.customers; salesByState[r.st] = r.sales; ordersByState[r.st] = r.orders; }
  const cities = db.all("SELECT ship_city || ', ' || ship_state AS place, COUNT(DISTINCT contact_id) AS customers, SUM(total_cents - refunded_cents) AS sales FROM orders WHERE status != 'canceled' AND total_cents > 0 AND ship_city != '' GROUP BY 1 ORDER BY customers DESC, sales DESC LIMIT 8");
  const attributed = campaigns.reduce((s, c) => s + c.report.revenue_cents, 0) + automations.reduce((s, a) => s + a.report.revenue_cents, 0);
  return { days, funnel, sources, audience: audienceStats(db, { days }), campaigns, automations, codes, byState, salesByState, ordersByState, cities, attributed_cents: attributed, checkout: checkoutStats(db, { days, now }) };
}
