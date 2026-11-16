import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { buildMessage, qpEncode, smtpSend, enqueue, sendOutbox, fill, noCRLF } from '../src/modules/mail.mjs';
import { upsertContact } from '../src/modules/contacts.mjs';
import { createCampaign, sendTest } from '../src/modules/campaigns.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('quoted-printable keeps ASCII readable, encodes UTF-8 and wraps at 76', () => {
  assert.equal(qpEncode('plain text'), 'plain text');
  assert.equal(qpEncode('café = ok'), 'caf=C3=A9 =3D ok');
  assert.ok(qpEncode('x'.repeat(200)).split('\r\n').every(l => l.length <= 76));
  const m = buildMessage({ from: 'Errer Labs <hello@errer.test>', to: 'a@b.test', subject: 'Résumé', text: 'hi\n', headers: { 'List-Unsubscribe': '<http://x>' } });
  assert.match(m.raw, /^Date: /); assert.ok(m.raw.includes('Subject: =?UTF-8?B?')); assert.ok(m.raw.includes('List-Unsubscribe: <http://x>')); assert.ok(m.id.endsWith('@errer.test>'));
  assert.equal(fill('Hi {{name}}, {{missing}}.', { name: 'Ada' }), 'Hi Ada, .');
});

// A fake SMTP server that records what it is told, no TLS (starttls=no).
function fakeSmtp() {
  const seen = { cmds: [], data: '' };
  const server = createServer((sock) => {
    let inData = false;
    sock.write('220 fake ESMTP\r\n');
    sock.on('data', (d) => {
      for (const line of d.toString().split('\r\n')) {
        if (!line && !inData) continue;
        if (inData) { if (line === '.') { inData = false; sock.write('250 queued\r\n'); } else seen.data += line + '\n'; continue; }
        seen.cmds.push(line);
        const up = line.toUpperCase();
        if (up.startsWith('EHLO')) sock.write('250-fake\r\n250-AUTH PLAIN LOGIN\r\n250 OK\r\n');
        else if (up.startsWith('AUTH PLAIN')) sock.write(line.endsWith(Buffer.from('\0u\0p').toString('base64')) ? '235 ok\r\n' : '535 no\r\n');
        else if (up.startsWith('MAIL FROM') || up.startsWith('RCPT TO')) sock.write('250 ok\r\n');
        else if (up === 'DATA') { inData = true; sock.write('354 go\r\n'); }
        else if (up === 'QUIT') { sock.write('221 bye\r\n'); sock.end(); }
        else sock.write('500 what\r\n');
      }
    });
  });
  return new Promise(r => server.listen(0, '127.0.0.1', () => r({ server, port: server.address().port, seen })));
}

