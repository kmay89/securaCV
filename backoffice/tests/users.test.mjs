// People and permissions: an owner invites by link, the person sets their
// own password and authenticator, roles decide by path and method, every
// action is written down under the person's name, and the QR the phone
// scans matches a reference encoder bit for bit.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { testDb, testCfg, catalog, sampleOrder, fixture } from './helpers.mjs';
import { setupOperator, login, sessionFor, inviteUser, inviteFor, acceptInvite, confirmTotp, pendingTotp, beginTotpRotation, changePassword, setRole, setStatus, removeUser, listUsers, userByEmail, can, ROLES, totp, carryLegacyOperator, hashPassword, base32Encode, endOtherSessions } from '../src/auth.mjs';
import { qrMatrix, qrSvg } from '../src/qr.mjs';
import { createApp } from '../src/server.mjs';
import { recordOrder, timeline, addNote } from '../src/modules/orders.mjs';
import { openTicket } from '../src/modules/support.mjs';
import { seedDemo, DEMO_PASSWORD, DEMO_TOTP_SECRET, DEMO_USERS } from '../src/modules/demo.mjs';
import { doctor } from '../src/modules/health.mjs';

const form = (o) => new URLSearchParams(o).toString();
async function boot(cfgOver = {}) {
  const db = testDb(); const cfg = testCfg(cfgOver); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const secret = setupOperator(db, 'owner password 1234', { email: 'owner@errer.test', name: 'Olive Owner' });
  const signIn = async (email, password, code) => { const r = await fetch(base + '/login', { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ email, password, code }) }); const sc = r.headers.get('set-cookie'); if (!sc || r.headers.get('location') !== '/') return null; const cookie = sc.split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions WHERE id = ?', cookie.split('=')[1]).csrf; return { cookie, csrf }; };
  const as = (s) => ({ get: (p) => fetch(base + p, { headers: { cookie: s.cookie }, redirect: 'manual' }), post: (p, body = {}) => fetch(base + p, { method: 'POST', redirect: 'manual', headers: { cookie: s.cookie, 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: s.csrf, ...body }) }) });
  return { db, cfg, app, base, secret, signIn, as, close: async () => { await app.close(); db.close(); } };
}

test('the permission rule: owner everything, ops the shop, support the people, viewer reads', () => {
  for (const r of ROLES) assert.ok(can(r, 'GET', '/orders'), r);
  assert.ok(can('owner', 'POST', '/users/invite') && can('owner', 'GET', '/books') && can('owner', 'POST', '/contacts/3/forget'));
  assert.ok(!can('ops', 'GET', '/users') && !can('ops', 'GET', '/settings') && !can('ops', 'GET', '/books') && !can('ops', 'POST', '/contacts/3/forget') && !can('ops', 'GET', '/contacts/3/export.json'));
  assert.ok(can('ops', 'POST', '/orders/3/transition') && can('ops', 'POST', '/purchasing/draft') && can('ops', 'GET', '/health') && can('ops', 'POST', '/campaigns/x/send') && can('ops', 'GET', '/me') && can('ops', 'POST', '/logout'));
  assert.ok(can('support', 'POST', '/tickets/3/reply') && can('support', 'POST', '/orders/3/note') && can('support', 'POST', '/contacts/3/tags') && can('support', 'POST', '/warranty/new') && can('support', 'POST', '/waitlist/notify') && can('support', 'GET', '/inventory'));
  assert.ok(!can('support', 'POST', '/orders/3/transition') && !can('support', 'POST', '/orders/3/refund') && !can('support', 'POST', '/inventory/move') && !can('support', 'POST', '/campaigns/x/send') && !can('support', 'POST', '/contacts/3/forget') && !can('support', 'GET', '/settings'));
  assert.ok(can('viewer', 'GET', '/health') && can('viewer', 'GET', '/tickets/3') && can('viewer', 'POST', '/me/password') && !can('viewer', 'POST', '/tickets/3/reply') && !can('viewer', 'POST', '/orders/3/note') && !can('viewer', 'GET', '/users'));
  assert.ok(!can('nobody', 'GET', '/orders') && !can(undefined, 'GET', '/'));
  assert.ok(can('viewer', 'GET', '/api/orders') && !can('viewer', 'POST', '/api/orders') && !can('ops', 'POST', '/api/jobs/backup') && can('ops', 'POST', '/api/orders/EL-1/transition'));
  // the id segment is matched by position, decoded, never by spelling: an encoded or signed id is the same id to the handler
  for (const p of ['/contacts/%31/export.json', '/contacts/+1/forget', '/contacts/1e0/forget', '/api/customers/%31/export', '/api/customers/+1/forget', '/contacts/1/export.json/']) assert.ok(!can('ops', 'GET', p) && !can('ops', 'POST', p) && !can('viewer', 'GET', p), p);
  assert.ok(!can('viewer', 'GET', '/contacts/%zz/export.json'), 'a path that does not decode is refused');
  assert.ok(can('support', 'POST', '/orders/%31/note') && !can('viewer', 'POST', '/orders/%31/note'));
});

