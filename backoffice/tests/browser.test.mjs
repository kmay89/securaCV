/* The admin driven by a real browser: the forms as a person submits them,
 * not as fetch() constructs them. This is the layer that catches a button
 * whose form is missing a field the pipeline requires (the logout CSRF bug
 * the HTTP test could not see). Runs on the demo seed in an in-memory DB.
 *
 * Needs Playwright's Chromium. Locally: `npm i --no-save playwright && npx
 * playwright install chromium` in backoffice/, or point PLAYWRIGHT_MODULE at
 * an installed copy. Without it the test skips and says so; CI sets
 * BACKOFFICE_BROWSER=require so a missing browser there is a failure.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createApp } from '../src/server.mjs';
import { setupOperator, totp } from '../src/auth.mjs';
import { seedDemo } from '../src/modules/demo.mjs';
import { testDb, testCfg } from './helpers.mjs';

const require = createRequire(import.meta.url);
function loadPlaywright() {
  for (const spec of [process.env.PLAYWRIGHT_MODULE, 'playwright'].filter(Boolean)) { try { return require(spec); } catch { /* next */ } }
  return null;
}
const pw = loadPlaywright();
const required = process.env.BACKOFFICE_BROWSER === 'require';
if (!pw && required) throw new Error('BACKOFFICE_BROWSER=require but playwright is not installed');

test('a person can log in, ship an order, answer a ticket, move stock, post an expense and log out', { skip: pw ? false : 'playwright not installed (see the header of this file)' }, async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' });
  await seedDemo(db, cfg);
  const totpSecret = setupOperator(db, 'browser-test-password-1');
  const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const browser = await pw.chromium.launch();
  const errors = [];
  try {
    const page = await browser.newPage();
    page.on('pageerror', e => errors.push(`pageerror: ${e.message}`));
    page.on('console', m => { if (m.type() === 'error') errors.push(`console: ${m.text()}`); });

    await page.goto(base + '/orders'); assert.match(page.url(), /\/login$/, 'the admin redirects to login');
    await page.fill('#pw', 'browser-test-password-1'); await page.fill('#code', totp(totpSecret)); await page.click('button:has-text("Log in")');
    await page.waitForURL(base + '/'); assert.ok(await page.textContent('h1') === 'Dashboard');

    // ship the packed order through its form
    const packed = db.get('SELECT id, number FROM orders WHERE status = \'packed\' ORDER BY id LIMIT 1');
    await page.goto(`${base}/orders/${packed.id}`);
    await page.fill('#f_tracking', '9400 1111 2222 3333 4444 55');
    await page.click('button:has-text("Mark shipped + email tracking")');
    await page.waitForURL(/\/orders\/\d+\?ok=/);
    assert.equal(db.get('SELECT status, tracking FROM orders WHERE id = ?', packed.id).status, 'shipped');
    assert.ok(db.get('SELECT id FROM mail_outbox WHERE ref = ?', `order:${packed.number}`), 'the tracking mail is queued');
    assert.ok((await page.textContent('body')).includes('is now shipped'));

    // answer the overdue ticket from a canned reply and close it
    const late = db.get('SELECT id, number FROM tickets WHERE status IN (\'open\',\'waiting_on_us\') AND first_response_at IS NULL ORDER BY sla_due_at LIMIT 1');
    await page.goto(`${base}/tickets/${late.id}`);
    await page.selectOption('#f_canned', 'batch-cadence'); await page.check('input[name="close"]'); await page.click('button:has-text("Send reply")');
    await page.waitForURL(/\/tickets\/\d+\?ok=/);
    const t = db.get('SELECT status, first_response_at FROM tickets WHERE id = ?', late.id);
    assert.equal(t.status, 'closed'); assert.ok(t.first_response_at);
    assert.ok(db.get('SELECT body_text FROM mail_outbox WHERE ref = ?', `ticket:${late.number}`).body_text.includes('ship the following week'), 'the canned text went out with {{name}} filled');

    // receive stock through the inventory form
    const before = db.get('SELECT on_hand FROM inventory_items WHERE sku = \'sd-32gb\'').on_hand;
    await page.goto(base + '/inventory');
    await page.selectOption('#f_sku', 'sd-32gb'); await page.fill('#f_delta', '20'); await page.selectOption('#f_reason', 'receive'); await page.fill('#f_ref', 'po:browser');
    await page.click('button:has-text("Move")'); await page.waitForURL(/\/inventory\?ok=/);
    assert.equal(db.get('SELECT on_hand FROM inventory_items WHERE sku = \'sd-32gb\'').on_hand, before + 20);

    // post an expense; the trial balance stays balanced
    await page.goto(base + '/books');
    await page.fill('#f_cents', '1234'); await page.fill('#f_memo', 'browser test'); await page.click('button:has-text("Post")');
    await page.waitForURL(/\/books\?ok=/);
    assert.ok((await page.textContent('body')).includes('(balanced)'));

    // the real Log out button works (it carries the CSRF token)
    await page.click('button:has-text("Log out")'); await page.waitForURL(base + '/login');
    await page.goto(base + '/orders'); assert.match(page.url(), /\/login$/);

    // the public pages need no login and set no cookie
    const res = await page.goto(base + '/status'); assert.equal(res.status(), 200); assert.ok((await page.textContent('body')).includes('operational'));
    assert.deepEqual(errors, [], 'no page or console errors anywhere');
  } finally { await browser.close(); await app.close(); db.close(); }
});
