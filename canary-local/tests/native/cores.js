// canary-local/tests/native/cores.js — which Lab WebAssembly core a page test
// drives (sweep A40).
//
//   const visionFactory = require("./native/cores.js").coreFactory("canary-vision-core");
//
// By default (LAB_CORES unset, or "dist") coreFactory returns the committed
// bundle, require("../../emulator/dist/<name>.js") — the very module the
// tests always loaded, and the only one CI's default runs and the Lab use.
//
// LAB_CORES=native builds the core from THIS tree's sources instead: the
// sources, flags and include paths build.sh hands em++ for the core's flavor
// (read from build.sh, not copied), compiled with g++ (or $CXX) beside a
// one-macro <emscripten.h> and core_server.cpp, and served to the test over
// a pipe. The factory it returns has the dist factory's shape: call it, await
// a module, cwrap its exports; ccall and the HEAP views are there when the
// dist exports them (build.sh's -sEXPORTED_RUNTIME_METHODS). Calls stay
// synchronous, as they are in wasm: a worker thread owns the pipes and the
// test's thread waits on a SharedArrayBuffer.
//
// What it proves and what it does not: the tree's sources pass the page
// tests. It is not the dist — another compiler, and a 64-bit ABI where wasm
// is 32-bit — so a page change that needs a core change still ships with
// CI's pinned-emsdk rebuild of dist/, which the default run keeps checking.
// See README.md beside this file.

"use strict";

const fs = require("node:fs");
const os = require("node:os");
const { join, relative } = require("node:path");
const { execFile } = require("node:child_process");

const LAB = join(__dirname, "..", "..");
const EMU = join(LAB, "emulator");
const REPO = join(LAB, "..");

// The cores a page test can ask for: the dist bundle's name → build.sh's
// flavor. A pointer export needs its window: how many bytes past the
// address JavaScript may read and write (the dist's whole heap is one
// ArrayBuffer; a native core has no wasm heap, so it opens only these).
const CORES = {
  "canary-vision-core": { flavor: "vision" },
  "canary-wap-audio": {
    flavor: "audio",
    memory: { audio_emu_frame_ptr: (call) => 2 * call("audio_emu_frame_samples") },
  },
};

const MODES = ["dist", "native"];

function mode() {
  const v = process.env.LAB_CORES;
  if (v === undefined || v === "") return "dist";
  if (!MODES.includes(v)) {
    throw new Error(`LAB_CORES=${v}: expected "dist" (the default: the committed emulator/dist bundles) ` +
      `or "native" (this tree's sources, built with g++; see canary-local/tests/native/README.md)`);
  }
  return v;
}

function coreFactory(name) {
  if (!CORES[name]) {
    throw new Error(`no Lab core ${name}; the cores are ${Object.keys(CORES).join(", ")}`);
  }
  if (mode() === "dist") return require(join(EMU, "dist", name + ".js"));
  return nativeFactory(name);
}

// ── build.sh, read ──────────────────────────────────────────────────────────

// $NAME / ${NAME} → vars[NAME]; anything else that starts with $ (or a
// backquoted command) is refused. `used` collects the names it read.
function expand(text, vars, where, used) {
  if (text.includes("`")) throw new Error(`build.sh ${where}: cannot expand a backquoted command in ${JSON.stringify(text)}`);
  const out = text.replace(/\$(?:\{(\w+)\}|(\w+))/g, (whole, a, b) => {
    const k = a || b;
    if (!(k in vars)) throw new Error(`build.sh ${where}: $${k} is not a variable cores.js can resolve`);
    if (used) used.add(k);
    return vars[k];
  });
  if (out.includes("$")) throw new Error(`build.sh ${where}: cannot expand ${JSON.stringify(text)}`);
  return out;
}

