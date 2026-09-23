#!/usr/bin/env python3
"""scad_probe.py — the one way the enclosure generators ask OpenSCAD a question.

The generators that read numbers OUT of the CAD rather than typing them:

  gen_assembled_dims.py    the assembled envelope of a multi-part device
                           (render a union, measure its bounding box, read
                           the seams the case echoes)
  gen_hardware.py          the hardware a committed preset needs (the case's
                           own `HARDWARE — …` echo, canary_core_lib hw_echo)
                           and its lid-rib headroom
  gen_enclosures.py        --check-previews: the coarse preview meshes the
                           Lab loads, re-rendered and compared by bounding box

They share this module so there is one answer to "was that render clean?"
and one STL reader. A probe that returns numbers from a dirty render is
worse than no probe — a WARNING from CGAL, a failed assert, or an export
that never happened all mean NOTHING was measured, so every entry point here
refuses (ProbeError) instead of returning.

Two ways in:

  probe(label, scad, overrides, body, …)   writes a throwaway file BESIDE the
      case files that `include`s the case, applies `overrides` AFTER the
      include (an include's own defaults win over anything set before it;
      the last assignment wins file-wide, exactly like -D), then `body`.
      OpenSCAD resolves `include` relative to the including file, which is
      why the probe cannot live in a temp directory.
  render(src, defines, …)                   renders a file as-is with -D
      defines — what render.sh and gen_enclosures.py --render do.

`export="echo"` asks for the echo stream only (no CGAL, milliseconds): the
file is still evaluated top to bottom, so every assert and echo still runs.
`export="binstl"` renders the geometry and returns its bounding box.

Bounding boxes are deterministic even though OpenSCAD's STL bytes are not,
so the callers compare numbers, never bytes.
"""

from __future__ import annotations

import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
OPENSCAD = os.environ.get("OPENSCAD", "openscad")
# The render gate enclosure.yml applies to every render log, verbatim in spirit:
# OpenSCAD exits 0 on geometry warnings, so the text is what decides.
DIRTY = re.compile(r"ERROR|WARNING")
_ECHO = re.compile(r'^ECHO: (.*)$', re.M)


class ProbeError(RuntimeError):
    """A render that measured nothing: dirty, missing, or without the echo asked for."""


class Result(NamedTuple):
    diag: str                        # stdout + stderr (+ the .echo file for echo exports)
    echoes: list[str]                # every `ECHO: …` payload, in order
    bbox: list[float] | None         # [x, y, z] mm, 3 dp — binstl exports only
    # the bbox's min corner [x, y, z] in the rendered file's own frame, mm,
    # 3 dp — binstl exports only. What turns a coordinate the case echoes
    # (a feature center) into a position on the measured envelope without
    # assuming the outline is centered on the origin
    lo: list[float] | None = None


def stl_bounds(path: Path) -> tuple[list[float], list[float]]:
    """(min corner, max corner) of a binary STL, [x, y, z] each, unrounded."""
    raw = Path(path).read_bytes()
    (n,) = struct.unpack_from("<I", raw, 80)
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    off = 84
    for _ in range(n):
        # 12 floats: normal + 3 vertices; then a u16 attribute
        vals = struct.unpack_from("<12f", raw, off)
        for v in range(3):
            for a in range(3):
                c = vals[3 + v * 3 + a]
                if c < lo[a]:
                    lo[a] = c
                if c > hi[a]:
                    hi[a] = c
        off += 50
    return lo, hi


def stl_bbox(path: Path) -> list[float]:
    """[x, y, z] extent of a binary STL, rounded to 0.001 mm."""
    lo, hi = stl_bounds(path)
    return [round(hi[a] - lo[a], 3) for a in range(3)]


def render(src: Path, defines: dict[str, str] | None = None, *, export: str = "binstl",
           label: str | None = None, keep: Path | None = None) -> Result:
    """Render `src` with -D `defines` (values already quoted the way -D wants
    them). `keep`, when given, is where the exported file is left for the
    caller (a binstl export only); otherwise it is discarded."""
    src = Path(src)
    label = label or src.name
    suffix = ".echo" if export == "echo" else ".stl"
    with tempfile.TemporaryDirectory() as td:
        out = keep if keep is not None else Path(td) / f"probe{suffix}"
        out.unlink(missing_ok=True)
        cmd = [OPENSCAD]
        if export != "echo":
            cmd += ["--export-format", export]
        cmd += ["-o", str(out)]
        for k, v in (defines or {}).items():
            cmd += ["-D", f"{k}={v}"]
        cmd.append(str(src))
        r = subprocess.run(cmd, cwd=src.parent, capture_output=True, text=True)
        diag = (r.stdout or "") + (r.stderr or "")
        if export == "echo" and out.exists():
            diag += out.read_text(encoding="utf-8", errors="replace")
        if DIRTY.search(diag) or not out.exists():
            raise ProbeError(f"{label}: dirty render, nothing measured\n{diag}")
        bbox = lo = None
        if export != "echo":
            lo_raw, hi_raw = stl_bounds(out)
            bbox = [round(hi_raw[a] - lo_raw[a], 3) for a in range(3)]
            lo = [round(v, 3) for v in lo_raw]
    return Result(diag=diag, echoes=_ECHO.findall(diag), bbox=bbox, lo=lo)


def probe(label: str, scad: str, overrides: dict[str, str], body: str = "", *,
          export: str = "binstl", root: Path = HERE) -> Result:
    """`include <scad>`, then `overrides` (Customizer assignments, applied
    after the include so they win), then `body` — rendered from a throwaway
    file beside the case files and deleted afterwards."""
    text = "include <{scad}>\n{ov}\n{body}\n".format(
        scad=scad,
        ov="\n".join(f"{k} = {v};" for k, v in overrides.items()),
        body=body,
    )
    safe = re.sub(r"[^A-Za-z0-9_]", "_", label)
    src = root / f".tmp_probe_{safe}.scad"
    src.write_text(text, encoding="utf-8")
    try:
        return render(src, export=export, label=label)
    finally:
        src.unlink(missing_ok=True)


def echo_value(result: Result, tag: str, label: str) -> str:
    """The payload after `"<tag>", ` in the FIRST `echo("<tag>", …)` line —
    refused when the render never said it."""
    prefix = f'"{tag}", '
    for e in result.echoes:
        if e.startswith(prefix):
            return e[len(prefix):]
    raise ProbeError(f"{label}: echo {tag!r} missing\n{result.diag}")


def echo_numbers(result: Result, tag: str, label: str) -> list[float]:
    """`echo("<tag>", [a, b, …])` or `echo("<tag>", a)` as floats, 3 dp."""
    raw = echo_value(result, tag, label).strip()
    body = raw[1:-1] if raw.startswith("[") and raw.endswith("]") else raw
    try:
        return [round(float(v), 3) for v in body.split(",") if v.strip()]
    except ValueError:
        raise ProbeError(f"{label}: echo {tag!r} is not numeric: {raw}") from None
