// `backoffice restore --offsite` as the server-died drill actually runs it: a separate process, a
// fresh data directory (so the CLI's own startup has already created and opened a database there),
// an S3 stand-in on localhost holding two sealed copies. The restored file must hold the backup's
// data (the live connection is closed before the file is replaced — an open WAL connection's final
// checkpoint used to write the old pages back over the restored copy), a named copy must be the one
// restored, and a name that is not there must fail rather than quietly restore the latest.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { join, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { createServer } from 'node:http';
import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { DatabaseSync } from 'node:sqlite';
import { backup, seal } from '../src/modules/backup.mjs';
import { openDb } from '../src/db.mjs';

const CLI = fileURLToPath(new URL('../bin/backoffice.mjs', import.meta.url));
const KEY = 'the passphrase the server was told';

function makeBackup(dir, marker) {
  const db = openDb(join(dir, 'src.db')); db.setSetting('demo', '0'); db.setSetting('marker', marker);
  const b = backup(db, join(dir, 'backups')); db.close();
  return { name: `${basename(b.path)}.enc`, body: seal(readFileSync(b.path), KEY) };
}
const settingOf = (dbPath, key) => { const d = new DatabaseSync(dbPath); try { return d.prepare('SELECT value FROM settings WHERE key = ?').get(key)?.value ?? null; } finally { d.close(); } };

test('restore --offsite in a fresh data directory restores the latest copy, a named copy, and refuses a missing name', async () => {
  const work = mkdtempSync(join(tmpdir(), 'bo-restore-cli-'));
  try {
    const older = makeBackup(join(work, 'a'), 'older-copy');
    await new Promise(r => setTimeout(r, 5));                 // the backup name carries milliseconds; keep the two distinct
    const latest = makeBackup(join(work, 'b'), 'latest-copy');
    const objects = { 'latest.txt': Buffer.from(latest.name), [latest.name]: latest.body, [older.name]: older.body };
    const srv = createServer((req, res) => {
      const name = decodeURIComponent(req.url.split('/').pop()); const body = objects[name];
      if (!body) { res.writeHead(404); return res.end(); }
      res.writeHead(200, { 'content-length': String(body.length) }); res.end(req.method === 'HEAD' ? undefined : body);
    });
    await new Promise(r => srv.listen(0, '127.0.0.1', r));
    const port = srv.address().port;
    const dataDir = join(work, 'live'); const dbPath = join(dataDir, 'backoffice.db');
    const env = { ...process.env, BACKOFFICE_DATA_DIR: dataDir, BACKUP_S3_URL: `http://127.0.0.1:${port}/bucket/prefix`, BACKUP_S3_KEY: 'k', BACKUP_S3_SECRET: 's', BACKUP_S3_REGION: 'auto', BACKUP_ENCRYPTION_KEY: KEY };
    delete env.BACKOFFICE_DEMO;
    // asynchronous on purpose: the S3 stand-in lives in this process, and a synchronous spawn would block it
    const run = (...a) => new Promise(resolve => execFile(process.execPath, ['--disable-warning=ExperimentalWarning', CLI, 'restore', ...a], { env, encoding: 'utf8', timeout: 60_000 },
      (err, stdout, stderr) => resolve({ status: err ? (err.code ?? 1) : 0, stdout, stderr })));
    try {
      const r1 = await run('--offsite');
      assert.equal(r1.status, 0, r1.stderr + r1.stdout);
      assert.equal(settingOf(dbPath, 'marker'), 'latest-copy', 'the restored database holds the backup, not the pages the startup connection wrote');
      const r2 = await run('--offsite', older.name);
      assert.equal(r2.status, 0, r2.stderr + r2.stdout);
      assert.match(r2.stdout, new RegExp(older.name.replace(/\./g, '\\.')), 'the output names the copy that was restored');
      assert.equal(settingOf(dbPath, 'marker'), 'older-copy', 'a named copy is the one restored');
      const r3 = await run('--offsite', 'backoffice-2020-01-01T00-00-00-000.db.enc');
      assert.notEqual(r3.status, 0, 'a name that is not off-site must fail');
      assert.match(r3.stderr, /404/);
      assert.equal(settingOf(dbPath, 'marker'), 'older-copy', 'and the database is untouched');
    } finally { srv.close(); }
  } finally { rmSync(work, { recursive: true, force: true }); }
});
