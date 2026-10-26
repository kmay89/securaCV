// The pack bench as a gate. A one- or two-person operation does not need a
// scanner and a workflow engine to stop a wrong box; it needs the four things
// that go wrong to be refused by name: a line that never went in, a board
// that is missing or is the wrong kit's, a box that weighs like the wrong
// count, and a plan frozen for lines that changed. The bench proves each
// (tick, scan, scale); the server checks the same proof, so the gate holds
// with the browser's CSS off and with a form posted by hand.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { recordOrder, transition, orderById, boardsNeeded, boardsOn, importLabels } from '../src/modules/orders.mjs';
import { registerDevice, assignToOrder, unassign } from '../src/modules/devices.mjs';
import { upsertItem, setRecipe, item } from '../src/modules/inventory.mjs';
import { DEMO_SHIPPING, DEMO_USERS, DEMO_PASSWORD, DEMO_TOTP_SECRET, seedDemo } from '../src/modules/demo.mjs';
import { createApp } from '../src/server.mjs';
import { totp } from '../src/auth.mjs';
import { qrSvg } from '../src/qr.mjs';
import { testDb, testCfg, catalog, sampleOrder, fixture } from './helpers.mjs';

const twoKits = (over = {}) => sampleOrder({ items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 2, unit_cents: 6900 }], ...over });
const stock = (db) => { upsertItem(db, { sku: 'xiao-esp32s3', name: 'board', on_hand: 20 }); upsertItem(db, { sku: 'xiao-esp32s3-sense', name: 'sense board', on_hand: 20 }); setRecipe(db, 'wap-kit', { 'xiao-esp32s3': 1 }); setRecipe(db, 'vision-kit', { 'xiao-esp32s3-sense': 1 }); setRecipe(db, 'witness-pair', { 'xiao-esp32s3': 2 }); };
const boards = (db) => { for (let i = 1; i <= 4; i++) registerDevice(db, { serial: `W-${i}`, board: 'xiao-esp32s3', product_sku: 'wap-kit', fw_version: '2.5.0' }); registerDevice(db, { serial: 'V-1', board: 'xiao-esp32s3-sense', product_sku: 'vision-kit', fw_version: '2.5.0' }); registerDevice(db, { serial: 'PLAIN-1' }); };

test('boardsNeeded reads the recipe (a board part per unit), falls back to what boards are flashed for, and says zero when the data cannot say', () => {
  const db = testDb(); const cat = catalog(); stock(db); boards(db);
  assert.deepEqual(boardsNeeded(db, recordOrder(db, twoKits(), { catalog: cat }).order), { needed: 2, lines: [{ sku: 'wap-kit', boards: 2 }] });
  const pair = testDb(); boards(pair); upsertItem(pair, { sku: 'xiao-esp32s3', name: 'board', on_hand: 20 }); setRecipe(pair, 'wap-kit', { 'xiao-esp32s3': 2 });
  assert.equal(boardsNeeded(pair, recordOrder(pair, twoKits(), { catalog: cat }).order).needed, 4, 'a recipe with two boards per unit, two units');
  assert.equal(boardsNeeded(db, recordOrder(db, sampleOrder({ external_id: 'parts', items: [{ sku: 'wap-parts', name: 'Parts', qty: 3, unit_cents: 1900 }] }), { catalog: cat }).order).needed, 0, 'a parts pack has no board');
  // no recipe at all: a SKU registered boards are flashed for still counts one per unit; an unknown SKU counts nothing
  const bare = testDb(); boards(bare);
  assert.equal(boardsNeeded(bare, recordOrder(bare, twoKits(), { catalog: cat }).order).needed, 2);
  assert.equal(boardsNeeded(bare, recordOrder(bare, sampleOrder({ external_id: 'parts', items: [{ sku: 'wap-parts', name: 'Parts', qty: 1, unit_cents: 1900 }] }), { catalog: cat }).order).needed, 0);
});

