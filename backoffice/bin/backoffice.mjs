#!/usr/bin/env node
// The command line is the same company as the web admin — same SQLite file,
// same functions — so an operator (or an AI operator) can run the whole
// business from a terminal. `backoffice help` lists everything.
import { readFileSync } from 'node:fs';
import { loadConfig } from '../src/config.mjs';
import { openDb } from '../src/db.mjs';
import { setupOperator, createApiToken, revokeApiToken, totpUri, hasOperator } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';
import { startScheduler, runJob, JOBS } from '../src/jobs.mjs';
import { loadCatalog } from '../src/modules/catalog.mjs';
import { recordOrder, transition, listOrders, orderByNumber, fulfillCsv } from '../src/modules/orders.mjs';
import { parseCsv, detectColumns, csvRowToOrder, signStripePayload } from '../src/modules/stripe.mjs';
import { listTickets, ticketByNumber, reply, queue } from '../src/modules/support.mjs';
import { upsertItem, move, setRecipe, listItems, buildable } from '../src/modules/inventory.mjs';
import { registerDevice, importManifest, listDevices } from '../src/modules/devices.mjs';
import { openClaim, ANSWERS, listClaims } from '../src/modules/warranty.mjs';
import { profitAndLoss, trialBalance, monthly, postExpense, postPayout, salesTaxByState, ledgerCsv, ACCOUNTS } from '../src/modules/ledger.mjs';
import { upsertProbe, DEFAULT_PROBES, runDueProbes, publicStatus } from '../src/modules/status.mjs';
import { backup, listBackups } from '../src/modules/backup.mjs';
import { snapshot } from '../src/modules/dashboard.mjs';
import { notifyWaitlist, createCampaign, queueCampaign } from '../src/modules/campaigns.mjs';
import { sendOutbox } from '../src/modules/mail.mjs';
import { openExceptions } from '../src/modules/exceptions.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { totp } from '../src/auth.mjs';
import { join } from 'node:path';

const [cmd = 'help', ...rest] = process.argv.slice(2);
const flags = {}; const args = [];
for (let i = 0; i < rest.length; i++) { const a = rest[i]; if (a.startsWith('--')) { const [k, v] = a.slice(2).split('='); flags[k] = v ?? (rest[i + 1] && !rest[i + 1].startsWith('--') ? rest[++i] : true); } else args.push(a); }
const out = (x) => console.log(typeof x === 'string' ? x : JSON.stringify(x, null, 2));
const cfg = loadConfig();
const db = openDb(cfg.dbPath);
const app = createApp({ db, cfg });

