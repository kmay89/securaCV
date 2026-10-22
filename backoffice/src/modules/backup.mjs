// Backups you can trust because they are checked, not assumed. Every backup
// is a consistent SQLite copy (VACUUM INTO) that is then opened and proven:
// integrity_check, the migrations present, row counts, the ledger balanced.
// With BACKUP_S3_URL set the copy also goes off-site to any S3-compatible
// bucket (Backblaze B2, Cloudflare R2, Tigris, AWS) with a HEAD afterwards
// to confirm the bytes landed, and a weekly drill pulls the latest copy
// back down and proves it boots. Restore is one command. Zero dependencies:
// the AWS Signature V4 is ~40 lines of node:crypto.
import { mkdirSync, readdirSync, unlinkSync, statSync, copyFileSync, readFileSync, writeFileSync, chmodSync } from 'node:fs';
import { join, basename, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { createHash, createHmac, scryptSync, randomBytes, createCipheriv, createDecipheriv } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';

export function backup(db, dir, { keep = 14, now = new Date() } = {}) {
  mkdirSync(dir, { recursive: true });
  const name = `backoffice-${now.toISOString().replace(/[:.]/g, '-').slice(0, 23)}.db`;   // to the millisecond: two "back up now" presses never collide
  const path = join(dir, name);
  db.raw.exec(`VACUUM INTO '${path.replace(/'/g, "''")}'`);
  try { chmodSync(path, 0o600); } catch { /* not every filesystem */ }
  const files = readdirSync(dir).filter(f => /^backoffice-.*\.db$/.test(f)).sort();
  for (const f of files.slice(0, Math.max(0, files.length - keep))) unlinkSync(join(dir, f));
  return { path, bytes: statSync(path).size, kept: Math.min(files.length, keep) };
}
export const listBackups = (dir) => { try { return readdirSync(dir).filter(f => /^backoffice-.*\.db$/.test(f)).sort().reverse().map(f => ({ name: f, bytes: statSync(join(dir, f)).size })); } catch { return []; } };

// Open a copy and prove it: a backup that cannot pass this is not a backup.
export function checkBackup(path, { expect = null } = {}) {
  const out = { path, bytes: 0, ok: false, integrity: null, migrations: 0, counts: {}, balanced: null, latest_at: null, errors: [] };
  try { out.bytes = statSync(path).size; } catch (e) { out.errors.push(`missing: ${e.message}`); return out; }
  let raw;
  try {
    raw = new DatabaseSync(path, { readOnly: true });
    out.integrity = raw.prepare('PRAGMA integrity_check').get().integrity_check;
    if (out.integrity !== 'ok') out.errors.push(`integrity_check: ${out.integrity}`);
    out.migrations = raw.prepare('SELECT COUNT(*) AS n FROM schema_migrations').get().n;
    for (const t of ['orders', 'contacts', 'tickets', 'ledger', 'inventory_items', 'devices', 'audit_log']) out.counts[t] = raw.prepare(`SELECT COUNT(*) AS n FROM ${t}`).get().n;
    const l = raw.prepare('SELECT COALESCE(SUM(debit_cents),0) AS d, COALESCE(SUM(credit_cents),0) AS c FROM ledger').get();
    out.balanced = l.d === l.c; if (!out.balanced) out.errors.push(`ledger unbalanced: debits ${l.d} credits ${l.c}`);
    out.latest_at = raw.prepare('SELECT MAX(at) AS at FROM audit_log').get().at;
    if (expect) for (const [t, n] of Object.entries(expect)) if (out.counts[t] !== n) out.errors.push(`${t}: copy has ${out.counts[t]}, live has ${n}`);
  } catch (e) { out.errors.push(String(e.message || e)); } finally { try { raw?.close(); } catch { /* closed */ } }
  out.ok = out.errors.length === 0;
  return out;
}
export const liveCounts = (db) => Object.fromEntries(['orders', 'contacts', 'tickets', 'ledger', 'inventory_items', 'devices', 'audit_log'].map(t => [t, db.get(`SELECT COUNT(*) AS n FROM ${t}`).n]));

// Restore: check the file first, then put it in place (the old file is kept beside it).
export function restore(src, destDbPath, { now = new Date() } = {}) {
  const v = checkBackup(src); if (!v.ok) throw new Error(`refusing to restore an unchecked copy: ${v.errors.join('; ')}`);
  mkdirSync(dirname(destDbPath), { recursive: true });
  let previous = null;
  try { statSync(destDbPath); previous = `${destDbPath}.replaced-${now.toISOString().replace(/[:.]/g, '-').slice(0, 19)}`; copyFileSync(destDbPath, previous); } catch { /* nothing to keep */ }
  for (const suffix of ['-wal', '-shm']) { try { unlinkSync(destDbPath + suffix); } catch { /* none */ } }
  copyFileSync(src, destDbPath);
  return { restored: destDbPath, from: src, previous, check: v };
}

// ---------- off-site: S3-compatible, AWS Signature V4, no SDK
const sha256 = (x) => createHash('sha256').update(x).digest('hex');
const hmac = (k, x) => createHmac('sha256', k).update(x).digest();
const encodeSeg = (s) => encodeURIComponent(s).replace(/[!'()*]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
export function signS3({ method, url, headers = {}, payloadHash, key, secret, region = 'us-east-1', service = 's3', date = new Date() }) {
  const u = new URL(url);
  const amzDate = date.toISOString().replace(/[:-]|\.\d{3}/g, ''); const day = amzDate.slice(0, 8);
  const h = { host: u.host, 'x-amz-content-sha256': payloadHash, 'x-amz-date': amzDate, ...Object.fromEntries(Object.entries(headers).map(([k, v]) => [k.toLowerCase(), String(v).trim()])) };
  const signed = Object.keys(h).sort();
  const canonicalUri = u.pathname.split('/').map(encodeSeg).join('/') || '/';
  const canonicalQuery = [...u.searchParams.entries()].map(([k, v]) => [encodeSeg(k), encodeSeg(v)]).sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : 1).map(([k, v]) => `${k}=${v}`).join('&');
  const canonical = [method, canonicalUri, canonicalQuery, signed.map(k => `${k}:${h[k]}\n`).join(''), signed.join(';'), payloadHash].join('\n');
  const scope = `${day}/${region}/${service}/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', amzDate, scope, sha256(canonical)].join('\n');
  const kSigning = hmac(hmac(hmac(hmac(`AWS4${secret}`, day), region), service), 'aws4_request');
  const signature = createHmac('sha256', kSigning).update(toSign).digest('hex');
  return { ...h, authorization: `AWS4-HMAC-SHA256 Credential=${key}/${scope}, SignedHeaders=${signed.join(';')}, Signature=${signature}`, signature };
}
export const offsiteConfigured = (cfg) => !!(cfg.backupS3Url && cfg.backupS3Key && cfg.backupS3Secret);
export const offsiteEncrypted = (cfg) => !!cfg.backupEncryptionKey;

// Customer names and addresses never sit in a bucket in the clear: with
// BACKUP_ENCRYPTION_KEY set, the copy is sealed with AES-256-GCM under a key
// derived from that passphrase before it leaves the machine, and the drill
// opens it the same way. Format: "BOENC1" + 12-byte IV + ciphertext + 16-byte tag.
const MAGIC = Buffer.from('BOENC1');
const sealKey = (pass) => scryptSync(String(pass), 'backoffice-backup-v1', 32, { N: 16384, r: 8, p: 1 });
export function seal(buf, pass) {
  const iv = randomBytes(12); const c = createCipheriv('aes-256-gcm', sealKey(pass), iv);
  const body = Buffer.concat([c.update(buf), c.final()]);
  return Buffer.concat([MAGIC, iv, body, c.getAuthTag()]);
}
export function unseal(buf, pass) {
  if (!buf.subarray(0, 6).equals(MAGIC)) throw new Error('not a sealed backup');
  const iv = buf.subarray(6, 18), tag = buf.subarray(buf.length - 16), body = buf.subarray(18, buf.length - 16);
  const d = createDecipheriv('aes-256-gcm', sealKey(pass), iv); d.setAuthTag(tag);
  return Buffer.concat([d.update(body), d.final()]);   // a wrong key or a flipped byte throws here
}
const objectUrl = (cfg, name) => `${cfg.backupS3Url.replace(/\/$/, '')}/${name}`;

async function s3(cfg, method, name, { body = null, fetchImpl = fetch, extra = {} } = {}) {
  const url = objectUrl(cfg, name); const payloadHash = body ? sha256(body) : sha256('');
  const headers = signS3({ method, url, headers: { ...(body ? { 'content-length': String(body.length) } : {}), ...extra }, payloadHash, key: cfg.backupS3Key, secret: cfg.backupS3Secret, region: cfg.backupS3Region || 'us-east-1' });
  const r = await fetchImpl(url, { method, headers, body: body || undefined });
  return r;
}
// PUT the file and a `latest.txt` pointer; then HEAD it and compare sizes — "uploaded" means "readable back".
export async function uploadOffsite(cfg, path, { fetchImpl = fetch } = {}) {
  if (!offsiteConfigured(cfg)) return { skipped: 'BACKUP_S3_URL / KEY / SECRET not set' };
  const plain = readFileSync(path); const sealed = offsiteEncrypted(cfg);
  const body = sealed ? seal(plain, cfg.backupEncryptionKey) : plain; const name = basename(path) + (sealed ? '.enc' : '');
  const put = await s3(cfg, 'PUT', name, { body, fetchImpl, extra: { 'content-type': sealed ? 'application/octet-stream' : 'application/vnd.sqlite3' } });
  if (!put.ok) throw new Error(`off-site PUT ${put.status}: ${(await put.text().catch(() => '')).slice(0, 200)}`);
  const head = await s3(cfg, 'HEAD', name, { fetchImpl });
  const len = Number(head.headers.get('content-length'));
  if (!head.ok || len !== body.length) throw new Error(`off-site HEAD ${head.status}: size ${len} ≠ ${body.length}`);
  const ptr = await s3(cfg, 'PUT', 'latest.txt', { body: Buffer.from(name), fetchImpl, extra: { 'content-type': 'text/plain' } });
  if (!ptr.ok) throw new Error(`off-site pointer PUT ${ptr.status}`);
  return { url: objectUrl(cfg, name), name, bytes: body.length, etag: head.headers.get('etag'), read_back: true, encrypted: sealed };
}
// The drill: pull the latest copy back, open it, prove it. Nothing in the live database is touched.
export async function restoreDrill(cfg, { fetchImpl = fetch, now = new Date(), localDir = null } = {}) {
  let path, source;
  if (offsiteConfigured(cfg)) {
    const ptr = await s3(cfg, 'GET', 'latest.txt', { fetchImpl }); if (!ptr.ok) throw new Error(`drill: no latest.txt off-site (${ptr.status})`);
    const name = (await ptr.text()).trim(); if (!/^backoffice-[0-9T-]+\.db(\.enc)?$/.test(name)) throw new Error(`drill: bad pointer ${JSON.stringify(name)}`);
    const get = await s3(cfg, 'GET', name, { fetchImpl }); if (!get.ok) throw new Error(`drill: GET ${name} ${get.status}`);
    let bytes = Buffer.from(await get.arrayBuffer());
    if (name.endsWith('.enc')) { if (!cfg.backupEncryptionKey) throw new Error('drill: the off-site copy is sealed and BACKUP_ENCRYPTION_KEY is not set'); bytes = unseal(bytes, cfg.backupEncryptionKey); }
    path = join(tmpdir(), `drill-${process.pid}-${name.replace(/\.enc$/, '')}`); writeFileSync(path, bytes, { mode: 0o600 }); source = objectUrl(cfg, name);
  } else {
    const latest = listBackups(localDir)[0]; if (!latest) throw new Error('drill: no backup exists yet');
    path = join(localDir, latest.name); source = path;
  }
  try {
    const v = checkBackup(path);
    const stamp = basename(path).match(/backoffice-(\d{4}-\d{2}-\d{2})T(\d{2})-(\d{2})-(\d{2})/);
    const taken = stamp ? Date.parse(`${stamp[1]}T${stamp[2]}:${stamp[3]}:${stamp[4]}Z`) : NaN;
    const ageH = Number.isFinite(taken) ? (now - taken) / 3600000 : null;
    if (ageH != null && ageH > 48) v.errors.push(`the latest copy is ${Math.round(ageH)} h old`);
    v.ok = v.errors.length === 0;
    return { ok: v.ok, source, offsite: offsiteConfigured(cfg), encrypted: offsiteConfigured(cfg) && source.endsWith('.enc'), age_h: ageH == null ? null : Math.round(ageH * 10) / 10, at: now.toISOString(), ...v };
  } finally { if (offsiteConfigured(cfg)) { try { unlinkSync(path); } catch { /* gone */ } } }
}
