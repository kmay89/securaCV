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

// ── The browser probes' bridge (probe_cores.js) and its stand-in factory ────
//
// Under LAB_CORES=native a probe's server answers emulator/dist/<core>.js with
// native/core_standin.js, which forwards each call over a synchronous request
// to the bridge. Here the stand-in runs in a vm context whose XMLHttpRequest
// hands the body straight to the bridge (as the probe server does), over the
// stand-in core: no compiler, no Chromium.
const probeBridge = require("./native/probe_cores.js");
const vm = require("node:vm");

const BRIDGE_PLAN = { ...FAKE_PLAN, exportName: "createFakeCore", runtime: ["ccall", "cwrap", "UTF8ToString", "HEAPU8"],
  memory: { buf: () => 8, big: () => 100000 }, exports: [...FAKE_PLAN.exports,
    { name: "buf", ret: "uint8_t*", kind: "p", params: [] },
    { name: "big", ret: "uint8_t*", kind: "p", params: [] },
    { name: "sum", ret: "int", kind: "n", params: [] },
    { name: "bump", ret: "void", kind: "v", params: [] }] };

// The page side: the stand-in as served, evaluated in its own context, with
// a synchronous XMLHttpRequest that posts to the bridge.
function pageWith(bridge, name, factory) {
  const sent = [];
  class XMLHttpRequest {
    open(method, url, isAsync) {
      assert.strictEqual(method, "POST");
      assert.strictEqual(url, probeBridge.ENDPOINT, "the stand-in posts to the bridge's endpoint only");
      assert.strictEqual(isAsync, false, "every core call is a synchronous request, as a wasm call is synchronous");
    }
    setRequestHeader() {}
    send(body) {
      sent.push(JSON.parse(body).op);
      this.status = 200;
      this.responseText = JSON.stringify(bridge.respond(body));
    }
  }
  const ctx = vm.createContext({ XMLHttpRequest });
  vm.runInContext(bridge.standin(name), ctx, { filename: "core_standin.js" });
  assert.strictEqual(typeof ctx[factory], "function", `the stand-in defines ${factory}`);
  return { ctx, sent, create: () => ctx[factory]() };
}

const fakeBridge = () => probeBridge.bridgeOver({
  "fake-core": { plan: BRIDGE_PLAN, bin: process.execPath, args: [join(__dirname, "native/fake_core.js")] },
});

test("unset or dist: the probes get no bridge and serve the committed dist; another value is refused", async () => {
  for (const v of [undefined, "", "dist"]) {
    const saved = process.env.LAB_CORES;
    if (v === undefined) delete process.env.LAB_CORES;
    else process.env.LAB_CORES = v;
    try {
      assert.strictEqual(await probeBridge.probeCores(["canary-vision-core"]), null, String(v));
      process.env.LAB_CORES = "Native";
      await assert.rejects(probeBridge.probeCores(["canary-vision-core"]), /expected "dist"/);
    } finally {
      if (saved === undefined) delete process.env.LAB_CORES;
      else process.env.LAB_CORES = saved;
    }
  }
});

test("the stand-in has the dist module's shape, and its calls answer synchronously as the dist's do", async () => {
  const bridge = fakeBridge();
  const page = pageWith(bridge, "fake-core", "createFakeCore");
  const pending = page.create();
  assert.ok(pending instanceof vm.runInContext("Promise", page.ctx), "the factory returns a promise, as the dist's does");
  const m = await pending;
  assert.deepStrictEqual(Object.keys(m).sort(), ["HEAPU8", "UTF8ToString", "ccall", "cwrap", "nativeCore"]);
  assert.strictEqual(m.nativeCore.name, "fake-core");
  const num = m.cwrap("num", "number", ["number"]);
  assert.deepStrictEqual([num(41.9), num(-1.5), num(2 ** 32 + 5), num(2 ** 31), num(NaN), num(true)],
    [41, -1, 5, -(2 ** 31), 0, 1], "ToInt32, as a wasm call applies it");
  assert.strictEqual(m.cwrap("str", "string", [])(), "hello from the fake core");
  assert.deepStrictEqual([m.cwrap("num", "boolean", ["number"])(7), m.ccall("num", "boolean", ["number"], [7]),
    m.ccall("num", "boolean", ["number"], [0])], [7, true, false], "cwrap's boolean is the raw number; ccall's converts");
  // refused at cwrap, before a wrapper exists: the request was a cwrap, not a call
  page.sent.length = 0;
  assert.throws(() => m.cwrap("str", "number", []), /returns a C string: cwrap it as "string"/);
  assert.throws(() => m.cwrap("gone", "number", []), /no export gone/);
  assert.throws(() => m.cwrap("num", "number", ["string"]), /passes number arguments only, not string/);
  assert.deepStrictEqual(page.sent, ["cwrap", "cwrap", "cwrap"]);
  assert.throws(() => m.UTF8ToString(16), /no wasm heap to read a string from/, "as the native module, by the same code");
  assert.strictEqual(bridge.calls.get("fake-core"), 6 + 1 + 3 + 1, "every call reached the bridge, and only calls counted");
  // a core that dies fails the page's call, and the next one
  assert.throws(() => m.cwrap("die", null, [])(), /native core exited \(code 3\)/);
  assert.throws(() => num(1), /native core exited \(code 3\)/);
  // each factory call is its own core, as each dist instance is its own heap
  assert.strictEqual((await page.create()).cwrap("num", "number", ["number"])(9), 9);
});

