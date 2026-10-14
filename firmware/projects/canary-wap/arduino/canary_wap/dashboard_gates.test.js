'use strict';

// What the admin dashboard (web_ui.h) shows where the build lacks a feature,
// and what Export saves (sweep F274, F276, F278, from F238's GATED list in
// firmware/tests_host/test_dashboard_route_match.test.js).
//
//   * Camera (F274): the peek routes are registered only under
//     FEATURE_CAMERA_PEEK (not in the DEV profile, the canary-wap-mobile env,
//     or FULL on the XIAO ESP32-C3). refreshPeekStatus() ignored the refused
//     status, so the tab kept "Ready" and "▶ Start", and Start answered an
//     alert. A status no route took (esp_http_server's 404), before any
//     status has answered, now shows the no-camera state (the "not_built"
//     gate of WebUiLogic.cameraPanelState) and hides the Camera Controls and
//     Camera Info cards, which hold every control that calls another camera
//     route; opening the tab reads the sensor state only once a status has
//     answered (the first answer pulls it), so such a build is asked only for
//     the status. No answer, a sign-in the route wants, or a text error page
//     from a route that exists says nothing about the build and changes
//     nothing; once a status has answered, a failed poll changes nothing. The refusals come from the page's own secureFetch() and api(),
//     run against a stub fetch.
//   * Community (F276): the /api/mesh and /api/chirp routes are registered only
//     under FEATURE_MESH_NETWORK (off in the DEV profile), and the Opera and
//     Chirp views kept their markup and controls. They now take F176's shape:
//     their sub-tabs start hidden and show on their status route's first
//     answer; BLE Discovery is the first view until Opera answers (then Opera
//     takes over, as it always opened, unless the owner picked a view); the
//     5 s poll refreshes only the view on screen, so it never asks for a
//     hidden one; opening the Community panel asks both again, so a probe a
//     busy load turned away recovers. BLE Discovery's 10 s refresh runs only
//     while it is on screen (leaving the panel or the view cancels a waiting
//     one), now that it can be the view a build opens on.
//   * Export (F278): POST /api/export now answers the bundle inline (no
//     download_url, which named a route nothing registers), and
//     exportWitness() saves that answer as a JSON download. The answer fed to
//     the page carries exactly the keys canary_wap.ino's handle_export writes.
//
// The page's own code is cut out of web_ui.h by markers that stand in the page
// before and after these changes (so the old page runs here and fails on what
// it does) and run against a stub DOM and a recording api(), the pattern of
// recovery_kit.test.js beside this file. WAP_WEB_UI and WAP_INO point it at
// another copy of the page and the sketch (an older tree's, to watch it fail).
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/dashboard_gates.test.js

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const UI = process.env.WAP_WEB_UI || path.join(__dirname, 'web_ui.h');
const INO = process.env.WAP_INO || path.join(__dirname, 'canary_wap.ino');
const src = fs.readFileSync(UI, 'utf8');

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// One function of the page's script, as it ships (4-space indent).
function uiFunction(name, isAsync) {
  const head = '\n    ' + (isAsync ? 'async ' : '') + 'function ' + name + '(';
  const at = src.indexOf(head);
  assert.ok(at >= 0, `${name} is in web_ui.h`);
  const end = src.indexOf('\n    }\n', at);
  return src.slice(at + 1, end + 7);
}