// The words of a shell array body or command: quotes, $VARs, comments, line
// continuations, and a whole-word "${ARRAY[@]}" spliced in. `seen` collects
// the variables and arrays the words read.
function words(text, vars, arrays, where, seen) {
  const out = [];
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (/\s/.test(c)) { i++; continue; }
    if (c === "\\" && text[i + 1] === "\n") { i += 2; continue; }
    if (c === "#") { while (i < text.length && text[i] !== "\n") i++; continue; }
    const splice = /^"\$\{(\w+)\[@\]\}"(?=\s|$)/.exec(text.slice(i));
    if (splice) {
      if (!arrays[splice[1]]) throw new Error(`build.sh ${where}: no array ${splice[1]} before it is used`);
      seen.arrays.add(splice[1]);
      out.push(...arrays[splice[1]]);
      i += splice[0].length;
      continue;
    }
    let w = "";
    while (i < text.length && !/\s/.test(text[i])) {
      if (text[i] === "'") {
        const j = text.indexOf("'", i + 1);
        if (j < 0) throw new Error(`build.sh ${where}: unterminated '`);
        w += text.slice(i + 1, j);
        i = j + 1;
      } else if (text[i] === '"') {
        const j = text.indexOf('"', i + 1);
        if (j < 0) throw new Error(`build.sh ${where}: unterminated "`);
        w += expand(text.slice(i + 1, j), vars, where, seen.vars);
        i = j + 1;
      } else if (text[i] === "\\") {
        w += text[i + 1];
        i += 2;
      } else {
        let j = i;
        while (j < text.length && !/[\s'"\\]/.test(text[j])) j++;
        // bash would glob or brace-expand these; cores.js would take them as written
        if (/[*?[{]/.test(text.slice(i, j).replace(/\$\{\w+\}/g, ""))) {
          throw new Error(`build.sh ${where}: cannot follow the unquoted pattern ${JSON.stringify(text.slice(i, j))}`);
        }
        w += expand(text.slice(i, j), vars, where, seen.vars);
        i = j;
      }
    }
    out.push(w);
  }
  return out;
}

// NAME="value" lines, in order; a value cores.js cannot expand (a command
// substitution, $PWD) is left out, and fails only if something uses it.
// deps[NAME] is the set of variables its value read.
function assignments(text, vars, deps) {
  for (const m of text.matchAll(/^[ \t]*([A-Z_][A-Z0-9_]*)="([^"\n]*)"[ \t]*$/gm)) {
    if (m[1] in SEEDS) continue;
    const used = new Set();
    try {
      vars[m[1]] = expand(m[2], vars, m[1], used);
      deps[m[1]] = used;
    } catch {
      delete vars[m[1]];
    }
  }
}

// The lines of `text` (continuations joined) that write the shell name `v`:
// v=, v+=, v[i]=, and declare/local/typeset/readonly/export/unset/mapfile/
// readarray/read naming it.
function writesOf(text, v) {
  const assign = new RegExp(String.raw`(?:^|[\s;&|(!])${v}(?:\[[^\]\n]*\])?\+?=`);
  const builtin = new RegExp(String.raw`(?:^|[\s;&|(!])(?:declare|local|typeset|readonly|export|unset|mapfile|readarray|read)\b[^\n;&|]*?[\s=]${v}\b`);
  return text.replace(/\\\n/g, " ").split("\n").filter((l) => assign.test(l) || builtin.test(l)).map((l) => l.trim());
}

// The two build.sh values that come from the shell, not from a line.
const SEEDS = { EMU_DIR: EMU, REPO_ROOT: REPO };

// Every EMSCRIPTEN_KEEPALIVE function in a source: its name, return kind and
// parameter types. Only what a wasm call can carry is accepted — 32-bit-or-
// narrower integers in, an integer, a C string, a pointer or nothing out —
// and anything else is refused by name, never guessed.
const INT_TYPES = new Set(["int", "signed", "signed int", "unsigned", "unsigned int", "int32_t", "uint32_t",
  "int16_t", "uint16_t", "int8_t", "uint8_t", "short", "unsigned short", "char", "signed char",
  "unsigned char", "bool"]);
const norm = (t) => t.replace(/\s+/g, " ").replace(/\s*\*/g, "*").trim();

function exportsOf(file) {
  const src = fs.readFileSync(file, "utf8").replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, "");
  const out = [];
  for (const m of src.matchAll(/\bEMSCRIPTEN_KEEPALIVE\s+([^()]*?)\s*\b([A-Za-z_]\w*)\s*\(([^)]*)\)/g)) {
    const [, retText, name, paramText] = m;
    const where = `${relative(REPO, file)}: ${name}`;
    const ret = norm(retText);
    const kind = ret === "void" ? "v" : ret === "const char*" ? "s" : ret.endsWith("*") ? "p"
      : INT_TYPES.has(ret) ? "n" : null;
    if (!kind) throw new Error(`${where} returns ${ret}, which a native core cannot serve as wasm does`);
    const params = [];
    const list = norm(paramText);
    if (list && list !== "void") {
      for (const p of list.split(",").map((s) => s.trim())) {
        const type = INT_TYPES.has(p) ? p : p.replace(/\s*\b[A-Za-z_]\w*$/, "");
        if (!INT_TYPES.has(type)) throw new Error(`${where} takes ${JSON.stringify(p)}, which a native core cannot pass as wasm does`);
        params.push(type);
      }
    }
    out.push({ name, ret, kind, params, file });
  }
  return out;
}

