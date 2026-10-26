// "The server died" is a drill, not a hope: the latest off-site copy comes back through the same
// function the weekly proof uses, is opened with the key, checked, and put in place — and a copy
// nobody can open, or a name nobody wrote, is refused by name before anything is replaced.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, existsSync, readFileSync } from 'node:fs';
import { join, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { DatabaseSync } from 'node:sqlite';
import { backup, seal, fetchOffsite, restore, restoreDrill, OFFSITE_NAME } from '../src/modules/backup.mjs';
import { openDb } from '../src/db.mjs';

const KEY = 'a passphrase long enough to seal with';
function freshDb(dir) { const db = openDb(join(dir, 'live.db')); db.setSetting('demo', '0'); return db; }

// An S3 stand-in: a map of object name → bytes, answering GET and HEAD the way the signer calls them.
function fakeS3(objects) {
  const calls = [];
  const fetchImpl = async (url, { method }) => {
    const name = decodeURIComponent(String(url).split('/').pop()); calls.push(`${method} ${name}`);
    const body = objects[name];
    if (!body) return new Response(null, { status: 404 });
    if (method === 'HEAD') return new Response(null, { status: 200, headers: { 'content-length': String(body.length) } });
    return new Response(body, { status: 200 });
  };
  return { fetchImpl, calls };
}
const cfg = { backupS3Url: 'https://s3.example.test/bucket/prefix', backupS3Key: 'k', backupS3Secret: 's', backupS3Region: 'auto', backupEncryptionKey: KEY };

test('restore --offsite: the latest sealed copy comes back, opens, checks and replaces the live file', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'bo-offsite-'));
  try {
    const db = freshDb(dir); db.setSetting('marker', 'from-the-backup'); db.close();
    const b = backup(openDb(join(dir, 'live.db')), join(dir, 'backups'));
    const name = `${basename(b.path)}.enc`;
    const objects = { 'latest.txt': Buffer.from(name), [name]: seal(readFileSync(b.path), KEY) };
    const { fetchImpl, calls } = fakeS3(objects);
    const got = await fetchOffsite(cfg, { fetchImpl });
    assert.equal(got.name, name); assert.equal(got.encrypted, true); assert.ok(existsSync(got.path));
    assert.deepEqual(calls, ['GET latest.txt', `GET ${name}`], 'the pointer, then the copy it names, nothing else');
    // the live database has moved on; the restore puts the copy in place and keeps the replaced file beside it
    const live = openDb(join(dir, 'live.db')); live.setSetting('marker', 'live-after-the-backup'); live.close();
    const r = restore(got.path, join(dir, 'live.db'));
    assert.ok(r.previous && existsSync(r.previous), 'the replaced file is kept');
    const back = new DatabaseSync(join(dir, 'live.db')); assert.equal(back.prepare("SELECT value FROM settings WHERE key='marker'").get().value, 'from-the-backup'); back.close();
    // the drill reads the same bytes the same way
    const d = await restoreDrill(cfg, { fetchImpl, now: new Date(Date.now() + 60_000) });
    assert.equal(d.ok, true); assert.equal(d.encrypted, true); assert.equal(d.source, got.source);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('a sealed copy without the key, a pointer to a name nobody wrote, and no configuration are each refused by name', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'bo-offsite-'));
  try {
    freshDb(dir).close();
    const b = backup(openDb(join(dir, 'live.db')), join(dir, 'backups'));
    const name = `${basename(b.path)}.enc`;
    const { fetchImpl } = fakeS3({ 'latest.txt': Buffer.from(name), [name]: seal(readFileSync(b.path), KEY), 'evil.txt': Buffer.from('../../etc/passwd') });
    await assert.rejects(fetchOffsite({ ...cfg, backupEncryptionKey: '' }, { fetchImpl }), /sealed and BACKUP_ENCRYPTION_KEY is not set/);
    await assert.rejects(fetchOffsite(cfg, { fetchImpl, name: '../../etc/passwd' }), /bad off-site name/);
    await assert.rejects(fetchOffsite(cfg, { fetchImpl, name: 'backoffice-2026-01-01T00-00-00.db' }), /GET .* 404/, 'a name that is not there is a 404, not a silent empty file');
    await assert.rejects(fetchOffsite({ ...cfg, backupS3Url: '' }, { fetchImpl }), /not set/);
    assert.ok(OFFSITE_NAME.test('backoffice-2026-10-25T03-00-00.db.enc') && !OFFSITE_NAME.test('latest.txt'));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
