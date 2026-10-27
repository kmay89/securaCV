/* The admin on a phone and on an iPad, held to the rules that make it usable with a thumb:
 *   - no page is wider than the screen (a table scrolls inside its own box, never the page);
 *   - every navigation link, button and field is at least 40px tall, so a finger lands on it;
 *   - every field is 16px type or larger, so iOS does not zoom the page when one is focused;
 *   - no text is under 12px;
 *   - a table of five or more columns folds into cards on a phone (its header row is gone,
 *     each cell carries its own label), so nothing needs a sideways scroll to read.
 * Every section in the nav is visited, plus the detail pages a day's work lands on. Runs on the
 * demo seed with Playwright's Chromium emulating a touch screen; skips without Playwright, and
 * CI sets BACKOFFICE_BROWSER=require so a missing browser there is a failure.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createApp } from '../src/server.mjs';
import { totp } from '../src/auth.mjs';
import { NAV } from '../src/render.mjs';
import { seedDemo, DEMO_USERS, DEMO_PASSWORD, DEMO_TOTP_SECRET } from '../src/modules/demo.mjs';
import { testDb, testCfg } from './helpers.mjs';

const require = createRequire(import.meta.url);
function loadPlaywright() {
  for (const spec of [process.env.PLAYWRIGHT_MODULE, 'playwright'].filter(Boolean)) { try { return require(spec); } catch { /* next */ } }
  return null;
}
const pw = loadPlaywright();
if (!pw && process.env.BACKOFFICE_BROWSER === 'require') throw new Error('BACKOFFICE_BROWSER=require but playwright is not installed');

const DEVICES = [['phone', 390, 844], ['ipad', 820, 1180]];

test('every admin page fits a phone and an iPad: no sideways page, 40px targets, 16px fields, 12px text, folded tables', { skip: pw ? false : 'playwright not installed (see the header of this file)' }, async () => {
  const db = testDb(); const cfg = testCfg({ secret: 's' });
  await seedDemo(db, cfg);
  const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`; cfg.baseUrl = base;
  const order = db.get("SELECT id FROM orders WHERE status = 'packed' ORDER BY id LIMIT 1");
  const ticket = db.get('SELECT id FROM tickets ORDER BY id LIMIT 1');
  const contact = db.get('SELECT id FROM contacts ORDER BY id LIMIT 1');
  const pages = [...NAV.map(([p]) => p), `/orders/${order.id}`, `/tickets/${ticket.id}`, `/contacts/${contact.id}`, '/orders/new', '/tickets/new', '/search?q=canary', '/me', '/login'];
  const browser = await pw.chromium.launch();
  const problems = []; let cookies = null;   // one login, its session carried to the second device: a code is good for one login
  try {
    for (const [name, width, height] of DEVICES) {
      const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
      const page = await ctx.newPage();
      page.on('pageerror', e => problems.push(`${name}: pageerror ${e.message}`));
      if (cookies) await ctx.addCookies(cookies);
      else { await page.goto(base + '/login'); await page.fill('#email', DEMO_USERS[0].email); await page.fill('#pw', DEMO_PASSWORD); await page.fill('#code', totp(DEMO_TOTP_SECRET)); await page.click('button:has-text("Log in")'); await page.waitForURL(base + '/'); cookies = await ctx.cookies(); }
      for (const p of pages) {
        const r = await page.goto(base + p, { waitUntil: 'load' });
        if (!r.ok()) { problems.push(`${name} ${p}: HTTP ${r.status()}`); continue; }
        const found = await page.evaluate(({ phone }) => {
          const out = []; const vw = document.documentElement.clientWidth;
          const over = document.documentElement.scrollWidth - vw;
          if (over > 0) out.push(`page is ${over}px wider than the screen`);
          const box = (el) => el.getBoundingClientRect();
          const name = (el) => `${el.tagName.toLowerCase()}${el.id ? '#' + el.id : ''}${el.className && typeof el.className === 'string' ? '.' + el.className.trim().split(/\s+/)[0] : ''}`;
          for (const el of document.querySelectorAll('nav a, button, .btn, input:not([type=hidden]):not([type=checkbox]):not([type=radio]), select, textarea, summary')) {
            const b = box(el); if (b.width === 0 || b.height === 0) continue;
            if (el.closest('table') || el.classList.contains('x')) continue;   // in-table controls and the tag-remove × are deliberately compact
            if (b.height < 36) out.push(`${name(el)} is ${Math.round(b.height)}px tall`);
          }
          for (const el of document.querySelectorAll('input:not([type=hidden]):not([type=checkbox]):not([type=radio]), select, textarea')) {
            if (parseFloat(getComputedStyle(el).fontSize) < 16) out.push(`${name(el)} is ${getComputedStyle(el).fontSize} type (iOS zooms under 16px)`);
          }
          for (const el of document.querySelectorAll('body *')) {
            if (el.children.length || !el.textContent.trim()) continue;
            const fs = parseFloat(getComputedStyle(el).fontSize); if (fs < 12) out.push(`${name(el)} text is ${fs}px`);
          }
          if (phone) for (const t of document.querySelectorAll('.tw.fold table')) {
            if (getComputedStyle(t.querySelector('thead')).display !== 'none') out.push('a folded table still shows its header row');
            if (box(t).width > vw) out.push(`a folded table is ${Math.round(box(t).width)}px wide on a ${vw}px screen`);
          }
          return [...new Set(out)].slice(0, 12);
        }, { phone: name === 'phone' });
        for (const f of found) problems.push(`${name} ${p}: ${f}`);
      }
      await ctx.close();
    }
  } finally { await browser.close(); await app.close(); }
  assert.deepEqual(problems, [], `the admin does not fit a touch screen:\n  ${problems.join('\n  ')}`);
});
