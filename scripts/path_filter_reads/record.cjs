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
// It arms the Python half for the processes it starts: when this directory
// is not on PYTHONPATH it is prepended there (the Python half does the same
// for NODE_OPTIONS), so a step that loads either half records the python3
// generators and the node scripts its tests spawn, whichever came first.
//
// What is recorded, per process, into <dir>/node-<pid>-<thread>-<rand>.jsonl:
//   {"suite", "op", "path", "at", "proc"}
//   suite  the test the read belongs to: PATH_FILTER_READS_SUITE when a parent
//          process set it, else this process's own entry script. A test's
//          child (node, or python3 through the Python half's sitecustomize)
//          inherits it, so a generator a test spawns is charged to the test.
//          `node --test a.js b.js` is no suite itself ("<node --test>"): it
//          runs each file in a child that names itself.
//   op     the fs call (readFileSync, existsSync, readdirSync, ...).
//   path   repo-relative, forward slashes. Paths outside the repo (tmp dirs,
//          the node install) are never recorded.
//   at     the outermost stack frame in the suite's own file (the test's own
//          line, not a helper's), else the innermost repo frame that is not
//          this file. Empty when no repo frame is on the stack: an ES
//          module's static `import` is resolved and read from the loader's
//          own frames, so its records name the test and the file but no
//          line. (A dynamic `import()` resolves on the caller's stack, so its
//          realpathSync record carries the import's line.)
//   proc   this process's entry script, repo-relative ("" for none).
// One record per (op, path) per process: the gate needs the set, not a log.
//
// Opens: recorded when the flags can read an existing file's bytes ("r",
// "r+", "a+", O_RDONLY or O_RDWR without O_TRUNC); not when they only write,
// truncate or create a new file ("w", "w+", "a", "wx", O_TRUNC, O_EXCL).
//
// Not seen: fs calls this file does not wrap (readlink, watch), a function
// some module took off `fs` before this preload ran, reads by native code or
// by a process whose environment was replaced (spawn with a fresh `env`),
// and reads by shell tools (cat, cmp, git).
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

  // `node --test a.js b.js` runs each file in a child of its own, and its
  // own argv[1] is merely the first of them: it is no suite, and it leaves
  // the children to name themselves. Every other process hands its suite
  // down to the processes it starts.
  const isTestRunner = process.execArgv.includes("--test");
  const entry = !isTestRunner && process.argv[1] ? rel(path.resolve(process.argv[1])) || "" : "";
  const inherited = process.env.PATH_FILTER_READS_SUITE || "";
  const suite = inherited || entry || (isTestRunner ? "<node --test>" : "<node>");
  if (!inherited && entry) process.env.PATH_FILTER_READS_SUITE = suite;
  const suiteAbs = path.join(ROOT, suite);

  // Arm the Python half in every python3 this process starts (see the header).
  const pyPath = process.env.PYTHONPATH || "";
  if (!pyPath.split(path.delimiter).some((p) => p && path.resolve(p) === __dirname)) {
    process.env.PYTHONPATH = pyPath ? `${__dirname}${path.delimiter}${pyPath}` : __dirname;
  }

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

  // Can an open with these flags read the bytes already in the file? fs's
  // own default is "r", and `fs.open(path, callback)` puts the callback where
  // the flags go. Writing, truncating or creating a new file reads nothing.
  const C = fs.constants;
  const canRead = (flags) => {
    if (flags === undefined || flags === null || typeof flags === "function") return true;
    if (typeof flags === "number") {
      const access = flags & 3;
      if (access !== C.O_RDONLY && access !== C.O_RDWR) return false;
      if (flags & C.O_TRUNC) return false;
      return !((flags & C.O_CREAT) && (flags & C.O_EXCL));
    }
    return typeof flags === "string" && /[r+]/.test(flags) && !/[wx]/.test(flags);
  };

  const wrap = (obj, name, test, op = name) => {
    const fn = obj && obj[name];
    if (typeof fn !== "function") return;
    const wrapped = function (p) {
      if (!test || test(arguments)) note(op, p);
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

  const openFlags = (args) => canRead(args[1]);
  const streamFlags = (args) => {
    const o = args[1];
    return canRead(o && typeof o === "object" ? o.flags : undefined);
  };
  for (const name of ["readFileSync", "existsSync", "readdirSync", "statSync", "lstatSync",
    "accessSync", "opendirSync", "realpathSync", "readFile", "readdir", "stat", "lstat",
    "access", "opendir", "exists", "realpath"]) {
    wrap(fs, name);
  }
  // realpath throws on a missing path, so it is an existence check too; its
  // `.native` forms are properties of the (now wrapped) functions.
  wrap(fs.realpathSync, "native", null, "realpathSync.native");
  wrap(fs.realpath, "native", null, "realpath.native");
  wrap(fs, "openSync", openFlags);
  wrap(fs, "open", openFlags);
  wrap(fs, "createReadStream", streamFlags);
  wrap(fs, "copyFileSync");
  wrap(fs, "cpSync");
  const promises = fs.promises;
  for (const name of ["readFile", "readdir", "stat", "lstat", "access", "opendir", "realpath",
    "copyFile", "cp"]) {
    wrap(promises, name);
  }
  wrap(promises, "open", openFlags);

  // An ES module's `import { readFileSync } from "node:fs"` binds the
  // builtin's ESM facade, which is built on first import, so a test module
  // loaded after this preload already gets the wrapped calls. A facade an
  // earlier preload built still holds the originals until this resync. The
  // recorder tests pin both cases.
  require("node:module").syncBuiltinESMExports();

  if (entry) note("exec", path.resolve(process.argv[1]));
}
