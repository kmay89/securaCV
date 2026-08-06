#!/usr/bin/env python3
"""Regenerate canary_s3_lcd7_stamp.scad — the build stamp the 7" case
debosses into the INNER face of its back plate.

    python3 gen_stamp.py            # regenerate the stamp
    python3 gen_stamp.py --check    # CI: fail if the committed stamp is stale

WHY THIS EXISTS — the anti-rot argument
---------------------------------------
A revision and a date hand-typed into a .scad rot silently: someone moves
a wall, ships it, and the case still says last month. You cannot fix that
with discipline; you fix it by making the stale state impossible to
commit. So the stamp has two halves that check each other:

  REV — human, CalVer, ONE string (below). Written by a person because a
        person is the only thing that knows a change is worth announcing.
        `2026.08a` is a date you can read off a shelf and a letter that
        revs within the month. It cannot silently disagree with reality,
        because of:
  SRC — machine, a digest of the geometry sources. Nobody types it. If
        the sources change and REV does not, this script REFUSES to
        regenerate, and CI refuses the commit. Rot is not discouraged —
        it is unrepresentable.

The digest ignores comments and whitespace on purpose: rewording a
comment is not a new revision, and if it demanded one, people would stop
bumping honestly — the gate has to fire only when it means something.

WHAT SRC IS AND IS NOT
----------------------
SRC identifies a DESIGN, not a manufacturer. Two cases printed from the
same committed sources carry the same eight characters, so you can ask a
part which design it came from and check the answer against this repo —
that is provenance, and it is genuinely useful when a fleet has parts
from three print runs and one of them has a fit problem.

It is NOT an authenticity check and must never be described as one:
anyone can deboss any string into any part. It proves nothing about who
printed it and stops no one from copying it. (Same doctrine as the rest
of this project — see AGENTS.md, "DON'T: Oversell security properties".)

Crockford base32: no I, L, O or U, so nothing is ambiguous when read off
a printed part in bad light.
"""
import hashlib
import re
import sys
from pathlib import Path

# ── The one human string. Bump it when you change the geometry. ──────────
# CalVer: YYYY.MM + a letter that revs within the month.
# (.09y through .10d are all spent — six revisions of this plate landed while
#  this branch was open. .10c moved the palette; .10d recomposed the plate; this
#  one takes the holes back out of it (no vent eggs, no radio window) and locks
#  the lockup to the mark. Each is a different DESIGN, so each takes its own
#  letter rather than reusing one already debossed into a part that is not this
#  one — that is the whole of what REV is for.)
#  .10f is the honest odd one out, and it is worth saying why rather than
#  leaving the next reader to wonder: this plate's geometry did NOT move. The
#  mark library grew measured TYPE METRICS (mark_word_min_h/mark_word_ink_w) —
#  pure arithmetic, consumed only by asserts on another case, drawing nothing.
#  But mark_lib is hashed here because a MARK change is a geometry change, and
#  the digest cannot tell an added function from a moved wall. That coupling is
#  deliberate and worth its false positives: the alternative is a digest with
#  holes in it. So SRC moves because the sources really did move, REV follows
#  because the two are not allowed to disagree, and the 7" plate printed under
#  .10f is dimensionally identical to the one printed under .10e.
STAMP_REV = "2026.10f"

HERE = Path(__file__).resolve().parent
OUT = HERE / "canary_s3_lcd7_stamp.scad"
# Everything whose bytes define this case's geometry. The stamp file is
# deliberately absent: hashing it would chase its own tail.
SOURCES = [
    "canary_s3_lcd7.scad",
    "canary_panel_lib.scad",   # the panel registry OWNS the panel/board numbers
                               # now, so a record edit is a geometry change and
                               # has to move SRC. Leaving it out would have been
                               # a hole straight through the anti-rot argument:
                               # you could change what board the case is cut for
                               # and the stamp would still claim the old design.
    "canary_vent_lib.scad",
    "canary_mark_lib.scad",   # THE BIRD — a mark change IS a geometry change
    "canary_s3_lcd7_qr.scad",
]

CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def strip_comments(src: str) -> str:
    """Drop // comments without touching // inside a string literal."""
    out, in_str, i = [], False, 0
    while i < len(src):
        ch = src[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < len(src):      # escaped char
                out.append(src[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            out.append(ch)
        elif ch == "/" and src[i : i + 2] == "//":
            while i < len(src) and src[i] != "\n":   # to end of line
                i += 1
            continue
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def geom_digest() -> str:
    """Eight Crockford-base32 chars over the comment-free, whitespace-
    normalized sources — so a reworded comment is not a new revision but a
    moved wall is."""
    h = hashlib.sha256()
    for name in SOURCES:
        text = (HERE / name).read_text(encoding="utf-8")
        norm = re.sub(r"\s+", " ", strip_comments(text)).strip()
        h.update(name.encode())          # the filename is part of the identity
        h.update(b"\0")
        h.update(norm.encode())
        h.update(b"\0")
    n = int.from_bytes(h.digest()[:5], "big")        # 40 bits -> 8 chars
    return "".join(CROCKFORD[(n >> (5 * i)) & 31] for i in range(7, -1, -1))


def render(rev: str, fp: str) -> str:
    return f"""\
// GENERATED by gen_stamp.py — do not hand-edit. Regenerate with:
//     python3 gen_stamp.py
// REV is human (CalVer, bumped deliberately); SRC is a digest of the
// geometry sources, so the two cannot silently disagree — gen_stamp.py
// refuses to regenerate if the sources moved and REV did not, and CI
// refuses the commit. See that script's header for what SRC does and
// does not prove (provenance, NOT authenticity).
// Functions, not variables, so nothing here surfaces in the Customizer.
function lcd7_stamp_rev() = "{rev}";
function lcd7_stamp_src() = "{fp}";
"""


def committed():
    """(rev, fp) recorded in the committed stamp, or (None, None)."""
    if not OUT.exists():
        return None, None
    text = OUT.read_text(encoding="utf-8")
    rev = re.search(r'lcd7_stamp_rev\(\) = "([^"]*)"', text)
    fp = re.search(r'lcd7_stamp_src\(\) = "([^"]*)"', text)
    return (rev.group(1) if rev else None, fp.group(1) if fp else None)


def main() -> int:
    check = "--check" in sys.argv
    fp = geom_digest()
    old_rev, old_fp = committed()
    want = render(STAMP_REV, fp)

    # The gate: geometry moved, revision did not.
    if old_fp is not None and old_fp != fp and old_rev == STAMP_REV:
        print(
            f"gen_stamp.py: the geometry sources changed ({old_fp} -> {fp}) but "
            f"STAMP_REV is still {STAMP_REV}.\n"
            "  A change worth printing is a change worth naming: bump STAMP_REV "
            "at the top of gen_stamp.py\n"
            "  (CalVer — same month, next letter: e.g. "
            f"{STAMP_REV[:-1]}{chr(ord(STAMP_REV[-1]) + 1)}) and run it again.",
            file=sys.stderr,
        )
        return 1

    if check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != want:
            print(
                "gen_stamp.py: the committed stamp is stale — run "
                "`python3 gen_stamp.py` and commit the result.",
                file=sys.stderr,
            )
            return 1
        print(f"stamp OK: REV {STAMP_REV}, SRC {fp}")
        return 0

    OUT.write_text(want, encoding="utf-8")
    print(f"OK {OUT.name}: REV {STAMP_REV}, SRC {fp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
