'use strict';

// Unit tests for the admin dashboard's pure helpers (camera peek URL,
// bounded-retry policy, BLE chirp endpoint). The logic lives in a DOM-free
// block inside web_ui.h fenced by `WEBUI_LOGIC:BEGIN/END` and self-exported
// under Node — same extraction pattern as selftest_ui.test.js — so these
// tests exercise the SAME source the firmware ships.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/web_ui_logic.test.js
//
// WAP_WEB_UI points it at another copy of the page (an older tree's, to watch
// it fail), as it does dashboard_gates.test.js.

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { parseFragment, textElement, shape, attr, HOSTILE } = require('./html_fragment.js');

const UI = process.env.WAP_WEB_UI || path.join(__dirname, 'web_ui.h');

function loadLogic() {
  const src = fs.readFileSync(UI, 'utf8');
  const m = src.match(/\/\*\s*WEBUI_LOGIC:BEGIN[\s\S]*?WEBUI_LOGIC:END\s*\*\//);
  if (!m) throw new Error('WEBUI_LOGIC block not found in web_ui.h');
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(m[0], sandbox, { filename: 'web_ui_logic.extracted.js' });
  const L = sandbox.module.exports;
  if (!L || typeof L.peekStreamUrl !== 'function') {
    throw new Error('extracted block did not export the expected WebUiLogic API');
  }
  return L;
}

let L;
before(() => { L = loadLogic(); });

describe('peekStreamUrl', () => {
  it('omits the token param entirely when there is no real token', () => {
    // The old code appended "&token=null" (a literal string) on a
    // cookie-authenticated session; the cookie path validated first so it
    // "worked", but a real token=null query attempt is nonsense. Now the
    // param is dropped so the cookie is used cleanly.
    const u = L.peekStreamUrl('', '/api/peek/stream', null, 1234);
    assert.equal(u, '/api/peek/stream?t=1234');
    assert.doesNotMatch(u, /token/);
  });
  it('appends an encoded token when one is present', () => {
    const u = L.peekStreamUrl('', '/api/peek/stream', 'cv_abc 123', 9);
    assert.match(u, /&token=cv_abc%20123$/);
  });
  it('respects an undefined/empty token as no-token', () => {
    assert.doesNotMatch(L.peekStreamUrl('', '/api/peek/snapshot', undefined, 1), /token/);
    assert.doesNotMatch(L.peekStreamUrl('', '/api/peek/snapshot', '', 1), /token/);
  });
});

describe('shouldRetryPeek', () => {
  it('retries up to the cap, then stops (no infinite reload loop)', () => {
    assert.ok(L.shouldRetryPeek(0));
    assert.ok(L.shouldRetryPeek(L.PEEK_MAX_RETRIES - 1));
    assert.ok(!L.shouldRetryPeek(L.PEEK_MAX_RETRIES));
    assert.ok(!L.shouldRetryPeek(L.PEEK_MAX_RETRIES + 10));
  });
  it('has a finite, small cap', () => {
    assert.ok(Number.isInteger(L.PEEK_MAX_RETRIES));
    assert.ok(L.PEEK_MAX_RETRIES >= 1 && L.PEEK_MAX_RETRIES <= 20);
  });
});

describe('BLE_CHIRP_ENDPOINT', () => {
  it('points at the BLE handler, not the ESP-NOW community chirp', () => {
    assert.equal(L.BLE_CHIRP_ENDPOINT, '/api/ble/chirp/send');
    assert.notEqual(L.BLE_CHIRP_ENDPOINT, '/api/chirp/send');
  });
});

describe('fmtKbps', () => {
  it('never renders NaN/undefined — missing or invalid input becomes a dash', () => {
    assert.equal(L.fmtKbps(undefined), '—');
    assert.equal(L.fmtKbps(null), '—');
    assert.equal(L.fmtKbps(NaN), '—');
    assert.equal(L.fmtKbps('800'), '—');   // firmware sends a number; strings are a bug upstream
    assert.equal(L.fmtKbps(-1), '—');
    assert.equal(L.fmtKbps(Infinity), '—');
  });
  it('formats sub-Mbps rates as whole kbps', () => {
    assert.equal(L.fmtKbps(0), '0 kbps');
    assert.equal(L.fmtKbps(1), '1 kbps');
    assert.equal(L.fmtKbps(999), '999 kbps');
  });
  it('switches to Mbps at 1000 kbps and keeps the string short', () => {
    assert.equal(L.fmtKbps(1000), '1.0 Mbps');
    assert.equal(L.fmtKbps(2450), '2.5 Mbps');   // typical VGA MJPEG rate
    assert.equal(L.fmtKbps(9999), '10.0 Mbps');
    assert.equal(L.fmtKbps(10000), '10 Mbps');
    assert.equal(L.fmtKbps(54321), '54 Mbps');
  });
});

describe('cameraPanelState', () => {
  it('healthy running camera needs no override', () => {
    const s = L.cameraPanelState(true, false, 'ok');
    assert.equal(s.usable, true);
    assert.equal(s.label, '');
  });

  it('parked camera is usable with wake copy, never "broken"', () => {
    const s = L.cameraPanelState(false, true, 'ok');
    assert.equal(s.usable, true);
    assert.match(s.label, /Asleep/);
    assert.match(s.offline, /wake/i);
  });

  it('genuine init failure reads as no camera', () => {
    const s = L.cameraPanelState(false, false, 'no_camera');
    assert.equal(s.usable, false);
    assert.match(s.offline, /not initialized/);
  });

  it('thermal gate blocks the preview with heat copy', () => {
    const s = L.cameraPanelState(true, false, 'thermal');
    assert.equal(s.usable, false);
    assert.match(s.offline, /too hot/i);
  });

  it('battery policy blocks the preview with battery copy', () => {
    const s = L.cameraPanelState(false, true, 'policy');
    assert.equal(s.usable, false);
    assert.match(s.offline, /battery/i);
  });

  it('a build without the camera preview says so, whatever the flags (F274)', () => {
    for (const [initialized, standby] of [[false, false], [true, false], [false, true]]) {
      const s = L.cameraPanelState(initialized, standby, 'not_built');
      assert.equal(s.usable, false);
      assert.equal(s.label, 'No camera in this build');
      assert.match(s.offline, /built without the camera preview/);
    }
  });
});

describe('otaBannerVisible', () => {
  it('shows only when an update exists and that version is undismissed', () => {
    assert.equal(L.otaBannerVisible(true, '2.3.0', ''), true);
    assert.equal(L.otaBannerVisible(true, '2.3.0', '2.2.1'), true);
  });
  it('never shows without an available update', () => {
    assert.equal(L.otaBannerVisible(false, '2.3.0', ''), false);
    assert.equal(L.otaBannerVisible(false, '', ''), false);
  });
  it('"Later" silences exactly the dismissed version, not the next one', () => {
    assert.equal(L.otaBannerVisible(true, '2.3.0', '2.3.0'), false);
    assert.equal(L.otaBannerVisible(true, '2.3.1', '2.3.0'), true);
  });
  it('refuses to advertise an update with a missing/empty version string', () => {
    assert.equal(L.otaBannerVisible(true, '', ''), false);
    assert.equal(L.otaBannerVisible(true, undefined, ''), false);
    assert.equal(L.otaBannerVisible(true, null, ''), false);
  });
});

// The Chirp send card, from GET /api/chirp's answer (chirp_api.h). The route
// names cannot_send_reason "clock_unsynced" when only the wall clock stops a
// send (sweep F146); the card said "Ready" with Send on, and the send was
// refused (as a cooldown, before F146).
describe('chirpSendGate', () => {
  const ready = { presence_met: true, cooldown_remaining_sec: 0, can_send: true };
  it('is Ready with Send on when the device can send', () => {
    assert.deepEqual({ ...L.chirpSendGate(ready) },
                     { text: 'Ready', sendDisabled: false, presenceHint: false });
  });
  it('waits for GPS time, Send off, when the clock is not set', () => {
    const g = L.chirpSendGate({ ...ready, can_send: false, cannot_send_reason: 'clock_unsynced' });
    assert.equal(g.sendDisabled, true);
    assert.equal(g.presenceHint, false);
    assert.match(g.text, /GPS time/);
    assert.notEqual(g.text, 'Ready');
  });
  it('keeps the warm-up first, with its hint', () => {
    const g = L.chirpSendGate({ ...ready, presence_met: false, can_send: false,
                                cannot_send_reason: 'presence_required' });
    assert.equal(g.text, 'Warming up…');
    assert.equal(g.sendDisabled, true);
    assert.equal(g.presenceHint, true);
  });
  it('counts a cooldown down as m:ss', () => {
    const g = L.chirpSendGate({ ...ready, cooldown_remaining_sec: 125, can_send: false,
                                cannot_send_reason: 'cooldown' });
    assert.equal(g.text, '2:05');
    assert.equal(g.sendDisabled, true);
    assert.equal(g.presenceHint, false);
  });
  // Sweep F178: GET /api/chirp could answer a cooldown with 0 s left (its
  // last second, or the pass after it ran out), and the card said Ready
  // with Send on for a send the device refused. The route rounds up now,
  // but the card turns Send off for any can_send that is not true.
  it('turns Send off for a cooldown that reads 0 s', () => {
    const g = L.chirpSendGate({ ...ready, cooldown_remaining_sec: 0, can_send: false,
                                cannot_send_reason: 'cooldown' });
    assert.equal(g.sendDisabled, true);
    assert.notEqual(g.text, 'Ready');
    assert.equal(g.presenceHint, false);
  });
  it('turns Send off for any can_send false, a reason it has no words for included', () => {
    for (const why of ['disabled', 'some_future_reason', undefined]) {
      const g = L.chirpSendGate({ ...ready, can_send: false, cannot_send_reason: why });
      assert.equal(g.sendDisabled, true, String(why));
      assert.notEqual(g.text, 'Ready', String(why));
    }
    assert.equal(L.chirpSendGate({ ...ready, can_send: undefined }).sendDisabled, true);
  });
});

// What the Chirp list says after a confirm or a dismiss (sweep F174), from
// the answers chirp_api.h's send_confirm_answer() and send_dismiss_answer()
// build (their shapes: test_chirp_commands_wap.cpp and rule CV9).
describe('chirpActionNote', () => {
  it('says why a confirm was refused, in the device\'s words', () => {
    assert.equal(L.chirpActionNote({ success: false, error: 'clock_unsynced',
                                     message: 'Waiting for the clock to be set from GPS time before confirming' }),
                 'Waiting for the clock to be set from GPS time before confirming');
    assert.equal(L.chirpActionNote({ success: false, error: 'not_found' }), 'not_found');
  });
  it('says a dismiss whose vote stayed home hid the chirp here only', () => {
    const m = 'Dismissed on this device only: a suppress vote needs 10 minutes active';
    assert.equal(L.chirpActionNote({ success: true, vote_sent: false, vote_error: 'presence_required', message: m }), m);
    assert.match(L.chirpActionNote({ success: true, vote_sent: false }), /this device only/);
  });
  it('says nothing for a confirm or a dismiss that went out', () => {
    assert.equal(L.chirpActionNote({ success: true }), '');
    assert.equal(L.chirpActionNote({ success: true, vote_sent: true }), '');
  });
  it('says what api() reports when the request itself failed', () => {
    assert.equal(L.chirpActionNote({ ok: false, error: 'Network error' }), 'Network error');
    assert.equal(L.chirpActionNote(null), '');
  });
});

// What the Bluetooth card says of NimBLE's bond store and of a late bring-up
// (#1836's F350; D17 renumbers it), from the keys GET /api/bluetooth writes
// (bluetooth_api.h's handle_bluetooth_status(): bond_store {held, capacity,
// unlisted} only while the store is known, late_bring_up and
// services_missing always).
describe('btStoreLines', () => {
  const API = path.join(__dirname, 'bluetooth_api.h');
  // The keys handle_bluetooth_status() sets, by the object it sets them on.
  function statusKeys() {
    const src = fs.readFileSync(API, 'utf8');
    const at = src.indexOf('inline esp_err_t handle_bluetooth_status(');
    assert.ok(at >= 0, 'handle_bluetooth_status() is in bluetooth_api.h');
    const body = src.slice(at, src.indexOf('\n}\n', at));
    const keys = {};
    for (const [, obj, key] of body.matchAll(/\b(\w+)\["(\w+)"\]/g)) (keys[obj] = keys[obj] || new Set()).add(key);
    return keys;
  }
  // An answer that records every key read of it and of its bond store.
  function recorded(answer) {
    const reads = { doc: new Set(), store: new Set() };
    const watch = (o, into) => new Proxy(o, { get(t, k) { if (typeof k === 'string') into.add(k); return t[k]; } });
    const doc = { ...answer };
    if (answer.bond_store) doc.bond_store = watch({ ...answer.bond_store }, reads.store);
    return { data: watch(doc, reads.doc), reads };
  }
  const none = { bonds: '', unlisted: '', subtitle: '' };
  const lines = (data) => JSON.parse(JSON.stringify(L.btStoreLines(data)));
  const UP = { state: 'advertising', late_bring_up: false, services_missing: [] };

  it('reads only keys the status route writes, on the objects it writes them on', () => {
    const sent = statusKeys();
    for (const k of ['bond_store', 'late_bring_up', 'services_missing']) assert.ok(sent.doc.has(k), k);
    assert.deepEqual([...sent.store].sort(), ['capacity', 'held', 'unlisted']);
    const { data, reads } = recorded({ ...UP, late_bring_up: true, services_missing: ['ble_status'],
                                       bond_store: { held: 8, capacity: 8, unlisted: 2 } });
    L.btStoreLines(data);
    for (const k of reads.doc) assert.ok(sent.doc.has(k), `reads doc.${k}, which the route does not send`);
    for (const k of reads.store) assert.ok(sent.store.has(k), `reads bond_store.${k}, which the route does not send`);
    assert.deepEqual([...reads.store].sort(), ['capacity', 'held', 'unlisted']);
  });
  it('draws nothing while the store is not known (no bond_store key) and the stack came up at boot', () => {
    assert.deepEqual(lines(UP), none);
    assert.deepEqual(lines(null), none);
    assert.deepEqual(lines({ ok: false, error: 'Network error' }), none);
  });
  it('says how full the store is beside the paired list', () => {
    assert.deepEqual(lines({ ...UP, bond_store: { held: 3, capacity: 8, unlisted: 0 } }),
                     { ...none, bonds: '3 of 8 bonds' });
    assert.equal(lines({ ...UP, bond_store: { held: 0, capacity: 8, unlisted: 0 } }).bonds, '0 of 8 bonds');
  });
  it('says what fills a full store that the list does not show, and that Clear All removes it', () => {
    assert.equal(lines({ ...UP, bond_store: { held: 8, capacity: 8, unlisted: 2 } }).unlisted,
                 'The bond store is full, so a new phone cannot pair. 2 of its bonds are not on this list. ' +
                 'Clear All removes them, and the phones listed here too.');
    assert.equal(lines({ ...UP, bond_store: { held: 8, capacity: 8, unlisted: 1 } }).unlisted,
                 'The bond store is full, so a new phone cannot pair. 1 of its bonds is not on this list. ' +
                 'Clear All removes it, and the phones listed here too.');
  });
  it('says nothing of unlisted bonds while the store has room, or when none are unlisted', () => {
    assert.equal(lines({ ...UP, bond_store: { held: 5, capacity: 8, unlisted: 2 } }).unlisted, '');
    assert.equal(lines({ ...UP, bond_store: { held: 8, capacity: 8, unlisted: 0 } }).unlisted, '');
  });
  it('draws no count it cannot read as one', () => {
    for (const bad of [{ held: '3', capacity: 8, unlisted: 0 }, { held: 3, capacity: null, unlisted: 0 },
                       { held: -1, capacity: 8, unlisted: 0 }, { held: 2.5, capacity: 8, unlisted: 0 }, 'x', null]) {
      assert.deepEqual(lines({ ...UP, bond_store: bad }), none, JSON.stringify(bad));
    }
    assert.equal(lines({ ...UP, bond_store: { held: 8, capacity: 8, unlisted: '2' } }).unlisted, '');
  });
  it('names what a late bring-up left off until a reboot', () => {
    assert.equal(lines({ ...UP, late_bring_up: true, services_missing: ['ble_status', 'ble_discovery'] }).subtitle,
                 'Phone status, Opera, Chirp and Nearby off until a reboot');
    assert.equal(lines({ ...UP, late_bring_up: true, services_missing: ['ble_discovery'] }).subtitle,
                 'Opera, Chirp and Nearby off until a reboot');
    assert.equal(lines({ ...UP, late_bring_up: true, services_missing: ['ble_status'] }).subtitle,
                 'Phone status off until a reboot');
    // A name this page has no words for reads as the device sent it.
    assert.equal(lines({ ...UP, late_bring_up: true, services_missing: ['ble_future', '__proto__'] }).subtitle,
                 'ble_future, __proto__ off until a reboot');
  });
  it('says nothing for a boot bring-up that missed a service, or a late one that missed none', () => {
    assert.equal(lines({ ...UP, late_bring_up: false, services_missing: ['ble_status'] }).subtitle, '');
    assert.equal(lines({ ...UP, late_bring_up: true, services_missing: [] }).subtitle, '');
    assert.equal(lines({ ...UP, late_bring_up: 'true', services_missing: ['ble_status'] }).subtitle, '');
  });
});

// The mute and unmute buttons show a refused answer under the Community
// Activity list (sweep F192: a mute or an unmute on a channel that is off is
// 409 chirp_disabled; both answers were thrown away). The two functions are
// the page's glue, outside the WEBUI_LOGIC block, so each is lifted out of
// web_ui.h whole and run with stand-ins for api(), the page and the status
// refresh.
function glueFunction(name) {
  const src = fs.readFileSync(UI, 'utf8');
  const m = new RegExp(`(?:async )?function ${name}\\(`).exec(src);
  if (!m) throw new Error(`${name}() not found in web_ui.h`);
  const start = m.index;
  let depth = 0;
  for (let i = src.indexOf('{', start); i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}' && --depth === 0) return src.slice(start, i + 1);
  }
  throw new Error(`${name}() has no closing brace`);
}
async function runGlue(name, answer, ...args) {
  const calls = [];
  const note = { textContent: 'stale' };
  let refreshed = 0;
  const sandbox = {
    WebUiLogic: L,
    api: async (endpoint, method, body) => { calls.push({ endpoint, method, body }); return answer; },
    document: { getElementById: (id) => (id === 'chirpActionNote' ? note : { textContent: '' }) },
    refreshChirpStatus: () => { refreshed++; },
  };
  vm.runInNewContext(`${glueFunction(name)}\nglobalThis.__run = ${name};`, sandbox);
  await sandbox.__run(...args);
  return { calls, note: note.textContent, refreshed };
}

