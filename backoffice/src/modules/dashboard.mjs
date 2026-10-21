// The private Engine Room: the numbers a company of one actually steers by.
// Not visitor analytics — the site has none and never will (store-README
// "No tracking"). These are orders, money, queue depth, stock and uptime.
import { orderStats } from './orders.mjs';
import { supportStats } from './support.mjs';
import { warrantyStats } from './warranty.mjs';
import { lowStock, inventoryValueCents } from './inventory.mjs';
import { waitlistSummary } from './contacts.mjs';
import { outboxStats } from './mail.mjs';
import { monthly, unreconciledStripeCents } from './ledger.mjs';
import { receivableCents } from './invoices.mjs';
import { openExceptions } from './exceptions.mjs';
import { listProbes } from './status.mjs';
import { mrrCents } from './subscriptions.mjs';
import { deviceStats } from './devices.mjs';

export function snapshot(db) {
  const months = monthly(db, 3);
  return {
    orders: orderStats(db), support: supportStats(db), warranty: warrantyStats(db),
    inventory: { low: lowStock(db), value_cents: inventoryValueCents(db) },
    waitlist: waitlistSummary(db), mail: outboxStats(db),
    books: { months, stripe_unpaid_out_cents: unreconciledStripeCents(db), receivable_cents: receivableCents(db), mrr_cents: mrrCents(db) },
    exceptions: openExceptions(db), probes: listProbes(db), devices: deviceStats(db),
    contacts: db.get('SELECT COUNT(*) AS n, SUM(consent_marketing) AS consented FROM contacts'),
  };
}
