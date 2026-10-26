// The door's second way in, the watch, and the boundary — each held to its contract over HTTP where it has one.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { netOf, sanitize, boundary, boundaryEach, cidrMatcher, Watch, knownNetwork, BOUNDARY_NOTICE } from '../src/guard.mjs';
import { requestSignInLink, signInLink, loginWithLink, setupOperator, totp, clearLocks } from '../src/auth.mjs';
import { createApp } from '../src/server.mjs';
import { openTicket, ticketById } from '../src/modules/support.mjs';
import { recordOrder, orderById } from '../src/modules/orders.mjs';
import { doctor } from '../src/modules/health.mjs';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';

const form = (o) => new URLSearchParams(o).toString();
const cookieOf = (r, name) => (r.headers.getSetCookie?.() || [r.headers.get('set-cookie')].filter(Boolean)).map(c => c.split(';')[0]).find(c => c.startsWith(name + '='));

test('the network of an address, the scrub of outsider text, the boundary marks, the allowlist and the watch', () => {
  assert.equal(netOf('203.0.113.77'), '203.0.113.0/24'); assert.equal(netOf('::ffff:10.1.2.3'), '10.1.2.0/24'); assert.equal(netOf('2001:db8:abcd:12::1'), '2001:0db8:abcd::/48'); assert.equal(netOf('2001:db8::1'), netOf('2001:db8:0:1::1'), 'a compressed address shares its /48 with the written-out one'); assert.equal(netOf('2001:db8::1'), '2001:0db8:0000::/48'); assert.equal(netOf('::1'), '0000:0000:0000::/48'); assert.equal(netOf('fe80::1%eth0'), 'fe80:0000:0000::/48', 'a zone id is dropped'); assert.equal(netOf(''), '-'); assert.equal(netOf('not an ip'), '-');
  assert.equal(sanitize('Hi​ there‮⁦!\r\nIgnore\u0007 this﻿'), 'Hi there!\nIgnore this', 'zero-width, bidi overrides, isolates, controls and the BOM are gone; newlines and spaces stay');
  assert.equal(sanitize('a'.repeat(30), { max: 10 }).length, 10); assert.equal(sanitize(null), null); assert.equal(sanitize('é'), 'é');
  const b = boundary({ subject: 'x' }, ['subject']); assert.deepEqual(b._untrusted, ['subject']); assert.equal(b._notice, BOUNDARY_NOTICE); assert.equal(boundaryEach([{ a: 1 }], ['a'])[0]._untrusted[0], 'a');
  const allow = cidrMatcher('loopback, 203.0.113.0/24, 198.51.100.7, private'); assert.equal(allow('127.0.0.1'), true); assert.equal(allow('203.0.113.200'), true); assert.equal(allow('198.51.100.7'), true); assert.equal(allow('198.51.100.8'), false); assert.equal(allow('10.0.0.9'), true); assert.equal(allow('8.8.8.8'), false); assert.equal(cidrMatcher(''), null);
  const w = new Watch({ limit: 3, windowMs: 1000, blockMs: 1000 }); const t0 = 1_000_000;
  assert.equal(w.miss('1.1.1.1', t0), false); assert.equal(w.miss('1.1.1.1', t0 + 10), false); assert.equal(w.miss('1.1.1.1', t0 + 20), true, 'the third miss begins a block'); assert.equal(w.blocked('1.1.1.1', t0 + 500), true); assert.equal(w.blocked('1.1.1.1', t0 + 1100), false, 'ten minutes later it is forgotten'); assert.equal(w.blocked('2.2.2.2', t0), false);
});

