// canary-local/tests/native/fake_core.js — a stand-in core for
// native_cores.test.js (sweeps A40, A41; not a test itself). It speaks
// core_server.cpp's protocol and misbehaves on request, so the pipe's
// failure paths are tested in the default run, with no compiler:
//
//   c num <i32>   →  =n <the i32 it was sent>   (what the bridge passed)
//   c str         →  =s <text>
//   c die         →  exits with status 3, no reply
//   c noise       →  two replies to one request
//   c spin        →  never replies and never reads stdin again
//
// and holds two buffers a pointer export hands out, as a core's frame
// buffer is (the browser probes' bridge mirrors them in the page's heap):
//
//   c buf         →  =p a0     (8 bytes)       c big  →  =p b0000 (100000 bytes)
//   c sum         →  =n <the sum of buf's bytes, as the core sees them>
//   c bump        →  =v, after adding 1 to every byte of buf
//   m / w / r     →  core_server.cpp's window open, write and read

"use strict";

const out = (line) => process.stdout.write(line + "\n");
const buffers = new Map([[0xa0, Buffer.alloc(8)], [0xb0000, Buffer.alloc(100000)]]);
const windows = new Map();   // address → length, opened by "m"

// the buffer and offset an address inside an open window falls in
function at(addr, len) {
  for (const [base, n] of windows) {
    if (addr >= base && addr + len <= base + n) return [buffers.get(base), addr - base];
  }
  return [null, 0];
}

require("node:readline").createInterface({ input: process.stdin }).on("line", (line) => {
  const [op, name, ...args] = line.split(" ");
  if (op === "m") {
    const addr = parseInt(name, 16);
    if (!buffers.has(addr)) return out("! m: no export returned that address");
    windows.set(addr, Number(args[0]));
    return out("=v");
  }
  if (op === "w" || op === "r") {
    const addr = parseInt(name, 16);
    const len = op === "w" ? args[0].length / 2 : Number(args[0]);
    const [buf, off] = at(addr, len);
    if (!buf) return out(`! ${op}: outside every open window`);
    if (op === "w") {
      Buffer.from(args[0], "hex").copy(buf, off);
      return out("=v");
    }
    return out("=b " + buf.subarray(off, off + len).toString("hex"));
  }
  if (op !== "c") return out("! the fake core only takes calls and windows");
  if (name === "num") return out("=n " + args[0]);
  if (name === "str") return out("=s hello from the fake core");
  if (name === "die") process.exit(3);
  if (name === "noise") return process.stdout.write("=n 1\n=n 2\n");
  if (name === "spin") for (;;) { /* stuck, as a core in an endless loop is */ }
  if (name === "buf") return out("=p a0");
  if (name === "big") return out("=p b0000");
  if (name === "sum") return out("=n " + buffers.get(0xa0).reduce((a, b) => a + b, 0));
  if (name === "bump") {
    const b = buffers.get(0xa0);
    for (let i = 0; i < b.length; i++) b[i] = (b[i] + 1) & 255;
    return out("=v");
  }
  return out("! no export " + name);
});
