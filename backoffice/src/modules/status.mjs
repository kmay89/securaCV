// Status page + uptime monitor. Probes are plain HTTP checks (status code,
// optional body text) on an interval; three consecutive failures raise an
// exception, recovery resolves it. The public page says what is up with
// 90-day uptime from the probe log, and incidents the operator writes.
import { nowIso } from '../db.mjs';
import { raise, resolve } from './exceptions.mjs';

export const DEFAULT_PROBES = (cfg) => [
  { name: 'Website', url: cfg.siteOrigin + '/', expect_text: 'SecuraCV' },
  { name: 'Store catalog', url: cfg.siteOrigin + '/store.json', expect_text: '"groups"' },
  { name: 'The Lab (browser flasher)', url: 'https://kmay89.github.io/securaCV/canary-local/', expect_status: 200 },
  { name: 'Firmware releases (OTA host)', url: 'https://github.com/kmay89/securaCV/releases', expect_status: 200 },
  { name: 'Backoffice forms', url: cfg.baseUrl + '/healthz', expect_text: 'ok', public: 0 },
];

export function upsertProbe(db, p) {
  db.run(`INSERT INTO probes (name, url, expect_status, expect_text, interval_s, enabled, public) VALUES (?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(name) DO UPDATE SET url = excluded.url, expect_status = excluded.expect_status, expect_text = excluded.expect_text, interval_s = excluded.interval_s, enabled = excluded.enabled, public = excluded.public`,
    p.name, p.url, p.expect_status || 200, p.expect_text || null, p.interval_s || 300, p.enabled ?? 1, p.public ?? 1);
  return db.get('SELECT * FROM probes WHERE name = ?', p.name);
}
export const listProbes = (db, { publicOnly = false } = {}) => db.all('SELECT * FROM probes WHERE (? = 0 OR public = 1) ORDER BY id', publicOnly ? 1 : 0);
export const deleteProbe = (db, id) => db.run('DELETE FROM probes WHERE id = ?', id).changes;

export async function runProbe(db, p, { fetchImpl = fetch, cfg = {}, now = nowIso } = {}) {
  const t0 = Date.now(); let ok = false, detail = '';
  try {
    const ctl = new AbortController(); const timer = setTimeout(() => ctl.abort(), 15_000);
    const r = await fetchImpl(p.url, { signal: ctl.signal, redirect: 'follow', headers: { 'user-agent': 'securacv-backoffice-probe/1' } });
    clearTimeout(timer);
    const body = p.expect_text ? await r.text() : '';
    ok = r.status === p.expect_status && (!p.expect_text || body.includes(p.expect_text));
    detail = ok ? `${r.status}` : `${r.status}${p.expect_text && !body.includes(p.expect_text) ? ` (missing "${p.expect_text}")` : ''}`;
  } catch (e) { detail = String(e.name === 'AbortError' ? 'timeout' : e.message || e).slice(0, 200); }
  const ms = Date.now() - t0; const at = now();
  db.run('INSERT INTO probe_results (probe_id, at, ok, ms, detail) VALUES (?, ?, ?, ?, ?)', p.id, at, ok ? 1 : 0, ms, detail);
  db.run('UPDATE probes SET last_ok = ?, last_ms = ?, last_at = ?, last_detail = ?, fails = CASE WHEN ? THEN 0 ELSE fails + 1 END WHERE id = ?', ok ? 1 : 0, ms, at, detail, ok ? 1 : 0, p.id);
  const fails = db.get('SELECT fails FROM probes WHERE id = ?', p.id).fails;
  if (!ok && fails === 3) await raise(db, cfg, `probe:${p.name}`, `status [down]: ${p.name}`, `${p.url} has failed 3 checks in a row.\nLast result: ${detail}\n\nThe public status page shows it red until it recovers.`);
  if (ok) await resolve(db, cfg, `probe:${p.name}`, `Recovered at ${at} (${ms} ms).`);
  return { ok, ms, detail };
}

export async function runDueProbes(db, opts = {}) {
  const out = [];
  for (const p of listProbes(db).filter(p => p.enabled)) {
    const due = !p.last_at || (Date.now() - Date.parse(p.last_at)) >= p.interval_s * 1000;
    if (due || opts.force) out.push({ name: p.name, ...(await runProbe(db, p, opts)) });
  }
  db.run('DELETE FROM probe_results WHERE at < ?', new Date(Date.now() - 100 * 86400000).toISOString());
  return out;
}

export function uptime(db, probeId, days = 90) {
  const r = db.get('SELECT COUNT(*) AS n, SUM(ok) AS ok FROM probe_results WHERE probe_id = ? AND at >= ?', probeId, new Date(Date.now() - days * 86400000).toISOString());
  return r.n ? Math.round((r.ok / r.n) * 10000) / 100 : null;
}

export function openIncident(db, { title, body = '', status = 'investigating' }) {
  const r = db.run('INSERT INTO incidents (title, status, body, started_at, updated_at) VALUES (?, ?, ?, ?, ?)', title, status, body, nowIso(), nowIso());
  return db.get('SELECT * FROM incidents WHERE id = ?', r.lastInsertRowid);
}
export function updateIncident(db, id, { status, body }) {
  db.run('UPDATE incidents SET status = COALESCE(?, status), body = COALESCE(?, body), updated_at = ?, resolved_at = CASE WHEN ? = \'resolved\' THEN ? ELSE resolved_at END WHERE id = ?', status || null, body || null, nowIso(), status || '', nowIso(), id);
  return db.get('SELECT * FROM incidents WHERE id = ?', id);
}
export const listIncidents = (db, { openOnly = false, limit = 20 } = {}) => db.all('SELECT * FROM incidents WHERE (? = 0 OR status != \'resolved\') ORDER BY id DESC LIMIT ?', openOnly ? 1 : 0, limit);

export function publicStatus(db) {
  const probes = listProbes(db, { publicOnly: true }).map(p => ({ name: p.name, ok: p.last_ok == null ? null : !!p.last_ok, checked_at: p.last_at, ms: p.last_ms, uptime_90d: uptime(db, p.id) }));
  const incidents = listIncidents(db, { limit: 10 });
  const degraded = probes.some(p => p.ok === false) || incidents.some(i => i.status !== 'resolved');
  return { overall: degraded ? 'degraded' : 'operational', generated_at: nowIso(), probes, incidents };
}