test('a board goes in only if the order asks for it and has room; it can come back out until the box ships', () => {
  const db = testDb(); const cat = catalog(); stock(db); boards(db);
  const o = recordOrder(db, twoKits(), { catalog: cat }).order;
  assert.throws(() => assignToOrder(db, 'V-1', o.id), /V-1 is flashed for vision-kit — EL-000001 has no vision-kit line \(it has 2× wap-kit\)/);
  assignToOrder(db, 'W-1', o.id); assignToOrder(db, 'W-2', o.id);
  assert.throws(() => assignToOrder(db, 'W-3', o.id), /needs 2 boards and already has W-1, W-2 — take one out first/);
  assignToOrder(db, 'W-2', o.id, { actor: 'web' }); // the same board again is not a third
  assert.throws(() => unassign(db, 'W-3'), /W-3 is not in any box/);
  unassign(db, 'W-2'); assert.deepEqual(boardsOn(db, o.id).map(b => b.serial), ['W-1']);
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? ORDER BY id DESC LIMIT 1", o.id).text, /W-2 taken back out of this box/);
  assignToOrder(db, 'W-3', o.id);
  // a plain serial (no product on record) is not second-guessed by product, only by count
  const p = recordOrder(db, sampleOrder({ external_id: 'plain' }), { catalog: cat }).order; assignToOrder(db, 'PLAIN-1', p.id);
  assert.throws(() => assignToOrder(db, 'W-4', p.id), /needs 1 board and already has PLAIN-1/);
  transition(db, o.id, 'packed', { shipping: DEMO_SHIPPING }); transition(db, o.id, 'shipped', { tracking: '9400 1' });
  assert.throws(() => unassign(db, 'W-1'), /went out with EL-000001 — it stays on the record/);
});