test('invite → password → authenticator → open: the link works once, for 48 hours, and nobody is handed a password', async () => {
  const b = await boot();
  try {
    const owner = await b.signIn('owner@errer.test', 'owner password 1234', totp(b.secret)); assert.ok(owner);
    assert.throws(() => inviteUser(b.db, { email: 'not an email', name: 'X' }), /valid email/); assert.throws(() => inviteUser(b.db, { email: 'a@b.co', name: 'X', role: 'god' }), /role is one of/); assert.throws(() => inviteUser(b.db, { email: 'owner@errer.test', name: 'X' }), /already a person/);
    // over HTTP, as the owner: the page shows the link once; the mail is queued when there is a relay (none here)
    let r = await b.as(owner).post('/users/invite', { name: 'Pat Packer', email: 'Pat@Errer.test', role: 'ops' }); assert.equal(r.status, 303); const tok = new URL(r.headers.get('location'), b.base).searchParams.get('invite'); assert.ok(tok);
    const u = userByEmail(b.db, 'pat@errer.test'); assert.equal(u.status, 'invited'); assert.equal(u.role, 'ops'); assert.equal(u.invited_by, 'owner@errer.test'); assert.equal(u.password_set_at, null);
    assert.equal((await b.as(owner).get('/users')).status, 200); assert.ok((await (await b.as(owner).get(`/users?invite=${tok}`)).text()).includes(`/invite/${tok}`));
    assert.equal(b.db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE ref = ?', 'invite:pat@errer.test').n, 0, 'no relay configured: the link is handed over by the owner');
    // the person, no session: the page opens; a short password is refused; the two must match
    r = await fetch(`${b.base}/invite/${tok}`); assert.equal(r.status, 200); assert.ok((await r.text()).includes('pat@errer.test')); assert.equal((await fetch(`${b.base}/invite/nope`)).status, 404);
    r = await fetch(`${b.base}/invite/${tok}`, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'short', again: 'short' }) }); assert.match(decodeURIComponent(r.headers.get('location')), /12 characters/);
    r = await fetch(`${b.base}/invite/${tok}`, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'pat packs parcels 1', again: 'pat packs parcels 2' }) }); assert.match(decodeURIComponent(r.headers.get('location')), /differ/);
    r = await fetch(`${b.base}/invite/${tok}`, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ password: 'pat packs parcels 1', again: 'pat packs parcels 1' }) }); assert.equal(r.status, 303);
    const pending = pendingTotp(b.db, u.id); assert.ok(pending && pending.length === 32); assert.equal(userByEmail(b.db, 'pat@errer.test').status, 'invited', 'not open until a code is seen');
    r = await fetch(`${b.base}/invite/${tok}`); const page = await r.text(); assert.ok(page.includes('<svg') && page.includes(pending) && page.includes('Open my account'), 'the QR and the secret');
    assert.equal(login(b.db, { email: 'pat@errer.test', password: 'pat packs parcels 1', code: totp(pending) }), null, 'cannot log in before confirming');
    r = await fetch(`${b.base}/invite/${tok}/confirm`, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ code: '000000' }) }); assert.equal(r.status, 400);
    r = await fetch(`${b.base}/invite/${tok}/confirm`, { method: 'POST', redirect: 'manual', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: form({ code: totp(pending) }) }); assert.equal(r.status, 303); assert.match(r.headers.get('location'), /^\/login\?ok=/);
    const pat = userByEmail(b.db, 'pat@errer.test'); assert.equal(pat.status, 'active'); assert.equal(inviteFor(b.db, tok), null, 'the link is burned'); assert.equal((await fetch(`${b.base}/invite/${tok}`)).status, 404);
    const s = await b.signIn('pat@errer.test', 'pat packs parcels 1', totp(pending)); assert.ok(s, 'and now Pat can log in');
    assert.equal(login(b.db, { password: 'pat packs parcels 1', code: totp(pending) }), null, 'two people: the email is required');
    // an expired invitation is dead; a new link revives it with a fresh token
    const v = inviteUser(b.db, { email: 'vic@errer.test', name: 'Vic Viewer', role: 'viewer' }); b.db.run('UPDATE users SET invite_expires_at = ? WHERE id = ?', '2000-01-01T00:00:00.000Z', v.id); assert.equal(inviteFor(b.db, v.invite_token), null); assert.throws(() => acceptInvite(b.db, v.invite_token, 'a long enough password'), /not valid/);
    const v2 = inviteUser(b.db, { email: 'vic@errer.test', name: 'Vic Viewer', role: 'viewer' }); assert.notEqual(v2.invite_token, v.invite_token); assert.ok(inviteFor(b.db, v2.invite_token));
    const acts = b.db.all("SELECT action, actor FROM audit_log WHERE entity = 'user' ORDER BY id").map(r => `${r.action}:${r.actor}`);
    assert.ok(acts.includes('user.invite:owner@errer.test') && acts.includes('user.accept:pat@errer.test') && acts.includes('user.activate:pat@errer.test') && acts.includes('auth.login:pat@errer.test'), acts.join(' '));
  } finally { await b.close(); }
});

