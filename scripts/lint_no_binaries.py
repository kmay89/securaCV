#!/usr/bin/env python3
"""Fail the build on a compiled program, object or library committed to git.

    python3 scripts/lint_no_binaries.py          # every tracked file
    python3 scripts/lint_no_binaries.py PATH...  # these files only

THE PROBLEM THIS CATCHES
========================
The host-test Makefiles build into `build/` (gitignored), but nothing stops
a hand build beside the source: `g++ test_x.cpp -o test_x` writes an
extensionless executable next to `test_x.cpp`, and `git add -A` takes it.
That happened: #1776 committed `firmware/tests_host/test_doorbell_audio`, a
21 KB x86-64 ELF, beside the test it was built from. It runs on nobody's
machine but its builder's, it goes stale on the next edit to the test, and a
reader cannot tell it from a source file by its name. No gate looked at it.
`.gitignore` names the WAP's old hand-built tests one by one; that list
cannot cover the next one.

WHAT IT DOES
============
Reads the first bytes of every file git tracks (`git ls-files`, so ignored
and untracked files are not its business) and names each one whose magic
says it is native machine code:

  ELF       7f 45 4c 46               Linux/BSD executable, .o, .so
  Mach-O    fe ed fa ce / cf, and     macOS executable, .o, .dylib
            the byte-swapped forms
  fat       ca fe ba be + small arch  macOS universal binary (a Java class
            count                     file shares the magic; its next word
                                      is a version >= 45, so it is told apart)
  PE        "MZ" + "PE\\0\\0" at the    Windows .exe / .dll
            offset in the DOS header
  ar        "!<arch>\\n"                static library (.a, .lib)

WebAssembly (`\\0asm`) is deliberately NOT here: the emulator's committed
`dist/` is generated and byte-gated by its own workflow. Firmware images
for the ESP32 (0xE9 first byte) are not matched either; none is tracked,
and a release image is published, not committed.

A file it cannot read is refused (exit 2), never passed: a lint that skips
what it could not open reads as covered while catching nothing. Outside a
git checkout it refuses the same way.

FIXING IT
=========
`git rm --cached <path>` and build into the directory the Makefile uses. If
a binary truly has to be tracked (none does today), it goes on ALLOW below
with the reason, and the review of that line is the decision.
"""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# path -> why it is allowed. Empty on purpose; growing it is a decision.
ALLOW: dict[str, str] = {}

HEAD = 64


