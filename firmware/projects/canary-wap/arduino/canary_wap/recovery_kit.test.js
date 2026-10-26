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
//     line names that Settings path, which exists;
//   * secureFetch's sign-in names what the page sent (sweep F289's
//     follow-up): a refused token is "Invalid or expired token", but a
//     refusal of a page that holds no token (the kit's 403 or the pairing
//     QR's 401 on a cookie session, or a session that has ended) says the
//     session does not cover it, never a token the page never had;
//   * the BOOT tap goes to one asker (sweep F342): a 409 tap_contended
//     stops the dashboard's sign-in on that answer and says another device
//     asked, keeping no token and offering no kit, and a second click then
//     asks first and signs in on the next tap; the Settings save
//     says the same instead of "HTTP 409"; both name pasting the token
//     first (the sign-in checks a pasted token by itself since the
//     owner-authority PR: dashboard_gates.test.js 'The shared sign-in'),
//     then the USB console, then the 10 s hold, said to clear what it clears; the dashboard's
//     timeout line sends the owner to click before the press (its old
//     "Press BOOT and click above" was the press-first order the arbiter
//     cannot protect); the
//     companion's Save asks first and keeps asking every 2 s for 30 s while
//     the owner presses BOOT (403, 403, 200 saves; a 409 says another device
//     asked and gives Save back; any other refusal ends the save with its
//     status; sixteen 403s, the last at 30 s, say no press was seen), its
//     Home Assistant step's no-session line says Save before BOOT, and
//     its banner is on screen whenever it holds a line.
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

// An element's classList: add, remove, toggle (with its force argument) and
// contains, as a browser's; `hidden` reads whether the class is set.
function fakeClassList() {
  const set = new Set();
  return {
    set,
    get hidden() { return set.has('hidden'); },
    add(c) { set.add(c); },
    remove(c) { set.delete(c); },
    toggle(c, force) {
      const on = force === undefined ? !set.has(c) : !!force;
      if (on) set.add(c); else set.delete(c);
      return on;
    },
    contains(c) { return set.has(c); },
  };
}

