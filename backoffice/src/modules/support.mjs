// The helpdesk. A ticket is a numbered thread with a token that rides in
// every subject line ([T-000045 k3f9ab]) so replies thread back without a
// Message-ID lookup — and the Message-ID lookup is there too. Replaces a
// hosted helpdesk; GitHub issues stay the public front door (doc 20).
import { nowIso } from '../db.mjs';
import { token } from '../http.mjs';
import { upsertContact, validEmail, markBounced } from './contacts.mjs';
import { enqueue, fill } from './mail.mjs';

export const STATUSES = ['open', 'waiting_on_customer', 'waiting_on_us', 'closed'];
export const PRIORITIES = ['low', 'normal', 'high', 'urgent'];
export const KINDS = ['support', 'sales', 'press', 'security', 'warranty', 'feedback', 'build_request', 'wholesale'];
const TOKEN_RE = /\[(T-\d{6}) ([a-z0-9]{6})\]/i;

export function classify(subject = '', body = '') {
  const s = `${subject}\n${body}`.toLowerCase();
  if (/vulnerab|security report|cve-|exploit|disclos/.test(s)) return { kind: 'security', priority: 'urgent' };
  if (/press|journalist|interview|article/.test(s)) return { kind: 'press', priority: 'normal' };
  if (/wholesale|distribut|reseller|bulk order|purchase order|\bpo\b/.test(s)) return { kind: 'wholesale', priority: 'normal' };
  if (/warranty|dead on arrival|doa|missing from|arrived broken|warped/.test(s)) return { kind: 'warranty', priority: 'high' };
  if (/build request|can you build|assemble.*for me/.test(s)) return { kind: 'build_request', priority: 'normal' };
  if (/feedback|suggest|idea:/.test(s)) return { kind: 'feedback', priority: 'low' };
  return { kind: 'support', priority: 'normal' };
}

export function openTicket(db, { email, name = null, subject, body, kind = null, priority = null, channel = 'email', cfg = {}, autoAck = true, messageId = null, inReplyTo = null, fromAddr = null }) {
  const guess = classify(subject, body);
  const contact = email && validEmail(email) ? upsertContact(db, { email, name, source: `support:${channel}` }) : null;
  return db.tx(() => {
    const number = db.nextNumber('T'); const tt = token(4).toLowerCase().replace(/[^a-z0-9]/g, 'x').slice(0, 6).padEnd(6, 'x');
    const now = nowIso();
    const sla = new Date(Date.now() + (cfg.slaHours || 24) * 3600_000).toISOString();
    const r = db.run(`INSERT INTO tickets (number, contact_id, subject, status, priority, channel, kind, created_at, updated_at, sla_due_at, thread_token)
      VALUES (?, ?, ?, 'open', ?, ?, ?, ?, ?, ?, ?)`, number, contact?.id ?? null, String(subject || '(no subject)').slice(0, 200), priority || guess.priority, channel, kind || guess.kind, now, now, sla, tt);
    const id = r.lastInsertRowid;
    addMessage(db, id, { direction: 'in', from_addr: fromAddr || contact?.email || null, body: body || '', message_id: messageId, in_reply_to: inReplyTo });
    // One acknowledgement per address per hour: a form or a mailbox fed someone else's address in a
    // loop would otherwise make this server the one flooding them.
    const ackedLately = contact?.email && db.get("SELECT id FROM mail_outbox WHERE to_addr = ? AND kind = 'support' AND ref LIKE 'ticket:%' AND created_at >= ?", contact.email, new Date(Date.now() - 3600_000).toISOString());
    if (autoAck && contact?.email && !ackedLately) enqueue(db, { to: contact.email, subject: `[${number} ${tt}] Re: ${subject}`, kind: 'support', ref: `ticket:${number}`,
      headers: inReplyToHeaders(messageId), text: fill(ACK_TPL, { name: contact.name || 'there', number, company: cfg.company || 'Errer Labs', hours: cfg.slaHours || 24 }) });
    db.audit('system', 'ticket.open', 'ticket', number, { kind: kind || guess.kind, channel });
    return ticketById(db, id);
  });
}

