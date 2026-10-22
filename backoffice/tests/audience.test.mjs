import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { testDb, testCfg, catalog, sampleOrder } from './helpers.mjs';
import { upsertContact, requestConsent, confirmConsent, unsubscribe, segment, segmentCount, audienceStats, markBounced, canMarket, setContactTags, SEGMENTS } from '../src/modules/contacts.mjs';
import { renderHtml, buildMessage, sendOutbox } from '../src/modules/mail.mjs';
import { createCampaign, updateCampaign, scheduleCampaign, sendDueCampaigns, sendTest, previewHtml, campaignReport, queueCampaign, duplicateCampaign } from '../src/modules/campaigns.mjs';
import { seedAutomations, runAutomations, listAutomations, automationReport, upsertAutomation, setAutomationEnabled } from '../src/modules/automations.mjs';
import { recordOrder, transition } from '../src/modules/orders.mjs';
import { ingestEmail, openTicket } from '../src/modules/support.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp, createApiToken } from '../src/auth.mjs';
import { backup, uploadOffsite, restoreDrill, seal, unseal } from '../src/modules/backup.mjs';
import { doctor } from '../src/modules/health.mjs';
import { runJob } from '../src/jobs.mjs';

const optIn = (db, email, name, extra = {}) => { const c = upsertContact(db, { email, name, ...extra }); confirmConsent(db, requestConsent(db, c.id)); return c; };

test('segments: every one starts from consent and narrows; counts match members; unknown words are refused', () => {
  const db = testDb(); const cat = catalog();
  const a = optIn(db, 'a@example.test', 'A'); const b = optIn(db, 'b@example.test', 'B'); const c = optIn(db, 'c@example.test', 'C', { kind: 'school' }); upsertContact(db, { email: 'd@example.test', name: 'No Consent' });
  setContactTags(db, a.id, 'vip');
  recordOrder(db, sampleOrder({ email: 'a@example.test', paid_at: new Date(Date.now() - 2 * 86400000).toISOString() }), { catalog: cat });
  recordOrder(db, sampleOrder({ external_id: 'cs_old', email: 'b@example.test', paid_at: new Date(Date.now() - 200 * 86400000).toISOString(), ship: { line1: 'x', city: 'y', state: 'tx', zip: '1' } }), { catalog: cat });
  const ids = (seg) => segment(db, seg).map(x => x.email).sort();
  assert.deepEqual(ids('all'), ['a@example.test', 'b@example.test', 'c@example.test']);
  assert.deepEqual(ids('customers'), ['a@example.test', 'b@example.test']); assert.deepEqual(ids('never-bought'), ['c@example.test']);
  assert.deepEqual(ids('recent:30'), ['a@example.test']); assert.deepEqual(ids('idle:180'), ['b@example.test']); assert.deepEqual(ids('bought:wap-kit'), ['a@example.test', 'b@example.test']);
  assert.deepEqual(ids('tag:VIP'), ['a@example.test']); assert.deepEqual(ids('kind:school'), ['c@example.test']); assert.deepEqual(ids('state:tx'), ['b@example.test']);
  for (const [k] of SEGMENTS.filter(([k]) => !k.includes('<'))) assert.equal(segmentCount(db, k), segment(db, k).length, k);
  assert.throws(() => segment(db, 'everyone'), /unknown segment/);
  unsubscribe(db, a.unsubscribe_token); assert.deepEqual(ids('customers'), ['b@example.test']);
  markBounced(db, 'b@example.test', { reason: 'test' }); assert.deepEqual(ids('customers'), []); assert.ok(!canMarket(db.get('SELECT * FROM contacts WHERE email = \'b@example.test\'')));
  const st = audienceStats(db); assert.equal(st.subscribers, 1); assert.equal(st.bounced, 1); assert.equal(st.left, 1); assert.equal(st.growth.length, 12); assert.equal(st.growth.at(-1).joined, 3);
});

