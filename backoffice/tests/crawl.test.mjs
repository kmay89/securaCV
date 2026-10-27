import { test } from 'node:test';
import assert from 'node:assert/strict';
import { seedDemo, DEMO_PASSWORD, DEMO_TOTP_SECRET, DEMO_USERS } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { totp } from '../src/auth.mjs';
import { testDb, testCfg } from './helpers.mjs';

// Log in through the form, as a demo person, and hand back the session cookie.
async function signIn(base, email) {
  const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }).toString() });
  assert.equal(r.status, 303, `${email} signs in`);
  return r.headers.get('set-cookie').split(';')[0];
}

// Every internal link reachable from Home, breadth-first: the page it lands on, its status and its HTML.
async function crawl(base, cookie, { skip = /\.(csv|json|png|svg)$|\/logout|\/u\/|\/confirm\//, cap = 1200 } = {}) {
  const seen = new Map(); const queue = ['/'];
  while (queue.length && seen.size < cap) {
    const p = queue.shift(); if (seen.has(p)) continue;
    const res = await fetch(base + p, { headers: { cookie }, redirect: 'manual' });
    const html = (res.headers.get('content-type') || '').includes('text/html') ? await res.text() : '';
    seen.set(p, { status: res.status, location: res.headers.get('location') || '', html });
    for (const m of html.matchAll(/href="(\/[^"#]*)"/g)) { const h = m[1].replace(/&amp;/g, '&'); if (!skip.test(h) && !seen.has(h) && !queue.includes(h)) queue.push(h); }
  }
  assert.ok(seen.size < cap, 'the crawl terminates');
  return seen;
}

test('every page the owner can reach from Home answers, and none of them leaks a bare undefined / NaN / [object Object]', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true });
  const app = createApp({ db, cfg }); await app.listen();
  try {
    await seedDemo(db, cfg);
    const pages = await crawl(cfg.baseUrl, await signIn(cfg.baseUrl, DEMO_USERS[0].email));
    assert.ok(pages.size > 150, `the demo has a few hundred linked pages (${pages.size})`);
    const bad = [...pages].filter(([p, r]) => !(r.status === 200 || (r.status === 303 && /^\/products\//.test(p) && /^\/inventory\//.test(r.location)))).map(([p, r]) => `${r.status} ${p} ${r.location}`);
    assert.deepEqual(bad, [], 'a link that is shown is a page that answers (a product that is also a part may redirect to it)');
    const leaks = [];
    for (const [p, r] of pages) for (const m of r.html.matchAll(/\b(undefined|NaN|\[object Object\])\b/g)) leaks.push(`${p}: …${r.html.slice(Math.max(0, m.index - 50), m.index + 20).replace(/\s+/g, ' ')}…`);
    assert.deepEqual(leaks.slice(0, 20), [], 'nothing renders a JavaScript value by accident');
  } finally { await app.close(); db.close(); }
});

test('a viewer reaches every page the nav shows them, and every link they are shown answers 200 or a clean 403', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true });
  const app = createApp({ db, cfg }); await app.listen();
  try {
    await seedDemo(db, cfg);
    const viewer = DEMO_USERS.find(u => u.role === 'viewer');
    const pages = await crawl(cfg.baseUrl, await signIn(cfg.baseUrl, viewer.email));
    const bad = [...pages].filter(([p, r]) => !(r.status === 200 || r.status === 403 || (r.status === 303 && /^\/products\//.test(p)))).map(([p, r]) => `${r.status} ${p}`);
    assert.deepEqual(bad, []);
    assert.ok(pages.size > 100, `a viewer still sees the read side (${pages.size})`);
    for (const owned of ['/users', '/settings']) assert.ok(!pages.has(owned) || pages.get(owned).status === 403, `${owned} is not a viewer page`);
    assert.ok(!pages.get('/').html.includes('href="/users"'), 'the nav does not show a viewer the people page');
  } finally { await app.close(); db.close(); }
});
