import { test } from 'node:test';
import assert from 'node:assert/strict';
import { hashPassword, verifyPassword, totp, verifyTotp, base32Encode, base32Decode, setupOperator, login, sessionFor, createApiToken, apiTokenFor, revokeApiToken } from '../src/auth.mjs';
import { testDb } from './helpers.mjs';

test('scrypt password hashes verify and do not match a wrong password', () => {
  const h = hashPassword('correct horse battery staple');
  assert.ok(verifyPassword('correct horse battery staple', h));
  assert.ok(!verifyPassword('correct horse battery stable', h));
  assert.ok(!verifyPassword('x', null));
});

test('TOTP matches the RFC 6238 SHA-1 test vectors (6-digit truncation)', () => {
  const secret = base32Encode(Buffer.from('12345678901234567890'));
  assert.equal(base32Decode(secret).toString(), '12345678901234567890');
  // RFC 6238 Appendix B, SHA1: T=59 → 94287082; T=1111111109 → 07081804; T=1234567890 → 89005924
  assert.equal(totp(secret, { time: 59 * 1000 }), '287082');
  assert.equal(totp(secret, { time: 1111111109 * 1000 }), '081804');
  assert.equal(totp(secret, { time: 1234567890 * 1000 }), '005924');
  assert.ok(verifyTotp(secret, '287082', { time: 59 * 1000 }));
  assert.ok(verifyTotp(secret, '287082', { time: 59 * 1000 + 30_000 }), 'one step of drift is accepted');
  assert.ok(!verifyTotp(secret, '287082', { time: 59 * 1000 + 90_000 }));
  assert.ok(!verifyTotp(secret, 'abcdef', { time: 59 * 1000 }));
});

test('operator setup, login with password + code, sessions and API tokens', () => {
  const db = testDb();
  const secret = setupOperator(db, 'a very long password');
  assert.equal(login(db, { password: 'a very long password', code: '000000' }), null, 'wrong code fails');
  assert.equal(login(db, { password: 'wrong', code: totp(secret) }), null, 'wrong password fails');
  const s = login(db, { password: 'a very long password', code: totp(secret) });
  assert.ok(s && s.id && s.csrf && s.role === 'owner', 'the only person can log in without typing an email');
  // a code is good for one login: the same one again is refused, the next step's is not
  assert.equal(login(db, { email: 'owner@localhost', password: 'a very long password', code: totp(secret) }), null, 'the code just used is spent');
  const later = Date.now() + 30_000;
  assert.ok(login(db, { email: 'owner@localhost', password: 'a very long password', code: totp(secret, { time: later }) }, { now: later }), 'and with the email');
  assert.equal(login(db, { email: 'nobody@localhost', password: 'a very long password', code: totp(secret, { time: later }) }, { now: later }), null);
  assert.ok(sessionFor(db, s.id));
  assert.equal(sessionFor(db, 'nope'), null);
  const t = createApiToken(db, 'cli');
  assert.ok(t.startsWith('bo_'));
  assert.equal(apiTokenFor(db, t).name, 'cli');
  assert.equal(apiTokenFor(db, 'bo_bad'), null);
  assert.equal(apiTokenFor(db, t.replace(/.$/, c => c === 'a' ? 'b' : 'a')), null, 'a one-character change fails');
  assert.equal(apiTokenFor(db, t.split('.')[0] + '.x'), null, 'the right id with the wrong secret fails');
  assert.equal(revokeApiToken(db, 'cli'), 1);
  assert.equal(apiTokenFor(db, t), null);
});
