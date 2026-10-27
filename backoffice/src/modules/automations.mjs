// Automations, the part of a mail tool people pay for: a welcome note after
// the opt-in click, a "how is it going?" a few days after delivery, a nudge
// to someone who has been quiet for half a year. Each is a trigger, a delay
// and a plain-text body with merge tags. The job enrolls who qualifies (once
// per thing that happened — the ref), waits out the delay, re-checks consent
// at send time, and records every send and every skip. No tracking, ever;
// the report is replies and orders in the week after, by address.
import { nowIso } from '../db.mjs';
import { enqueue, fill } from './mail.mjs';
import { canMarket } from './contacts.mjs';
import { unsubscribeUrl } from './campaigns.mjs';

export const TRIGGERS = {
  consent_confirmed: 'someone confirms the double opt-in',
  order_delivered: 'an order is marked delivered',
  waitlist_joined: 'someone joins a waitlist',
  idle_days: 'a customer has not ordered in N days (once)',
};
export const DEFAULTS = [
  { slug: 'welcome', name: 'Welcome', trigger: 'consent_confirmed', delay_hours: 1, subject: 'Welcome — here is what to expect', body_text: 'Hi {{first_name}},\n\nThanks for confirming. Here is the deal: a short note from the bench about once a month, a one-time heads-up when something you are waiting for is back in stock, and nothing else. No tracking pixels, no "we noticed you opened".\n\nIf you ever want out, every mail has a one-click unsubscribe.\n\n# Where to start\n\n- The FAQ: {{site}}/faq\n- What a Canary actually records: {{site}}/privacy\n\n— {{company}}' },
  { slug: 'settled-in', name: 'Settled in?', trigger: 'order_delivered', delay_hours: 72, subject: 'Is your {{product}} settled in?', body_text: 'Hi {{first_name}},\n\nYour order {{order}} should have arrived a few days ago. Did it provision itself the way the QR card promised?\n\nIf anything is off — a part missing, a lid that does not seat, a Canary that blinks twice and never joins — just reply to this mail. We fix, we do not ask you to send things back.\n\nIf it is all working: what surprised you? A one-line reply helps the next batch.\n\n— {{company}}' },
  { slug: 'quiet-half-year', name: 'Quiet half-year', trigger: 'idle_days', trigger_arg: 180, delay_hours: 0, subject: 'Still on the wall?', body_text: 'Hi {{first_name}},\n\nIt has been a while since your last order, so this is the one note we send: the firmware moved on, the fleet got a few new faces, and your Canary can take the update from the Lab in a minute.\n\nIf it is boxed in a drawer, no hard feelings — reply and say why, that is the most useful mail we get.\n\n— {{company}}' },
];

export function seedAutomations(db, { enabled = 1 } = {}) {
  for (const a of DEFAULTS) db.run('INSERT OR IGNORE INTO automations (slug, name, trigger, trigger_arg, delay_hours, subject, body_text, enabled, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)', a.slug, a.name, a.trigger, a.trigger_arg || 0, a.delay_hours, a.subject, a.body_text, enabled, nowIso());
  return listAutomations(db);
}
export const listAutomations = (db) => db.all(`SELECT a.*, (SELECT COUNT(*) FROM automation_runs r WHERE r.automation_slug = a.slug AND r.sent_at IS NOT NULL) AS sent, (SELECT COUNT(*) FROM automation_runs r WHERE r.automation_slug = a.slug AND r.sent_at IS NULL AND r.skipped IS NULL) AS waiting, (SELECT COUNT(*) FROM automation_runs r WHERE r.automation_slug = a.slug AND r.skipped IS NOT NULL) AS skipped FROM automations a ORDER BY a.created_at`);
export const automation = (db, slug) => db.get('SELECT * FROM automations WHERE slug = ?', slug);
export function upsertAutomation(db, { slug, name, trigger, trigger_arg = 0, delay_hours = 0, subject, body_text, enabled = 1 }) {
  if (!/^[a-z0-9][a-z0-9-]{1,40}$/.test(String(slug || ''))) throw new Error('a slug is lowercase letters, digits and dashes');
  if (!TRIGGERS[trigger]) throw new Error(`trigger is one of ${Object.keys(TRIGGERS).join(', ')}`);
  db.run('INSERT INTO automations (slug, name, trigger, trigger_arg, delay_hours, subject, body_text, enabled, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(slug) DO UPDATE SET name = excluded.name, trigger = excluded.trigger, trigger_arg = excluded.trigger_arg, delay_hours = excluded.delay_hours, subject = excluded.subject, body_text = excluded.body_text, enabled = excluded.enabled',
    slug, name || slug, trigger, Number(trigger_arg) || 0, Number(delay_hours) || 0, subject, body_text, enabled ? 1 : 0, nowIso());
  db.audit('web', 'automation.save', 'automation', slug); return automation(db, slug);
}
export function setAutomationEnabled(db, slug, on) { db.run('UPDATE automations SET enabled = ? WHERE slug = ?', on ? 1 : 0, slug); db.audit('web', on ? 'automation.enable' : 'automation.disable', 'automation', slug); return automation(db, slug); }
export const runsFor = (db, slug, limit = 50) => db.all('SELECT r.*, c.email, c.name FROM automation_runs r JOIN contacts c ON c.id = r.contact_id WHERE r.automation_slug = ? ORDER BY r.id DESC LIMIT ?', slug, limit);

