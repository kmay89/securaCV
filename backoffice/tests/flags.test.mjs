// Feature flags: the registry is the only source of names, the environment wins over Settings over the default,
// a flip is audited and refused where the environment pins it, a rollout past its date is a doctor finding, and
// the one real flag (stripe_tax) reaches the checkout branch by every spelling.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FLAGS, isOn, listFlags, setFlag, clearFlag } from '../src/flags.mjs';
import { createApp } from '../src/server.mjs';
import { createCheckout } from '../src/modules/storefront.mjs';
import { DEMO_CATALOG } from '../src/modules/demo.mjs';
import { doctor } from '../src/modules/health.mjs';
import { setupOperator, createApiToken, totp } from '../src/auth.mjs';
import { testDb, testCfg } from './helpers.mjs';

const REG = { rollout_x: { kind: 'rollout', default: false, owner: 'ops', since: '2026-01-01', remove_by: '2026-06-01', description: 'a dark launch' }, switch_y: { kind: 'switch', default: true, owner: 'owner', since: '2026-01-01', remove_by: null, description: 'a standing choice' } };

test('the registry is the only source of names; precedence is environment, then Settings, then the default; flips are audited', () => {
  const db = testDb(); const cfg = testCfg();
  assert.throws(() => isOn(db, cfg, 'no_such_flag'), /unknown feature flag "no_such_flag"/);
  assert.equal(isOn(db, cfg, 'rollout_x', { registry: REG }), false); assert.equal(isOn(db, cfg, 'switch_y', { registry: REG }), true);
  assert.deepEqual(setFlag(db, cfg, 'rollout_x', true, { actor: 'o@errer.test', registry: REG }), { name: 'rollout_x', on: true, was: false });
  assert.equal(isOn(db, cfg, 'rollout_x', { registry: REG }), true);
  const row = listFlags(db, cfg, { registry: REG }).find(f => f.name === 'rollout_x'); assert.equal(row.source, 'setting'); assert.ok(row.set_at);
  assert.deepEqual(clearFlag(db, cfg, 'rollout_x', { registry: REG }), { name: 'rollout_x', on: false, was: true });
  assert.equal(listFlags(db, cfg, { registry: REG }).find(f => f.name === 'rollout_x').source, 'default');
  // the environment pins it: Settings cannot flip it, and says why
  const pinned = testCfg({ flagOverrides: { switch_y: false } });
  assert.equal(isOn(db, pinned, 'switch_y', { registry: REG }), false);
  assert.equal(listFlags(db, pinned, { registry: REG }).find(f => f.name === 'switch_y').source, 'environment');
  assert.throws(() => setFlag(db, pinned, 'switch_y', true, { registry: REG }), /set by the environment \(FLAG_SWITCH_Y\)/);
  const audit = db.all("SELECT actor, action, entity_id, detail FROM audit_log WHERE action LIKE 'flag.%' ORDER BY id");
  assert.deepEqual(audit.map(a => [a.actor, a.action, a.entity_id]), [['o@errer.test', 'flag.set', 'rollout_x'], ['web', 'flag.clear', 'rollout_x']]);
  assert.deepEqual(JSON.parse(audit[0].detail), { on: true, was: false });
});

test('a rollout flag past its date is a doctor finding; a switch never is; the real registry has only well-formed entries', () => {
  const db = testDb(); const cfg = testCfg();
  const rows = listFlags(db, cfg, { registry: REG, now: new Date('2026-07-01T00:00:00Z') });
  assert.equal(rows.find(f => f.name === 'rollout_x').overdue, true); assert.equal(rows.find(f => f.name === 'switch_y').overdue, false);
  assert.equal(listFlags(db, cfg, { registry: REG, now: new Date('2026-05-01T00:00:00Z') }).find(f => f.name === 'rollout_x').overdue, false);
  for (const [name, f] of Object.entries(FLAGS)) {
    assert.match(name, /^[a-z][a-z0-9_]{2,40}$/); assert.ok(['rollout', 'switch'].includes(f.kind), name); assert.equal(typeof f.default, 'boolean');
    assert.ok(f.description.length > 20 && f.owner && /^\d{4}-\d{2}-\d{2}$/.test(f.since), name);
    if (f.kind === 'rollout') assert.match(f.remove_by || '', /^\d{4}-\d{2}-\d{2}$/, `${name}: a rollout names its remove_by date`); else assert.equal(f.remove_by, null, `${name}: a switch has no date`);
  }
  const h = doctor(db, cfg); const row = h.checks.find(c => c.key === 'flags'); assert.equal(row.ok, true); assert.match(row.detail, /^1 declared · none on$/);
});