const logicBlock = (() => {
  const m = src.match(/\/\*\s*WEBUI_LOGIC:BEGIN[\s\S]*?WEBUI_LOGIC:END\s*\*\//);
  assert.ok(m, 'the WEBUI_LOGIC block is in web_ui.h');
  return m[0];
})();

// What the page's own api() (and the secureFetch() under it) returns for a
// response, or for no answer at all: lifted from the page and run against a
// stub fetch, so a refusal a test feeds a view is the one the page makes.
const apiSrc = uiFunction('secureFetch', true) + uiFunction('api', true);
async function answerTo(res) {
  const ctx = {
    API_BASE: '', apiToken: null, console: { error() {} }, showAuthModal() {},
    fetch: async () => {
      if (res === 'no answer') throw new TypeError('Failed to fetch');
      return { status: res.status, ok: res.status >= 200 && res.status < 300,
               text: async () => res.body, json: async () => JSON.parse(res.body) };
    },
  };
  vm.createContext(ctx);
  vm.runInContext(apiSrc + '\nglobalThis.__api = api;\n', ctx);
  return ctx.__api('/api/peek/status');
}
// esp_http_server's own 404, for a path no route registers.
const IDF_404 = { status: 404, body: 'Nothing matches the given URI' };
// What api() makes of it (pinned to the page below).
const NOT_FOUND = { ok: false, error: 'Nothing matches the given URI', unrouted: true };
const settle = () => new Promise((r) => setImmediate(r));

function makeEl(id, init = {}) {
  const classes = new Set(init.classes || []);
  return {
    id, textContent: init.text || '', innerHTML: '', value: '', disabled: false, checked: false, className: '',
    style: { display: init.display === undefined ? '' : init.display },
    dataset: init.dataset || {},
    classList: {
      add: (c) => classes.add(c), remove: (c) => classes.delete(c), contains: (c) => classes.has(c),
      toggle: (c, on) => (on === undefined ? (classes.has(c) ? classes.delete(c) : classes.add(c)) : on ? classes.add(c) : classes.delete(c)),
    },
    _classes: classes,
  };
}

// A page context: getElementById makes generic elements on first use; the
// Community panel's sub-nav buttons and sub-panels are built from the markup,
// as written (their classes, data-tab, id and starting display).
function pageContext(answers, extra = {}) {
  const els = new Map();
  const navBtns = [];
  for (const m of src.matchAll(/<button\b[^>]*onclick="switchCommunityTab\('([^']+)'\)"[^>]*>/g)) {
    const tag = m[0];
    const id = (tag.match(/\bid="([^"]+)"/) || [])[1];
    const tab = (tag.match(/\bdata-tab="([^"]+)"/) || [])[1];
    const cls = (tag.match(/\bclass="([^"]+)"/) || [, ''])[1].split(/\s+/);
    const hidden = /style="[^"]*display:\s*none/.test(tag);
    const el = makeEl(id || `btn:${m[1]}`, { classes: cls, dataset: tab ? { tab } : {}, display: hidden ? 'none' : '' });
    el.opens = m[1];
    navBtns.push(el);
    if (id) els.set(id, el);
  }
  const subPanels = [];
  for (const m of src.matchAll(/<div class="(sub-panel(?: active)?)" id="(community-[\w-]+)">/g)) {
    const el = makeEl(m[2], { classes: m[1].split(' ') });
    subPanels.push(el);
    els.set(m[2], el);
  }
  const seedText = (id) => {
    const m = src.match(new RegExp(`\\bid="${id}"[^>]*>([^<]*)<`));
    return m ? m[1] : '';
  };
  const el = (id) => {
    if (!els.has(id)) els.set(id, makeEl(id, { text: seedText(id) }));
    return els.get(id);
  };
  const calls = [];
  const timers = [];
  const later = [];
  const record = { alerts: [], blobs: [], clicks: [], navigated: null, revoked: [] };
  class Blob {
    constructor(parts, opts) { this.text = parts.join(''); this.type = opts && opts.type; record.blobs.push(this); }
  }
  const ctx = {
    console, Blob,
    URL: { createObjectURL: (b) => `blob:${record.blobs.indexOf(b)}`, revokeObjectURL: (u) => record.revoked.push(u) },
    document: {
      getElementById: el,
      querySelector: () => makeEl('nav'),
      querySelectorAll: (sel) => (sel === '#panel-community .sub-nav-btn' ? navBtns
        : sel === '#panel-community .sub-panel' ? subPanels : []),
      createElement: () => ({ click() { record.clicks.push({ href: this.href, download: this.download }); }, remove() {} }),
      body: { appendChild() {} },
    },
    window: { location: { set href(v) { record.navigated = v; }, get href() { return ''; } } },
    alert: (m) => record.alerts.push(m),
    confirm: () => true,
    setTimeout: (fn, ms) => { timers.push(ms); later.push(fn); return timers.length; },
    clearTimeout() {}, setInterval: () => 0, clearInterval() {},
    escapeHtml: (s) => String(s),
    api: async (url, method = 'GET') => {
      calls.push(method === 'GET' ? url : `${method} ${url}`);
      const a = answers[url];
      return a === undefined ? NOT_FOUND : typeof a === 'function' ? a() : a;
    },
    ...extra,
  };
  vm.createContext(ctx);
  vm.runInContext(logicBlock.replace(/const WebUiLogic\s*=/, 'globalThis.WebUiLogic ='), ctx);
  return { ctx, el, calls, timers, later, record, navBtns, subPanels };
}

