#!/usr/bin/env node
// The command line is the same company as the web admin — same SQLite file,
// same functions — so an operator (or an AI operator) can run the whole
// business from a terminal. `backoffice help` lists everything.
import { readFileSync } from 'node:fs';
import { loadConfig } from '../src/config.mjs';
import { openDb } from '../src/db.mjs';
import { setupOperator, createApiToken, revokeApiToken, totpUri, hasOperator, listUsers, inviteUser, userByEmail, setRole, setStatus, removeUser, ROLES, ROLE_TEXT } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';
import { startScheduler, runJob, JOBS } from '../src/jobs.mjs';
import { loadCatalog } from '../src/modules/catalog.mjs';
import { recordOrder, transition, listOrders, orderByNumber, fulfillCsv, timeline, addNote, setTags, batches } from '../src/modules/orders.mjs';
import { listCustomers, customerStats, contactById, setContactTags } from '../src/modules/contacts.mjs';
import { listDiscounts, createDiscount, setDiscountStatus, evaluate as evaluateDiscount, describe as describeDiscount } from '../src/modules/discounts.mjs';
import { analytics } from '../src/modules/analytics.mjs';
import { listProducts } from '../src/modules/products.mjs';
import { search } from '../src/modules/search.mjs';
import { parseCsv, detectColumns, csvRowToOrder, signStripePayload } from '../src/modules/stripe.mjs';
import { listTickets, ticketByNumber, reply, queue } from '../src/modules/support.mjs';
import { upsertItem, move, setRecipe, listItems, buildable } from '../src/modules/inventory.mjs';
import { registerDevice, importManifest, listDevices } from '../src/modules/devices.mjs';
import { openClaim, ANSWERS, listClaims } from '../src/modules/warranty.mjs';
import { profitAndLoss, trialBalance, monthly, postExpense, postPayout, salesTaxByState, ledgerCsv, ACCOUNTS } from '../src/modules/ledger.mjs';
import { upsertProbe, DEFAULT_PROBES, runDueProbes, publicStatus } from '../src/modules/status.mjs';
import { backup, listBackups, checkBackup, liveCounts, restore, restoreDrill, uploadOffsite, offsiteConfigured } from '../src/modules/backup.mjs';
import { doctor, digestText } from '../src/modules/health.mjs';
import { reconcile } from '../src/modules/reconcile.mjs';
import { snapshot } from '../src/modules/dashboard.mjs';
import { notifyWaitlist, createCampaign, queueCampaign } from '../src/modules/campaigns.mjs';
import { sendOutbox } from '../src/modules/mail.mjs';
import { openExceptions } from '../src/modules/exceptions.mjs';
import { seedDemo, resetDemo, isEmpty, isDemo, ensureDemo, DEMO_PASSWORD, DEMO_TOTP_SECRET } from '../src/modules/demo.mjs';
import { totp } from '../src/auth.mjs';
import { listPos, poByNumber, createPo, markOrdered, receivePo, cancelPo, draftReorder, buildableAhead } from '../src/modules/purchasing.mjs';
import { recallQuery, batches as deviceBatches, createRecall, sendRecall, listRecalls, DEFAULT_BODY } from '../src/modules/recall.mjs';
import { forgetContact, exportContact } from '../src/modules/privacy.mjs';
import { suggestions } from '../src/modules/suggestions.mjs';
import { marketing } from '../src/modules/marketing.mjs';
import { pollTracking } from '../src/modules/shipping.mjs';
import { editItems } from '../src/modules/orders.mjs';
import { remindInvoice } from '../src/modules/invoices.mjs';
import { orderStatusUrl } from '../src/modules/delivery.mjs';
import { join } from 'node:path';

const [cmd = 'help', ...rest] = process.argv.slice(2);
const flags = {}; const args = [];
for (let i = 0; i < rest.length; i++) { const a = rest[i]; if (a.startsWith('--')) { const [k, v] = a.slice(2).split('='); flags[k] = v ?? (rest[i + 1] && !rest[i + 1].startsWith('--') ? rest[++i] : true); } else args.push(a); }
const out = (x) => console.log(typeof x === 'string' ? x : JSON.stringify(x, null, 2));
const cfg = loadConfig();
const db = openDb(cfg.dbPath);
const app = createApp({ db, cfg });

