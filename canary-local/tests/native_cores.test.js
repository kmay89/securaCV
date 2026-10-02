// canary-local/tests/native_cores.test.js — the honesty gate for
// tests/native/cores.js, the loader the core-driving page tests take their
// WebAssembly core from (sweep A40).
//
// The default run (LAB_CORES unset, as CI's page logic job runs it) compiles
// nothing. It holds what the default promises — the tests get the committed
// dist bundle, the very module they used to require — and holds the native
// build's reading of build.sh and of the bindings to the dist it stands in
// for: the reader refuses a build.sh shape it cannot follow (an appended or
// reassigned array, an extra compile flag), and the pinned source lists
// below change with build.sh's. With LAB_CORES=native it also builds both
// cores and drives each next to its committed dist, call for call, on the
// scenarios below: a difference names a stale dist, or a place where the
// native build and the wasm one differ (another compiler, a 64-bit ABI).
// Agreement covers the calls those scenarios make, not every input.

const { test } = require("node:test");
const assert = require("node:assert");
const fs = require("node:fs");
const os = require("node:os");
const { join, relative } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const cores = require("./native/cores.js");
const NAMES = Object.keys(cores.CORES);

function withMode(value, fn) {
  const saved = process.env.LAB_CORES;
  if (value === undefined) delete process.env.LAB_CORES;
  else process.env.LAB_CORES = value;
  try {
    return fn();
  } finally {
    if (saved === undefined) delete process.env.LAB_CORES;
    else process.env.LAB_CORES = saved;
  }
}

test("unset or dist: the tests get the committed bundle, the module they always required", () => {
  for (const name of NAMES) {
    const dist = require(join(ROOT, "emulator/dist", name + ".js"));
    assert.strictEqual(withMode(undefined, () => cores.coreFactory(name)), dist, name + " (unset)");
    assert.strictEqual(withMode("", () => cores.coreFactory(name)), dist, name + " (empty)");
    assert.strictEqual(withMode("dist", () => cores.coreFactory(name)), dist, name + " (dist)");
  }
});

test("any other LAB_CORES value is refused, never read as the dist", () => {
  for (const v of ["Native", "1", "true", "wasm"]) {
    assert.throws(() => withMode(v, () => cores.coreFactory("canary-vision-core")),
      /expected "dist" \(the default: the committed emulator\/dist bundles\) or "native"/, v);
  }
  assert.throws(() => cores.coreFactory("canary-display-watch"), /no Lab core canary-display-watch/,
    "a display flavor is not a core cores.js can build");
});

