// canary-local/tests/cores_wasm32.test.js — the whole Vision and WAP audio
// cores built for wasm32-wasi from this tree's sources, held to the g++
// build of the same sources call for call (sweep A58).
//
// The emulator's dist is wasm32, where long is 32 bits (as on the ESP32);
// LAB_CORES=native and every firmware host suite build with the host's 64-bit
// long. vision_wasm32.test.js (A49) shows that half from the sources for the
// Vision box pipeline alone, freestanding. Here tests/native/wasi.js builds
// each whole core (build.sh's sources, flags and include paths, read through
// cores.js) for wasm32-wasi with the host's clang against a WASI sysroot
// (wasi-libc, libc++, compiler-rt), and node:wasi runs it; cores.js builds
// the same sources with g++. Both are driven through the scenarios in
// tests/native/scenarios.js, the ones native_cores.test.js holds the native
// build to the committed dist with, and must answer every call alike.
// The last test puts a long back in each core, in a scratch copy, and
// requires the g++ build to miss it and the wasm32 build to catch it, so the
// comparison is shown to see what it is here for.
//
// This is the host's clang and wasi-libc, not emscripten, and not the dist:
// it settles what the sources compute in wasm32 with a 32-bit long, not
// every way two toolchains can differ. Where clang++ (with a wasm32 target),
// wasm-ld, the WASI sysroot or g++ is missing these skip and say which; CI
// installs the sysroot and sets CORES_WASM32=require so that fails instead
// (tests/native/README.md).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const fs = require("node:fs");
const os = require("node:os");
const { join, relative, basename, dirname } = require("node:path");
const cores = require("./native/cores.js");
const wasi = require("./native/wasi.js");
const scenarios = require("./native/scenarios.js");

const REPO = join(__dirname, "..", "..");
const NAMES = Object.keys(cores.CORES);
const opts = wasi.gate();

const DIFFERS = "differs between the wasm32-wasi build (a 32-bit long, as the dist and the ESP32 have) and the " +
  "g++ build (the host's 64-bit long) of the same sources: a value the core takes in long or unsigned long, or " +
  "something the two compilers or C libraries resolve otherwise (see tests/native/README.md)";

// One wasm32 build and one g++ build of each core, shared by the tests below.
const wasmBuilds = new Map();
const wasmCore = (name) => {
  if (!wasmBuilds.has(name)) wasmBuilds.set(name, wasi.wasmFactory(name));
  return wasmBuilds.get(name).then((make) => make());
};
const hostCore = async (name) => cores.instance(await cores.build(cores.buildPlan(name)));