// A Message-ID is one bracketed token with no whitespace, or nothing: anything else (a CRLF, a second header) is dropped before it can reach a mail header.
export const messageIdOrNull = (s) => (typeof s === 'string' && /^<[^\s<>]{1,500}>$/.test(s.trim())) ? s.trim() : null;
export const inReplyToHeaders = (mid) => { const m = messageIdOrNull(mid); return m ? { 'In-Reply-To': m, References: m } : {}; };

export function addMessage(db, ticketId, { direction, body, from_addr = null, to_addr = null, message_id = null, in_reply_to = null, slaHours = 24 }) {
  message_id = messageIdOrNull(message_id); in_reply_to = messageIdOrNull(in_reply_to);
  const now = nowIso();
  if (message_id && db.get('SELECT id FROM ticket_messages WHERE message_id = ?', message_id)) return null;
  const r = db.run('INSERT INTO ticket_messages (ticket_id, direction, from_addr, to_addr, message_id, in_reply_to, body, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
    ticketId, direction, from_addr, to_addr, message_id, in_reply_to, body, now);
  const t = db.get('SELECT * FROM tickets WHERE id = ?', ticketId);
  if (direction === 'out') db.run('UPDATE tickets SET updated_at = ?, first_response_at = COALESCE(first_response_at, ?), status = CASE WHEN status = \'closed\' THEN \'closed\' ELSE \'waiting_on_customer\' END WHERE id = ?', now, now, ticketId);
  // A customer writing to a closed ticket reopens it as a new question: the clock and the first-response mark
  // start over, so the SLA job sees it as unanswered again. A reply while we wait on them resets the clock only.
  else if (direction === 'in') db.run('UPDATE tickets SET updated_at = ?, first_response_at = CASE WHEN status = \'closed\' THEN NULL ELSE first_response_at END, closed_at = CASE WHEN status = \'closed\' THEN NULL ELSE closed_at END, status = CASE WHEN status = \'closed\' THEN \'open\' ELSE \'waiting_on_us\' END, sla_due_at = CASE WHEN status IN (\'closed\',\'waiting_on_customer\') THEN ? ELSE sla_due_at END WHERE id = ?', now, new Date(Date.now() + (Number(slaHours) || 24) * 3600_000).toISOString(), ticketId);
  else db.run('UPDATE tickets SET updated_at = ? WHERE id = ?', now, ticketId);
  return { ...db.get('SELECT * FROM ticket_messages WHERE id = ?', r.lastInsertRowid), ticket: t };
}

export const ticketById = (db, id) => { const t = db.get('SELECT t.*, c.email, c.name AS contact_name FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id WHERE t.id = ?', id); if (t) t.messages = db.all('SELECT * FROM ticket_messages WHERE ticket_id = ? ORDER BY id', id); return t; };
export const ticketByNumber = (db, n) => { const t = db.get('SELECT id FROM tickets WHERE number = ?', n); return t ? ticketById(db, t.id) : null; };
export const listTickets = (db, { status = '', kind = '', q = '', limit = 300 } = {}) => db.all(
  `SELECT t.*, c.email FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id
   WHERE (? = '' OR t.status = ? OR (? = 'needs' AND t.status IN ('open','waiting_on_us'))) AND (? = '' OR t.kind = ?) AND (? = '' OR t.subject LIKE ? OR c.email LIKE ? OR t.number LIKE ?)
   ORDER BY CASE t.status WHEN 'closed' THEN 1 ELSE 0 END, CASE WHEN t.first_response_at IS NULL AND t.status IN ('open','waiting_on_us') THEN t.sla_due_at ELSE '9' END, CASE t.priority WHEN 'urgent' THEN 0 WHEN 'high' THEN 1 WHEN 'normal' THEN 2 ELSE 3 END, t.updated_at DESC LIMIT ?`,
  status, status, status, kind, kind, q, `%${q}%`, `%${q}%`, `%${q}%`, limit);
export const queue = (db) => db.all('SELECT t.*, c.email FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id WHERE t.status IN (\'open\',\'waiting_on_us\') ORDER BY t.sla_due_at');

