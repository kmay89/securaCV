// The environment label and the build: what each environment refuses and mutes, how every page and the
// health endpoint name it, that the first boot of a build is written down, and that the changelog's top
// entry is the package version.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createApp } from '../src/server.mjs';
import { resolveEnv, envRules, applyEnv, ENVIRONMENTS } from '../src/env.mjs';
import { VERSION, build, describeBuild, recordBoot } from '../src/version.mjs';
import { layout, currentEnv } from '../src/render.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { doctor } from '../src/modules/health.mjs';
import { uploadOffsite } from '../src/modules/backup.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('the label resolves: explicit wins, a demo seed says demo, nothing says development, a wrong name is refused', () => {
  const db = testDb();
  assert.equal(resolveEnv(testCfg(), db), 'development');
  assert.equal(resolveEnv(testCfg({ env: 'staging' }), db), 'staging');
  assert.equal(resolveEnv(testCfg({ demo: true }), db), 'demo');
  db.setSetting('demo', '1'); assert.equal(resolveEnv(testCfg(), db), 'demo', 'a seeded database is the demo whatever the flag says');
  assert.throws(() => resolveEnv(testCfg({ env: 'prod' }), db), /one of production, staging, demo, development/);
  assert.deepEqual(ENVIRONMENTS, ['production', 'staging', 'demo', 'development']);
});

test('production refuses a demo seed, a database another environment labeled, and a plain-http base; staging refuses a live Stripe key', async () => {
  const prod = (o = {}) => testCfg({ env: 'production', baseUrl: 'https://ops.example.test', secure: true, ...o });
  const fatals = (cfg, db) => envRules(cfg, db).filter(r => r.level === 'fatal').map(r => r.text);
  assert.deepEqual(fatals(prod(), testDb()), []);
  assert.match(fatals(prod({ demo: true }), testDb())[0], /demo seed/);
  const seeded = testDb(); await seedDemo(seeded, testCfg()); assert.match(fatals(prod(), seeded)[0], /demo seed/);
  const stagingFile = testDb(); stagingFile.setSetting('env:label', 'staging'); assert.match(fatals(prod(), stagingFile)[0], /last run as staging/);
  assert.match(fatals(prod({ baseUrl: 'http://ops.example.test', secure: false }), testDb())[0], /https/);
  assert.match(fatals(testCfg({ env: 'staging', stripeApiKey: 'sk_live_abc' }), testDb())[0], /LIVE key/);
  assert.deepEqual(fatals(testCfg({ env: 'staging', stripeApiKey: 'sk_test_abc' }), testDb()), []);
  // the warnings the doctor shows
  assert.match(envRules(prod({ stripeApiKey: 'sk_test_x' }), testDb())[0].text, /test-mode key/);
  assert.match(envRules(testCfg({ env: '', baseUrl: 'https://ops.example.test', secure: true }), testDb())[0].text, /BACKOFFICE_ENV is not set/);
  // createApp refuses with every fatal line in one message, before it serves
  assert.throws(() => createApp({ db: stagingFile, cfg: prod({ demo: false }) }), /refusing to run as production:\n {2}- this database was last run as staging/);
});

test('staging mutes mail, IMAP, issues and off-site writes, labels the database, and the label survives a reopen', async () => {
  const db = testDb(); const cfg = testCfg({ env: 'staging', smtpUrl: 'smtp://u:p@relay.test:587', imapUrl: 'imaps://u:p@mail.test', githubToken: 'ghp_x', githubRepo: 'o/r', backupS3Url: 'https://s3.test/b/p', backupS3Key: 'k', backupS3Secret: 's' });
  const r = applyEnv(cfg, db);
  assert.equal(r.env, 'staging'); assert.equal(cfg.smtpUrl, ''); assert.equal(cfg.imapUrl, ''); assert.equal(cfg.githubToken, ''); assert.equal(cfg.offsiteWrites, false);
  assert.equal(db.setting('env:label'), 'staging');
  await assert.rejects(uploadOffsite(cfg, '/nonexistent'), /off-site writes are off in staging/);
  const allowed = testCfg({ env: 'staging', smtpUrl: 'smtp://u:p@relay.test:587', stagingAllowMail: true }); applyEnv(allowed, testDb()); assert.equal(allowed.smtpUrl, 'smtp://u:p@relay.test:587', 'STAGING_ALLOW_MAIL keeps the relay');
  // production labels its database too, and a production file is accepted back by staging (that is the refresh)
  const p = testDb(); applyEnv(testCfg({ env: 'production', baseUrl: 'https://x.test', secure: true }), p); assert.equal(p.setting('env:label'), 'production');
  applyEnv(testCfg({ env: 'staging' }), p); assert.equal(p.setting('env:label'), 'staging');
});

test('every page carries the label: badge, title prefix, the staging banner; /healthz and /api/health name the build', async () => {
  const db = testDb(); const cfg = testCfg({ env: 'staging', secret: 's', buildSha: 'abcdef123456ff', buildDate: '2026-10-26T12:00:00Z' }); await seedDemo(db, cfg);
  cfg.demo = false; db.setSetting('demo', '0');   // the seed is data only here; the label under test is staging
  const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    assert.equal(currentEnv().name, 'staging');
    const page = layout({ title: 'Orders', body: '' });
    assert.ok(page.includes('<title>[staging] Orders · Errer Labs backoffice</title>'), 'the title says which');
    assert.ok(page.includes('>staging</span>'), 'the badge'); assert.ok(page.includes('class="demo envbar"'), 'the banner');
    const hz = await (await fetch(base + '/healthz')).text(); assert.equal(hz, 'ok staging abcdef123456');
    assert.equal(build(cfg).sha, 'abcdef123456'); assert.equal(describeBuild(cfg), `${VERSION} · build abcdef123456 · 2026-10-26`);
    const dev = testCfg(); assert.equal(describeBuild(dev), `${VERSION} · build dev`);
    const h = doctor(db, cfg); assert.equal(h.checks[0].key, 'environment'); assert.match(h.checks[0].detail, /^staging · .* · build abcdef123456/);
  } finally { await app.close(); }
});

test('the first boot of a build writes one audit row naming the build before it; the same build again writes none', () => {
  const db = testDb(); const cfg = testCfg({ buildSha: '111111111111', env: 'development' });
  assert.deepEqual(recordBoot(db, cfg), { from: null, to: '111111111111' });
  assert.equal(recordBoot(db, cfg), null, 'same build, no second row');
  assert.deepEqual(recordBoot(db, testCfg({ buildSha: '222222222222' })), { from: '111111111111', to: '222222222222' });
  const rows = db.all("SELECT entity_id, detail FROM audit_log WHERE action = 'deploy.boot' ORDER BY id");
  assert.deepEqual(rows.map(r => r.entity_id), ['111111111111', '222222222222']);
  assert.equal(JSON.parse(rows[1].detail).from, '111111111111');
});

test('the changelog leads with the package version', () => {
  const top = readFileSync(new URL('../CHANGELOG.md', import.meta.url), 'utf8').match(/^## (\S+)/m)[1];
  assert.equal(top, VERSION, `CHANGELOG.md's first entry is ${top}; package.json says ${VERSION} — bump both together`);
});