test('the designed mail: plain text in, escaped branded HTML out, as a multipart message with the text first', () => {
  const html = renderHtml('Hi <b>Ada</b>,\n\n# Two things\n\n- one & two\n- see https://securacv.com/faq now\n\n**Bold** words.\n\n— Errer Labs', { company: 'Errer Labs', siteOrigin: 'https://securacv.com', unsubscribe: 'https://x.test/u/t', preheader: 'A "quoted" preheader' });
  assert.ok(html.includes('Hi &lt;b&gt;Ada&lt;/b&gt;,'), 'markup in the text is escaped, not rendered'); assert.ok(html.includes('<h2') && html.includes('Two things')); assert.ok(html.includes('<li') && html.includes('one &amp; two'));
  assert.ok(html.includes('<a href="https://securacv.com/faq"')); assert.ok(html.includes('<strong>Bold</strong>')); assert.ok(html.includes('unsubscribe in one click')); assert.ok(html.includes('A &quot;quoted&quot; preheader')); assert.ok(!/<script|<img/.test(html), 'no scripts, no images, no pixels');
  const m = buildMessage({ from: 'Errer Labs <hello@errer.test>', to: 'a@example.test', subject: 'Hi', text: 'plain', html: '<p>rich</p>', headers: { 'List-Unsubscribe': '<https://x>' } });
  assert.match(m.raw, /Content-Type: multipart\/alternative; boundary="=_bo_/); assert.ok(m.raw.indexOf('text/plain') < m.raw.indexOf('text/html'), 'the plain part comes first'); assert.ok(m.raw.includes('List-Unsubscribe: <https://x>'));
  const plain = buildMessage({ from: 'a@b.test', to: 'c@d.test', subject: 's', text: 't' }); assert.match(plain.raw, /Content-Type: text\/plain/); assert.ok(!plain.raw.includes('multipart'));
});

test('campaigns: counts before sending, a test send, a schedule the outbox honors, an honest report; a sent campaign is history', async () => {
  const db = testDb(); const cfg = testCfg({ operatorEmail: 'owner@errer.test' }); const cat = catalog();
  for (const [em, nm] of [['a@example.test', 'Ada Lovelace'], ['b@example.test', 'Bob'], ['c@example.test', 'Cy']]) optIn(db, em, nm);
  const cp = createCampaign(db, { slug: 'nov', subject: 'Hello {{first_name}}', preheader: 'pre', segment: 'all', body_text: 'Hi {{first_name}},\n\n# News\n\nFrom {{company}} at {{site}}.' });
  assert.throws(() => createCampaign(db, { slug: 'Bad Slug', subject: 's', body_text: 'b' }), /slug/); assert.throws(() => createCampaign(db, { slug: 'xx', subject: 's', body_text: 'b', segment: 'nope' }), /unknown segment/);
  assert.equal(segmentCount(db, cp.segment), 3);
  const t = sendTest(db, 'nov', 'owner@errer.test', cfg); assert.equal(t.to_addr, 'owner@errer.test'); assert.match(t.subject, /^\[TEST\] Hello Ada$/); assert.ok(t.body_text.includes('Hi Ada,') && t.body_text.includes('Unsubscribe in one click')); assert.ok(t.body_text.includes('/u/test-send-inert') && !t.body_text.includes(db.get('SELECT unsubscribe_token FROM contacts WHERE email = \'a@example.test\'').unsubscribe_token), 'the test mail never carries a real unsubscribe token');
  assert.ok(previewHtml(db, 'nov', cfg).includes('Hi Sample,') && previewHtml(db, 'nov', cfg).includes('pre'));
  updateCampaign(db, 'nov', { segment: 'customers' }); assert.equal(segmentCount(db, 'customers'), 0);
  updateCampaign(db, 'nov', { segment: 'all' });
  assert.throws(() => scheduleCampaign(db, 'nov', '2000-01-01T00:00:00Z'), /passed/);
  scheduleCampaign(db, 'nov', new Date(Date.now() + 3600000).toISOString()); assert.equal(db.get('SELECT status FROM campaigns WHERE slug = \'nov\'').status, 'scheduled');
  assert.deepEqual(sendDueCampaigns(db, cfg), [], 'not yet');
  db.run('UPDATE campaigns SET scheduled_at = ? WHERE slug = \'nov\'', new Date(Date.now() - 1000).toISOString());
  assert.deepEqual(sendDueCampaigns(db, cfg), [{ slug: 'nov', recipients: 3 }]);
  const rows = db.all('SELECT * FROM mail_outbox WHERE ref = \'campaign:nov\''); assert.equal(rows.length, 3); assert.ok(rows.every(r => r.contact_id)); assert.ok(rows[0].subject.startsWith('Hello ')); assert.ok(JSON.parse(rows[0].headers)['List-Unsubscribe-Post']);
  // the HTML part is built at send time and the preheader header never leaves the house
  const sent = []; await sendOutbox(db, { ...cfg, mailFrom: 'Errer Labs <hello@errer.test>' }, { transport: async (m) => { sent.push(m.raw); return true; } });
  assert.ok(sent.some(r => r.includes('multipart/alternative') && r.replace(/=\r\n/g, '').includes('unsubscribe in one click') && !r.includes('X-Preheader')), 'the HTML part is built at send time, the preheader header never leaves');
  assert.throws(() => updateCampaign(db, 'nov', { subject: 'x' }), /history/); assert.throws(() => queueCampaign(db, 'nov', cfg), /already sent/);
  // the report: one unsubscribes, one orders, one replies — in the week after
  const c = db.get('SELECT * FROM contacts WHERE email = \'b@example.test\''); unsubscribe(db, c.unsubscribe_token);
  recordOrder(db, sampleOrder({ external_id: 'cs_after', email: 'a@example.test', paid_at: new Date().toISOString() }), { catalog: cat });
  openTicket(db, { email: 'c@example.test', subject: 'Re: News', body: 'Love it', cfg, autoAck: false });
  const rep = campaignReport(db, 'nov'); assert.equal(rep.sent, 3); assert.equal(rep.unsubscribed, 1); assert.equal(rep.orders, 1); assert.equal(rep.revenue_cents, 7700); assert.equal(rep.replies, 1);
  const d = duplicateCampaign(db, 'nov'); assert.equal(d.slug, 'nov-2'); assert.equal(d.status, 'draft'); assert.equal(d.body_text, cp.body_text);
});

test('automations: enrolled once per thing, sent after the delay, skipped without consent, reported by replies and orders', async () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog(); const now = Date.now();
  seedAutomations(db); assert.equal(listAutomations(db).length, 3);
  const a = optIn(db, 'a@example.test', 'Ada Lovelace'); upsertContact(db, { email: 'n@example.test', name: 'No Consent' });
  const o = recordOrder(db, sampleOrder({ email: 'a@example.test' }), { catalog: cat }).order; transition(db, o.id, 'packed'); transition(db, o.id, 'shipped', { cfg }); transition(db, o.id, 'delivered');
  const o2 = recordOrder(db, sampleOrder({ external_id: 'cs_n', email: 'n@example.test' }), { catalog: cat }).order; transition(db, o2.id, 'packed'); transition(db, o2.id, 'shipped', { cfg }); transition(db, o2.id, 'delivered');
  let r = runAutomations(db, cfg, { now }); assert.equal(r.enrolled, 3, 'welcome for the opt-in, settled-in for both deliveries'); assert.equal(r.sent, 0, 'the delays have not passed');
  r = runAutomations(db, cfg, { now }); assert.equal(r.enrolled, 0, 'enrollment is idempotent');
  r = runAutomations(db, cfg, { now: now + 2 * 3600000 }); assert.equal(r.sent, 1, 'the welcome, an hour later');
  const welcome = db.get('SELECT * FROM mail_outbox WHERE kind = \'automation\''); assert.equal(welcome.to_addr, 'a@example.test'); assert.ok(welcome.body_text.includes('Hi Ada,') && welcome.body_text.includes('Unsubscribe in one click'));
  r = runAutomations(db, cfg, { now: now + 80 * 3600000 }); assert.equal(r.sent, 1, 'settled-in for the consented customer'); assert.equal(r.skipped, 1, 'and skipped for the one without consent');
  assert.ok(db.get('SELECT body_text FROM mail_outbox WHERE ref = ?', `automation:settled-in:order:${o.number}`).body_text.includes(o.number));
  assert.equal(db.get('SELECT skipped FROM automation_runs WHERE ref = ?', `order:${o2.number}`).skipped, 'no consent');
  // the idle nudge fires once, and never again for the same person
  db.run('UPDATE orders SET paid_at = ? WHERE contact_id = ?', new Date(now - 200 * 86400000).toISOString(), a.id);
  r = runAutomations(db, cfg, { now }); assert.equal(r.sent, 1); r = runAutomations(db, cfg, { now: now + 86400000 }); assert.equal(r.sent, 0);
  // report, pause, edit
  const t = openTicket(db, { email: 'a@example.test', subject: 'Re: Settled in?', body: 'All good', cfg, autoAck: false }); db.run('UPDATE tickets SET created_at = ? WHERE id = ?', new Date(now + 81 * 3600000).toISOString(), t.id);   // the reply lands an hour after the (simulated) send
  const rep = automationReport(db, 'settled-in'); assert.equal(rep.sent, 1); assert.equal(rep.replies, 1);
  setAutomationEnabled(db, 'welcome', false); assert.equal(listAutomations(db).find(x => x.slug === 'welcome').enabled, 0);
  assert.throws(() => upsertAutomation(db, { slug: 'xx', trigger: 'full_moon', subject: 's', body_text: 'b' }), /trigger is one of/);
  upsertAutomation(db, { slug: 'thanks', name: 'Thanks', trigger: 'waitlist_joined', delay_hours: 0, subject: 'Thanks {{first_name}}', body_text: 'b' }); assert.equal(listAutomations(db).length, 4);
});

