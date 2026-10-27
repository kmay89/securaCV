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
//   * The shared sign-in (the owner-authority PR: F334, F335, F346): a pasted
//     token is checked by itself (GET /api/status with credentials 'omit'),
//     because a page holding the landing page's session passed that route on
//     the cookie alone, so a mistyped token was taken and refused one step
//     later; signInRaised() is the one test for "secureFetch already showed
//     the sign-in"; Not now puts away a sign-in a running dashboard raised;
//     and every "takes the API token" card note is one sentence shape.
//   * Credential grants ask for sign-in (F335, F346): registering the vault
//     key and the two pairings' confirms take the bearer alone, so the page
//     asks for the token at Start Pairing and at the opera's Create/Join
//     (before the phone's or the opera's clock runs), says nothing more under
//     a refused key registration (the key stays in the box) or a refused
//     opera confirm (the code stays up), shows 'hidden' for a Passkey
//     Entry's digits a session is not sent, leads with "Don't match" for a
//     comparison on a page without the token, and asks nobody for a PIN once
//     a pairing is over.
//   * The network moves on the bearer alone (F334, F345): after setup the
//     Wi-Fi card's Connect and Forget take the API token, so a session-only
//     page's refusal brings the sign-in up and alerts nothing (the typed
//     network stays in the form), the card says so first, Forget's confirm
//     names what it re-opens (the setup door, a camera's code scan) and the
//     broker login it drops (F333), and a Forget whose login clear the
//     device could not make says where to clear it.
//   * The reboot takes the API token (F361): Reboot Device and Retry full
//     boot go through the same sign-in, so a session-only page's refusal
//     says "Rebooting…" to nobody, and the card says the token is needed.
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
// api() asks signInRaised() (the shared sign-in) where the page has it; an
// older page (WAP_WEB_UI) has none, and its api() does not call it.
const apiSrc = (src.includes('\n    function signInRaised(') ? uiFunction('signInRaised', false) : '') +
  uiFunction('secureFetch', true) + uiFunction('api', true);
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
  // Every way a page sends the browser elsewhere is recorded: location.href,
  // location = ..., window.location = ..., assign/replace.
  const loc = {
    set href(v) { record.navigated = v; }, get href() { return ''; }, pathname: '/',
    assign: (v) => { record.navigated = v; }, replace: (v) => { record.navigated = v; },
  };
  const nav = { get() { return loc; }, set(v) { record.navigated = v; } };
  const win = Object.defineProperty({}, 'location', nav);
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
    window: win,
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
  Object.defineProperty(ctx, 'location', nav);
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
    // An id with characters a file name should not carry.
    Object.assign(answer, { ok: true, sd_available: true, device_id: 'canary s3/4dC2', card_path: '/EXPORT/bundle_7.json' });
    const p = exportPage(answer);
    await p.ctx.__t.exportWitness();
    assert.deepEqual(p.calls, ['POST /api/export']);
    assert.deepEqual(p.record.alerts, [], 'no alert after a good export');
    assert.equal(p.record.navigated, null, 'the browser is not sent anywhere');
    assert.equal(p.record.blobs.length, 1);
    assert.equal(p.record.blobs[0].type, 'application/json');
    assert.deepEqual(JSON.parse(p.record.blobs[0].text), answer, 'the file is the answer, every field');
    assert.deepEqual(p.record.clicks, [{ href: 'blob:0', download: 'witness-export-canary_s3_4dC2.json' }],
      'saved under the id with every other character made _');
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

// ── The shared sign-in (F334, F335, F346) ────────────────────────────────

// What a session page's device does with GET /api/status: api_auth_check
// takes the cookie before it compares anything (api_auth.h), so a request
// that carries the cookie (credentials not 'omit') is answered 200 whatever
// its bearer says; without the cookie the bearer is compared.
function cookieDevice(sent) {
  return async (url, opts = {}) => {
    const auth = (opts.headers || {}).Authorization;
    sent.push({ url, credentials: opts.credentials, cache: opts.cache, auth });
    const ok = opts.credentials !== 'omit' || auth === 'Bearer cv_right';
    return { status: ok ? 200 : 403, ok, json: async () => ({}), text: async () => '{}' };
  };
}

