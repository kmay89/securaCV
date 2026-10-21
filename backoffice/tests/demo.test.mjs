import { test } from 'node:test';
import assert from 'node:assert/strict';
import { seedDemo } from '../src/modules/demo.mjs';
import { trialBalance } from '../src/modules/ledger.mjs';
import { snapshot } from '../src/modules/dashboard.mjs';
import { testDb, testCfg } from './helpers.mjs';

test('the demo seed fills an empty database consistently and refuses a used one', async () => {
  const db = testDb(); const cfg = testCfg();
  const r = await seedDemo(db, cfg);
  assert.ok(r.orders >= 7 && r.tickets >= 4 && r.contacts >= 8 && r.items === 10);
  assert.ok(trialBalance(db).balanced, 'the demo books balance');
  const s = snapshot(db);
  assert.ok(s.orders.open >= 2); assert.equal(s.support.breached, 1, 'one ticket is past its SLA on purpose'); assert.equal(s.exceptions.length, 1);
  assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE status = \'sent\'').n, 0, 'nothing is sent by a seed');
  await assert.rejects(seedDemo(db, cfg), /already has orders/);
});
