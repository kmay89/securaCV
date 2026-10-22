import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { buildMessage, qpEncode, smtpSend, enqueue, sendOutbox, fill } from '../src/modules/mail.mjs';
import { upsertContact } from '../src/modules/contacts.mjs';
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
  assert.equal(db.get('SELECT status, attempts FROM mail_outbox WHERE to_addr = ?', 'later@b.test').status, 'queued');
  // no SMTP configured → nothing is sent, nothing is lost
  assert.equal((await sendOutbox(db, testCfg({ smtpUrl: '' }))).skipped, 1);
});

test('smtp:// fails closed when the relay does not offer STARTTLS', async () => {
  const { server, port } = await fakeSmtp();
  await assert.rejects(smtpSend(`smtp://u:p@127.0.0.1:${port}`, { from: 'x@y.test', to: 'a@b.test', raw: 'x' }), /does not offer STARTTLS/);
  server.close();
});
