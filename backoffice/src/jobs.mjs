// The scheduler. Every recurring task is a named job with an interval; a
// run is recorded; a failure raises an exception instead of being logged
// into the void. `backoffice job <name>` runs one by hand.
import { nowIso } from './db.mjs';
import { sendOutbox } from './modules/mail.mjs';
import { imapFetchUnseen } from './modules/imap.mjs';
import { parseEmail, ingestEmail, slaBreaches } from './modules/support.mjs';
import { runDueProbes } from './modules/status.mjs';
import { lowStock } from './modules/inventory.mjs';
import { overdue } from './modules/invoices.mjs';
import { raise, resolve } from './modules/exceptions.mjs';
import { backup } from './modules/backup.mjs';
import { unreconciledStripeCents } from './modules/ledger.mjs';
import { resetDemo, isDemo } from './modules/demo.mjs';
import { join } from 'node:path';

export const JOBS = {
  // name: [interval seconds, fn(app)]
  outbox:   [30, async ({ db, cfg }) => sendOutbox(db, cfg)],
  inbox:    [120, async ({ db, cfg }) => {
    if (!cfg.imapUrl) return { skipped: 'IMAP_URL not set' };
    const msgs = await imapFetchUnseen(cfg.imapUrl);
    let created = 0, appended = 0, dup = 0;
    for (const m of msgs) {
      if (db.get('SELECT uid FROM mail_seen WHERE uid = ?', m.uid)) { dup++; continue; }
      const r = ingestEmail(db, parseEmail(m.raw), { cfg });
      db.run('INSERT INTO mail_seen (uid, seen_at) VALUES (?, ?)', m.uid, nowIso());
      if (r.created) created++; else if (r.ticket) appended++; else dup++;
    }
    db.run('DELETE FROM mail_seen WHERE seen_at < ?', new Date(Date.now() - 60 * 86400000).toISOString());
    return { fetched: msgs.length, created, appended, ignored: dup };
  }],
  probes:   [60, async (app) => ({ ran: (await runDueProbes(app.db, { cfg: app.cfg })).length })],
  sla:      [600, async ({ db, cfg }) => {
    const late = slaBreaches(db);
    for (const t of late) await raise(db, cfg, `sla:${t.number}`, `support [sla]: ${t.number} unanswered past ${cfg.slaHours}h`, `${t.subject}\nfrom ${t.email || 'unknown'} · opened ${t.created_at} · due ${t.sla_due_at}\n\nReply at ${cfg.baseUrl}/tickets/${t.id}`);
    const lateKeys = new Set(late.map(t => `sla:${t.number}`));
    for (const ex of db.all('SELECT key FROM exceptions WHERE key LIKE \'sla:%\' AND resolved_at IS NULL')) if (!lateKeys.has(ex.key)) await resolve(db, cfg, ex.key, 'Answered.');
    return { breached: late.length };
  }],
  stock:    [3600, async ({ db, cfg }) => {
    const low = lowStock(db);
    for (const i of low) await raise(db, cfg, `stock:${i.sku}`, `inventory [reorder]: ${i.sku}`, `${i.name}: ${i.on_hand} on hand, reorder point ${i.reorder_point}, reorder qty ${i.reorder_qty}.\n${i.supplier ? `Supplier: ${i.supplier} ${i.supplier_url || ''}` : ''}`);
    const lowKeys = new Set(low.map(i => `stock:${i.sku}`));
    for (const ex of db.all('SELECT key FROM exceptions WHERE key LIKE \'stock:%\' AND resolved_at IS NULL')) if (!lowKeys.has(ex.key)) await resolve(db, cfg, ex.key, 'Restocked.');
    return { low: low.length };
  }],
  invoices: [3600, async ({ db, cfg }) => {
    const late = overdue(db);
    for (const i of late) await raise(db, cfg, `invoice:${i.number}`, `books [overdue]: invoice ${i.number}`, `${i.number} for ${i.email || i.bill_to} — ${(i.total_cents / 100).toFixed(2)} USD was due ${i.due_at}.`);
    return { overdue: late.length };
  }],
  books:    [86400, async ({ db, cfg }) => {
    const c = unreconciledStripeCents(db);
    if (c > 50_000) await raise(db, cfg, 'books:stripe-balance', 'books [reconcile]: Stripe balance not paid out', `The ledger shows ${(c / 100).toFixed(2)} USD cleared through Stripe but not yet recorded as a payout. Either the payout webhook is not configured or a payout is missing — import it on /books.`);
    else await resolve(db, cfg, 'books:stripe-balance', 'Reconciled.');
    return { stripe_unpaid_out_cents: c };
  }],
  backup:   [86400, async ({ db, cfg }) => backup(db, join(cfg.dataDir, 'backups'), { keep: cfg.backupKeep })],
  // A public demo starts over on a cadence so visitors always find the seeded week. Never runs outside demo mode.
  demo_reset: [60, async ({ db, cfg }) => {
    if (!cfg.demo || !isDemo(db) || !cfg.demoResetMinutes) return { skipped: 'not a timed public demo' };
    const last = db.setting('demo:last_reset', db.setting('demo:seeded_at', ''));
    if (last && Date.now() - Date.parse(last) < cfg.demoResetMinutes * 60_000) return { next_in_s: Math.round((cfg.demoResetMinutes * 60_000 - (Date.now() - Date.parse(last))) / 1000) };
    const r = await resetDemo(db, cfg); db.setSetting('demo:last_reset', nowIso());
    return { reset: true, ...r };
  }],
};

export async function runJob(app, name) {
  const job = JOBS[name]; if (!job) throw new Error(`unknown job ${name}`);
  const t0 = Date.now();
  try {
    const result = await job[1](app);
    app.db.run('INSERT INTO job_runs (name, last_run_at, last_ok, last_detail) VALUES (?, ?, 1, ?) ON CONFLICT(name) DO UPDATE SET last_run_at = excluded.last_run_at, last_ok = 1, last_detail = excluded.last_detail', name, nowIso(), JSON.stringify(result).slice(0, 500));
    return { job: name, ok: true, ms: Date.now() - t0, result };
  } catch (e) {
    app.db.run('INSERT INTO job_runs (name, last_run_at, last_ok, last_detail) VALUES (?, ?, 0, ?) ON CONFLICT(name) DO UPDATE SET last_run_at = excluded.last_run_at, last_ok = 0, last_detail = excluded.last_detail', name, nowIso(), String(e.message || e).slice(0, 500));
    await raise(app.db, app.cfg, `job:${name}`, `backoffice [job failed]: ${name}`, String(e.stack || e).slice(0, 2000)).catch(() => {});
    return { job: name, ok: false, ms: Date.now() - t0, error: String(e.message || e) };
  }
}

export function startScheduler(app, { tickMs = 10_000 } = {}) {
  const last = {};
  let running = false;
  const tick = async () => {
    if (running) return; running = true;
    try {
      for (const [name, [interval]] of Object.entries(JOBS)) {
        if (Date.now() - (last[name] || 0) < interval * 1000) continue;
        last[name] = Date.now();
        const r = await runJob(app, name);
        if (r.ok) { const ex = app.db.get('SELECT key FROM exceptions WHERE key = ? AND resolved_at IS NULL', `job:${name}`); if (ex) await resolve(app.db, app.cfg, `job:${name}`, 'Ran clean.'); }
      }
    } finally { running = false; }
  };
  const timer = setInterval(tick, tickMs); timer.unref();
  setTimeout(tick, 1500).unref();
  return () => clearInterval(timer);
}
export const jobRuns = (db) => db.all('SELECT * FROM job_runs ORDER BY name');
