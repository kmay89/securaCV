// One SQLite file is the company. node:sqlite (Node ≥ 22.13), no driver.
import { DatabaseSync } from 'node:sqlite';
import { readdirSync, readFileSync, mkdirSync, chmodSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { AsyncLocalStorage } from 'node:async_hooks';

// Who is acting right now. The HTTP pipeline runs each request inside this store with the
// logged-in person's email, so a handler that says actor 'web' is written down under their name.
export const actorStore = new AsyncLocalStorage();
export const currentActor = (fallback = 'web') => actorStore.getStore()?.actor || fallback;

const here = dirname(fileURLToPath(import.meta.url));

export function nowIso() { return new Date().toISOString(); }
export function today() { return new Date().toISOString().slice(0, 10); }

export function openDb(path = ':memory:') {
  if (path !== ':memory:') mkdirSync(dirname(path), { recursive: true });
  const db = new DatabaseSync(path);
  if (path !== ':memory:') { try { chmodSync(path, 0o600); } catch { /* not every filesystem */ } }   // the company's data is the operator's alone
  db.exec('PRAGMA busy_timeout = 5000');
  if (path !== ':memory:') { db.exec('PRAGMA journal_mode = WAL'); db.exec('PRAGMA synchronous = FULL'); }
  migrate(db);
  return wrap(db);
}

function migrate(db) {
  db.exec('CREATE TABLE IF NOT EXISTS schema_migrations (name TEXT PRIMARY KEY, applied_at TEXT NOT NULL)');
  const applied = new Set(db.prepare('SELECT name FROM schema_migrations').all().map(r => r.name));
  const files = readdirSync(join(here, 'migrations')).filter(f => f.endsWith('.sql')).sort();
  for (const f of files) {
    if (applied.has(f)) continue;
    db.exec(readFileSync(join(here, 'migrations', f), 'utf8'));
    db.prepare('INSERT INTO schema_migrations (name, applied_at) VALUES (?, ?)').run(f, nowIso());
  }
}

// A thin convenience layer: db.get/all/run with positional or named params,
// plus transactions and settings. Everything else is plain SQL.
function wrap(db) {
  const cache = new Map(); let depth = 0;
  const stmt = (sql) => { let s = cache.get(sql); if (!s) { s = db.prepare(sql); cache.set(sql, s); } return s; };
  const fix = (p) => p.map(v => (v === undefined ? null : typeof v === 'boolean' ? (v ? 1 : 0) : v));
  const api = {
    raw: db,
    get: (sql, ...p) => stmt(sql).get(...fix(p)) ?? null,
    all: (sql, ...p) => stmt(sql).all(...fix(p)),
    run: (sql, ...p) => stmt(sql).run(...fix(p)),
    exec: (sql) => db.exec(sql),
    // Nested calls become savepoints, so a module can wrap another's work.
    tx(fn) {
      const outer = depth === 0; const sp = `sp${depth}`;
      db.exec(outer ? 'BEGIN' : `SAVEPOINT ${sp}`); depth++;
      try { const r = fn(api); depth--; db.exec(outer ? 'COMMIT' : `RELEASE ${sp}`); return r; }
      catch (e) { depth--; db.exec(outer ? 'ROLLBACK' : `ROLLBACK TO ${sp}; RELEASE ${sp}`); throw e; }
    },
    setting: (key, fallback = null) => api.get('SELECT value FROM settings WHERE key = ?', key)?.value ?? fallback,
    setSetting: (key, value) => api.run(
      'INSERT INTO settings (key, value, updated_at) VALUES (?, ?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at',
      key, String(value), nowIso()),
    // sequential human-facing numbers: EL-000123, T-000045, WC-000012, INV-2026-0007
    nextNumber(prefix, { year = false, width = 6 } = {}) {
      const key = year ? `seq:${prefix}:${new Date().getUTCFullYear()}` : `seq:${prefix}`;
      const n = Number(api.setting(key, '0')) + 1;
      api.setSetting(key, n);
      return year ? `${prefix}-${new Date().getUTCFullYear()}-${String(n).padStart(4, '0')}` : `${prefix}-${String(n).padStart(width, '0')}`;
    },
    audit: (actor, action, entity = null, entityId = null, detail = null) => api.run(
      'INSERT INTO audit_log (at, actor, action, entity, entity_id, detail) VALUES (?, ?, ?, ?, ?, ?)',
      nowIso(), actor === 'web' ? currentActor('web') : actor, action, entity, entityId == null ? null : String(entityId), detail == null ? null : (typeof detail === 'string' ? detail : JSON.stringify(detail))),
    close: () => db.close(),
  };
  return api;
}
