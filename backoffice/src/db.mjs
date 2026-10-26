// One SQLite file is the company. node:sqlite (Node ≥ 22.13), no driver.
import { DatabaseSync } from 'node:sqlite';
import { readdirSync, readFileSync, mkdirSync, chmodSync, unlinkSync, statSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { AsyncLocalStorage } from 'node:async_hooks';

// Who is acting right now. The HTTP pipeline runs each request inside this store with the
// logged-in person's email, so a handler that says actor 'web' is written down under their name.
export const actorStore = new AsyncLocalStorage();
export const currentActor = (fallback = 'web') => actorStore.getStore()?.actor || fallback;

const here = dirname(fileURLToPath(import.meta.url));

export function nowIso() { return new Date().toISOString(); }
export function today() { return new Date().toISOString().slice(0, 10); }

// migrationsDir and allowNewerSchema are for tests and for a rescue: BACKOFFICE_ALLOW_NEWER_SCHEMA=1 lets an
// older build open a database a newer one migrated (to read or back it up), which is otherwise refused.
export function openDb(path = ':memory:', { migrationsDir = join(here, 'migrations'), allowNewerSchema = process.env.BACKOFFICE_ALLOW_NEWER_SCHEMA === '1', keepPreMigrate = 5 } = {}) {
  if (path !== ':memory:') mkdirSync(dirname(path), { recursive: true });
  const db = new DatabaseSync(path);
  if (path !== ':memory:') { try { chmodSync(path, 0o600); } catch { /* not every filesystem */ } }   // the company's data is the operator's alone
  db.exec('PRAGMA busy_timeout = 5000');
  if (path !== ':memory:') { db.exec('PRAGMA journal_mode = WAL'); db.exec('PRAGMA synchronous = FULL'); }
  const schema = migrate(db, { path, migrationsDir, allowNewerSchema, keepPreMigrate });
  const api = wrap(db); api.schema = schema;
  return api;
}

// Migrations are forward-only and additive by rule (migrations/README.md). Two guards make a deploy that
// goes wrong recoverable instead of silent:
//   - before the first pending migration touches a file database, a checked copy of it goes to
//     backups/pre-migrate-<stamp>-<migration>.db (VACUUM INTO: consistent, no lock held), the newest
//     keepPreMigrate of them kept — so "roll back the image and put the old file back" is one command;
//   - a database that carries migrations this build does not know is refused: an older image started on a
//     newer schema would run with columns and tables it never heard of, and write half of what the newer
//     code expects. The message names the migrations and says to start the newer build or restore.
function migrate(db, { path, migrationsDir, allowNewerSchema, keepPreMigrate }) {
  db.exec('CREATE TABLE IF NOT EXISTS schema_migrations (name TEXT PRIMARY KEY, applied_at TEXT NOT NULL)');
  const applied = new Set(db.prepare('SELECT name FROM schema_migrations').all().map(r => r.name));
  const files = readdirSync(migrationsDir).filter(f => f.endsWith('.sql')).sort();
  const known = new Set(files); const ahead = [...applied].filter(n => !known.has(n)).sort();
  if (ahead.length && !allowNewerSchema) throw new Error(`database schema is ahead of this build: ${ahead.join(', ')} ${ahead.length === 1 ? 'was' : 'were'} applied by a newer version — start that version again, or restore a backup taken before it (BACKOFFICE_ALLOW_NEWER_SCHEMA=1 opens it read-mostly for a rescue)`);
  const pending = files.filter(f => !applied.has(f));
  let preMigrate = null;
  if (pending.length && applied.size && path !== ':memory:') {
    const dir = join(dirname(path), 'backups'); mkdirSync(dir, { recursive: true });
    const stamp = new Date().toISOString().replace(/[:.]/g, '-').replace('T', 'T').slice(0, 23);
    preMigrate = join(dir, `pre-migrate-${stamp}-${basename(pending[0], '.sql')}.db`);
    db.exec(`VACUUM INTO '${preMigrate.replace(/'/g, "''")}'`);
    try { chmodSync(preMigrate, 0o600); } catch { /* not every filesystem */ }
    const old = readdirSync(dir).filter(f => /^pre-migrate-.*\.db$/.test(f)).sort().reverse().slice(keepPreMigrate);
    for (const f of old) { try { unlinkSync(join(dir, f)); } catch { /* already gone */ } }
  }
  for (const f of files) {
    if (applied.has(f)) continue;
    // One file, one transaction: a crash halfway (docker's ten seconds, a full disk) leaves nothing
    // applied, so the next boot runs the file again instead of failing on a column that already exists.
    // PRAGMAs cannot run inside it (openDb sets the journal mode itself), so they are left out here.
    const sql = readFileSync(join(migrationsDir, f), 'utf8').replace(/^\s*PRAGMA[^;]*;[^\n]*$/gm, '');
    db.exec('BEGIN');
    try { db.exec(sql); db.prepare('INSERT INTO schema_migrations (name, applied_at) VALUES (?, ?)').run(f, nowIso()); db.exec('COMMIT'); }
    catch (e) { try { db.exec('ROLLBACK'); } catch { /* nothing to roll back */ } throw new Error(`migration ${f}: ${e.message}${preMigrate ? ` — the copy from before it is ${preMigrate}` : ''}`); }
  }
  const latest = files.at(-1) || null;
  return { applied: files.length, pending_applied: pending, latest, ahead, pre_migrate: preMigrate, pre_migrate_copies: path === ':memory:' ? 0 : (() => { try { return readdirSync(join(dirname(path), 'backups')).filter(f => /^pre-migrate-.*\.db$/.test(f)).length; } catch { return 0; } })() };
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
    // BEGIN IMMEDIATE takes the write lock up front, so a second writer (the CLI beside serve) waits
    // out busy_timeout instead of failing at commit. The depth counter is restored on every path: a
    // COMMIT that fails (disk full) used to leave it at -1 and every later transaction broken until restart.
    tx(fn) {
      const outer = depth === 0; const sp = `sp${depth}`;
      db.exec(outer ? 'BEGIN IMMEDIATE' : `SAVEPOINT ${sp}`); depth++;
      let r;
      try { r = fn(api); }
      catch (e) { depth--; try { db.exec(outer ? 'ROLLBACK' : `ROLLBACK TO ${sp}; RELEASE ${sp}`); } catch { /* the transaction is already gone */ } throw e; }
      depth--;
      try { db.exec(outer ? 'COMMIT' : `RELEASE ${sp}`); }
      catch (e) { if (outer) { try { db.exec('ROLLBACK'); } catch { /* already rolled back */ } } throw e; }
      return r;
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
