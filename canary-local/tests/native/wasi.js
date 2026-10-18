// canary-local/tests/native/wasi.js — a whole Lab core built for wasm32-wasi
// from this tree's sources, with the host's clang and a WASI sysroot, and run
// under node:wasi (sweep A58). cores_wasm32.test.js holds it to the g++ build
// of the same sources, call for call.
//
//   const wasi = require("./native/wasi.js");
//   const t = wasi.tools();                              // what was found, and what is missing
//   const make = await wasi.wasmFactory("canary-vision-core");
//   const mod = await make();                            // the dist module's shape: cwrap, HEAP views
//
// Why: the emulator's dist is wasm32, where long is 32 bits, as on the ESP32;
// LAB_CORES=native and every firmware host suite build with the host's 64-bit
// long. vision_wasm32.test.js (A49) shows the 32-bit long from the sources
// for the Vision box pipeline alone, built freestanding. The rest of the
// Vision core (detect_config.cpp, presence_fsm.cpp, voxel_tracker.cpp, the
// bindings' JSON) and the WAP audio core need a C library (snprintf), and the
// Vision shim's Preferences map needs the C++ library, which a freestanding
// build lacks. A WASI sysroot has both.
//
// What it builds: each core exactly as build.sh hands it to em++ — the same
// sources, flags and include paths, read through cores.js's buildPlan, not
// copied — compiled one source at a time by clang++ --target=wasm32-wasi
// against the sysroot's C library (wasi-libc), libc++ and compiler-rt
// builtins, beside a one-macro <emscripten.h> and wasm32/wasi_support.cpp.
// wasm-ld links it as a WASI reactor that exports each EMSCRIPTEN_KEEPALIVE
// function by name (an export it cannot find fails the link) and refuses any
// undefined symbol. node:wasi instantiates it; its only imports are the WASI
// calls libc's stdio links. The module has the dist's shape: cwrap, and
// ccall, UTF8ToString and the HEAP views where build.sh's
// -sEXPORTED_RUNTIME_METHODS lists them, with emscripten's conversions (a
// cwrap with number arguments and a non-string return is the raw export).
//
// What it is not: emscripten, or the dist. It is the host's clang (18 on CI's
// runner) and wasi-libc, where the dist is emscripten's clang and its musl
// libc, so it settles what the sources compute in wasm32 with a 32-bit long,
// not every way two toolchains can differ (an undefined signed overflow, a
// libm that rounds otherwise). The dist is still what ships, and
// native_cores.test.js still holds the native build to it.
//
// Where a tool or a sysroot part is missing the tests skip and say which;
// CORES_WASM32=require (CI) turns that into a failure. WASM32_CLANGXX and
// WASM_LD name the tools as wasm32.js reads them; WASI_SYSROOT names a
// sysroot (a wasi-sdk's share/wasi-sysroot) where clang has none of its own.

"use strict";

const fs = require("node:fs");
const os = require("node:os");
const { join, relative, resolve } = require("node:path");
const { spawnSync, execFile } = require("node:child_process");
const w32 = require("./wasm32.js");
const { buildPlan } = require("./cores.js");

const REPO = join(__dirname, "..", "..", "..");
const SUPPORT = join(__dirname, "wasm32", "wasi_support.cpp");
const TARGET = "--target=wasm32-wasi";

// em++ keeps and exports an EMSCRIPTEN_KEEPALIVE function; here `used` keeps
// it and wasm-ld's --export (one per function the plan read) exports it.
const STUB_EMSCRIPTEN_H = "// Written by canary-local/tests/native/wasi.js: the wasm32-wasi build keeps\n" +
  "// each export, and wasm-ld exports it by name (--export=<name>).\n" +
  "#pragma once\n#define EMSCRIPTEN_KEEPALIVE __attribute__((used))\n";

function required() {
  const v = process.env.CORES_WASM32;
  if (v === undefined || v === "") return false;
  if (v === "require") return true;
  throw new Error(`CORES_WASM32=${v}: expected "require" (fail where a tool or the WASI sysroot is missing) ` +
    `or nothing (skip there); see canary-local/tests/native/README.md`);
}

function run(cmd, args, input) {
  const r = spawnSync(cmd, args, { encoding: "utf8", input });
  return r.error ? { ok: false, out: String(r.error) } : { ok: r.status === 0, out: (r.stdout || "") + (r.stderr || "") };
}

