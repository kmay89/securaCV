// canary-local/tests/vision_wasm32.test.js — the Vision detection pipeline's
// 32-bit long, shown from this tree's sources (sweep A49).
//
// The emulator's dist is wasm32, where long is 32 bits (as on the ESP32);
// LAB_CORES=native and every firmware host suite build with the host's 64-bit
// long. A42 took the pipeline's box arithmetic out of long and int into
// int64_t, and found that a product put back in long passes every 64-bit
// build, because a 64-bit long holds it: only a wasm32 build shows it, and
// vision.test.js's A42 test caught it only on a dist CI had rebuilt from the
// mutated sources. Here tests/native/wasm32.js builds detection_pipeline.h
// for wasm32 with the host's clang and wasm-ld (freestanding: no libc, no
// C++ library, two stub headers) and for the host with g++, from build.sh's
// Vision flags, and both read the same boxes built from the int range's
// ends: every four-way pick of the V[] that test_vision_detection_pipeline.cpp
// holds the host build to exact (__int128) arithmetic on. The two must agree
// on every field.
// The last test puts each of A42's two long multiplies back in a scratch copy
// of the header and requires the host build to miss it and the wasm32 build
// to catch it, so the comparison is shown to see what it is here for.
//
// This is the host's clang for wasm32-unknown-unknown, not emscripten's, and
// not the dist: it settles what the sources compute with a 32-bit long, not
// every way two compilers can differ. Where clang++ (with a wasm32 target)
// or wasm-ld is missing these skip and say which; CI sets
// VISION_WASM32=require so that fails instead (tests/native/README.md).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const fs = require("node:fs");
const { join } = require("node:path");
const w = require("./native/wasm32.js");

const REPO = join(__dirname, "..", "..");
const VISION_INC = join(REPO, "firmware/projects/canary-vision/include");
const opts = w.gate();

const INT_MIN = -2147483648, INT_MAX = 2147483647;

// One build of each, shared by the tests below.
let pair = null;
function builds() {
  if (!pair) pair = Promise.all([w.wasmProbe(), w.hostProbe()]);
  return pair;
}

// test_vision_detection_pipeline.cpp's V (the int range's ends, the frame's
// own sizes, the overflow thresholds between), read from that file so the
// two grids cannot drift, and crossed four ways. Each entry is a literal or
// INT_MIN / INT_MAX / FRAME_W, optionally "/ n" or "- n"; anything else is
// refused by name rather than guessed.
const HOST_SUITE = join(REPO, "firmware/tests_host/test_vision_detection_pipeline.cpp");
function hostGridValues(frameW, src = fs.readFileSync(HOST_SUITE, "utf8")) {
  const m = /static const int V\[\] = \{([^}]*)\};/.exec(src);
  assert.ok(m, "test_vision_detection_pipeline.cpp has no `static const int V[] = { ... };`");
  const names = { INT_MIN, INT_MAX, FRAME_W: frameW };
  return m[1].split(",").map((t) => t.trim()).filter(Boolean).map((t) => {
    const e = /^(-?\d+|[A-Z_]+)(?:\s*([/-])\s*(\d+))?$/.exec(t);
    assert.ok(e && (/^-?\d/.test(e[1]) || e[1] in names), `cannot read V entry "${t}" in test_vision_detection_pipeline.cpp`);
    const v = /^-?\d/.test(e[1]) ? Number(e[1]) : names[e[1]];
    return !e[2] ? v : e[2] === "/" ? Math.trunc(v / Number(e[3])) : v - Number(e[3]);
  });
}

function grid(frameW) {
  const V = hostGridValues(frameW);
  assert.ok(V.every((v) => Number.isInteger(v) && v >= INT_MIN && v <= INT_MAX), "every V entry is an int");
  const boxes = new Int32Array(V.length ** 4 * 4);
  let i = 0;
  for (const x of V) for (const y of V) for (const ww of V) for (const h of V) boxes.set([x, y, ww, h], 4 * i++);
  return boxes;
}

// The boxes the two readings disagree on, each with the fields that differ.
function differences(boxes, a, b, fields) {
  const out = [];
  for (let i = 0; i < boxes.length / 4; i++) {
    const diff = [];
    for (let f = 0; f < fields; f++) {
      const x = a[i * fields + f], y = b[i * fields + f];
      if (x !== y) diff.push(`${w.FIELDS[f]} ${x} vs ${y}`);
    }
    if (diff.length) out.push(`box [${Array.from(boxes.subarray(4 * i, 4 * i + 4))}]: ${diff.join(", ")}`);
  }
  return out;
}

