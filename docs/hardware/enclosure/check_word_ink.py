#!/usr/bin/env python3
"""Measure the 7-inch frame's brand words and hold the keep-outs the case
asserts against their real ink, not against the estimate.

    python3 check_word_ink.py                     # report
    python3 check_word_ink.py --check             # the gate enclosure.yml runs
    python3 check_word_ink.py --check -D edge_text_size=6.2    # what-if

WHY THIS EXISTS
---------------
canary_s3_lcd7.scad debosses two brand words, edge_text_l and edge_text_r,
into the frame's bottom wall either side of the USB port. Every keep-out
around them (the port flange, the leash anchor, the peel scoop, the port
label, the landscape dock keys, the wall's flat span) is asserted against an
ESTIMATE of their width, because OpenSCAD 2021.01 has no textmetrics:

    vword_half = 0.392 * edge_text_size * max(len(edge_text_l), len(edge_text_r))

The estimate reads short. At the shipped size 7, SECURACV inks 58.72 mm wide
against 43.90 estimated, so every assert passes while the peel scoop's
keep-out reaches 15.44 mm out from the port and the S starts at 13.74:
1.70 mm short (C33 in docs/audit/repo_todo_sweep_2026-09.md).

WHAT IT DOES
------------
1. Reads the case's own values through a scad_probe echo, with any -D
   overrides applied after the include (exactly like openscad -D), so a
   changed default is followed and the case's own asserts still run. The
   strings come back as character codes, never re-quoted.
2. Refuses unless fc-match resolves label_font to the family and style it
   names. OpenSCAD takes fontconfig's best match without a word, so a
   machine without Liberation Sans Bold would cut, and this would measure, a
   fallback font.
3. Renders each word to DXF with the case's size and font and frame()'s
   spacing and alignment, and reads the ink's x extent.
4. Places each word as frame() does: edge_text_l at +vword_x, edge_text_r at
   -vword_x, both turned 180 degrees about the wall normal.
5. Holds the five word asserts with the measured ink standing in for
   vword_x +/- vword_half (ASSERTS below). A keep-out is an interval on the
   wall, a word's ink is an interval, and they must not meet; each keep-out
   is tested against both words, so its side follows the sign of its dx.
6. Reports, and never gates, two facts about the face: the clear space each
   word leaves of vent_gap, and whether its ink fits the band vword_x centers
   it in. Gating either would pick the fix.

It decides nothing about the face. The fix is a maintainer's pick: (a) a
smaller edge_text_size, (b) a per-word offset for the scoop-side word, or
(c) port_scoop_dx inboard. KNOWN holds the one failure today, so the gate is
green until that pick lands and red the moment anything else moves.

KNOWN FAILURES
--------------
In the style of gen_hardware.py's KNOWN_DRIFT: --check fails on a failure
that is not in KNOWN (it is new) and on a KNOWN entry that no longer happens
(it was fixed: take it out). So (a) or (c) flips the gate with no edit here,
and the change that makes the pick removes the entry.

WHAT IT PINS, AND WHY IT REFUSES
--------------------------------
The check is only as good as its copy of the case. It refuses (exit 2),
naming what to update, when:

  * frame()'s brand-word cut is not the one place_word() models
    (PLACEMENT). Under (b), the change that adds the per-word offset must
    point place_word() at it, or this would go on measuring the old place.
  * one of the five asserts it transcribes changed (ASSERTS).
  * vword_x is no longer the midpoint of the band this reports.
  * fc-match resolves label_font to anything but the family and style it
    names (or fc-match is not there to ask).
  * the probe or a word render is not clean (scad_probe.DIRTY), or a word
    rendered nothing, or the DXF holds an entity whose extent its points do
    not bound, or the probe echoes a value that is missing, of the wrong
    kind, or not finite.
  * OPENSCAD_FONT_PATH is set: OpenSCAD searches it and fc-match does not,
    so the font check would vouch for a lookup it never saw.

Exit status: 0 clean (the known failures still happening, nothing new),
1 the gate failed, 2 refused (nothing was measured).
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import scad_probe

HERE = Path(__file__).resolve().parent
CASE = "canary_s3_lcd7.scad"
GH_FILE = f"docs/hardware/enclosure/{CASE}"

# The failures that are KNOWN and await a human. Key: "<keep-out>|<word knob>".
KNOWN: dict[str, str] = {
    "scoop|edge_text_l": (
        "C33 (docs/audit/repo_todo_sweep_2026-09.md): the peel scoop's keep-out runs "
        "into the scoop-side brand word. Awaits the maintainer's pick: (a) a smaller "
        "edge_text_size, (b) a per-word offset, or (c) port_scoop_dx inboard. The "
        "change that makes the pick takes this entry out."
    ),
}

# frame()'s brand-word cut, comment-stripped and compared with all whitespace
# removed. place_word() is this loop's transform, and measure_word() renders
# its text() call with frame()'s spacing and alignment.
PLACEMENT = """
for (s = [1, -1])
    translate([0, -fr_yo/2 + edge_lbl_depth, usb_z])
        rotate([90, 0, 0]) rotate([0, 0, 180]) translate([-s*vword_x, 0])
            linear_extrude(edge_lbl_depth + 0.1)
                text(s > 0 ? edge_text_l : edge_text_r,
                     size = edge_text_size, font = label_font,
                     spacing = 1.12, halign = "center",
                     valign = "center");
