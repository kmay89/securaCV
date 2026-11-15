// The mail domain's public DNS (maildns.mjs) — SPF, DMARC, MX as receivers read them — with a fake
// resolver, the job that remembers the answer, the doctor row that reads it; and the backoffice's
// own security.txt (RFC 9116).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mailDomainOf, checkMailDomain, rememberMailDomainCheck, lastMailDomainCheck } from '../src/modules/maildns.mjs';
import { doctor } from '../src/modules/health.mjs';
import { runJob } from '../src/jobs.mjs';
import { createApp } from '../src/server.mjs';
import { setupOperator } from '../src/auth.mjs';
import { testDb, testCfg } from './helpers.mjs';

const notFound = (code = 'ENOTFOUND') => Object.assign(new Error(code), { code });
const fakeDns = (records) => ({
  resolveTxt: async (name) => { if (records.txt?.[name]) return records.txt[name]; throw notFound(records.code || 'ENOTFOUND'); },
  resolveMx: async (name) => { if (records.mx?.[name]) return records.mx[name]; throw notFound('ENODATA'); },
});

test('the domain is MAIL_FROM\'s, else SUPPORT_ADDR\'s, else nothing', () => {
  assert.equal(mailDomainOf({ mailFrom: 'Errer Labs <hello@Errer.test>', supportAddr: 'x@other.test' }), 'errer.test');
  assert.equal(mailDomainOf({ mailFrom: '', supportAddr: 'support@mail.errer.test' }), 'mail.errer.test');
  assert.equal(mailDomainOf({ mailFrom: 'nonsense', supportAddr: '' }), null);
  assert.equal(mailDomainOf({}), null);
});

test('SPF, DMARC (with its policy) and MX are read; missing records are named with what to do; a lookup error is not a verdict', async () => {
  const cfg = testCfg();   // hello@errer.test
  let r = await checkMailDomain(cfg, { resolver: fakeDns({ txt: { 'errer.test': [['v=spf1 include:_spf.relay.test ', '-all'], ['google-site-verification=abc']], '_dmarc.errer.test': [['v=DMARC1; p=quarantine; rua=mailto:dmarc@errer.test']] }, mx: { 'errer.test': [{ priority: 20, exchange: 'mx2.relay.test' }, { priority: 10, exchange: 'mx1.relay.test' }] } }) });
  assert.equal(r.domain, 'errer.test'); assert.equal(r.ok, true); assert.deepEqual(r.problems, []);
  assert.equal(r.spf.record, 'v=spf1 include:_spf.relay.test -all', 'TXT fragments are joined'); assert.equal(r.dmarc.policy, 'quarantine'); assert.deepEqual(r.mx.hosts, ['mx1.relay.test', 'mx2.relay.test']);
  r = await checkMailDomain(cfg, { resolver: fakeDns({ txt: { 'errer.test': [['v=spf1 +all']], '_dmarc.errer.test': [['v=DMARC1; p=none']] } }) });
  assert.equal(r.ok, false); assert.equal(r.mx.found, false); assert.match(r.problems[0], /\+all/); assert.match(r.problems[1], /p=none/); assert.match(r.problems[2], /no MX/);
  r = await checkMailDomain(cfg, { resolver: fakeDns({}) });
  assert.equal(r.ok, false); assert.equal(r.spf.found, false); assert.equal(r.dmarc.found, false); assert.equal(r.problems.length, 3); assert.match(r.problems[0], /no SPF/); assert.match(r.problems[1], /no DMARC/);
  r = await checkMailDomain(cfg, { resolver: { resolveTxt: async () => { throw Object.assign(new Error('queryTxt ETIMEOUT'), { code: 'ETIMEOUT' }); }, resolveMx: async () => [] } });
  assert.equal(r.ok, null); assert.match(r.error, /^ETIMEOUT/);
  r = await checkMailDomain({ mailFrom: '', supportAddr: '' }); assert.equal(r.domain, null); assert.equal(r.ok, null);
});