test("the wasm32 build is freestanding with a 32-bit long, the host build has the host's (sweep A49)", opts, async () => {
  w.assertTools();
  const [wasm, host] = await builds();
  assert.deepStrictEqual(WebAssembly.Module.imports(wasm.module), [], "the wasm32 module imports nothing: no libc, no host");
  assert.deepStrictEqual(WebAssembly.Module.exports(wasm.module).map((e) => e.name).sort(), ["memory", ...w.EXPORTS].sort());
  assert.strictEqual(wasm.longBits, 32, "wasm32's long is 32 bits, as the dist's and the ESP32's are");
  assert.strictEqual(host.longBits, 64, "the host build's long is 64 bits: the comparison separates the two only then");
  assert.strictEqual(wasm.fields, w.FIELDS.length);
  assert.deepStrictEqual([host.fields, host.frameW, host.frameH], [wasm.fields, wasm.frameW, wasm.frameH],
    "both builds read the same config.h");
});

test("the wasm32 build reads every extreme box as the host build does (sweep A49)", opts, async () => {
  w.assertTools();
  const [wasm, host] = await builds();
  const V = hostGridValues(host.frameW);
  assert.ok(V.includes(INT_MIN) && V.includes(INT_MAX),
    "the host suite's V[] holds both ends of the int range (A42's boxes)");
  const boxes = grid(host.frameW);
  assert.strictEqual(boxes.length / 4, V.length ** 4,
    `every four-way pick of the host suite's ${V.length} V[] entries`);
  const a = wasm.read(boxes), b = host.read(boxes);
  const wrong = differences(boxes, a, b, wasm.fields);
  assert.deepStrictEqual(wrong.slice(0, 3), [], `${wrong.length} of ${boxes.length / 4} boxes read otherwise in ` +
    "wasm32 (32-bit long) than in the host build (64-bit long): a product taken in long, or an overflow the two " +
    "compilers resolve differently");
  // The sweep item's box, two billion pixels wide from the frame's corner,
  // read proximity unknown on the wasm32 dist before A42 and near natively.
  const one = wasm.read(Int32Array.from([0, 0, 2000000000, 70]));
  const at = (name) => one[w.FIELDS.indexOf(name)];
  assert.deepStrictEqual([at("voxel.r"), at("voxel.c"), at("posture"), at("proximity")], [0, 2, 3, 3],
    "the last column, horizontal (3) and near (3)");
});

// A42's two mutants: the posture products, and the area, taken in long again
// (as before A42). Each must still apply to the header as it stands; when the
// source moves, move the spelling here with it.
const MUTANTS = [
  { name: "the posture multiplies in long", file: "canary/vision/optical_features.h", edits: [
    ["(int64_t)h * 100 >= (int64_t)w * OPT_POSTURE_UPRIGHT_RATIO_X100", "(long)h * 100 >= (long)w * OPT_POSTURE_UPRIGHT_RATIO_X100"],
    ["(int64_t)w * 100 >= (int64_t)h * OPT_POSTURE_HORIZONTAL_RATIO_X100", "(long)w * 100 >= (long)h * OPT_POSTURE_HORIZONTAL_RATIO_X100"],
  ] },
  { name: "the area multiply in long", file: "canary/vision/detection_pipeline.h", edits: [
    [": (int64_t)w * h;", ": (long)w * h;"],
  ] },
];

test("A42's two long multiplies: the host build misses each, the wasm32 build catches it (sweep A49)", opts, async () => {
  w.assertTools();
  const [, host] = await builds();
  const boxes = grid(host.frameW);
  const want = host.read(boxes);
  const scratch = fs.mkdtempSync(join(require("node:os").tmpdir(), "securacv-vision-mutant-"));
  try {
    for (const m of MUTANTS) {
      let src = fs.readFileSync(join(VISION_INC, m.file), "utf8");
      for (const [from, to] of m.edits) {
        assert.strictEqual(src.split(from).length - 1, 1, `${m.name}: "${from}" appears once in ${m.file}`);
        src = src.replace(from, to);
      }
      const dir = join(scratch, m.name.replace(/\W+/g, "-"));
      fs.mkdirSync(join(dir, "canary/vision"), { recursive: true });
      fs.writeFileSync(join(dir, m.file), src);
      const [mw, mh] = await Promise.all([w.wasmProbe(undefined, { includeFirst: [dir] }),
        w.hostProbe(undefined, { includeFirst: [dir] })]);
      assert.deepStrictEqual(differences(boxes, mh.read(boxes), want, mh.fields), [],
        `${m.name}: the host build reads it as the sources do (a 64-bit long holds the product)`);
      const caught = differences(boxes, mw.read(boxes), want, mw.fields);
      assert.ok(caught.length > 0, `${m.name}: the wasm32 build must read some box otherwise, and read none`);
    }
  } finally {
    fs.rmSync(scratch, { recursive: true, force: true });
  }
});
