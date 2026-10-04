// canary-local/tests/native/core_worker.js — the thread that owns the pipes
// to natively built Lab cores (sweep A40; loaded by cores.js, never a test).
//
// A wasm call is synchronous, so a page test cannot await a reply from a
// child process. cores.js posts each request here and blocks on a
// SharedArrayBuffer; this thread writes the request to the core's stdin,
// waits for its one reply line, and wakes the test with it:
//
//   ctl[0]  0 waiting · 1 answered · 2 failed (the text says why)
//   ctl[1]  the answer's length in bytes, which follow the 8-byte header
//
// One request is in flight at a time (the test's thread is blocked on it),
// so one answer slot is enough.

"use strict";

const { parentPort, workerData } = require("node:worker_threads");
const { spawn } = require("node:child_process");

const ctl = new Int32Array(workerData.sab, 0, 2);
const data = new Uint8Array(workerData.sab, 8);
const kids = new Map();
let nextId = 1;

function answer(ok, text) {
  let bytes = Buffer.from(text, "utf8");
  if (bytes.length > data.length) {
    ok = false;
    bytes = Buffer.from(`a ${bytes.length}-byte answer does not fit the ${data.length}-byte slot`);
  }
  data.set(bytes);
  Atomics.store(ctl, 1, bytes.length);
  Atomics.store(ctl, 0, ok ? 1 : 2);
  Atomics.notify(ctl, 0);
}

function gone(kid, why) {
  if (kid.gone) return;
  kid.gone = why;
  if (kid.waiting) {
    kid.waiting = false;
    answer(false, `native core ${why}`);
  }
}

function take(kid) {
  const nl = kid.buf.indexOf("\n");
  if (nl < 0) return;
  const line = kid.buf.slice(0, nl);
  kid.buf = kid.buf.slice(nl + 1);
  if (!kid.waiting || kid.buf) {
    // a line nobody asked for: the reply stream can no longer be trusted
    kid.child.kill();
    return gone(kid, `wrote a line it was not asked for: ${JSON.stringify(line.slice(0, 80))}`);
  }
  kid.waiting = false;
  answer(true, line);
}

parentPort.on("message", (msg) => {
  try {
    if (msg.op === "spawn") {
      const child = spawn(msg.bin, msg.args || [], { stdio: ["pipe", "pipe", "inherit"] });
      const kid = { child, buf: "", waiting: false, gone: null };
      const id = nextId++;
      kids.set(id, kid);
      child.stdout.setEncoding("utf8");
      child.stdout.on("data", (s) => { kid.buf += s; take(kid); });
      child.stdin.on("error", () => {});   // a dead core's EPIPE: "close" says why
      child.on("error", (e) => gone(kid, `could not start: ${e.message}`));
      // "close", not "exit": a reply written just before exiting is read first
      child.on("close", (code, signal) => gone(kid, `exited (${signal || "code " + code})`));
      return answer(true, `${id} ${child.pid || 0}`);
    }
    const kid = kids.get(msg.id);
    if (!kid) return answer(false, `no native core instance ${msg.id}`);
    if (kid.gone) return answer(false, `native core ${kid.gone}`);
    kid.waiting = true;
    kid.child.stdin.write(msg.line + "\n");
  } catch (e) {
    answer(false, `core_worker.js: ${e && e.stack ? e.stack : e}`);
  }
});