const HELP = `backoffice — the company in one SQLite file (${cfg.dbPath})

  setup [--email you@… --name …]           the first owner: password + authenticator (first run; --force resets that owner)
  users | users invite <email> --name … --role owner|ops|support|viewer | users role|disable|enable|remove <email> | users roles
  demo [--reset|--if-empty]                fill an EMPTY database with a synthetic week to explore (--reset wipes a DEMO db and reseeds; --if-empty is a no-op on a used one)
  code                                     print the current authenticator code (same as your phone shows; for local trying-out)
  serve                                    run the web admin, API, forms, webhook and the scheduler
  token create <name> | token revoke <name>  API bearer tokens for the CLI/remote/an AI operator
  snapshot                                 the dashboard numbers as JSON
  doctor                                   the health checklist (exit 1 when a check fails) — same list as /health and the digest
  backup [--no-offsite]                    a checked copy (opened, integrity, books balanced), then off-site when BACKUP_S3_* is set
  backup check <file> [--live]            prove a copy boots; --live also compares row counts with the running database
  backup drill                             pull the latest copy back (off-site when configured) and prove it
  restore <file> [--to <db path>]          put a checked copy in place (stop serve first; the old file is kept beside it)
  reconcile                                Stripe payouts → the books, charges → orders (needs STRIPE_API_KEY)
  digest                                   print the morning digest (what the job emails to OPERATOR_EMAIL)
  job <name> | job list                    run one scheduled job now (outbox inbox probes sla stock invoices books backup)
  backup | backups                         take a backup now / list them

orders
  orders [--status paid] [--batch 2026-W43] [--q text]
  order <EL-000123>
  orders import <stripe-export.csv>        catch-up from the Stripe Payments export
  orders fulfill-csv [--batch W] > orders.csv   the CSV the website's fulfill.mjs eats
  order pack|ship|deliver <EL-…> [--carrier USPS --tracking 9400…]
  order new --email a@b.c --name "…" --sku wap-kit [--qty 1] --line1 … --city … --state XX --zip 12345 [--cents 6900] [--discount CODE] [--tags a,b]
  order timeline <number> | order note <number> <text> | order tag <number> <a,b>
  order items <number> sku=qty sku=qty         change the lines before packing (the charge stays)
  order status-url <number>                the customer's signed status page link
  track                                    ask the carrier about every open parcel now (USPS_CLIENT_ID/SECRET)
  batches                                  the weekly batches: what each holds, what is still to pack or ship
  customers [--segment repeat|consented|waitlist|orgs|never] [--tag …] [--sort spent|orders|name] [--q …]
  customer <email|id> [tag <a,b>]          lifetime orders, spend, tickets, claims
  forget <email|id>                        the right to erasure: blank the person, keep the books (logged)
  export <email|id> > their-data.json      the right to a copy: everything we hold, as JSON
  invoice remind <INV-…>                   queue a payment reminder
  products [--days 30]                     the catalog joined to stock, sales and the waitlist
  discounts | discounts create CODE --kind percent|fixed|free_shipping --value N [--min cents] [--max-uses N] [--starts d] [--ends d] [--note …]
  discounts try CODE --subtotal cents | discounts disable|enable CODE
  analytics [--days 7|30|90|365]           sales, orders, margin, returning rate, top products, the series
  search <text>                            orders, customers, tickets, serials, invoices, claims, codes by number or name
  marketing [--days 30]                    the funnel, sources, what every send did, sales by state
  suggestions                              the Decide queue: what the system would do next, and why (approve it in the admin or via the API)

support
  tickets [--status open] [--kind support]   |  ticket <T-000045>  |  queue (what needs an answer, oldest first)
  ticket reply <T-…> "text" [--close]

inventory & devices
  inv | inv set <sku> --name … [--kind part|printed|finished|packaging] [--reorder 10 --reorder-qty 50 --cost-cents 450 --supplier …]
  inv move <sku> <±n> [--reason receive|adjust|scrap] [--ref …]   |   inv recipe <finished-sku> part=qty part=qty
  inv buildable <sku>                      now, and after what is on order lands
  po | po <PO-000001> | po draft           purchase orders; draft = one per supplier from the reorder points
  po new --supplier … [--expected YYYY-MM-DD] sku=qty[@cents] …  |  po order <PO-…> [--expected d]  |  po receive <PO-…> [sku=qty …]  |  po cancel <PO-…>
  recall [--batch B] [--fw 2.4.1] [--sku s]  who has which board; recall new --title … [--file body.txt] <same filters>; recall send <RC-…>
  devices [--q …]  |  device add <serial> [--mac … --board … --sku … --fw 2.5.0 --sha …]  |  devices import <manifest.json> [--batch W --sku …]

warranty
  warranty | warranty open --order EL-… --category ${Object.keys(ANSWERS).join('|')} ["what happened"]

books
  pnl [--from 2026-01-01 --to 2026-12-31] | pnl months | trial-balance | sales-tax --from … --to …
  expense --account expenses:supplies --cents 1234 --memo "filament" [--date YYYY-MM-DD] [--from assets:bank]
  payout --id po_… --cents 12345 [--fee-cents 390] [--date …]
  books csv [--from --to] > ledger.csv

mail
  outbox                                   send what's queued (the scheduler does this every 30 s)
  waitlist notify <sku> [--name "Canary WAP Kit"] [--url …]
  campaign new <slug> --subject … --file body.txt [--segment all|customers|waitlist:<sku>|kind:<kind>]  |  campaign send <slug>

status
  probes seed | probes run | status

dev
  stripe sign <file.json> [--secret …]     a Stripe-Signature header for a test post
`;