const visibleSubPanel = (p) => p.subPanels.filter((s) => s._classes.has('active')).map((s) => s.id);
const askedOf = (calls, family) => calls.filter((c) => c === family || c.startsWith(family + '/') || c.startsWith(family + '?'));

// ── Community (F276) ─────────────────────────────────────────────────────

const community = slice('    function initialPanelFromPath() {', '    function switchRecordsTab(tab) {');
const opera = slice('    let pairingPollingInterval = null;', '    async function loadPeers() {');
const chirp = uiFunction('refreshChirpStatus', true);
const panelPoll = (() => {
  const s = slice('        window._panelInterval = setInterval(() => {', '        }, 5000);');
  return s.replace(/^\s*window\._panelInterval = setInterval\(/, '');
})();
const initialCommunityTab = (() => {
  const m = src.match(/let currentCommunityTab = '([^']+)';/);
  assert.ok(m, 'the page names its first Community view');
  return m[1];
})();

function communityPage(answers) {
  const p = pageContext(answers, {
    currentPanel: 'community', currentCommunityTab: initialCommunityTab,
    currentRecordsTab: 'logs', currentSettingsTab: 'wifi',
  });
  for (const stub of ['loadPeers', 'loadOperaAlerts', 'loadChirps', 'refreshBleDiscovery', 'refreshPresence',
                      'refreshHousehold', 'refreshWifiPresence', 'loadLogs', 'loadWitness', 'refreshBtStatus']) {
    p.ctx[stub] = () => p.calls.push(stub);
  }
  Object.assign(p.ctx, { peekActive: false, bleDiscoveryTimer: null, stopPeek() {}, stopCamInfoPolling() {} });
  vm.runInContext(community + opera + chirp + uiFunction('switchPanel', false) +
    '\nglobalThis.__panelPoll = ' + panelPoll + '};\n' +
    'globalThis.__t = { refreshOpera, refreshChirpStatus, switchCommunityTab, switchPanel };\n', p.ctx);
  // A click on a sub-nav button, as the browser delivers it (the old page
  // read the global event's target).
  p.click = (tab) => {
    const btn = p.navBtns.find((b) => b.opens === tab);
    p.ctx.event = { target: btn };
    p.ctx.__t.switchCommunityTab(tab);
  };
  return p;
}

const MESH_STATUS = { ok: true, state: 'NO_OPERA', enabled: true, has_opera: false, peers_total: 0, alerts_received: 0 };
const CHIRP_STATUS = { state: 'active', session_emoji: '🐦', nearby_count: 0, recent_chirps: 0, presence_met: true,
                       can_send: true, cooldown_remaining_sec: 0, muted: false, relay_enabled: true };

