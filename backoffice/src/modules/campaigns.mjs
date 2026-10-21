// Newsletters and "back in stock" mail. Campaign mail goes only to contacts
// with double-opt-in consent and carries List-Unsubscribe (+ one-click,
// RFC 8058) and a plain-text unsubscribe link; the outbox suppresses anyone
// who withdrew consent between queue and send. Waitlist notifications are
// the one transactional exception: one mail, because they asked for it.
import { nowIso } from '../db.mjs';
import { segment, waitlistFor, requestConsent, contactById } from './contacts.mjs';
import { enqueue, fill } from './mail.mjs';

export const unsubscribeUrl = (cfg, c) => `${cfg.baseUrl}/u/${c.unsubscribe_token}`;
export const confirmUrl = (cfg, t) => `${cfg.baseUrl}/confirm/${t}`;

export function createCampaign(db, { slug, subject, body_text, segment: seg = 'all' }) {
  db.run('INSERT INTO campaigns (slug, subject, body_text, segment, created_at) VALUES (?, ?, ?, ?, ?)', slug, subject, body_text, seg, nowIso());
  return db.get('SELECT * FROM campaigns WHERE slug = ?', slug);
}
export const listCampaigns = (db) => db.all('SELECT * FROM campaigns ORDER BY id DESC');
export const campaign = (db, slug) => db.get('SELECT * FROM campaigns WHERE slug = ?', slug);

export function queueCampaign(db, slug, cfg, { actor = 'web' } = {}) {
  const c = campaign(db, slug); if (!c) throw new Error('no such campaign');
  if (c.status === 'sent') throw new Error('already sent');
  const people = segment(db, c.segment);
  db.tx(() => {
    for (const p of people) {
      const unsub = unsubscribeUrl(cfg, p);
      enqueue(db, { to: p.email, subject: c.subject, kind: 'campaign', ref: `campaign:${slug}`,
        headers: { 'List-Unsubscribe': `<${unsub}>`, 'List-Unsubscribe-Post': 'List-Unsubscribe=One-Click', Precedence: 'bulk' },
        text: fill(c.body_text, { name: p.name || 'there', email: p.email, unsubscribe: unsub }) + `\n\n—\nYou're getting this because you confirmed at ${cfg.siteOrigin}. Unsubscribe in one click: ${unsub}\n` });
    }
    db.run('UPDATE campaigns SET status = \'sent\', sent_at = ?, recipients = ? WHERE id = ?', nowIso(), people.length, c.id);
  });
  db.audit(actor, 'campaign.queue', 'campaign', slug, { recipients: people.length });
  return people.length;
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