"""
SPACING = 1.12

# The five asserts the case holds against the estimate, verbatim (whitespace
# aside). evaluate() transcribes each with the measured ink in place of
# vword_x +/- vword_half; restate the transcription when one of these moves.
ASSERTS: dict[str, str] = {
    "flat_span": """
assert(vword_x - vword_half > usb_open_w/2 + grom_lip + 2
       && vword_x + vword_half + 2 < fr_xi/2 - fr_ri,
       "frame: brand deboss words don't fit between the USB port and the wall's flat span");
""",
    "anchor": """
assert(!port_tether || !usb_port
       || abs(usb_dx + port_teth_dx) + sd_teth_hole/2 + 1 < vword_x - vword_half,
       "frame: the port leash anchor lands on a brand word — move port_teth_dx inboard");
""",
    "scoop": """
assert(!port_scoop || !usb_port
       || abs(usb_dx + port_scoop_dx) + port_scoop_ftp + 1 < vword_x - vword_half,
       "frame: the peel scoop runs into a brand word — move port_scoop_dx inboard");
""",
    "port_label": """
assert(!port_labels || !usb_port
       || vword_x + vword_half + 6.0 + 2.2*len(port_lbl_a) < fr_xi/2 - fr_ri,
       "frame: the bottom port label runs off the wall's flat span — shorten port_lbl_a");
""",
    "dock_key": """
assert(!dock_keys || (dock_key_bx - gill_w/2 - 2 > vword_x + vword_half
    && dock_key_bx + gill_w/2 + 2 < fr_xi/2 - fr_ri),
       "frame: landscape keying slot lands on the brand words or off the wall's flat span");