describe('the Chirp mute and unmute buttons', () => {
  const off = { success: false, error: 'chirp_disabled', message: 'Chirp channel is not enabled' };
  it('a mute on a channel that is off says so', async () => {
    const r = await runGlue('muteChirps', off, 30);
    // Built in the sandbox's realm: compared as JSON, not by prototype.
    assert.equal(JSON.stringify(r.calls),
                 JSON.stringify([{ endpoint: '/api/chirp/mute', method: 'POST', body: { duration_minutes: 30 } }]));
    assert.equal(r.note, 'Chirp channel is not enabled');
    assert.equal(r.refreshed, 1);
  });
  it('an unmute on a channel that is off says so', async () => {
    const r = await runGlue('unmuteChirps', off);
    assert.equal(r.calls.length, 1);
    assert.equal(r.calls[0].endpoint, '/api/chirp/unmute');
    assert.equal(r.note, 'Chirp channel is not enabled');
    assert.equal(r.refreshed, 1);
  });
  it('a mute that runs clears the note', async () => {
    const r = await runGlue('muteChirps', { success: true }, 15);
    assert.equal(r.note, '');
    assert.equal(r.refreshed, 1);
  });
  it('a duration the channel lacks says which it offers', async () => {
    const r = await runGlue('muteChirps', { success: false, error: 'invalid_duration',
                                            message: 'Duration must be 15, 30, 60, or 120 minutes' }, 7);
    assert.equal(r.note, 'Duration must be 15, 30, 60, or 120 minutes');
  });
});