export function reply(db, ticketId, body, { cfg = {}, actor = 'web', close = false } = {}) {
  const t = ticketById(db, ticketId); if (!t) throw new Error('no such ticket');
  if (!t.email) throw new Error('ticket has no email to reply to');
  const lastIn = [...t.messages].reverse().find(m => m.direction === 'in' && m.message_id);
  const subj = TOKEN_RE.test(t.subject) ? t.subject : `[${t.number} ${t.thread_token}] Re: ${t.subject}`;
  const m = enqueue(db, { to: t.email, subject: subj, kind: 'support', ref: `ticket:${t.number}`, headers: inReplyToHeaders(lastIn?.message_id), text: body });
  addMessage(db, ticketId, { direction: 'out', to_addr: t.email, body, in_reply_to: lastIn?.message_id || null, message_id: `outbox:${m.id}` });
  if (close) setStatus(db, ticketId, 'closed', { actor });
  db.audit(actor, 'ticket.reply', 'ticket', t.number);
  return ticketById(db, ticketId);
}
export function setStatus(db, ticketId, status, { actor = 'web' } = {}) {
  if (!STATUSES.includes(status)) throw new Error(`bad status ${status}`);
  db.run('UPDATE tickets SET status = ?, updated_at = ?, closed_at = CASE WHEN ? = \'closed\' THEN ? ELSE NULL END WHERE id = ?', status, nowIso(), status, nowIso(), ticketId);
  db.audit(actor, `ticket.${status}`, 'ticket', ticketId);
  return ticketById(db, ticketId);
}
export function update(db, ticketId, { kind, priority, labels }) {
  if (kind && KINDS.includes(kind)) db.run('UPDATE tickets SET kind = ? WHERE id = ?', kind, ticketId);
  if (priority) { if (!PRIORITIES.includes(priority)) throw new Error(`priority is one of ${PRIORITIES.join(', ')}`); db.run('UPDATE tickets SET priority = ? WHERE id = ?', priority, ticketId); }
  if (labels != null) db.run('UPDATE tickets SET labels = ? WHERE id = ?', labels, ticketId);
  return ticketById(db, ticketId);
}

// Inbound mail → the right ticket. Order of matching: thread token in the
// subject, then In-Reply-To / References against our outbound ids, then a
// new ticket. Bounces and auto-replies are filed as notes, never answered.
export function ingestEmail(db, mail, { cfg = {} } = {}) {
  const from = mail.from || ''; const email = (from.match(/<([^>]+)>/) || [, from])[1].trim();
  const name = (from.match(/^\s*"?([^"<]+?)"?\s*</) || [])[1] || null;
  const isAuto = /auto-?reply|automatic reply|out of office|mailer-daemon|postmaster|delivery status|undeliverable/i.test(`${mail.subject} ${from} ${mail.headers?.['auto-submitted'] || ''}`) || /^(auto-generated|auto-replied)/i.test(mail.headers?.['auto-submitted'] || '');
  // A bounce (a delivery status notification) names the address that failed: it is suppressed from marketing mail from here on.
  const isBounce = /mailer-daemon|postmaster|delivery status|undeliverable|delivery has failed|could not be delivered/i.test(`${mail.subject} ${from}`);
  const bounced = [];
  if (isBounce) { const seen = new Set(); for (const [, addr] of String(mail.text || '').matchAll(/([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})/g)) { const a = addr.toLowerCase(); if (seen.has(a) || a === email.toLowerCase() || /mailer-daemon|postmaster/.test(a)) continue; seen.add(a); if (markBounced(db, a, { reason: mail.subject || 'bounce' })) bounced.push(a); } }
  const tok = (mail.subject || '').match(TOKEN_RE);
  let t = tok ? db.get('SELECT * FROM tickets WHERE number = ? AND thread_token = ?', tok[1].toUpperCase(), tok[2].toLowerCase()) : null;
  if (!t) {
    const refs = [mail.inReplyTo, ...(mail.references || [])].filter(Boolean);
    for (const r of refs) { const m = db.get('SELECT ticket_id FROM ticket_messages WHERE message_id = ?', r) || db.get('SELECT ref FROM mail_outbox WHERE message_id = ?', r); if (m?.ticket_id) { t = db.get('SELECT * FROM tickets WHERE id = ?', m.ticket_id); break; } if (m?.ref?.startsWith('ticket:')) { t = db.get('SELECT * FROM tickets WHERE number = ?', m.ref.slice(7)); break; } }
  }
  if (t) {
    const added = addMessage(db, t.id, { direction: isAuto ? 'note' : 'in', from_addr: email, body: (isAuto ? '[auto-reply] ' : '') + (mail.text || ''), message_id: mail.messageId || null, in_reply_to: mail.inReplyTo || null, slaHours: cfg.slaHours || 24 });
    if (added && !isAuto && mail.attachments?.length) saveAttachments(db, added.id, mail.attachments);
    return { ticket: ticketById(db, t.id), created: false, duplicate: !added, attachments: added && mail.attachments ? mail.attachments.length : 0 };
  }
  if (isAuto) return { ticket: null, created: false, ignored: isBounce ? 'bounce' : 'auto-reply', bounced };
  if (mail.messageId && db.get('SELECT id FROM ticket_messages WHERE message_id = ?', mail.messageId)) return { ticket: null, created: false, duplicate: true };
  const ticket = openTicket(db, { email, name, subject: mail.subject || '(no subject)', body: mail.text || '', channel: 'email', cfg, messageId: mail.messageId || null, inReplyTo: mail.inReplyTo || null, fromAddr: email });
  if (mail.attachments?.length) saveAttachments(db, ticket.messages[0].id, mail.attachments);
  return { ticket, created: true, attachments: mail.attachments?.length || 0 };
}