describe('The shared sign-in (F334, F335, F346)', () => {
  it('signInRaised() is true exactly for the refusals secureFetch showed the sign-in for', () => {
    const p = pageContext({});
    vm.runInContext(uiFunction('signInRaised', false) + '\nglobalThis.__r = signInRaised;\n', p.ctx);
    for (const r of [{ ok: false, error: 'auth_required' }, { ok: false, error: 'rate_limited' },
                     new Error('auth_required'), new Error('rate_limited')]) {
      assert.equal(p.ctx.__r(r), true, JSON.stringify(r) + String(r));
    }
    for (const r of [{ ok: false, error: 'Network error' }, { ok: false, error: 'unauthorized' },
                     new Error('HTTP 500'), { ok: true }, null, undefined]) {
      assert.equal(p.ctx.__r(r), false, JSON.stringify(r) + String(r));
    }
  });

  it('a pasted token is checked by itself, not by the session cookie', async () => {
    for (const [typed, signedIn] of [['cv_wrong', false], ['cv_right', true]]) {
      const sent = [];
      const p = pageContext({}, {
        API_BASE: '', apiToken: null, fetch: cookieDevice(sent),
        startDashboard() { p.started = (p.started || 0) + 1; },
      });
      vm.runInContext(uiFunction('secureFetch', true) + uiFunction('showAuthModal', false) +
        uiFunction('hideAuthModal', false) + uiFunction('connectWithToken', true) +
        '\nglobalThis.__c = connectWithToken; globalThis.__show = showAuthModal;\n', p.ctx);
      p.ctx.__show();
      p.el('tokenInput').value = typed;
      await p.ctx.__c();
      assert.equal(p.el('authOverlay').classList.contains('hidden'), signedIn, typed);
      assert.equal(p.ctx.apiToken, signedIn ? 'cv_right' : null, typed);
      assert.equal(p.started || 0, signedIn ? 1 : 0, typed);
      if (!signedIn) assert.equal(p.el('authStatus').textContent, 'Invalid or expired token. Please re-enter.');
      assert.deepEqual(sent, [{ url: '/api/status', credentials: 'omit', cache: 'no-store', auth: 'Bearer ' + typed }]);
    }
  });

  it('the sign-in a running dashboard brings up can be put away (Not now)', () => {
    assert.match(src, /<button[^>]*\bid="authNotNow"[^>]*\bonclick="hideAuthModal\(\)"[^>]*>Not now<\/button>/);
    for (const running of [true, false]) {
      const p = pageContext({});
      p.ctx.window._refreshInterval = running ? 7 : undefined;
      vm.runInContext(uiFunction('showAuthModal', false) + uiFunction('hideAuthModal', false) +
        '\nglobalThis.__s = showAuthModal; globalThis.__h = hideAuthModal;\n', p.ctx);
      p.el('authNotNow').style.display = running ? 'none' : '';
      p.ctx.__s('Pasting the token');
      assert.equal(p.el('authNotNow').style.display, running ? '' : 'none', running ? 'running' : 'first load');
      assert.equal(p.el('authOverlay').classList.contains('hidden'), false);
      p.ctx.__h();
      assert.equal(p.el('authOverlay').classList.contains('hidden'), true);
    }
  });

  it('every "takes the API token" card note is the one template', () => {
    const notes = [...src.matchAll(/<p class="tnote">([^<]*)<\/p>/g)].map((m) => m[1]);
    for (const n of notes) {
      assert.match(n, /^[A-Z][a-z]+ [^:]+ takes the API token: paste it \(it is in your recovery kit\), or use BOOT Tap Once\.$/, n);
    }
    assert.ok(notes.includes('Showing the QR takes the API token: paste it (it is in your recovery kit), or use BOOT Tap Once.'),
      'the pairing QR card (GET /api/pairing-qr takes the bearer alone, sweep F289) carries the note');
    assert.match(src, /\.tnote \{[^}]*color: var\(--muted\)/);
  });
});

// ── Credential grants ask for sign-in (F335, F346) ───────────────────────

