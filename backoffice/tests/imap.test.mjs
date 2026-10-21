import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { imapFetchUnseen } from '../src/modules/imap.mjs';

const MSG = 'From: ada@example.test\r\nSubject: hi\r\nMessage-ID: <1@x>\r\n\r\nbody text\r\n';

function fakeImap() {
  const stored = [];
  const server = createServer((sock) => {
    sock.write('* OK fake IMAP ready\r\n');
    sock.on('data', (d) => {
      for (const line of d.toString().split('\r\n').filter(Boolean)) {
        const [tag, ...rest] = line.split(' '); const cmd = rest.join(' ');
        if (/^LOGIN "u" "p"$/.test(cmd)) sock.write(`${tag} OK logged in\r\n`);
        else if (/^LOGIN/.test(cmd)) sock.write(`${tag} NO bad creds\r\n`);
        else if (/^SELECT/.test(cmd)) sock.write(`* 2 EXISTS\r\n* OK [UIDVALIDITY 77] UIDs valid\r\n${tag} OK [READ-WRITE] SELECT completed\r\n`);
        else if (cmd === 'UID SEARCH UNSEEN') sock.write(`* SEARCH 5 9\r\n${tag} OK SEARCH completed\r\n`);
        else if (/^UID FETCH (\d+)/.test(cmd)) { const uid = cmd.match(/^UID FETCH (\d+)/)[1]; sock.write(`* 1 FETCH (UID ${uid} BODY[] {${Buffer.byteLength(MSG)}}\r\n${MSG})\r\n${tag} OK FETCH completed\r\n`); }
        else if (/^UID STORE/.test(cmd)) { stored.push(cmd); sock.write(`${tag} OK STORE completed\r\n`); }
        else if (cmd === 'LOGOUT') { sock.write(`* BYE\r\n${tag} OK bye\r\n`); sock.end(); }
        else sock.write(`${tag} BAD what\r\n`);
      }
    });
  });
  return new Promise(r => server.listen(0, '127.0.0.1', () => r({ server, port: server.address().port, stored })));
}

test('IMAP: login, select, search unseen, fetch literals, mark seen', async () => {
  const { server, port, stored } = await fakeImap();
  const msgs = await imapFetchUnseen(`imap://u:p@127.0.0.1:${port}/INBOX`);
  assert.equal(msgs.length, 2);
  assert.equal(msgs[0].uid, '77:5'); assert.equal(msgs[1].uid, '77:9');
  assert.equal(msgs[0].raw.toString(), MSG);
  assert.deepEqual(stored, ['UID STORE 5 +FLAGS (\\Seen)', 'UID STORE 9 +FLAGS (\\Seen)']);
  await assert.rejects(imapFetchUnseen(`imap://u:wrong@127.0.0.1:${port}/INBOX`), /imap login/);
  server.close();
});
