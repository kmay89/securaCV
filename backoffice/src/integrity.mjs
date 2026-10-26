// Knowing it has not been breached, and being able to say so. Four things, each stated once:
//
//   verifyChain     — walks the audit log and proves every row is the one that was written, in the order it
//                     was written, under the key outside the database; an anchor (yesterday's head, mailed
//                     off the box in the digest) is checked against it, so even a rewrite of the whole file and
//                     the key beside it is caught by the one copy the attacker could not reach
//   securitySummary — the last 24 hours as a person would ask about them: who signed in from where, what
//                     failed, who was blocked, what changed about people, tokens and settings, what is open now
//   lockdown        — "I think I have been breached": every session ended, every token revoked, every sign-in
//                     link spent, in one transaction, audited
//   preflight       — what the server proves about itself before it listens: the key is private, the data dir
//                     is private, this Node is still maintained, the chain's tail verifies
import { statSync } from 'node:fs';
import { auditMac, CHAIN_GENESIS } from './db.mjs';

// ---- the chain ----
// Walks every row by id. Rows from before the chain began (no mark) are counted and skipped; the first marked
// row must chain from 'genesis'. Stops at the first break and names the row.
export function verifyChain(db, { anchor = null, key = db.auditKey?.key, from = 1 } = {}) {
  const out = { ok: true, rows: 0, chained: 0, unchained: 0, first_id: null, head: null, broken_at: null, reason: null, anchor: null };
  if (!key) return { ...out, ok: false, reason: 'no audit key' };
  let prev = CHAIN_GENESIS; let seenChain = false;
  const stmt = db.raw.prepare('SELECT id, at, actor, action, entity, entity_id, detail, prev, mac FROM audit_log WHERE id >= ? ORDER BY id');
  for (const r of stmt.iterate(from)) {
    out.rows++;
    if (r.mac == null) { if (seenChain) { out.ok = false; out.broken_at = r.id; out.reason = 'an unmarked row after the chain began'; break; } out.unchained++; continue; }
    if (!seenChain) { seenChain = true; out.first_id = r.id; }
    if (r.prev !== prev) { out.ok = false; out.broken_at = r.id; out.reason = `prev does not match the row before it${r.prev === CHAIN_GENESIS ? ' (a second genesis: rows removed before it)' : ''}`; break; }
    if (auditMac(key, r, r.prev) !== r.mac) { out.ok = false; out.broken_at = r.id; out.reason = 'the mark does not match the row (changed, or marked under another key)'; break; }
    prev = r.mac; out.chained++; out.head = { id: r.id, mac: r.mac, at: r.at };
  }
  if (out.ok && anchor) {
    // "id:mac" from a digest or a verify run: the chain must still pass through that row with that mark
    const m = /^(\d+):([A-Za-z0-9_-]+)$/.exec(String(anchor).trim());
    if (!m) out.anchor = { ok: false, reason: 'an anchor is id:mac' };
    else { const row = db.get('SELECT id, mac FROM audit_log WHERE id = ?', Number(m[1])); out.anchor = { id: Number(m[1]), ok: !!row && row.mac === m[2], reason: !row ? 'no such row any more' : row.mac === m[2] ? null : 'that row carries another mark now' }; if (!out.anchor.ok) out.ok = false; }
  }
  return out;
}
// Re-marks every row in order under the key. For SYNTHETIC history only: the demo seeder backdates and prunes
// its own rows and must say so. There is no path to this from a page or the CLI; the call is written down as a row.
export function rechain(db, { synthetic = false, why = '' } = {}) {
  if (!synthetic) throw new Error('rechain is for synthetic history only (the demo); a real record is never re-marked');
  return db.tx(() => {
    let prev = CHAIN_GENESIS; let n = 0; const upd = db.raw.prepare('UPDATE audit_log SET prev = ?, mac = ? WHERE id = ?');
    for (const r of db.raw.prepare('SELECT id, at, actor, action, entity, entity_id, detail FROM audit_log ORDER BY id').iterate()) { const mac = auditMac(db.auditKey.key, r, prev); upd.run(prev, mac, r.id); prev = mac; n++; }
    db.audit('system', 'audit.rechain', 'audit', null, { rows: n, why: String(why).slice(0, 200) });
    return n;
  });
}
export const chainHead = (db) => db.get('SELECT id, mac, at FROM audit_log WHERE mac IS NOT NULL ORDER BY id DESC LIMIT 1');
export const anchorText = (h) => h ? `${h.id}:${h.mac}` : null;
// The last verify run, as the doctor and the digest read it (settings 'chain:last', written by the verify job).
export const lastVerify = (db) => { try { return JSON.parse(db.setting('chain:last') || 'null'); } catch { return null; } };