test('the SMTP client authenticates, dot-stuffs and delivers; the outbox suppresses campaign mail without consent', async () => {
  const { server, port, seen } = await fakeSmtp();
  const url = `smtp://u:p@127.0.0.1:${port}?starttls=no`;
  await smtpSend(url, { from: 'Errer Labs <hello@errer.test>', to: 'a@b.test', raw: 'Subject: t\r\n\r\n.leading dot\r\nok\r\n' });
  assert.ok(seen.cmds.some(c => c === 'MAIL FROM:<hello@errer.test>')); assert.ok(seen.cmds.some(c => c === 'RCPT TO:<a@b.test>'));
  assert.ok(seen.data.includes('..leading dot'), 'dot-stuffed');
  await assert.rejects(smtpSend(`smtp://u:wrong@127.0.0.1:${port}?starttls=no`, { from: 'x@y', to: 'a@b', raw: 'x' }), /AUTH PLAIN/);

  const db = testDb(); const cfg = testCfg({ smtpUrl: url });
  upsertContact(db, { email: 'nope@b.test' });
  enqueue(db, { to: 'yes@b.test', subject: 'order', text: 'transactional' });
  enqueue(db, { to: 'nope@b.test', subject: 'news', text: 'campaign', kind: 'campaign' });
  const r = await sendOutbox(db, cfg);
  assert.deepEqual(r, { sent: 1, failed: 0, suppressed: 1, skipped: 0 });
  assert.equal(db.get('SELECT status FROM mail_outbox WHERE to_addr = ?', 'nope@b.test').status, 'suppressed');
  assert.ok(db.get('SELECT message_id FROM mail_outbox WHERE to_addr = ?', 'yes@b.test').message_id);
  // a dead relay fails the attempt and keeps the mail queued for a retry
  server.close();
  enqueue(db, { to: 'later@b.test', subject: 'x', text: 'y' });
  const r2 = await sendOutbox(db, cfg);
  assert.equal(r2.failed, 1);
  const later = db.get('SELECT status, attempts, next_attempt_at FROM mail_outbox WHERE to_addr = ?', 'later@b.test');
  assert.equal(later.status, 'queued'); assert.equal(later.attempts, 1);
  assert.ok(later.next_attempt_at > new Date().toISOString(), 'the retry waits: a dead relay is not hammered every minute');
  assert.equal((await sendOutbox(db, cfg)).failed, 0, 'and the next run leaves it alone until then');
  // no SMTP configured → nothing is sent, nothing is lost
  enqueue(db, { to: 'nowhere@b.test', subject: 'x', text: 'y' });
  assert.equal((await sendOutbox(db, testCfg({ smtpUrl: '' }))).skipped, 1);
  assert.equal(db.get('SELECT status FROM mail_outbox WHERE to_addr = ?', 'nowhere@b.test').status, 'queued');
});

test('smtp:// fails closed when the relay does not offer STARTTLS', async () => {
  const { server, port } = await fakeSmtp();
  await assert.rejects(smtpSend(`smtp://u:p@127.0.0.1:${port}`, { from: 'x@y.test', to: 'a@b.test', raw: 'x' }), /does not offer STARTTLS/);
  server.close();
});

