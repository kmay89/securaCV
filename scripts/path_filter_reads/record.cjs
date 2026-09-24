// scripts/path_filter_reads/record.cjs — the Node half of the path-filter
// read recorder (sweep CI2). Preloaded into every node process of a CI step
// through NODE_OPTIONS="--require <abs path to this file>"; it records each
// repo-relative path a process hands to the fs read calls, so
// scripts/check_path_filter_reads.py can hold them to the workflow's
// `pull_request` paths filter. A filtered workflow whose test reads a file
// its filter does not list is a workflow that a PR editing that file never
// runs: the red lands on the next unrelated PR instead.
//
// Inert unless PATH_FILTER_READS_DIR is set (the directory is made on the
// first record). It never changes what a call returns or throws: each wrapper
// notes its path argument and then calls the original with the same `this`
// and arguments.
//
// What is recorded, per process, into <dir>/node-<pid>-<thread>-<rand>.jsonl:
//   {"suite", "op", "path", "at", "proc"}
//   suite  the test the read belongs to: PATH_FILTER_READS_SUITE when a parent
//          process set it, else this process's own entry script. A test's
//          child (node, or python3 through the Python half's sitecustomize)
//          inherits it, so a generator a test spawns is charged to the test.
//   op     the fs call (readFileSync, existsSync, readdirSync, ...).
//   path   repo-relative, forward slashes. Paths outside the repo (tmp dirs,
//          the node install) are never recorded.
//   at     the outermost stack frame in the suite's own file (the test's own
//          line, not a helper's), else the innermost repo frame that is not
//          this file.
//   proc   this process's entry script, repo-relative ("" for none).
// One record per (op, path) per process: the gate needs the set, not a log.
//
// Not seen: reads by native code or by a process whose environment was
// replaced (spawn with a fresh `env`), and reads by shell tools (cat, git).
"use strict";

const DIR = process.env.PATH_FILTER_READS_DIR;
if (DIR) install(DIR);

