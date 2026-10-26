// A minimal IMAP client: enough to pull unseen messages from one folder and
// mark them seen. LOGIN · SELECT · UID SEARCH UNSEEN · UID FETCH BODY.PEEK[]
// · UID STORE +FLAGS \Seen. Implicit TLS (imaps://…:993) only — every
// provider offers it and it keeps the client small.
import { connect as tlsConnect } from 'node:tls';
import { connect as tcpConnect } from 'node:net';

export function parseImapUrl(url) {
  const u = new URL(url);
  return { host: u.hostname, port: Number(u.port) || 993, tls: u.protocol !== 'imap:', user: decodeURIComponent(u.username), pass: decodeURIComponent(u.password), folder: decodeURIComponent(u.pathname.replace(/^\//, '')) || 'INBOX' };
}

class Imap {
  constructor(sock) { this.sock = sock; this.buf = Buffer.alloc(0); this.n = 0; this.waiters = []; sock.on('data', d => { this.buf = Buffer.concat([this.buf, d]); this.pump(); }); sock.on('error', e => this.fail(e)); sock.on('close', () => this.fail(new Error('imap: closed'))); }
  fail(e) { this.err = e; while (this.waiters.length) this.waiters.shift().reject(e); }
  pump() { while (this.waiters.length) { const w = this.waiters[0]; const r = w.tryParse(); if (!r) break; this.waiters.shift(); w.resolve(r); } }
  // Read one complete response block for a tag: lines until "<tag> OK|NO|BAD", honoring {n} literals.
  cmd(line) {
    const tag = 'A' + String(++this.n).padStart(4, '0');
    this.sock.write(`${tag} ${line}\r\n`);
    return new Promise((resolve, reject) => {
      const w = { resolve, reject, tryParse: () => {
        const lines = []; let pos = 0;
        for (;;) {
          const nl = this.buf.indexOf('\r\n', pos); if (nl < 0) return null;
          let line = this.buf.subarray(pos, nl).toString('latin1'); let next = nl + 2;
          const lit = line.match(/\{(\d+)\}$/);
          if (lit) { const len = Number(lit[1]); if (this.buf.length < next + len) return null; lines.push({ text: line, literal: this.buf.subarray(next, next + len) }); next += len; const rest = this.buf.indexOf('\r\n', next); if (rest < 0) return null; lines[lines.length - 1].tail = this.buf.subarray(next, rest).toString('latin1'); next = rest + 2; }
          else lines.push({ text: line });
          pos = next;
          if (line.startsWith(tag + ' ')) { this.buf = this.buf.subarray(pos); const m = line.match(/^A\d+ (OK|NO|BAD)\b(.*)$/); return { ok: m?.[1] === 'OK', status: m?.[1], msg: m?.[2]?.trim() || '', lines }; }
        }
      } };
      this.waiters.push(w); if (this.err) reject(this.err); else this.pump();
    });
  }
}

export async function imapFetchUnseen(url, { limit = 50, markSeen = true, timeoutMs = 60_000 } = {}) {
  const o = parseImapUrl(url);
  const sock = o.tls ? tlsConnect({ host: o.host, port: o.port, servername: o.host }) : tcpConnect({ host: o.host, port: o.port });
  sock.setTimeout(timeoutMs, () => sock.destroy(new Error('imap: timeout')));
  await new Promise((res, rej) => { sock.once(o.tls ? 'secureConnect' : 'connect', res); sock.once('error', rej); });
  const c = new Imap(sock);
  // greeting: an untagged "* OK" line; consume it by waiting for the first command's block to include it.
  const quote = (s) => `"${String(s).replace(/(["\\])/g, '\\$1')}"`;
  let r = await c.cmd(`LOGIN ${quote(o.user)} ${quote(o.pass)}`); if (!r.ok) { sock.end(); throw new Error(`imap login: ${r.msg}`); }
  r = await c.cmd(`SELECT ${quote(o.folder)}`); if (!r.ok) { sock.end(); throw new Error(`imap select ${o.folder}: ${r.msg}`); }
  const validity = (r.lines.map(l => l.text).join('\n').match(/UIDVALIDITY (\d+)/) || [])[1] || '0';
  r = await c.cmd('UID SEARCH UNSEEN'); if (!r.ok) { sock.end(); throw new Error(`imap search: ${r.msg}`); }
  const uids = (r.lines.map(l => l.text).find(t => t.startsWith('* SEARCH')) || '').split(/\s+/).slice(2).filter(Boolean).slice(0, limit);
  const out = [];
  const MAX = 8 * 1024 * 1024;
  for (const uid of uids) {
    // the size before the body: a message over the cap is never pulled into memory (one 64 MB message used to cost 26 s of CPU and the container's memory)
    const sz = await c.cmd(`UID FETCH ${uid} (RFC822.SIZE)`); const n = Number((sz.lines.map(l => l.text).join(' ').match(/RFC822\.SIZE (\d+)/) || [])[1]);
    if (Number.isFinite(n) && n > MAX) { out.push({ uid: `${validity}:${uid}`, raw: null, skipped: `message is ${Math.round(n / 1048576)} MB, over the ${MAX / 1048576} MB cap` }); if (markSeen) await c.cmd(`UID STORE ${uid} +FLAGS (\\Seen)`); continue; }
    r = await c.cmd(`UID FETCH ${uid} (BODY.PEEK[])`); if (!r.ok) continue;
    const lit = r.lines.find(l => l.literal); if (!lit) continue;
    out.push({ uid: `${validity}:${uid}`, raw: lit.literal });
    if (markSeen) await c.cmd(`UID STORE ${uid} +FLAGS (\\Seen)`);
  }
  await c.cmd('LOGOUT').catch(() => {});
  sock.end();
  return out;
}