describe('Community tab: Opera and Chirp views gated on a first answer (F276)', () => {
  it('the Opera and Chirp sub-tabs start hidden, and BLE Discovery is the first view', () => {
    const p = communityPage({});
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.opera.style.display, 'none');
    assert.equal(byTab.chirp.style.display, 'none');
    assert.equal(byTab['ble-discovery'].style.display, '');
    assert.ok(byTab['ble-discovery']._classes.has('active'), 'BLE Discovery is the active sub-tab');
    assert.deepEqual(visibleSubPanel(p), ['community-ble-discovery']);
    assert.equal(initialCommunityTab, 'ble-discovery');
  });

  it('refused /api/mesh and /api/chirp keep both views hidden, and the poll asks for neither', async () => {
    const p = communityPage({});
    await p.ctx.__t.refreshOpera();
    await p.ctx.__t.refreshChirpStatus();
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.opera.style.display, 'none');
    assert.equal(byTab.chirp.style.display, 'none');
    assert.deepEqual(visibleSubPanel(p), ['community-ble-discovery']);
    p.calls.length = 0;
    for (let i = 0; i < 3; ++i) p.ctx.__panelPoll();
    await settle();
    assert.deepEqual(askedOf(p.calls, '/api/mesh'), [], 'no mesh request from the poll');
    assert.deepEqual(askedOf(p.calls, '/api/chirp'), [], 'no chirp request from the poll');
  });

  it('a mesh status shows the Opera view and opens on it, and the poll then refreshes it', async () => {
    const p = communityPage({ '/api/mesh': MESH_STATUS });
    await p.ctx.__t.refreshOpera();
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.opera.style.display, '');
    assert.ok(byTab.opera._classes.has('active'), 'Opera is the active sub-tab');
    assert.ok(!byTab['ble-discovery']._classes.has('active'));
    assert.deepEqual(visibleSubPanel(p), ['community-opera']);
    assert.equal(p.el('operaState').textContent, 'NO_OPERA');
    p.calls.length = 0;
    p.ctx.__panelPoll();
    await settle();
    assert.deepEqual(askedOf(p.calls, '/api/mesh'), ['/api/mesh']);
  });

  it('a view the owner picked stays picked when Opera answers later', async () => {
    const answers = {};
    const p = communityPage(answers);
    p.click('ble-discovery');
    answers['/api/mesh'] = MESH_STATUS;
    await p.ctx.__t.refreshOpera();
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.opera.style.display, '', 'the Opera sub-tab shows');
    assert.ok(byTab['ble-discovery']._classes.has('active'), 'BLE Discovery stays the view');
    assert.deepEqual(visibleSubPanel(p), ['community-ble-discovery']);
    p.click('opera');
    assert.deepEqual(visibleSubPanel(p), ['community-opera'], 'and a click still switches');
  });

  it('opening the Community panel asks Opera and Chirp again after a load that got no answer', async () => {
    const answers = { '/api/mesh': await answerTo('no answer'), '/api/chirp': await answerTo('no answer') };
    const p = communityPage(answers);
    await p.ctx.__t.refreshOpera();
    await p.ctx.__t.refreshChirpStatus();
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.opera.style.display, 'none');
    assert.equal(byTab.chirp.style.display, 'none');
    answers['/api/mesh'] = MESH_STATUS;
    answers['/api/chirp'] = CHIRP_STATUS;
    p.calls.length = 0;
    p.ctx.currentPanel = 'status';
    p.ctx.__t.switchPanel('community');
    await settle();
    assert.deepEqual(askedOf(p.calls, '/api/mesh'), ['/api/mesh']);
    assert.deepEqual(askedOf(p.calls, '/api/chirp'), ['/api/chirp']);
    assert.equal(byTab.opera.style.display, '', 'the Opera sub-tab shows');
    assert.equal(byTab.chirp.style.display, '', 'the Chirp sub-tab shows');
    assert.deepEqual(visibleSubPanel(p), ['community-opera'], 'and Opera takes over, no view having been picked');
  });

  it('a Chirp status shows the Chirp view without switching to it', async () => {
    const p = communityPage({ '/api/chirp': CHIRP_STATUS });
    await p.ctx.__t.refreshChirpStatus();
    const byTab = Object.fromEntries(p.navBtns.map((b) => [b.opens, b]));
    assert.equal(byTab.chirp.style.display, '');
    assert.equal(byTab.opera.style.display, 'none');
    assert.deepEqual(visibleSubPanel(p), ['community-ble-discovery']);
    assert.ok(p.calls.includes('loadChirps'), 'the recent list loads');
  });
});

describe("BLE Discovery's 10 s refresh runs only while the Community panel is open (F276)", () => {
  const ble = uiFunction('refreshBleDiscovery', true);
  function blePage(panel) {
    // One Canary near: refreshBleDiscovery() returns before its timer on an
    // empty or refused nearby list, so only a listed one reaches the timer.
    const near = { canaries: [{ device_id_prefix: 'a1b2', rssi_current: -60, rssi_quality: 'good', last_seen_sec_ago: 1 }],
                   non_canary_device_count: 0 };
    const p = pageContext({ '/api/ble/status': { ble_available: true }, '/api/nearby': near },
      { currentPanel: panel, currentCommunityTab: 'ble-discovery', bleDiscoveryTimer: 0,
        peekActive: false, stopPeek() {}, stopCamInfoPolling() {} });
    p.cleared = [];
    p.ctx.clearTimeout = (id) => p.cleared.push(id);
    vm.runInContext(ble + uiFunction('switchPanel', false) + uiFunction('selectCommunityTab', false) +
      '\nglobalThis.__t = { refreshBleDiscovery, switchPanel, selectCommunityTab };\n', p.ctx);
    return p;
  }
  it('schedules the next refresh with the panel open', async () => {
    const p = blePage('community');
    await p.ctx.__t.refreshBleDiscovery();
    assert.deepEqual(p.timers, [10000]);
  });
  it('schedules nothing once the panel is left', async () => {
    const p = blePage('status');
    await p.ctx.__t.refreshBleDiscovery();
    assert.deepEqual(p.timers, []);
  });
  for (const [name, leave] of [['leaving the Community panel', (p) => p.ctx.__t.switchPanel('status')],
                               ['picking another Community view', (p) => p.ctx.__t.selectCommunityTab('opera')]]) {
    it(`${name} cancels a refresh already waiting`, async () => {
      const p = blePage('community');
      await p.ctx.__t.refreshBleDiscovery();
      assert.deepEqual(p.timers, [10000]);
      p.cleared.length = 0;
      leave(p);
      assert.ok(p.cleared.includes(1), `the waiting refresh (timer 1) is canceled (${p.cleared})`);
    });
  }
  it('opening the Community panel keeps a waiting refresh', async () => {
    const p = blePage('community');
    await p.ctx.__t.refreshBleDiscovery();
    p.cleared.length = 0;
    p.ctx.__t.switchPanel('community', false);
    assert.deepEqual(p.cleared, []);
  });
});