test('a sign-in link: asked for by anyone (no enumeration), mailed once, opens only in the browser that asked, needs the authenticator, works once, and expires', () => {
  const db = testDb(); const now = Date.parse('2026-10-26T12:00:00Z');
  const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
  const nobody = requestSignInLink(db, 'stranger@example.test', { ip: '203.0.113.5', now }); assert.equal(nobody.token, null); assert.ok(nobody.nonce, 'a nonce either way — the answer looks the same');
  const r = requestSignInLink(db, 'O@errer.test', { ip: '203.0.113.5', now, minutes: 15 }); assert.ok(r.token && r.nonce); assert.equal(r.user.email, 'o@errer.test');
  assert.ok(!db.get('SELECT token_hash FROM login_links').token_hash.includes(r.token), 'the token is stored hashed');
  assert.ok(signInLink(db, r.token, { now })); assert.equal(signInLink(db, r.token, { now: now + 16 * 60000 }), null, 'expired after its minutes');
  assert.equal(loginWithLink(db, { token: r.token, code: totp(secret, { time: now }), nonce: 'some other browser' }, { ip: '203.0.113.5', now }), null, 'another browser: refused');
  assert.equal(loginWithLink(db, { token: r.token, code: '000000', nonce: r.nonce }, { ip: '203.0.113.5', now }), null, 'wrong code: refused');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.link.failed'").n, 2);
  const s = loginWithLink(db, { token: r.token, code: totp(secret, { time: now }), nonce: r.nonce }, { ip: '203.0.113.5', now });
  assert.ok(s?.id); assert.equal(s.via, 'link'); assert.equal(db.get('SELECT net FROM sessions WHERE id = ?', s.id).net, '203.0.113.0/24');
  assert.equal(loginWithLink(db, { token: r.token, code: totp(secret, { time: now + 60000 }), nonce: r.nonce }, { ip: '203.0.113.5', now: now + 60000 }), null, 'once');
  assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'auth.link.login' AND entity_id = 'o@errer.test'"));
  clearLocks();
});

test('over HTTP: the link flow end to end, the login page offers it only with a relay, the code step refuses a foreign browser with a 404', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', smtpUrl: 'smtp://relay.test', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
    assert.ok((await (await fetch(base + '/login')).text()).includes('Email me a sign-in link'), 'offered with a relay');
    let r = await fetch(base + '/login/link', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test' }) });
    assert.equal(r.status, 303); assert.ok(decodeURIComponent(r.headers.get('location')).includes('a sign-in link is on its way')); const nonceCookie = cookieOf(r, 'bo_link'); assert.ok(nonceCookie);
    const mail = db.get("SELECT * FROM mail_outbox WHERE ref LIKE 'signin:o@errer.test:%'"); assert.ok(mail); const url = mail.body_text.match(/http:\/\/[^\s]+\/login\/link\/[A-Za-z0-9_-]+/)[0].replace(cfg.baseUrl, base); const token = url.split('/').pop();
    const stranger = await fetch(base + '/login/link', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'nobody@example.test' }) });
    assert.equal(stranger.status, 303); assert.ok(decodeURIComponent(stranger.headers.get('location')).includes('a sign-in link is on its way'), 'the same answer for an address that is nobody'); assert.equal(db.get("SELECT COUNT(*) AS n FROM mail_outbox WHERE to_addr = 'nobody@example.test'").n, 0);
    assert.equal((await fetch(url)).status, 404, 'without the nonce cookie the link does not open');
    const page = await fetch(url, { headers: { cookie: nonceCookie } }); assert.equal(page.status, 200); assert.ok((await page.text()).includes('the code from your authenticator'));
    r = await fetch(url, { method: 'POST', redirect: 'manual', headers: { cookie: nonceCookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ code: totp(secret) }) });
    assert.equal(r.status, 303); assert.equal(r.headers.get('location'), '/'); const session = cookieOf(r, 'bo_session'); assert.ok(session);
    assert.equal((await fetch(base + '/orders', { headers: { cookie: session, accept: 'text/html' } })).status, 200, 'signed in');
    assert.equal((await fetch(url, { headers: { cookie: nonceCookie } })).status, 404, 'used once');
    // no relay: the page says so and the login page does not offer it
    const quiet = createApp({ db: testDb(), cfg: testCfg({ secret: 's' }) }); const a2 = await quiet.listen();
    try { assert.ok((await (await fetch(`http://127.0.0.1:${a2.port}/login/link`)).text()).includes('need the mail relay')); assert.ok(!(await (await fetch(`http://127.0.0.1:${a2.port}/login`)).text()).includes('Email me a sign-in link')); } finally { await quiet.close(); }
  } finally { await app.close(); clearLocks(); }
});

