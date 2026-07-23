// canary-local/tests/audio.test.js — the smoke-bench's honesty gate.
//
// Executes the committed WebAssembly core built from the production WAP
// acoustic detector (securacv_audio.cpp) and proves, in Node, that it behaves
// like the firmware: a synthesized NFPA-72 T3 smoke cadence and a UL-2034 T4
// CO cadence each raise their event, while a correctly-timed OFF-BAND rhythm
// and a steady in-band tone raise nothing (the 3.4 kHz tone gate + the cadence
// templates, not a JavaScript mirror that could drift). The browser probe
// (audio_probe.mjs) covers the page + mic path; this is the fast, headless
// wasm check that runs in the logic-tests job.

const { test } = require("node:test");
const assert = require("node:assert");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const factory = require(join(ROOT, "emulator/dist/canary-wap-audio.js"));

async function core() {
  const m = await factory();
  const c = {
    contract: JSON.parse(m.cwrap("audio_emu_contract_json", "string", [])()),
    reset: m.cwrap("audio_emu_reset", null, []),
    N: m.cwrap("audio_emu_frame_samples", "number", [])(),
    framePtr: m.cwrap("audio_emu_frame_ptr", "number", []),
    proc: m.cwrap("audio_emu_process_frame", "string", []),
    m,
  };
  return c;
}

// Feed cadences through the real detector and collect which events fired.
function driver(c) {
  let ph = 0;
  const events = new Set();
  const emit = (hz, nFrames) => {
    for (let f = 0; f < nFrames; f++) {
      const view = new Int16Array(c.m.HEAP16.buffer, c.framePtr(), c.N);
      for (let i = 0; i < c.N; i++) {
        if (hz <= 0) { view[i] = 0; continue; }
        ph += (2 * Math.PI * hz) / 16000;
        if (ph > 2 * Math.PI) ph -= 2 * Math.PI;
        view[i] = Math.round(8000 * Math.sin(ph));
      }
      const j = JSON.parse(c.proc());
      if (j.event) events.add(j.event);
    }
  };
  return { emit, events, resetPhase: () => { ph = 0; } };
}

test("audio core: the committed wasm exposes the real detector contract", async () => {
  const c = await core();
  assert.strictEqual(c.contract.schema, "securacv.canary-wap.audio-core/v1");
  assert.strictEqual(c.contract.sample_rate_hz, 16000);
  assert.strictEqual(c.contract.frame_samples, 320);
  assert.strictEqual(c.contract.tone_fc_hz, 3400);
  assert.strictEqual(c.contract.rms_on, 800);
  assert.strictEqual(c.contract.t3.beeps, 3);
  assert.strictEqual(c.contract.t4.beeps, 4);
});

test("audio core: a synthesized T3 smoke cadence fires the smoke event", async () => {
  const c = await core();
  c.reset();
  const d = driver(c);
  d.emit(0, 30);
  for (let cyc = 0; cyc < 2; cyc++) {
    d.emit(3400, 25); d.emit(0, 25);
    d.emit(3400, 25); d.emit(0, 25);
    d.emit(3400, 25); d.emit(0, 90);
  }
  assert.ok(d.events.has(1), "T3 smoke event (1) should have fired");
  assert.ok(!d.events.has(2), "T3 audio should not misfire as T4");
});

test("audio core: a synthesized T4 CO cadence fires the CO event", async () => {
  const c = await core();
  c.reset();
  const d = driver(c);
  d.emit(0, 30);
  for (let cyc = 0; cyc < 2; cyc++) {
    for (let b = 0; b < 4; b++) { d.emit(3400, 5); d.emit(0, 5); }
    d.emit(0, 300);
  }
  assert.ok(d.events.has(2), "T4 CO event (2) should have fired");
});

test("audio core: cadence-right / off-band rhythm is rejected (the tone gate holds)", async () => {
  const c = await core();
  c.reset();
  const d = driver(c);
  d.emit(0, 30);
  for (let cyc = 0; cyc < 2; cyc++) {
    d.emit(300, 25); d.emit(0, 25);
    d.emit(300, 25); d.emit(0, 25);
    d.emit(300, 25); d.emit(0, 90);
  }
  assert.ok(!d.events.has(1), "off-band rhythm must not read as smoke");
  assert.ok(!d.events.has(2), "off-band rhythm must not read as CO");
});

test("audio core: a steady in-band tone (no cadence) fires nothing", async () => {
  const c = await core();
  c.reset();
  const d = driver(c);
  d.emit(3400, 200);
  d.emit(0, 60);
  assert.ok(!d.events.has(1) && !d.events.has(2), "a continuous tone is not an alarm cadence");
});