// CI's half, held with no compiler: the page logic job installs the sysroot
// for clang 18 through .github/actions/setup-wasi-sysroot and runs this file
// with CORES_WASM32=require, whenever the dist step ran, red or green, as it
// runs vision_wasm32.test.js. The steps are read as the runner reads them
// (native/ci_steps.js), so a comment that says "CORES_WASM32: require", a
// "|| true" after the command or a continue-on-error fails here; and both
// steps carry a timeout (an apt mirror that stalls, a wasm32 call that never
// returns), so neither can use the job's whole budget.
test("CI installs the WASI sysroot and runs this file with CORES_WASM32=require (sweep A58)", () => {
  const ci = require("./native/ci_steps.js");
  const { keys: jobKeys, steps } = ci.job("logic-tests");
  const named = (name) => {
    const at = steps.findIndex((st) => st.name === name);
    assert.ok(at >= 0, `canary-local.yml's logic-tests job has the step "${name}"`);
    return { at, step: steps[at] };
  };
  const install = named("WASI sysroot for the runner's clang 18 (Ubuntu's wasi-libc, libc++, libc++abi, compiler-rt)");
  const run = named("Whole Vision and WAP audio cores as wasm32-wasi, call for call against g++ (the runner's clang 18 + wasi-libc, not emscripten)");
  assert.strictEqual(run.at, install.at + 1, "the cores step comes straight after the install");
  for (const [what, { step }] of [["the install step", install], ["the cores step", run]]) {
    assert.strictEqual(step.keys.if, ci.RAN, `${what} runs whenever the dist step ran`);
    assert.ok(!("continue-on-error" in step.keys), `${what}'s red stays red (no continue-on-error)`);
    assert.strictEqual(step.keys["timeout-minutes"], "5", `${what} has its own 5-minute timeout, under the job's ${jobKeys["timeout-minutes"]}`);
  }
  assert.deepStrictEqual(Object.keys(install.step.keys).sort(), ["if", "name", "timeout-minutes", "uses", "with"]);
  assert.strictEqual(install.step.keys.uses, "./.github/actions/setup-wasi-sysroot", "the install is the composite action (CI.md R10)");
  const major = install.step.with["clang-major"];
  assert.deepStrictEqual(install.step.with, { "clang-major": "18" }, "for clang 18");
  assert.deepStrictEqual(Object.keys(run.step.keys).sort(), ["env", "if", "name", "run", "timeout-minutes"]);
  assert.strictEqual(run.step.run, "node --test canary-local/tests/cores_wasm32.test.js", "it runs this file alone, and its exit status is the step's");
  assert.deepStrictEqual(run.step.env, { CORES_WASM32: "require", WASM32_CLANGXX: `clang++-${major}`, ...ci.RECORDER },
    "CORES_WASM32=require, the clang the sysroot is for, and the path-filter recorder's three lines");
  // clang 18 is ubuntu-24.04's default clang. When the job moves image, the
  // action's clang-major and WASM32_CLANGXX move with it, together.
  assert.strictEqual(jobKeys["runs-on"], "ubuntu-24.04",
    `the job left ubuntu-24.04: move clang-major (${major}) and WASM32_CLANGXX to the new image's clang, then this pin`);

  // The action: Ubuntu's four packages for that clang, exactly wasi.js's
  // list, through an apt that retries a dropped fetch and gives up on a
  // stalled one (setup-libseccomp's guards).
  const action = fs.readFileSync(join(REPO, ".github/actions/setup-wasi-sysroot/action.yml"), "utf8")
    .split("\n").filter((l) => !/^\s*#/.test(l)).join("\n");
  assert.match(action, /\n {2}clang-major:\n(?: {4}[^\n]*\n)* {4}required: true\n/, "the action takes clang-major, required");
  assert.match(action, /\n {6}env:\n {8}CLANG_MAJOR: \$\{\{ inputs\.clang-major \}\}\n/, "and hands it to its shell as CLANG_MAJOR");
  assert.match(action, /\n {8}set -euo pipefail\n/);
  assert.match(action, /\n {8}apt=\(sudo env DEBIAN_FRONTEND=noninteractive apt-get\n {13}-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30\)\n/,
    "apt retries a dropped fetch and gives up on a stalled one");
  const script = /\n {6}run: \|\n((?: {8}[^\n]*\n?)*)/.exec(action);
  assert.ok(script, "the action's step runs a script");
  const aptLines = script[1].split("\n").filter((l) => /apt-get|\$\{apt\[@\]\}/.test(l)).map((l) => l.trim());
  assert.deepStrictEqual(aptLines.map((l) => l.split(" ").slice(0, 2).join(" ")), ["apt=(sudo env", '"${apt[@]}" update', '"${apt[@]}" install'],
    "every apt call goes through the guarded command, update then install");
  const words = aptLines[2].split(/\s+/).slice(2);
  assert.deepStrictEqual(words.filter((w) => w.startsWith("-")), ["-y", "--no-install-recommends"]);
  assert.deepStrictEqual(words.filter((w) => !w.startsWith("-")).map((w) => w.replace(/^"|"$/g, "")).sort(),
    Object.values(wasi.packages("${CLANG_MAJOR}")).sort(), "it installs exactly the packages wasi.js names when a part is missing");
});

test("CORES_WASM32 takes \"require\" or nothing, and a missing part is named, never guessed", () => {
  const saved = process.env.CORES_WASM32;
  try {
    for (const [v, want] of [[undefined, false], ["", false], ["require", true]]) {
      if (v === undefined) delete process.env.CORES_WASM32;
      else process.env.CORES_WASM32 = v;
      assert.strictEqual(wasi.required(), want, String(v));
    }
    for (const v of ["1", "true", "required", "Require"]) {
      process.env.CORES_WASM32 = v;
      assert.throws(() => wasi.required(), /expected "require" \(fail where a tool or the WASI sysroot is missing\)/, v);
    }
  } finally {
    if (saved === undefined) delete process.env.CORES_WASM32;
    else process.env.CORES_WASM32 = saved;
  }
  assert.deepStrictEqual(wasi.gate({ missing: [], required: false }), {});
  assert.deepStrictEqual(wasi.gate({ missing: ["wasm-ld"], required: true }), {}, "required: the test runs and fails by name");
  assert.match(wasi.gate({ missing: ["the wasm32-wasi C library and headers (Ubuntu: wasi-libc)"], required: false }).skip,
    /^needs the wasm32-wasi C library and headers \(Ubuntu: wasi-libc\); CORES_WASM32=require fails here instead/);
  assert.throws(() => wasi.assertTools({ missing: ["libc++abi for wasm32-wasi (Ubuntu: libc++abi-18-dev-wasm32)"] }),
    /CORES_WASM32=require, and the wasm32-wasi build is missing libc\+\+abi for wasm32-wasi \(Ubuntu: libc\+\+abi-18-dev-wasm32\)/);
});

test("a mutant's swap must name a source the core builds", () => {
  const plan = cores.buildPlan("canary-wap-audio");
  assert.throws(() => wasi.variant(plan, { replace: { [join(REPO, "firmware/projects/canary-vision/src/detect_config.cpp")]: "/x.cpp" } }),
    /canary-wap-audio does not build firmware\/projects\/canary-vision\/src\/detect_config\.cpp/);
  const v = wasi.variant(plan, { includeFirst: ["/first"], replace: { [plan.sources[0]]: "/swapped.cpp" } });
  assert.deepStrictEqual(v.sources, ["/swapped.cpp", ...plan.sources.slice(1)]);
  assert.deepStrictEqual(v.flags.slice(0, 2), ["-I", "/first"]);
  assert.deepStrictEqual(v.flags.slice(2), plan.flags, "the plan's own flags follow, unchanged");
});

test("each core builds for wasm32-wasi with a 32-bit long, imports only WASI, and has the dist's shape (sweep A58)", opts, async () => {
  wasi.assertTools();
  assert.strictEqual(wasi.hostLongBits(), 64, "the g++ build's long is 64 bits: the comparison separates the two only then");
  for (const name of NAMES) {
    const plan = cores.buildPlan(name);
    const m = await wasmCore(name);
    assert.strictEqual(m.wasm32Core.longBits, 32, `${name}: wasm32's long is 32 bits, as the dist's and the ESP32's are`);
    assert.deepStrictEqual([...new Set(m.wasm32Core.imports.map((i) => i.module))], ["wasi_snapshot_preview1"],
      `${name}: it imports WASI calls only (libc's stdio), nothing undefined`);
    assert.deepStrictEqual(m.wasm32Core.exports.map((e) => e.name).sort(),
      ["_initialize", "lab_wasm32_long_bits", "memory", ...plan.exports.map((e) => e.name)].sort(),
      `${name}: every EMSCRIPTEN_KEEPALIVE export build.sh's sources declare, the reactor's own, and nothing else`);
    assert.deepStrictEqual(Object.keys(m).sort(), [...plan.runtime].sort(),
      `${name}: the runtime methods build.sh's -sEXPORTED_RUNTIME_METHODS gives the dist`);
    assert.deepStrictEqual(m.wasm32Core.sources, plan.sources.map((s) => relative(REPO, s)));
  }
});

test("wasm32 vision core = g++ build, tick for tick (sweep A58)", opts, async () => {
  wasi.assertTools();
  scenarios.visionTicks(await hostCore("canary-vision-core"), await wasmCore("canary-vision-core"), DIFFERS);
});

test("wasm32 vision core = g++ build on out-of-range boxes (sweep A58)", opts, async () => {
  wasi.assertTools();
  scenarios.visionExtremes(await hostCore("canary-vision-core"), await wasmCore("canary-vision-core"), DIFFERS);
});

test("wasm32 audio core = g++ build, frame for frame (sweep A58)", opts, async () => {
  wasi.assertTools();
  scenarios.audioFrames(await hostCore("canary-wap-audio"), await wasmCore("canary-wap-audio"), DIFFERS);
});

// A long put back where a core needs 64 bits, in a scratch copy: A42's
// posture products in the Vision pipeline (the spelling vision_wasm32.test.js
// carries for its own mutant), and the audio RMS's sum of squares, which
// reaches about 1e10 on a loud frame. Each edit must still apply to the
// source as it stands; when the source moves, move the spelling with it.
const MUTANTS = [
  { name: "the Vision posture multiplies in long", core: "canary-vision-core", scenario: scenarios.visionExtremes,
    header: "canary/vision/optical_features.h", root: join(REPO, "firmware/projects/canary-vision/include"), edits: [
      ["(int64_t)h * 100 >= (int64_t)w * OPT_POSTURE_UPRIGHT_RATIO_X100", "(long)h * 100 >= (long)w * OPT_POSTURE_UPRIGHT_RATIO_X100"],
      ["(int64_t)w * 100 >= (int64_t)h * OPT_POSTURE_HORIZONTAL_RATIO_X100", "(long)w * 100 >= (long)h * OPT_POSTURE_HORIZONTAL_RATIO_X100"],
    ] },
  { name: "the audio RMS sum of squares in long", core: "canary-wap-audio", scenario: scenarios.audioFrames,
    source: "firmware/projects/canary-wap/arduino/canary_wap/securacv_audio.cpp", edits: [
      ["int64_t sum = 0;\n  int64_t sumsq = 0;", "int64_t sum = 0;\n  long sumsq = 0;"],
      ["sumsq += (int64_t)s * s;", "sumsq += (long)s * s;"],
    ] },
];

test("a long put back in each core: the g++ build misses it, the wasm32 build catches it (sweep A58)", opts, async () => {
  wasi.assertTools();
  const scratch = fs.mkdtempSync(join(os.tmpdir(), "securacv-cores-mutant-"));
  try {
    for (const m of MUTANTS) {
      const file = m.header ? join(m.root, m.header) : join(REPO, m.source);
      let src = fs.readFileSync(file, "utf8");
      for (const [from, to] of m.edits) {
        assert.strictEqual(src.split(from).length - 1, 1, `${m.name}: "${from}" appears once in ${file}`);
        src = src.replace(from, to);
      }
      const dir = join(scratch, m.name.replace(/\W+/g, "-"));
      const copy = m.header ? join(dir, m.header) : join(dir, basename(file));
      fs.mkdirSync(dirname(copy), { recursive: true });
      fs.writeFileSync(copy, src);
      const options = m.header ? { includeFirst: [dir] } : { replace: { [file]: copy } };
      const plan = wasi.variant(cores.buildPlan(m.core), options);
      const [mutantWasm, mutantHost] = await Promise.all([wasi.wasmFactory(m.core, options).then((make) => make()),
        cores.compile(plan).then((built) => cores.instance(built))]);
      // the g++ build reads the mutant as the sources: a 64-bit long holds the product
      m.scenario(await hostCore(m.core), mutantHost, `${m.name}: the g++ build of the mutant answers otherwise than the ` +
        "g++ build of the sources, so the edit changes 64-bit behavior too and no longer isolates a 32-bit long; " +
        "move it to a spelling only a 32-bit long sees");
      const ref = await hostCore(m.core);
      assert.throws(() => m.scenario(ref, mutantWasm, DIFFERS), (e) => e instanceof assert.AssertionError &&
        e.message.includes(DIFFERS), `${m.name}: the wasm32 build must answer some call otherwise, and answered none`);
    }
  } finally {
    fs.rmSync(scratch, { recursive: true, force: true });
  }
});
