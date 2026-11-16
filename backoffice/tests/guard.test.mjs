// The door's second way in, the watch, and the boundary — each held to its contract over HTTP where it has one.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { netOf, sanitize, boundary, boundaryEach, cidrMatcher, parseCidrRule, Watch, knownNetwork, BOUNDARY_NOTICE } from '../src/guard.mjs';
import { seedDemo, DEMO_PASSWORD, DEMO_TOTP_SECRET, DEMO_USERS } from '../src/modules/demo.mjs';
import { orderToken } from '../src/modules/delivery.mjs';
import { requestSignInLink, signInLink, loginWithLink, setupOperator, totp, clearLocks, sessionFor } from '../src/auth.mjs';
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
  const db = testDb(); const now = Date.now();   // a real clock: the session this opens expires 12 h from "now", and a fixed day the clock has passed is already expired
  const secret = setupOperator(db, 'owner password 1234', { email: 'o@errer.test', name: 'Owner One' });
  const nobody = requestSignInLink(db, 'stranger@example.test', { ip: '203.0.113.5', now }); assert.equal(nobody.token, null); assert.ok(nobody.nonce, 'a nonce either way — the answer looks the same');
  const r = requestSignInLink(db, 'O@errer.test', { ip: '203.0.113.5', now, minutes: 15 }); assert.ok(r.token && r.nonce); assert.equal(r.user.email, 'o@errer.test');
  assert.ok(!db.get('SELECT token_hash FROM login_links').token_hash.includes(r.token), 'the token is stored hashed');
  assert.ok(signInLink(db, r.token, { now })); assert.equal(signInLink(db, r.token, { now: now + 16 * 60000 }), null, 'expired after its minutes');
  assert.equal(loginWithLink(db, { token: r.token, code: totp(secret, { time: now }), nonce: 'some other browser' }, { ip: '203.0.113.5', now }), null, 'another browser: refused');
  assert.equal(loginWithLink(db, { token: r.token, code: '000000', nonce: r.nonce }, { ip: '203.0.113.5', now }), null, 'wrong code: refused');
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.link.failed'").n, 2);
  const s = loginWithLink(db, { token: r.token, code: totp(secret, { time: now }), nonce: r.nonce }, { ip: '203.0.113.5', now });
  assert.ok(s?.id); assert.equal(s.via, 'link'); assert.equal(sessionFor(db, s.id).net, '203.0.113.0/24'); assert.equal(db.get('SELECT COUNT(*) AS n FROM sessions WHERE id = ?', s.id).n, 0, 'the row holds the hash, not the cookie');
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