// --- a minimal RFC 5322 / MIME reader: enough for the plain-text part of
// what people and mail clients send. HTML-only mail is tag-stripped.
export function parseEmail(raw) {
  const txt = Buffer.isBuffer(raw) ? raw.toString('latin1') : raw;
  const sep = txt.indexOf('\r\n\r\n') >= 0 ? '\r\n\r\n' : '\n\n';
  const i = txt.indexOf(sep); const head = i < 0 ? txt : txt.slice(0, i); const body = i < 0 ? '' : txt.slice(i + sep.length);
  const headers = parseHeaders(head);
  const ct = headers['content-type'] || 'text/plain';
  const text = extractText(body, ct, headers['content-transfer-encoding'] || '7bit');
  const refs = (headers.references || '').match(/<[^>]+>/g) || [];
  return { headers, from: decodeWords(headers.from || ''), to: decodeWords(headers.to || ''), subject: decodeWords(headers.subject || ''), messageId: (headers['message-id'] || '').trim() || null,
    inReplyTo: ((headers['in-reply-to'] || '').match(/<[^>]+>/) || [])[0] || null, references: refs, date: headers.date || null, text: text.trim(), attachments: extractAttachments(body, ct) };
}
// Photos of a warped lid, a PDF of a purchase order: the parts a person attached, kept with the message.
// Images, PDFs and text only; 5 MB each, five per mail; anything else is named in the text and dropped.
export const ATTACHMENT_TYPES = /^(image\/(png|jpeg|gif|webp|heic)|application\/pdf|text\/plain|text\/csv)$/i;
export function extractAttachments(body, ct, { max = 5, maxBytes = 5 * 1024 * 1024, out = [] } = {}) {
  const type = ct.split(';')[0].trim().toLowerCase(); if (!type.startsWith('multipart/') || out.length >= max) return out;
  const boundary = (ct.match(/boundary="?([^";]+)"?/i) || [])[1]; if (!boundary) return out;
  const parts = body.split(new RegExp(`(?:\\r?\\n)?--${boundary.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?:--)?(?:\\r?\\n|$)`)).slice(1).filter(p => p.trim());
  for (const p of parts) {
    if (out.length >= max) break;
    const sep = p.indexOf('\r\n\r\n') >= 0 ? '\r\n\r\n' : '\n\n'; const i = p.indexOf(sep); const h = parseHeaders(i < 0 ? p : p.slice(0, i)); const b = i < 0 ? '' : p.slice(i + sep.length);
    const pct = h['content-type'] || 'text/plain'; const ptype = pct.split(';')[0].trim().toLowerCase();
    if (ptype.startsWith('multipart/')) { extractAttachments(b, pct, { max, maxBytes, out }); continue; }
    const disp = h['content-disposition'] || ''; const filename = decodeWords(((disp.match(/filename\*?="?([^";]+)"?/i) || pct.match(/name="?([^";]+)"?/i) || [])[1] || '').replace(/^UTF-8''/i, '')).trim();
    if (!/attachment/i.test(disp) && !filename) continue; if (!ATTACHMENT_TYPES.test(ptype)) continue;
    const cte = (h['content-transfer-encoding'] || '7bit').toLowerCase().trim();
    const bytes = cte === 'base64' ? Buffer.from(b.replace(/\s+/g, ''), 'base64') : cte === 'quoted-printable' ? Buffer.from(b.replace(/=\r?\n/g, '').replace(/=([0-9A-Fa-f]{2})/g, (_, x) => String.fromCharCode(parseInt(x, 16))), 'latin1') : Buffer.from(b, 'latin1');
    if (!bytes.length || bytes.length > maxBytes) continue;
    out.push({ filename: (filename || `attachment-${out.length + 1}`).replace(/[\\/\u0000-\u001f]/g, '_').slice(0, 120), mime: ptype, bytes });
  }
  return out;
}
export function saveAttachments(db, messageId, attachments = []) { let n = 0; for (const a of attachments) { db.run('INSERT INTO attachments (ticket_message_id, filename, mime, size, bytes, created_at) VALUES (?, ?, ?, ?, ?, ?)', messageId, a.filename, a.mime, a.bytes.length, a.bytes, nowIso()); n++; } return n; }
export const attachmentsFor = (db, ticketId) => db.all('SELECT a.id, a.ticket_message_id, a.filename, a.mime, a.size FROM attachments a JOIN ticket_messages m ON m.id = a.ticket_message_id WHERE m.ticket_id = ? ORDER BY a.id', ticketId);
export const attachment = (db, id) => { const a = db.get('SELECT a.*, m.ticket_id FROM attachments a JOIN ticket_messages m ON m.id = a.ticket_message_id WHERE a.id = ?', id); if (a) a.bytes = Buffer.from(a.bytes); return a; };
function parseHeaders(head) {
  const out = {}; let last = null;
  for (const line of head.split(/\r?\n/)) {
    if (/^[ \t]/.test(line) && last) { out[last] += ' ' + line.trim(); continue; }
    const m = line.match(/^([!-9;-~]+):\s*(.*)$/); if (!m) continue;
    last = m[1].toLowerCase(); out[last] = out[last] ? out[last] + ', ' + m[2] : m[2];
  }
  return out;
}
export function decodeWords(s) {
  return s.replace(/=\?([^?]+)\?([bBqQ])\?([^?]*)\?=/g, (_, cs, enc, data) => {
    const buf = enc.toUpperCase() === 'B' ? Buffer.from(data, 'base64') : Buffer.from(data.replace(/_/g, ' ').replace(/=([0-9A-Fa-f]{2})/g, (__, h) => String.fromCharCode(parseInt(h, 16))), 'latin1');
    return decodeCharset(buf, cs);
  }).replace(/\?=\s+=\?/g, '?==?');
}
const decodeCharset = (buf, cs) => { try { return new TextDecoder(cs.toLowerCase()).decode(buf); } catch { return buf.toString('utf8'); } };
function decodeBody(body, cte, cs) {
  const e = cte.toLowerCase().trim();
  if (e === 'base64') return decodeCharset(Buffer.from(body.replace(/\s+/g, ''), 'base64'), cs);
  if (e === 'quoted-printable') return decodeCharset(Buffer.from(body.replace(/=\r?\n/g, '').replace(/=([0-9A-Fa-f]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), 'latin1'), cs);
  return decodeCharset(Buffer.from(body, 'latin1'), cs);
}
function extractText(body, ct, cte) {
  const type = ct.split(';')[0].trim().toLowerCase();
  const cs = (ct.match(/charset="?([^";\s]+)"?/i) || [, 'utf-8'])[1];
  if (type.startsWith('multipart/')) {
    const boundary = (ct.match(/boundary="?([^";]+)"?/i) || [])[1]; if (!boundary) return '';
    const parts = body.split(new RegExp(`(?:\\r?\\n)?--${boundary.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?:--)?(?:\\r?\\n|$)`)).slice(1).filter(p => p.trim());
    const parsed = parts.map(p => { const sep = p.indexOf('\r\n\r\n') >= 0 ? '\r\n\r\n' : '\n\n'; const i = p.indexOf(sep); const h = parseHeaders(i < 0 ? p : p.slice(0, i)); return { h, b: i < 0 ? '' : p.slice(i + sep.length) }; });
    const plain = parsed.find(p => (p.h['content-type'] || 'text/plain').toLowerCase().startsWith('text/plain'));
    if (plain) return extractText(plain.b, plain.h['content-type'] || 'text/plain', plain.h['content-transfer-encoding'] || '7bit');
    for (const p of parsed) { const t = extractText(p.b, p.h['content-type'] || 'text/plain', p.h['content-transfer-encoding'] || '7bit'); if (t.trim()) return t; }
    return '';
  }
  const decoded = decodeBody(body, cte, cs).replace(/\r\n/g, '\n');
  if (type === 'text/html') return htmlToText(decoded);
  if (type.startsWith('text/')) return decoded;
  return '';
}

// HTML-only mail → readable text. Not a sanitizer: the result is stored and
// always HTML-escaped on output. A character walk, so there is no tag regex
// to get half right; script/style bodies are dropped, block ends become
// newlines, entities are decoded exactly once at the end.
export function htmlToText(html) {
  let out = '', i = 0; const n = html.length;
  while (i < n) {
    if (html[i] !== '<') { out += html[i++]; continue; }
    const close = html.indexOf('>', i); if (close < 0) break;
    const tag = html.slice(i + 1, close).trim().toLowerCase();
    const name = tag.replace(/^\//, '').split(/[\s/]/)[0];
    if ((name === 'script' || name === 'style') && !tag.startsWith('/')) {
      const end = html.toLowerCase().indexOf('</' + name, close); i = end < 0 ? n : html.indexOf('>', end) + 1 || n; continue;
    }
    if (name === 'br' || tag === '/p' || tag === '/div' || tag === '/li' || tag === '/tr' || /^\/h[1-6]$/.test(tag)) out += '\n';
    i = close + 1;
  }
  return out.replace(/&(nbsp|amp|lt|gt|quot|#39|#x27);/g, (_, e) => ({ nbsp: ' ', amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'", '#x27': "'" }[e]));
}

export const ticketCounts = (db) => { const out = { open: 0, waiting_on_us: 0, waiting_on_customer: 0, closed: 0 }; for (const r of db.all('SELECT status, COUNT(*) AS n FROM tickets GROUP BY status')) out[r.status] = r.n; out.needs = out.open + out.waiting_on_us; out.all = out.needs + out.waiting_on_customer + out.closed; return out; };
export const supportStats = (db) => ({
  open: db.get('SELECT COUNT(*) AS n FROM tickets WHERE status IN (\'open\',\'waiting_on_us\')').n,
  breached: db.get('SELECT COUNT(*) AS n FROM tickets WHERE status IN (\'open\',\'waiting_on_us\') AND first_response_at IS NULL AND sla_due_at < ?', nowIso()).n,
  medianFirstResponseH: (() => { const xs = db.all('SELECT (julianday(first_response_at) - julianday(created_at)) * 24 AS h FROM tickets WHERE first_response_at IS NOT NULL AND created_at >= ? ORDER BY h', new Date(Date.now() - 30 * 86400000).toISOString()).map(r => r.h); return xs.length ? Math.round(xs[Math.floor(xs.length / 2)] * 10) / 10 : null; })(),
});
export const slaBreaches = (db) => db.all('SELECT t.*, c.email FROM tickets t LEFT JOIN contacts c ON c.id = t.contact_id WHERE t.status IN (\'open\',\'waiting_on_us\') AND t.first_response_at IS NULL AND t.sla_due_at < ? ORDER BY t.sla_due_at', nowIso());

const ACK_TPL = `Hi {{name}},

Got it — this is ticket {{number}}. A person (not a bot) reads every one; you'll hear back within {{hours}} hours, usually sooner. Keep the ticket number in the subject and replies land in the same thread.

— {{company}}
`;