describe('Credential grants ask for sign-in (F335, F346)', () => {
  // The page with `apiToken` (null: a page signed in by the landing page's
  // session alone). Its sign-in records each message; its secureFetch is a
  // device that refuses every request the way the page's own does (the
  // sign-in raised, then auth_required thrown).
  function grantPage(answers, apiToken) {
    const p = pageContext(answers, { apiToken, refreshVault() {}, auth: [] });
    p.ctx.showAuthModal = (m) => p.ctx.auth.push(m);
    p.ctx.secureFetch = async () => { p.ctx.showAuthModal('refused'); throw new Error('auth_required'); };
    p.ctx.startPairingPolling = () => p.calls.push('poll');
    p.ctx.window.__btPollTimer = 1;
    vm.runInContext(uiFunction('signInRaised', false) + uiFunction('refreshBtStatus', true) +
      uiFunction('btStartPairing', true) + uiFunction('registerVaultKey', true) +
      uiFunction('startPairing', true) + uiFunction('confirmPairing', true) +
      '\nglobalThis.__g = { refreshBtStatus, btStartPairing, registerVaultKey, startPairing, confirmPairing };\n', p.ctx);
    return p;
  }
  const bt = (pairing) => ({ '/api/bluetooth': { state: 'idle', enabled: true, advertising: false, connected: false, pairing } });
  // The card-note template, ending in what to press again ("on each one":
  // an opera's two Canaries each take their own token).
  const ASK = /^[A-Z][a-z]+ [^:]+ takes the API token(?: on each one)?: paste it \(it is in your recovery kit\), or use BOOT Tap Once, then /;

  it('a Passkey Entry the session is not sent shows "hidden" and keeps no digits', async () => {
    const p = grantPage(bt({ state: 'pin_displayed', pin_hidden: true }), null);
    await p.ctx.__g.refreshBtStatus();
    assert.equal(p.el('btPairingPin').textContent, 'hidden');
    assert.equal(p.ctx.window.__btPairingPin, null);
    assert.match(p.el('btPairingHint').textContent, /signed in with the API token\. If you did not start this, press Cancel Pairing\./);
    assert.equal(p.el('btPairingConfirmRow').style.display, 'none');
  });

  it('a finished or failed pairing asks nobody for a PIN', async () => {
    for (const [state, says] of [['complete', /^Paired\. The phone is listed under Paired devices\.$/],
                                 ['failed', /^This pairing did not finish\. Press Start Pairing to try again\.$/]]) {
      const p = grantPage(bt({ state }), null);
      p.ctx.window.__btPairingPin = 482913;
      await p.ctx.__g.refreshBtStatus();
      assert.equal(p.el('btPairingPin').textContent, '------', state);
      assert.equal(p.ctx.window.__btPairingPin, null, state);
      assert.match(p.el('btPairingHint').textContent, says);
      assert.equal(p.el('btPairingConfirmRow').style.display, 'none', state);
    }
  });

  it('a comparison on a page without the token leads with "Don\'t match"', async () => {
    const p = grantPage(bt({ state: 'confirming', pin: 2913 }), null);
    await p.ctx.__g.refreshBtStatus();
    assert.equal(p.el('btPairingPin').textContent, '002913');
    assert.equal(p.ctx.window.__btPairingPin, 2913);
    assert.match(p.el('btPairingHint').textContent, /^If you did not start this pairing, tap "Don't match"\. To accept it, "Numbers match" asks for the API token first/);
    assert.equal(p.el('btPairingConfirmRow').style.display, 'flex');
    const q = grantPage(bt({ state: 'confirming', pin: 482913 }), 'cv_x');
    await q.ctx.__g.refreshBtStatus();
    assert.equal(q.el('btPairingHint').textContent,
      'Compare this 6-digit code to the code on your phone. If they match, tap "Numbers match".');
  });

  it('Start Pairing and Create/Join ask for the token before the radio pairs', async () => {
    const p = grantPage({}, null);
    await p.ctx.__g.btStartPairing();
    assert.ok(!p.calls.includes('POST /api/bluetooth/pair/start'), 'no pairing started');
    assert.equal(p.ctx.auth.length, 1);
    assert.match(p.ctx.auth[0], ASK);
    assert.match(p.ctx.auth[0], /then press Start Pairing again\.$/);
    for (const mode of ['init', 'join']) {
      const m = grantPage({}, null);
      await m.ctx.__g.startPairing(mode);
      assert.deepEqual(m.calls, [], mode);
      assert.equal(m.ctx.auth.length, 1, mode);
      assert.match(m.ctx.auth[0], ASK);
    }
    // With the token in the page, both go to the device as before.
    const t = grantPage({ '/api/mesh/pair/start': { ok: true } }, 'cv_x');
    await t.ctx.__g.btStartPairing();
    await t.ctx.__g.startPairing('init');
    assert.ok(t.calls.includes('POST /api/bluetooth/pair/start') && t.calls.includes('POST /api/mesh/pair/start'));
    assert.ok(t.calls.includes('poll'));
    assert.deepEqual(t.ctx.auth, []);
  });

  it('a refused opera confirm never says "Completing…"; a served one does', async () => {
    const p = grantPage({ '/api/mesh/pair/confirm': { ok: false, error: 'auth_required' } }, null);
    p.el('pairingStatus').textContent = 'Verify code:';
    await p.ctx.__g.confirmPairing();
    assert.equal(p.el('pairingStatus').textContent, 'Verify code:');
    const q = grantPage({ '/api/mesh/pair/confirm': { ok: true } }, 'cv_x');
    await q.ctx.__g.confirmPairing();
    assert.equal(q.el('pairingStatus').textContent, 'Completing…');
  });

  it('a refused key registration alerts nothing and keeps the key; the card says so first', async () => {
    const p = grantPage({}, null);
    p.el('vaultPubkey').value = 'a'.repeat(64);
    await p.ctx.__g.registerVaultKey();
    assert.deepEqual(p.record.alerts, []);
    assert.equal(p.el('vaultPubkey').value, 'a'.repeat(64));
    assert.equal(p.ctx.auth.length, 1);
    assert.ok(src.includes('<p class="tnote">Registering a key takes the API token: paste it (it is in your recovery kit), or use BOOT Tap Once.</p>'),
      'the vault card carries the note');
    const card = slice('<div class="form-label">Unlock key</div>', '<div id="vaultKeyEntry"');
    assert.ok(card.includes('class="tnote"'), 'above the key box');
  });
});