// ADMIN_ALLOW_CIDRS names networks, and an IPv6 network is a CIDR: a rotating privacy address is useless as an exact
// rule. A rule that is not a range at all used to match nothing — a 404 on every admin path — so it stops the boot.
test('the allowlist takes v6 CIDRs and exact v6 addresses however they are spelled, keeps v4-mapped clients on their v4 rule, and refuses an unparseable rule at load', async () => {
  const allow = cidrMatcher('2001:db8:1200::/56, 2a02:abcd:12::/48, 2001:DB8:0:0:0:0:0:99, 203.0.113.0/24, 198.51.100.7');
  // a /56: the first 56 bits decide
  for (const ip of ['2001:db8:1200::1', '2001:db8:1200:ff:aaaa:bbbb:cccc:dddd', '2001:0db8:1200:00ff::1', '2001:DB8:1200:42::7']) assert.equal(allow(ip), true, ip);
  for (const ip of ['2001:db8:1200:100::1', '2001:db8:1201::1', '2001:db8:11ff:ffff::1', '2001:db9:1200::1']) assert.equal(allow(ip), false, ip);
  // a /48
  for (const ip of ['2a02:abcd:12::5', '2a02:abcd:12:ffff:ffff:ffff:ffff:ffff', '2a02:abcd:0012:9::1']) assert.equal(allow(ip), true, ip);
  for (const ip of ['2a02:abcd:13::5', '2a02:abcd:11:ffff::1', '2a02:abce:12::5']) assert.equal(allow(ip), false, ip);
  // an exact v6 address matches whatever its spelling, on either side
  for (const ip of ['2001:db8::99', '2001:0db8:0000:0000:0000:0000:0000:0099', '2001:DB8::99']) assert.equal(allow(ip), true, ip);
  assert.equal(allow('2001:db8::98'), false);
  // v4 rules still hold, and a v4 client that arrives v4-mapped (a dual-stack socket) is that v4 client
  assert.equal(allow('203.0.113.9'), true); assert.equal(allow('::ffff:203.0.113.9'), true); assert.equal(allow('::ffff:cb00:7109'), true, 'the hex spelling of the same mapped address');
  assert.equal(allow('::ffff:198.51.100.7'), true); assert.equal(allow('::ffff:198.51.100.8'), false); assert.equal(allow('203.0.114.9'), false); assert.equal(allow('::ffff:8.8.8.8'), false);
  // a v4 rule never lets a v6 client in, and the other way around; nothing that is not an address gets in
  assert.equal(cidrMatcher('0.0.0.0/0')('2001:db8::1'), false); assert.equal(cidrMatcher('::/0')('8.8.8.8'), false); assert.equal(cidrMatcher('::/0')('2001:db8::1'), true); assert.equal(cidrMatcher('0.0.0.0/0')('8.8.8.8'), true);
  for (const ip of ['', 'garbage', undefined, '2001:db8:1200::/56']) assert.equal(allow(ip), false, String(ip));
  // the named ranges read as before
  const named = cidrMatcher('loopback, private'); for (const ip of ['127.0.0.1', '127.9.9.9', '::1', '0:0:0:0:0:0:0:1', '::ffff:127.0.0.1', '10.1.2.3', '172.16.0.1', '172.31.255.255', '192.168.5.5', '::ffff:10.0.0.9']) assert.equal(named(ip), true, ip);
  for (const ip of ['172.32.0.1', '172.15.0.1', '11.0.0.1', '192.169.0.1', '8.8.8.8', '::2', 'fd00::1']) assert.equal(named(ip), false, ip);
  // a rule that is none of those is refused where the list is read, by name: never a silent 404 for everyone
  for (const bad of ['office', '2a02:zz::/56', '203.0.113.0/33', '2001:db8::/129', '300.1.1.1/24', '203.0.113/24', '203.0.113.0/', '/24', '::ffff:10.0.0.0/90', 'Private']) {
    assert.equal(parseCidrRule(bad), null, bad);
    assert.throws(() => cidrMatcher(`203.0.113.0/24, ${bad}`), (e) => e.message.startsWith('ADMIN_ALLOW_CIDRS') && e.message.includes(JSON.stringify(bad)) && /would hide the admin from everyone/.test(e.message), bad);
  }
  assert.throws(() => cidrMatcher('office, 2a02:zz::/56'), /"office", "2a02:zz::\/56" are not/);
  assert.equal(cidrMatcher(''), null); assert.equal(cidrMatcher(' , '), null, 'an empty list is no allowlist');
  // and the server does not start with one: the reason is the error, not a 404 on every admin path
  const db = testDb();
  assert.throws(() => createApp({ db, cfg: testCfg({ secret: 's', adminAllowCidrs: '203.0.113.0/24, 2a02:1234::/5six' }) }), /ADMIN_ALLOW_CIDRS: "2a02:1234::\/5six" is not/);
  assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'deploy.boot'").n, 0, 'a refused boot is not written down as a boot');
  // over HTTP: an operator on a v6 /56 reaches the door; a neighbor outside it sees a 404
  const cfg = testCfg({ secret: 's', adminAllowCidrs: '2001:db8:1200::/56', trustedProxies: 'loopback', attentionTtlMs: 0 }); const app = createApp({ db: testDb(), cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const from = (ip) => ({ 'x-forwarded-for': ip });
    assert.equal((await fetch(base + '/login', { headers: from('2001:db8:1200:7::abcd') })).status, 200, 'inside the /56');
    assert.equal((await fetch(base + '/login', { headers: from('2001:db8:1300::1') })).status, 404, 'outside it');
    assert.equal((await fetch(base + '/healthz', { headers: from('2001:db8:1300::1') })).status, 200, 'the public endpoints stay open');
  } finally { await app.close(); }
});

// Every answer goes out through send() (http.mjs), which is where the hardening headers are written. Two JSON
// routes wrote their own response and carried none of them; this walks every registered GET route so the next
// hand-rolled res.end() fails here.
test('every GET route answers through send(): nosniff and noindex on each, /api/users and /api/health included', async () => {
  const db = testDb(); const cfg = testCfg({ demo: true, attentionTtlMs: 0 }); const app = createApp({ db, cfg }); await app.listen(); const base = cfg.baseUrl;
  try {
    await seedDemo(db, cfg);
    const r0 = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: DEMO_USERS[0].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }) });
    assert.equal(r0.status, 303); const cookie = r0.headers.get('set-cookie').split(';')[0];
    // a real id for each parameter, so the handler itself answers (a miss would only prove the error page)
    const one = (sql) => { try { const row = db.get(sql); return row ? String(Object.values(row)[0]) : 'x'; } catch { return 'x'; } };
    const order = db.get('SELECT id, number FROM orders ORDER BY id LIMIT 1'); const sku = [...(await app.catalog()).products.keys()][0];
    const fill = (path) => {
      const v = /^\/(api\/orders|q\/o)\//.test(path) ? [order.number] : /^\/o\//.test(path) ? [order.number, orderToken(cfg.secret, order.number)] : /^\/orders\//.test(path) ? [order.id]
        : /^\/api\/tickets\//.test(path) ? [one('SELECT number FROM tickets LIMIT 1')] : /^\/tickets\/[^/]+\/att\//.test(path) ? [one('SELECT m.ticket_id FROM attachments a JOIN ticket_messages m ON m.id = a.ticket_message_id LIMIT 1'), one('SELECT id FROM attachments LIMIT 1')] : /^\/tickets\//.test(path) ? [one('SELECT id FROM tickets LIMIT 1')]
        : /^\/(contacts|api\/customers)\//.test(path) ? [one('SELECT id FROM contacts LIMIT 1')] : /^\/(api\/)?products\//.test(path) ? [sku] : /^\/inventory\//.test(path) ? [one('SELECT sku FROM inventory_items LIMIT 1')]
        : /^\/warranty\//.test(path) ? [one('SELECT id FROM warranty_claims LIMIT 1')] : /^\/invoices\//.test(path) ? [one('SELECT id FROM invoices LIMIT 1')] : /^\/campaigns\//.test(path) ? [one('SELECT slug FROM campaigns LIMIT 1')]
        : /^\/purchasing\//.test(path) ? [one('SELECT id FROM purchase_orders LIMIT 1')] : /^\/api\/discounts\//.test(path) ? [one('SELECT code FROM discounts LIMIT 1')] : [];
      let i = 0; return path.replace(/\(\[\^\/\]\+\)/g, () => encodeURIComponent(v[i++] ?? 'x'));
    };
    const gets = app.router.routes.filter(r => r.method === 'GET'); assert.ok(gets.length > 100, `the admin registers its GET routes (${gets.length})`);
    const seen = []; const bare = [];
    for (const route of gets) {
      const path = fill(route.re.source.replace(/^\^/, '').replace(/\\\/\?\$$/, '').replace(/\\\//g, '/'));
      const r = await fetch(base + path, { headers: { cookie }, redirect: 'manual' }); await r.arrayBuffer();
      assert.ok(r.status < 500 || (path === '/api/health' && r.status === 503), `${path} answered ${r.status}`);
      seen.push(`${r.status} ${path}`);
      if (r.status >= 300 && r.status < 400) continue;   // a redirect has no body to sniff or index
      // framing: DENY everywhere but the campaign preview, which the admin frames itself (SAMEORIGIN)
      if (r.headers.get('x-content-type-options') !== 'nosniff' || r.headers.get('x-robots-tag') !== 'noindex, nofollow, noarchive, noai, noimageai' || !['DENY', 'SAMEORIGIN'].includes(r.headers.get('x-frame-options'))) bare.push(`${r.status} ${path}`);
    }
    assert.deepEqual(bare, [], 'an answer without the hardening headers was written past send()');
    for (const p of ['/api/users', '/api/orders', `/orders/${order.id}`, `/api/orders/${order.number}/status-url`, '/settings', '/audit.csv']) assert.ok(seen.includes(`200 ${p}`), `${p} was answered by its handler (${seen.find(s => s.endsWith(' ' + p))})`);
    assert.ok(seen.some(s => /^(200|503) \/api\/health$/.test(s)));
    // the two that used to write their own response: their JSON is intact, and an owner-only list stays the owner's
    const users = await fetch(base + '/api/users', { headers: { cookie } }); assert.match(users.headers.get('content-type'), /^application\/json/); assert.ok((await users.json()).some(u => u.email === DEMO_USERS[0].email));
    const health = await fetch(base + '/api/health', { headers: { cookie } }); assert.equal(health.headers.get('x-content-type-options'), 'nosniff'); const h = await health.json(); assert.equal(health.status, h.ok ? 200 : 503); assert.ok(Array.isArray(h.checks));
    const r1 = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email: DEMO_USERS[1].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }) });
    const ops = await fetch(base + '/api/users', { headers: { cookie: r1.headers.get('set-cookie').split(';')[0] } }); assert.equal(ops.status, 403); assert.equal(ops.headers.get('x-content-type-options'), 'nosniff');
  } finally { await app.close(); }
});