""",
}

# What the probe echoes: name -> the case expression it reads.
NUMBERS: dict[str, str] = {
    "vword_x": "vword_x",
    "vword_half": "vword_half",
    "edge_text_size": "edge_text_size",
    "usb_dx": "usb_dx",
    "usb_open_w": "usb_open_w",
    "grom_lip": "grom_lip",
    "vent_gap": "vent_gap",
    "port_teth_dx": "port_teth_dx",
    "sd_teth_hole": "sd_teth_hole",
    "port_scoop_dx": "port_scoop_dx",
    "port_scoop_ftp": "port_scoop_ftp",
    "dock_key_bx": "dock_key_bx",
    "gill_w": "gill_w",
    "flat": "fr_xi/2 - fr_ri",
    # the band vword_x centers each word in (its own formula's two ends)
    "band_lo": "usb_open_w/2 + grom_lip + vent_gap",
    "band_hi": "(stand_w - 2*stand_cheek_t)/2 - 2",
}
FLAGS = ("usb_port", "port_tether", "port_scoop", "port_labels", "dock_keys")
STRINGS = ("edge_text_l", "edge_text_r", "label_font", "port_lbl_a")
WORDS = (("edge_text_l", 1), ("edge_text_r", -1))  # frame()'s s for each knob
TAG = "word_ink:"


class Refusal(RuntimeError):
    """Nothing was measured, or what was measured cannot be trusted."""


@dataclass(frozen=True)
class Ink:
    """One placed word: its knob, its text, and its ink on the wall (world x, mm)."""
    knob: str
    text: str
    lo: float
    hi: float

    @property
    def width(self) -> float:
        return self.hi - self.lo

    @property
    def inner(self) -> float:
        """Distance from x = 0 to the ink's inboard edge (0 if it straddles)."""
        return 0.0 if self.lo <= 0.0 <= self.hi else min(abs(self.lo), abs(self.hi))

    @property
    def outer(self) -> float:
        """Distance from x = 0 to the ink's outboard edge."""
        return max(abs(self.lo), abs(self.hi))


@dataclass(frozen=True)
class Failure:
    key: str        # "<keep-out>|<word knob>", or "port_label"
    assert_name: str
    message: str


# ---------------------------------------------------------------- source pins