// ── The network moves on the bearer alone (F334, F345) ───────────────────

describe('The network moves on the bearer alone (F334, F345)', () => {
  // The Wi-Fi card over the page's own api(), secureFetch() and sign-in,
  // against a device that answers every request `answer` (a session-only
  // page: no token held).
  function wifiPage(answer) {
    const sent = [];
    const p = pageContext({}, {
      API_BASE: '', apiToken: null,
      fetch: async (url, opts = {}) => {
        sent.push(`${opts.method || 'GET'} ${url}`);
        const a = answer(url);
        return { status: a.status, ok: a.status >= 200 && a.status < 300,
                 text: async () => JSON.stringify(a.body), json: async () => a.body };
      },
      stopWifiPolling() {}, startWifiPolling() { p.polling = true; },
      loadWifiStatus: async () => {}, clearWifiInputs() { p.cleared = true; },
    });
    p.ctx.window._refreshInterval = 7;   // a running dashboard
    vm.runInContext(apiSrc + uiFunction('showAuthModal', false) + uiFunction('hideAuthModal', false) +
      uiFunction('connectWifi', true) + uiFunction('forgetWifi', true) +
      '\nglobalThis.__w = { connectWifi, forgetWifi };\n', p.ctx);
    p.sent = sent;
    return p;
  }
  const REFUSED = { status: 401, body: { error: 'unauthorized' } };

  it('a session-only Connect brings the sign-in up and alerts nothing; the network stays typed', async () => {
    const p = wifiPage(() => REFUSED);
    p.el('wifiSsidInput').value = 'Home';
    p.el('wifiPassword').value = 'hunter22';
    await p.ctx.__w.connectWifi();
    assert.deepEqual(p.sent, ['POST /api/wifi/connect']);
    assert.deepEqual(p.record.alerts, []);
    assert.equal(p.el('authOverlay').classList.contains('hidden'), false);
    assert.match(p.el('authStatus').textContent, /Paste the API token \(it is in your recovery kit\), or use BOOT Tap Once\./);
    assert.equal(p.el('authNotNow').style.display, '');
    assert.equal(p.el('wifiProgress').style.display, 'none');
    assert.equal(p.el('wifiPassword').value, 'hunter22');
    assert.ok(!p.polling);
  });

  it('a session-only Forget brings the sign-in up and alerts nothing', async () => {
    const p = wifiPage(() => REFUSED);
    await p.ctx.__w.forgetWifi();
    assert.deepEqual(p.sent, ['POST /api/wifi/forget']);
    assert.deepEqual(p.record.alerts, []);
    assert.equal(p.el('authOverlay').classList.contains('hidden'), false);
    assert.ok(!p.cleared);
  });

  it("Forget's confirm names what it re-opens and the broker login it drops", async () => {
    const asked = [];
    const p = wifiPage(() => ({ status: 200, body: { ok: true, broker_login_cleared: true } }));
    p.ctx.confirm = (m) => { asked.push(m); return false; };
    await p.ctx.__w.forgetWifi();
    assert.deepEqual(p.sent, [], 'a declined confirm sends nothing');
    assert.equal(asked.length, 1);
    assert.match(asked[0], /saved broker login/);
    assert.match(asked[0], /if its build has the Bluetooth setup door and Bluetooth is on, that door is open for 30 minutes to the first phone or computer nearby that connects/);
    assert.match(asked[0], /a camera unit takes a Wi-Fi code shown to its lens/);
    assert.match(asked[0], /use Connect instead: it keeps the broker login/);
  });

  it('a Forget whose broker login stayed on file says where to clear it', async () => {
    for (const [cleared, alerts] of [[false, 1], [true, 0], [undefined, 0]]) {
      const p = wifiPage(() => ({ status: 200, body: { ok: true, broker_login_cleared: cleared } }));
      await p.ctx.__w.forgetWifi();
      assert.equal(p.record.alerts.length, alerts, String(cleared));
      if (alerts) assert.match(p.record.alerts[0], /broker login could not be cleared\. Clear it on the MQTT page\./);
      assert.ok(p.cleared, 'the form is cleared once the networks are forgotten');
    }
  });

  it('the Wi-Fi card says the token is needed, above its buttons', () => {
    const card = slice('<div id="wifiSetupSection">', 'id="wifiConnectBtn"');
    assert.ok(card.includes('<p class="tnote">Changing Wi-Fi takes the API token: paste it (it is in your recovery kit), or use BOOT Tap Once.</p>'));
  });
});