// ---- the last 24 hours, as questions a person asks ----
export function securitySummary(db, cfg, { hours = 24, now = Date.now() } = {}) {
  const since = new Date(now - hours * 3600_000).toISOString();
  const rows = db.all("SELECT at, actor, action, entity_id, detail FROM audit_log WHERE at >= ? AND (action LIKE 'auth.%' OR action LIKE 'guard.%' OR action LIKE 'user.%' OR action LIKE 'flag.%' OR action LIKE 'notify.%' OR action IN ('deploy.boot', 'lockdown', 'health.external_monitor', 'backup.restore')) ORDER BY id", since);
  const d = (r) => { try { return JSON.parse(r.detail || '{}'); } catch { return {}; } };
  const by = (pred) => rows.filter(pred);
  const logins = by(r => r.action === 'auth.login' || r.action === 'auth.link.login').map(r => ({ at: r.at, who: r.entity_id, ip: d(r).ip || '-', via: d(r).via || 'password' }));
  const failed = by(r => ['auth.login.failed', 'auth.link.failed', 'auth.login.locked'].includes(r.action));
  const failedBy = {}; for (const r of failed) { const ip = d(r).ip || '-'; failedBy[ip] = (failedBy[ip] || 0) + 1; }
  const blocks = by(r => r.action === 'guard.block' || r.action === 'guard.tripwire').map(r => ({ at: r.at, ip: r.entity_id, why: r.action === 'guard.tripwire' ? `tripwire ${d(r).path || ''}` : 'scanner' }));
  const lockouts = by(r => r.action === 'guard.lockout').map(r => r.entity_id);
  const newNets = by(r => r.action === 'auth.login.new_network').map(r => ({ who: r.entity_id, net: d(r).net || d(r).ip || '-' }));
  const moved = by(r => r.action === 'auth.session.moved').length;
  const links = by(r => r.action === 'auth.link.requested').length;
  const changes = by(r => /^(user\.|flag\.|notify\.|auth\.token\.)/.test(r.action) || ['lockdown', 'health.external_monitor', 'backup.restore', 'deploy.boot'].includes(r.action)).map(r => ({ at: r.at, who: r.actor, what: r.action, on: r.entity_id }));
  const sessions = db.all('SELECT s.net, s.created_at, s.expires_at, u.email FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.expires_at > ? ORDER BY s.created_at DESC', new Date(now).toISOString());
  const tokens = db.all('SELECT name, scope, last_used_at FROM api_tokens ORDER BY name');
  const tokensUsed = tokens.filter(t => t.last_used_at && t.last_used_at >= since);
  const chain = lastVerify(db); const head = chainHead(db);
  const quiet = !failed.length && !blocks.length && !lockouts.length && !newNets.length && !moved && !changes.filter(c => c.what !== 'deploy.boot').length;
  return { hours, since, logins, failed: failed.length, failedBy, blocks, lockouts, newNets, moved, links, changes, sessions, tokens, tokensUsed, chain, head: head ? { id: head.id, mac: head.mac, at: head.at } : null, quiet };
}
// The SECURITY block of the digest, and `backoffice verify`'s report: plain text, one screen, the anchor last.
export function securityText(db, cfg, opts = {}) {
  const s = securitySummary(db, cfg, opts);
  const L = [];
  L.push(`SECURITY — the last ${s.hours} h ${s.quiet ? '— quiet' : ''}`.trim());
  L.push(`  sign-ins      ${s.logins.length ? s.logins.map(l => `${l.who} from ${l.ip}${l.via === 'link' ? ' (link)' : ''}`).join('; ') : 'none'}`);
  L.push(`  failed        ${s.failed ? `${s.failed} (${Object.entries(s.failedBy).map(([ip, n]) => `${ip}: ${n}`).join(', ')})` : 'none'}${s.lockouts.length ? ` · locked out: ${[...new Set(s.lockouts)].join(', ')}` : ''}`);
  L.push(`  blocked       ${s.blocks.length ? s.blocks.map(b => `${b.ip} (${b.why})`).join('; ') : 'nobody'}`);
  if (s.newNets.length) L.push(`  new networks  ${s.newNets.map(n => `${n.who} from ${n.net}`).join('; ')}`);
  if (s.moved) L.push(`  sessions ended for moving networks: ${s.moved}`);
  if (s.links) L.push(`  sign-in links asked for: ${s.links}`);
  L.push(`  changes       ${s.changes.length ? s.changes.map(c => `${c.what} ${c.on || ''} by ${c.who}`).join('; ') : 'none to people, tokens, flags or settings'}`);
  L.push(`  open now      ${s.sessions.length} session${s.sessions.length === 1 ? '' : 's'}${s.sessions.length ? ` (${s.sessions.map(x => `${x.email} on ${x.net || '?'}`).join('; ')})` : ''} · ${s.tokens.length} API token${s.tokens.length === 1 ? '' : 's'}${s.tokensUsed.length ? `, used: ${s.tokensUsed.map(t => t.name).join(', ')}` : ''}`);
  L.push(`  audit chain   ${s.chain ? `${s.chain.ok ? 'verified' : `BROKEN at row ${s.chain.broken_at}: ${s.chain.reason}`} ${s.chain.at ? `(${s.chain.at.slice(0, 16)}Z)` : ''} · ${s.chain.chained} rows` : 'not yet verified (the verify job runs daily)'}`);
  L.push(`  anchor        ${s.head ? `${s.head.id}:${s.head.mac}` : 'none yet'}`);
  L.push(`                keep this line; \`backoffice verify --anchor ${s.head ? `${s.head.id}:${s.head.mac}` : 'id:mac'}\` proves the record still passes through it`);
  return L.join('\n');
}

