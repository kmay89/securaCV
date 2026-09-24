"""The Python half of the path-filter read recorder (sweep CI2).

Put this directory on PYTHONPATH and every python3 process of the CI step
imports it at startup (site.py imports `sitecustomize`). It installs a
`sys.addaudithook` hook that records each repo-relative path the process opens
for reading, and each directory it lists, so
scripts/check_path_filter_reads.py can hold them to the workflow's
`pull_request` paths filter — including the generators a Node test spawns
(`gen_csp.py --check`), which are the test's reads too.

Inert unless PATH_FILTER_READS_DIR is set (the directory is made on the first
record). The hook only ever records; it never raises into the audited call.
It arms the Node half for the processes this one starts: when NODE_OPTIONS
does not already `--require` this directory's record.cjs, it is added there
(the Node half does the same for PYTHONPATH), so a step that loads either
half records the other's children too.

Records, one JSON line per (op, path, suite), into
<dir>/py-<pid>-<rand>.jsonl, with the same fields as the Node half:
  suite  PATH_FILTER_READS_SUITE when a parent process set it (a Node test
         that spawned this generator); else the innermost test module
         (`test_*.py`) on the stack — `unittest discover` runs several suites
         in one process; else this process's entry script, or `-m <module>`.
  op     "open" (opens that can read the file's bytes, imports included:
         "r", "r+", "a+", O_RDONLY or O_RDWR without O_TRUNC, and not "w",
         "w+", "a", "x" or O_TRUNC, which write, truncate or create), "listdir"
         (os.listdir / os.scandir, which glob and pathlib's iterdir go through).
  path   repo-relative, forward slashes; nothing outside the repo is kept.
  at     the outermost frame in the suite's own file (the test method's
         line), else the innermost repo frame.
  proc   the entry script, repo-relative ("" for `-m` and `-c`).
Two kinds of event are not the program reading the path they carry, and are
skipped: directory listings made by the import system (its path-entry
caches), and the entries shutil.rmtree / os.fwalk open relative to a
directory fd (a TemporaryDirectory's cleanup reports bare names like "ssl",
which would otherwise resolve against the cwd).

Not seen: os.stat / os.path.exists / os.path.realpath / pathlib's exists()
and is_file() (CPython raises no audit event for them), so a python3 process
records what it opens and lists but not what it only checks for existence;
and processes started with -I / -E / -S or a replaced environment, which
never import this file.

If another sitecustomize sits further down sys.path (Debian ships one), it
still runs: this module executes it after installing the hook.
"""

from __future__ import annotations

import os
import sys

_DIR = os.environ.get("PATH_FILTER_READS_DIR")
_SUITE_ENV = "PATH_FILTER_READS_SUITE"