test('stripe_tax reaches the checkout by every spelling: the default is off, Settings turns it on, TAX_MODE=stripe and FLAG_STRIPE_TAX pin it', async () => {
  const db = testDb(); const cfg = testCfg({ stripeApiKey: 'sk_test_x', secret: 's' });
  const sent = []; const fetchImpl = async (url, init) => { sent.push(Object.fromEntries(new URLSearchParams(init.body))); return { ok: true, status: 200, json: async () => ({ id: 'cs_1', url: 'https://checkout.stripe.test/cs_1' }) }; };
  const go = (c) => createCheckout(db, c, DEMO_CATALOG, { items: [{ sku: 'wap-parts', qty: 1 }] }, { fetchImpl, now: new Date('2026-10-21T12:00:00Z') });
  await go(cfg); assert.equal(sent.at(-1)['automatic_tax[enabled]'], undefined, 'off by default');
  setFlag(db, cfg, 'stripe_tax', true); await go(cfg); assert.equal(sent.at(-1)['automatic_tax[enabled]'], 'true', 'on from Settings');
  clearFlag(db, cfg, 'stripe_tax'); await go({ ...cfg, taxMode: 'stripe' }); assert.equal(sent.at(-1)['automatic_tax[enabled]'], 'true', 'the old spelling');
  await go({ ...cfg, flagOverrides: { stripe_tax: true } }); assert.equal(sent.at(-1)['automatic_tax[enabled]'], 'true', 'the new spelling');
  setFlag(db, cfg, 'stripe_tax', true); await go({ ...cfg, flagOverrides: { stripe_tax: false } }); assert.equal(sent.at(-1)['automatic_tax[enabled]'], undefined, 'the environment wins over Settings');
  assert.throws(() => setFlag(db, { ...cfg, taxMode: 'stripe' }, 'stripe_tax', false), /FLAG_STRIPE_TAX or TAX_MODE/);
});

test('over HTTP: the Settings page lists and flips a flag for the owner, /api/flags answers a bearer token, and both are the owner\'s', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'O' });
    const form = (o) => new URLSearchParams(o).toString();
    const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) });
    const cookie = r.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf;
    const page = await (await fetch(base + '/settings', { headers: { cookie, accept: 'text/html' } })).text();
    assert.ok(page.includes('<h2>Feature flags</h2>') && page.includes('<b>stripe_tax</b>') && page.includes('Turn on'), 'the section, the flag, the toggle');
    const flip = await fetch(base + '/settings/flags/stripe_tax', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, on: '1' }) });
    assert.equal(flip.status, 303); assert.equal(isOn(db, cfg, 'stripe_tax'), true);
    assert.equal((await fetch(base + '/settings/flags/nope', { method: 'POST', redirect: 'manual', headers: { cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: csrf, on: '1' }) })).status, 400, 'an unknown name is a 400, not a row');
    const tok = createApiToken(db, 'cli'); const h = { authorization: `Bearer ${tok}`, 'content-type': 'application/json' };
    const list = await (await fetch(base + '/api/flags', { headers: h })).json(); assert.equal(list[0].name, 'stripe_tax'); assert.equal(list[0].on, true); assert.equal(list[0].source, 'setting');
    assert.deepEqual(await (await fetch(base + '/api/flags/stripe_tax', { method: 'POST', headers: h, body: JSON.stringify({ on: false }) })).json(), { name: 'stripe_tax', on: false, was: true });
    assert.equal((await (await fetch(base + '/api/flags/stripe_tax', { method: 'POST', headers: h, body: JSON.stringify({ on: null }) })).json()).on, false, 'null clears to the default');
    assert.equal(db.get("SELECT COUNT(*) AS n FROM settings WHERE key = 'flag:stripe_tax'").n, 0);
    assert.equal(db.get("SELECT actor FROM audit_log WHERE action = 'flag.set' ORDER BY id DESC LIMIT 1").actor, 'token:cli');
  } finally { await app.close(); }
});