function install(dir) {
  const fs = require("node:fs");
  const path = require("node:path");
  const { fileURLToPath } = require("node:url");
  const { threadId } = require("node:worker_threads");

  const ROOT = path.resolve(__dirname, "..", "..");
  const SELF = __filename;
  const orig = {
    mkdirSync: fs.mkdirSync,
    openSync: fs.openSync,
    writeSync: fs.writeSync,
  };

  const rel = (abs) => {
    const r = path.relative(ROOT, abs);
    if (!r || r.startsWith("..") || path.isAbsolute(r)) return null;
    return r.split(path.sep).join("/");
  };

  const toAbs = (p) => {
    try {
      if (typeof p === "string") {
        if (p.startsWith("file:")) return fileURLToPath(p);
        return path.resolve(p);
      }
      if (p instanceof URL) return p.protocol === "file:" ? fileURLToPath(p) : null;
      if (Buffer.isBuffer(p)) return path.resolve(p.toString());
    } catch {
      // A malformed URL or path is the call's own error to throw, not ours.
    }
    return null; // a file descriptor, or something fs itself will refuse
  };

  const entry = process.argv[1] ? rel(path.resolve(process.argv[1])) || "" : "";
  const inherited = process.env.PATH_FILTER_READS_SUITE || "";
  const suite = inherited || entry || "<node>";
  // `node --test a.js b.js` runs each file in a child of its own: leave the
  // children to name themselves. Every other process hands its suite down.
  const isTestRunner = process.execArgv.includes("--test");
  if (!inherited && entry && !isTestRunner) process.env.PATH_FILTER_READS_SUITE = suite;
  const suiteAbs = path.join(ROOT, suite);

  let fd = null;
  const seen = new Set();
  let busy = false;

  const frameOf = () => {
    const saveP = Error.prepareStackTrace;
    const saveL = Error.stackTraceLimit;
    let sites = [];
    try {
      Error.prepareStackTrace = (_, cs) => cs;
      Error.stackTraceLimit = 80;
      const holder = {};
      Error.captureStackTrace(holder, frameOf);
      sites = holder.stack || [];
    } catch {
      sites = [];
    } finally {
      Error.prepareStackTrace = saveP;
      Error.stackTraceLimit = saveL;
    }
    let inRepo = "";
    let inSuite = "";
    for (const cs of sites) {
      let file = cs && typeof cs.getFileName === "function" ? cs.getFileName() : null;
      if (!file) continue;
      if (file.startsWith("file:")) {
        try { file = fileURLToPath(file); } catch { continue; }
      }
      if (file === SELF || !path.isAbsolute(file)) continue;
      const r = rel(file);
      if (!r) continue;
      if (file === suiteAbs) inSuite = `${r}:${cs.getLineNumber()}`; // outermost wins: the test's line
      if (!inRepo) inRepo = `${r}:${cs.getLineNumber()}`;
    }
    return inSuite || inRepo;
  };

  const note = (op, p) => {
    if (busy) return;
    busy = true;
    try {
      const abs = toAbs(p);
      const r = abs && rel(abs);
      if (!r) return;
      const key = `${op}\0${r}`;
      if (seen.has(key)) return;
      seen.add(key);
      if (fd === null) {
        const name = `node-${process.pid}-${threadId}-${Math.random().toString(36).slice(2, 10)}.jsonl`;
        orig.mkdirSync(dir, { recursive: true });
        fd = orig.openSync(path.join(dir, name), "a");
      }
      const line = JSON.stringify({ suite, op, path: r, at: frameOf(), proc: entry }) + "\n";
      orig.writeSync(fd, line);
    } catch {
      // The recorder must never be the reason a test fails; a lost record
      // shows up as a suite the checker never heard from, and that fails there.
    } finally {
      busy = false;
    }
  };

  // Is an open(2) flags argument read-only? fs's own default is "r".
  const readOnly = (flags) => {
    if (flags === undefined || flags === null) return true;
    if (typeof flags === "number") return (flags & 3) === fs.constants.O_RDONLY;
    return typeof flags === "string" && /^r[s]?$/.test(flags);
  };

  const wrap = (obj, name, test) => {
    const fn = obj && obj[name];
    if (typeof fn !== "function") return;
    const wrapped = function (p) {
      if (!test || test(arguments)) note(name, p);
      return fn.apply(this, arguments);
    };
    Object.defineProperty(wrapped, "name", { value: fn.name });
    Object.defineProperty(wrapped, "length", { value: fn.length });
    for (const key of Reflect.ownKeys(fn)) {
      if (key === "length" || key === "name" || key === "prototype") continue;
      try { Object.defineProperty(wrapped, key, Object.getOwnPropertyDescriptor(fn, key)); } catch { /* keep going */ }
    }
    obj[name] = wrapped;
  };

  const openFlags = (args) => readOnly(args[1]);
  const streamFlags = (args) => {
    const o = args[1];
    return readOnly(o && typeof o === "object" ? o.flags : undefined);
  };
  for (const name of ["readFileSync", "existsSync", "readdirSync", "statSync", "lstatSync",
    "accessSync", "opendirSync", "readFile", "readdir", "stat", "lstat", "access", "opendir", "exists"]) {
    wrap(fs, name);
  }
  wrap(fs, "openSync", openFlags);
  wrap(fs, "open", openFlags);
  wrap(fs, "createReadStream", streamFlags);
  wrap(fs, "copyFileSync");
  wrap(fs, "cpSync");
  const promises = fs.promises;
  for (const name of ["readFile", "readdir", "stat", "lstat", "access", "opendir", "copyFile", "cp"]) {
    wrap(promises, name);
  }
  wrap(promises, "open", openFlags);

  // An ES module's `import { readFileSync } from "node:fs"` binds the
  // builtin's ESM facade, which is built on first import, so a test module
  // loaded after this preload already gets the wrapped calls (the recorder
  // tests pin that). Resync anyway, for a facade built before it.
  require("node:module").syncBuiltinESMExports();

  if (entry) note("exec", path.resolve(process.argv[1]));
}
