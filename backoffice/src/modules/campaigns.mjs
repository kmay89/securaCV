// Newsletters and "back in stock" mail. Campaign mail goes only to contacts
// with double-opt-in consent and carries List-Unsubscribe (+ one-click,
// RFC 8058) and a plain-text unsubscribe link; the outbox suppresses anyone
// who withdrew consent between queue and send. Waitlist notifications are
// the one transactional exception: one mail, because they asked for it.
import { nowIso } from '../db.mjs';
import { segment, segmentCount, waitlistFor, requestConsent, contactById } from './contacts.mjs';
import { enqueue, fill, renderHtml, brandOpts } from './mail.mjs';

export const unsubscribeUrl = (cfg, c) => `${cfg.baseUrl}/u/${c.unsubscribe_token}`;
export const confirmUrl = (cfg, t) => `${cfg.baseUrl}/confirm/${t}`;

export function createCampaign(db, { slug, subject, body_text, segment: seg = 'all', preheader = null }) {
  if (!/^[a-z0-9][a-z0-9-]{1,60}$/.test(String(slug || ''))) throw new Error('a slug is lowercase letters, digits and dashes');
  segmentCount(db, seg);   // an unknown segment is refused here, not at send time
  db.run('INSERT INTO campaigns (slug, subject, body_text, segment, preheader, created_at) VALUES (?, ?, ?, ?, ?, ?)', slug, subject, body_text, seg, preheader || null, nowIso());
  return db.get('SELECT * FROM campaigns WHERE slug = ?', slug);
}
export function updateCampaign(db, slug, { subject, body_text, segment: seg, preheader }) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign'); if (c.status === 'sent') throw new Error('a sent campaign is history — duplicate it instead');
  if (seg != null) segmentCount(db, seg);
  db.run('UPDATE campaigns SET subject = COALESCE(?, subject), body_text = COALESCE(?, body_text), segment = COALESCE(?, segment), preheader = COALESCE(?, preheader) WHERE slug = ?', subject ?? null, body_text ?? null, seg ?? null, preheader ?? null, slug);
  return campaign(db, slug);
}
export function duplicateCampaign(db, slug) { const c = campaign(db, slug); if (!c) throw new Error('no such campaign'); let n = 2; while (campaign(db, `${slug}-${n}`)) n++; return createCampaign(db, { slug: `${slug}-${n}`, subject: c.subject, body_text: c.body_text, segment: c.segment, preheader: c.preheader }); }
export const listCampaigns = (db) => db.all('SELECT c.*, (SELECT COUNT(*) FROM mail_outbox m WHERE m.ref = \'campaign:\' || c.slug AND m.status = \'sent\') AS delivered FROM campaigns c ORDER BY CASE c.status WHEN \'draft\' THEN 0 WHEN \'scheduled\' THEN 1 ELSE 2 END, c.id DESC');
export const campaign = (db, slug) => db.get('SELECT * FROM campaigns WHERE slug = ?', slug);
const MERGE = (p, cfg) => ({ name: p.name || 'there', first_name: (p.name || 'there').split(' ')[0], email: p.email, city: p.city || '', unsubscribe: unsubscribeUrl(cfg, p), site: cfg.siteOrigin, company: cfg.company || 'Errer Labs' });
const FOOT = (cfg, unsub) => `\n\n—\nYou're getting this because you confirmed at ${cfg.siteOrigin}. Unsubscribe in one click: ${unsub}\n`;