def strip_comments(src: str) -> str:
    """OpenSCAD source with // and /* */ comments blanked (newlines kept),
    string literals left whole, so a // inside a string is not a comment."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            end = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, end))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _squash(text: str) -> tuple[str, list[int]]:
    """Text with every whitespace character removed, and the 1-based source
    line each kept character came from."""
    chars: list[str] = []
    lines: list[int] = []
    line = 1
    for c in text:
        if c == "\n":
            line += 1
        if not c.isspace():
            chars.append(c)
            lines.append(line)
    return "".join(chars), lines


def find_pinned(src: str, pinned: str) -> list[int]:
    """The source lines where `pinned` occurs in `src`, comments and
    whitespace aside — one entry per occurrence."""
    code, lines = _squash(strip_comments(src))
    want, _ = _squash(pinned)
    hits: list[int] = []
    start = 0
    while True:
        k = code.find(want, start)
        if k < 0:
            return hits
        hits.append(lines[k])
        start = k + 1


def check_pins(src: str) -> dict[str, int]:
    """Line of the placement loop and each transcribed assert; Refusal when
    one is missing or appears twice."""
    found: dict[str, int] = {}
    for name, pinned in [("placement", PLACEMENT), *ASSERTS.items()]:
        hits = find_pinned(src, pinned)
        if len(hits) != 1:
            what = ("frame()'s brand-word cut" if name == "placement"
                    else f"the case's {name!r} assert")
            fix = ("place_word() and measure_word() model that loop: point them at "
                   "the new placement, then update PLACEMENT" if name == "placement"
                   else "restate its transcription in evaluate(), then update ASSERTS")
            raise Refusal(f"{what} is {'not in' if not hits else 'repeated in'} {CASE} "
                          f"as this check models it ({len(hits)} matches) — {fix}")
        found[name] = hits[0]
    return found


# ---------------------------------------------------------------- the probe

def probe_body() -> str:
    """The echo statements appended after the include."""
    lines = [f'echo("{TAG}{k}", {expr});' for k, expr in NUMBERS.items()]
    lines += [f'echo("{TAG}{k}", {k});' for k in FLAGS]
    lines += [f'echo("{TAG}{k}", [for (c = {k}) ord(c)]);' for k in STRINGS]
    return "\n".join(lines) + "\n"


def parse_probe(echoes: list[str]) -> dict:
    """Probe echo payloads -> {name: float | bool | str}; Refusal on a value
    missing or not of its kind."""
    raw: dict[str, str] = {}
    for e in echoes:
        m = re.match(r'^"' + re.escape(TAG) + r'(\w+)", (.*)$', e)
        if m and m.group(1) not in raw:
            raw[m.group(1)] = m.group(2).strip()
    case: dict = {}
    for k in NUMBERS:
        try:
            case[k] = float(raw[k])
        except (KeyError, ValueError):
            raise Refusal(f"probe: {k} is missing or not a number ({raw.get(k)!r})") from None
        if not math.isfinite(case[k]):
            raise Refusal(f"probe: {k} is {raw[k]}, which nothing here can measure or spell")
    for k in FLAGS:
        if raw.get(k) not in ("true", "false"):
            raise Refusal(f"probe: {k} is missing or not a bool ({raw.get(k)!r})")
        case[k] = raw[k] == "true"
    for k in STRINGS:
        v = raw.get(k, "")
        if not (v.startswith("[") and v.endswith("]")):
            raise Refusal(f"probe: {k} is missing or not a string ({raw.get(k)!r})")
        body = v[1:-1].strip()
        try:
            case[k] = "".join(chr(int(float(t))) for t in body.split(",")) if body else ""
        except ValueError:
            raise Refusal(f"probe: {k} is not a list of character codes ({v!r})") from None
    if abs((case["band_lo"] + case["band_hi"]) / 2 - case["vword_x"]) > 1e-3:
        raise Refusal(
            f"probe: vword_x {case['vword_x']} is no longer the midpoint of "
            f"{case['band_lo']}..{case['band_hi']} — update NUMBERS' band_lo/band_hi "
            "to vword_x's formula")
    return case


def read_case(overrides: dict[str, str]) -> dict:
    try:
        r = scad_probe.probe("word_ink", CASE, overrides, probe_body(), export="echo")
    except scad_probe.ProbeError as err:
        raise Refusal(str(err)) from None
    return parse_probe(r.echoes)


# ---------------------------------------------------------------- the font

def parse_font_pattern(pattern: str) -> tuple[str, str | None]:
    """'Family Name:style=Bold' -> ('Family Name', 'Bold'). Refusal on what
    this cannot read as one family and at most one style."""
    family, _, props = pattern.partition(":")
    family = family.strip()
    style = None
    for prop in filter(None, (p.strip() for p in props.split(":"))):
        key, eq, val = prop.partition("=")
        if not eq:
            raise Refusal(f"label_font {pattern!r}: cannot read the property {prop!r}")
        if key.strip().lower() == "style":
            style = val.strip()
    if not family or "," in family or "\\" in pattern or (style and "," in style):
        raise Refusal(f"label_font {pattern!r} is not one family and one style "
                      "this check can hold fc-match to")
    return family, style


def font_resolves(pattern: str, fc_output: str) -> str | None:
    """None when fc-match's answer ('families|styles|file', comma lists) is the
    family and style `pattern` names; otherwise why not."""
    family, style = parse_font_pattern(pattern)
    parts = fc_output.strip().split("|")
    if len(parts) != 3:
        return f"fc-match answered {fc_output.strip()!r}"
    fams = [f.strip().lower() for f in parts[0].split(",")]
    styles = [s.strip().lower() for s in parts[1].split(",")]
    if family.lower() not in fams or (style and style.lower() not in styles):
        return (f"fontconfig resolves {pattern!r} to {parts[0]} {parts[1]} "
                f"({parts[2]}), a fallback")
    return None


def check_font(pattern: str) -> str:
    """The file fontconfig resolves label_font to; Refusal when it is a fallback."""
    if os.environ.get("OPENSCAD_FONT_PATH"):
        raise Refusal("OPENSCAD_FONT_PATH is set: OpenSCAD searches it and fc-match "
                      "does not, so the font check cannot vouch for what OpenSCAD cuts — unset it")
    try:
        r = subprocess.run(["fc-match", "-f", "%{family}|%{style}|%{file}", pattern],
                           capture_output=True, text=True)
    except OSError as err:
        raise Refusal(f"fc-match is not runnable ({err.strerror}) — install fontconfig") from None
    if r.returncode != 0:
        raise Refusal(f"fc-match {pattern!r} failed: {r.stderr.strip()}")
    why = font_resolves(pattern, r.stdout)
    if why:
        raise Refusal(f"{why}. Install the font (fonts-liberation on Ubuntu); "
                      "a fallback's ink is not the case's")
    return r.stdout.strip().split("|")[2]


# ---------------------------------------------------------------- measuring

_ALLOWED_ENTITIES = {"LINE", "LWPOLYLINE", "POLYLINE", "VERTEX", "SEQEND"}


def dxf_x_extent(text: str) -> tuple[float, float]:
    """(min x, max x) over every point of every entity in a DXF's ENTITIES
    section. Refusal on an entity whose extent its points do not bound (an
    arc, a spline), a malformed file, or no points at all."""
    rows = text.splitlines()
    if len(rows) % 2:
        raise Refusal("DXF: odd number of lines, not group-code pairs")
    xs: list[float] = []
    section = None
    pairs = [(rows[i].strip(), rows[i + 1].strip()) for i in range(0, len(rows), 2)]
    for i, (code, val) in enumerate(pairs):
        if code == "0" and val == "SECTION" and i + 1 < len(pairs):
            section = pairs[i + 1][1]
        elif code == "0" and val == "ENDSEC":
            section = None
        elif section == "ENTITIES":
            if code == "0" and val not in _ALLOWED_ENTITIES:
                raise Refusal(f"DXF: a {val} entity, whose extent its points do not bound")
            if code in ("10", "11"):
                try:
                    xs.append(float(val))
                except ValueError:
                    raise Refusal(f"DXF: x coordinate {val!r} is not a number") from None
    if not xs:
        raise Refusal("DXF: no ink")
    return min(xs), max(xs)


def scad_str(s: str) -> str:
    """An OpenSCAD expression for `s` that needs no quoting: chr([codes])."""
    return "chr([" + ", ".join(str(ord(c)) for c in s) + "])"


def measure_word(word: str, size: float, font: str) -> tuple[float, float]:
    """The ink's x extent of `word` as frame() cuts it, centered on x = 0."""
    src = (f"text({scad_str(word)}, size = {size!r}, font = {scad_str(font)}, "
           f'spacing = {SPACING!r}, halign = "center", valign = "center");\n')
    with tempfile.TemporaryDirectory() as td:
        scad, dxf = Path(td) / "word.scad", Path(td) / "word.dxf"
        scad.write_text(src, encoding="utf-8")
        try:
            r = subprocess.run([scad_probe.OPENSCAD, "-o", str(dxf), str(scad)],
                               capture_output=True, text=True)
        except OSError as err:
            raise Refusal(f"OpenSCAD is not runnable ({scad_probe.OPENSCAD!r}: "
                          f"{err.strerror})") from None
        diag = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or scad_probe.DIRTY.search(diag) or not dxf.exists():
            raise Refusal(f"{word!r}: dirty render, nothing measured\n{diag}")
        return dxf_x_extent(dxf.read_text(encoding="utf-8", errors="replace"))