def _install(out_dir: str) -> None:
    import json
    import random

    # realpath: the cwd a relative path resolves against is a real path too
    here = os.path.dirname(os.path.realpath(__file__))
    root = os.path.dirname(os.path.dirname(here))

    # Arm the Node half in every node this process starts (see the docstring).
    node_rec = os.path.join(here, "record.cjs")
    node_opts = os.environ.get("NODE_OPTIONS", "")
    if os.path.isfile(node_rec) and node_rec not in node_opts:
        word = f'"{node_rec}"' if " " in node_rec else node_rec
        os.environ["NODE_OPTIONS"] = f"{node_opts} --require {word}".strip()
    root_sep = root + os.sep
    self_file = os.path.abspath(__file__)

    def rel(abs_path: str) -> str | None:
        if not abs_path.startswith(root_sep):
            return None
        return abs_path[len(root_sep):].replace(os.sep, "/")

    argv = list(getattr(sys, "orig_argv", None) or [])
    entry = ""
    label = ""
    # orig_argv: [interpreter, *options, script | -m mod | -c code, *args]
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "-m" and i + 1 < len(argv):
            label = "python3 -m " + " ".join(argv[i + 1:])
            break
        if a == "-c":
            label = "python3 -c"
            break
        if a.startswith("-"):
            # options that take a separate value
            if a in ("-W", "-X") and i + 1 < len(argv):
                i += 1
            i += 1
            continue
        entry = rel(os.path.abspath(a)) or ""
        label = entry or a
        break

    inherited = os.environ.get(_SUITE_ENV, "")
    # `busy` is per thread: the hook's own writes must not re-enter it, but a
    # second thread's open while this one records is a read like any other.
    from _thread import get_ident
    busy: set[int] = set()
    state = {"fd": None}
    seen: set[tuple[str, str, str]] = set()

    def frames():
        f = sys._getframe(1)
        while f is not None:
            yield f
            f = f.f_back

    def is_test_module(r: str) -> bool:
        base = r.rsplit("/", 1)[-1]
        return base.startswith("test_") and base.endswith(".py")

    def attribute() -> tuple[str, str, str]:
        """(suite, at, skip) for the event being audited. `skip` names why an
        event is not a read of the path it carries, or is ""."""
        suite_at = ""
        test_suite = ""
        first_repo = ""
        skip = ""
        for f in frames():
            fn = f.f_code.co_filename
            if fn.startswith("<frozen importlib"):
                if not first_repo:
                    skip = skip or "import-cache"
                continue
            base = os.path.basename(fn)
            name = f.f_code.co_name
            if (base == "shutil.py" and name.startswith("_rmtree")) or \
                    (base == "os.py" and name in ("fwalk", "_fwalk")):
                # rmtree / fwalk open entries relative to a directory fd; the
                # audit event carries the bare name, not a path from the cwd.
                skip = "dir-fd"
                continue
            if fn == self_file or not os.path.isabs(fn):
                continue
            r = rel(fn)
            if r is None:
                continue
            where = f"{r}:{f.f_lineno}"
            if not first_repo:
                first_repo = where
            if not test_suite and is_test_module(r):
                test_suite = r
            if r == test_suite:
                suite_at = where  # keeps walking out: the test method's line
        if inherited:
            return inherited, first_repo, skip
        if test_suite:
            return test_suite, suite_at, skip
        return label or "<python>", first_repo, skip

    def write(rec: dict) -> None:
        if state["fd"] is None:
            os.makedirs(out_dir, exist_ok=True)
            name = f"py-{os.getpid()}-{random.getrandbits(32):08x}.jsonl"
            state["fd"] = os.open(os.path.join(out_dir, name),
                                  os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        os.write(state["fd"], (json.dumps(rec) + "\n").encode())

    def note(op: str, raw) -> None:
        if isinstance(raw, int) or raw is None:
            return  # a file descriptor: its path was recorded when opened
        try:
            p = os.fsdecode(raw)
        except TypeError:
            return
        r = rel(os.path.abspath(p))
        if r is None:
            return
        suite, at, skip = attribute()
        if skip == "dir-fd" or (op == "listdir" and skip == "import-cache"):
            return
        key = (op, r, suite)
        if key in seen:
            return
        seen.add(key)
        write({"suite": suite, "op": op, "path": r, "at": at, "proc": entry})

    def hook(event: str, args: tuple) -> None:
        me = get_ident()
        if me in busy:
            return
        if event == "open":
            path, mode, flags = (tuple(args) + (None, None, None))[:3]
            if isinstance(flags, int):
                # io.open and os.open always pass the flags
                if flags & 3 not in (os.O_RDONLY, os.O_RDWR) or flags & os.O_TRUNC \
                        or (flags & os.O_CREAT and flags & os.O_EXCL):
                    return
            elif isinstance(mode, str):
                # a raw sys.audit("open", path, mode, None) from other code
                if not any(c in mode for c in "r+") or any(c in mode for c in "wx"):
                    return
            op = "open"
        elif event in ("os.listdir", "os.scandir"):
            path = args[0] if args else "."
            op = "listdir"
        elif event in ("subprocess.Popen", "os.posix_spawn", "os.exec", "os.spawn"):
            # A multi-suite runner (unittest discover) hands the CURRENT test
            # to the child it is about to start, through the inherited env.
            # A child given its own env (subprocess's 4th audit argument) gets
            # it there, when that env keeps the recorder armed at all.
            if not inherited:
                busy.add(me)
                try:
                    suite, _, _ = attribute()
                    os.environ[_SUITE_ENV] = suite
                    env = args[3] if event == "subprocess.Popen" and len(args) > 3 else None
                    if isinstance(env, dict) and "PATH_FILTER_READS_DIR" in env:
                        env[_SUITE_ENV] = suite
                except Exception:
                    pass
                finally:
                    busy.discard(me)
            return
        else:
            return
        busy.add(me)
        try:
            note(op, path)
        except Exception:
            pass  # never the reason a test fails; the checker sees the gap
        finally:
            busy.discard(me)

    sys.addaudithook(hook)
    if entry:
        busy.add(get_ident())
        try:
            write({"suite": inherited or entry, "op": "exec", "path": entry,
                   "at": "", "proc": entry})
            seen.add(("exec", entry, inherited or entry))
        except Exception:
            pass
        finally:
            busy.discard(get_ident())


def _chain() -> None:
    """Run the next sitecustomize on sys.path, the one this file shadows."""
    here = os.path.dirname(os.path.abspath(__file__))
    for entry in sys.path:
        base = os.path.abspath(entry or os.curdir)
        if base == here:
            continue
        cand = os.path.join(base, "sitecustomize.py")
        if os.path.isfile(cand):
            with open(cand, "rb") as fh:
                code = compile(fh.read(), cand, "exec")
            exec(code, {"__name__": "sitecustomize", "__file__": cand})
            return


if _DIR:
    try:
        _install(_DIR)
    except Exception as exc:  # pragma: no cover - a broken recorder is loud, not fatal
        print(f"path-filter read recorder: not installed ({exc})", file=sys.stderr)
_chain()