// ── The reboot takes the API token (F361) ────────────────────────────────

describe('The reboot takes the API token (F361)', () => {
  function rebootPage(status) {
    const sent = [];
    const p = pageContext({}, {
      API_BASE: '', apiToken: null,
      fetch: async (url, opts = {}) => {
        sent.push(`${opts.method || 'GET'} ${url}`);
        if (status === 'drop') throw new TypeError('Failed to fetch');   // the restart cut the link
        return { status, ok: status === 200, text: async () => '{"ok":true}', json: async () => ({}) };
      },
    });
    p.ctx.window._refreshInterval = 7;
    vm.runInContext(apiSrc + uiFunction('showAuthModal', false) + uiFunction('hideAuthModal', false) +
      uiFunction('confirmReboot', true) + uiFunction('retryFullBoot', true) +
      '\nglobalThis.__r = { confirmReboot, retryFullBoot };\n', p.ctx);
    p.sent = sent;
    return p;
  }

  it('a session-only reboot brings the sign-in up and says "Rebooting…" to nobody', async () => {
    for (const [fn, url] of [['confirmReboot', '/api/reboot'], ['retryFullBoot', '/api/safe-mode/retry']]) {
      const p = rebootPage(401);
      await p.ctx.__r[fn]();
      assert.deepEqual(p.sent, [`POST ${url}`], fn);
      assert.deepEqual(p.record.alerts, [], fn);
      assert.equal(p.el('authOverlay').classList.contains('hidden'), false, fn);
      assert.match(p.el('authStatus').textContent, /Paste the API token \(it is in your recovery kit\), or use BOOT Tap Once\./);
    }
  });

  it('a served reboot, or one whose restart cut the link, says it is rebooting', async () => {
    for (const status of [200, 'drop']) {
      const p = rebootPage(status);
      await p.ctx.__r.confirmReboot();
      assert.deepEqual(p.record.alerts, ['Rebooting…'], String(status));
      const q = rebootPage(status);
      await q.ctx.__r.retryFullBoot();
      assert.deepEqual(q.record.alerts, ['Rebooting into full operation…'], String(status));
    }
  });

  it('the device card says the token is needed, above Reboot Device', () => {
    const card = slice('<p class="tnote">Rebooting this Canary takes the API token', 'onclick="confirmReboot()"');
    assert.ok(card.includes('<p class="tnote">Rebooting this Canary takes the API token: paste it (it is in your recovery kit), or use BOOT Tap Once.</p>'));
    assert.ok(card.length < 600, 'the note sits with the Save and Reboot buttons');
  });
});