const main = async () => {
  switch (cmd) {
    case 'help': return out(HELP);
    case 'setup': {
      if (hasOperator(db) && !flags.force) return out('an owner exists; pass --force to reset that owner\'s password and authenticator, or invite people with `backoffice users invite`');
      const pw = flags.password || await ask('New owner password: ', true);
      if (String(pw).length < 12) throw new Error('use at least 12 characters');
      const secret = setupOperator(db, pw, { email: flags.email || cfg.supportAddr || 'owner@localhost', name: flags.name || 'Owner' });
      for (const p of DEFAULT_PROBES(cfg)) upsertProbe(db, p);
      return out(`Operator set. Add this to your authenticator app (TOTP):\n\n  ${totpUri(secret, cfg.supportAddr || 'operator', `${cfg.company} backoffice`)}\n\n(secret: ${secret})\n\nDefault status probes seeded. Now: backoffice serve`);
    }
    case 'demo': {
      if (flags['if-empty'] && (!isEmpty(db) || hasOperator(db))) return out(isDemo(db) ? 'demo database already seeded; serving it as is' : 'database is not empty and not a demo — leaving it alone');
      out(flags.reset ? await resetDemo(db, cfg) : await seedDemo(db, cfg));
      return out(`Demo data loaded (synthetic; nothing leaves this machine). Now: backoffice serve  → http://127.0.0.1:${cfg.port}/  — the login page shows the password and the current code.`);
    }
    case 'code': {
      if (isDemo(db)) return out(totp(DEMO_TOTP_SECRET));
      const owners = flags.email ? db.all('SELECT email, totp_secret FROM users WHERE email = ? AND status = \'active\' AND totp_secret IS NOT NULL', String(flags.email).toLowerCase()) : db.all("SELECT email, totp_secret FROM users WHERE role = 'owner' AND status = 'active' AND totp_secret IS NOT NULL ORDER BY id");
      if (!owners.length) throw new Error(flags.email ? `${flags.email} is not an active person with an authenticator` : 'no active owner yet — run setup or demo');
      if (owners.length > 1) throw new Error(`more than one active owner — say which: backoffice code --email <one of ${owners.map(o => o.email).join(', ')}>`);
      return out(totp(owners[0].totp_secret));
    }
    case 'users': {
      const sub = args[0];
      if (!sub) return out(listUsers(db).map(u => `${u.email.padEnd(40)} ${u.role.padEnd(8)} ${u.status.padEnd(9)} ${u.name}${u.last_login_at ? `  last login ${u.last_login_at.slice(0, 16)}` : ''}${u.locked_until && u.locked_until > new Date().toISOString() ? '  LOCKED' : ''}`).join('\n') || 'nobody yet');
      if (sub === 'roles') return out(Object.entries(ROLE_TEXT).map(([r, t]) => `${r.padEnd(8)} ${t}`).join('\n'));
      if (sub === 'invite') { const u = inviteUser(db, { email: args[1], name: flags.name || args[1], role: flags.role || 'ops', actor: 'cli' }); return out(`Invited ${u.email} as ${u.role}. Send them this link (it works once, for 48 hours):\n\n  ${cfg.baseUrl}/invite/${u.invite_token}\n`); }
      const u = userByEmail(db, args[1]); if (!u) throw new Error(`no such person: ${args[1]}`);
      if (sub === 'role') return out(setRole(db, u.id, args[2], { actor: 'cli' }));
      if (sub === 'disable') return out(setStatus(db, u.id, 'disabled', { actor: 'cli' }));
      if (sub === 'enable') return out(setStatus(db, u.id, 'active', { actor: 'cli' }));
      if (sub === 'remove') return out(removeUser(db, u.id, { actor: 'cli' }));
      throw new Error(`users [invite <email> --name … --role ${ROLES.join('|')}] | role <email> <role> | disable|enable|remove <email> | roles`);
    }
    case 'serve': {
      // BACKOFFICE_DEMO=1 on an empty database: seed itself, so a container is the whole demo.
      if (cfg.demo) { const r = await ensureDemo(db, cfg); if (r) console.log('[backoffice] demo mode: seeded', JSON.stringify(r)); }
      if (!hasOperator(db)) console.error('[backoffice] no owner yet — run `backoffice setup` in another shell; serving anyway');
      const addr = await app.listen(); startScheduler(app);
      console.log(`[backoffice] listening on http://${addr.address}:${addr.port}  (public base: ${cfg.baseUrl})`);
      return new Promise(() => {});
    }
    case 'token': return args[0] === 'revoke' ? out(`revoked ${revokeApiToken(db, args[1])}`) : out(`${createApiToken(db, args[1] || 'cli', { scope: flags['read-only'] ? 'ro' : 'rw' })}\n(shown once — store it in the client's environment${flags['read-only'] ? '; read-only: GET only' : ''})`);
    case 'snapshot': return out(snapshot(db));
    case 'job': return args[0] === 'list' ? out(Object.keys(JOBS)) : out(await runJob(app, args[0]));
    case 'backup': {
      if (args[0] === 'check') { const v = checkBackup(args[1], { expect: flags.live ? liveCounts(db) : null }); out(v); if (!v.ok) process.exitCode = 1; return; }
      if (args[0] === 'drill') { const r = await restoreDrill(cfg, { localDir: join(cfg.dataDir, 'backups') }); out(r); if (!r.ok) process.exitCode = 1; return; }
      const b = backup(db, join(cfg.dataDir, 'backups'), { keep: cfg.backupKeep }); const checked = checkBackup(b.path, { expect: liveCounts(db) });
      const offsite = flags.offsite === false ? { skipped: '--no-offsite' } : offsiteConfigured(cfg) ? await uploadOffsite(cfg, b.path) : { skipped: 'BACKUP_S3_URL / KEY / SECRET not set' };
      out({ ...b, checked, offsite }); if (!checked.ok) process.exitCode = 1; return;
    }
    case 'backups': return out(listBackups(join(cfg.dataDir, 'backups')));
    case 'restore': {
      if (!args[0]) throw new Error('restore <backup file> [--to <db path>] — stop `serve` first');
      const r = restore(args[0], flags.to || cfg.dbPath); return out({ ...r, note: 'start `backoffice serve` again; the replaced file is kept beside the database until you delete it' });
    }
    case 'doctor': {
      const h = doctor(db, cfg); const ic = (ok) => ok === true ? 'ok ' : ok === false ? '!! ' : '-- ';
      out(`${h.ok ? 'every check passes' : `${h.failing.length} failing`}\n` + h.checks.map(c => `  ${ic(c.ok)} ${c.label.padEnd(44)} ${c.detail}${c.ok === false && c.fix ? `\n        → ${c.fix}` : ''}`).join('\n'));
      if (!h.ok) process.exitCode = 1; return;
    }
    case 'reconcile': return out(await reconcile(db, cfg));
    case 'digest': return out(digestText(db, cfg));

    case 'orders': {
      if (args[0] === 'import') {
        const rows = parseCsv(readFileSync(args[1], 'utf8')); if (!rows.length) return out('no rows');
        const map = detectColumns(rows[0]); const catalog = await loadCatalog(cfg).catch(() => null);
        const res = { created: 0, existing: 0, skipped: 0, errors: [] };
        for (const r of rows) { const o = csvRowToOrder(r, map); if (o.status_hint && !/paid|succeeded|complete/i.test(o.status_hint)) { res.skipped++; continue; } try { const { created } = recordOrder(db, o, { catalog, actor: 'cli' }); created ? res.created++ : res.existing++; } catch (e) { res.errors.push(`${o.external_id || o.email}: ${e.message}`); } }
        return out(res);
      }
      if (args[0] === 'fulfill-csv') return process.stdout.write(fulfillCsv(db, { batch: flags.batch || '', status: flags.status || 'paid' }));
      return out(listOrders(db, { status: flags.status || '', batch: flags.batch || '', q: flags.q || '' }).map(o => `${o.number}  ${o.status.padEnd(9)} ${String((o.total_cents / 100).toFixed(2)).padStart(8)}  ${o.batch}  ${o.ship_name}  ${o.items_text || ''}`).join('\n') || 'no orders');
    }
    case 'order': {
      if (args[0] === 'items') { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(editItems(db, o.id, args.slice(2).map(a => { const [sku, q] = a.split('='); return { sku, qty: Number(q || 1) }; }), { catalog: await loadCatalog(cfg).catch(() => null), actor: 'cli' })); }
      if (args[0] === 'status-url') { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(orderStatusUrl(cfg, o)); }
      if (args[0] === 'new') {
        const catalog = await loadCatalog(cfg).catch(() => null);
        const { order } = recordOrder(db, { source: 'manual', email: flags.email, name: flags.name, total_cents: Number(flags.cents || 0), subtotal_cents: Number(flags.cents || 0), discount_code: flags.discount || null, tags: flags.tags || '', ship: { line1: flags.line1, line2: flags.line2, city: flags.city, state: flags.state, zip: flags.zip, country: flags.country || 'US' }, items: [{ sku: flags.sku, name: flags.sku, qty: Number(flags.qty || 1), unit_cents: Number(flags.cents || 0) / Number(flags.qty || 1) }] }, { catalog, actor: 'cli' });
        return out(order);
      }
      if (args[0] === 'note') { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(addNote(db, o.id, args.slice(2).join(' '), { actor: 'cli' })); }
      if (args[0] === 'tag') { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(setTags(db, o.id, args[2] || '', { actor: 'cli' }).tags || '(no tags)'); }
      if (args[0] === 'timeline') { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(timeline(db, o.id).map(ev => `${ev.at}  ${ev.actor.padEnd(7)} ${ev.text}`).join('\n')); }
      if (['pack', 'ship', 'deliver'].includes(args[0])) { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(transition(db, o.id, { pack: 'packed', ship: 'shipped', deliver: 'delivered' }[args[0]], { actor: 'cli', carrier: flags.carrier, tracking: flags.tracking, cfg })); }
      return out(orderByNumber(db, args[0]) || 'no such order');
    }
    case 'batches': return out(batches(db).map(b => `${b.batch}  orders ${String(b.orders).padStart(3)}  units ${String(b.units).padStart(3)}  to pack ${b.to_pack}  to ship ${b.to_ship}  shipped ${b.shipped}  ${(b.sales_cents / 100).toFixed(2).padStart(9)}`).join('\n') || 'no orders');
    case 'customers': return out(listCustomers(db, { q: flags.q || '', tag: flags.tag || '', segment: flags.segment || '', sort: flags.sort || 'recent' }).map(c => `${c.email.padEnd(40)} ${String(c.orders).padStart(3)} orders  ${(c.spent_cents / 100).toFixed(2).padStart(9)}  ${c.tags ? `[${c.tags}] ` : ''}${c.name || ''}`).join('\n') || 'no customers');
    case 'customer': {
      const ct = /^\d+$/.test(args[0] || '') ? contactById(db, Number(args[0])) : db.get('SELECT * FROM contacts WHERE email = ?', String(args[0] || '').toLowerCase()); if (!ct) throw new Error('no such customer');
      if (args[1] === 'tag') return out(setContactTags(db, ct.id, args[2] || '').tags || '(no tags)');
      return out({ ...ct, stats: customerStats(db, ct.id) });
    }
    case 'products': { const catalog = await app.catalog(); return out(listProducts(db, catalog, { days: Number(flags.days || 30) }).map(p => `${p.sku.padEnd(18)} ${(p.price_cents / 100).toFixed(2).padStart(7)}  ${p.stock.padEnd(9)} ${p.buildable == null ? '  —' : String(p.buildable).padStart(3)} buildable  ${String(p.units_recent).padStart(3)} sold/${flags.days || 30}d  ${String(p.waitlist_people).padStart(2)} waiting  ${p.name}`).join('\n')); }
    case 'discounts': {
      if (args[0] === 'create') return out(createDiscount(db, { code: args[1], kind: flags.kind || 'percent', value: Number(flags.value || 0), min_subtotal_cents: Number(flags.min || 0), max_uses: flags['max-uses'] ? Number(flags['max-uses']) : null, starts_at: flags.starts ? flags.starts + 'T00:00:00.000Z' : null, ends_at: flags.ends ? flags.ends + 'T23:59:59.999Z' : null, note: flags.note || null }));
      if (args[0] === 'disable' || args[0] === 'enable') return out(setDiscountStatus(db, args[1], args[0] === 'enable' ? 'active' : 'disabled'));
      if (args[0] === 'try') return out(evaluateDiscount(db, args[1], { subtotal_cents: Number(flags.subtotal || 0), shipping_cents: Number(flags.shipping || 800) }));
      return out(listDiscounts(db).map(d => `${d.code.padEnd(14)} ${d.status.padEnd(9)} ${describeDiscount(d).padEnd(16)} uses ${String(d.uses).padStart(3)}${d.max_uses ? `/${d.max_uses}` : ''}  given ${(d.given_cents / 100).toFixed(2).padStart(8)}  ${d.note || ''}`).join('\n') || 'no codes');
    }
    case 'analytics': { const a = analytics(db, { days: Number(flags.days || 30) }); const t = a.totals, p = a.previous; const m = (c) => (c / 100).toFixed(2); return out(`${a.days} days to ${a.to.slice(0, 10)} (previous period in brackets)\n  total sales   ${m(t.total_cents).padStart(10)}  [${m(p.total_cents)}]\n  net sales     ${m(t.net_cents).padStart(10)}  [${m(p.net_cents)}]\n  orders        ${String(t.orders).padStart(10)}  [${p.orders}]\n  average order ${m(t.aov_cents).padStart(10)}  [${m(p.aov_cents)}]\n  units         ${String(t.units).padStart(10)}  [${p.units}]\n  gross margin  ${(Math.round(t.gross_margin * 100) + '%').padStart(10)}\n  returning     ${(Math.round(t.returning_rate * 100) + '%').padStart(10)}\n  refunds       ${m(t.refunds_cents).padStart(10)}   discounts ${m(t.discounts_cents)}\n\ntop products\n${a.topProducts.map(x => `  ${x.sku.padEnd(18)} ${String(x.units).padStart(4)} u  ${m(x.gross_cents).padStart(9)}  ${Math.round(x.share * 100)}%`).join('\n')}\n\nby ${a.series.step}\n${a.series.buckets.map(b => `  ${b.label}  ${String(b.orders).padStart(3)} orders  ${m(b.net_cents).padStart(9)}`).join('\n')}`); }
    case 'marketing': { const m = marketing(db, { days: Number(flags.days || 30) }); const f = m.funnel; const $ = (c) => (c / 100).toFixed(2); return out(`${m.days} days\n  people ${f.contacts}  opted in ${f.opted_in}  bought ${f.customers}  bought again ${f.repeat}  attributed ${$(m.attributed_cents)}\n  this period: ${f.period.contacts} new, ${f.period.opted_in} opt-ins, ${f.period.first_orders} first orders, ${f.period.repeat_orders} repeat orders\n\nsources\n${m.sources.map(s => `  ${s.source.padEnd(12)} ${String(s.contacts).padStart(4)} people  ${String(s.opted_in).padStart(3)} opted in  ${String(s.customers).padStart(3)} bought`).join('\n')}\n\ncampaigns (7 days after each send)\n${m.campaigns.map(c => `  ${c.slug.padEnd(28)} to ${String(c.report.sent || 0).padStart(3)}  ${String(c.report.replies).padStart(2)} replies  ${String(c.report.orders).padStart(2)} orders  ${$(c.report.revenue_cents).padStart(8)}`).join('\n') || '  none sent'}\n\nautomations\n${m.automations.map(a => `  ${a.slug.padEnd(28)} sent ${String(a.report.sent).padStart(3)}  ${String(a.report.replies).padStart(2)} replies  ${String(a.report.orders).padStart(2)} orders  ${$(a.report.revenue_cents).padStart(8)}`).join('\n')}\n\nby state\n${Object.entries(m.salesByState).sort((a, b) => b[1] - a[1]).map(([st, c]) => `  ${st}  ${String(m.byState[st]).padStart(3)} customers  ${$(c).padStart(9)}`).join('\n')}`); }
    case 'suggestions': { const s = suggestions(db, cfg); return out(s.map(x => `[${x.kind}] ${x.title}\n    ${x.detail}\n    why: ${x.why}${x.action ? `\n    do: POST ${x.action.href} ${JSON.stringify(x.action.fields || {})}` : ''}`).join('\n') || 'nothing to decide — the system has no move to suggest'); }
    case 'track': return out(await pollTracking(db, cfg, { minAgeH: 0 }));
    case 'forget': return out(forgetContact(db, args[0], { actor: 'cli' }));
    case 'export': return out(exportContact(db, args[0]));
    case 'invoice': { if (args[0] !== 'remind') throw new Error('invoice remind <INV-…>'); const inv = db.get('SELECT id FROM invoices WHERE number = ?', args[1]); if (!inv) throw new Error('no such invoice'); return out(remindInvoice(db, inv.id, cfg, { actor: 'cli' })); }
    case 'po': {
      const kv = (list) => list.filter(a => a.includes('=')).map(a => { const [sku, rest] = a.split('='); const [qty, cents] = rest.split('@'); return { sku, qty: Number(qty), unit_cost_cents: cents === undefined ? undefined : Number(cents) }; });
      const byNum = (n) => { const p = poByNumber(db, n); if (!p) throw new Error(`no such purchase order ${n}`); return p; };
      if (!args[0]) return out(listPos(db, { status: flags.status || '' }).map(p => `${p.number}  ${p.status.padEnd(9)} ${p.supplier.padEnd(22)} ${(p.total_cents / 100).toFixed(2).padStart(9)}  ${p.items_text || ''}  ${p.expected_at ? 'expected ' + p.expected_at : ''}`).join('\n') || 'no purchase orders');
      if (args[0] === 'draft') return out(draftReorder(db, { actor: 'cli' }).map(p => `${p.number} to ${p.supplier}: ${p.items.map(i => `${i.qty}× ${i.sku}`).join(', ')}`).join('\n') || 'nothing to draft');
      if (args[0] === 'new') return out(createPo(db, { supplier: flags.supplier, supplier_url: flags.url || null, expected_at: flags.expected || null, notes: flags.notes || null, items: kv(args.slice(1)), actor: 'cli' }));
      if (args[0] === 'order') return out(markOrdered(db, byNum(args[1]).id, { expected_at: flags.expected || null, actor: 'cli' }));
      if (args[0] === 'receive') { const got = kv(args.slice(2)); return out(receivePo(db, byNum(args[1]).id, { received: got.length ? Object.fromEntries(got.map(g => [g.sku, g.qty])) : null, actor: 'cli' })); }
      if (args[0] === 'cancel') return out(cancelPo(db, byNum(args[1]).id, { actor: 'cli' }));
      return out(byNum(args[0]));
    }
    case 'recall': {
      const filter = { batch: flags.batch || '', fw: flags.fw || '', sku: flags.sku || '' };
      if (args[0] === 'new') return out(createRecall(db, { title: flags.title, body: flags.file ? readFileSync(flags.file, 'utf8') : DEFAULT_BODY, filter, actor: 'cli' }));
      if (args[0] === 'send') { const r = db.get('SELECT id FROM recalls WHERE number = ?', args[1]); if (!r) throw new Error('no such notice'); return out(sendRecall(db, r.id, cfg, { actor: 'cli' })); }
      if (args[0] === 'list') return out(listRecalls(db));
      if (!filter.batch && !filter.fw && !filter.sku) return out(deviceBatches(db).map(b => `${String(b.batch || '(none)').padEnd(16)} fw ${String(b.fw_version || '—').padEnd(8)} ${String(b.product_sku || '').padEnd(14)} ${String(b.n).padStart(3)} boards, ${b.shipped} shipped`).join('\n') || 'no devices');
      const q = recallQuery(db, filter); return out(`${q.devices.length} boards, ${q.shipped} shipped, ${q.in_stock} on the shelf, ${q.customers.length} customers\n${q.customers.map(c => `  ${c.email.padEnd(40)} ${c.serials.join(', ')}  (${c.orders.join(', ')})`).join('\n')}`);
    }
    case 'search': { const r = search(db, args.join(' ')); return out(r.groups.map(g => `${g.kind}\n${g.rows.map(x => '  ' + JSON.stringify(x)).join('\n')}`).join('\n') || 'nothing matches'); }
    case 'tickets': return out(listTickets(db, { status: flags.status || '', kind: flags.kind || '' }).map(t => `${t.number}  ${t.status.padEnd(19)} ${t.priority.padEnd(6)} ${t.kind.padEnd(13)} ${t.email || ''}  ${t.subject}`).join('\n') || 'no tickets');
    case 'queue': return out(queue(db).map(t => `${t.number}  due ${t.sla_due_at}  ${t.email || ''}  ${t.subject}`).join('\n') || 'inbox zero');
    case 'ticket': {
      if (args[0] === 'reply') { const t = ticketByNumber(db, args[1]); if (!t) throw new Error('no such ticket'); return out(reply(db, t.id, args[2], { cfg, actor: 'cli', close: !!flags.close })); }
      return out(ticketByNumber(db, args[0]) || 'no such ticket');
    }
    case 'inv': {
      if (args[0] === 'set') return out(upsertItem(db, { sku: args[1], name: flags.name || args[1], kind: flags.kind, reorder_point: Number(flags.reorder || 0), reorder_qty: Number(flags['reorder-qty'] || 0), unit_cost_cents: Number(flags['cost-cents'] || 0), supplier: flags.supplier, supplier_url: flags.url, location: flags.location }));
      if (args[0] === 'move') return out(move(db, args[1], Number(args[2]), flags.reason || 'adjust', flags.ref || null));
      if (args[0] === 'recipe') { setRecipe(db, args[1], Object.fromEntries(args.slice(2).map(p => p.split('=')))); return out(`recipe set for ${args[1]}`); }
      if (args[0] === 'buildable') { const b = buildableAhead(db, args[1]); return out(`${args[1]}: ${buildable(db, args[1])} now${b.later > b.now ? `, ${b.later} after what is on order lands${b.expected_at ? ` (expected ${b.expected_at})` : ''}` : ''}`); }
      return out(listItems(db).map(i => `${i.low ? '!' : ' '} ${i.sku.padEnd(24)} ${String(i.on_hand).padStart(5)}  (reorder at ${i.reorder_point})  ${i.kind.padEnd(9)} ${i.name}`).join('\n') || 'no inventory items');
    }
    case 'devices': {
      if (args[0] === 'import') return out(`imported ${importManifest(db, JSON.parse(readFileSync(args[1], 'utf8')), { batch: flags.batch, product_sku: flags.sku, board: flags.board })} devices`);
      return out(listDevices(db, { q: flags.q || '' }).map(d => `${d.serial.padEnd(18)} ${(d.mac || '').padEnd(18)} ${d.status.padEnd(8)} ${d.fw_version || ''}  ${d.order_number || ''}  ${d.batch || ''}`).join('\n') || 'no devices');
    }
    case 'device': return out(registerDevice(db, { serial: args[1], mac: flags.mac, board: flags.board, product_sku: flags.sku, fw_version: flags.fw, fw_sha256: flags.sha, batch: flags.batch }));
    case 'warranty': {
      if (args[0] === 'open') return out(openClaim(db, { orderNumber: flags.order, email: flags.email, serial: flags.serial, category: flags.category, description: args[1] || '', cfg, actor: 'cli' }));
      return out(listClaims(db).map(c => `${c.number}  ${c.status.padEnd(9)} ${c.category.padEnd(13)} ${c.order_number || ''}  ${c.email || ''}  → ${c.resolution}`).join('\n') || 'no claims');
    }
    case 'pnl': return out(args[0] === 'months' ? monthly(db, Number(flags.n || 12)).map(m => `${m.month}  income ${(m.income_cents / 100).toFixed(2).padStart(10)}  expenses ${(m.expenses_cents / 100).toFixed(2).padStart(10)}  net ${(m.net_cents / 100).toFixed(2).padStart(10)}`).join('\n') : profitAndLoss(db, { from: flags.from || '0000-00-00', to: flags.to || '9999-99-99' }));
    case 'trial-balance': return out(trialBalance(db));
    case 'sales-tax': return out(salesTaxByState(db, { from: flags.from || '0000-00-00', to: flags.to || '9999-99-99' }));
    case 'expense': { if (!ACCOUNTS[flags.account]) throw new Error(`account must be one of: ${Object.keys(ACCOUNTS).filter(a => a.startsWith('expenses:')).join(' ')}`); return out({ txn: postExpense(db, { date: flags.date, account: flags.account, cents: Number(flags.cents), memo: flags.memo || '', paidFrom: flags.from || 'assets:bank' }) }); }
    case 'payout': return out({ txn: postPayout(db, { id: flags.id, date: flags.date, amount_cents: Number(flags.cents), fee_cents: Number(flags['fee-cents'] || 0) }) });
    case 'books': return process.stdout.write(ledgerCsv(db, { from: flags.from, to: flags.to }));
    case 'outbox': return out(await sendOutbox(db, cfg));
    case 'waitlist': return out(`notified ${notifyWaitlist(db, args[1], cfg, { productName: flags.name || args[1], url: flags.url, actor: 'cli' })}`);
    case 'campaign': {
      if (args[0] === 'new') return out(createCampaign(db, { slug: args[1], subject: flags.subject, body_text: readFileSync(flags.file, 'utf8'), segment: flags.segment || 'all' }));
      if (args[0] === 'send') return out(`queued for ${queueCampaign(db, args[1], cfg, { actor: 'cli' })} people`);
      throw new Error('campaign new|send');
    }
    case 'probes': { if (args[0] === 'seed') { for (const p of DEFAULT_PROBES(cfg)) upsertProbe(db, p); return out('seeded'); } return out(await runDueProbes(db, { cfg, force: true })); }
    case 'status': return out(publicStatus(db));
    case 'exceptions': return out(openExceptions(db));
    case 'stripe': { if (args[0] !== 'sign') throw new Error('stripe sign <file>'); const body = readFileSync(args[1], 'utf8'); return out(signStripePayload(body, flags.secret || cfg.stripeWebhookSecret)); }
    default: throw new Error(`unknown command ${cmd} — try: backoffice help`);
  }
};

function ask(q, hidden = false) {
  return new Promise((resolve) => {
    process.stdout.write(q);
    const stdin = process.stdin; let s = '';
    if (hidden && stdin.isTTY) stdin.setRawMode(true);
    stdin.resume(); stdin.setEncoding('utf8');
    stdin.on('data', function h(ch) { if (ch === '\n' || ch === '\r' || ch === '\u0004') { if (hidden && stdin.isTTY) stdin.setRawMode(false); stdin.pause(); stdin.off('data', h); process.stdout.write('\n'); resolve(s.trim()); } else if (ch === '\u0003') process.exit(1); else s += ch; });
  });
}

main().then(() => { if (cmd !== 'serve') db.close(); }).catch(err => { console.error(`error: ${err.message}`); process.exit(1); });