test('roles are enforced on every request: the sidebar shrinks, a forbidden POST is refused with the reason, an owner-only page is 403', async () => {
  const b = await boot(); const cat = catalog();
  try {
    const owner = await b.signIn('owner@errer.test', 'owner password 1234', totp(b.secret));
    const mk = (email, name, role) => { const u = inviteUser(b.db, { email, name, role }); acceptInvite(b.db, u.invite_token, `${name} has a long password`); const p = pendingTotp(b.db, u.id); confirmTotp(b.db, u.id, totp(p)); return { email, secret: p, pw: `${name} has a long password` }; };
    const ops = mk('ops@errer.test', 'Oli Ops', 'ops'), sup = mk('sup@errer.test', 'Sam Support', 'support'), vw = mk('vw@errer.test', 'Val Viewer', 'viewer');
    const S = {}; for (const [k, who] of Object.entries({ ops, sup, vw })) { S[k] = await b.signIn(who.email, who.pw, totp(who.secret)); assert.ok(S[k], k); }
    const { order } = recordOrder(b.db, sampleOrder(), { catalog: cat }); const t = openTicket(b.db, { email: 'q@example.test', subject: 'Hi', body: 'Hello', cfg: b.cfg, autoAck: false });
    // the sidebar: what each can open
    const nav = async (s) => (await (await b.as(s).get('/')).text()).match(/<nav[^>]*>[\s\S]*?<\/nav>/)[0];
    assert.ok((await nav(owner)).includes('href="/users"') && (await nav(owner)).includes('href="/books"'));
    const on = await nav(S.ops); assert.ok(!on.includes('href="/users"') && !on.includes('href="/settings"') && !on.includes('href="/books"') && on.includes('href="/inventory"') && on.includes('href="/health"'));
    assert.ok((await nav(S.vw)).includes('href="/orders"') && !(await nav(S.vw)).includes('href="/users"'));
    // owner-only pages
    assert.equal((await b.as(S.ops).get('/users')).status, 403); assert.equal((await b.as(S.ops).get('/settings')).status, 403); assert.equal((await b.as(S.ops).get('/books')).status, 403); assert.equal((await b.as(S.vw).get('/books')).status, 403); assert.equal((await b.as(owner).get('/users')).status, 200);
    assert.equal((await b.as(S.sup).get(`/contacts/${order.contact_id}/export.json`)).status, 403);
    assert.equal((await b.as(S.ops).get(`/contacts/%3${order.contact_id}/export.json`)).status, 403, 'a percent-encoded id is the same id'); assert.equal((await b.as(S.ops).post(`/contacts/+${order.contact_id}/forget`, { sure: 'forget' })).status, 403, 'a signed id too'); assert.ok(!b.db.get('SELECT forgotten_at FROM contacts WHERE id = ?', order.contact_id).forgotten_at);
    assert.equal((await b.as(S.vw).get(`/api/customers/%3${order.contact_id}/export`)).status, 403);
    // a forbidden POST goes back to the page with the reason; an allowed one works
    let r = await b.as(S.sup).post(`/orders/${order.id}/transition`, { to: 'packed' }); assert.equal(r.status, 403, 'no referer: a plain refusal'); assert.equal(b.db.get('SELECT status FROM orders WHERE id = ?', order.id).status, 'paid');
    r = await fetch(`${b.base}/orders/${order.id}/transition`, { method: 'POST', redirect: 'manual', headers: { cookie: S.sup.cookie, referer: `${b.base}/orders/${order.id}`, accept: 'text/html', 'content-type': 'application/x-www-form-urlencoded' }, body: form({ _csrf: S.sup.csrf, to: 'packed' }) });
    assert.equal(r.status, 303); assert.match(decodeURIComponent(r.headers.get('location')), /your role \(support\) cannot do this/);
    r = await b.as(S.sup).post(`/orders/${order.id}/note`, { body: 'Customer called' }); assert.equal(r.status, 303); assert.equal(timeline(b.db, order.id)[0].actor, 'sup@errer.test', 'written down under their name');
    r = await b.as(S.sup).post(`/tickets/${t.id}/reply`, { body: 'Hello back' }); assert.equal(r.status, 303); assert.equal(b.db.get("SELECT actor FROM audit_log WHERE action = 'ticket.reply' ORDER BY id DESC LIMIT 1").actor, 'sup@errer.test');
    r = await b.as(S.vw).post(`/orders/${order.id}/note`, { body: 'x' }); assert.equal(r.status, 403); r = await b.as(S.vw).get(`/orders/${order.id}`); assert.equal(r.status, 200);
    r = await b.as(S.ops).post(`/orders/${order.id}/transition`, { to: 'packed' }); assert.equal(r.status, 303); assert.equal(b.db.get("SELECT actor FROM audit_log WHERE action = 'order.packed' ORDER BY id DESC LIMIT 1").actor, 'ops@errer.test');
    // the API with a session follows the role; an owner-made token does not carry one
    assert.equal((await b.as(S.vw).get('/api/orders')).status, 200); assert.equal((await fetch(`${b.base}/api/orders/${order.number}/note`, { method: 'POST', headers: { cookie: S.vw.cookie, 'content-type': 'application/json' }, body: '{"body":"x"}' })).status, 403);
    assert.equal((await b.as(S.ops).get('/api/users')).status, 403); assert.equal((await b.as(owner).get('/api/users')).status, 200);
    // disabling ends the person's sessions at once; the last owner cannot be demoted, disabled or removed
    const vid = userByEmail(b.db, 'vw@errer.test').id; r = await b.as(owner).post(`/users/${vid}/status`, { status: 'disabled' }); assert.equal(r.status, 303);
    assert.equal((await b.as(S.vw).get('/orders')).status, 303, 'bounced to login'); assert.equal(login(b.db, { email: 'vw@errer.test', password: vw.pw, code: totp(vw.secret) }), null);
    setStatus(b.db, vid, 'active'); assert.ok(login(b.db, { email: 'vw@errer.test', password: vw.pw, code: totp(vw.secret) }));
    const oid = userByEmail(b.db, 'owner@errer.test').id; assert.throws(() => setRole(b.db, oid, 'ops'), /last owner/); assert.throws(() => setStatus(b.db, oid, 'disabled'), /last owner/); assert.throws(() => removeUser(b.db, oid), /last owner/);
    assert.equal((await b.as(owner).post(`/users/${oid}/role`, { role: 'ops' })).status, 400, 'nor can an owner change their own role');
    setRole(b.db, userByEmail(b.db, 'ops@errer.test').id, 'owner'); setRole(b.db, oid, 'support'); assert.equal(userByEmail(b.db, 'owner@errer.test').role, 'support');
    assert.equal((await b.as(owner).get('/users')).status, 403, 'the demotion applies to the live session');
    const pid = userByEmail(b.db, 'sup@errer.test').id; removeUser(b.db, pid, { actor: 'ops@errer.test' }); assert.equal(userByEmail(b.db, 'sup@errer.test') ?? null, null); assert.equal((await b.as(S.sup).get('/')).status, 303);
    assert.equal(b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE actor = 'sup@errer.test'").n > 0, true, 'their past actions stay');
    assert.equal(listUsers(b.db).length, 3);
  } finally { await b.close(); }
});

test('lockout per person, password change, a new authenticator confirmed before it counts, ending other sessions', async () => {
  const b = await boot();
  try {
    const me = userByEmail(b.db, 'owner@errer.test');
    for (let i = 0; i < 8; i++) assert.equal(login(b.db, { email: 'owner@errer.test', password: 'wrong wrong wrong', code: '000000' }), null);
    assert.ok(userByEmail(b.db, 'owner@errer.test').locked_until > new Date().toISOString(), 'eight failures lock the person');
    assert.equal(login(b.db, { email: 'owner@errer.test', password: 'owner password 1234', code: totp(b.secret) }), null, 'even with the right password');
    assert.equal(b.db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'auth.login.locked'").n, 1);
    b.db.run('UPDATE users SET locked_until = NULL, failed_logins = 0 WHERE id = ?', me.id);
    const s = await b.signIn('owner@errer.test', 'owner password 1234', totp(b.secret)); assert.ok(s);
    assert.throws(() => changePassword(b.db, me.id, 'nope', 'a brand new password 9'), /current password is wrong/); assert.throws(() => changePassword(b.db, me.id, 'owner password 1234', 'short'), /12 characters/);
    let r = await b.as(s).post('/me/password', { current: 'owner password 1234', next: 'a brand new password 9' }); assert.equal(r.status, 303);
    assert.equal(login(b.db, { email: 'owner@errer.test', password: 'owner password 1234', code: totp(b.secret) }), null); assert.ok(login(b.db, { email: 'owner@errer.test', password: 'a brand new password 9', code: totp(b.secret) }));
    r = await b.as(s).post('/me/totp'); assert.equal(r.status, 303); const next = pendingTotp(b.db, me.id); assert.ok(next && next !== b.secret);
    assert.ok(login(b.db, { email: 'owner@errer.test', password: 'a brand new password 9', code: totp(b.secret) }), 'the old authenticator works until the new one is confirmed');
    assert.ok((await (await b.as(s).get('/me')).text()).includes(next), 'the new secret and its QR are on the page');
    r = await b.as(s).post('/me/totp/confirm', { code: '000000' }); assert.equal(r.status, 400);
    r = await b.as(s).post('/me/totp/confirm', { code: totp(next) }); assert.equal(r.status, 303);
    assert.equal(login(b.db, { email: 'owner@errer.test', password: 'a brand new password 9', code: totp(b.secret) }), null, 'the old one is gone'); assert.ok(login(b.db, { email: 'owner@errer.test', password: 'a brand new password 9', code: totp(next) }));
    assert.equal(b.db.get('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?', me.id).n, 4); r = await b.as(s).post('/me/sessions/end'); assert.equal(r.status, 303); assert.equal(b.db.get('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?', me.id).n, 1); assert.equal((await b.as(s).get('/me')).status, 200, 'this one stays');
    const h = doctor(b.db, b.cfg); const by = Object.fromEntries(h.checks.map(c => [c.key, c])); assert.ok(by.operator.ok); assert.match(by.operator.detail, /1 owner/); assert.ok(by.password_age.ok); assert.ok(by.invites.ok);
  } finally { await b.close(); }
});

test('the operator from before people existed is carried over as the first owner, once; the demo seeds one of each role', async () => {
  const db = testDb();
  db.setSetting('auth:password', hashPassword('legacy operator pw 1')); db.setSetting('auth:totp', base32Encode(Buffer.from('12345678901234567890'))); db.setSetting('auth:password_set_at', '2026-01-01T00:00:00.000Z');
  const u = carryLegacyOperator(db, { email: 'hello@errer.test' }); assert.equal(u.role, 'owner'); assert.equal(u.status, 'active'); assert.equal(u.password_set_at, '2026-01-01T00:00:00.000Z');
  assert.equal(carryLegacyOperator(db, { email: 'hello@errer.test' }), null, 'only once');
  assert.ok(login(db, { password: 'legacy operator pw 1', code: totp(base32Encode(Buffer.from('12345678901234567890'))) }), 'the old password and authenticator still work');
  db.close();
  const d = testDb(); const cfg = testCfg(); await seedDemo(d, cfg);
  const roles = listUsers(d).map(x => `${x.role}:${x.status}`).sort(); assert.deepEqual(roles, ['ops:active', 'owner:active', 'support:active', 'viewer:active', 'viewer:invited']);
  for (const x of DEMO_USERS) { assert.match(x.email, /\.example\.test$/); assert.ok(login(d, { email: x.email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), x.email); }
  d.close();
});

test('the QR encoder matches a reference encoder bit for bit at versions 1, 8 and 11, and picks a mask by the standard penalty', () => {
  for (const c of JSON.parse(readFileSync(fixture('qr.json'), 'utf8'))) {
    const g = qrMatrix(c.text, { mask: c.mask }); assert.equal((g.length - 17) / 4, c.version, `version for ${c.text.length} bytes`);
    assert.deepEqual(g.map(r => Array.from(r).join('')), c.rows, `v${c.version} mask ${c.mask}`);
  }
  const auto = qrMatrix('hello'); assert.equal(auto.length, 21); assert.ok([0, 1, 2, 3, 4, 5, 6, 7].some(m => qrMatrix('hello', { mask: m }).every((row, i) => row.every((x, j) => x === auto[i][j]))), 'the automatic choice is one of the eight');
  assert.throws(() => qrMatrix('z'.repeat(300)), /too long/);
  const svg = qrSvg('otpauth://totp/x?secret=ABC', { size: 180, label: 'scan me' }); assert.ok(svg.startsWith('<svg') && svg.includes('aria-label="scan me"') && svg.includes('width="180"') && svg.includes('<path d="M'));
});