test("every test that requires a dist core takes it through cores.js", () => {
  // (this file requires the dist on purpose: it compares the two)
  const takers = [];
  for (const f of fs.readdirSync(__dirname).filter((x) => /\.test\.m?js$/.test(x) && x !== "native_cores.test.js").sort()) {
    const src = fs.readFileSync(join(__dirname, f), "utf8");
    assert.doesNotMatch(src, /require\([^)]*emulator\/dist\//, `${f} requires a dist bundle directly`);
    for (const m of src.matchAll(/require\("\.\/native\/cores\.js"\)\.coreFactory\("([\w-]+)"\)/g)) {
      takers.push(`${f}:${m[1]}`);
    }
  }
  assert.deepStrictEqual(takers, ["audio.test.js:canary-wap-audio", "eyes.test.js:canary-vision-core",
    "vision.test.js:canary-vision-core"]);
});

// The build plan is build.sh's, so these hold the reading, not a copy.
test("the vision plan is the five sources build.sh hands em++, with its flags", () => {
  const plan = cores.buildPlan("canary-vision-core");
  assert.strictEqual(plan.flavor, "vision");
  assert.deepStrictEqual(plan.sources.map((s) => relative(REPO, s)), [
    "firmware/projects/canary-vision/src/detect_config.cpp",
    "firmware/projects/canary-vision/src/state/presence_fsm.cpp",
    "firmware/projects/canary-vision/src/state/voxel_tracker.cpp",
    "canary-local/emulator/vision/vision_core_bindings.cpp",
    "canary-local/emulator/vision/vision_core_shim.cpp",
  ]);
  for (const f of ["-std=gnu++17", "-fno-exceptions", "-fno-rtti", "-DARDUINO=10812", '-D__DATE__="emu"'])
    assert.ok(plan.flags.includes(f), "flag " + f);
  const incs = plan.flags.flatMap((f, i) => (f === "-I" ? [relative(REPO, plan.flags[i + 1])] : []));
  assert.deepStrictEqual(incs, ["canary-local/emulator/shim", "canary-local/emulator/vision",
    "firmware/projects/canary-vision/include", "firmware/configs/canary-vision/default"]);
  assert.strictEqual(plan.exportName, "createCanaryVisionCore");
});

test("the audio plan is build.sh's two sources against the WAP's host stubs", () => {
  const plan = cores.buildPlan("canary-wap-audio");
  assert.strictEqual(plan.flavor, "audio");
  assert.deepStrictEqual(plan.sources.map((s) => relative(REPO, s)), [
    "firmware/projects/canary-wap/arduino/canary_wap/securacv_audio.cpp",
    "canary-local/emulator/audio/audio_core_bindings.cpp",
  ]);
  const incs = plan.flags.flatMap((f, i) => (f === "-I" ? [relative(REPO, plan.flags[i + 1])] : []));
  assert.deepStrictEqual(incs, ["canary-local/emulator/audio", "firmware/projects/canary-wap/tests_host/stubs/audio",
    "firmware/projects/canary-wap/arduino/canary_wap"]);
  assert.strictEqual(plan.exportName, "createCanaryAudioCore");
});

// The reader refuses, by name, what it cannot follow. Each case is build.sh
// with one edit made in the core's flavor block (or at its top level), found
// by shape rather than by line, so a renamed array still gets every case.
test("a build.sh edit the reader cannot follow is refused, never skipped", () => {
  const sh = fs.readFileSync(join(ROOT, "emulator/build.sh"), "utf8");
  const flavorOf = { "canary-vision-core": "vision", "canary-wap-audio": "audio" };
  for (const name of NAMES) {
    const head = `if [[ "$FLAVOR" == "${flavorOf[name]}" ]]; then\n`;
    const at = sh.indexOf(head), end = sh.indexOf("\nfi\n", at);
    assert.ok(at >= 0 && end > at, `build.sh has the ${flavorOf[name]} block`);
    const block = sh.slice(at, end);
    const SRCS = /^[ \t]*for src in "\$\{(\w+)\[@\]\}"; do$/m.exec(block)[1];
    const compile = /^[ \t]*em\+\+ -c "\$src" "\$\{(\w+)\[@\]\}" -o "\$obj"$/m.exec(block);
    const FLAGS = compile[1];
    const INCLUDES = /"\$\{(\w+_INCLUDES)\[@\]\}"/.exec(block)[1];
    const PROJ = /^[ \t]*(\w+_PROJ)="/m.exec(block)[1];
    const inBlock = (f) => sh.slice(0, at) + f(block) + sh.slice(end);
    const beforeLoop = (line) => inBlock((b) => b.replace(/^([ \t]*for src in )/m, `  ${line}\n$1`));
    const inLoop = (line) => inBlock((b) => b.replace(/^([ \t]*for src in .*\n)/m, `$1    ${line}\n`));
    const onCompile = (f) => inBlock((b) => b.replace(compile[0], f(compile[0])));
    const inSources = (word) => inBlock((b) => b.replace(`${SRCS}=(\n`, `${SRCS}=(\n    ${word}\n`));
    const plan = cores.buildPlan(name);
    const refused = (text, why, label) => {
      assert.notStrictEqual(text, sh, `${name}: the "${label}" edit applies`);
      assert.throws(() => cores.buildPlan(name, text), why, `${name}: ${label}`);
    };

    assert.deepStrictEqual(cores.buildPlan(name, sh), plan, `${name}: the text and the file give one plan`);
    // an array the build reads, written anywhere but its literal
    refused(beforeLoop(`${FLAGS}+=(-DOPT_PROXIMITY_NEAR_PCT=50)`), new RegExp(`${FLAGS} is written 2 times`), "a flag appended");
    refused(beforeLoop(`${SRCS}+=("$EMU_DIR/extra.cpp")`), new RegExp(`${SRCS} is written 2 times`), "a source appended");
    refused(beforeLoop(`${INCLUDES}+=(-I "$EMU_DIR/x")`), new RegExp(`${INCLUDES} is written 2 times`), "an include appended");
    refused(beforeLoop(`${FLAGS}=(-O0)`), new RegExp(`${FLAGS} is written 2 times`), "the flags reassigned on one line");
    refused(beforeLoop(`${FLAGS}[0]=-O0`), new RegExp(`${FLAGS} is written 2 times`), "a flag element set");
    refused(beforeLoop(`declare -a ${SRCS}`), new RegExp(`${SRCS} is written 2 times`), "the sources declared again");
    // a variable the words expand, or one it was built from
    refused(beforeLoop(`${PROJ}="$FW/elsewhere"`), new RegExp(`\\$${PROJ} is written 2 times`), "the project reassigned");
    refused(sh.replace(/^FW=.*\n/m, (l) => l + 'FW+="/x"\n'), /\$FW is written 2 times/, "the firmware root appended");
    refused(inLoop('src="${src%.cpp}_emu.cpp"'), /\$src is written in the block/, "the loop's src rewritten");
    // the compile line, exactly
    refused(onCompile((l) => l.replace(' -o "$obj"', ' -DPROBE_EXTRA=1 -o "$obj"')), /expected one `em\+\+ -c/, "an extra compile flag");
    refused(onCompile((l) => l.replace(' -o "$obj"', ' \\\n      -DPROBE_EXTRA=1 -o "$obj"')), /expected one `em\+\+ -c/,
      "an extra flag on a continuation line");
    refused(onCompile((l) => `${l}\n    emcc -c extra.c -o extra.o`), /expected one `em\+\+ -c/, "a second compile");
    refused(sh.replace(/^export LC_ALL=C$/m, "export LC_ALL=C\nexport EMCC_CFLAGS=-DX"), /EMCC_CFLAGS adds flags/, "EMCC_CFLAGS");
    // words bash would expand differently from how they read
    refused(inSources('"$EMU_DIR"/*.cpp'), /cannot follow the unquoted pattern/, "a glob");
    refused(inSources('"`echo x`.cpp"'), /backquoted command/, "a backquoted command");
    // a read is not a write
    assert.deepStrictEqual(cores.buildPlan(name, beforeLoop(`echo "\${${FLAGS}[@]}"`)), plan, `${name}: an echo of the flags`);
  }
});

test("the plan reads every export and runtime method the committed dist has", async () => {
  for (const name of NAMES) {
    const plan = cores.buildPlan(name);
    const declared = plan.sources.map((s) => fs.readFileSync(s, "utf8")
      .replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, "").split("EMSCRIPTEN_KEEPALIVE").length - 1)
      .reduce((a, b) => a + b, 0);
    assert.strictEqual(plan.exports.length, declared, `${name}: every EMSCRIPTEN_KEEPALIVE is read`);
    const mod = await require(join(ROOT, "emulator/dist", name + ".js"))();
    const keys = Object.keys(mod);
    for (const k of keys.filter((x) => x.startsWith("_")))
      assert.ok(plan.exports.some((e) => "_" + e.name === k), `${name}: the dist exports ${k}, the plan does not`);
    assert.deepStrictEqual(plan.runtime, keys.filter((x) => !x.startsWith("_")),
      `${name}: -sEXPORTED_RUNTIME_METHODS, as the dist was linked`);
  }
});

test("every export a page or test cwraps is one the native build serves", () => {
  const wrapped = (file) => [...fs.readFileSync(join(ROOT, file), "utf8").matchAll(/cwrap\("(\w+)"/g)].map((m) => m[1]);
  const served = (name) => new Set(cores.buildPlan(name).exports.map((e) => e.name));
  const vision = served("canary-vision-core"), audio = served("canary-wap-audio");
  for (const fn of wrapped("emulator/web/vision-core.js")) assert.ok(vision.has(fn), "vision-core.js wraps " + fn);
  for (const f of ["assets/smoke-bench.js", "tests/audio.test.js"])
    for (const fn of wrapped(f)) assert.ok(audio.has(fn), `${f} wraps ${fn}`);
});

test("a signature a wasm call cannot carry is refused by name, never guessed", () => {
  const dir = fs.mkdtempSync(join(os.tmpdir(), "securacv-cores-test-"));
  try {
    const refuse = (sig, why) => {
      const f = join(dir, "b.cpp");
      fs.writeFileSync(f, `#include <emscripten.h>\nextern "C" {\nEMSCRIPTEN_KEEPALIVE ${sig} { }\n}\n`);
      assert.throws(() => cores.exportsOf(f), why, sig);
    };
    refuse("double f(int x)", /f returns double/);
    refuse("int f(double x)", /f takes "double x"/);
    refuse("int f(long long x)", /f takes "long long x"/);
    refuse("int f(const char* s)", /f takes "const char\* s"/);
    refuse("long f()", /f returns long/);
    const f = join(dir, "ok.cpp");
    fs.writeFileSync(f, "EMSCRIPTEN_KEEPALIVE unsigned int g(unsigned int a, int b) { return a; }\n" +
      "// EMSCRIPTEN_KEEPALIVE double commented_out();\n" +
      "EMSCRIPTEN_KEEPALIVE const char *h(void) { return \"\"; }\n");
    assert.deepStrictEqual(cores.exportsOf(f).map(({ name, kind, params }) => [name, kind, params]),
      [["g", "n", ["unsigned int", "int"]], ["h", "s", []]]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

// The pipe itself, with a stand-in core (native/fake_core.js speaks
// core_server.cpp's protocol and misbehaves on request), so the default run
// holds it with no compiler.
const FAKE_PLAN = { name: "fake-core", sources: [], runtime: ["ccall", "cwrap"], memory: {}, exports: [
  { name: "num", ret: "int", kind: "n", params: ["int"] },
  { name: "str", ret: "const char*", kind: "s", params: [] },
  { name: "die", ret: "void", kind: "v", params: [] },
  { name: "noise", ret: "int", kind: "n", params: [] },
  { name: "spin", ret: "void", kind: "v", params: [] },
] };
const fakeCore = () => cores.instance({ plan: FAKE_PLAN, bin: process.execPath, args: [join(__dirname, "native/fake_core.js")] });

test("the pipe answers synchronously, with the i32s a wasm call would pass", () => {
  const m = fakeCore();
  const num = m.cwrap("num", "number", ["number"]);
  // ToInt32, as wasm applies to a JS number passed to an int parameter
  assert.deepStrictEqual([num(41.9), num(-1.5), num(2 ** 32 + 5), num(2 ** 31), num(NaN), num(true)],
    [41, -1, 5, -(2 ** 31), 0, 1]);
  assert.strictEqual(m.cwrap("str", "string", [])(), "hello from the fake core");
  // refused by cwrap itself, before a wrapper exists to call: nothing runs
  assert.throws(() => m.cwrap("str", "number", []), /returns a C string: cwrap it as "string"/);
  assert.throws(() => m.cwrap("num", "string", ["number"]), /returns int, not a C string/);
  assert.throws(() => m.cwrap("gone", "number", []), /no export gone/);
  assert.throws(() => m.cwrap("num", "number", ["string"]), /passes number arguments only, not string/);
});

// What the dist's cwrap and ccall hand back for a "boolean" return, read off
// the committed bundle rather than assumed: cwrap returns the raw export (a
// number), ccall converts it. The native module must do the same.
test("a \"boolean\" return is a number through cwrap and a boolean through ccall, as on the dist", async () => {
  const dist = await require(join(ROOT, "emulator/dist/canary-wap-audio.js"))();
  const fake = fakeCore();
  const seen = (m, fn, args) => [m.cwrap(fn, "boolean", args.map(() => "number"))(...args),
    m.ccall(fn, "boolean", args.map(() => "number"), args)];
  assert.deepStrictEqual(seen(dist, "audio_emu_set_thresholds", [1200, 600]), [1, true], "the dist");
  assert.deepStrictEqual(seen(dist, "audio_emu_frame_samples", []).map((x) => typeof x), ["number", "boolean"],
    "the dist");
  assert.deepStrictEqual(seen(fake, "num", [1]), [1, true], "the native module");
  assert.deepStrictEqual(seen(fake, "num", [0]), [0, false], "the native module");
  assert.deepStrictEqual(seen(fake, "num", [7]), [7, true], "the native module");
  assert.strictEqual(fake.ccall("num", "number", ["number"], [5]), 5, "ccall with a number return");
});

test("a core that dies or answers twice fails the call that saw it, and every call after", () => {
  const m = fakeCore();
  assert.throws(() => m.cwrap("die", null, [])(), /native core exited \(code 3\)/);
  assert.throws(() => m.cwrap("num", "number", ["number"])(1), /native core exited \(code 3\)/);
  const n = fakeCore();
  const said = [];
  for (const call of [() => n.cwrap("noise", "number", [])(), () => n.cwrap("num", "number", ["number"])(2)]) {
    try { said.push(call()); } catch (e) { said.push(e.message); }
  }
  assert.ok(said.some((x) => /wrote a line it was not asked for/.test(x)), JSON.stringify(said));
  assert.strictEqual(fakeCore().cwrap("num", "number", ["number"])(7), 7, "a fresh instance is unaffected");
});

test("a core that stops answering is killed when its call times out, not left running", () => {
  const m = fakeCore();
  const pid = m.nativeCore.pid;
  assert.ok(pid > 0, "the worker reports the core's pid");
  assert.strictEqual(m.cwrap("num", "number", ["number"])(3), 3, "it answers before it is stuck");
  const saved = cores.limits.timeoutMs;
  cores.limits.timeoutMs = 400;
  try {
    assert.throws(() => m.cwrap("spin", null, [])(), /no answer to c spin in 0\.4 s \(core killed\)/);
    assert.throws(() => m.cwrap("num", "number", ["number"])(1), /torn down after an earlier timeout/);
    // gone, or a zombie nobody reaps until the test exits: either way not running
    const deadline = Date.now() + 5000;
    const state = () => {
      try { return fs.readFileSync(`/proc/${pid}/stat`, "utf8").split(") ")[1][0]; } catch { return "gone"; }
    };
    if (process.platform === "linux") {
      while (!["gone", "Z", "X"].includes(state()) && Date.now() < deadline) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
      assert.ok(["gone", "Z", "X"].includes(state()), `the stuck core (pid ${pid}) is still running: ${state()}`);
    } else {
      const ps = require("node:child_process").spawnSync("ps", ["-o", "stat=", "-p", String(pid)], { encoding: "utf8" });
      assert.match(ps.stdout.trim() || "gone", /^(gone|Z)/, `the stuck core (pid ${pid}) is still running`);
    }
  } finally {
    cores.limits.timeoutMs = saved;
  }
  assert.strictEqual(fakeCore().cwrap("num", "number", ["number"])(9), 9, "the next instance gets a fresh channel");
});

// ── LAB_CORES=native only: the native core next to its committed dist ──────

const native = { skip: cores.mode() !== "native" && "LAB_CORES=native only (builds with g++)" };

function lcg(seed) {
  let s = seed >>> 0;
  return (n) => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    return s % n;
  };
}

const STALE = "differs from the committed dist: if this tree changed the core's sources, the dist is stale " +
  "(Actions -> \"Rebuild emulator dist (pinned emsdk)\"); if not, the native build is not the wasm one";

test("native vision core = committed dist, tick for tick (LAB_CORES=native)", native, async () => {
  const pair = await Promise.all([require(join(ROOT, "emulator/dist/canary-vision-core.js"))(),
    cores.coreFactory("canary-vision-core")()]);
  const [d, n] = pair.map((m) => ({
    contract: m.cwrap("vision_emu_contract_json", "string", []),
    reset: m.cwrap("vision_emu_reset", null, []),
    config: m.cwrap("vision_emu_set_config", null, ["number", "number", "number", "number"]),
    begin: m.cwrap("vision_emu_begin_frame", null, []),
    push: m.cwrap("vision_emu_push_box", "number", Array(6).fill("number")),
    tick: m.cwrap("vision_emu_tick_json", "string", ["number"]),
  }));
  assert.ok(pair[1].nativeCore, "the second core is the native one");
  d.reset(); n.reset();
  assert.strictEqual(n.contract(), d.contract(), "contract " + STALE);
  const r = lcg(1762);
  let t = 0;
  for (let i = 0; i < 3000; i++) {
    if (r(200) === 0) {
      // out-of-range values on purpose: the clamps are the firmware's
      const cfg = [r(300) - 20, r(300) - 20, r(40000), r(120000)];
      d.config(...cfg); n.config(...cfg);
      assert.strictEqual(n.contract(), d.contract(), `contract after set_config(${cfg}) ` + STALE);
    }
    d.begin(); n.begin();
    for (let b = r(40) < 30 ? r(3) : r(36); b > 0; b--) {
      const box = [r(700) - 50, r(500) - 50, r(400), r(500), r(110), r(3)];
      assert.strictEqual(n.push(...box), d.push(...box), "push_box " + STALE);
    }
    t += 50 + r(400);
    const want = d.tick(t);
    assert.strictEqual(n.tick(t), want, `tick ${i} at ${t} ms ` + STALE);
  }
});

test("native audio core = committed dist, frame for frame (LAB_CORES=native)", native, async () => {
  const pair = await Promise.all([require(join(ROOT, "emulator/dist/canary-wap-audio.js"))(),
    cores.coreFactory("canary-wap-audio")()]);
  const [d, n] = pair.map((m) => ({
    m,
    contract: m.cwrap("audio_emu_contract_json", "string", []),
    reset: m.cwrap("audio_emu_reset", null, []),
    samples: m.cwrap("audio_emu_frame_samples", "number", []),
    ptr: m.cwrap("audio_emu_frame_ptr", "number", []),
    thresholds: m.cwrap("audio_emu_set_thresholds", "number", ["number", "number"]),
    proc: m.cwrap("audio_emu_process_frame", "string", []),
  }));
  assert.ok(pair[1].nativeCore, "the second core is the native one");
  d.reset(); n.reset();
  assert.strictEqual(n.contract(), d.contract(), "contract " + STALE);
  const N = d.samples();
  assert.strictEqual(n.samples(), N);
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
        assert.strictEqual(n.thresholds(...th), d.thresholds(...th), "set_thresholds " + STALE);
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
      assert.strictEqual(n.proc(), want, `frame ${frame} (${hz} Hz) ` + STALE);
    }
  }
});
