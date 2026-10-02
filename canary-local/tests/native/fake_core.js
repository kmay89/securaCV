// canary-local/tests/native/fake_core.js — a stand-in core for
// native_cores.test.js (sweep A40; not a test itself). It speaks
// core_server.cpp's protocol and misbehaves on request, so the pipe's
// failure paths are tested in the default run, with no compiler:
//
//   c num <i32>   →  =n <the i32 it was sent>   (what the bridge passed)
//   c str         →  =s <text>
//   c die         →  exits with status 3, no reply
//   c noise       →  two replies to one request
//   c spin        →  never replies and never reads stdin again

"use strict";

const out = (line) => process.stdout.write(line + "\n");

require("node:readline").createInterface({ input: process.stdin }).on("line", (line) => {
  const [op, name, ...args] = line.split(" ");
  if (op !== "c") return out("! the fake core only takes calls");
  if (name === "num") return out("=n " + args[0]);
  if (name === "str") return out("=s hello from the fake core");
  if (name === "die") process.exit(3);
  if (name === "noise") return process.stdout.write("=n 1\n=n 2\n");
  if (name === "spin") for (;;) { /* stuck, as a core in an endless loop is */ }
  return out("! no export " + name);
});
