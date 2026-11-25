import { test } from 'node:test';
import assert from 'node:assert/strict';
import { seedDemo, DEMO_PASSWORD, DEMO_TOTP_SECRET, DEMO_USERS } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { totp } from '../src/auth.mjs';
import { orderToken } from '../src/modules/delivery.mjs';
import { invoiceToken } from '../src/modules/invoices.mjs';
import { testDb, testCfg } from './helpers.mjs';

// Log in through the form, as a demo person, and hand back the session cookie.
async function signIn(base, email) {
  const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }).toString() });
  assert.equal(r.status, 303, `${email} signs in`);
  return r.headers.get('set-cookie').split(';')[0];
}

// The public answers that choose to be cached say so themselves; everything else is send()'s default.
const OWN_CACHE = { '/status': 'public, max-age=60', '/status.json': 'public, max-age=60', '/store/catalog': 'public, max-age=60', '/store/estimate': 'public, max-age=300',
  '/robots.txt': 'public, max-age=86400', '/security.txt': 'public, max-age=86400', '/.well-known/security.txt': 'public, max-age=86400' };
// An answer a browser may not keep: no-store (a customer's page and an attachment add private), and never sniffed.
const unkept = (path, r) => r.sniff === 'nosniff' && (OWN_CACHE[path.split('?')[0]] ? r.cache === OWN_CACHE[path.split('?')[0]] : /(^|, )no-store$/.test(r.cache || ''));

