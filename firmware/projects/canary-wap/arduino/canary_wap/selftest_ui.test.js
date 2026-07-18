'use strict';

// Unit tests for the Canary setup wizard's pre-flight self-test UI logic.
//
// The decision logic (status icons/labels, the per-probe "what to look into"
// hint matrix, the pass/fail verdict copy) lives in a pure, DOM-free block
// inside companion_pwa.h, fenced by `SELFTEST_LOGIC:BEGIN/END` markers and
// self-exported via `module.exports` when run under Node. We extract that
// exact block and evaluate it here, so these tests exercise the SAME source
// the firmware ships — no duplicated copy to drift.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/selftest_ui.test.js

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const PWA = path.join(__dirname, 'companion_pwa.h');

function loadLogic() {
  const src = fs.readFileSync(PWA, 'utf8');
  // Capture the whole fenced block INCLUDING the delimiting /* … */ comments
  // so the slice is valid standalone JS.
  const m = src.match(/\/\*\s*SELFTEST_LOGIC:BEGIN[\s\S]*?SELFTEST_LOGIC:END\s*\*\//);
  if (!m) throw new Error('SELFTEST_LOGIC block not found in companion_pwa.h');
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(m[0], sandbox, { filename: 'selftest_logic.extracted.js' });
  const L = sandbox.module.exports;
  if (!L || typeof L.hintFor !== 'function') {
    throw new Error('extracted block did not export the expected SelftestLogic API');
  }
  return L;
}

// Convenience: build a probe object the way /api/selftest emits them.
const probe = (name, status, metric) => ({ name, status, metric: metric || {} });

// Every status the firmware can emit (selftest::status_name).
const STATUSES = ['pass', 'fail', 'skip', 'absent', 'unknown'];

// Every probe name the firmware emits, in the order run_to_json() pushes
// them, plus the synthetic 'fetch' row the UI injects when /api/selftest is
// unreachable. Kept in sync with selftest_api.h's ORDER[].
const PROBE_NAMES = [
  'wifi', 'camera', 'bluetooth', 'gps', 'sd',
  'power', 'microphone', 'buzzer', 'tamper', 'gpio', 'fetch',
];

let L;
before(() => { L = loadLogic(); });

describe('icons and labels', () => {
  it('defines a glyph and a spoken label for every status', () => {
    for (const s of STATUSES) {
      assert.equal(typeof L.ICON[s], 'string', `ICON[${s}]`);
      assert.ok(L.ICON[s].length >= 1, `ICON[${s}] non-empty`);
      assert.equal(typeof L.ICON_LABEL[s], 'string', `ICON_LABEL[${s}]`);
      assert.ok(L.ICON_LABEL[s].length > 0, `ICON_LABEL[${s}] non-empty`);
    }
  });
});

describe('leadFor', () => {
  it('uses "What to do:" only for failures, "Note:" otherwise', () => {
    assert.equal(L.leadFor('fail'), 'What to do: ');
    assert.equal(L.leadFor('FAIL'), 'What to do: ');
    for (const s of ['pass', 'skip', 'absent', 'unknown']) {
      assert.equal(L.leadFor(s), 'Note: ', `leadFor(${s})`);
    }
  });
});

describe('verdict', () => {
  it('reports ready only when all_passed is true', () => {
    const ok = L.verdict({ all_passed: true });
    assert.match(ok.heading, /ready/i);
    assert.ok(ok.sub.length > 0);

    const bad = L.verdict({ all_passed: false });
    assert.match(bad.heading, /attention/i);
    assert.ok(bad.sub.length > 0);

    // Missing/garbage report is treated as not-passed (fail-safe).
    assert.match(L.verdict(undefined).heading, /attention/i);
    assert.match(L.verdict({}).heading, /attention/i);
  });
});

describe('safeModeNote', () => {
  it('shows a recovery banner only when the report flags safe_mode', () => {
    assert.equal(L.safeModeNote({ safe_mode: false }), '');
    assert.equal(L.safeModeNote({}), '');
    assert.equal(L.safeModeNote(undefined), '');
    const note = L.safeModeNote({ safe_mode: true });
    assert.ok(note.length > 0);
    // Explains grey rows as paused-not-broken and how to clear it.
    assert.match(note, /recovery/i);
    assert.match(note, /paused/i);
    assert.doesNotMatch(note, /faulty/i);
  });
});

describe('expansionNote — Sense expansion-board correlator', () => {
  const probes = (cam, mic, sd) => ({ probes: [
    { name: 'camera',     status: cam },
    (typeof mic === 'object') ? Object.assign({ name: 'microphone' }, mic)
                              : { name: 'microphone', status: mic },
    { name: 'sd',         status: sd },
  ]});
  // The mic's real-world bring-up failure shape: never 'fail' (optional
  // peripherals must not gate setup) — 'skip' with a negative code.
  const MIC_DOWN = { status: 'skip', code: -1 };
  it('fires on the bench scenario: camera absent + mic skip(code<0) + SD absent', () => {
    const note = L.expansionNote(probes('absent', MIC_DOWN, 'absent'));
    assert.ok(note.length > 0);
    assert.match(note, /expansion board/i);
    assert.match(note, /reseat/i);
    // Also fires on hard fails.
    assert.ok(L.expansionNote(probes('fail', 'fail', 'fail')).length > 0);
  });
  it('stays silent when any of the three is healthy or benignly paused', () => {
    assert.equal(L.expansionNote(probes('pass', MIC_DOWN, 'absent')), '');
    assert.equal(L.expansionNote(probes('absent', 'pass', 'absent')), '');
    assert.equal(L.expansionNote(probes('absent', MIC_DOWN, 'pass')), '');
    // Muted-by-user is deliberate, not evidence (skip, code 0).
    assert.equal(L.expansionNote(probes('absent',
        { status: 'skip', code: 0, metric: { muted: true } }, 'absent')), '');
    // A code-less skip (e.g. still starting) does not convict.
    assert.equal(L.expansionNote(probes('absent', { status: 'skip' }, 'absent')), '');
    // SD merely skipped (safe mode) does not convict the board either.
    assert.equal(L.expansionNote(probes('absent', MIC_DOWN, 'skip')), '');
  });
  it('survives missing/garbage reports', () => {
    assert.equal(L.expansionNote(undefined), '');
    assert.equal(L.expansionNote({}), '');
    assert.equal(L.expansionNote({ probes: 'nope' }), '');
    assert.equal(L.expansionNote({ probes: [null, {}] }), '');
  });
});

describe('hintFor — bluetooth crash evidence', () => {
  it('names the crash stage and reason in the recovery note', () => {
    const p = { name: 'bluetooth', status: 'skip',
                metric: { safe_mode: true,
                          last_crash_stage: 'loop:ble-finalize',
                          last_crash_reason: 'task-watchdog' } };
    const note = L.hintFor(p);
    assert.match(note, /loop:ble-finalize/);
    assert.match(note, /task-watchdog/);
  });
  it('keeps the plain recovery note when no evidence is recorded', () => {
    const p = { name: 'bluetooth', status: 'skip', metric: { safe_mode: true } };
    const note = L.hintFor(p);
    assert.match(note, /recovery/i);
    assert.doesNotMatch(note, /crash \(/i);
  });
});

describe('hintFor — robustness', () => {
  it('never throws and returns a string for any input', () => {
    const inputs = [null, undefined, {}, { name: 'nope', status: 'fail' }, probe('', 'fail')];
    for (const p of inputs) {
      const h = L.hintFor(p);
      assert.equal(typeof h, 'string', `hintFor(${JSON.stringify(p)})`);
    }
  });

  it('returns nothing for an unknown probe name', () => {
    assert.equal(L.hintFor(probe('flux-capacitor', 'fail')), '');
  });

  it('handles every probe × every status without throwing', () => {
    for (const name of PROBE_NAMES) {
      for (const status of STATUSES) {
        const h = L.hintFor(probe(name, status));
        assert.equal(typeof h, 'string', `${name}/${status} returns string`);
      }
    }
  });
});

// The actionable matrix: which (probe, status) combinations MUST surface
// guidance, and which must stay silent. This is the heart of "the user
// always knows how to proceed / what to look into".
describe('hintFor — guidance matrix', () => {
  const hint = (name, status, metric) => L.hintFor(probe(name, status, metric));
  const has = (name, status, metric) => assert.ok(hint(name, status, metric).length > 0,
    `expected guidance for ${name}/${status}`);
  const none = (name, status, metric) => assert.equal(hint(name, status, metric), '',
    `expected NO guidance for ${name}/${status}`);

  it('Wi-Fi: guidance on fail only', () => {
    has('wifi', 'fail');
    for (const s of ['pass', 'skip', 'absent', 'unknown']) none('wifi', s);
  });

  it('Camera: fail + absent are actionable', () => {
    has('camera', 'fail');
    has('camera', 'absent');
    assert.match(hint('camera', 'absent'), /ribbon|camera/i);
    for (const s of ['pass', 'skip', 'unknown']) none('camera', s);
  });

  it('Bluetooth: fail + absent + skip are actionable; skip explains starting/safe-mode', () => {
    has('bluetooth', 'fail');
    assert.match(hint('bluetooth', 'fail'), /continue/i); // tells user they can proceed
    has('bluetooth', 'absent');
    // Skip is now a common, non-alarming state (radio on but idle, still
    // starting up, or paused in safe mode) — each variant reassures rather
    // than telling the user their Bluetooth is faulty.
    has('bluetooth', 'skip');                                   // idle default
    assert.match(hint('bluetooth', 'skip', { safe_mode: true }), /recovery/i);
    assert.match(hint('bluetooth', 'skip', { init_attempted: false }), /starting/i);
    assert.doesNotMatch(hint('bluetooth', 'skip'), /faulty/i);
    for (const s of ['pass', 'unknown']) none('bluetooth', s);
  });

  it('GPS: fail, skip (no fix yet) and absent all guide; pass is silent', () => {
    has('gps', 'fail');
    has('gps', 'skip');
    assert.match(hint('gps', 'skip'), /fix|sky|window/i);
    has('gps', 'absent');
    assert.match(hint('gps', 'absent'), /optional/i);
    none('gps', 'pass');
    none('gps', 'unknown');
  });

  it('SD: fail + absent guide, and absent tells them to insert a FAT32 card', () => {
    has('sd', 'fail');
    assert.match(hint('sd', 'fail'), /FAT32/);
    has('sd', 'absent');
    assert.match(hint('sd', 'absent'), /insert|FAT32/i);
    for (const s of ['pass', 'skip', 'unknown']) none('sd', s);
  });

  it('Battery: absent guides; pass guides only when the level is low', () => {
    has('power', 'absent');
    assert.match(hint('power', 'absent'), /USB|battery/i);
    has('power', 'pass', { soc_pct: 9 });          // low → "charge it"
    assert.match(hint('power', 'pass', { soc_pct: 9 }), /charge/i);
    none('power', 'pass', { soc_pct: 80 });         // healthy → silent
    none('power', 'pass', {});                       // unknown level → silent
    none('power', 'fail');
  });

  it('Microphone: muted vs not-started guide differently; pass teaches the alarm test', () => {
    has('microphone', 'pass');
    assert.match(hint('microphone', 'pass'), /TEST button/);
    has('microphone', 'skip', { muted: true });
    assert.match(hint('microphone', 'skip', { muted: true }), /muted/i);
    has('microphone', 'skip');                      // bring-up failure path
    assert.match(hint('microphone', 'skip'), /didn't start/i);
    assert.match(hint('microphone', 'skip'), /Run again/);
    has('microphone', 'absent');
    none('microphone', 'fail');                     // contract: mic never FAILs
    none('microphone', 'unknown');
  });

  it('Buzzer: pass/skip/absent all carry a note; fail is silent', () => {
    has('buzzer', 'pass');
    assert.match(hint('buzzer', 'pass'), /test tone|LED/i);
    has('buzzer', 'skip');
    has('buzzer', 'absent');
    none('buzzer', 'fail');
    none('buzzer', 'unknown');
  });

  it('Tamper: absent explains how to enable it; otherwise silent', () => {
    has('tamper', 'absent');
    assert.match(hint('tamper', 'absent'), /tamper|reed|contact/i);
    for (const s of ['pass', 'fail', 'skip', 'unknown']) none('tamper', s);
  });

  it('GPIO: fail + skip (stuck pin) guide', () => {
    has('gpio', 'fail');
    has('gpio', 'skip');
    assert.match(hint('gpio', 'skip'), /BOOT/);
    none('gpio', 'pass');
    none('gpio', 'absent');
  });

  it('Connection (fetch) row: always tells them how to reconnect', () => {
    for (const s of STATUSES) {
      assert.match(hint('fetch', s), /SecuraCV-|setup Wi-Fi/i, `fetch/${s}`);
    }
  });
});

// End-to-end style: feed whole /api/selftest-shaped reports covering the
// permutations the user described — "all green lights go", a single hard
// failure, and a fully-loaded board with every optional peripheral absent.
describe('whole-report scenarios', () => {
  it('all green / optional-absent → ready, and no FAIL means non-blocking', () => {
    const probes = [
      probe('wifi', 'pass'), probe('camera', 'pass'), probe('bluetooth', 'pass'),
      probe('gps', 'absent'), probe('sd', 'pass'), probe('power', 'absent'),
      probe('microphone', 'pass'), probe('buzzer', 'pass'),
      probe('tamper', 'absent'), probe('gpio', 'pass'),
    ];
    const fails = probes.filter(p => p.status === 'fail');
    assert.equal(fails.length, 0);
    const report = { all_passed: fails.length === 0, probes };
    assert.match(L.verdict(report).heading, /ready/i);
    // Optional-absent rows still teach the user what they could add.
    assert.ok(L.hintFor(probe('gps', 'absent')).length > 0);
    assert.ok(L.hintFor(probe('power', 'absent')).length > 0);
  });

  it('one hard failure (Bluetooth) → attention, with actionable + continue copy', () => {
    const probes = [
      probe('wifi', 'pass'), probe('camera', 'absent'),
      probe('bluetooth', 'fail'), probe('sd', 'pass'),
    ];
    const report = { all_passed: false, probes };
    assert.match(L.verdict(report).heading, /attention/i);
    const h = L.hintFor(probe('bluetooth', 'fail'));
    assert.match(h, /Run again/);
    assert.match(h, /continue/i);
  });

  it('unreachable device → fetch row guides reconnection', () => {
    const report = { all_passed: false, probes: [probe('fetch', 'fail')] };
    assert.match(L.verdict(report).heading, /attention/i);
    assert.match(L.hintFor(report.probes[0]), /SecuraCV-/);
  });
});
