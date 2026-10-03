// canary-local/tests/native/wasm32.js — the Vision detection pipeline built
// twice from this tree's sources: for wasm32 by the host's clang and wasm-ld,
// and for the host by g++ (sweep A49). vision_wasm32.test.js hands both the
// same boxes.
//
//   const w = require("./native/wasm32.js");
//   const tools = w.tools();          // what was found, and what is missing
//   const wasm = await w.wasmProbe(tools), host = await w.hostProbe(tools);
//
// Why: the emulator's dist is wasm32, where long is 32 bits, as on the ESP32;
// LAB_CORES=native and every firmware host suite build with the host's 64-bit
// long. So a product the pipeline takes in long (A42 found four) reads the
// same box differently in the dist and in every native build, and no 64-bit
// build can see it. Before this, only vision.test.js's A42 test on the dist
// caught one, and only after CI rebuilt the dist from the mutated sources.
//
// What it builds: wasm32/pipeline_probe.cpp (detection_pipeline.h behind a C
// ABI), with the flags build.sh hands em++ for the Vision core (read from
// build.sh by cores.js's buildPlan, not copied). The wasm32 build is
// freestanding: no libc and no C++ library, the two headers in wasm32/stubs
// stand in for what the firmware headers include (size_t and a strcmp
// declaration), and wasm-ld refuses any undefined symbol. node runs the
// module, which imports nothing. The host build is the same file with g++ (or
// $CXX) and PIPELINE_PROBE_HOSTED, which serves boxes over stdin and stdout.
//
// What it is not: emscripten's clang, or the dist. It is the host's clang
// (whatever version is installed, 18 on CI's runner) for
// wasm32-unknown-unknown, so it settles what the sources compute with a
// 32-bit long, not every way two compilers can differ (an undefined signed
// overflow can still split them). The dist is still what ships, and
// vision.test.js still checks it.
//
// Where clang++ (with a wasm32 target) or wasm-ld is missing the test skips,
// and says which; VISION_WASM32=require (CI) turns that into a failure.
// WASM32_CLANGXX and WASM_LD name the tools when they are not on PATH under
// the names below.

"use strict";

const fs = require("node:fs");
const os = require("node:os");
const { join } = require("node:path");
const { spawnSync, execFile } = require("node:child_process");
const { buildPlan } = require("./cores.js");

const HERE = join(__dirname, "wasm32");
const PROBE = join(HERE, "pipeline_probe.cpp");
const STUBS = join(HERE, "stubs");
const EXPORTS = ["probe_box", "probe_long_bits", "probe_fields", "probe_frame_w", "probe_frame_h"];
// The host build's stdout starts with these, then one answer per box.
const HEADER = ["longBits", "fields", "frameW", "frameH"];

function required() {
  const v = process.env.VISION_WASM32;
  if (v === undefined || v === "") return false;
  if (v === "require") return true;
  throw new Error(`VISION_WASM32=${v}: expected "require" (fail where clang++ or wasm-ld is missing) ` +
    `or nothing (skip there); see canary-local/tests/native/README.md`);
}

function probe(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: "utf8" });
  return r.error || r.status !== 0 ? null : (r.stdout || "") + (r.stderr || "");
}

// The tools, found once. clang++ must list a wasm32 target; wasm-ld is taken
// at clang's own major version first (the runner image installs lld-<n>
// beside each clang-<n>, without an unversioned wasm-ld).
let found = null;
function tools() {
  if (found) return found;
  const missing = [];
  const clangNames = process.env.WASM32_CLANGXX ? [process.env.WASM32_CLANGXX]
    : ["clang++", "clang++-20", "clang++-19", "clang++-18", "clang++-17", "clang++-16"];
  let clang = null, clangVersion = null, major = null;
  for (const c of clangNames) {
    const out = probe(c, ["--version"]);
    const m = out && /clang version ((\d+)\.[\w.-]+)/.exec(out);
    if (m) { clang = c; clangVersion = m[1]; major = m[2]; break; }
  }
  if (!clang) missing.push(`clang++ (looked for ${clangNames.join(", ")})`);
  else if (!/^\s*wasm32\b/m.test(probe(clang, ["-print-targets"]) || "")) missing.push(`a wasm32 target in ${clang}`);
  const ldNames = process.env.WASM_LD ? [process.env.WASM_LD]
    : [...(major ? [`wasm-ld-${major}`, `/usr/lib/llvm-${major}/bin/wasm-ld`] : []), "wasm-ld"];
  let wasmLd = null, ldVersion = null;
  for (const l of ldNames) {
    const out = probe(l, ["--version"]);
    const m = out && /LLD (\d+[\w.-]*)/.exec(out);
    if (m) { wasmLd = l; ldVersion = m[1]; break; }
  }
  if (!wasmLd) missing.push(`wasm-ld (looked for ${ldNames.join(", ")}; it ships in lld)`);
  const cxx = process.env.CXX || "g++";
  if (!probe(cxx, ["--version"])) missing.push(`${cxx} (the host build; set CXX to another)`);
  found = { clang, clangVersion, wasmLd, ldVersion, cxx, missing, required: required() };
  return found;
}