// ── Camera (F274) ────────────────────────────────────────────────────────

const camera = slice('    function stopCamInfoPolling() {', '    function togglePeek() {');

function cameraPage(answers) {
  const p = pageContext(answers, {
    camInfoTimer: null, peekActive: false, cameraReady: false, cameraStandby: false, cameraGate: 'ok',
    currentResolution: 8, currentResolutionName: null,
  });
  for (const stub of ['startCamInfoPolling', 'applyCameraInfo', 'updateResolutionUI', 'refreshSensorState']) {
    p.ctx[stub] = () => p.calls.push(stub);
  }
  // The cards as the markup writes them (shown; an old page names neither).
  for (const id of ['peekControlsCard', 'camInfoCard']) {
    const tag = src.match(new RegExp(`<div\\b[^>]*\\bid="${id}"[^>]*>`));
    p.el(id).style.display = tag && /display:\s*none/.test(tag[0]) ? 'none' : '';
  }
  vm.runInContext(camera + '\nglobalThis.__t = { refreshPeekStatus };\n', p.ctx);
  return p;
}

const PEEK_STATUS = { ok: true, camera_initialized: true, standby: false, gate: 'ok', peek_active: false, resolution: 8 };

// Opening the Camera tab: the page's switchPanel() and refreshSensorState()
// run beside the camera code, so a request the opening makes is recorded as
// the request it is.
function cameraPanelPage(answers, init = {}) {
  const p = cameraPage(answers);
  Object.assign(p.ctx, { currentPanel: 'status', bleDiscoveryTimer: null, applySensorState() {},
                         stopPeek() {} }, init);
  for (const stub of ['refreshVault']) p.ctx[stub] = () => p.calls.push(stub);
  vm.runInContext(uiFunction('switchPanel', false) + uiFunction('refreshSensorState', true) +
                  '\nglobalThis.__t.switchPanel = switchPanel;\n', p.ctx);
  p.open = async () => { p.ctx.__t.switchPanel('camera'); await settle(); await settle(); };
  p.asked = () => p.calls.filter((c) => c.includes('/api/'));
  return p;
}

