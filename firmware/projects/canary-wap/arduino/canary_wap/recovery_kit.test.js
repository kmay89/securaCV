'use strict';

// The recovery kit after a BOOT-tap sign-in on the dashboard (sweep F257).
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
//     and Save hands the browser the receipt text exactly as served, every
//     field identity_json.h writes, as canary-recovery-kit.json through a
//     blob: URL, with no request of any kind after the receipt GET;
//   * a tap that never served a receipt (403 every poll, an answer that is
//     not a receipt) offers nothing;
//   * the wizard's close-out saves through the same builder.
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
// home network (base_url at the .local name since sweep F255).
const FIELDS = {
  device_id: 'canary-s3-4dC2',
  base_url: 'https://canary-7916.local',
  token: 'cv_' + 'ab12'.repeat(8),
  pubkey_fp: '7916ca487912fa1b',
  firmware: '2.4.15',
  hw_token: '0123456789abcdef0123456789abcdef',
  ap_ssid: 'SecuraCV-7916',
  ap_password: 'quiet-owl-lamp-42',
  tls_cert_fp: 'cdfcca58db62ca948e8741d19e28ce1c79b4a818755559b419b1dd17c439dcfa',
  provisioned_at: 'boot:7',
};
const RECEIPT = '{\n' + Object.entries(FIELDS).map(([k, v]) => '  "' + k + '": ' + JSON.stringify(v)).join(',\n') + '\n}';

