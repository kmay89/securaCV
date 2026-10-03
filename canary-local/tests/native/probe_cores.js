// canary-local/tests/native/probe_cores.js — the browser probes' way onto
// this tree's core sources (sweep A41; loaded by a probe, never a test).
//
//   const bridge = await probeCores(["canary-vision-core"]);   // null unless LAB_CORES=native
//   ... in the probe's server: if (bridge && await bridge.handle(req, res)) return;
//
// vision.html, eyes.html and smoke.html load their core with a <script> tag
// (emulator/dist/canary-vision-core.js, canary-wap-audio.js), so the probes
// that drive those pages in Chromium (vision_probe.mjs, eyes_probe.mjs,
// audio_probe.mjs) always ran the committed dist. Under LAB_CORES=native the
// probe's server answers that URL with core_standin.js instead: a factory of
// the same name and shape that forwards every call to the core cores.js
// builds from this tree's sources (the build the Node page tests use, A40),
// over synchronous same-origin requests to this server. Nothing about the
// page changes: the stand-in is a 'self' script, its requests are 'self'
// connects, and the page's policy and markup are served as committed.
//
// Unset (or LAB_CORES=dist) probeCores returns null and the probe serves the
// committed bytes exactly as before. Any other value is refused by cores.js.
//
// What a pass proves: this tree's sources pass the probe in Chromium. It is
// not the dist (another compiler, a 64-bit ABI, see README.md), and a page
// served the stand-in has made a request per core call that the dist does
// not make; the default run keeps checking the dist itself.

"use strict";

const fs = require("node:fs");
const { join, posix } = require("node:path");
const cores = require("./cores.js");

// The URL the stand-in posts to. Nothing in the repository lives there, and
// the probe servers ask the bridge before they look a path up.
const ENDPOINT = "/__lab_native_core";
const BODY_CAP = 8 << 20;
const STANDIN = join(__dirname, "core_standin.js");
const PLACEHOLDER = "__LAB_NATIVE_CORE__";