// Every internal link reachable from Home, breadth-first: the page it lands on, its status and its HTML.
async function crawl(base, cookie, { skip = /\.(csv|json|png|svg)$|\/logout|\/u\/|\/confirm\//, cap = 1200 } = {}) {
  const seen = new Map(); const queue = ['/'];
  while (queue.length && seen.size < cap) {
    const p = queue.shift(); if (seen.has(p)) continue;
    const res = await fetch(base + p, { headers: { cookie }, redirect: 'manual' });
    const html = (res.headers.get('content-type') || '').includes('text/html') ? await res.text() : '';
    seen.set(p, { status: res.status, location: res.headers.get('location') || '', html, cache: res.headers.get('cache-control'), sniff: res.headers.get('x-content-type-options') });
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
    const kept = [...pages].filter(([p, r]) => r.status !== 303 && !unkept(p, r)).map(([p, r]) => `${r.status} ${p}: cache-control ${r.cache}, nosniff ${r.sniff}`);
    assert.deepEqual(kept, [], 'every page and export an operator opens, its parameters filled by the links, is no-store and nosniff');
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

test('every GET answer is no-store and nosniff unless it is a public answer that names its own caching: the admin, the API, the CSV and JSON exports, the customer\'s page', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true });
  const app = createApp({ db, cfg }); await app.listen();
  try {
    await seedDemo(db, cfg);
    const cookie = await signIn(cfg.baseUrl, DEMO_USERS[0].email);
    // Every GET route the router holds, read off the routes themselves rather than a list, each key filled from the
    // seeded demo: a key names the same record on every route under one prefix. A route whose key has no value
    // here fails the test by name, so a new route is asked too.
    const t = db.get('SELECT t.* FROM tickets t JOIN ticket_messages m ON m.ticket_id = t.id JOIN attachments a ON a.ticket_message_id = m.id ORDER BY a.id LIMIT 1');
    const att = db.get('SELECT a.id FROM attachments a JOIN ticket_messages m ON m.id = a.ticket_message_id WHERE m.ticket_id = ? LIMIT 1', t.id);
    const o = db.get('SELECT * FROM orders ORDER BY id LIMIT 1'); const inv = db.get('SELECT * FROM invoices ORDER BY id LIMIT 1');
    const ct = db.get('SELECT * FROM contacts WHERE unsubscribe_token IS NOT NULL ORDER BY id LIMIT 1');
    const one = (sql) => db.get(sql) || {}; const product = [...(await app.catalog()).products.keys()][0];
    const ids = { '/store/session/': 'cs_test_pending01', '/orders/': o.id, '/tickets/': t.id, '/contacts/': ct.id, '/api/customers/': ct.id, '/invoices/': inv.id,
      '/purchasing/': one('SELECT id FROM purchase_orders ORDER BY id LIMIT 1').id, '/warranty/': one('SELECT id FROM warranty_claims ORDER BY id LIMIT 1').id, '/devices/recall/': one('SELECT id FROM recalls ORDER BY id LIMIT 1').id ?? 1 };
    const value = (tpl, key) => {
      const prefix = tpl.slice(0, tpl.indexOf(':'));
      if (key === 'number') return /^\/(api\/)?tickets\//.test(tpl) ? t.number : prefix === '/i/' ? inv.number : o.number;
      if (key === 'token') return { '/o/': orderToken(cfg.secret, o.number), '/i/': invoiceToken(cfg.secret, inv.number), '/u/': ct.unsubscribe_token }[prefix] ?? 'no-such-token';
      return { id: ids[prefix], aid: att.id, sku: prefix === '/inventory/' ? one('SELECT sku FROM inventory_items ORDER BY sku LIMIT 1').sku : product, code: one('SELECT code FROM discounts ORDER BY code LIMIT 1').code,
        serial: one('SELECT serial FROM devices ORDER BY id LIMIT 1').serial, slug: one('SELECT slug FROM campaigns ORDER BY id LIMIT 1').slug }[key];
    };
    const routes = app.router.routes.filter(r => r.method === 'GET').map(r => {
      let i = 0; const tpl = r.re.source.replace(/^\^/, '').replace(/\\?\/\?\$$/, '').replace(/\\\//g, '/').replace(/\\\./g, '.').replace(/\(\[\^\/\]\+\)/g, () => `:${r.keys[i++]}`) || '/';
      return { tpl, path: tpl.replace(/:(\w+)/g, (_, k) => { const v = value(tpl, k); assert.ok(v != null, `${tpl}: the demo gives :${k} a value`); return encodeURIComponent(String(v)); }) };
    });
    assert.ok(routes.length > 100 && routes.some(r => r.tpl === '/api/users') && routes.some(r => r.tpl === '/contacts/:id/export.json'), `the route list is read whole (${routes.length})`);
    // what each answer must say: a public answer that names its own caching keeps it, a customer's page and an
    // attachment add private, everything else is send()'s no-store; a redirect carries no body to keep
    const PRIVATE = /^\/(o|i|store\/session)\/|^\/tickets\/:id\/att\//;
    const answers = [];
    for (const r of routes) for (const who of ['signed in', 'signed out']) {
      const res = await fetch(cfg.baseUrl + r.path, { headers: who === 'signed in' ? { cookie } : {}, redirect: 'manual' }); const len = (await res.arrayBuffer()).byteLength;
      answers.push({ ...r, who, status: res.status, len, cache: res.headers.get('cache-control'), sniff: res.headers.get('x-content-type-options') });
    }
    const wrong = answers.filter(a => a.status === 303 ? a.len !== 0 || (a.cache != null && !/(^|, )no-store$/.test(a.cache))
      : a.sniff !== 'nosniff' || a.cache !== (OWN_CACHE[a.tpl] || (PRIVATE.test(a.tpl) ? 'private, no-store' : 'no-store'))).map(a => `${a.who} ${a.status} ${a.path}: cache-control ${a.cache}, nosniff ${a.sniff}`);
    assert.deepEqual(wrong, [], 'an answer a browser could keep on disk, or sniff into another type');
    // the keys were real: the answers that hold a person's data were asked for a record that exists
    const ok = new Set(answers.filter(a => a.who === 'signed in' && a.status === 200).map(a => a.tpl));
    for (const tpl of ['/contacts/:id/export.json', '/api/customers/:id/export', '/api/customers/:id', '/api/orders/:number', '/api/orders/:number/status-url', '/api/tickets/:number', '/tickets/:id/att/:aid', '/o/:number/:token', '/i/:number/:token', '/store/session/:id', '/api/products/:sku', '/orders.csv', '/api/users'])
      assert.ok(ok.has(tpl), `${tpl} answered 200 to the owner`);
    assert.ok(ok.size > 100, `most routes answered 200 to the owner (${ok.size} of ${routes.length})`);
  } finally { await app.close(); db.close(); }
});