// A browser window just big enough for the pages' kit code: elements by id
// (starting as `markup` draws them), a body, Blob, URL and timers, with every
// save recorded.
function fakeWindow(ids, markup) {
  const els = {};
  for (const id of ids) {
    const display = markup ? startingDisplay(markup, id) : 'none';
    els[id] = { id, style: { display }, textContent: '', className: '', classList: fakeClassList() };
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
// queues the next sign-in's answers), each route in `routes` (url → answer)
// as given, and every other route 200. Its
// startDashboard() makes the dashboard's own first requests through the same
// fetch, so a test that says "the Save added no request" means the Save.
function dashboard(answers, routes = {}) {
  const src = read(UI);
  const code = [
    kitBlock(src, 'web_ui.h'),
    src.match(/\n {4}let apiToken = null;\n/)[0],
    uiFunction(src, 'secureFetch', true),
    uiFunction(src, 'signInRaised'),
    uiFunction(src, 'showAuthModal'),
    uiFunction(src, 'hideAuthModal'),
    src.match(/\n {4}let recoveryKitText = null;\n/)[0],
    uiFunction(src, 'offerRecoveryKit'),
    uiFunction(src, 'closeRecoveryKit'),
    uiFunction(src, 'saveRecoveryKit'),
    uiFunction(src, 'tapContendedText'),
    uiFunction(src, 'saveRecoveryKitFromSettings', true),
    uiFunction(src, 'requestFromDevice', true),
    ';({ requestFromDevice, saveRecoveryKit, closeRecoveryKit, saveRecoveryKitFromSettings, secureFetch,',
    '    kitText: () => recoveryKitText, token: () => apiToken, setToken: (t) => { apiToken = t; } })',
  ].join('\n');
  const win = fakeWindow(DASH_IDS, src);
  const asked = [];
  // The sign-in's status line as each receipt ask leaves (what the owner
  // reads while the page is asking), kept apart from `asked`.
  const statusAtAsk = [];
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
      if (routes[url]) {
        const r = routes[url];
        return { status: r.status, ok: r.status >= 200 && r.status < 300,
                 json: async () => JSON.parse(r.body), text: async () => r.body };
      }
      if (url !== '/api/provisioning-receipt') {
        return { status: 200, ok: true, json: async () => ({}), text: async () => '{}' };
      }
      statusAtAsk.push(win.els.authStatus.textContent);
      const a = queue[Math.min(polls++, queue.length - 1)];
      if (a.blip) throw new TypeError('Failed to fetch');   // the ask never reached the device
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
  return { api, win, asked, statusAtAsk, calls, ctx, more };
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
// The device's answer when another address asked during the same BOOT tap
// (canary_wap.ino handle_provisioning_receipt, sweep F342).
const TAP_409 = { status: 409, body: '{"error":"tap_contended","hint":"Another device on this network asked for the receipt during the same BOOT tap, so the tap was closed and nothing was handed out. When only one device is asking, start your request first and keep it asking, then press BOOT. Or send the recovery kit\'s token as a bearer, or read the token from the USB serial console."}' };
const CONTENDED = /^Another device on your network asked for this Canary's keys at the same time, so it said no\./;
// A request that never reached the device (a network blip, a TLS handshake
// that failed): fetch rejects.
const BLIP = { blip: true };
// The retry each page's 409 names (sweep F342's fourth review): the click
// that asks first, then the press. The sign-in's own button, and the
// Settings save, whose next 403 opens the sign-in.
const SIGN_IN_RETRY = 'wait 10 seconds, then click Use BOOT Tap Once again, and only then short-tap BOOT.';
const SETTINGS_RETRY = 'wait 10 seconds, then click Save my recovery kit again; when the sign-in opens, ' +
                       'click Use BOOT Tap Once there, and only then short-tap BOOT.';
// The click comes before the press in `text`, and no line in it re-tries
// without naming the click.
function clickBeforePress(text, click) {
  const c = text.indexOf(click);
  const p = text.indexOf('short-tap BOOT');
  assert.ok(c >= 0, 'the retry names no "' + click + '": ' + text);
  assert.ok(p > c, 'the press comes before the click: ' + text);
  assert.doesNotMatch(text, /try again/i, 'a bare "try again" reads as "press BOOT again"');
}

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

  it("the timeout copy names the click before BOOT (sweep F342's second review)", async () => {
    // The tap goes only to an address that asked from 10 s before it onward,
    // so the retry must ask first. The old copy ("Press BOOT and click above
    // to try again") led the owner to press while no page of theirs was
    // asking, and a host watching /api/logs for the tap was then the first
    // and only asker.
    const { win, asked } = await dashboardTap([GATE_403]);
    assert.equal(asked.filter((q) => q.url === '/api/provisioning-receipt').length, 30, 'the 60 s budget');
    const text = win.els.authStatus.textContent;
    assert.equal(text, "Didn't see a tap. Click Use BOOT Tap Once again, then short-tap BOOT.");
    assert.equal(win.els.authStatus.className, 'auth-status error');
    assert.ok(text.indexOf('Click') >= 0 && text.indexOf('Click') < text.indexOf('short-tap BOOT'),
              'the click comes before the press');
    assert.doesNotMatch(read(UI), /Press BOOT and click/);
  });

  it('the sign-in card and the listening line put the click before BOOT too', async () => {
    // The card's standing instruction is the first thing the owner reads,
    // and press-first is the one order the device cannot protect: a tap no
    // page of the owner's was asking for goes to the first asker.
    const card = read(UI).match(
      /onclick="requestFromDevice\(\)">Use BOOT Tap Once<\/button>\s*<p[^>]*>\s*([^<]*?)\s*<\/p>/);
    assert.ok(card, 'the instruction under Use BOOT Tap Once was not found');
    assert.equal(card[1], 'Click the button, then short-tap BOOT on the Canary when the line below says to, within 60 seconds.');
    assert.ok(card[1].indexOf('Click') < card[1].indexOf('short-tap BOOT'), 'the click comes before the press');
    // The first ask leaves with "don't press yet"; once the device has
    // answered it (403), the line says to press now, with the countdown.
    const { statusAtAsk } = await dashboardTap([GATE_403, GATE_403, { status: 200, body: RECEIPT }]);
    assert.equal(statusAtAsk[0], "Asking the Canary first. Don't press BOOT yet.");
    assert.equal(statusAtAsk[1], 'Now short-tap BOOT on the device. Listening for BOOT tap — 58s left ' +
                                 '(device holds the press for 30s).');
    assert.ok(statusAtAsk.length === 3 && statusAtAsk.slice(1).every((t) => /^Now short-tap BOOT on the device\. Listening for BOOT tap — \d+s left/.test(t)),
              'the press line and its countdown follow the first answer');
    assert.ok(statusAtAsk.every((t) => !/click/i.test(t)), 'no line asks for a click after the press');
  });

  it("the line says to press only once the device has answered an ask (sweep F342's fourth review)", async () => {
    // The tap goes to an address that is already asking, and the page's
    // first ask can still be on its way when the line appears (a TLS
    // handshake, a blip the page swallows, a busy server). A press made as
    // the old line appeared ("Now short-tap BOOT", set before the first
    // fetch) could land before any ask of the page's had reached the
    // device, and a host watching /api/logs for the tap was then its only
    // asker. Two asks that never reached the device, then the device's 403.
    const { statusAtAsk } = await dashboardTap([BLIP, BLIP, GATE_403, GATE_403, { status: 200, body: RECEIPT }]);
    assert.equal(statusAtAsk.length, 5);
    for (const t of statusAtAsk.slice(0, 3)) {
      assert.match(t, /^Asking the Canary first\. Don't press BOOT yet/, 'before the device answered: ' + t);
      assert.doesNotMatch(t, /Now short-tap/);
    }
    assert.match(statusAtAsk[3], /^Now short-tap BOOT on the device\./, 'after its first answer');
    // A device that never answers never told the owner to press, so the
    // page does not say it missed a tap.
    const { win } = await dashboardTap([BLIP]);
    assert.equal(win.els.authStatus.textContent,
                 "The Canary didn't answer. Click Use BOOT Tap Once again, and press BOOT only when this line says to.");
    assert.doesNotMatch(read(UI), /'Now short-tap BOOT on the device\. Listening for 60 seconds\.'/);
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

  it('another device asking during the tap (409) stops the sign-in on that answer and says so (sweep F342)', async () => {
    // Before: a 409 was just another non-200, so the page polled all 30
    // ticks and ended "Didn't see a tap", which is not what happened.
    const { api, win, asked, calls } = await dashboardTap([GATE_403, GATE_403, TAP_409, { status: 200, body: RECEIPT }]);
    assert.equal(asked.filter((q) => q.url === '/api/provisioning-receipt').length, 3, 'the poll stops at the 409');
    assert.match(win.els.authStatus.textContent, CONTENDED);
    assert.equal(win.els.authStatus.className, 'auth-status error');
    assert.doesNotMatch(win.els.authStatus.textContent, /Didn't see a tap/);
    assert.equal(api.token(), null, 'no token kept');
    assert.equal(calls.start, 0, 'the dashboard is not started');
    assert.equal(win.els.recoveryKitCard.style.display, 'none', 'no kit offered');
    assert.equal(api.kitText(), null);
  });

  it('after a 409, a second click asks first and is served on the next tap (the bench retry)', async () => {
    // The device's 409 ends ten seconds after the other device's last ask
    // (sweep F342's review), so the click the copy prescribes ("wait 10
    // seconds, then click Use BOOT Tap Once again, and only then short-tap
    // BOOT") hears 403 first and keeps listening.
    const d = dashboard([GATE_403, TAP_409]);
    await d.api.requestFromDevice();
    assert.match(d.win.els.authStatus.textContent, CONTENDED);
    assert.equal(d.api.token(), null);
    d.more([GATE_403, GATE_403, { status: 200, body: RECEIPT }]);
    await d.api.requestFromDevice();
    assert.equal(d.api.token(), FIELDS.token, 'the retry signs in');
    assert.equal(d.calls.start, 1, 'the dashboard starts once');
    assert.equal(d.win.els.recoveryKitCard.style.display, 'block', 'and offers the kit');
    assert.doesNotMatch(d.win.els.authStatus.textContent, CONTENDED);
  });

  it("the contention copy names the click before the press (sweep F342's fourth review)", async () => {
    // The page stops asking on the 409, and the device's 409 comes only
    // while another address is asking. Ten seconds after the page stopped,
    // a bare re-tap goes to the first asker, which is that address if it is
    // still asking: the token, the network password and the TLS pin. The
    // copy used to say "wait 10 seconds, and try again", which an owner can
    // read as "press BOOT again".
    const { win } = await dashboardTap([GATE_403, TAP_409]);
    const text = win.els.authStatus.textContent;
    assert.match(text, CONTENDED);
    assert.ok(text.includes('Close any other Canary pages and ' + SIGN_IN_RETRY), text);
    clickBeforePress(text, 'click Use BOOT Tap Once');
    // The Settings save: its next Save opens the sign-in, whose button asks first.
    const d = dashboard([TAP_409]);
    await d.api.saveRecoveryKitFromSettings();
    const st = d.win.els.settingsKitErr.textContent;
    assert.ok(st.includes('Close any other Canary pages and ' + SETTINGS_RETRY), st);
    clickBeforePress(st, 'click Use BOOT Tap Once');
    assert.ok(st.indexOf('click Save my recovery kit again') < st.indexOf('click Use BOOT Tap Once'));
    assert.doesNotMatch(read(UI), /wait 10 seconds, and try again/);
  });

  it("the contention copy names the pasted token first, then the console, then the hold", () => {
    const d = dashboard([GATE_403]);
    const fn = uiFunction(read(UI), 'tapContendedText');
    const text = vm.runInNewContext(fn + '\n;tapContendedText("click Use BOOT Tap Once again, and only then short-tap BOOT.")', {});
    assert.match(text, CONTENDED);
    assert.match(text, /recovery kit/);
    assert.match(text, /USB console/);
    assert.match(text, /hold BOOT for 10 seconds/);
    // What the hold clears is factory_reset_by_hold()'s list (the saved
    // Wi-Fi, the MQTT bridge's broker, the device name), not "its Wi-Fi".
    assert.match(text, /this clears its Wi-Fi, its Home Assistant link and its name, and keeps the token/);
    // The hold reopens the first-boot Bluetooth setup door, which goes to the
    // first client in radio range that connects (magic_pairing.md §2.2): a
    // host on the network that is also in range can take it and keep the
    // token. So the kit and the console come first, the hold last, with the
    // phone ready and the door's first-come rule named. The kit's way out is
    // the token pasted into the sign-in: it needs no tap, so no other asker
    // can close it, and the sign-in compares it without the session cookie
    // (before the owner-authority PR it took the cookie's word for any
    // pasted string, so this copy steered clear of the token box).
    assert.match(text, /paste the token instead: it is in your recovery kit, and the USB console prints it/);
    const kit = text.indexOf('recovery kit');
    const usb = text.indexOf('USB console');
    const hold = text.indexOf('hold BOOT for 10 seconds');
    assert.ok(kit >= 0 && usb > kit && hold > usb, 'the kit, then the console, then the hold: ' + text);
    assert.match(text, /with your iPhone ready: hold BOOT for 10 seconds/);
    assert.match(text, /Bluetooth setup goes to the first phone or computer nearby that connects/);
    assert.ok(d.api);
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
    assert.equal(d.win.els.authStatus.textContent, NO_TOKEN_REFUSAL, 'the sign-in names no token the page never had');
  });

  it('an answer that is not a receipt saves nothing and says so', async () => {
    const d = dashboard([{ status: 200, body: '{"ok":true}' }]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.win.saved.length, 0);
    assert.match(d.win.els.settingsKitErr.textContent, /Could not fetch the recovery kit: not a recovery kit/);
    assert.equal(d.win.els.settingsKitDone.style.display, 'none');
  });

  it('another device asking during the tap (409) says so, instead of "HTTP 409" (sweep F342)', async () => {
    const d = dashboard([TAP_409]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.asked.length, 1);
    assert.equal(d.win.saved.length, 0);
    assert.equal(d.win.els.settingsKitErr.style.display, 'block');
    assert.match(d.win.els.settingsKitErr.textContent, CONTENDED);
    assert.doesNotMatch(d.win.els.settingsKitErr.textContent, /HTTP 409/);
    assert.equal(d.win.els.authOverlay.classList.hidden, false, 'the sign-in was never shown over it');
    assert.equal(d.win.els.authStatus.textContent, '');
    assert.equal(d.win.els.settingsKitDone.style.display, 'none');
  });

  it("after a 409, the retry the copy names: a second Save opens the sign-in, and it is served on the next tap", async () => {
    // Ten seconds on, the device's 409 has ended and the second Save hears
    // 403: secureFetch opens the sign-in, and Use BOOT Tap Once asks first.
    const d = dashboard([TAP_409]);
    await d.api.saveRecoveryKitFromSettings();
    assert.match(d.win.els.settingsKitErr.textContent, CONTENDED);
    d.more([GATE_403]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.win.els.authOverlay.classList.hidden, false, 'the sign-in is open');
    assert.equal(d.win.els.authStatus.textContent, NO_TOKEN_REFUSAL);
    d.more([GATE_403, { status: 200, body: RECEIPT }]);
    await d.api.requestFromDevice();
    assert.equal(d.api.token(), FIELDS.token, 'signed in on the next tap');
    assert.equal(d.win.els.recoveryKitCard.style.display, 'block', 'and the kit is offered');
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

// secureFetch's sign-in line for a refusal, by what the page sent.
const TOKEN_REFUSAL = 'Invalid or expired token. Please re-enter.';
const NO_TOKEN_REFUSAL = 'Your session does not cover this, or has ended. Paste the API token, or use BOOT Tap Once.';
const QR_401 = { status: 401, body: '{"error":"unauthorized","hint":"Include \'Authorization: Bearer cv_...\' header"}' };

describe("secureFetch's sign-in names what the page sent (sweep F289's follow-up)", () => {
  it('a cookie session refused on the recovery kit (403): the session does not cover it, no token named', async () => {
    const d = dashboard([GATE_403]);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.asked[0].opts.headers.Authorization, undefined, 'the page held no token');
    assert.equal(d.win.els.authOverlay.classList.hidden, false);
    assert.equal(d.win.els.authStatus.textContent, NO_TOKEN_REFUSAL);
    assert.doesNotMatch(d.win.els.authStatus.textContent, /token\. Please re-enter/);
    assert.equal(d.win.els.authStatus.className, 'auth-status error');
  });

  it('a cookie session refused on the pairing QR (401): the same line', async () => {
    const d = dashboard([GATE_403], { '/api/pairing-qr': QR_401 });
    await assert.rejects(d.api.secureFetch('/api/pairing-qr', {}), /auth_required/);
    assert.equal(d.asked[0].opts.headers.Authorization, undefined);
    assert.equal(d.win.els.authOverlay.classList.hidden, false);
    assert.equal(d.win.els.authStatus.textContent, NO_TOKEN_REFUSAL);
  });

  it('a token the page held and the device refused: "Invalid or expired token", and the token is dropped', async () => {
    for (const [route, answer] of [['/api/provisioning-receipt', GATE_403], ['/api/pairing-qr', QR_401],
                                   ['/api/status', { status: 403, body: '{"error":"forbidden"}' }]]) {
      const d = dashboard([answer], { [route]: answer });
      d.api.setToken(FIELDS.token);
      await assert.rejects(d.api.secureFetch(route, {}), /auth_required/);
      assert.equal(d.asked[0].opts.headers.Authorization, 'Bearer ' + FIELDS.token, route);
      assert.equal(d.win.els.authStatus.textContent, TOKEN_REFUSAL, route);
      assert.equal(d.api.token(), null, route);
      // The next refusal finds no token in the page, and says so.
      await assert.rejects(d.api.secureFetch(route, {}), /auth_required/);
      assert.equal(d.asked[1].opts.headers.Authorization, undefined, route);
      assert.equal(d.win.els.authStatus.textContent, NO_TOKEN_REFUSAL, route);
    }
  });

  it('the lockout (429) still reads as the lockout, the token kept', async () => {
    const locked = { status: 429, body: '{"error":"too_many_requests","retry_after_s":9}' };
    const d = dashboard([locked]);
    d.api.setToken(FIELDS.token);
    await d.api.saveRecoveryKitFromSettings();
    assert.equal(d.win.els.authStatus.textContent, 'Too many failed attempts. Retry in 9 seconds.');
    assert.equal(d.api.token(), FIELDS.token, 'a 429 drops no token');
    assert.match(d.win.els.settingsKitErr.textContent, /Sign in again first/);
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
  // The Save handler against a device answering `answers` in turn (the last
  // repeats; one answer is every answer), each 2 s sleep run at once.
  function wizardSave(answers) {
    const queue = Array.isArray(answers) ? answers : [answers];
    const src = read(PWA);
    const m = src.match(/\$w\('wiz-recovery-save'\)\.addEventListener\('click', (async \(\) => \{[\s\S]*?\n {2}\})\);\n/);
    if (!m) throw new Error('the wiz-recovery-save handler was not found in companion_pwa.h');
    const ids = ['wiz-recovery-err', 'wiz-recovery-save', 'wiz-recovery-done', 'wiz-recovery-skip', 'wiz-ha-block'];
    const win = fakeWindow(ids);
    const asked = [];
    // The banner as each ask leaves: what the owner sees while Save asks.
    const banners = [];
    const ctx = {
      $w: (id) => win.els[id],
      fetch: async (url, opts) => {
        const answer = queue[Math.min(asked.length, queue.length - 1)];
        const err = win.els['wiz-recovery-err'];
        banners.push({ text: err.textContent, show: err.classList.contains('show'), note: err.classList.contains('note') });
        asked.push({ url, opts, disabled: win.els['wiz-recovery-save'].disabled });
        return { status: answer.status, ok: answer.status === 200, text: async () => answer.body };
      },
      Promise,
      requestAnimationFrame: () => {}, focusActiveStepHeading: () => {},
      document: win.document, Blob: win.Blob, URL: win.URL, JSON, Error,
    };
    ctx.window = ctx;
    ctx.setTimeout = (fn, ms) => { win.timers.push(ms); fn(); };
    const code = kitBlock(src, 'companion_pwa.h') + '\n;(' + m[1] + ')';
    const handler = vm.runInNewContext(code, ctx, { filename: 'wizard.recovery_save.js' });
    return handler().then(() => ({ win, asked, banners }));
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
  it('Save asks first and keeps asking while the owner presses BOOT: 403, 403, 200 saves (sweep F342)', async () => {
    // Before: the first 403 ended the save with "Press the BOOT button on
    // your Canary first", so the page was never asking when the press
    // landed, and the device hands a tap only to an address that was.
    const { win, asked } = await wizardSave([GATE_403, GATE_403, { status: 200, body: RECEIPT }]);
    assert.equal(asked.length, 3);
    assert.ok(asked.every((q) => q.url === '/api/provisioning-receipt' && q.opts.cache === 'no-store'));
    assert.deepEqual(win.timers.slice(0, 2), [2000, 2000], 'two sleeps of 2 s between the three asks');
    assert.equal(win.timers.filter((ms) => ms === 2000).length, 2, 'and no more (the rest is the blob URL\'s revoke)');
    assert.ok(asked.every((q) => q.disabled === true), 'Save stays disabled while it asks');
    assert.equal(win.saved.length, 1);
    assert.deepEqual([...win.URL.made[0].blob.parts], [RECEIPT]);
    assert.equal(win.els['wiz-recovery-done'].style.display, 'block');
    assert.equal(win.els['wiz-ha-block'].style.display, 'block');
    assert.equal(win.els['wiz-recovery-err'].textContent, '');
    assert.equal(win.els['wiz-recovery-err'].classList.contains('show'), false, 'the banner is put away');
    assert.equal(win.els['wiz-recovery-save'].disabled, false);
  });
  it('the old order still works: pressed first, the first ask is served', async () => {
    const { win, asked } = await wizardSave({ status: 200, body: RECEIPT });
    assert.equal(asked.length, 1);
    assert.equal(win.saved.length, 1);
  });
  it('the instruction is on screen while it asks, in the calm style', async () => {
    const src = read(PWA);
    assert.match(src, /say\('Now short-press BOOT on your Canary\.', true\);/);
    const { win, banners } = await wizardSave([GATE_403, GATE_403, TAP_409]);
    // While Save asks, the banner is shown, in the calm (note) style: "don't
    // press yet" as the first ask leaves, the press once the device has
    // answered it (sweep F342's fourth review: a press the first ask has
    // not reached is one no page of the owner's is asking for).
    assert.equal(banners.length, 3);
    assert.deepEqual(banners[0], { text: "Asking your Canary first. Don't press BOOT yet.", show: true, note: true });
    for (const b of banners.slice(1)) {
      assert.deepEqual(b, { text: 'Now short-press BOOT on your Canary.', show: true, note: true });
    }
    // After the 409 the banner holds the contention copy, shown, not calm.
    assert.ok(win.els['wiz-recovery-err'].classList.contains('show'), 'the banner is on screen');
    assert.equal(win.els['wiz-recovery-err'].classList.contains('note'), false);
  });
  it('another device asking during the press (409) says so and gives Save back', async () => {
    const { win, asked } = await wizardSave([GATE_403, TAP_409, { status: 200, body: RECEIPT }]);
    assert.equal(asked.length, 2, 'the asking stops at the 409');
    assert.equal(win.saved.length, 0);
    assert.match(win.els['wiz-recovery-err'].textContent, CONTENDED);
    const text = win.els['wiz-recovery-err'].textContent;
    assert.ok(text.includes('Close any other Canary pages and wait 10 seconds, then tap Save again, ' +
                            'and short-press BOOT only when this note says to.'), text);
    assert.ok(text.indexOf('tap Save again') < text.indexOf('short-press BOOT'), 'the tap on Save comes before the press');
    assert.equal(win.els['wiz-recovery-save'].disabled, false, 'Save is enabled again');
    assert.equal(win.els['wiz-ha-block'].style.display, 'none');
  });
  it('no press in 30 s (sixteen 403s, the last at 30 s) saves nothing and says so', async () => {
    // The asks go out at 0, 2, ..., 30 s, so a press up to 30 s after Save
    // is asked for (fifteen asks ended at 28 s, and a press between 28 and
    // 30 s heard "Didn't see a press" with the gate open).
    const { win, asked } = await wizardSave(GATE_403);
    assert.equal(asked.length, 16);
    assert.equal(win.timers.filter((ms) => ms === 2000).length, 15, 'the last ask goes out 30 s after the first');
    assert.equal(win.saved.length, 0);
    assert.equal(win.els['wiz-recovery-err'].textContent,
                 "Didn't see a press. Tap Save, then short-press BOOT within 30 seconds.");
    assert.ok(win.els['wiz-recovery-err'].classList.contains('show'), 'on screen');
    assert.equal(win.els['wiz-recovery-save'].disabled, false);
  });
  it('the steps say Save first, then BOOT', () => {
    const src = read(PWA);
    const block = src.match(/<div id="wiz-recovery-block"[^>]*>([\s\S]*?)\n {6}<\/div>\n/)[1];
    const steps = [...block.matchAll(/<li>([\s\S]*?)<\/li>/g)].map((m) => m[1]);
    assert.deepEqual(steps, ['Tap <strong>Save my recovery kit</strong> below.',
                             'When the note below says to, short-press the <strong>BOOT</strong> button on your Canary (within 30 seconds).']);
  });
  it('an answer that is not 200, 403 or 409 (a 503 after the tap was spent) ends the save with its status', async () => {
    // The receipt can refuse after it has spent the tap (503 no_address, 500
    // out_of_memory); the page must say so, not keep asking for a press the
    // device already took and end on "Didn't see a press".
    const { win, asked } = await wizardSave([GATE_403, { status: 503, body: '{"error":"no_address"}' }, { status: 200, body: RECEIPT }]);
    assert.equal(asked.length, 2, 'the asking stops at the 503');
    assert.equal(win.saved.length, 0);
    assert.equal(win.els['wiz-recovery-err'].textContent,
                 'Could not fetch the recovery kit: HTTP 503. Try again, or save it later from the dashboard.');
    assert.ok(win.els['wiz-recovery-err'].classList.contains('show'), 'on screen');
    assert.equal(win.els['wiz-recovery-save'].disabled, false, 'Save is enabled again');
    assert.equal(win.els['wiz-ha-block'].style.display, 'none');
  });
  it("the Home Assistant step's no-session line sends the owner to Save before BOOT", () => {
    const src = read(PWA);
    assert.match(src, /This needs the recovery-kit step first: go back, tap Save my recovery kit, then short-press BOOT\./);
    assert.doesNotMatch(src, /press BOOT, and save the kit/);
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