// The bridge over built cores: { name: { plan, bin, args? } }, as cores.build
// returns them (native_cores.test.js hands it a stand-in core instead).
// `answer` is the whole protocol and is synchronous, as the page's request is.
function bridgeOver(built, { distPrefix = "/canary-local/emulator/dist/" } = {}) {
  const instances = new Map();   // id → { mod, wraps: [] }
  const calls = new Map(Object.keys(built).map((name) => [name, 0]));
  const served = new Set();
  let nextId = 1;

  const standin = (name) => {
    const { plan } = built[name];
    const config = { core: name, exportName: plan.exportName, endpoint: ENDPOINT };
    const src = fs.readFileSync(STANDIN, "utf8");
    if (src.split(PLACEHOLDER).length !== 2) throw new Error(`core_standin.js must name ${PLACEHOLDER} once`);
    return src.replace(PLACEHOLDER, JSON.stringify(config));
  };

  const memOf = (mod) => mod.nativeCore.windows().map(({ offset, len }) =>
    [offset, Buffer.from(mod.nativeCore.heap(), offset, len).toString("hex")]);

  // The page's copy of each window, written into this side's heap before the
  // call (cores.js pushes what changed to the core), and every window as it
  // stands after it.
  const run = (inst, msg, fn) => {
    const { mod } = inst;
    const open = new Map(mod.nativeCore.windows().map((w) => [w.offset, w.len]));
    for (const entry of msg.mem || []) {
      const [offset, hex] = Array.isArray(entry) ? entry : [];
      const bytes = typeof hex === "string" && /^(?:[0-9a-f]{2})*$/.test(hex) ? Buffer.from(hex, "hex") : null;
      if (!bytes || !open.has(offset) || bytes.length !== open.get(offset)) {
        throw new Error(`native core ${mod.nativeCore.name}: the page sent memory that is not an open window`);
      }
      new Uint8Array(mod.nativeCore.heap(), offset, bytes.length).set(bytes);
    }
    const ret = fn();
    return { ret, heap: mod.nativeCore.heap().byteLength, mem: memOf(mod) };
  };

  const argsOf = (msg) => (Array.isArray(msg.args) ? msg.args : []);

  function answer(msg) {
    if (!msg || typeof msg !== "object") throw new Error("native core bridge: not a request");
    if (msg.op === "new") {
      if (!Object.hasOwn(built, msg.core)) throw new Error(`native core bridge: no core ${msg.core} here`);
      const mod = cores.instance(built[msg.core]);
      const id = nextId++;
      instances.set(id, { mod, core: msg.core, wraps: [] });
      const keys = Object.keys(mod).filter((k) => k !== "nativeCore");
      return {
        id,
        functions: keys.filter((k) => typeof mod[k] === "function"),
        views: keys.filter((k) => ArrayBuffer.isView(mod[k])),
        heap: mod.nativeCore.heap().byteLength,
        mem: memOf(mod),
        nativeCore: { name: mod.nativeCore.name, pid: mod.nativeCore.pid, sources: mod.nativeCore.sources },
      };
    }
    const inst = instances.get(msg.id);
    if (!inst) throw new Error(`native core bridge: no instance ${msg.id}`);
    const { mod } = inst;
    if (msg.op === "cwrap") {
      if (typeof mod.cwrap !== "function") throw new Error(`native core ${inst.core}: no cwrap`);
      // cores.js refuses here what the dist cannot do, before any call runs
      inst.wraps.push(mod.cwrap(msg.fn, msg.ret, msg.argTypes));
      return { wrap: inst.wraps.length - 1 };
    }
    if (msg.op === "call") {
      const fn = Number.isInteger(msg.wrap) ? inst.wraps[msg.wrap] : undefined;
      if (!fn) throw new Error(`native core ${inst.core}: no wrapped export ${msg.wrap}`);
      calls.set(inst.core, calls.get(inst.core) + 1);
      return run(inst, msg, () => fn(...argsOf(msg)));
    }
    if (msg.op === "ccall" || msg.op === "UTF8ToString") {
      if (typeof mod[msg.op] !== "function") throw new Error(`native core ${inst.core}: no ${msg.op}`);
      calls.set(inst.core, calls.get(inst.core) + 1);
      return run(inst, msg, () => (msg.op === "ccall"
        ? mod.ccall(msg.fn, msg.ret, msg.argTypes, argsOf(msg))
        : mod.UTF8ToString(...argsOf(msg))));
    }
    throw new Error(`native core bridge: unknown request ${JSON.stringify(msg.op)}`);
  }

  // One request body in, one reply out, never a throw: an error is answered,
  // not a 500, so a page that catches the throw (as it would a wasm trap)
  // does not also leave a failed load in its console. The reply carries an
  // Error's message (the refusal this bridge, cores.js or JSON.parse wrote),
  // never the exception itself: its stack, and anything thrown that is not an
  // Error, stay in this server's log.
  function respond(text) {
    try {
      return answer(JSON.parse(text));
    } catch (e) {
      if (e instanceof Error) return { error: e.message };
      console.error("native core bridge: a request threw a non-Error:", e);
      return { error: "native core bridge: the request failed (see the probe server's log)" };
    }
  }

  // The path a probe server reads for a request (decodeURIComponent of the
  // URL's pathname, joined onto the repository root, which normalizes it),
  // so that no spelling of a dist core's URL (dist//x.js, %2e%2e, %66) slips
  // past the stand-in to the committed bytes. null when it cannot be decoded:
  // the server refuses that too.
  const pathOf = (url) => {
    try { return posix.normalize(decodeURIComponent(new URL(url, "http://x").pathname)); } catch { return null; }
  };

  // The probe server's first stop: the stand-in in place of each dist core,
  // and the endpoint. True when it took the request. In a native run the
  // committed core is never served: a GET of its URL gets the stand-in, and
  // anything else is refused here rather than falling through to the file.
  async function handle(req, res) {
    const path = pathOf(req.url);
    if (path === null) return false;
    const name = Object.keys(built).find((n) => path === `${distPrefix}${n}.js`);
    if (name) {
      if (req.method !== "GET") {
        res.writeHead(405, { allow: "GET", "content-type": "text/plain", "cache-control": "no-store" });
        res.end(`native core bridge: ${name}.js is the stand-in here, and a ${req.method} of it is not served`);
        return true;
      }
      served.add(name);
      res.writeHead(200, { "content-type": "text/javascript", "cache-control": "no-store" });
      res.end(standin(name));
      return true;
    }
    if (path !== ENDPOINT) return false;
    let reply;
    if (req.method !== "POST") reply = { error: `native core bridge: ${req.method} is not a request` };
    else {
      const chunks = [];
      let size = 0;
      for await (const c of req) {
        size += c.length;
        if (size <= BODY_CAP) chunks.push(c);
      }
      reply = size > BODY_CAP ? { error: `native core bridge: a request over ${BODY_CAP} bytes` }
        : respond(Buffer.concat(chunks).toString("utf8"));
    }
    res.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
    res.end(JSON.stringify(reply));
    return true;
  }

  // page.goto(url, { waitUntil: "networkidle" }) for a page whose core
  // calls are now requests (vision.html calls its core every animation
  // frame, so the network never idles): the same wait, no request in flight
  // for 500 ms, counting every request but the bridge's own.
  async function gotoIdle(page, url, { timeout = 30000 } = {}) {
    const inflight = new Set();
    let quietSince = Date.now();
    const bridged = (r) => {
      try { return new URL(r.url()).pathname === ENDPOINT; } catch { return false; }
    };
    const start = (r) => { if (!bridged(r)) inflight.add(r); };
    const end = (r) => { if (inflight.delete(r) && !inflight.size) quietSince = Date.now(); };
    page.on("request", start);
    page.on("requestfinished", end);
    page.on("requestfailed", end);
    try {
      const deadline = Date.now() + timeout;
      await page.goto(url, { waitUntil: "load", timeout });
      while (inflight.size || Date.now() - quietSince < 500) {
        if (Date.now() > deadline) {
          throw new Error(`page.goto: the network (the native core's own requests aside) never went idle in ${timeout} ms; ` +
            `in flight: ${[...inflight].map((r) => r.url()).join(", ")}`);
        }
        await new Promise((r) => setTimeout(r, 50));
      }
    } finally {
      page.off("request", start);
      page.off("requestfinished", end);
      page.off("requestfailed", end);
    }
  }

  // What the probe reports, and checks: a native run whose page never
  // loaded the stand-in, or never called it, proved nothing.
  const summary = () => Object.keys(built).map((n) =>
    `${n}: stand-in ${served.has(n) ? "served" : "NOT served"}, ${calls.get(n)} call${calls.get(n) === 1 ? "" : "s"} ` +
    `to this tree's ${built[n].plan.sources.length} sources`).join("; ");
  const used = () => Object.keys(built).every((n) => served.has(n) && calls.get(n) > 0);

  return { answer, respond, handle, standin, gotoIdle, summary, used, calls, ENDPOINT };
}

// null unless LAB_CORES=native; then each named core is built once (cores.js
// reads build.sh and compiles with g++ or $CXX) and the bridge is returned.
async function probeCores(names, options) {
  if (cores.mode() !== "native") return null;
  const built = {};
  for (const name of names) {
    if (!cores.CORES[name]) throw new Error(`no Lab core ${name}; the cores are ${Object.keys(cores.CORES).join(", ")}`);
    built[name] = await cores.build(cores.buildPlan(name));
  }
  return bridgeOver(built, options);
}

module.exports = { probeCores, bridgeOver, ENDPOINT, PLACEHOLDER };
