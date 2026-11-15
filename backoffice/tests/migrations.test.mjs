// The migration rules (src/migrations/README.md): every file is additive unless it says `-- contract:`, files are
// numbered without gaps and carry no transaction of their own; the app takes a copy before the first pending
// migration on a file database and refuses a database a newer build migrated.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readdirSync, readFileSync, writeFileSync, mkdirSync, copyFileSync, existsSync, chmodSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
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

// Going back from a release that migrated is the copy first, then the code. The other order was what the docs said
// and what `kit deploy <previous>` did, and it cannot work: the previous build refuses the migrated file.
const REPO = fileURLToPath(new URL('../..', import.meta.url)); const KIT = join(REPO, 'infra', 'bin', 'kit');
test('the rollback procedure, wherever it is written, restores the pre-migrate copy before it starts the previous build', () => {
  const flat = (p) => readFileSync(p, 'utf8').replace(/\s+/g, ' ');
  const readme = flat(join(DIR, 'README.md'));
  const para = readme.slice(readme.indexOf('Rolling back a release that migrated')); assert.ok(para.length > 100, 'the README has the rollback paragraph');
  const restoreAt = para.indexOf('backoffice restore backups/pre-migrate-'); const startAt = para.indexOf('start the previous build');
  assert.ok(restoreAt > 0 && startAt > restoreAt, 'restore is named before the previous build is started'); assert.match(para, /Stop the server; `backoffice restore/); assert.match(para, /kit rollback backoffice/); assert.match(para, /The other order cannot work/);
  assert.ok(!/deploy the previous tag, then `backoffice restore/.test(readme), 'the order that cannot work is gone'); assert.ok(!/the previous image on the current file/.test(readme), 'and so is the rule that promised it');
  const maintenance = flat(join(REPO, 'backoffice', 'MAINTENANCE.md')); const m = maintenance.slice(maintenance.indexOf('Going back from an update that migrated'));
  assert.ok(m.indexOf('backoffice restore backups/pre-migrate-') > 0 && m.indexOf('start the previous build') > m.indexOf('backoffice restore backups/pre-migrate-'));
  if (!existsSync(KIT)) return;   // the backoffice directory on its own (an image build context) has no server kit beside it
  for (const doc of [join(REPO, 'infra', 'README.md'), join(REPO, 'infra', 'OWNERS-MANUAL.md')]) { const t = flat(doc); assert.match(t, /kit rollback backoffice/, doc); assert.ok(!/kit deploy backoffice <previous/.test(t), `${doc} no longer names a plain deploy as the rollback`); }
  const kit = readFileSync(KIT, 'utf8'); assert.match(kit, /^\s*rollback\) shift; cmd_rollback "\$@";;$/m, 'kit has the subcommand'); assert.match(kit, /^cmd_rollback\(\) \{$/m); assert.match(kit, /^ {2}kit rollback <app>/m, 'and its help says so');
});