export function queueCampaign(db, slug, cfg, { actor = 'web' } = {}) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign');
  if (c.status === 'sent') throw new Error('already sent');
  const people = segment(db, c.segment);
  db.tx(() => {
    for (const p of people) {
      const unsub = unsubscribeUrl(cfg, p);
      enqueue(db, { to: p.email, subject: fill(c.subject, MERGE(p, cfg)), kind: 'campaign', ref: `campaign:${slug}`, contactId: p.id,
        headers: { 'List-Unsubscribe': `<${unsub}>`, 'List-Unsubscribe-Post': 'List-Unsubscribe=One-Click', Precedence: 'bulk', ...(c.preheader ? { 'X-Preheader': c.preheader } : {}) },
        text: fill(c.body_text, MERGE(p, cfg)) + FOOT(cfg, unsub) });
    }
    db.run('UPDATE campaigns SET status = \'sent\', sent_at = ?, scheduled_at = NULL, recipients = ? WHERE id = ?', nowIso(), people.length, c.id);
  });
  db.audit(actor, 'campaign.queue', 'campaign', slug, { recipients: people.length });
  return people.length;
}
// Schedule: the outbox job sends it when the time comes; unschedule returns it to draft.
export function scheduleCampaign(db, slug, at, { actor = 'web' } = {}) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign'); if (c.status === 'sent') throw new Error('already sent');
  const t = at ? new Date(at) : null; if (at && Number.isNaN(t.getTime())) throw new Error('bad time'); if (t && t.getTime() < Date.now() - 60_000) throw new Error('that time has passed — send it now instead');
  db.run('UPDATE campaigns SET status = ?, scheduled_at = ? WHERE slug = ?', t ? 'scheduled' : 'draft', t ? t.toISOString() : null, slug);
  db.audit(actor, t ? 'campaign.schedule' : 'campaign.unschedule', 'campaign', slug, { at: t?.toISOString() });
  return campaign(db, slug);
}
export function sendDueCampaigns(db, cfg) { const due = db.all('SELECT slug FROM campaigns WHERE status = \'scheduled\' AND scheduled_at <= ?', nowIso()); return due.map(c => ({ slug: c.slug, recipients: queueCampaign(db, c.slug, cfg, { actor: 'scheduler' }) })); }
// A test send: the operator sees exactly what a subscriber will, filled in for a sample person.
export function sendTest(db, slug, to, cfg, { actor = 'web' } = {}) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign'); if (!to) throw new Error('set OPERATOR_EMAIL, or type an address');
  // Filled in with a real subscriber's name so the merge tags read true — but the unsubscribe link is inert:
  // a token nobody holds, so clicking it in the test can never unsubscribe a customer.
  const first = segment(db, c.segment)[0]; const sample = { name: first?.name || 'Test Reader', email: first?.email || to, unsubscribe_token: 'test-send-inert' };
  const m = enqueue(db, { to, subject: `[TEST] ${fill(c.subject, MERGE(sample, cfg))}`, kind: 'campaign-test', ref: `campaign-test:${slug}`, headers: { 'List-Unsubscribe': `<${unsubscribeUrl(cfg, sample)}>`, ...(c.preheader ? { 'X-Preheader': c.preheader } : {}) }, text: fill(c.body_text, MERGE(sample, cfg)) + FOOT(cfg, unsubscribeUrl(cfg, sample)) });
  db.audit(actor, 'campaign.test', 'campaign', slug, { to }); return m;
}
export function previewHtml(db, slug, cfg) { const c = campaign(db, slug); if (!c) throw new Error('no such campaign'); const sample = { name: 'Sample Reader', email: 'reader@example.test', unsubscribe_token: 'preview' }; return renderHtml(fill(c.body_text, MERGE(sample, cfg)) + FOOT(cfg, unsubscribeUrl(cfg, sample)), brandOpts(cfg, { unsubscribe: unsubscribeUrl(cfg, sample), preheader: c.preheader || '' })); }
// The report, without a single tracking pixel: what was delivered, who left, and what the recipients did in the week after — orders and replies, matched by address.
export function campaignReport(db, slug) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign');
  const m = db.get('SELECT COUNT(*) AS queued, SUM(status = \'sent\') AS sent, SUM(status = \'suppressed\') AS suppressed, SUM(status = \'failed\') AS failed, SUM(status = \'queued\') AS pending FROM mail_outbox WHERE ref = ?', `campaign:${slug}`);
  if (!c.sent_at) return { campaign: c, ...m, unsubscribed: 0, orders: 0, revenue_cents: 0, replies: 0 };
  const until = new Date(Date.parse(c.sent_at) + 7 * 86400000).toISOString();
  const recips = `SELECT DISTINCT to_addr FROM mail_outbox WHERE ref = 'campaign:${slug.replace(/'/g, "''")}' AND status = 'sent'`;
  const unsubscribed = db.get(`SELECT COUNT(*) AS n FROM contacts WHERE email IN (${recips}) AND unsubscribed_at >= ? AND unsubscribed_at < ?`, c.sent_at, until).n;
  const o = db.get(`SELECT COUNT(*) AS n, COALESCE(SUM(o.total_cents - o.refunded_cents),0) AS c FROM orders o JOIN contacts ct ON ct.id = o.contact_id WHERE ct.email IN (${recips}) AND o.paid_at >= ? AND o.paid_at < ? AND o.status != 'canceled'`, c.sent_at, until);
  const replies = db.get(`SELECT COUNT(*) AS n FROM tickets t JOIN contacts ct ON ct.id = t.contact_id WHERE ct.email IN (${recips}) AND t.created_at >= ? AND t.created_at < ?`, c.sent_at, until).n;
  return { campaign: c, ...m, unsubscribed, orders: o.n, revenue_cents: o.c, replies, window_days: 7 };
}

// Double opt-in step 1: the confirm mail.
export function sendConsentRequest(db, contactId, cfg) {
  const c = contactById(db, contactId); if (!c) throw new Error('no such contact');
  const t = requestConsent(db, contactId);
  enqueue(db, { to: c.email, subject: `Confirm: occasional ${cfg.product || 'SecuraCV'} news`, kind: 'transactional', ref: `consent:${c.id}`,
    text: `Someone (we hope you) asked for occasional ${cfg.product || 'SecuraCV'} news at ${cfg.siteOrigin}.\n\nConfirm here: ${confirmUrl(cfg, t)}\n\nIf that wasn't you, ignore this — nothing is sent without the click. No tracking pixels, ever; every mail has a one-click unsubscribe.\n\n— ${cfg.company || 'Errer Labs'}\n` });
  return t;
}

// Back in stock: everyone waiting on a SKU gets one mail, once.
export function notifyWaitlist(db, sku, cfg, { productName = sku, url = `${cfg.siteOrigin}/store`, actor = 'web' } = {}) {
  const rows = waitlistFor(db, sku);
  db.tx(() => {
    for (const w of rows) {
      enqueue(db, { to: w.email, subject: `${productName} is in stock`, kind: 'transactional', ref: `waitlist:${sku}`,
        text: `Hi ${w.name || 'there'},\n\nYou asked to hear when ${productName} came back — it's in stock now, in this batch:\n${url}\n\nThis is the one notification you asked for; you won't get another unless you join the waitlist again.\n\n— ${cfg.company || 'Errer Labs'}\n` });
      db.run('UPDATE waitlist SET notified_at = ? WHERE id = ?', nowIso(), w.id);
    }
  });
  db.audit(actor, 'waitlist.notify', 'waitlist', sku, { n: rows.length });
  return rows.length;
}
