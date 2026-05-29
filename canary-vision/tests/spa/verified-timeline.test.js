'use strict';

// Unit tests for the SPA's verified-timeline helpers (Phase 4). app.js is browser-only and has no
// module exports, so we load it in a VM sandbox with minimal browser-global stubs and exercise the
// pure functions directly — covering the envelope→timeline logic without a real DOM. This mirrors
// the viewer's smoke-test approach and keeps app.js free of test-only code.

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');

const { buildEvidenceEnvelope } = require('../../device-api/lib/envelope-bridge');
const { computeHash, signHash } = require('../../device-api/lib/witness-chain');

// Load app.js into a sandbox. Stub just enough of the browser surface that top-level code
// (a DOMContentLoaded registration + a few constructors referenced at parse time) doesn't throw.
function loadApp() {
  const code = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');
  const noop = () => {};
  const sandbox = {
    document: { addEventListener: noop, getElementById: () => null, createElement: () => ({}) },
    window: { addEventListener: noop, location: { hash: '' }, history: {}, matchMedia: () => ({ matches: false, addEventListener: noop }) },
    localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
    sessionStorage: { getItem: () => null, setItem: noop, removeItem: noop },
    navigator: { onLine: true },
    setInterval: noop, clearInterval: noop, setTimeout: noop, clearTimeout: noop,
    fetch: noop, console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(code, sandbox);
  return sandbox;
}

function signedChain(privateKey, specs) {
  const records = [];
  let prevHash = '0'.repeat(64);
  specs.forEach((s, i) => {
    const hash = computeHash(i + 1, prevHash, s.timestamp, s.event_type, s.zone, 'device_clock', '');
    records.push({ seq: i + 1, hash, prev_hash: prevHash, timestamp: s.timestamp,
      event_type: s.event_type, zone: s.zone, signature: signHash(hash, privateKey), time_source: 'device_clock' });
    prevHash = hash;
  });
  return records;
}

describe('SPA verified-timeline helpers', () => {
  let app;
  before(() => { app = loadApp(); });

  it('exposes the helper functions and event metadata', () => {
    assert.equal(typeof app.envelopeTimelineEvents, 'function');
    assert.equal(typeof app.formatTimeBucket, 'function');
    assert.equal(typeof app.envelopeEventMeta, 'function');
    assert.equal(typeof app.ENVELOPE_EVENT_META, 'object');
  });

  it('handles null / malformed envelopes without throwing', () => {
    // (Arrays cross the VM realm boundary, so assert on length rather than deepEqual reference.)
    assert.equal(app.envelopeTimelineEvents(null).length, 0);
    assert.equal(app.envelopeTimelineEvents({}).length, 0);
    assert.equal(app.envelopeTimelineEvents({ ledgers: {} }).length, 0);
    // A payload_json of literal "null" or invalid JSON must not crash the loop.
    const env = { ledgers: { sealed_events: { entries: [{ payload_json: 'null' }, { payload_json: '{bad' }] } } };
    assert.equal(app.envelopeTimelineEvents(env).length, 0);
  });

  it('maps kernel EventType variants to friendly labels', () => {
    assert.equal(app.envelopeEventMeta('VehiclePresenceAfterHours').label, 'Vehicle (after hours)');
    assert.equal(app.envelopeEventMeta('BoundaryCrossingObjectLarge').cssClass, 'type-person');
    // Unknown variant falls back without throwing.
    const fallback = app.envelopeEventMeta('SomethingNew');
    assert.equal(fallback.label, 'SomethingNew');
    assert.ok(fallback.cssClass);
  });

  it('formats a coarse time bucket as a UTC window, never a precise instant', () => {
    const out = app.formatTimeBucket({ start_epoch_s: 1780056000, size_s: 600 });
    assert.match(out, /UTC/);
    assert.match(out, /10 min window/);
    assert.doesNotMatch(out, /\d{2}:\d{2}:\d{2}/, 'must not expose seconds-level precision');
    assert.equal(app.formatTimeBucket(null), '—');
  });

  it('extracts verified events from a real envelope, newest first', async () => {
    const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
    const t0 = Date.parse('2026-05-29T12:00:00.000Z');
    const records = signedChain(privateKey, [
      { zone: 'front', event_type: 'person_detected', timestamp: new Date(t0).toISOString() },
      { zone: 'drive', event_type: 'vehicle_detected', timestamp: new Date(t0 + 700000).toISOString() },
    ]);
    const env = await buildEvidenceEnvelope({ records, publicKey, privateKey,
      device: { device_id: 'x', name: 'y', firmware_version: '1' }, nowMs: t0 });

    const events = app.envelopeTimelineEvents(env);
    assert.equal(events.length, 2);
    // newest first
    assert.ok(events[0].time_bucket.start_epoch_s >= events[1].time_bucket.start_epoch_s);
    // coarsened: carries zone + bucket, never precise timestamp / gps / thumbnail
    events.forEach((ev) => {
      assert.ok(ev.type && ev.time_bucket);
      assert.ok(!('timestamp' in ev) && !('gps_timestamp' in ev) && !('thumbnail' in ev));
    });
    const types = events.map((e) => e.type);
    assert.ok(types.includes('VehiclePresenceAfterHours'));
    assert.ok(types.includes('BoundaryCrossingObjectLarge'));
  });
});