// What a whole core needs past wasm32.js's clang++ and wasm-ld: the sysroot's
// C library and headers, libc++ and libc++abi, and compiler-rt's wasm32
// builtins. Each missing part is named with the Ubuntu package that ships it.
function sysrootMissing(clang, major, sysroot) {
  const base = [TARGET, ...(sysroot ? ["--sysroot=" + sysroot] : [])];
  const fileOf = (name) => {
    const r = run(clang, [...base, "-print-file-name=" + name]);
    const p = r.ok ? r.out.trim() : "";
    return p && p !== name && fs.existsSync(p) ? p : null;
  };
  const n = major || "<n>";
  const where = sysroot ? ` under WASI_SYSROOT=${sysroot}` : "";
  const missing = [];
  if (!fileOf("libc.a") || !fileOf("crt1-reactor.o") || !run(clang, [...base, "-x", "c", "-fsyntax-only", "-"], "#include <stdio.h>\n").ok) {
    missing.push(`the wasm32-wasi C library and headers${where} (Ubuntu: wasi-libc)`);
  }
  if (!fileOf("libc++.a") || !run(clang, [...base, "-x", "c++", "-fsyntax-only", "-"], "#include <map>\n").ok) {
    missing.push(`libc++ for wasm32-wasi${where} (Ubuntu: libc++-${n}-dev-wasm32)`);
  }
  if (!fileOf("libc++abi.a")) missing.push(`libc++abi for wasm32-wasi${where} (Ubuntu: libc++abi-${n}-dev-wasm32)`);
  const rt = run(clang, [...base, "-rtlib=compiler-rt", "-print-libgcc-file-name"]);
  if (!rt.ok || !fs.existsSync(rt.out.trim())) {
    missing.push(`compiler-rt's wasm32 builtins for ${clang} (Ubuntu: libclang-rt-${n}-dev-wasm32)`);
  }
  return missing;
}

// The tools, found once: wasm32.js's clang++ (with a wasm32 target), wasm-ld
// and host compiler, then the WASI sysroot parts that clang finds.
let found = null;
function tools() {
  if (found) return found;
  const req = required();
  const t = w32.tools();
  const sysroot = process.env.WASI_SYSROOT || null;
  const missing = [...t.missing];
  if (t.clang && !missing.some((m) => m.startsWith("a wasm32 target"))) {
    const major = /^(\d+)/.exec(t.clangVersion || "");
    missing.push(...sysrootMissing(t.clang, major && major[1], sysroot));
  }
  found = { ...t, sysroot, missing, required: req };
  return found;
}

// node:test options: skip by name where something is missing, unless required.
function gate(t = tools()) {
  if (!t.missing.length || t.required) return {};
  return { skip: `needs ${t.missing.join(", ")}; CORES_WASM32=require fails here instead (tests/native/README.md)` };
}

function assertTools(t = tools()) {
  if (t.missing.length) {
    throw new Error(`CORES_WASM32=require, and the wasm32-wasi build is missing ${t.missing.join(", ")} ` +
      `(install them, or point WASM32_CLANGXX / WASM_LD / WASI_SYSROOT at them)`);
  }
}

// The host compiler's own sizeof(long) in bits, from its predefined macros.
function hostLongBits(t = tools()) {
  const r = run(t.cxx, ["-dM", "-E", "-x", "c++", "-"], "");
  const m = r.ok && /#define __SIZEOF_LONG__ (\d+)/.exec(r.out);
  if (!m) throw new Error(`${t.cxx} did not say its __SIZEOF_LONG__:\n${r.out}`);
  return Number(m[1]) * 8;
}

let scratch = null;
function tmp(tag) {
  if (!scratch) {
    scratch = fs.mkdtempSync(join(os.tmpdir(), "securacv-cores-wasm32-"));
    process.once("exit", () => fs.rmSync(scratch, { recursive: true, force: true }));
  }
  return fs.mkdtempSync(join(scratch, tag + "-"));
}

function exec(cmd, args, what) {
  return new Promise((done, failed) => {
    execFile(cmd, args, { maxBuffer: 64 << 20 }, (err, stdout, stderr) => {
      if (err) failed(new Error(`${what}:\n${cmd} ${args.join(" ")}\n${stderr || err.message}`));
      else done(stdout);
    });
  });
}

// A variant of a core's build plan, for a test's self-check: include
// directories put first (a mutated copy of a header) and sources swapped for
// scratch copies ({ absolute source path: replacement path }). Every swap
// must name a source the plan builds.
// (build.sh spells a firmware source through emulator/../../firmware, so
// paths are compared resolved.)
function variant(plan, { includeFirst = [], replace = {} } = {}) {
  const swaps = new Map(Object.entries(replace).map(([from, to]) => [resolve(from), to]));
  const built = new Set(plan.sources.map((s) => resolve(s)));
  for (const s of swaps.keys()) {
    if (!built.has(s)) throw new Error(`${plan.name} does not build ${relative(REPO, s)}; it builds ` +
      plan.sources.map((x) => relative(REPO, x)).join(", "));
  }
  return { ...plan, flags: [...includeFirst.flatMap((d) => ["-I", d]), ...plan.flags],
    sources: plan.sources.map((s) => swaps.get(resolve(s)) || s) };
}

