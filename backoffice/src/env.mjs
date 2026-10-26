// Which environment this process is: production, staging, demo or development. The label is
// explicit (BACKOFFICE_ENV), shown on every page, in the title, on /healthz and in the doctor, and
// it carries rules — a few things an environment must never do, checked before the app serves:
//
//   production   real data, real services. Refuses a database another environment labeled, a demo
//                seed, and a base URL that is not https. Nothing is muted.
//   staging      a copy of production (the nightly `kit refresh` restores the latest off-site backup)
//                on tomorrow's build, with Stripe TEST keys. Mail and IMAP are muted unless the
//                operator allows them, GitHub issues are never posted, and nothing is ever written to
//                the backup bucket: staging reads production's copies, it does not add its own.
//                A live Stripe key is refused outright.
//   demo         synthetic data; demo.mjs already turns every integration off. Said once, here, for
//                the label and the title.
//   development  a laptop or the test suite. No rules; the doctor says to set the label when the base
//                URL looks public.
//
// A database carries the label of the environment that last booted it (settings env:label), so a
// staging or demo file copied under production is refused with a message that says what it is.
import { isDemo } from './modules/demo.mjs';

export const ENVIRONMENTS = ['production', 'staging', 'demo', 'development'];

// The label this process runs under: BACKOFFICE_ENV when set; demo when demo mode is on (by flag or by
// a seeded database); development otherwise.
export function resolveEnv(cfg, db) {
  const asked = String(cfg.env || '').trim().toLowerCase();
  if (asked && !ENVIRONMENTS.includes(asked)) throw new Error(`BACKOFFICE_ENV is "${asked}" — it is one of ${ENVIRONMENTS.join(', ')}`);
  if (asked) return asked;
  return cfg.demo || (db && isDemo(db)) ? 'demo' : 'development';
}

const liveStripe = (k) => /^(sk|rk)_live_/.test(String(k || ''));

// What this environment refuses (fatal — the app does not serve) and what it notes (warn — the doctor
// shows it). Pure: it reads cfg and the database's label and returns the list.
export function envRules(cfg, db) {
  const env = resolveEnv(cfg, db); const out = []; const fatal = (text) => out.push({ level: 'fatal', text }); const warn = (text) => out.push({ level: 'warn', text });
  const label = db ? db.setting('env:label', '') : '';
  const seeded = !!(cfg.demo || (db && isDemo(db)));
  if (env === 'production') {
    if (seeded) fatal('this database carries the demo seed — production never runs on synthetic data (BACKOFFICE_DEMO is set, or this file came from the demo)');
    if (label && label !== 'production') fatal(`this database was last run as ${label} — a copy made for ${label} must not become production; restore a production backup instead`);
    if (!cfg.secure) fatal(`BACKOFFICE_BASE_URL is ${cfg.baseUrl} — production serves over https (cookies are Secure, HSTS is on)`);
    if (cfg.stripeApiKey && !liveStripe(cfg.stripeApiKey)) warn('STRIPE_API_KEY is a test-mode key (the shadow fortnight; switch to live before real orders)');
  }
  if (env === 'staging') {
    if (liveStripe(cfg.stripeApiKey)) fatal('STRIPE_API_KEY is a LIVE key — staging charges nobody; use a test-mode key');
    if (cfg.smtpUrl && !cfg.stagingAllowMail) warn('SMTP_URL is set but staging mutes mail (STAGING_ALLOW_MAIL=1 to send from a throwaway relay)');
  }
  if (env === 'demo') {
    for (const [k, v] of [['SMTP_URL', cfg.smtpUrl], ['IMAP_URL', cfg.imapUrl], ['STRIPE_API_KEY', cfg.stripeApiKey], ['GITHUB_TOKEN', cfg.githubToken]]) if (v) warn(`${k} is set but the demo never uses it (ignored)`);
  }
  if (env === 'development' && cfg.secure && !/^https:\/\/(localhost|127\.0\.0\.1)/.test(cfg.baseUrl)) warn(`BACKOFFICE_BASE_URL is public (${cfg.baseUrl}) but BACKOFFICE_ENV is not set — set it to production, staging or demo so the pages say which this is`);
  return out;
}

// Apply the environment: resolve the label, enforce the rules, mute what the label mutes, and label the
// database. Throws when a fatal rule holds, with every fatal line in the message, so `serve` stops before
// it listens and a container goes unhealthy with the reason in its log.
export function applyEnv(cfg, db) {
  const env = resolveEnv(cfg, db); cfg.env = env;
  const rules = envRules(cfg, db); const fatal = rules.filter(r => r.level === 'fatal');
  if (fatal.length) throw new Error(`refusing to run as ${env}:\n  - ${fatal.map(r => r.text).join('\n  - ')}`);
  if (env === 'staging') {
    // what staging may never do: post issues, reach the real mailbox, write to the backup bucket
    cfg.githubToken = ''; cfg.githubRepo = '';
    if (!cfg.stagingAllowMail) { cfg.smtpUrl = ''; cfg.imapUrl = ''; }
    cfg.offsiteWrites = false;
  }
  if (db) { if (db.setting('env:label', '') !== env) db.setSetting('env:label', env); }
  return { env, rules };
}

// The words the pages, the title and the doctor use for each environment.
export const ENV_TEXT = {
  production: { badge: '', banner: '' },
  staging: { badge: 'staging', banner: 'Staging — a copy of production on the next build. Mail, issues and off-site backups are muted; Stripe is in test mode.' },
  demo: { badge: 'demo', banner: '' },        // the demo has its own banner (synthetic data) in render.mjs
  development: { badge: 'dev', banner: '' },
};
