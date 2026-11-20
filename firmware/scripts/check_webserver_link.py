#!/usr/bin/env python3
"""Did a core 2.0.17 build link the patched WebServer, or the core's own?

arduino-esp32 2.0.17's WebServer sizes a stack array from the client's
multipart boundary (GHSA-8cmm-3887-r32j), and no 2.x release fixes it. The
core-2 builds that serve HTTP link firmware/third_party/WebServer-2.0.17-securacv
instead (its README.md). Passing the copy to the build is not proof that the
build used it: a resolver change, or a renamed library.properties, would link
the core's copy and still build green. So a build is checked by what it says
it linked, and both build tools say it:

  pio LOG
      A PlatformIO `pio run` log. Its dependency graph names every library an
      env links; `|-- WebServer @ 2.0.0` is the core 2.0.17 copy (the patched
      copy is `2.0.17+securacv.1`). Exit 1 when the log names the core's copy,
      0 otherwise (an env that links no WebServer, or the patched one).

  cli RESULT.json [--expect DIR]
      The output of `arduino-cli compile --format json`. Echoes the compiler's
      own output to stdout/stderr (the log reads as a plain compile), then:
      exit 2 when the compile failed (no image to judge); with --expect, 1
      when the result lists no WebServer, or one from any directory but DIR;
      otherwise 0. Without --expect (a core-3 build, or a release tag cut
      before DIR existed) it only reports the compile.

Used by firmware.yml (every push), firmware-release.yml and flasher-release.yml
(on the release channel, from the tooling overlay, so a fix reaches older
tags). Tests: scripts/tests/test_vendored_webserver_core2.py.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

# PlatformIO prints the graph as `|-- <name> @ <version>` (deeper levels
# indent with `|   `). 2.0.0 is the version the core's library.properties
# carries; the patched copy's is 2.0.17+securacv.1.
CORE_COPY = re.compile(r"\|-- WebServer @ 2\.0\.0(?:[^.0-9+]|$)", re.MULTILINE)


def pio(log_path: str) -> int:
    with open(log_path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if CORE_COPY.search(text):
        print(f"{log_path}: links core 2.0.17's own WebServer (GHSA-8cmm-3887-r32j)",
              file=sys.stderr)
        return 1
    return 0


def _same_dir(a: str, b: str) -> bool:
    return os.path.realpath(a.rstrip("/")) == os.path.realpath(b.rstrip("/"))


def cli(result_path: str, expect: str | None) -> int:
    with open(result_path, encoding="utf-8", errors="replace") as f:
        raw = f.read()
    try:
        result = json.loads(raw)
    except json.JSONDecodeError:
        sys.stdout.write(raw)
        print(f"{result_path}: arduino-cli printed no JSON result (was --format json passed?)",
              file=sys.stderr)
        return 2
    sys.stdout.write(result.get("compiler_out") or "")
    sys.stderr.write(result.get("compiler_err") or "")
    if result.get("error"):
        print(result["error"], file=sys.stderr)
    # `success` is arduino-cli 1.x's own verdict; a result without the key
    # counts as failed only when it carries an error.
    if not result.get("success", not result.get("error")):
        return 2
    if expect is None:
        return 0
    libs = (result.get("builder_result") or {}).get("used_libraries") or []
    webservers = [lib for lib in libs if lib.get("name") == "WebServer"]
    for lib in libs:
        print(f"used library: {lib.get('name')} {lib.get('version', '')} {lib.get('install_dir', '')}")
    if len(webservers) != 1:
        print(f"{result_path}: {len(webservers)} WebServer libraries in builder_result.used_libraries; "
              f"expected exactly one, from {expect}", file=sys.stderr)
        return 1
    got = webservers[0].get("install_dir") or ""
    if not _same_dir(got, expect):
        print(f"{result_path}: linked WebServer from {got or '(no install_dir)'}, not {expect} "
              "(GHSA-8cmm-3887-r32j)", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("pio", help="check a pio run log")
    p.add_argument("log")
    c = sub.add_parser("cli", help="check an arduino-cli --format json result")
    c.add_argument("result")
    c.add_argument("--expect", help="the WebServer directory the build must link")
    args = ap.parse_args(argv)
    if args.mode == "pio":
        return pio(args.log)
    return cli(args.result, args.expect)


if __name__ == "__main__":
    sys.exit(main())