test('the doctor reads the job\'s last answer: not looked up yet, then the verdict with the fix; the demo and an unset domain are neutral', async () => {
  const db = testDb(); const cfg = testCfg(); setupOperator(db, 'operator password 1');
  const row = () => doctor(db, cfg).checks.find(c => c.key === 'mail_dns');
  assert.equal(row().ok, null); assert.match(row().detail, /errer\.test · not looked up yet/);
  rememberMailDomainCheck(db, await checkMailDomain(cfg, { resolver: fakeDns({}) }));
  assert.equal(row().ok, false); assert.match(row().detail, /^errer\.test · SPF MISSING · DMARC MISSING · MX MISSING · checked /); assert.match(row().fix, /v=spf1/); assert.match(row().fix, /_dmarc/);
  assert.ok(doctor(db, cfg).failing.includes('mail_dns'));
  rememberMailDomainCheck(db, await checkMailDomain(cfg, { resolver: fakeDns({ txt: { 'errer.test': [['v=spf1 -all']], '_dmarc.errer.test': [['v=DMARC1; p=reject']] }, mx: { 'errer.test': [{ priority: 10, exchange: 'mx.relay.test' }] } }) }));
  assert.equal(row().ok, true); assert.match(row().detail, /SPF ok · DMARC p=reject · MX 1 host/);
  // the answer belongs to the domain it was made for: a changed MAIL_FROM starts over
  assert.equal(doctor(db, { ...cfg, mailFrom: 'Errer Labs <hello@elsewhere.test>' }).checks.find(c => c.key === 'mail_dns').ok, null);
  assert.equal(doctor(db, { ...cfg, demo: true }).checks.find(c => c.key === 'mail_dns').detail, 'demo mode');
  assert.match(doctor(db, { ...cfg, mailFrom: '', supportAddr: '' }).checks.find(c => c.key === 'mail_dns').detail, /no MAIL_FROM/);
  // the job: skipped in the demo and without a domain; otherwise it remembers what it found
  assert.deepEqual((await runJob(createApp({ db, cfg: { ...cfg, demo: true } }), 'maildns')).result, { skipped: 'demo mode' });
  assert.deepEqual((await runJob(createApp({ db, cfg: { ...cfg, mailFrom: '', supportAddr: '' } }), 'maildns')).result, { skipped: 'no MAIL_FROM or SUPPORT_ADDR' });
  assert.equal(lastMailDomainCheck(db).ok, true, 'a skipped run leaves the last answer alone');
});

test('security.txt: both paths, the support address, an Expires inside a year, a 404 without an address; it is a customer-facing path for the admin allowlist', async () => {
  const db = testDb(); const cfg = testCfg({ secret: 'sekret', siteOrigin: 'https://securacv.example' }); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  try {
    for (const path of ['/.well-known/security.txt', '/security.txt']) {
      const r = await fetch(base + path); assert.equal(r.status, 200); assert.match(r.headers.get('content-type'), /^text\/plain/);
      const body = await r.text(); assert.match(body, /^Contact: mailto:hello@errer\.test$/m); assert.match(body, /^Preferred-Languages: en$/m); assert.match(body, /^Policy: https:\/\/securacv\.example\/security\.txt$/m);
      assert.equal(body.match(/^Canonical: (.*)$/m)[1], `${base}/.well-known/security.txt`);
      const exp = Date.parse(body.match(/^Expires: (.*)$/m)[1]); assert.ok(exp > Date.now() + 300 * 86400000 && exp < Date.now() + 366 * 86400000, 'between ten and twelve months out');
    }
    assert.equal((await fetch(base + '/security.txt', { headers: { 'x-forwarded-for': '203.0.113.9' } })).status, 200, 'not a tripwire, not behind the door');
  } finally { await app.close?.(); }
  const quiet = createApp({ db: testDb(), cfg: testCfg({ supportAddr: '', mailFrom: '' }) }); const a2 = await quiet.listen();
  try { assert.equal((await fetch(`http://127.0.0.1:${a2.port}/security.txt`)).status, 404); } finally { await quiet.close?.(); }
});