// node:test options: skip by name where a tool is missing, unless required.
function gate(t = tools()) {
  if (!t.missing.length || t.required) return {};
  return { skip: `needs ${t.missing.join(", ")}; VISION_WASM32=require fails here instead (tests/native/README.md)` };
}

function assertTools(t = tools()) {
  if (t.missing.length) {
    throw new Error(`VISION_WASM32=require, and the wasm32 build is missing ${t.missing.join(", ")} ` +
      `(install clang and lld, or point WASM32_CLANGXX / WASM_LD at them)`);
  }
}

// build.sh's Vision flags (the -std, -O2, defines and include paths em++
// gets), after any include directories a caller puts first (a mutated copy
// of a header, in the test's self-check).
function flags(includeFirst) {
  return [...includeFirst.flatMap((d) => ["-I", d]), ...buildPlan("canary-vision-core").flags];
}

let scratch = null;
function tmp(tag) {
  if (!scratch) {
    scratch = fs.mkdtempSync(join(os.tmpdir(), "securacv-vision-wasm32-"));
    process.once("exit", () => fs.rmSync(scratch, { recursive: true, force: true }));
  }
  return fs.mkdtempSync(join(scratch, tag + "-"));
}

function run(cmd, args, what) {
  return new Promise((resolve, reject) => {
    execFile(cmd, args, { maxBuffer: 16 << 20 }, (err, stdout, stderr) => {
      if (err) reject(new Error(`${what}: ${cmd} ${args.join(" ")}\n${stderr || err.message}`));
      else resolve(stdout);
    });
  });
}

// The wasm32 module: clang compiles, wasm-ld links with no libc and no
// undefined symbol allowed, node instantiates it with no imports.
async function wasmProbe(t = tools(), { includeFirst = [] } = {}) {
  assertTools(t);
  const dir = tmp("wasm32");
  const obj = join(dir, "pipeline_probe.o"), out = join(dir, "pipeline_probe.wasm");
  await run(t.clang, ["--target=wasm32-unknown-unknown", "-ffreestanding", "-nostdlib", "-nostdlibinc", "-nostdinc++",
    "-I", STUBS, ...flags(includeFirst), "-c", PROBE, "-o", obj], "the wasm32 build of the Vision pipeline");
  await run(t.wasmLd, ["--no-entry", ...EXPORTS.map((e) => "--export=" + e), obj, "-o", out],
    "linking the wasm32 Vision pipeline (no libc)");
  const module = new WebAssembly.Module(fs.readFileSync(out));
  const e = new WebAssembly.Instance(module, {}).exports;
  const fields = e.probe_fields();
  return {
    module,
    longBits: e.probe_long_bits(), fields, frameW: e.probe_frame_w(), frameH: e.probe_frame_h(),
    // boxes: Int32Array of x, y, w, h quads; returns fields int32s per box
    read(boxes) {
      const n = boxes.length / 4, answers = new Int32Array(n * fields);
      for (let i = 0; i < n; i++) {
        const p = e.probe_box(boxes[4 * i], boxes[4 * i + 1], boxes[4 * i + 2], boxes[4 * i + 3]);
        answers.set(new Int32Array(e.memory.buffer, p, fields), i * fields);
      }
      return answers;
    },
  };
}

// The host build: the same file and flags with g++ (or $CXX).
async function hostProbe(t = tools(), { includeFirst = [] } = {}) {
  assertTools(t);
  const bin = join(tmp("host"), "pipeline_probe");
  await run(t.cxx, ["-DPIPELINE_PROBE_HOSTED", ...flags(includeFirst), PROBE, "-o", bin],
    "the host build of the Vision pipeline");
  const ask = (boxes) => {
    const r = spawnSync(bin, [], { input: Buffer.from(boxes.buffer, boxes.byteOffset, boxes.byteLength),
      maxBuffer: 256 << 20 });
    if (r.error || r.status !== 0) throw new Error(`${bin} exited ${r.status}: ${r.error || r.stderr}`);
    const words = new Int32Array(r.stdout.buffer, r.stdout.byteOffset, r.stdout.byteLength / 4);
    return { head: Object.fromEntries(HEADER.map((k, i) => [k, words[i]])), answers: words.slice(HEADER.length) };
  };
  const { head } = ask(new Int32Array(0));
  return {
    ...head,
    read(boxes) {
      const { answers } = ask(boxes);
      if (answers.length !== (boxes.length / 4) * head.fields) {
        throw new Error(`${bin} answered ${answers.length} words for ${boxes.length / 4} boxes`);
      }
      return answers;
    },
  };
}

// The answer's fields, in pipeline_probe.cpp's order.
const FIELDS = ["person_now", "person_count", "voxel.r", "voxel.c", "voxel.rows", "voxel.cols", "voxel_mask",
  "posture", "proximity", "bbox.x", "bbox.y", "bbox.w", "bbox.h", "bbox.score"];

module.exports = { tools, gate, assertTools, wasmProbe, hostProbe, flags, FIELDS, EXPORTS, PROBE, STUBS };
