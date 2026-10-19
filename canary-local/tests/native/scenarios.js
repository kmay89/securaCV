// canary-local/tests/native/scenarios.js — the call-for-call scenarios two
// builds of a Lab core are held to (sweeps A40, A42, A58; not a test itself).
//
//   const s = require("./native/scenarios.js");
//   s.visionTicks(reference, candidate, why);   // two modules of the dist's shape
//
// Each scenario drives the two modules with the same calls, in step, and
// asserts every answer equal, naming the first call that differs and adding
// `why` (what a difference means for that pair). native_cores.test.js holds
// the native g++ build to the committed dist with them (LAB_CORES=native);
// cores_wasm32.test.js holds a wasm32-wasi clang build of the same sources
// to the g++ one. Agreement covers the calls these scenarios make, not every
// input.

"use strict";

const assert = require("node:assert");

function lcg(seed) {
  let s = seed >>> 0;
  return (n) => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s % n;
  };
}

const visionCalls = (m) => ({
  contract: m.cwrap("vision_emu_contract_json", "string", []),
  reset: m.cwrap("vision_emu_reset", null, []),
  config: m.cwrap("vision_emu_set_config", null, ["number", "number", "number", "number"]),
  begin: m.cwrap("vision_emu_begin_frame", null, []),
  push: m.cwrap("vision_emu_push_box", "number", Array(6).fill("number")),
  tick: m.cwrap("vision_emu_tick_json", "string", ["number"]),
});

// 3000 Vision ticks of random boxes in and around the frame, with
// out-of-range tuning now and then.
function visionTicks(ref, cand, why) {
  const [d, n] = [ref, cand].map(visionCalls);
  d.reset(); n.reset();
  assert.strictEqual(n.contract(), d.contract(), "contract " + why);
  const r = lcg(1762);
  let t = 0;
  for (let i = 0; i < 3000; i++) {
    if (r(200) === 0) {
      // out-of-range values on purpose: the clamps are the firmware's
      const cfg = [r(300) - 20, r(300) - 20, r(40000), r(120000)];
      d.config(...cfg); n.config(...cfg);
      assert.strictEqual(n.contract(), d.contract(), `contract after set_config(${cfg}) ` + why);
    }
    d.begin(); n.begin();
    for (let b = r(40) < 30 ? r(3) : r(36); b > 0; b--) {
      const box = [r(700) - 50, r(500) - 50, r(400), r(500), r(110), r(3)];
      assert.strictEqual(n.push(...box), d.push(...box), `push_box(${box}) before tick ${i} ` + why);
    }
    t += 50 + r(400);
    const want = d.tick(t);
    assert.strictEqual(n.tick(t), want, `tick ${i} at ${t} ms ` + why);
  }
}

// Sweep A42: boxes far outside the frame, where the pipeline's int and long
// arithmetic used to overflow (the center x + w/2, the cell px * cols, the
// area (long)w*h on wasm32's 32-bit long, and area * 100). Before A42 the
// same box read proximity "near" in a 64-bit build and "unknown" in wasm32,
// and landed in different cells. The sources now take each of those in
// int64_t, where no int box overflows, so two builds must agree on every one
// of these boxes, value for value. Each frame holds one extreme box, or two
// (so the occupied-cell mask sees both), and the clock runs on, so the FSM
// sees them as a visit too. Returns how many frames it played.
const EXTREMES = [-2147483648, -2000000000, -1, 0, 240, 50000, 2000000000, 2147483647];