def place_word(knob: str, word: str, ink_lo: float, ink_hi: float, s: int,
               vword_x: float) -> Ink:
    """frame()'s transform on the ink's x extent: translate(-s*vword_x), then
    rotate 180 degrees (x -> -x); rotate([90, 0, 0]) and the outer translate
    leave x alone. So edge_text_l (s = 1) inks at vword_x - hi .. vword_x - lo."""
    return Ink(knob, word, s * vword_x - ink_hi, s * vword_x - ink_lo)


# ---------------------------------------------------------------- the asserts

def _keepout(key: str, name: str, what: str, center: float, reach: float,
             inks: list[Ink]) -> list[Failure]:
    """A keep-out spans center +/- reach on the wall; no word's ink may meet it.
    Strict, like the case's asserts."""
    out = []
    for w in inks:
        if center + reach < w.lo or center - reach > w.hi:
            continue
        short = min(center + reach - w.lo, w.hi - (center - reach))
        out.append(Failure(f"{key}|{w.knob}", name,
                           f"{what} (x {center - reach:.2f}..{center + reach:.2f}) meets "
                           f"{w.text} ({w.knob}, ink {w.lo:.2f}..{w.hi:.2f}) by {short:.2f} mm"))
    return out


def evaluate(case: dict, inks: list[Ink]) -> list[Failure]:
    """The five asserts, with the measured ink in place of the estimate."""
    fails: list[Failure] = []
    flange = case["usb_open_w"] / 2 + case["grom_lip"]
    # :flat_span, first half. The case reads the flange at x = 0; the port is
    # cut at usb_dx, so it is held there (the same at the shipped usb_dx = 0).
    fails += _keepout("flange", "flat_span", "the port flange + 2",
                      case["usb_dx"], flange + 2, inks)
    # :flat_span, second half: the ink stays 2 inside the wall's flat span.
    for w in inks:
        if not w.outer + 2 < case["flat"]:
            fails.append(Failure(f"flat_span|{w.knob}", "flat_span",
                                 f"{w.text} ({w.knob}) inks to {w.outer:.2f}, past the flat "
                                 f"span's {case['flat']:.2f} - 2"))
    if case["port_tether"] and case["usb_port"]:
        fails += _keepout("anchor", "anchor", "the port leash anchor + 1",
                          case["usb_dx"] + case["port_teth_dx"],
                          case["sd_teth_hole"] / 2 + 1, inks)
    if case["port_scoop"] and case["usb_port"]:
        fails += _keepout("scoop", "scoop", "the peel scoop + 1",
                          case["usb_dx"] + case["port_scoop_dx"],
                          case["port_scoop_ftp"] + 1, inks)
    # :port_label — frame() sets the label on +x, beyond the word it puts there
    plus = [w for w in inks if dict(WORDS)[w.knob] > 0]
    if case["port_labels"] and case["usb_port"] and plus:
        end = plus[0].outer + 6.0 + 2.2 * len(case["port_lbl_a"])
        if not end < case["flat"]:
            fails.append(Failure("port_label", "port_label",
                                 f"the bottom port label, 6.0 beyond the +x ink, ends at "
                                 f"{end:.2f}, past the flat span's {case['flat']:.2f}"))
    if case["dock_keys"]:
        for sx in (1, -1):
            fails += _keepout("dock_key", "dock_key",
                              f"the {'+' if sx > 0 else '-'}x landscape key slot + 2",
                              sx * case["dock_key_bx"], case["gill_w"] / 2 + 2, inks)
    return fails