test("a pointer export's window is mirrored in the page's heap, both ways, and grows as wasm memory does", async () => {
  const page = pageWith(fakeBridge(), "fake-core", "createFakeCore");
  const m = await page.create();
  const ptr = m.cwrap("buf", "number", [])();
  assert.ok(Number.isInteger(ptr) && ptr > 0, "an offset into the page's heap, as the dist returns one");
  new Uint8Array(m.HEAPU8.buffer, ptr, 8).set([1, 2, 3, 4, 5, 6, 7, 8]);
  assert.strictEqual(m.cwrap("sum", "number", [])(), 36, "the page's writes reach the core before the call");
  m.cwrap("bump", null, [])();
  assert.deepStrictEqual([...new Uint8Array(m.HEAPU8.buffer, ptr, 8)], [2, 3, 4, 5, 6, 7, 8, 9],
    "the core's writes reach the page after the call");
  const before = m.HEAPU8.buffer;
  const big = m.cwrap("big", "number", [])();
  assert.ok(m.HEAPU8.buffer.byteLength >= big + 100000, "the heap grew to hold the new window");
  assert.notStrictEqual(m.HEAPU8.buffer, before, "and the views were re-made over the grown buffer");
  assert.strictEqual(before.byteLength, 0, "the old buffer is detached, as wasm memory growth detaches it");
  assert.deepStrictEqual([...new Uint8Array(m.HEAPU8.buffer, ptr, 8)], [2, 3, 4, 5, 6, 7, 8, 9], "and kept its bytes");
  assert.strictEqual(m.cwrap("sum", "number", [])(), 44);
});

test("the bridge refuses a request it cannot trust, by answering it, never by throwing at the server", () => {
  const bridge = fakeBridge();
  const made = bridge.respond(JSON.stringify({ op: "new", core: "fake-core" }));
  assert.ok(made.id > 0 && !made.error, JSON.stringify(made));
  const wrap = bridge.respond(JSON.stringify({ op: "cwrap", id: made.id, fn: "buf", ret: "number", argTypes: [] })).wrap;
  const opened = bridge.respond(JSON.stringify({ op: "call", id: made.id, wrap, args: [], mem: [] }));
  assert.strictEqual(opened.mem.length, 1, "the call that opened the window answers with it");
  const [offset] = opened.mem[0];
  const call = (mem) => bridge.respond(JSON.stringify({ op: "call", id: made.id, wrap, args: [], mem })).error;
  assert.strictEqual(call([[offset, "00".repeat(8)]]), undefined, "the open window, whole");
  for (const bad of [[[offset + 1, "00"]], [[offset, "00".repeat(9)]], [[offset, "zz".repeat(8)]], [["x", ""]], [7]]) {
    assert.match(call(bad), /the page sent memory that is not an open window/, JSON.stringify(bad));
  }
  assert.match(bridge.respond("{").error, /JSON/);
  assert.match(bridge.respond(JSON.stringify({ op: "new", core: "canary-display-watch" })).error, /no core canary-display-watch here/);
  assert.match(bridge.respond(JSON.stringify({ op: "call", id: 999, wrap: 0 })).error, /no instance 999/);
  assert.match(bridge.respond(JSON.stringify({ op: "call", id: made.id, wrap: 99 })).error, /no wrapped export 99/);
  assert.match(bridge.respond(JSON.stringify({ op: "call", id: made.id, wrap: "constructor" })).error,
    /no wrapped export constructor/, "an index, never a property of the wrapper list");
  assert.match(bridge.respond(JSON.stringify({ op: "eval", id: made.id })).error, /unknown request "eval"/);
});

