// The doctor: every condition a reliable installation needs, as a checklist
// a person can read — configuration, backups and their proofs, the books,
// probes, jobs, disk — with what to do about each. The same list is the
// Health page, `backoffice doctor`, and the top of the morning digest.
import { attention } from '../attention.mjs';
import { shipFromIssues } from './labels.mjs';
import { pushConfigured, newOrderOn, ntfyUrlFor } from './notify.mjs';
import { lastVerify, chainHead, securityText, securitySummary, nodeSupport } from '../integrity.mjs';
import { taxVerdict } from './nexus.mjs';
import { statfsSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { hasOperator } from '../auth.mjs';
import { listBackups, offsiteConfigured, offsiteEncrypted } from './backup.mjs';
import { trialBalance } from './ledger.mjs';
import { listProbes } from './status.mjs';
import { openExceptions } from './exceptions.mjs';
import { lastReconcile } from './reconcile.mjs';
import { jobRuns } from '../jobs.mjs';
import { orderStats } from './orders.mjs';
import { shippingStatus } from './catalog.mjs';
import { supportStats } from './support.mjs';
import { lowStock } from './inventory.mjs';
import { analytics } from './analytics.mjs';
import { envRules } from '../env.mjs';
import { describeBuild } from '../version.mjs';
import { listFlags } from '../flags.mjs';

const hoursSince = (iso) => iso ? (Date.now() - Date.parse(iso)) / 3600000 : null;
const fmtH = (h) => h == null ? 'never' : h < 1 ? `${Math.round(h * 60)} min ago` : h < 48 ? `${Math.round(h)} h ago` : `${Math.round(h / 24)} d ago`;

export function doctor(db, cfg) {
  const checks = []; const add = (key, label, ok, detail, fix = '') => checks.push({ key, label, ok, detail, fix });
  const runs = Object.fromEntries(jobRuns(db).map(r => [r.name, r]));
  const parse = (s) => { try { return JSON.parse(s || 'null'); } catch { return null; } };
  const demo = !!cfg.demo;
  // which this is, and what build
  const warns = envRules(cfg, db).filter(r => r.level === 'warn');
  add('environment', 'Environment', cfg.env === 'production' ? warns.length === 0 : warns.length ? false : null, `${cfg.env || 'development'} · ${describeBuild(cfg)}${warns.length ? ` · ${warns.map(w => w.text).join('; ')}` : ''}`, 'BACKOFFICE_ENV names it: production, staging, demo or development (env.mjs says what each refuses)');
  const sc = db.schema || {}; add('schema', 'Database schema', sc.ahead?.length ? false : true, `${sc.applied ?? '?'} migrations · latest ${sc.latest || '—'}${sc.pre_migrate_copies ? ` · ${sc.pre_migrate_copies} pre-migration cop${sc.pre_migrate_copies === 1 ? 'y' : 'ies'} on disk` : ''}${sc.ahead?.length ? ` · AHEAD of this build: ${sc.ahead.join(', ')}` : ''}`, 'start the build that applied them, or restore the pre-migration copy');
  const flags = listFlags(db, cfg); const overdue = flags.filter(f => f.overdue); const on = flags.filter(f => f.on);
  add('flags', 'Feature flags', overdue.length ? false : true, `${flags.length} declared · ${on.length ? `on: ${on.map(f => `${f.name} (${f.source})`).join(', ')}` : 'none on'}${overdue.length ? ` · past their date: ${overdue.map(f => f.name).join(', ')}` : ''}`, 'a rollout flag past remove_by is on for good or dead: delete it and the branch it guarded (src/flags.mjs)');
  // identity & reach
  const people = db.all("SELECT role, COUNT(*) AS n FROM users WHERE status = 'active' GROUP BY role"); const locked = db.get('SELECT COUNT(*) AS n FROM users WHERE locked_until > ?', new Date().toISOString()).n;
  add('operator', 'An owner can log in', hasOperator(db), hasOperator(db) ? `password + authenticator · ${people.map(p => `${p.n} ${p.role}`).join(', ')}${locked ? ` · ${locked} locked out` : ''}` : 'no owner', 'run `backoffice setup`, or accept the invitation');
  const stale = db.get("SELECT COUNT(*) AS n FROM users WHERE status = 'invited' AND invite_expires_at < ?", new Date().toISOString()).n; add('invites', 'Invitations', stale === 0, stale ? `${stale} expired without being accepted` : 'none waiting past their time', 'remove them on /users, or invite again');
  add('https', 'Served over HTTPS (secure cookies)', demo ? null : cfg.secure, demo ? 'demo mode' : cfg.secure ? cfg.baseUrl : `BACKOFFICE_BASE_URL is ${cfg.baseUrl}`, 'put Caddy or Fly in front and set BACKOFFICE_BASE_URL=https://…');
  const defaultUrl = /^https?:\/\/(127\.0\.0\.1|localhost)(:|\/|$)/.test(cfg.baseUrl);
  add('base_url', 'The codes on slips and labels point here', demo ? null : !defaultUrl, demo ? 'demo mode' : defaultUrl ? `BACKOFFICE_BASE_URL is ${cfg.baseUrl} — a phone scanning a slip lands nowhere` : cfg.baseUrl, 'set BACKOFFICE_BASE_URL to the address the phone reaches');
  const sh = shippingStatus();
  const edited = db.all('SELECT sku FROM product_overrides').map(r => r.sku); add('catalog', 'Store listing vs store.json', null, edited.length ? `${edited.length} edited here, not yet in the file: ${edited.join(', ')} — the store serves the edits; paste the patch from Products when convenient` : 'the store says what the file says');
  add('push', 'Phone push on a new order', demo ? null : pushConfigured(db, cfg) ? (newOrderOn(db, cfg) ? true : null) : null, demo ? 'demo mode' : pushConfigured(db, cfg) ? (newOrderOn(db, cfg) ? `to ${ntfyUrlFor(db, cfg)}` : 'a topic is set, but new orders are set to stay quiet') : `no topic — a new order ${cfg.operatorEmail ? `mails ${cfg.operatorEmail}` : 'reaches nobody (OPERATOR_EMAIL is empty)'}`, 'paste an ntfy topic under Settings → Your phone');
  const tax = taxVerdict(db, cfg); add('sales_tax', 'Sales tax', tax.ok, tax.text, 'turn on the stripe_tax flag in Settings, and register in each state you owe');
  add('sign_in_links', 'Sign-in links (mailed, once, this browser only; the authenticator still needed)', demo ? null : cfg.smtpUrl ? true : null, demo ? 'demo mode (the link opens straight to the code step)' : cfg.smtpUrl ? `on · good for ${cfg.signInLinkMinutes} min` : 'off — needs SMTP_URL; password + authenticator only', 'set SMTP_URL');
  add('allowlist', 'Admin reachable only from allowed networks', demo ? null : cfg.adminAllowCidrs ? true : null, cfg.adminAllowCidrs ? `ADMIN_ALLOW_CIDRS = ${cfg.adminAllowCidrs} (everything else sees a 404; the store, the webhook and customers\' pages are open)` : 'any network — set ADMIN_ALLOW_CIDRS to your home, office or VPN ranges to hide the admin from the internet', 'set ADMIN_ALLOW_CIDRS');
  add('session_bind', 'Sessions bound to the network they opened on', demo ? null : cfg.sessionBindIp ? true : null, cfg.sessionBindIp ? 'on — a session that moves networks is ended' : 'off — a stolen cookie works from anywhere until it expires; SESSION_BIND_IP=1 ends it the moment it moves (a phone changing networks signs in again)', 'set SESSION_BIND_IP=1');
  const recentNew = db.all("SELECT entity_id, detail, at FROM audit_log WHERE action = 'auth.login.new_network' AND at >= ? ORDER BY id DESC LIMIT 5", new Date(Date.now() - 7 * 86400000).toISOString()); const blocks = db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action IN ('guard.block','guard.lockout') AND at >= ?", new Date(Date.now() - 86400000).toISOString()).n;
  add('watch', 'The watch (scanners, lockouts, new networks)', null, `${blocks} block${blocks === 1 ? '' : 's'} or lockout${blocks === 1 ? '' : 's'} in 24 h · ${recentNew.length ? `sign-ins from a new network this week: ${recentNew.map(r => { try { return `${r.entity_id} from ${JSON.parse(r.detail).net}`; } catch { return r.entity_id; } }).join('; ')}` : 'no sign-in from a new network this week'}`, 'each one is a mail to the person and a push to the phone the moment it happens');
  add('labels', 'Labels from the ship bench', demo ? null : cfg.shippoToken ? shipFromIssues(cfg).length === 0 : null, demo ? 'demo labels (pretend, printable)' : cfg.shippoToken ? (shipFromIssues(cfg).length ? `SHIPPO_API_TOKEN set, return address incomplete: ${shipFromIssues(cfg).join(', ')}` : `SHIPPO_API_TOKEN set · from ${cfg.shipFrom.city}, ${cfg.shipFrom.state}`) : 'no SHIPPO_API_TOKEN — labels come from the website\'s fulfill.mjs and the manifest', 'set SHIPPO_API_TOKEN and SHIP_FROM_NAME/LINE1/CITY/STATE/ZIP');
  add('shipping_data', 'Box data (shipping.json) for the pack bench', demo ? null : sh.loaded_at ? !sh.error || hoursSince(sh.loaded_at) < 24 : sh.error ? false : null, demo ? 'demo boxes' : sh.loaded_at ? `${sh.boxes} boxes, ${sh.parcels} parcels · loaded ${fmtH(hoursSince(sh.loaded_at))}${sh.error ? ` · last refresh failed: ${sh.error}` : ''}` : sh.error ? sh.error : 'not asked for yet — opens with the first order page', 'set SHIPPING_JSON (or put shipping.json beside store.json) so boxes are planned, not guessed');
  add('smtp', 'Outbound mail relay', demo ? null : !!cfg.smtpUrl, demo ? 'demo transport' : cfg.smtpUrl ? 'SMTP_URL set' : 'mail is queued, never sent', 'set SMTP_URL to a relay with SPF/DKIM for your domain');
  add('imap', 'Support mailbox read', demo ? null : !!cfg.imapUrl, demo ? 'demo mode' : cfg.imapUrl ? 'IMAP_URL set' : 'inbound mail is not read', 'set IMAP_URL and SUPPORT_ADDR');
  add('stripe_webhook', 'Stripe webhook signing secret', !!cfg.stripeWebhookSecret, cfg.stripeWebhookSecret ? 'set' : 'orders cannot arrive', 'set STRIPE_WEBHOOK_SECRET from the Stripe dashboard');
  add('stripe_api', 'Stripe API key (line items, reconciliation)', demo ? null : !!cfg.stripeApiKey, demo ? 'demo mode' : cfg.stripeApiKey ? 'set' : 'orders without inline line items stay empty; no reconciliation', 'set STRIPE_API_KEY (a restricted key: read charges/payouts, read checkout sessions)');
  add('operator_email', 'Morning digest recipient', !!cfg.operatorEmail, cfg.operatorEmail || 'nobody gets the daily summary', 'set OPERATOR_EMAIL');
  // the door
  const fails24 = db.get('SELECT COUNT(*) AS n FROM audit_log WHERE action = \'auth.login.failed\' AND at >= ?', new Date(Date.now() - 86400000).toISOString()).n;
  add('logins', 'Failed logins, 24 h', fails24 <= 20, `${fails24}${fails24 ? ` · last from ${(() => { try { return JSON.parse(db.get('SELECT detail FROM audit_log WHERE action = \'auth.login.failed\' ORDER BY id DESC LIMIT 1')?.detail || '{}').ip || '?'; } catch { return '?'; } })()}` : ''} · lockout after 8 in 15 min per address`, 'read the audit log; if it is you, use `backoffice code`; if not, the lockout is already holding');
  const pwAge = hoursSince(db.get("SELECT MIN(password_set_at) AS t FROM users WHERE role = 'owner' AND status = 'active'")?.t); add('password_age', 'Oldest owner password', pwAge == null ? null : pwAge < 365 * 24, pwAge == null ? 'set before this check existed' : fmtH(pwAge), 'change it on /me (Your account)');
  const rwTokens = db.all('SELECT name, last_used_at FROM api_tokens WHERE scope = \'rw\''); add('tokens', 'API tokens that can write', null, rwTokens.length ? rwTokens.map(t => `${t.name}${t.last_used_at ? '' : ' (never used)'}`).join(', ') : 'none', 'revoke what is not used; a dashboard needs only a read-only token');
  // data & its proofs
  const integrity = db.get('PRAGMA integrity_check').integrity_check;
  add('integrity', 'Database integrity', integrity === 'ok', integrity, 'restore from the last checked backup');
  const tb = trialBalance(db); add('books', 'Books balance', tb.balanced, tb.balanced ? 'trial balance sums to zero' : 'NOT balanced', 'a bug — report it with the ledger CSV');
  // the jobs leave compact records in settings (backup:last, drill:last); the page's last_detail is truncated and never parsed here
  const bd = parse(db.setting('backup:last')); const backups = listBackups(join(cfg.dataDir, 'backups'));
  add('backup', 'Last backup checked', !!(bd?.checked?.ok && hoursSince(bd.at) < 36), bd ? `${fmtH(hoursSince(bd.at))}${bd.checked?.ok ? ' · opened, integrity ok, books balanced' : ` · CHECK FAILED: ${(bd.checked?.errors || []).join('; ')}`} · ${backups.length} kept` : 'never', 'Health → Back up now; the nightly job should do this');
  add('offsite', 'Off-site copy', offsiteConfigured(cfg) ? !!(bd?.offsite?.read_back && hoursSince(bd.at) < 36) : false, offsiteConfigured(cfg) ? (bd?.offsite?.read_back ? `${bd.offsite.name} · ${bd.offsite.bytes} bytes, read back${bd.offsite.encrypted ? ', sealed' : ''}` : bd?.offsite?.error ? `FAILED: ${bd.offsite.error}` : bd?.offsite?.skipped || 'not yet uploaded') : 'local disk only — the host dying takes the backups with it', 'set BACKUP_S3_URL / BACKUP_S3_KEY / BACKUP_S3_SECRET (B2, R2, Tigris, S3)');
  add('offsite_sealed', 'Off-site copy sealed', offsiteConfigured(cfg) ? offsiteEncrypted(cfg) : null, offsiteConfigured(cfg) ? (offsiteEncrypted(cfg) ? 'AES-256-GCM under BACKUP_ENCRYPTION_KEY' : 'customer names and addresses sit in the bucket in the clear') : 'no off-site copy', 'set BACKUP_ENCRYPTION_KEY (a long passphrase; keep it somewhere the server is not)');
  const dd = parse(db.setting('drill:last'));
  add('drill', 'Restore drill', !!(dd?.ok && hoursSince(dd.at) < 8 * 24), dd ? `${fmtH(hoursSince(dd.at))} · ${dd.ok ? `the ${dd.offsite ? 'off-site' : 'local'} copy boots, ${dd.orders ?? '?'} orders, ${dd.age_h} h old` : (dd.errors || []).join('; ')}` : 'never run', 'Health → Run the restore drill; weekly by the job');
  // money
  const rc = lastReconcile(db);
  add('reconcile', 'Reconciled with Stripe', demo ? null : rc ? rc.orders_missing === 0 && hoursSince(rc.at) < 48 : false, demo ? 'demo mode' : rc ? `${fmtH(hoursSince(rc.at))} · ${rc.charges_seen} charges, ${rc.orders_missing} missing · ${rc.payouts_posted} payouts posted` : runs.reconcile?.last_detail || 'never', 'needs STRIPE_API_KEY; then daily by the job');
  // watching
  const probes = listProbes(db); const down = probes.filter(p => p.last_ok === 0);
  add('probes', 'Probes', probes.length ? down.length === 0 : null, probes.length ? down.length ? `down: ${down.map(p => p.name).join(', ')}` : `${probes.length} up` : 'none configured', 'Status → add a probe');
  const ext = db.setting('health:external_monitor', '');
  add('external', 'Watched from outside', !!ext, ext ? `recorded: ${ext}` : 'the app cannot report its own death', 'point any uptime checker at /healthz and record its URL on the Health page');
  const ex = openExceptions(db); add('exceptions', 'Open exceptions', ex.length === 0, ex.length ? ex.map(e => e.key).join(', ') : 'none — silence means healthy', 'decide each on the Exceptions page');
  const failed = Object.values(runs).filter(r => r.last_ok === 0); add('jobs', 'Scheduled jobs', failed.length === 0, failed.length ? `failed: ${failed.map(r => r.name).join(', ')}` : `${Object.keys(runs).length} ran clean`, 'Settings → Jobs → run, read the detail');
  try { const s = statfsSync(cfg.dataDir); const free = s.bavail * s.bsize; const size = (() => { try { return statSync(cfg.dbPath).size; } catch { return 0; } })(); add('disk', 'Disk', free > 500 * 1024 * 1024, `${(free / 1048576 / 1024).toFixed(1)} GB free · database ${(size / 1048576).toFixed(1)} MB`, 'free space or move BACKOFFICE_DATA_DIR'); } catch { add('disk', 'Disk', null, 'not measurable here'); }
  // knowing it has not been breached
  const cv = lastVerify(db); const head = chainHead(db);
  add('audit_chain', 'Audit chain', cv ? cv.ok && hoursSince(cv.at) < 48 : (head ? null : null), cv ? `${cv.ok ? 'verified' : `BROKEN at row ${cv.broken_at}: ${cv.reason}`} ${fmtH(hoursSince(cv.at))} · ${cv.chained} rows chained${cv.unchained ? `, ${cv.unchained} from before the chain` : ''}${head ? ` · anchor ${head.id}:${String(head.mac).slice(0, 12)}…` : ''}` : head ? `not yet verified · ${head.id} rows marked` : 'no marked rows yet', '`backoffice verify`; the job runs daily; a break: SECURITY.md "Knowing it has not been breached"');
  const pf = parse(db.setting('boot:preflight')); const pfBad = (pf?.rows || []).filter(r => r.ok === false);
  add('preflight', 'Boot self-check', pf ? pfBad.length === 0 : null, pf ? (pfBad.length ? `failing: ${pfBad.map(r => `${r.key} (${r.text})`).join('; ')}` : `${pf.rows.length} checks passed at boot${db.auditKey?.source && db.auditKey.source !== 'memory' ? ` · audit key ${db.auditKey.source === 'BACKOFFICE_AUDIT_KEY' ? 'from the environment' : 'beside the database'}` : ''}`) : 'the server has not booted on this database', pfBad[0]?.fix || 'restart the server and read its first lines');
  const ns = nodeSupport(); add('node', 'Node is maintained', ns.ok, ns.text, 'move to the current LTS before the date');
  const ss = securitySummary(db, cfg, { hours: 24 });
  add('security_day', 'The last 24 hours', ss.quiet ? true : (ss.blocks.length || ss.lockouts.length || ss.newNets.length || ss.changes.filter(c => c.what !== 'deploy.boot').length) ? null : true, ss.quiet ? `quiet · ${ss.logins.length} sign-in${ss.logins.length === 1 ? '' : 's'} · ${ss.sessions.length} session${ss.sessions.length === 1 ? '' : 's'} open` : `${ss.failed} failed sign-ins · ${ss.blocks.length} blocked · ${ss.newNets.length} new network${ss.newNets.length === 1 ? '' : 's'} · ${ss.changes.filter(c => c.what !== 'deploy.boot').length} change${ss.changes.length === 1 ? '' : 's'} to people/tokens/settings — the Health page's Security block names each`, 'read the Security block; `backoffice lockdown` if any of it is not yours');
  if (demo) add('demo', 'Demo mode', null, 'synthetic data; nothing reaches a real service; the database reseeds on its cadence', '');
  const bad = checks.filter(c => c.ok === false);
  return { ok: bad.length === 0, checks, failing: bad.map(c => c.key), at: new Date().toISOString() };
}

// What needs doing today, as lines — the Home page's list and the digest's.
export function todayLines(db, cfg, { base = '' } = {}) {
  // One list for the home page, the sidebar and this mail: attention.mjs ranks it by deadline (the
  // health row is left out here; the digest has its own HEALTH section and the home its pill).
  return attention(db, cfg).items.filter(i => i.key !== 'health').map(i => ({ n: i.n, what: i.what, href: `${base}${i.href}`, action: i.action, urgent: i.level === 'now', level: i.level, due: i.due }));
}

// The morning mail: today's list, the thirty-day numbers, the health checklist. Plain text, one screen.
export function digestText(db, cfg) {
  const lines = todayLines(db, cfg, { base: cfg.baseUrl }); const a = analytics(db, { days: 30 }); const t = a.totals, p = a.previous; const h = doctor(db, cfg);
  const m = (c) => `$${(c / 100).toFixed(2)}`; const pct = (x, y) => y ? ` (${x >= y ? '+' : ''}${Math.round((x - y) / y * 100)}%)` : '';
  return `${cfg.company} — ${new Date().toISOString().slice(0, 10)}

TODAY
${lines.length ? lines.map(l => `  ${String(l.n).padStart(3)}  ${l.what}\n       ${l.href}`).join('\n') : '  nothing waiting — silence means healthy'}

LAST 30 DAYS (vs the 30 before)
  total sales   ${m(t.total_cents)}${pct(t.total_cents, p.total_cents)}
  orders        ${t.orders}${pct(t.orders, p.orders)}
  average order ${m(t.aov_cents)}${pct(t.aov_cents, p.aov_cents)}
  returning     ${Math.round(t.returning_rate * 100)}%

HEALTH ${h.ok ? '— all checks pass' : `— ${h.failing.length} failing: ${h.failing.join(', ')}`}
${h.checks.map(c => `  ${c.ok === true ? 'ok ' : c.ok === false ? '!! ' : '-- '} ${c.label}: ${c.detail}`).join('\n')}

${securityText(db, cfg, { hours: 24 })}

${cfg.baseUrl}/health
`;
}