def verdict(fails: list[Failure], known: dict[str, str]) -> tuple[list[Failure], list[str]]:
    """(failures not in `known`, `known` keys that no longer happen)."""
    seen = {f.key for f in fails}
    return [f for f in fails if f.key not in known], sorted(set(known) - seen)


def facts(case: dict, inks: list[Ink]) -> list[str]:
    """What the report says and the gate never reads: each word's width
    against the estimate, its clear space against vent_gap, and its fit in
    the band vword_x centers it in."""
    out = []
    flange = case["usb_open_w"] / 2 + case["grom_lip"]
    est = 2 * case["vword_half"]
    for w in inks:
        side = 1 if (w.lo + w.hi) > 0 else -1
        clear = w.inner - (flange + side * case["usb_dx"])
        band_lo, band_hi = case["band_lo"], case["band_hi"]
        over_in = band_lo - w.inner
        over_out = w.outer - band_hi
        if over_in <= 0 and over_out <= 0:
            fit = "fits it"
        else:
            parts = []
            if over_in > 0:
                parts.append(f"{over_in:.2f} inside its inner edge")
            if over_out > 0:
                parts.append(f"{over_out:.2f} past its outer edge")
            fit = "runs " + " and ".join(parts)
        out.append(
            f"{w.text} ({w.knob}): {w.width:.2f} mm of ink at size "
            f"{case['edge_text_size']:g} against the {est:.2f} estimated; it leaves "
            f"{clear:.2f} of vent_gap's {case['vent_gap']:g} mm clear of the port flange; "
            f"the band vword_x centers it in, |x| {band_lo:.2f}..{band_hi:.2f} "
            f"({band_hi - band_lo:.2f} wide) — the ink {fit}")
    return out