// The kit, run for real against a docker that is a script: what it asks docker to do, and in which order. The fake
// keeps one fact — whether the database carries a migration the old build does not know — and answers as the
// backoffice's images would: the old build refuses a migrated file, the restore puts the old schema back.
test('kit rollback: stop, restore the newest pre-migrate copy, then deploy the previous tag; a plain deploy when no migration separates them; a deploy the schema guard refuses says so', { skip: !existsSync(KIT) || spawnSync('bash', ['-c', 'exit 0']).status !== 0 }, () => {
  const work = mkdtempSync(join(tmpdir(), 'bo-kit-'));
  try {
    const apps = join(work, 'apps'), bin = join(work, 'bin'), log = join(work, 'calls.log'), state = join(work, 'state');
    mkdirSync(join(apps, 'backoffice'), { recursive: true }); mkdirSync(join(apps, 'other'), { recursive: true }); mkdirSync(bin);
    writeFileSync(join(apps, 'backoffice', 'compose.yml'), 'services:\n  backoffice:\n    environment:\n      BACKOFFICE_ENV: production\n'); writeFileSync(join(apps, 'other', 'compose.yml'), 'services:\n  other:\n    image: x\n'); writeFileSync(join(apps, 'other', '.tag'), 'a1\n');
    const GUARD = 'error: database schema is ahead of this build: 002_more.sql was applied by a newer version';
    writeFileSync(join(bin, 'docker'), `#!/usr/bin/env bash
# state: "migrated" or "old" (the schema of the database file); up: the tag the app container runs; BROKEN: a tag that never gets healthy
S="${state}"; schema() { cat "$S/schema"; }; up() { cat "$S/up" 2>/dev/null || true; }
refuses() { [ "$1" = old ] && [ "$(schema)" = migrated ]; }
echo "TAG=\${TAG:-} $*" >> "${log}"
case "$1" in
  compose) shift 3
    case "$1" in
      stop) echo stopped > "$S/up";;
      pull) :;;
      config) :;;
      up) echo "$TAG" > "$S/up"; if [ "$TAG" = new ]; then echo migrated > "$S/schema"; fi;;
      run)
        if [[ "$*" == *"--entrypoint sh"* ]]; then if [ -f "$S/nocopy" ]; then exit 0; fi; printf '/data/backups/pre-migrate-2026-11-01T00-00-00-000-001_init.db\\n/data/backups/pre-migrate-2026-11-14T09-30-00-000-002_more.db\\r\\n' | sort | tail -n 1
        elif [[ "$*" == *" help" ]]; then if refuses "$TAG"; then echo "${GUARD}" >&2; exit 1; fi; echo usage
        elif [[ "$*" == *" restore "* ]]; then if refuses "$TAG" && [[ "$*" != *"BACKOFFICE_ALLOW_NEWER_SCHEMA=1"* ]]; then echo "${GUARD}" >&2; exit 1; fi; echo old > "$S/schema"; echo '{"restored":"/data/backoffice.db"}'
        else exit 9; fi;;
      *) exit 9;;
    esac;;
  inspect) if [[ "$*" == *".Config.Image"* ]]; then echo "img:$(up)"; elif refuses "$(up)" || [ "$(up)" = "$(cat "$S/broken" 2>/dev/null)" ] || [ "$(up)" = stopped ]; then echo unhealthy; else echo healthy; fi;;
  logs) if refuses "$(up)"; then echo "${GUARD}"; else echo "[backoffice] listening"; fi;;
  image) :;;
  *) exit 9;;
esac
`); chmodSync(join(bin, 'docker'), 0o755);
    writeFileSync(join(bin, 'sleep'), '#!/usr/bin/env bash\nexit 0\n'); chmodSync(join(bin, 'sleep'), 0o755);   // the two-minute health wait, without the two minutes
    mkdirSync(state);
    const set = ({ schema, tag, previous = null, up = tag, broken = '', nocopy = false }) => { writeFileSync(join(state, 'schema'), schema); writeFileSync(join(state, 'up'), up); writeFileSync(join(state, 'broken'), broken); if (nocopy) writeFileSync(join(state, 'nocopy'), '1'); else rmSync(join(state, 'nocopy'), { force: true });
      writeFileSync(join(apps, 'backoffice', '.tag'), tag + '\n'); if (previous) writeFileSync(join(apps, 'backoffice', '.tag.previous'), previous + '\n'); else rmSync(join(apps, 'backoffice', '.tag.previous'), { force: true }); writeFileSync(log, ''); };
    const kit = (...args) => { const r = spawnSync('bash', [KIT, ...args], { encoding: 'utf8', env: { ...process.env, PATH: `${bin}:${process.env.PATH}`, APPS_DIR: apps, DATA_DIR: join(work, 'data'), KIT_DIR: join(REPO, 'infra') } }); return { code: r.status, out: r.stdout, err: r.stderr, calls: readFileSync(log, 'utf8').trim().split('\n').filter(Boolean) }; };
    const at = (calls, re) => calls.findIndex(c => re.test(c)); const tagNow = () => readFileSync(join(apps, 'backoffice', '.tag'), 'utf8').trim(); const upNow = () => readFileSync(join(state, 'up'), 'utf8').trim(); const schemaNow = () => readFileSync(join(state, 'schema'), 'utf8').trim();

    // 1. production runs a build that migrated; going back is stop → restore the newest copy → deploy the previous tag
    set({ schema: 'migrated', tag: 'new', previous: 'old' });
    let r = kit('rollback', 'backoffice'); assert.equal(r.code, 0, r.err + r.out);
    const stop = at(r.calls, / stop$/), restore = at(r.calls, / restore /), upOld = at(r.calls, /^TAG=old .* up -d/);
    assert.ok(stop >= 0 && restore > stop && upOld > restore, `stop, then restore, then the previous tag: ${r.calls.join(' | ')}`);
    assert.match(r.calls[restore], /^TAG=new .*run --rm --no-deps -T -e BACKOFFICE_ALLOW_NEWER_SCHEMA=1 backoffice node bin\/backoffice\.mjs restore \/data\/backups\/pre-migrate-2026-11-14T09-30-00-000-002_more\.db --force$/, 'the newest copy, restored by the build that is deployed');
    assert.ok(!r.calls.slice(0, restore).some(c => /^TAG=old .* up /.test(c)), 'the previous build is never started on the migrated file');
    assert.deepEqual([tagNow(), upNow(), schemaNow()], ['old', 'old', 'old']); assert.equal(readFileSync(join(apps, 'backoffice', '.tag.previous'), 'utf8').trim(), 'new'); assert.match(r.out, /what was written after it .* is not in it/);

    // 2. no migration between the two builds: nothing is restored, it is a plain deploy of the previous tag
    set({ schema: 'old', tag: 'old2', previous: 'old' });
    r = kit('rollback', 'backoffice'); assert.equal(r.code, 0, r.err + r.out); assert.equal(at(r.calls, / restore /), -1, 'no restore'); assert.match(r.out, /nothing to restore/); assert.deepEqual([tagNow(), upNow()], ['old', 'old']);

    // 3. what the docs used to say: `kit deploy <previous>` after a migration. The guard refuses it; the build that was
    //    running comes back, and the operator is told the command that works
    set({ schema: 'migrated', tag: 'new', previous: 'old' });
    r = kit('deploy', 'backoffice', 'old'); assert.equal(r.code, 1); assert.match(r.err, /old cannot open the database: a newer build migrated it/); assert.match(r.err, /run: kit rollback backoffice/);
    assert.deepEqual([tagNow(), upNow(), schemaNow()], ['new', 'new', 'migrated'], 'production is back on the build that knows the schema, nothing was restored'); assert.equal(at(r.calls, / restore /), -1);

    // 4. a deploy that migrated and then failed its health check: the automatic step back lands on a build that cannot
    //    open the file. The kit says so, and `kit rollback` restores the copy and starts the build that was deployed
    set({ schema: 'old', tag: 'old', broken: 'new' });
    r = kit('deploy', 'backoffice', 'new'); assert.equal(r.code, 1); assert.match(r.err, /rolling back to old/); assert.match(r.err, /old cannot open the database: new migrated it before it failed\. Run: kit rollback backoffice/);
    assert.deepEqual([tagNow(), upNow(), schemaNow()], ['old', 'old', 'migrated']);
    writeFileSync(log, ''); r = kit('rollback', 'backoffice'); assert.equal(r.code, 0, r.err + r.out); assert.match(r.out, /old is deployed and cannot open the database/);
    assert.ok(at(r.calls, / restore /) > at(r.calls, / stop$/) && at(r.calls, /^TAG=old .* up -d/) > at(r.calls, / restore /)); assert.match(r.calls[at(r.calls, / restore /)], /^TAG=old .*BACKOFFICE_ALLOW_NEWER_SCHEMA=1/, 'the deployed build opens the file it is about to replace');
    assert.deepEqual([tagNow(), upNow(), schemaNow()], ['old', 'old', 'old']);

    // 5. it changes nothing it cannot finish: no copy to restore, no previous tag, not a backoffice
    set({ schema: 'migrated', tag: 'new', previous: 'old', nocopy: true });
    r = kit('rollback', 'backoffice'); assert.equal(r.code, 1); assert.match(r.err, /no pre-migrate copy/); assert.equal(at(r.calls, / restore /), -1); assert.deepEqual([tagNow(), upNow(), schemaNow()], ['new', 'new', 'migrated'], 'started again on the tag it was running');
    //    …and the backup the operator names instead is the one restored, in the same order
    writeFileSync(log, ''); r = kit('rollback', 'backoffice', '/data/backups/backoffice-2026-11-13.db'); assert.equal(r.code, 0, r.err + r.out);
    assert.match(r.calls[at(r.calls, / restore /)], / restore \/data\/backups\/backoffice-2026-11-13\.db --force$/); assert.ok(at(r.calls, /^TAG=old .* up -d/) > at(r.calls, / restore /)); assert.deepEqual([tagNow(), upNow(), schemaNow()], ['old', 'old', 'old']);
    r = kit('rollback', 'backoffice', '../../etc/passwd'); assert.equal(r.code, 1); assert.match(r.err, /the copy is a \.db file/); r = kit('rollback', 'backoffice', '/data/x.db; rm -rf /'); assert.equal(r.code, 1);
    // a deploy of a tag that is not the previous one, refused by the guard: the kit says what rollback does, and promises nothing else
    set({ schema: 'migrated', tag: 'new', previous: 'mid' });
    r = kit('deploy', 'backoffice', 'old'); assert.equal(r.code, 1); assert.match(r.err, /kit rollback backoffice goes back one build \(to mid\)/); assert.ok(!/kit offline/.test(r.err)); assert.deepEqual([tagNow(), upNow()], ['new', 'new']);
    set({ schema: 'old', tag: 'only' });
    r = kit('rollback', 'backoffice'); assert.equal(r.code, 1); assert.match(r.err, /no previous tag on record/); assert.deepEqual([tagNow(), upNow()], ['only', 'only']);
    r = kit('rollback', 'other'); assert.equal(r.code, 1); assert.match(r.err, /not a backoffice app/);
    r = kit('rollback'); assert.equal(r.code, 1); assert.match(r.err, /which app\?/);
  } finally { rmSync(work, { recursive: true, force: true }); }
});