def kind(head: bytes, read_at=None) -> str | None:
    """Name the native-code format `head` starts with, or None.

    `read_at(offset, n)` reads more of the file for the PE check, whose
    signature sits at an offset the DOS header names. Without it a PE whose
    signature lies past `head` is judged from `head` alone.
    """
    m4 = head[:4]
    if m4 == b"\x7fELF":
        return "ELF"
    if m4 in (b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf",
              b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe"):
        return "Mach-O"
    if m4 == b"\xca\xfe\xba\xbe" and len(head) >= 8:
        # Universal binary: the next big-endian word is the arch count, a
        # handful. A Java class file puts minor/major version there; the
        # major (low half) is 45 or more, so the word is 45+ as well.
        (word,) = struct.unpack(">I", head[4:8])
        if 0 < word < 45:
            return "Mach-O universal"
        return None
    if head[:8] == b"!<arch>\n":
        return "ar archive"
    if head[:2] == b"MZ" and len(head) >= 0x40:
        (off,) = struct.unpack("<I", head[0x3C:0x40])
        sig = head[off:off + 4] if off + 4 <= len(head) else (
            read_at(off, 4) if read_at else b"")
        if sig == b"PE\x00\x00":
            return "PE"
    return None


# The guard on the guard: shapes that must keep their verdict, so an edit
# that loosens a magic check fails here instead of letting a binary through.
def _pe(sig_off: int = 0x80) -> bytes:
    b = bytearray(b"MZ" + b"\x00" * (sig_off + 8))
    b[0x3C:0x40] = struct.pack("<I", sig_off)
    b[sig_off:sig_off + 4] = b"PE\x00\x00"
    return bytes(b)


MUST_FLAG = {
    b"\x7fELF\x02\x01\x01" + b"\x00" * 57: "ELF",
    b"\xcf\xfa\xed\xfe\x07\x00\x00\x01" + b"\x00" * 56: "Mach-O",
    b"\xfe\xed\xfa\xce" + b"\x00" * 60: "Mach-O",
    b"\xca\xfe\xba\xbe\x00\x00\x00\x02" + b"\x00" * 56: "Mach-O universal",
    b"!<arch>\n/               0" + b" " * 32: "ar archive",
    _pe(): "PE",
}
MUST_PASS = [
    b"\xca\xfe\xba\xbe\x00\x00\x00\x34" + b"\x00" * 56,  # Java class, v52
    b"MZ is how this sentence starts, and it is text." + b" " * 30,
    _pe()[:0x80] + b"NE\x00\x00" + b"\x00" * 4,           # 16-bit NE, not PE
    b"\x00asm\x01\x00\x00\x00" + b"\x00" * 56,           # WebAssembly
    b"\xe9\x03\x02\x20" + b"\x00" * 60,                  # ESP32 image
    b"#!/usr/bin/env python3\n",
    b"\x89PNG\r\n\x1a\n" + b"\x00" * 56,
    b"",
]


def self_test() -> list[str]:
    wrong = []
    for blob, want in MUST_FLAG.items():
        got = kind(blob[:HEAD], lambda o, n, b=blob: b[o:o + n])
        if got != want:
            wrong.append(f"flag {want}: got {got!r} for {blob[:8]!r}")
    for blob in MUST_PASS:
        got = kind(blob[:HEAD], lambda o, n, b=blob: b[o:o + n])
        if got is not None:
            wrong.append(f"pass: got {got!r} for {blob[:16]!r}")
    return wrong


def tracked(root: Path) -> list[str]:
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                             capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"lint_no_binaries.py: cannot list tracked files ({e}); "
              "refusing rather than passing an unread tree", file=sys.stderr)
        raise SystemExit(2)
    return [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]


def problems(root: Path, paths: list[str]) -> tuple[list[str], list[str]]:
    found, unreadable = [], []
    for rel in paths:
        p = root / rel
        if p.is_symlink() or not p.is_file():
            # A symlink's target is checked where git tracks the target; a
            # tracked path missing from the checkout (sparse, deleted in the
            # work tree) has no bytes here to judge.
            continue
        try:
            with open(p, "rb") as fh:
                head = fh.read(HEAD)

                def read_at(off: int, n: int, fh=fh) -> bytes:
                    fh.seek(off)
                    return fh.read(n)

                k = kind(head, read_at)
        except OSError as e:
            unreadable.append(f"{rel}: {e}")
            continue
        if k and rel not in ALLOW:
            found.append(f"{rel}: {k}")
    return found, unreadable


def main(argv: list[str]) -> int:
    wrong = self_test()
    if wrong:
        print("lint_no_binaries.py: the magic checks no longer behave — "
              "these self-test shapes get the wrong verdict:", file=sys.stderr)
        for w in wrong:
            print(f"  {w}", file=sys.stderr)
        return 2
    if argv:
        paths = []
        for a in argv:
            p = Path(a).resolve()
            if not p.exists():
                print(f"lint_no_binaries.py: no such path: {a}", file=sys.stderr)
                return 2
            paths.append(p.relative_to(ROOT).as_posix()
                         if p.is_relative_to(ROOT) else str(p))
    else:
        paths = tracked(ROOT)
    found, unreadable = problems(ROOT, paths)
    if unreadable:
        print("lint_no_binaries.py: could not read (refused, not passed):",
              file=sys.stderr)
        for u in unreadable:
            print(f"  {u}", file=sys.stderr)
        return 2
    if found:
        print(f"lint_no_binaries.py: {len(found)} compiled file(s) committed — "
              "build into the Makefile's build/ directory and "
              "`git rm --cached` these:", file=sys.stderr)
        for f in found:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"no compiled binaries — {len(paths)} "
          f"{'named' if argv else 'tracked'} file(s) read")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