# ---------------------------------------------------------------- main

def parse_defines(items: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in items:
        name, eq, value = item.partition("=")
        name = name.strip()
        if not eq or not re.fullmatch(r"[A-Za-z_]\w*", name) or "\n" in value or not value.strip():
            raise Refusal(f"-D {item!r}: expected name=value, one OpenSCAD expression")
        out[name] = value.strip()
    return out


def measure(overrides: dict[str, str]) -> tuple[dict, list[Ink], str, dict[str, int]]:
    """Everything the gate reads: the case's values, the placed inks, the font
    file, and the pinned lines. Refusal on any doubt."""
    pins = check_pins((HERE / CASE).read_text(encoding="utf-8"))
    case = read_case(overrides)
    font_file = check_font(case["label_font"])
    inks = []
    for knob, s in WORDS:
        word = case[knob]
        if not word:
            continue          # an empty word cuts nothing and keeps nothing out
        lo, hi = measure_word(word, case["edge_text_size"], case["label_font"])
        inks.append(place_word(knob, word, lo, hi, s, case["vword_x"]))
    return case, inks, font_file, pins


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 on a failure not in KNOWN, or a KNOWN one that stopped")
    ap.add_argument("-D", dest="defines", action="append", default=[], metavar="NAME=VALUE",
                    help="override a case value after the include, like openscad -D")
    args = ap.parse_args(argv)
    try:
        case, inks, font_file, pins = measure(parse_defines(args.defines))
    except Refusal as err:
        print(f"::error file={GH_FILE}::word ink: refused, nothing measured — {err}")
        return 2
    where = ", ".join(f"-D {d}" for d in args.defines) or "the shipped defaults"
    print(f"word ink: {CASE} at {where}; label_font {case['label_font']!r} is {font_file}")
    for w in inks:
        print(f"  {w.text} ({w.knob}): ink {w.lo:.2f}..{w.hi:.2f}, {w.width:.2f} mm wide")
    fails = evaluate(case, inks)
    new, gone = verdict(fails, KNOWN)
    for f in fails:
        if f.key in KNOWN:
            print(f"  known failure ({f.key}, the case's assert at :{pins[f.assert_name]}): "
                  f"{f.message} — {KNOWN[f.key]}")
    for line in facts(case, inks):
        print(f"  reported, not gated: {line}")
    bad = [f"new failure ({f.key}, the case's assert at :{pins[f.assert_name]}): {f.message}"
           for f in new]
    bad += [f"known failure no longer happens ({k}) — take it out of KNOWN" for k in gone]
    if not args.check:
        for b in bad:
            print(f"  {b}")
        return 0
    for b in bad:
        print(f"::error file={GH_FILE}::word ink: {b}")
    if bad:
        return 1
    print(f"word ink OK: {len(KNOWN)} known failure(s) still happening, nothing new")
    return 0


if __name__ == "__main__":
    sys.exit(main())