const HELP = `backoffice — the company in one SQLite file (${cfg.dbPath})

  setup                                    set the operator password + authenticator (first run)
  demo                                     fill an EMPTY database with a realistic week (orders, tickets, stock, books…) to explore
  code                                     print the current authenticator code (same as your phone shows; for local trying-out)
  serve                                    run the web admin, API, forms, webhook and the scheduler
  token create <name> | token revoke <name>  API bearer tokens for the CLI/remote/an AI operator
  snapshot                                 the dashboard numbers as JSON
  job <name> | job list                    run one scheduled job now (outbox inbox probes sla stock invoices books backup)
  backup | backups                         take a backup now / list them

orders
  orders [--status paid] [--batch 2026-W43] [--q text]
  order <EL-000123>
  orders import <stripe-export.csv>        catch-up from the Stripe Payments export
  orders fulfill-csv [--batch W] > orders.csv   the CSV the website's fulfill.mjs eats
  order pack|ship|deliver <EL-…> [--carrier USPS --tracking 9400…]
  order new --email a@b.c --name "…" --sku wap-kit [--qty 1] --line1 … --city … --state XX --zip 12345 [--cents 6900]

support
  tickets [--status open] [--kind support]   |  ticket <T-000045>  |  queue (what needs an answer, oldest first)
  ticket reply <T-…> "text" [--close]

inventory & devices
  inv | inv set <sku> --name … [--kind part|printed|finished|packaging] [--reorder 10 --reorder-qty 50 --cost-cents 450 --supplier …]
  inv move <sku> <±n> [--reason receive|adjust|scrap] [--ref …]   |   inv recipe <finished-sku> part=qty part=qty
  inv buildable <sku>
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
      if (hasOperator(db) && !flags.force) return out('operator exists; pass --force to reset the password and authenticator');
      const pw = flags.password || await ask('New operator password: ', true);
      if (String(pw).length < 12) throw new Error('use at least 12 characters');
      const secret = setupOperator(db, pw);
      for (const p of DEFAULT_PROBES(cfg)) upsertProbe(db, p);
      return out(`Operator set. Add this to your authenticator app (TOTP):\n\n  ${totpUri(secret, cfg.supportAddr || 'operator', `${cfg.company} backoffice`)}\n\n(secret: ${secret})\n\nDefault status probes seeded. Now: backoffice serve`);
    }
    case 'demo': {
      if (!hasOperator(db)) { const secret = setupOperator(db, flags.password || 'demo-password-change-me'); for (const p of DEFAULT_PROBES(cfg)) upsertProbe(db, p); out(`Operator set with password "${flags.password || 'demo-password-change-me'}". Authenticator secret: ${secret} — or run \`backoffice code\` for the current code.`); }
      out(await seedDemo(db, cfg));
      return out('Demo data loaded. Now: backoffice serve  → http://127.0.0.1:' + cfg.port + '/  (log in with the password above and `backoffice code`)');
    }
    case 'code': { const secret = db.setting('auth:totp'); if (!secret) throw new Error('no operator yet — run setup or demo'); return out(totp(secret)); }
    case 'serve': {
      if (!hasOperator(db)) console.error('[backoffice] no operator yet — run `backoffice setup` in another shell; serving anyway');
      const addr = await app.listen(); startScheduler(app);
      console.log(`[backoffice] listening on http://${addr.address}:${addr.port}  (public base: ${cfg.baseUrl})`);
      return new Promise(() => {});
    }
    case 'token': return args[0] === 'revoke' ? out(`revoked ${revokeApiToken(db, args[1])}`) : out(`${createApiToken(db, args[1] || 'cli')}\n(shown once — store it in the client's environment)`);
    case 'snapshot': return out(snapshot(db));
    case 'job': return args[0] === 'list' ? out(Object.keys(JOBS)) : out(await runJob(app, args[0]));
    case 'backup': return out(backup(db, join(cfg.dataDir, 'backups'), { keep: cfg.backupKeep }));
    case 'backups': return out(listBackups(join(cfg.dataDir, 'backups')));

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
      if (args[0] === 'new') {
        const catalog = await loadCatalog(cfg).catch(() => null);
        const { order } = recordOrder(db, { source: 'manual', email: flags.email, name: flags.name, total_cents: Number(flags.cents || 0), subtotal_cents: Number(flags.cents || 0), ship: { line1: flags.line1, line2: flags.line2, city: flags.city, state: flags.state, zip: flags.zip, country: flags.country || 'US' }, items: [{ sku: flags.sku, name: flags.sku, qty: Number(flags.qty || 1), unit_cents: Number(flags.cents || 0) / Number(flags.qty || 1) }] }, { catalog, actor: 'cli' });
        return out(order);
      }
      if (['pack', 'ship', 'deliver'].includes(args[0])) { const o = orderByNumber(db, args[1]); if (!o) throw new Error('no such order'); return out(transition(db, o.id, { pack: 'packed', ship: 'shipped', deliver: 'delivered' }[args[0]], { actor: 'cli', carrier: flags.carrier, tracking: flags.tracking, cfg })); }
      return out(orderByNumber(db, args[0]) || 'no such order');
    }
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
      if (args[0] === 'buildable') return out(`${args[1]}: ${buildable(db, args[1])}`);
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
