import { test } from 'node:test';
import assert from 'node:assert/strict';
import { openTicket, ingestEmail, parseEmail, classify, reply, slaBreaches, ticketByNumber, decodeWords, htmlToText, BODY_MAX, MIME_DEPTH } from '../src/modules/support.mjs';
import { enqueue } from '../src/modules/mail.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('parseEmail handles folded headers, RFC 2047, multipart with QP and base64, and HTML-only mail', () => {
  const raw = ['From: =?UTF-8?B?QWRhIEzDtnZlbGFjZQ==?= <ada@example.test>', 'To: hello@errer.test', 'Subject: =?utf-8?q?Caf=C3=A9_question?=', 'Message-ID: <abc@example.test>', 'In-Reply-To: <xyz@errer.test>',
    'Content-Type: multipart/alternative;', '\tboundary="B1"', '', '--B1', 'Content-Type: text/plain; charset=utf-8', 'Content-Transfer-Encoding: quoted-printable', '', 'Hello =E2=80=94 caf=C3=A9?=', '=0A', 'bye', '--B1', 'Content-Type: text/html; charset=utf-8', 'Content-Transfer-Encoding: base64', '', Buffer.from('<p>Hello</p>').toString('base64'), '--B1--', ''].join('\r\n');
  const m = parseEmail(raw);
  assert.equal(m.from, 'Ada Lövelace <ada@example.test>'); assert.equal(m.subject, 'Café question'); assert.equal(m.messageId, '<abc@example.test>'); assert.equal(m.inReplyTo, '<xyz@errer.test>');
  assert.equal(m.text, 'Hello — café?\n\nbye');
  const htmlOnly = parseEmail('From: x@y.z\r\nContent-Type: text/html\r\n\r\n<div>Hi<br>there &amp; you</div>');
  assert.equal(htmlOnly.text, 'Hi\nthere & you');
  assert.equal(decodeWords('=?UTF-8?B?w6k=?='), 'é');
});

test('tickets open with a thread token, auto-ack, classify, and replies thread back by token or Message-ID', () => {
  const db = testDb(); const cfg = testCfg();
  const t = openTicket(db, { email: 'ada@example.test', subject: 'My board arrived dead', body: 'DOA', cfg });
  assert.equal(t.number, 'T-000001'); assert.equal(t.kind, 'warranty'); assert.equal(t.priority, 'high'); assert.match(t.thread_token, /^[a-z0-9]{6}$/);
  const ack = db.get('SELECT * FROM mail_outbox WHERE ref = ?', 'ticket:T-000001');
  assert.ok(ack && ack.subject.includes(`[T-000001 ${t.thread_token}]`));
  // customer replies with the token in the subject
  const r1 = ingestEmail(db, { from: 'Ada <ada@example.test>', subject: `Re: [T-000001 ${t.thread_token}] Re: My board arrived dead`, text: 'photo attached', messageId: '<m1@x>' }, { cfg });
  assert.ok(!r1.created); assert.equal(r1.ticket.messages.length, 2); assert.equal(r1.ticket.status, 'waiting_on_us');
  // duplicate delivery of the same Message-ID is dropped
  assert.ok(ingestEmail(db, { from: 'ada@example.test', subject: 'whatever', text: 'again', messageId: '<m1@x>' }, { cfg }).duplicate);
  // we reply; the reply's message id threads a later answer with no token
  reply(db, t.id, 'Replacement on the way.', { cfg });
  const out = db.get('SELECT * FROM mail_outbox WHERE kind = \'support\' AND body_text LIKE \'Replacement%\'');
  assert.ok(out.headers.includes('<m1@x>'), 'In-Reply-To the customer\'s last message');
  assert.equal(ticketByNumber(db, 'T-000001').status, 'waiting_on_customer');
  db.run('UPDATE mail_outbox SET message_id = ? WHERE id = ?', '<ours@errer.test>', out.id);
  const r2 = ingestEmail(db, { from: 'ada@example.test', subject: 'thanks!!', text: 'ty', messageId: '<m2@x>', inReplyTo: '<ours@errer.test>' }, { cfg });
  assert.equal(r2.ticket.number, 'T-000001');
  // an auto-reply never opens a ticket, and files as a note on an existing one
  assert.equal(ingestEmail(db, { from: 'MAILER-DAEMON@x', subject: 'Undeliverable: hi', text: 'bounce', messageId: '<b@x>' }, { cfg }).ignored, 'bounce');
  const r3 = ingestEmail(db, { from: 'ada@example.test', subject: `Automatic reply: [T-000001 ${t.thread_token}]`, text: 'OOO', messageId: '<m3@x>' }, { cfg });
  assert.equal(r3.ticket.messages.at(-1).direction, 'note');
  // a brand-new mail opens a new ticket
  const r4 = ingestEmail(db, { from: 'Press <press@paper.test>', subject: 'Interview request for an article', text: '...', messageId: '<p@x>' }, { cfg });
  assert.ok(r4.created); assert.equal(r4.ticket.kind, 'press');
  assert.equal(classify('security vulnerability in the portal').priority, 'urgent');
});

test('SLA: an unanswered ticket past the window is a breach; an answered one is not', () => {
  const db = testDb(); const cfg = testCfg({ slaHours: 1 });
  const t = openTicket(db, { email: 'a@b.test', subject: 'help', body: 'x', cfg });
  assert.equal(slaBreaches(db).length, 0);
  db.run('UPDATE tickets SET sla_due_at = ? WHERE id = ?', '2000-01-01T00:00:00Z', t.id);
  assert.equal(slaBreaches(db).length, 1);
  reply(db, t.id, 'on it', { cfg });
  assert.equal(slaBreaches(db).length, 0);
});