function visionExtremes(ref, cand, why) {
  const [d, n] = [ref, cand].map(visionCalls);
  d.reset(); n.reset();
  d.config(0, 50, 1500, 10000); n.config(0, 50, 1500, 10000);
  const V = EXTREMES;
  let t = 0, frames = 0;
  const frame = (boxes) => {
    d.begin(); n.begin();
    for (const b of boxes) {
      assert.strictEqual(n.push(...b, 90, 0), d.push(...b, 90, 0), `push_box(${[...b, 90, 0]}) before the tick at ${t + 100} ms ` + why);
    }
    t += 100;
    const want = d.tick(t);
    assert.strictEqual(n.tick(t), want, `boxes ${JSON.stringify(boxes)} at ${t} ms ` + why);
    frames++;
    return JSON.parse(want);
  };
  for (const x of V) for (const y of V) for (const w of V) for (const h of V) frame([[x, y, w, h]]);
  const r = lcg(42);
  for (let i = 0; i < 1000; i++) {
    const pick = () => V[r(V.length)];
    frame([[pick(), pick(), pick(), pick()], [pick(), pick(), pick(), pick()]]);
  }
  // The sweep item's box, two billion pixels wide from the frame's corner:
  // near, in the last column, on both builds.
  const wide = frame([[0, 0, 2000000000, 70]]).sample;
  assert.deepStrictEqual([wide.proximity, wide.posture, wide.voxel.r, wide.voxel.c], ["near", "horizontal", 0, 2]);
  assert.strictEqual(frames, V.length ** 4 + 1000 + 1);
  return frames;
}

// The audio cadences (T3 and T4, an off-band rhythm) and noise, frame for
// frame, with the envelope thresholds retuned now and then. Each frame's PCM
// is written through the module's own HEAP16 at audio_emu_frame_ptr().
function audioFrames(ref, cand, why) {
  const [d, n] = [ref, cand].map((m) => ({
    m,
    contract: m.cwrap("audio_emu_contract_json", "string", []),
    reset: m.cwrap("audio_emu_reset", null, []),
    samples: m.cwrap("audio_emu_frame_samples", "number", []),
    ptr: m.cwrap("audio_emu_frame_ptr", "number", []),
    thresholds: m.cwrap("audio_emu_set_thresholds", "number", ["number", "number"]),
    proc: m.cwrap("audio_emu_process_frame", "string", []),
  }));
  d.reset(); n.reset();
  assert.strictEqual(n.contract(), d.contract(), "contract " + why);
  const N = d.samples();
  assert.strictEqual(n.samples(), N, "frame_samples " + why);
  const r = lcg(4040);
  let ph = 0;
  // a T3 cadence, a T4 cadence, an off-band rhythm and noise, in turns
  const plan = [];
  for (let k = 0; k < 3; k++) for (const [hz, f] of [[3400, 25], [0, 25], [3400, 25], [0, 25], [3400, 25], [0, 90]]) plan.push([hz, f]);
  for (let k = 0; k < 2; k++) for (let b = 0; b < 4; b++) plan.push([3400, 5], [0, 5]);
  plan.push([0, 300], [300, 25], [0, 25], [-1, 60], [3400, 200], [0, 60]);
  let frame = 0;
  for (const [hz, frames] of plan) {
    for (let f = 0; f < frames; f++, frame++) {
      if (r(400) === 0) {
        const th = [r(3000), r(2000)];
        assert.strictEqual(n.thresholds(...th), d.thresholds(...th), `set_thresholds(${th}) before frame ${frame} ` + why);
      }
      const pcm = new Int16Array(N);
      for (let i = 0; i < N; i++) {
        if (hz < 0) { pcm[i] = r(20000) - 10000; continue; }
        if (hz === 0) { pcm[i] = r(64) - 32; continue; }
        ph += (2 * Math.PI * hz) / 16000;
        if (ph > 2 * Math.PI) ph -= 2 * Math.PI;
        pcm[i] = Math.round(8000 * Math.sin(ph));
      }
      for (const c of [d, n]) new Int16Array(c.m.HEAP16.buffer, c.ptr(), N).set(pcm);
      const want = d.proc();
      assert.strictEqual(n.proc(), want, `frame ${frame} (${hz} Hz) ` + why);
    }
  }
  return frame;
}

module.exports = { lcg, visionTicks, visionExtremes, audioFrames, EXTREMES };
