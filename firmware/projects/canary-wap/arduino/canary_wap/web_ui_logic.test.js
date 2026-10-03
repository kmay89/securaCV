'use strict';

// Unit tests for the admin dashboard's pure helpers (camera peek URL,
// bounded-retry policy, BLE chirp endpoint). The logic lives in a DOM-free
// block inside web_ui.h fenced by `WEBUI_LOGIC:BEGIN/END` and self-exported
// under Node — same extraction pattern as selftest_ui.test.js — so these
// tests exercise the SAME source the firmware ships.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/web_ui_logic.test.js

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const UI = path.join(__dirname, 'web_ui.h');

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