test('packing with the bench\'s check: every line ticked, every board scanned, the scale within tolerance — each refused by name, "pack anyway" needs a reason and is written down', () => {
  const db = testDb(); const cfg = testCfg(); const cat = catalog(); stock(db); boards(db);
  const o = recordOrder(db, twoKits(), { catalog: cat }).order;
  const pack = (check) => transition(db, o.id, 'packed', { shipping: DEMO_SHIPPING, cfg, check });
  assert.throws(() => pack({ ticked: [] }), /not every line is ticked: 2× wap-kit — tick each line as it goes in the box, or tick "pack anyway"/);
  assert.throws(() => pack({ ticked: ['wap-kit'] }), /needs 2 boards in the box, 0 scanned — scan the 2 missing/);
  assignToOrder(db, 'W-1', o.id);
  assert.throws(() => pack({ ticked: ['wap-kit'] }), /needs 2 boards in the box, 1 scanned — scan the 1 missing/);
  assignToOrder(db, 'W-2', o.id);
  assert.throws(() => pack({ ticked: ['wap-kit'], measuredOz: 7 }), /scale reads 7 oz, the plan expects 14 oz \(-7 oz, over the 1\.5 oz tolerance\) — recount the box, or tick "pack anyway"/);
  assert.throws(() => pack({ ticked: ['wap-kit'], measuredOz: 'a lot' }), /number of ounces above zero/);
  assert.throws(() => pack({ ticked: [], anyway: true }), /"pack anyway" needs a reason/);
  assert.equal(orderById(db, o.id).status, 'paid'); assert.equal(item(db, 'xiao-esp32s3').on_hand, 20, 'nothing was pulled from stock by a refused pack');
  pack({ ticked: ['wap-kit'], measuredOz: 14.4 });
  const packed = orderById(db, o.id); assert.equal(packed.status, 'packed'); assert.equal(packed.measured_oz, 14.4);
  assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'packed'", o.id).text, /every line ticked · boards W-1, W-2 · scale 14\.4 oz vs 14 oz ✓/);
  // the override: what was skipped and why, on the timeline and in the audit
  const p = recordOrder(db, sampleOrder({ external_id: 'p2' }), { catalog: cat }).order;
  transition(db, p.id, 'packed', { shipping: DEMO_SHIPPING, cfg, check: { ticked: [], measuredOz: 20, anyway: true, reason: 'board arrives tomorrow, customer asked for the shell today' } });
  const ev = db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'packed'", p.id).text;
  assert.match(ev, /1 line unticked, 0 of 1 boards, scale \+13 oz, packed anyway: board arrives tomorrow/);
  assert.deepEqual(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'order.packed' ORDER BY id DESC LIMIT 1").detail).skipped.length, 4);
  // no check at all (the API, a seed) packs as it always did
  const q = recordOrder(db, sampleOrder({ external_id: 'q3' }), { catalog: cat }).order; transition(db, q.id, 'packed', { shipping: DEMO_SHIPPING });
  assert.equal(orderById(db, q.id).status, 'packed');
});

test('over HTTP: the bench shows the box card, one tick per line and the board count; its post carries the check; the order page sends you to the bench', async () => {
  const cfg = testCfg({ demo: true }); const db = testDb(); await seedDemo(db, cfg); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const login = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ email: DEMO_USERS[0].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), redirect: 'manual' });
    const cookie = login.headers.get('set-cookie').split(';')[0]; const get = (p) => fetch(base + p, { headers: { cookie } }); const csrf = db.get('SELECT csrf FROM sessions').csrf;
    // a browser's post: it accepts HTML and says where it came from, so a refusal comes back as the bench with the reason
    const post = (p, body) => fetch(base + p, { method: 'POST', headers: { cookie, accept: 'text/html', referer: base + (body._back || '/pack') }, body: new URLSearchParams({ _csrf: csrf, ...body }), redirect: 'manual' });
    const o = db.get("SELECT o.id, o.number FROM orders o WHERE o.status = 'paid' AND (SELECT COUNT(*) FROM order_items i WHERE i.order_id = o.id) = 1 AND EXISTS (SELECT 1 FROM order_items i WHERE i.order_id = o.id AND i.sku = 'wap-kit' AND i.qty = 2) ORDER BY o.id LIMIT 1");
    assert.ok(o, 'the demo has a paid order of two WAP kits');
    const html = await (await get(`/pack?order=${o.id}`)).text();
    assert.ok(html.includes('class="boxcard"') && /<div class="oz">\d+(\.\d+)? <small>oz<\/small>/.test(html), 'the box card with the expected weight');
    assert.equal((html.match(/name="ticked\[\]"/g) || []).length, db.all('SELECT 1 FROM order_items WHERE order_id = ?', o.id).length, 'one tick per line');
    assert.ok(html.includes('name="bench" value="1"') && html.includes('0 of 2 scanned') && /class="need" disabled hidden >/.test(html), 'boards wanted, none scanned, the gate closed');
    assert.ok((await (await get(`/orders/${o.id}`)).text()).includes(`href="/pack?order=${o.id}">Pack it at the bench`));
    // posted without the ticks: refused, back to the bench with the reason; nothing moved
    let r = await post(`/orders/${o.id}/transition`, { to: 'packed', bench: '1', _back: `/pack?order=${o.id}` });
    assert.equal(r.status, 303); assert.match(decodeURIComponent(r.headers.get('location')), /not every line is ticked/); assert.equal(db.get('SELECT status FROM orders WHERE id = ?', o.id).status, 'paid');
    // a board for the wrong kit is refused at the scan; the right one goes in and the bench says so
    const vision = db.get("SELECT serial FROM devices WHERE product_sku = 'vision-kit' AND order_id IS NULL LIMIT 1"); const [wap, wap2] = db.all("SELECT serial FROM devices WHERE product_sku = 'wap-kit' AND order_id IS NULL ORDER BY id LIMIT 2");
    r = await post(`/orders/${o.id}/assign`, { serial: vision.serial, _back: `/pack?order=${o.id}` }); assert.match(decodeURIComponent(r.headers.get('location')), /is flashed for vision-kit/);
    r = await post(`/orders/${o.id}/assign`, { serial: wap.serial, _back: `/pack?order=${o.id}` }); assert.match(decodeURIComponent(r.headers.get('location')), new RegExp(`${wap.serial} is in ${o.number}`));
    assert.ok((await (await get(`/pack?order=${o.id}`)).text()).includes('1 of 2 scanned'));
    r = await post(`/orders/${o.id}/assign`, { serial: wap2.serial, _back: `/pack?order=${o.id}` });
    assert.ok((await (await get(`/pack?order=${o.id}`)).text()).includes('2 of 2 scanned ✓'));
    // ticked, scanned, weighed: packed, and the bench moves on
    const body = new URLSearchParams({ _csrf: csrf, to: 'packed', bench: '1', measured_oz: '14.2', _back: '/pack' }); body.append('ticked[]', 'wap-kit');
    r = await fetch(base + `/orders/${o.id}/transition`, { method: 'POST', headers: { cookie }, body, redirect: 'manual' });
    assert.match(r.headers.get('location'), /^\/pack\?ok=/); assert.equal(db.get('SELECT status FROM orders WHERE id = ?', o.id).status, 'packed');
    assert.match(db.get("SELECT text FROM order_events WHERE order_id = ? AND kind = 'packed'", o.id).text, /every line ticked · boards .* · scale 14\.2 oz vs 14 oz ✓/);
    assert.ok((await (await get(`/pack?order=${o.id}`)).text()).includes('is <b>packed</b> — nothing to pack'));
    // a board can be taken back out of an unshipped box from the order page
    r = await post(`/orders/${o.id}/unassign`, { serial: wap.serial }); assert.match(decodeURIComponent(r.headers.get('location')), /is back out of the box/);
    assert.equal(db.get('SELECT order_id FROM devices WHERE serial = ?', wap.serial).order_id, null);
  } finally { await app.close(); db.close(); }
});