// Who qualifies right now, per trigger, as (contact_id, ref, happened_at). Enrollment is idempotent on (automation, contact, ref).
function candidates(db, a, now) {
  const since = new Date(now - 30 * 86400000).toISOString();   // a trigger older than a month is history, not a reason to mail
  if (a.trigger === 'consent_confirmed') return db.all('SELECT id AS contact_id, \'consent:\' || consent_at AS ref, consent_at AS at FROM contacts WHERE consent_marketing = 1 AND consent_at >= ?', since);
  if (a.trigger === 'order_delivered') return db.all('SELECT contact_id, \'order:\' || number AS ref, delivered_at AS at FROM orders WHERE delivered_at >= ? AND contact_id IS NOT NULL AND total_cents > 0', since);
  if (a.trigger === 'waitlist_joined') return db.all('SELECT contact_id, \'waitlist:\' || sku AS ref, created_at AS at FROM waitlist WHERE created_at >= ?', since);
  if (a.trigger === 'idle_days') { const cutoff = new Date(now - (a.trigger_arg || 180) * 86400000).toISOString(); return db.all(`SELECT c.id AS contact_id, 'idle' AS ref, ? AS at FROM contacts c WHERE c.id IN (SELECT contact_id FROM orders WHERE status != 'canceled' AND total_cents > 0) AND c.id NOT IN (SELECT contact_id FROM orders WHERE status != 'canceled' AND total_cents > 0 AND paid_at >= ?)`, new Date(now).toISOString(), cutoff); }
  return [];
}

export function runAutomations(db, cfg, { now = Date.now() } = {}) {
  const out = { enrolled: 0, sent: 0, skipped: 0 };
  for (const a of db.all('SELECT * FROM automations WHERE enabled = 1')) {
    for (const c of candidates(db, a, now)) {
      const due = new Date(Date.parse(c.at) + a.delay_hours * 3600000).toISOString();
      const r = db.run('INSERT OR IGNORE INTO automation_runs (automation_slug, contact_id, ref, due_at) VALUES (?, ?, ?, ?)', a.slug, c.contact_id, c.ref, due);
      if (r.changes) out.enrolled++;
    }
  }
  const nowIsoS = new Date(now).toISOString();
  for (const r of db.all('SELECT r.*, a.subject, a.body_text, a.enabled, a.name AS aname FROM automation_runs r JOIN automations a ON a.slug = r.automation_slug WHERE r.sent_at IS NULL AND r.skipped IS NULL AND r.due_at <= ? ORDER BY r.due_at LIMIT 200', nowIsoS)) {
    const p = db.get('SELECT * FROM contacts WHERE id = ?', r.contact_id);
    const skip = !r.enabled ? 'disabled' : !p ? 'no contact' : p.bounced_at ? 'bounced' : !canMarket(p) ? 'no consent' : null;
    if (skip) { db.run('UPDATE automation_runs SET skipped = ? WHERE id = ?', skip, r.id); out.skipped++; continue; }
    const order = r.ref.startsWith('order:') ? r.ref.slice(6) : ''; const unsub = unsubscribeUrl(cfg, p);
    const vars = { name: p.name || 'there', first_name: (p.name || 'there').split(' ')[0], email: p.email, order, product: cfg.product || 'SecuraCV', company: cfg.company || 'Errer Labs', site: cfg.siteOrigin, unsubscribe: unsub };
    enqueue(db, { to: p.email, subject: fill(r.subject, vars), kind: 'automation', ref: `automation:${r.automation_slug}:${r.ref}`, contactId: p.id,
      headers: { 'List-Unsubscribe': `<${unsub}>`, 'List-Unsubscribe-Post': 'List-Unsubscribe=One-Click', Precedence: 'bulk' },
      text: fill(r.body_text, vars) + `\n\n—\nYou're getting this because you confirmed at ${cfg.siteOrigin}. Unsubscribe in one click: ${unsub}\n` });
    db.run('UPDATE automation_runs SET sent_at = ? WHERE id = ?', nowIsoS, r.id); out.sent++;
  }
  if (out.enrolled || out.sent) db.audit('system', 'automation.run', 'automation', null, out);
  return out;
}
export function automationReport(db, slug) {
  const sent = db.all('SELECT r.sent_at, c.email FROM automation_runs r JOIN contacts c ON c.id = r.contact_id WHERE r.automation_slug = ? AND r.sent_at IS NOT NULL', slug);
  let replies = 0, orders = 0, revenue = 0;
  for (const s of sent) { const until = new Date(Date.parse(s.sent_at) + 7 * 86400000).toISOString();
    replies += db.get('SELECT COUNT(*) AS n FROM tickets t JOIN contacts c ON c.id = t.contact_id WHERE c.email = ? AND t.created_at >= ? AND t.created_at < ?', s.email, s.sent_at, until).n;
    const o = db.get('SELECT COUNT(*) AS n, COALESCE(SUM(total_cents - refunded_cents),0) AS c FROM orders o JOIN contacts ct ON ct.id = o.contact_id WHERE ct.email = ? AND o.paid_at >= ? AND o.paid_at < ? AND o.status != \'canceled\'', s.email, s.sent_at, until); orders += o.n; revenue += o.c; }
  return { sent: sent.length, replies, orders, revenue_cents: revenue, window_days: 7 };
}
