'use strict';

// The recovery kit on the dashboard (sweep F257).
//
// A WAP set up through the Bluetooth setup door never runs the /companion
// wizard's close-out, which was the one place canary-recovery-kit.json was
// offered; the dashboard's BOOT-tap sign-in took the receipt for its token
// and offered nothing, so the iPhone's recovery path (PairView) had no kit to
// pair from. These tests run the pages' own code, cut out of web_ui.h and
// companion_pwa.h as the firmware ships it (the same pattern as
// wizard_logic.test.js beside this file):
//
//   * both pages carry the RECOVERY_KIT builder, line for line;
//   * the dashboard's BOOT-tap success offers the kit in the wizard's words,
//     its Save button showing, again after a second sign-in in the same page;
//     Save hands the browser the receipt text exactly as served, every field
//     identity_json.h writes, as canary-recovery-kit.json through a blob: URL.
//     The Save itself adds no request (the dashboard's own refreshes go on
//     around it), and no request carries the kit's text;
//   * a tap that never served a receipt (403 every poll, an answer that is
//     not a receipt) offers nothing;
//   * Settings → Device → Recovery kit saves it again for a page that is
//     already signed in: one GET, the same builder, the token as a Bearer
//     header when the page holds one and no header on a cookie session (the
//     device refuses that one since sweep F289 — the receipt's bearer grant
//     takes the header alone — which is the refused case below); a refused
//     session brings the sign-in back and saves nothing;
//   * the wizard's close-out saves through the same builder, and its saved
//     line names that Settings path, which exists.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const UI = path.join(__dirname, 'web_ui.h');
const PWA = path.join(__dirname, 'companion_pwa.h');
const IDJSON = path.join(__dirname, 'identity_json.h');

const read = (p) => fs.readFileSync(p, 'utf8');