// ---- the lockdown ----
// One transaction: every session gone (the owner's too — they sign in again with password + authenticator),
// every API token revoked, every live sign-in link spent. Nothing about people, data or settings changes.
export function lockdown(db, { actor = 'cli', why = '' } = {}) {
  return db.tx(() => {
    const sessions = db.run('DELETE FROM sessions').changes;
    const tokens = db.all('SELECT name FROM api_tokens').map(r => r.name); db.run('DELETE FROM api_tokens');
    const links = db.run('UPDATE login_links SET used_at = ? WHERE used_at IS NULL', new Date().toISOString()).changes;
    db.audit(actor, 'lockdown', 'security', null, { sessions, tokens, links, why: String(why || '').slice(0, 300) });
    return { sessions, tokens, links };
  });
}

// ---- tripwires ----
// Paths no customer and no page here ever asks for; one request is a scanner, blocked at once and pushed about.
export const TRIPWIRE = /^\/(wp-login\.php|wp-admin(\/|$)|xmlrpc\.php|\.env|\.git(\/|$)|\.aws(\/|$)|\.ssh(\/|$)|phpmyadmin(\/|$)|admin\.php|config\.php|\.DS_Store|backup\.(zip|sql|tar\.gz)|server-status|actuator(\/|$)|cgi-bin(\/|$)|vendor\/phpunit)/i;