// A header is one line. A recipient or a header value with a line break in it would add recipients and headers of
// its own (a Bcc:, a second RCPT TO); the mail builder is the last line against that, whatever a caller validated.
test('a line break in a recipient, a sender or a header is refused where mail is queued, where it is built and on the wire; a subject is folded to one line', async () => {
  const evil = 'victim@x.test\r\nBcc: attacker@evil.test'; const ok = { from: 'Errer Labs <hello@errer.test>', to: 'a@b.test', subject: 'order', text: 'hi\n' };
  assert.throws(() => noCRLF('a\nb', 'the recipient'), (e) => e.code === 'MAIL_HEADER' && /a line break in the recipient is refused/.test(e.message)); assert.equal(noCRLF('a@b.test', 'the recipient'), 'a@b.test');
  // buildMessage: every place a caller's text becomes a header line
  for (const [what, over] of [['the To address', { to: evil }], ['the To address (a bare LF)', { to: 'victim@x.test\nBcc: attacker@evil.test' }], ['the From address', { from: 'Errer Labs <hello@errer.test>\r\nBcc: attacker@evil.test' }],
    ['a header value', { headers: { 'List-Unsubscribe': '<http://x>\r\nBcc: attacker@evil.test' } }], ['a header name', { headers: { 'X-Tag\r\nBcc': 'attacker@evil.test' } }], ['the Message-ID', { id: '<a@b>\r\nBcc: attacker@evil.test' }]])
    assert.throws(() => buildMessage({ ...ok, ...over }), /mail: a line break in .* is refused/, what);
  // a subject is outsider text (a ticket's subject): folded to one line, never refused, never a header of its own
  const m = buildMessage({ ...ok, subject: 'Re: hello\r\nBcc: attacker@evil.test' }); const head = m.raw.slice(0, m.raw.indexOf('\r\n\r\n')).split('\r\n');
  assert.ok(head.includes('Subject: Re: hello Bcc: attacker@evil.test'), 'the subject is one line'); assert.ok(!head.some(l => /^Bcc:/i.test(l)), 'and no line of the head is a Bcc');
  assert.ok(buildMessage(ok).raw.includes('To: a@b.test\r\nSubject: order\r\n'), 'a clean message is built as before');
  // enqueue: refused before a row exists
  const db = testDb();
  assert.throws(() => enqueue(db, { to: evil, subject: 'x', text: 'y' }), /a line break in the recipient is refused/);
  assert.throws(() => enqueue(db, { to: 'a@b.test', subject: 'x', text: 'y', headers: { 'In-Reply-To': '<m@x>\nBcc: attacker@evil.test' } }), /a line break in the In-Reply-To header is refused/);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox').n, 0, 'nothing was queued');
  assert.equal(enqueue(db, { to: 'a@b.test', subject: 'Re: two\nlines', text: 'y' }).subject, 'Re: two lines', 'a ticket subject with a line break still gets its reply');
  db.run('DELETE FROM mail_outbox');
  // the campaign test send: the one recipient a person types straight into a send
  createCampaign(db, { slug: 'nov', subject: 'Hello {{first_name}}', body_text: 'Hi {{first_name}},\n\nNews.' }); const cfg0 = testCfg({ secret: 's' });
  for (const to of [evil, 'victim@x.test attacker@evil.test', 'not-an-address', 'a@b.test, c@d.test']) assert.throws(() => sendTest(db, 'nov', to, cfg0), /a valid email is required/, to);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox').n, 0); assert.equal(db.get("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'campaign.test'").n, 0);
  assert.equal(sendTest(db, 'nov', ' owner@errer.test ', cfg0).to_addr, 'owner@errer.test', 'a real address still gets its test');
  db.run('DELETE FROM mail_outbox');
  // on the wire: the relay never hears a second recipient or a Bcc, even from a row that was queued before the check existed
  const { server, port, seen } = await fakeSmtp(); const url = `smtp://u:p@127.0.0.1:${port}?starttls=no`;
  try {
    await assert.rejects(smtpSend(url, { from: 'hello@errer.test', to: 'victim@x.test>\r\nRCPT TO:<attacker@evil.test', raw: 'Subject: t\r\n\r\nx\r\n' }), /an address with whitespace, a control character or a bracket is refused/);
    await assert.rejects(smtpSend(url, { from: 'hello@errer.test\r\nRCPT TO:<attacker@evil.test>', to: 'a@b.test', raw: 'x' }), /is refused/);
    await assert.rejects(smtpSend(url, { from: 'hello@errer.test', to: ['a@b.test', 'victim@x.test attacker@evil.test'], raw: 'x' }), /is refused/);
    assert.deepEqual(seen.cmds, [], 'refused before a connection is opened');
    db.run("INSERT INTO mail_outbox (to_addr, subject, body_text, headers, kind, created_at) VALUES (?, 'old row', 'x', '{}', 'transactional', ?)", evil, new Date().toISOString());
    db.run("INSERT INTO mail_outbox (to_addr, subject, body_text, headers, kind, created_at) VALUES ('a@b.test', 'old header', 'x', ?, 'transactional', ?)", JSON.stringify({ 'X-Note': 'a\r\nBcc: attacker@evil.test' }), new Date().toISOString());
    enqueue(db, { to: 'good@b.test', subject: 'after the bad ones', text: 'still sent' });
    const r = await sendOutbox(db, testCfg({ smtpUrl: url }));
    assert.deepEqual(r, { sent: 1, failed: 2, suppressed: 0, skipped: 0 }, 'the bad rows fail, the queue behind them is sent');
    const bad = db.all("SELECT status, last_error FROM mail_outbox WHERE subject LIKE 'old %' ORDER BY id"); assert.deepEqual(bad.map(b => b.status), ['failed', 'failed'], 'failed at once: a retry cannot pass'); assert.match(bad[0].last_error, /a line break in the To address is refused/); assert.match(bad[1].last_error, /a line break in the X-Note header is refused/);
    assert.deepEqual(seen.cmds.filter(c => /^RCPT TO/i.test(c)), ['RCPT TO:<good@b.test>'], 'one recipient reached the relay'); assert.ok(!/attacker@evil\.test/.test(seen.cmds.join('\n') + seen.data), 'the attacker\'s address never did'); assert.ok(!/^Bcc:/mi.test(seen.data));
  } finally { server.close(); }
});