describe('Camera tab: the no-camera state on a refused status (F274)', () => {
  it("api() reads esp_http_server's 404 as no route of this build", async () => {
    assert.deepEqual({ ...(await answerTo(IDF_404)) }, NOT_FOUND);
  });

  it('a GET /api/peek/status no route took shows the no-camera state and hides the camera cards', async () => {
    const p = cameraPage({ '/api/peek/status': await answerTo(IDF_404) });
    await p.ctx.__t.refreshPeekStatus();
    assert.equal(p.el('peekToggle').disabled, true, 'Start is off');
    assert.equal(p.el('peekToggle').textContent, '⚠ No Camera');
    assert.equal(p.el('peekStatus').textContent, 'No camera in this build');
    assert.match(p.el('peekOfflineText').textContent, /built without the camera preview/);
    assert.equal(p.el('peekControlsCard').style.display, 'none');
    assert.equal(p.el('camInfoCard').style.display, 'none');
    assert.deepEqual(p.calls.filter((c) => c.includes('/api/')), ['/api/peek/status'], 'nothing else is asked of the camera');
  });

  it('a camera status shows the cards and the preview\'s own state', async () => {
    const p = cameraPage({ '/api/peek/status': PEEK_STATUS });
    await p.ctx.__t.refreshPeekStatus();
    assert.equal(p.el('peekToggle').disabled, false);
    assert.equal(p.el('peekToggle').textContent, '▶ Start');
    assert.equal(p.el('peekStatus').textContent, 'Ready');
    assert.equal(p.el('peekControlsCard').style.display, '');
    assert.equal(p.el('camInfoCard').style.display, '');
  });

  // esp_http_server's own text pages for a route that exists (a handler that
  // failed, a request it timed out) and a JSON 404 from a route that answered
  // are not "no such route" either: only a 404 that is not JSON is.
  for (const [name, res] of [['no answer', 'no answer'],
                             ['the route wants a token (401)', { status: 401, body: '{"ok":false,"error":"unauthorized"}' }],
                             ['the sign-in is rate-limited (429)', { status: 429, body: '{"ok":false,"retry_after_s":30}' }],
                             ['a text error page from a route that exists (500)', { status: 500, body: 'Internal Server Error' }],
                             ['a text timeout page (408)', { status: 408, body: 'Server closed this connection' }],
                             ['a JSON 404 from a route that answered', { status: 404, body: '{"ok":false,"error":"not_found"}' }]]) {
    it(`${name}, before any status, says nothing about the build`, async () => {
      const refused = await answerTo(res);
      assert.notEqual(refused.ok, true);
      const p = cameraPage({ '/api/peek/status': refused });
      const before = ['peekStatus', 'peekToggle', 'peekOfflineText'].map((id) => p.el(id).textContent);
      await p.ctx.__t.refreshPeekStatus();
      assert.deepEqual(['peekStatus', 'peekToggle', 'peekOfflineText'].map((id) => p.el(id).textContent), before,
        'no claim about the build');
      assert.equal(p.el('peekToggle').disabled, false);
      assert.equal(p.el('peekControlsCard').style.display, '');
      assert.equal(p.el('camInfoCard').style.display, '');
    });
  }

  it('a camera status after a status no route took brings the cards back and Start with them', async () => {
    const answers = { '/api/peek/status': await answerTo(IDF_404) };
    const p = cameraPage(answers);
    await p.ctx.__t.refreshPeekStatus();
    assert.equal(p.el('peekControlsCard').style.display, 'none');
    answers['/api/peek/status'] = PEEK_STATUS;
    await p.ctx.__t.refreshPeekStatus();
    assert.equal(p.el('peekControlsCard').style.display, '');
    assert.equal(p.el('camInfoCard').style.display, '');
    assert.equal(p.el('peekToggle').disabled, false);
    assert.equal(p.el('peekToggle').textContent, '▶ Start');
    assert.equal(p.el('peekStatus').textContent, 'Ready');
  });

  it('opening the Camera tab in a build without the camera asks only for the status, every time', async () => {
    const p = cameraPanelPage({ '/api/peek/status': await answerTo(IDF_404) });
    await p.open();
    assert.equal(p.el('peekStatus').textContent, 'No camera in this build');
    await p.open();
    assert.deepEqual(p.asked(), ['/api/peek/status', '/api/peek/status'],
      'no GET /api/peek/sensor while no status has answered');
  });

  for (const [name, init] of [['the camera was not known to be up', { cameraReady: false }],
                              ['the camera was already known to be up', { cameraReady: true }]]) {
    it(`opening the Camera tab in a camera build reads the sensor state once per open (${name})`, async () => {
      const p = cameraPanelPage({ '/api/peek/status': PEEK_STATUS, '/api/peek/sensor': { ok: true } }, init);
      await p.open();
      assert.deepEqual(p.asked(), ['/api/peek/status', '/api/peek/sensor'],
        'the first status pulls the sensor state the opening held back');
      p.calls.length = 0;
      await p.open();
      assert.deepEqual([...p.asked()].sort(), ['/api/peek/sensor', '/api/peek/status'],
        'after a status has answered, the opening reads it again');
    });
  }

  it('once a status has answered, a failed poll changes nothing', async () => {
    const answers = { '/api/peek/status': { ...PEEK_STATUS, peek_active: true } };
    const p = cameraPage(answers);
    await p.ctx.__t.refreshPeekStatus();
    assert.equal(p.el('peekToggle').textContent, '⏹ Stop');
    for (const res of ['no answer', IDF_404]) {
      answers['/api/peek/status'] = await answerTo(res);
      await p.ctx.__t.refreshPeekStatus();
    }
    assert.equal(p.el('peekToggle').textContent, '⏹ Stop', 'still streaming');
    assert.equal(p.el('peekToggle').disabled, false);
    assert.equal(p.el('peekControlsCard').style.display, '');
    assert.equal(p.el('camInfoCard').style.display, '');
  });

  it('the cards a refusal hides hold every control that calls another camera route', () => {
    const panel = slice('<div class="panel" id="panel-camera">', '<div class="panel" id="panel-presence">');
    const a = panel.indexOf('id="peekControlsCard"');
    const b = panel.indexOf('id="camInfoCard"');
    const c = panel.indexOf('id="vaultCard"');
    assert.ok(a > 0 && b > a && c > b, 'both cards are in the Camera panel, before the vault card');
    const cards = panel.slice(a, c);
    const rest = src.replace(cards, '');
    for (const h of ['takeSnapshot', 'setResolution', 'onSensorSlider', 'onSensorToggle', 'onSensorSelect',
                     'resetSensorDefaults', 'reinitCamera']) {
      assert.ok(cards.includes(`${h}(`), `${h} is in the hidden cards`);
      assert.ok(!new RegExp(`on(?:click|input|change)="${h}\\(`).test(rest), `${h} is called from no other control`);
    }
  });
});