test('the phone is the scanner: the slip\'s code opens the order where it stands, a board\'s code offers one tap into the open bench order, the manifest ships the batch', async () => {
  const cfg = testCfg({ demo: true }); const db = testDb(); await seedDemo(db, cfg); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const login = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ email: DEMO_USERS[0].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), redirect: 'manual' });
    let cookie = login.headers.get('set-cookie').split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const get = (p) => fetch(base + p, { headers: { cookie }, redirect: 'manual' });
    const paid = db.get("SELECT o.id, o.number FROM orders o WHERE o.status = 'paid' AND EXISTS (SELECT 1 FROM order_items i WHERE i.order_id = o.id AND i.sku = 'wap-kit') ORDER BY o.id LIMIT 1");
    const packed = db.get("SELECT id, number FROM orders WHERE status = 'packed' ORDER BY id LIMIT 1"); const shipped = db.get("SELECT id, number FROM orders WHERE status = 'shipped' ORDER BY id LIMIT 1");
    // the slip's code: by status, to the pack bench, the ship bench, or the order
    assert.equal((await get(`/q/o/${paid.number}`)).headers.get('location'), `/pack?order=${paid.id}`);
    assert.equal((await get(`/q/o/${packed.number}`)).headers.get('location'), `/ship?order=${packed.id}`);
    assert.equal((await get(`/q/o/${shipped.number}`)).headers.get('location'), `/orders/${shipped.id}`);
    assert.equal((await get('/q/o/EL-999999')).status, 404);
    const slipHtml = await (await get(`/orders/${paid.id}/packing-slip`)).text(); assert.ok(slipHtml.includes(`aria-label="Order ${paid.number}"`) && slipHtml.includes('<svg'), 'the slip carries its code');
    // a board's code with no bench open: the board, and what to do
    const wap = db.get("SELECT serial FROM devices WHERE product_sku = 'wap-kit' AND order_id IS NULL ORDER BY id LIMIT 1"); const vision = db.get("SELECT serial FROM devices WHERE product_sku = 'vision-kit' AND order_id IS NULL LIMIT 1");
    let html = await (await get(`/q/b/${wap.serial}`)).text(); assert.ok(html.includes('No order is open on the pack bench') && !html.includes('/assign"'));
    assert.ok((await (await get('/q/b/NOPE-1')).text()).includes('No board with that serial is registered'));
    // opening the bench on an order sets the bench cookie; now the board's code offers one tap, the wrong kit's board says why not
    const bench = await get(`/pack?order=${paid.id}`); const bc = (bench.headers.get('set-cookie') || '').split(';')[0]; assert.equal(bc, `bo_bench=${paid.id}`); cookie = `${cookie}; ${bc}`;
    html = await (await get(`/q/b/${wap.serial}`)).text(); assert.ok(html.includes(`action="/orders/${paid.id}/assign"`) && html.includes(`Put ${wap.serial} into ${paid.number}`), 'one tap into the open order');
    html = await (await get(`/q/b/${vision.serial}`)).text(); assert.ok(html.includes('Not this one') && html.includes('is flashed for vision-kit') && !html.includes('/assign"'));
    const r = await fetch(base + `/orders/${paid.id}/assign`, { method: 'POST', headers: { cookie }, body: new URLSearchParams({ _csrf: csrf, serial: wap.serial, _back: `/pack?order=${paid.id}` }), redirect: 'manual' });
    assert.match(r.headers.get('location'), new RegExp(`^/pack\\?order=${paid.id}&ok=`)); assert.equal(db.get('SELECT order_id FROM devices WHERE serial = ?', wap.serial).order_id, paid.id);
    html = await (await get(`/q/b/${wap.serial}`)).text(); assert.ok(html.includes(`is already in ${paid.number}`));
    // the label sheet: a code per board
    const sheet = await (await get('/devices/labels?status=stock')).text(); assert.equal((sheet.match(/class="label"/g) || []).length, db.get("SELECT COUNT(*) n FROM devices WHERE status = 'stock'").n); assert.ok(sheet.includes(`<b>${wap.serial}</b>`) && sheet.includes(qrSvg(`${cfg.baseUrl}/q/b/${wap.serial}`, { size: 96, label: wap.serial })), 'each label is the serial and the code that opens it');
    // the manifest: the packed order ships with its tracking and rate, the rest is reported by name
    const manifest = [{ order_id: packed.number, tracking: '9400 1000 0000 0000 0000 11', rate: 4.8, status: 'labeled' }, { order_id: shipped.number, tracking: '9400 1000 0000 0000 0000 22', status: 'labeled' }, { order_id: paid.number, tracking: '9400 1000 0000 0000 0000 33', status: 'labeled' }, { order_id: 'EL-999999', tracking: '9400 1', status: 'labeled' }, { order_id: packed.number, status: 'blocked', tracking: null }];
    const imp = importLabels(db, manifest, { cfg });
    assert.deepEqual(imp.shipped, [`${packed.number}: 9400100000000000000011`]);
    assert.equal(imp.problems.length, 3); assert.match(imp.problems.join('\n'), /already shipped with/); assert.match(imp.problems.join('\n'), /is paid, not packed — pack it first/); assert.match(imp.problems.join('\n'), /EL-999999: no such order here/);
    assert.deepEqual(imp.skipped, [`${packed.number}: no label (blocked)`]);
    const s = orderById(db, packed.id); assert.equal(s.status, 'shipped'); assert.equal(s.tracking, '9400100000000000000011'); assert.equal(s.postage_cents, 480);
    assert.deepEqual(importLabels(db, manifest.slice(0, 1), { cfg }).skipped, [`${packed.number}: already shipped with that tracking`], 'the same manifest twice changes nothing');
    assert.throws(() => importLabels(db, { nope: 1 }), /manifest.json is a list of orders/);
    const posted = await fetch(base + '/orders/import-labels', { method: 'POST', headers: { cookie }, body: new URLSearchParams({ _csrf: csrf, json: JSON.stringify(manifest) }) });
    assert.ok((await posted.text()).includes('<b>0 shipped</b>'), 'the page reports the count');
  } finally { await app.close(); db.close(); }
});