test('htmlToText drops script/style bodies, keeps block breaks, decodes entities once', async () => {
  const { htmlToText } = await import('../src/modules/support.mjs');
  assert.equal(htmlToText('<div>Hi<br>there &amp;lt; you</div><script>alert(1)</script><style>p{}</style><p>bye</p>'), 'Hi\nthere &lt; you\nbye\n');
  assert.equal(htmlToText('<scr<script>ipt>x</script>'), 'ipt>x');
  assert.equal(htmlToText('a <b'), 'a ');
});

test('a 2 MB bounce body is read in milliseconds, not minutes: the text is capped before anything scans it', () => {
  const db = testDb(); const cfg = testCfg();
  const raw = 'From: postmaster@relay.example.test\r\nSubject: Undeliverable: hi\r\nMessage-ID: <big@relay.example.test>\r\n\r\n' + 'a'.repeat(2 * 1024 * 1024);
  const t0 = Date.now(); const m = parseEmail(raw); const r = ingestEmail(db, m, { cfg }); const ms = Date.now() - t0;
  assert.ok(ms < 500, `took ${ms} ms`); assert.equal(m.text.length, BODY_MAX); assert.equal(r.ignored, 'bounce'); assert.deepEqual(r.bounced, []);
  // the same body handed straight to the API is capped the same way
  const t1 = Date.now(); const r2 = ingestEmail(db, { from: 'postmaster@relay.example.test', subject: 'Undeliverable: hi', text: 'b'.repeat(2 * 1024 * 1024) + ' x@y.test', messageId: '<big2@relay.example.test>' }, { cfg });
  assert.ok(Date.now() - t1 < 500); assert.deepEqual(r2.bounced, []);
  const r3 = ingestEmail(db, { from: 'ada@example.test', subject: 'long', text: 'c'.repeat(2 * 1024 * 1024), messageId: '<big3@x>' }, { cfg });
  assert.equal(r3.ticket.messages[0].body.length, BODY_MAX);
  db.close();
});

test('HTML-only mail with twenty thousand script tags is one walk, and a multipart nested thousands deep is read, not recursed', () => {
  const t0 = Date.now(); const out = htmlToText('<p>hi</p>' + '<SCRIPT>var x = 1;</script>'.repeat(20_000) + '<style>a{}</STYLE>bye' + '<script>never closed');
  assert.ok(Date.now() - t0 < 500); assert.equal(out, 'hi\nbye');
  let body = 'Content-Type: text/plain\r\n\r\ndeep hello\r\n';
  for (let i = 0; i < 3800; i++) body = `Content-Type: multipart/mixed; boundary="b${i}"\r\n\r\n--b${i}\r\n${body}\r\n--b${i}--\r\n`;
  const m = parseEmail('From: a@b.test\r\nSubject: nest\r\n' + body);
  assert.equal(typeof m.text, 'string'); assert.ok(m.text.length > 0, 'the part past the depth cap is kept as text'); assert.ok(m.text.includes('multipart/mixed'), `depth ${MIME_DEPTH} reached: the rest is opaque`); assert.deepEqual(m.attachments, []);
  // a shallow nest still finds the plain part
  const ok = parseEmail('From: a@b.test\r\nContent-Type: multipart/mixed; boundary="o"\r\n\r\n--o\r\nContent-Type: multipart/alternative; boundary="i"\r\n\r\n--i\r\nContent-Type: text/plain\r\n\r\ninner\r\n--i--\r\n--o--\r\n');
  assert.equal(ok.text, 'inner');
});

test('threading trusts only our own Message-IDs, and a reply from another address than the ticket\'s contact is a flagged note', () => {
  const db = testDb(); const cfg = testCfg();
  const t = openTicket(db, { email: 'ada@example.test', subject: 'Help', body: 'x', cfg, messageId: '<ada-1@customer.test>' });
  // a stranger who quotes the customer's own (sender-chosen) id does not land on her ticket
  const r1 = ingestEmail(db, { from: 'Mallory <mallory@evil.test>', subject: 'Re: Help', text: 'me too', messageId: '<m-1@evil.test>', inReplyTo: '<ada-1@customer.test>' }, { cfg });
  assert.ok(r1.created); assert.notEqual(r1.ticket.number, t.number);
  // our outbox id does thread — for the customer as her own word, for anyone else as a note
  const out = enqueue(db, { to: 'ada@example.test', subject: 'Re: Help', text: 'reply', kind: 'support', ref: `ticket:${t.number}` }); db.run('UPDATE mail_outbox SET message_id = ? WHERE id = ?', '<ours-1@errer.test>', out.id);
  const r2 = ingestEmail(db, { from: 'ada@example.test', subject: 'Re: Help', text: 'thanks', messageId: '<ada-2@customer.test>', inReplyTo: '<ours-1@errer.test>' }, { cfg });
  assert.equal(r2.ticket.number, t.number); assert.equal(r2.ticket.messages.at(-1).direction, 'in'); assert.equal(r2.stranger, undefined);
  const r3 = ingestEmail(db, { from: 'mallory@evil.test', subject: `Re: [${t.number} ${t.thread_token}] Help`, text: 'send it to me instead', messageId: '<m-2@evil.test>', attachments: [{ filename: 'x.png', mime: 'image/png', bytes: Buffer.from('x') }] }, { cfg });
  assert.equal(r3.ticket.number, t.number); assert.equal(r3.stranger, true); const note = r3.ticket.messages.at(-1); assert.equal(note.direction, 'note'); assert.match(note.body, /^\[from another address than the ticket's contact\] send it/); assert.equal(note.from_addr, 'mallory@evil.test'); assert.equal(r3.attachments, 0);
  assert.equal(ticketByNumber(db, t.number).status, 'waiting_on_us', 'a note does not move the status the customer\'s reply set');
  db.close();
});