// ── Export (F278) ────────────────────────────────────────────────────────

// The keys canary_wap.ino's handle_export writes into its answer.
function exportKeys() {
  const ino = fs.readFileSync(INO, 'utf8');
  const a = ino.indexOf('static esp_err_t handle_export(httpd_req_t* req) {');
  const b = ino.indexOf('\n}\n', a);
  assert.ok(a >= 0 && b > a, 'handle_export is in canary_wap.ino');
  return [...new Set([...ino.slice(a, b).matchAll(/\bdoc\["(\w+)"\]/g)].map((m) => m[1]))];
}

function exportPage(answer) {
  const p = pageContext({ '/api/export': answer });
  vm.runInContext(uiFunction('exportWitness', true) + '\nglobalThis.__t = { exportWitness };\n', p.ctx);
  return p;
}

describe('Export saves what it exported (F278)', () => {
  it('handle_export answers the bundle inline and names no URL to fetch', () => {
    const keys = exportKeys();
    for (const k of ['ok', 'device_id', 'pubkey', 'chain_seq', 'version']) assert.ok(keys.includes(k), `the answer carries ${k}`);
    assert.ok(!keys.some((k) => /url/i.test(k)), `no URL in the answer (${keys})`);
  });

  it('Export saves the answer it got, as a JSON download, and alerts nothing', async () => {
    const answer = {};
    for (const k of exportKeys()) answer[k] = `${k}-value`;
    Object.assign(answer, { ok: true, sd_available: true, device_id: 'canary-s3-4dC2', card_path: '/EXPORT/bundle_7.json' });
    const p = exportPage(answer);
    await p.ctx.__t.exportWitness();
    assert.deepEqual(p.calls, ['POST /api/export']);
    assert.deepEqual(p.record.alerts, [], 'no alert after a good export');
    assert.equal(p.record.navigated, null, 'the browser is not sent anywhere');
    assert.equal(p.record.blobs.length, 1);
    assert.equal(p.record.blobs[0].type, 'application/json');
    assert.deepEqual(JSON.parse(p.record.blobs[0].text), answer, 'the file is the answer, every field');
    assert.deepEqual(p.record.clicks, [{ href: 'blob:0', download: 'witness-export-canary-s3-4dC2.json' }]);
    assert.deepEqual(p.record.revoked, [], 'the link is not revoked before the save can start');
    for (const t of p.later) t();
    assert.deepEqual(p.record.revoked, ['blob:0'], 'and is revoked after it');
  });

  it('a refused export alerts the device\'s reason and saves nothing', async () => {
    const p = exportPage({ ok: false, error: 'auth_required' });
    await p.ctx.__t.exportWitness();
    assert.deepEqual(p.record.alerts, ['Export failed: auth_required']);
    assert.deepEqual(p.record.blobs, []);
    assert.equal(p.record.navigated, null);
  });
});
