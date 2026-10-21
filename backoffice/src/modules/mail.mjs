// Outbound mail, replacing a transactional sender and a newsletter sender.
// A queue table (mail_outbox) + a tiny SMTP client (STARTTLS or SMTPS, AUTH
// PLAIN/LOGIN) that speaks to any relay — a mailbox provider's SMTP, a
// self-hosted Postfix, or a $0-tier relay. Nothing here phones home.
import { connect as tcpConnect } from 'node:net';
import { connect as tlsConnect } from 'node:tls';
import { randomBytes } from 'node:crypto';
import { nowIso } from '../db.mjs';
import { canMarket, contactByEmail } from './contacts.mjs';

// ---------- message building (RFC 5322 + MIME, text/plain, quoted-printable)
export function qpEncode(str) {
  const bytes = Buffer.from(str.replace(/\r?\n/g, '\r\n'), 'utf8');
  let out = '', line = '';
  const push = (s) => { if (line.length + s.length > 74) { out += line + '=\r\n'; line = ''; } line += s; };
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i];
    if (b === 13 && bytes[i + 1] === 10) { out += line.replace(/[ \t]$/, m => `=${m.charCodeAt(0).toString(16).toUpperCase().padStart(2, '0')}`) + '\r\n'; line = ''; i++; continue; }
    if ((b >= 33 && b <= 126 && b !== 61) || b === 32 || b === 9) push(String.fromCharCode(b));
    else push('=' + b.toString(16).toUpperCase().padStart(2, '0'));
  }
  return out + line;
}
export const encodeHeader = (s) => /^[\x20-\x7e]*$/.test(s) ? s : `=?UTF-8?B?${Buffer.from(s, 'utf8').toString('base64')}?=`;
export const messageId = (domain = 'backoffice.local') => `<${Date.now().toString(36)}.${randomBytes(8).toString('hex')}@${domain}>`;

export function buildMessage({ from, to, subject, text, headers = {}, id = null }) {
  const domain = (String(from).match(/@([^>\s]+)/) || [])[1] || 'backoffice.local';
  const mid = id || messageId(domain);
  const h = [
    `Date: ${new Date().toUTCString().replace('GMT', '+0000')}`,
    `From: ${from}`, `To: ${to}`, `Subject: ${encodeHeader(subject)}`, `Message-ID: ${mid}`,
    'MIME-Version: 1.0', 'Content-Type: text/plain; charset=utf-8', 'Content-Transfer-Encoding: quoted-printable',
    ...Object.entries(headers).filter(([, v]) => v != null && v !== '').map(([k, v]) => `${k}: ${v}`),
  ];
  return { id: mid, raw: h.join('\r\n') + '\r\n\r\n' + qpEncode(text) + '\r\n' };
}

// ---------- SMTP client
function parseSmtpUrl(url) {
  const u = new URL(url);
  return { host: u.hostname, port: Number(u.port) || (u.protocol === 'smtps:' ? 465 : 587), tls: u.protocol === 'smtps:',
    user: decodeURIComponent(u.username || ''), pass: decodeURIComponent(u.password || ''), starttls: u.searchParams.get('starttls') !== 'no' };
}

class LineReader {
  constructor(sock) { this.buf = ''; this.waiters = []; this.lines = []; sock.on('data', d => { this.buf += d.toString('utf8'); this.drain(); }); sock.on('error', e => this.fail(e)); sock.on('close', () => this.fail(new Error('smtp: connection closed'))); }
  drain() { let i; while ((i = this.buf.indexOf('\r\n')) >= 0) { this.lines.push(this.buf.slice(0, i)); this.buf = this.buf.slice(i + 2); } while (this.lines.length && this.waiters.length) this.waiters.shift().resolve(this.lines.shift()); }
  fail(e) { this.err = e; while (this.waiters.length) this.waiters.shift().reject(e); }
  line() { if (this.lines.length) return Promise.resolve(this.lines.shift()); if (this.err) return Promise.reject(this.err); return new Promise((resolve, reject) => this.waiters.push({ resolve, reject })); }
  async reply() { const lines = []; for (;;) { const l = await this.line(); lines.push(l); if (/^\d{3} /.test(l) || !/^\d{3}-/.test(l)) break; } return { code: Number(lines[lines.length - 1].slice(0, 3)), lines }; }
}