// The Bluetooth card after a tap brings a refused stack up (sweep F235):
// refreshBtStatus() put "Radio off — <reason>" in the card's subtitle while
// GET /api/bluetooth named a refusal, and nothing put the card's own line
// back once a bring-up cleared it, so the card read "Advertising" under the
// old refusal until a reload. refreshBtStatus() is the page's glue, lifted
// out of web_ui.h whole and run against stand-ins for api(), the page's
// elements and the poll timer.
describe('the Bluetooth card after a refused stack comes up', () => {
  function cardMarkupSubtitle() {
    const src = fs.readFileSync(UI, 'utf8');
    const m = src.match(/id="btSubtitle">([^<]*)</);
    if (!m) throw new Error('btSubtitle not found in web_ui.h markup');
    return m[1];
  }
  function bluetoothPage() {
    const els = new Map();
    const el = (id) => {
      if (!els.has(id)) {
        els.set(id, { id, textContent: '', innerHTML: '', className: '', checked: false,
                      style: {}, dataset: {} });
      }
      return els.get(id);
    };
    return { el, document: { getElementById: el } };
  }
  async function refreshWith(page, answer) {
    const sandbox = {
      WebUiLogic: L,
      api: async () => answer,
      document: page.document,
      window: {},
      setInterval: () => 1,
      clearInterval: () => {},
      distancePretty: () => '',
      rssiBarCount: () => 0,
    };
    vm.runInNewContext(`${glueFunction('refreshBtStatus')}\nglobalThis.__run = refreshBtStatus;`, sandbox);
    await sandbox.__run();
  }
  const base = { enabled: true, device_name: 'Canary', tx_power: 9, paired_count: 0, scanned_count: 0,
                 connected: false };
  it('names the refusal while there is one', async () => {
    const page = bluetoothPage();
    page.el('btSubtitle').textContent = cardMarkupSubtitle();
    await refreshWith(page, { ...base, state: 'disabled', advertising: false,
                              init_fail_reason: 'Not started: internal RAM too low' });
    assert.equal(page.el('btSubtitle').textContent, 'Radio off — Not started: internal RAM too low');
    assert.equal(page.el('btStateText').textContent, 'Off');
  });
  it('puts the card\'s own line back once a bring-up clears it', async () => {
    const page = bluetoothPage();
    page.el('btSubtitle').textContent = cardMarkupSubtitle();
    await refreshWith(page, { ...base, state: 'disabled', advertising: false,
                              init_fail_reason: 'Not started: internal RAM too low' });
    // The owner taps Start Advertising and the stack comes up: no reason now.
    await refreshWith(page, { ...base, state: 'advertising', advertising: true });
    assert.equal(page.el('btSubtitle').textContent, cardMarkupSubtitle());
    assert.equal(page.el('btSubtitle').textContent, 'BLE connectivity');
    assert.equal(page.el('btStateText').textContent, 'Advertising');
    assert.equal(page.el('btStateBadge').className, 'badge info');
  });
  // A refusal can clear on a status saying Bluetooth is off (a tap brought
  // the stack up, and the owner then turned it off before the next poll):
  // the card's own line comes back there too. Restored only while enabled,
  // the card would read "Radio off — <old refusal>" under a Disabled badge
  // (wave 16's verify).
  it('puts it back when the refusal clears on a status saying Bluetooth is off', async () => {
    const page = bluetoothPage();
    page.el('btSubtitle').textContent = cardMarkupSubtitle();
    await refreshWith(page, { ...base, state: 'disabled', advertising: false,
                              init_fail_reason: 'Not started: internal RAM too low' });
    assert.equal(page.el('btSubtitle').textContent, 'Radio off — Not started: internal RAM too low');
    await refreshWith(page, { ...base, enabled: false, state: 'disabled', advertising: false });
    assert.equal(page.el('btSubtitle').textContent, 'BLE connectivity');
    assert.equal(page.el('btStateText').textContent, 'Disabled');
    assert.equal(page.el('btEnabled').checked, false);
  });
  it('puts it back after a refresh that found no backend, too', async () => {
    const page = bluetoothPage();
    await refreshWith(page, null);                    // a failed request: "not registered"
    assert.match(page.el('btSubtitle').textContent, /not registered/);
    await refreshWith(page, { ...base, state: 'idle', advertising: false });
    assert.equal(page.el('btSubtitle').textContent, 'BLE connectivity');
  });
  // The bond store's lines and a late bring-up's clause (#1836's F350).
  it('shows the bond store beside the paired list, and the note only when one is owed', async () => {
    const page = bluetoothPage();
    await refreshWith(page, { ...base, state: 'idle', advertising: false,
                              bond_store: { held: 3, capacity: 8, unlisted: 0 } });
    assert.equal(page.el('btBondStore').textContent, '3 of 8 bonds');
    assert.equal(page.el('btBondStoreNote').style.display, 'none');
    await refreshWith(page, { ...base, state: 'idle', advertising: false,
                              bond_store: { held: 8, capacity: 8, unlisted: 2 } });
    assert.equal(page.el('btBondStore').textContent, '8 of 8 bonds');
    assert.equal(page.el('btBondStoreNote').style.display, '');
    assert.match(page.el('btBondStoreNote').textContent, /^The bond store is full.*Clear All removes them/);
    // The store not known (a stack not up): nothing, and no stale count.
    await refreshWith(page, { ...base, state: 'disabled', advertising: false });
    assert.equal(page.el('btBondStore').textContent, '');
    assert.equal(page.el('btBondStoreNote').textContent, '');
    assert.equal(page.el('btBondStoreNote').style.display, 'none');
  });
  it('clears the bond store\'s lines on a refresh that found no backend', async () => {
    const page = bluetoothPage();
    await refreshWith(page, { ...base, state: 'idle', advertising: false,
                              bond_store: { held: 8, capacity: 8, unlisted: 1 } });
    await refreshWith(page, null);
    assert.equal(page.el('btBondStore').textContent, '');
    assert.equal(page.el('btBondStoreNote').style.display, 'none');
  });
  it('names what a late bring-up left off until a reboot, after the card\'s own line', async () => {
    const page = bluetoothPage();
    page.el('btSubtitle').textContent = cardMarkupSubtitle();
    await refreshWith(page, { ...base, state: 'advertising', advertising: true, late_bring_up: true,
                              services_missing: ['ble_status', 'ble_discovery'] });
    assert.equal(page.el('btSubtitle').textContent,
                 'BLE connectivity · Phone status, Opera, Chirp and Nearby off until a reboot');
    await refreshWith(page, { ...base, state: 'advertising', advertising: true, late_bring_up: false,
                              services_missing: [] });
    assert.equal(page.el('btSubtitle').textContent, 'BLE connectivity');
  });
  it('the bond store\'s lines are in the markup, the note hidden until one is owed', () => {
    const src = fs.readFileSync(UI, 'utf8');
    const card = src.slice(src.indexOf('<div class="card-title">Paired Devices</div>'), src.indexOf('id="btPairedList"'));
    assert.match(card, /<div class="card-subtitle" id="btBondStore"><\/div>/);
    assert.match(card, /<p id="btBondStoreNote" style="display:none;[^"]*"><\/p>/);
  });
});

