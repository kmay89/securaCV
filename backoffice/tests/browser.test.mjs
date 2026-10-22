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
    await page.waitForURL(base + '/'); assert.ok(await page.$('.hero'), 'the home page shows the hero');

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

    // the storefront pages: a note and tags on an order, a discount created through its form, a customer, a product, analytics, search, a packing slip
    const shipped = db.get('SELECT id, number, contact_id FROM orders WHERE status = \'shipped\' ORDER BY id DESC LIMIT 1');
    await page.goto(`${base}/orders/${shipped.id}`);
    await page.fill('#f_body', 'Customer asked for a gift note'); await page.click('button:has-text("Add note")'); await page.waitForURL(/\/orders\/\d+\?ok=/);
    await page.fill('#f_tags', 'gift, vip'); await page.click('button:has-text("Save tags")'); await page.waitForURL(/\/orders\/\d+\?ok=/);
    assert.equal(db.get('SELECT tags FROM orders WHERE id = ?', shipped.id).tags, 'gift,vip');
    assert.ok((await page.textContent('.tl')).includes('Customer asked for a gift note'), 'the note is on the timeline');
    await page.click('button:has-text("Re-send tracking email")'); await page.waitForURL(/\/orders\/\d+\?ok=/);
    await page.goto(`${base}/orders/${shipped.id}/packing-slip`); assert.ok((await page.textContent('.slip')).includes(shipped.number));
    await page.goto(base + '/discounts');
    await page.fill('#f_code', 'BROWSER10'); await page.fill('#f_value', '10'); await page.fill('#f_note', 'from the browser test'); await page.click('button:has-text("Create code")'); await page.waitForURL(/\/discounts\?ok=/);
    assert.equal(db.get('SELECT kind, value FROM discounts WHERE code = \'BROWSER10\'').value, 10);
    await page.fill('#f_try', 'browser10'); await page.fill('#f_subtotal', '6900'); await page.click('button:has-text("Evaluate")'); await page.waitForURL(/\/discounts\?try=/);
    assert.ok((await page.textContent('.flash')).includes('$6.90'));
    await page.goto(`${base}/contacts/${shipped.contact_id}`); assert.ok((await page.textContent('.cards')).includes('Lifetime spend'));
    await page.fill('#f_tags', 'vip'); await page.click('button:has-text("Save tags")'); await page.waitForURL(/\/contacts\/\d+\?ok=/);
    await page.goto(base + '/contacts?segment=repeat'); assert.ok((await page.textContent('table')).includes('@customer.example.test'));
    await page.goto(base + '/products'); await page.click('a:has-text("Canary WAP Kit")'); await page.waitForURL(/\/products\/wap-kit/); assert.ok((await page.textContent('body')).includes('What one unit consumes'));
    await page.goto(base + '/analytics?days=90'); assert.ok((await page.$$('svg.chart')).length >= 3, 'the charts render'); assert.ok((await page.textContent('body')).includes('Top products'));
    await page.fill('input[name="q"]', shipped.number); await page.press('input[name="q"]', 'Enter'); await page.waitForURL(/\/search\?q=/);
    assert.ok((await page.textContent('body')).includes(shipped.number), 'the header search finds the order');
    await page.goto(base + '/batches'); assert.ok((await page.textContent('body')).includes('this week'));
    // bulk: select every "to pack" order and pack it
    await page.goto(base + '/orders?status=paid'); const boxes = await page.$$('input[name="ids[]"]'); assert.ok(boxes.length >= 1);
    for (const b of boxes) await b.check();
    await page.selectOption('#f_action', 'packed'); await page.click('button:has-text("Apply")'); await page.waitForURL(/\/orders\?ok=/);
    assert.equal(db.get('SELECT COUNT(*) AS n FROM orders WHERE status = \'paid\'').n, 0, 'every paid order is packed');

    // the loop closes: a Decide card approves a drafted purchase order in one click and lands back on Home
    await page.goto(base + '/'); assert.ok(await page.$('.decide'), 'Home shows the Decide queue');
    const draft = db.get("SELECT id, number FROM purchase_orders WHERE status = 'draft'"); assert.ok(draft, 'the seed drafts a print run');
    await page.click(`form[action="/purchasing/${draft.id}/order"] button`); await page.waitForURL(/\/\?ok=/);
    assert.equal(db.get('SELECT status FROM purchase_orders WHERE id = ?', draft.id).status, 'ordered', 'approved from the card');
    assert.ok(!(await page.textContent('body')).includes(`Approve ${draft.number}`), 'and the card is gone');
    // receive it on its page; stock moves, the books book it
    await page.goto(`${base}/purchasing/${draft.id}`); await page.click('button:has-text("Receive into stock")'); await page.waitForURL(/\/purchasing\/\d+\?ok=/);
    assert.equal(db.get('SELECT status FROM purchase_orders WHERE id = ?', draft.id).status, 'received');
    // Marketing: the funnel, the two tile maps and the sends
    await page.goto(base + '/marketing'); const mk = await page.textContent('body'); assert.ok(mk.includes('Bought again') && mk.includes('Customers by state') && mk.includes('What each campaign did'), 'the marketing view renders'); assert.ok((await page.$$('svg.chart')).length >= 2, 'the maps are drawn');
    // a recall by firmware: look it up, write it, see who gets it; sending stays a separate step
    await page.goto(base + '/devices/recall?fw=2.4.1'); assert.ok((await page.textContent('body')).includes('Customers to notify'));
    await page.click('button:has-text("Create the notice")'); await page.waitForURL(/\/devices\/recall\/\d+\?ok=/); assert.ok((await page.textContent('body')).includes('not sent'), 'nothing is sent by creating it');
    assert.equal(db.get('SELECT COUNT(*) AS n FROM mail_outbox WHERE ref LIKE \'recall:%\'').n, 0);
    // an order on its way shows the carrier's word and the customer's status page, which opens without a login
    const otw = db.get("SELECT id, number FROM orders WHERE tracking_status = 'out_for_delivery'"); await page.goto(`${base}/orders/${otw.id}`);
    assert.ok((await page.textContent('.where')).includes('Out for delivery'), 'the delivery line'); const statusHref = await page.getAttribute('a:has-text("status page")', 'href');
    // the ticket with the photo shows it inline
    const withPhoto = db.get('SELECT m.ticket_id FROM attachments a JOIN ticket_messages m ON m.id = a.ticket_message_id'); await page.goto(`${base}/tickets/${withPhoto.ticket_id}`);
    assert.ok(await page.$('.atts img'), 'the attachment renders'); assert.equal(await page.$eval('.atts img', (i) => i.naturalWidth), 96, 'as a real image');

    // the real Log out button works (it carries the CSRF token)
    await page.click('button:has-text("Log out")'); await page.waitForURL(base + '/login');
    await page.goto(base + '/orders'); assert.match(page.url(), /\/login$/);

    // the public pages need no login and set no cookie
    const res = await page.goto(base + '/status'); assert.equal(res.status(), 200); assert.ok((await page.textContent('body')).includes('operational'));
    const sp = await page.goto(statusHref); assert.equal(sp.status(), 200); assert.ok((await page.textContent('body')).includes('Out for delivery'), 'the customer sees the same line');
    assert.deepEqual(errors, [], 'no page or console errors anywhere');
  } finally { await browser.close(); await app.close(); db.close(); }
});