function kitBlock(src, name) {
  const m = src.match(/[ \t]*\/\*\s*RECOVERY_KIT:BEGIN[\s\S]*?RECOVERY_KIT:END\s*\*\//);
  if (!m) throw new Error('RECOVERY_KIT block not found in ' + name);
  return m[0];
}

function loadKit(src, name) {
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(kitBlock(src, name), sandbox, { filename: name + '.recovery_kit.js' });
  const K = sandbox.module.exports;
  if (!K || typeof K.save !== 'function') throw new Error('the RECOVERY_KIT block exported no save()');
  return K;
}

// One function of web_ui.h's script, as it ships (4-space indent).
function uiFunction(src, name, isAsync) {
  const head = '\n    ' + (isAsync ? 'async ' : '') + 'function ' + name + '(';
  const at = src.indexOf(head);
  if (at < 0) throw new Error(name + ' not found in web_ui.h');
  const end = src.indexOf('\n    }\n', at);
  return src.slice(at + 1, end + 7);
}

// The receipt's keys in the order identity_json.h's write_receipt() writes them.
function receiptKeys() {
  const src = read(IDJSON);
  const body = src.split('inline void write_receipt(wap_json::Writer& w, const Receipt& in) {')[1].split('\n}\n')[0];
  return [...body.matchAll(/\\"(\w+)\\": /g)].map((m) => m[1]);
}

// A receipt in the bytes the builder writes, for a TLS device read over the
// home network (base_url at the .local name since sweep F255): the repo test
// key's names (device id and SSID suffix 4dC2, fingerprint 7916ca48..., so
// mDNS host canary-7916), which scripts/tests/test_wap_name_examples.py holds
// every source to.
const FIELDS = {
  device_id: 'canary-s3-4dC2',
  base_url: 'https://canary-7916.local',
  token: 'cv_' + 'ab12'.repeat(8),
  pubkey_fp: '7916ca487912fa1b',
  firmware: '2.4.15',
  hw_token: '0123456789abcdef0123456789abcdef',
  ap_ssid: 'SecuraCV-4dC2',
  ap_password: 'quiet-owl-lamp-42',
  tls_cert_fp: 'cdfcca58db62ca948e8741d19e28ce1c79b4a818755559b419b1dd17c439dcfa',
  provisioned_at: 'boot:7',
};
const RECEIPT = '{\n' + Object.entries(FIELDS).map(([k, v]) => '  "' + k + '": ' + JSON.stringify(v)).join(',\n') + '\n}';

// Each element's starting display, read from its markup: 'none' where its
// style attribute hides it, '' (shown) otherwise. An id the markup does not
// carry starts hidden.
function startingDisplay(src, id) {
  const m = src.match(new RegExp('<[a-z0-9]+\\b[^>]*\\bid="' + id + '"[^>]*>'));
  if (!m) return 'none';
  const style = (m[0].match(/\bstyle="([^"]*)"/) || [])[1] || '';
  return /display\s*:\s*none/.test(style) ? 'none' : '';
}

// A browser window just big enough for the pages' kit code: elements by id
// (starting as `markup` draws them), a body, Blob, URL and timers, with every
// save recorded.
function fakeWindow(ids, markup) {
  const els = {};
  for (const id of ids) {
    const display = markup ? startingDisplay(markup, id) : 'none';
    els[id] = { id, style: { display }, textContent: '', className: '',
                classList: { hidden: false, add(c) { if (c === 'hidden') this.hidden = true; },
                             remove(c) { if (c === 'hidden') this.hidden = false; } } };
  }
  const saved = [];
  const appended = [];
  const timers = [];
  const win = {
    els, saved, appended, timers,
    document: {
      getElementById: (id) => els[id] || null,
      createElement: (tag) => {
        const a = { tag, href: '', download: '', clicked: 0, click() { this.clicked++; saved.push(this); } };
        return a;
      },
      body: {
        appendChild: (n) => { appended.push(n); },
        removeChild: (n) => { const i = appended.indexOf(n); if (i >= 0) appended.splice(i, 1); },
      },
    },
    Blob: class { constructor(parts, opts) { this.parts = parts; this.type = opts && opts.type; } },
    URL: {
      made: [], revoked: [],
      createObjectURL(b) { const u = 'blob:wap/' + this.made.length; this.made.push({ url: u, blob: b }); return u; },
      revokeObjectURL(u) { this.revoked.push(u); },
    },
    setTimeout: (fn, ms) => { timers.push(ms); fn(); },
  };
  return win;
}

const DASH_IDS = ['authStatus', 'authOverlay', 'recoveryKitCard', 'recoveryKitDone', 'recoveryKitSave',
                  'recoveryKitLater', 'settingsKitDone', 'settingsKitErr'];

// The dashboard exactly as web_ui.h ships its sign-in and kit code: the
// RECOVERY_KIT block, secureFetch, the auth modal, requestFromDevice, the
// three offer functions and the Settings save, run against a device that
// answers the receipt route from `answers` (the last one repeats; `more()`
// queues the next sign-in's answers) and every other route 200. Its
// startDashboard() makes the dashboard's own first requests through the same
// fetch, so a test that says "the Save added no request" means the Save.
function dashboard(answers) {
  const src = read(UI);
  const code = [
    kitBlock(src, 'web_ui.h'),
    src.match(/\n {4}let apiToken = null;\n/)[0],
    uiFunction(src, 'secureFetch', true),
    uiFunction(src, 'showAuthModal'),
    uiFunction(src, 'hideAuthModal'),
    src.match(/\n {4}let recoveryKitText = null;\n/)[0],
    uiFunction(src, 'offerRecoveryKit'),
    uiFunction(src, 'closeRecoveryKit'),
    uiFunction(src, 'saveRecoveryKit'),
    uiFunction(src, 'saveRecoveryKitFromSettings', true),
    uiFunction(src, 'requestFromDevice', true),
    ';({ requestFromDevice, saveRecoveryKit, closeRecoveryKit, saveRecoveryKitFromSettings,',
    '    kitText: () => recoveryKitText, token: () => apiToken, setToken: (t) => { apiToken = t; } })',
  ].join('\n');
  const win = fakeWindow(DASH_IDS, src);
  const asked = [];
  let queue = answers.slice();
  let polls = 0;
  const calls = { start: 0 };
  const ctx = {
    API_BASE: '',
    document: win.document,
    Blob: win.Blob,
    URL: win.URL,
    JSON, Number, Error, Promise,
    // Every timer runs at once: the 2 s sleep between polls, and the blob
    // URL's revoke (its delay recorded).
    setTimeout: (fn, ms) => { win.timers.push(ms); fn(); },
    fetch: async (url, opts) => {
      asked.push({ url, opts });
      if (url !== '/api/provisioning-receipt') {
        return { status: 200, ok: true, json: async () => ({}), text: async () => '{}' };
      }
      const a = queue[Math.min(polls++, queue.length - 1)];
      return { status: a.status, ok: a.status >= 200 && a.status < 300,
               json: async () => JSON.parse(a.body), text: async () => a.body };
    },
    // The dashboard's own first requests (refreshAll's /api/status and the
    // OTA banner), made through the same fetch.
    startDashboard: () => { calls.start++; ctx.fetch('/api/status'); ctx.fetch('/api/ota/status'); },
  };
  ctx.window = ctx;   // the page's global object is its window
  const api = vm.runInNewContext(code, ctx, { filename: 'web_ui.recovery_kit.js' });
  const more = (next) => { queue = next.slice(); polls = 0; };
  return { api, win, asked, calls, ctx, more };
}

async function dashboardTap(answers) {
  const d = dashboard(answers);
  await d.api.requestFromDevice();
  return d;
}

// Whether any request carried `text` (in its URL, a header or a body).
function carried(asked, text) {
  return asked.some((q) => JSON.stringify(q).includes(text));
}

const GATE_403 = { status: 403, body: '{"error":"physical_confirmation_required","gate_ttl_seconds":30}' };

describe('one builder, two pages', () => {
  it('web_ui.h and companion_pwa.h carry the same RECOVERY_KIT lines', () => {
    const lines = (s) => s.split('\n').map((l) => l.trim());
    assert.deepEqual(lines(kitBlock(read(UI), 'web_ui.h')), lines(kitBlock(read(PWA), 'companion_pwa.h')));
  });
  it('the builder makes no request and touches no storage', () => {
    // The code, its comments left out (they say what it does not do).
    const block = kitBlock(read(UI), 'web_ui.h').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
    for (const word of ['fetch', 'XMLHttpRequest', 'sendBeacon', 'WebSocket', 'EventSource', 'localStorage',
                        'sessionStorage', 'indexedDB', 'document.cookie']) {
      assert.ok(!block.includes(word), 'RECOVERY_KIT names ' + word);
    }
  });
});

describe('RecoveryKit.isReceipt / save', () => {
  const K = loadKit(read(UI), 'web_ui.h');
  it('takes the receipt the device serves', () => {
    assert.equal(K.isReceipt(RECEIPT), true);
    assert.equal(K.FILE_NAME, 'canary-recovery-kit.json');
  });
  it('refuses what PairView could not pair from', () => {
    for (const bad of [undefined, null, '', 42, '{', '[]', 'null', '"x"', '{}',
                       '{"token":"cv_x"}', '{"base_url":"http://canary-7916.local"}',
                       '{"token":"","base_url":"http://a.local"}', '{"token":"cv_x","base_url":""}',
                       '{"token":7,"base_url":"http://a.local"}',
                       '{"error":"physical_confirmation_required"}']) {
      assert.equal(K.isReceipt(bad), false, JSON.stringify(bad));
    }
  });
  it('saves the text exactly, as canary-recovery-kit.json, through a blob: URL it then revokes', () => {
    const win = fakeWindow([]);
    assert.equal(K.save(win, RECEIPT), true);
    assert.equal(win.saved.length, 1);
    const a = win.saved[0];
    assert.equal(a.download, 'canary-recovery-kit.json');
    assert.equal(a.href, 'blob:wap/0');
    assert.equal(a.clicked, 1);
    assert.deepEqual([...win.URL.made[0].blob.parts], [RECEIPT]);
    assert.equal(win.URL.made[0].blob.type, 'application/json');
    assert.deepEqual(win.appended, [], 'the link is removed again');
    assert.deepEqual(win.URL.revoked, ['blob:wap/0']);
    // Revoked later, not at once: a blob: URL revoked as the click returns
    // can cancel the download it started.
    assert.deepEqual(win.timers, [4000]);
  });
  it('saves nothing that is not a receipt', () => {
    const win = fakeWindow([]);
    assert.equal(K.save(win, '{"error":"x"}'), false);
    assert.equal(win.saved.length, 0);
    assert.equal(win.URL.made.length, 0);
  });
});

describe("the dashboard's BOOT-tap sign-in offers the kit (sweep F257)", () => {
  it('the fixture is the receipt identity_json.h writes, field for field', () => {
    assert.deepEqual(Object.keys(FIELDS), receiptKeys());
  });

  it('the markup draws the card hidden, and its Save and Later showing', () => {
    const win = fakeWindow(DASH_IDS, read(UI));
    assert.equal(win.els.recoveryKitCard.style.display, 'none');
    assert.equal(win.els.recoveryKitDone.style.display, 'none');
    assert.equal(win.els.recoveryKitSave.style.display, '');
    assert.equal(win.els.recoveryKitLater.style.display, '');
  });

  it('a tap that served a receipt signs in and offers the kit, its Save button showing', async () => {
    const { api, win, asked, calls } = await dashboardTap([GATE_403, { status: 200, body: RECEIPT }]);
    assert.equal(win.els.authOverlay.classList.hidden, true, 'the sign-in is put away');
    assert.equal(calls.start, 1);
    assert.equal(api.token(), FIELDS.token, 'the dashboard still signs in with the receipt token');
    assert.equal(win.els.recoveryKitCard.style.display, 'block', 'the offer is on screen');
    assert.notEqual(win.els.recoveryKitSave.style.display, 'none', 'with a Save button to press');
    assert.notEqual(win.els.recoveryKitLater.style.display, 'none');
    assert.equal(win.els.recoveryKitDone.style.display, 'none');
    assert.equal(win.els.recoveryKitLater.textContent, 'Do this later');
    assert.equal(api.kitText(), RECEIPT, 'held for the save, as served');
    assert.equal(win.saved.length, 0, 'nothing is saved before the person asks');
    assert.deepEqual(asked.filter((q) => q.url === '/api/provisioning-receipt').length, 2);
  });

  it('Save hands the browser the receipt as canary-recovery-kit.json, and adds no request', async () => {
    const { api, win, asked } = await dashboardTap([{ status: 200, body: RECEIPT }]);
    // The dashboard started and made its own requests: the Save is judged
    // against them, not against a page that makes none.
    assert.ok(asked.some((q) => q.url === '/api/status'), 'the dashboard\'s own refresh ran');
    const before = asked.length;
    api.saveRecoveryKit();
    assert.equal(asked.length, before, 'the Save adds no request');
    assert.ok(!carried(asked, FIELDS.ap_password), 'no request carries the kit\'s text');
    for (const q of asked) {
      assert.ok(!q.opts || ((!q.opts.method || q.opts.method === 'GET') && q.opts.body === undefined),
        'no request writes anything: ' + q.url);
    }
    assert.equal(win.saved.length, 1);
    const a = win.saved[0];
    assert.equal(a.download, 'canary-recovery-kit.json');
    assert.match(a.href, /^blob:/, 'a same-page blob: URL, not a network address');
    const blob = win.URL.made.find((m) => m.url === a.href).blob;
    assert.deepEqual([...blob.parts], [RECEIPT], 'the receipt text exactly as served');
    // The kit's fields: every one the receipt carries, with its value.
    const kit = JSON.parse(blob.parts.join(''));
    assert.deepEqual(Object.keys(kit), receiptKeys());
    assert.deepEqual(kit, FIELDS);
    assert.equal(kit.base_url, 'https://canary-7916.local');
    // The card says it is saved, offers no second save, and the text is dropped.
    assert.equal(win.els.recoveryKitDone.style.display, 'block');
    assert.equal(win.els.recoveryKitSave.style.display, 'none');
    assert.equal(win.els.recoveryKitLater.textContent, 'Continue');
    assert.equal(api.kitText(), null);
    api.saveRecoveryKit();
    assert.equal(win.saved.length, 1, 'a second Save saves nothing');
  });

  it('a second sign-in in the same page offers the kit afresh', async () => {
    // Saved once; then the session is lost (a 401/403 brings secureFetch's
    // sign-in back) and BOOT is tapped again: the offer comes back whole,
    // Save showing, the saved line hidden, Later saying "Do this later".
    const d = await dashboardTap([{ status: 200, body: RECEIPT }]);
    d.api.saveRecoveryKit();
    assert.equal(d.win.els.recoveryKitSave.style.display, 'none');
    d.more([{ status: 403, body: '{"error":"physical_confirmation_required"}' }]);
    await d.api.saveRecoveryKitFromSettings();   // the session is gone: refused
    assert.equal(d.win.els.authOverlay.classList.hidden, false, 'the sign-in is back');
    assert.equal(d.win.saved.length, 1, 'the refused fetch saved nothing');
    const again = RECEIPT.replace('"boot:7"', '"boot:8"');
    assert.notEqual(again, RECEIPT);
    d.more([GATE_403, { status: 200, body: again }]);
    await d.api.requestFromDevice();
    assert.equal(d.win.els.recoveryKitCard.style.display, 'block');
    assert.notEqual(d.win.els.recoveryKitSave.style.display, 'none', 'Save shows again');
    assert.equal(d.win.els.recoveryKitDone.style.display, 'none', 'the old saved line is gone');
    assert.equal(d.win.els.recoveryKitLater.textContent, 'Do this later');
    assert.equal(d.api.kitText(), again, 'the new receipt, held for the save');
    d.api.saveRecoveryKit();
    assert.equal(d.win.saved.length, 2);
    assert.deepEqual([...d.win.URL.made[1].blob.parts], [again]);
  });

  it('Do this later closes the offer and drops the text, saving nothing', async () => {
    const { api, win } = await dashboardTap([{ status: 200, body: RECEIPT }]);
    api.closeRecoveryKit();
    assert.equal(win.els.recoveryKitCard.style.display, 'none');
    assert.equal(api.kitText(), null);
    api.saveRecoveryKit();
    assert.equal(win.saved.length, 0);
  });

  it('a tap that never came (403 on every poll) offers nothing', async () => {
    const { api, win, calls } = await dashboardTap([GATE_403]);
    assert.equal(calls.start, 0);
    assert.equal(win.els.recoveryKitCard.style.display, 'none');
    assert.equal(api.kitText(), null);
    assert.equal(win.saved.length, 0);
    assert.match(win.els.authStatus.textContent, /Didn't see a tap/);
  });

  it('an answer that is not a receipt offers nothing', async () => {
    for (const body of ['{"ok":true}', '{"token":"cv_x"}']) {
      const { api, win } = await dashboardTap([{ status: 200, body }]);
      assert.equal(win.els.recoveryKitCard.style.display, 'none', body);
      assert.equal(api.kitText(), null, body);
    }
    // Unparseable: the poll goes on (as it always did) and the tap is never offered.
    const { api, win } = await dashboardTap([{ status: 200, body: 'not json' }, GATE_403]);
    assert.equal(win.els.recoveryKitCard.style.display, 'none');
    assert.equal(api.kitText(), null);
  });
});

describe('Settings → Device → Recovery kit saves it again for a signed-in page (sweep F257)', () => {
  it('on a cookie session: one GET with no token (what the page holds); a receipt served is saved as served', async () => {
    // A resumed session: no token in the page, so the request carries none.
    // Since sweep F289 the device answers this request 403 (the case two
    // below); this holds the page's side, whatever a device answers.
    const d = dashboard([{ status: 200, body: RECEIPT }]);
    await d.api.saveRecoveryKitFromSettings();
    assert.deepEqual(d.asked.map((q) => q.url), ['/api/provisioning-receipt']);
    const q = d.asked[0];
    assert.equal(q.opts.cache, 'no-store');
    assert.ok(!q.opts.method || q.opts.method === 'GET');
    assert.equal(q.opts.body, undefined);
    assert.equal(q.opts.headers.Authorization, undefined, 'the cookie carries the session, not the page');
    assert.equal(d.win.saved.length, 1);
    assert.equal(d.win.saved[0].download, 'canary-recovery-kit.json');
    assert.deepEqual([...d.win.URL.made[0].blob.parts], [RECEIPT]);
    assert.equal(d.win.els.settingsKitDone.style.display, 'block');
    assert.equal(d.win.els.settingsKitErr.style.display, 'none');
    assert.equal(d.win.els.recoveryKitCard.style.display, 'none', 'the BOOT-tap offer stays away');
  });

  it('on a token: the same GET carries it as a Bearer header', async () => {
    const d = dashboard([{ status: 200, body: RECEIPT }]);
    d.api.setToken(FIELDS.token);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.asked.length, 1);
    assert.equal(d.asked[0].opts.headers.Authorization, 'Bearer ' + FIELDS.token);
    assert.equal(d.win.saved.length, 1);
  });

  it('a refused session saves nothing and brings the sign-in back', async () => {
    const d = dashboard([GATE_403]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.asked.length, 1);
    assert.equal(d.win.saved.length, 0);
    assert.equal(d.win.URL.made.length, 0);
    assert.equal(d.win.els.authOverlay.classList.hidden, false, 'secureFetch shows the sign-in');
    assert.equal(d.win.els.settingsKitDone.style.display, 'none');
    assert.equal(d.win.els.settingsKitErr.style.display, 'block');
    assert.match(d.win.els.settingsKitErr.textContent, /Sign in again first/);
  });

  it('an answer that is not a receipt saves nothing and says so', async () => {
    const d = dashboard([{ status: 200, body: '{"ok":true}' }]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.win.saved.length, 0);
    assert.match(d.win.els.settingsKitErr.textContent, /Could not fetch the recovery kit: not a recovery kit/);
    assert.equal(d.win.els.settingsKitDone.style.display, 'none');
  });

  it('a second save after a failure clears the failure', async () => {
    const d = dashboard([{ status: 500, body: '{}' }]);
    await d.api.saveRecoveryKitFromSettings();
    assert.match(d.win.els.settingsKitErr.textContent, /HTTP 500/);
    d.more([{ status: 200, body: RECEIPT }]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.win.els.settingsKitErr.style.display, 'none');
    assert.equal(d.win.els.settingsKitDone.style.display, 'block');
    assert.equal(d.win.saved.length, 1);
  });

  it('the handler builds no download of its own', () => {
    const fn = uiFunction(read(UI), 'saveRecoveryKitFromSettings', true);
    assert.match(fn, /RecoveryKit\.save\(window, await resp\.text\(\)\)/);
    assert.match(fn, /secureFetch\('\/api\/provisioning-receipt', \{ cache: 'no-store' \}\)/);
    assert.doesNotMatch(fn, /new Blob|createObjectURL|\.download\s*=|method:/);
  });
});

describe("the offer speaks the wizard's words", () => {
  const ui = read(UI);
  const pwa = read(PWA);
  const inner = (src, re, what) => {
    const m = src.match(re);
    assert.ok(m, what + ' not found');
    return m[1];
  };
  const wizBlock = inner(pwa, /<div id="wiz-recovery-block"[^>]*>([\s\S]*?)\n {6}<\/div>\n/, 'the wizard block');
  const wizTitle = inner(wizBlock, /<h3[^>]*>([^<]*)<\/h3>/, 'the wizard title');
  const wizLead = inner(wizBlock, /<p class="wiz-sub" style="margin:0 0 \.6rem">([^]*?)<\/p>/, 'the wizard lead');
  const wizSave = inner(wizBlock, /id="wiz-recovery-save">([^<]*)</, 'the wizard Save');
  const wizLater = inner(wizBlock, /id="wiz-recovery-skip">([^<]*)</, 'the wizard Later');
  const wizDone = inner(wizBlock, /id="wiz-recovery-done"[^>]*>([^]*?)<\/p>/, 'the wizard saved line');
  const firstSentence = wizDone.slice(0, wizDone.indexOf('</strong>.') + '</strong>.'.length);

  it('title, lead, buttons and the saved line match the /companion close-out', () => {
    assert.equal(inner(ui, /id="recoveryKitTitle">([^<]*)</, 'the offer title'), wizTitle);
    assert.equal(inner(ui, /id="recoveryKitLead">([^]*?)<\/div>/, 'the offer lead'), wizLead);
    assert.equal(inner(ui, /id="recoveryKitSave"[^>]*>([^<]*)</, 'the offer Save'), wizSave);
    assert.equal(inner(ui, /id="recoveryKitLater"[^>]*>([^<]*)</, 'the offer Later'), wizLater);
    assert.equal(inner(ui, /id="recoveryKitDone"[^>]*>([^]*?)<\/p>/, 'the offer saved line'), wizDone);
    assert.equal(wizTitle, 'Save your recovery kit');
    assert.equal(wizSave, 'Save my recovery kit');
  });

  it('the Settings card says the same: the lead, the Save, the saved line\'s first sentence', () => {
    assert.equal(inner(ui, /id="settingsKitTitle">([^<]*)</, 'the Settings title'), 'Recovery kit');
    assert.equal(inner(ui, /id="settingsKitLead">([^]*?)<\/div>/, 'the Settings lead'), wizLead);
    assert.equal(inner(ui, /id="settingsKitSave"[^>]*>([^<]*)</, 'the Settings Save'), wizSave);
    assert.equal(inner(ui, /id="settingsKitDone"[^>]*>([^]*?)<\/p>/, 'the Settings saved line'), firstSentence);
    assert.match(firstSentence, /canary-recovery-kit\.json/);
  });

  it('the saved line names a Settings path the dashboard has', () => {
    // "You can save it again any time from the dashboard: Settings, Device,
    // Recovery kit." — each of those is on web_ui.h's page, in that order.
    assert.match(wizDone, /from the dashboard: Settings, Device, Recovery kit\.$/);
    assert.match(ui, /<button class="nav-btn" data-panel="settings">Settings<\/button>/);
    assert.match(ui, /onclick="switchSettingsTab\('device'\)">[^<]*Device<\/button>/);
    const device = ui.indexOf('<div class="sub-panel" id="settings-device">');
    const next = ui.indexOf('<div class="sub-panel"', device + 1);
    const card = ui.indexOf('id="settingsKitCard"');
    assert.ok(device > 0 && card > device && (next < 0 || card < next), 'the Recovery kit card sits in Settings → Device');
    assert.match(ui, /<div class="card" id="settingsKitCard">/, 'shown, not hidden behind a tap');
  });

  it('the cards are where they should be, each id appears once, and the buttons are wired', () => {
    assert.match(ui, /<div class="card" id="recoveryKitCard" style="display:none"/);
    for (const id of DASH_IDS.slice(2).concat(['recoveryKitTitle', 'recoveryKitLead', 'settingsKitCard',
                                               'settingsKitTitle', 'settingsKitLead', 'settingsKitSave'])) {
      assert.equal((ui.match(new RegExp('id="' + id + '"', 'g')) || []).length, 1, id);
    }
    assert.match(ui, /id="recoveryKitLater" onclick="closeRecoveryKit\(\)"/);
    assert.match(ui, /id="recoveryKitSave" onclick="saveRecoveryKit\(\)"/);
    assert.match(ui, /onclick="saveRecoveryKitFromSettings\(\)" id="settingsKitSave"/);
    // The sign-in offers it right after it starts the dashboard, with the text it read.
    assert.match(uiFunction(ui, 'requestFromDevice', true),
      /const text = await resp\.text\(\);\n\s*const receipt = JSON\.parse\(text\);\n\s*apiToken = receipt\.token;\n\s*hideAuthModal\(\);\n\s*startDashboard\(\);\n\s*offerRecoveryKit\(text\);\n\s*return;/);
  });
});

describe("the wizard's close-out saves through the same builder", () => {
  function wizardSave(answer) {
    const src = read(PWA);
    const m = src.match(/\$w\('wiz-recovery-save'\)\.addEventListener\('click', (async \(\) => \{[\s\S]*?\n {2}\})\);\n/);
    if (!m) throw new Error('the wiz-recovery-save handler was not found in companion_pwa.h');
    const ids = ['wiz-recovery-err', 'wiz-recovery-save', 'wiz-recovery-done', 'wiz-recovery-skip', 'wiz-ha-block'];
    const win = fakeWindow(ids);
    const asked = [];
    const ctx = {
      $w: (id) => win.els[id],
      fetch: async (url, opts) => { asked.push({ url, opts }); return { status: answer.status, ok: answer.status === 200, text: async () => answer.body }; },
      requestAnimationFrame: () => {}, focusActiveStepHeading: () => {},
      document: win.document, Blob: win.Blob, URL: win.URL, JSON, Error,
    };
    ctx.window = ctx;
    ctx.setTimeout = (fn, ms) => { win.timers.push(ms); fn(); };
    const code = kitBlock(src, 'companion_pwa.h') + '\n;(' + m[1] + ')';
    const handler = vm.runInNewContext(code, ctx, { filename: 'wizard.recovery_save.js' });
    return handler().then(() => ({ win, asked }));
  }
  it('a served receipt is saved by RecoveryKit.save, exactly as served', async () => {
    const { win, asked } = await wizardSave({ status: 200, body: RECEIPT });
    assert.deepEqual(asked.map((q) => q.url), ['/api/provisioning-receipt']);
    assert.equal(win.saved.length, 1);
    assert.equal(win.saved[0].download, 'canary-recovery-kit.json');
    assert.deepEqual([...win.URL.made[0].blob.parts], [RECEIPT]);
    assert.equal(win.els['wiz-recovery-done'].style.display, 'block');
    assert.equal(win.els['wiz-ha-block'].style.display, 'block');
  });
  it('a closed gate saves nothing and says to press BOOT', async () => {
    const { win } = await wizardSave(GATE_403);
    assert.equal(win.saved.length, 0);
    assert.match(win.els['wiz-recovery-err'].textContent, /Press the BOOT button/);
  });
  it('a 200 that is not a receipt saves nothing and says so', async () => {
    const { win } = await wizardSave({ status: 200, body: '{"ok":true}' });
    assert.equal(win.saved.length, 0);
    assert.match(win.els['wiz-recovery-err'].textContent, /Could not fetch the recovery kit: not a recovery kit/);
    assert.equal(win.els['wiz-ha-block'].style.display, 'none');
  });
  it('the handler builds no download of its own', () => {
    const src = read(PWA);
    const m = src.match(/\$w\('wiz-recovery-save'\)\.addEventListener\('click', (async \(\) => \{[\s\S]*?\n {2}\})\);\n/);
    assert.match(m[1], /RecoveryKit\.save\(window, await r\.text\(\)\)/);
    assert.doesNotMatch(m[1], /new Blob|createObjectURL|\.download\s*=/);
  });
});