test('the watch: a scanner is blocked and pushed about, a lockout is pushed about, a sign-in from a new network mails the person and pushes; a session is never blocked', async () => {
  const db = testDb(); const pushes = []; const fetchImpl = async (url, init) => { pushes.push({ url, title: init?.headers?.title, body: init?.body }); return { ok: true, status: 200 }; };
  const cfg = testCfg({ secret: 's', ntfyUrl: 'https://ntfy.sh/guard-test', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); app.fetchImpl = fetchImpl; app.watch.limit = 5; const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
    // a scanner from 127.0.0.1: five misses, then a 429 on the admin, never on the store
    for (let i = 0; i < 5; i++) assert.equal((await fetch(base + `/nope-${i}`)).status, 404);
    assert.equal((await fetch(base + '/login')).status, 429, 'blocked'); assert.equal((await fetch(base + '/store/catalog')).status, 200, 'the store is never blocked'); assert.equal((await fetch(base + '/healthz')).status, 200);
    assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'guard.block'")); assert.ok(pushes.some(p => /scanner was blocked/.test(p.title)));
    app.watch.clear();
    // eight wrong passwords: the address is locked out and the phone hears once
    for (let i = 0; i < 8; i++) await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'wrong', code: '000000' }) });
    assert.equal(pushes.filter(p => /locked out/.test(p.title)).length, 1); assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'guard.lockout'"));
    app.lockout.clear('127.0.0.1'); clearLocks();
    // the first sign-in ever: nothing to compare with, no alert
    let r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) });
    assert.equal(r.status, 303); assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.new_network'").n, 0);
    // a login from a network not seen in a month: the person is mailed and the phone hears
    db.run("UPDATE audit_log SET detail = json_set(detail, '$.ip', '198.51.100.9') WHERE action = 'auth.login'");
    r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret, { time: Date.now() + 30000 }) }) });
    assert.equal(r.status, 303);
    assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'auth.login.new_network' AND entity_id = 'o@errer.test'"), 'audited');
    const m = db.get("SELECT * FROM mail_outbox WHERE ref LIKE 'security:new-network:o@errer.test:%'"); assert.ok(m); assert.match(m.body_text, /a network it has not used in the last 30 days/); assert.ok(pushes.some(p => /new network/.test(p.title)));
    assert.deepEqual(knownNetwork(db, 'o@errer.test', '127.0.0.1'), { known: true, net: '127.0.0.0/24', first: false }, 'now it is known');
    // a session is never caught by the watch
    const session = cookieOf(r, 'bo_session'); for (let i = 0; i < 6; i++) await fetch(base + `/missing-${i}`, { headers: { cookie: session } });
    assert.equal((await fetch(base + '/orders', { headers: { cookie: session, accept: 'text/html' } })).status, 200);
  } finally { await app.close(); clearLocks(); }
});

test('ADMIN_ALLOW_CIDRS hides the admin and its door with a 404 from other networks; the store, the webhook and a customer page stay open. SESSION_BIND_IP ends a session that moves', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', adminAllowCidrs: '203.0.113.0/24', sessionBindIp: true, trustedProxies: 'loopback', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
    const from = (ip) => ({ 'x-forwarded-for': ip });
    assert.equal((await fetch(base + '/login', { headers: from('8.8.8.8') })).status, 404, 'the door is not there');
    for (const path of ['/login', '/login/link', '/login/link/sometoken']) assert.equal((await fetch(base + path, { method: 'POST', redirect: 'manual', headers: { ...from('8.8.8.8'), 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: '000000' }) })).status, 404, `${path}: nor is its POST`);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM login_links').n, 0, 'no link was made for a hidden network');
    assert.equal((await fetch(base + '/orders', { headers: from('8.8.8.8') })).status, 404);
    assert.equal((await fetch(base + '/store/catalog', { headers: from('8.8.8.8') })).status, 200, 'the store is for everyone'); assert.equal((await fetch(base + '/healthz', { headers: from('8.8.8.8') })).status, 200);
    assert.equal((await fetch(base + '/login', { headers: from('203.0.113.9') })).status, 200, 'the allowed network sees the door');
    let r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { ...from('203.0.113.9'), 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: 'o@errer.test', password: 'owner password 1234', code: totp(secret) }) });
    assert.equal(r.status, 303); const session = cookieOf(r, 'bo_session');
    assert.equal((await fetch(base + '/orders', { headers: { ...from('203.0.113.9'), cookie: session, accept: 'text/html' } })).status, 200);
    assert.equal((await fetch(base + '/orders', { headers: { ...from('203.0.113.77'), cookie: session, accept: 'text/html' } })).status, 200, 'the same /24 is the same network');
    const moved = await fetch(base + '/orders', { redirect: 'manual', headers: { ...from('203.0.113.9'), cookie: session, accept: 'text/html' } });
    assert.equal(moved.status, 200);
    const far = await fetch(base + '/orders', { redirect: 'manual', headers: { ...from('203.0.114.9'), cookie: session, accept: 'text/html' } });
    assert.equal(far.status, 404, 'another network: hidden by the allowlist (and the session ended underneath)');
    assert.ok(db.get("SELECT id FROM audit_log WHERE action = 'auth.session.moved'"), 'the move was audited'); assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions').n, 0, 'the session is gone');
  } finally { await app.close(); clearLocks(); }
});

