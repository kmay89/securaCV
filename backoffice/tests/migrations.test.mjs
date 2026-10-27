// The migration rules (src/migrations/README.md): every file is additive unless it says `-- contract:`, files are
// numbered without gaps and carry no transaction of their own; the app takes a copy before the first pending
// migration on a file database and refuses a database a newer build migrated.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readdirSync, readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { openDb } from '../src/db.mjs';
import { doctor } from '../src/modules/health.mjs';
import { testDb, testCfg } from './helpers.mjs';

const DIR = fileURLToPath(new URL('../src/migrations', import.meta.url));

test('every committed migration is additive (or says `-- contract:`), numbered without gaps, and leaves transactions to openDb', () => {
  const files = readdirSync(DIR).filter(f => f.endsWith('.sql')).sort();
  assert.ok(files.length >= 10);
  files.forEach((f, i) => assert.match(f, new RegExp(`^${String(i + 1).padStart(3, '0')}_[a-z0-9_]+\\.sql$`), `${f} is not number ${i + 1} in the sequence`));
  for (const f of files) {
    const sql = readFileSync(join(DIR, f), 'utf8'); const code = sql.replace(/--[^\n]*/g, '');
    const destructive = /\b(DROP\s+(TABLE|COLUMN|INDEX)|RENAME\s+(TO|COLUMN)|ALTER\s+TABLE\s+\S+\s+DROP)\b/i.exec(code);
    if (destructive) assert.match(sql.split('\n')[0], /^-- contract:/, `${f} has "${destructive[0]}" without a leading "-- contract:" line (README rule 2)`);
    assert.ok(!/\b(BEGIN|COMMIT|ROLLBACK)\b/i.test(code), `${f} opens its own transaction (README rule 3)`);
  }
});

test('a copy is taken before the first pending migration on a file database, kept to five, and a newer schema is refused', () => {
  const work = mkdtempSync(join(tmpdir(), 'bo-migrate-'));
  try {
    const v1 = join(work, 'v1'); const v2 = join(work, 'v2'); mkdirSync(v1); mkdirSync(v2);
    writeFileSync(join(v1, '001_init.sql'), 'CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT (strftime(\'%Y-%m-%dT%H:%M:%fZ\',\'now\')));\nCREATE TABLE IF NOT EXISTS audit_log (id INTEGER PRIMARY KEY, at TEXT, actor TEXT, action TEXT, entity TEXT, entity_id TEXT, detail TEXT);\nCREATE TABLE things (id INTEGER PRIMARY KEY, name TEXT);');
    copyFileSync(join(v1, '001_init.sql'), join(v2, '001_init.sql'));
    writeFileSync(join(v2, '002_more.sql'), 'ALTER TABLE things ADD COLUMN note TEXT;');
    const path = join(work, 'data', 'x.db');
    // first build: a fresh file gets no copy (there is nothing to protect yet)
    let db = openDb(path, { migrationsDir: v1 }); db.run("INSERT INTO things (name) VALUES ('a')");
    assert.deepEqual([db.schema.applied, db.schema.pending_applied, db.schema.pre_migrate], [1, ['001_init.sql'], null]); db.close();
    // the next build: one pending migration → a copy first, then the column
    db = openDb(path, { migrationsDir: v2 });
    assert.equal(db.schema.applied, 2); assert.deepEqual(db.schema.pending_applied, ['002_more.sql']); assert.match(db.schema.pre_migrate, /backups\/pre-migrate-.*-002_more\.db$/);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM things').n, 1);
    const copy = openDb(db.schema.pre_migrate, { migrationsDir: v1 }); assert.equal(copy.get('SELECT COUNT(*) AS n FROM things').n, 1, 'the copy holds the data'); assert.equal(copy.schema.applied, 1); copy.close();
    assert.equal(db.schema.pre_migrate_copies, 1); db.close();
    // reopening on the same build takes no second copy
    db = openDb(path, { migrationsDir: v2 }); assert.equal(db.schema.pre_migrate, null); assert.equal(db.schema.pre_migrate_copies, 1); db.close();
    // the older build on the newer file: refused, by name
    assert.throws(() => openDb(path, { migrationsDir: v1 }), /schema is ahead of this build: 002_more\.sql was applied by a newer version/);
    const rescue = openDb(path, { migrationsDir: v1, allowNewerSchema: true }); assert.deepEqual(rescue.schema.ahead, ['002_more.sql']); rescue.close();
    // only the newest five copies stay
    for (let i = 3; i <= 9; i++) { writeFileSync(join(v2, `${String(i).padStart(3, '0')}_m${i}.sql`), `ALTER TABLE things ADD COLUMN c${i} TEXT;`); const d = openDb(path, { migrationsDir: v2 }); d.close(); }
    const copies = readdirSync(join(work, 'data', 'backups')).filter(f => f.startsWith('pre-migrate-')).sort();
    assert.equal(copies.length, 5); assert.match(copies.at(-1), /009_m9\.db$/); assert.match(copies[0], /005_m5\.db$/);
  } finally { rmSync(work, { recursive: true, force: true }); }
});

test('the doctor reports the schema: the count and the latest file, the copies on disk, and a file that is ahead', () => {
  const db = testDb(); const row = () => doctor(db, testCfg()).checks.find(c => c.key === 'schema');
  const files = readdirSync(DIR).filter(f => f.endsWith('.sql')).sort();
  assert.equal(row().ok, true); assert.equal(row().detail, `${files.length} migrations · latest ${files.at(-1)}`);
  db.schema = { ...db.schema, pre_migrate_copies: 2 }; assert.match(row().detail, / · 2 pre-migration copies on disk$/);
  db.schema = { ...db.schema, ahead: ['011_future.sql'] }; assert.equal(row().ok, false); assert.match(row().detail, /AHEAD of this build: 011_future\.sql/);
});