// The Bluetooth page's lists draw what a peer or a bond carries as text
// (sweep F407). loadBtPairedDevices() wrote a bond's name and address into
// innerHTML as they came, and spliced the address into the Remove button's
// onclick string, where escapeHtml() (it leaves quotes alone) could not have
// helped. The functions are lifted out of web_ui.h whole, beside the page's
// own escapeHtml() (glueFunction() lifts a plain function as well as an async
// one), and the markup they write is read as a browser parses it
// (html_fragment.js): no element, attribute or comment but the row's own.
const settleAll = () => new Promise((r) => setImmediate(r));

describe('the paired list draws each bond as text, and Remove posts its address (F407)', () => {
  function pairedPage(answers) {
    const src = fs.readFileSync(UI, 'utf8');
    const els = new Map();
    const el = (id) => {
      if (!els.has(id)) els.set(id, { id, textContent: '', innerHTML: '' });
      return els.get(id);
    };
    const calls = [];
    let served = 0;
    const ctx = {
      api: async (endpoint, method, body) => {
        calls.push({ endpoint, method: method || 'GET', body });
        if (endpoint === '/api/bluetooth/paired' && !method) {
          const devices = answers[Math.min(served++, answers.length - 1)];
          return { count: devices.length, devices };
        }
        return { success: true };
      },
      document: { getElementById: el, createElement: () => textElement() },
      confirm: () => true,
    };
    vm.createContext(ctx);
    // The list's own state where the page keeps one (an older page has none).
    const decl = (src.match(/\n    let btPairedShown = [^\n]*\n/) || [''])[0];
    vm.runInContext(`${decl}${glueFunction('escapeHtml')}\n${glueFunction('loadBtPairedDevices')}\n` +
      `${glueFunction('btRemovePaired')}\n`, ctx);
    return {
      calls,
      load: () => ctx.loadBtPairedDevices(),
      list: () => parseFragment(el('btPairedList').innerHTML),
      // A tap on an element: its onclick attribute run as the page's script.
      tap: async (button) => {
        const code = attr(button, 'onclick');
        assert.ok(code, `${shape(button)} has an onclick`);
        await vm.runInContext(code, ctx);
        await settleAll();
      },
      deletes: () => calls.filter((c) => c.method === 'DELETE').map((c) => c.body && c.body.address),
    };
  }
  const BENIGN = [{ name: 'Pixel 8', address: 'aa:bb:cc:dd:ee:ff' }];
  async function rowOf(devices) {
    const p = pairedPage([devices]);
    await p.load();
    return p.list();
  }

  it('a bond row is a box, its name, its address and Remove', async () => {
    const f = await rowOf(BENIGN);
    assert.deepEqual(f.elements.map(shape),
                     ['div[class style]', 'div[]', 'strong[]', 'div[style]', 'button[class onclick]']);
    assert.equal(f.elements[2].text, 'Pixel 8');
    assert.equal(f.elements[3].text, 'aa:bb:cc:dd:ee:ff');
    assert.equal(f.elements[4].text, 'Remove');
  });

  it('a name or an address that reads as markup makes no element, attribute or comment', async () => {
    const own = (await rowOf(BENIGN)).elements;
    const devices = HOSTILE.map((h, k) => ({ name: h, address: HOSTILE[(k + 1) % HOSTILE.length] }));
    const p = pairedPage([devices]);
    await p.load();
    const f = p.list();
    assert.deepEqual(f.elements.map(shape), devices.flatMap(() => own.map(shape)),
                     'each row has exactly the row\'s own elements and attributes');
    assert.deepEqual(f.comments, [], 'no comment opened');
    devices.forEach((d, k) => {
      const row = f.elements.slice(k * own.length, (k + 1) * own.length);
      row.forEach((e, j) => {
        for (const [n, v] of e.attrs) {
          if (n !== 'onclick') assert.equal(v, attr(own[j], n), `row ${k}: ${e.tag} ${n}`);
          assert.ok(!v.includes(d.name) && !v.includes(d.address), `row ${k}: no bond text in ${e.tag} ${n}`);
        }
      });
      assert.equal(row[2].text, d.name, `row ${k}: the name reads as sent`);
      assert.equal(row[3].text, d.address, `row ${k}: the address reads as sent`);
    });
  });

  it('Remove posts an address that holds a quote as it was listed', async () => {
    for (const address of ["aa:bb'cc", 'aa:bb"cc', "x'); alert(1);//"]) {
      const p = pairedPage([[{ name: 'Phone', address }]]);
      await p.load();
      const [button] = p.list().elements.filter((e) => e.tag === 'button');
      await p.tap(button);
      assert.deepEqual(p.deletes(), [address]);
    }
  });

  it('Remove on each row posts exactly that row\'s address', async () => {
    const devices = HOSTILE.map((h) => ({ name: 'Phone', address: h }));
    const p = pairedPage([devices]);
    await p.load();
    const buttons = p.list().elements.filter((e) => e.tag === 'button');
    assert.equal(buttons.length, devices.length);
    for (const [k, b] of buttons.entries()) {
      await p.tap(b);
      assert.equal(p.deletes().at(-1), devices[k].address, `row ${k}`);
    }
    assert.deepEqual(p.deletes(), devices.map((d) => d.address));
  });

  it('a bond with no name reads Unknown', async () => {
    const f = await rowOf([{ name: '', address: 'aa:bb:cc:dd:ee:ff' }]);
    assert.equal(f.elements[2].text, 'Unknown');
  });

  it('Remove names the bond the list shows now, after the list has changed', async () => {
    const first = [{ name: 'A', address: '11:11:11:11:11:11' }, { name: 'B', address: '22:22:22:22:22:22' }];
    const then = [{ name: 'B', address: '22:22:22:22:22:22' }];
    const p = pairedPage([first, then]);
    await p.load();
    await p.load();                       // the list as the device now answers it
    const buttons = p.list().elements.filter((e) => e.tag === 'button');
    assert.equal(buttons.length, 1);
    await p.tap(buttons[0]);
    assert.deepEqual(p.deletes(), ['22:22:22:22:22:22']);
  });
});