// A browser window just big enough for the pages' kit code: elements by id,
// a body, Blob, URL and timers, with every save recorded.
function fakeWindow(ids) {
  const els = {};
  for (const id of ids) els[id] = { id, style: { display: 'none' }, textContent: '', className: '' };
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

const DASH_IDS = ['authStatus', 'recoveryKitCard', 'recoveryKitDone', 'recoveryKitSave', 'recoveryKitLater'];

// The dashboard's BOOT-tap sign-in exactly as web_ui.h ships it: the RECOVERY_KIT
// block, requestFromDevice and the three kit functions, run against a device
// that answers each poll from `answers` (the last one repeats).
async function dashboardTap(answers) {
  const src = read(UI);
  const code = [
    kitBlock(src, 'web_ui.h'),
    src.match(/\n {4}let recoveryKitText = null;\n/)[0],
    uiFunction(src, 'offerRecoveryKit'),
    uiFunction(src, 'closeRecoveryKit'),
    uiFunction(src, 'saveRecoveryKit'),
    uiFunction(src, 'requestFromDevice', true),
    ';({ requestFromDevice, saveRecoveryKit, closeRecoveryKit, kitText: () => recoveryKitText, token: () => apiToken })',
  ].join('\n');
  const win = fakeWindow(DASH_IDS);
  const asked = [];
  let polls = 0;
  const calls = { hide: 0, start: 0 };
  const ctx = {
    API_BASE: '',
    apiToken: null,
    document: win.document,
    Blob: win.Blob,
    URL: win.URL,
    JSON, Number, Error, Promise,
    // Every timer runs at once: the 2 s sleep between polls, and the blob
    // URL's revoke (its delay recorded).
    setTimeout: (fn, ms) => { win.timers.push(ms); fn(); },
    fetch: async (url, opts) => {
      asked.push({ url, opts });
      const a = answers[Math.min(polls++, answers.length - 1)];
      return { status: a.status, json: async () => JSON.parse(a.body), text: async () => a.body };
    },
    hideAuthModal: () => { calls.hide++; },
    startDashboard: () => { calls.start++; },
  };
  ctx.window = ctx;   // the page's global object is its window
  const api = vm.runInNewContext(code, ctx, { filename: 'web_ui.boot_tap.js' });
  await api.requestFromDevice();
  return { api, win, asked, calls, ctx };
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

  it('a tap that served a receipt signs in and offers the kit', async () => {
    const { api, win, asked, calls } = await dashboardTap([GATE_403, { status: 200, body: RECEIPT }]);
    assert.equal(calls.hide, 1);
    assert.equal(calls.start, 1);
    assert.equal(api.token(), FIELDS.token, 'the dashboard still signs in with the receipt token');
    assert.equal(win.els.recoveryKitCard.style.display, 'block', 'the offer is on screen');
    assert.equal(win.els.recoveryKitDone.style.display, 'none');
    assert.equal(win.els.recoveryKitLater.textContent, 'Do this later');
    assert.equal(api.kitText(), RECEIPT, 'held for the save, as served');
    assert.equal(win.saved.length, 0, 'nothing is saved before the person asks');
    assert.deepEqual(asked.map((q) => q.url), ['/api/provisioning-receipt', '/api/provisioning-receipt']);
  });

  it('Save hands the browser the receipt as canary-recovery-kit.json and sends nothing', async () => {
    const { api, win, asked } = await dashboardTap([{ status: 200, body: RECEIPT }]);
    const before = asked.length;
    api.saveRecoveryKit();
    assert.equal(asked.length, before, 'no request after the receipt GET');
    for (const q of asked) {
      assert.equal(q.url, '/api/provisioning-receipt');
      assert.ok(!q.opts || ((!q.opts.method || q.opts.method === 'GET') && q.opts.body === undefined),
        'the only request is the receipt GET');
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

describe("the offer speaks the wizard's words", () => {
  const ui = read(UI);
  const pwa = read(PWA);
  const inner = (src, re, what) => {
    const m = src.match(re);
    assert.ok(m, what + ' not found');
    return m[1];
  };
  it('title, lead, buttons and the saved line match the /companion close-out', () => {
    const wizBlock = inner(pwa, /<div id="wiz-recovery-block"[^>]*>([\s\S]*?)\n {6}<\/div>\n/, 'the wizard block');
    const wizTitle = inner(wizBlock, /<h3[^>]*>([^<]*)<\/h3>/, 'the wizard title');
    const wizLead = inner(wizBlock, /<p class="wiz-sub" style="margin:0 0 \.6rem">([^]*?)<\/p>/, 'the wizard lead');
    const wizSave = inner(wizBlock, /id="wiz-recovery-save">([^<]*)</, 'the wizard Save');
    const wizLater = inner(wizBlock, /id="wiz-recovery-skip">([^<]*)</, 'the wizard Later');
    const wizDone = inner(wizBlock, /id="wiz-recovery-done"[^>]*>([^]*?)<\/p>/, 'the wizard saved line');

    assert.equal(inner(ui, /id="recoveryKitTitle">([^<]*)</, 'the offer title'), wizTitle);
    assert.equal(inner(ui, /id="recoveryKitLead">([^]*?)<\/div>/, 'the offer lead'), wizLead);
    assert.equal(inner(ui, /id="recoveryKitSave"[^>]*>([^<]*)</, 'the offer Save'), wizSave);
    assert.equal(inner(ui, /id="recoveryKitLater"[^>]*>([^<]*)</, 'the offer Later'), wizLater);
    // The wizard's saved line names the file, then how to get it again. The
    // dashboard says the first sentence only: the save itself just gave this
    // browser a session, so a reload here goes straight in, past the BOOT-tap
    // sign-in where the kit is offered.
    const dashDone = inner(ui, /id="recoveryKitDone"[^>]*>([^]*?)<\/p>/, 'the offer saved line');
    assert.equal(dashDone, wizDone.slice(0, wizDone.indexOf('</strong>.') + '</strong>.'.length));
    assert.match(dashDone, /canary-recovery-kit\.json/);
    assert.equal(wizTitle, 'Save your recovery kit');
    assert.equal(wizSave, 'Save my recovery kit');
  });
  it('the card is hidden until a tap, each id appears once, and the buttons are wired', () => {
    assert.match(ui, /<div class="card" id="recoveryKitCard" style="display:none"/);
    for (const id of DASH_IDS.slice(1).concat(['recoveryKitTitle', 'recoveryKitLead'])) {
      assert.equal((ui.match(new RegExp('id="' + id + '"', 'g')) || []).length, 1, id);
    }
    assert.match(ui, /id="recoveryKitLater" onclick="closeRecoveryKit\(\)"/);
    assert.match(ui, /id="recoveryKitSave" onclick="saveRecoveryKit\(\)"/);
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