export async function smtpSend(url, { from, to, raw }, { timeoutMs = 30_000 } = {}) {
  const o = parseSmtpUrl(url);
  let sock = o.tls ? tlsConnect({ host: o.host, port: o.port, servername: o.host }) : tcpConnect({ host: o.host, port: o.port });
  sock.setTimeout(timeoutMs, () => sock.destroy(new Error('smtp: timeout')));
  let rd = new LineReader(sock);
  const expect = async (ok, what) => { const r = await rd.reply(); if (!ok.includes(r.code)) throw new Error(`smtp ${what}: ${r.lines.join(' | ')}`); return r; };
  const cmd = (s) => { sock.write(s + '\r\n'); };
  await new Promise((res, rej) => { sock.once(o.tls ? 'secureConnect' : 'connect', res); sock.once('error', rej); });
  await expect([220], 'greeting');
  cmd(`EHLO backoffice`); let ehlo = await expect([250], 'EHLO');
  if (!o.tls && o.starttls && ehlo.lines.some(l => /STARTTLS/i.test(l))) {
    cmd('STARTTLS'); await expect([220], 'STARTTLS');
    sock = tlsConnect({ socket: sock, servername: o.host });
    await new Promise((res, rej) => { sock.once('secureConnect', res); sock.once('error', rej); });
    rd = new LineReader(sock);
    cmd('EHLO backoffice'); ehlo = await expect([250], 'EHLO (tls)');
  }
  if (o.user) {
    const auth = ehlo.lines.find(l => /^250[- ]AUTH/i.test(l)) || '';
    if (/PLAIN/i.test(auth) || !/LOGIN/i.test(auth)) { cmd(`AUTH PLAIN ${Buffer.from(`\0${o.user}\0${o.pass}`).toString('base64')}`); await expect([235], 'AUTH PLAIN'); }
    else { cmd('AUTH LOGIN'); await expect([334], 'AUTH LOGIN'); cmd(Buffer.from(o.user).toString('base64')); await expect([334], 'AUTH user'); cmd(Buffer.from(o.pass).toString('base64')); await expect([235], 'AUTH pass'); }
  }
  const addr = (s) => (String(s).match(/<([^>]+)>/) || [, String(s).trim()])[1];
  cmd(`MAIL FROM:<${addr(from)}>`); await expect([250], 'MAIL FROM');
  for (const t of [].concat(to)) { cmd(`RCPT TO:<${addr(t)}>`); await expect([250, 251], 'RCPT TO'); }
  cmd('DATA'); await expect([354], 'DATA');
  sock.write(raw.replace(/\r?\n/g, '\r\n').replace(/^\./gm, '..') + (raw.endsWith('\r\n') ? '' : '\r\n') + '.\r\n');
  await expect([250], 'message');
  cmd('QUIT'); try { await rd.reply(); } catch { /* server may just close */ }
  sock.end();
  return true;
}

// ---------- the outbox
export function enqueue(db, { to, subject, text, kind = 'transactional', ref = null, headers = {} }) {
  const r = db.run('INSERT INTO mail_outbox (to_addr, subject, body_text, headers, kind, ref, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)',
    to, subject, text, JSON.stringify(headers), kind, ref, nowIso());
  return db.get('SELECT * FROM mail_outbox WHERE id = ?', r.lastInsertRowid);
}

// A transport is (message) => Promise; the default is the SMTP client. Tests
// and `--dry-run` swap one in. Campaign mail to anyone without consent is
// suppressed at send time, not just at queue time — consent can be withdrawn
// between the two.
export async function sendOutbox(db, cfg, { transport = null, limit = 50, now = nowIso } = {}) {
  const tx = transport || (cfg.smtpUrl ? (m) => smtpSend(cfg.smtpUrl, m) : null);
  const rows = db.all('SELECT * FROM mail_outbox WHERE status = \'queued\' AND attempts < 5 ORDER BY id LIMIT ?', limit);
  const out = { sent: 0, failed: 0, suppressed: 0, skipped: 0 };
  for (const m of rows) {
    if (m.kind === 'campaign' && !canMarket(contactByEmail(db, m.to_addr))) { db.run('UPDATE mail_outbox SET status = \'suppressed\' WHERE id = ?', m.id); out.suppressed++; continue; }
    if (!tx || !cfg.mailFrom) { out.skipped++; continue; }
    const msg = buildMessage({ from: cfg.mailFrom, to: m.to_addr, subject: m.subject, text: m.body_text, headers: JSON.parse(m.headers || '{}') });
    try {
      await tx({ from: cfg.mailFrom, to: m.to_addr, raw: msg.raw, id: msg.id });
      db.run('UPDATE mail_outbox SET status = \'sent\', sent_at = ?, message_id = ?, attempts = attempts + 1 WHERE id = ?', now(), msg.id, m.id);
      out.sent++;
    } catch (e) {
      db.run('UPDATE mail_outbox SET attempts = attempts + 1, last_error = ?, status = CASE WHEN attempts + 1 >= 5 THEN \'failed\' ELSE \'queued\' END WHERE id = ?', String(e.message || e).slice(0, 500), m.id);
      out.failed++;
    }
  }
  return out;
}

export const outboxStats = (db) => Object.fromEntries(db.all('SELECT status, COUNT(*) AS n FROM mail_outbox GROUP BY status').map(r => [r.status, r.n]));

// {{name}} substitution with a safe fallback — templates are plain text.
export const fill = (tpl, vars) => tpl.replace(/\{\{\s*([a-zA-Z0-9_.]+)\s*\}\}/g, (_, k) => k.split('.').reduce((o, p) => (o == null ? '' : o[p]), vars) ?? '');