describe('the nearby scan list draws every answer field as text (F407)', () => {
  function scanList(devices) {
    const els = new Map();
    const el = (id) => {
      if (!els.has(id)) els.set(id, { id, textContent: '', innerHTML: '', dataset: {} });
      return els.get(id);
    };
    const ctx = { document: { getElementById: el, createElement: () => textElement() } };
    vm.createContext(ctx);
    vm.runInContext(['escapeHtml', 'rssiBarCount', 'distancePretty', 'deviceIcon', 'renderBtScanList']
      .map(glueFunction).join('\n'), ctx);
    ctx.renderBtScanList(devices);
    return parseFragment(el('btScanList').innerHTML);
  }
  const plain = { name: 'Watch', address: '11:22:33:44:55:66', rssi: -60, distance_m: 2.5,
                  distance_label: 'Near', type: 'wearable', is_securacv: false };

  it('a name, an address, a distance word or a signal that reads as markup makes nothing', () => {
    const own = scanList([plain]).elements;
    for (const h of HOSTILE) {
      const f = scanList([{ ...plain, name: h, address: h, distance_label: h, rssi: h, type: h }]);
      assert.deepEqual(f.comments, [], h.slice(0, 30));
      assert.deepEqual(f.elements.map(shape), own.map(shape), h.slice(0, 30));
      const meta = f.elements.find((e) => attr(e, 'class') === 'ble-name');
      const sub = f.elements.find((e) => attr(e, 'class') === 'ble-sub');
      const word = f.elements.find((e) => attr(e, 'class') === 'ble-dist-l');
      assert.equal(meta.text, h);
      assert.equal(sub.text, `${h} · ${h} dBm`);
      assert.equal(word.text, h);
    }
  });
});