// One core, built for wasm32-wasi: each source compiled (two at a time),
// then linked as a reactor exporting the plan's functions by name.
async function buildWasm(plan, t = tools()) {
  assertTools(t);
  const dir = tmp(plan.name);
  fs.writeFileSync(join(dir, "emscripten.h"), STUB_EMSCRIPTEN_H);
  const base = [TARGET, ...(t.sysroot ? ["--sysroot=" + t.sysroot] : [])];
  const units = [...plan.sources, SUPPORT];
  const objs = units.map((s, i) => join(dir, `${i}.o`));
  const t0 = Date.now();
  let next = 0;
  const worker = async () => {
    while (next < units.length) {
      const i = next++;
      await exec(t.clang, [...base, "-I", dir, ...plan.flags, "-c", units[i], "-o", objs[i]],
        `the wasm32-wasi build of ${plan.name} could not compile ${relative(REPO, units[i])}`);
    }
  };
  await Promise.all([worker(), worker()]);
  const out = join(dir, plan.name + ".wasm");
  await exec(t.clang, [...base, "-mexec-model=reactor", "--ld-path=" + t.wasmLd, ...objs,
    ...plan.exports.map((e) => "-Wl,--export=" + e.name), "-o", out],
  `linking ${plan.name} for wasm32-wasi (no undefined symbol allowed)`);
  process.stderr.write(`CORES_WASM32: ${plan.name} built for wasm32-wasi from ${plan.sources.length} ` +
    `"${plan.flavor}" sources with ${t.clang} ${t.clangVersion} in ${((Date.now() - t0) / 1000).toFixed(1)} s\n`);
  return { plan, wasm: out, module: new WebAssembly.Module(fs.readFileSync(out)) };
}

const HEAP_VIEWS = { HEAP8: Int8Array, HEAPU8: Uint8Array, HEAP16: Int16Array, HEAPU16: Uint16Array,
  HEAP32: Int32Array, HEAPU32: Uint32Array, HEAPF32: Float32Array, HEAPF64: Float64Array };

// One instance (its own memory, as each dist instance is its own heap), in
// the shape `await factory()` resolves to on the dist.
function instance({ plan, module }) {
  const { WASI } = require("node:wasi");
  const wasi = new WASI({ version: "preview1", args: [], env: {}, returnOnExit: true });
  const inst = new WebAssembly.Instance(module, wasi.getImportObject());
  wasi.initialize(inst);
  const e = inst.exports;
  const memory = e.memory;
  const utf8 = (ptr) => {
    const bytes = new Uint8Array(memory.buffer);
    let end = ptr;
    while (end < bytes.length && bytes[end] !== 0) end++;
    return Buffer.from(bytes.buffer, ptr, end - ptr).toString("utf8");
  };
  const exported = new Set(plan.exports.map((x) => x.name));
  const fn = (name) => {
    if (!exported.has(name) || typeof e[name] !== "function") {
      throw new Error(`wasm32 core ${plan.name}: no export ${name} (its exports: ${[...exported].join(", ")})`);
    }
    return e[name];
  };
  const mod = {};
  Object.defineProperty(mod, "wasm32Core", { value: { name: plan.name, longBits: e.lab_wasm32_long_bits(),
    sources: plan.sources.map((s) => relative(REPO, s)), imports: WebAssembly.Module.imports(module),
    exports: WebAssembly.Module.exports(module) } });
  // As emscripten's: with number (or boolean) arguments and any return but
  // "string", cwrap hands back the raw export; a "string" return reads the C
  // string at the address the export returns.
  const cwrap = (name, returnType, argTypes) => {
    const f = fn(name);
    for (const a of argTypes || []) {
      if (a !== "number" && a !== "boolean") throw new Error(`${name}: the wasm32 core passes number arguments only, not ${a}`);
    }
    return returnType === "string" ? (...args) => utf8(f(...args)) : f;
  };
  if (plan.runtime.includes("cwrap")) mod.cwrap = cwrap;
  if (plan.runtime.includes("ccall")) {
    mod.ccall = (name, returnType, argTypes, args) => {
      const out = cwrap(name, returnType, argTypes)(...(args || []));
      return returnType === "boolean" ? Boolean(out) : out;
    };
  }
  if (plan.runtime.includes("UTF8ToString")) mod.UTF8ToString = utf8;
  // a view of the memory as it is now, so a grown memory is seen, as the
  // dist re-makes its views when its memory grows
  for (const k of plan.runtime) {
    if (HEAP_VIEWS[k]) Object.defineProperty(mod, k, { enumerable: true, get: () => new HEAP_VIEWS[k](memory.buffer) });
  }
  return mod;
}

// A factory with the dist's shape, over one wasm32-wasi build of the core.
const builds = new Map();
async function wasmFactory(name, options) {
  const plan = options ? variant(buildPlan(name), options) : buildPlan(name);
  let built;
  if (options) built = await buildWasm(plan);
  else {
    if (!builds.has(name)) builds.set(name, buildWasm(plan));
    built = await builds.get(name);
  }
  return async () => instance(built);
}

module.exports = { tools, gate, assertTools, required, hostLongBits, variant, buildWasm, wasmFactory, instance, SUPPORT };