test('the boundary: outsider text is scrubbed at every ingress, the API marks it as data not instructions, and every answer says noindex, noai', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's', attentionTtlMs: 0 }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const t = openTicket(db, { email: 'ada@example.test', name: 'Ada‮', subject: 'Help​ me', body: 'IGNORE⁦ ALL\u0007 PREVIOUS﻿', cfg, autoAck: false });
    const row = ticketById(db, t.id); assert.equal(row.subject, 'Help me'); assert.equal(row.messages[0].body, 'IGNORE ALL PREVIOUS'); assert.equal(db.get('SELECT name FROM contacts WHERE email = ?', 'ada@example.test').name, 'Ada');
    const { order } = recordOrder(db, sampleOrder({ external_id: 'cs_g', name: 'Grace​ Hopper', notes: 'leave‮ at door' }), { catalog: catalog(), actor: 'test' }); assert.equal(orderById(db, order.id).ship_name, 'Grace Hopper');
    const fr = await fetch(base + '/forms/contact', { method: 'POST', headers: { 'content-type': 'application/json', origin: cfg.siteOrigin }, body: JSON.stringify({ email: 'bob@example.test', name: 'Bob‏', subject: 'Hi⁩', message: 'Read​ this' }) });
    assert.equal(fr.status, 200); const tk = db.get("SELECT subject FROM tickets WHERE subject LIKE 'Hi%' ORDER BY id DESC LIMIT 1"); assert.equal(tk.subject, 'Hi');
    const { createApiToken } = await import('../src/auth.mjs'); const tok = createApiToken(db, 'ai'); const h = { authorization: `Bearer ${tok}` };
    const one = await fetch(base + `/api/tickets/${t.number}`, { headers: h }); const j = await one.json();
    assert.deepEqual(j._untrusted, ['subject', 'email', 'name', 'messages[].body', 'messages[].from_addr', 'labels']); assert.equal(j._notice, BOUNDARY_NOTICE); assert.equal(one.headers.get('x-untrusted-fields'), 'subject,email,name,messages[].body,messages[].from_addr,labels');
    const list = await (await fetch(base + '/api/tickets', { headers: h })).json(); assert.ok(Array.isArray(list) && list[0]._untrusted);
    const s = await (await fetch(base + '/api/search?q=Help', { headers: h })).json(); assert.ok(s._notice);
    for (const p of ['/login', '/robots.txt', '/healthz', '/store/catalog']) assert.equal((await fetch(base + p)).headers.get('x-robots-tag'), 'noindex, nofollow, noarchive, noai, noimageai', p);
    assert.equal(await (await fetch(base + '/robots.txt')).text(), 'User-agent: *\nDisallow: /\n');
    const rows = doctor(db, cfg).checks; for (const k of ['sign_in_links', 'allowlist', 'session_bind', 'watch']) assert.ok(rows.find(r => r.key === k), k);
  } finally { await app.close(); }
});