test("the probe server's first stop: the stand-in at each core's dist URL, the endpoint, nothing else", async () => {
  const http = require("node:http");
  const bridge = fakeBridge();
  const server = http.createServer(async (req, res) => {
    if (await bridge.handle(req, res)) return;
    res.writeHead(404); res.end("fell through");
  }).listen(0);
  try {
    const base = `http://localhost:${server.address().port}`;
    const js = await fetch(`${base}/canary-local/emulator/dist/fake-core.js?v=1`);
    assert.strictEqual(js.status, 200);
    assert.match(js.headers.get("content-type"), /^text\/javascript/);
    const text = await js.text();
    assert.strictEqual(text, bridge.standin("fake-core"));
    assert.ok(!text.includes(probeBridge.PLACEHOLDER), "served with its config written in");
    assert.match(text, /"exportName":"createFakeCore"/);
    for (const miss of ["/canary-local/emulator/dist/fake-core.meta.json", "/canary-local/emulator/dist/canary-vision-core.js",
      "/canary-local/vision.html", "/canary-local/fake-core.js", "/canary-local/emulator/dist/sub/fake-core.js",
      "/x/canary-local/emulator/dist/fake-core.js"]) {
      assert.strictEqual(await (await fetch(base + miss)).text(), "fell through", miss);
    }
    // every spelling the probe servers ever read as the dist core's file
    // (until sweep A46 they decoded the pathname and joined it onto the root;
    // the index they look it up in now reads fewer) is the stand-in, sent as
    // written: fetch would tidy some of these before they left
    const raw = (path, method = "GET") => new Promise((done, failed) => {
      http.request({ host: "localhost", port: server.address().port, path, method }, (r) => {
        let body = "";
        r.on("data", (c) => { body += c; });
        r.on("end", () => done({ status: r.statusCode, body }));
      }).on("error", failed).end();
    });
    for (const spelling of ["/canary-local/emulator//dist/fake-core.js", "/canary-local//emulator/dist/fake-core.js?x=1",
      "/canary-local/emulator/dist/%66ake-core.js", "/canary-local/emulator/dist%2Ffake-core.js",
      "/canary-local/emulator/x/../dist/fake-core.js", "/canary-local/emulator/dist/%2e/fake-core.js",
      "/canary-local/emulator/dist/x/%2e%2e/fake-core.js"]) {
      assert.deepStrictEqual(await raw(spelling), { status: 200, body: text }, spelling);
    }
    // and nothing else of it is ever the committed file
    for (const method of ["POST", "HEAD", "PUT"]) {
      const r = await raw("/canary-local/emulator/dist/fake-core.js", method);
      assert.strictEqual(r.status, 405, `${method}: refused, not fallen through to the dist`);
    }
    assert.strictEqual((await raw("/canary-local/emulator/dist/%E0fake-core.js")).body, "fell through",
      "a path that cannot be decoded is the server's to refuse");
    const post = (body) => fetch(base + probeBridge.ENDPOINT, { method: "POST", body }).then((r) => r.json());
    assert.ok((await post(JSON.stringify({ op: "new", core: "fake-core" }))).id > 0);
    assert.match((await post("x".repeat((8 << 20) + 1))).error, /a request over/);
    assert.match((await (await fetch(base + probeBridge.ENDPOINT)).json()).error, /GET is not a request/);
  } finally {
    server.close();
  }
});