test('production edges: the session rides a Lax cookie so a scanned link opens logged in; phones get no autofocus; a double tap is not an error; a viewer can look, not pack; logout drops the bench; the doctor names a default base URL and missing box data', async () => {
  const cfg = testCfg({ demo: true }); const db = testDb(); await seedDemo(db, cfg); const app = createApp({ db, cfg }); const addr = await app.listen(); const base = `http://127.0.0.1:${addr.port}`;
  try {
    const login = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ email: DEMO_USERS[0].email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), redirect: 'manual' });
    const set = login.headers.get('set-cookie'); assert.match(set, /SameSite=Lax/); assert.match(set, /HttpOnly/); assert.ok(!/SameSite=Strict/.test(set));
    const cookie = set.split(';')[0]; const csrf = db.get('SELECT csrf FROM sessions').csrf;
    const get = (p, h = {}) => fetch(base + p, { headers: { cookie, ...h }, redirect: 'manual' });
    const paid = db.get("SELECT id, number FROM orders WHERE status = 'paid' AND EXISTS (SELECT 1 FROM order_items i WHERE i.order_id = orders.id AND i.sku = 'wap-kit') ORDER BY id LIMIT 1");
    const desk = await (await get(`/pack?order=${paid.id}`)).text(); assert.ok(desk.includes('id="f_serial" name="serial" autofocus'), 'a laptop focuses the scan field');
    const phone = await (await get(`/pack?order=${paid.id}`, { 'user-agent': 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148 Safari/604.1' })).text();
    assert.ok(phone.includes('id="f_serial" name="serial"  autocomplete') && !phone.includes('autofocus'), 'a phone keeps its keyboard down');
    const packed = db.get("SELECT id, number FROM orders WHERE status = 'packed' ORDER BY id LIMIT 1");
    const again = await fetch(base + `/orders/${packed.id}/transition`, { method: 'POST', headers: { cookie }, body: new URLSearchParams({ _csrf: csrf, to: 'packed', bench: '1', _back: '/pack' }), redirect: 'manual' });
    assert.match(decodeURIComponent(again.headers.get('location')), new RegExp(`^/pack\\?ok=${packed.number} is already packed`)); assert.equal(db.get('SELECT status FROM orders WHERE id = ?', packed.id).status, 'packed');
    // the viewer: the scan page shows the board and the open order, with no button
    const viewer = DEMO_USERS.find(u => u.role === 'viewer'); assert.ok(viewer, 'the demo has a viewer');
    const vl = await fetch(base + '/login', { method: 'POST', body: new URLSearchParams({ email: viewer.email, password: DEMO_PASSWORD, code: totp(DEMO_TOTP_SECRET) }), redirect: 'manual' }); const vcookie = vl.headers.get('set-cookie').split(';')[0];
    const vb = await fetch(base + `/pack?order=${paid.id}`, { headers: { cookie: vcookie } }); const vbench = (vb.headers.get('set-cookie') || '').split(';')[0];
    const wap = db.get("SELECT serial FROM devices WHERE product_sku = 'wap-kit' AND order_id IS NULL ORDER BY id LIMIT 1");
    const vhtml = await (await fetch(base + `/q/b/${wap.serial}`, { headers: { cookie: `${vcookie}; ${vbench}` } })).text();
    assert.ok(vhtml.includes('can look, not pack') && !vhtml.includes('/assign"'));
    // logout drops both cookies
    const out = await fetch(base + '/logout', { method: 'POST', headers: { cookie: `${cookie}; bo_bench=${paid.id}` }, body: new URLSearchParams({ _csrf: csrf }), redirect: 'manual' });
    const dropped = out.headers.getSetCookie ? out.headers.getSetCookie() : [out.headers.get('set-cookie')]; assert.ok(dropped.some(x => x.startsWith('bo_bench=') && /Max-Age=0/.test(x)) && dropped.some(x => x.startsWith('bo_session=') && /Max-Age=0/.test(x)));
    // the devices list links each board's label
    const devs = await (await fetch(base + '/devices', { headers: { cookie: vcookie } })).text(); assert.ok(devs.includes(`/devices/labels?serials=${wap.serial}`));
    // the manifest import leaves an audit line
    importLabels(db, [{ order_id: 'EL-999999', tracking: '9400 1', status: 'labeled' }], { cfg });
    assert.deepEqual(JSON.parse(db.get("SELECT detail FROM audit_log WHERE action = 'orders.import_labels' ORDER BY id DESC LIMIT 1").detail).problems, ['EL-999999: no such order here']);
  } finally { await app.close(); db.close(); }
  // the doctor, outside the demo: the default base URL and the unloaded box data are named
  const { doctor } = await import('../src/modules/health.mjs'); const { loadShipping } = await import('../src/modules/catalog.mjs');
  const prod = testCfg({ demo: false, baseUrl: 'http://127.0.0.1:8787' }); const pdb = testDb();
  let by = Object.fromEntries(doctor(pdb, prod).checks.map(c => [c.key, c]));
  assert.equal(by.base_url.ok, false); assert.match(by.base_url.detail, /127\.0\.0\.1:8787 — a phone scanning a slip lands nowhere/);
  assert.equal(by.shipping_data.ok, null); assert.match(by.shipping_data.detail, /not asked for yet/);
  await loadShipping({ ...prod, shippingJson: '', storeJson: '/nonexistent/store.json' }).catch(() => {});
  by = Object.fromEntries(doctor(pdb, prod).checks.map(c => [c.key, c])); assert.equal(by.shipping_data.ok, false, by.shipping_data.detail); assert.match(by.shipping_data.detail, /ENOENT|no such file/);
  await loadShipping({ ...prod, shippingJson: '', storeJson: 'https://example.test/catalog' }).catch(() => {});
  by = Object.fromEntries(doctor(pdb, prod).checks.map(c => [c.key, c])); assert.match(by.shipping_data.detail, /no shipping.json source/, 'a catalog URL with no shipping.json beside it says so');
  await loadShipping({ ...prod, shippingJson: fixture('shipping.json') }, { force: true });
  by = Object.fromEntries(doctor(pdb, { ...prod, baseUrl: 'https://ops.example.test' }).checks.map(c => [c.key, c])); assert.equal(by.shipping_data.ok, true); assert.match(by.shipping_data.detail, /\d+ boxes, \d+ parcels/); assert.equal(by.base_url.ok, true);
  pdb.close();
});