// The household list draws each paired device's label as text, and its
// forget button and role picker name their row by its index (sweep F423,
// F407's shape). refreshHousehold() wrote the label into innerHTML as it came
// and spliced it into the forget button's onclick string with only its single
// quotes escaped, so a double quote ended the attribute and a trailing
// backslash ate the string's closing quote. The label is NVS's
// (household::get_label(); ArduinoJson escapes it for JSON, not for HTML).
// The functions are lifted out of web_ui.h whole, beside the page's own
// escapeHtml(), fmtAgo() and roleBadgeStyle(), and the list's state and the
// name helper where the page keeps them (an older page has neither).
describe('the household list draws each label as text, and forget posts its row\'s slot (F423)', () => {
  function householdPage(answers) {
    const src = fs.readFileSync(UI, 'utf8');
    const els = new Map();
    const el = (id) => {
      if (!els.has(id)) els.set(id, { id, textContent: '', innerHTML: '' });
      return els.get(id);
    };
    const calls = [];
    const confirms = [];
    let served = 0;
    const ctx = {
      api: async (endpoint, method, body) => {
        calls.push({ endpoint, method: method || 'GET', body });
        if (endpoint === '/api/household' && !method) {
          return { devices: answers[Math.min(served++, answers.length - 1)] };
        }
        return { success: true };
      },
      document: { getElementById: el, createElement: () => textElement() },
      confirm: (m) => { confirms.push(m); return true; },
      alert: () => {},
      refreshPresence: () => {},
    };
    vm.createContext(ctx);
    const decl = (src.match(/\n    let householdShown = [^\n]*\n/) || [''])[0];
    const helpers = /function householdName\(/.test(src) ? glueFunction('householdName') : '';
    vm.runInContext([decl, helpers, ...['escapeHtml', 'fmtAgo', 'roleBadgeStyle', 'refreshHousehold',
      'setHouseholdRole', 'forgetHousehold'].map(glueFunction)].join('\n'), ctx);
    return {
      calls, confirms,
      load: () => ctx.refreshHousehold(),
      list: () => parseFragment(el('householdList').innerHTML),
      // An element's handler run as the page's script, `this` the element.
      fire: async (element, handler, value) => {
        const code = attr(element, handler);
        assert.ok(code, `${shape(element)} has an ${handler}`);
        // The value reaches the handler as a context global, never spliced
        // into the code string (CodeQL js/bad-code-sanitization: a
        // JSON.stringify'd value is not a safe code fragment).
        ctx.__fireValue = value;
        await vm.runInContext(`(function () { return ${code}; }).call({ value: __fireValue })`, ctx);
        delete ctx.__fireValue;
        await settleAll();
      },
      forgets: () => calls.filter((c) => c.method === 'DELETE').map((c) => JSON.stringify(c.body)),
      roles: () => calls.filter((c) => c.endpoint === '/api/household/role').map((c) => JSON.stringify(c.body)),
    };
  }
  const PLAIN = { slot: 3, label: 'Kim\'s phone', role: 'guest', paired_since_ms: 0, last_seen_ago_ms: 65000 };
  async function rowOf(devices) {
    const p = householdPage([devices]);
    await p.load();
    return p.list();
  }
  const buttonOf = (f) => f.elements.filter((e) => e.tag === 'button');

  it('a household row is its name, when it was seen, its role, the role picker and forget', async () => {
    const f = await rowOf([PLAIN]);
    assert.deepEqual(f.elements.map(shape),
                     ['div[style]', 'div[style]', 'div[style]', 'div[style]', 'span[class style]',
                      'select[onchange style]', 'option[value selected]', 'option[value]', 'option[value]',
                      'button[class onclick title style]']);
    assert.equal(f.elements[2].text, 'Kim\'s phone');
    assert.equal(f.elements[3].text, 'seen 1 min ago');
    assert.equal(f.elements[4].text, 'guest');
  });

  // What the old splice broke: a double quote ended the onclick attribute,
  // so the rest of the label became attributes of its own (a second onclick
  // is dropped, and the first then holds a cut string; any other name is a
  // new handler), and a trailing backslash escaped the quote that closed the
  // label's string.
  for (const label of ['x" onclick="alert(1)', 'x" onmouseover="alert(1)', 'Phone\\']) {
    it(`a label ${JSON.stringify(label)} is text, and forget posts its row's slot`, async () => {
      const own = (await rowOf([PLAIN])).elements;
      const p = householdPage([[{ ...PLAIN, slot: 4, label }]]);
      await p.load();
      const f = p.list();
      assert.deepEqual(f.elements.map(shape), own.map(shape));
      assert.equal(f.elements[2].text, label);
      await p.fire(buttonOf(f)[0], 'onclick');
      assert.deepEqual(p.forgets(), [JSON.stringify({ slot: 4 })]);
    });
  }

  // Every field the row draws from the answer goes through escapeHtml():
  // the label, the slot a row with no label is named by, and the role badge
  // (role_name()'s vocabulary today, so the badge's escape is defense in
  // depth; a suite that sends hostile text only in the label would not see
  // it dropped). A role the picker does not offer selects no option, so the
  // hostile rows' own shape is a row with such a role.
  it('a label, slot or role that reads as markup makes no element, attribute or comment', async () => {
    const own = (await rowOf([{ ...PLAIN, role: 'visitor' }])).elements;
    const hostile = ['x" onclick="alert(1)', 'Phone\\', ...HOSTILE];
    const devices = hostile.map((text, k) => (k % 2 === 0)
      ? { ...PLAIN, slot: k + 1, label: text, role: hostile[(k + 1) % hostile.length] }
      : { ...PLAIN, slot: text, label: '', role: hostile[(k + 1) % hostile.length] });
    const p = householdPage([devices]);
    await p.load();
    const f = p.list();
    assert.deepEqual(f.elements.map(shape), devices.flatMap(() => own.map(shape)),
                     'each row has exactly the row\'s own elements and attributes');
    assert.deepEqual(f.comments, [], 'no comment opened');
    devices.forEach((d, k) => {
      const row = f.elements.slice(k * own.length, (k + 1) * own.length);
      // The hostile text this row carries (a numeric slot is a digit any
      // style attribute may hold, so only a string slot is held out).
      const sent = { label: d.label, role: d.role, slot: typeof d.slot === 'string' ? d.slot : '' };
      row.forEach((e, j) => {
        for (const [n, v] of e.attrs) {
          if (n !== 'onclick' && n !== 'onchange') assert.equal(v, attr(own[j], n), `row ${k}: ${e.tag} ${n}`);
          for (const [field, text] of Object.entries(sent)) {
            if (text) assert.ok(!v.includes(text), `row ${k}: no ${field} in ${e.tag} ${n}`);
          }
        }
      });
      assert.equal(row[2].text, d.label || 'Slot ' + d.slot, `row ${k}: the name reads as sent`);
      assert.equal(row[4].text, d.role, `row ${k}: the role reads as sent`);
    });
    // A slot the row is named by goes to the forget route as the list holds it.
    await p.fire(buttonOf(f)[1], 'onclick');
    assert.deepEqual(p.forgets(), [JSON.stringify({ slot: devices[1].slot })]);
  });

  it('forget posts the slot of the row clicked, and its confirm names the label as sent', async () => {
    const labels = ['x" onclick="alert(1)', 'Phone\\', ...HOSTILE];
    const devices = labels.map((label, k) => ({ ...PLAIN, slot: 7 - (k % 8), label }));
    const p = householdPage([devices]);
    await p.load();
    const buttons = buttonOf(p.list());
    assert.equal(buttons.length, devices.length);
    for (const [k, b] of buttons.entries()) {
      await p.fire(b, 'onclick');
      assert.equal(p.forgets().at(-1), JSON.stringify({ slot: devices[k].slot }), `row ${k}`);
      assert.equal(p.confirms.at(-1),
                   `Forget ${devices[k].label}? It will need to re-pair to be recognized again.`, `row ${k}`);
    }
    assert.equal(p.forgets().length, devices.length);
  });

  it('the role picker posts the slot of its row and the role picked', async () => {
    const devices = [{ ...PLAIN, slot: 5, label: 'x" onclick="alert(1)' }, { ...PLAIN, slot: 0, label: 'Phone\\' }];
    const p = householdPage([devices]);
    await p.load();
    const pickers = p.list().elements.filter((e) => e.tag === 'select');
    await p.fire(pickers[0], 'onchange', 'owner');
    await p.fire(pickers[1], 'onchange', 'family');
    assert.deepEqual(p.roles(), [JSON.stringify({ slot: 5, role: 'owner' }), JSON.stringify({ slot: 0, role: 'family' })]);
  });

  it('a device with no label reads as its slot, and its confirm as this paired device', async () => {
    const p = householdPage([[{ ...PLAIN, slot: 6, label: '' }]]);
    await p.load();
    const f = p.list();
    assert.equal(f.elements[2].text, 'Slot 6');
    await p.fire(buttonOf(f)[0], 'onclick');
    assert.equal(p.confirms.at(-1), 'Forget this paired device? It will need to re-pair to be recognized again.');
    assert.deepEqual(p.forgets(), [JSON.stringify({ slot: 6 })]);
  });

  it('forget names the device the list shows now, after the list has changed', async () => {
    const first = [{ ...PLAIN, slot: 1, label: 'A' }, { ...PLAIN, slot: 2, label: 'B' }];
    const then = [{ ...PLAIN, slot: 2, label: 'B' }];
    const p = householdPage([first, then]);
    await p.load();
    await p.load();
    const buttons = buttonOf(p.list());
    assert.equal(buttons.length, 1);
    await p.fire(buttons[0], 'onclick');
    assert.deepEqual(p.forgets(), [JSON.stringify({ slot: 2 })]);
  });
});