// used() is what keeps a native run from passing on the committed dist: a
// probe fails unless every core it asked for had its stand-in served AND
// called. A page that fell back to the dist (or never booted its core) must
// read as unused, and summary() must say which half was missing.
test("a native run whose page never loaded, or never called, the stand-in is not used()", async () => {
  const get = (bridge, url) => bridge.handle({ url, method: "GET" }, { writeHead() {}, end() {} });
  const call = (bridge, core) => {
    const made = bridge.respond(JSON.stringify({ op: "new", core }));
    const wrap = bridge.respond(JSON.stringify({ op: "cwrap", id: made.id, fn: "num", ret: "number", argTypes: ["number"] })).wrap;
    const r = bridge.respond(JSON.stringify({ op: "call", id: made.id, wrap, args: [5] }));
    assert.strictEqual(r.ret, 5, JSON.stringify(r));
  };
  const one = fakeBridge();
  assert.strictEqual(one.used(), false, "a fresh bridge");
  assert.match(one.summary(), /^fake-core: stand-in NOT served, 0 calls to this tree's 0 sources$/);
  assert.strictEqual(await get(one, "/canary-local/emulator/dist/fake-core.meta.json"), false);
  assert.strictEqual(one.used(), false, "a request for anything but the core");
  assert.strictEqual(await get(one, "/canary-local/emulator/dist/fake-core.js"), true);
  assert.strictEqual(one.used(), false, "served, never called: the page loaded the stand-in and never ran it");
  assert.match(one.summary(), /^fake-core: stand-in served, 0 calls/);
  one.respond(JSON.stringify({ op: "new", core: "fake-core" }));
  assert.strictEqual(one.used(), false, "an instance is not a call");
  call(one, "fake-core");
  assert.strictEqual(one.used(), true, "served and called");
  assert.match(one.summary(), /^fake-core: stand-in served, 1 call to this tree's 0 sources$/);

  const unserved = fakeBridge();
  call(unserved, "fake-core");
  assert.strictEqual(unserved.used(), false, "called, never served: the page got its core from somewhere else");
  assert.match(unserved.summary(), /^fake-core: stand-in NOT served, 1 call/);

  const core = { bin: process.execPath, args: [join(__dirname, "native/fake_core.js")] };
  const two = probeBridge.bridgeOver({
    "fake-core": { ...core, plan: BRIDGE_PLAN },
    "other-core": { ...core, plan: { ...BRIDGE_PLAN, name: "other-core", exportName: "createOtherCore" } },
  });
  await get(two, "/canary-local/emulator/dist/fake-core.js");
  call(two, "fake-core");
  assert.strictEqual(two.used(), false, "every core the probe asked for, not any one of them");
  assert.match(two.summary(), /^fake-core: stand-in served, 1 call[^;]*; other-core: stand-in NOT served, 0 calls/);
  await get(two, "/canary-local/emulator/dist/other-core.js");
  call(two, "other-core");
  assert.strictEqual(two.used(), true);
});

// gotoIdle stands in for waitUntil: "networkidle", which a page that calls
// its core every animation frame never reaches once each call is a request:
// the same 500 ms without a request in flight, not counting the core's own.
test("gotoIdle waits out the page's own requests and 500 ms of quiet, never the core's", async () => {
  const { EventEmitter } = require("node:events");
  const bridge = fakeBridge();
  const req = (path) => ({ url: () => `http://localhost:1${path}` });
  const page = new EventEmitter();
  page.goto = async (url, opts) => {
    assert.strictEqual(opts.waitUntil, "load", "the page's load event first, as networkidle implies");
    page.emit("request", req(probeBridge.ENDPOINT));   // a core call, in flight for good
    const late = req("/canary-local/assets/late.js");
    page.emit("request", late);
    setTimeout(() => page.emit("requestfinished", late), 300);
  };
  const t0 = Date.now();
  await bridge.gotoIdle(page, "http://localhost:1/canary-local/vision.html", { timeout: 5000 });
  assert.ok(Date.now() - t0 >= 780, `waited for the page's request and then 500 ms of quiet (took ${Date.now() - t0} ms)`);
  assert.strictEqual(page.listenerCount("request") + page.listenerCount("requestfinished") + page.listenerCount("requestfailed"), 0,
    "and left no listener behind");
  const stuck = new EventEmitter();
  stuck.goto = async () => { stuck.emit("request", req("/canary-local/stuck.json")); };
  await assert.rejects(bridge.gotoIdle(stuck, "http://localhost:1/x.html", { timeout: 700 }),
    /never went idle in 700 ms; in flight: http:\/\/localhost:1\/canary-local\/stuck\.json/);
  assert.strictEqual(stuck.listenerCount("request"), 0, "nor after a timeout");
});

test("the stand-in asks the page's policy for nothing new: no eval, no inline code, same-origin requests only", () => {
  const src = fs.readFileSync(join(__dirname, "native/core_standin.js"), "utf8").replace(/\/\/[^\n]*/g, "");
  assert.doesNotMatch(src, /\beval\s*\(|\bFunction\s*\(|setTimeout\s*\(\s*["'`]|document\.write|importScripts|createElement/);
  assert.doesNotMatch(src, /https?:/, "it posts to a path on the page's own origin");
  assert.strictEqual(src.split(probeBridge.PLACEHOLDER).length, 2, "the bridge writes its config in one place");
  assert.ok(probeBridge.ENDPOINT.startsWith("/") && !probeBridge.ENDPOINT.startsWith("//"));
});

test("every probe that drives a core page takes the bridge, before its own file lookup", () => {
  const takers = [];
  for (const f of fs.readdirSync(__dirname).filter((x) => /_probe\.mjs$/.test(x)).sort()) {
    const src = fs.readFileSync(join(__dirname, f), "utf8");
    const loadsCorePage = /canary-local\/(?:vision|eyes|smoke)\.html/.test(src);
    const m = /probeCores\(\[([^\]]*)\]\)/.exec(src);
    if (!loadsCorePage && !m) continue;
    assert.ok(m, `${f} opens a page that loads a dist core but never asks probeCores for it`);
    for (const name of m[1].match(/[\w-]+/g)) takers.push(`${f}:${name}`);
    const server = src.indexOf("createServer(");
    const handled = src.indexOf("if (cores && await cores.handle(req, res)) return;", server);
    const lookup = src.indexOf("readFile(", server);
    assert.ok(server > 0 && handled > server && handled < lookup, `${f}: the bridge answers before the file lookup`);
    assert.match(src, /if \(cores\) await cores\.gotoIdle\(page, url/, `${f}: a native run waits without the core's requests`);
    assert.match(src, /if \(cores && !cores\.used\(\)\) fail\(/, `${f}: a native run whose page never called the core fails`);
  }
  assert.deepStrictEqual(takers, ["audio_probe.mjs:canary-wap-audio", "eyes_probe.mjs:canary-vision-core",
    "vision_probe.mjs:canary-vision-core"]);
});

// CI's wasm job runs each of those probes on the dist, then again under
// LAB_CORES=native whenever the dist step ran, red or green, as the logic
// job does for the Node tests (A40).
test("CI runs each core probe on the dist, then on this tree's sources even after a red dist step", () => {
  const wf = fs.readFileSync(join(REPO, ".github/workflows/canary-local.yml"), "utf8");
  for (const [id, probe] of [["vision-probe", "vision_probe.mjs"], ["eyes-probe", "eyes_probe.mjs"], ["audio-probe", "audio_probe.mjs"]]) {
    const dist = new RegExp(String.raw`\n {8}id: ${id}\n {8}run: node canary-local/tests/${probe}\n`);
    assert.match(wf, dist, `${probe}: its dist step carries id ${id}`);
    const after = wf.slice(wf.search(dist));
    const next = /\n {6}- name: [^\n]*\n((?: {8}[^\n]*\n)+)/.exec(after.slice(1))[1];
    assert.ok(next.includes(`if: \${{ (success() || failure()) && steps.${id}.outcome != 'skipped' }}`), `${probe}: native step's if:`);
    assert.ok(next.includes("LAB_CORES: native") && next.includes(`run: node canary-local/tests/${probe}`),
      `${probe}: the step after its dist step runs it under LAB_CORES=native`);
  }
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
  "(Actions -> \"Rebuild emulator dist (pinned emsdk)\"); if not, the native build and the wasm one disagree " +
  "on this input (another compiler, a 64-bit ABI: see tests/native/README.md)";

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

// Sweep A42: boxes far outside the frame, where the pipeline's int and long
// arithmetic used to overflow (the center x + w/2, the cell px * cols, the
// area (long)w*h on wasm32's 32-bit long, and area * 100). The native build
// is 64-bit g++ and the dist wasm32 clang, so before A42 the same box read
// proximity "near" here and "unknown" there, and landed in different cells.
// The sources now take each of those in int64_t, where no int box overflows,
// so the two builds must agree on every one of these boxes, value for value.
// Each frame holds one extreme box, or two (so the occupied-cell mask sees
// both), and the clock runs on, so the FSM sees them as a visit too. Like
// the scenario above it is opt-in, and no CI job runs this file under
// LAB_CORES=native; vision.test.js's A42 test is the CI gate on the dist,
// and vision_wasm32.test.js (sweep A49) shows the 32-bit long half from the
// sources, in a clang wasm32 build of the pipeline beside a g++ one.
test("native vision core = committed dist on out-of-range boxes (LAB_CORES=native)", native, async () => {
  const pair = await Promise.all([require(join(ROOT, "emulator/dist/canary-vision-core.js"))(),
    cores.coreFactory("canary-vision-core")()]);
  const [d, n] = pair.map((m) => ({
    reset: m.cwrap("vision_emu_reset", null, []),
    config: m.cwrap("vision_emu_set_config", null, ["number", "number", "number", "number"]),
    begin: m.cwrap("vision_emu_begin_frame", null, []),
    push: m.cwrap("vision_emu_push_box", "number", Array(6).fill("number")),
    tick: m.cwrap("vision_emu_tick_json", "string", ["number"]),
  }));
  assert.ok(pair[1].nativeCore, "the second core is the native one");
  d.reset(); n.reset();
  d.config(0, 50, 1500, 10000); n.config(0, 50, 1500, 10000);
  const V = [-2147483648, -2000000000, -1, 0, 240, 50000, 2000000000, 2147483647];
  let t = 0, frames = 0;
  const frame = (boxes) => {
    d.begin(); n.begin();
    for (const b of boxes) assert.strictEqual(n.push(...b, 90, 0), d.push(...b, 90, 0), "push_box " + STALE);
    t += 100;
    const want = d.tick(t);
    assert.strictEqual(n.tick(t), want, `boxes ${JSON.stringify(boxes)} at ${t} ms ` + STALE);
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

// What the browser probes run under LAB_CORES=native: the stand-in, in its
// own context, over the bridge, over the native core. It must answer exactly
// as cores.js's module over the same build does, the audio frame written
// through the stand-in's own HEAP16 included.
test("the probes' stand-in answers as the native module does, call for call (LAB_CORES=native)", native, async () => {
  const bridge = await probeBridge.probeCores(["canary-vision-core", "canary-wap-audio"]);
  const vis = [await pageWith(bridge, "canary-vision-core", "createCanaryVisionCore").create(),
    await cores.coreFactory("canary-vision-core")()];
  const [sv, dv] = vis.map((m) => ({
    contract: m.cwrap("vision_emu_contract_json", "string", []),
    config: m.cwrap("vision_emu_set_config", null, ["number", "number", "number", "number"]),
    begin: m.cwrap("vision_emu_begin_frame", null, []),
    push: m.cwrap("vision_emu_push_box", "number", Array(6).fill("number")),
    tick: m.cwrap("vision_emu_tick_json", "string", ["number"]),
  }));
  assert.strictEqual(sv.contract(), dv.contract());
  const r = lcg(41);
  let t = 0;
  for (let i = 0; i < 500; i++) {
    if (r(100) === 0) { const cfg = [r(3), r(120), r(9000), r(9000)]; sv.config(...cfg); dv.config(...cfg); }
    sv.begin(); dv.begin();
    for (let b = r(4); b > 0; b--) {
      const box = [r(640), r(480), r(300), r(400), r(100), r(3)];
      assert.strictEqual(sv.push(...box), dv.push(...box));
    }
    t += 50 + r(300);
    assert.strictEqual(sv.tick(t), dv.tick(t), `vision tick ${i}`);
  }
  const aud = [await pageWith(bridge, "canary-wap-audio", "createCanaryAudioCore").create(),
    await cores.coreFactory("canary-wap-audio")()];
  const [sa, da] = aud.map((m) => ({ m, N: m.cwrap("audio_emu_frame_samples", "number", [])(),
    ptr: m.cwrap("audio_emu_frame_ptr", "number", []), proc: m.cwrap("audio_emu_process_frame", "string", []) }));
  let ph = 0;
  for (const [hz, frames] of [[0, 30], [3400, 25], [0, 25], [3400, 25], [0, 25], [3400, 25], [0, 90], [-1, 40]]) {
    for (let f = 0; f < frames; f++) {
      const pcm = new Int16Array(sa.N);
      for (let i = 0; i < pcm.length; i++) {
        if (hz <= 0) { pcm[i] = hz < 0 ? r(20000) - 10000 : 0; continue; }
        ph += (2 * Math.PI * hz) / 16000;
        pcm[i] = Math.round(8000 * Math.sin(ph));
      }
      for (const c of [sa, da]) new Int16Array(c.m.HEAP16.buffer, c.ptr(), c.N).set(pcm);
      assert.strictEqual(sa.proc(), da.proc(), `audio frame ${f} at ${hz} Hz`);
    }
  }
  assert.ok(bridge.calls.get("canary-vision-core") > 1000 && bridge.calls.get("canary-wap-audio") > 500, bridge.summary());
});