// ---- what the server proves about itself before it listens ----
// Node's maintenance ends (nodejs.org/en/about/previous-releases); a build past it is a doctor failure, 90 days before a warning.
export const NODE_EOL = { 22: '2027-04-30', 24: '2028-04-30', 26: '2029-04-30' };
export function nodeSupport(version = process.versions.node, now = Date.now()) {
  const major = Number(String(version).split('.')[0]); const eol = NODE_EOL[major];
  if (!eol) return { major, eol: null, ok: major > Math.max(...Object.keys(NODE_EOL).map(Number)) ? null : false, text: major < 22 ? `Node ${version} is below the 22.13 this needs` : `Node ${version}: no end-of-life date on record here` };
  const days = Math.round((Date.parse(eol) - now) / 86400000);
  return { major, eol, days, ok: days > 90 ? true : days > 0 ? null : false, text: days > 0 ? `Node ${version} is maintained until ${eol} (${days} days)` : `Node ${version} reached end of life on ${eol}` };
}
const mode = (p) => { try { return statSync(p).mode & 0o777; } catch { return null; } };
export function preflight(db, cfg, { now = Date.now() } = {}) {
  const rows = []; const add = (key, ok, text, fix = '') => rows.push({ key, ok, text, fix });
  // The demo runs wherever it is put (a laptop's data dir, a throwaway container): what is about the host is said, not failed.
  const host = (ok) => (cfg.demo && ok === false ? null : ok);
  const k = db.auditKey || {};
  add('audit_key', host(!!k.source && k.source !== 'memory'), k.source === 'BACKOFFICE_AUDIT_KEY' ? 'audit chain key from the environment' : k.source ? `audit chain key at ${k.source}${k.created ? ' (made now)' : ''}` : 'no audit chain key', 'set BACKOFFICE_AUDIT_KEY, or let the server write audit.key beside the database');
  const km = k.source && k.source !== 'memory' && k.source !== 'BACKOFFICE_AUDIT_KEY' ? mode(k.source) : null;
  if (km != null) add('audit_key_mode', host((km & 0o077) === 0), `audit.key mode ${km.toString(8)}`, 'chmod 600 audit.key');
  const dm = mode(cfg.dataDir); if (dm != null) add('data_dir_mode', host((dm & 0o007) === 0), `${cfg.dataDir} mode ${dm.toString(8)}${(dm & 0o007) ? ' — readable by everyone on the box' : ''}`, `chmod 700 ${cfg.dataDir}`);
  const bm = cfg.dbPath !== ':memory:' ? mode(cfg.dbPath) : null; if (bm != null) add('db_mode', host((bm & 0o077) === 0), `database mode ${bm.toString(8)}`, `chmod 600 ${cfg.dbPath}`);
  const n = nodeSupport(process.versions.node, now); add('node', n.ok, n.text, 'move to the current LTS: FROM node:<lts>-slim, or the distribution package');
  const tail = db.get('SELECT MAX(id) AS id FROM audit_log')?.id || 0; const v = verifyChain(db, { from: Math.max(1, tail - 2000) });
  add('chain_tail', v.ok, v.ok ? `the last ${v.chained} audit rows verify${v.unchained ? ` (${v.unchained} from before the chain)` : ''}` : `audit chain BROKEN at row ${v.broken_at}: ${v.reason}`, 'backoffice verify; compare the anchor in yesterday\'s digest; SECURITY.md "Knowing it has not been breached"');
  if (cfg.env === 'production') add('allowlist_or_bind', !!(cfg.adminAllowCidrs || cfg.sessionBindIp), cfg.adminAllowCidrs ? `admin allowlisted to ${cfg.adminAllowCidrs}` : cfg.sessionBindIp ? 'sessions bound to their network' : 'neither ADMIN_ALLOW_CIDRS nor SESSION_BIND_IP is set', 'one of the two makes a stolen cookie worth far less');
  return { ok: rows.every(r => r.ok !== false), rows, at: new Date(now).toISOString() };
}