test('a bounce names the address and suppresses it; the newsletter form double-opts-in; a sealed off-site copy round-trips through the drill', async () => {
  const db = testDb(); const cfg = testCfg();
  optIn(db, 'gone@example.test', 'Gone');
  const r = ingestEmail(db, { from: 'Mail Delivery Subsystem <mailer-daemon@relay.example.test>', subject: 'Undeliverable: October from the bench', text: 'Your message to gone@example.test could not be delivered.\n550 5.1.1 User unknown', messageId: '<dsn1@relay.example.test>' }, { cfg });
  assert.equal(r.ignored, 'bounce'); assert.deepEqual(r.bounced, ['gone@example.test']); assert.ok(db.get('SELECT bounced_at FROM contacts WHERE email = \'gone@example.test\'').bounced_at); assert.equal(db.get('SELECT COUNT(*) AS n FROM tickets').n, 0, 'a bounce is not a ticket');
  setupOperator(db, 'audience-test-pw-1'); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const f = await fetch(base + '/forms/newsletter', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ email: 'new@example.test', name: 'New' }) });
    assert.equal((await f.json()).newsletter, 'confirmation mail sent'); assert.ok(db.get('SELECT consent_token FROM contacts WHERE email = \'new@example.test\'').consent_token); assert.equal(db.get('SELECT consent_marketing FROM contacts WHERE email = \'new@example.test\'').consent_marketing, 0);
    // security: the door
    const login = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ password: 'audience-test-pw-1', code: totp(db.setting('auth:totp')) }), redirect: 'manual' });
    const cookie = login.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const home = await fetch(base + '/', { headers: { cookie } }); assert.equal(home.headers.get('cache-control'), 'no-store'); assert.match(home.headers.get('permissions-policy'), /camera=\(\)/); assert.equal(home.headers.get('cross-origin-opener-policy'), 'same-origin'); assert.ok(!home.headers.get('strict-transport-security'), 'no HSTS over plain http');
    const xs = await fetch(base + '/orders/bulk', { method: 'POST', headers: { cookie, 'sec-fetch-site': 'cross-site' }, body: new URLSearchParams({ _csrf: csrf, action: 'packed' }), redirect: 'manual' }); assert.equal(xs.status, 403, 'a cross-site POST is refused before the body matters');
    const same = await fetch(base + '/orders/bulk', { method: 'POST', headers: { cookie, 'sec-fetch-site': 'same-origin', accept: 'application/json' }, body: new URLSearchParams({ _csrf: csrf, action: 'packed' }), redirect: 'manual' }); assert.notEqual(same.status, 403);
    const ro = createApiToken(db, 'dash', { scope: 'ro' }); const rw = createApiToken(db, 'cli');
    assert.equal((await fetch(base + '/api/orders', { headers: { authorization: `Bearer ${ro}` } })).status, 200);
    assert.equal((await fetch(base + '/api/inventory/move', { method: 'POST', headers: { authorization: `Bearer ${ro}`, 'content-type': 'application/json' }, body: '{}' })).status, 403, 'a read-only token cannot write');
    assert.notEqual((await fetch(base + '/api/inventory/move', { method: 'POST', headers: { authorization: `Bearer ${rw}`, 'content-type': 'application/json' }, body: '{}' })).status, 403);
    for (let i = 0; i < 8; i++) await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ password: 'wrong', code: '000000' }), redirect: 'manual' });
    const locked = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ password: 'audience-test-pw-1', code: totp(db.setting('auth:totp')) }), redirect: 'manual' });
    assert.equal(locked.status, 429, 'eight failures lock the address, even for the right password');
    assert.ok(db.get('SELECT detail FROM audit_log WHERE action = \'auth.login.locked\'')); assert.ok(JSON.parse(db.get('SELECT detail FROM audit_log WHERE action = \'auth.login.failed\' ORDER BY id DESC LIMIT 1').detail).ip);
    const by = Object.fromEntries(doctor(db, cfg).checks.map(c => [c.key, c])); assert.equal(by.logins.ok, true); assert.match(by.logins.detail, /^8 /); assert.equal(by.password_age.ok, true); assert.equal(by.offsite_sealed.ok, null);
    // the mail pages
    const pages = ['/campaigns', '/campaigns/automations', '/campaigns/audience', '/campaigns/outbox']; for (const p of pages) assert.equal((await fetch(base + p, { headers: { cookie } })).status, 200, p);
    createCampaign(db, { slug: 'pv', subject: 'Preview me', body_text: 'Hi {{first_name}}' });
    const pv = await fetch(base + '/campaigns/pv/preview.html', { headers: { cookie } }); assert.equal(pv.status, 200); assert.equal(pv.headers.get('x-frame-options'), 'SAMEORIGIN'); assert.match(pv.headers.get('content-security-policy'), /frame-ancestors 'self'/); assert.ok((await pv.text()).includes('Preview me') === false && (await fetch(base + '/campaigns/pv', { headers: { cookie } })).status === 200);
  } finally { await app.close(); }
  // sealed copies
  const dir = mkdtempSync(join(tmpdir(), 'bo-seal-')); const store = new Map();
  const fetchImpl = async (url, { method, body }) => { const key = new URL(url).pathname; if (method === 'PUT') { store.set(key, Buffer.from(body)); return { ok: true, status: 200, headers: new Headers(), text: async () => '' }; } const b = store.get(key); if (method === 'HEAD') return { ok: !!b, status: b ? 200 : 404, headers: new Headers(b ? { 'content-length': String(b.length) } : {}) }; return { ok: !!b, status: b ? 200 : 404, text: async () => b.toString(), arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }; };
  const scfg = testCfg({ dataDir: dir, backupS3Url: 'https://files.example.test/b', backupS3Key: 'k', backupS3Secret: 's', backupEncryptionKey: 'correct horse battery staple' });
  const b = backup(db, join(dir, 'backups')); const up = await uploadOffsite(scfg, b.path, { fetchImpl });
  assert.ok(up.encrypted && up.name.endsWith('.db.enc')); const blob = store.get('/b/' + up.name); assert.ok(blob.subarray(0, 6).toString() === 'BOENC1' && !blob.includes('SQLite format 3'), 'the bucket never sees a readable database');
  const drill = await restoreDrill(scfg, { fetchImpl }); assert.ok(drill.ok && drill.encrypted, (drill.errors || []).join('; '));
  await assert.rejects(restoreDrill({ ...scfg, backupEncryptionKey: 'wrong' }, { fetchImpl }), /Unsupported state|unable to authenticate/);
  await assert.rejects(restoreDrill({ ...scfg, backupEncryptionKey: '' }, { fetchImpl }), /BACKUP_ENCRYPTION_KEY/);
  assert.equal(unseal(seal(Buffer.from('x'), 'p'), 'p').toString(), 'x'); assert.equal(Object.fromEntries(doctor(db, scfg).checks.map(c => [c.key, c])).offsite_sealed.ok, true);
  const unsealedCfg = { ...scfg, backupEncryptionKey: '' }; assert.equal(Object.fromEntries(doctor(db, unsealedCfg).checks.map(c => [c.key, c])).offsite_sealed.ok, false);
  db.close(); rmSync(dir, { recursive: true, force: true });
});

test('the demo carries a sent campaign with a report, a scheduled one, and automations with history; the outbox job sends a due campaign', async () => {
  const db = testDb(); const cfg = testCfg(); await seedDemo(db, cfg);
  const rep = campaignReport(db, 'october-from-the-bench'); assert.ok(rep.sent >= 5, 'it went to the opted-in'); assert.ok(rep.orders >= 0);
  assert.equal(db.get('SELECT status FROM campaigns WHERE slug = \'november-batch-note\'').status, 'scheduled');
  assert.ok(listAutomations(db).some(a => a.sent > 0), 'automations have sent');
  const app = createApp({ db, cfg });
  db.run('UPDATE campaigns SET scheduled_at = ? WHERE slug = \'november-batch-note\'', new Date(Date.now() - 1000).toISOString());
  const r = await runJob(app, 'outbox'); assert.ok(r.ok); assert.equal(r.result.campaigns[0].slug, 'november-batch-note'); assert.ok(r.result.campaigns[0].recipients > 0);
  assert.equal(db.get('SELECT status FROM campaigns WHERE slug = \'november-batch-note\'').status, 'sent');
});
