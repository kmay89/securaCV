import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { upsertProbe, runProbe, runDueProbes, publicStatus, openIncident, updateIncident, uptime } from '../src/modules/status.mjs';
import { raise, resolve, openExceptions } from '../src/modules/exceptions.mjs';
import { backup, listBackups } from '../src/modules/backup.mjs';
import { createApp } from '../src/server.mjs';
import { runJob } from '../src/jobs.mjs';
import { upsertItem } from '../src/modules/inventory.mjs';
import { testDb, testCfg } from './helpers.mjs';

const fakeFetch = (status, body = '') => async () => ({ status, text: async () => body });

test('probes: up/down by status and body text; three failures raise an exception; recovery resolves it', async () => {
  const db = testDb(); const cfg = testCfg();
  const p = upsertProbe(db, { name: 'Website', url: 'https://x.test/', expect_text: 'SecuraCV' });
  assert.ok((await runProbe(db, p, { fetchImpl: fakeFetch(200, '<h1>SecuraCV</h1>'), cfg })).ok);
  assert.ok(!(await runProbe(db, p, { fetchImpl: fakeFetch(200, 'parked domain'), cfg })).ok);
  assert.match((await runProbe(db, p, { fetchImpl: fakeFetch(503), cfg })).detail, /^503/);
  assert.equal(openExceptions(db).length, 0, 'two failures are not yet an exception');
  assert.ok(!(await runProbe(db, p, { fetchImpl: async () => { throw new Error('ECONNREFUSED'); }, cfg })).ok);
  assert.equal(openExceptions(db)[0].key, 'probe:Website');
  assert.equal(publicStatus(db).overall, 'degraded');
  await runProbe(db, p, { fetchImpl: fakeFetch(200, 'SecuraCV'), cfg });
  assert.equal(openExceptions(db).length, 0);
  assert.equal(uptime(db, p.id), 40);
  assert.equal(publicStatus(db).overall, 'operational');
  const inc = openIncident(db, { title: 'Lab slow', body: 'CDN issue' });
  assert.equal(publicStatus(db).overall, 'degraded');
  updateIncident(db, inc.id, { status: 'resolved' });
  assert.equal(publicStatus(db).overall, 'operational');
  db.run('UPDATE probes SET last_at = NULL');
  assert.equal((await runDueProbes(db, { fetchImpl: fakeFetch(200, 'SecuraCV'), cfg })).length, 1);
});

test('exceptions dedupe by key and mirror to GitHub when a token is set', async () => {
  const db = testDb(); const calls = [];
  const fetchImpl = async (url, opts) => { calls.push([opts.method, url]); return { ok: true, json: async () => ({ number: 42 }) }; };
  const cfg = testCfg({ githubToken: 'ghp_x', githubRepo: 'o/r' });
  const e1 = await raise(db, cfg, 'stock:x', 'low x', 'body', { fetchImpl });
  assert.equal(e1.github_issue, 42);
  const e2 = await raise(db, cfg, 'stock:x', 'low x', 'body2', { fetchImpl });
  assert.ok(e2.bumped); assert.equal(calls.length, 1, 'no second issue');
  await resolve(db, cfg, 'stock:x', 'restocked', { fetchImpl });
  assert.equal(calls.length, 3); assert.equal(calls[2][0], 'PATCH');
  assert.equal(openExceptions(db).length, 0);
  assert.equal(await resolve(db, cfg, 'stock:x', 'again', { fetchImpl }), null);
});

test('jobs run, record their result, and the stock job raises/resolves reorder exceptions', async () => {
  const db = testDb(); const cfg = testCfg({ githubToken: '' });
  const app = createApp({ db, cfg });
  upsertItem(db, { sku: 'cable', name: 'USB-C cable', on_hand: 1, reorder_point: 5, reorder_qty: 50, supplier: 'Digi-Key' });
  const r = await runJob(app, 'stock');
  assert.ok(r.ok); assert.equal(r.result.low, 1); assert.equal(openExceptions(db)[0].key, 'stock:cable');
  db.run('UPDATE inventory_items SET on_hand = 100');
  await runJob(app, 'stock');
  assert.equal(openExceptions(db).length, 0);
  assert.equal(db.get('SELECT last_ok FROM job_runs WHERE name = ?', 'stock').last_ok, 1);
  const bad = await runJob(app, 'nope').catch(e => e);
  assert.match(bad.message, /unknown job/);
  assert.equal((await runJob(app, 'inbox')).result.skipped, 'IMAP_URL not set');
});

test('backups are consistent copies with retention', () => {
  const db = testDb(); const dir = mkdtempSync(join(tmpdir(), 'bo-'));
  db.setSetting('k', 'v');
  for (let i = 0; i < 4; i++) backup(db, dir, { keep: 2, now: new Date(2026, 0, 1 + i) });
  const files = listBackups(dir);
  assert.equal(files.length, 2);
  const { openDb } = await_import_sync();
  const copy = openDb(join(dir, files[0].name));
  assert.equal(copy.setting('k'), 'v');
  copy.close(); rmSync(dir, { recursive: true });
});
import { openDb as _openDb } from "../src/db.mjs"; function await_import_sync() { return { openDb: _openDb }; }