// The build of one core, as build.sh does it for the core's flavor: its
// sources (the array its `for src in` loop walks), the flags its `em++ -c`
// line passes, and the link line's EXPORT_NAME and runtime methods. What it
// cannot follow it refuses by name, never skips: an array or variable the
// build reads that is written anywhere but its one literal (an append, a
// reassignment, an element), a compile line with anything beyond the flags
// array, EMCC_CFLAGS. `script` is build.sh's text (native_cores.test.js
// hands it edited copies).
function buildPlan(name, script = fs.readFileSync(join(EMU, "build.sh"), "utf8")) {
  const core = CORES[name];
  if (!core) throw new Error(`no Lab core ${name}`);
  const head = `if [[ "$FLAVOR" == "${core.flavor}" ]]; then\n`;
  const start = script.indexOf(head);
  if (start < 0) throw new Error(`build.sh has no "${core.flavor}" flavor block (${head.trim()})`);
  const end = script.indexOf("\nfi\n", start);
  if (end < 0) throw new Error(`build.sh: the "${core.flavor}" block never closes`);
  const block = script.slice(start, end);
  const refuse = (why) => {
    throw new Error(`build.sh, for the "${core.flavor}" core: ${why}. cores.js cannot follow that, so it ` +
      `refuses rather than build something build.sh does not (canary-local/tests/native/cores.js)`);
  };
  if (/\bEMCC_CFLAGS\b/.test(script)) refuse("EMCC_CFLAGS adds flags to every em++ call");

  // build.sh's top level (every flavor block cut out of it), then the block
  const top = script.slice(0, start).replace(/^if \[\[ "\$FLAVOR" == "\w+" \]\]; then\n[\s\S]*?\nfi\n/gm, "");
  const vars = { ...SEEDS };
  const deps = {};
  assignments(top, vars, deps);
  assignments(block, vars, deps);
  if (vars.OUT_BASE !== name) {
    throw new Error(`build.sh's "${core.flavor}" block writes ${vars.OUT_BASE}.js, not ${name}.js`);
  }
  const arrays = {};
  const reads = {};
  for (const m of block.matchAll(/^[ \t]*([A-Z_][A-Z0-9_]*)=\(\n([\s\S]*?)^[ \t]*\)[ \t]*$/gm)) {
    reads[m[1]] = { vars: new Set(), arrays: new Set() };
    arrays[m[1]] = words(m[2], vars, arrays, m[1], reads[m[1]]);
  }

  // The loop and the compile line, exactly; any other shape is refused.
  const lines = block.replace(/\\\n/g, " ").split("\n");
  const loops = lines.filter((l) => /^\s*for\s+src\b/.test(l));
  const compiles = lines.filter((l) => /(?:^|[\s;&|(])em(?:\+\+|cc)\s/.test(l) && /\s-c(?:\s|$)/.test(l));
  const loopRe = /^\s*for src in "\$\{(\w+)\[@\]\}"; do\s*$/;
  const compileRe = /^\s*em\+\+ -c "\$src" "\$\{(\w+)\[@\]\}" -o "\$obj"\s*$/;
  if (loops.length !== 1 || !loopRe.test(loops[0])) {
    refuse(`expected one \`for src in "\${SOURCES[@]}"; do\` line, found ${JSON.stringify(loops.map((l) => l.trim()))}`);
  }
  if (compiles.length !== 1 || !compileRe.test(compiles[0])) {
    refuse(`expected one \`em++ -c "$src" "\${FLAGS[@]}" -o "$obj"\` line, found ${JSON.stringify(compiles.map((l) => l.trim()))}`);
  }
  const srcArr = loopRe.exec(loops[0])[1];
  const flagArr = compileRe.exec(compiles[0])[1];
  if (!arrays[srcArr]) refuse(`no ${srcArr}=( … ) source array`);
  if (!arrays[flagArr]) refuse(`no ${flagArr}=( … ) flag array`);

  // Every array the build reads (the two, and what they splice in) and every
  // variable their words expand (and what those were built from) is written
  // exactly once, by the line cores.js read; src is written only by the loop.
  const readArrays = new Set([srcArr, flagArr]);
  for (const a of readArrays) for (const b of reads[a].arrays) readArrays.add(b);
  const readVars = new Set();
  for (const a of readArrays) for (const v of reads[a].vars) readVars.add(v);
  for (const v of readVars) for (const d of deps[v] || []) readVars.add(d);
  const scope = top + "\n" + block;
  for (const a of readArrays) {
    const w = writesOf(scope, a);
    if (w.length !== 1 || w[0] !== `${a}=(`) refuse(`${a} is written ${w.length} times (${JSON.stringify(w)}), not only by its ${a}=( … ) literal`);
  }
  for (const v of readVars) {
    const w = writesOf(scope, v);
    const literal = v in SEEDS ? w.length === 1 : w.length === 1 && new RegExp(String.raw`^${v}="[^"]*"$`).test(w[0]);
    if (!literal) refuse(`$${v} is written ${w.length} times (${JSON.stringify(w)}), not only by one ${v}="…" line`);
  }
  const srcWrites = writesOf(block, "src");
  if (srcWrites.length) refuse(`$src is written in the block (${JSON.stringify(srcWrites)}), not only by the loop`);
  const sources = arrays[srcArr];
  for (const s of sources) {
    if (!/\.(cpp|cc|cxx)$/.test(s)) throw new Error(`build.sh's ${srcArr} lists ${s}: cores.js builds C++ sources only`);
    if (!fs.existsSync(s)) throw new Error(`build.sh's ${srcArr} lists ${s}, which does not exist`);
  }
  const exportName = (/-sEXPORT_NAME=(\w+)/.exec(block) || [])[1];
  const runtime = ((/-sEXPORTED_RUNTIME_METHODS=([\w,]+)/.exec(block) || [])[1] || "").split(",").filter(Boolean);
  if (!exportName) throw new Error(`build.sh's "${core.flavor}" link line has no -sEXPORT_NAME`);
  const exports = sources.flatMap(exportsOf);
  if (!exports.length) throw new Error(`no EMSCRIPTEN_KEEPALIVE export in ${srcArr}`);
  for (const e of exports) {
    if (e.kind === "p" && !(core.memory && core.memory[e.name])) {
      throw new Error(`${e.name} returns a pointer: give it a window in CORES["${name}"].memory (tests/native/cores.js)`);
    }
  }
  return { name, flavor: core.flavor, sources, flags: arrays[flagArr], exportName, runtime, exports,
    memory: core.memory || {} };
}

// core_exports.inc: the extern "C" declarations and the table core_server.cpp serves.
function exportTable(plan) {
  const lines = [`// Generated by canary-local/tests/native/cores.js for ${plan.name} (build.sh's "${plan.flavor}" flavor).`];
  for (const e of plan.exports) lines.push(`extern "C" ${e.ret} ${e.name}(${e.params.join(", ")});`);
  lines.push("static const Export kExports[] = {");
  for (const e of plan.exports) {
    const call = `${e.name}(${e.params.map((t, i) => `(${t})a[${i}]`).join(", ")})`;
    const body = e.kind === "v" ? `${call}; return ret_void();`
      : e.kind === "s" ? `return ret_str(${call});`
      : e.kind === "p" ? `return ret_ptr((const void*)${call});`
      : `return ret_num((int32_t)${call});`;
    lines.push(`  {"${e.name}", ${e.params.length}, [](const int32_t* a) -> Ret { (void)a; ${body} }},`);
  }
  lines.push("};", "");
  return lines.join("\n");
}

// ── the native build ────────────────────────────────────────────────────────

const STUB_EMSCRIPTEN_H = "// Written by canary-local/tests/native/cores.js: a hosted build keeps every\n" +
  "// extern \"C\" function, so the keep-alive marker has nothing to do.\n" +
  "#pragma once\n#define EMSCRIPTEN_KEEPALIVE\n";

const builds = new Map();

function build(plan) {
  if (!builds.has(plan.name)) builds.set(plan.name, compile(plan));
  return builds.get(plan.name);
}

async function compile(plan) {
  const dir = fs.mkdtempSync(join(os.tmpdir(), `securacv-${plan.name}-`));
  process.once("exit", () => fs.rmSync(dir, { recursive: true, force: true }));
  fs.writeFileSync(join(dir, "emscripten.h"), STUB_EMSCRIPTEN_H);
  fs.writeFileSync(join(dir, "core_exports.inc"), exportTable(plan));
  const bin = join(dir, plan.name);
  const cxx = process.env.CXX || "g++";
  const args = ["-I", dir, ...plan.flags, ...plan.sources, join(__dirname, "core_server.cpp"), "-o", bin];
  const t0 = Date.now();
  await new Promise((resolve, reject) => {
    execFile(cxx, args, { maxBuffer: 64 << 20 }, (err, stdout, stderr) => {
      if (err) {
        reject(new Error(`LAB_CORES=native: ${cxx} could not build ${plan.name} from build.sh's ` +
          `"${plan.flavor}" sources:\n${cxx} ${args.join(" ")}\n${stderr || err.message}`));
      } else resolve();
    });
  });
  process.stderr.write(`LAB_CORES=native: ${plan.name} built from this tree's ${plan.sources.length} ` +
    `"${plan.flavor}" sources with ${cxx} in ${((Date.now() - t0) / 1000).toFixed(1)} s\n`);
  return { plan, bin };
}

// ── the pipe, kept synchronous ──────────────────────────────────────────────

const REPLY_CAP = 1 << 20;
// How long a call may go unanswered (native_cores.test.js shortens it to
// test the stuck-core path).
const limits = { timeoutMs: 30000 };
let channel = null;

function open() {
  if (channel) return channel;
  const { Worker } = require("node:worker_threads");
  const sab = new SharedArrayBuffer(8 + REPLY_CAP);
  const worker = new Worker(join(__dirname, "core_worker.js"), { workerData: { sab } });
  worker.unref();
  channel = { worker, ctl: new Int32Array(sab, 0, 2), data: new Uint8Array(sab, 8) };
  return channel;
}

// One request to the worker; the test's thread waits for the answer.
function rpc(msg, what, pid) {
  const ch = open();
  Atomics.store(ch.ctl, 0, 0);
  ch.worker.postMessage(msg);
  if (Atomics.wait(ch.ctl, 0, 0, limits.timeoutMs) === "timed-out") {
    // The core is stuck (it would outlive the test: a loop never reads the
    // stdin that closes when the test exits), so it is killed; and a late
    // answer would land on the next request, so the channel starts over.
    if (pid) {
      try { process.kill(pid, "SIGKILL"); } catch { /* already gone */ }
    }
    ch.worker.terminate();
    channel = null;
    throw new Error(`native core: no answer to ${what} in ${limits.timeoutMs / 1000} s (core killed)`);
  }
  const text = Buffer.from(ch.data.subarray(0, Atomics.load(ch.ctl, 1))).toString("utf8");
  if (Atomics.load(ch.ctl, 0) !== 1) throw new Error(text);
  return text;
}

const HEAP_VIEWS = { HEAP8: Int8Array, HEAPU8: Uint8Array, HEAP16: Int16Array, HEAPU16: Uint16Array,
  HEAP32: Int32Array, HEAPU32: Uint32Array, HEAPF32: Float32Array, HEAPF64: Float64Array };

const hexOf = (bytes) => Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString("hex");

// One instance: what `await factory()` resolves to on the dist.
function instance({ plan, bin, args = [] }) {
  const ch = open();
  const [id, pid] = rpc({ op: "spawn", bin, args }, `starting ${plan.name}`).split(" ").map(Number);
  const ask = (line) => {
    if (channel !== ch) throw new Error(`native core ${plan.name}: torn down after an earlier timeout`);
    const reply = rpc({ op: "call", id, line }, line.slice(0, 60), pid);
    if (reply.startsWith("! ")) throw new Error(`native core ${plan.name}: ${reply.slice(2)} (asked: ${line.slice(0, 60)})`);
    return reply;
  };
  const byName = new Map(plan.exports.map((e) => [e.name, e]));
  const mod = { nativeCore: { name: plan.name, pid, sources: plan.sources.map((s) => relative(REPO, s)) } };

  // The heap the HEAP views see: one window per pointer an export returned,
  // pushed to the core before every call and read back after it.
  let heap = new ArrayBuffer(64 * 1024);
  let top = 16;
  const windows = new Map();   // hex address → { offset, len, sent }
  const views = () => {
    for (const k of plan.runtime) if (HEAP_VIEWS[k]) mod[k] = new HEAP_VIEWS[k](heap);
  };
  views();
  // For the browser probes' bridge (probe_cores.js), which mirrors this heap
  // in the page: the buffer the HEAP views see now, and its open windows.
  // Not enumerable, so the module's keys stay the dist's.
  Object.defineProperties(mod.nativeCore, {
    heap: { value: () => heap },
    windows: { value: () => [...windows.values()].map((w) => ({ offset: w.offset, len: w.len })) },
  });
  const push = () => {
    for (const [addr, w] of windows) {
      const now = new Uint8Array(heap, w.offset, w.len);
      if (Buffer.compare(now, w.sent) !== 0) {
        ask(`w ${addr} ${hexOf(now)}`);
        w.sent.set(now);
      }
    }
  };
  const pull = () => {
    for (const [addr, w] of windows) {
      const bytes = Buffer.from(ask(`r ${addr} ${w.len}`).slice(3), "hex");
      new Uint8Array(heap, w.offset, w.len).set(bytes);
      w.sent.set(bytes);
    }
  };
  const invoke = (e, args) => {
    push();
    const reply = ask(["c", e.name, ...args.map((x) => Number(x) | 0)].join(" "));
    pull();
    const kind = reply.slice(1, 2), rest = reply.slice(3);
    if (kind === "v") return undefined;
    if (kind === "n") return Number(rest);
    if (kind === "s") return rest;
    if (kind === "p") return windowAt(e, rest);
    throw new Error(`native core ${plan.name}: unreadable reply ${reply.slice(0, 60)}`);
  };
  const callNumber = (fn) => {
    const e = byName.get(fn);
    if (!e || e.kind !== "n") throw new Error(`${fn} is not a number export of ${plan.name}`);
    return invoke(e, []);
  };
  function windowAt(e, addr) {
    if (/^0+$/.test(addr)) return 0;
    if (!windows.has(addr)) {
      const len = plan.memory[e.name](callNumber);
      if (!(Number.isInteger(len) && len > 0)) throw new Error(`${e.name}: window length ${len}`);
      const offset = top;
      top = offset + Math.ceil(len / 16) * 16;
      if (top > heap.byteLength) {
        // as wasm memory growth does: the old buffer is detached, re-view it
        heap = heap.transfer(Math.ceil(top / 65536) * 65536);
        views();
      }
      ask(`m ${addr} ${len}`);
      windows.set(addr, { offset, len, sent: Buffer.alloc(len) });
      pull();
    }
    return windows.get(addr).offset;
  }

  mod.cwrap = (fn, returnType, argTypes) => {
    const e = byName.get(fn);
    if (!e) throw new Error(`native core ${plan.name}: no export ${fn} (its exports: ${[...byName.keys()].join(", ")})`);
    for (const t of argTypes || []) {
      if (t !== "number" && t !== "boolean") throw new Error(`${fn}: a native core passes number arguments only, not ${t}`);
    }
    // Refused here, before any call runs: wasm would read a number as a
    // string address, or hand back a string's address as a number.
    if (returnType === "string" && e.kind !== "s") throw new Error(`${fn} returns ${e.ret}, not a C string`);
    if (returnType !== "string" && e.kind === "s") {
      throw new Error(`${fn} returns a C string: cwrap it as "string" (a native core has no wasm address for it)`);
    }
    // As the dist's cwrap: with number arguments and any return but
    // "string" it hands back the raw export, so a "boolean" return is the
    // export's number, unconverted. Only ccall converts it.
    return (...args) => invoke(e, args);
  };
  if (plan.runtime.includes("ccall")) {
    mod.ccall = (fn, returnType, argTypes, args) => {
      const out = mod.cwrap(fn, returnType, argTypes)(...(args || []));
      return returnType === "boolean" ? Boolean(out) : out;
    };
  }
  if (plan.runtime.includes("UTF8ToString")) {
    mod.UTF8ToString = () => {
      throw new Error(`native core ${plan.name}: no wasm heap to read a string from; cwrap the export as "string"`);
    };
  }
  if (!plan.runtime.includes("cwrap")) delete mod.cwrap;
  return mod;
}

function nativeFactory(name) {
  const plan = buildPlan(name);   // refuse an unreadable build.sh before any test runs
  return async () => instance(await build(plan));
}

// instance and limits are exported for native_cores.test.js, which drives the
// pipe with a stand-in core (fake_core.js) instead of a compiled one; build
// for probe_cores.js, which compiles a core once before its probe's browser
// starts and hands each page its own instance.
module.exports = { coreFactory, buildPlan, build, exportsOf, exportTable, mode, CORES, instance, limits };
